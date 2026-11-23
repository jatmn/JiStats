#include <catch_amalgamated.hpp>
#include "db/SchemaManager.hpp"
#include "db/SettingsService.hpp"
#include "db/StorageService.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include "sqlite_test_support.hpp"
#include "temp_test_directory.hpp"

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
#include <sqlite3.h>
#endif

using namespace JiStats;
using JiStats::TestSupport::TempTestDatabase;
using JiStats::TestSupport::TempTestDirectory;
using JiStats::TestSupport::requireSqlite;

namespace {

#ifdef _WIN32
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
#endif

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

void removeHistoricalAnalysisSchema(StorageService& storage) {
    REQUIRE(storage.execute("DROP TABLE recovery_checkpoints; DROP TABLE export_runs;").success());
    for (const std::string table : {"usage_rollups_hourly", "usage_rollups_daily"}) {
        for (const char* column : {"local_request_count", "imported_request_count", "api_request_count",
                                   "unknown_request_count", "backfilled_request_count"}) {
            REQUIRE(storage.execute("ALTER TABLE " + table + " DROP COLUMN " + column + ";").success());
        }
    }
}

std::string readDatabaseFile(const TempTestDatabase& db) {
    std::ifstream input(db.path(), std::ios::binary);
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
struct ConcurrentBaselineOpen {
    std::atomic<int> inspections{0};
    std::atomic<bool> timedOut{false};
    std::promise<void> bothInspected;
    std::shared_future<void> ready = bothInspected.get_future();
};

thread_local ConcurrentBaselineOpen* baselineOpen = nullptr;

int installBaselineInspectionGate(sqlite3* db, char**, const sqlite3_api_routines*) {
    if (!baselineOpen) {
        return SQLITE_OK;
    }
    return sqlite3_trace_v2(
        db, SQLITE_TRACE_STMT,
        [](unsigned, void*, void* statement, void*) {
            const auto* sql = sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
            if (baselineOpen && sql &&
                std::string_view(sql) == "SELECT 1 FROM main.providers WHERE provider_key = 'kimi';") {
                // Each opener already holds a read snapshot. Synchronize only its first inspection.
                auto* gate = std::exchange(baselineOpen, nullptr);
                if (++gate->inspections == 2) {
                    gate->bothInspected.set_value();
                }
                if (gate->ready.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
                    gate->timedOut = true;
                }
            }
            return 0;
        },
        nullptr);
}

bool denySchemaVersionRead = false;

int installSchemaVersionReadFailure(sqlite3* db, char**, const sqlite3_api_routines*) {
    return sqlite3_set_authorizer(
        db,
        [](void*, int action, const char* table, const char* column, const char*, const char*) {
            if (denySchemaVersionRead && action == SQLITE_READ && table && column &&
                std::string_view(table) == "schema_version" && std::string_view(column) == "version") {
                denySchemaVersionRead = false;
                return SQLITE_DENY;
            }
            return SQLITE_OK;
        },
        nullptr);
}

int installCompactionFailure(sqlite3* db, char**, const sqlite3_api_routines*) {
    // VACUUM attaches a temporary database before copying the live rows.
    sqlite3_limit(db, SQLITE_LIMIT_ATTACHED, 0);
    return SQLITE_OK;
}
#endif

} // namespace

TEST_CASE("SchemaManager::getLatestVersion includes the bundled privacy migration", "[schema]") {
    StorageService storage;
    SchemaManager manager(storage);

    int latest = manager.getLatestVersion();
    REQUIRE(latest == 13);
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
    REQUIRE(manager.getCurrentVersion() == 13);
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

    auto versions = storage.execute("SELECT version FROM schema_version ORDER BY version;");
    REQUIRE(versions.success());
    REQUIRE(versions.rowCount() == 2);
    CHECK(versions.getInt(0, 0) == 12);
    CHECK(versions.getInt(1, 0) == 13);

    auto providers = storage.execute("SELECT provider_key FROM providers ORDER BY id;");
    REQUIRE(providers.success());
    REQUIRE(providers.rowCount() == 7);
    CHECK(providers.getString(0, 0) == "openai");
    CHECK(providers.getString(1, 0) == "minimax");
    CHECK(providers.getString(2, 0) == "claude_code");
    CHECK(providers.getString(3, 0) == "kimi");
    CHECK(providers.getString(4, 0) == "hicap-ai");
    CHECK(providers.getString(5, 0) == "grok");
    CHECK(providers.getString(6, 0) == "cursor");

    auto runtime = storage.execute("SELECT id FROM runtime_state;");
    REQUIRE(runtime.success());
    REQUIRE(runtime.rowCount() == 1);
    CHECK(runtime.getInt(0, 0) == 1);

    SettingsService settings(storage);
    CHECK(settings.getBool("ui.taskbar_overlay_enabled") == true);
}

TEST_CASE("StorageService preserves database settings on reopen without rewriting it",
          "[schema][migration][settings]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    SettingsService settings(storage);
    REQUIRE(settings.setBool("ui.taskbar_overlay_enabled", false));
    REQUIRE(settings.setString("ui.last_selected_provider_id", "minimax"));
    storage.close();
    const auto before = readDatabaseFile(tempDb);

    REQUIRE(storage.init(tempDb.path()));
    SchemaManager manager(storage);
    CHECK(manager.getCurrentVersion() == 13);
    CHECK_FALSE(manager.needsMigration());
    const auto versions = storage.execute("SELECT version FROM schema_version ORDER BY version;");
    REQUIRE(versions.success());
    REQUIRE(versions.rowCount() == 2);
    CHECK(versions.getInt(0, 0) == 12);
    CHECK(versions.getInt(1, 0) == 13);
    CHECK(settings.getBool("ui.taskbar_overlay_enabled") == false);
    CHECK(settings.getString("ui.last_selected_provider_id") == std::optional<std::string>("minimax"));
    storage.close();
    CHECK((readDatabaseFile(tempDb) == before));
}

TEST_CASE(
    "StorageService adds missing subscription providers and Codex event precision on an existing version 12 database",
    "[schema][migration]") {
    requireSqlite();
    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));
    REQUIRE(storage.execute("DELETE FROM schema_version WHERE version = 13;").success());
    REQUIRE(storage.execute("DELETE FROM providers WHERE provider_key = 'grok';").success());
    REQUIRE(storage.execute("UPDATE providers SET enabled = 0 WHERE provider_key = 'cursor';").success());
    REQUIRE(storage.execute("ALTER TABLE usage_events DROP COLUMN event_subsecond_ns;").success());
    storage.close();

    REQUIRE(storage.init(tempDb.path()));
    CHECK(columnExists(storage, "usage_events", "event_subsecond_ns"));
    const auto providers =
        storage.execute("SELECT provider_key, enabled FROM providers WHERE provider_key IN ('grok', 'cursor') "
                        "ORDER BY provider_key;");
    REQUIRE(providers.success());
    REQUIRE(providers.rowCount() == 2);
    CHECK(providers.getString(0, 0) == "cursor");
    CHECK_FALSE(providers.getBool(0, 1));
    CHECK(providers.getString(1, 0) == "grok");
    CHECK(providers.getBool(1, 1));
}

