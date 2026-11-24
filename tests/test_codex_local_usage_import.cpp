#include <catch_amalgamated.hpp>

#include <filesystem>
#include <string>

#include "db/StorageService.hpp"
#include "providers/openai/CodexLocalSqliteReader.hpp"
#include "providers/openai/CodexLocalUsageImporter.hpp"
#include "temp_test_directory.hpp"

#if defined(JISTATS_USE_SYSTEM_SQLITE)
#include <sqlite3.h>
#elif defined(JISTATS_HAS_SQLITE)
#include "sqlite3.h"
#endif

using namespace JiStats;
using JiStats::TestSupport::TempTestDatabase;
using JiStats::TestSupport::TempTestDirectory;

namespace {

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
void execSql(sqlite3* db, const char* sql) {
    char* err = nullptr;
    REQUIRE(sqlite3_exec(db, sql, nullptr, nullptr, &err) == SQLITE_OK);
    if (err) sqlite3_free(err);
}

void writeCodexStateDb(const std::string& path, long long tokens, long long newThreadTokens = -1,
                       const char* newThreadProvider = "hicap-ai") {
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
                                   "INSERT INTO threads (id, model_provider, model, reasoning_effort, tokens_used, "
                                   "created_at_ms, updated_at_ms) "
                                   "VALUES ('thread-2', ?, 'gpt-5.5', 'medium', ?, 1700010900000, 1700010900000);",
                                   -1, &stmt, nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_bind_text(stmt, 1, newThreadProvider, -1, SQLITE_TRANSIENT) == SQLITE_OK);
        REQUIRE(sqlite3_bind_int64(stmt, 2, newThreadTokens) == SQLITE_OK);
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

TEST_CASE("CodexLocalSqliteReader opens the literal state database path", "[codex][storage][reader]") {
#if !defined(JISTATS_USE_SYSTEM_SQLITE) && !defined(JISTATS_HAS_SQLITE)
    SKIP("SQLite unavailable");
#else
    const std::string folder = GENERATE("plain", "folder#1", "folder%20name");
    CAPTURE(folder);
    const TempTestDirectory directory("codex_state_path");
    const auto source = directory.path() / folder / "state_5.sqlite";
    std::filesystem::create_directory(source.parent_path());
    writeCodexStateDb(source.string(), 1250);

    if (folder == "folder%20name") {
        const auto decoded = directory.path() / "folder name" / "state_5.sqlite";
        std::filesystem::create_directory(decoded.parent_path());
        writeCodexStateDb(decoded.string(), 42);
    }

    const auto result = CodexLocalSqliteReader().readThreads(source.string());
    INFO(result.error);
    REQUIRE(result.success);
    CHECK(result.sourcePath == source.string());
    CHECK(result.sourceIdentity == source.string());
    REQUIRE(result.threads.size() == 1);
    CHECK(result.threads.front().threadId == "thread-1");
    CHECK(result.threads.front().tokensUsed == 1250);
#endif
}

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

TEST_CASE("CodexLocalUsageImporter starts a literal path baseline after a legacy URI checkpoint",
          "[codex][storage][import]") {
#if !defined(JISTATS_USE_SYSTEM_SQLITE) && !defined(JISTATS_HAS_SQLITE)
    SKIP("SQLite unavailable");
#else
    TempTestDatabase appDb;
    const TempTestDirectory directory("codex_literal_baseline");
    const auto decoded = directory.path() / "folder name" / "state_5.sqlite";
    const auto literal = directory.path() / "folder%20name" / "state_5.sqlite";
    std::filesystem::create_directory(decoded.parent_path());
    std::filesystem::create_directory(literal.parent_path());
    writeCodexStateDb(decoded.string(), 42);
    writeCodexStateDb(literal.string(), 1250);

    StorageService storage;
    REQUIRE(storage.init(appDb.path()));
    CodexLocalUsageImporter importer(storage);
    constexpr int generation = 2;
    REQUIRE(importer.importFromSource(decoded.string(), 1700007200, generation).success);
    REQUIRE(std::filesystem::remove(decoded));
    writeCodexStateDb(decoded.string(), 50);
    REQUIRE(importer.importFromSource(decoded.string(), 1700010800, generation).success);
    REQUIRE(totalUsageTokens(storage) == 8);

    // Earlier releases opened the decoded file but keyed its checkpoint by the requested path.
    REQUIRE(storage
                .execute("UPDATE codex_local_sources SET source_path = ?, source_identity = ? WHERE source_path = ?;",
                         {literal.string(), literal.string() + "#attribution-2", decoded.string()})
                .success());
    const auto first = importer.importFromSource(literal.string(), 1700014400, generation);
    REQUIRE(first.success);
    CHECK(first.baselinedThreads == 1);
    CHECK(first.deltaEventsInserted == 0);
    CHECK(totalUsageTokens(storage) == 8);

    REQUIRE(std::filesystem::remove(literal));
    writeCodexStateDb(literal.string(), 1325);
    const auto next = importer.importFromSource(literal.string(), 1700018000, generation);
    REQUIRE(next.success);
    CHECK(next.deltaEventsInserted == 1);
    CHECK(totalUsageTokens(storage) == 83);
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

    const std::string conflictAction = GENERATE("ABORT", "ROLLBACK");
    CAPTURE(conflictAction);
    writeCodexStateDb(sourceDb.path(), 950, 300);
    REQUIRE(storage
                .execute("CREATE TRIGGER fail_codex_baseline BEFORE INSERT ON codex_thread_import_state "
                         "WHEN NEW.thread_id = 'thread-2' BEGIN SELECT RAISE(" +
                         conflictAction + ", 'test checkpoint failure'); END;")
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

TEST_CASE("CodexLocalUsageImporter retries later checkpoint writes without counting overlapping deltas",
          "[codex][storage][import]") {
#if !defined(JISTATS_USE_SYSTEM_SQLITE) && !defined(JISTATS_HAS_SQLITE)
    SKIP("SQLite unavailable");
#else
    TempTestDatabase appDb;
    TempTestDatabase sourceDb;
    StorageService storage;
    REQUIRE(storage.init(appDb.path()));
    CodexLocalUsageImporter importer(storage);
    const bool newThread = GENERATE(false, true);
    CAPTURE(newThread);

    writeCodexStateDb(sourceDb.path(), 1000);
    REQUIRE(importer.importFromSource(sourceDb.path(), 1700007200, 2).success);
    CHECK(countUsageEvents(storage) == 0);

    const std::string failure = newThread ? "INSERT" : "UPDATE";
    const std::string thread = newThread ? "thread-2" : "thread-1";
    REQUIRE(storage
                .execute("CREATE TRIGGER reject_checkpoint BEFORE " + failure +
                         " ON codex_thread_import_state WHEN NEW.thread_id = '" + thread +
                         "' BEGIN SELECT RAISE(ABORT, 'test checkpoint failure'); END;")
                .success());
    std::filesystem::remove(sourceDb.path());
    writeCodexStateDb(sourceDb.path(), 1250, newThread ? 300 : -1);
    const auto failed = importer.importFromSource(sourceDb.path(), 1700010800, 2);
    REQUIRE_FALSE(failed.success);
    CHECK(failed.error.find("Failed to upsert Codex thread import state") != std::string::npos);
    CHECK(failed.deltaEventsInserted == 0);
    CHECK(totalUsageTokens(storage) == 0);

    REQUIRE(storage.execute("DROP TRIGGER reject_checkpoint;").success());
    storage.close();
    REQUIRE(storage.init(appDb.path()));
    std::filesystem::remove(sourceDb.path());
    writeCodexStateDb(sourceDb.path(), 1350, newThread ? 400 : -1);
    const auto retry = importer.importFromSource(sourceDb.path(), 1700014400, 2);
    REQUIRE(retry.success);
    const int expectedEvents = newThread ? 2 : 1;
    const int expectedTokens = newThread ? 750 : 350;
    CHECK(retry.deltaEventsInserted == expectedEvents);
    CHECK(countUsageEvents(storage) == expectedEvents);
    CHECK(totalUsageTokens(storage) == expectedTokens);

    const auto repeated = importer.importFromSource(sourceDb.path(), 1700018000, 2);
    REQUIRE(repeated.success);
    CHECK(repeated.deltaEventsInserted == 0);
    CHECK(countUsageEvents(storage) == expectedEvents);
    CHECK(totalUsageTokens(storage) == expectedTokens);
#endif
}

TEST_CASE("CodexLocalUsageImporter stops when an account write rolls the scan back", "[codex][storage][import]") {
#if !defined(JISTATS_USE_SYSTEM_SQLITE) && !defined(JISTATS_HAS_SQLITE)
    SKIP("SQLite unavailable");
#else
    TempTestDatabase appDb;
    TempTestDatabase sourceDb;
    StorageService storage;
    REQUIRE(storage.init(appDb.path()));
    CodexLocalUsageImporter importer(storage);

    writeCodexStateDb(sourceDb.path(), 1000);
    REQUIRE(importer.importFromSource(sourceDb.path(), 1700007200, 2).success);

    REQUIRE(storage
                .execute("CREATE TRIGGER reject_openai_account BEFORE INSERT ON accounts "
                         "WHEN NEW.account_key = 'openai.codex_local_sqlite' "
                         "BEGIN SELECT RAISE(ROLLBACK, 'test account rollback'); END; "
                         "CREATE TRIGGER reject_thread_checkpoint BEFORE UPDATE ON codex_thread_import_state "
                         "BEGIN SELECT RAISE(ABORT, 'test checkpoint failure after account rollback'); END;")
                .success());

    // The earlier HICAP thread has a pending delta when the OpenAI account write rolls back.
    std::filesystem::remove(sourceDb.path());
    writeCodexStateDb(sourceDb.path(), 1250, 300, "openai");
    const auto failed = importer.importFromSource(sourceDb.path(), 1700010800, 2);
    REQUIRE_FALSE(failed.success);
    CHECK(failed.error == "Could not bind a Codex thread to its local account");
    CHECK(countUsageEvents(storage) == 0);
    const auto checkpoints =
        storage.execute("SELECT thread_id, last_imported_tokens_used FROM codex_thread_import_state;");
    REQUIRE(checkpoints.success());
    REQUIRE(checkpoints.rowCount() == 1);
    CHECK(checkpoints.getString(0, 0) == "thread-1");
    CHECK(checkpoints.getInt64(0, 1) == 1000);

    REQUIRE(storage.execute("DROP TRIGGER reject_openai_account; DROP TRIGGER reject_thread_checkpoint;").success());
    storage.close();
    REQUIRE(storage.init(appDb.path()));
    std::filesystem::remove(sourceDb.path());
    writeCodexStateDb(sourceDb.path(), 1350, 400, "openai");
    const auto retry = importer.importFromSource(sourceDb.path(), 1700014400, 2);
    REQUIRE(retry.success);
    CHECK(retry.deltaEventsInserted == 2);
    CHECK(countUsageEvents(storage) == 2);
    CHECK(totalUsageTokens(storage) == 750);
#endif
}
