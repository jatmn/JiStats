#include "db/StorageService.hpp"
#include "core/LocalPathReference.hpp"
#include "db/QueryResult.hpp"
#include "db/SchemaManager.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <map>
#include <new>
#include <tuple>

#ifdef _WIN32
#include <windows.h>
#endif

// Note: SQLite integration requires sqlite3.h to be available
// For now, we provide a minimal implementation that will compile
// but requires JISTATS_USE_SYSTEM_SQLITE or JISTATS_HAS_SQLITE to be defined

#if defined(JISTATS_USE_SYSTEM_SQLITE)
#include <sqlite3.h>
#elif defined(JISTATS_HAS_SQLITE)
#include "sqlite3.h"
#else
// Minimal SQLite stubs for compilation without SQLite
typedef struct sqlite3_struct sqlite3;
typedef struct sqlite3_stmt_struct sqlite3_stmt;
#define SQLITE_OK 0
#define SQLITE_DONE 101
#define SQLITE_ROW 100
#define SQLITE_NULL 5
#define SQLITE_TRANSIENT ((void(*)(void*))-1)
#endif

namespace JiStats {

namespace {

constexpr const char* SQLITE_UNAVAILABLE_ERROR =
    "SQLite not available - compile with JISTATS_USE_SYSTEM_SQLITE or JISTATS_HAS_SQLITE";
constexpr const char* DEFAULT_DB_FILENAME = "jistats.db";

std::filesystem::path getExecutableDirectory() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return std::filesystem::path(buffer).parent_path();
    }
#endif
    return {};
}

std::filesystem::path resolveDefaultDataDirectory() {
#ifdef _WIN32
    char* localAppData = nullptr;
    std::size_t localAppDataLength = 0;
    if (_dupenv_s(&localAppData, &localAppDataLength, "LOCALAPPDATA") == 0 && localAppData != nullptr) {
        const std::string localAppDataValue(localAppData);
        free(localAppData);
        if (!localAppDataValue.empty()) {
            return std::filesystem::path(localAppDataValue) / "JiStats";
        }
    }
#endif
    return std::filesystem::current_path();
}

bool shouldRedirectToStableDefaultPath(const std::string& dbPath) {
    return dbPath.empty() || dbPath == DEFAULT_DB_FILENAME;
}

bool isUsableDatabaseSeedCandidate(const std::filesystem::path& path) {
    std::error_code ec;
    return !path.empty() &&
        std::filesystem::exists(path, ec) &&
        !ec &&
        std::filesystem::is_regular_file(path, ec) &&
        !ec &&
        std::filesystem::file_size(path, ec) > 0;
}

std::filesystem::path chooseLegacyDatabaseSeedCandidate(const std::filesystem::path& targetPath) {
    std::vector<std::filesystem::path> candidates;
    candidates.push_back(std::filesystem::current_path() / DEFAULT_DB_FILENAME);
    const std::filesystem::path executableDirectory = getExecutableDirectory();
    if (!executableDirectory.empty()) {
        candidates.push_back(executableDirectory / DEFAULT_DB_FILENAME);
    }

    std::filesystem::path bestCandidate;
    std::filesystem::file_time_type bestWriteTime{};
    bool found = false;
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (candidate.empty() || candidate == targetPath || !isUsableDatabaseSeedCandidate(candidate)) {
            continue;
        }

        const auto writeTime = std::filesystem::last_write_time(candidate, ec);
        if (ec) {
            continue;
        }

        if (!found || writeTime > bestWriteTime) {
            bestCandidate = candidate;
            bestWriteTime = writeTime;
            found = true;
        }
    }

    return found ? bestCandidate : std::filesystem::path();
}

std::string resolveDatabasePath(const std::string& requestedPath) {
    if (!shouldRedirectToStableDefaultPath(requestedPath)) {
        return requestedPath;
    }

    const std::filesystem::path targetPath = resolveDefaultDataDirectory() / DEFAULT_DB_FILENAME;
    std::error_code ec;
    std::filesystem::create_directories(targetPath.parent_path(), ec);

    const bool targetUsable = isUsableDatabaseSeedCandidate(targetPath);
    if (!targetUsable) {
        const std::filesystem::path seedCandidate = chooseLegacyDatabaseSeedCandidate(targetPath);
        if (!seedCandidate.empty()) {
            std::filesystem::copy_file(
                seedCandidate,
                targetPath,
                std::filesystem::copy_options::overwrite_existing,
                ec);
        }
    }

    return targetPath.string();
}

bool isValidActivityEvent(const ActivityEvent& event, std::string& error) {
    if (event.providerKey.empty()) {
        error = "Activity event is missing providerKey";
        return false;
    }

    if (event.providerSurface.empty()) {
        error = "Activity event is missing providerSurface";
        return false;
    }

    if (event.startedAt <= 0) {
        error = "Activity event must have a valid startedAt";
        return false;
    }

    if (event.endedAt != 0 && event.endedAt < event.startedAt) {
        error = "Activity event endedAt cannot be earlier than startedAt";
        return false;
    }

    if (event.durationSeconds < 0) {
        error = "Activity event durationSeconds cannot be negative";
        return false;
    }

    return true;
}

std::string toUtcIsoString(std::time_t value) {
    if (value <= 0) {
        return "";
    }

    std::tm utcTime{};
#ifdef _WIN32
    if (gmtime_s(&utcTime, &value) != 0) {
        return "";
    }
#else
    if (gmtime_r(&value, &utcTime) == nullptr) {
        return "";
    }
#endif

    std::ostringstream stream;
    stream << std::put_time(&utcTime, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}

bool fromUtcIsoString(const std::string& value, std::time_t& parsedValue) {
    if (value.empty()) {
        return false;
    }

    std::tm utcTime{};
    std::istringstream stream(value);
    stream >> std::get_time(&utcTime, "%Y-%m-%dT%H:%M:%SZ");
    if (stream.fail()) {
        return false;
    }

#ifdef _WIN32
    parsedValue = _mkgmtime(&utcTime);
#else
    parsedValue = timegm(&utcTime);
#endif

    return parsedValue != static_cast<std::time_t>(-1);
}

ActivityKind parseActivityKind(const std::string& value) {
    if (value == "session_active") {
        return ActivityKind::SessionActive;
    }
    if (value == "message_send") {
        return ActivityKind::MessageSend;
    }
    if (value == "tool_run") {
        return ActivityKind::ToolRun;
    }
    return ActivityKind::UnknownActivity;
}

ActivityConfidence parseActivityConfidence(const std::string& value) {
    if (value == "LocalExact") {
        return ActivityConfidence::LocalExact;
    }
    if (value == "Imported") {
        return ActivityConfidence::Imported;
    }
    if (value == "Estimated") {
        return ActivityConfidence::Estimated;
    }
    return ActivityConfidence::Unknown;
}

std::time_t parseUtcOrZero(const char* value) {
    std::time_t parsedValue = 0;
    if (!value) {
        return 0;
    }

    return fromUtcIsoString(value, parsedValue) ? parsedValue : 0;
}

std::string rollupTableName(UsageRollupResolution resolution) {
    return resolution == UsageRollupResolution::Hourly
        ? "usage_rollups_hourly"
        : "usage_rollups_daily";
}

std::string rollupPeriodExpression(UsageRollupResolution resolution) {
    return resolution == UsageRollupResolution::Hourly
        ? "strftime('%Y-%m-%dT%H:00:00Z', event_time_utc)"
        : "strftime('%Y-%m-%dT00:00:00Z', event_time_utc)";
}

std::time_t normalizeRollupPeriodStart(UsageRollupResolution resolution, std::time_t value) {
    if (value <= 0) {
        return 0;
    }

    std::tm utcTime{};
#ifdef _WIN32
    if (gmtime_s(&utcTime, &value) != 0) {
        return 0;
    }
#else
    if (gmtime_r(&value, &utcTime) == nullptr) {
        return 0;
    }
#endif

    utcTime.tm_min = 0;
    utcTime.tm_sec = 0;
    if (resolution == UsageRollupResolution::Daily) {
        utcTime.tm_hour = 0;
    }

#ifdef _WIN32
    return _mkgmtime(&utcTime);
#else
    return timegm(&utcTime);
#endif
}

long long rollupWindowSeconds(UsageRollupResolution resolution) {
    return resolution == UsageRollupResolution::Hourly ? 3600LL : 86400LL;
}

int quotaPlanBucketId(const std::optional<int>& planId) {
    return planId.has_value() ? *planId : 0;
}

struct QuotaRollupKey {
    std::time_t periodStart = 0;
    int planBucketId = 0;
    std::string quotaDimension;

    bool operator<(const QuotaRollupKey& other) const {
        return std::tie(periodStart, planBucketId, quotaDimension) <
               std::tie(other.periodStart, other.planBucketId, other.quotaDimension);
    }
};

struct QuotaRollupAggregate {
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
    std::string latestConfidenceTier = "Unknown";
    std::optional<std::pair<std::time_t, std::time_t>> previousWindow;
};

std::optional<long long> normalizedQuotaRemainingValue(const PersistedQuotaSnapshot& snapshot) {
    if (snapshot.remainingValue.has_value()) {
        return std::max<long long>(0, *snapshot.remainingValue);
    }
    if (snapshot.limitValue.has_value()) {
        return std::max<long long>(0, *snapshot.limitValue - snapshot.usedValue);
    }
    return std::nullopt;
}

bool isValidSyncState(const PersistedSyncState& state, std::string& error) {
    if (state.providerId <= 0 || state.accountId <= 0) {
        error = "Sync state requires providerId and accountId";
        return false;
    }

    if (state.syncKind.empty()) {
        error = "Sync state requires syncKind";
        return false;
    }

    return true;
}

bool isValidMirrorRule(const PersistedMirrorRule& rule, std::string& error) {
    if (rule.providerId <= 0) {
        error = "Mirror rule requires providerId";
        return false;
    }

    if (rule.ruleKey.empty()) {
        error = "Mirror rule requires ruleKey";
        return false;
    }

    if (rule.ruleVersion <= 0) {
        error = "Mirror rule requires positive ruleVersion";
        return false;
    }

    return true;
}

bool isValidConnectivityState(const PersistedConnectivityState& state, std::string& error) {
    if (state.providerId <= 0 || state.accountId <= 0) {
        error = "Connectivity state requires providerId and accountId";
        return false;
    }

    if (state.status.empty()) {
        error = "Connectivity state requires status";
        return false;
    }

    return true;
}

bool isValidRuntimeState(const PersistedRuntimeState& state, std::string& error) {
    if (state.id != 1) {
        error = "Runtime state must use singleton id 1";
        return false;
    }

    return true;
}

bool isValidGapMarker(const PersistedGapMarker& marker, std::string& error) {
    if (marker.providerId <= 0 || marker.accountId <= 0) {
        error = "Gap marker requires providerId and accountId";
        return false;
    }

    if (marker.periodStart <= 0 || marker.periodEnd <= 0) {
        error = "Gap marker requires a valid period range";
        return false;
    }

    if (marker.periodEnd <= marker.periodStart) {
        error = "Gap marker requires periodEnd after periodStart";
        return false;
    }

    return true;
}

bool isValidProviderCapability(const PersistedProviderCapability& capability, std::string& error) {
    if (capability.providerId <= 0) {
        error = "Provider capability requires providerId";
        return false;
    }

    if (capability.capabilityKey.empty()) {
        error = "Provider capability requires capabilityKey";
        return false;
    }

    return true;
}

bool isValidCredentialProfile(const PersistedCredentialProfile& profile, std::string& error) {
    if (profile.providerId <= 0 || profile.accountId <= 0) {
        error = "Credential profile requires providerId and accountId";
        return false;
    }

    if (profile.profileName.empty()) {
        error = "Credential profile requires profileName";
        return false;
    }

    if (profile.secretRef.empty()) {
        error = "Credential profile requires secretRef";
        return false;
    }

    if (profile.credentialType.empty()) {
        error = "Credential profile requires credentialType";
        return false;
    }

    return true;
}

} // namespace

class StorageService::Impl {
public:
    sqlite3* db = nullptr;
    std::string dbPath;
    std::string lastError;
    std::optional<int> rejectedSchemaVersion;

    static int callback(void*, int, char**, char**) {
        // Callback implementation would be here
        return 0;
    }
};

StorageService::StorageService() : pImpl(std::make_unique<Impl>()) {
}

StorageService::~StorageService() {
    close();
}

bool StorageService::init(const std::string& dbPath) {
    pImpl->rejectedSchemaVersion.reset();
#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    pImpl->dbPath = resolveDatabasePath(dbPath);
    
    int rc = sqlite3_open(pImpl->dbPath.c_str(), &pImpl->db);
    if (rc != SQLITE_OK) {
        pImpl->lastError = "Failed to open database";
        return false;
    }

    const auto hashReference = [](sqlite3_context* context, int, sqlite3_value** values) {
        const auto* path = reinterpret_cast<const char*>(sqlite3_value_text(values[0]));
        if (path == nullptr) {
            sqlite3_result_null(context);
            return;
        }
        try {
            const auto length = static_cast<size_t>(sqlite3_value_bytes(values[0]));
            const auto reference = localPathReference(std::string_view(path, length));
            sqlite3_result_text(context, reference.c_str(), -1, SQLITE_TRANSIENT);
        } catch (const std::bad_alloc&) {
            sqlite3_result_error_nomem(context);
        }
    };
    if (sqlite3_create_function_v2(pImpl->db, "jistats_path_reference", 1,
                                   SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_DIRECTONLY, nullptr, hashReference,
                                   nullptr, nullptr, nullptr) != SQLITE_OK) {
        pImpl->lastError = "Failed to register local path reference normalization";
        close();
        return false;
    }

    // Give concurrent readers/writers a short grace period before surfacing
    // SQLITE_BUSY so background retention/recovery work can coexist with tests.
    sqlite3_busy_timeout(pImpl->db, 5000);
    
    execute("PRAGMA foreign_keys = ON;");
    
    // Run migrations
    SchemaManager schemaManager(*this);
    if (!schemaManager.migrate()) {
        pImpl->rejectedSchemaVersion = schemaManager.getRejectedVersion();
        pImpl->lastError = "Migration failed: " + schemaManager.getLastError();
        close();
        return false;
    }

    return true;
#else
    pImpl->lastError = "SQLite not available - compile with JISTATS_USE_SYSTEM_SQLITE or JISTATS_HAS_SQLITE";
    return false;
#endif
}

void StorageService::close() {
#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    if (pImpl->db) {
        sqlite3_close(pImpl->db);
        pImpl->db = nullptr;
    }
#endif
}

bool StorageService::isOpen() const {
    return pImpl->db != nullptr;
}

