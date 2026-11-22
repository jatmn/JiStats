#include <catch_amalgamated.hpp>
#include "providers/claude/ClaudeCodeLocalParser.hpp"
#include "providers/claude/ClaudeCodeAdapter.hpp"
#include "db/StorageService.hpp"
#include "sqlite_test_support.hpp"
#include "temp_test_directory.hpp"
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace JiStats {

using TestSupport::requireSqlite;
using TestSupport::TempTestDatabase;
using TestSupport::TempTestDirectory;

namespace {

class TempFixtureFile {
public:
    explicit TempFixtureFile(const std::string& contents)
        : path_((directory_.path() / "fixture.json").string()) {
        std::ofstream out(path_, std::ios::binary);
        REQUIRE(out.is_open());
        out << contents;
        REQUIRE(out.good());
    }

    const std::string& path() const { return path_; }
private:
    TempTestDirectory directory_{"jistats_claude_file"};
    std::string path_;
};

} // namespace

TEST_CASE("Claude stats-cache parsing extracts valid aggregates", "[claude][parser]") {
    const std::string fixturePath = "docs/fixtures/claude/stats-cache.redacted.json";
    auto result = ClaudeCodeLocalParser::parseStatsCache(fixturePath);

    REQUIRE(result.success);
    REQUIRE(result.error.empty());
    REQUIRE(result.parserVersion == std::string(ClaudeCodeLocalParser::kParserVersion));
    REQUIRE(result.version == 1);
    REQUIRE(result.lastComputedDate == "2025-01-03");
    REQUIRE(result.totalSessions == 9);
    REQUIRE(result.totalMessages == 39);
    REQUIRE(result.firstSessionDate == "2025-01-01");
    REQUIRE(result.dailyActivity.size() == 3);

    REQUIRE(result.dailyActivity[0].date == "2025-01-01");
    REQUIRE(result.dailyActivity[0].messageCount == 11);
    REQUIRE(result.dailyActivity[0].sessionCount == 2);
    REQUIRE(result.dailyActivity[0].toolCallCount == 4);

    REQUIRE(result.dailyModelTokens.size() == 2);
    REQUIRE(result.dailyModelTokens[0].date == "2025-01-02");
    REQUIRE(result.dailyModelTokens[0].tokensByModel.size() == 2);
    REQUIRE(result.dailyModelTokens[0].tokensByModel.at("claude-sonnet-4-20250514") == 7710);

    REQUIRE(result.modelUsage.size() == 2);
    bool foundSonnet = false;
    bool foundOpus = false;
    for (const auto& model : result.modelUsage) {
        if (model.modelKey == "claude-sonnet-4-20250514") {
            foundSonnet = true;
            REQUIRE(model.inputTokens == 4000);
            REQUIRE(model.outputTokens == 6000);
            REQUIRE(std::abs(model.costUsd - 0.042) < 0.001);
        }
        if (model.modelKey == "claude-opus-4-20250514") {
            foundOpus = true;
            REQUIRE(model.inputTokens == 2000);
            REQUIRE(model.outputTokens == 2465);
            REQUIRE(std::abs(model.costUsd - 0.1335) < 0.001);
        }
    }
    REQUIRE(foundSonnet);
    REQUIRE(foundOpus);
}

TEST_CASE("Claude settings parsing extracts model and channel", "[claude][parser]") {
    const std::string fixturePath = "docs/fixtures/claude/settings.redacted.json";
    auto result = ClaudeCodeLocalParser::parseSettings(fixturePath);

    REQUIRE(result.success);
    REQUIRE(result.error.empty());
    REQUIRE(result.defaultModel == "claude-sonnet-4-20250514");
    REQUIRE(result.autoUpdatesChannel == "stable");
}

TEST_CASE("Claude parser returns error for missing file", "[claude][parser]") {
    auto result = ClaudeCodeLocalParser::parseStatsCache("/nonexistent/path/stats-cache.json");
    REQUIRE(!result.success);
    REQUIRE(!result.error.empty());
}

TEST_CASE("Claude parser returns error for invalid JSON", "[claude][parser]") {
    TempFixtureFile badFixture("not json at all");
    auto result = ClaudeCodeLocalParser::parseStatsCache(badFixture.path());
    REQUIRE(!result.success);
    REQUIRE(!result.error.empty());
}

TEST_CASE("Claude toUsageEvents converts daily model tokens", "[claude][parser]") {
    const std::string fixturePath = "docs/fixtures/claude/stats-cache.redacted.json";
    auto result = ClaudeCodeLocalParser::parseStatsCache(fixturePath);
    REQUIRE(result.success);

    auto events = ClaudeCodeLocalParser::toUsageEvents(result);
    REQUIRE(!events.empty());

    bool foundSonnet = false;
    bool foundOpus = false;
    for (const auto& event : events) {
        REQUIRE(event.sourceType == "local");
        REQUIRE(!event.sourceRunId.empty());
        REQUIRE(!event.rawReference.empty());
        if (event.modelKey == "claude-sonnet-4-20250514") {
            foundSonnet = true;
        }
        if (event.modelKey == "claude-opus-4-20250514") {
            foundOpus = true;
        }
    }
    REQUIRE(foundSonnet);
    REQUIRE(foundOpus);
}

TEST_CASE("Claude toActivityEvents converts daily activity", "[claude][parser]") {
    const std::string fixturePath = "docs/fixtures/claude/stats-cache.redacted.json";
    auto result = ClaudeCodeLocalParser::parseStatsCache(fixturePath);
    REQUIRE(result.success);

    auto events = ClaudeCodeLocalParser::toActivityEvents(result);
    REQUIRE(events.size() == 3);

    for (const auto& event : events) {
        REQUIRE(event.providerKey == "claude_code");
        REQUIRE(event.providerSurface == "claude_code_local_stats");
        REQUIRE(event.activityKind == ActivityKind::SessionActive);
        REQUIRE(event.sourceType == "local");
        REQUIRE(event.confidence == ActivityConfidence::LocalExact);
    }
}

TEST_CASE("Claude toUsageEvents falls back to modelUsage when no daily tokens", "[claude][parser]") {
    const std::string json = R"json({
        "version": 1,
        "lastComputedDate": "2026-04-19",
        "modelUsage": {
            "claude-sonnet-4-20250514": {
                "inputTokens": 1000,
                "outputTokens": 2000,
                "cost_usd": 0.05
            }
        }
    })json";
    TempFixtureFile fixture(json);
    auto result = ClaudeCodeLocalParser::parseStatsCache(fixture.path());
    REQUIRE(result.success);

    auto events = ClaudeCodeLocalParser::toUsageEvents(result);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].modelKey == "claude-sonnet-4-20250514");
    REQUIRE(events[0].totalTokens == 3000);
    REQUIRE(events[0].inputTokens == 1000);
    REQUIRE(events[0].outputTokens == 2000);
}

