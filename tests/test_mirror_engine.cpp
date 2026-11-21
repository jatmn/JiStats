#include <catch_amalgamated.hpp>
#include "analytics/MirrorEngine.hpp"
#include "providers/IProviderAdapter.hpp"

using namespace JiStats;

static UsageEvent makeEvent(std::time_t time, long long tokens, long long cost) {
    UsageEvent e;
    e.eventTime = time;
    e.modelKey = "gpt-4";
    e.totalTokens = tokens;
    e.costMinorUnits = cost;
    e.sourceType = "api";
    return e;
}

static UsageEvent makeReplayEvent(
    std::time_t time,
    long long inputTokens,
    long long outputTokens,
    long long cost,
    const std::string& sourceType,
    const std::string& sourceRunId
) {
    UsageEvent e;
    e.eventTime = time;
    e.modelKey = "gpt-4.1";
    e.requestType = "responses.create";
    e.inputTokens = inputTokens;
    e.outputTokens = outputTokens;
    e.totalTokens = inputTokens + outputTokens;
    e.costMinorUnits = cost;
    e.sourceType = sourceType;
    e.sourceRunId = sourceRunId;
    e.rawReference = "fixture://mirror-replay";
    return e;
}

TEST_CASE("MirrorEngine::computeMirror with valid events", "[mirror]") {
    MirrorEngine engine;
    MirrorInput input;
    input.providerKey = "openai";
    input.windowStart = 1000;
    input.windowEnd = 2000;
    input.localEvents = { makeEvent(1200, 500, 10), makeEvent(1500, 300, 5) };

    MirrorOutput output = engine.computeMirror(input);
    REQUIRE(output.success);
    REQUIRE(output.error.empty());
}

TEST_CASE("MirrorEngine::computeMirror with empty events", "[mirror]") {
    MirrorEngine engine;
    MirrorInput input;
    input.providerKey = "openai";
    input.windowStart = 1000;
    input.windowEnd = 2000;

    MirrorOutput output = engine.computeMirror(input);
    REQUIRE(output.success);
}

TEST_CASE("MirrorEngine::computeMirror with invalid time window", "[mirror]") {
    MirrorEngine engine;
    MirrorInput input;
    input.providerKey = "openai";
    input.windowStart = 2000;
    input.windowEnd = 1000;

    MirrorOutput output = engine.computeMirror(input);
    REQUIRE_FALSE(output.success);
    REQUIRE(output.error == "Invalid time window");
}

TEST_CASE("MirrorEngine::computeMirror with equal start/end", "[mirror]") {
    MirrorEngine engine;
    MirrorInput input;
    input.windowStart = 1000;
    input.windowEnd = 1000;

    MirrorOutput output = engine.computeMirror(input);
    REQUIRE_FALSE(output.success);
}

TEST_CASE("MirrorEngine::classifyConfidence", "[mirror]") {
    QuotaResponse successQuota;
    successQuota.success = true;

    QuotaResponse failQuota;
    failQuota.success = false;

    SECTION("High when provider quota succeeds") {
        auto tier = MirrorEngine::classifyConfidence(successQuota, 5, false);
        REQUIRE(tier == MirrorEngine::ConfidenceTier::High);
    }

    SECTION("High when provider succeeds regardless of other data") {
        auto tier = MirrorEngine::classifyConfidence(successQuota, 0, false);
        REQUIRE(tier == MirrorEngine::ConfidenceTier::High);
    }

    SECTION("Medium when has imported data and local events") {
        auto tier = MirrorEngine::classifyConfidence(failQuota, 5, true);
        REQUIRE(tier == MirrorEngine::ConfidenceTier::Medium);
    }

    SECTION("Medium when has local events only") {
        auto tier = MirrorEngine::classifyConfidence(failQuota, 5, false);
        REQUIRE(tier == MirrorEngine::ConfidenceTier::Medium);
    }

    SECTION("Low with no provider, no imports, no local events") {
        auto tier = MirrorEngine::classifyConfidence(failQuota, 0, false);
        REQUIRE(tier == MirrorEngine::ConfidenceTier::Low);
    }
}