QueryResult StorageService::execute(const std::string& sql) {
    QueryResult result;

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    if (!pImpl->db) {
        result.pImpl->success_ = false;
        result.pImpl->error_ = "Database not open";
        return result;
    }

    const char* cursor = sql.c_str();
    while (cursor && *cursor != '\0') {
        sqlite3_stmt* stmt = nullptr;
        const char* tail = nullptr;
        const int prepareResult = sqlite3_prepare_v2(pImpl->db, cursor, -1, &stmt, &tail);
        if (prepareResult != SQLITE_OK) {
            result.pImpl->success_ = false;
            result.pImpl->error_ = "Failed to prepare statement";
            pImpl->lastError = result.pImpl->error_;
            if (stmt) {
                sqlite3_finalize(stmt);
            }
            return result;
        }

        if (!stmt) {
            cursor = tail;
            continue;
        }

        const int colCount = sqlite3_column_count(stmt);
        if (colCount > 0) {
            result.pImpl->columnNames_.clear();
            result.pImpl->data_.clear();
            result.pImpl->nullFlags_.clear();
            for (int i = 0; i < colCount; ++i) {
                result.pImpl->columnNames_.push_back(
                    sqlite3_column_name(stmt, i) ? sqlite3_column_name(stmt, i) : "");
            }
        }

        int rc = SQLITE_OK;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            std::vector<std::string> row;
            std::vector<bool> nullFlags;

            for (int i = 0; i < colCount; ++i) {
                if (sqlite3_column_type(stmt, i) == SQLITE_NULL) {
                    row.push_back("");
                    nullFlags.push_back(true);
                } else {
                    const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
                    row.push_back(text ? text : "");
                    nullFlags.push_back(false);
                }
            }

            result.pImpl->data_.push_back(std::move(row));
            result.pImpl->nullFlags_.push_back(std::move(nullFlags));
        }

        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            result.pImpl->success_ = false;
            result.pImpl->error_ = "Execution failed";
            pImpl->lastError = result.pImpl->error_;
            return result;
        }

        cursor = tail;
    }

    result.pImpl->success_ = true;
    result.pImpl->lastInsertRowId_ = sqlite3_last_insert_rowid(pImpl->db);
    result.pImpl->rowsAffected_ = sqlite3_changes(pImpl->db);
    pImpl->lastError.clear();
#else
    result.pImpl->success_ = false;
    result.pImpl->error_ = SQLITE_UNAVAILABLE_ERROR;
#endif

    return result;
}

QueryResult StorageService::execute(const std::string& sql, const std::vector<std::string>& params) {
    QueryResult result;
    
#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    if (!pImpl->db) {
        result.pImpl->success_ = false;
        result.pImpl->error_ = "Database not open";
        return result;
    }
    
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(pImpl->db, sql.c_str(), -1, &stmt, nullptr);
    
    if (rc != SQLITE_OK) {
        result.pImpl->success_ = false;
        result.pImpl->error_ = "Failed to prepare statement";
        return result;
    }
    
    // Bind parameters
    for (size_t i = 0; i < params.size(); i++) {
        sqlite3_bind_text(stmt, static_cast<int>(i + 1), params[i].c_str(), -1, SQLITE_TRANSIENT);
    }
    
    // Get column names
    int colCount = sqlite3_column_count(stmt);
    for (int i = 0; i < colCount; i++) {
        result.pImpl->columnNames_.push_back(sqlite3_column_name(stmt, i) ? sqlite3_column_name(stmt, i) : "");
    }
    
    // Execute and fetch results
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        std::vector<std::string> row;
        std::vector<bool> nullFlags;
        
        for (int i = 0; i < colCount; i++) {
            if (sqlite3_column_type(stmt, i) == SQLITE_NULL) {
                row.push_back("");
                nullFlags.push_back(true);
            } else {
                const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
                row.push_back(text ? text : "");
                nullFlags.push_back(false);
            }
        }
        
        result.pImpl->data_.push_back(row);
        result.pImpl->nullFlags_.push_back(nullFlags);
    }
    
    if (rc != SQLITE_DONE) {
        result.pImpl->success_ = false;
        result.pImpl->error_ = "Execution failed";
    } else {
        result.pImpl->success_ = true;
        result.pImpl->lastInsertRowId_ = sqlite3_last_insert_rowid(pImpl->db);
        result.pImpl->rowsAffected_ = sqlite3_changes(pImpl->db);
    }
    
    sqlite3_finalize(stmt);
#else
    result.pImpl->success_ = false;
    result.pImpl->error_ = SQLITE_UNAVAILABLE_ERROR;
#endif
    
    return result;
}

bool StorageService::beginTransaction(TransactionMode mode) {
#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    if (pImpl->db && sqlite3_get_autocommit(pImpl->db) == 0) {
        return mode == TransactionMode::Deferred;
    }
#endif
    return execute(mode == TransactionMode::Immediate ? "BEGIN IMMEDIATE;" : "BEGIN TRANSACTION;").success();
}

bool StorageService::commitTransaction() {
#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    if (pImpl->db && sqlite3_get_autocommit(pImpl->db) == 0) {
        return execute("COMMIT;").success();
    }
#endif
    return true;
}

bool StorageService::rollbackTransaction() {
#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    // SQLite can already have rolled back a transaction on errors such as SQLITE_FULL.
    if (pImpl->db && sqlite3_get_autocommit(pImpl->db) == 0) {
        return execute("ROLLBACK;").success();
    }
#endif
    return true;
}

std::string StorageService::getLastError() const {
    return pImpl->lastError;
}

std::optional<int> StorageService::getRejectedSchemaVersion() const {
    return pImpl->rejectedSchemaVersion;
}

std::string StorageService::getDbPath() const {
    return pImpl->dbPath;
}

bool StorageService::insertActivityEvent(const PersistedActivityEvent& event, long long* insertedId) {
    if (!isOpen()) {
        pImpl->lastError = "Database not open";
        return false;
    }

    if (event.providerId <= 0 || event.accountId <= 0) {
        pImpl->lastError = "Activity event requires providerId and accountId";
        return false;
    }

    if (event.activity.providerSurface.empty()) {
        pImpl->lastError = "Activity event requires providerSurface";
        return false;
    }

    if (event.activity.startedAt <= 0) {
        pImpl->lastError = "Activity event requires a valid startedAt";
        return false;
    }

    if (event.activity.endedAt != 0 && event.activity.endedAt < event.activity.startedAt) {
        pImpl->lastError = "Activity event endedAt cannot be earlier than startedAt";
        return false;
    }

    if (event.activity.durationSeconds < 0) {
        pImpl->lastError = "Activity event durationSeconds cannot be negative";
        return false;
    }

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    static constexpr char claudeDailyActivityPredicate[] =
        "provider_surface = 'claude_code_local_stats' AND activity_kind = 'session_active' "
        "AND source_type = 'local' AND surface_key = 'stats-cache' AND raw_reference GLOB 'scope:fnv1a64:?*'";
    auto activityEventExists = [this](const PersistedActivityEvent& candidate) -> std::optional<bool> {
        const auto& activity = candidate.activity;
        const auto& reference = activity.rawReference;
        if (activity.providerSurface == "claude_code_local_stats" &&
            activity.activityKind == ActivityKind::SessionActive &&
            (activity.sourceType.empty() || activity.sourceType == "local") && activity.surfaceKey == "stats-cache" &&
            reference.size() > kLocalPathReferencePrefix.size() &&
            reference.compare(0, kLocalPathReferencePrefix.size(), kLocalPathReferencePrefix) == 0) {
            // A conflicting AUTOINCREMENT insert still advances sqlite_sequence.
            static const std::string lookup =
                std::string("SELECT 1 FROM activity_events WHERE provider_id = ? AND account_id = ? "
                            "AND started_at_utc = ? AND raw_reference = ? AND ") +
                claudeDailyActivityPredicate + " LIMIT 1";
            const auto result =
                execute(lookup, {std::to_string(candidate.providerId), std::to_string(candidate.accountId),
                                 toUtcIsoString(activity.startedAt), reference});
            return result.success() ? std::optional<bool>(result.rowCount() != 0) : std::nullopt;
        }

        std::string sql = "SELECT 1 FROM activity_events "
                          "WHERE provider_id = ? AND account_id = ? AND ";
        std::vector<std::string> params = {
            std::to_string(candidate.providerId),
            std::to_string(candidate.accountId)
        };

        if (candidate.planId.has_value()) {
            sql += "plan_id = ? AND ";
            params.push_back(std::to_string(*candidate.planId));
        } else {
            sql += "plan_id IS NULL AND ";
        }

        sql +=
            "provider_surface = ? AND activity_kind = ? AND started_at_utc = ? AND ";
        params.push_back(candidate.activity.providerSurface);
        params.push_back(toString(candidate.activity.activityKind));
        params.push_back(toUtcIsoString(candidate.activity.startedAt));

        if (candidate.activity.endedAt > 0) {
            sql += "ended_at_utc = ? AND ";
            params.push_back(toUtcIsoString(candidate.activity.endedAt));
        } else {
            sql += "ended_at_utc IS NULL AND ";
        }

        sql += "duration_seconds = ? AND source_type = ? AND ";
        params.push_back(std::to_string(candidate.activity.durationSeconds));
        params.push_back(candidate.activity.sourceType.empty() ? "local" : candidate.activity.sourceType);

        if (!candidate.activity.sourceRunId.empty()) {
            sql += "source_run_id = ? AND ";
            params.push_back(candidate.activity.sourceRunId);
        } else {
            sql += "source_run_id IS NULL AND ";
        }

        if (!candidate.activity.surfaceKey.empty()) {
            sql += "surface_key = ? AND ";
            params.push_back(candidate.activity.surfaceKey);
        } else {
            sql += "surface_key IS NULL AND ";
        }

        if (!candidate.activity.windowTitleHint.empty()) {
            sql += "window_title_hint = ? AND ";
            params.push_back(candidate.activity.windowTitleHint);
        } else {
            sql += "window_title_hint IS NULL AND ";
        }

        sql += "confidence_tier = ? AND ";
        params.push_back(toString(candidate.activity.confidence));

        if (!candidate.activity.rawReference.empty()) {
            sql += "raw_reference = ? ";
            params.push_back(candidate.activity.rawReference);
        } else {
            sql += "raw_reference IS NULL ";
        }

        sql += "LIMIT 1";

        QueryResult result = execute(sql, params);
        if (!result.success()) {
            return std::nullopt;
        }

        return result.rowCount() > 0;
    };

    const auto alreadyExists = activityEventExists(event);
    if (!alreadyExists.has_value()) {
        pImpl->lastError = "Failed to check for duplicate activity event";
        return false;
    }
    if (*alreadyExists) {
        pImpl->lastError.clear();
        return true;
    }

    constexpr const char* insertSql =
        "INSERT INTO activity_events ("
        "provider_id, account_id, plan_id, provider_surface, activity_kind, started_at_utc, ended_at_utc, "
        "duration_seconds, source_type, source_run_id, surface_key, window_title_hint, confidence_tier, raw_reference"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    static const std::string insertWithClaudeConflictSql =
        std::string(insertSql) + " ON CONFLICT(provider_id, account_id, started_at_utc, raw_reference) WHERE " +
        claudeDailyActivityPredicate + " DO NOTHING;";

    // A task's runtime status and duration change during its lifetime, but its reference does not.
    // Preserve terminal states even when a refresh replays an earlier, unfinished snapshot.
    static const std::string upsertKimiTaskSql = std::string(insertSql) + R"sql(
            ON CONFLICT(provider_id, account_id, raw_reference)
            WHERE provider_surface = 'kimi_local_runtime' AND source_type = 'local'
              AND activity_kind = 'session_active' AND raw_reference IS NOT NULL
            DO UPDATE SET plan_id = excluded.plan_id, started_at_utc = excluded.started_at_utc,
                          ended_at_utc = excluded.ended_at_utc, duration_seconds = excluded.duration_seconds,
                          surface_key = excluded.surface_key, window_title_hint = excluded.window_title_hint
            WHERE (COALESCE(lower(excluded.surface_key) IN ('completed', 'failed', 'killed', 'lost', 'succeeded', 'success'), 0),
                   COALESCE(excluded.ended_at_utc, '')) >=
                  (COALESCE(lower(activity_events.surface_key) IN ('completed', 'failed', 'killed', 'lost', 'succeeded', 'success'), 0),
                   COALESCE(activity_events.ended_at_utc, ''))
        )sql";

    const std::string sourceType = event.activity.sourceType.empty() ? "local" : event.activity.sourceType;
    const bool isKimiTask = event.activity.providerSurface == "kimi_local_runtime" && sourceType == "local" &&
                            event.activity.activityKind == ActivityKind::SessionActive &&
                            !event.activity.rawReference.empty();
    const auto& sql = isKimiTask ? upsertKimiTaskSql : insertWithClaudeConflictSql;
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(pImpl->db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        pImpl->lastError = "Failed to prepare activity event insert";
        return false;
    }

    sqlite3_bind_int(stmt, 1, event.providerId);
    sqlite3_bind_int(stmt, 2, event.accountId);
    if (event.planId.has_value()) {
        sqlite3_bind_int(stmt, 3, *event.planId);
    } else {
        sqlite3_bind_null(stmt, 3);
    }

    const std::string startedAtUtc = toUtcIsoString(event.activity.startedAt);
    const std::string endedAtUtc = toUtcIsoString(event.activity.endedAt);

    sqlite3_bind_text(stmt, 4, event.activity.providerSurface.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, toString(event.activity.activityKind).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, startedAtUtc.c_str(), -1, SQLITE_TRANSIENT);
    if (event.activity.endedAt > 0) {
        sqlite3_bind_text(stmt, 7, endedAtUtc.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 7);
    }
    sqlite3_bind_int64(stmt, 8, event.activity.durationSeconds);
    sqlite3_bind_text(stmt, 9, sourceType.c_str(), -1, SQLITE_TRANSIENT);
    if (!event.activity.sourceRunId.empty()) {
        sqlite3_bind_text(stmt, 10, event.activity.sourceRunId.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 10);
    }
    if (!event.activity.surfaceKey.empty()) {
        sqlite3_bind_text(stmt, 11, event.activity.surfaceKey.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 11);
    }
    if (!event.activity.windowTitleHint.empty()) {
        sqlite3_bind_text(stmt, 12, event.activity.windowTitleHint.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 12);
    }
    sqlite3_bind_text(stmt, 13, toString(event.activity.confidence).c_str(), -1, SQLITE_TRANSIENT);
    if (!event.activity.rawReference.empty()) {
        sqlite3_bind_text(stmt, 14, event.activity.rawReference.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 14);
    }

    // Keep the conflict result and insert ID tied to this step on the shared connection.
    sqlite3_mutex* const dbMutex = sqlite3_db_mutex(pImpl->db);
    sqlite3_mutex_enter(dbMutex);
    rc = sqlite3_step(stmt);
    const bool readWrittenId = rc == SQLITE_DONE && insertedId && sqlite3_changes(pImpl->db) != 0;
    if (readWrittenId && !isKimiTask) {
        *insertedId = sqlite3_last_insert_rowid(pImpl->db);
    }
    sqlite3_mutex_leave(dbMutex);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        pImpl->lastError = "Failed to insert activity event";
        return false;
    }

    if (readWrittenId && isKimiTask) {
        // Read the keyed ID after the upsert so system SQLite builds before 3.35 can save tasks.
        const auto task =
            execute("SELECT id FROM activity_events WHERE provider_id = ? AND account_id = ? AND raw_reference = ? "
                    "AND provider_surface = 'kimi_local_runtime' AND source_type = 'local' AND activity_kind = "
                    "'session_active';",
                    {std::to_string(event.providerId), std::to_string(event.accountId), event.activity.rawReference});
        if (!task.success() || task.rowCount() != 1) {
            pImpl->lastError = "Failed to read activity event ID";
            return false;
        }
        *insertedId = task.getInt64(0, 0);
    }
    pImpl->lastError.clear();
    return true;
