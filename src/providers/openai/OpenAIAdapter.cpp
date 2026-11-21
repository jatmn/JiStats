#include "providers/openai/OpenAIAdapter.hpp"
#include "core/JsonParser.hpp"
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
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

bool tryGetFiniteNumber(const JsonValue& value, double& out) {
    if (!value.isNumber()) {
        return false;
    }

    const double number = value.getNumber();
    if (!std::isfinite(number)) {
        return false;
    }

    out = number;
    return true;
}

bool tryGetRequiredNonNegativeIntegralField(
    const JsonValue& object,
    const char* fieldName,
    long long& out) {
    const JsonValue* value = object.getMember(fieldName);
    if (!value || !tryGetIntegralNumber(*value, out) || out < 0) {
        return false;
    }

    return true;
}

bool tryGetRequiredPositiveIntegralField(
    const JsonValue& object,
    const char* fieldName,
    long long& out) {
    return tryGetRequiredNonNegativeIntegralField(object, fieldName, out) && out > 0;
}

bool tryGetRequiredNonEmptyStringField(
    const JsonValue& object,
    const char* fieldName,
    std::string& out) {
    const JsonValue* value = object.getMember(fieldName);
    if (!value || !value->isString()) {
        return false;
    }

    out = value->getString();
    return !out.empty();
}

bool tryParseCostMinorUnits(const JsonValue& object, long long& out) {
    const JsonValue* exactCost = object.getMember("cost_minor_units");
    if (exactCost) {
        return tryGetIntegralNumber(*exactCost, out) && out >= 0;
    }

    const JsonValue* dollarsCost = object.getMember("cost_dollars");
    if (!dollarsCost) {
        out = 0;
        return true;
    }

    if (!dollarsCost->isNumber()) {
        return false;
    }

    const double dollars = dollarsCost->getNumber();
    if (!std::isfinite(dollars) || dollars < 0.0) {
        return false;
    }

    const double roundedMinorUnits = std::round(dollars * 10000.0);
    if (roundedMinorUnits > static_cast<double>(std::numeric_limits<long long>::max())) {
        return false;
    }

    out = static_cast<long long>(roundedMinorUnits);
    return true;
}

bool looksLikeUsageEventArray(const JsonValue& value) {
    if (!value.isArray()) {
        return false;
    }

    const auto& array = value.getArray();
    if (array.empty()) {
        return true;
    }

    for (const JsonValue& entry : array) {
        if (!entry.isObject()) {
            continue;
        }

        if (entry.getMember("timestamp") || entry.getMember("model") ||
            entry.getMember("request_type") || entry.getMember("input_tokens")) {
            return true;
        }
    }

    return false;
}

const JsonValue* findEventsArray(const JsonValue& root) {
    if (root.isArray()) {
        return &root;
    }

    if (!root.isObject()) {
        return nullptr;
    }

    const char* preferredKeys[] = {"events", "data"};
    for (const char* key : preferredKeys) {
        const JsonValue* candidate = root.getMember(key);
        if (candidate && looksLikeUsageEventArray(*candidate)) {
            return candidate;
        }
    }

    for (const auto& pair : root.getObject()) {
        if (looksLikeUsageEventArray(pair.second)) {
            return &pair.second;
        }
    }

    return nullptr;
}

bool tryParseUsageEvent(const JsonValue& eventJson, const std::string& filePath, UsageEvent& event) {
    if (!eventJson.isObject()) {
        return false;
    }

    long long timestamp = 0;
    long long inputTokens = 0;
    long long outputTokens = 0;
    long long totalTokens = 0;
    long long costMinorUnits = 0;
    std::string modelKey;
    std::string requestType;

    if (!tryGetRequiredNonNegativeIntegralField(eventJson, "timestamp", timestamp) ||
        timestamp == 0 ||
        !tryGetRequiredNonEmptyStringField(eventJson, "model", modelKey) ||
        !tryGetRequiredNonEmptyStringField(eventJson, "request_type", requestType) ||
        !tryGetRequiredNonNegativeIntegralField(eventJson, "input_tokens", inputTokens) ||
        !tryGetRequiredNonNegativeIntegralField(eventJson, "output_tokens", outputTokens) ||
        !tryGetRequiredNonNegativeIntegralField(eventJson, "total_tokens", totalTokens) ||
        !tryParseCostMinorUnits(eventJson, costMinorUnits)) {
        return false;
    }

    if (inputTokens + outputTokens != totalTokens) {
        return false;
    }

    event.eventTime = static_cast<std::time_t>(timestamp);
    event.modelKey = std::move(modelKey);
    event.requestType = std::move(requestType);
    event.inputTokens = inputTokens;
    event.outputTokens = outputTokens;
    event.totalTokens = totalTokens;
    event.costMinorUnits = costMinorUnits;
    event.sourceType = "api";
    event.sourceRunId = "fixture";
    event.rawReference = filePath;
    return true;
}

