#include <catch_amalgamated.hpp>
#include "providers/claude/ClaudeCodeLocalParser.hpp"
#include "providers/claude/ClaudeCodeAdapter.hpp"
#include "db/StorageService.hpp"
#include "app_controller_test_support.hpp"
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

TEST_CASE("Claude recovery does not attribute lifetime totals to a day", "[claude][parser]") {
    const auto dailyTokens = GENERATE("[]", R"([{"date":"2025-01-02","tokensByModel":{}}])");
    TempFixtureFile fixture(std::string(R"json({
      "lastComputedDate": "2025-01-02",
      "dailyModelTokens": )json") +
                            dailyTokens + R"json(,
      "modelUsage": {"claude-sonnet": {"inputTokens": 100, "outputTokens": 50}}
    })json");
    ClaudeCodeAdapter adapter;
    adapter.setMockStatsCachePath(fixture.path());

    const auto undated = adapter.fetchUsage(0, 0);
    REQUIRE(undated.success);
    REQUIRE(undated.events.size() == 1);
    CHECK(undated.events.front().totalTokens == 150);

    const auto day = undated.events.front().eventTime;
    const auto recovery = adapter.fetchUsage(day - 86400, day + 86400);
    REQUIRE(recovery.success);
    CHECK(recovery.events.empty());
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
    CHECK_FALSE(events[0].rawReference.empty());
    CHECK(events[0].rawReference.find(std::filesystem::path(fixture.path()).filename().string()) == std::string::npos);
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

