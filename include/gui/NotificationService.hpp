#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace JiStats {

/**
 * Notice types for different notification scenarios.
 * Each type has its own cooldown and dismissal state.
 */
enum class NoticeType {
    Offline,           // Provider connectivity lost
    AuthFailure,       // Invalid API key or expired credentials
    SubscriptionExpired, // Provider subscription ended
    SyncError,         // Failed to sync with provider
    QuotaWarning,      // Approaching quota limit
    SystemError        // General system error
};

/**
 * Notice priority levels for UI presentation.
 */
enum class NoticePriority {
    Info,      // Informational, low urgency
    Warning,   // Warning, medium urgency
    Error      // Error, high urgency
};

/**
 * Represents an active notice to display.
 */
struct Notice {
    NoticeType type;
    NoticePriority priority;
    std::string title;
    std::string message;
    std::string providerName;  // Optional: which provider this relates to
    bool hasRetryAction;
    std::chrono::system_clock::time_point createdAt;
};

/**
 * NotificationService handles dismissible notices per docs/reference/runtime.md.
 *
 * Key behaviors:
 * - One notice on initial state transition
 * - 60-minute cooldown after dismissal
 * - Same-state suppression (no repeat while in same state)
 * - Persistent status indicator even after dismissal
 * - Auto-clear on state recovery
 */
class NotificationService {
public:
    NotificationService();
    ~NotificationService();
    
    // Non-copyable
    NotificationService(const NotificationService&) = delete;
    NotificationService& operator=(const NotificationService&) = delete;
    
    /**
     * Show a notice. Returns true if the notice was actually shown
     * (i.e., not suppressed by cooldown or same-state rules).
     */
    bool showNotice(const Notice& notice);
    
    /**
     * Dismiss the current notice of the given type.
     * Starts the cooldown period.
     */
    void dismissNotice(NoticeType type);
    
    /**
     * Check if a notice of the given type is currently active.
     */
    bool hasActiveNotice(NoticeType type) const;
    
    /**
     * Get the current active notice of the given type.
     * Returns a copy of the notice, or std::nullopt if no active notice.
     */
    std::optional<Notice> getActiveNotice(NoticeType type) const;
    
    /**
     * Clear notice state for a type (e.g., on recovery).
     * This resets both the active state and cooldown.
     */
    void clearNotice(NoticeType type);
    
    /**
     * Check if a notice type is in cooldown (dismissed recently).
     */
    bool isInCooldown(NoticeType type) const;
    
    /**
     * Set the cooldown duration (default 60 minutes per policy).
     */
    void setCooldownDuration(std::chrono::minutes duration);
    
    /**
     * Suppress notices of a type until explicitly cleared.
     * Used for same-state suppression.
     */
    void suppressUntilCleared(NoticeType type);
    
    /**
     * Check if a notice type is suppressed.
     */
    bool isSuppressed(NoticeType type) const;
    
    /**
     * Set callback for when a notice should be displayed.
     */
    void setOnShowCallback(std::function<void(const Notice&)> callback);
    
    /**
     * Set callback for when a notice is dismissed.
     */
    void setOnDismissCallback(std::function<void(NoticeType)> callback);
    
    /**
     * Get human-readable title for a notice type.
     */
    static std::string getDefaultTitle(NoticeType type);
    
    /**
     * Get priority for a notice type.
     */
    static NoticePriority getDefaultPriority(NoticeType type);

private:
    struct Impl;
    Impl* pImpl;
};

} // namespace JiStats
