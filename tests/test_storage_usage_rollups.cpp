#include <catch_amalgamated.hpp>

#include <filesystem>
#include <string>

#include "db/StorageService.hpp"
#include "sqlite_test_support.hpp"
#include "temp_test_directory.hpp"

using namespace JiStats;
using JiStats::TestSupport::TempTestDatabase;
using JiStats::TestSupport::requireSqlite;

namespace {

long long requireSingleId(const QueryResult& result) {
    REQUIRE(result.success());
    REQUIRE(result.rowCount() == 1);
    return result.getInt64(0, 0);
}

struct StorageIds {
    int providerId = 0;
    int accountId = 0;
};

StorageIds seedProviderAccount(StorageService& storage) {
    StorageIds ids;
    ids.providerId = static_cast<int>(requireSingleId(
        storage.execute("SELECT id FROM providers WHERE provider_key = 'openai';")));

    auto accountInsert = storage.execute(
        "INSERT INTO accounts (provider_id, account_key, display_name, scope_type, scope_identifier) "
        "VALUES (?, ?, ?, ?, ?);",
        {
            std::to_string(ids.providerId),
            "acct-rollup-test",
            "Rollup Test Account",
            "user",
            "user-rollup-test"
        });
    REQUIRE(accountInsert.success());
    ids.accountId = static_cast<int>(accountInsert.lastInsertRowId());
    return ids;
}

void insertUsageEvent(
    StorageService& storage,
    int providerId,
    int accountId,
    const std::string& eventTimeUtc,
    const std::string& modelKey,
    int inputTokens,
    int outputTokens,
    int totalTokens,
    int costMinorUnits,
    const std::string& sourceType = "api",
    bool isBackfilled = false) {
    auto insert = storage.execute(
        "INSERT INTO usage_events ("
        "provider_id, account_id, event_time_utc, source_type, model_key, request_type, "
        "input_tokens, output_tokens, total_tokens, cost_minor_units, is_backfilled"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
        {
            std::to_string(providerId),
            std::to_string(accountId),
            eventTimeUtc,
            sourceType,
            modelKey,
            "chat",
            std::to_string(inputTokens),
            std::to_string(outputTokens),
            std::to_string(totalTokens),
            std::to_string(costMinorUnits),
            isBackfilled ? "1" : "0"
        });
    REQUIRE(insert.success());
}

constexpr std::time_t HOUR_ZERO = 1768438800; // 2026-01-15T01:00:00Z
constexpr std::time_t NEXT_HOUR = HOUR_ZERO + 3600;
constexpr std::time_t NEXT_DAY = HOUR_ZERO + 86400;

} // namespace

TEST_CASE("StorageService rebuilds hourly and daily usage rollups when SQLite is available", "[storage][rollups]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    const auto ids = seedProviderAccount(storage);
    insertUsageEvent(storage, ids.providerId, ids.accountId, "2026-01-15T01:15:00Z", "gpt-4", 60, 40, 100, 10);
    insertUsageEvent(storage, ids.providerId, ids.accountId, "2026-01-15T01:45:00Z", "gpt-4", 90, 110, 200, 20);
    insertUsageEvent(storage, ids.providerId, ids.accountId, "2026-01-15T02:10:00Z", "gpt-4o-mini", 120, 180, 300, 30);
    insertUsageEvent(storage, ids.providerId, ids.accountId, "2026-01-16T01:05:00Z", "gpt-4", 200, 200, 400, 40);

    REQUIRE(storage.rebuildUsageRollups(
        UsageRollupResolution::Hourly,
        ids.providerId,
        ids.accountId,
        HOUR_ZERO,
        NEXT_DAY + 7200));

    const auto hourly = storage.getUsageRollups(
        UsageRollupResolution::Hourly,
        ids.providerId,
        ids.accountId,
        HOUR_ZERO,
        NEXT_DAY + 7200);

    REQUIRE(hourly.size() == 3);
    CHECK(hourly[0].periodStart == HOUR_ZERO);
    CHECK(hourly[0].modelKey == "gpt-4");
    CHECK(hourly[0].requestCount == 2);
    CHECK(hourly[0].inputTokens == 150);
    CHECK(hourly[0].outputTokens == 150);
    CHECK(hourly[0].totalTokens == 300);
    CHECK(hourly[0].totalCostMinorUnits == 30);
    CHECK(hourly[1].periodStart == NEXT_HOUR);
    CHECK(hourly[1].modelKey == "gpt-4o-mini");
    CHECK(hourly[1].requestCount == 1);
    CHECK(hourly[1].totalTokens == 300);
    CHECK(hourly[2].periodStart == NEXT_DAY);
    CHECK(hourly[2].modelKey == "gpt-4");
    CHECK(hourly[2].requestCount == 1);
    CHECK(hourly[2].totalTokens == 400);

    REQUIRE(storage.rebuildUsageRollups(
        UsageRollupResolution::Daily,
        ids.providerId,
        ids.accountId,
        HOUR_ZERO,
        NEXT_DAY + 7200));

    const auto daily = storage.getUsageRollups(
        UsageRollupResolution::Daily,
        ids.providerId,
        ids.accountId,
        HOUR_ZERO - 3600,
        NEXT_DAY + 7200);

    REQUIRE(daily.size() == 3);
    CHECK(daily[0].periodStart == HOUR_ZERO - 3600);
    CHECK(daily[0].modelKey == "gpt-4");
    CHECK(daily[0].requestCount == 2);
    CHECK(daily[0].totalTokens == 300);
    CHECK(daily[1].periodStart == HOUR_ZERO - 3600);
    CHECK(daily[1].modelKey == "gpt-4o-mini");
    CHECK(daily[1].requestCount == 1);
    CHECK(daily[1].totalTokens == 300);
    CHECK(daily[2].periodStart == NEXT_DAY - 3600);
    CHECK(daily[2].modelKey == "gpt-4");
    CHECK(daily[2].requestCount == 1);
    CHECK(daily[2].totalTokens == 400);
}