TEST_CASE("Claude adapter uses UTC dates for bounded daily usage", "[claude][adapter]") {
    const int localHour = GENERATE(0, 9);
    CAPTURE(localHour);
    struct ResetTimeZone {
        ~ResetTimeZone() {
            _tzset();
        }
    } resetTimeZone;
    const TestSupport::ScopedEnvironmentVariable timeZone("TZ", localHour == 0 ? "UTC0" : "JST-9");
    _tzset();

    ClaudeCodeAdapter adapter;
    adapter.setMockStatsCachePath("docs/fixtures/claude/stats-cache.redacted.json");

    constexpr std::time_t dayStart = 1735862400; // 2025-01-03 00:00:00 UTC
    std::tm localTime{};
    REQUIRE(localtime_s(&localTime, &dayStart) == 0);
    REQUIRE(localTime.tm_hour == localHour);

    const std::time_t dayEnd = dayStart + 86400 - 1;
    const auto usage = adapter.fetchUsage(dayStart, dayEnd);
    REQUIRE(usage.success);
    REQUIRE(usage.events.size() == 1);
    CHECK(usage.events.front().modelKey == "claude-sonnet-4-20250514");
    CHECK(usage.events.front().eventTime == dayStart);
    CHECK(usage.events.front().totalTokens == 4100);
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

TEST_CASE("Claude stats-cache references are redacted and preserve historical replay identity",
          "[claude][persistence]") {
    requireSqlite();
    const bool advanceCache = GENERATE(false, true);
    CAPTURE(advanceCache);

    TempFixtureFile source(R"json({
        "lastComputedDate": "2025-01-02",
        "dailyActivity": [{"date": "2025-01-02", "messageCount": 3}],
        "dailyModelTokens": [{"date": "2025-01-02", "tokensByModel": {"fixture-model": 42}}]
    })json");
    const auto parsed = ClaudeCodeLocalParser::parseStatsCache(source.path());
    REQUIRE(parsed.success);
    const auto activities = ClaudeCodeLocalParser::toActivityEvents(parsed);
    REQUIRE(activities.size() == 1);
    const auto& activity = activities.front();
    const auto& reference = activity.rawReference;
    REQUIRE_FALSE(reference.empty());
    const std::filesystem::path sourcePath(source.path());
    CHECK(reference.find(sourcePath.filename().string()) == std::string::npos);
    CHECK(reference.find(sourcePath.parent_path().filename().string()) == std::string::npos);

    ClaudeCodeAdapter adapter;
    adapter.setMockStatsCachePath(source.path());
    constexpr std::time_t usageDayStart = 1735776000; // 2025-01-02 00:00:00 UTC
    constexpr std::time_t usageDayEnd = usageDayStart + 86400 - 1;
    const auto usage = adapter.fetchUsage(usageDayStart, usageDayEnd);
    REQUIRE(usage.success);
    REQUIRE(usage.events.size() == 1);
    CHECK(usage.events.front().totalTokens == 42);
    CHECK(usage.events.front().rawReference == reference);

    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));
    REQUIRE(ClaudeCodeLocalParser::importLocalActivity(storage, source.path()));
    const auto binding = storage.execute("SELECT provider_id, account_id, raw_reference, id FROM activity_events;");
    REQUIRE(binding.success());
    REQUIRE(binding.rowCount() == 1);
    CHECK(binding.getString(0, 2) == reference);
    REQUIRE(storage.execute("UPDATE activity_events SET raw_reference = ?;", {source.path() + "|date=2025-01-02"})
                .success());

    // Reproduce a usage row saved by an older version of the recovery path.
    PersistedUsageEvent backfill;
    backfill.providerId = binding.getInt(0, 0);
    backfill.accountId = binding.getInt(0, 1);
    backfill.eventTime = usage.events.front().eventTime;
    backfill.sourceType = "provider_backfill";
    backfill.sourceRunId = usage.events.front().sourceRunId;
    backfill.modelKey = usage.events.front().modelKey;
    backfill.requestType = usage.events.front().requestType;
    backfill.totalTokens = usage.events.front().totalTokens;
    backfill.isBackfilled = true;
    backfill.rawReference = source.path();
    REQUIRE(storage.insertUsageEvents({backfill}));
    REQUIRE(storage
                .execute("DELETE FROM schema_version WHERE version >= 13;"
                         "ALTER TABLE quota_snapshots DROP COLUMN snapshot_subsecond_ns;")
                .success());

    storage.close();
    if (advanceCache) {
        std::ofstream updated(source.path());
        updated << R"json({
            "lastComputedDate": "2025-01-03",
            "dailyActivity": [{"date": "2025-01-02", "messageCount": 3}],
            "dailyModelTokens": [{"date": "2025-01-02", "tokensByModel": {"fixture-model": 42}}]
        })json";
        REQUIRE(updated.good());
    }
    REQUIRE(storage.init(tempDb.path()));
    const auto migratedActivity = storage.execute("SELECT id, raw_reference, source_run_id FROM activity_events;");
    REQUIRE(migratedActivity.success());
    REQUIRE(migratedActivity.rowCount() == 1);
    CHECK(migratedActivity.getInt64(0, 0) == binding.getInt64(0, 3));
    CHECK(migratedActivity.getString(0, 1) == reference);
    CHECK(migratedActivity.getString(0, 2) == activity.sourceRunId);
    const auto migratedUsage =
        storage.getUsageEvents(backfill.providerId, backfill.accountId, backfill.eventTime - 1, backfill.eventTime + 1);
    REQUIRE(migratedUsage.size() == 1);
    CHECK(migratedUsage.front().rawReference == reference);
    CHECK(migratedUsage.front().sourceRunId == backfill.sourceRunId);
    CHECK(migratedUsage.front().totalTokens == 42);

    const auto replay = adapter.fetchUsage(usageDayStart, usageDayEnd);
    REQUIRE(replay.success);
    REQUIRE(replay.events.size() == 1);
    CHECK((replay.events.front().sourceRunId != backfill.sourceRunId) == advanceCache);
    REQUIRE(ClaudeCodeLocalParser::importLocalActivity(storage, source.path()));
    backfill.sourceRunId = replay.events.front().sourceRunId;
    backfill.rawReference = replay.events.front().rawReference;
    REQUIRE(storage.insertUsageEvents({backfill}));

    storage.close();
    REQUIRE(storage.init(tempDb.path()));
    const auto activityRows = storage.execute("SELECT raw_reference FROM activity_events;");
    REQUIRE(activityRows.success());
    const size_t expectedRows = advanceCache ? 2 : 1;
    REQUIRE(activityRows.rowCount() == expectedRows);
    for (size_t row = 0; row < activityRows.rowCount(); ++row) {
        CHECK(activityRows.getString(row, 0) == reference);
    }
    const auto usageRows =
        storage.getUsageEvents(backfill.providerId, backfill.accountId, backfill.eventTime - 1, backfill.eventTime + 1);
    REQUIRE(usageRows.size() == expectedRows);
    for (const auto& row : usageRows) {
        CHECK(row.rawReference == reference);
        CHECK(row.totalTokens == 42);
    }

    TempFixtureFile otherSource(R"json({
        "lastComputedDate": "2025-01-02",
        "dailyActivity": [{"date": "2025-01-02", "messageCount": 3}]
    })json");
    REQUIRE(ClaudeCodeLocalParser::importLocalActivity(storage, otherSource.path()));
    const auto sources = storage.execute("SELECT COUNT(DISTINCT raw_reference) FROM activity_events;");
    REQUIRE(sources.success());
    CHECK(sources.getInt(0, 0) == 2);
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
