#include "db/SchemaManager.hpp"
#include "db/StorageService.hpp"
#include "BaselineSchema.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <thread>

namespace JiStats {

namespace {
constexpr int kBaselineVersion = 12;
constexpr int kClaudeRedactionVersion = 13;
constexpr int kChatGptDedupVersion = 14;
constexpr int kQuotaPrecisionVersion = 15;
constexpr int kClaudeActivityDedupVersion = 16;
}

class SchemaManager::Impl {
public:
    std::string lastError;
    std::optional<int> rejectedVersion;
    std::string migrationsPath = "db_migrations";
};

SchemaManager::SchemaManager(StorageService& storage) 
    : storage(storage), pImpl(std::make_unique<Impl>()) {
}

SchemaManager::~SchemaManager() = default;

bool SchemaManager::needsMigration() const {
    return getCurrentVersion() < getLatestVersion();
}

int SchemaManager::getCurrentVersion() const {
    // Check if schema_version table exists
    auto result = storage.execute(
        "SELECT MAX(version) FROM schema_version;"
    );
    
    if (result.success() && result.rowCount() > 0) {
        return result.getInt(0, 0);
    }
    
    return 0;
}

int SchemaManager::getLatestVersion() const {
    int latest = kClaudeActivityDedupVersion;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(pImpl->migrationsPath)) {
            const std::string name = entry.path().filename().string();
            if (name.size() >= 4 && name.substr(name.size() - 4) == ".sql") {
                try {
                    int version = std::stoi(name.substr(0, 3));
                    if (version > latest) {
                        latest = version;
                    }
                } catch (...) {}
            }
        }
    } catch (...) {}
    return latest;
}

bool SchemaManager::migrate() {
    pImpl->lastError.clear();
    pImpl->rejectedVersion.reset();

    int current = 0;
    auto baseline = runBaselineTransaction(current, false);
    if (baseline == BaselineResult::NeedsWrite) {
        // End the read snapshot before acquiring a write lock, then recheck under that lock.
        // Another instance may have completed the same repair while we waited.
        baseline = runBaselineTransaction(current, true);
    }
    if (baseline == BaselineResult::NeedsCompaction) {
        // A page split can leave plaintext outside the live cells. Rebuild the file
        // after the redaction transaction commits. VACUUM requires autocommit.
        const auto compacted = storage.execute("VACUUM main;");
        if (!compacted.success()) {
            pImpl->lastError =
                "Cannot compact the database after redacting legacy Claude references: " + compacted.error();
            return false;
        }
        if (!checkpointClaudeRedaction()) {
            return false;
        }
        // Leave version 12 recorded until the file is compacted so a failed or
        // interrupted open retries the redaction and rebuild on its next attempt.
        baseline = recordClaudeRedaction(current);
    }
    if (baseline != BaselineResult::Ready) {
        return false;
    }

    const int latest = getLatestVersion();
    if (current >= latest) {
        return true;
    }

    for (int version = current + 1; version <= latest; version++) {
        if (!migrateToVersion(version)) {
            return false;
        }
    }
    
    return true;
}

SchemaManager::BaselineResult SchemaManager::runBaselineTransaction(int& current, bool writable) {
    const auto mode = writable ? StorageService::TransactionMode::Immediate : StorageService::TransactionMode::Deferred;
    if (!storage.beginTransaction(mode)) {
        pImpl->lastError =
            writable ? "Failed to begin baseline write transaction" : "Failed to begin schema inspection transaction";
        return BaselineResult::Failed;
    }

    const auto result = prepareBaseline(current, writable);
    if (result == BaselineResult::Failed) {
        storage.rollbackTransaction();
        return result;
    }
    if (!storage.commitTransaction()) {
        storage.rollbackTransaction();
        pImpl->lastError = "Failed to commit baseline transaction";
        return BaselineResult::Failed;
    }

    return result;
}

