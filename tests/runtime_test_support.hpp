#pragma once

#include <algorithm>
#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gui/DashboardPresenter.hpp"
#include "providers/IProviderAdapter.hpp"
#include "providers/ProviderManager.hpp"
#include "taskbar/TaskbarPresenter.hpp"

namespace JiStats::TestSupport {

constexpr std::time_t kDefaultQuotaWindowSeconds = 3600;
constexpr std::time_t kDefaultUsageWindowSeconds = 86400;
constexpr std::time_t kDefaultStaleQuotaAgeSeconds = 2 * 3600;

inline QuotaResponse makeQuotaResponse(std::time_t snapshotTime,
                                       long long used,
                                       long long limit,
                                       std::string windowType = "daily",
                                       std::string providerSurface = "",
                                       std::time_t windowSeconds = kDefaultQuotaWindowSeconds) {
    QuotaResponse quota;
    quota.success = true;
    quota.snapshotTime = snapshotTime;
    quota.windowStart = snapshotTime - windowSeconds;
    quota.windowEnd = snapshotTime;
    quota.used = used;
    quota.limit = limit;
    quota.remaining = std::max(0LL, limit - used);
    quota.windowType = std::move(windowType);
    quota.providerSurface = std::move(providerSurface);
    return quota;
}

inline QuotaResponse makeFailedQuotaResponse(const std::string& error) {
    QuotaResponse quota;
    quota.success = false;
    quota.error = error;
    return quota;
}

inline UsageResponse makeUsageResponse(std::time_t now,
                                       std::time_t windowSeconds = kDefaultUsageWindowSeconds) {
    UsageResponse usage;
    usage.success = true;
    usage.windowStart = now - windowSeconds;
    usage.windowEnd = now;
    return usage;
}

class RuntimeMockProviderAdapter : public IProviderAdapter {
public:
    RuntimeMockProviderAdapter(std::string providerKey, std::string displayName)
        : providerKey_(std::move(providerKey)),
          displayName_(std::move(displayName)) {}

    void setUsageResponse(const UsageResponse& response) {
        usageResponse_ = response;
    }

    void setQuotaResponse(const QuotaResponse& response) {
        quotaResponse_ = response;
    }

    int getUsageFetchCount() const {
        return usageFetchCount_;
    }

    int getQuotaFetchCount() const {
        return quotaFetchCount_;
    }

    void setDirectQuotaApiConfigured(bool value, const std::string& throttleKey = "") {
        directQuotaApiConfigured_ = value;
        directQuotaApiThrottleKey_ = throttleKey;
    }

    UsageResponse fetchUsage(std::time_t from, std::time_t to) override {
        (void)from;
        (void)to;
        ++usageFetchCount_;
        return usageResponse_;
    }

    QuotaResponse fetchQuota() override {
        ++quotaFetchCount_;
        return quotaResponse_;
    }

    bool validateCredentials() override {
        return true;
    }

    std::vector<Capability> getCapabilities() override {
        return {{"usage_api", "Usage API access", true, ""},
                {"quota_api", "Quota API access", true, ""}};
    }

    std::string getProviderKey() const override {
        return providerKey_;
    }

    std::string getDisplayName() const override {
        return displayName_;
    }

    bool isEnabled() const override {
        return enabled_;
    }

    void setEnabled(bool enabled) override {
        enabled_ = enabled;
    }

    bool hasDirectQuotaApiConfigured() const override {
        return directQuotaApiConfigured_;
    }

