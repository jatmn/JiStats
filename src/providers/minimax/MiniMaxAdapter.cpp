#include "providers/minimax/MiniMaxAdapter.hpp"
#include "core/HttpClient.hpp"
#include "core/JsonParser.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <tuple>

namespace JiStats {

class MiniMaxAdapter::Impl {
public:
    std::string apiKey;
    std::string groupId;
    bool enabled = true;
    MiniMaxSubscriptionCredentialProfile subscriptionProfile;
    std::shared_ptr<WindowsCredentialStore> credentialStore = std::make_shared<WindowsCredentialStore>();
    std::mutex credentialMutex;
    std::uint64_t validationRevision = 0;
    std::string lastResolvedEndpoint;
    
    // HTTP client for API calls
    HttpClient httpClient;
    
    // Mock responses for testing
    std::string mockQuotaResponse;
    std::string mockUsageResponse;

#ifdef JISTATS_ENABLE_TEST_HOOKS
    ValidationTransport mockValidationTransport;
#endif
    
    // MiniMax API base URL
    static constexpr const char* MINIMAX_API_BASE = "https://api.minimax.io";
    static constexpr const char* CODING_PLAN_ENDPOINT = "/v1/api/openplatform/coding_plan/remains";
    static constexpr const char* TOKEN_PLAN_ENDPOINT = "/v1/token_plan/remains";
    static constexpr const char* MODELS_ENDPOINT = "/v1/models";
    
    // Parse timestamp from MiniMax format (milliseconds since epoch)
    std::time_t parseTimestamp(long long ms) const {
        return static_cast<std::time_t>(ms / 1000);
    }
    
    // Format time as ISO string
    std::string formatTime(std::time_t time) const {
        std::tm tm;
        gmtime_s(&tm, &time);
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        return oss.str();
    }

    bool tryGetRequiredNumber(const JsonValue& object, const char* fieldName,
        long long& value, std::string& error) const {
        const JsonValue* member = object.getMember(fieldName);
        if (!member) {
            error = "Missing required field: ";
            error += fieldName;
            return false;
        }
        if (!member->isNumber()) {
            error = "Invalid numeric field: ";
            error += fieldName;
            return false;
        }

        value = JsonParser::getLongLong(*member);
        return true;
    }

    bool parseQuotaPayload(const std::string& body, bool tokenPlan, QuotaResponse& response) const {
        response.rawResponse = body;
        response.success = false;

        JsonValue json;
        if (!JsonParser::parse(body, json)) {
            response.error = "Failed to parse JSON response";
            return false;
        }

        const JsonValue* modelRemains = json.getMember("model_remains");
        if (!modelRemains || !modelRemains->isArray() || modelRemains->getArray().empty()) {
            response.error = "No model_remains in response";
            return false;
        }

        struct Interval {
            long long startMs = 0;
            long long endMs = 0;
            long long remaining = 0;
            long long limit = 0;
        };

        const auto readWindow = [this](const JsonValue& row, Interval& interval, std::string& error) {
            if (!row.isObject()) {
                error = "Invalid model_remains entry";
                return false;
            }
            return tryGetRequiredNumber(row, "start_time", interval.startMs, error) &&
                   tryGetRequiredNumber(row, "end_time", interval.endMs, error);
        };
        const auto readCounts = [this](const JsonValue& row, Interval& interval, std::string& error) {
            return tryGetRequiredNumber(row, "current_interval_usage_count", interval.remaining, error) &&
                   tryGetRequiredNumber(row, "current_interval_total_count", interval.limit, error);
        };

        const auto& rows = modelRemains->getArray();
        Interval selected;
        if (rows.size() == 1 && !rows.front().getMember("model_name")) {
            // Legacy single-row responses omit the model identity.
            if (!readWindow(rows.front(), selected, response.error) ||
                !readCounts(rows.front(), selected, response.error)) {
                return false;
            }
        } else {
            std::optional<Interval> latest;
            bool selectedCountQuota = false;
            bool conflictingBalance = false;
            std::string invalidInterval;
            constexpr auto hourSeconds = 60 * 60;
            for (const auto& row : rows) {
                const auto* modelName = row.getMember("model_name");
                if (!modelName || !modelName->isString()) {
                    continue;
                }
                const bool countQuota = modelName->getString() == "MiniMax-M*";
                if (!countQuota && !(tokenPlan && modelName->getString() == "general")) {
                    continue;
                }

                Interval interval;
                if (!readWindow(row, interval, invalidInterval) || interval.startMs <= 0 ||
                    interval.endMs <= interval.startMs) {
                    continue;
                }
                const auto span = parseTimestamp(interval.endMs) - parseTimestamp(interval.startMs);
                if (span <= 0) {
                    continue;
                }
                // Only the shared five-hour row establishes a request balance. Token Plan general
                // rows establish a reset window even when the meaning of their counts is unknown.
                if (countQuota && (span <= 4 * hourSeconds || span > 5 * hourSeconds ||
                                   !readCounts(row, interval, invalidInterval) || interval.limit <= 0)) {
                    continue;
                }

                const auto window = std::tie(countQuota, interval.endMs, interval.startMs);
                if (!latest || window > std::tie(selectedCountQuota, latest->endMs, latest->startMs)) {
                    latest = interval;
                    selectedCountQuota = countQuota;
                    conflictingBalance = false;
                } else if (window == std::tie(selectedCountQuota, latest->endMs, latest->startMs) &&
                           (interval.limit != latest->limit || interval.remaining != latest->remaining)) {
                    conflictingBalance = true;
                }
            }
            if (!latest) {
                response.error = rows.size() == 1 && !invalidInterval.empty()
                                     ? invalidInterval
                                     : "No supported MiniMax coding quota in response";
                return false;
            }
            if (conflictingBalance) {
                response.error = "Conflicting MiniMax coding quota balances";
                return false;
            }
            selected = *latest;
        }

        response.windowStart = parseTimestamp(selected.startMs);
        response.windowEnd = parseTimestamp(selected.endMs);
        response.limit = std::max<long long>(0, selected.limit);
        response.remaining = std::clamp(selected.remaining, 0LL, response.limit);
        response.used = response.limit - response.remaining;
        response.windowType = "interval";
        response.error.clear();
        response.success = true;
        return true;
    }

