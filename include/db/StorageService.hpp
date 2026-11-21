#pragma once

#include <string>
#include <memory>
#include <vector>
#include <optional>
#include <ctime>
#include "analytics/GapClassifier.hpp"
#include "db/QueryResult.hpp"
#include "observation/ActivityEvent.hpp"

namespace JiStats {

struct PersistedActivityEvent {
    long long id = 0;
    int providerId = 0;
    int accountId = 0;
    std::optional<int> planId;
    ActivityEvent activity;
    std::string createdAtUtc;
};

struct PersistedSyncState {
    long long id = 0;
    int providerId = 0;
    int accountId = 0;
    std::string syncKind;
    std::string cursorValue;
    std::time_t lastAttemptAt = 0;
    std::time_t lastSuccessAt = 0;
    std::time_t lastFailureAt = 0;
    std::string lastFailureCategory;
    std::string createdAtUtc;
    std::string updatedAtUtc;
};

struct PersistedMirrorRule {
    long long id = 0;
    int providerId = 0;
    std::string ruleKey;
    int ruleVersion = 1;
    std::string configJson;
    std::string createdAtUtc;
};

struct PersistedConnectivityState {
    long long id = 0;
    int providerId = 0;
    int accountId = 0;
    std::string status = "online";
    std::string failureCategory;
    std::time_t lastCheckedAt = 0;
    std::time_t nextRetryAt = 0;
    std::time_t noticeDismissedAt = 0;
    std::string createdAtUtc;
    std::string updatedAtUtc;
};

struct PersistedRuntimeState {
    int id = 1;
    std::time_t lastLaunchAt = 0;
    std::time_t lastSuccessfulSyncAt = 0;
    std::time_t lastLocalObservationAt = 0;
    std::string recoveryMarker;
    std::string createdAtUtc;
    std::string updatedAtUtc;
};

struct PersistedGapMarker {
    long long id = 0;
    int providerId = 0;
    int accountId = 0;
    std::time_t periodStart = 0;
    std::time_t periodEnd = 0;
    GapClassification classification = GapClassification::Unknown;
    std::string basisSummary;
    std::string sourceKind;
    std::string createdAtUtc;
    std::string updatedAtUtc;
};

struct PersistedProviderCapability {
    long long id = 0;
    int providerId = 0;
    std::string capabilityKey;
    bool supported = false;
    std::string configJson;
    std::time_t discoveredAt = 0;
};

struct PersistedCredentialProfile {
    long long id = 0;
    int providerId = 0;
    int accountId = 0;
    std::string profileName;
    std::string secretRef;
    std::string credentialType;
    std::time_t lastValidatedAt = 0;
    std::string createdAtUtc;
    std::string updatedAtUtc;
};

enum class UsageRollupResolution {
    Hourly,
    Daily
};

struct PersistedUsageEvent {
    long long id = 0;
    int providerId = 0;
    int accountId = 0;
    std::optional<int> planId;
    std::time_t eventTime = 0;
    std::string sourceType;
    std::string sourceRunId;
    std::string modelKey;
    std::string requestType;
    long long inputTokens = 0;
    long long outputTokens = 0;
    long long totalTokens = 0;
    long long costMinorUnits = 0;
    bool isBackfilled = false;
    std::string rawReference;
    std::string createdAtUtc;
};

struct PersistedUsageRollup {
    long long id = 0;
    int providerId = 0;
    int accountId = 0;
    std::time_t periodStart = 0;
    std::string modelKey;
    int requestCount = 0;
    long long inputTokens = 0;
    long long outputTokens = 0;
    long long totalTokens = 0;
    long long totalCostMinorUnits = 0;
    long long localRequestCount = 0;
    long long importedRequestCount = 0;
    long long apiRequestCount = 0;
    long long unknownRequestCount = 0;
    long long backfilledRequestCount = 0;
    std::string createdAtUtc;
    std::string updatedAtUtc;
};

struct PersistedCodexLocalSource {
    long long id = 0;
    std::string sourcePath;
    std::string sourceKind = "codex_state_sqlite";
    std::string sourceIdentity;
    std::time_t firstSeenAt = 0;
    std::time_t lastSeenAt = 0;
    std::time_t lastReadAt = 0;
};

struct PersistedCodexThreadImportState {
    long long id = 0;
    long long sourceId = 0;
    std::string threadId;
    std::string modelProvider;
    std::string modelKey;
    std::string reasoningEffort;
    long long firstSeenCreatedAtMs = 0;
    long long lastSeenUpdatedAtMs = 0;
    long long baselineTokensUsed = 0;
    long long lastSeenTokensUsed = 0;
    long long lastImportedTokensUsed = 0;
    std::time_t baselineEstablishedAt = 0;
    std::time_t lastEventTime = 0;
    int counterResetCount = 0;
    std::string lastStatus = "active";
    std::time_t updatedAt = 0;
};

struct PersistedModelUsagePoint {
    std::time_t bucketStart = 0;
    std::string providerKey;
    std::string modelKey;
    long long totalTokens = 0;
};

struct PersistedQuotaSnapshot {
    long long id = 0;
    int providerId = 0;
    int accountId = 0;
    std::optional<int> planId;
    std::time_t snapshotTime = 0;
    std::time_t windowStart = 0;
    std::time_t windowEnd = 0;
    long long usedValue = 0;
    std::optional<long long> limitValue;
    std::optional<long long> remainingValue;
    std::string confidenceTier;
    std::string quotaDimension = "requests";
    bool isReconciled = false;
    std::string basisSummary;
    std::string createdAtUtc;
};

struct PersistedQuotaSnapshotRollup {
    long long id = 0;
    int providerId = 0;
    int accountId = 0;
    std::optional<int> planId;
    std::string quotaDimension = "requests";
    std::time_t periodStart = 0;
    int sampleCount = 0;
    std::time_t latestSnapshotTime = 0;
    std::time_t latestWindowStart = 0;
    std::time_t latestWindowEnd = 0;
    long long latestUsedValue = 0;
    std::optional<long long> latestLimitValue;
    std::optional<long long> latestRemainingValue;
    std::optional<long long> lowWaterRemainingValue;
    std::optional<long long> lowWaterLimitValue;
    int resetCount = 0;
    int reconciledSampleCount = 0;
    std::string latestConfidenceTier;
    std::string createdAtUtc;
    std::string updatedAtUtc;
};

struct PersistedRecoveryCheckpoint {
    long long id = 0;
    int providerId = 0;
    int accountId = 0;
    std::string syncKind;
    std::time_t recoveryStart = 0;
    std::time_t recoveryEnd = 0;
    std::time_t attemptAt = 0;
    std::string outcome;
    long long fetchedEventCount = 0;
    std::string comparisonResult;
    long long localEventCount = 0;
    long long localTotalTokens = 0;
    long long localTotalCostMinorUnits = 0;
    long long importedTotalTokens = 0;
    long long importedTotalCostMinorUnits = 0;
    std::string failureCategory;
    std::string createdAtUtc;
};

struct PersistedExportRun {
    long long id = 0;
    std::string runKind;
    std::string format;
    std::time_t requestedAt = 0;
    std::time_t completedAt = 0;
    bool success = false;
    std::string outputPath;
    std::string errorSummary;
    std::time_t rangeStart = 0;
    std::time_t rangeEnd = 0;
    std::string providerFilterCsv;
    std::string accountFilterCsv;
    std::string createdAtUtc;
};

class StorageService {
public:
    StorageService();
    ~StorageService();
    
