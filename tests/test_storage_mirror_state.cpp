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

StorageIds seedProviderAccount(
    StorageService& storage,
    const std::string& providerKey,
    const std::string& accountKey,
    const std::string& displayName) {
    StorageIds ids;
    ids.providerId = static_cast<int>(requireSingleId(
        storage.execute(
            "SELECT id FROM providers WHERE provider_key = ?;",
            {providerKey})));

    auto accountInsert = storage.execute(
        "INSERT INTO accounts (provider_id, account_key, display_name, scope_type, scope_identifier) "
        "VALUES (?, ?, ?, ?, ?);",
        {
            std::to_string(ids.providerId),
            accountKey,
            displayName,
            "user",
            "user-" + accountKey
        });
    REQUIRE(accountInsert.success());
    ids.accountId = static_cast<int>(accountInsert.lastInsertRowId());
    return ids;
}

StorageIds seedProviderAccount(StorageService& storage) {
    return seedProviderAccount(storage, "openai", "acct-mirror-test", "Mirror Test Account");
}

} // namespace

TEST_CASE("StorageService upserts and queries sync state when SQLite is available", "[storage][mirror]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    const auto ids = seedProviderAccount(storage);

    PersistedSyncState initial;
    initial.providerId = ids.providerId;
    initial.accountId = ids.accountId;
    initial.syncKind = "quota";
    initial.cursorValue = "cursor-1";
    initial.lastAttemptAt = 1713500000;
    initial.lastSuccessAt = 1713500060;
    initial.lastFailureCategory = "none";

    long long rowId = 0;
    REQUIRE(storage.upsertSyncState(initial, &rowId));
    CHECK(rowId > 0);

    auto stored = storage.getSyncState(ids.providerId, ids.accountId, "quota");
    REQUIRE(stored.has_value());
    CHECK(stored->id == rowId);
    CHECK(stored->cursorValue == "cursor-1");
    CHECK(stored->lastAttemptAt == 1713500000);
    CHECK(stored->lastSuccessAt == 1713500060);
    CHECK(stored->lastFailureCategory == "none");

    PersistedSyncState updated = *stored;
    updated.cursorValue = "cursor-2";
    updated.lastFailureAt = 1713500120;
    updated.lastFailureCategory = "network";

    REQUIRE(storage.upsertSyncState(updated, &rowId));
    auto refreshed = storage.getSyncState(ids.providerId, ids.accountId, "quota");
    REQUIRE(refreshed.has_value());
    CHECK(refreshed->id == rowId);
    CHECK(refreshed->cursorValue == "cursor-2");
    CHECK(refreshed->lastFailureAt == 1713500120);
    CHECK(refreshed->lastFailureCategory == "network");
}

TEST_CASE("StorageService upserts and queries mirror rules when SQLite is available", "[storage][mirror]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    const int providerId = static_cast<int>(requireSingleId(
        storage.execute("SELECT id FROM providers WHERE provider_key = 'openai';")));

    PersistedMirrorRule quotaRule;
    quotaRule.providerId = providerId;
    quotaRule.ruleKey = "quota_window";
    quotaRule.ruleVersion = 1;
    quotaRule.configJson = "{\"window\":\"rolling\"}";

    PersistedMirrorRule retryRule;
    retryRule.providerId = providerId;
    retryRule.ruleKey = "retry_policy";
    retryRule.ruleVersion = 2;
    retryRule.configJson = "{\"backoff\":\"exp\"}";

    long long quotaRuleId = 0;
    REQUIRE(storage.upsertMirrorRule(quotaRule, &quotaRuleId));
    REQUIRE(storage.upsertMirrorRule(retryRule, nullptr));

    auto rules = storage.getMirrorRules(providerId);
    REQUIRE(rules.size() == 2);
    CHECK(rules[0].ruleKey == "quota_window");
    CHECK(rules[0].ruleVersion == 1);
    CHECK(rules[0].configJson == "{\"window\":\"rolling\"}");
    CHECK(rules[1].ruleKey == "retry_policy");
    CHECK(rules[1].ruleVersion == 2);

    quotaRule.configJson = "{\"window\":\"daily\"}";
    REQUIRE(storage.upsertMirrorRule(quotaRule, &quotaRuleId));

    rules = storage.getMirrorRules(providerId);
    REQUIRE(rules.size() == 2);
    CHECK(rules[0].id == quotaRuleId);
    CHECK(rules[0].configJson == "{\"window\":\"daily\"}");
}

