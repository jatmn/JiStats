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

PersistedActivityEvent makePersistedActivityEvent(int providerId, int accountId, std::optional<int> planId = std::nullopt) {
    PersistedActivityEvent event;
    event.providerId = providerId;
    event.accountId = accountId;
    event.planId = planId;
    event.activity.providerKey = "openai";
    event.activity.providerSurface = "openai_subscription";
    event.activity.activityKind = ActivityKind::SessionActive;
    event.activity.startedAt = 1713416400;
    event.activity.endedAt = 1713416460;
    event.activity.durationSeconds = 60;
    event.activity.sourceType = "local";
    event.activity.sourceRunId = "run-activity-1";
    event.activity.surfaceKey = "foreground_window";
    event.activity.windowTitleHint = "ChatGPT";
    event.activity.confidence = ActivityConfidence::LocalExact;
    event.activity.rawReference = "window:chatgpt";
    return event;
}

PersistedUsageEvent makePersistedUsageEvent(int providerId, int accountId, std::optional<int> planId = std::nullopt) {
    PersistedUsageEvent event;
    event.providerId = providerId;
    event.accountId = accountId;
    event.planId = planId;
    event.eventTime = 1713416400;
    event.sourceType = "provider_backfill";
    event.sourceRunId = "backfill-run-1";
    event.modelKey = "gpt-4o-mini";
    event.requestType = "chat.completions";
    event.inputTokens = 123;
    event.outputTokens = 45;
    event.totalTokens = 168;
    event.costMinorUnits = 17;
    event.isBackfilled = true;
    event.rawReference = "fixture://usage-event-1";
    return event;
}

} // namespace

TEST_CASE("StorageService persists and queries activity events when SQLite is available", "[storage][activity]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    const long long providerId = requireSingleId(
        storage.execute("SELECT id FROM providers WHERE provider_key = 'openai';"));

    auto accountInsert = storage.execute(
        "INSERT INTO accounts (provider_id, account_key, display_name, scope_type, scope_identifier) "
        "VALUES (?, ?, ?, ?, ?);",
        {std::to_string(providerId), "acct-storage-test", "Storage Test Account", "user", "user-storage-test"});
    REQUIRE(accountInsert.success());
    const int accountId = static_cast<int>(accountInsert.lastInsertRowId());

    auto planInsert = storage.execute(
        "INSERT INTO plans (account_id, plan_key, plan_type, display_name, quota_window, limit_value) "
        "VALUES (?, ?, ?, ?, ?, ?);",
        {std::to_string(accountId), "plan-storage-test", "subscription", "ChatGPT Plus", "monthly", "1"});
    REQUIRE(planInsert.success());
    const int planId = static_cast<int>(planInsert.lastInsertRowId());

    PersistedActivityEvent event = makePersistedActivityEvent(static_cast<int>(providerId), accountId, planId);
    long long insertedId = 0;

    REQUIRE(storage.insertActivityEvent(event, &insertedId));
    CHECK(insertedId > 0);

    const auto results = storage.getActivityEvents(
        static_cast<int>(providerId),
        accountId,
        event.activity.startedAt - 10,
        event.activity.startedAt + 120);

    REQUIRE(results.size() == 1);
    CHECK(results[0].id == insertedId);
    CHECK(results[0].providerId == providerId);
    CHECK(results[0].accountId == accountId);
    REQUIRE(results[0].planId.has_value());
    CHECK(results[0].planId.value() == planId);
    CHECK(results[0].activity.providerSurface == "openai_subscription");
    CHECK(results[0].activity.providerKey == "openai");
    CHECK(results[0].activity.activityKind == ActivityKind::SessionActive);
    CHECK(results[0].activity.startedAt == event.activity.startedAt);
    CHECK(results[0].activity.endedAt == event.activity.endedAt);
    CHECK(results[0].activity.durationSeconds == 60);
    CHECK(results[0].activity.sourceType == "local");
    CHECK(results[0].activity.sourceRunId == "run-activity-1");
    CHECK(results[0].activity.surfaceKey == "foreground_window");
    CHECK(results[0].activity.windowTitleHint == "ChatGPT");
    CHECK(results[0].activity.confidence == ActivityConfidence::LocalExact);
    CHECK(results[0].activity.rawReference == "window:chatgpt");
    CHECK_FALSE(results[0].createdAtUtc.empty());
}