bool tryParseCodexRateLimitBucket(
    const JsonValue& object,
    const char* bucketKey,
    CodexRateLimitBucket& bucket) {
    const JsonValue* bucketValue = object.getMember(bucketKey);
    if (!bucketValue) {
        return false;
    }

    if (bucketValue->isNull()) {
        bucket = {};
        bucket.bucketKey = bucketKey;
        return true;
    }

    if (!bucketValue->isObject()) {
        return false;
    }

    double usedPercent = 0.0;
    long long windowDurationMins = 0;
    long long resetsAt = 0;
    const JsonValue* usedPercentValue = bucketValue->getMember("usedPercent");
    if (!usedPercentValue) {
        usedPercentValue = bucketValue->getMember("used_percent");
    }

    const JsonValue* windowDurationValue = bucketValue->getMember("windowDurationMins");
    if (!windowDurationValue) {
        windowDurationValue = bucketValue->getMember("window_minutes");
    }

    const JsonValue* resetsAtValue = bucketValue->getMember("resetsAt");
    if (!resetsAtValue) {
        resetsAtValue = bucketValue->getMember("resets_at");
    }

    if (!usedPercentValue || !tryGetFiniteNumber(*usedPercentValue, usedPercent) ||
        usedPercent > 100 ||
        usedPercent < 0.0 ||
        !windowDurationValue || !tryGetIntegralNumber(*windowDurationValue, windowDurationMins) ||
        windowDurationMins <= 0 ||
        !resetsAtValue || !tryGetIntegralNumber(*resetsAtValue, resetsAt) ||
        resetsAt <= 0) {
        return false;
    }

    bucket.bucketKey = bucketKey;
    bucket.usedPercent = usedPercent;
    bucket.windowDurationMinutes = static_cast<int>(windowDurationMins);
    bucket.resetsAt = static_cast<std::time_t>(resetsAt);
    bucket.present = true;
    return true;
}

bool tryParseUtcZuluTimestamp(const std::string& value, std::time_t& out) {
    if (value.empty()) {
        return false;
    }

    std::string normalized = value;
    const std::size_t decimalPoint = normalized.find('.');
    const std::size_t zSuffix = normalized.rfind('Z');
    if (decimalPoint != std::string::npos && zSuffix != std::string::npos && decimalPoint < zSuffix) {
        normalized.erase(decimalPoint, zSuffix - decimalPoint);
    }

    std::tm utcTime{};
    std::istringstream stream(normalized);
    stream >> std::get_time(&utcTime, "%Y-%m-%dT%H:%M:%SZ");
    if (stream.fail()) {
        return false;
    }

#ifdef _WIN32
    out = _mkgmtime(&utcTime);
#else
    out = timegm(&utcTime);
#endif
    return out != static_cast<std::time_t>(-1);
}

std::time_t tryExtractCodexEventTimestamp(const JsonValue& root) {
    if (!root.isObject()) {
        return 0;
    }

    const JsonValue* timestampValue = root.getMember("timestamp");
    if (!timestampValue || !timestampValue->isString()) {
        return 0;
    }

    std::time_t parsed = 0;
    return tryParseUtcZuluTimestamp(timestampValue->getString(), parsed) ? parsed : 0;
}

