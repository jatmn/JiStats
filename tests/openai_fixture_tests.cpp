// tests/openai_fixture_tests.cpp
// Tests for OpenAI fixture file parsing

#define JISTATS_ENABLE_TEST_HOOKS 1

#include <catch_amalgamated.hpp>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "providers/openai/OpenAIAdapter.hpp"
#include "temp_test_directory.hpp"

using namespace JiStats;
using JiStats::TestSupport::TempTestDirectory;

namespace {

const std::string kSampleFixturePath = "docs/fixtures/openai/sample_openai_fixture.json";
const std::string kCodexRateLimitFixturePath = "docs/fixtures/openai/codex-rate-limits-shape.sample.json";
const std::string kCodexRedactedRolloutFixturePath =
    "docs/fixtures/openai/codex-rollout-rate-limits.redacted.jsonl";

class TempFixtureFile {
public:
    explicit TempFixtureFile(const std::string& contents)
        : path_((directory_.path() / "fixture.json").string()) {
        std::ofstream out(path_, std::ios::binary);
        if (!out.is_open()) {
            throw std::runtime_error("Failed to create temporary fixture file");
        }

        out << contents;
    }

    const std::string& path() const {
        return path_;
    }

private:
    TempTestDirectory directory_{"jistats_openai_file"};
    std::string path_;
};

class TempFixtureDirectory {
public:
    std::filesystem::path makeFile(const std::filesystem::path& relativePath, const std::string& contents) const {
        const std::filesystem::path filePath = directory_.path() / relativePath;
        std::filesystem::create_directories(filePath.parent_path());
        std::ofstream out(filePath, std::ios::binary);
        if (!out.is_open()) {
            throw std::runtime_error("Failed to create temporary fixture file in directory");
        }

        out << contents;
        return filePath;
    }

    const std::filesystem::path& path() const {
        return directory_.path();
    }

private:
    TempTestDirectory directory_{"jistats_openai_dir"};
};

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const char* name, const std::string& value)
        : name_(name ? name : "") {
        if (name_.empty()) {
            throw std::runtime_error("Environment variable name is required");
        }

        char* existing = nullptr;
        std::size_t existingLength = 0;
        if (_dupenv_s(&existing, &existingLength, name_.c_str()) == 0 && existing != nullptr) {
            hadOriginal_ = true;
            originalValue_ = existing;
            free(existing);
        }

        set(value);
    }

    ~ScopedEnvironmentVariable() {
        if (hadOriginal_) {
            set(originalValue_);
        } else {
            set("");
        }
    }

private:
    void set(const std::string& value) const {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value.c_str());
#else
        if (value.empty()) {
            unsetenv(name_.c_str());
        } else {
            setenv(name_.c_str(), value.c_str(), 1);
        }
#endif
    }

    std::string name_;
    bool hadOriginal_ = false;
    std::string originalValue_;
};

std::vector<UsageEvent> parseTempFixture(const std::string& contents) {
    TempFixtureFile fixture(contents);
    return OpenAIAdapter::parseFixtureFile(fixture.path());
}

CodexRateLimitSnapshot parseTempCodexRateLimitFixture(const std::string& contents) {
    TempFixtureFile fixture(contents);
    return OpenAIAdapter::parseCodexRateLimitFixtureFile(fixture.path());
}

} // namespace

TEST_CASE("OpenAI sample fixture parsing extracts valid events", "[openai][fixtures]") {
    auto events = OpenAIAdapter::parseFixtureFile(kSampleFixturePath);

    REQUIRE(events.size() == 3);

    REQUIRE(events[0].eventTime == 1713464400);
    REQUIRE(events[0].modelKey == "gpt-3.5-turbo");
    REQUIRE(events[0].requestType == "chat");
    REQUIRE(events[0].inputTokens == 1200);
    REQUIRE(events[0].outputTokens == 450);
    REQUIRE(events[0].totalTokens == 1650);
    REQUIRE(events[0].costMinorUnits == 243);

    REQUIRE(events[1].eventTime == 1713468000);
    REQUIRE(events[1].modelKey == "gpt-4");
    REQUIRE(events[1].requestType == "chat");
    REQUIRE(events[1].inputTokens == 800);
    REQUIRE(events[1].outputTokens == 1200);
    REQUIRE(events[1].totalTokens == 2000);
    REQUIRE(events[1].costMinorUnits == 1260);

    REQUIRE(events[2].eventTime == 1713471600);
    REQUIRE(events[2].modelKey == "gpt-4-turbo");
    REQUIRE(events[2].requestType == "completion");
    REQUIRE(events[2].inputTokens == 2500);
    REQUIRE(events[2].outputTokens == 1800);
    REQUIRE(events[2].totalTokens == 4300);
    REQUIRE(events[2].costMinorUnits == 3870);

    for (const auto& event : events) {
        REQUIRE(event.inputTokens + event.outputTokens == event.totalTokens);
        REQUIRE(event.sourceType == "api");
        REQUIRE(event.sourceRunId == "fixture");
        REQUIRE(event.rawReference == kSampleFixturePath);
    }
}

