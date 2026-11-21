#pragma once

#include <functional>
#include <string>
#include <memory>
#include <chrono>

namespace JiStats {

// Sync operation result for callback reporting
enum class SyncResult {
    Success,        // Sync completed successfully
    NetworkError,   // Network connectivity issue
    Timeout,        // Request timed out
    AuthError,      // Authentication failure (API key invalid/expired)
    RateLimit,      // Provider rate limited
    ApiError,       // Provider server error (5xx)
    UnknownError    // Unclassified failure
};

// Sync type for different operations
enum class SyncType {
    Full,           // Full sync of all data
    QuotaOnly,      // Only sync quota information
    UsageOnly,      // Only sync usage history
    QuickCheck      // Lightweight connectivity check
};

// Sync callback types
using SyncCallback = std::function<void(SyncResult result, const std::string& message)>;
using SyncOperation = std::function<SyncResult()>;

class SyncScheduler {
public:
    SyncScheduler();
    ~SyncScheduler();

    // Initialize the scheduler
    bool init();

    // Shutdown the scheduler
    void shutdown();

    // Schedule a sync operation
    // Returns a sync ID that can be used to cancel
    int scheduleSync(SyncType type, SyncOperation operation, SyncCallback callback);

    // Schedule a periodic sync
    int schedulePeriodicSync(SyncType type, SyncOperation operation, 
                             std::chrono::milliseconds interval, SyncCallback callback);

    // Cancel a scheduled sync by ID
    bool cancelSync(int syncId);

    // Pause all sync operations (e.g., when going offline)
    void pause();

    // Resume sync operations (e.g., when coming back online)
    void resume();

    // Check if scheduler is paused
    bool isPaused() const;

    // Trigger immediate sync (bypasses scheduling)
    void triggerImmediateSync(SyncType type, SyncOperation operation, SyncCallback callback);

    // Get next scheduled sync time (for UI display)
    std::chrono::system_clock::time_point getNextSyncTime() const;

    // Get time until next sync
    std::chrono::seconds getTimeUntilNextSync() const;

    // Report a sync failure (used for backoff calculation)
    void reportFailure(SyncResult errorType);

    // Report a sync success (resets backoff)
    void reportSuccess();

    // Get current backoff delay
    std::chrono::seconds getCurrentBackoff() const;

    // Check if scheduler is initialized
    bool isInitialized() const;

    // Get last error message
    std::string getLastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace JiStats
