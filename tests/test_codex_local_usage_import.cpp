#include <catch_amalgamated.hpp>

#include <filesystem>
#include <string>

#include "db/StorageService.hpp"
#include "providers/openai/CodexLocalUsageImporter.hpp"
#include "temp_test_directory.hpp"

#if defined(JISTATS_USE_SYSTEM_SQLITE)
#include <sqlite3.h>
#elif defined(JISTATS_HAS_SQLITE)
#include "sqlite3.h"
#endif

using namespace JiStats;
using JiStats::TestSupport::TempTestDatabase;

namespace {

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
void execSql(sqlite3* db, const char* sql) {
    char* err = nullptr;
    REQUIRE(sqlite3_exec(db, sql, nullptr, nullptr, &err) == SQLITE_OK);
    if (err) sqlite3_free(err);
}

void writeCodexStateDb(const std::string& path, long long tokens, long long newThreadTokens = -1) {
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    execSql(db,
        "CREATE TABLE threads ("
        "id TEXT PRIMARY KEY, model_provider TEXT, model TEXT, reasoning_effort TEXT, "
        "tokens_used INTEGER, created_at_ms INTEGER, updated_at_ms INTEGER);"
        "INSERT INTO threads (id, model_provider, model, reasoning_effort, tokens_used, created_at_ms, updated_at_ms) "
        "VALUES ('thread-1', 'hicap-ai', 'gpt-5.4', 'high', 0, 1700000000000, 1700000000000);");
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(db,
        "UPDATE threads SET tokens_used = ?, updated_at_ms = ? WHERE id = 'thread-1';",
        -1,
        &stmt,
        nullptr) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, tokens);
    sqlite3_bind_int64(stmt, 2, 1700003600000 + tokens);
    REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    if (newThreadTokens >= 0) {
        REQUIRE(sqlite3_prepare_v2(db,
            "INSERT INTO threads (id, model_provider, model, reasoning_effort, tokens_used, created_at_ms, updated_at_ms) "
            "VALUES ('thread-2', 'hicap-ai', 'gpt-5.5', 'medium', ?, 1700010900000, 1700010900000);",
            -1,
            &stmt,
            nullptr) == SQLITE_OK);
        sqlite3_bind_int64(stmt, 1, newThreadTokens);
        REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
}

int countUsageEvents(StorageService& storage) {
    auto result = storage.execute("SELECT COUNT(*) FROM usage_events;");
    REQUIRE(result.success());
    REQUIRE(result.rowCount() == 1);
    return result.getInt(0, 0);
}

long long totalUsageTokens(StorageService& storage) {
    auto result = storage.execute("SELECT COALESCE(SUM(total_tokens), 0) FROM usage_events;");
    REQUIRE(result.success());
    REQUIRE(result.rowCount() == 1);
    return result.getInt64(0, 0);
}

#endif

} // namespace

TEST_CASE("CodexLocalUsageImporter baselines first scan then imports token deltas", "[codex][storage][import]") {
#if !defined(JISTATS_USE_SYSTEM_SQLITE) && !defined(JISTATS_HAS_SQLITE)
    SKIP("SQLite unavailable");
#else
    TempTestDatabase appDb;
    TempTestDatabase sourceDb;
    StorageService storage;
    REQUIRE(storage.init(appDb.path()));

    writeCodexStateDb(sourceDb.path(), 1000);
    CodexLocalUsageImporter importer(storage);
    auto first = importer.importFromSource(sourceDb.path(), 1700007200);
    REQUIRE(first.success);
    CHECK(first.baselinedThreads == 1);
    CHECK(first.deltaEventsInserted == 0);
    CHECK(countUsageEvents(storage) == 0);
    const auto recordedSource = storage.getCodexLocalSource(first.sourcePath, first.sourcePath);
    REQUIRE(recordedSource.has_value());
    CHECK(recordedSource->firstSeenAt == 1700007200);

    std::filesystem::remove(sourceDb.path());
    writeCodexStateDb(sourceDb.path(), 1250, 300);
    auto second = importer.importFromSource(sourceDb.path(), 1700010800);
    REQUIRE(second.success);
    CHECK(second.baselinedThreads == 0);
    CHECK(second.deltaEventsInserted == 2);
    CHECK(countUsageEvents(storage) == 2);
    CHECK(totalUsageTokens(storage) == 550);

    auto repeated = importer.importFromSource(sourceDb.path(), 1700014400);
    REQUIRE(repeated.success);
    CHECK(repeated.deltaEventsInserted == 0);
    CHECK(countUsageEvents(storage) == 2);
#endif
}

TEST_CASE("CodexLocalUsageImporter retries an incomplete consent baseline without importing cumulative usage",
          "[codex][storage][import]") {
#if !defined(JISTATS_USE_SYSTEM_SQLITE) && !defined(JISTATS_HAS_SQLITE)
    SKIP("SQLite unavailable");
#else
    TempTestDatabase appDb;
    TempTestDatabase sourceDb;
    StorageService storage;
    REQUIRE(storage.init(appDb.path()));

    writeCodexStateDb(sourceDb.path(), 950, 300);
    REQUIRE(storage
                .execute("CREATE TRIGGER fail_codex_baseline BEFORE INSERT ON codex_thread_import_state "
                         "WHEN NEW.thread_id = 'thread-2' BEGIN SELECT RAISE(ABORT, 'test checkpoint failure'); END;")
                .success());
    CodexLocalUsageImporter importer(storage);
    const auto failed = importer.importFromSource(sourceDb.path(), 1700003500, 2);
    REQUIRE_FALSE(failed.success);
    const auto state = storage.execute("SELECT (SELECT COUNT(*) FROM codex_local_sources), "
                                       "(SELECT COUNT(*) FROM codex_thread_import_state);");
    REQUIRE(state.success());
    CHECK(state.getInt(0, 0) == 0);
    CHECK(state.getInt(0, 1) == 0);
    CHECK(countUsageEvents(storage) == 0);

    REQUIRE(storage.execute("DROP TRIGGER fail_codex_baseline;").success());
    const auto retry = importer.importFromSource(sourceDb.path(), 1700014400, 2);
    REQUIRE(retry.success);
    CHECK(retry.baselinedThreads == 2);
    CHECK(countUsageEvents(storage) == 0);

    std::filesystem::remove(sourceDb.path());
    writeCodexStateDb(sourceDb.path(), 1000, 300);
    const auto later = importer.importFromSource(sourceDb.path(), 1700018000, 2);
    REQUIRE(later.success);
    CHECK(countUsageEvents(storage) == 1);
    CHECK(totalUsageTokens(storage) == 50);
#endif
}
