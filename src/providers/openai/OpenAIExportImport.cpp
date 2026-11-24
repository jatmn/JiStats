#include "providers/openai/OpenAIExportImport.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>

#include "core/JsonParser.hpp"
#include "db/StorageService.hpp"
#include "observation/ActivityEvent.hpp"
#include "providers/openai/ImportTransaction.hpp"

namespace JiStats {

namespace {

constexpr std::string_view kConversationFileName = "conversations.json";
constexpr std::string_view kImportedSourceType = "imported";
constexpr std::string_view kSurfaceKey = "chatgpt_export_conversations";
constexpr std::string_view kImportedAccountKey = "imported-history:openai_subscription_export_import";
constexpr std::string_view kImportedAccountDisplayName = "OpenAI Export Import (Historical)";
constexpr std::string_view kImportedAccountScopeType = "imported_history";
constexpr std::string_view kImportedAccountScopeIdentifier = "openai_subscription_export_import";

const char* const kExportMarkerFileNames[] = {
    "chat.html",
    "message_feedback.json",
    "user.json"
};

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return {};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string jsonEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());

    for (const char ch : value) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }

    return escaped;
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

    char buffer[32] = {};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utcTime) == 0) {
        return "";
    }

    return buffer;
}

bool tryGetFiniteTimestampSeconds(const JsonValue& value, std::time_t& out) {
    if (!value.isNumber()) {
        return false;
    }

    const double raw = value.getNumber();
    if (!std::isfinite(raw) || raw <= 0.0) {
        return false;
    }

    const double floored = std::floor(raw);
    if (floored > static_cast<double>(std::numeric_limits<std::time_t>::max())) {
        return false;
    }

    out = static_cast<std::time_t>(floored);
    return out > 0;
}

bool tryGetNonEmptyString(const JsonValue& object, const char* key, std::string& out) {
    if (!object.isObject()) {
        return false;
    }

    const JsonValue* value = object.getMember(key);
    if (!value || !value->isString()) {
        return false;
    }

    out = value->getString();
    return !out.empty();
}

bool hasExportMarkerSibling(const std::filesystem::path& directoryPath) {
    for (const char* marker : kExportMarkerFileNames) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(directoryPath / marker, ec)) {
            return true;
        }
    }

    return false;
}

bool tryResolveConversationsPath(
    const std::filesystem::path& inputPath,
    std::filesystem::path& conversationsPath,
    std::string& inputType,
    std::string& artifactName,
    std::string& error) {
    std::error_code ec;
    if (!std::filesystem::exists(inputPath, ec)) {
        error = "OpenAI export input path does not exist";
        return false;
    }

    if (std::filesystem::is_directory(inputPath, ec)) {
        const std::filesystem::path candidate = inputPath / kConversationFileName;
        if (!std::filesystem::is_regular_file(candidate, ec)) {
            error = "OpenAI export directory is missing conversations.json";
            return false;
        }

        if (!hasExportMarkerSibling(inputPath)) {
            error = "OpenAI export directory is missing official export marker files";
            return false;
        }

        conversationsPath = candidate;
        inputType = "directory";
        artifactName = inputPath.filename().string();
        return true;
    }

    if (!std::filesystem::is_regular_file(inputPath, ec)) {
        error = "OpenAI export input path is not a regular file";
        return false;
    }

    if (inputPath.extension() == ".zip") {
        error = "OpenAI export zip archives are not supported in the first import slice";
        return false;
    }

    if (inputPath.filename() != kConversationFileName) {
        error = "OpenAI export import currently accepts only conversations.json from an official export";
        return false;
    }

    const std::filesystem::path parentPath = inputPath.parent_path();
    if (parentPath.empty() || !hasExportMarkerSibling(parentPath)) {
        error = "Selected conversations.json is missing official export marker siblings";
        return false;
    }

    conversationsPath = inputPath;
    inputType = "conversations_json";
    artifactName = parentPath.filename().empty()
        ? inputPath.stem().string()
        : parentPath.filename().string();
    return true;
}