    // Initialize the database
    bool init(const std::string& dbPath = "jistats.db");
    
    // Close the database
    void close();
    
    // Check if database is open
    bool isOpen() const;
    
    // Execute a query
    QueryResult execute(const std::string& sql);
    
    // Execute a query with parameters
    QueryResult execute(const std::string& sql, const std::vector<std::string>& params);
    
    // Begin transaction
    bool beginTransaction();
    
    // Commit transaction
    bool commitTransaction();
    
    // Rollback transaction
    bool rollbackTransaction();
    
    // Get last error message
    std::string getLastError() const;
    
    // Get database path
    std::string getDbPath() const;

    // Persist and query activity_events rows
    bool insertActivityEvent(const PersistedActivityEvent& event, long long* insertedId = nullptr);
    std::vector<PersistedActivityEvent> getActivityEvents(
        int providerId,
        int accountId,
        std::time_t startedAtFrom,
        std::time_t startedAtTo);

    // Persist and query mirror-state rows
    bool upsertSyncState(const PersistedSyncState& state, long long* rowId = nullptr);
    std::optional<PersistedSyncState> getSyncState(
        int providerId,
        int accountId,
        const std::string& syncKind);

    bool upsertMirrorRule(const PersistedMirrorRule& rule, long long* rowId = nullptr);
    std::vector<PersistedMirrorRule> getMirrorRules(int providerId);

