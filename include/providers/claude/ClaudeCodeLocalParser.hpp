#pragma once

#include "providers/IProviderAdapter.hpp"
#include "observation/ActivityEvent.hpp"
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace JiStats {

class StorageService;

struct ClaudeCodeDailyActivity {
    std::string date;
    long long messageCount = 0;
    long long sessionCount = 0;
    long long toolCallCount = 0;
};

struct ClaudeCodeDailyModelTokens {
    std::string date;
    std::map<std::string, long long> tokensByModel;
};

struct ClaudeCodeModelAggregate {
    std::string modelKey;
    long long inputTokens = 0;
    long long outputTokens = 0;
    long long cacheReadInputTokens = 0;
    long long cacheCreationInputTokens = 0;
    double costUsd = 0.0;
};

struct ClaudeCodeLocalParseResult {
    bool success = false;
    std::string error;
    std::string parserVersion;
    std::string statsCachePath;
    std::string settingsPath;
    int version = 0;
    std::string lastComputedDate;
    std::string defaultModel;
    std::string autoUpdatesChannel;
    std::vector<ClaudeCodeDailyActivity> dailyActivity;
    std::vector<ClaudeCodeDailyModelTokens> dailyModelTokens;
    std::vector<ClaudeCodeModelAggregate> modelUsage;
    long long totalSessions = 0;
    long long totalMessages = 0;
    std::string firstSessionDate;
    std::time_t parsedAt = 0;
};

class ClaudeCodeLocalParser {
public:
    static constexpr const char* kProviderKey = "claude_code";
    static constexpr const char* kProviderSurface = "claude_code_local_stats";
    static constexpr const char* kParserVersion = "claude_code_local_v1";
    static constexpr const char* kSyncKind = "claude_code_local_stats";

    static ClaudeCodeLocalParseResult parseStatsCache(const std::string& filePath);
    static ClaudeCodeLocalParseResult parseSettings(const std::string& filePath);

    static std::vector<UsageEvent> toUsageEvents(const ClaudeCodeLocalParseResult& result,
                                                 bool includeLifetimeFallback = true);
    static std::vector<ActivityEvent> toActivityEvents(const ClaudeCodeLocalParseResult& result);

    static std::string resolveDefaultStatsCachePath();
    static std::string resolveDefaultSettingsPath();

    static bool importLocalActivity(
        StorageService& storage,
        const std::string& statsCachePath,
        const std::string& settingsPath = "");
};

} // namespace JiStats
