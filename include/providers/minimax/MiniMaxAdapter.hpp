#pragma once

#include "providers/IProviderAdapter.hpp"
#include "platform/WindowsCredentialStore.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace JiStats {

struct MiniMaxSubscriptionCredentialProfile {
    std::string providerKey = "minimax";
    std::string accountKey;
    std::string accountDisplayName;
    std::string scopeType = "subscription";
    std::string scopeIdentifier;
    std::string profileName;
    std::string planKey;
    std::string planDisplayName;
    std::string credentialType = "subscription_key";
    std::string secretRef;
    std::string endpointFamily;
    std::string validationState = "unknown";
    std::time_t lastValidatedAt = 0;
    std::string lastValidationError;
};

struct MiniMaxSubscriptionPollingConfig {
    MiniMaxSubscriptionCredentialProfile profile;
    std::string endpointPath;
    std::string bearerToken;
};

class MiniMaxAdapter : public IProviderAdapter {
public:
    MiniMaxAdapter(const std::string& apiKey = "", const std::string& groupId = "");
    ~MiniMaxAdapter() override;
    
    // IProviderAdapter implementation
    UsageResponse fetchUsage(std::time_t from, std::time_t to) override;
    bool supportsUsageHistory() const override {
        return false;
    }
    QuotaResponse fetchQuota() override;
    bool validateCredentials() override;
    std::vector<Capability> getCapabilities() override;
    std::string getProviderKey() const override;
    std::string getDisplayName() const override;
    bool isEnabled() const override;
    void setEnabled(bool enabled) override;
    bool hasDirectQuotaApiConfigured() const override;
    std::string directQuotaApiThrottleKey() const override;
    
    // MiniMax specific methods
    void setApiKey(const std::string& apiKey);
    void setGroupId(const std::string& groupId);
    std::string getApiKey() const;
    std::string getGroupId() const;

    void setSubscriptionCredentialProfile(const MiniMaxSubscriptionCredentialProfile& profile);
    MiniMaxSubscriptionCredentialProfile getSubscriptionCredentialProfile() const;
    bool storeSubscriptionCredential(
        const MiniMaxSubscriptionCredentialProfile& profile,
        const std::string& secret);
    std::function<MiniMaxSubscriptionCredentialProfile()>
    prepareSubscriptionCredentialValidation(const MiniMaxSubscriptionCredentialProfile& profile);
    std::optional<MiniMaxSubscriptionPollingConfig> resolveSubscriptionPollingConfig(
        std::string& error);
    void setCredentialStore(std::shared_ptr<WindowsCredentialStore> credentialStore);

    static bool validateSubscriptionCredentialProfile(
        const MiniMaxSubscriptionCredentialProfile& profile,
        std::string& error);
    static std::string buildSecretRef(const MiniMaxSubscriptionCredentialProfile& profile);
    
    // Test helper - set mock responses
    void setMockQuotaResponse(const std::string& jsonResponse);
    void setMockUsageResponse(const std::string& jsonResponse);

#ifdef JISTATS_ENABLE_TEST_HOOKS
    void setMockValidationResponse(bool success, int statusCode,
        const std::string& body = "", const std::string& error = "");
    std::string getLastResolvedEndpointForTesting() const;
#endif
    
private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace JiStats