    std::string directQuotaApiThrottleKey() const override {
        return directQuotaApiThrottleKey_;
    }

private:
    std::string providerKey_;
    std::string displayName_;
    bool enabled_ = true;
    bool directQuotaApiConfigured_ = false;
    std::string directQuotaApiThrottleKey_;
    UsageResponse usageResponse_;
    QuotaResponse quotaResponse_;
    int usageFetchCount_ = 0;
    int quotaFetchCount_ = 0;
};

inline std::shared_ptr<RuntimeMockProviderAdapter> makeQuotaProvider(
    const std::string& providerKey,
    const std::string& displayName,
    const QuotaResponse& quota,
    std::time_t usageNow = 0) {
    auto provider = std::make_shared<RuntimeMockProviderAdapter>(providerKey, displayName);
    provider->setQuotaResponse(quota);
    if (usageNow != 0) {
        provider->setUsageResponse(makeUsageResponse(usageNow));
    }
    return provider;
}

inline std::shared_ptr<RuntimeMockProviderAdapter> makeCurrentQuotaProvider(
    const std::string& providerKey,
    const std::string& displayName,
    long long used,
    long long limit,
    std::time_t now,
    const std::string& windowType = "daily",
    const std::string& providerSurface = "") {
    return makeQuotaProvider(
        providerKey,
        displayName,
        makeQuotaResponse(now, used, limit, windowType, providerSurface),
        now);
}

inline std::shared_ptr<RuntimeMockProviderAdapter> makeStaleQuotaProvider(
    const std::string& providerKey,
    const std::string& displayName,
    long long used,
    long long limit,
    std::time_t now,
    const std::string& windowType = "daily",
    const std::string& providerSurface = "",
    std::time_t staleAgeSeconds = kDefaultStaleQuotaAgeSeconds) {
    return makeQuotaProvider(
        providerKey,
        displayName,
        makeQuotaResponse(now - staleAgeSeconds, used, limit, windowType, providerSurface),
        now);
}

inline std::shared_ptr<RuntimeMockProviderAdapter> makeOfflineQuotaProvider(
    const std::string& providerKey,
    const std::string& displayName,
    const std::string& error = "network unreachable") {
    return makeQuotaProvider(providerKey, displayName, makeFailedQuotaResponse(error));
}

inline std::shared_ptr<ProviderManager> makeProviderManager(
    const std::vector<std::shared_ptr<RuntimeMockProviderAdapter>>& providers) {
    auto providerManager = std::make_shared<ProviderManager>();
    for (const auto& provider : providers) {
        providerManager->addProvider(provider);
    }
    return providerManager;
}

inline const TaskbarMetrics* findTaskbarMetric(const std::vector<TaskbarMetrics>& metrics,
                                               const std::string& providerKey) {
    const auto it = std::find_if(
        metrics.begin(),
        metrics.end(),
        [&providerKey](const TaskbarMetrics& metric) {
            return metric.providerKey == providerKey;
        });
    return it == metrics.end() ? nullptr : &(*it);
}

inline DashboardPreferences makeOverlayPreferences(bool enabled) {
    DashboardPreferences preferences;
    preferences.taskbarOverlayEnabled = enabled;
    return preferences;
}

inline DashboardPreferences makeOverlayEnabledPreferences() {
    return makeOverlayPreferences(true);
}

inline DashboardPreferences makeOverlayDisabledPreferences() {
    return makeOverlayPreferences(false);
}

inline TaskbarMetrics makeQuotaTaskbarMetric(const std::string& providerKey,
                                             const std::string& displayName,
                                             long long used,
                                             long long limit,
                                             std::time_t lastUpdated = 0,
                                             const std::string& confidenceTier = "high") {
    TaskbarMetrics metric;
    metric.providerKey = providerKey;
    metric.providerDisplayName = displayName;
    metric.used = used;
    metric.limit = limit;
    metric.confidenceTier = confidenceTier;
    metric.lastUpdated = lastUpdated;
    return metric;
}

inline TaskbarMetrics makeRuntimeOnlyTaskbarMetric(RuntimeState runtimeState,
                                                   std::time_t lastUpdated) {
    TaskbarMetrics metric;
    metric.providerKey = "runtime";
    metric.providerDisplayName = "Runtime";
    metric.kind = TaskbarMetricKind::PersistedActivity;
    metric.lastUpdated = lastUpdated;
    metric.summaryLabel = "Runtime";
    metric.primaryMetric = runtimeStateCompactLabel(runtimeState);
    metric.secondaryMetric = runtimeStateLabel(runtimeState);
    metric.statusIcon = runtimeStateIndicator(runtimeState);
    return metric;
}

inline std::vector<TaskbarMetrics> makeCompactMultiProviderTaskbarMetrics(
    std::time_t lastUpdated) {
    return {
        makeQuotaTaskbarMetric("openai", "OpenAI", 45, 90, lastUpdated, "high"),
        makeQuotaTaskbarMetric("minimax", "MiniMax", 42000, 100000, lastUpdated, "medium")
    };
}

} // namespace JiStats::TestSupport
