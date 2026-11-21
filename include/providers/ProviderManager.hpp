#pragma once

#include <string>
#include <vector>
#include <memory>
#include <map>
#include "providers/IProviderAdapter.hpp"

namespace JiStats {

class ProviderManager {
public:
    ProviderManager();
    ~ProviderManager();
    
    // Add a provider adapter
    bool addProvider(std::shared_ptr<IProviderAdapter> provider);
    
    // Get provider by key
    std::shared_ptr<IProviderAdapter> getProvider(const std::string& providerKey) const;
    
    // Get all providers
    std::vector<std::shared_ptr<IProviderAdapter>> getAllProviders() const;
    
    // Get enabled providers
    std::vector<std::shared_ptr<IProviderAdapter>> getEnabledProviders() const;
    
    // Remove provider
    bool removeProvider(const std::string& providerKey);
    
    // Check if provider exists
    bool hasProvider(const std::string& providerKey) const;
    
    // Get provider count
    size_t getProviderCount() const;
    
    // Initialize all providers
    bool initializeAll();
    
    // Shutdown all providers
    void shutdownAll();
    
    // Fetch usage from all enabled providers
    std::vector<UsageResponse> fetchAllUsage(std::time_t from, std::time_t to);
    
    // Fetch quota from all enabled providers
    std::vector<QuotaResponse> fetchAllQuota();
    
private:
    std::map<std::string, std::shared_ptr<IProviderAdapter>> providers;
};

} // namespace JiStats