TEST_CASE("MirrorEngine::tierToString", "[mirror]") {
    REQUIRE(MirrorEngine::tierToString(MirrorEngine::ConfidenceTier::High) == "High");
    REQUIRE(MirrorEngine::tierToString(MirrorEngine::ConfidenceTier::Medium) == "Medium");
    REQUIRE(MirrorEngine::tierToString(MirrorEngine::ConfidenceTier::Low) == "Low");
}

TEST_CASE("MirrorEngine::getRollingWindow filters by time range", "[mirror]") {
    MirrorEngine engine;
    std::vector<UsageEvent> events = {
        makeEvent(500, 100, 1),   // before window
        makeEvent(1200, 200, 2),  // in window
        makeEvent(1500, 300, 3),  // in window
        makeEvent(2500, 400, 4),  // after window
    };

    auto stats = engine.getRollingWindow(events, 1000, 2000);
    REQUIRE(stats.eventCount == 2);
    REQUIRE(stats.totalUsage == 500);
    REQUIRE(stats.totalCost == 5);
}

TEST_CASE("MirrorEngine::getRollingWindow confidence scoring", "[mirror]") {
    MirrorEngine engine;

    SECTION("0 events = 0.0 confidence") {
        std::vector<UsageEvent> events;
        auto stats = engine.getRollingWindow(events, 1000, 2000);
        REQUIRE(stats.confidenceScore == 0.0);
    }

    SECTION("<5 events = 0.5 confidence") {
        std::vector<UsageEvent> events = {
            makeEvent(1100, 100, 1),
            makeEvent(1200, 100, 1),
            makeEvent(1300, 100, 1),
        };
        auto stats = engine.getRollingWindow(events, 1000, 2000);
        REQUIRE(stats.confidenceScore == 0.5);
    }

    SECTION("5-19 events = 0.75 confidence") {
        std::vector<UsageEvent> events;
        for (int i = 0; i < 10; ++i) {
            events.push_back(makeEvent(1000 + i * 50, 100, 1));
        }
        auto stats = engine.getRollingWindow(events, 1000, 2000);
        REQUIRE(stats.confidenceScore == 0.75);
    }

    SECTION("20+ events = 1.0 confidence") {
        std::vector<UsageEvent> events;
        for (int i = 0; i < 25; ++i) {
            events.push_back(makeEvent(1000 + i * 30, 100, 1));
        }
        auto stats = engine.getRollingWindow(events, 1000, 2000);
        REQUIRE(stats.confidenceScore == 1.0);
    }
}

TEST_CASE("MirrorEngine replay local-only rolling window uses only in-window events", "[mirror][replay]") {
    MirrorEngine engine;
    const std::time_t windowStart = 1'710'000'000;
    const std::time_t windowEnd = windowStart + 300;

    std::vector<UsageEvent> replay = {
        makeReplayEvent(windowStart - 1, 70, 30, 4, "api", "run-before"),
        makeReplayEvent(windowStart, 120, 30, 8, "api", "run-start"),
        makeReplayEvent(windowStart + 120, 80, 20, 6, "api", "run-middle"),
        makeReplayEvent(windowEnd, 50, 10, 3, "api", "run-end"),
        makeReplayEvent(windowEnd + 1, 100, 50, 9, "api", "run-after")
    };

    auto stats = engine.getRollingWindow(replay, windowStart, windowEnd);

    REQUIRE(stats.windowStart == windowStart);
    REQUIRE(stats.windowEnd == windowEnd);
    REQUIRE(stats.eventCount == 3);
    REQUIRE(stats.totalUsage == 310);
    REQUIRE(stats.totalCost == 17);
    REQUIRE(stats.confidenceScore == 0.5);
}