TEST_CASE("StorageService rebuilds complete affected rollup buckets for partial time windows", "[storage][rollups]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    const auto ids = seedProviderAccount(storage);
    insertUsageEvent(storage, ids.providerId, ids.accountId, "2026-01-15T01:05:00Z", "gpt-4", 10, 10, 20, 2);
    insertUsageEvent(storage, ids.providerId, ids.accountId, "2026-01-15T01:35:00Z", "gpt-4", 20, 20, 40, 4);
    insertUsageEvent(storage, ids.providerId, ids.accountId, "2026-01-15T01:55:00Z", "gpt-4", 30, 30, 60, 6);

    REQUIRE(storage.rebuildUsageRollups(
        UsageRollupResolution::Hourly,
        ids.providerId,
        ids.accountId,
        HOUR_ZERO,
        HOUR_ZERO + 3599));

    auto hourly = storage.getUsageRollups(
        UsageRollupResolution::Hourly,
        ids.providerId,
        ids.accountId,
        HOUR_ZERO,
        HOUR_ZERO);
    REQUIRE(hourly.size() == 1);
    CHECK(hourly.front().requestCount == 3);
    CHECK(hourly.front().totalTokens == 120);

    REQUIRE(storage.rebuildUsageRollups(
        UsageRollupResolution::Hourly,
        ids.providerId,
        ids.accountId,
        HOUR_ZERO + 1800,
        HOUR_ZERO + 2400));

    hourly = storage.getUsageRollups(
        UsageRollupResolution::Hourly,
        ids.providerId,
        ids.accountId,
        HOUR_ZERO,
        HOUR_ZERO);
    REQUIRE(hourly.size() == 1);
    CHECK(hourly.front().requestCount == 3);
    CHECK(hourly.front().totalTokens == 120);
}

