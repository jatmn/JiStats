#include "providers/claude/ClaudeCodeLocalParser.hpp"
#include "core/JsonParser.hpp"
#include "db/StorageService.hpp"
#include "observation/ObservationPersistenceBridge.hpp"
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace JiStats {

namespace {

bool tryGetIntegralNumber(const JsonValue& value, long long& out) {
    if (!value.isNumber()) {
        return false;
    }
    const double number = value.getNumber();
    if (!std::isfinite(number)) {
        return false;
    }
    double integerPart = 0.0;
    if (std::modf(number, &integerPart) != 0.0) {
        return false;
    }
    if (integerPart < static_cast<double>(std::numeric_limits<long long>::min()) ||
        integerPart > static_cast<double>(std::numeric_limits<long long>::max())) {
        return false;
    }
    out = static_cast<long long>(integerPart);
    return true;
}

bool tryGetNonNegativeIntegralField(const JsonValue& object, const char* fieldName, long long& out) {
    const JsonValue* value = object.getMember(fieldName);
    if (!value || !tryGetIntegralNumber(*value, out) || out < 0) {
        return false;
    }
    return true;
}

bool tryGetStringField(const JsonValue& object, const char* fieldName, std::string& out) {
    const JsonValue* value = object.getMember(fieldName);
    if (!value || !value->isString()) {
        return false;
    }
    out = value->getString();
    return true;
}

bool tryGetNumberField(const JsonValue& object, const char* fieldName, double& out) {
    const JsonValue* value = object.getMember(fieldName);
    if (!value || !value->isNumber()) {
        return false;
    }
    const double number = value->getNumber();
    if (!std::isfinite(number)) {
        return false;
    }
    out = number;
    return true;
}

std::string tryGetEnvironmentVariable(const char* name) {
    if (!name || *name == '\0') {
        return {};
    }

    char* value = nullptr;
    std::size_t valueLength = 0;
    if (_dupenv_s(&value, &valueLength, name) != 0 || value == nullptr) {
        return {};
    }

    const std::string resolvedValue(value);
    free(value);
    return resolvedValue;
}

std::time_t parseDateToUnixTime(const std::string& dateStr) {
    std::tm tm = {};
    std::istringstream ss(dateStr);
    ss >> std::get_time(&tm, "%Y-%m-%d");
    if (ss.fail()) {
        return 0;
    }
    return std::mktime(&tm);
}

std::string readFileContents(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::string(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

} // namespace

ClaudeCodeLocalParseResult ClaudeCodeLocalParser::parseStatsCache(const std::string& filePath) {
    ClaudeCodeLocalParseResult result;
    result.statsCachePath = filePath;
    result.parserVersion = kParserVersion;
    result.parsedAt = std::time(nullptr);

    if (filePath.empty()) {
        result.error = "Claude stats-cache path is empty";
        return result;
    }

    std::error_code ec;
    if (!std::filesystem::exists(filePath, ec) || !std::filesystem::is_regular_file(filePath, ec)) {
        result.error = "Claude stats-cache file does not exist: " + filePath;
        return result;
    }

    const std::string content = readFileContents(filePath);
    if (content.empty()) {
        result.error = "Claude stats-cache file is empty: " + filePath;
        return result;
    }

    JsonValue root;
    if (!JsonParser::parse(content, root) || !root.isObject()) {
        result.error = "Failed to parse Claude stats-cache JSON: " + filePath;
        return result;
    }

    // version
    const JsonValue* versionField = root.getMember("version");
    if (versionField && versionField->isNumber()) {
        result.version = static_cast<int>(versionField->getNumber());
    }

    // lastComputedDate
    tryGetStringField(root, "lastComputedDate", result.lastComputedDate);

    // totalSessions
    tryGetNonNegativeIntegralField(root, "totalSessions", result.totalSessions);

    // totalMessages
    tryGetNonNegativeIntegralField(root, "totalMessages", result.totalMessages);

    // firstSessionDate
    tryGetStringField(root, "firstSessionDate", result.firstSessionDate);

    // dailyActivity
    const JsonValue* dailyActivityField = root.getMember("dailyActivity");
    if (dailyActivityField && dailyActivityField->isArray()) {
        for (const auto& dayValue : dailyActivityField->getArray()) {
            if (!dayValue.isObject()) {
                continue;
            }
            ClaudeCodeDailyActivity day;
            if (!tryGetStringField(dayValue, "date", day.date)) {
                continue;
            }
            tryGetNonNegativeIntegralField(dayValue, "messageCount", day.messageCount);
            tryGetNonNegativeIntegralField(dayValue, "sessionCount", day.sessionCount);
            tryGetNonNegativeIntegralField(dayValue, "toolCallCount", day.toolCallCount);
            result.dailyActivity.push_back(std::move(day));
        }
    }

    // dailyModelTokens
    const JsonValue* dailyModelTokensField = root.getMember("dailyModelTokens");
    if (dailyModelTokensField && dailyModelTokensField->isArray()) {
        for (const auto& dayValue : dailyModelTokensField->getArray()) {
            if (!dayValue.isObject()) {
                continue;
            }
            ClaudeCodeDailyModelTokens day;
            if (!tryGetStringField(dayValue, "date", day.date)) {
                continue;
            }
            const JsonValue* tokensByModelField = dayValue.getMember("tokensByModel");
            if (tokensByModelField && tokensByModelField->isObject()) {
                for (const auto& [modelKey, tokenValue] : tokensByModelField->getObject()) {
                    long long tokens = 0;
                    if (tryGetIntegralNumber(tokenValue, tokens) && tokens >= 0) {
                        day.tokensByModel[modelKey] = tokens;
                    }
                }
            }
            result.dailyModelTokens.push_back(std::move(day));
        }
    }

    // modelUsage
    const JsonValue* modelUsageField = root.getMember("modelUsage");
    if (modelUsageField && modelUsageField->isObject()) {
        for (const auto& [modelKey, modelValue] : modelUsageField->getObject()) {
            if (!modelValue.isObject()) {
                continue;
            }
            ClaudeCodeModelAggregate agg;
            agg.modelKey = modelKey;
            tryGetNonNegativeIntegralField(modelValue, "inputTokens", agg.inputTokens);
            tryGetNonNegativeIntegralField(modelValue, "outputTokens", agg.outputTokens);
            tryGetNonNegativeIntegralField(modelValue, "cache_read_input_tokens", agg.cacheReadInputTokens);
            tryGetNonNegativeIntegralField(modelValue, "cache_creation_input_tokens", agg.cacheCreationInputTokens);
            double costUsd = 0.0;
            if (tryGetNumberField(modelValue, "cost_usd", costUsd)) {
                agg.costUsd = costUsd;
            }
            result.modelUsage.push_back(std::move(agg));
        }
    }

    result.success = true;
    return result;
}

ClaudeCodeLocalParseResult ClaudeCodeLocalParser::parseSettings(const std::string& filePath) {
    ClaudeCodeLocalParseResult result;
    result.settingsPath = filePath;
    result.parserVersion = kParserVersion;
    result.parsedAt = std::time(nullptr);

    if (filePath.empty()) {
        result.error = "Claude settings path is empty";
        return result;
    }

    std::error_code ec;
    if (!std::filesystem::exists(filePath, ec) || !std::filesystem::is_regular_file(filePath, ec)) {
        result.error = "Claude settings file does not exist: " + filePath;
        return result;
    }

    const std::string content = readFileContents(filePath);
    if (content.empty()) {
        result.error = "Claude settings file is empty: " + filePath;
        return result;
    }

    JsonValue root;
    if (!JsonParser::parse(content, root) || !root.isObject()) {
        result.error = "Failed to parse Claude settings JSON: " + filePath;
        return result;
    }

    tryGetStringField(root, "model", result.defaultModel);
    tryGetStringField(root, "autoUpdatesChannel", result.autoUpdatesChannel);

    result.success = true;
    return result;
}

std::vector<UsageEvent> ClaudeCodeLocalParser::toUsageEvents(const ClaudeCodeLocalParseResult& result) {
    std::vector<UsageEvent> events;
    if (!result.success) {
        return events;
    }

    const std::string sourceRunId = std::string(kParserVersion) + "|" + result.lastComputedDate;

    for (const auto& day : result.dailyModelTokens) {
        const std::time_t dayTime = parseDateToUnixTime(day.date);
        if (dayTime <= 0) {
            continue;
        }
        for (const auto& [modelKey, totalTokens] : day.tokensByModel) {
            UsageEvent event;
            event.eventTime = dayTime;
            event.modelKey = modelKey;
            event.requestType = "chat";
            event.totalTokens = totalTokens;
            event.inputTokens = 0;
            event.outputTokens = 0;
            event.costMinorUnits = 0;
            event.sourceType = "local";
            event.sourceRunId = sourceRunId;
            event.rawReference = result.statsCachePath;
            events.push_back(std::move(event));
        }
    }

    // If no daily model tokens, fall back to aggregate modelUsage without specific dates
    if (events.empty() && !result.modelUsage.empty()) {
        const std::time_t aggregateTime = parseDateToUnixTime(result.lastComputedDate);
        for (const auto& model : result.modelUsage) {
            UsageEvent event;
            event.eventTime = aggregateTime > 0 ? aggregateTime : result.parsedAt;
            event.modelKey = model.modelKey;
            event.requestType = "chat";
            event.totalTokens = model.inputTokens + model.outputTokens + model.cacheReadInputTokens + model.cacheCreationInputTokens;
            event.inputTokens = model.inputTokens + model.cacheReadInputTokens + model.cacheCreationInputTokens;
            event.outputTokens = model.outputTokens;
            event.costMinorUnits = static_cast<long long>(model.costUsd * 10000.0);
            event.sourceType = "local";
            event.sourceRunId = sourceRunId;
            event.rawReference = result.statsCachePath;
            events.push_back(std::move(event));
        }
    }

    return events;
}

std::vector<ActivityEvent> ClaudeCodeLocalParser::toActivityEvents(const ClaudeCodeLocalParseResult& result) {
    std::vector<ActivityEvent> events;
    if (!result.success) {
        return events;
    }

    const std::string sourceRunId = std::string(kParserVersion) + "|" + result.lastComputedDate;

    for (const auto& day : result.dailyActivity) {
        const std::time_t dayTime = parseDateToUnixTime(day.date);
        if (dayTime <= 0) {
            continue;
        }

        ActivityEvent event;
        event.providerKey = kProviderKey;
        event.providerSurface = kProviderSurface;
        event.activityKind = ActivityKind::SessionActive;
        event.startedAt = dayTime;
        event.endedAt = dayTime + 86400 - 1;
        event.durationSeconds = 0;
        event.sourceType = "local";
        event.sourceRunId = sourceRunId;
        event.surfaceKey = "stats-cache";
        event.windowTitleHint = result.defaultModel;
        event.confidence = ActivityConfidence::LocalExact;
        event.rawReference = result.statsCachePath + "|date=" + day.date;
        events.push_back(std::move(event));
    }

    return events;
}

std::string ClaudeCodeLocalParser::resolveDefaultStatsCachePath() {
    std::string homeEnv = tryGetEnvironmentVariable("USERPROFILE");
    if (homeEnv.empty()) {
        homeEnv = tryGetEnvironmentVariable("HOME");
    }
    if (homeEnv.empty()) {
        return {};
    }
    std::filesystem::path path(homeEnv);
    path /= ".claude";
    path /= "stats-cache.json";
    return path.string();
}

std::string ClaudeCodeLocalParser::resolveDefaultSettingsPath() {
    std::string homeEnv = tryGetEnvironmentVariable("USERPROFILE");
    if (homeEnv.empty()) {
        homeEnv = tryGetEnvironmentVariable("HOME");
    }
    if (homeEnv.empty()) {
        return {};
    }
    std::filesystem::path path(homeEnv);
    path /= ".claude";
    path /= "settings.json";
    return path.string();
}

bool ClaudeCodeLocalParser::importLocalActivity(
    StorageService& storage,
    const std::string& statsCachePath,
    const std::string& settingsPath) {

    auto statsResult = parseStatsCache(statsCachePath);
    if (!statsResult.success) {
        return false;
    }

    if (!settingsPath.empty()) {
        auto settingsResult = parseSettings(settingsPath);
        if (settingsResult.success && !settingsResult.defaultModel.empty()) {
            statsResult.defaultModel = settingsResult.defaultModel;
        }
    }

    ObservationPersistenceBridge bridge(storage);
    bool anyPersisted = false;

    auto activityEvents = toActivityEvents(statsResult);
    for (const auto& event : activityEvents) {
        if (bridge.persist(event)) {
            anyPersisted = true;
        }
    }

    return anyPersisted || activityEvents.empty();
}

} // namespace JiStats
