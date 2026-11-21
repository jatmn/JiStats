#include "observation/LocalObservationService.hpp"

#include <mutex>
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

} // namespace

class LocalObservationService::Impl {
public:
    bool initialized = false;
    std::string lastError;
    ActivityCallback callback;
    std::vector<ActivityEvent> pendingActivities;
    mutable std::mutex mutex;
};

LocalObservationService::LocalObservationService()
    : pImpl(std::make_unique<Impl>()) {
}

LocalObservationService::~LocalObservationService() = default;

bool LocalObservationService::init() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->initialized = true;
    pImpl->lastError.clear();
    return true;
}

void LocalObservationService::shutdown() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->pendingActivities.clear();
    pImpl->callback = nullptr;
    pImpl->initialized = false;
    pImpl->lastError.clear();
}

bool LocalObservationService::isInitialized() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->initialized;
}

std::string LocalObservationService::getLastError() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->lastError;
}

void LocalObservationService::setActivityCallback(ActivityCallback callback) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->callback = std::move(callback);
}

bool LocalObservationService::recordActivity(const ActivityEvent& event) {
    ActivityCallback callback;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        if (!pImpl->initialized) {
            pImpl->lastError = "LocalObservationService is not initialized";
            return false;
        }

        if (!isValidActivityEvent(event, pImpl->lastError)) {
            return false;
        }

        pImpl->pendingActivities.push_back(event);
        callback = pImpl->callback;
        pImpl->lastError.clear();
    }

    if (callback) {
        callback(event);
    }

    return true;
}

std::vector<ActivityEvent> LocalObservationService::takePendingActivities() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    std::vector<ActivityEvent> activities = std::move(pImpl->pendingActivities);
    pImpl->pendingActivities.clear();
    return activities;
}

size_t LocalObservationService::getPendingActivityCount() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->pendingActivities.size();
}

std::string toString(ActivityKind kind) {
    switch (kind) {
        case ActivityKind::SessionActive:
            return "session_active";
        case ActivityKind::MessageSend:
            return "message_send";
        case ActivityKind::ToolRun:
            return "tool_run";
        case ActivityKind::UnknownActivity:
        default:
            return "unknown_activity";
    }
}

std::string toString(ActivityConfidence confidence) {
    switch (confidence) {
        case ActivityConfidence::LocalExact:
            return "LocalExact";
        case ActivityConfidence::Imported:
            return "Imported";
        case ActivityConfidence::Estimated:
            return "Estimated";
        case ActivityConfidence::Unknown:
        default:
            return "Unknown";
    }
}

} // namespace JiStats
