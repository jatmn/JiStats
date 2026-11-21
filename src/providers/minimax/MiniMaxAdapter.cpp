#include "providers/minimax/MiniMaxAdapter.hpp"
#include "core/HttpClient.hpp"
#include "core/JsonParser.hpp"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <optional>

namespace JiStats {

class MiniMaxAdapter::Impl {
public:
    std::string apiKey;
    std::string groupId;
    bool enabled = true;
    MiniMaxSubscriptionCredentialProfile subscriptionProfile;
    std::shared_ptr<WindowsCredentialStore> credentialStore = std::make_shared<WindowsCredentialStore>();
    std::string lastResolvedEndpoint;
    
    // HTTP client for API calls
    HttpClient httpClient;
    
    // Mock responses for testing
    std::string mockQuotaResponse;
    std::string mockUsageResponse;

#ifdef JISTATS_ENABLE_TEST_HOOKS
    std::optional<HttpResponse> mockValidationResponse;
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

    bool parseQuotaPayload(const std::string& body, QuotaResponse& response) const {
        response.rawResponse = body;

        JsonValue json;
        if (!JsonParser::parse(body, json)) {
            response.success = false;
            response.error = "Failed to parse JSON response";
            return false;
        }

        const JsonValue* modelRemains = json.getMember("model_remains");
        if (!modelRemains || !modelRemains->isArray() || modelRemains->getArray().empty()) {
            response.success = false;
            response.error = "No model_remains in response";
            return false;
        }

        const JsonValue& firstModel = modelRemains->getArray()[0];
        if (!firstModel.isObject()) {
            response.success = false;
            response.error = "Invalid model_remains entry";
            return false;
        }

        long long startTimeMs = 0;
        long long endTimeMs = 0;
        long long remainingCount = 0;
        long long limitCount = 0;

        if (!tryGetRequiredNumber(firstModel, "start_time", startTimeMs, response.error) ||
            !tryGetRequiredNumber(firstModel, "end_time", endTimeMs, response.error) ||
            !tryGetRequiredNumber(firstModel, "current_interval_usage_count", remainingCount, response.error) ||
            !tryGetRequiredNumber(firstModel, "current_interval_total_count", limitCount, response.error)) {
            response.success = false;
            return false;
        }

        response.windowStart = parseTimestamp(startTimeMs);
        response.windowEnd = parseTimestamp(endTimeMs);
        response.limit = std::max<long long>(0, limitCount);
        response.remaining = std::clamp(remainingCount, 0LL, response.limit);
        response.used = std::clamp(response.limit - response.remaining, 0LL, response.limit);
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

    bool hasActiveSubscriptionProfile() const {
        return !subscriptionProfile.secretRef.empty() ||
            !subscriptionProfile.planKey.empty() ||
            !subscriptionProfile.endpointFamily.empty();
    }

    void recordValidationResult(
        bool success,
        const std::string& validationState,
        const std::string& error) {
        if (!hasActiveSubscriptionProfile()) {
            return;
        }

        subscriptionProfile.lastValidatedAt = std::time(nullptr);
        subscriptionProfile.validationState = validationState;
        subscriptionProfile.lastValidationError = success ? std::string() : error;
    }

    std::optional<std::string> resolveBearerToken(std::string& error) {
        if (!hasActiveSubscriptionProfile()) {
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

        std::optional<std::string> secret =
            credentialStore->readGenericCredential(subscriptionProfile.secretRef);
        if (!secret.has_value()) {
            error = credentialStore->getLastError();
            return std::nullopt;
        }

        error.clear();
        return secret;
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

    std::string resolveValidationEndpoint() {
        const std::string endpoint = endpointPathForFamily(subscriptionProfile.endpointFamily);
        if (!endpoint.empty()) {
            lastResolvedEndpoint = endpoint;
            return endpoint;
        }

        lastResolvedEndpoint = MODELS_ENDPOINT;
        return MODELS_ENDPOINT;
    }
    
    // Fetch quota from a specific endpoint
    QuotaResponse fetchQuotaFromEndpoint(const std::string& endpoint, const std::string& bearerToken) {
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
        if (!groupId.empty()) {
            httpClient.setGroupId(groupId);
        }
        
        // Make request
        HttpResponse httpResp = httpClient.get(url);
        
        if (!httpResp.success) {
            response.success = false;
            response.error = "HTTP " + std::to_string(httpResp.statusCode) + ": " + httpResp.error;
            response.rawResponse = httpResp.body;
            return response;
        }

        parseQuotaPayload(httpResp.body, response);
        const std::string endpointFamily =
            canonicalEndpointFamily(subscriptionProfile.endpointFamily, subscriptionProfile.planKey);
        if (response.success) {
            response.providerSurface = providerSurfaceForFamily(endpointFamily);
        }
        return response;
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
    
    if (!pImpl->mockQuotaResponse.empty()) {
        std::string tokenError;
        if (!pImpl->resolveBearerToken(tokenError).has_value()) {
            response.success = false;
            response.error = tokenError;
            return response;
        }
        pImpl->lastResolvedEndpoint = pImpl->resolveQuotaEndpoint();
        pImpl->parseQuotaPayload(pImpl->mockQuotaResponse, response);
        if (response.success) {
            response.providerSurface = MiniMaxAdapter::Impl::providerSurfaceForFamily(
                MiniMaxAdapter::Impl::canonicalEndpointFamily(
                    pImpl->subscriptionProfile.endpointFamily,
                    pImpl->subscriptionProfile.planKey));
        }
        return response;
    }

    std::string tokenError;
    const std::optional<std::string> bearerToken = pImpl->resolveBearerToken(tokenError);
    if (!bearerToken.has_value()) {
        response.success = false;
        response.error = tokenError;
        return response;
    }
    
    // Real implementation: fetch from MiniMax API
    const std::string configuredEndpoint = pImpl->resolveQuotaEndpoint();
    response = pImpl->fetchQuotaFromEndpoint(configuredEndpoint, *bearerToken);

    if (!response.success && pImpl->subscriptionProfile.endpointFamily.empty()) {
        response = pImpl->fetchQuotaFromEndpoint(MiniMaxAdapter::Impl::TOKEN_PLAN_ENDPOINT, *bearerToken);
        pImpl->lastResolvedEndpoint = MiniMaxAdapter::Impl::TOKEN_PLAN_ENDPOINT;
    }
    
    return response;
}

bool MiniMaxAdapter::validateCredentials() {
    std::string tokenError;
    const std::optional<std::string> bearerToken = pImpl->resolveBearerToken(tokenError);
    if (!bearerToken.has_value()) {
        if (tokenError.rfind("Credential not found:", 0) == 0 &&
            pImpl->hasActiveSubscriptionProfile()) {
            tokenError = "Stored MiniMax credential is missing from Windows Credential Manager";
        }
        pImpl->recordValidationResult(false, "invalid", tokenError);
        return false;
    }

    const std::string endpoint = pImpl->resolveValidationEndpoint();
    HttpResponse resp;
#ifdef JISTATS_ENABLE_TEST_HOOKS
    if (pImpl->mockValidationResponse.has_value()) {
        resp = *pImpl->mockValidationResponse;
    } else {
#endif
        HttpClient client;
        client.setApiKey(*bearerToken);
        if (!pImpl->groupId.empty()) {
            client.setGroupId(pImpl->groupId);
        }

        resp = client.get(std::string(MiniMaxAdapter::Impl::MINIMAX_API_BASE) + endpoint);
#ifdef JISTATS_ENABLE_TEST_HOOKS
    }
#endif

    if (resp.success) {
        pImpl->recordValidationResult(true, "valid", "");
        return true;
    }

    pImpl->recordValidationResult(
        false,
        MiniMaxAdapter::Impl::classifyValidationState(resp),
        MiniMaxAdapter::Impl::classifyValidationError(
            pImpl->hasActiveSubscriptionProfile(),
            resp));
    return false;
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
    if (!pImpl->subscriptionProfile.secretRef.empty()) {
        return true;
    }
    return !pImpl->apiKey.empty();
}

std::string MiniMaxAdapter::directQuotaApiThrottleKey() const {
    if (!hasDirectQuotaApiConfigured()) {
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
    pImpl->apiKey = apiKey;
}

void MiniMaxAdapter::setGroupId(const std::string& groupId) {
    pImpl->groupId = groupId;
}

std::string MiniMaxAdapter::getApiKey() const {
    return pImpl->apiKey;
}

std::string MiniMaxAdapter::getGroupId() const {
    return pImpl->groupId;
}

void MiniMaxAdapter::setSubscriptionCredentialProfile(
    const MiniMaxSubscriptionCredentialProfile& profile) {
    std::string error;
    if (!validateSubscriptionCredentialProfile(profile, error)) {
        return;
    }

    pImpl->subscriptionProfile = MiniMaxAdapter::Impl::normalizeProfile(profile);
}

MiniMaxSubscriptionCredentialProfile MiniMaxAdapter::getSubscriptionCredentialProfile() const {
    return pImpl->subscriptionProfile;
}

bool MiniMaxAdapter::storeSubscriptionCredential(
    const MiniMaxSubscriptionCredentialProfile& profile,
    const std::string& secret) {
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
    normalized.lastValidationError.clear();
    pImpl->subscriptionProfile = normalized;
    pImpl->apiKey.clear();
    return true;
}

std::optional<MiniMaxSubscriptionPollingConfig> MiniMaxAdapter::resolveSubscriptionPollingConfig(
    std::string& error) {
    if (pImpl->subscriptionProfile.secretRef.empty()) {
        error = "No MiniMax credential configured";
        return std::nullopt;
    }

    const std::optional<std::string> bearerToken = pImpl->resolveBearerToken(error);
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
    pImpl->credentialStore = credentialStore;
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
    pImpl->mockQuotaResponse = jsonResponse;
}

void MiniMaxAdapter::setMockUsageResponse(const std::string& jsonResponse) {
    pImpl->mockUsageResponse = jsonResponse;
}

#ifdef JISTATS_ENABLE_TEST_HOOKS
void MiniMaxAdapter::setMockValidationResponse(bool success, int statusCode,
    const std::string& body, const std::string& error) {
    pImpl->mockValidationResponse = HttpResponse{success, statusCode, body, error};
}

std::string MiniMaxAdapter::getLastResolvedEndpointForTesting() const {
    return pImpl->lastResolvedEndpoint;
}
#endif

} // namespace JiStats