TEST_CASE("OpenAI fixture parsing supports exact cost minor units", "[openai][fixtures]") {
    const auto events = parseTempFixture(R"json(
{
    "events": [
        {
            "timestamp": 1713464400,
            "model": "gpt-4.1-mini",
            "request_type": "chat",
            "input_tokens": 10,
            "output_tokens": 5,
            "total_tokens": 15,
            "cost_minor_units": 243,
            "cost_dollars": 9.99
        }
    ]
}
)json");

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].costMinorUnits == 243);
}

TEST_CASE("OpenAI fixture parsing preserves sub-cent precision from cost_dollars", "[openai][fixtures]") {
    const auto events = parseTempFixture(R"json(
{
    "events": [
        {
            "timestamp": 1713464400,
            "model": "gpt-4.1-mini",
            "request_type": "chat",
            "input_tokens": 10,
            "output_tokens": 5,
            "total_tokens": 15,
            "cost_dollars": 0.0243
        }
    ]
}
)json");

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].costMinorUnits == 243);
}

TEST_CASE("OpenAI fixture parsing handles missing files and malformed JSON", "[openai][fixtures]") {
    REQUIRE(OpenAIAdapter::parseFixtureFile("nonexistent/file.json").empty());

    const auto malformedEvents = parseTempFixture(R"json(
{ "events": [ { "timestamp": 1, }
)json");
    REQUIRE(malformedEvents.empty());

    const auto wrongShapeEvents = parseTempFixture(R"json(
{ "events": { "timestamp": 1 } }
)json");
    REQUIRE(wrongShapeEvents.empty());
}

TEST_CASE("OpenAI fixture parsing skips events with missing required fields", "[openai][fixtures]") {
    const auto events = parseTempFixture(R"json(
{
    "events": [
        {
            "timestamp": 1713464400,
            "model": "gpt-4.1-mini",
            "request_type": "chat",
            "input_tokens": 10,
            "output_tokens": 5,
            "total_tokens": 15,
            "cost_minor_units": 3
        },
        {
            "model": "gpt-4.1-mini",
            "request_type": "chat",
            "input_tokens": 10,
            "output_tokens": 5,
            "total_tokens": 15
        },
        {
            "timestamp": 1713464401,
            "request_type": "chat",
            "input_tokens": 10,
            "output_tokens": 5,
            "total_tokens": 15
        },
        {
            "timestamp": 1713464402,
            "model": "gpt-4.1-mini",
            "request_type": "chat",
            "input_tokens": 10,
            "output_tokens": 5
        }
    ]
}
)json");

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].eventTime == 1713464400);
}

TEST_CASE("OpenAI fixture parsing skips malformed events instead of emitting defaults", "[openai][fixtures]") {
    const auto events = parseTempFixture(R"json(
{
    "events": [
        "not-an-object",
        {
            "timestamp": 1713464400,
            "model": "gpt-4.1-mini",
            "request_type": "chat",
            "input_tokens": 10,
            "output_tokens": 5,
            "total_tokens": 15,
            "cost_minor_units": 3
        },
        {
            "timestamp": "1713464401",
            "model": "gpt-4.1-mini",
            "request_type": "chat",
            "input_tokens": 10,
            "output_tokens": 5,
            "total_tokens": 15
        },
        {
            "timestamp": 1713464402,
            "model": "gpt-4.1-mini",
            "request_type": "chat",
            "input_tokens": -1,
            "output_tokens": 5,
            "total_tokens": 4
        },
        {
            "timestamp": 1713464403,
            "model": "gpt-4.1-mini",
            "request_type": "chat",
            "input_tokens": 10,
            "output_tokens": 5,
            "total_tokens": 999
        },
        {
            "timestamp": 1713464404,
            "model": "gpt-4.1-mini",
            "request_type": "chat",
            "input_tokens": 10,
            "output_tokens": 5,
            "total_tokens": 15,
            "cost_dollars": "0.15"
        }
    ]
}
)json");

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].eventTime == 1713464400);
    REQUIRE(events[0].modelKey == "gpt-4.1-mini");
}

