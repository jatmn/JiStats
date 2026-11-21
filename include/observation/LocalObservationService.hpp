#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "observation/ActivityEvent.hpp"

namespace JiStats {

using ActivityCallback = std::function<void(const ActivityEvent&)>;

class LocalObservationService {
public:
    LocalObservationService();
    ~LocalObservationService();

    bool init();
    void shutdown();

    bool isInitialized() const;
    std::string getLastError() const;

    void setActivityCallback(ActivityCallback callback);

    bool recordActivity(const ActivityEvent& event);
    std::vector<ActivityEvent> takePendingActivities();
    size_t getPendingActivityCount() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace JiStats