#else
    pImpl->lastError = SQLITE_UNAVAILABLE_ERROR;
    return false;
#endif
}

std::vector<PersistedActivityEvent> StorageService::getActivityEvents(
    int providerId,
    int accountId,
    std::time_t startedAtFrom,
    std::time_t startedAtTo) {
    std::vector<PersistedActivityEvent> events;

    if (!isOpen() || providerId <= 0 || accountId <= 0 || startedAtTo < startedAtFrom) {
        return events;
    }

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT ae.id, ae.provider_id, ae.account_id, ae.plan_id, ae.provider_surface, ae.activity_kind, ae.started_at_utc, ae.ended_at_utc, "
        "ae.duration_seconds, ae.source_type, ae.source_run_id, ae.surface_key, ae.window_title_hint, ae.confidence_tier, ae.raw_reference, ae.created_at_utc, "
        "p.provider_key "
        "FROM activity_events ae "
        "JOIN providers p ON p.id = ae.provider_id "
        "WHERE ae.provider_id = ? AND ae.account_id = ? AND ae.started_at_utc >= ? AND ae.started_at_utc <= ? "
        "ORDER BY ae.started_at_utc ASC, ae.id ASC;";

    if (sqlite3_prepare_v2(pImpl->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        pImpl->lastError = "Failed to prepare activity event query";
        return events;
    }

    const std::string fromUtc = toUtcIsoString(startedAtFrom);
    const std::string toUtc = toUtcIsoString(startedAtTo);
    sqlite3_bind_int(stmt, 1, providerId);
    sqlite3_bind_int(stmt, 2, accountId);
    sqlite3_bind_text(stmt, 3, fromUtc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, toUtc.c_str(), -1, SQLITE_TRANSIENT);

    auto parseActivityKind = [](const std::string& value) {
        if (value == "session_active") {
            return ActivityKind::SessionActive;
        }
        if (value == "message_send") {
            return ActivityKind::MessageSend;
        }
        if (value == "tool_run") {
            return ActivityKind::ToolRun;
        }
        return ActivityKind::UnknownActivity;
    };

    auto parseConfidence = [](const std::string& value) {
        if (value == "LocalExact") {
            return ActivityConfidence::LocalExact;
        }
        if (value == "Imported") {
            return ActivityConfidence::Imported;
        }
        if (value == "Estimated") {
            return ActivityConfidence::Estimated;
        }
        return ActivityConfidence::Unknown;
    };

    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        PersistedActivityEvent persisted;
        persisted.id = sqlite3_column_int64(stmt, 0);
        persisted.providerId = sqlite3_column_int(stmt, 1);
        persisted.accountId = sqlite3_column_int(stmt, 2);
        if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
            persisted.planId = sqlite3_column_int(stmt, 3);
        }

        const char* providerSurface = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        const char* activityKind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        const char* startedAtUtc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        const char* endedAtUtc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        const char* sourceType = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        const char* sourceRunId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        const char* surfaceKey = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
        const char* windowTitleHint = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
        const char* confidenceTier = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
        const char* rawReference = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 14));
        const char* createdAtUtc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 15));
        const char* providerKey = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 16));

        persisted.activity.providerKey = providerKey ? providerKey : "";
        persisted.activity.providerSurface = providerSurface ? providerSurface : "";
        persisted.activity.activityKind = parseActivityKind(activityKind ? activityKind : "");
        persisted.activity.startedAt = parseUtcOrZero(startedAtUtc);
        persisted.activity.endedAt = parseUtcOrZero(endedAtUtc);
        persisted.activity.durationSeconds = sqlite3_column_int64(stmt, 8);
        persisted.activity.sourceType = sourceType ? sourceType : "";
        persisted.activity.sourceRunId = sourceRunId ? sourceRunId : "";
        persisted.activity.surfaceKey = surfaceKey ? surfaceKey : "";
        persisted.activity.windowTitleHint = windowTitleHint ? windowTitleHint : "";
        persisted.activity.confidence = parseConfidence(confidenceTier ? confidenceTier : "");
        persisted.activity.rawReference = rawReference ? rawReference : "";
        persisted.createdAtUtc = createdAtUtc ? createdAtUtc : "";
        events.push_back(std::move(persisted));
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        pImpl->lastError = "Failed to query activity events";
    } else {
        pImpl->lastError.clear();
    }
#endif

    return events;
}

bool StorageService::upsertSyncState(const PersistedSyncState& state, long long* rowId) {
    if (!isOpen()) {
        pImpl->lastError = "Database not open";
        return false;
    }

    std::string error;
    if (!isValidSyncState(state, error)) {
        pImpl->lastError = error;
        return false;
    }

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO sync_state ("
        "provider_id, account_id, sync_kind, cursor_value, last_attempt_at_utc, last_success_at_utc, "
        "last_failure_at_utc, last_failure_category"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(provider_id, account_id, sync_kind) DO UPDATE SET "
        "cursor_value = excluded.cursor_value, "
        "last_attempt_at_utc = excluded.last_attempt_at_utc, "
        "last_success_at_utc = excluded.last_success_at_utc, "
        "last_failure_at_utc = excluded.last_failure_at_utc, "
        "last_failure_category = excluded.last_failure_category, "
        "updated_at_utc = (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'));";

    if (sqlite3_prepare_v2(pImpl->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        pImpl->lastError = "Failed to prepare sync state upsert";
        return false;
    }

    const std::string lastAttemptAtUtc = toUtcIsoString(state.lastAttemptAt);
    const std::string lastSuccessAtUtc = toUtcIsoString(state.lastSuccessAt);
    const std::string lastFailureAtUtc = toUtcIsoString(state.lastFailureAt);

    sqlite3_bind_int(stmt, 1, state.providerId);
    sqlite3_bind_int(stmt, 2, state.accountId);
    sqlite3_bind_text(stmt, 3, state.syncKind.c_str(), -1, SQLITE_TRANSIENT);
    if (!state.cursorValue.empty()) {
        sqlite3_bind_text(stmt, 4, state.cursorValue.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 4);
    }
    if (!lastAttemptAtUtc.empty()) {
        sqlite3_bind_text(stmt, 5, lastAttemptAtUtc.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 5);
    }
    if (!lastSuccessAtUtc.empty()) {
        sqlite3_bind_text(stmt, 6, lastSuccessAtUtc.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 6);
    }
    if (!lastFailureAtUtc.empty()) {
        sqlite3_bind_text(stmt, 7, lastFailureAtUtc.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 7);
    }
    if (!state.lastFailureCategory.empty()) {
        sqlite3_bind_text(stmt, 8, state.lastFailureCategory.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 8);
    }

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        pImpl->lastError = "Failed to upsert sync state";
        return false;
    }

    if (rowId) {
        auto stateRow = getSyncState(state.providerId, state.accountId, state.syncKind);
        *rowId = stateRow.has_value() ? stateRow->id : 0;
    }
    pImpl->lastError.clear();
    return true;
#else
    pImpl->lastError = SQLITE_UNAVAILABLE_ERROR;
    return false;
#endif
}

std::optional<PersistedSyncState> StorageService::getSyncState(
    int providerId,
    int accountId,
    const std::string& syncKind) {
    if (!isOpen() || providerId <= 0 || accountId <= 0 || syncKind.empty()) {
        return std::nullopt;
    }

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, provider_id, account_id, sync_kind, cursor_value, last_attempt_at_utc, last_success_at_utc, "
        "last_failure_at_utc, last_failure_category, created_at_utc, updated_at_utc "
        "FROM sync_state WHERE provider_id = ? AND account_id = ? AND sync_kind = ?;";

    if (sqlite3_prepare_v2(pImpl->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        pImpl->lastError = "Failed to prepare sync state query";
        return std::nullopt;
    }

    sqlite3_bind_int(stmt, 1, providerId);
    sqlite3_bind_int(stmt, 2, accountId);
    sqlite3_bind_text(stmt, 3, syncKind.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        if (rc == SQLITE_DONE) {
            pImpl->lastError.clear();
        } else {
            pImpl->lastError = "Failed to query sync state";
        }
        return std::nullopt;
    }

    PersistedSyncState state;
    state.id = sqlite3_column_int64(stmt, 0);
    state.providerId = sqlite3_column_int(stmt, 1);
    state.accountId = sqlite3_column_int(stmt, 2);
    state.syncKind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    const char* cursorValue = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    const char* lastFailureCategory = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
    const char* createdAtUtc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
    const char* updatedAtUtc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));

    state.cursorValue = cursorValue ? cursorValue : "";
    state.lastAttemptAt = parseUtcOrZero(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)));
    state.lastSuccessAt = parseUtcOrZero(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6)));
    state.lastFailureAt = parseUtcOrZero(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7)));
    state.lastFailureCategory = lastFailureCategory ? lastFailureCategory : "";
    state.createdAtUtc = createdAtUtc ? createdAtUtc : "";
    state.updatedAtUtc = updatedAtUtc ? updatedAtUtc : "";

    sqlite3_finalize(stmt);
    pImpl->lastError.clear();
    return state;
#else
    pImpl->lastError = SQLITE_UNAVAILABLE_ERROR;
    return std::nullopt;
#endif
}

bool StorageService::upsertMirrorRule(const PersistedMirrorRule& rule, long long* rowId) {
    if (!isOpen()) {
        pImpl->lastError = "Database not open";
        return false;
    }

    std::string error;
    if (!isValidMirrorRule(rule, error)) {
        pImpl->lastError = error;
        return false;
    }

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO mirror_rules (provider_id, rule_key, rule_version, config_json) "
        "VALUES (?, ?, ?, ?) "
        "ON CONFLICT(provider_id, rule_key, rule_version) DO UPDATE SET "
        "config_json = excluded.config_json;";

    if (sqlite3_prepare_v2(pImpl->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        pImpl->lastError = "Failed to prepare mirror rule upsert";
        return false;
    }

    sqlite3_bind_int(stmt, 1, rule.providerId);
    sqlite3_bind_text(stmt, 2, rule.ruleKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, rule.ruleVersion);
    if (!rule.configJson.empty()) {
        sqlite3_bind_text(stmt, 4, rule.configJson.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 4);
    }

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        pImpl->lastError = "Failed to upsert mirror rule";
        return false;
    }

    if (rowId) {
        auto rules = getMirrorRules(rule.providerId);
        *rowId = 0;
        for (const auto& candidate : rules) {
            if (candidate.ruleKey == rule.ruleKey && candidate.ruleVersion == rule.ruleVersion) {
                *rowId = candidate.id;
                break;
            }
        }
    }
    pImpl->lastError.clear();
    return true;
#else
    pImpl->lastError = SQLITE_UNAVAILABLE_ERROR;
    return false;
#endif
}

std::vector<PersistedMirrorRule> StorageService::getMirrorRules(int providerId) {
    std::vector<PersistedMirrorRule> rules;
    if (!isOpen() || providerId <= 0) {
        return rules;
    }

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, provider_id, rule_key, rule_version, config_json, created_at_utc "
        "FROM mirror_rules WHERE provider_id = ? ORDER BY rule_key ASC, rule_version ASC;";

    if (sqlite3_prepare_v2(pImpl->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        pImpl->lastError = "Failed to prepare mirror rule query";
        return rules;
    }

    sqlite3_bind_int(stmt, 1, providerId);
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        PersistedMirrorRule rule;
        rule.id = sqlite3_column_int64(stmt, 0);
        rule.providerId = sqlite3_column_int(stmt, 1);
        const char* ruleKey = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* configJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        const char* createdAtUtc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        rule.ruleKey = ruleKey ? ruleKey : "";
        rule.ruleVersion = sqlite3_column_int(stmt, 3);
        rule.configJson = configJson ? configJson : "";
        rule.createdAtUtc = createdAtUtc ? createdAtUtc : "";
        rules.push_back(std::move(rule));
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        pImpl->lastError = "Failed to query mirror rules";
    } else {
        pImpl->lastError.clear();
    }
#endif

    return rules;
}

bool StorageService::upsertConnectivityState(const PersistedConnectivityState& state, long long* rowId) {
    if (!isOpen()) {
        pImpl->lastError = "Database not open";
        return false;
    }

    std::string error;
    if (!isValidConnectivityState(state, error)) {
        pImpl->lastError = error;
        return false;
    }

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO connectivity_state ("
        "provider_id, account_id, status, failure_category, last_checked_at_utc, next_retry_at_utc, notice_dismissed_at_utc"
        ") VALUES (?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(provider_id, account_id) DO UPDATE SET "
        "status = excluded.status, "
        "failure_category = excluded.failure_category, "
        "last_checked_at_utc = excluded.last_checked_at_utc, "
        "next_retry_at_utc = excluded.next_retry_at_utc, "
        "notice_dismissed_at_utc = excluded.notice_dismissed_at_utc, "
        "updated_at_utc = (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'));";

    if (sqlite3_prepare_v2(pImpl->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        pImpl->lastError = "Failed to prepare connectivity state upsert";
        return false;
    }

    const std::string lastCheckedAtUtc = toUtcIsoString(state.lastCheckedAt);
    const std::string nextRetryAtUtc = toUtcIsoString(state.nextRetryAt);
    const std::string noticeDismissedAtUtc = toUtcIsoString(state.noticeDismissedAt);

    sqlite3_bind_int(stmt, 1, state.providerId);
    sqlite3_bind_int(stmt, 2, state.accountId);
    sqlite3_bind_text(stmt, 3, state.status.c_str(), -1, SQLITE_TRANSIENT);
    if (!state.failureCategory.empty()) {
        sqlite3_bind_text(stmt, 4, state.failureCategory.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 4);
    }
    if (!lastCheckedAtUtc.empty()) {
        sqlite3_bind_text(stmt, 5, lastCheckedAtUtc.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 5);
    }
    if (!nextRetryAtUtc.empty()) {
        sqlite3_bind_text(stmt, 6, nextRetryAtUtc.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 6);
    }
    if (!noticeDismissedAtUtc.empty()) {
        sqlite3_bind_text(stmt, 7, noticeDismissedAtUtc.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 7);
    }

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        pImpl->lastError = "Failed to upsert connectivity state";
        return false;
    }

    if (rowId) {
        auto currentState = getConnectivityState(state.providerId, state.accountId);
        *rowId = currentState.has_value() ? currentState->id : 0;
    }
    pImpl->lastError.clear();
    return true;
#else
    pImpl->lastError = SQLITE_UNAVAILABLE_ERROR;
    return false;
#endif
}

std::optional<PersistedConnectivityState> StorageService::getConnectivityState(int providerId, int accountId) {
    if (!isOpen() || providerId <= 0 || accountId <= 0) {
        return std::nullopt;
    }

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, provider_id, account_id, status, failure_category, last_checked_at_utc, next_retry_at_utc, "
        "notice_dismissed_at_utc, created_at_utc, updated_at_utc "
        "FROM connectivity_state WHERE provider_id = ? AND account_id = ?;";

    if (sqlite3_prepare_v2(pImpl->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        pImpl->lastError = "Failed to prepare connectivity state query";
        return std::nullopt;
    }

    sqlite3_bind_int(stmt, 1, providerId);
    sqlite3_bind_int(stmt, 2, accountId);

    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        if (rc == SQLITE_DONE) {
            pImpl->lastError.clear();
        } else {
            pImpl->lastError = "Failed to query connectivity state";
        }
        return std::nullopt;
    }

    PersistedConnectivityState state;
    state.id = sqlite3_column_int64(stmt, 0);
    state.providerId = sqlite3_column_int(stmt, 1);
    state.accountId = sqlite3_column_int(stmt, 2);
    const char* status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    const char* failureCategory = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    const char* createdAtUtc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
    const char* updatedAtUtc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));

    state.status = status ? status : "";
    state.failureCategory = failureCategory ? failureCategory : "";
    state.lastCheckedAt = parseUtcOrZero(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)));
    state.nextRetryAt = parseUtcOrZero(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6)));
    state.noticeDismissedAt = parseUtcOrZero(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7)));
    state.createdAtUtc = createdAtUtc ? createdAtUtc : "";
    state.updatedAtUtc = updatedAtUtc ? updatedAtUtc : "";

    sqlite3_finalize(stmt);
    pImpl->lastError.clear();
    return state;
