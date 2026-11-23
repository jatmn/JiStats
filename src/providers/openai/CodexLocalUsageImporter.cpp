#include "providers/openai/CodexLocalUsageImporter.hpp"

#include "db/StorageService.hpp"
#include "providers/openai/CodexLocalSqliteReader.hpp"

#include <algorithm>
#include <ctime>
#include <optional>
#include <sstream>
#include <vector>

namespace JiStats {
namespace {

class ImportTransaction {
  public:
    explicit ImportTransaction(StorageService& storage) : storage_(storage) {}

    ~ImportTransaction() {
        if (active_) {
            storage_.rollbackTransaction();
        }
    }

    bool begin() {
        active_ = storage_.beginTransaction(StorageService::TransactionMode::Immediate);
        return active_;
    }

    bool commit() {
        if (active_ && !storage_.commitTransaction()) {
            return false;
        }
        active_ = false;
        return true;
    }

  private:
    StorageService& storage_;
    bool active_ = false;
};

std::optional<std::string> mapCodexProviderToJiStatsProvider(const std::string& modelProvider) {
    if (modelProvider == "openai") return std::string("openai");
    if (modelProvider == "hicap-ai") return std::string("hicap-ai");
    return std::nullopt;
}

std::optional<int> findProviderId(StorageService& storage, const std::string& providerKey) {
    QueryResult result = storage.execute(
        "SELECT id FROM providers WHERE provider_key = ? LIMIT 1;",
        {providerKey});
    if (!result.success() || result.rowCount() == 0) return std::nullopt;
    return result.getInt(0, 0);
}

std::optional<int> ensureLocalCodexAccount(StorageService& storage, int providerId, const std::string& providerKey) {
    const std::string accountKey = providerKey + ".codex_local_sqlite";
    QueryResult existing = storage.execute(
        "SELECT id FROM accounts WHERE provider_id = ? AND account_key = ? LIMIT 1;",
        {std::to_string(providerId), accountKey});
    if (existing.success() && existing.rowCount() == 1) return existing.getInt(0, 0);

    QueryResult insert = storage.execute(
        "INSERT INTO accounts (provider_id, account_key, display_name, scope_type, scope_identifier) "
        "VALUES (?, ?, ?, ?, ?);",
        {std::to_string(providerId),
         accountKey,
         providerKey == "hicap-ai" ? "HICAP Codex Local" : "OpenAI Codex Local",
         "local_codex_sqlite",
         accountKey});
    if (!insert.success()) return std::nullopt;
    return static_cast<int>(insert.lastInsertRowId());
}

std::time_t eventTimeFromUpdatedAtMs(long long updatedAtMs, std::time_t fallback) {
    if (updatedAtMs > 0) {
        return static_cast<std::time_t>(updatedAtMs / 1000);
    }
    return fallback;
}

std::string buildRawReference(long long sourceId, const std::string& threadId, long long previousTokens, long long currentTokens) {
    std::ostringstream out;
    out << "codex-state:" << sourceId << ":" << threadId << ":" << previousTokens << "-" << currentTokens;
    return out.str();
}

std::string buildRunId(long long sourceId, const std::string& threadId, long long updatedAtMs) {
    std::ostringstream out;
    out << "codex-state:" << sourceId << ":" << threadId << ":" << updatedAtMs;
    return out.str();
}

bool isThreadObservedAfterSourceBaseline(const CodexLocalThreadRow& row, std::time_t sourceBaselineAt) {
    const long long baselineMs = static_cast<long long>(sourceBaselineAt) * 1000;
    if (row.createdAtMs > 0 && row.createdAtMs >= baselineMs) return true;
    if (row.updatedAtMs > 0 && row.updatedAtMs >= baselineMs) return true;
    return row.createdAtMs <= 0 && row.updatedAtMs <= 0;
}

} // namespace

CodexLocalUsageImporter::CodexLocalUsageImporter(StorageService& storage)
    : storage_(storage) {
}

CodexLocalUsageImportResult CodexLocalUsageImporter::importFromDefaultSource(std::time_t now) {
    return importFromSource({}, now);
}

CodexLocalUsageImportResult CodexLocalUsageImporter::importFromSource(const std::string& sourcePath, std::time_t now,
                                                                      int attributionGeneration) {
    if (now <= 0) now = std::time(nullptr);
    CodexLocalUsageImportResult result;

    CodexLocalSqliteReader reader;
    CodexLocalSqliteReadResult readResult = reader.readThreads(sourcePath);
    result.sourcePath = readResult.sourcePath;
    if (!readResult.success) {
        result.error = readResult.error;
        return result;
    }

    result.rowsRead = static_cast<int>(readResult.threads.size());

    // Each consent period starts with independent checkpoints.
    const std::string sourceIdentity =
        attributionGeneration > 0 ? readResult.sourceIdentity + "#attribution-" + std::to_string(attributionGeneration)
                                  : readResult.sourceIdentity;
    const auto existingSource = storage_.getCodexLocalSource(readResult.sourcePath, sourceIdentity);
    const bool sourceBaselineEstablished = existingSource.has_value();
    ImportTransaction transaction(storage_);
    if (!storage_.getLastError().empty() || !transaction.begin()) {
        result.error = "Could not start the Codex usage import transaction";
        return result;
    }
    if (!sourceBaselineEstablished) {
        // Another instance can establish the baseline while this scan waits for the write lock.
        // Retry from a fresh source snapshot in that case.
        if (storage_.getCodexLocalSource(readResult.sourcePath, sourceIdentity) || !storage_.getLastError().empty()) {
            result.error = "Codex source baseline changed during import";
            return result;
        }
    }
    const std::time_t sourceBaselineAt = existingSource.has_value() && existingSource->firstSeenAt > 0
        ? existingSource->firstSeenAt
        : now;

    PersistedCodexLocalSource source;
    source.sourcePath = readResult.sourcePath;
    source.sourceKind = "codex_state_sqlite";
    source.sourceIdentity = sourceIdentity;
    source.firstSeenAt = sourceBaselineEstablished && existingSource->firstSeenAt > 0
        ? existingSource->firstSeenAt
        : now;
    source.lastSeenAt = now;
    source.lastReadAt = now;
    long long sourceId = 0;
    if (!storage_.upsertCodexLocalSource(source, &sourceId)) {
        result.error = storage_.getLastError();
        return result;
    }

    std::vector<PersistedUsageEvent> pendingEvents;
    std::vector<PersistedCodexThreadImportState> pendingStates;
    std::time_t minEventTime = 0;
    std::time_t maxEventTime = 0;

    for (const auto& row : readResult.threads) {
        if (row.threadId.empty() || row.modelProvider.empty() || row.model.empty() || row.tokensUsed <= 0) {
            ++result.skippedRows;
            continue;
        }

        const auto providerKey = mapCodexProviderToJiStatsProvider(row.modelProvider);
        if (!providerKey.has_value()) {
            ++result.skippedRows;
            continue;
        }
        const auto providerId = findProviderId(storage_, *providerKey);
        const auto accountId = providerId ? ensureLocalCodexAccount(storage_, *providerId, *providerKey) : std::nullopt;
        if (!accountId) {
            if (!sourceBaselineEstablished) {
                result.error = "Could not bind a Codex thread to its local account during baseline";
                return result;
            }
            ++result.skippedRows;
            continue;
        }

        auto existing =
            sourceBaselineEstablished ? storage_.getCodexThreadImportState(sourceId, row.threadId) : std::nullopt;
        PersistedCodexThreadImportState state;
        if (existing.has_value()) {
            state = *existing;
        } else {
            const bool importAsNewDelta =
                sourceBaselineEstablished &&
                isThreadObservedAfterSourceBaseline(row, sourceBaselineAt);
            state.sourceId = sourceId;
            state.threadId = row.threadId;
            state.modelProvider = row.modelProvider;
            state.modelKey = row.model;
            state.reasoningEffort = row.reasoningEffort;
            state.firstSeenCreatedAtMs = row.createdAtMs;
            state.lastSeenUpdatedAtMs = row.updatedAtMs;
            state.baselineTokensUsed = importAsNewDelta ? 0 : row.tokensUsed;
            state.lastSeenTokensUsed = row.tokensUsed;
            state.lastImportedTokensUsed = row.tokensUsed;
            state.baselineEstablishedAt = importAsNewDelta ? sourceBaselineAt : now;
            state.lastStatus = "active";
            state.updatedAt = now;
            if (importAsNewDelta) {
                const std::time_t eventTime = eventTimeFromUpdatedAtMs(row.updatedAtMs, now);
                PersistedUsageEvent event;
                event.providerId = *providerId;
                event.accountId = *accountId;
                event.eventTime = eventTime;
                event.sourceType = "codex_local_sqlite";
                event.sourceRunId = buildRunId(sourceId, row.threadId, row.updatedAtMs);
                event.modelKey = row.model;
                event.requestType = "codex_thread_delta";
                event.totalTokens = row.tokensUsed;
                event.costMinorUnits = 0;
                event.isBackfilled = false;
                event.rawReference = buildRawReference(sourceId, row.threadId, 0, row.tokensUsed);
                pendingEvents.push_back(event);

                state.lastEventTime = eventTime;
                minEventTime = minEventTime == 0 ? eventTime : std::min(minEventTime, eventTime);
                maxEventTime = std::max(maxEventTime, eventTime);
            } else {
                ++result.baselinedThreads;
            }
            pendingStates.push_back(state);
            continue;
        }

        state.modelProvider = row.modelProvider;
        state.modelKey = row.model;
        state.reasoningEffort = row.reasoningEffort;
        state.firstSeenCreatedAtMs = row.createdAtMs;
        state.lastSeenUpdatedAtMs = row.updatedAtMs;
        state.lastSeenTokensUsed = row.tokensUsed;
        state.updatedAt = now;

        if (row.tokensUsed > state.lastImportedTokensUsed) {
            const long long previousTokens = state.lastImportedTokensUsed;
            const long long delta = row.tokensUsed - previousTokens;
            const std::time_t eventTime = eventTimeFromUpdatedAtMs(row.updatedAtMs, now);

            PersistedUsageEvent event;
            event.providerId = *providerId;
            event.accountId = *accountId;
            event.eventTime = eventTime;
            event.sourceType = "codex_local_sqlite";
            event.sourceRunId = buildRunId(sourceId, row.threadId, row.updatedAtMs);
            event.modelKey = row.model;
            event.requestType = "codex_thread_delta";
            event.totalTokens = delta;
            event.costMinorUnits = 0;
            event.isBackfilled = false;
            event.rawReference = buildRawReference(sourceId, row.threadId, previousTokens, row.tokensUsed);
            pendingEvents.push_back(event);

            state.lastImportedTokensUsed = row.tokensUsed;
            state.lastEventTime = eventTime;
            state.lastStatus = "active";
            minEventTime = minEventTime == 0 ? eventTime : std::min(minEventTime, eventTime);
            maxEventTime = std::max(maxEventTime, eventTime);
        } else if (row.tokensUsed < state.lastImportedTokensUsed) {
            state.baselineTokensUsed = row.tokensUsed;
            state.lastImportedTokensUsed = row.tokensUsed;
            state.lastSeenTokensUsed = row.tokensUsed;
            state.baselineEstablishedAt = now;
            state.lastStatus = "reset_detected";
            state.counterResetCount += 1;
            ++result.resetThreads;
        }

        pendingStates.push_back(state);
    }

    if (!pendingEvents.empty() && !storage_.insertUsageEvents(pendingEvents)) {
        result.error = storage_.getLastError();
        return result;
    }

    for (const auto& state : pendingStates) {
        if (!storage_.upsertCodexThreadImportState(state)) {
            result.error = storage_.getLastError();
            return result;
        }
    }

    if (!transaction.commit()) {
        result.error = "Could not commit the Codex usage import";
        return result;
    }

    result.deltaEventsInserted = static_cast<int>(pendingEvents.size());
    if (minEventTime > 0) {
        for (const char* providerKey : {"openai", "hicap-ai"}) {
            const auto providerId = findProviderId(storage_, providerKey);
            if (!providerId.has_value()) continue;
            const auto accountId = ensureLocalCodexAccount(storage_, *providerId, providerKey);
            if (!accountId.has_value()) continue;
            storage_.rebuildUsageRollups(
                UsageRollupResolution::Hourly,
                *providerId,
                *accountId,
                minEventTime,
                maxEventTime + 1);
            storage_.rebuildUsageRollups(
                UsageRollupResolution::Daily,
                *providerId,
                *accountId,
                minEventTime,
                maxEventTime + 1);
        }
    }

    result.success = true;
    return result;
}

} // namespace JiStats