TEST_CASE("StorageService reconciles either historical version 12 schema in place", "[schema][migration]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));
    REQUIRE(storage.execute("DELETE FROM schema_version WHERE version = 13;").success());

    SettingsService settings(storage);
    REQUIRE(settings.setString("ui.last_selected_provider_id", "minimax"));
    REQUIRE(
        storage
            .execute("INSERT INTO accounts (id, provider_id, account_key, display_name, scope_type, scope_identifier) "
                     "VALUES (8, 1, 'test', 'Test', 'account', 'test');"
                     "INSERT INTO usage_rollups_hourly "
                     "(provider_id, account_id, rollup_period_start_utc, request_count, local_request_count) "
                     "VALUES (1, 8, '2026-01-01T00:00:00Z', 7, 7);")
            .success());

    // Remove only the schema and seed effects that the old loader could skip at 010.
    bool hadAnalysisMigration = false;
    SECTION("Only the analysis migration ran") {
        hadAnalysisMigration = true;
        REQUIRE(storage.execute("DELETE FROM providers WHERE provider_key = 'kimi';").success());
    }
    SECTION("Only the Kimi migration ran") {
        removeHistoricalAnalysisSchema(storage);
        REQUIRE(storage.execute("UPDATE providers SET enabled = 0 WHERE provider_key = 'kimi';").success());
    }
    REQUIRE(
        storage.execute("INSERT INTO schema_version (version, description) VALUES (10, 'Historical migration 010');")
            .success());
    storage.close();

    REQUIRE(storage.init(tempDb.path()));
    REQUIRE(settings.getString("ui.last_selected_provider_id") == std::optional<std::string>("minimax"));
    const auto versions = storage.execute("SELECT version FROM schema_version ORDER BY version;");
    REQUIRE(versions.rowCount() == 3);
    CHECK(versions.getInt(0, 0) == 10);
    CHECK(versions.getInt(1, 0) == 12);
    CHECK(versions.getInt(2, 0) == 13);
    const auto kimi = storage.execute("SELECT enabled FROM providers WHERE provider_key = 'kimi';");
    REQUIRE(kimi.rowCount() == 1);
    CHECK(kimi.getBool(0, 0) == hadAnalysisMigration);
    for (const std::string table : {"usage_rollups_hourly", "usage_rollups_daily"}) {
        const auto columns = storage.execute(
            "SELECT local_request_count, imported_request_count, api_request_count, unknown_request_count, "
            "backfilled_request_count FROM " +
            table + " LIMIT 0;");
        CHECK(columns.success());
    }
    constexpr std::time_t rollupStart = 1767225600; // 2026-01-01T00:00:00Z
    const auto rollups = storage.getUsageRollups(UsageRollupResolution::Hourly, 1, 8, rollupStart, rollupStart);
    REQUIRE(rollups.size() == 1);
    CHECK(rollups.front().requestCount == 7);
    CHECK(rollups.front().localRequestCount == (hadAnalysisMigration ? 7 : 0));
    CHECK(indexExists(storage, "idx_recovery_checkpoints_provider_account_kind_attempt"));
    CHECK(indexExists(storage, "idx_export_runs_kind_requested"));

    PersistedRecoveryCheckpoint checkpoint;
    checkpoint.providerId = 1;
    checkpoint.accountId = 8;
    checkpoint.syncKind = "runtime_backfill";
    checkpoint.recoveryStart = 100;
    checkpoint.recoveryEnd = 200;
    checkpoint.attemptAt = 300;
    checkpoint.outcome = "success";
    REQUIRE(storage.insertRecoveryCheckpoint(checkpoint));
    CHECK(storage.getLatestRecoveryCheckpoint(1, 8, checkpoint.syncKind).has_value());

    PersistedExportRun exportRun;
    exportRun.runKind = "manual";
    exportRun.format = "html";
    exportRun.requestedAt = 300;
    REQUIRE(storage.insertExportRun(exportRun));
    CHECK(storage.getLatestExportRun(exportRun.runKind).has_value());
    storage.close();

    const auto repaired = readDatabaseFile(tempDb);
    REQUIRE(storage.init(tempDb.path()));
    storage.close();
    CHECK((readDatabaseFile(tempDb) == repaired));
}

