#include "core/SyncScheduler.hpp"
#include "core/Logger.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <vector>

namespace JiStats {

// Maximum backoff duration (5 minutes per policy)
constexpr auto MAX_BACKOFF = std::chrono::seconds(300);

// Default sync intervals
constexpr auto DEFAULT_QUOTA_SYNC_INTERVAL = std::chrono::minutes(5);  // 5 minutes
constexpr auto DEFAULT_USAGE_SYNC_INTERVAL = std::chrono::hours(1);    // 1 hour

struct ScheduledSync {
    int id;
    SyncType type;
    SyncOperation operation;
    SyncCallback callback;
    std::chrono::system_clock::time_point nextRun;
    std::chrono::milliseconds interval;
    bool isPeriodic;
    bool isCancelled;
};

class SyncScheduler::Impl {
public:
    std::atomic<bool> initialized{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> shutdown{false};
    std::atomic<int> nextSyncId{1};
    std::string lastError;

    // Backoff state
    std::mutex stateMutex;
    int consecutiveFailures{0};
    std::chrono::seconds currentBackoff{0};

    // Scheduled syncs
    std::map<int, std::unique_ptr<ScheduledSync>> syncs;
    std::mutex syncsMutex;

    // Worker thread
    std::thread workerThread;
    std::condition_variable workerCv;
    std::mutex workerMutex;

    // Queue for immediate syncs
    std::queue<std::unique_ptr<ScheduledSync>> immediateQueue;
    std::mutex immediateMutex;

    void workerLoop() {
        JISTATS_LOG_INFO(LogCategory::Sync, "SyncScheduler worker thread started");

        while (!shutdown.load()) {
            processImmediateSyncs();

            if (!paused.load()) {
                processScheduledSyncs();
            }

            if (shutdown.load()) {
                break;
            }

            std::unique_lock<std::mutex> lock(workerMutex);

            if (paused.load()) {
                workerCv.wait(lock, [this] {
                    return shutdown.load() || !paused.load() || hasImmediateWork();
                });
                continue;
            }

            const auto now = std::chrono::system_clock::now();
            if (hasImmediateWork() || hasDueScheduledSyncs(now)) {
                continue;
            }

            const auto nextSync = getNextSyncTimeSnapshot();
            if (nextSync.has_value()) {
                workerCv.wait_until(lock, nextSync.value(), [this, nextSync] {
                    return shutdown.load() || paused.load() || hasImmediateWork() ||
                           nextSyncChanged(nextSync);
                });
            } else {
                workerCv.wait(lock, [this] {
                    return shutdown.load() || paused.load() || hasImmediateWork() ||
                           hasScheduledSyncs();
                });
            }
        }

        JISTATS_LOG_INFO(LogCategory::Sync, "SyncScheduler worker thread stopped");
    }

    std::optional<std::chrono::system_clock::time_point> getNextSyncTimeSnapshot() {
        std::lock_guard<std::mutex> lock(syncsMutex);
        
        std::optional<std::chrono::system_clock::time_point> nextTime;
        for (const auto& [id, sync] : syncs) {
            if (!sync->isCancelled && (!nextTime.has_value() || sync->nextRun < nextTime.value())) {
                nextTime = sync->nextRun;
            }
        }
        return nextTime;
    }

    bool hasImmediateWork() {
        std::lock_guard<std::mutex> lock(immediateMutex);
        return !immediateQueue.empty();
    }

    bool hasScheduledSyncs() {
        std::lock_guard<std::mutex> lock(syncsMutex);
        for (const auto& [id, sync] : syncs) {
            if (!sync->isCancelled) {
                return true;
            }
        }
        return false;
    }

    bool hasDueScheduledSyncs(std::chrono::system_clock::time_point now) {
        std::lock_guard<std::mutex> lock(syncsMutex);
        for (const auto& [id, sync] : syncs) {
            if (!sync->isCancelled && sync->nextRun <= now) {
                return true;
            }
        }
        return false;
    }

    bool nextSyncChanged(const std::optional<std::chrono::system_clock::time_point>& expected) {
        return getNextSyncTimeSnapshot() != expected;
    }

    std::chrono::seconds getCurrentBackoffSnapshot() {
        std::lock_guard<std::mutex> lock(stateMutex);
        return currentBackoff;
    }

    std::chrono::system_clock::time_point calculateNextRun(bool isPeriodic,
                                                           std::chrono::milliseconds interval = std::chrono::milliseconds(0)) {
        const auto backoff = getCurrentBackoffSnapshot();
        const auto backoffDelay = std::chrono::duration_cast<std::chrono::milliseconds>(backoff);
        const auto delay = isPeriodic ? std::max(interval, backoffDelay) : backoffDelay;
        return std::chrono::system_clock::now() + delay;
    }

    void processImmediateSyncs() {
        while (true) {
            std::unique_ptr<ScheduledSync> sync;
            {
                std::lock_guard<std::mutex> lock(immediateMutex);
                if (immediateQueue.empty()) {
                    break;
                }
                sync = std::move(immediateQueue.front());
                immediateQueue.pop();
            }

            executeSync(*sync);
        }
    }

    void processScheduledSyncs() {
        std::vector<ScheduledSync> syncsToRun;

        {
            std::lock_guard<std::mutex> lock(syncsMutex);
            const auto now = std::chrono::system_clock::now();
            for (const auto& [id, sync] : syncs) {
                if (!sync->isCancelled && sync->nextRun <= now) {
                    ScheduledSync scheduled{
                        sync->id,
                        sync->type,
                        sync->operation,
                        sync->callback,
                        sync->nextRun,
                        sync->interval,
                        sync->isPeriodic,
                        sync->isCancelled
                    };
                    syncsToRun.push_back(std::move(scheduled));
                }
            }
        }

        for (const ScheduledSync& syncToRun : syncsToRun) {
            executeSync(syncToRun);

            std::lock_guard<std::mutex> lock(syncsMutex);
            auto it = syncs.find(syncToRun.id);
            if (it == syncs.end()) {
                continue;
            }

            if (it->second->isCancelled) {
                syncs.erase(it);
                continue;
            }

            if (it->second->isPeriodic) {
                it->second->nextRun = calculateNextRun(true, it->second->interval);
            } else {
                syncs.erase(it);
            }
        }

        cleanupCancelledSyncs();
    }

    void executeSync(const ScheduledSync& sync) {
        JISTATS_LOG_DEBUG(LogCategory::Sync, 
            "Executing sync ID " + std::to_string(sync.id));

        SyncResult result = SyncResult::UnknownError;
        std::string message;

        try {
            if (sync.operation) {
                result = sync.operation();
                message = resultToString(result);
            } else {
                result = SyncResult::UnknownError;
                message = "No operation provided";
            }
        } catch (const std::exception& e) {
            result = SyncResult::UnknownError;
            message = std::string("Exception: ") + e.what();
            JISTATS_LOG_ERROR(LogCategory::Sync, 
                "Sync ID " + std::to_string(sync.id) + " failed: " + message);
        }

        if (sync.callback) {
            sync.callback(result, message);
        }
    }

    void cleanupCancelledSyncs() {
        std::lock_guard<std::mutex> lock(syncsMutex);
        for (auto it = syncs.begin(); it != syncs.end(); ) {
            if (it->second->isCancelled) {
                it = syncs.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::string resultToString(SyncResult result) {
        switch (result) {
            case SyncResult::Success: return "Success";
            case SyncResult::NetworkError: return "Network Error";
            case SyncResult::Timeout: return "Timeout";
            case SyncResult::AuthError: return "Authentication Error";
            case SyncResult::RateLimit: return "Rate Limited";
            case SyncResult::ApiError: return "API Error";
            case SyncResult::UnknownError: return "Unknown Error";
            default: return "Unknown";
        }
    }

    std::chrono::seconds calculateBackoff(int failures) {
        // Exponential backoff: 1s, 2s, 4s, 8s, 16s, 32s, 64s, max 5 min
        if (failures <= 0) {
            return std::chrono::seconds(0);
        }

        int delay = 1;
        const int maxDelay = static_cast<int>(MAX_BACKOFF.count());
        for (int attempt = 1; attempt < failures && delay < maxDelay; ++attempt) {
            delay = std::min(delay * 2, maxDelay);
        }
        return std::chrono::seconds(delay);
    }
};

SyncScheduler::SyncScheduler() : pImpl(std::make_unique<Impl>()) {
}

SyncScheduler::~SyncScheduler() {
    shutdown();
}

bool SyncScheduler::init() {
    if (pImpl->initialized) {
        return true;
    }

    JISTATS_LOG_INFO(LogCategory::Startup, "SyncScheduler initializing");

    pImpl->shutdown = false;
    pImpl->workerThread = std::thread(&Impl::workerLoop, pImpl.get());

    pImpl->initialized = true;
    JISTATS_LOG_INFO(LogCategory::Startup, "SyncScheduler initialized");
    return true;
}

void SyncScheduler::shutdown() {
    if (!pImpl->initialized) {
        return;
    }

    JISTATS_LOG_INFO(LogCategory::Startup, "SyncScheduler shutting down");

    pImpl->shutdown = true;
    pImpl->workerCv.notify_all();

    if (pImpl->workerThread.joinable()) {
        pImpl->workerThread.join();
    }

    // Clear all scheduled syncs
    {
        std::lock_guard<std::mutex> lock(pImpl->syncsMutex);
        pImpl->syncs.clear();
    }

    pImpl->initialized = false;
}

int SyncScheduler::scheduleSync(SyncType type, SyncOperation operation, SyncCallback callback) {
    if (!pImpl->initialized) {
        return -1;
    }

    auto sync = std::make_unique<ScheduledSync>();
    sync->id = pImpl->nextSyncId++;
    sync->type = type;
    sync->operation = std::move(operation);
    sync->callback = std::move(callback);
    sync->nextRun = pImpl->calculateNextRun(false);
    sync->isPeriodic = false;
    sync->isCancelled = false;

    int id = sync->id;
    {
        std::lock_guard<std::mutex> lock(pImpl->syncsMutex);
        pImpl->syncs[id] = std::move(sync);
    }

    pImpl->workerCv.notify_one();
    JISTATS_LOG_DEBUG(LogCategory::Sync, "Scheduled sync ID " + std::to_string(id));
    return id;
}

int SyncScheduler::schedulePeriodicSync(SyncType type, SyncOperation operation, 
                                         std::chrono::milliseconds interval, SyncCallback callback) {
    if (!pImpl->initialized) {
        return -1;
    }

    auto sync = std::make_unique<ScheduledSync>();
    sync->id = pImpl->nextSyncId++;
    sync->type = type;
    sync->operation = std::move(operation);
    sync->callback = std::move(callback);
    sync->nextRun = pImpl->calculateNextRun(true, interval);
    sync->interval = interval;
    sync->isPeriodic = true;
    sync->isCancelled = false;

    int id = sync->id;
    {
        std::lock_guard<std::mutex> lock(pImpl->syncsMutex);
        pImpl->syncs[id] = std::move(sync);
    }

    pImpl->workerCv.notify_one();
    JISTATS_LOG_DEBUG(LogCategory::Sync, 
        "Scheduled periodic sync ID " + std::to_string(id) + 
        " with interval " + std::to_string(interval.count()) + "ms");
    return id;
}

bool SyncScheduler::cancelSync(int syncId) {
    bool cancelled = false;

    {
        std::lock_guard<std::mutex> lock(pImpl->syncsMutex);
        auto it = pImpl->syncs.find(syncId);
        if (it != pImpl->syncs.end()) {
            it->second->isCancelled = true;
            cancelled = true;
        }
    }

    if (cancelled) {
        pImpl->workerCv.notify_one();
        JISTATS_LOG_DEBUG(LogCategory::Sync, "Cancelled sync ID " + std::to_string(syncId));
        return true;
    }

    return false;
}

void SyncScheduler::pause() {
    pImpl->paused = true;
    pImpl->workerCv.notify_one();
    JISTATS_LOG_INFO(LogCategory::Sync, "SyncScheduler paused");
}

void SyncScheduler::resume() {
    pImpl->paused = false;
    pImpl->workerCv.notify_one();
    JISTATS_LOG_INFO(LogCategory::Sync, "SyncScheduler resumed");
}

bool SyncScheduler::isPaused() const {
    return pImpl->paused;
}

void SyncScheduler::triggerImmediateSync(SyncType type, SyncOperation operation, SyncCallback callback) {
    if (!pImpl->initialized) {
        return;
    }

    auto sync = std::make_unique<ScheduledSync>();
    sync->id = 0; // Immediate syncs don't have persistent IDs
    sync->type = type;
    sync->operation = std::move(operation);
    sync->callback = std::move(callback);
    sync->nextRun = std::chrono::system_clock::now();
    sync->isPeriodic = false;
    sync->isCancelled = false;

    {
        std::lock_guard<std::mutex> lock(pImpl->immediateMutex);
        pImpl->immediateQueue.push(std::move(sync));
    }

    pImpl->workerCv.notify_one();
    JISTATS_LOG_DEBUG(LogCategory::Sync, "Triggered immediate sync");
}

std::chrono::system_clock::time_point SyncScheduler::getNextSyncTime() const {
    std::lock_guard<std::mutex> lock(pImpl->syncsMutex);
    
    std::optional<std::chrono::system_clock::time_point> nextTime;
    for (const auto& [id, sync] : pImpl->syncs) {
        if (!sync->isCancelled && (!nextTime.has_value() || sync->nextRun < nextTime.value())) {
            nextTime = sync->nextRun;
        }
    }
    
    return nextTime.value_or(std::chrono::system_clock::now());
}

std::chrono::seconds SyncScheduler::getTimeUntilNextSync() const {
    auto nextTime = getNextSyncTime();
    auto now = std::chrono::system_clock::now();
    
    if (nextTime > now) {
        return std::chrono::duration_cast<std::chrono::seconds>(nextTime - now);
    }
    return std::chrono::seconds(0);
}

void SyncScheduler::reportFailure(SyncResult errorType) {
    int failures = 0;
    std::chrono::seconds backoff(0);

    {
        std::lock_guard<std::mutex> lock(pImpl->stateMutex);
        pImpl->consecutiveFailures++;
        pImpl->currentBackoff = pImpl->calculateBackoff(pImpl->consecutiveFailures);
        failures = pImpl->consecutiveFailures;
        backoff = pImpl->currentBackoff;
    }

    pImpl->workerCv.notify_one();
    
    JISTATS_LOG_WARN(LogCategory::Sync, 
        "Sync failure reported (type: " + pImpl->resultToString(errorType) + 
        "), consecutive failures: " + std::to_string(failures) +
        ", backoff: " + std::to_string(backoff.count()) + "s");
}

void SyncScheduler::reportSuccess() {
    int failures = 0;

    {
        std::lock_guard<std::mutex> lock(pImpl->stateMutex);
        failures = pImpl->consecutiveFailures;
        pImpl->consecutiveFailures = 0;
        pImpl->currentBackoff = std::chrono::seconds(0);
    }

    pImpl->workerCv.notify_one();

    if (failures > 0) {
        JISTATS_LOG_INFO(LogCategory::Sync, 
            "Sync success after " + std::to_string(failures) + " failures, resetting backoff");
    }
}

std::chrono::seconds SyncScheduler::getCurrentBackoff() const {
    return pImpl->getCurrentBackoffSnapshot();
}

bool SyncScheduler::isInitialized() const {
    return pImpl->initialized;
}

std::string SyncScheduler::getLastError() const {
    return pImpl->lastError;
}

} // namespace JiStats

#ifdef JISTATS_ENABLE_TEST_HOOKS
#include <catch_amalgamated.hpp>

namespace {

using namespace std::chrono_literals;

bool waitForCondition(const std::function<bool()>& predicate,
                      std::chrono::milliseconds timeout,
                      std::chrono::milliseconds pollInterval = 10ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(pollInterval);
    }

    return predicate();
}

TEST_CASE("SyncScheduler backoff grows exponentially and resets", "[sync_scheduler][backoff]") {
    JiStats::SyncScheduler scheduler;
    REQUIRE(scheduler.init());

    const std::vector<std::chrono::seconds> expectedBackoff{
        1s, 2s, 4s, 8s, 16s, 32s, 64s, 128s, 256s, 300s, 300s
    };

    for (const auto expected : expectedBackoff) {
        scheduler.reportFailure(JiStats::SyncResult::NetworkError);
        REQUIRE(scheduler.getCurrentBackoff() == expected);
    }

    scheduler.reportSuccess();
    REQUIRE(scheduler.getCurrentBackoff() == 0s);

    scheduler.shutdown();
}

TEST_CASE("SyncScheduler keeps scheduled work paused until resume", "[sync_scheduler][wait]") {
    JiStats::SyncScheduler scheduler;
    REQUIRE(scheduler.init());
    scheduler.pause();

    std::atomic<int> callbackCount{0};

    REQUIRE(scheduler.scheduleSync(
        JiStats::SyncType::QuickCheck,
        [] { return JiStats::SyncResult::Success; },
        [&callbackCount](JiStats::SyncResult, const std::string&) {
            callbackCount.fetch_add(1);
        }) > 0);

    std::this_thread::sleep_for(250ms);
    REQUIRE(callbackCount.load() == 0);

    scheduler.resume();
    REQUIRE(waitForCondition([&callbackCount] { return callbackCount.load() == 1; }, 1s));

    scheduler.shutdown();
}

TEST_CASE("SyncScheduler periodic sync respects active backoff when scheduled", "[sync_scheduler][backoff][timing]") {
    JiStats::SyncScheduler scheduler;
    REQUIRE(scheduler.init());

    std::atomic<int> callbackCount{0};
    std::atomic<int> syncId{0};

    scheduler.reportFailure(JiStats::SyncResult::NetworkError);
    scheduler.reportFailure(JiStats::SyncResult::NetworkError);
    REQUIRE(scheduler.getCurrentBackoff() == 2s);

    syncId.store(scheduler.schedulePeriodicSync(
        JiStats::SyncType::QuickCheck,
        [] { return JiStats::SyncResult::Success; },
        100ms,
        [&callbackCount, &scheduler, &syncId](JiStats::SyncResult, const std::string&) {
            callbackCount.fetch_add(1);
            scheduler.cancelSync(syncId.load());
        }));

    REQUIRE(syncId.load() > 0);
    std::this_thread::sleep_for(1300ms);
    REQUIRE(callbackCount.load() == 0);
    REQUIRE(waitForCondition([&callbackCount] { return callbackCount.load() == 1; }, 2s));

    scheduler.shutdown();
}

TEST_CASE("SyncScheduler callbacks can cancel another scheduled sync without deadlocking", "[sync_scheduler][runtime]") {
    JiStats::SyncScheduler scheduler;
    REQUIRE(scheduler.init());

    std::atomic<int> immediateCallbackCount{0};
    std::atomic<int> delayedCallbackCount{0};

    scheduler.reportFailure(JiStats::SyncResult::NetworkError);
    const int delayedSyncId = scheduler.scheduleSync(
        JiStats::SyncType::QuickCheck,
        [] { return JiStats::SyncResult::Success; },
        [&delayedCallbackCount](JiStats::SyncResult, const std::string&) {
            delayedCallbackCount.fetch_add(1);
        });
    REQUIRE(delayedSyncId > 0);

    scheduler.reportSuccess();

    REQUIRE(scheduler.scheduleSync(
        JiStats::SyncType::QuickCheck,
        [] { return JiStats::SyncResult::Success; },
        [&scheduler, &immediateCallbackCount, delayedSyncId](JiStats::SyncResult, const std::string&) {
            immediateCallbackCount.fetch_add(1);
            scheduler.cancelSync(delayedSyncId);
        }) > 0);

    REQUIRE(waitForCondition([&immediateCallbackCount] { return immediateCallbackCount.load() == 1; }, 1s));

    std::this_thread::sleep_for(1300ms);
    REQUIRE(delayedCallbackCount.load() == 0);

    scheduler.shutdown();
}

} // namespace
#endif