    static std::string trimAndLower(const std::string& value) {
        std::string normalized = value;
        normalized.erase(
            normalized.begin(),
            std::find_if(
                normalized.begin(),
                normalized.end(),
                [](unsigned char ch) { return !std::isspace(ch); }));
        normalized.erase(
            std::find_if(
                normalized.rbegin(),
                normalized.rend(),
                [](unsigned char ch) { return !std::isspace(ch); })
                .base(),
            normalized.end());
        std::transform(
            normalized.begin(),
            normalized.end(),
            normalized.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return normalized;
    }

    static std::string canonicalPlanKey(const std::string& planKey) {
        const std::string normalized = trimAndLower(planKey);
        if (normalized == "coding" || normalized == "coding_plan") {
            return "coding_plan";
        }
        if (normalized == "token" || normalized == "token_plan") {
            return "token_plan";
        }
        return normalized;
    }

    static std::string canonicalEndpointFamily(const std::string& endpointFamily,
        const std::string& planKey) {
        const std::string normalizedEndpoint = trimAndLower(endpointFamily);
        if (normalizedEndpoint == "coding_plan_remains" ||
            normalizedEndpoint == "coding_plan" ||
            normalizedEndpoint == "coding") {
            return "coding_plan_remains";
        }
        if (normalizedEndpoint == "token_plan_remains" ||
            normalizedEndpoint == "token_plan" ||
            normalizedEndpoint == "token") {
            return "token_plan_remains";
        }

        const std::string normalizedPlan = canonicalPlanKey(planKey);
        if (normalizedPlan == "coding_plan") {
            return "coding_plan_remains";
        }
        if (normalizedPlan == "token_plan") {
            return "token_plan_remains";
        }

        return normalizedEndpoint;
    }

    static std::string canonicalAccountKey(const std::string& accountKey) {
        return accountKey.empty() ? "default" : accountKey;
    }

    static std::string buildSecretRef(const MiniMaxSubscriptionCredentialProfile& profile) {
        return "JiStats/minimax/subscription/" +
            canonicalPlanKey(profile.planKey) + "/" +
            canonicalAccountKey(profile.accountKey);
    }

    static bool validateSubscriptionProfile(
        const MiniMaxSubscriptionCredentialProfile& input,
        std::string& error) {
        if (!input.providerKey.empty() && input.providerKey != "minimax") {
            error = "MiniMax subscription profile providerKey must be minimax";
            return false;
        }

        if (input.accountKey.empty()) {
            error = "MiniMax subscription profile accountKey is required";
            return false;
        }

        if (!input.scopeType.empty() && trimAndLower(input.scopeType) != "subscription") {
            error = "MiniMax subscription profile scopeType must be subscription";
            return false;
        }

        if (canonicalPlanKey(input.planKey) != "coding_plan" &&
            canonicalPlanKey(input.planKey) != "token_plan") {
            error = "MiniMax subscription profile planKey must be coding_plan or token_plan";
            return false;
        }

        if (!input.credentialType.empty() && input.credentialType != "subscription_key") {
            error = "MiniMax subscription profile credentialType must be subscription_key";
            return false;
        }

        const std::string endpointFamily =
            canonicalEndpointFamily(input.endpointFamily, input.planKey);
        if (endpointFamily != "coding_plan_remains" &&
            endpointFamily != "token_plan_remains") {
            error = "MiniMax subscription profile endpointFamily must resolve to coding_plan_remains or token_plan_remains";
            return false;
        }

        error.clear();
        return true;
    }

    static MiniMaxSubscriptionCredentialProfile normalizeProfile(
        const MiniMaxSubscriptionCredentialProfile& input) {
        MiniMaxSubscriptionCredentialProfile normalized = input;
        normalized.providerKey = "minimax";
        normalized.scopeType = "subscription";
        normalized.planKey = canonicalPlanKey(input.planKey);
        normalized.credentialType = "subscription_key";
        normalized.endpointFamily = canonicalEndpointFamily(input.endpointFamily, input.planKey);
        if (normalized.scopeIdentifier.empty()) {
            normalized.scopeIdentifier = canonicalAccountKey(input.accountKey);
        }
        if (normalized.planDisplayName.empty()) {
            normalized.planDisplayName =
                normalized.planKey == "coding_plan" ? "MiniMax Coding Plan" : "MiniMax Token Plan";
        }
        if (normalized.profileName.empty()) {
            normalized.profileName = normalized.planDisplayName;
        }
        if (normalized.accountDisplayName.empty()) {
            normalized.accountDisplayName = normalized.planDisplayName + " Subscription";
        }
        if (normalized.secretRef.empty()) {
            normalized.secretRef = buildSecretRef(normalized);
        }
        if (normalized.validationState.empty()) {
            normalized.validationState = "unknown";
        }
        return normalized;
    }

    static std::string endpointPathForFamily(const std::string& endpointFamily) {
        if (endpointFamily == "coding_plan_remains") {
            return CODING_PLAN_ENDPOINT;
        }
        if (endpointFamily == "token_plan_remains") {
            return TOKEN_PLAN_ENDPOINT;
        }
        return std::string();
    }

    static std::string providerSurfaceForFamily(const std::string& endpointFamily) {
        if (endpointFamily == "coding_plan_remains") {
            return "minimax_subscription_coding_plan";
        }
        if (endpointFamily == "token_plan_remains") {
            return "minimax_subscription_token_plan";
        }
        return "";
    }

    static bool hasSubscriptionProfile(const MiniMaxSubscriptionCredentialProfile& profile) {
        return !profile.secretRef.empty() || !profile.planKey.empty() || !profile.endpointFamily.empty();
    }

    std::optional<std::string> resolveBearerToken(const MiniMaxSubscriptionCredentialProfile& profile,
                                                  std::string& error) {
        if (!hasSubscriptionProfile(profile)) {
            if (!apiKey.empty()) {
                error.clear();
                return apiKey;
            }
            error = "No MiniMax credential configured";
            return std::nullopt;
        }

        if (!credentialStore) {
            error = "Windows credential store is not configured";
            return std::nullopt;
        }

        std::optional<std::string> secret = credentialStore->readGenericCredential(profile.secretRef);
        if (!secret.has_value()) {
            error = credentialStore->getLastError();
            return std::nullopt;
        }

        error.clear();
        return secret;
    }

    struct ValidationRequest {
        MiniMaxSubscriptionCredentialProfile profile;
        std::optional<std::string> bearerToken;
        std::string tokenError;
        std::string endpoint;
        std::string groupId;
#ifdef JISTATS_ENABLE_TEST_HOOKS
        ValidationTransport mockTransport;
#endif
    };

    ValidationRequest prepareValidation(const MiniMaxSubscriptionCredentialProfile& profile) {
        ValidationRequest request;
        request.profile = profile;
        request.bearerToken = resolveBearerToken(profile, request.tokenError);
        request.endpoint = endpointPathForFamily(profile.endpointFamily);
        if (request.endpoint.empty()) {
            request.endpoint = MODELS_ENDPOINT;
        }
        request.groupId = groupId;
#ifdef JISTATS_ENABLE_TEST_HOOKS
        request.mockTransport = mockValidationTransport;
#endif
        return request;
    }

    static std::string classifyValidationError(
        bool subscriptionProfileActive,
        const HttpResponse& resp) {
        if (resp.statusCode == 0) {
            if (resp.error.empty()) {
                return "Could not reach MiniMax to validate this credential";
            }
            return "Could not reach MiniMax to validate this credential: " + resp.error;
        }

        if (resp.statusCode == 404 && subscriptionProfileActive) {
            return "This credential does not match the selected MiniMax subscription surface";
        }

        if (resp.statusCode == 401 || resp.statusCode == 403) {
            return subscriptionProfileActive
                ? "MiniMax subscription key rejected"
                : "MiniMax credential rejected";
        }

        if (resp.statusCode >= 500) {
            return "MiniMax validation endpoint is temporarily unavailable";
        }

        if (!resp.error.empty()) {
            return resp.error;
        }

        return "MiniMax validation failed with HTTP " + std::to_string(resp.statusCode);
    }

    static std::string classifyValidationState(const HttpResponse& resp) {
        if (resp.statusCode == 0 || resp.statusCode >= 500) {
            return "unreachable";
        }
        return "invalid";
    }

    std::string resolveQuotaEndpoint() {
        const std::string endpoint = endpointPathForFamily(subscriptionProfile.endpointFamily);
        if (!endpoint.empty()) {
            lastResolvedEndpoint = endpoint;
            return endpoint;
        }

        lastResolvedEndpoint = CODING_PLAN_ENDPOINT;
        return CODING_PLAN_ENDPOINT;
    }

    // Fetch quota from a specific endpoint
    QuotaResponse fetchQuotaFromEndpoint(const std::string& endpoint, const std::string& bearerToken,
                                         const std::string& requestGroupId) {
        QuotaResponse response;
        response.snapshotTime = std::time(nullptr);
        
        if (bearerToken.empty()) {
            response.success = false;
            response.error = "No API key configured";
            return response;
        }
        
        // Build full URL
        std::string url = MINIMAX_API_BASE;
        url += endpoint;
        
        // Configure HTTP client
        httpClient.setApiKey(bearerToken);
        if (!requestGroupId.empty()) {
            httpClient.setGroupId(requestGroupId);
        }

        // Make request
        HttpResponse httpResp = httpClient.get(url);
        
        if (!httpResp.success) {
            response.success = false;
            response.error = "HTTP " + std::to_string(httpResp.statusCode) + ": " + httpResp.error;
            response.rawResponse = httpResp.body;
            return response;
        }

        parseQuotaPayload(httpResp.body, endpoint == TOKEN_PLAN_ENDPOINT, response);
        return response;
    }

    static bool validate(ValidationRequest& request) {
        const bool subscription = hasSubscriptionProfile(request.profile);
        const auto complete = [&](std::string state, std::string error = {}) {
            if (subscription) {
                request.profile.lastValidatedAt = std::time(nullptr);
                request.profile.validationState = std::move(state);
                request.profile.lastValidationError = std::move(error);
            }
        };
        if (!request.bearerToken.has_value()) {
            if (subscription && request.tokenError.rfind("Credential not found:", 0) == 0) {
                request.tokenError = "Stored MiniMax credential is missing from Windows Credential Manager";
            }
            complete("invalid", std::move(request.tokenError));
            return false;
        }

        const std::string url = std::string(MINIMAX_API_BASE) + request.endpoint;
        HttpResponse response;
#ifdef JISTATS_ENABLE_TEST_HOOKS
        if (request.mockTransport) {
            response = request.mockTransport(url, *request.bearerToken);
        } else {
#endif
            HttpClient client;
            client.setApiKey(*request.bearerToken);
            if (!request.groupId.empty()) {
                client.setGroupId(request.groupId);
            }
            response = client.get(url);
#ifdef JISTATS_ENABLE_TEST_HOOKS
        }
#endif
        if (response.success && request.endpoint != MODELS_ENDPOINT) {
            JsonValue payload;
            const auto* baseResponse =
                JsonParser::parse(response.body, payload) ? payload.getMember("base_resp") : nullptr;
            const auto* statusCode = baseResponse ? baseResponse->getMember("status_code") : nullptr;
            if (!statusCode || !statusCode->isNumber()) {
                complete("unreachable", "MiniMax subscription validation response has no numeric status code");
                return false;
            }
            if (statusCode->getNumber() != 0) {
                complete("invalid", "MiniMax subscription validation failed with a provider error");
                return false;
            }
        }

        if (response.success) {
            complete("valid");
            return true;
        }
        complete(classifyValidationState(response), classifyValidationError(subscription, response));
        return false;
    }
};

MiniMaxAdapter::MiniMaxAdapter(const std::string& apiKey, const std::string& groupId) 
    : pImpl(std::make_unique<Impl>()) {
    pImpl->apiKey = apiKey;
    pImpl->groupId = groupId;
}

MiniMaxAdapter::~MiniMaxAdapter() = default;

UsageResponse MiniMaxAdapter::fetchUsage(std::time_t from, std::time_t to) {
    UsageResponse response;
    response.success = false;
    response.error = "MiniMax provider-side usage history remains deferred for the current subscription-first baseline";
    response.windowStart = from;
    response.windowEnd = to;

    // The shipped MiniMax v1.0 path uses provider-exact subscription quota mirrors
    // from the remains endpoints. Broader provider-side usage-history import stays
    // deferred until a later scoped validation/implementation pass.
    return response;
}

QuotaResponse MiniMaxAdapter::fetchQuota() {
    QuotaResponse response;
    response.snapshotTime = std::time(nullptr);
    std::unique_lock<std::mutex> lock(pImpl->credentialMutex);
    const auto& profile = pImpl->subscriptionProfile;
    const auto configuredSurface =
        Impl::providerSurfaceForFamily(Impl::canonicalEndpointFamily(profile.endpointFamily, profile.planKey));
    if (Impl::hasSubscriptionProfile(profile)) {
        response.providerSurface = configuredSurface;
    }

    std::string tokenError;
    const auto bearerToken = pImpl->resolveBearerToken(profile, tokenError);
    if (!bearerToken.has_value()) {
        response.error = tokenError;
        return response;
    }

    const bool useLegacyFallback = profile.endpointFamily.empty();
    const auto configuredEndpoint = pImpl->resolveQuotaEndpoint();
    std::string requestGroupId = pImpl->groupId;
    std::string mockQuota = pImpl->mockQuotaResponse;
    lock.unlock();

    if (!mockQuota.empty()) {
        pImpl->parseQuotaPayload(mockQuota, configuredEndpoint == Impl::TOKEN_PLAN_ENDPOINT, response);
        if (response.success) {
            response.providerSurface = configuredSurface;
        }
        return response;
    }

    auto endpoint = configuredEndpoint;
    response = pImpl->fetchQuotaFromEndpoint(endpoint, *bearerToken, requestGroupId);
    if (!response.success && useLegacyFallback) {
        endpoint = Impl::TOKEN_PLAN_ENDPOINT;
        response = pImpl->fetchQuotaFromEndpoint(endpoint, *bearerToken, requestGroupId);
        lock.lock();
        pImpl->lastResolvedEndpoint = endpoint;
    }
    if (response.success) {
        response.providerSurface = endpoint == Impl::TOKEN_PLAN_ENDPOINT
                                       ? Impl::providerSurfaceForFamily("token_plan_remains")
                                       : configuredSurface;
    } else if (!useLegacyFallback) {
        response.providerSurface = configuredSurface;
    }
    return response;
}

bool MiniMaxAdapter::validateCredentials() {
    std::unique_lock<std::mutex> lock(pImpl->credentialMutex);
    auto request = pImpl->prepareValidation(pImpl->subscriptionProfile);
    const auto revision = ++pImpl->validationRevision;
    if (request.bearerToken) {
        pImpl->lastResolvedEndpoint = request.endpoint;
    }
    lock.unlock();

    const bool valid = Impl::validate(request);

    lock.lock();
    if (pImpl->validationRevision == revision) {
        pImpl->subscriptionProfile = std::move(request.profile);
    }
    return valid;
}

std::function<MiniMaxSubscriptionCredentialProfile()>
MiniMaxAdapter::prepareSubscriptionCredentialValidation(const MiniMaxSubscriptionCredentialProfile& profile) {
    std::lock_guard<std::mutex> lock(pImpl->credentialMutex);
    return [request = pImpl->prepareValidation(Impl::normalizeProfile(profile))]() mutable {
        Impl::validate(request);
        return std::move(request.profile);
    };
}

std::vector<Capability> MiniMaxAdapter::getCapabilities() {
    std::vector<Capability> capabilities = {
        {"quota_monitoring", "Monitor subscription quota usage", true, "Uses coding_plan/remains endpoint"},
        {"usage_tracking", "Track API usage", false, "Aggregate PAYG history not implemented; per-request usage validated via chat completions"},
        {"subscription_plans", "Support subscription plans", true, "Coding and Token plans supported"},
        {"real_time_updates", "Real-time quota updates", true, "Quota updates available"}
    };
    return capabilities;
}

std::string MiniMaxAdapter::getProviderKey() const {
    return "minimax";
}

std::string MiniMaxAdapter::getDisplayName() const {
    return "MiniMax";
}

bool MiniMaxAdapter::isEnabled() const {
    return pImpl->enabled;
}

void MiniMaxAdapter::setEnabled(bool enabled) {
    pImpl->enabled = enabled;
}

bool MiniMaxAdapter::hasDirectQuotaApiConfigured() const {
    std::lock_guard<std::mutex> lock(pImpl->credentialMutex);
    if (!pImpl->subscriptionProfile.secretRef.empty()) {
        return true;
    }
    return !pImpl->apiKey.empty();
}

std::string MiniMaxAdapter::directQuotaApiThrottleKey() const {
    std::lock_guard<std::mutex> lock(pImpl->credentialMutex);
    if (pImpl->subscriptionProfile.secretRef.empty() && pImpl->apiKey.empty()) {
        return {};
    }

    const std::string endpointFamily =
        Impl::canonicalEndpointFamily(
            pImpl->subscriptionProfile.endpointFamily,
            pImpl->subscriptionProfile.planKey);
    if (!endpointFamily.empty()) {
        return "minimax:" + endpointFamily;
    }

    return "minimax:legacy_quota_api";
}

void MiniMaxAdapter::setApiKey(const std::string& apiKey) {
    std::lock_guard<std::mutex> lock(pImpl->credentialMutex);
    ++pImpl->validationRevision;
    pImpl->apiKey = apiKey;
}

void MiniMaxAdapter::setGroupId(const std::string& groupId) {
    std::lock_guard<std::mutex> lock(pImpl->credentialMutex);
    ++pImpl->validationRevision;
    pImpl->groupId = groupId;
}

std::string MiniMaxAdapter::getApiKey() const {
    std::lock_guard<std::mutex> lock(pImpl->credentialMutex);
    return pImpl->apiKey;
}

std::string MiniMaxAdapter::getGroupId() const {
    std::lock_guard<std::mutex> lock(pImpl->credentialMutex);
    return pImpl->groupId;
}

void MiniMaxAdapter::setSubscriptionCredentialProfile(
    const MiniMaxSubscriptionCredentialProfile& profile) {
    std::string error;
    if (!validateSubscriptionCredentialProfile(profile, error)) {
        return;
    }

    std::lock_guard<std::mutex> lock(pImpl->credentialMutex);
    ++pImpl->validationRevision;
    pImpl->subscriptionProfile = MiniMaxAdapter::Impl::normalizeProfile(profile);
}

MiniMaxSubscriptionCredentialProfile MiniMaxAdapter::getSubscriptionCredentialProfile() const {
    std::lock_guard<std::mutex> lock(pImpl->credentialMutex);
    return pImpl->subscriptionProfile;
}

bool MiniMaxAdapter::storeSubscriptionCredential(
    const MiniMaxSubscriptionCredentialProfile& profile,
    const std::string& secret) {
    std::lock_guard<std::mutex> lock(pImpl->credentialMutex);
    ++pImpl->validationRevision;
    std::string error;
    if (!validateSubscriptionCredentialProfile(profile, error)) {
        pImpl->subscriptionProfile = MiniMaxSubscriptionCredentialProfile{};
        pImpl->subscriptionProfile.lastValidationError = error;
        pImpl->subscriptionProfile.validationState = "invalid";
        return false;
    }

    if (secret.empty()) {
        pImpl->subscriptionProfile.lastValidationError = "MiniMax subscription secret is required";
        pImpl->subscriptionProfile.validationState = "invalid";
        return false;
    }

    MiniMaxSubscriptionCredentialProfile normalized =
        MiniMaxAdapter::Impl::normalizeProfile(profile);

    if (!pImpl->credentialStore) {
        normalized.validationState = "invalid";
        normalized.lastValidationError = "Windows credential store is not configured";
        pImpl->subscriptionProfile = normalized;
        return false;
    }

    WindowsCredentialWriteRequest request;
    request.targetName = normalized.secretRef;
    request.secret = secret;
    request.userName = normalized.profileName;
    request.comment = "JiStats MiniMax subscription credential";

    if (!pImpl->credentialStore->writeGenericCredential(request)) {
        normalized.validationState = "invalid";
        normalized.lastValidationError = pImpl->credentialStore->getLastError();
        pImpl->subscriptionProfile = normalized;
        return false;
    }

    normalized.validationState = "pending";
    normalized.lastValidatedAt = 0;
    normalized.lastValidationError.clear();
    pImpl->subscriptionProfile = normalized;
    pImpl->apiKey.clear();
    return true;
}

std::optional<MiniMaxSubscriptionPollingConfig> MiniMaxAdapter::resolveSubscriptionPollingConfig(
    std::string& error) {
    std::lock_guard<std::mutex> lock(pImpl->credentialMutex);
    if (pImpl->subscriptionProfile.secretRef.empty()) {
        error = "No MiniMax credential configured";
        return std::nullopt;
    }

    const std::optional<std::string> bearerToken = pImpl->resolveBearerToken(pImpl->subscriptionProfile, error);
    if (!bearerToken.has_value()) {
        return std::nullopt;
    }

    MiniMaxSubscriptionPollingConfig config;
    config.profile = pImpl->subscriptionProfile;
    config.endpointPath = pImpl->resolveQuotaEndpoint();
    config.bearerToken = *bearerToken;
    error.clear();
    return config;
}

void MiniMaxAdapter::setCredentialStore(std::shared_ptr<WindowsCredentialStore> credentialStore) {
    std::lock_guard<std::mutex> lock(pImpl->credentialMutex);
    if (pImpl->credentialStore != credentialStore) {
        ++pImpl->validationRevision;
        pImpl->credentialStore = std::move(credentialStore);
    }
}

bool MiniMaxAdapter::validateSubscriptionCredentialProfile(
    const MiniMaxSubscriptionCredentialProfile& profile,
    std::string& error) {
    return MiniMaxAdapter::Impl::validateSubscriptionProfile(profile, error);
}

std::string MiniMaxAdapter::buildSecretRef(const MiniMaxSubscriptionCredentialProfile& profile) {
    return MiniMaxAdapter::Impl::buildSecretRef(MiniMaxAdapter::Impl::normalizeProfile(profile));
}

void MiniMaxAdapter::setMockQuotaResponse(const std::string& jsonResponse) {
    std::lock_guard<std::mutex> lock(pImpl->credentialMutex);
    pImpl->mockQuotaResponse = jsonResponse;
}

void MiniMaxAdapter::setMockUsageResponse(const std::string& jsonResponse) {
    pImpl->mockUsageResponse = jsonResponse;
}

#ifdef JISTATS_ENABLE_TEST_HOOKS
void MiniMaxAdapter::setMockValidationTransport(ValidationTransport transport) {
    std::lock_guard<std::mutex> lock(pImpl->credentialMutex);
    pImpl->mockValidationTransport = std::move(transport);
}

void MiniMaxAdapter::setMockValidationResponse(bool success, int statusCode,
    const std::string& body, const std::string& error) {
    setMockValidationTransport([response = HttpResponse{success, statusCode, body, error}](
                                   const std::string&, const std::string&) { return response; });
}

std::string MiniMaxAdapter::getLastResolvedEndpointForTesting() const {
    std::lock_guard<std::mutex> lock(pImpl->credentialMutex);
    return pImpl->lastResolvedEndpoint;
}
#endif

} // namespace JiStats
