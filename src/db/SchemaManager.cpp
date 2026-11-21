#include "db/SchemaManager.hpp"
#include "db/StorageService.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace JiStats {

class SchemaManager::Impl {
public:
    std::string lastError;
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
    int latest = 0;
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
    int current = getCurrentVersion();
    int latest = getLatestVersion();
    
    if (current >= latest) {
        return true; // Already up to date
    }
    
    for (int version = current + 1; version <= latest; version++) {
        if (!migrateToVersion(version)) {
            return false;
        }
    }
    
    return true;
}

bool SchemaManager::migrateToVersion(int version) {
    std::string migration = buildRecoveryMigration(version);
    if (migration.empty()) {
        migration = loadMigration(version);
    }

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

std::string SchemaManager::buildRecoveryMigration(int version) const {
    if (version != 4) {
        return "";
    }

    if (!tableExists("quota_snapshots")) {
        return "";
    }

    std::ostringstream sql;

    if (!columnExists("quota_snapshots", "quota_dimension")) {
        sql << "ALTER TABLE quota_snapshots ADD COLUMN quota_dimension TEXT NOT NULL DEFAULT 'requests';\n";
    }
    if (!columnExists("quota_snapshots", "remaining_value")) {
        sql << "ALTER TABLE quota_snapshots ADD COLUMN remaining_value INTEGER;\n";
    }
    if (!columnExists("quota_snapshots", "is_reconciled")) {
        sql << "ALTER TABLE quota_snapshots ADD COLUMN is_reconciled INTEGER NOT NULL DEFAULT 0;\n";
    }

    if (columnExists("quota_snapshots", "remaining_value")) {
        sql << "UPDATE quota_snapshots "
               "SET remaining_value = limit_value - used_value "
               "WHERE remaining_value IS NULL AND limit_value IS NOT NULL;\n";
    }

    if (!tableExists("sync_state")) {
        sql << "CREATE TABLE IF NOT EXISTS sync_state ("
               "id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "provider_id INTEGER NOT NULL REFERENCES providers(id),"
               "account_id INTEGER NOT NULL REFERENCES accounts(id),"
               "sync_kind TEXT NOT NULL,"
               "cursor_value TEXT,"
               "last_attempt_at_utc TEXT,"
               "last_success_at_utc TEXT,"
               "last_failure_at_utc TEXT,"
               "last_failure_category TEXT,"
               "created_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),"
               "updated_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),"
               "UNIQUE(provider_id, account_id, sync_kind)"
               ");\n";
    }

    if (!tableExists("mirror_rules")) {
        sql << "CREATE TABLE IF NOT EXISTS mirror_rules ("
               "id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "provider_id INTEGER NOT NULL REFERENCES providers(id),"
               "rule_key TEXT NOT NULL,"
               "rule_version INTEGER NOT NULL DEFAULT 1,"
               "config_json TEXT,"
               "created_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),"
               "UNIQUE(provider_id, rule_key, rule_version)"
               ");\n";
    }

    if (!tableExists("connectivity_state")) {
        sql << "CREATE TABLE IF NOT EXISTS connectivity_state ("
               "id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "provider_id INTEGER NOT NULL REFERENCES providers(id),"
               "account_id INTEGER NOT NULL REFERENCES accounts(id),"
               "status TEXT NOT NULL DEFAULT 'online',"
               "failure_category TEXT,"
               "last_checked_at_utc TEXT,"
               "next_retry_at_utc TEXT,"
               "notice_dismissed_at_utc TEXT,"
               "created_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),"
               "updated_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),"
               "UNIQUE(provider_id, account_id)"
               ");\n";
    }

    sql << "CREATE INDEX IF NOT EXISTS idx_quota_snapshots_dimension_time "
           "ON quota_snapshots(quota_dimension, snapshot_time_utc);\n";
    sql << "CREATE INDEX IF NOT EXISTS idx_sync_state_provider_account_kind "
           "ON sync_state(provider_id, account_id, sync_kind);\n";
    sql << "CREATE INDEX IF NOT EXISTS idx_connectivity_state_provider_account "
           "ON connectivity_state(provider_id, account_id);\n";
    sql << "CREATE INDEX IF NOT EXISTS idx_mirror_rules_provider_key "
           "ON mirror_rules(provider_id, rule_key);\n";
    sql << "INSERT OR IGNORE INTO schema_version (version, description) "
           "VALUES (4, 'Add mirror-state tables and quota snapshot metadata');\n";

    return sql.str();
}

bool SchemaManager::tableExists(const std::string& tableName) const {
    auto result = storage.execute(
        "SELECT name FROM sqlite_master WHERE type = 'table' AND name = ?;",
        {tableName});
    return result.success() && result.rowCount() == 1;
}

bool SchemaManager::columnExists(const std::string& tableName, const std::string& columnName) const {
    auto result = storage.execute("PRAGMA table_info(" + tableName + ");");
    if (!result.success()) {
        return false;
    }

    for (size_t row = 0; row < result.rowCount(); ++row) {
        if (result.getString(row, 1) == columnName) {
            return true;
        }
    }

    return false;
}

} // namespace JiStats