TEST_CASE("OpenAI fixture parsing accepts alternate array wrappers", "[openai][fixtures]") {
    const auto events = parseTempFixture(R"json(
{
    "data": [
        {
            "timestamp": 1713464400,
            "model": "gpt-4.1-mini",
            "request_type": "chat",
            "input_tokens": 10,
            "output_tokens": 5,
            "total_tokens": 15,
            "cost_dollars": 0.0243
        }
    ]
}
)json");

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].costMinorUnits == 243);
}

TEST_CASE("OpenAI Codex rate-limit fixture parsing extracts primary bucket metadata", "[openai][fixtures][codex]") {
    const CodexRateLimitSnapshot snapshot =
        OpenAIAdapter::parseCodexRateLimitFixtureFile(kCodexRateLimitFixturePath);

    REQUIRE(snapshot.success);
    REQUIRE(snapshot.error.empty());
    REQUIRE(snapshot.sourceType == "codex_local_fixture");
    REQUIRE(snapshot.snapshotTime == 0);
    REQUIRE(snapshot.primary.present);
    REQUIRE(snapshot.primary.bucketKey == "primary");
    REQUIRE(snapshot.primary.usedPercent == 25.0);
    REQUIRE(snapshot.primary.windowDurationMinutes == 15);
    REQUIRE(snapshot.primary.resetsAt == 1730947200);
    REQUIRE_FALSE(snapshot.secondary.present);
    REQUIRE(snapshot.secondary.bucketKey == "secondary");
    REQUIRE(snapshot.reachedType.empty());
    REQUIRE(snapshot.limitId.empty());
    REQUIRE(snapshot.planType.empty());
    REQUIRE(snapshot.rawResponse.empty());
}

TEST_CASE("OpenAI Codex rate-limit fixture parsing preserves reached type", "[openai][fixtures][codex]") {
    const CodexRateLimitSnapshot snapshot = parseTempCodexRateLimitFixture(
        R"json({"rateLimits":{"primary":{"usedPercent":100,"windowDurationMins":300,"resetsAt":1730947200},"secondary":null,"rateLimitReachedType":"primary"}})json");

    REQUIRE(snapshot.success);
    REQUIRE(snapshot.reachedType == "primary");
}

TEST_CASE("OpenAI Codex rollout fixture parsing accepts local rollout rate-limit payloads", "[openai][fixtures][codex]") {
    const CodexRateLimitSnapshot snapshot = parseTempCodexRateLimitFixture(R"json(
{"timestamp":"2025-04-18T16:35:16.962Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":2200,"cached_input_tokens":730,"output_tokens":150,"reasoning_output_tokens":17,"total_tokens":2350},"last_token_usage":{"input_tokens":2200,"cached_input_tokens":730,"output_tokens":150,"reasoning_output_tokens":17,"total_tokens":2350},"model_context_window":258400},"rate_limits":{"limit_id":"codex","limit_name":null,"primary":{"used_percent":12.0,"window_minutes":300,"resets_at":1745009457},"secondary":{"used_percent":3.0,"window_minutes":10080,"resets_at":1745596257},"credits":null,"plan_type":"prolite"}}}
)json");

    REQUIRE(snapshot.success);
    REQUIRE(snapshot.sourceType == "codex_local_rollout");
    REQUIRE(snapshot.limitId == "codex");
    REQUIRE(snapshot.planType == "prolite");
    REQUIRE(snapshot.primary.present);
    REQUIRE(snapshot.primary.usedPercent == 12.0);
    REQUIRE(snapshot.primary.windowDurationMinutes == 300);
    REQUIRE(snapshot.primary.resetsAt == 1745009457);
    REQUIRE(snapshot.secondary.present);
    REQUIRE(snapshot.secondary.usedPercent == 3.0);
    REQUIRE(snapshot.secondary.windowDurationMinutes == 10080);
    REQUIRE(snapshot.secondary.resetsAt == 1745596257);
    REQUIRE(snapshot.eventTime == 1744994116);
    REQUIRE_FALSE(snapshot.sourcePath.empty());
}