TEST_CASE("Claude adapter fetchQuota returns local aggregate", "[claude][adapter]") {
    ClaudeCodeAdapter adapter;
    adapter.setMockStatsCachePath("docs/fixtures/claude/stats-cache.redacted.json");

    auto quota = adapter.fetchQuota();
    REQUIRE(quota.success);
    REQUIRE(quota.providerSurface == "claude_code_local_stats");
    REQUIRE(quota.windowType == "local_aggregate");
    REQUIRE(quota.limit == 0);
    REQUIRE(quota.remaining == 0);
    REQUIRE(quota.used > 0);
}

TEST_CASE("Claude adapter fetchUsage returns usage events", "[claude][adapter]") {
    ClaudeCodeAdapter adapter;
    adapter.setMockStatsCachePath("docs/fixtures/claude/stats-cache.redacted.json");

    auto usage = adapter.fetchUsage(0, std::time(nullptr));
    REQUIRE(usage.success);
    REQUIRE(!usage.events.empty());
}

TEST_CASE("Claude adapter fetchUsage filters to the requested time window", "[claude][adapter]") {
    ClaudeCodeAdapter adapter;
    adapter.setMockStatsCachePath("docs/fixtures/claude/stats-cache.redacted.json");

    std::tm tm{};
    tm.tm_year = 2025 - 1900;
    tm.tm_mon = 0;
    tm.tm_mday = 3;
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
#ifdef _WIN32
    const std::time_t dayStart = _mkgmtime(&tm);
#else
    const std::time_t dayStart = timegm(&tm);
#endif
    REQUIRE(dayStart > 0);

    const std::time_t dayEnd = dayStart + 86400 - 1;
    auto usage = adapter.fetchUsage(dayStart, dayEnd);
    REQUIRE(usage.success);
    REQUIRE(usage.events.size() == 1);
    CHECK(usage.events.front().modelKey == "claude-sonnet-4-20250514");
    for (const auto& event : usage.events) {
        CHECK(event.eventTime >= dayStart);
        CHECK(event.eventTime <= dayEnd);
    }
}

TEST_CASE("Claude adapter validateCredentials succeeds for existing fixture", "[claude][adapter]") {
    ClaudeCodeAdapter adapter;
    adapter.setMockStatsCachePath("docs/fixtures/claude/stats-cache.redacted.json");
    REQUIRE(adapter.validateCredentials());
}

TEST_CASE("Claude adapter validateCredentials fails for missing file", "[claude][adapter]") {
    ClaudeCodeAdapter adapter;
    adapter.setMockStatsCachePath("/nonexistent/path/stats-cache.json");
    REQUIRE(!adapter.validateCredentials());
}

TEST_CASE("Claude adapter getCapabilities notes quota limitation", "[claude][adapter]") {
    ClaudeCodeAdapter adapter;
    auto caps = adapter.getCapabilities();
    REQUIRE(!caps.empty());

    bool foundQuota = false;
    for (const auto& cap : caps) {
        if (cap.name == "quota") {
            foundQuota = true;
            REQUIRE(!cap.supported);
        }
    }
    REQUIRE(foundQuota);
}

TEST_CASE("Claude adapter provider key and display name", "[claude][adapter]") {
    ClaudeCodeAdapter adapter;
    REQUIRE(adapter.getProviderKey() == "claude_code");
    REQUIRE(adapter.getDisplayName() == "Claude Code");
    REQUIRE(adapter.isEnabled());
}

TEST_CASE("Claude importLocalActivity persists activity events", "[claude][persistence]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    bool imported = ClaudeCodeLocalParser::importLocalActivity(
        storage,
        "docs/fixtures/claude/stats-cache.redacted.json",
        "docs/fixtures/claude/settings.redacted.json");

    REQUIRE(imported);
    const auto result = storage.execute(
        "SELECT COUNT(*) FROM activity_events WHERE provider_surface = 'claude_code_local_stats';");
    REQUIRE(result.success());
    REQUIRE(result.rowCount() == 1);
    CHECK(result.getInt64(0, 0) == 3);
}

TEST_CASE("Claude importLocalActivity fails for missing file", "[claude][persistence]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    bool imported = ClaudeCodeLocalParser::importLocalActivity(
        storage,
        "/nonexistent/path/stats-cache.json");

    REQUIRE(!imported);
}

} // namespace JiStats