#else
    pImpl->lastError = SQLITE_UNAVAILABLE_ERROR;
    return std::nullopt;
#endif
}

bool StorageService::upsertRuntimeState(const PersistedRuntimeState& state) {
    if (!isOpen()) {
        pImpl->lastError = "Database not open";
        return false;
    }

    std::string error;
    if (!isValidRuntimeState(state, error)) {
        pImpl->lastError = error;
        return false;
    }

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO runtime_state ("
        "id, last_launch_at_utc, last_successful_sync_utc, last_local_observation_utc, recovery_marker"
        ") VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "last_launch_at_utc = excluded.last_launch_at_utc, "
        "last_successful_sync_utc = excluded.last_successful_sync_utc, "
        "last_local_observation_utc = excluded.last_local_observation_utc, "
        "recovery_marker = excluded.recovery_marker, "
        "updated_at_utc = (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'));";

    if (sqlite3_prepare_v2(pImpl->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        pImpl->lastError = "Failed to prepare runtime state upsert";
        return false;
    }

    const std::string lastLaunchAtUtc = toUtcIsoString(state.lastLaunchAt);
    const std::string lastSuccessfulSyncUtc = toUtcIsoString(state.lastSuccessfulSyncAt);
    const std::string lastLocalObservationUtc = toUtcIsoString(state.lastLocalObservationAt);

    sqlite3_bind_int(stmt, 1, state.id);
    if (!lastLaunchAtUtc.empty()) {
        sqlite3_bind_text(stmt, 2, lastLaunchAtUtc.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 2);
    }
    if (!lastSuccessfulSyncUtc.empty()) {
        sqlite3_bind_text(stmt, 3, lastSuccessfulSyncUtc.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 3);
    }
    if (!lastLocalObservationUtc.empty()) {
        sqlite3_bind_text(stmt, 4, lastLocalObservationUtc.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 4);
    }
    if (!state.recoveryMarker.empty()) {
        sqlite3_bind_text(stmt, 5, state.recoveryMarker.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 5);
    }

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        pImpl->lastError = "Failed to upsert runtime state";
        return false;
    }

    pImpl->lastError.clear();
    return true;
#else
    pImpl->lastError = SQLITE_UNAVAILABLE_ERROR;
    return false;
#endif
}

std::optional<PersistedRuntimeState> StorageService::getRuntimeState() {
    if (!isOpen()) {
        return std::nullopt;
    }

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, last_launch_at_utc, last_successful_sync_utc, last_local_observation_utc, recovery_marker, "
        "created_at_utc, updated_at_utc "
        "FROM runtime_state WHERE id = 1;";

    if (sqlite3_prepare_v2(pImpl->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        pImpl->lastError = "Failed to prepare runtime state query";
        return std::nullopt;
    }

    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        if (rc == SQLITE_DONE) {
            pImpl->lastError.clear();
        } else {
            pImpl->lastError = "Failed to query runtime state";
        }
        return std::nullopt;
    }

    PersistedRuntimeState state;
    state.id = sqlite3_column_int(stmt, 0);
    state.lastLaunchAt = parseUtcOrZero(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
    state.lastSuccessfulSyncAt = parseUtcOrZero(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
    state.lastLocalObservationAt = parseUtcOrZero(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
    const char* recoveryMarker = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    const char* createdAtUtc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    const char* updatedAtUtc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
    state.recoveryMarker = recoveryMarker ? recoveryMarker : "";
    state.createdAtUtc = createdAtUtc ? createdAtUtc : "";
    state.updatedAtUtc = updatedAtUtc ? updatedAtUtc : "";

    sqlite3_finalize(stmt);
    pImpl->lastError.clear();
    return state;
#else
    pImpl->lastError = SQLITE_UNAVAILABLE_ERROR;
    return std::nullopt;
#endif
}

bool StorageService::upsertGapMarker(const PersistedGapMarker& marker, long long* rowId) {
    if (!isOpen()) {
        pImpl->lastError = "Database not open";
        return false;
    }

    std::string error;
    if (!isValidGapMarker(marker, error)) {
        pImpl->lastError = error;
        return false;
    }

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO gap_markers ("
        "provider_id, account_id, period_start_utc, period_end_utc, classification, basis_summary, source_kind"
        ") VALUES (?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(provider_id, account_id, period_start_utc, period_end_utc) DO UPDATE SET "
        "classification = excluded.classification, "
        "basis_summary = excluded.basis_summary, "
        "source_kind = excluded.source_kind, "
        "updated_at_utc = (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'));";

    if (sqlite3_prepare_v2(pImpl->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        pImpl->lastError = "Failed to prepare gap marker upsert";
        return false;
    }

    const std::string periodStartUtc = toUtcIsoString(marker.periodStart);
    const std::string periodEndUtc = toUtcIsoString(marker.periodEnd);
    const std::string classification = toString(marker.classification);

    sqlite3_bind_int(stmt, 1, marker.providerId);
    sqlite3_bind_int(stmt, 2, marker.accountId);
    sqlite3_bind_text(stmt, 3, periodStartUtc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, periodEndUtc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, classification.c_str(), -1, SQLITE_TRANSIENT);
    if (!marker.basisSummary.empty()) {
        sqlite3_bind_text(stmt, 6, marker.basisSummary.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 6);
    }
    if (!marker.sourceKind.empty()) {
        sqlite3_bind_text(stmt, 7, marker.sourceKind.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 7);
    }

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        pImpl->lastError = "Failed to upsert gap marker";
        return false;
    }

    if (rowId) {
        sqlite3_stmt* idStmt = nullptr;
        const char* idSql =
            "SELECT id FROM gap_markers "
            "WHERE provider_id = ? AND account_id = ? "
            "AND period_start_utc = ? AND period_end_utc = ?;";

        if (sqlite3_prepare_v2(pImpl->db, idSql, -1, &idStmt, nullptr) != SQLITE_OK) {
            pImpl->lastError = "Failed to prepare gap marker id query";
            return false;
        }

        sqlite3_bind_int(idStmt, 1, marker.providerId);
        sqlite3_bind_int(idStmt, 2, marker.accountId);
        sqlite3_bind_text(idStmt, 3, periodStartUtc.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(idStmt, 4, periodEndUtc.c_str(), -1, SQLITE_TRANSIENT);

        const int idRc = sqlite3_step(idStmt);
        if (idRc == SQLITE_ROW) {
            *rowId = sqlite3_column_int64(idStmt, 0);
            sqlite3_finalize(idStmt);
        } else {
            sqlite3_finalize(idStmt);
            pImpl->lastError = "Failed to resolve gap marker id";
            return false;
        }
    }
    pImpl->lastError.clear();
    return true;
#else
    pImpl->lastError = SQLITE_UNAVAILABLE_ERROR;
    return false;
#endif
}

bool StorageService::persistRecoveryCompletion(const std::vector<PersistedGapMarker>& markers,
                                               const PersistedRuntimeState& state) {
#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    if (!isOpen()) {
        pImpl->lastError = "Database not open";
        return false;
    }

    // Keep unrelated writes on this shared connection outside the recovery transaction.
    sqlite3_mutex* const dbMutex = sqlite3_db_mutex(pImpl->db);
    sqlite3_mutex_enter(dbMutex);
    const std::unique_ptr<sqlite3_mutex, decltype(&sqlite3_mutex_leave)> releaseConnection(dbMutex,
                                                                                           sqlite3_mutex_leave);
    if (!beginTransaction(TransactionMode::Immediate)) {
        pImpl->lastError = "Failed to start recovery completion transaction";
        return false;
    }

    const bool saved =
        std::all_of(markers.begin(), markers.end(), [this](const auto& marker) { return upsertGapMarker(marker); }) &&
        upsertRuntimeState(state);
    if (saved && commitTransaction()) {
        return true;
    }
    std::string error = saved ? "Failed to commit recovery completion" : pImpl->lastError;
    if (!rollbackTransaction()) {
        error += ". Failed to roll back recovery completion";
    }
    pImpl->lastError = std::move(error);
    return false;
#else
    pImpl->lastError = SQLITE_UNAVAILABLE_ERROR;
    return false;
#endif
}

std::vector<PersistedGapMarker> StorageService::getGapMarkers(
    int providerId,
    int accountId,
    std::time_t periodStartFrom,
    std::time_t periodEndTo) {
    std::vector<PersistedGapMarker> markers;
    if (!isOpen() || providerId <= 0 || accountId <= 0 || periodStartFrom > periodEndTo) {
        return markers;
    }

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, provider_id, account_id, period_start_utc, period_end_utc, classification, "
        "basis_summary, source_kind, created_at_utc, updated_at_utc "
        "FROM gap_markers "
        "WHERE provider_id = ? AND account_id = ? "
        "AND period_end_utc > ? AND period_start_utc < ? "
        "ORDER BY period_start_utc ASC, period_end_utc ASC;";

    if (sqlite3_prepare_v2(pImpl->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        pImpl->lastError = "Failed to prepare gap marker query";
        return markers;
    }

    const std::string fromUtc = toUtcIsoString(periodStartFrom);
    const std::string toUtc = toUtcIsoString(periodEndTo);
    sqlite3_bind_int(stmt, 1, providerId);
    sqlite3_bind_int(stmt, 2, accountId);
    sqlite3_bind_text(stmt, 3, fromUtc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, toUtc.c_str(), -1, SQLITE_TRANSIENT);

    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        PersistedGapMarker marker;
        marker.id = sqlite3_column_int64(stmt, 0);
        marker.providerId = sqlite3_column_int(stmt, 1);
        marker.accountId = sqlite3_column_int(stmt, 2);
        marker.periodStart = parseUtcOrZero(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
        marker.periodEnd = parseUtcOrZero(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)));
        const char* classificationValue =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        const char* basisSummary = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        const char* sourceKind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        const char* createdAtUtc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        const char* updatedAtUtc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));

        marker.classification =
            gapClassificationFromString(classificationValue ? classificationValue : "")
                .value_or(GapClassification::Unknown);
        marker.basisSummary = basisSummary ? basisSummary : "";
        marker.sourceKind = sourceKind ? sourceKind : "";
        marker.createdAtUtc = createdAtUtc ? createdAtUtc : "";
        marker.updatedAtUtc = updatedAtUtc ? updatedAtUtc : "";
        markers.push_back(std::move(marker));
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        pImpl->lastError = "Failed to query gap markers";
    } else {
        pImpl->lastError.clear();
    }
#endif

    return markers;
}

bool StorageService::deleteGapMarkersOlderThan(std::time_t olderThan) {
    if (!isOpen()) {
        pImpl->lastError = "Database not open";
        return false;
    }

    if (olderThan <= 0) {
        pImpl->lastError = "Gap-marker retention cutoff must be positive";
        return false;
    }

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM gap_markers WHERE period_end_utc < ?;";
    if (sqlite3_prepare_v2(pImpl->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        pImpl->lastError = "Failed to prepare gap-marker retention delete";
        return false;
    }

    const std::string olderThanUtc = toUtcIsoString(olderThan);
    sqlite3_bind_text(stmt, 1, olderThanUtc.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        pImpl->lastError = "Failed to delete old gap markers";
        return false;
    }

    pImpl->lastError.clear();
    return true;
#else
    pImpl->lastError = SQLITE_UNAVAILABLE_ERROR;
    return false;
#endif
}

bool StorageService::upsertProviderCapability(const PersistedProviderCapability& capability, long long* rowId) {
    if (!isOpen()) {
        pImpl->lastError = "Database not open";
        return false;
    }

    std::string error;
    if (!isValidProviderCapability(capability, error)) {
        pImpl->lastError = error;
        return false;
    }

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO provider_capabilities ("
        "provider_id, capability_key, supported, config_json, discovered_at_utc"
        ") VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(provider_id, capability_key) DO UPDATE SET "
        "supported = excluded.supported, "
        "config_json = excluded.config_json, "
        "discovered_at_utc = excluded.discovered_at_utc;";

    if (sqlite3_prepare_v2(pImpl->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        pImpl->lastError = "Failed to prepare provider capability upsert";
        return false;
    }

    const std::string discoveredAtUtc = toUtcIsoString(capability.discoveredAt);
    sqlite3_bind_int(stmt, 1, capability.providerId);
    sqlite3_bind_text(stmt, 2, capability.capabilityKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, capability.supported ? 1 : 0);
    if (!capability.configJson.empty()) {
        sqlite3_bind_text(stmt, 4, capability.configJson.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 4);
    }
    if (!discoveredAtUtc.empty()) {
        sqlite3_bind_text(stmt, 5, discoveredAtUtc.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 5);
    }

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        pImpl->lastError = "Failed to upsert provider capability";
        return false;
    }

    if (rowId) {
        auto capabilities = getProviderCapabilities(capability.providerId);
        *rowId = 0;
        for (const auto& candidate : capabilities) {
            if (candidate.capabilityKey == capability.capabilityKey) {
                *rowId = candidate.id;
                break;
            }
        }
    }
    pImpl->lastError.clear();
    return true;
#else
    pImpl->lastError = SQLITE_UNAVAILABLE_ERROR;
    return false;
#endif
}

std::vector<PersistedProviderCapability> StorageService::getProviderCapabilities(int providerId) {
    std::vector<PersistedProviderCapability> capabilities;
    if (!isOpen() || providerId <= 0) {
        return capabilities;
    }

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, provider_id, capability_key, supported, config_json, discovered_at_utc "
        "FROM provider_capabilities WHERE provider_id = ? ORDER BY capability_key ASC;";

    if (sqlite3_prepare_v2(pImpl->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        pImpl->lastError = "Failed to prepare provider capability query";
        return capabilities;
    }

    sqlite3_bind_int(stmt, 1, providerId);
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        PersistedProviderCapability capability;
        capability.id = sqlite3_column_int64(stmt, 0);
        capability.providerId = sqlite3_column_int(stmt, 1);
        const char* capabilityKey = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* configJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        capability.capabilityKey = capabilityKey ? capabilityKey : "";
        capability.supported = sqlite3_column_int(stmt, 3) != 0;
        capability.configJson = configJson ? configJson : "";
        capability.discoveredAt = parseUtcOrZero(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)));
        capabilities.push_back(std::move(capability));
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        pImpl->lastError = "Failed to query provider capabilities";
    } else {
        pImpl->lastError.clear();
    }
#endif

    return capabilities;
}

bool StorageService::upsertCredentialProfile(
    const PersistedCredentialProfile& profile,
    long long* rowId) {
    if (!isOpen()) {
        pImpl->lastError = "Database not open";
        return false;
    }

    std::string error;
    if (!isValidCredentialProfile(profile, error)) {
        pImpl->lastError = error;
        return false;
    }

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO credential_profiles ("
        "provider_id, account_id, profile_name, secret_ref, credential_type, last_validated_at_utc"
        ") VALUES (?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(account_id, profile_name) DO UPDATE SET "
        "secret_ref = excluded.secret_ref, "
        "credential_type = excluded.credential_type, "
        "last_validated_at_utc = excluded.last_validated_at_utc, "
        "updated_at_utc = (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'));";

    if (sqlite3_prepare_v2(pImpl->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        pImpl->lastError = "Failed to prepare credential profile upsert";
        return false;
    }

    const std::string lastValidatedAtUtc = toUtcIsoString(profile.lastValidatedAt);
    sqlite3_bind_int(stmt, 1, profile.providerId);
    sqlite3_bind_int(stmt, 2, profile.accountId);
    sqlite3_bind_text(stmt, 3, profile.profileName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, profile.secretRef.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, profile.credentialType.c_str(), -1, SQLITE_TRANSIENT);
    if (!lastValidatedAtUtc.empty()) {
        sqlite3_bind_text(stmt, 6, lastValidatedAtUtc.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 6);
    }

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        pImpl->lastError = "Failed to upsert credential profile";
        return false;
    }

    if (rowId) {
        auto stored = getCredentialProfile(profile.accountId, profile.profileName);
        *rowId = stored.has_value() ? stored->id : 0;
    }
    pImpl->lastError.clear();
    return true;
#else
    pImpl->lastError = SQLITE_UNAVAILABLE_ERROR;
    return false;
#endif
}

std::optional<PersistedCredentialProfile> StorageService::getCredentialProfile(
    int accountId,
    const std::string& profileName) {
    if (!isOpen() || accountId <= 0 || profileName.empty()) {
        return std::nullopt;
    }

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, provider_id, account_id, profile_name, secret_ref, credential_type, "
        "created_at_utc, updated_at_utc, last_validated_at_utc "
        "FROM credential_profiles WHERE account_id = ? AND profile_name = ? LIMIT 1;";

    if (sqlite3_prepare_v2(pImpl->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        pImpl->lastError = "Failed to prepare credential profile query";
        return std::nullopt;
    }

    sqlite3_bind_int(stmt, 1, accountId);
    sqlite3_bind_text(stmt, 2, profileName.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        if (rc == SQLITE_DONE) {
            pImpl->lastError.clear();
        } else {
            pImpl->lastError = "Failed to query credential profile";
        }
        return std::nullopt;
    }

    PersistedCredentialProfile profile;
    profile.id = sqlite3_column_int64(stmt, 0);
    profile.providerId = sqlite3_column_int(stmt, 1);
    profile.accountId = sqlite3_column_int(stmt, 2);
    const char* storedProfileName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    const char* secretRef = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    const char* credentialType = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    const char* createdAtUtc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
    const char* updatedAtUtc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));

    profile.profileName = storedProfileName ? storedProfileName : "";
    profile.secretRef = secretRef ? secretRef : "";
    profile.credentialType = credentialType ? credentialType : "";
    profile.createdAtUtc = createdAtUtc ? createdAtUtc : "";
    profile.updatedAtUtc = updatedAtUtc ? updatedAtUtc : "";
    profile.lastValidatedAt = parseUtcOrZero(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8)));

    sqlite3_finalize(stmt);
    pImpl->lastError.clear();
    return profile;
#else
    pImpl->lastError = SQLITE_UNAVAILABLE_ERROR;
    return std::nullopt;
#endif
}

