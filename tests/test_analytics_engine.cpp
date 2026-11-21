#include <catch_amalgamated.hpp>
#include "analytics/AnalyticsEngine.hpp"
#include "providers/IProviderAdapter.hpp"

using namespace JiStats;

// Use a fixed base time so tests are deterministic
// 2026-01-15 12:00:00 UTC (approximate)
static const std::time_t BASE_TIME = 1768512000;
static const std::time_t ONE_HOUR = 3600;
static const std::time_t ONE_DAY = 86400;

static UsageEvent makeEvent(std::time_t time, const std::string& model,
                            long long tokens, long long cost) {
    UsageEvent e;
    e.eventTime = time;
    e.modelKey = model;
    e.totalTokens = tokens;
    e.inputTokens = tokens * 3 / 4;
    e.outputTokens = tokens / 4;
    e.costMinorUnits = cost;
    e.sourceType = "api";
    e.requestType = "chat";
    return e;
}

TEST_CASE("AnalyticsEngine::computeDailyUsage groups by date", "[analytics]") {
    AnalyticsEngine engine;
    std::vector<UsageEvent> events = {
        makeEvent(BASE_TIME, "gpt-4", 1000, 10),
        makeEvent(BASE_TIME + ONE_HOUR, "gpt-4", 2000, 20),
        makeEvent(BASE_TIME + ONE_DAY, "gpt-4", 3000, 30),
    };

    auto daily = engine.computeDailyUsage(events, "openai",
        BASE_TIME - ONE_HOUR, BASE_TIME + 2 * ONE_DAY);

    REQUIRE(daily.size() == 2);
    // First day: 1000 + 2000 = 3000 tokens
    REQUIRE(daily[0].totalTokens == 3000);
    REQUIRE(daily[0].requestCount == 2);
    // Second day: 3000 tokens
    REQUIRE(daily[1].totalTokens == 3000);
    REQUIRE(daily[1].requestCount == 1);
}

TEST_CASE("AnalyticsEngine::computeModelUsage groups by model", "[analytics]") {
    AnalyticsEngine engine;
    std::vector<UsageEvent> events = {
        makeEvent(BASE_TIME, "gpt-4", 5000, 50),
        makeEvent(BASE_TIME + 100, "gpt-3.5", 2000, 5),
        makeEvent(BASE_TIME + 200, "gpt-4", 3000, 30),
    };

    auto models = engine.computeModelUsage(events,
        BASE_TIME - 1, BASE_TIME + ONE_HOUR);

    REQUIRE(models.size() == 2);
    // Sorted by tokens descending: gpt-4 (8000) first
    REQUIRE(models[0].modelKey == "gpt-4");
    REQUIRE(models[0].totalTokens == 8000);
    REQUIRE(models[0].requestCount == 2);
    REQUIRE(models[1].modelKey == "gpt-3.5");
    REQUIRE(models[1].totalTokens == 2000);
}

TEST_CASE("AnalyticsEngine::getUsageTrend", "[analytics]") {
    AnalyticsEngine engine;

    SECTION("Increasing when current > previous by >10%") {
        std::vector<UsageEvent> events = {
            makeEvent(BASE_TIME, "gpt-4", 100, 1),           // previous period
            makeEvent(BASE_TIME + ONE_DAY, "gpt-4", 200, 2), // current period
        };
        auto trend = engine.getUsageTrend(events,
            BASE_TIME + ONE_DAY - 1, BASE_TIME + 2 * ONE_DAY,
            BASE_TIME - 1, BASE_TIME + ONE_DAY - 2);
        REQUIRE(trend == AnalyticsEngine::Trend::Increasing);
    }

    SECTION("Decreasing when current < previous by >10%") {
        std::vector<UsageEvent> events = {
            makeEvent(BASE_TIME, "gpt-4", 200, 2),           // previous period
            makeEvent(BASE_TIME + ONE_DAY, "gpt-4", 50, 1),  // current period
        };
        auto trend = engine.getUsageTrend(events,
            BASE_TIME + ONE_DAY - 1, BASE_TIME + 2 * ONE_DAY,
            BASE_TIME - 1, BASE_TIME + ONE_DAY - 2);
        REQUIRE(trend == AnalyticsEngine::Trend::Decreasing);
    }

    SECTION("Stable when difference <= 10%") {
        std::vector<UsageEvent> events = {
            makeEvent(BASE_TIME, "gpt-4", 100, 1),
            makeEvent(BASE_TIME + ONE_DAY, "gpt-4", 105, 1),
        };
        auto trend = engine.getUsageTrend(events,
            BASE_TIME + ONE_DAY - 1, BASE_TIME + 2 * ONE_DAY,
            BASE_TIME - 1, BASE_TIME + ONE_DAY - 2);
        REQUIRE(trend == AnalyticsEngine::Trend::Stable);
    }
}

TEST_CASE("AnalyticsEngine::computePeakHours covers all 24 hours", "[analytics]") {
    AnalyticsEngine engine;
    std::vector<UsageEvent> events = {
        makeEvent(BASE_TIME, "gpt-4", 1000, 10), // just one event
    };

    auto hours = engine.computePeakHours(events,
        BASE_TIME - ONE_DAY, BASE_TIME + ONE_DAY);

    REQUIRE(hours.size() == 24);
}

TEST_CASE("AnalyticsEngine::estimateMonthlyCost projects linearly", "[analytics]") {
    AnalyticsEngine engine;
    // 10 events over 1 day, each costing 10 = 100/day
    std::vector<UsageEvent> events;
    for (int i = 0; i < 10; ++i) {
        events.push_back(makeEvent(BASE_TIME + i * ONE_HOUR, "gpt-4", 1000, 10));
    }

    long long monthly = engine.estimateMonthlyCost(events,
        BASE_TIME - 1, BASE_TIME + ONE_DAY);

    // Total cost in period: 100, period ~1 day, projection: 100 * 30 = ~3000
    // Allow some margin for rounding
    REQUIRE(monthly > 2000);
    REQUIRE(monthly < 4000);
}

TEST_CASE("AnalyticsEngine::computeDailyUsage with empty events", "[analytics]") {
    AnalyticsEngine engine;
    std::vector<UsageEvent> events;

    auto daily = engine.computeDailyUsage(events, "openai",
        BASE_TIME, BASE_TIME + ONE_DAY);

    REQUIRE(daily.empty());
}