TEST_CASE("OpenAI Codex redacted rollout fixture stays parser-safe", "[openai][fixtures][codex]") {
    const CodexRateLimitSnapshot snapshot =
        OpenAIAdapter::parseCodexRateLimitFixtureFile(kCodexRedactedRolloutFixturePath);

    REQUIRE(snapshot.success);
    REQUIRE(snapshot.sourceType == "codex_local_rollout");
    REQUIRE(snapshot.limitId == "codex");
    REQUIRE(snapshot.planType == "prolite");
    REQUIRE(snapshot.primary.present);
    REQUIRE(snapshot.primary.usedPercent == 28.0);
    REQUIRE(snapshot.secondary.present);
    REQUIRE(snapshot.secondary.usedPercent == 6.0);
    REQUIRE(snapshot.eventTime == 1745007696);
    REQUIRE(snapshot.sourcePath == kCodexRedactedRolloutFixturePath);
}

TEST_CASE("OpenAI Codex rollout directory parsing selects the newest valid snapshot", "[openai][fixtures][codex]") {
    TempFixtureDirectory fixtureDirectory;
    fixtureDirectory.makeFile(
        std::filesystem::path("2025") / "04" / "18" / "rollout-old.jsonl",
        R"json({"timestamp":"2025-04-18T20:00:00.000Z","type":"event_msg","payload":{"type":"token_count","rate_limits":{"limit_id":"codex","primary":{"used_percent":11.0,"window_minutes":300,"resets_at":1745006400},"secondary":null,"plan_type":"prolite"}}})json");
    fixtureDirectory.makeFile(
        std::filesystem::path("2025") / "04" / "18" / "rollout-null.jsonl",
        R"json({"timestamp":"2025-04-18T20:10:00.000Z","type":"event_msg","payload":{"type":"token_count","rate_limits":null}})json");
    const std::filesystem::path newestPath = fixtureDirectory.makeFile(
        std::filesystem::path("2025") / "04" / "18" / "rollout-new.jsonl",
        R"json({"timestamp":"2025-04-18T20:21:36.945Z","type":"event_msg","payload":{"type":"token_count","rate_limits":{"limit_id":"codex","primary":{"used_percent":28.0,"window_minutes":300,"resets_at":1745009457},"secondary":{"used_percent":6.0,"window_minutes":10080,"resets_at":1745596257},"plan_type":"prolite"}}})json");

    const CodexRateLimitSnapshot snapshot =
        OpenAIAdapter::parseLatestCodexRateLimitSnapshotFromRolloutDirectory(
            fixtureDirectory.path().string());

    REQUIRE(snapshot.success);
    REQUIRE(snapshot.sourceType == "codex_local_rollout");
    REQUIRE(snapshot.eventTime == 1745007696);
    REQUIRE(snapshot.primary.usedPercent == 28.0);
    REQUIRE(std::filesystem::path(snapshot.sourcePath) == newestPath);
}

TEST_CASE("OpenAI Codex rollout directory parsing rejects missing directories", "[openai][fixtures][codex]") {
    const CodexRateLimitSnapshot snapshot =
        OpenAIAdapter::parseLatestCodexRateLimitSnapshotFromRolloutDirectory(
            "nonexistent/codex-rollout-dir");

    REQUIRE_FALSE(snapshot.success);
    REQUIRE(snapshot.error == "Codex rollout directory does not exist");
}

TEST_CASE("OpenAI Codex rollout directory parsing reports malformed snapshot diagnostics", "[openai][fixtures][codex]") {
    TempFixtureDirectory fixtureDirectory;
    fixtureDirectory.makeFile(
        std::filesystem::path("2025") / "04" / "18" / "rollout-invalid.jsonl",
        R"json({"timestamp":"2025-04-18T20:21:36.945Z","type":"event_msg","payload":{"type":"token_count","rate_limits":{"limit_id":"codex","primary":{"window_minutes":300,"resets_at":1745009457},"secondary":null,"plan_type":"prolite"}}})json");

    const CodexRateLimitSnapshot snapshot =
        OpenAIAdapter::parseLatestCodexRateLimitSnapshotFromRolloutDirectory(
            fixtureDirectory.path().string());

    REQUIRE_FALSE(snapshot.success);
    CHECK_THAT(
        snapshot.error,
        Catch::Matchers::ContainsSubstring(
            "Codex rollout files were found, but none contained a supported rate-limit snapshot"));
    CHECK_THAT(snapshot.error, Catch::Matchers::ContainsSubstring("rollout-invalid.jsonl"));
    CHECK_THAT(snapshot.error, Catch::Matchers::ContainsSubstring("invalid primary bucket"));
}

