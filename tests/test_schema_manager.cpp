#include <catch_amalgamated.hpp>
#include "db/SchemaManager.hpp"
#include "db/SettingsService.hpp"
#include "db/StorageService.hpp"
#include <cstdlib>
#include <filesystem>
#include <string>
#include "sqlite_test_support.hpp"
#include "temp_test_directory.hpp"

using namespace JiStats;
using JiStats::TestSupport::TempTestDatabase;
using JiStats::TestSupport::TempTestDirectory;
using JiStats::TestSupport::requireSqlite;

namespace {

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

TEST_CASE("SchemaManager::getLatestVersion reports the baseline version", "[schema]") {
    StorageService storage;
    SchemaManager manager(storage);

    int latest = manager.getLatestVersion();
    REQUIRE(latest == 12);
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

    bool needs = manager.needsMigration();
    REQUIRE(needs);
}

TEST_CASE("SchemaManager migrates storage baseline tables and quota metadata when SQLite is available", "[schema][migration]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    SchemaManager manager(storage);
    REQUIRE(manager.getCurrentVersion() == 12);
    REQUIRE_FALSE(manager.needsMigration());

    REQUIRE(tableExists(storage, "activity_events"));
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

    auto versions = storage.execute("SELECT version FROM schema_version;");
    REQUIRE(versions.success());
    REQUIRE(versions.rowCount() == 1);
    CHECK(versions.getInt(0, 0) == 12);

    auto providers = storage.execute("SELECT provider_key FROM providers ORDER BY id;");
    REQUIRE(providers.success());
    REQUIRE(providers.rowCount() == 5);
    CHECK(providers.getString(0, 0) == "openai");
    CHECK(providers.getString(1, 0) == "minimax");
    CHECK(providers.getString(2, 0) == "claude_code");
    CHECK(providers.getString(3, 0) == "kimi");
    CHECK(providers.getString(4, 0) == "hicap-ai");

    auto runtime = storage.execute("SELECT id FROM runtime_state;");
    REQUIRE(runtime.success());
    REQUIRE(runtime.rowCount() == 1);
    CHECK(runtime.getInt(0, 0) == 1);

    SettingsService settings(storage);
    CHECK(settings.getBool("ui.taskbar_overlay_enabled") == true);
}

TEST_CASE("StorageService preserves version 12 database settings on reopen", "[schema][migration][settings]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    SettingsService settings(storage);
    REQUIRE(storage.execute(
        "INSERT INTO schema_version (version, description) VALUES (11, 'Previous schema');").success());
    REQUIRE(settings.setBool("ui.taskbar_overlay_enabled", false));
    REQUIRE(settings.setString("ui.last_selected_provider_id", "minimax"));
    storage.close();

    REQUIRE(storage.init(tempDb.path()));
    SchemaManager manager(storage);
    CHECK(manager.getCurrentVersion() == 12);
    CHECK_FALSE(manager.needsMigration());
    const auto versions = storage.execute("SELECT version FROM schema_version ORDER BY version;");
    REQUIRE(versions.success());
    REQUIRE(versions.rowCount() == 2);
    CHECK(versions.getInt(0, 0) == 11);
    CHECK(settings.getBool("ui.taskbar_overlay_enabled") == false);
    CHECK(settings.getString("ui.last_selected_provider_id") == std::optional<std::string>("minimax"));
}

TEST_CASE("StorageService rejects databases older than the baseline", "[schema][migration]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    REQUIRE(storage.execute("UPDATE schema_version SET version = 11;").success());
    storage.close();

    REQUIRE_FALSE(storage.init(tempDb.path()));
    CHECK_FALSE(storage.isOpen());
    CHECK(storage.getLastError().find("Database schema version 11 predates the supported baseline") != std::string::npos);
}

TEST_CASE("StorageService redirects the default database to LOCALAPPDATA",
          "[schema][storage]") {
    requireSqlite();

    TempTestDirectory localAppData("jistats_storage_localappdata");

    ScopedEnvironmentVariable localAppDataEnv("LOCALAPPDATA", localAppData.path().string());

    StorageService redirectedStorage;
    REQUIRE(redirectedStorage.init("jistats.db"));
    const std::filesystem::path resolvedPath = redirectedStorage.getDbPath();
    CHECK(resolvedPath == (localAppData.path() / "JiStats" / "jistats.db"));
    CHECK(std::filesystem::exists(resolvedPath));
}