bool tryParseConversationMessage(
    const JsonValue& conversation,
    const std::string& conversationId,
    const std::string& conversationTitle,
    std::vector<OpenAIExportConversationMessage>& out) {
    const JsonValue* mapping = conversation.getMember("mapping");
    if (!mapping || !mapping->isObject()) {
        return false;
    }

    bool foundImportableMessage = false;
    for (const auto& entry : mapping->getObject()) {
        const JsonValue& node = entry.second;
        if (!node.isObject()) {
            continue;
        }

        const JsonValue* message = node.getMember("message");
        if (!message || !message->isObject()) {
            continue;
        }

        const JsonValue* author = message->getMember("author");
        if (!author || !author->isObject()) {
            continue;
        }

        std::string role;
        if (!tryGetNonEmptyString(*author, "role", role) || role != "user") {
            continue;
        }

        const JsonValue* createTime = message->getMember("create_time");
        if (!createTime) {
            continue;
        }

        std::time_t messageTime = 0;
        if (!tryGetFiniteTimestampSeconds(*createTime, messageTime)) {
            continue;
        }

        std::string messageId;
        if (!tryGetNonEmptyString(*message, "id", messageId)) {
            if (!tryGetNonEmptyString(node, "id", messageId)) {
                messageId = entry.first;
            }
        }

        if (messageId.empty()) {
            continue;
        }

        OpenAIExportConversationMessage parsed;
        parsed.conversationId = conversationId;
        parsed.conversationTitle = conversationTitle;
        parsed.messageId = messageId;
        parsed.messageTime = messageTime;
        out.push_back(std::move(parsed));
        foundImportableMessage = true;
    }

    return foundImportableMessage;
}

std::string sanitizeRunIdToken(std::string value) {
    for (char& ch : value) {
        const bool alphaNum = (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9');
        if (!alphaNum) {
            ch = '_';
        }
    }

    if (value.empty()) {
        return "export";
    }

    return value;
}

std::string makeImportRunId(const OpenAIExportImportParseResult& parsed) {
    return std::string(OpenAIExportImport::kParserVersion) + ":" +
        sanitizeRunIdToken(parsed.artifactName) + ":" +
        std::to_string(static_cast<long long>(parsed.coverageStartAt)) + ":" +
        std::to_string(static_cast<long long>(parsed.coverageEndAt)) + ":" +
        std::to_string(static_cast<long long>(parsed.userMessages.size()));
}

std::string makeRawReference(
    const OpenAIExportConversationMessage& message) {
    return "conversation:" + message.conversationId + "/message:" + message.messageId;
}

bool tryGetSingleId(
    StorageService& storage,
    const std::string& sql,
    const std::vector<std::string>& params,
    int& outId,
    std::string& error) {
    outId = 0;

    QueryResult result = storage.execute(sql, params);
    if (!result.success()) {
        error = result.error().empty() ? storage.getLastError() : result.error();
        return false;
    }

    if (result.rowCount() == 0) {
        error.clear();
        return false;
    }

    const int id = result.getInt(0, 0);
    if (id <= 0) {
        error = "Resolved row id was invalid";
        return false;
    }

    outId = id;
    error.clear();
    return true;
}

bool ensureOpenAIProvider(StorageService& storage, int& providerId, std::string& error) {
    if (tryGetSingleId(
            storage,
            "SELECT id FROM providers WHERE provider_key = ? LIMIT 1;",
            {OpenAIExportImport::kProviderKey},
            providerId,
            error)) {
        return true;
    }

    QueryResult insertResult = storage.execute(
        "INSERT INTO providers (provider_key, display_name) VALUES (?, ?);",
        {OpenAIExportImport::kProviderKey, "OpenAI"});
    if (insertResult.success()) {
        providerId = static_cast<int>(insertResult.lastInsertRowId());
        error.clear();
        return providerId > 0;
    }

    if (tryGetSingleId(
            storage,
            "SELECT id FROM providers WHERE provider_key = ? LIMIT 1;",
            {OpenAIExportImport::kProviderKey},
            providerId,
            error)) {
        return true;
    }

    if (error.empty()) {
        error = insertResult.error().empty() ? storage.getLastError() : insertResult.error();
    }
    return false;
}

bool ensureImportedAccount(
    StorageService& storage,
    int providerId,
    int& accountId,
    std::string& error) {
    if (tryGetSingleId(
            storage,
            "SELECT id FROM accounts WHERE provider_id = ? AND account_key = ? LIMIT 1;",
            {std::to_string(providerId), std::string(kImportedAccountKey)},
            accountId,
            error)) {
        return true;
    }

    QueryResult insertResult = storage.execute(
        "INSERT INTO accounts (provider_id, account_key, display_name, scope_type, scope_identifier) "
        "VALUES (?, ?, ?, ?, ?);",
        {
            std::to_string(providerId),
            std::string(kImportedAccountKey),
            std::string(kImportedAccountDisplayName),
            std::string(kImportedAccountScopeType),
            std::string(kImportedAccountScopeIdentifier)
        });
    if (insertResult.success()) {
        accountId = static_cast<int>(insertResult.lastInsertRowId());
        error.clear();
        return accountId > 0;
    }

    if (tryGetSingleId(
            storage,
            "SELECT id FROM accounts WHERE provider_id = ? AND account_key = ? LIMIT 1;",
            {std::to_string(providerId), std::string(kImportedAccountKey)},
            accountId,
            error)) {
        return true;
    }

    if (error.empty()) {
        error = insertResult.error().empty() ? storage.getLastError() : insertResult.error();
    }
    return false;
}

std::string makeImportMetadataJson(
    const OpenAIExportImportParseResult& parsed,
    std::string_view importRunId) {
    std::ostringstream stream;
    stream << "{"
        << "\"source_type\":\"chatgpt_export\","
        << "\"provider_surface\":\"" << OpenAIExportImport::kProviderSurface << "\","
        << "\"parser_version\":\"" << parsed.parserVersion << "\","
        << "\"input_type\":\"" << parsed.inputType << "\","
        << "\"artifact_name\":\"" << jsonEscape(parsed.artifactName) << "\","
        << "\"conversations_path\":\"" << jsonEscape(parsed.conversationsPath) << "\","
        << "\"import_run_id\":\"" << jsonEscape(std::string(importRunId)) << "\","
        << "\"coverage_start_utc\":\"" << toUtcIsoString(parsed.coverageStartAt) << "\","
        << "\"coverage_end_utc\":\"" << toUtcIsoString(parsed.coverageEndAt) << "\","
        << "\"user_message_count\":" << parsed.userMessages.size()
        << "}";
    return stream.str();
}

} // namespace