TEST_CASE("OpenAI fetchQuota maps the newest local Codex rollout snapshot into quota pressure", "[openai][quota][codex]") {
    TempFixtureDirectory userProfileRoot;
    userProfileRoot.makeFile(
        std::filesystem::path(".codex") / "sessions" / "2025" / "04" / "18" / "rollout-old.jsonl",
        R"json({"timestamp":"2025-04-18T20:00:00.000Z","type":"event_msg","payload":{"type":"token_count","rate_limits":{"limit_id":"codex","primary":{"used_percent":11.0,"window_minutes":300,"resets_at":1745006400},"secondary":null,"plan_type":"prolite"}}})json");
    userProfileRoot.makeFile(
        std::filesystem::path(".codex") / "sessions" / "2025" / "04" / "18" / "rollout-new.jsonl",
        R"json({"timestamp":"2025-04-18T20:21:36.945Z","type":"event_msg","payload":{"type":"token_count","rate_limits":{"limit_id":"codex","primary":{"used_percent":28.0,"window_minutes":300,"resets_at":1745009457},"secondary":{"used_percent":6.0,"window_minutes":10080,"resets_at":1745596257},"plan_type":"prolite"}}})json");

    ScopedEnvironmentVariable codexHome("CODEX_HOME", "");
    ScopedEnvironmentVariable userProfile("USERPROFILE", userProfileRoot.path().string());

    OpenAIAdapter adapter;
    const QuotaResponse quota = adapter.fetchQuota();

    REQUIRE(quota.success);
    CHECK(quota.snapshotTime == 1745007696);
    CHECK(quota.windowStart == 1744991457);
    CHECK(quota.windowEnd == 1745009457);
    CHECK(quota.used == 28);
    CHECK(quota.limit == 100);
    CHECK(quota.remaining == 72);
    CHECK(quota.windowType == "interval");
    CHECK(quota.providerSurface == "openai_subscription_codex_local");
    CHECK(quota.rawResponse.empty());
}

TEST_CASE("OpenAI fetchQuota selects the latest subsecond reading in one Codex rollout", "[openai][quota][codex]") {
    TempFixtureDirectory codexHomeRoot;
    ScopedEnvironmentVariable codexHome("CODEX_HOME", codexHomeRoot.path().string());
    const std::filesystem::path rollout = std::filesystem::path("sessions") / "rollout-same-second.jsonl";
    const std::string earlier =
        R"json({"timestamp":"2025-04-18T20:21:36.100Z","payload":{"type":"token_count","rate_limits":{"primary":{"used_percent":6,"window_minutes":300,"resets_at":1745009457},"secondary":null}}})json";
    const std::string later =
        R"json({"timestamp":"2025-04-18T20:21:36.900Z","payload":{"type":"token_count","rate_limits":{"primary":{"used_percent":17,"window_minutes":300,"resets_at":1745009457},"secondary":null}}})json";

    for (const auto& contents : {earlier + '\n' + later, later + '\n' + earlier}) {
        codexHomeRoot.makeFile(rollout, contents);
        OpenAIAdapter adapter;
        const QuotaResponse quota = adapter.fetchQuota();

        INFO(quota.error);
        REQUIRE(quota.success);
        CHECK(quota.snapshotTime == 1745007696);
        CHECK(quota.remaining == 83);
    }
}