    bool upsertConnectivityState(const PersistedConnectivityState& state, long long* rowId = nullptr);
    std::optional<PersistedConnectivityState> getConnectivityState(int providerId, int accountId);

    bool upsertRuntimeState(const PersistedRuntimeState& state);
    std::optional<PersistedRuntimeState> getRuntimeState();

    bool upsertGapMarker(const PersistedGapMarker& marker, long long* rowId = nullptr);
    std::vector<PersistedGapMarker> getGapMarkers(
        int providerId,
        int accountId,
        std::time_t periodStartFrom,
        std::time_t periodEndTo);
    bool deleteGapMarkersOlderThan(std::time_t olderThan);

    bool upsertProviderCapability(const PersistedProviderCapability& capability, long long* rowId = nullptr);
    std::vector<PersistedProviderCapability> getProviderCapabilities(int providerId);

    bool upsertCredentialProfile(const PersistedCredentialProfile& profile, long long* rowId = nullptr);
    std::optional<PersistedCredentialProfile> getCredentialProfile(
        int accountId,
        const std::string& profileName);
    std::vector<PersistedCredentialProfile> getCredentialProfiles(int accountId);

    bool upsertCodexLocalSource(const PersistedCodexLocalSource& source, long long* rowId = nullptr);
    std::optional<PersistedCodexLocalSource> getCodexLocalSource(
        const std::string& sourcePath,
        const std::string& sourceIdentity);
    std::optional<PersistedCodexThreadImportState> getCodexThreadImportState(
        long long sourceId,
        const std::string& threadId);
    bool upsertCodexThreadImportState(const PersistedCodexThreadImportState& state, long long* rowId = nullptr);

    bool rebuildUsageRollups(
        UsageRollupResolution resolution,
        int providerId,
        int accountId,
        std::time_t from,
        std::time_t to);
    std::vector<PersistedUsageRollup> getUsageRollups(
        UsageRollupResolution resolution,
        int providerId,
        int accountId,
        std::time_t periodStartFrom,
        std::time_t periodStartTo);
    std::vector<PersistedQuotaSnapshot> getQuotaSnapshots(
        int providerId,
        int accountId,
        std::time_t snapshotTimeFrom,
        std::time_t snapshotTimeTo);
    bool rebuildQuotaSnapshotRollups(
        int providerId,
        int accountId,
        std::time_t from,
        std::time_t to);
    std::vector<PersistedQuotaSnapshotRollup> getQuotaSnapshotRollups(
        int providerId,
        int accountId,
        std::time_t periodStartFrom,
        std::time_t periodStartTo);
    bool insertRecoveryCheckpoint(
        const PersistedRecoveryCheckpoint& checkpoint,
        long long* rowId = nullptr);
    std::vector<PersistedRecoveryCheckpoint> getRecoveryCheckpoints(
        int providerId,
        int accountId,
        const std::string& syncKind,
        std::time_t attemptAtFrom,
        std::time_t attemptAtTo);
    std::optional<PersistedRecoveryCheckpoint> getLatestRecoveryCheckpoint(
        int providerId,
        int accountId,
        const std::string& syncKind);
    bool insertExportRun(const PersistedExportRun& exportRun, long long* rowId = nullptr);
    std::optional<PersistedExportRun> getLatestExportRun(const std::string& runKind);
    bool deleteUsageEventsOlderThan(std::time_t olderThan);
    bool deleteUsageRollupsOlderThan(UsageRollupResolution resolution, std::time_t olderThan);

    // Batch persist usage events for backfill/recovery
    bool insertUsageEvents(const std::vector<PersistedUsageEvent>& events);
    std::vector<PersistedUsageEvent> getUsageEvents(
        int providerId,
        int accountId,
        std::time_t from,
        std::time_t to);
    bool hasUsageEvents(int providerId, int accountId, std::time_t from, std::time_t to);
    std::vector<PersistedModelUsagePoint> getModelUsagePoints(
        const std::vector<std::string>& providerKeys,
        const std::string& sourceType,
        std::time_t from,
        std::time_t to,
        int bucketSeconds);
    
private:
    // Private implementation (PIMPL pattern)
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace JiStats