TEST_CASE("StorageService upgrades legacy Claude references without reading the source cache", "[schema][migration]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));
    REQUIRE(storage
                .execute(R"sql(
        DELETE FROM schema_version WHERE version = 13;
        INSERT INTO accounts (id, provider_id, account_key, display_name, scope_type, scope_identifier) VALUES
            (21, (SELECT id FROM providers WHERE provider_key = 'claude_code'), 'fixture-claude', 'Claude', 'user', 'fixture'),
            (22, (SELECT id FROM providers WHERE provider_key = 'openai'), 'fixture-openai', 'OpenAI', 'user', 'fixture');
        INSERT INTO activity_events
            (id, provider_id, account_id, provider_surface, activity_kind, started_at_utc, duration_seconds,
             source_type, source_run_id, surface_key, raw_reference)
        SELECT id + 10, provider_id, id, 'claude_code_local_stats', 'session_active', '2025-01-02T00:00:00Z', 30,
               'local', 'claude_code_local_v1|2025-01-02', 'stats-cache',
               CASE WHEN id = 21 THEN 'C:/private-fixture/stats-cache.json|date=2025-01-02'
                    ELSE 'C:/other-fixture/stats-cache.json|date=2025-01-02' END
        FROM accounts WHERE id IN (21, 22);
        INSERT INTO usage_events
            (id, provider_id, account_id, event_time_utc, source_type, source_run_id, total_tokens,
             is_backfilled, raw_reference)
        SELECT id + 20, provider_id, id, '2025-01-02T00:00:00Z', 'provider_backfill',
               'claude_code_local_v1|2025-01-02', 42, 1,
               CASE WHEN id = 21 THEN 'C:/private-fixture/stats-cache.json'
                    ELSE 'C:/other-fixture/stats-cache.json' END
        FROM accounts WHERE id IN (21, 22);
        -- Spread the same legacy reference across ordinary activity and usage pages.
        WITH RECURSIVE days(n) AS (VALUES (1) UNION ALL SELECT n + 1 FROM days WHERE n < 90)
        INSERT INTO activity_events
            (provider_id, account_id, provider_surface, activity_kind, started_at_utc, duration_seconds,
             source_type, source_run_id, surface_key, raw_reference)
        SELECT provider_id, account_id, provider_surface, activity_kind,
               datetime(started_at_utc, n || ' days'), duration_seconds,
               source_type, source_run_id, surface_key, raw_reference
        FROM activity_events CROSS JOIN days WHERE id = 31;
        WITH RECURSIVE days(n) AS (VALUES (1) UNION ALL SELECT n + 1 FROM days WHERE n < 90)
        INSERT INTO usage_events
            (provider_id, account_id, event_time_utc, source_type, source_run_id, total_tokens,
             is_backfilled, raw_reference)
        SELECT provider_id, account_id, datetime(event_time_utc, n || ' days'), source_type, source_run_id,
               total_tokens, is_backfilled, raw_reference
        FROM usage_events CROSS JOIN days WHERE id = 41;
    )sql")
                .success());
    SECTION("WAL database") {
        const auto journalMode = storage.execute("PRAGMA main.journal_mode = WAL;");
        REQUIRE(journalMode.success());
        REQUIRE(journalMode.rowCount() == 1);
        REQUIRE(journalMode.getString(0, 0) == "wal");
    }
    storage.close();

    constexpr auto legacyPath = "C:/private-fixture/stats-cache.json";
    REQUIRE(readDatabaseFile(tempDb).find(legacyPath) != std::string::npos);

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    SECTION("Failed compaction leaves the upgrade pending") {
        const auto extension = reinterpret_cast<void (*)()>(installCompactionFailure);
        REQUIRE(sqlite3_auto_extension(extension) == SQLITE_OK);
        const bool opened = storage.init(tempDb.path());
        sqlite3_cancel_auto_extension(extension);
        REQUIRE_FALSE(opened);
        CHECK(storage.getLastError().find("Cannot compact the database") != std::string::npos);
        CHECK(readDatabaseFile(tempDb).find(legacyPath) != std::string::npos);

        sqlite3* connection = nullptr;
        const int openedRaw = sqlite3_open_v2(tempDb.path().c_str(), &connection, SQLITE_OPEN_READONLY, nullptr);
        const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(connection, sqlite3_close);
        REQUIRE(openedRaw == SQLITE_OK);
        sqlite3_stmt* query = nullptr;
        REQUIRE(sqlite3_prepare_v2(db.get(), "SELECT MAX(version) FROM schema_version;", -1, &query, nullptr) ==
                SQLITE_OK);
        const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> version(query, sqlite3_finalize);
        REQUIRE(sqlite3_step(version.get()) == SQLITE_ROW);
        CHECK(sqlite3_column_int(version.get(), 0) == 12);
    }
#endif
    SECTION("Rollback-journal database") {}

    REQUIRE(storage.init(tempDb.path()));
    SchemaManager manager(storage);
    CHECK(manager.getCurrentVersion() == 13);
    constexpr auto hashedPath = "scope:fnv1a64:1370ad0dcd1472bd";
    const auto activity =
        storage.execute("SELECT id, raw_reference, duration_seconds FROM activity_events ORDER BY id;");
    REQUIRE(activity.success());
    REQUIRE(activity.rowCount() == 92);
    CHECK(activity.getInt(0, 0) == 31);
    CHECK(activity.getString(0, 1) == hashedPath);
    CHECK(activity.getInt(0, 2) == 30);
    CHECK(activity.getString(1, 1) == "C:/other-fixture/stats-cache.json|date=2025-01-02");
    for (size_t row = 2; row < activity.rowCount(); ++row) {
        CHECK(activity.getString(row, 1) == hashedPath);
    }
    const auto usage = storage.execute("SELECT id, raw_reference, total_tokens FROM usage_events ORDER BY id;");
    REQUIRE(usage.success());
    REQUIRE(usage.rowCount() == 92);
    CHECK(usage.getInt(0, 0) == 41);
    CHECK(usage.getString(0, 1) == hashedPath);
    CHECK(usage.getInt(0, 2) == 42);
    CHECK(usage.getString(1, 1) == "C:/other-fixture/stats-cache.json");
    for (size_t row = 2; row < usage.rowCount(); ++row) {
        CHECK(usage.getString(row, 1) == hashedPath);
    }
    // A WAL database must have the compacted pages in the main file before close.
    CHECK(readDatabaseFile(tempDb).find(legacyPath) == std::string::npos);
    storage.close();

    const auto upgraded = readDatabaseFile(tempDb);
    CHECK(upgraded.find(legacyPath) == std::string::npos);
    REQUIRE(storage.init(tempDb.path()));
    storage.close();
    CHECK((readDatabaseFile(tempDb) == upgraded));
}