bool tryExtractCodexRateLimitObject(
    const JsonValue& root,
    const JsonValue*& rateLimitsObject,
    std::string& sourceType) {
    if (!root.isObject()) {
        return false;
    }

    const JsonValue* directRateLimits = root.getMember("rateLimits");
    if (directRateLimits && directRateLimits->isObject()) {
        rateLimitsObject = directRateLimits;
        sourceType = "codex_local_fixture";
        return true;
    }

    const JsonValue* payload = root.getMember("payload");
    if (!payload || !payload->isObject()) {
        return false;
    }

    const JsonValue* typeValue = payload->getMember("type");
    if (!typeValue || !typeValue->isString() || typeValue->getString() != "token_count") {
        return false;
    }

    const JsonValue* rolloutRateLimits = payload->getMember("rate_limits");
    if (!rolloutRateLimits || !rolloutRateLimits->isObject()) {
        return false;
    }

    rateLimitsObject = rolloutRateLimits;
    sourceType = "codex_local_rollout";
    return true;
}

bool tryParseCodexRateLimitSnapshot(
    const JsonValue& rateLimitsObject,
    const std::string& sourceType,
    const std::string& rawResponse,
    const std::string& sourcePath,
    std::time_t eventTime,
    CodexRateLimitSnapshot& snapshot) {
    if (!tryParseCodexRateLimitBucket(rateLimitsObject, "primary", snapshot.primary)) {
        snapshot.error = "Codex rate-limit fixture has an invalid primary bucket";
        return false;
    }

    if (!tryParseCodexRateLimitBucket(rateLimitsObject, "secondary", snapshot.secondary)) {
        snapshot.error = "Codex rate-limit fixture has an invalid secondary bucket";
        return false;
    }

    const JsonValue* limitId = rateLimitsObject.getMember("limit_id");
    if (limitId) {
        if (!limitId->isString()) {
            snapshot.error = "Codex rate-limit fixture has an invalid limit_id";
            return false;
        }
        snapshot.limitId = limitId->getString();
    }

    const JsonValue* planType = rateLimitsObject.getMember("plan_type");
    if (planType) {
        if (planType->isNull()) {
            snapshot.planType.clear();
        } else if (planType->isString()) {
            snapshot.planType = planType->getString();
        } else {
            snapshot.error = "Codex rate-limit fixture has an invalid plan_type";
            return false;
        }
    }

    const JsonValue* reachedType = rateLimitsObject.getMember("rateLimitReachedType");
    if (reachedType) {
        if (reachedType->isNull()) {
            snapshot.reachedType.clear();
        } else if (reachedType->isString()) {
            snapshot.reachedType = reachedType->getString();
        } else {
            snapshot.error = "Codex rate-limit fixture has an invalid rateLimitReachedType";
            return false;
        }
    }

    snapshot.success = true;
    snapshot.snapshotTime = eventTime > 0 ? eventTime : std::time(nullptr);
    snapshot.eventTime = eventTime;
    snapshot.rawResponse = rawResponse;
    snapshot.sourcePath = sourcePath;
    snapshot.sourceType = sourceType;
    return true;
}

bool tryExtractCodexTokenUsage(const JsonValue& root, CodexTokenUsageSnapshot& snapshot) {
    if (!root.isObject()) {
        return false;
    }

    const JsonValue* payload = root.getMember("payload");
    if (!payload || !payload->isObject()) {
        return false;
    }

    const JsonValue* typeValue = payload->getMember("type");
    if (!typeValue || !typeValue->isString() || typeValue->getString() != "token_count") {
        return false;
    }

    const JsonValue* info = payload->getMember("info");
    if (!info || !info->isObject()) {
        return false;
    }

    const JsonValue* totalUsage = info->getMember("total_token_usage");
    if (!totalUsage || !totalUsage->isObject()) {
        totalUsage = info->getMember("last_token_usage");
        if (!totalUsage || !totalUsage->isObject()) {
            return false;
        }
    }

    long long inputTokens = 0;
    long long cachedInputTokens = 0;
    long long outputTokens = 0;
    long long reasoningOutputTokens = 0;
    long long totalTokens = 0;

    if (!tryGetRequiredNonNegativeIntegralField(*totalUsage, "input_tokens", inputTokens) ||
        !tryGetRequiredNonNegativeIntegralField(*totalUsage, "output_tokens", outputTokens) ||
        !tryGetRequiredNonNegativeIntegralField(*totalUsage, "total_tokens", totalTokens)) {
        return false;
    }

    // Optional fields
    tryGetRequiredNonNegativeIntegralField(*totalUsage, "cached_input_tokens", cachedInputTokens);
    tryGetRequiredNonNegativeIntegralField(*totalUsage, "reasoning_output_tokens", reasoningOutputTokens);

    snapshot.inputTokens = inputTokens;
    snapshot.cachedInputTokens = cachedInputTokens;
    snapshot.outputTokens = outputTokens;
    snapshot.reasoningOutputTokens = reasoningOutputTokens;
    snapshot.totalTokens = totalTokens;
    return true;
}

