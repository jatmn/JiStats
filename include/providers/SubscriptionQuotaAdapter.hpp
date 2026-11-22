#pragma once

#include "providers/IProviderAdapter.hpp"
#include <atomic>

namespace JiStats {

enum class SubscriptionProvider { Grok, Cursor };

class SubscriptionQuotaAdapter : public IProviderAdapter {
  public:
    explicit SubscriptionQuotaAdapter(SubscriptionProvider provider);

    UsageResponse fetchUsage(std::time_t from, std::time_t to) override;
    QuotaResponse fetchQuota() override;
    bool validateCredentials() override;
    std::vector<Capability> getCapabilities() override;
    std::string getProviderKey() const override;
    std::string getDisplayName() const override;
    bool isEnabled() const override;
    void setEnabled(bool enabled) override;
    bool hasDirectQuotaApiConfigured() const override;

    static QuotaResponse parseQuotaResponse(SubscriptionProvider provider, const std::string& body,
                                            std::time_t checkedAt);

  private:
    SubscriptionProvider provider_;
    std::atomic<bool> enabled_{false};
};

} // namespace JiStats
