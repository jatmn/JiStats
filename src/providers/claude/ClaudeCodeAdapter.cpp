#include "providers/claude/ClaudeCodeAdapter.hpp"
#include "core/RuntimeStatus.hpp"
#include "providers/claude/ClaudeCodeLocalParser.hpp"
#include <algorithm>
#include <filesystem>

namespace JiStats {

namespace {

std::time_t cacheSnapshotTime(const ClaudeCodeLocalParseResult& result) {
    const auto& date = result.lastComputedDate;
    if (date.size() != 10) {
        return 0;
    }

    const auto snapshotTime = parseRuntimeTimestamp(date + "T00:00:00Z");
    if (snapshotTime <= 0 || snapshotTime > result.parsedAt) {
        return 0;
    }

    std::tm utcDate{};
#ifdef _WIN32
    if (gmtime_s(&utcDate, &snapshotTime) != 0) {
#else
    if (gmtime_r(&snapshotTime, &utcDate) == nullptr) {
#endif
        return 0;
    }

    // Reject calendar dates that the timestamp parser normalized into a different day.
    char canonicalDate[11]{};
    std::strftime(canonicalDate, sizeof(canonicalDate), "%Y-%m-%d", &utcDate);
    return date == canonicalDate ? snapshotTime : 0;
}

} // namespace

struct ClaudeCodeAdapter::Impl {
public:
    bool enabled = true;
    std::string mockStatsCachePath;
    std::string mockSettingsPath;
    std::string lastError;
};

ClaudeCodeAdapter::ClaudeCodeAdapter()
    : pImpl(std::make_unique<Impl>()) {
}

ClaudeCodeAdapter::~ClaudeCodeAdapter() = default;

UsageResponse ClaudeCodeAdapter::fetchUsage(std::time_t from, std::time_t to) {
    UsageResponse response;
    response.windowStart = from;
    response.windowEnd = to;

    const std::string statsCachePath = pImpl->mockStatsCachePath.empty()
        ? ClaudeCodeLocalParser::resolveDefaultStatsCachePath()
        : pImpl->mockStatsCachePath;

    if (statsCachePath.empty()) {
        pImpl->lastError = "Could not resolve Claude stats-cache path";
        response.error = pImpl->lastError;
        return response;
    }

    auto result = ClaudeCodeLocalParser::parseStatsCache(statsCachePath);
    if (!result.success) {
        pImpl->lastError = result.error;
        response.error = result.error;
        return response;
    }

    response.success = true;
    const bool includeLifetimeFallback = from <= 0 && to <= 0;
    for (const auto& event : ClaudeCodeLocalParser::toUsageEvents(result, includeLifetimeFallback)) {
        if (event.eventTime == 0) {
            continue;
        }
        if (from > 0 && event.eventTime < from) {
            continue;
        }
        if (to > 0 && event.eventTime > to) {
            continue;
        }
        response.events.push_back(event);
    }

    for (const auto& event : response.events) {
        response.totalInputTokens += event.inputTokens;
        response.totalOutputTokens += event.outputTokens;
        response.totalCost += event.costMinorUnits;
    }

    return response;
}

QuotaResponse ClaudeCodeAdapter::fetchQuota() {
    QuotaResponse response;

    const std::string statsCachePath = pImpl->mockStatsCachePath.empty()
        ? ClaudeCodeLocalParser::resolveDefaultStatsCachePath()
        : pImpl->mockStatsCachePath;

    if (statsCachePath.empty()) {
        pImpl->lastError = "Could not resolve Claude stats-cache path";
        response.error = pImpl->lastError;
        return response;
    }

    auto result = ClaudeCodeLocalParser::parseStatsCache(statsCachePath);
    if (!result.success) {
        pImpl->lastError = result.error;
        response.error = result.error;
        return response;
    }

    response.success = true;
    response.snapshotTime = cacheSnapshotTime(result);
    response.providerSurface = ClaudeCodeLocalParser::kProviderSurface;
    response.windowType = "local_aggregate";

    // Use total tokens as the "used" metric when available; fall back to total messages.
    long long totalTokens = 0;
    for (const auto& model : result.modelUsage) {
        totalTokens += model.inputTokens + model.outputTokens + model.cacheReadInputTokens + model.cacheCreationInputTokens;
    }
    response.used = totalTokens > 0 ? totalTokens : result.totalMessages;

    // Local files do not contain limit/remaining; keep them at 0 to signal unknown.
    response.limit = 0;
    response.remaining = 0;

    return response;
}

bool ClaudeCodeAdapter::validateCredentials() {
    const std::string statsCachePath = pImpl->mockStatsCachePath.empty()
        ? ClaudeCodeLocalParser::resolveDefaultStatsCachePath()
        : pImpl->mockStatsCachePath;

    if (statsCachePath.empty()) {
        pImpl->lastError = "Could not resolve Claude stats-cache path";
        return false;
    }

    std::error_code ec;
    return std::filesystem::exists(statsCachePath, ec) &&
           std::filesystem::is_regular_file(statsCachePath, ec);
}

std::vector<Capability> ClaudeCodeAdapter::getCapabilities() {
    return {
        {"usage", "Local usage summary from Claude Code runtime artifacts", true,
         "Derived from ~/.claude/stats-cache.json aggregates"},
        {"quota", "Quota mirroring capability", false,
         "Local artifacts do not contain authoritative provider quota limits"},
        {"local_aggregate", "Local aggregate activity", true,
         "Daily message/session counts and per-model token totals"},
    };
}

std::string ClaudeCodeAdapter::getProviderKey() const {
    return "claude_code";
}

std::string ClaudeCodeAdapter::getDisplayName() const {
    return "Claude Code";
}

bool ClaudeCodeAdapter::isEnabled() const {
    return pImpl->enabled;
}

void ClaudeCodeAdapter::setEnabled(bool enabled) {
    pImpl->enabled = enabled;
}

void ClaudeCodeAdapter::setMockStatsCachePath(const std::string& path) {
    pImpl->mockStatsCachePath = path;
}

void ClaudeCodeAdapter::setMockSettingsPath(const std::string& path) {
    pImpl->mockSettingsPath = path;
}

#ifdef JISTATS_ENABLE_TEST_HOOKS
std::string ClaudeCodeAdapter::getLastStatsCachePathForTesting() const {
    return pImpl->mockStatsCachePath;
}

std::string ClaudeCodeAdapter::getLastErrorForTesting() const {
    return pImpl->lastError;
}
#endif

} // namespace JiStats