TEST_CASE("OpenAI fetchQuota reports rollout discovery failures without falling back to scaffolds", "[openai][quota][codex]") {
    {
        TempFixtureDirectory userProfileRoot;
        ScopedEnvironmentVariable codexHome("CODEX_HOME", "");
        ScopedEnvironmentVariable userProfile("USERPROFILE", userProfileRoot.path().string());

        OpenAIAdapter adapter;
        const QuotaResponse quota = adapter.fetchQuota();

        REQUIRE_FALSE(quota.success);
        CHECK(quota.error == "Codex rollout directory does not exist");
    }

    {
        TempFixtureDirectory userProfileRoot;
        userProfileRoot.makeFile(
            std::filesystem::path(".codex") / "sessions" / "2025" / "04" / "18" / "rollout-invalid.jsonl",
            R"json({"timestamp":"2025-04-18T20:21:36.945Z","type":"event_msg","payload":{"type":"token_count","rate_limits":{"limit_id":"codex","primary":{"window_minutes":300,"resets_at":1745009457},"secondary":null,"plan_type":"prolite"}}})json");

        ScopedEnvironmentVariable codexHome("CODEX_HOME", "");
        ScopedEnvironmentVariable userProfile("USERPROFILE", userProfileRoot.path().string());

        OpenAIAdapter adapter;
        const QuotaResponse quota = adapter.fetchQuota();

        REQUIRE_FALSE(quota.success);
        CHECK_THAT(
            quota.error,
            Catch::Matchers::ContainsSubstring(
                "Codex rollout files were found, but none contained a supported rate-limit snapshot"));
        CHECK_THAT(quota.error, Catch::Matchers::ContainsSubstring("rollout-invalid.jsonl"));
    }
}

TEST_CASE("OpenAI Codex rate-limit fixture parsing rejects malformed shapes", "[openai][fixtures][codex]") {
    {
        const CodexRateLimitSnapshot snapshot =
            OpenAIAdapter::parseCodexRateLimitFixtureFile("nonexistent/codex-rate-limit.json");
        REQUIRE_FALSE(snapshot.success);
        REQUIRE(snapshot.error == "Failed to open Codex rate-limit fixture");
    }

    {
        const CodexRateLimitSnapshot snapshot = parseTempCodexRateLimitFixture(R"json({ "rateLimits": [] })json");
        REQUIRE_FALSE(snapshot.success);
        REQUIRE(snapshot.error == "Codex rate-limit fixture is missing a supported rate-limit payload");
    }

    {
        const CodexRateLimitSnapshot snapshot = parseTempCodexRateLimitFixture(R"json(
{
    "rateLimits": {
        "primary": {
            "windowDurationMins": 15,
            "resetsAt": 1730947200
        },
        "secondary": null,
        "rateLimitReachedType": null
    }
}
)json");
        REQUIRE_FALSE(snapshot.success);
        REQUIRE(snapshot.error == "Codex rate-limit fixture has an invalid primary bucket");
    }

    {
        const CodexRateLimitSnapshot snapshot = parseTempCodexRateLimitFixture(R"json(
{
    "rateLimits": {
        "primary": {
            "usedPercent": 101,
            "windowDurationMins": 15,
            "resetsAt": 1730947200
        },
        "secondary": null,
        "rateLimitReachedType": null
    }
}
)json");
        REQUIRE_FALSE(snapshot.success);
        REQUIRE(snapshot.error == "Codex rate-limit fixture has an invalid primary bucket");
    }
}

TEST_CASE("OpenAI Codex token-usage rollout parsing extracts total_token_usage", "[openai][fixtures][codex]") {
    const auto snapshots = OpenAIAdapter::parseCodexTokenUsageFromRolloutFile(kCodexRedactedRolloutFixturePath);
    REQUIRE(snapshots.size() == 1);
    REQUIRE(snapshots[0].eventTime == 1745007696);
    REQUIRE(snapshots[0].inputTokens == 3200);
    REQUIRE(snapshots[0].cachedInputTokens == 1400);
    REQUIRE(snapshots[0].outputTokens == 260);
    REQUIRE(snapshots[0].reasoningOutputTokens == 51);
    REQUIRE(snapshots[0].totalTokens == 3460);
    REQUIRE(snapshots[0].sourcePath == kCodexRedactedRolloutFixturePath);
}