bool isRolloutFilePath(const std::filesystem::path& path) {
    const std::string filename = path.filename().string();
    return filename.size() >= 14 &&
        filename.rfind("rollout-", 0) == 0 &&
        path.extension() == ".jsonl";
}

bool shouldReplaceCodexSnapshot(
    const CodexRateLimitSnapshot& candidate,
    const CodexRateLimitSnapshot& current) {
    if (!current.success) {
        return true;
    }

    if (candidate.eventTime != current.eventTime) {
        return candidate.eventTime > current.eventTime;
    }

    return candidate.sourcePath > current.sourcePath;
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

std::string resolveCodexRolloutDirectoryImpl() {
    const std::string codexHome = tryGetEnvironmentVariable("CODEX_HOME");
    if (!codexHome.empty()) {
        return (std::filesystem::path(codexHome) / "sessions").string();
    }

    const std::string userProfile = tryGetEnvironmentVariable("USERPROFILE");
    if (!userProfile.empty()) {
        return (std::filesystem::path(userProfile) / ".codex" / "sessions").string();
    }

    const std::string home = tryGetEnvironmentVariable("HOME");
    if (!home.empty()) {
        return (std::filesystem::path(home) / ".codex" / "sessions").string();
    }

    return {};
}

const CodexRateLimitBucket* chooseCodexQuotaBucket(const CodexRateLimitSnapshot& snapshot) {
    if (snapshot.primary.present) {
        return &snapshot.primary;
    }

    if (snapshot.secondary.present) {
        return &snapshot.secondary;
    }

    return nullptr;
}

bool tryMapCodexSnapshotToQuotaResponse(
    const CodexRateLimitSnapshot& snapshot,
    QuotaResponse& response) {
    const CodexRateLimitBucket* bucket = chooseCodexQuotaBucket(snapshot);
    if (!bucket) {
        response.error = "Codex rollout snapshot does not contain a usable rate-limit bucket";
        response.snapshotTime = snapshot.snapshotTime > 0 ? snapshot.snapshotTime : std::time(nullptr);
        response.rawResponse = snapshot.rawResponse;
        return false;
    }

    const long long usedPercent = std::clamp<long long>(
        static_cast<long long>(std::llround(bucket->usedPercent)),
        0,
        100);
    const long long windowDurationSeconds =
        static_cast<long long>(bucket->windowDurationMinutes) * 60LL;

    response.success = true;
    response.snapshotTime = snapshot.snapshotTime;
    response.windowEnd = bucket->resetsAt;
    response.windowStart = (bucket->resetsAt > windowDurationSeconds)
        ? (bucket->resetsAt - windowDurationSeconds)
        : 0;
    response.used = usedPercent;
    response.limit = 100;
    response.remaining = response.limit - response.used;
    response.windowType = "interval";
    if (snapshot.secondary.present) {
        const long long secondaryUsedPercent = std::clamp<long long>(
            static_cast<long long>(std::llround(snapshot.secondary.usedPercent)),
            0,
            100);
        const long long secondaryWindowDurationSeconds =
            static_cast<long long>(snapshot.secondary.windowDurationMinutes) * 60LL;
        response.secondaryWindowEnd = snapshot.secondary.resetsAt;
        response.secondaryWindowStart = (snapshot.secondary.resetsAt > secondaryWindowDurationSeconds)
            ? (snapshot.secondary.resetsAt - secondaryWindowDurationSeconds)
            : 0;
        response.secondaryUsed = secondaryUsedPercent;
        response.secondaryLimit = 100;
        response.secondaryRemaining = response.secondaryLimit - response.secondaryUsed;
        response.secondaryWindowType =
            snapshot.secondary.windowDurationMinutes >= (24 * 60) ? "weekly" : "interval";
    }
    response.providerSurface = "openai_subscription_codex_local";
    response.rawResponse = snapshot.rawResponse;
    return true;
}

} // namespace

