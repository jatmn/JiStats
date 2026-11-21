#pragma once

#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "db/StorageService.hpp"
#include "observation/LocalObservationService.hpp"

namespace JiStats {

class ObservationPersistenceBridge {
public:
    explicit ObservationPersistenceBridge(StorageService& storage);

    bool persist(const ActivityEvent& event, long long* insertedId = nullptr);

    size_t persistPending(
        LocalObservationService& observationService,
        std::vector<long long>* insertedIds = nullptr);
    size_t persistPending(
        LocalObservationService& observationService,
        const std::function<bool(const ActivityEvent&)>& shouldPersist,
        std::vector<long long>* insertedIds = nullptr);

    std::vector<PersistedActivityEvent> getPersistedActivities(
        const std::string& providerKey,
        const std::string& providerSurface,
        std::time_t startedAtFrom,
        std::time_t startedAtTo);

    std::string getLastError() const;

private:
    struct ResolvedBinding {
        int providerId = 0;
        int accountId = 0;
        std::optional<int> planId;
    };

    bool resolveBindingForEvent(const ActivityEvent& event, ResolvedBinding& binding);
    bool findBinding(
        const std::string& providerKey,
        const std::string& providerSurface,
        ResolvedBinding& binding);
    bool ensureProviderId(const std::string& providerKey, int& providerId);
    bool ensureObservationAccount(
        const std::string& providerKey,
        int providerId,
        const std::string& providerSurface,
        int& accountId);
    std::optional<int> resolveActivePlanId(int accountId);
    bool tryGetSingleId(
        const std::string& sql,
        const std::vector<std::string>& params,
        int& id);

    StorageService& storage_;
    std::vector<ActivityEvent> retryBuffer_;
    std::string lastError_;
};

} // namespace JiStats