std::vector<PersistedCredentialProfile> StorageService::getCredentialProfiles(int accountId) {
    std::vector<PersistedCredentialProfile> profiles;
    if (!isOpen() || accountId <= 0) {
        return profiles;
    }

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, provider_id, account_id, profile_name, secret_ref, credential_type, "
        "created_at_utc, updated_at_utc, last_validated_at_utc "
        "FROM credential_profiles WHERE account_id = ? ORDER BY profile_name ASC;";

    if (sqlite3_prepare_v2(pImpl->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        pImpl->lastError = "Failed to prepare credential profile list query";
        return profiles;
    }

    sqlite3_bind_int(stmt, 1, accountId);

    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        PersistedCredentialProfile profile;
        profile.id = sqlite3_column_int64(stmt, 0);
        profile.providerId = sqlite3_column_int(stmt, 1);
        profile.accountId = sqlite3_column_int(stmt, 2);
        const char* storedProfileName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const char* secretRef = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        const char* credentialType = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        const char* createdAtUtc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        const char* updatedAtUtc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));

        profile.profileName = storedProfileName ? storedProfileName : "";
        profile.secretRef = secretRef ? secretRef : "";
        profile.credentialType = credentialType ? credentialType : "";
        profile.createdAtUtc = createdAtUtc ? createdAtUtc : "";
        profile.updatedAtUtc = updatedAtUtc ? updatedAtUtc : "";
        profile.lastValidatedAt = parseUtcOrZero(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8)));
        profiles.push_back(std::move(profile));
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        pImpl->lastError = "Failed to query credential profiles";
    } else {
        pImpl->lastError.clear();
    }
#endif

    return profiles;
}

bool StorageService::rebuildUsageRollups(
    UsageRollupResolution resolution,
    int providerId,
    int accountId,
    std::time_t from,
    std::time_t to) {
    if (!isOpen()) {
        pImpl->lastError = "Database not open";
        return false;
    }

    if (providerId <= 0 || accountId <= 0) {
        pImpl->lastError = "Usage rollup rebuild requires providerId and accountId";
        return false;
    }

    if (from <= 0 || to <= 0 || to < from) {
        pImpl->lastError = "Usage rollup rebuild requires a valid time range";
        return false;
    }

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    const std::string tableName = rollupTableName(resolution);
    const std::string periodExpression = rollupPeriodExpression(resolution);
    const std::time_t normalizedFrom = normalizeRollupPeriodStart(resolution, from);
    const std::time_t normalizedTo = normalizeRollupPeriodStart(resolution, to);
    const std::time_t normalizedToEnd = normalizedTo + rollupWindowSeconds(resolution);
    if (normalizedFrom <= 0 || normalizedTo <= 0) {
        pImpl->lastError = "Failed to normalize usage rollup period boundaries";
        return false;
    }

    const std::string normalizedFromUtc = toUtcIsoString(normalizedFrom);
    const std::string normalizedToUtc = toUtcIsoString(normalizedTo);
    const std::string normalizedToEndUtc = toUtcIsoString(normalizedToEnd);

    if (!beginTransaction()) {
        pImpl->lastError = "Failed to begin usage rollup rebuild transaction";
        return false;
    }

    const std::string deleteSql =
        "DELETE FROM " + tableName +
        " WHERE provider_id = ? AND account_id = ? AND rollup_period_start_utc >= ? AND rollup_period_start_utc <= ?;";
    auto deleteResult = execute(
        deleteSql,
        {
            std::to_string(providerId),
            std::to_string(accountId),
            normalizedFromUtc,
            normalizedToUtc
        });
    if (!deleteResult.success()) {
        rollbackTransaction();
        pImpl->lastError = "Failed to clear overlapping usage rollups";
        return false;
    }

    const std::string insertSql =
        std::string("INSERT INTO ") + tableName + " ("
        "provider_id, account_id, rollup_period_start_utc, model_key, request_count, "
        "input_tokens, output_tokens, total_tokens, total_cost_minor_units, "
        "local_request_count, imported_request_count, api_request_count, "
        "unknown_request_count, backfilled_request_count"
        ") "
        "SELECT "
        "provider_id, "
        "account_id, "
        + periodExpression
        + " AS rollup_period_start_utc, "
        "COALESCE(model_key, '') AS model_key, "
        "COUNT(*) AS request_count, "
        "COALESCE(SUM(input_tokens), 0) AS input_tokens, "
        "COALESCE(SUM(output_tokens), 0) AS output_tokens, "
        "COALESCE(SUM(total_tokens), 0) AS total_tokens, "
        "COALESCE(SUM(cost_minor_units), 0) AS total_cost_minor_units, "
        "COALESCE(SUM(CASE "
        "    WHEN source_type IN ('local', 'local_runtime', 'codex_local_rollout', 'codex_local_fixture') "
        "      OR source_type LIKE '%local%' "
        "    THEN 1 ELSE 0 END), 0) AS local_request_count, "
        "COALESCE(SUM(CASE "
        "    WHEN source_type IN ('imported', 'provider_backfill', 'chatgpt_export') "
        "      OR source_type LIKE '%import%' "
        "      OR source_type LIKE '%backfill%' "
        "    THEN 1 ELSE 0 END), 0) AS imported_request_count, "
        "COALESCE(SUM(CASE "
        "    WHEN source_type = 'api' OR source_type LIKE '%api%' "
        "    THEN 1 ELSE 0 END), 0) AS api_request_count, "
        "COALESCE(SUM(CASE "
        "    WHEN source_type IS NULL OR TRIM(source_type) = '' "
        "    THEN 1 ELSE 0 END), 0) AS unknown_request_count, "
        "COALESCE(SUM(CASE WHEN is_backfilled = 1 THEN 1 ELSE 0 END), 0) AS backfilled_request_count "
        "FROM usage_events "
        "WHERE provider_id = ? AND account_id = ? AND event_time_utc >= ? AND event_time_utc < ? "
        "AND source_type != 'codex_local_rollout' "
        "AND source_type != 'codex_local_sqlite' "
        "GROUP BY provider_id, account_id, "
        + periodExpression
        + ", COALESCE(model_key, '') "
        "ORDER BY rollup_period_start_utc ASC, model_key ASC;";

    auto insertResult = execute(
        insertSql,
        {
            std::to_string(providerId),
            std::to_string(accountId),
            normalizedFromUtc,
            normalizedToEndUtc
        });
    if (!insertResult.success()) {
        rollbackTransaction();
        pImpl->lastError = "Failed to rebuild usage rollups";
        return false;
    }

    if (!commitTransaction()) {
        rollbackTransaction();
        pImpl->lastError = "Failed to commit usage rollup rebuild transaction";
        return false;
    }

    pImpl->lastError.clear();
    return true;
#else
    pImpl->lastError = SQLITE_UNAVAILABLE_ERROR;
    return false;
#endif
}

std::vector<PersistedUsageRollup> StorageService::getUsageRollups(
    UsageRollupResolution resolution,
    int providerId,
    int accountId,
    std::time_t periodStartFrom,
    std::time_t periodStartTo) {
    std::vector<PersistedUsageRollup> rollups;
    if (!isOpen() || providerId <= 0 || accountId <= 0 || periodStartTo < periodStartFrom) {
        return rollups;
    }

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    sqlite3_stmt* stmt = nullptr;
    const std::string sql =
        "SELECT id, provider_id, account_id, rollup_period_start_utc, model_key, request_count, "
        "input_tokens, output_tokens, total_tokens, total_cost_minor_units, "
        "local_request_count, imported_request_count, api_request_count, unknown_request_count, "
        "backfilled_request_count, created_at_utc, updated_at_utc "
        "FROM " + rollupTableName(resolution) + " "
        "WHERE provider_id = ? AND account_id = ? AND rollup_period_start_utc >= ? AND rollup_period_start_utc <= ? "
        "ORDER BY rollup_period_start_utc ASC, model_key ASC;";

    if (sqlite3_prepare_v2(pImpl->db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        pImpl->lastError = "Failed to prepare usage rollup query";
        return rollups;
    }

    const std::string fromUtc = toUtcIsoString(periodStartFrom);
    const std::string toUtc = toUtcIsoString(periodStartTo);
    sqlite3_bind_int(stmt, 1, providerId);
    sqlite3_bind_int(stmt, 2, accountId);
    sqlite3_bind_text(stmt, 3, fromUtc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, toUtc.c_str(), -1, SQLITE_TRANSIENT);

    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        PersistedUsageRollup rollup;
        rollup.id = sqlite3_column_int64(stmt, 0);
        rollup.providerId = sqlite3_column_int(stmt, 1);
        rollup.accountId = sqlite3_column_int(stmt, 2);
        rollup.periodStart = parseUtcOrZero(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
        const char* modelKey = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        const char* createdAtUtc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 15));
        const char* updatedAtUtc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 16));
        rollup.modelKey = modelKey ? modelKey : "";
        rollup.requestCount = sqlite3_column_int(stmt, 5);
        rollup.inputTokens = sqlite3_column_int64(stmt, 6);
        rollup.outputTokens = sqlite3_column_int64(stmt, 7);
        rollup.totalTokens = sqlite3_column_int64(stmt, 8);
        rollup.totalCostMinorUnits = sqlite3_column_int64(stmt, 9);
        rollup.localRequestCount = sqlite3_column_int64(stmt, 10);
        rollup.importedRequestCount = sqlite3_column_int64(stmt, 11);
        rollup.apiRequestCount = sqlite3_column_int64(stmt, 12);
        rollup.unknownRequestCount = sqlite3_column_int64(stmt, 13);
        rollup.backfilledRequestCount = sqlite3_column_int64(stmt, 14);
        rollup.createdAtUtc = createdAtUtc ? createdAtUtc : "";
        rollup.updatedAtUtc = updatedAtUtc ? updatedAtUtc : "";
        rollups.push_back(std::move(rollup));
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        pImpl->lastError = "Failed to query usage rollups";
    } else {
        pImpl->lastError.clear();
    }
#endif

    return rollups;
}

