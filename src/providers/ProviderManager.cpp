#include "providers/ProviderManager.hpp"
#include <algorithm>

namespace JiStats {

ProviderManager::ProviderManager() = default;

ProviderManager::~ProviderManager() {
    shutdownAll();
}

bool ProviderManager::addProvider(std::shared_ptr<IProviderAdapter> provider) {
    if (!provider) {
        return false;
    }
    
    std::string key = provider->getProviderKey();
    if (key.empty()) {
        return false;
    }
    
    providers[key] = provider;
    return true;
}

std::shared_ptr<IProviderAdapter> ProviderManager::getProvider(const std::string& providerKey) const {
    auto it = providers.find(providerKey);
    if (it != providers.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<std::shared_ptr<IProviderAdapter>> ProviderManager::getAllProviders() const {
    std::vector<std::shared_ptr<IProviderAdapter>> result;
    for (const auto& pair : providers) {
        result.push_back(pair.second);
    }
    return result;
}

std::vector<std::shared_ptr<IProviderAdapter>> ProviderManager::getEnabledProviders() const {
    std::vector<std::shared_ptr<IProviderAdapter>> result;
    for (const auto& pair : providers) {
        if (pair.second->isEnabled()) {
            result.push_back(pair.second);
        }
    }
    return result;
}

bool ProviderManager::removeProvider(const std::string& providerKey) {
    return providers.erase(providerKey) > 0;
}

bool ProviderManager::hasProvider(const std::string& providerKey) const {
    return providers.find(providerKey) != providers.end();
}

size_t ProviderManager::getProviderCount() const {
    return providers.size();
}

bool ProviderManager::initializeAll() {
    bool allSuccess = true;
    for ([[maybe_unused]] auto& pair : providers) {
        // For now, just mark as initialized
        // In future, might call an initialize method on the adapter
    }
    return allSuccess;
}

void ProviderManager::shutdownAll() {
    // Clean up resources if needed
}

std::vector<UsageResponse> ProviderManager::fetchAllUsage(std::time_t from, std::time_t to) {
    std::vector<UsageResponse> responses;
    for (auto& pair : providers) {
        if (pair.second->isEnabled()) {
            responses.push_back(pair.second->fetchUsage(from, to));
        }
    }
    return responses;
}

std::vector<QuotaResponse> ProviderManager::fetchAllQuota() {
    std::vector<QuotaResponse> responses;
    for (auto& pair : providers) {
        if (pair.second->isEnabled()) {
            responses.push_back(pair.second->fetchQuota());
        }
    }
    return responses;
}

} // namespace JiStats