SchemaManager::BaselineResult SchemaManager::prepareBaseline(int& current, bool writable) {
    const auto catalog =
        storage.execute("SELECT type FROM main.sqlite_master WHERE name = 'schema_version' COLLATE NOCASE "
                        "AND type IN ('table', 'view');");
    if (!catalog.success()) {
        pImpl->lastError = "Cannot inspect schema version table: " + catalog.error();
        return BaselineResult::Failed;
    }

    if (catalog.rowCount() != 0) {
        if (catalog.getString(0, 0) != "table") {
            pImpl->lastError = "Cannot read schema version: schema_version is not a table";
            return BaselineResult::Failed;
        }
        const auto version = storage.execute("SELECT version FROM main.schema_version ORDER BY version DESC LIMIT 1;");
        if (!version.success()) {
            pImpl->lastError = "Cannot read schema version: " + version.error();
            return BaselineResult::Failed;
        }

        if (version.rowCount() != 0) {
            const auto value = version.getString(0, 0);
            const auto end = value.data() + value.size();
            const auto parsed = std::from_chars(value.data(), end, current);
            if (parsed.ec != std::errc() || parsed.ptr != end || current < 1) {
                pImpl->lastError = "Invalid database schema version";
                return BaselineResult::Failed;
            }
            if (current < kBaselineVersion) {
                pImpl->rejectedVersion = current;
                pImpl->lastError = "Database schema version " + std::to_string(current) +
                                   " predates the supported baseline (version " + std::to_string(kBaselineVersion) +
                                   ")";
                return BaselineResult::Failed;
            }
            if (current > kBaselineVersion) {
                return BaselineResult::Ready;
            }
            const auto baseline = reconcileBaseline(writable);
            return baseline == BaselineResult::Ready ? migrateClaudeReferences(writable) : baseline;
        }
    }

    if (!writable) {
        return BaselineResult::NeedsWrite;
    }

    const auto pages = storage.execute("PRAGMA main.page_count;");
    if (!pages.success() || pages.rowCount() != 1) {
        pImpl->lastError = "Cannot inspect database page count before initializing the baseline";
        return BaselineResult::Failed;
    }
    const auto result = storage.execute(kBaselineSchemaSql);
    if (!result.success()) {
        pImpl->lastError = "Cannot initialize baseline: " + result.error();
        return BaselineResult::Failed;
    }
    // An empty database has no historical cells to redact or compact.
    return pages.getInt(0, 0) == 0 ? recordClaudeRedaction(current) : migrateClaudeReferences(writable);
}

SchemaManager::BaselineResult SchemaManager::migrateClaudeReferences(bool writable) {
    if (!writable) {
        return BaselineResult::NeedsWrite;
    }

    const auto result = storage.execute(kClaudeRedactionSql);
    if (!result.success()) {
        pImpl->lastError = "Cannot redact legacy Claude source references: " + result.error();
        return BaselineResult::Failed;
    }
    return BaselineResult::NeedsCompaction;
}

bool SchemaManager::checkpointClaudeRedaction() {
    // Flush the rebuilt WAL pages into the main file. SQLite bypasses the
    // connection's busy handler if another opener owns the checkpoint lock.
    const auto normalTimeout = storage.execute("PRAGMA busy_timeout;");
    if (!normalTimeout.success() || normalTimeout.rowCount() != 1) {
        pImpl->lastError = "Cannot read the SQLite busy timeout for the Claude reference upgrade";
        return false;
    }
    const auto normalWait = std::chrono::milliseconds(normalTimeout.getInt(0, 0));
    const auto setBusyTimeout = [&](std::chrono::milliseconds wait) {
        const auto result = storage.execute("PRAGMA busy_timeout = " + std::to_string(wait.count()) + ";");
        return result.success() && result.rowCount() == 1 && result.getInt(0, 0) == wait.count();
    };

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    bool checkpointed = false;
    while (true) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining.count() <= 0 || !setBusyTimeout(std::min(remaining, normalWait))) {
            break;
        }
        const auto checkpoint = storage.execute("PRAGMA main.wal_checkpoint(TRUNCATE);");
        if (!checkpoint.success() || checkpoint.rowCount() != 1) {
            break;
        }
        const int busy = checkpoint.getInt(0, 0);
        if (busy != 1) {
            checkpointed = busy == 0;
            break;
        }
        std::this_thread::sleep_until(std::min(deadline, Clock::now() + std::chrono::milliseconds(25)));
    }

    if (!setBusyTimeout(normalWait)) {
        pImpl->lastError = "Cannot restore the SQLite busy timeout after the Claude reference upgrade";
        return false;
    }
    if (!checkpointed) {
        pImpl->lastError = "Cannot checkpoint the compacted database after redacting legacy Claude references";
    }
    return checkpointed;
}