TEST_CASE("MirrorEngine replay provider quota plus local events favors fresher in-window local overlay", "[mirror][replay]") {
    MirrorEngine engine;
    const std::time_t windowStart = 1'710'086'400;
    const std::time_t windowEnd = windowStart + 900;

    QuotaResponse quota;
    quota.success = true;
    quota.snapshotTime = windowEnd;
    quota.windowStart = windowStart;
    quota.windowEnd = windowEnd;
    quota.used = 600;
    quota.limit = 1000;
    quota.remaining = 400;
    quota.windowType = "rolling";

    std::vector<UsageEvent> localEvents = {
        makeReplayEvent(windowStart + 10, 200, 100, 10, "api", "run-001"),
        makeReplayEvent(windowStart + 400, 150, 120, 11, "api", "run-002"),
        makeReplayEvent(windowStart - 60, 999, 1, 99, "api", "run-old")
    };
    std::vector<UsageEvent> importedEvents = {
        makeReplayEvent(windowStart + 50, 80, 20, 5, "imported", "import-001"),
        makeReplayEvent(windowEnd + 1, 1, 1, 1, "imported", "import-after")
    };

    auto output = engine.reconcile(localEvents, importedEvents, quota, windowStart, windowEnd);

    REQUIRE(output.success);
    REQUIRE(output.tier == MirrorEngine::ConfidenceTier::High);
    REQUIRE(output.basisSummary == "Provider quota reconciled with local events");
    REQUIRE(output.snapshot.windowStart == windowStart);
    REQUIRE(output.snapshot.windowEnd == windowEnd);
    REQUIRE(output.snapshot.limit == 1000);
    REQUIRE(output.snapshot.used == 670);
    REQUIRE(output.snapshot.remaining == 330);
    REQUIRE(output.snapshot.events.size() == 5);
    REQUIRE(output.snapshot.events.front().sourceRunId == "run-old");
    REQUIRE(output.snapshot.events.back().sourceRunId == "import-after");
}

TEST_CASE("MirrorEngine replay provider quota remains source of truth when local overlay stays below provider use", "[mirror][replay]") {
    MirrorEngine engine;
    const std::time_t windowStart = 1'710'172'800;
    const std::time_t windowEnd = windowStart + 600;

    QuotaResponse quota;
    quota.success = true;
    quota.used = 900;
    quota.limit = 1200;
    quota.remaining = 300;
    quota.windowType = "rolling";

    std::vector<UsageEvent> localEvents = {
        makeReplayEvent(windowStart + 30, 100, 50, 6, "api", "run-101"),
        makeReplayEvent(windowStart + 300, 80, 40, 5, "api", "run-102")
    };
    std::vector<UsageEvent> importedEvents = {
        makeReplayEvent(windowStart + 200, 60, 20, 3, "imported", "import-101")
    };

    auto output = engine.reconcile(localEvents, importedEvents, quota, windowStart, windowEnd);

    REQUIRE(output.success);
    REQUIRE(output.tier == MirrorEngine::ConfidenceTier::High);
    REQUIRE(output.snapshot.used == 900);
    REQUIRE(output.snapshot.limit == 1200);
    REQUIRE(output.snapshot.remaining == 300);
}