std::vector<PersistedQuotaSnapshot> StorageService::getQuotaSnapshots(
    int providerId,
    int accountId,
    std::time_t snapshotTimeFrom,
    std::time_t snapshotTimeTo) {
    std::vector<PersistedQuotaSnapshot> snapshots;
    if (!isOpen() ||
        providerId <= 0 ||
        accountId <= 0 ||
        snapshotTimeFrom <= 0 ||
        snapshotTimeTo <= 0 ||
        snapshotTimeTo < snapshotTimeFrom) {
        return snapshots;
    }

    QueryResult result = execute(
        "SELECT qs.id, qs.provider_id, qs.account_id, qs.plan_id, qs.snapshot_time_utc, "
        "qs.window_start_utc, qs.window_end_utc, qs.used_value, qs.limit_value, qs.remaining_value, "
        "qs.confidence_tier, qs.quota_dimension, qs.is_reconciled, COALESCE(qs.basis_summary, ''), qs.created_at_utc, "
        "(pl.plan_key = 'openai_subscription_codex_local' OR pl.plan_key GLOB 'openai_subscription_codex_local:*'), "
        "qs.snapshot_subsecond_ns "
        "FROM quota_snapshots qs "
        "LEFT JOIN plans pl ON pl.id = qs.plan_id AND pl.account_id = qs.account_id "
        "WHERE qs.provider_id = ? AND qs.account_id = ? AND qs.snapshot_time_utc >= ? AND qs.snapshot_time_utc <= ? "
        "ORDER BY qs.snapshot_time_utc ASC, qs.snapshot_subsecond_ns ASC, qs.id ASC;",
        {std::to_string(providerId), std::to_string(accountId), toUtcIsoString(snapshotTimeFrom),
         toUtcIsoString(snapshotTimeTo)});
    if (!result.success()) {
        pImpl->lastError = "Failed to query quota snapshots";
        return snapshots;
    }

    using CodexWindow = std::tuple<int, std::time_t, std::time_t>;
    using CodexReading = std::tuple<std::string, long long, std::optional<long long>, std::optional<long long>>;
    std::map<CodexWindow, CodexReading> previousCodexReadingByWindow;
    std::time_t codexReadingTime = 0;
    for (size_t row = 0; row < result.rowCount(); ++row) {
        PersistedQuotaSnapshot snapshot;
        snapshot.id = result.getInt64(row, 0);
        snapshot.providerId = result.getInt(row, 1);
        snapshot.accountId = result.getInt(row, 2);
        const std::string planIdValue = result.getString(row, 3);
        if (!planIdValue.empty()) {
            snapshot.planId = std::stoi(planIdValue);
        }
        snapshot.snapshotTime = parseUtcOrZero(result.getString(row, 4).c_str());
        snapshot.windowStart = parseUtcOrZero(result.getString(row, 5).c_str());
        snapshot.windowEnd = parseUtcOrZero(result.getString(row, 6).c_str());
        snapshot.usedValue = result.getInt64(row, 7);
        const std::string limitValue = result.getString(row, 8);
        if (!limitValue.empty()) {
            snapshot.limitValue = result.getInt64(row, 8);
        }
        const std::string remainingValue = result.getString(row, 9);
        if (!remainingValue.empty()) {
            snapshot.remainingValue = result.getInt64(row, 9);
        }
        snapshot.confidenceTier = result.getString(row, 10);
        const bool codexReading = result.getBool(row, 15);
        snapshot.quotaDimension = codexReading ? "percent" : result.getString(row, 11);
        // Early percentage snapshots used a zero-length window when no start was reported.
        if (snapshot.quotaDimension == "percent" && snapshot.windowStart == snapshot.windowEnd) {
            snapshot.windowStart = 0;
            if (snapshot.windowEnd == snapshot.snapshotTime) {
                snapshot.windowEnd = 0;
            }
        }
        if (codexReading) {
            if (snapshot.snapshotTime != codexReadingTime) {
                previousCodexReadingByWindow.clear();
                codexReadingTime = snapshot.snapshotTime;
            }
            const CodexWindow window{snapshot.planId.value_or(0), snapshot.windowStart, snapshot.windowEnd};
            const CodexReading reading{result.getString(row, 16), snapshot.usedValue, snapshot.limitValue,
                                       normalizedQuotaRemainingValue(snapshot)};
            auto [previous, inserted] = previousCodexReadingByWindow.try_emplace(window, reading);
            if (!inserted) {
                if (previous->second == reading) {
                    continue;
                }
                previous->second = reading;
            }
        }
        snapshot.isReconciled = result.getInt(row, 12) != 0;
        snapshot.basisSummary = result.getString(row, 13);
        snapshot.createdAtUtc = result.getString(row, 14);
        snapshots.push_back(std::move(snapshot));
    }

    pImpl->lastError.clear();
    return snapshots;
}

bool StorageService::rebuildQuotaSnapshotRollups(
    int providerId,
    int accountId,
    std::time_t from,
    std::time_t to) {
    if (!isOpen()) {
        pImpl->lastError = "Database not open";
        return false;
    }

    if (providerId <= 0 || accountId <= 0) {
        pImpl->lastError = "Quota snapshot rollup rebuild requires providerId and accountId";
        return false;
    }

    if (from <= 0 || to <= 0 || to < from) {
        pImpl->lastError = "Quota snapshot rollup rebuild requires a valid time range";
        return false;
    }

    const std::time_t normalizedFrom = normalizeRollupPeriodStart(UsageRollupResolution::Daily, from);
    const std::time_t normalizedTo = normalizeRollupPeriodStart(UsageRollupResolution::Daily, to);
    if (normalizedFrom <= 0 || normalizedTo <= 0) {
        pImpl->lastError = "Failed to normalize quota snapshot rollup period boundaries";
        return false;
    }

    const std::time_t normalizedToEnd =
        normalizedTo + rollupWindowSeconds(UsageRollupResolution::Daily) - 1;
    const std::string normalizedFromUtc = toUtcIsoString(normalizedFrom);
    const std::string normalizedToUtc = toUtcIsoString(normalizedTo);

    if (!beginTransaction()) {
        pImpl->lastError = "Failed to begin quota snapshot rollup rebuild transaction";
        return false;
    }

    const QueryResult deleteResult = execute(
        "DELETE FROM quota_snapshot_rollups_daily "
        "WHERE provider_id = ? AND account_id = ? AND rollup_period_start_utc >= ? AND rollup_period_start_utc <= ?;",
        {
            std::to_string(providerId),
            std::to_string(accountId),
            normalizedFromUtc,
            normalizedToUtc
        });
    if (!deleteResult.success()) {
        rollbackTransaction();
        pImpl->lastError = "Failed to clear overlapping quota snapshot rollups";
        return false;
    }

    const auto snapshots = getQuotaSnapshots(providerId, accountId, normalizedFrom, normalizedToEnd);
    std::map<QuotaRollupKey, QuotaRollupAggregate> aggregates;
    for (const auto& snapshot : snapshots) {
        const std::time_t periodStart = normalizeRollupPeriodStart(
            UsageRollupResolution::Daily,
            snapshot.snapshotTime);
        if (periodStart < normalizedFrom || periodStart > normalizedTo) {
            continue;
        }

        QuotaRollupKey key;
        key.periodStart = periodStart;
        key.planBucketId = quotaPlanBucketId(snapshot.planId);
        key.quotaDimension = snapshot.quotaDimension.empty() ? "requests" : snapshot.quotaDimension;

        auto& aggregate = aggregates[key];
        ++aggregate.sampleCount;
        if (snapshot.isReconciled) {
            ++aggregate.reconciledSampleCount;
        }

        if (snapshot.observeBillingWindow(aggregate.previousWindow)) {
            ++aggregate.resetCount;
        }

        const auto remainingValue = normalizedQuotaRemainingValue(snapshot);
        if (remainingValue.has_value() &&
            (!aggregate.lowWaterRemainingValue.has_value() ||
             *remainingValue < *aggregate.lowWaterRemainingValue)) {
            aggregate.lowWaterRemainingValue = remainingValue;
            aggregate.lowWaterLimitValue = snapshot.limitValue;
        }

        if (snapshot.snapshotTime >= aggregate.latestSnapshotTime) {
            aggregate.latestSnapshotTime = snapshot.snapshotTime;
            aggregate.latestWindowStart = snapshot.windowStart;
            aggregate.latestWindowEnd = snapshot.windowEnd;
            aggregate.latestUsedValue = snapshot.usedValue;
            aggregate.latestLimitValue = snapshot.limitValue;
            aggregate.latestRemainingValue = remainingValue;
            aggregate.latestConfidenceTier =
                snapshot.confidenceTier.empty() ? "Unknown" : snapshot.confidenceTier;
        }
    }

    for (const auto& entry : aggregates) {
        const auto& key = entry.first;
        const auto& aggregate = entry.second;
        if (aggregate.sampleCount <= 0 || aggregate.latestSnapshotTime <= 0) {
            continue;
        }

        std::string sql =
            "INSERT INTO quota_snapshot_rollups_daily ("
            "provider_id, account_id, plan_bucket_id, quota_dimension, rollup_period_start_utc, "
            "sample_count, latest_snapshot_time_utc, latest_window_start_utc, latest_window_end_utc, "
            "latest_used_value, latest_limit_value, latest_remaining_value, "
            "low_water_remaining_value, low_water_limit_value, reset_count, "
            "reconciled_sample_count, latest_confidence_tier"
            ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ";
        sql += aggregate.latestLimitValue.has_value() ? "?" : "NULL";
        sql += ", ";
        sql += aggregate.latestRemainingValue.has_value() ? "?" : "NULL";
        sql += ", ";
        sql += aggregate.lowWaterRemainingValue.has_value() ? "?" : "NULL";
        sql += ", ";
        sql += aggregate.lowWaterLimitValue.has_value() ? "?" : "NULL";
        sql += ", ?, ?, ?);";

        std::vector<std::string> params = {
            std::to_string(providerId),
            std::to_string(accountId),
            std::to_string(key.planBucketId),
            key.quotaDimension,
            toUtcIsoString(key.periodStart),
            std::to_string(aggregate.sampleCount),
            toUtcIsoString(aggregate.latestSnapshotTime),
            toUtcIsoString(aggregate.latestWindowStart),
            toUtcIsoString(aggregate.latestWindowEnd),
            std::to_string(aggregate.latestUsedValue)
        };
        if (aggregate.latestLimitValue.has_value()) {
            params.push_back(std::to_string(*aggregate.latestLimitValue));
        }
        if (aggregate.latestRemainingValue.has_value()) {
            params.push_back(std::to_string(*aggregate.latestRemainingValue));
        }
        if (aggregate.lowWaterRemainingValue.has_value()) {
            params.push_back(std::to_string(*aggregate.lowWaterRemainingValue));
        }
        if (aggregate.lowWaterLimitValue.has_value()) {
            params.push_back(std::to_string(*aggregate.lowWaterLimitValue));
        }
        params.push_back(std::to_string(aggregate.resetCount));
        params.push_back(std::to_string(aggregate.reconciledSampleCount));
        params.push_back(aggregate.latestConfidenceTier);

        const QueryResult insertResult = execute(sql, params);
        if (!insertResult.success()) {
            rollbackTransaction();
            pImpl->lastError = "Failed to rebuild quota snapshot rollups";
            return false;
        }
    }

    if (!commitTransaction()) {
        rollbackTransaction();
        pImpl->lastError = "Failed to commit quota snapshot rollup rebuild transaction";
        return false;
    }

    pImpl->lastError.clear();
    return true;
}

std::vector<PersistedQuotaSnapshotRollup> StorageService::getQuotaSnapshotRollups(
    int providerId,
    int accountId,
    std::time_t periodStartFrom,
    std::time_t periodStartTo) {
    std::vector<PersistedQuotaSnapshotRollup> rollups;
    if (!isOpen() ||
        providerId <= 0 ||
        accountId <= 0 ||
        periodStartFrom <= 0 ||
        periodStartTo <= 0 ||
        periodStartTo < periodStartFrom) {
        return rollups;
    }

    const QueryResult result = execute(
        "SELECT r.id, r.provider_id, r.account_id, r.plan_bucket_id, r.quota_dimension, r.rollup_period_start_utc, "
        "r.sample_count, r.latest_snapshot_time_utc, r.latest_window_start_utc, r.latest_window_end_utc, "
        "r.latest_used_value, r.latest_limit_value, r.latest_remaining_value, r.low_water_remaining_value, "
        "r.low_water_limit_value, r.reset_count, r.reconciled_sample_count, r.latest_confidence_tier, "
        "r.created_at_utc, r.updated_at_utc, "
        "(pl.plan_key = 'openai_subscription_codex_local' OR pl.plan_key GLOB 'openai_subscription_codex_local:*') "
        "FROM quota_snapshot_rollups_daily r "
        "LEFT JOIN plans pl ON pl.id = r.plan_bucket_id AND pl.account_id = r.account_id "
        "WHERE r.provider_id = ? AND r.account_id = ? AND r.rollup_period_start_utc >= ? "
        "AND r.rollup_period_start_utc <= ? "
        "ORDER BY r.rollup_period_start_utc ASC, r.plan_bucket_id ASC, r.quota_dimension ASC;",
        {std::to_string(providerId), std::to_string(accountId), toUtcIsoString(periodStartFrom),
         toUtcIsoString(periodStartTo)});
    if (!result.success()) {
        pImpl->lastError = "Failed to query quota snapshot rollups";
        return rollups;
    }

    for (size_t row = 0; row < result.rowCount(); ++row) {
        PersistedQuotaSnapshotRollup rollup;
        rollup.id = result.getInt64(row, 0);
        rollup.providerId = result.getInt(row, 1);
        rollup.accountId = result.getInt(row, 2);
        const int planBucketId = result.getInt(row, 3);
        if (planBucketId > 0) {
            rollup.planId = planBucketId;
        }
        rollup.quotaDimension = result.getBool(row, 20) ? "percent" : result.getString(row, 4);
        rollup.periodStart = parseUtcOrZero(result.getString(row, 5).c_str());
        rollup.sampleCount = result.getInt(row, 6);
        rollup.latestSnapshotTime = parseUtcOrZero(result.getString(row, 7).c_str());
        rollup.latestWindowStart = parseUtcOrZero(result.getString(row, 8).c_str());
        rollup.latestWindowEnd = parseUtcOrZero(result.getString(row, 9).c_str());
        rollup.latestUsedValue = result.getInt64(row, 10);
        const std::string latestLimitValue = result.getString(row, 11);
        if (!latestLimitValue.empty()) {
            rollup.latestLimitValue = result.getInt64(row, 11);
        }
        const std::string latestRemainingValue = result.getString(row, 12);
        if (!latestRemainingValue.empty()) {
            rollup.latestRemainingValue = result.getInt64(row, 12);
        }
        const std::string lowWaterRemainingValue = result.getString(row, 13);
        if (!lowWaterRemainingValue.empty()) {
            rollup.lowWaterRemainingValue = result.getInt64(row, 13);
        }
        const std::string lowWaterLimitValue = result.getString(row, 14);
        if (!lowWaterLimitValue.empty()) {
            rollup.lowWaterLimitValue = result.getInt64(row, 14);
        }
        rollup.resetCount = result.getInt(row, 15);
        rollup.reconciledSampleCount = result.getInt(row, 16);
        rollup.latestConfidenceTier = result.getString(row, 17);
        rollup.createdAtUtc = result.getString(row, 18);
        rollup.updatedAtUtc = result.getString(row, 19);
        rollups.push_back(std::move(rollup));
    }

    pImpl->lastError.clear();
    return rollups;
}

bool StorageService::insertRecoveryCheckpoint(
    const PersistedRecoveryCheckpoint& checkpoint,
    long long* rowId) {
    if (!isOpen()) {
        pImpl->lastError = "Database not open";
        return false;
    }
    if (checkpoint.providerId <= 0 || checkpoint.accountId <= 0 || checkpoint.syncKind.empty()) {
        pImpl->lastError = "Recovery checkpoint requires provider/account ids and sync kind";
        return false;
    }

    QueryResult result = execute(
        "INSERT INTO recovery_checkpoints ("
        "provider_id, account_id, sync_kind, recovery_start_utc, recovery_end_utc, attempt_at_utc, "
        "outcome, fetched_event_count, comparison_result, local_event_count, local_total_tokens, "
        "local_total_cost_minor_units, imported_total_tokens, imported_total_cost_minor_units, failure_category"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
        {
            std::to_string(checkpoint.providerId),
            std::to_string(checkpoint.accountId),
            checkpoint.syncKind,
            toUtcIsoString(checkpoint.recoveryStart),
            toUtcIsoString(checkpoint.recoveryEnd),
            toUtcIsoString(checkpoint.attemptAt),
            checkpoint.outcome,
            std::to_string(checkpoint.fetchedEventCount),
            checkpoint.comparisonResult,
            std::to_string(checkpoint.localEventCount),
            std::to_string(checkpoint.localTotalTokens),
            std::to_string(checkpoint.localTotalCostMinorUnits),
            std::to_string(checkpoint.importedTotalTokens),
            std::to_string(checkpoint.importedTotalCostMinorUnits),
            checkpoint.failureCategory
        });
    if (!result.success()) {
        pImpl->lastError = "Failed to insert recovery checkpoint";
        return false;
    }

    if (rowId != nullptr) {
        *rowId = result.lastInsertRowId();
    }
    pImpl->lastError.clear();
    return true;
}