// Private implementation (PImpl) for OpenAIAdapter
struct OpenAIAdapter::Impl {
    bool enabled_ = true;
    // Future: store API key, organization, endpoints, etc.
};

OpenAIAdapter::OpenAIAdapter(const std::string& apiKey, const std::string& groupId)
    : pImpl(std::make_unique<Impl>()) {
    (void)apiKey;
    (void)groupId;
}

OpenAIAdapter::~OpenAIAdapter() = default;

// IProviderAdapter implementation
UsageResponse OpenAIAdapter::fetchUsage(std::time_t from, std::time_t to) {
    UsageResponse resp;
    resp.success = true;
    resp.windowStart = from;
    resp.windowEnd = to;
    resp.rawResponse.clear();
    // No real data here in scaffold
    return resp;
}

QuotaResponse OpenAIAdapter::fetchQuota() {
    QuotaResponse q;
    const std::string rolloutDirectory = resolveCodexRolloutDirectoryImpl();
    if (rolloutDirectory.empty()) {
        q.success = false;
        q.error = "Unable to determine the local Codex rollout directory";
        q.snapshotTime = std::time(nullptr);
        return q;
    }

    const CodexRateLimitSnapshot snapshot =
        parseLatestCodexRateLimitSnapshotFromRolloutDirectory(rolloutDirectory);
    if (!snapshot.success) {
        q.success = false;
        q.error = snapshot.error;
        q.snapshotTime = std::time(nullptr);
        q.rawResponse = snapshot.rawResponse;
        return q;
    }

    if (!tryMapCodexSnapshotToQuotaResponse(snapshot, q)) {
        q.success = false;
        return q;
    }

    return q;
}

bool OpenAIAdapter::validateCredentials() {
    // In scaffolding, assume credentials are valid; real validation to be added later
    return true;
}

std::vector<Capability> OpenAIAdapter::getCapabilities() {
    return {
        {"usage", "Fetch and mirror usage data", true, ""},
        {"quota", "Quota mirroring capability", true, ""}
    };
}

std::string OpenAIAdapter::getProviderKey() const {
    return "openai";
}

std::string OpenAIAdapter::getDisplayName() const {
    return "OpenAI";
}

bool OpenAIAdapter::isEnabled() const {
    return pImpl ? pImpl->enabled_ : true;
}

void OpenAIAdapter::setEnabled(bool enabled) {
    if (pImpl) pImpl->enabled_ = enabled;
}