TEST_CASE("OpenAI Codex token-usage rollout parsing handles multiple token_count events", "[openai][fixtures][codex]") {
    TempFixtureFile fixture(
        R"json({"timestamp":"2025-04-18T20:00:00.000Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":100,"cached_input_tokens":0,"output_tokens":50,"reasoning_output_tokens":0,"total_tokens":150}},"rate_limits":null}})json"
        "\n"
        R"json({"timestamp":"2025-04-18T20:01:00.000Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":200,"cached_input_tokens":10,"output_tokens":100,"reasoning_output_tokens":5,"total_tokens":300}},"rate_limits":null}})json"
        "\n"
        R"json({"timestamp":"2025-04-18T20:02:00.000Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":300,"cached_input_tokens":20,"output_tokens":150,"reasoning_output_tokens":10,"total_tokens":450}},"rate_limits":null}})json"
    );

    const auto snapshots = OpenAIAdapter::parseCodexTokenUsageFromRolloutFile(fixture.path());
    REQUIRE(snapshots.size() == 3);
    REQUIRE(snapshots[0].eventTime == 1745006400);
    REQUIRE(snapshots[0].totalTokens == 150);
    REQUIRE(snapshots[1].eventTime == 1745006460);
    REQUIRE(snapshots[1].totalTokens == 300);
    REQUIRE(snapshots[2].eventTime == 1745006520);
    REQUIRE(snapshots[2].totalTokens == 450);
}

TEST_CASE("Codex records requests once when ledger and context counters overlap", "[openai][fixtures][codex]") {
    TempFixtureFile fixture(R"json(
{"type":"session_meta","payload":{"id":"child"}}
{"type":"turn_context","payload":{}}
{"timestamp":"2026-04-20T12:00:00.1Z","type":"token_usage_record","payload":{"thread_id":"parent","response_id":"copied","usage":{"input_tokens":999,"output_tokens":1}}}
{"timestamp":"2026-04-20T12:00:01.1Z","type":"token_usage_record","payload":{"thread_id":"child","response_id":"first","usage":{"input_tokens":120,"cached_input_tokens":40,"output_tokens":30}}}
{"timestamp":"2026-04-20T12:00:01.2Z","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":120,"output_tokens":30},"total_token_usage":{"input_tokens":120,"output_tokens":30}}}}
{"timestamp":"2026-04-20T12:00:01.3Z","type":"token_usage_record","payload":{"response_id":"first","usage":{"input_tokens":120,"output_tokens":30}}}
{"timestamp":"2026-04-20T12:00:01.4Z","type":"token_usage_record","payload":{"response_id":"compaction","usage":{"input_tokens":40,"output_tokens":10}}}
{"type":"turn_context","payload":{}}
{"timestamp":"2026-04-20T12:01:00Z","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":20,"output_tokens":5},"total_token_usage":{"input_tokens":20,"output_tokens":5}}}}
{"timestamp":"2026-04-20T12:01:01Z","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":20,"output_tokens":5},"total_token_usage":{"input_tokens":20,"output_tokens":5}}}}
{"timestamp":"2026-04-20T12:01:02Z","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":0,"output_tokens":0,"total_tokens":1000}}}}
)json");
    const auto records = OpenAIAdapter::parseCodexTokenUsageFromRolloutFile(fixture.path());
    REQUIRE(records.size() == 3);
    CHECK(records[0].totalTokens == 150);
    CHECK(records[0].cachedInputTokens == 40);
    CHECK(records[0].requestId == "response:first");
    CHECK(records[0].eventSubsecondNs == 100000000);
    CHECK(records[1].totalTokens == 50);
    CHECK(records[1].eventTime == records[0].eventTime);
    CHECK(records[1].eventSubsecondNs == 400000000);
    CHECK_FALSE(records[2].eventSubsecondNs.has_value());
    CHECK(records[2].totalTokens == 25);
    CHECK_FALSE(records[2].requestId.empty());
}

TEST_CASE("OpenAI Codex token-usage rollout parsing skips events without valid timestamps", "[openai][fixtures][codex]") {
    TempFixtureFile fixture(
        R"json({"type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":100,"output_tokens":50,"total_tokens":150}},"rate_limits":null}})json"
        "\n"
        R"json({"timestamp":"2025-04-18T20:01:00.000Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":200,"output_tokens":100,"total_tokens":300}},"rate_limits":null}})json"
    );

    const auto snapshots = OpenAIAdapter::parseCodexTokenUsageFromRolloutFile(fixture.path());
    REQUIRE(snapshots.size() == 1);
    REQUIRE(snapshots[0].eventTime == 1745006460);
    REQUIRE(snapshots[0].totalTokens == 300);
}