TEST_CASE("StorageService rollups preserve request provenance counters for analysis reporting", "[storage][rollups][analysis]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    const auto ids = seedProviderAccount(storage);
    insertUsageEvent(
        storage,
        ids.providerId,
        ids.accountId,
        "2026-01-15T01:05:00Z",
        "gpt-4",
        20,
        10,
        30,
        3,
        "local_runtime",
        false);
    insertUsageEvent(
        storage,
        ids.providerId,
        ids.accountId,
        "2026-01-15T01:15:00Z",
        "gpt-4",
        30,
        20,
        50,
        5,
        "provider_backfill",
        true);
    insertUsageEvent(
        storage,
        ids.providerId,
        ids.accountId,
        "2026-01-15T01:25:00Z",
        "gpt-4",
        40,
        30,
        70,
        7,
        "api",
        false);
    insertUsageEvent(
        storage,
        ids.providerId,
        ids.accountId,
        "2026-01-15T01:35:00Z",
        "gpt-4",
        10,
        10,
        20,
        2,
        "",
        false);
    insertUsageEvent(
        storage,
        ids.providerId,
        ids.accountId,
        "2026-01-15T01:45:00Z",
        "gpt-4",
        50,
        50,
        100,
        10,
        "codex_local_rollout",
        true);

    REQUIRE(storage.rebuildUsageRollups(
        UsageRollupResolution::Hourly,
        ids.providerId,
        ids.accountId,
        HOUR_ZERO,
        NEXT_HOUR));

    const auto hourly = storage.getUsageRollups(
        UsageRollupResolution::Hourly,
        ids.providerId,
        ids.accountId,
        HOUR_ZERO,
        HOUR_ZERO);

    REQUIRE(hourly.size() == 1);
    CHECK(hourly.front().requestCount == 4);
    CHECK(hourly.front().localRequestCount == 1);
    CHECK(hourly.front().importedRequestCount == 1);
    CHECK(hourly.front().apiRequestCount == 1);
    CHECK(hourly.front().unknownRequestCount == 1);
    CHECK(hourly.front().backfilledRequestCount == 1);
    CHECK(hourly.front().totalTokens == 170);

    REQUIRE(storage.rebuildUsageRollups(
        UsageRollupResolution::Daily,
        ids.providerId,
        ids.accountId,
        HOUR_ZERO,
        NEXT_HOUR));

    const auto daily = storage.getUsageRollups(
        UsageRollupResolution::Daily,
        ids.providerId,
        ids.accountId,
        HOUR_ZERO - 3600,
        HOUR_ZERO - 3600);

    REQUIRE(daily.size() == 1);
    CHECK(daily.front().requestCount == 4);
    CHECK(daily.front().localRequestCount == 1);
    CHECK(daily.front().importedRequestCount == 1);
    CHECK(daily.front().apiRequestCount == 1);
    CHECK(daily.front().unknownRequestCount == 1);
    CHECK(daily.front().backfilledRequestCount == 1);
    CHECK(daily.front().totalTokens == 170);
}

TEST_CASE("StorageService rebuilds daily quota snapshot rollups for long-range analysis", "[storage][rollups][quota]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    const auto ids = seedProviderAccount(storage);
    const auto planInsert = storage.execute(
        "INSERT INTO plans (account_id, plan_key, plan_type, display_name, quota_window, limit_value) "
        "VALUES (?, ?, ?, ?, ?, ?);",
        {
            std::to_string(ids.accountId),
            "openai_subscription_5h",
            "subscription",
            "OpenAI 5h",
            "rolling",
            "1000"
        });
    REQUIRE(planInsert.success());
    const long long planId = planInsert.lastInsertRowId();

    auto insertSnapshot = [&](const char* snapshotTime,
                              const char* windowStart,
                              const char* windowEnd,
                              const char* usedValue,
                              const char* remainingValue,
                              const char* reconciled) {
        const auto insert = storage.execute(
            "INSERT INTO quota_snapshots ("
            "provider_id, account_id, plan_id, snapshot_time_utc, window_start_utc, window_end_utc, "
            "used_value, limit_value, confidence_tier, basis_summary, quota_dimension, remaining_value, is_reconciled"
            ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
            {
                std::to_string(ids.providerId),
                std::to_string(ids.accountId),
                std::to_string(planId),
                snapshotTime,
                windowStart,
                windowEnd,
                usedValue,
                "1000",
                "high",
                "Quota rollup test snapshot",
                "requests",
                remainingValue,
                reconciled
            });
        REQUIRE(insert.success());
    };

    insertSnapshot("2026-01-15T08:00:00Z", "2026-01-15T05:00:00Z", "2026-01-15T10:00:00Z", "100", "900", "0");
    insertSnapshot("2026-01-15T09:30:00Z", "2026-01-15T05:00:00Z", "2026-01-15T10:00:00Z", "450", "550", "1");
    insertSnapshot("2026-01-15T11:00:00Z", "2026-01-15T10:00:00Z", "2026-01-15T15:00:00Z", "200", "800", "1");
    insertSnapshot("2026-01-16T09:00:00Z", "2026-01-16T05:00:00Z", "2026-01-16T10:00:00Z", "300", "700", "0");

    REQUIRE(storage.rebuildQuotaSnapshotRollups(
        ids.providerId,
        ids.accountId,
        HOUR_ZERO - 3600,
        NEXT_DAY + 3600));

    const auto rollups = storage.getQuotaSnapshotRollups(
        ids.providerId,
        ids.accountId,
        HOUR_ZERO - 3600,
        NEXT_DAY - 3600);

    REQUIRE(rollups.size() == 2);

    CHECK(rollups[0].periodStart == HOUR_ZERO - 3600);
    REQUIRE(rollups[0].planId.has_value());
    CHECK(*rollups[0].planId == planId);
    CHECK(rollups[0].quotaDimension == "requests");
    CHECK(rollups[0].sampleCount == 3);
    CHECK(rollups[0].latestSnapshotTime == HOUR_ZERO + (10 * 3600));
    CHECK(rollups[0].latestUsedValue == 200);
    REQUIRE(rollups[0].latestLimitValue.has_value());
    CHECK(*rollups[0].latestLimitValue == 1000);
    REQUIRE(rollups[0].latestRemainingValue.has_value());
    CHECK(*rollups[0].latestRemainingValue == 800);
    REQUIRE(rollups[0].lowWaterRemainingValue.has_value());
    CHECK(*rollups[0].lowWaterRemainingValue == 550);
    REQUIRE(rollups[0].lowWaterLimitValue.has_value());
    CHECK(*rollups[0].lowWaterLimitValue == 1000);
    CHECK(rollups[0].resetCount == 1);
    CHECK(rollups[0].reconciledSampleCount == 2);
    CHECK(rollups[0].latestConfidenceTier == "high");

    CHECK(rollups[1].periodStart == NEXT_DAY - 3600);
    CHECK(rollups[1].sampleCount == 1);
    CHECK(rollups[1].resetCount == 0);
    REQUIRE(rollups[1].latestRemainingValue.has_value());
    CHECK(*rollups[1].latestRemainingValue == 700);
}