TEST_CASE("StorageService rejects invalid activity events before insert", "[storage][activity]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    PersistedActivityEvent event;
    event.providerId = 0;
    event.accountId = 0;
    event.activity.providerSurface = "openai_subscription";
    event.activity.startedAt = 1713416400;

    REQUIRE_FALSE(storage.insertActivityEvent(event));
    CHECK(storage.getLastError() == "Activity event requires providerId and accountId");
}

TEST_CASE("StorageService skips duplicate activity events during repeated local imports", "[storage][activity]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    const long long providerId = requireSingleId(
        storage.execute("SELECT id FROM providers WHERE provider_key = 'openai';"));

    auto accountInsert = storage.execute(
        "INSERT INTO accounts (provider_id, account_key, display_name, scope_type, scope_identifier) "
        "VALUES (?, ?, ?, ?, ?);",
        {std::to_string(providerId), "acct-activity-dup-test", "Activity Dup Test Account", "user", "user-activity-dup-test"});
    REQUIRE(accountInsert.success());
    const int accountId = static_cast<int>(accountInsert.lastInsertRowId());

    PersistedActivityEvent event = makePersistedActivityEvent(static_cast<int>(providerId), accountId);

    REQUIRE(storage.insertActivityEvent(event));
    REQUIRE(storage.insertActivityEvent(event));

    const QueryResult countResult = storage.execute(
        "SELECT COUNT(*) FROM activity_events WHERE provider_id = ? AND account_id = ?;",
        {std::to_string(providerId), std::to_string(accountId)});
    REQUIRE(countResult.success());
    REQUIRE(countResult.rowCount() == 1);
    CHECK(countResult.getInt64(0, 0) == 1);

    const auto persisted = storage.getActivityEvents(
        static_cast<int>(providerId),
        accountId,
        event.activity.startedAt - 10,
        event.activity.startedAt + 120);
    REQUIRE(persisted.size() == 1);
    CHECK(persisted.front().activity.sourceRunId == event.activity.sourceRunId);
    CHECK(persisted.front().activity.rawReference == event.activity.rawReference);
}

TEST_CASE("StorageService skips duplicate usage events during recovery/backfill inserts", "[storage][activity]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    const long long providerId = requireSingleId(
        storage.execute("SELECT id FROM providers WHERE provider_key = 'openai';"));

    auto accountInsert = storage.execute(
        "INSERT INTO accounts (provider_id, account_key, display_name, scope_type, scope_identifier) "
        "VALUES (?, ?, ?, ?, ?);",
        {std::to_string(providerId), "acct-usage-test", "Usage Test Account", "user", "user-usage-test"});
    REQUIRE(accountInsert.success());
    const int accountId = static_cast<int>(accountInsert.lastInsertRowId());

    auto planInsert = storage.execute(
        "INSERT INTO plans (account_id, plan_key, plan_type, display_name, quota_window, limit_value) "
        "VALUES (?, ?, ?, ?, ?, ?);",
        {std::to_string(accountId), "plan-usage-test", "subscription", "Usage Test Plan", "daily", "1000"});
    REQUIRE(planInsert.success());
    const int planId = static_cast<int>(planInsert.lastInsertRowId());

    const PersistedUsageEvent event = makePersistedUsageEvent(static_cast<int>(providerId), accountId, planId);

    REQUIRE(storage.insertUsageEvents({event}));
    REQUIRE(storage.insertUsageEvents({event}));
    REQUIRE(storage.insertUsageEvents({event, event}));

    const QueryResult countResult = storage.execute(
        "SELECT COUNT(*) FROM usage_events WHERE provider_id = ? AND account_id = ?;",
        {std::to_string(providerId), std::to_string(accountId)});
    REQUIRE(countResult.success());
    REQUIRE(countResult.rowCount() == 1);
    CHECK(countResult.getInt64(0, 0) == 1);

    const auto persisted = storage.getUsageEvents(
        static_cast<int>(providerId),
        accountId,
        event.eventTime - 10,
        event.eventTime + 10);
    REQUIRE(persisted.size() == 1);
    CHECK(persisted.front().sourceRunId == event.sourceRunId);
    CHECK(persisted.front().rawReference == event.rawReference);
    CHECK(persisted.front().isBackfilled);
}