std::vector<PersistedRecoveryCheckpoint> StorageService::getRecoveryCheckpoints(
    int providerId,
    int accountId,
    const std::string& syncKind,
    std::time_t attemptAtFrom,
    std::time_t attemptAtTo) {
    std::vector<PersistedRecoveryCheckpoint> checkpoints;
    if (!isOpen() ||
        providerId <= 0 ||
        accountId <= 0 ||
        syncKind.empty() ||
        attemptAtFrom <= 0 ||
        attemptAtTo <= 0 ||
        attemptAtTo < attemptAtFrom) {
        return checkpoints;
    }

    QueryResult result = execute(
        "SELECT id, provider_id, account_id, sync_kind, recovery_start_utc, recovery_end_utc, attempt_at_utc, "
        "outcome, fetched_event_count, comparison_result, local_event_count, local_total_tokens, "
        "local_total_cost_minor_units, imported_total_tokens, imported_total_cost_minor_units, "
        "COALESCE(failure_category, ''), created_at_utc "
        "FROM recovery_checkpoints "
        "WHERE provider_id = ? AND account_id = ? AND sync_kind = ? "
        "AND attempt_at_utc >= ? AND attempt_at_utc <= ? "
        "ORDER BY attempt_at_utc ASC, id ASC;",
        {
            std::to_string(providerId),
            std::to_string(accountId),
            syncKind,
            toUtcIsoString(attemptAtFrom),
            toUtcIsoString(attemptAtTo)
        });
    if (!result.success()) {
        pImpl->lastError = "Failed to query recovery checkpoints";
        return checkpoints;
    }

    for (size_t row = 0; row < result.rowCount(); ++row) {
        PersistedRecoveryCheckpoint checkpoint;
        checkpoint.id = result.getInt64(row, 0);
        checkpoint.providerId = result.getInt(row, 1);
        checkpoint.accountId = result.getInt(row, 2);
        checkpoint.syncKind = result.getString(row, 3);
        checkpoint.recoveryStart = parseUtcOrZero(result.getString(row, 4).c_str());
        checkpoint.recoveryEnd = parseUtcOrZero(result.getString(row, 5).c_str());
        checkpoint.attemptAt = parseUtcOrZero(result.getString(row, 6).c_str());
        checkpoint.outcome = result.getString(row, 7);
        checkpoint.fetchedEventCount = result.getInt64(row, 8);
        checkpoint.comparisonResult = result.getString(row, 9);
        checkpoint.localEventCount = result.getInt64(row, 10);
        checkpoint.localTotalTokens = result.getInt64(row, 11);
        checkpoint.localTotalCostMinorUnits = result.getInt64(row, 12);
        checkpoint.importedTotalTokens = result.getInt64(row, 13);
        checkpoint.importedTotalCostMinorUnits = result.getInt64(row, 14);
        checkpoint.failureCategory = result.getString(row, 15);
        checkpoint.createdAtUtc = result.getString(row, 16);
        checkpoints.push_back(std::move(checkpoint));
    }

    pImpl->lastError.clear();
    return checkpoints;
}

std::optional<PersistedRecoveryCheckpoint> StorageService::getLatestRecoveryCheckpoint(
    int providerId,
    int accountId,
    const std::string& syncKind) {
    if (!isOpen() || providerId <= 0 || accountId <= 0 || syncKind.empty()) {
        return std::nullopt;
    }

    QueryResult result = execute(
        "SELECT id, provider_id, account_id, sync_kind, recovery_start_utc, recovery_end_utc, attempt_at_utc, "
        "outcome, fetched_event_count, comparison_result, local_event_count, local_total_tokens, "
        "local_total_cost_minor_units, imported_total_tokens, imported_total_cost_minor_units, "
        "COALESCE(failure_category, ''), created_at_utc "
        "FROM recovery_checkpoints "
        "WHERE provider_id = ? AND account_id = ? AND sync_kind = ? "
        "ORDER BY attempt_at_utc DESC, id DESC LIMIT 1;",
        {
            std::to_string(providerId),
            std::to_string(accountId),
            syncKind
        });
    if (!result.success() || result.rowCount() == 0) {
        return std::nullopt;
    }

    PersistedRecoveryCheckpoint checkpoint;
    checkpoint.id = result.getInt64(0, 0);
    checkpoint.providerId = result.getInt(0, 1);
    checkpoint.accountId = result.getInt(0, 2);
    checkpoint.syncKind = result.getString(0, 3);
    checkpoint.recoveryStart = parseUtcOrZero(result.getString(0, 4).c_str());
    checkpoint.recoveryEnd = parseUtcOrZero(result.getString(0, 5).c_str());
    checkpoint.attemptAt = parseUtcOrZero(result.getString(0, 6).c_str());
    checkpoint.outcome = result.getString(0, 7);
    checkpoint.fetchedEventCount = result.getInt64(0, 8);
    checkpoint.comparisonResult = result.getString(0, 9);
    checkpoint.localEventCount = result.getInt64(0, 10);
    checkpoint.localTotalTokens = result.getInt64(0, 11);
    checkpoint.localTotalCostMinorUnits = result.getInt64(0, 12);
    checkpoint.importedTotalTokens = result.getInt64(0, 13);
    checkpoint.importedTotalCostMinorUnits = result.getInt64(0, 14);
    checkpoint.failureCategory = result.getString(0, 15);
    checkpoint.createdAtUtc = result.getString(0, 16);
    pImpl->lastError.clear();
    return checkpoint;
}

bool StorageService::insertExportRun(const PersistedExportRun& exportRun, long long* rowId) {
    if (!isOpen()) {
        pImpl->lastError = "Database not open";
        return false;
    }
    if (exportRun.runKind.empty() || exportRun.format.empty() || exportRun.requestedAt <= 0) {
        pImpl->lastError = "Export run requires kind, format, and requested time";
        return false;
    }

    QueryResult result = execute(
        "INSERT INTO export_runs ("
        "run_kind, format, requested_at_utc, completed_at_utc, success, output_path, error_summary, "
        "range_start_utc, range_end_utc, provider_filter_csv, account_filter_csv"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
        {
            exportRun.runKind,
            exportRun.format,
            toUtcIsoString(exportRun.requestedAt),
            exportRun.completedAt > 0 ? toUtcIsoString(exportRun.completedAt) : "",
            exportRun.success ? "1" : "0",
            exportRun.outputPath,
            exportRun.errorSummary,
            exportRun.rangeStart > 0 ? toUtcIsoString(exportRun.rangeStart) : "",
            exportRun.rangeEnd > 0 ? toUtcIsoString(exportRun.rangeEnd) : "",
            exportRun.providerFilterCsv,
            exportRun.accountFilterCsv
        });
    if (!result.success()) {
        pImpl->lastError = "Failed to insert export run";
        return false;
    }

    if (rowId != nullptr) {
        *rowId = result.lastInsertRowId();
    }
    pImpl->lastError.clear();
    return true;
}

std::optional<PersistedExportRun> StorageService::getLatestExportRun(const std::string& runKind) {
    if (!isOpen() || runKind.empty()) {
        return std::nullopt;
    }

    QueryResult result = execute(
        "SELECT id, run_kind, format, requested_at_utc, completed_at_utc, success, "
        "COALESCE(output_path, ''), COALESCE(error_summary, ''), COALESCE(range_start_utc, ''), "
        "COALESCE(range_end_utc, ''), COALESCE(provider_filter_csv, ''), COALESCE(account_filter_csv, ''), "
        "created_at_utc "
        "FROM export_runs WHERE run_kind = ? "
        "ORDER BY requested_at_utc DESC, id DESC LIMIT 1;",
        {runKind});
    if (!result.success() || result.rowCount() == 0) {
        return std::nullopt;
    }

    PersistedExportRun exportRun;
    exportRun.id = result.getInt64(0, 0);
    exportRun.runKind = result.getString(0, 1);
    exportRun.format = result.getString(0, 2);
    exportRun.requestedAt = parseUtcOrZero(result.getString(0, 3).c_str());
    exportRun.completedAt = parseUtcOrZero(result.getString(0, 4).c_str());
    exportRun.success = result.getInt(0, 5) != 0;
    exportRun.outputPath = result.getString(0, 6);
    exportRun.errorSummary = result.getString(0, 7);
    exportRun.rangeStart = parseUtcOrZero(result.getString(0, 8).c_str());
    exportRun.rangeEnd = parseUtcOrZero(result.getString(0, 9).c_str());
    exportRun.providerFilterCsv = result.getString(0, 10);
    exportRun.accountFilterCsv = result.getString(0, 11);
    exportRun.createdAtUtc = result.getString(0, 12);
    pImpl->lastError.clear();
    return exportRun;
}

bool StorageService::deleteUsageEventsOlderThan(std::time_t olderThan) {
    if (!isOpen()) {
        pImpl->lastError = "Database not open";
        return false;
    }

    if (olderThan <= 0) {
        pImpl->lastError = "Usage-event retention cutoff must be positive";
        return false;
    }

    auto result = execute(
        "DELETE FROM usage_events WHERE event_time_utc < ?;",
        {toUtcIsoString(olderThan)});
    if (!result.success()) {
        pImpl->lastError = "Failed to delete expired usage events";
        return false;
    }

    pImpl->lastError.clear();
    return true;
}

bool StorageService::deleteUsageRollupsOlderThan(
    UsageRollupResolution resolution,
    std::time_t olderThan) {
    if (!isOpen()) {
        pImpl->lastError = "Database not open";
        return false;
    }

    if (olderThan <= 0) {
        pImpl->lastError = "Usage-rollup retention cutoff must be positive";
        return false;
    }

    const std::time_t normalizedCutoff = normalizeRollupPeriodStart(resolution, olderThan);
    if (normalizedCutoff <= 0) {
        pImpl->lastError = "Failed to normalize usage-rollup retention cutoff";
        return false;
    }

    auto result = execute(
        "DELETE FROM " + rollupTableName(resolution) + " WHERE rollup_period_start_utc < ?;",
        {toUtcIsoString(normalizedCutoff)});
    if (!result.success()) {
        pImpl->lastError = "Failed to delete expired usage rollups";
        return false;
    }

    pImpl->lastError.clear();
    return true;
}

bool StorageService::upsertCodexLocalSource(const PersistedCodexLocalSource& source, long long* rowId) {
    if (!isOpen()) {
        pImpl->lastError = "Database not open";
        return false;
    }

    const std::time_t lastSeen = source.lastSeenAt > 0 ? source.lastSeenAt : std::time(nullptr);
    const std::time_t firstSeen = source.firstSeenAt > 0 ? source.firstSeenAt : lastSeen;
    const std::time_t lastRead = source.lastReadAt > 0 ? source.lastReadAt : lastSeen;

    QueryResult result = execute(
        R"(INSERT INTO codex_local_sources (
                source_path, source_kind, source_identity, first_seen_utc, last_seen_utc, last_read_utc
            ) VALUES (?, ?, ?, ?, ?, ?)
            ON CONFLICT(source_path, source_identity) DO UPDATE SET
                source_kind = excluded.source_kind,
                last_seen_utc = excluded.last_seen_utc,
                last_read_utc = excluded.last_read_utc;)" ,
        {source.sourcePath,
         source.sourceKind.empty() ? "codex_state_sqlite" : source.sourceKind,
         source.sourceIdentity,
         toUtcIsoString(firstSeen),
         toUtcIsoString(lastSeen),
         toUtcIsoString(lastRead)});
    if (!result.success()) {
        pImpl->lastError = "Failed to upsert Codex local source: " + result.error();
        return false;
    }

    QueryResult idResult = execute(
        "SELECT id FROM codex_local_sources WHERE source_path = ? AND source_identity = ? LIMIT 1;",
        {source.sourcePath, source.sourceIdentity});
    if (!idResult.success() || idResult.rowCount() == 0) {
        pImpl->lastError = "Failed to read Codex local source id";
        return false;
    }

    if (rowId) {
        *rowId = idResult.getInt64(0, 0);
    }
    pImpl->lastError.clear();
    return true;
}

std::optional<PersistedCodexLocalSource> StorageService::getCodexLocalSource(
    const std::string& sourcePath,
    const std::string& sourceIdentity) {
    if (!isOpen()) {
        pImpl->lastError = "Database not open";
        return std::nullopt;
    }

    QueryResult result = execute(
        R"(SELECT id, source_path, source_kind, source_identity, first_seen_utc, last_seen_utc, last_read_utc
           FROM codex_local_sources
           WHERE source_path = ? AND source_identity = ? LIMIT 1;)" ,
        {sourcePath, sourceIdentity});
    if (!result.success()) {
        pImpl->lastError = "Failed to query Codex local source";
        return std::nullopt;
    }
    if (result.rowCount() == 0) {
        pImpl->lastError.clear();
        return std::nullopt;
    }

    PersistedCodexLocalSource source;
    source.id = result.getInt64(0, 0);
    source.sourcePath = result.getString(0, 1);
    source.sourceKind = result.getString(0, 2);
    source.sourceIdentity = result.getString(0, 3);
    fromUtcIsoString(result.getString(0, 4), source.firstSeenAt);
    fromUtcIsoString(result.getString(0, 5), source.lastSeenAt);
    fromUtcIsoString(result.getString(0, 6), source.lastReadAt);
    pImpl->lastError.clear();
    return source;
}

std::optional<PersistedCodexThreadImportState> StorageService::getCodexThreadImportState(
    long long sourceId,
    const std::string& threadId) {
    if (!isOpen()) {
        pImpl->lastError = "Database not open";
        return std::nullopt;
    }

    QueryResult result = execute(
        R"(SELECT id, source_id, thread_id, model_provider, model_key, reasoning_effort,
                  first_seen_created_at_ms, last_seen_updated_at_ms, baseline_tokens_used,
                  last_seen_tokens_used, last_imported_tokens_used, baseline_established_utc,
                  last_event_time_utc, counter_reset_count, last_status, updated_utc
           FROM codex_thread_import_state
           WHERE source_id = ? AND thread_id = ? LIMIT 1;)" ,
        {std::to_string(sourceId), threadId});
    if (!result.success()) {
        pImpl->lastError = "Failed to query Codex thread import state";
        return std::nullopt;
    }
    if (result.rowCount() == 0) {
        pImpl->lastError.clear();
        return std::nullopt;
    }

    PersistedCodexThreadImportState state;
    state.id = result.getInt64(0, 0);
    state.sourceId = result.getInt64(0, 1);
    state.threadId = result.getString(0, 2);
    state.modelProvider = result.getString(0, 3);
    state.modelKey = result.getString(0, 4);
    state.reasoningEffort = result.getString(0, 5);
    state.firstSeenCreatedAtMs = result.getInt64(0, 6);
    state.lastSeenUpdatedAtMs = result.getInt64(0, 7);
    state.baselineTokensUsed = result.getInt64(0, 8);
    state.lastSeenTokensUsed = result.getInt64(0, 9);
    state.lastImportedTokensUsed = result.getInt64(0, 10);
    fromUtcIsoString(result.getString(0, 11), state.baselineEstablishedAt);
    fromUtcIsoString(result.getString(0, 12), state.lastEventTime);
    state.counterResetCount = result.getInt(0, 13);
    state.lastStatus = result.getString(0, 14);
    fromUtcIsoString(result.getString(0, 15), state.updatedAt);
    pImpl->lastError.clear();
    return state;
}