TEST_CASE("StorageService retention helpers delete expired usage events and rollups when SQLite is available", "[storage][rollups][retention]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    const auto ids = seedProviderAccount(storage);
    insertUsageEvent(storage, ids.providerId, ids.accountId, "2026-01-15T01:15:00Z", "gpt-4", 60, 40, 100, 10);
    insertUsageEvent(storage, ids.providerId, ids.accountId, "2026-01-16T01:05:00Z", "gpt-4", 200, 200, 400, 40);

    REQUIRE(storage.rebuildUsageRollups(
        UsageRollupResolution::Hourly,
        ids.providerId,
        ids.accountId,
        HOUR_ZERO,
        NEXT_DAY + 3600));
    REQUIRE(storage.rebuildUsageRollups(
        UsageRollupResolution::Daily,
        ids.providerId,
        ids.accountId,
        HOUR_ZERO,
        NEXT_DAY + 3600));

    REQUIRE(storage.deleteUsageEventsOlderThan(NEXT_DAY));
    auto remainingEvents = storage.execute(
        "SELECT COUNT(*) FROM usage_events WHERE provider_id = ? AND account_id = ?;",
        {std::to_string(ids.providerId), std::to_string(ids.accountId)});
    REQUIRE(remainingEvents.success());
    REQUIRE(remainingEvents.rowCount() == 1);
    CHECK(remainingEvents.getInt(0, 0) == 1);

    REQUIRE(storage.deleteUsageRollupsOlderThan(UsageRollupResolution::Hourly, NEXT_DAY));
    const auto hourly = storage.getUsageRollups(
        UsageRollupResolution::Hourly,
        ids.providerId,
        ids.accountId,
        HOUR_ZERO,
        NEXT_DAY + 3600);
    REQUIRE(hourly.size() == 1);
    CHECK(hourly.front().periodStart == NEXT_DAY);

    REQUIRE(storage.deleteUsageRollupsOlderThan(UsageRollupResolution::Daily, NEXT_DAY));
    const auto daily = storage.getUsageRollups(
        UsageRollupResolution::Daily,
        ids.providerId,
        ids.accountId,
        HOUR_ZERO - 3600,
        NEXT_DAY + 3600);
    REQUIRE(daily.size() == 1);
    CHECK(daily.front().periodStart == NEXT_DAY - 3600);
}
