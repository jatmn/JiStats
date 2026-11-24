#include "providers/openai/CodexLocalSqliteReader.hpp"

#include <cstdlib>
#include <filesystem>
#include <set>

#if defined(JISTATS_USE_SYSTEM_SQLITE)
#include <sqlite3.h>
#elif defined(JISTATS_HAS_SQLITE)
#include "sqlite3.h"
#else
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
#endif

namespace JiStats {
namespace {

std::string getEnv(const char* name) {
    if (!name || *name == '\0') return {};
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return {};
    }
    std::string result(value);
    free(value);
    return result;
}

std::string sourceIdentityForPath(const std::filesystem::path& path) {
    // Keep identity stable across file writes so thread import state can detect
    // cumulative counter deltas instead of treating every changed DB file as new.
    return path.string();
}

bool hasRequiredColumns(sqlite3* db) {
#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(threads);", -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    std::set<std::string> columns;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* name = sqlite3_column_text(stmt, 1);
        if (name) columns.insert(reinterpret_cast<const char*>(name));
    }
    sqlite3_finalize(stmt);

    for (const char* required : {
        "id", "model_provider", "model", "reasoning_effort", "tokens_used", "created_at_ms", "updated_at_ms"
    }) {
        if (columns.find(required) == columns.end()) return false;
    }
    return true;
#else
    (void)db;
    return false;
#endif
}

std::string columnText(sqlite3_stmt* stmt, int index) {
#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    const unsigned char* value = sqlite3_column_text(stmt, index);
    return value ? reinterpret_cast<const char*>(value) : "";
#else
    (void)stmt;
    (void)index;
    return {};
#endif
}

} // namespace

std::string CodexLocalSqliteReader::resolveDefaultStateDbPath() {
    const std::string codexHome = getEnv("CODEX_HOME");
    if (!codexHome.empty()) {
        std::filesystem::path candidate = std::filesystem::path(codexHome) / "state_5.sqlite";
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec) return candidate.string();
    }

    const std::string userProfile = getEnv("USERPROFILE");
    if (!userProfile.empty()) {
        std::filesystem::path candidate = std::filesystem::path(userProfile) / ".codex" / "state_5.sqlite";
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec) return candidate.string();
    }

    const std::string home = getEnv("HOME");
    if (!home.empty()) {
        std::filesystem::path candidate = std::filesystem::path(home) / ".codex" / "state_5.sqlite";
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec) return candidate.string();
    }

    return {};
}

CodexLocalSqliteReadResult CodexLocalSqliteReader::readThreads(const std::string& requestedPath) const {
    CodexLocalSqliteReadResult result;
#if !defined(JISTATS_USE_SYSTEM_SQLITE) && !defined(JISTATS_HAS_SQLITE)
    result.error = "SQLite not available";
    return result;
#else
    const std::string resolvedPath = requestedPath.empty() ? resolveDefaultStateDbPath() : requestedPath;
    if (resolvedPath.empty()) {
        result.error = "Codex state SQLite database not found";
        return result;
    }

    const std::filesystem::path path(resolvedPath);
    std::error_code ec;
    auto openPath = path;
#ifdef _WIN32
    if (path.native().find_first_not_of(L"/\\") == 2) {
        // Keep both leading separators on fully qualified UNC and device paths.
        openPath.make_preferred();
    } else
#endif
    {
        openPath = std::filesystem::absolute(path, ec);
    }
    if (ec || !std::filesystem::exists(openPath, ec)) {
        result.error = "Codex state SQLite database not found";
        result.sourcePath = resolvedPath;
        return result;
    }

    sqlite3* db = nullptr;
    // Pass a literal filesystem name so SQLite does not decode filename characters as URI syntax.
    if (sqlite3_open_v2(openPath.u8string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        result.error = db ? sqlite3_errmsg(db) : "Failed to open Codex state SQLite database";
        if (db) sqlite3_close(db);
        result.sourcePath = resolvedPath;
        return result;
    }

    result.sourcePath = resolvedPath;
    result.sourceIdentity = sourceIdentityForPath(path);

    if (!hasRequiredColumns(db)) {
        result.error = "Codex threads table is missing required columns";
        sqlite3_close(db);
        return result;
    }

    const char* sql =
        "SELECT id, model_provider, model, reasoning_effort, tokens_used, created_at_ms, updated_at_ms "
        "FROM threads "
        "WHERE id IS NOT NULL AND TRIM(id) != '' "
        "ORDER BY updated_at_ms ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        result.error = sqlite3_errmsg(db);
        sqlite3_close(db);
        return result;
    }

    int stepResult = SQLITE_ROW;
    while ((stepResult = sqlite3_step(stmt)) == SQLITE_ROW) {
        CodexLocalThreadRow row;
        row.threadId = columnText(stmt, 0);
        row.modelProvider = columnText(stmt, 1);
        row.model = columnText(stmt, 2);
        row.reasoningEffort = columnText(stmt, 3);
        row.tokensUsed = sqlite3_column_int64(stmt, 4);
        row.createdAtMs = sqlite3_column_int64(stmt, 5);
        row.updatedAtMs = sqlite3_column_int64(stmt, 6);
        result.threads.push_back(std::move(row));
    }

    sqlite3_finalize(stmt);
    if (stepResult != SQLITE_DONE) {
        result.error = sqlite3_errmsg(db);
        sqlite3_close(db);
        return result;
    }

    sqlite3_close(db);
    result.success = true;
    return result;
#endif
}

} // namespace JiStats