// Fixture parsing helpers
std::vector<UsageEvent> OpenAIAdapter::parseFixtureFile(const std::string& filePath) {
    std::vector<UsageEvent> events;

    // Read file content
    std::ifstream in(filePath);
    if (!in.is_open()) {
        return events;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string content = buffer.str();

    // Parse JSON using proper JsonParser
    JsonValue root;
    if (!JsonParser::parse(content, root)) {
        return events;
    }

    const JsonValue* eventsArray = findEventsArray(root);
    if (!eventsArray) {
        return events;
    }

    // Process each event in the array
    for (const JsonValue& eventJson : eventsArray->getArray()) {
        UsageEvent event;
        if (tryParseUsageEvent(eventJson, filePath, event)) {
            events.push_back(std::move(event));
        }
    }

    return events;
}

CodexRateLimitSnapshot OpenAIAdapter::parseCodexRateLimitFixtureFile(const std::string& filePath) {
    CodexRateLimitSnapshot snapshot;

    std::ifstream in(filePath);
    if (!in.is_open()) {
        snapshot.error = "Failed to open Codex rate-limit fixture";
        return snapshot;
    }

    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string content = buffer.str();

    JsonValue root;
    if (JsonParser::parse(content, root)) {
        const JsonValue* rateLimitsObject = nullptr;
        std::string sourceType;
        if (tryExtractCodexRateLimitObject(root, rateLimitsObject, sourceType)) {
            const std::time_t eventTime = tryExtractCodexEventTimestamp(root);
            tryParseCodexRateLimitSnapshot(
                *rateLimitsObject,
                sourceType,
                content,
                filePath,
                eventTime,
                snapshot);
            if (snapshot.success) {
                return snapshot;
            }
        }
    }

    CodexRateLimitSnapshot latestSnapshot;
    std::istringstream lines(content);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }

        JsonValue lineRoot;
        if (!JsonParser::parse(line, lineRoot)) {
            continue;
        }

        const JsonValue* rateLimitsObject = nullptr;
        std::string sourceType;
        if (!tryExtractCodexRateLimitObject(lineRoot, rateLimitsObject, sourceType)) {
            continue;
        }

        CodexRateLimitSnapshot candidate;
        const std::time_t eventTime = tryExtractCodexEventTimestamp(lineRoot);
        if (!tryParseCodexRateLimitSnapshot(
                *rateLimitsObject,
                sourceType,
                line,
                filePath,
                eventTime,
                candidate)) {
            snapshot.error = candidate.error;
            continue;
        }

        if (shouldReplaceCodexSnapshot(candidate, latestSnapshot)) {
            latestSnapshot = std::move(candidate);
        }
    }

    if (latestSnapshot.success) {
        return latestSnapshot;
    }

    if (snapshot.error.empty()) {
        snapshot.error = "Codex rate-limit fixture is missing a supported rate-limit payload";
    }
    return snapshot;
}

CodexRateLimitSnapshot OpenAIAdapter::parseLatestCodexRateLimitSnapshotFromRolloutDirectory(
    const std::string& directoryPath) {
    CodexRateLimitSnapshot latestSnapshot;

    if (directoryPath.empty()) {
        latestSnapshot.error = "Codex rollout directory path is empty";
        return latestSnapshot;
    }

    std::error_code ec;
    const std::filesystem::path root(directoryPath);
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        latestSnapshot.error = "Codex rollout directory does not exist";
        return latestSnapshot;
    }

    std::size_t rolloutFileCount = 0;
    std::size_t failedSnapshotCount = 0;
    std::string lastFailureError;
    std::string lastFailurePath;

    for (std::filesystem::recursive_directory_iterator it(
             root,
             std::filesystem::directory_options::skip_permission_denied,
             ec),
         end;
         it != end;
         it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }

        if (!it->is_regular_file(ec) || ec || !isRolloutFilePath(it->path())) {
            ec.clear();
            continue;
        }

        ++rolloutFileCount;
        CodexRateLimitSnapshot candidate =
            parseCodexRateLimitFixtureFile(it->path().string());
        if (!candidate.success) {
            ++failedSnapshotCount;
            lastFailureError = candidate.error;
            lastFailurePath = it->path().filename().string();
            continue;
        }

        if (shouldReplaceCodexSnapshot(candidate, latestSnapshot)) {
            latestSnapshot = std::move(candidate);
        }
    }

    if (!latestSnapshot.success) {
        if (rolloutFileCount == 0) {
            latestSnapshot.error = "No Codex rollout files were found in the directory";
        } else if (failedSnapshotCount > 0) {
            latestSnapshot.error =
                "Codex rollout files were found, but none contained a supported rate-limit snapshot";
            if (!lastFailurePath.empty() && !lastFailureError.empty()) {
                latestSnapshot.error +=
                    " (last failed file: " + lastFailurePath + "; error: " + lastFailureError + ")";
            } else if (!lastFailureError.empty()) {
                latestSnapshot.error += " (last parse error: " + lastFailureError + ")";
            }
        } else {
            latestSnapshot.error =
                "No supported Codex rollout rate-limit snapshot was found in the directory";
        }
    }

    return latestSnapshot;
}