SchemaManager::BaselineResult SchemaManager::recordClaudeRedaction(int& current) {
    const auto result = storage.execute("INSERT INTO main.schema_version (version, description) "
                                        "VALUES (13, 'Redact Claude source references') "
                                        "ON CONFLICT(version) DO NOTHING;");
    if (!result.success()) {
        pImpl->lastError = "Cannot record the Claude reference upgrade: " + result.error();
        return BaselineResult::Failed;
    }

    current = kClaudeRedactionVersion;
    return BaselineResult::Ready;
}

SchemaManager::BaselineResult SchemaManager::reconcileBaseline(bool writable) {
    // The historical loader selected only one of the two migrations numbered 010.
    std::string repair;
    for (const std::string table : {"usage_rollups_hourly", "usage_rollups_daily"}) {
        const auto columns = storage.execute("PRAGMA main.table_info(" + table + ");");
        if (!columns.success() || columns.rowCount() == 0) {
            pImpl->lastError = "Cannot inspect baseline table " + table;
            return BaselineResult::Failed;
        }
        for (const char* column : {"local_request_count", "imported_request_count", "api_request_count",
                                   "unknown_request_count", "backfilled_request_count"}) {
            bool present = false;
            for (size_t row = 0; row < columns.rowCount(); ++row) {
                if (columns.getString(row, 1) == column) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                repair += "ALTER TABLE " + table + " ADD COLUMN " + column + " INTEGER NOT NULL DEFAULT 0;\n";
            }
        }
    }

    const auto subsecond =
        storage.execute("SELECT 1 FROM pragma_table_info('usage_events', 'main') WHERE name = 'event_subsecond_ns';");
    if (!subsecond.success()) {
        pImpl->lastError = "Cannot inspect usage event precision: " + subsecond.error();
        return BaselineResult::Failed;
    }
    if (subsecond.rowCount() == 0) {
        repair += "ALTER TABLE main.usage_events ADD COLUMN event_subsecond_ns INTEGER;\n";
    }

    const auto objects =
        storage.execute("SELECT COUNT(*) FROM main.sqlite_master WHERE "
                        "(type = 'table' AND name IN ('recovery_checkpoints', 'export_runs')) OR "
                        "(type = 'index' AND name IN ('idx_recovery_checkpoints_provider_account_kind_attempt', "
                        "'idx_export_runs_kind_requested'));");
    if (!objects.success()) {
        pImpl->lastError = "Cannot inspect recovery and export schema: " + objects.error();
        return BaselineResult::Failed;
    }
    if (objects.getInt(0, 0) != 4) {
        repair += kRecoveryAndExportSql;
    }

    const auto kimi = storage.execute("SELECT 1 FROM main.providers WHERE provider_key = 'kimi';");
    if (!kimi.success()) {
        pImpl->lastError = "Cannot inspect Kimi provider seed: " + kimi.error();
        return BaselineResult::Failed;
    }
    if (kimi.rowCount() == 0) {
        repair += "INSERT INTO providers (provider_key, display_name, enabled) VALUES ('kimi', 'Kimi Code', 1);";
    }

    const auto subscriptions =
        storage.execute("SELECT COUNT(*) FROM main.providers WHERE provider_key IN ('grok', 'cursor');");
    if (!subscriptions.success()) {
        pImpl->lastError = "Cannot inspect subscription provider seeds: " + subscriptions.error();
        return BaselineResult::Failed;
    }
    if (subscriptions.getInt(0, 0) != 2) {
        repair += "INSERT OR IGNORE INTO providers (provider_key, display_name, enabled) "
                  "VALUES ('grok', 'Grok', 1), ('cursor', 'Cursor', 1);";
    }

    if (!repair.empty()) {
        if (!writable) {
            return BaselineResult::NeedsWrite;
        }
        const auto result = storage.execute(repair);
        if (!result.success()) {
            pImpl->lastError = "Cannot reconcile historical version 12 schema: " + result.error();
            return BaselineResult::Failed;
        }
    }
    return BaselineResult::Ready;
}