TEST_CASE("StorageService upserts and queries connectivity state when SQLite is available", "[storage][mirror]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    const auto ids = seedProviderAccount(storage);

    PersistedConnectivityState state;
    state.providerId = ids.providerId;
    state.accountId = ids.accountId;
    state.status = "offline";
    state.failureCategory = "network";
    state.lastCheckedAt = 1713600000;
    state.nextRetryAt = 1713600300;

    long long rowId = 0;
    REQUIRE(storage.upsertConnectivityState(state, &rowId));
    CHECK(rowId > 0);

    auto stored = storage.getConnectivityState(ids.providerId, ids.accountId);
    REQUIRE(stored.has_value());
    CHECK(stored->id == rowId);
    CHECK(stored->status == "offline");
    CHECK(stored->failureCategory == "network");
    CHECK(stored->lastCheckedAt == 1713600000);
    CHECK(stored->nextRetryAt == 1713600300);

    state.status = "online";
    state.failureCategory.clear();
    state.nextRetryAt = 0;
    state.noticeDismissedAt = 1713600400;
    REQUIRE(storage.upsertConnectivityState(state, &rowId));

    stored = storage.getConnectivityState(ids.providerId, ids.accountId);
    REQUIRE(stored.has_value());
    CHECK(stored->id == rowId);
    CHECK(stored->status == "online");
    CHECK(stored->failureCategory.empty());
    CHECK(stored->nextRetryAt == 0);
    CHECK(stored->noticeDismissedAt == 1713600400);
}

TEST_CASE("StorageService upserts and queries runtime state when SQLite is available", "[storage][mirror]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    PersistedRuntimeState state;
    state.lastLaunchAt = 1713700000;
    state.lastSuccessfulSyncAt = 1713700100;
    state.lastLocalObservationAt = 1713700200;
    state.recoveryMarker = "gap:openai:2026-04-18";

    REQUIRE(storage.upsertRuntimeState(state));

    auto stored = storage.getRuntimeState();
    REQUIRE(stored.has_value());
    CHECK(stored->id == 1);
    CHECK(stored->lastLaunchAt == 1713700000);
    CHECK(stored->lastSuccessfulSyncAt == 1713700100);
    CHECK(stored->lastLocalObservationAt == 1713700200);
    CHECK(stored->recoveryMarker == "gap:openai:2026-04-18");

    state.lastLocalObservationAt = 1713700300;
    state.recoveryMarker = "gap:minimax:2026-04-19";
    REQUIRE(storage.upsertRuntimeState(state));

    stored = storage.getRuntimeState();
    REQUIRE(stored.has_value());
    CHECK(stored->lastLocalObservationAt == 1713700300);
    CHECK(stored->recoveryMarker == "gap:minimax:2026-04-19");
}

TEST_CASE("StorageService upserts and queries provider capabilities when SQLite is available", "[storage][mirror]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    const int providerId = static_cast<int>(requireSingleId(
        storage.execute("SELECT id FROM providers WHERE provider_key = 'openai';")));

    PersistedProviderCapability capability;
    capability.providerId = providerId;
    capability.capabilityKey = "subscription_quota";
    capability.supported = true;
    capability.configJson = "{\"source\":\"local-fixture\"}";
    capability.discoveredAt = 1713800000;

    long long rowId = 0;
    REQUIRE(storage.upsertProviderCapability(capability, &rowId));
    CHECK(rowId > 0);

    auto capabilities = storage.getProviderCapabilities(providerId);
    REQUIRE(capabilities.size() == 1);
    CHECK(capabilities[0].id == rowId);
    CHECK(capabilities[0].capabilityKey == "subscription_quota");
    CHECK(capabilities[0].supported);
    CHECK(capabilities[0].configJson == "{\"source\":\"local-fixture\"}");
    CHECK(capabilities[0].discoveredAt == 1713800000);

    capability.supported = false;
    capability.configJson = "{\"source\":\"disabled\"}";
    REQUIRE(storage.upsertProviderCapability(capability, &rowId));

    capabilities = storage.getProviderCapabilities(providerId);
    REQUIRE(capabilities.size() == 1);
    CHECK(capabilities[0].id == rowId);
    CHECK_FALSE(capabilities[0].supported);
    CHECK(capabilities[0].configJson == "{\"source\":\"disabled\"}");
}