TEST_CASE("MirrorEngine replay confidence classification edges stay deterministic", "[mirror][replay]") {
    QuotaResponse providerSuccess;
    providerSuccess.success = true;

    QuotaResponse providerUnavailable;
    providerUnavailable.success = false;

    SECTION("Imported-only replay still classifies low without provider success or local events") {
        auto tier = MirrorEngine::classifyConfidence(providerUnavailable, 0, true);
        REQUIRE(tier == MirrorEngine::ConfidenceTier::Low);
    }

    SECTION("First local event moves replay from low to medium") {
        auto tier = MirrorEngine::classifyConfidence(providerUnavailable, 1, false);
        REQUIRE(tier == MirrorEngine::ConfidenceTier::Medium);
    }

    SECTION("Provider success keeps classification high even with no replayed events") {
        auto tier = MirrorEngine::classifyConfidence(providerSuccess, 0, true);
        REQUIRE(tier == MirrorEngine::ConfidenceTier::High);
    }

    SECTION("Rolling confidence score changes exactly at event-count boundaries") {
        MirrorEngine engine;
        const std::time_t windowStart = 2'000;
        const std::time_t windowEnd = 3'000;

        std::vector<UsageEvent> fourEvents;
        std::vector<UsageEvent> fiveEvents;
        std::vector<UsageEvent> nineteenEvents;
        std::vector<UsageEvent> twentyEvents;

        for (int i = 0; i < 20; ++i) {
            UsageEvent event = makeReplayEvent(
                windowStart + 10 + i,
                10,
                5,
                1,
                "api",
                "edge-" + std::to_string(i)
            );

            if (i < 4) {
                fourEvents.push_back(event);
            }
            if (i < 5) {
                fiveEvents.push_back(event);
            }
            if (i < 19) {
                nineteenEvents.push_back(event);
            }
            twentyEvents.push_back(event);
        }

        REQUIRE(engine.getRollingWindow(fourEvents, windowStart, windowEnd).confidenceScore == 0.5);
        REQUIRE(engine.getRollingWindow(fiveEvents, windowStart, windowEnd).confidenceScore == 0.75);
        REQUIRE(engine.getRollingWindow(nineteenEvents, windowStart, windowEnd).confidenceScore == 0.75);
        REQUIRE(engine.getRollingWindow(twentyEvents, windowStart, windowEnd).confidenceScore == 1.0);
    }
}

TEST_CASE("MirrorEngine::reconcile with provider quota", "[mirror]") {
    MirrorEngine engine;
    QuotaResponse quota;
    quota.success = true;
    quota.used = 1000;
    quota.limit = 5000;
    quota.remaining = 4000;

    std::vector<UsageEvent> local = { makeEvent(1200, 500, 5) };
    std::vector<UsageEvent> imported;

    auto output = engine.reconcile(local, imported, quota, 1000, 2000);
    REQUIRE(output.success);
    REQUIRE(output.tier == MirrorEngine::ConfidenceTier::High);
    REQUIRE(output.snapshot.limit == 5000);
}

TEST_CASE("MirrorEngine::reconcile local exceeds provider", "[mirror]") {
    MirrorEngine engine;
    QuotaResponse quota;
    quota.success = true;
    quota.used = 100;
    quota.limit = 5000;
    quota.remaining = 4900;

    // Local events total 500 tokens, exceeding provider's 100
    std::vector<UsageEvent> local = { makeEvent(1200, 500, 5) };
    std::vector<UsageEvent> imported;

    auto output = engine.reconcile(local, imported, quota, 1000, 2000);
    REQUIRE(output.success);
    REQUIRE(output.snapshot.used == 500); // local wins
    REQUIRE(output.snapshot.remaining == 4500);
}

TEST_CASE("MirrorEngine::reconcile without provider quota", "[mirror]") {
    MirrorEngine engine;
    QuotaResponse quota;
    quota.success = false;

    std::vector<UsageEvent> local = { makeEvent(1200, 500, 5) };
    std::vector<UsageEvent> imported;

    auto output = engine.reconcile(local, imported, quota, 1000, 2000);
    REQUIRE(output.success);
    REQUIRE(output.tier == MirrorEngine::ConfidenceTier::Medium);
    REQUIRE(output.snapshot.used == 500);
    REQUIRE(output.snapshot.limit == 0);
}

TEST_CASE("MirrorEngine::reconcile no events no quota", "[mirror]") {
    MirrorEngine engine;
    QuotaResponse quota;
    quota.success = false;

    std::vector<UsageEvent> local;
    std::vector<UsageEvent> imported;

    auto output = engine.reconcile(local, imported, quota, 1000, 2000);
    REQUIRE(output.success);
    REQUIRE(output.tier == MirrorEngine::ConfidenceTier::Low);
}
