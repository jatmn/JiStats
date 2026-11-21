#pragma once

#include <cstddef>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace JiStats {

class StorageService;

struct OpenAIExportConversationMessage {
    std::string conversationId;
    std::string conversationTitle;
    std::string messageId;
    std::time_t messageTime = 0;
};

struct OpenAIExportImportParseResult {
    bool success = false;
    std::string error;
    std::string inputType;
    std::string sourcePath;
    std::string conversationsPath;
    std::string artifactName;
    std::string parserVersion;
    std::time_t coverageStartAt = 0;
    std::time_t coverageEndAt = 0;
    std::vector<OpenAIExportConversationMessage> userMessages;
};

struct OpenAIExportImportRunResult {
    bool success = false;
    std::string error;
    std::string importRunId;
    std::size_t insertedCount = 0;
    OpenAIExportImportParseResult parsed;
};

class OpenAIExportImport {
public:
    static constexpr const char* kProviderKey = "openai";
    static constexpr const char* kProviderSurface = "openai_subscription_export_import";
    static constexpr const char* kSyncKind = "openai_export_import_history";
    static constexpr const char* kParserVersion = "openai_export_import_v1";

    static OpenAIExportImportParseResult parseInputPath(const std::string& inputPath);
    static OpenAIExportImportRunResult importHistoricalActivity(
        StorageService& storage,
        const std::string& inputPath);
};

} // namespace JiStats