TEST_CASE("StorageService supports overlapping opens during historical baseline repair", "[schema][migration]") {
#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));
    REQUIRE(storage.execute("DELETE FROM schema_version WHERE version = 13;").success());
    SettingsService settings(storage);
    REQUIRE(settings.setString("ui.last_selected_provider_id", "minimax"));
    removeHistoricalAnalysisSchema(storage);
    storage.close();

    ConcurrentBaselineOpen gate;
    const auto extension = reinterpret_cast<void (*)()>(installBaselineInspectionGate);
    REQUIRE(sqlite3_auto_extension(extension) == SQLITE_OK);

    const auto open = [&] {
        baselineOpen = &gate;
        StorageService connection;
        if (!connection.init(tempDb.path())) {
            return connection.getLastError();
        }
        const auto repair = connection.execute("SELECT local_request_count FROM usage_rollups_hourly LIMIT 0;");
        SettingsService reopenedSettings(connection);
        if (!repair.success() || !tableExists(connection, "export_runs") ||
            reopenedSettings.getString("ui.last_selected_provider_id") != std::optional<std::string>("minimax")) {
            return std::string("The opened database did not expose the repaired schema and original settings");
        }
        return std::string{};
    };

    auto first = std::async(std::launch::async, open);
    auto second = std::async(std::launch::async, open);
    const auto firstError = first.get();
    const auto secondError = second.get();
    sqlite3_cancel_auto_extension(extension);

    REQUIRE(gate.inspections.load() == 2);
    REQUIRE_FALSE(gate.timedOut.load());
    INFO("First open: " << firstError << ", second open: " << secondError);
    CHECK(firstError.empty());
    CHECK(secondError.empty());
#else
    requireSqlite();
#endif
}