OpenAIExportImportParseResult OpenAIExportImport::parseInputPath(const std::string& inputPath) {
    OpenAIExportImportParseResult result;
    result.parserVersion = kParserVersion;
    result.sourcePath = inputPath;

    if (inputPath.empty()) {
        result.error = "OpenAI export input path is required";
        return result;
    }

    std::filesystem::path conversationsPath;
    if (!tryResolveConversationsPath(
            std::filesystem::path(inputPath),
            conversationsPath,
            result.inputType,
            result.artifactName,
            result.error)) {
        return result;
    }

    result.conversationsPath = conversationsPath.string();
    const std::string contents = readFile(conversationsPath);
    if (contents.empty()) {
        result.error = "Failed to read OpenAI export conversations.json";
        return result;
    }

    JsonValue root;
    if (!JsonParser::parse(contents, root) || !root.isArray()) {
        result.error = "OpenAI export conversations.json is not the expected top-level array";
        return result;
    }

    std::time_t coverageStartAt = 0;
    std::time_t coverageEndAt = 0;

    for (const JsonValue& conversation : root.getArray()) {
        if (!conversation.isObject()) {
            continue;
        }

        std::string conversationId;
        if (!tryGetNonEmptyString(conversation, "id", conversationId)) {
            continue;
        }

        std::string conversationTitle;
        tryGetNonEmptyString(conversation, "title", conversationTitle);

        const std::size_t previousCount = result.userMessages.size();
        if (!tryParseConversationMessage(
                conversation,
                conversationId,
                conversationTitle,
                result.userMessages)) {
            continue;
        }

        for (std::size_t i = previousCount; i < result.userMessages.size(); ++i) {
            const std::time_t messageTime = result.userMessages[i].messageTime;
            if (coverageStartAt == 0 || messageTime < coverageStartAt) {
                coverageStartAt = messageTime;
            }
            if (coverageEndAt == 0 || messageTime > coverageEndAt) {
                coverageEndAt = messageTime;
            }
        }
    }

    if (result.userMessages.empty()) {
        result.error = "OpenAI export did not contain importable user-authored message timestamps";
        return result;
    }

    std::sort(
        result.userMessages.begin(),
        result.userMessages.end(),
        [](const OpenAIExportConversationMessage& left, const OpenAIExportConversationMessage& right) {
            if (left.messageTime != right.messageTime) {
                return left.messageTime < right.messageTime;
            }
            if (left.conversationId != right.conversationId) {
                return left.conversationId < right.conversationId;
            }
            return left.messageId < right.messageId;
        });

    result.coverageStartAt = coverageStartAt;
    result.coverageEndAt = coverageEndAt;
    result.success = true;
    result.error.clear();
    return result;
}

