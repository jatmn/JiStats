#include "gui/NotificationService.hpp"

#include <chrono>
#include <map>
#include <mutex>
#include <set>

namespace {

using CooldownClock = std::chrono::steady_clock;

bool isInCooldownLocked(
    const std::map<JiStats::NoticeType, CooldownClock::time_point>& dismissedAt,
    JiStats::NoticeType type,
    std::chrono::minutes cooldownDuration,
    CooldownClock::time_point now) {
    if (cooldownDuration <= std::chrono::minutes::zero()) {
        return false;
    }

    const auto it = dismissedAt.find(type);
    if (it == dismissedAt.end()) {
        return false;
    }

    return now < (it->second + cooldownDuration);
}

} // namespace

namespace JiStats {

struct NotificationService::Impl {
    std::map<NoticeType, Notice> activeNotices;
    std::map<NoticeType, CooldownClock::time_point> dismissedAt;
    std::set<NoticeType> suppressed;
    std::chrono::minutes cooldownDuration{60};  // Default 60 minutes per policy
    std::function<void(const Notice&)> onShowCallback;
    std::function<void(NoticeType)> onDismissCallback;
    mutable std::mutex mutex;
};

NotificationService::NotificationService() : pImpl(new Impl()) {
}

NotificationService::~NotificationService() {
    delete pImpl;
}

bool NotificationService::showNotice(const Notice& notice) {
    std::function<void(const Notice&)> onShowCallback;

    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);

        if (pImpl->suppressed.count(notice.type) > 0) {
            return false;
        }

        if (isInCooldownLocked(
                pImpl->dismissedAt,
                notice.type,
                pImpl->cooldownDuration,
                CooldownClock::now())) {
            return false;
        }

        if (pImpl->activeNotices.count(notice.type) > 0) {
            return false;
        }

        pImpl->dismissedAt.erase(notice.type);
        pImpl->activeNotices[notice.type] = notice;
        onShowCallback = pImpl->onShowCallback;
    }

    if (onShowCallback) {
        onShowCallback(notice);
    }

    return true;
}

void NotificationService::dismissNotice(NoticeType type) {
    std::function<void(NoticeType)> onDismissCallback;
    bool dismissed = false;

    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);

        if (pImpl->activeNotices.erase(type) == 0) {
            return;
        }

        pImpl->dismissedAt[type] = CooldownClock::now();
        onDismissCallback = pImpl->onDismissCallback;
        dismissed = true;
    }

    if (dismissed && onDismissCallback) {
        onDismissCallback(type);
    }
}

bool NotificationService::hasActiveNotice(NoticeType type) const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->activeNotices.count(type) > 0;
}

std::optional<Notice> NotificationService::getActiveNotice(NoticeType type) const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto it = pImpl->activeNotices.find(type);
    if (it != pImpl->activeNotices.end()) {
        return it->second;
    }
    return std::nullopt;
}

void NotificationService::clearNotice(NoticeType type) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->activeNotices.erase(type);
    pImpl->dismissedAt.erase(type);
    pImpl->suppressed.erase(type);
}

bool NotificationService::isInCooldown(NoticeType type) const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);

    return isInCooldownLocked(
        pImpl->dismissedAt,
        type,
        pImpl->cooldownDuration,
        CooldownClock::now());
}

void NotificationService::setCooldownDuration(std::chrono::minutes duration) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (duration < std::chrono::minutes::zero()) {
        duration = std::chrono::minutes::zero();
    }

    pImpl->cooldownDuration = duration;
}

void NotificationService::suppressUntilCleared(NoticeType type) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->suppressed.insert(type);
}

bool NotificationService::isSuppressed(NoticeType type) const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->suppressed.count(type) > 0;
}

void NotificationService::setOnShowCallback(std::function<void(const Notice&)> callback) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->onShowCallback = std::move(callback);
}

void NotificationService::setOnDismissCallback(std::function<void(NoticeType)> callback) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->onDismissCallback = std::move(callback);
}

std::string NotificationService::getDefaultTitle(NoticeType type) {
    switch (type) {
        case NoticeType::Offline:
            return "Connection Lost";
        case NoticeType::AuthFailure:
            return "Authentication Failed";
        case NoticeType::SubscriptionExpired:
            return "Subscription Expired";
        case NoticeType::SyncError:
            return "Sync Error";
        case NoticeType::QuotaWarning:
            return "Quota Warning";
        case NoticeType::SystemError:
            return "System Error";
        default:
            return "Notice";
    }
}

NoticePriority NotificationService::getDefaultPriority(NoticeType type) {
    switch (type) {
        case NoticeType::Offline:
            return NoticePriority::Warning;
        case NoticeType::AuthFailure:
            return NoticePriority::Error;
        case NoticeType::SubscriptionExpired:
            return NoticePriority::Error;
        case NoticeType::SyncError:
            return NoticePriority::Warning;
        case NoticeType::QuotaWarning:
            return NoticePriority::Warning;
        case NoticeType::SystemError:
            return NoticePriority::Error;
        default:
            return NoticePriority::Info;
    }
}

} // namespace JiStats