bool SchemaManager::migrateToVersion(int version) {
    const std::string migration = loadMigration(version);
    if (migration.empty()) {
        pImpl->lastError = "Migration file not found for version " + std::to_string(version);
        return false;
    }

    if (!storage.beginTransaction(StorageService::TransactionMode::Immediate)) {
        pImpl->lastError = "Failed to begin migration transaction";
        return false;
    }

    // Another opener may have completed this migration while the write lock was pending.
    const auto current = storage.execute("SELECT MAX(version) FROM schema_version;");
    if (!current.success() || current.rowCount() != 1) {
        storage.rollbackTransaction();
        pImpl->lastError = "Cannot verify the current schema version during migration";
        return false;
    }
    if (current.getInt(0, 0) < version) {
        const auto result = storage.execute(migration);
        if (!result.success()) {
            storage.rollbackTransaction();
            pImpl->lastError = "Migration failed: " + result.error();
            return false;
        }
    }

    if (!storage.commitTransaction()) {
        storage.rollbackTransaction();
        pImpl->lastError = "Failed to commit migration transaction";
        return false;
    }

    return true;
}

std::string SchemaManager::getStatus() const {
    std::ostringstream oss;
    oss << "Schema version: " << getCurrentVersion() << "/" << getLatestVersion();
    if (needsMigration()) {
        oss << " (migration needed)";
    } else {
        oss << " (up to date)";
    }
    return oss.str();
}

std::string SchemaManager::getLastError() const {
    return pImpl->lastError;
}

std::optional<int> SchemaManager::getRejectedVersion() const {
    return pImpl->rejectedVersion;
}

std::string SchemaManager::loadMigration(int version) const {
    if (version == kChatGptDedupVersion) {
        return kChatGptDedupSql;
    }
    if (version == kQuotaPrecisionVersion) {
        return kQuotaPrecisionSql;
    }
    if (version == kClaudeActivityDedupVersion) {
        return kClaudeActivityDedupSql;
    }

    // Build the version prefix (e.g. "001")
    std::ostringstream prefixStream;
    prefixStream << std::setfill('0') << std::setw(3) << version;
    const std::string prefix = prefixStream.str();

    // Scan db_migrations/ for a file starting with the version prefix
    try {
        for (const auto& entry : std::filesystem::directory_iterator(pImpl->migrationsPath)) {
            const std::string name = entry.path().filename().string();
            if (name.size() > 4
                && name.substr(0, 3) == prefix
                && name.substr(name.size() - 4) == ".sql") {
                std::ifstream file(entry.path());
                if (!file.is_open()) {
                    pImpl->lastError = "Cannot open migration file: " + entry.path().string();
                    return "";
                }
                std::ostringstream content;
                content << file.rdbuf();
                return content.str();
            }
        }
    } catch (const std::exception& e) {
        pImpl->lastError = std::string("Error scanning migrations: ") + e.what();
        return "";
    }

    pImpl->lastError = "Migration file not found for version " + std::to_string(version);
    return "";
}

} // namespace JiStats
