#include "observation/ObservationPersistenceBridge.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <utility>

namespace JiStats {

namespace {

bool isValidActivityEvent(const ActivityEvent& event, std::string& error) {
    if (event.providerKey.empty()) {
        error = "Activity event is missing providerKey";
        return false;
    }

    if (event.providerSurface.empty()) {
        error = "Activity event is missing providerSurface";
        return false;
    }

    if (event.startedAt <= 0) {
        error = "Activity event must have a valid startedAt";
        return false;
    }

    if (event.endedAt != 0 && event.endedAt < event.startedAt) {
        error = "Activity event endedAt cannot be earlier than startedAt";
        return false;
    }

    if (event.durationSeconds < 0) {
        error = "Activity event durationSeconds cannot be negative";
        return false;
    }

    return true;
}

std::string makeObservationAccountKey(const std::string& providerSurface) {
    return "local-observation:" + providerSurface;
}

std::string makeProviderDisplayName(const std::string& providerKey) {
    if (providerKey == "openai") {
        return "OpenAI";
    }

    if (providerKey == "minimax") {
        return "MiniMax";
    }

    std::string displayName;
    bool capitalizeNext = true;

    for (char ch : providerKey) {
        if (ch == '_' || ch == '-') {
            if (!displayName.empty() && displayName.back() != ' ') {
                displayName.push_back(' ');
            }
            capitalizeNext = true;
            continue;
        }

        const unsigned char value = static_cast<unsigned char>(ch);
        displayName.push_back(
            capitalizeNext ? static_cast<char>(std::toupper(value)) : ch);
        capitalizeNext = false;
    }

    return displayName.empty() ? providerKey : displayName;
}

std::string makeObservationDisplayName(
    const std::string& providerKey,
    const std::string& providerSurface) {
    return makeProviderDisplayName(providerKey) + " Local Observation (" + providerSurface + ")";
}

} // namespace

ObservationPersistenceBridge::ObservationPersistenceBridge(StorageService& storage)
    : storage_(storage) {
}

bool ObservationPersistenceBridge::persist(const ActivityEvent& event, long long* insertedId) {
    if (insertedId != nullptr) {
        *insertedId = 0;
    }

    if (!storage_.isOpen()) {
        lastError_ = "Database not open";
        return false;
    }

    if (!isValidActivityEvent(event, lastError_)) {
        return false;
    }

    ResolvedBinding binding;
    if (!resolveBindingForEvent(event, binding)) {
        return false;
    }

    PersistedActivityEvent persisted;
    persisted.providerId = binding.providerId;
    persisted.accountId = binding.accountId;
    persisted.planId = binding.planId;
    persisted.activity = event;

    if (!storage_.insertActivityEvent(persisted, insertedId)) {
        lastError_ = storage_.getLastError();
        return false;
    }

    lastError_.clear();
    return true;
}

size_t ObservationPersistenceBridge::persistPending(
    LocalObservationService& observationService,
    std::vector<long long>* insertedIds) {
    return persistPending(
        observationService,
        std::function<bool(const ActivityEvent&)>(),
        insertedIds);
}

size_t ObservationPersistenceBridge::persistPending(
    LocalObservationService& observationService,
    const std::function<bool(const ActivityEvent&)>& shouldPersist,
    std::vector<long long>* insertedIds) {
    std::vector<ActivityEvent> pending = std::move(retryBuffer_);
    retryBuffer_.clear();

    std::vector<ActivityEvent> observed = observationService.takePendingActivities();
    pending.insert(
        pending.end(),
        std::make_move_iterator(observed.begin()),
        std::make_move_iterator(observed.end()));

    size_t persistedCount = 0;
    for (size_t i = 0; i < pending.size(); ++i) {
        if (shouldPersist && !shouldPersist(pending[i])) {
            continue;
        }

        long long insertedId = 0;
        if (!persist(pending[i], &insertedId)) {
            retryBuffer_.insert(
                retryBuffer_.end(),
                std::make_move_iterator(pending.begin() + i),
                std::make_move_iterator(pending.end()));
            return persistedCount;
        }

        ++persistedCount;
        if (insertedIds != nullptr) {
            insertedIds->push_back(insertedId);
        }
    }

    lastError_.clear();
    return persistedCount;
}

std::vector<PersistedActivityEvent> ObservationPersistenceBridge::getPersistedActivities(
    const std::string& providerKey,
    const std::string& providerSurface,
    std::time_t startedAtFrom,
    std::time_t startedAtTo) {
    std::vector<PersistedActivityEvent> events;

    if (!storage_.isOpen()) {
        lastError_ = "Database not open";
        return events;
    }

    if (providerKey.empty()) {
        lastError_ = "providerKey is required";
        return events;
    }

    if (providerSurface.empty()) {
        lastError_ = "providerSurface is required";
        return events;
    }

    if (startedAtTo < startedAtFrom) {
        lastError_ = "startedAtTo cannot be earlier than startedAtFrom";
        return events;
    }

    ResolvedBinding binding;
    if (!findBinding(providerKey, providerSurface, binding)) {
        if (lastError_.empty()) {
            lastError_.clear();
        }
        return events;
    }

    events = storage_.getActivityEvents(
        binding.providerId,
        binding.accountId,
        startedAtFrom,
        startedAtTo);

    const std::string storageError = storage_.getLastError();
    if (!storageError.empty()) {
        lastError_ = storageError;
        return {};
    }

    events.erase(
        std::remove_if(
            events.begin(),
            events.end(),
            [&](PersistedActivityEvent& event) {
                return event.activity.providerSurface != providerSurface;
            }),
        events.end());

    for (auto& event : events) {
        event.activity.providerKey = providerKey;
    }

    lastError_.clear();
    return events;
}

std::string ObservationPersistenceBridge::getLastError() const {
    return lastError_;
}

bool ObservationPersistenceBridge::resolveBindingForEvent(
    const ActivityEvent& event,
    ResolvedBinding& binding) {
    if (!ensureProviderId(event.providerKey, binding.providerId)) {
        return false;
    }

    if (!ensureObservationAccount(
            event.providerKey,
            binding.providerId,
            event.providerSurface,
            binding.accountId)) {
        return false;
    }

    binding.planId = resolveActivePlanId(binding.accountId);
    return true;
}

bool ObservationPersistenceBridge::findBinding(
    const std::string& providerKey,
    const std::string& providerSurface,
    ResolvedBinding& binding) {
    if (!tryGetSingleId(
            "SELECT id FROM providers WHERE provider_key = ? LIMIT 1;",
            {providerKey},
            binding.providerId)) {
        return false;
    }

    return tryGetSingleId(
        "SELECT id FROM accounts WHERE provider_id = ? AND account_key = ? LIMIT 1;",
        {std::to_string(binding.providerId), makeObservationAccountKey(providerSurface)},
        binding.accountId);
}

bool ObservationPersistenceBridge::ensureProviderId(const std::string& providerKey, int& providerId) {
    if (tryGetSingleId(
            "SELECT id FROM providers WHERE provider_key = ? LIMIT 1;",
            {providerKey},
            providerId)) {
        return true;
    }

    QueryResult insertResult = storage_.execute(
        "INSERT INTO providers (provider_key, display_name) VALUES (?, ?);",
        {providerKey, makeProviderDisplayName(providerKey)});
    // A different writer can change the connection-wide last insert ID.
    if (tryGetSingleId(
            "SELECT id FROM providers WHERE provider_key = ? LIMIT 1;",
            {providerKey},
            providerId)) {
        return true;
    }

    if (!insertResult.success()) {
        lastError_ = insertResult.error().empty() ? storage_.getLastError() : insertResult.error();
    }
    if (lastError_.empty()) {
        lastError_ = "Failed to resolve provider row for " + providerKey;
    }
    return false;
}

bool ObservationPersistenceBridge::ensureObservationAccount(
    const std::string& providerKey,
    int providerId,
    const std::string& providerSurface,
    int& accountId) {
    const std::string accountKey = makeObservationAccountKey(providerSurface);

    if (tryGetSingleId(
            "SELECT id FROM accounts WHERE provider_id = ? AND account_key = ? LIMIT 1;",
            {std::to_string(providerId), accountKey},
            accountId)) {
        return true;
    }

    QueryResult insertResult = storage_.execute(
        "INSERT INTO accounts (provider_id, account_key, display_name, scope_type, scope_identifier) "
        "VALUES (?, ?, ?, ?, ?);",
        {std::to_string(providerId), accountKey, makeObservationDisplayName(providerKey, providerSurface),
         "local_observation", providerSurface});
    if (tryGetSingleId(
            "SELECT id FROM accounts WHERE provider_id = ? AND account_key = ? LIMIT 1;",
            {std::to_string(providerId), accountKey},
            accountId)) {
        return true;
    }

    if (!insertResult.success()) {
        lastError_ = insertResult.error().empty() ? storage_.getLastError() : insertResult.error();
    }
    if (lastError_.empty()) {
        lastError_ = "Failed to resolve observation account for " + providerSurface;
    }
    return false;
}

std::optional<int> ObservationPersistenceBridge::resolveActivePlanId(int accountId) {
    QueryResult result = storage_.execute(
        "SELECT id FROM plans WHERE account_id = ? AND is_active = 1 ORDER BY id ASC;",
        {std::to_string(accountId)});
    if (!result.success()) {
        return std::nullopt;
    }

    if (result.rowCount() != 1) {
        return std::nullopt;
    }

    const int planId = result.getInt(0, 0);
    if (planId <= 0) {
        return std::nullopt;
    }

    return planId;
}

bool ObservationPersistenceBridge::tryGetSingleId(
    const std::string& sql,
    const std::vector<std::string>& params,
    int& id) {
    id = 0;

    QueryResult result = storage_.execute(sql, params);
    if (!result.success()) {
        lastError_ = result.error().empty() ? storage_.getLastError() : result.error();
        return false;
    }

    if (result.rowCount() == 0) {
        lastError_.clear();
        return false;
    }

    id = result.getInt(0, 0);
    if (id <= 0) {
        lastError_ = "Resolved row id was invalid";
        return false;
    }

    lastError_.clear();
    return true;
}

} // namespace JiStats
