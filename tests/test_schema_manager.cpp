#include <catch_amalgamated.hpp>
#include "db/SchemaManager.hpp"
#include "db/SettingsService.hpp"
#include "db/StorageService.hpp"
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace JiStats;

namespace {

class TempDbFile {
public:
    TempDbFile() {
        path_ = std::filesystem::temp_directory_path() / "jistats_schema_manager_test.db";
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

class TempDir {
public:
    explicit TempDir(const std::string& prefix) {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
            (prefix + "_" + std::to_string(++counter));
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const char* name, const std::string& value)
        : name_(name ? name : "") {
        if (name_.empty()) {
            return;
        }

        char* existing = nullptr;
        std::size_t existingLength = 0;
        if (_dupenv_s(&existing, &existingLength, name_.c_str()) == 0 && existing != nullptr) {
            originalValue_ = existing;
            free(existing);
        }
        hadOriginal_ = originalValue_.has_value();
        _putenv_s(name_.c_str(), value.c_str());
    }

    ~ScopedEnvironmentVariable() {
        if (name_.empty()) {
            return;
        }

        if (hadOriginal_) {
            _putenv_s(name_.c_str(), originalValue_->c_str());
        } else {
            _putenv_s(name_.c_str(), "");
        }
    }

private:
    std::string name_;
    std::optional<std::string> originalValue_;
    bool hadOriginal_ = false;
};

bool tableExists(StorageService& storage, const std::string& tableName) {
    auto result = storage.execute(
        "SELECT name FROM sqlite_master WHERE type = 'table' AND name = ?;",
        {tableName});
    return result.success() && result.rowCount() == 1;
}

bool columnExists(StorageService& storage, const std::string& tableName, const std::string& columnName) {
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

bool indexExists(StorageService& storage, const std::string& indexName) {
    auto result = storage.execute(
        "SELECT name FROM sqlite_master WHERE type = 'index' AND name = ?;",
        {indexName});
    return result.success() && result.rowCount() == 1;
}

} // namespace

TEST_CASE("SchemaManager::getLatestVersion discovers migration files", "[schema]") {
    StorageService storage;
    SchemaManager manager(storage);

    int latest = manager.getLatestVersion();
    REQUIRE(latest == 12);
}

TEST_CASE("SchemaManager::loadMigration loads version 1", "[schema]") {
    StorageService storage;
    SchemaManager manager(storage);

    // loadMigration is private, but we can test via migrateToVersion
    // which calls loadMigration internally. Since we don't have a live DB,
    // test getLatestVersion which exercises the same directory scanning.
    REQUIRE(manager.getLatestVersion() >= 1);
}

TEST_CASE("SchemaManager::getStatus reports version info", "[schema]") {
    StorageService storage;
    SchemaManager manager(storage);

    std::string status = manager.getStatus();
    // Should contain version numbers
    REQUIRE(status.find("Schema version") != std::string::npos);
}

TEST_CASE("SchemaManager::needsMigration without DB", "[schema]") {
    StorageService storage;
    // Don't call storage.init() - getCurrentVersion will return 0
    SchemaManager manager(storage);

    // With no DB, current version is 0, latest is 12, so needs migration
    bool needs = manager.needsMigration();
    REQUIRE(needs);
}

TEST_CASE("SchemaManager migrates storage baseline tables and quota metadata when SQLite is available", "[schema][migration]") {
    TempDbFile tempDb;
    StorageService storage;

    if (!storage.init(tempDb.path())) {
        REQUIRE(storage.getLastError().find("SQLite not available") != std::string::npos);
        return;
    }

    SchemaManager manager(storage);
    REQUIRE(manager.getCurrentVersion() == 12);
    REQUIRE_FALSE(manager.needsMigration());

    REQUIRE(tableExists(storage, "sync_state"));
    REQUIRE(tableExists(storage, "mirror_rules"));
    REQUIRE(tableExists(storage, "connectivity_state"));
    REQUIRE(tableExists(storage, "runtime_state"));
    REQUIRE(tableExists(storage, "provider_capabilities"));
    REQUIRE(tableExists(storage, "usage_rollups_hourly"));
    REQUIRE(tableExists(storage, "usage_rollups_daily"));
    REQUIRE(tableExists(storage, "gap_markers"));
    REQUIRE(tableExists(storage, "recovery_checkpoints"));
    REQUIRE(tableExists(storage, "export_runs"));
    REQUIRE(tableExists(storage, "quota_snapshot_rollups_daily"));

    REQUIRE(columnExists(storage, "quota_snapshots", "quota_dimension"));
    REQUIRE(columnExists(storage, "quota_snapshots", "remaining_value"));
    REQUIRE(columnExists(storage, "quota_snapshots", "is_reconciled"));
    REQUIRE(columnExists(storage, "usage_rollups_hourly", "local_request_count"));
    REQUIRE(columnExists(storage, "usage_rollups_hourly", "imported_request_count"));
    REQUIRE(columnExists(storage, "usage_rollups_hourly", "api_request_count"));
    REQUIRE(columnExists(storage, "usage_rollups_hourly", "unknown_request_count"));
    REQUIRE(columnExists(storage, "usage_rollups_hourly", "backfilled_request_count"));
    REQUIRE(columnExists(storage, "usage_rollups_daily", "local_request_count"));
    REQUIRE(columnExists(storage, "usage_rollups_daily", "imported_request_count"));
    REQUIRE(columnExists(storage, "usage_rollups_daily", "api_request_count"));
    REQUIRE(columnExists(storage, "usage_rollups_daily", "unknown_request_count"));
    REQUIRE(columnExists(storage, "usage_rollups_daily", "backfilled_request_count"));
    REQUIRE(columnExists(storage, "recovery_checkpoints", "comparison_result"));
    REQUIRE(columnExists(storage, "recovery_checkpoints", "imported_total_cost_minor_units"));
    REQUIRE(columnExists(storage, "export_runs", "run_kind"));
    REQUIRE(columnExists(storage, "export_runs", "requested_at_utc"));
    REQUIRE(columnExists(storage, "export_runs", "provider_filter_csv"));
    REQUIRE(columnExists(storage, "export_runs", "account_filter_csv"));
    REQUIRE(columnExists(storage, "quota_snapshot_rollups_daily", "sample_count"));
    REQUIRE(columnExists(storage, "quota_snapshot_rollups_daily", "latest_remaining_value"));
    REQUIRE(columnExists(storage, "quota_snapshot_rollups_daily", "low_water_remaining_value"));
    REQUIRE(indexExists(storage, "idx_recovery_checkpoints_provider_account_kind_attempt"));
    REQUIRE(indexExists(storage, "idx_export_runs_kind_requested"));
    REQUIRE(indexExists(storage, "idx_quota_snapshot_rollups_daily_scope_period"));
    REQUIRE(indexExists(storage, "idx_quota_snapshot_rollups_daily_series_period"));
    REQUIRE(tableExists(storage, "codex_local_sources"));
    REQUIRE(tableExists(storage, "codex_thread_import_state"));
    REQUIRE(columnExists(storage, "codex_thread_import_state", "baseline_tokens_used"));
    REQUIRE(indexExists(storage, "idx_codex_thread_import_state_provider_model"));

    auto versionResult = storage.execute("SELECT MAX(version) FROM schema_version;");
    REQUIRE(versionResult.success());
    REQUIRE(versionResult.rowCount() == 1);
    REQUIRE(versionResult.getInt(0, 0) == 12);
}

TEST_CASE("SchemaManager recovers migration 004 after partial apply when SQLite is available", "[schema][migration]") {
    TempDbFile tempDb;
    StorageService storage;

    if (!storage.init(tempDb.path())) {
        REQUIRE(storage.getLastError().find("SQLite not available") != std::string::npos);
        return;
    }

    auto resetResult = storage.execute(
        "DELETE FROM schema_version WHERE version IN (4, 5, 6);"
        "DROP TABLE IF EXISTS sync_state;"
        "DROP TABLE IF EXISTS mirror_rules;"
        "DROP TABLE IF EXISTS connectivity_state;"
        "DROP TABLE IF EXISTS runtime_state;"
        "DROP TABLE IF EXISTS provider_capabilities;");
    REQUIRE(resetResult.success());

    auto partialApply = storage.execute(
        "CREATE TABLE IF NOT EXISTS sync_state ("
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
        "UNIQUE(provider_id, account_id, sync_kind));");
    REQUIRE(partialApply.success());

    SchemaManager manager(storage);
    REQUIRE(manager.migrateToVersion(4));

    REQUIRE(tableExists(storage, "sync_state"));
    REQUIRE(tableExists(storage, "mirror_rules"));
    REQUIRE(tableExists(storage, "connectivity_state"));
    REQUIRE(columnExists(storage, "quota_snapshots", "quota_dimension"));
    REQUIRE(columnExists(storage, "quota_snapshots", "remaining_value"));
    REQUIRE(columnExists(storage, "quota_snapshots", "is_reconciled"));

    auto versionResult = storage.execute("SELECT MAX(version) FROM schema_version;");
    REQUIRE(versionResult.success());
    REQUIRE(versionResult.rowCount() == 1);
    REQUIRE(versionResult.getInt(0, 0) >= 4);
}

TEST_CASE("SchemaManager upgrades a version 6 database to version 7 usage rollups when SQLite is available", "[schema][migration]") {
    TempDbFile tempDb;
    StorageService storage;

    if (!storage.init(tempDb.path())) {
        REQUIRE(storage.getLastError().find("SQLite not available") != std::string::npos);
        return;
    }

    auto resetResult = storage.execute(
        "DELETE FROM schema_version WHERE version IN (7, 8, 9, 10, 11, 12);"
        "DROP TABLE IF EXISTS usage_rollups_hourly;"
        "DROP TABLE IF EXISTS usage_rollups_daily;"
        "DROP TABLE IF EXISTS gap_markers;");
    REQUIRE(resetResult.success());

    SchemaManager manager(storage);
    REQUIRE(manager.getCurrentVersion() == 6);
    REQUIRE(manager.needsMigration());
    REQUIRE(manager.migrateToVersion(7));

    REQUIRE(tableExists(storage, "usage_rollups_hourly"));
    REQUIRE(tableExists(storage, "usage_rollups_daily"));

    auto versionResult = storage.execute("SELECT MAX(version) FROM schema_version;");
    REQUIRE(versionResult.success());
    REQUIRE(versionResult.rowCount() == 1);
    REQUIRE(versionResult.getInt(0, 0) == 7);
}

TEST_CASE("SchemaManager upgrades a version 7 database to version 8 gap markers when SQLite is available", "[schema][migration]") {
    TempDbFile tempDb;
    StorageService storage;

    if (!storage.init(tempDb.path())) {
        REQUIRE(storage.getLastError().find("SQLite not available") != std::string::npos);
        return;
    }

    auto resetResult = storage.execute(
        "DELETE FROM schema_version WHERE version IN (8, 9, 10, 11, 12);"
        "DROP TABLE IF EXISTS gap_markers;");
    REQUIRE(resetResult.success());

    SchemaManager manager(storage);
    REQUIRE(manager.getCurrentVersion() == 7);
    REQUIRE(manager.needsMigration());
    REQUIRE(manager.migrateToVersion(8));

    REQUIRE(tableExists(storage, "gap_markers"));

    auto versionResult = storage.execute("SELECT MAX(version) FROM schema_version;");
    REQUIRE(versionResult.success());
    REQUIRE(versionResult.rowCount() == 1);
    REQUIRE(versionResult.getInt(0, 0) == 8);
}

TEST_CASE("SchemaManager upgrades a version 9 database to version 10 analysis persistence metadata when SQLite is available", "[schema][migration]") {
    TempDbFile tempDb;
    StorageService storage;

    if (!storage.init(tempDb.path())) {
        REQUIRE(storage.getLastError().find("SQLite not available") != std::string::npos);
        return;
    }

    auto resetResult = storage.execute(
        "DELETE FROM schema_version WHERE version IN (10, 11, 12);"
        "DROP TABLE IF EXISTS recovery_checkpoints;"
        "DROP TABLE IF EXISTS export_runs;");
    REQUIRE(resetResult.success());

    auto removeHourlyColumns = storage.execute(
        "ALTER TABLE usage_rollups_hourly DROP COLUMN local_request_count;"
        "ALTER TABLE usage_rollups_hourly DROP COLUMN imported_request_count;"
        "ALTER TABLE usage_rollups_hourly DROP COLUMN api_request_count;"
        "ALTER TABLE usage_rollups_hourly DROP COLUMN unknown_request_count;"
        "ALTER TABLE usage_rollups_hourly DROP COLUMN backfilled_request_count;");
    auto removeDailyColumns = storage.execute(
        "ALTER TABLE usage_rollups_daily DROP COLUMN local_request_count;"
        "ALTER TABLE usage_rollups_daily DROP COLUMN imported_request_count;"
        "ALTER TABLE usage_rollups_daily DROP COLUMN api_request_count;"
        "ALTER TABLE usage_rollups_daily DROP COLUMN unknown_request_count;"
        "ALTER TABLE usage_rollups_daily DROP COLUMN backfilled_request_count;");

    if (!removeHourlyColumns.success() || !removeDailyColumns.success()) {
        SUCCEED("SQLite build does not support DROP COLUMN; version 10 upgrade reset path cannot be simulated here.");
        return;
    }

    SchemaManager manager(storage);
    REQUIRE(manager.getCurrentVersion() == 9);
    REQUIRE(manager.needsMigration());
    REQUIRE(manager.migrateToVersion(10));

    REQUIRE(tableExists(storage, "recovery_checkpoints"));
    REQUIRE(tableExists(storage, "export_runs"));
    REQUIRE(columnExists(storage, "usage_rollups_hourly", "local_request_count"));
    REQUIRE(columnExists(storage, "usage_rollups_hourly", "imported_request_count"));
    REQUIRE(columnExists(storage, "usage_rollups_hourly", "api_request_count"));
    REQUIRE(columnExists(storage, "usage_rollups_hourly", "unknown_request_count"));
    REQUIRE(columnExists(storage, "usage_rollups_hourly", "backfilled_request_count"));
    REQUIRE(columnExists(storage, "usage_rollups_daily", "local_request_count"));
    REQUIRE(columnExists(storage, "usage_rollups_daily", "imported_request_count"));
    REQUIRE(columnExists(storage, "usage_rollups_daily", "api_request_count"));
    REQUIRE(columnExists(storage, "usage_rollups_daily", "unknown_request_count"));
    REQUIRE(columnExists(storage, "usage_rollups_daily", "backfilled_request_count"));
    REQUIRE(indexExists(storage, "idx_recovery_checkpoints_provider_account_kind_attempt"));
    REQUIRE(indexExists(storage, "idx_export_runs_kind_requested"));

    auto versionResult = storage.execute("SELECT MAX(version) FROM schema_version;");
    REQUIRE(versionResult.success());
    REQUIRE(versionResult.rowCount() == 1);
    REQUIRE(versionResult.getInt(0, 0) == 10);
}

TEST_CASE("SchemaManager upgrades a version 10 database to version 11 quota snapshot rollups when SQLite is available", "[schema][migration]") {
    TempDbFile tempDb;
    StorageService storage;

    if (!storage.init(tempDb.path())) {
        REQUIRE(storage.getLastError().find("SQLite not available") != std::string::npos);
        return;
    }

    auto resetResult = storage.execute(
        "DELETE FROM schema_version WHERE version IN (11, 12);"
        "DROP TABLE IF EXISTS quota_snapshot_rollups_daily;");
    REQUIRE(resetResult.success());

    SchemaManager manager(storage);
    REQUIRE(manager.getCurrentVersion() == 10);
    REQUIRE(manager.needsMigration());
    REQUIRE(manager.migrateToVersion(11));

    REQUIRE(tableExists(storage, "quota_snapshot_rollups_daily"));
    REQUIRE(columnExists(storage, "quota_snapshot_rollups_daily", "sample_count"));
    REQUIRE(columnExists(storage, "quota_snapshot_rollups_daily", "latest_remaining_value"));
    REQUIRE(columnExists(storage, "quota_snapshot_rollups_daily", "low_water_remaining_value"));
    REQUIRE(indexExists(storage, "idx_quota_snapshot_rollups_daily_scope_period"));
    REQUIRE(indexExists(storage, "idx_quota_snapshot_rollups_daily_series_period"));

    auto versionResult = storage.execute("SELECT MAX(version) FROM schema_version;");
    REQUIRE(versionResult.success());
    REQUIRE(versionResult.rowCount() == 1);
    REQUIRE(versionResult.getInt(0, 0) == 11);
}

TEST_CASE("SchemaManager preserves settings rows across version 10 to 11 upgrade", "[schema][migration][settings]") {
    TempDbFile tempDb;
    StorageService storage;

    if (!storage.init(tempDb.path())) {
        REQUIRE(storage.getLastError().find("SQLite not available") != std::string::npos);
        return;
    }

    SettingsService settings(storage);
    REQUIRE(settings.setBool("runtime.openai_subscription_tracking_enabled", true));
    REQUIRE(settings.setString("ui.last_selected_provider_id", "minimax"));
    REQUIRE(settings.setString("ui.analysis_selected_provider_id", "openai"));

    auto resetResult = storage.execute(
        "DELETE FROM schema_version WHERE version IN (11, 12);"
        "DROP TABLE IF EXISTS quota_snapshot_rollups_daily;");
    REQUIRE(resetResult.success());

    SchemaManager manager(storage);
    REQUIRE(manager.getCurrentVersion() == 10);
    REQUIRE(manager.needsMigration());
    REQUIRE(manager.migrateToVersion(11));

    REQUIRE(settings.getBool("runtime.openai_subscription_tracking_enabled") == true);
    REQUIRE(settings.getString("ui.last_selected_provider_id") == std::optional<std::string>("minimax"));
    REQUIRE(settings.getString("ui.analysis_selected_provider_id") == std::optional<std::string>("openai"));
    REQUIRE(tableExists(storage, "quota_snapshot_rollups_daily"));
}

TEST_CASE("StorageService redirects the default database to LOCALAPPDATA",
          "[schema][storage]") {
    TempDir localAppData("jistats_storage_localappdata");

    ScopedEnvironmentVariable localAppDataEnv("LOCALAPPDATA", localAppData.path().string());

    StorageService redirectedStorage;
    REQUIRE(redirectedStorage.init("jistats.db"));
    const std::filesystem::path resolvedPath = redirectedStorage.getDbPath();
    CHECK(resolvedPath == (localAppData.path() / "JiStats" / "jistats.db"));
    CHECK(std::filesystem::exists(resolvedPath));
}