OpenAIExportImportRunResult OpenAIExportImport::importHistoricalActivity(
    StorageService& storage,
    const std::string& inputPath) {
    OpenAIExportImportRunResult run;
    run.parsed = parseInputPath(inputPath);
    if (!run.parsed.success) {
        run.error = run.parsed.error;
        return run;
    }

    if (!storage.isOpen()) {
        run.error = "Database not open";
        return run;
    }

    OpenAIImportDetail::ImportTransaction transaction(storage);
    if (!transaction.begin()) {
        run.error = "Could not start the ChatGPT export import transaction";
        return run;
    }

    int providerId = 0;
    int accountId = 0;
    if (!ensureOpenAIProvider(storage, providerId, run.error) ||
        !ensureImportedAccount(storage, providerId, accountId, run.error)) {
        return run;
    }

    run.importRunId = makeImportRunId(run.parsed);

    std::set<std::string> seenReferences;
    {
        const QueryResult existing =
            storage.execute("SELECT raw_reference FROM activity_events "
                            "WHERE provider_id = ? AND account_id = ? AND provider_surface = ? AND source_type = ? "
                            "AND activity_kind = ? AND raw_reference IS NOT NULL;",
                            {std::to_string(providerId), std::to_string(accountId), kProviderSurface,
                             std::string(kImportedSourceType), toString(ActivityKind::MessageSend)});
        if (!existing.success()) {
            run.error = existing.error();
            return run;
        }
        for (std::size_t row = 0; row < existing.rowCount(); ++row) {
            seenReferences.insert(existing.getString(row, 0));
        }
    }

    std::size_t insertedCount = 0;
    for (const auto& message : run.parsed.userMessages) {
        PersistedActivityEvent persisted;
        persisted.providerId = providerId;
        persisted.accountId = accountId;
        persisted.activity.providerKey = kProviderKey;
        persisted.activity.providerSurface = kProviderSurface;
        persisted.activity.activityKind = ActivityKind::MessageSend;
        persisted.activity.startedAt = message.messageTime;
        persisted.activity.endedAt = message.messageTime;
        persisted.activity.durationSeconds = 0;
        persisted.activity.sourceType = std::string(kImportedSourceType);
        persisted.activity.sourceRunId = run.importRunId;
        persisted.activity.surfaceKey = std::string(kSurfaceKey);
        persisted.activity.windowTitleHint = message.conversationTitle;
        persisted.activity.confidence = ActivityConfidence::Imported;
        persisted.activity.rawReference = makeRawReference(message);

        if (!seenReferences.insert(persisted.activity.rawReference).second) {
            continue;
        }

        if (!storage.insertActivityEvent(persisted, nullptr)) {
            run.error = storage.getLastError();
            return run;
        }

        ++insertedCount;
    }

    PersistedSyncState syncState;
    syncState.providerId = providerId;
    syncState.accountId = accountId;
    syncState.syncKind = kSyncKind;
    syncState.cursorValue = makeImportMetadataJson(run.parsed, run.importRunId);
    syncState.lastAttemptAt = std::time(nullptr);
    syncState.lastSuccessAt = syncState.lastAttemptAt;

    if (!storage.upsertSyncState(syncState, nullptr)) {
        run.error = storage.getLastError();
        return run;
    }

    if (!transaction.commit()) {
        run.error = "Could not commit the ChatGPT export import";
        return run;
    }

    run.insertedCount = insertedCount;
    run.success = true;
    return run;
}

} // namespace JiStats