std::vector<CodexTokenUsageSnapshot> OpenAIAdapter::parseCodexTokenUsageFromRolloutFile(
    const std::string& filePath) {
    std::vector<CodexTokenUsageSnapshot> snapshots;

    std::ifstream in(filePath);
    if (!in.is_open()) {
        return snapshots;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }

        JsonValue root;
        if (!JsonParser::parse(line, root)) {
            continue;
        }

        CodexTokenUsageSnapshot snapshot;
        if (!tryExtractCodexTokenUsage(root, snapshot)) {
            continue;
        }

        snapshot.eventTime = tryExtractCodexEventTimestamp(root);
        if (snapshot.eventTime <= 0) {
            continue;
        }
        snapshot.sourcePath = filePath;
        snapshots.push_back(std::move(snapshot));
    }

    return snapshots;
}

std::vector<CodexTokenUsageSnapshot> OpenAIAdapter::collectCodexTokenUsageSnapshots(
    const std::string& directoryPath) {
    std::vector<CodexTokenUsageSnapshot> allSnapshots;

    if (directoryPath.empty()) {
        return allSnapshots;
    }

    std::error_code ec;
    const std::filesystem::path root(directoryPath);
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return allSnapshots;
    }

    // Scan only the last N days of rollout directories to avoid enumerating
    // the entire historical session tree. Codex stores rollouts under
    // sessions/YYYY/MM/DD/rollout-*.jsonl.
    const std::time_t now = std::time(nullptr);
    std::vector<std::filesystem::path> candidateDirs;
    for (int dayOffset = 0; dayOffset < 3; ++dayOffset) {
        const std::time_t dayTime = now - (dayOffset * 86400);
        std::tm utcTime{};
        gmtime_s(&utcTime, &dayTime);
        char year[8]{};
        char month[8]{};
        char day[8]{};
        std::strftime(year, sizeof(year), "%Y", &utcTime);
        std::strftime(month, sizeof(month), "%m", &utcTime);
        std::strftime(day, sizeof(day), "%d", &utcTime);
        candidateDirs.push_back(root / year / month / day);
    }

    constexpr std::size_t kMaxRolloutFilesToScan = 100;
    std::size_t scannedFileCount = 0;

    for (const auto& candidateDir : candidateDirs) {
        if (!std::filesystem::exists(candidateDir, ec) || !std::filesystem::is_directory(candidateDir, ec)) {
            continue;
        }

        std::filesystem::directory_iterator it(
            candidateDir,
            std::filesystem::directory_options::skip_permission_denied,
            ec);
        if (ec) {
            ec.clear();
            continue;
        }

        for (; it != std::filesystem::directory_iterator(); ++it) {
            if (ec) {
                ec.clear();
                continue;
            }

            if (!it->is_regular_file(ec) || ec || !isRolloutFilePath(it->path())) {
                ec.clear();
                continue;
            }

            if (++scannedFileCount > kMaxRolloutFilesToScan) {
                break;
            }

            std::vector<CodexTokenUsageSnapshot> fileSnapshots =
                parseCodexTokenUsageFromRolloutFile(it->path().string());
            allSnapshots.insert(
                allSnapshots.end(),
                std::make_move_iterator(fileSnapshots.begin()),
                std::make_move_iterator(fileSnapshots.end()));
        }

        if (scannedFileCount > kMaxRolloutFilesToScan) {
            break;
        }
    }

    std::sort(
        allSnapshots.begin(),
        allSnapshots.end(),
        [](const CodexTokenUsageSnapshot& a, const CodexTokenUsageSnapshot& b) {
            return a.eventTime < b.eventTime;
        });

    constexpr std::size_t kMaxSnapshotsToRetain = 1000;
    if (allSnapshots.size() > kMaxSnapshotsToRetain) {
        allSnapshots.erase(
            allSnapshots.begin(),
            allSnapshots.begin() + (allSnapshots.size() - kMaxSnapshotsToRetain));
    }

    return allSnapshots;
}

std::string OpenAIAdapter::resolveCodexRolloutDirectory() {
    return resolveCodexRolloutDirectoryImpl();
}

} // namespace JiStats