TEST_CASE("StorageService upserts and queries credential profiles when SQLite is available", "[storage][mirror][credentials]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    const auto ids = seedProviderAccount(
        storage,
        "minimax",
        "acct-minimax-credentials",
        "MiniMax Credentials");

    PersistedCredentialProfile profile;
    profile.providerId = ids.providerId;
    profile.accountId = ids.accountId;
    profile.profileName = "MiniMax Coding Plan";
    profile.secretRef = "JiStats/minimax/subscription/coding_plan/acct-minimax-credentials";
    profile.credentialType = "subscription_key";
    profile.lastValidatedAt = 1713900000;

    long long rowId = 0;
    REQUIRE(storage.upsertCredentialProfile(profile, &rowId));
    CHECK(rowId > 0);

    auto stored = storage.getCredentialProfile(ids.accountId, "MiniMax Coding Plan");
    REQUIRE(stored.has_value());
    CHECK(stored->id == rowId);
    CHECK(stored->providerId == ids.providerId);
    CHECK(stored->accountId == ids.accountId);
    CHECK(stored->secretRef == profile.secretRef);
    CHECK(stored->credentialType == "subscription_key");
    CHECK(stored->lastValidatedAt == 1713900000);
    CHECK_FALSE(stored->createdAtUtc.empty());
    CHECK_FALSE(stored->updatedAtUtc.empty());

    profile.secretRef = "JiStats/minimax/subscription/token_plan/acct-minimax-credentials";
    profile.lastValidatedAt = 1713900300;
    REQUIRE(storage.upsertCredentialProfile(profile, &rowId));

    stored = storage.getCredentialProfile(ids.accountId, "MiniMax Coding Plan");
    REQUIRE(stored.has_value());
    CHECK(stored->id == rowId);
    CHECK(stored->secretRef == "JiStats/minimax/subscription/token_plan/acct-minimax-credentials");
    CHECK(stored->lastValidatedAt == 1713900300);

    auto profiles = storage.getCredentialProfiles(ids.accountId);
    REQUIRE(profiles.size() == 1);
    CHECK(profiles.front().profileName == "MiniMax Coding Plan");
}

TEST_CASE("StorageService rejects invalid mirror-state inputs before insert", "[storage][mirror]") {
    StorageService storage;

    PersistedSyncState invalidSync;
    REQUIRE_FALSE(storage.upsertSyncState(invalidSync));

    PersistedMirrorRule invalidRule;
    invalidRule.providerId = 1;
    invalidRule.ruleVersion = 0;
    REQUIRE_FALSE(storage.upsertMirrorRule(invalidRule));

    PersistedConnectivityState invalidConnectivity;
    invalidConnectivity.providerId = 1;
    invalidConnectivity.accountId = 0;
    invalidConnectivity.status = "";
    REQUIRE_FALSE(storage.upsertConnectivityState(invalidConnectivity));

    PersistedRuntimeState invalidRuntime;
    invalidRuntime.id = 2;
    REQUIRE_FALSE(storage.upsertRuntimeState(invalidRuntime));

    PersistedGapMarker invalidGapMarker;
    invalidGapMarker.providerId = 1;
    invalidGapMarker.accountId = 1;
    invalidGapMarker.periodStart = 100;
    invalidGapMarker.periodEnd = 99;
    REQUIRE_FALSE(storage.upsertGapMarker(invalidGapMarker));

    PersistedProviderCapability invalidCapability;
    invalidCapability.providerId = 1;
    REQUIRE_FALSE(storage.upsertProviderCapability(invalidCapability));

    PersistedCredentialProfile invalidCredentialProfile;
    invalidCredentialProfile.providerId = 1;
    REQUIRE_FALSE(storage.upsertCredentialProfile(invalidCredentialProfile));
}
