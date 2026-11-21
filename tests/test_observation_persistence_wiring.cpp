#include <catch_amalgamated.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#include "db/StorageService.hpp"
#include "observation/LocalObservationService.hpp"
#include "observation/ObservationPersistenceBridge.hpp"

using namespace JiStats;

namespace {

class TempDbFile {
public:
    TempDbFile() {
        const auto uniqueSuffix = std::to_string(reinterpret_cast<std::uintptr_t>(this));
        path_ = std::filesystem::temp_directory_path() /
            ("jistats_observation_persistence_" + uniqueSuffix + ".db");

        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    ~TempDbFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    std::string path() const {
        return path_.string();
    }

private:
    std::filesystem::path path_;
};

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

TEST_CASE("Local observation runtime path persists activity through the callback wiring", "[observation][storage][integration]") {
    TempDbFile tempDb;
    StorageService storage;

    if (!storage.init(tempDb.path())) {
        const std::string error = storage.getLastError();
        if (error.find("SQLite not available") != std::string::npos) {
            WARN(error);
            SUCCEED("SQLite-backed persistence is not compiled into this test build");
            return;
        }
        FAIL("StorageService init failed: " << error);
    }

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
    REQUIRE(observationService.recordActivity(event));
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
    TempDbFile tempDb;
    StorageService storage;

    if (!storage.init(tempDb.path())) {
        const std::string error = storage.getLastError();
        if (error.find("SQLite not available") != std::string::npos) {
            WARN(error);
            SUCCEED("SQLite-backed persistence is not compiled into this test build");
            return;
        }
        FAIL("StorageService init failed: " << error);
    }

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
