#pragma once

#include <string>
#include <vector>
#include <memory>
#include <ctime>

namespace JiStats {

// Forward declarations
struct UsageResponse;
struct QuotaResponse;
struct Capability;
struct UsageEvent;

// Provider adapter interface
class IProviderAdapter {
public:
    virtual ~IProviderAdapter() = default;
    
    // Fetch usage data for a time range
    virtual UsageResponse fetchUsage(std::time_t from, std::time_t to) = 0;
    
    // Fetch current quota information
    virtual QuotaResponse fetchQuota() = 0;
    
    // Validate credentials
    virtual bool validateCredentials() = 0;
    
    // Get provider capabilities
    virtual std::vector<Capability> getCapabilities() = 0;
    
    // Get provider identifier
    virtual std::string getProviderKey() const = 0;
    
    // Get display name
    virtual std::string getDisplayName() const = 0;
    
    // Check if provider is enabled
    virtual bool isEnabled() const = 0;
    
    // Enable/disable provider
    virtual void setEnabled(bool enabled) = 0;

    // Return true when fetchQuota() may call a direct provider API in the current configuration.
    // Local-only providers should leave this false so the app can refresh local state freely
    // without applying the direct-API polling budget.
    virtual bool hasDirectQuotaApiConfigured() const { return false; }

    // Return a stable key representing the direct quota API surface that fetchQuota() would hit.
    // This lets the app throttle each API surface independently instead of treating an entire
    // provider as one global bucket.
    virtual std::string directQuotaApiThrottleKey() const { return {}; }
};

    // Usage response structure
    struct UsageResponse {
        bool success = false;
        std::string error;
        int64_t windowStart = 0;
        int64_t windowEnd = 0;
        int64_t totalInputTokens = 0;
        int64_t totalOutputTokens = 0;
        int64_t totalCost = 0;
        std::vector<UsageEvent> events;
        std::string rawResponse;
        bool isBackfilled = false;
    };

    // Quota response structure
struct QuotaResponse {
    bool success = false;
    std::string error;
    std::time_t snapshotTime = 0;
    std::time_t windowStart = 0;
    std::time_t windowEnd = 0;
    long long used = 0;
    long long limit = 0;
    long long remaining = 0;
    std::string windowType; // "interval", "daily", "monthly", "rolling"
    std::time_t secondaryWindowStart = 0;
    std::time_t secondaryWindowEnd = 0;
    long long secondaryUsed = 0;
    long long secondaryLimit = 0;
    long long secondaryRemaining = 0;
    std::string secondaryWindowType; // e.g. a second rolling or weekly window
    std::string providerSurface; // normalized provenance label when known
    std::string rawResponse;
};

// Provider capability
struct Capability {
    std::string name;
    std::string description;
    bool supported;
    std::string notes;
};

// Usage event
struct UsageEvent {
    std::time_t eventTime = 0;
    std::string modelKey;
    std::string requestType;
    long long inputTokens = 0;
    long long outputTokens = 0;
    long long totalTokens = 0;
    long long costMinorUnits = 0; // Cost in internal provider-normalized units; OpenAI fixtures use 1 = $0.0001
    std::string sourceType; // "api", "subscription", "imported"
    std::string sourceRunId;
    std::string rawReference;
};

} // namespace JiStats