TEST_CASE("OpenAI Codex token-usage collection aggregates and sorts across files", "[openai][fixtures][codex]") {
    const std::time_t now = std::time(nullptr);
    std::tm utcTime{};
    gmtime_s(&utcTime, &now);
    char year[8]{};
    char month[8]{};
    char day[8]{};
    std::strftime(year, sizeof(year), "%Y", &utcTime);
    std::strftime(month, sizeof(month), "%m", &utcTime);
    std::strftime(day, sizeof(day), "%d", &utcTime);

    auto makeTimestamp = [&](int hour, int min, int sec) -> std::string {
        std::tm fixedTm = utcTime;
        fixedTm.tm_hour = hour;
        fixedTm.tm_min = min;
        fixedTm.tm_sec = sec;
        const std::time_t fixedTime = _mkgmtime(&fixedTm);
        std::tm outTm{};
        gmtime_s(&outTm, &fixedTime);
        char buf[32]{};
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &outTm);
        return buf;
    };

    const std::string ts0 = makeTimestamp(20, 0, 0);
    const std::string ts1 = makeTimestamp(20, 1, 0);
    const std::string ts2 = makeTimestamp(20, 2, 0);

    TempFixtureDirectory fixtureDirectory;
    fixtureDirectory.makeFile(
        std::filesystem::path("2020") / "01" / "01" / "rollout-later.jsonl",
        std::string(R"json({"timestamp":")json") + ts2 +
            R"json(","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":300,"output_tokens":150,"total_tokens":450}},"rate_limits":null}})json");
    fixtureDirectory.makeFile(
        std::filesystem::path(year) / month / day / "rollout-earlier.jsonl",
        std::string(R"json({"timestamp":")json") + ts0 +
            R"json(","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":100,"output_tokens":50,"total_tokens":150}},"rate_limits":null}})json"
            "\n" +
            std::string(R"json({"timestamp":")json") + ts1 +
            R"json(","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":200,"output_tokens":100,"total_tokens":300}},"rate_limits":null}})json");

    const auto snapshots =
        OpenAIAdapter::collectCodexTokenUsageSnapshots(fixtureDirectory.path().string()).observations;
    REQUIRE(snapshots.size() == 3);
    REQUIRE(snapshots[0].totalTokens == 150);
    REQUIRE(snapshots[1].totalTokens == 300);
    REQUIRE(snapshots[2].totalTokens == 450);
}

TEST_CASE("OpenAI Codex token-usage throughput computes expected rates from known deltas", "[openai][fixtures][codex]") {
    // Three events spaced 60 seconds apart with total tokens increasing by 150 each
    // tokens/min = (150 / 60) * 60 = 150
    // prompts/min = (1 / 60) * 60 = 1
    TempFixtureFile fixture(
        R"json({"timestamp":"2025-04-18T20:00:00.000Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":100,"output_tokens":50,"total_tokens":150}},"rate_limits":null}})json"
        "\n"
        R"json({"timestamp":"2025-04-18T20:01:00.000Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":200,"output_tokens":100,"total_tokens":300}},"rate_limits":null}})json"
        "\n"
        R"json({"timestamp":"2025-04-18T20:02:00.000Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":300,"output_tokens":150,"total_tokens":450}},"rate_limits":null}})json"
    );

    const auto snapshots = OpenAIAdapter::parseCodexTokenUsageFromRolloutFile(fixture.path());
    REQUIRE(snapshots.size() == 3);

    double totalTokenRate = 0.0;
    double totalPromptRate = 0.0;
    int pairCount = 0;
    for (size_t i = 1; i < snapshots.size(); ++i) {
        const double timeDelta = static_cast<double>(snapshots[i].eventTime - snapshots[i - 1].eventTime);
        REQUIRE(timeDelta == 60.0);
        const long long tokenDelta = snapshots[i].totalTokens - snapshots[i - 1].totalTokens;
        REQUIRE(tokenDelta == 150);
        totalTokenRate += (static_cast<double>(tokenDelta) / timeDelta) * 60.0;
        totalPromptRate += (1.0 / timeDelta) * 60.0;
        ++pairCount;
    }
    REQUIRE(pairCount == 2);
    REQUIRE((totalTokenRate / pairCount) == Catch::Approx(150.0));
    REQUIRE((totalPromptRate / pairCount) == Catch::Approx(1.0));
}
