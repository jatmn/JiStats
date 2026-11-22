#include "db/SchemaManager.hpp"
#include "db/StorageService.hpp"
#include "BaselineSchema.hpp"
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace JiStats {

namespace {
constexpr int kBaselineVersion = 12;
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
    int latest = kBaselineVersion;
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
            return current == kBaselineVersion ? reconcileBaseline(writable) : BaselineResult::Ready;
        }
    }

    if (!writable) {
        return BaselineResult::NeedsWrite;
    }

    const auto result = storage.execute(kBaselineSchemaSql);
    if (!result.success()) {
        pImpl->lastError = "Cannot initialize baseline: " + result.error();
        return BaselineResult::Failed;
    }
    current = kBaselineVersion;
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

    if (!storage.beginTransaction()) {
        pImpl->lastError = "Failed to begin migration transaction";
        return false;
    }

    auto result = storage.execute(migration);
    if (!result.success()) {
        storage.rollbackTransaction();
        pImpl->lastError = "Migration failed: " + result.error();
        return false;
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
