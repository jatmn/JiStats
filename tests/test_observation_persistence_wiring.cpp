#include <catch_amalgamated.hpp>

#include <string>

#ifdef JISTATS_TEST_WRAP_SQLITE_STEP
#include <sqlite3.h>
#include <string_view>
#include <thread>
#endif

#include "db/StorageService.hpp"
#include "observation/LocalObservationService.hpp"
#include "observation/ObservationPersistenceBridge.hpp"
#include "sqlite_test_support.hpp"
#include "temp_test_directory.hpp"

using namespace JiStats;
using JiStats::TestSupport::TempTestDatabase;
using JiStats::TestSupport::requireSqlite;

namespace {

#ifdef JISTATS_TEST_WRAP_SQLITE_STEP
thread_local int* settingsInsertResult = nullptr;
#endif

ActivityEvent makeObservedActivityEvent() {
    ActivityEvent event;
    event.providerKey = "openai";
    event.providerSurface = "openai_subscription";
    event.activityKind = ActivityKind::SessionActive;
    event.startedAt = 1713416400;
    event.endedAt = 1713416460;
    event.durationSeconds = 60;
    event.sourceType = "local";
    event.sourceRunId = "run-observation-persist-1";
    event.surfaceKey = "foreground_window";
    event.windowTitleHint = "ChatGPT";
    event.confidence = ActivityConfidence::LocalExact;
    event.rawReference = "window:chatgpt";
    return event;
}

} // namespace

#ifdef JISTATS_TEST_WRAP_SQLITE_STEP
extern "C" int __real_sqlite3_step(sqlite3_stmt* statement);

extern "C" int __wrap_sqlite3_step(sqlite3_stmt* statement) {
    const int result = __real_sqlite3_step(statement);
    if (settingsInsertResult && result == SQLITE_DONE &&
        std::string_view(sqlite3_sql(statement)).rfind("INSERT INTO accounts ", 0) == 0) {
        // Run a separate writer after SQLite releases its statement lock.
        auto* const connection = sqlite3_db_handle(statement);
        int& insertResult = *settingsInsertResult;
        std::thread writer([&] {
            insertResult = sqlite3_exec(
                connection, "INSERT INTO settings (rowid, key, value) VALUES (4242, 'observation-race', '1');", nullptr,
                nullptr, nullptr);
        });
        writer.join();
    }
    return result;
}
#endif

TEST_CASE("Local observation runtime path persists activity through the callback wiring", "[observation][storage][integration]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    LocalObservationService observationService;
    REQUIRE(observationService.init());

    ObservationPersistenceBridge persistenceBridge(storage);
    int callbackCount = 0;
    int persistedCount = 0;
    long long lastInsertedId = 0;
    std::string lastPersistError;

    // Mirror the current AppController runtime path: callback drain plus bridge persistence.
    observationService.setActivityCallback([&](const ActivityEvent&) {
        ++callbackCount;
        std::vector<long long> insertedIds;
        persistedCount += static_cast<int>(
            persistenceBridge.persistPending(observationService, &insertedIds));
        if (!insertedIds.empty()) {
            lastInsertedId = insertedIds.back();
            lastPersistError.clear();
        } else {
            lastPersistError = persistenceBridge.getLastError();
        }
    });

    const ActivityEvent event = makeObservedActivityEvent();
#ifdef JISTATS_TEST_WRAP_SQLITE_STEP
    int insertResult = SQLITE_ERROR;
    settingsInsertResult = &insertResult;
#endif
    const bool recorded = observationService.recordActivity(event);
#ifdef JISTATS_TEST_WRAP_SQLITE_STEP
    settingsInsertResult = nullptr;
    REQUIRE(insertResult == SQLITE_OK);
#endif
    REQUIRE(recorded);
    CHECK(observationService.getLastError().empty());
    CHECK(observationService.getPendingActivityCount() == 0);

    REQUIRE(callbackCount == 1);
    REQUIRE(persistedCount == 1);
    CHECK(lastPersistError.empty());
    REQUIRE(lastInsertedId > 0);

    const auto persistedEvents = persistenceBridge.getPersistedActivities(
        "openai",
        "openai_subscription",
        event.startedAt - 10,
        event.startedAt + 120);

    REQUIRE(persistedEvents.size() == 1);
    const auto& persisted = persistedEvents.front();
    CHECK(persisted.id == lastInsertedId);
    CHECK(persisted.providerId > 0);
    CHECK(persisted.accountId > 0);
    CHECK_FALSE(persisted.planId.has_value());
    CHECK(persisted.activity.providerKey == event.providerKey);
    CHECK(persisted.activity.providerSurface == event.providerSurface);
    CHECK(persisted.activity.activityKind == event.activityKind);
    CHECK(persisted.activity.startedAt == event.startedAt);
    CHECK(persisted.activity.endedAt == event.endedAt);
    CHECK(persisted.activity.durationSeconds == event.durationSeconds);
    CHECK(persisted.activity.sourceType == event.sourceType);
    CHECK(persisted.activity.sourceRunId == event.sourceRunId);
    CHECK(persisted.activity.surfaceKey == event.surfaceKey);
    CHECK(persisted.activity.windowTitleHint == event.windowTitleHint);
    CHECK(persisted.activity.confidence == event.confidence);
    CHECK(persisted.activity.rawReference == event.rawReference);
    CHECK_FALSE(persisted.createdAtUtc.empty());

    const auto localObservationAccount = storage.execute(
        "SELECT account_key, scope_type, scope_identifier FROM accounts WHERE id = ?;",
        {std::to_string(persisted.accountId)});
    REQUIRE(localObservationAccount.success());
    REQUIRE(localObservationAccount.rowCount() == 1);
    CHECK(localObservationAccount.getString(0, 0) == "local-observation:openai_subscription");
    CHECK(localObservationAccount.getString(0, 1) == "local_observation");
    CHECK(localObservationAccount.getString(0, 2) == "openai_subscription");
}

TEST_CASE("ObservationPersistenceBridge preserves binding query errors instead of clearing them", "[observation][storage][integration]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    ObservationPersistenceBridge persistenceBridge(storage);
    long long insertedId = 0;
    REQUIRE(persistenceBridge.persist(makeObservedActivityEvent(), &insertedId));
    REQUIRE(insertedId > 0);

    auto breakQueryResult = storage.execute(
        "ALTER TABLE accounts RENAME TO accounts_broken;");
    REQUIRE(breakQueryResult.success());

    const auto persistedEvents = persistenceBridge.getPersistedActivities(
        "openai",
        "openai_subscription",
        1713416300,
        1713416600);

    CHECK(persistedEvents.empty());
    CHECK_FALSE(persistenceBridge.getLastError().empty());
}