bool StorageService::upsertCodexThreadImportState(const PersistedCodexThreadImportState& state, long long* rowId) {
    if (!isOpen()) {
        pImpl->lastError = "Database not open";
        return false;
    }

    const std::time_t now = std::time(nullptr);
    const std::time_t baselineAt = state.baselineEstablishedAt > 0 ? state.baselineEstablishedAt : now;
    const std::time_t updatedAt = state.updatedAt > 0 ? state.updatedAt : now;

    QueryResult result = execute(
        R"(INSERT INTO codex_thread_import_state (
                source_id, thread_id, model_provider, model_key, reasoning_effort,
                first_seen_created_at_ms, last_seen_updated_at_ms, baseline_tokens_used,
                last_seen_tokens_used, last_imported_tokens_used, baseline_established_utc,
                last_event_time_utc, counter_reset_count, last_status, updated_utc
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(source_id, thread_id) DO UPDATE SET
                model_provider = excluded.model_provider,
                model_key = excluded.model_key,
                reasoning_effort = excluded.reasoning_effort,
                first_seen_created_at_ms = excluded.first_seen_created_at_ms,
                last_seen_updated_at_ms = excluded.last_seen_updated_at_ms,
                baseline_tokens_used = excluded.baseline_tokens_used,
                last_seen_tokens_used = excluded.last_seen_tokens_used,
                last_imported_tokens_used = excluded.last_imported_tokens_used,
                baseline_established_utc = excluded.baseline_established_utc,
                last_event_time_utc = excluded.last_event_time_utc,
                counter_reset_count = excluded.counter_reset_count,
                last_status = excluded.last_status,
                updated_utc = excluded.updated_utc;)" ,
        {std::to_string(state.sourceId),
         state.threadId,
         state.modelProvider,
         state.modelKey,
         state.reasoningEffort,
         std::to_string(state.firstSeenCreatedAtMs),
         std::to_string(state.lastSeenUpdatedAtMs),
         std::to_string(state.baselineTokensUsed),
         std::to_string(state.lastSeenTokensUsed),
         std::to_string(state.lastImportedTokensUsed),
         toUtcIsoString(baselineAt),
         state.lastEventTime > 0 ? toUtcIsoString(state.lastEventTime) : "",
         std::to_string(state.counterResetCount),
         state.lastStatus.empty() ? "active" : state.lastStatus,
         toUtcIsoString(updatedAt)});
    if (!result.success()) {
        pImpl->lastError = "Failed to upsert Codex thread import state: " + result.error();
        return false;
    }

    QueryResult idResult = execute(
        "SELECT id FROM codex_thread_import_state WHERE source_id = ? AND thread_id = ? LIMIT 1;",
        {std::to_string(state.sourceId), state.threadId});
    if (!idResult.success() || idResult.rowCount() == 0) {
        pImpl->lastError = "Failed to read Codex thread import state id";
        return false;
    }

    if (rowId) {
        *rowId = idResult.getInt64(0, 0);
    }
    pImpl->lastError.clear();
    return true;
}

bool StorageService::insertUsageEvents(const std::vector<PersistedUsageEvent>& events,
                                       const std::vector<PersistedUsageEvent>& savedCodexCounters) {
    if (!isOpen()) {
        pImpl->lastError = "Database not open";
        return false;
    }

    if (events.empty() && savedCodexCounters.empty()) {
        return true;
    }

    const bool ownsTransaction = sqlite3_get_autocommit(pImpl->db) != 0;
    if (ownsTransaction && !beginTransaction()) {
        pImpl->lastError = "Failed to begin transaction for usage events";
        return false;
    }

    auto findUsageEvent = [this](const PersistedUsageEvent& event, bool matchesLegacyCounter) {
        std::string sql = "SELECT id, event_subsecond_ns, source_run_id FROM usage_events "
                          "WHERE provider_id = ? AND account_id = ? AND ";
        std::vector<std::string> params = {
            std::to_string(event.providerId),
            std::to_string(event.accountId)
        };

        if (event.planId.has_value()) {
            sql += "plan_id = ? AND ";
            params.push_back(std::to_string(*event.planId));
        } else {
            sql += "plan_id IS NULL AND ";
        }

        sql += "event_time_utc = ? AND source_type = ? ";
        sql += matchesLegacyCounter ? "AND (source_run_id = ? OR source_run_id = 'codex_token_usage') "
                                    : "AND source_run_id = ? ";
        sql += "AND model_key = ? AND request_type = ? "
               "AND input_tokens = ? AND output_tokens = ? AND total_tokens = ? "
               "AND cost_minor_units = ? AND is_backfilled = ? AND raw_reference = ? ";

        params.push_back(toUtcIsoString(event.eventTime));
        params.push_back(event.sourceType);
        params.push_back(event.sourceRunId);
        params.push_back(event.modelKey);
        params.push_back(event.requestType);
        params.push_back(std::to_string(event.inputTokens));
        params.push_back(std::to_string(event.outputTokens));
        params.push_back(std::to_string(event.totalTokens));
        params.push_back(std::to_string(event.costMinorUnits));
        params.push_back(event.isBackfilled ? "1" : "0");
        params.push_back(event.rawReference);

        if (event.eventSubsecondNs.has_value()) {
            sql += "AND (event_subsecond_ns = ? OR event_subsecond_ns IS NULL) ";
            params.push_back(std::to_string(*event.eventSubsecondNs));
        } else if (matchesLegacyCounter) {
            sql += "AND event_subsecond_ns IS NULL ";
        }
        if (matchesLegacyCounter) {
            sql += "ORDER BY (source_run_id = 'codex_token_usage'), event_subsecond_ns DESC ";
        } else if (event.eventSubsecondNs.has_value()) {
            sql += "ORDER BY event_subsecond_ns DESC ";
        }
        sql += "LIMIT 1";
        return execute(sql, params);
    };

    const char* sql = R"(
        INSERT INTO usage_events (
            provider_id, account_id, plan_id, event_time_utc, source_type,
            source_run_id, model_key, request_type, input_tokens, output_tokens,
            total_tokens, cost_minor_units, is_backfilled, raw_reference, created_at_utc, event_subsecond_ns
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(pImpl->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        pImpl->lastError = "Failed to prepare usage events insert statement";
        if (ownsTransaction) {
            rollbackTransaction();
        }
        return false;
    }

    bool success = true;
    for (std::size_t i = 0; i < events.size() + savedCodexCounters.size(); ++i) {
        const bool updateOnly = i >= events.size();
        const auto& event = updateOnly ? savedCodexCounters[i - events.size()] : events[i];
        const bool matchesLegacyCounter = event.sourceType == "codex_local_rollout" &&
                                          event.requestType == "codex_turn" && event.sourceRunId.rfind("line:", 0) == 0;
        const auto existing = findUsageEvent(event, matchesLegacyCounter);
        if (!existing.success()) {
            pImpl->lastError = "Failed to check for duplicate usage event";
            success = false;
            break;
        }
        if (existing.rowCount() > 0) {
            const bool needsSourceId = matchesLegacyCounter && existing.getString(0, 2) != event.sourceRunId;
            const bool needsPrecision = event.eventSubsecondNs.has_value() && existing.getString(0, 1).empty();
            if (needsSourceId || needsPrecision) {
                const std::string subsecond =
                    event.eventSubsecondNs.has_value() ? std::to_string(*event.eventSubsecondNs) : std::string{};
                const auto updated =
                    execute("UPDATE usage_events SET source_run_id = ?, "
                            "event_subsecond_ns = COALESCE(NULLIF(?, ''), event_subsecond_ns) WHERE id = ?;",
                            {event.sourceRunId, subsecond, existing.getString(0, 0)});
                if (!updated.success()) {
                    pImpl->lastError = "Failed to update usage event source";
                    success = false;
                    break;
                }
            }
            continue;
        }
        if (updateOnly) {
            continue;
        }

        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);

        sqlite3_bind_int(stmt, 1, event.providerId);
        sqlite3_bind_int(stmt, 2, event.accountId);
        if (event.planId.has_value()) {
            sqlite3_bind_int(stmt, 3, *event.planId);
        } else {
            sqlite3_bind_null(stmt, 3);
        }
        sqlite3_bind_text(stmt, 4, toUtcIsoString(event.eventTime).c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, event.sourceType.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, event.sourceRunId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, event.modelKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 8, event.requestType.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 9, event.inputTokens);
        sqlite3_bind_int64(stmt, 10, event.outputTokens);
        sqlite3_bind_int64(stmt, 11, event.totalTokens);
        sqlite3_bind_int64(stmt, 12, event.costMinorUnits);
        sqlite3_bind_int(stmt, 13, event.isBackfilled ? 1 : 0);
        sqlite3_bind_text(stmt, 14, event.rawReference.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 15, event.createdAtUtc.c_str(), -1, SQLITE_TRANSIENT);
        if (event.eventSubsecondNs.has_value()) {
            sqlite3_bind_int(stmt, 16, *event.eventSubsecondNs);
        } else {
            sqlite3_bind_null(stmt, 16);
        }

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            pImpl->lastError = "Failed to insert usage event: " + std::string(sqlite3_errmsg(pImpl->db));
            success = false;
            break;
        }
    }

    sqlite3_finalize(stmt);

    if (ownsTransaction) {
        if (!success) {
            const std::string error = pImpl->lastError;
            rollbackTransaction();
            pImpl->lastError = error;
        } else if (!commitTransaction()) {
            rollbackTransaction();
            pImpl->lastError = "Failed to commit usage events";
            return false;
        }
    }

    return success;
}

std::vector<PersistedUsageEvent> StorageService::getUsageEvents(
    int providerId,
    int accountId,
    std::time_t from,
    std::time_t to) {
    std::vector<PersistedUsageEvent> events;

    if (!isOpen()) {
        pImpl->lastError = "Database not open";
        return events;
    }

    auto result = execute(
        R"(SELECT id, provider_id, account_id, plan_id, event_time_utc, source_type,
                  source_run_id, model_key, request_type, input_tokens, output_tokens,
                  total_tokens, cost_minor_units, is_backfilled, raw_reference, created_at_utc, event_subsecond_ns
            FROM usage_events
            WHERE provider_id = ? AND account_id = ?
              AND event_time_utc >= ? AND event_time_utc < ?
            ORDER BY event_time_utc ASC)",
        {std::to_string(providerId), std::to_string(accountId), toUtcIsoString(from), toUtcIsoString(to)});

    if (!result.success()) {
        pImpl->lastError = "Failed to query usage events";
        return events;
    }

    for (size_t i = 0; i < result.rowCount(); ++i) {
        const auto& row = result.getRowAsMap(i);
        PersistedUsageEvent event;
        event.id = std::stoll(row.at("id"));
        event.providerId = std::stoi(row.at("provider_id"));
        event.accountId = std::stoi(row.at("account_id"));
        if (!row.at("plan_id").empty() && row.at("plan_id") != "NULL") {
            event.planId = std::stoi(row.at("plan_id"));
        }
        std::time_t parsedEventTime = 0;
        if (fromUtcIsoString(row.at("event_time_utc"), parsedEventTime)) {
            event.eventTime = parsedEventTime;
        }
        event.sourceType = row.at("source_type");
        event.sourceRunId = row.at("source_run_id");
        event.modelKey = row.at("model_key");
        event.requestType = row.at("request_type");
        event.inputTokens = std::stoll(row.at("input_tokens"));
        event.outputTokens = std::stoll(row.at("output_tokens"));
        event.totalTokens = std::stoll(row.at("total_tokens"));
        event.costMinorUnits = std::stoll(row.at("cost_minor_units"));
        event.isBackfilled = row.at("is_backfilled") == "1";
        event.rawReference = row.at("raw_reference");
        event.createdAtUtc = row.at("created_at_utc");
        if (!row.at("event_subsecond_ns").empty()) {
            event.eventSubsecondNs = std::stoi(row.at("event_subsecond_ns"));
        }
        events.push_back(event);
    }

    pImpl->lastError.clear();
    return events;
}

bool StorageService::hasUsageEvents(int providerId, int accountId, std::time_t from, std::time_t to) {
    if (!isOpen()) {
        return false;
    }

    auto result = execute(
        R"(SELECT 1 FROM usage_events
            WHERE provider_id = ? AND account_id = ?
              AND event_time_utc >= ? AND event_time_utc < ?
            LIMIT 1)",
        {std::to_string(providerId),
         std::to_string(accountId),
         toUtcIsoString(from),
         toUtcIsoString(to)});

    return result.success() && result.rowCount() > 0;
}

std::vector<PersistedModelUsagePoint> StorageService::getModelUsagePoints(
    const std::vector<std::string>& providerKeys,
    const std::string& sourceType,
    std::time_t from,
    std::time_t to,
    int bucketSeconds) {
    std::vector<PersistedModelUsagePoint> points;
    if (!isOpen()) {
        pImpl->lastError = "Database not open";
        return points;
    }
    if (providerKeys.empty() || bucketSeconds <= 0) {
        return points;
    }

    std::string sql =
        "SELECT p.provider_key, COALESCE(ue.model_key, ''), "
        "((CAST(strftime('%s', ue.event_time_utc) AS INTEGER) / ?) * ?) AS bucket_start, "
        "COALESCE(SUM(ue.total_tokens), 0) "
        "FROM usage_events ue "
        "JOIN providers p ON p.id = ue.provider_id "
        "WHERE ue.event_time_utc >= ? AND ue.event_time_utc < ? "
        "AND ue.source_type = ? AND p.provider_key IN (";
    std::vector<std::string> params = {
        std::to_string(bucketSeconds),
        std::to_string(bucketSeconds),
        toUtcIsoString(from),
        toUtcIsoString(to),
        sourceType
    };
    for (size_t i = 0; i < providerKeys.size(); ++i) {
        if (i > 0) sql += ",";
        sql += "?";
        params.push_back(providerKeys[i]);
    }
    sql += ") GROUP BY p.provider_key, ue.model_key, bucket_start "
           "ORDER BY bucket_start ASC, p.provider_key ASC, ue.model_key ASC;";

    QueryResult result = execute(sql, params);
    if (!result.success()) {
        pImpl->lastError = "Failed to query model usage points: " + result.error();
        return points;
    }

    for (size_t i = 0; i < result.rowCount(); ++i) {
        PersistedModelUsagePoint point;
        point.providerKey = result.getString(i, 0);
        point.modelKey = result.getString(i, 1);
        point.bucketStart = static_cast<std::time_t>(result.getInt64(i, 2));
        point.totalTokens = result.getInt64(i, 3);
        points.push_back(std::move(point));
    }

    pImpl->lastError.clear();
    return points;
}

} // namespace JiStats