TEST_CASE("StorageService does not bootstrap after a failed version read", "[schema][migration]") {
#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));
    REQUIRE(storage
                .execute("DELETE FROM schema_version; INSERT INTO schema_version (version, description) VALUES (11, "
                         "'Unsupported fixture');")
                .success());
    storage.close();
    const auto before = readDatabaseFile(tempDb);

    const auto extension = reinterpret_cast<void (*)()>(installSchemaVersionReadFailure);
    REQUIRE(sqlite3_auto_extension(extension) == SQLITE_OK);
    denySchemaVersionRead = true;
    const bool opened = storage.init(tempDb.path());
    sqlite3_cancel_auto_extension(extension);

    CHECK_FALSE(denySchemaVersionRead);
    REQUIRE_FALSE(opened);
    CHECK_FALSE(storage.isOpen());
    CHECK_FALSE(storage.getRejectedSchemaVersion().has_value());
    CHECK(storage.getLastError().find("Cannot read schema version") != std::string::npos);
    CHECK((readDatabaseFile(tempDb) == before));
#else
    requireSqlite();
#endif
}

TEST_CASE("StorageService rejects databases older than the baseline", "[schema][migration]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    REQUIRE(storage
                .execute("DELETE FROM schema_version; INSERT INTO schema_version (version, description) VALUES (11, "
                         "'Unsupported fixture');")
                .success());
    storage.close();
    const auto before = readDatabaseFile(tempDb);

    REQUIRE_FALSE(storage.init(tempDb.path()));
    CHECK_FALSE(storage.isOpen());
    CHECK(storage.getRejectedSchemaVersion() == 11);
    CHECK(storage.getLastError().find("Database schema version 11 predates the supported baseline") !=
          std::string::npos);
    CHECK((readDatabaseFile(tempDb) == before));
}

#ifdef _WIN32
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
#endif
