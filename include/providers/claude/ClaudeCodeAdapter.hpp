#pragma once

#include "providers/IProviderAdapter.hpp"
#include <memory>
#include <string>

namespace JiStats {

class ClaudeCodeAdapter : public IProviderAdapter {
public:
    ClaudeCodeAdapter();
    ~ClaudeCodeAdapter() override;

    // IProviderAdapter implementation
    UsageResponse fetchUsage(std::time_t from, std::time_t to) override;
    QuotaResponse fetchQuota() override;
    bool validateCredentials() override;
    std::vector<Capability> getCapabilities() override;
    std::string getProviderKey() const override;
    std::string getDisplayName() const override;
    bool isEnabled() const override;
    void setEnabled(bool enabled) override;

    // Test helpers
    void setMockStatsCachePath(const std::string& path);
    void setMockSettingsPath(const std::string& path);

#ifdef JISTATS_ENABLE_TEST_HOOKS
    std::string getLastStatsCachePathForTesting() const;
    std::string getLastErrorForTesting() const;
#endif

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace JiStats
