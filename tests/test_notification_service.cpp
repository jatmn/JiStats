#include <catch_amalgamated.hpp>

#include <chrono>
#include <optional>
#include <string>

#include "gui/NotificationService.hpp"

using namespace JiStats;

namespace {

Notice makeNotice(NoticeType type = NoticeType::Offline) {
    Notice notice;
    notice.type = type;
    notice.priority = NotificationService::getDefaultPriority(type);
    notice.title = NotificationService::getDefaultTitle(type);
    notice.message = "Connectivity lost";
    notice.providerName = "openai";
    notice.hasRetryAction = true;
    notice.createdAt = std::chrono::system_clock::now();
    return notice;
}

} // namespace

TEST_CASE("NotificationService shows notices and keeps show callbacks re-entrant safe", "[notification]") {
    NotificationService service;

    int showCallbackCount = 0;
    bool callbackSawActiveNotice = false;
    std::optional<Notice> callbackNotice;

    service.setOnShowCallback([&](const Notice& notice) {
        ++showCallbackCount;
        callbackSawActiveNotice = service.hasActiveNotice(notice.type);
        callbackNotice = service.getActiveNotice(notice.type);
    });

    const Notice notice = makeNotice();

    REQUIRE(service.showNotice(notice));
    CHECK(showCallbackCount == 1);
    CHECK(callbackSawActiveNotice);
    REQUIRE(callbackNotice.has_value());
    CHECK(callbackNotice->title == notice.title);
    CHECK(callbackNotice->message == notice.message);

    CHECK(service.hasActiveNotice(notice.type));
    const std::optional<Notice> activeNotice = service.getActiveNotice(notice.type);
    REQUIRE(activeNotice.has_value());
    CHECK(activeNotice->providerName == "openai");
    CHECK_FALSE(service.showNotice(notice));
}

TEST_CASE("NotificationService dismiss starts cooldown and keeps dismiss callbacks re-entrant safe", "[notification]") {
    NotificationService service;
    service.setCooldownDuration(std::chrono::minutes(5));

    const Notice notice = makeNotice(NoticeType::SyncError);
    REQUIRE(service.showNotice(notice));

    int dismissCallbackCount = 0;
    bool callbackSawNoActiveNotice = false;
    bool callbackSawCooldown = false;

    service.setOnDismissCallback([&](NoticeType type) {
        ++dismissCallbackCount;
        callbackSawNoActiveNotice = !service.hasActiveNotice(type);
        callbackSawCooldown = service.isInCooldown(type);
    });

    service.dismissNotice(notice.type);

    CHECK(dismissCallbackCount == 1);
    CHECK(callbackSawNoActiveNotice);
    CHECK(callbackSawCooldown);
    CHECK_FALSE(service.hasActiveNotice(notice.type));
    CHECK_FALSE(service.getActiveNotice(notice.type).has_value());
    CHECK(service.isInCooldown(notice.type));
    CHECK_FALSE(service.showNotice(notice));
}

TEST_CASE("NotificationService clearNotice resets cooldown and suppression state", "[notification]") {
    NotificationService service;
    service.setCooldownDuration(std::chrono::minutes(5));

    const Notice notice = makeNotice(NoticeType::AuthFailure);
    REQUIRE(service.showNotice(notice));
    service.dismissNotice(notice.type);
    REQUIRE(service.isInCooldown(notice.type));

    service.suppressUntilCleared(notice.type);
    REQUIRE(service.isSuppressed(notice.type));

    service.clearNotice(notice.type);

    CHECK_FALSE(service.hasActiveNotice(notice.type));
    CHECK_FALSE(service.isInCooldown(notice.type));
    CHECK_FALSE(service.isSuppressed(notice.type));
    CHECK(service.showNotice(notice));
}

TEST_CASE("NotificationService suppression blocks notices until cleared", "[notification]") {
    NotificationService service;

    const Notice notice = makeNotice(NoticeType::QuotaWarning);
    service.suppressUntilCleared(notice.type);

    CHECK(service.isSuppressed(notice.type));
    CHECK_FALSE(service.showNotice(notice));
    CHECK_FALSE(service.hasActiveNotice(notice.type));

    service.clearNotice(notice.type);

    CHECK_FALSE(service.isSuppressed(notice.type));
    CHECK(service.showNotice(notice));
}

TEST_CASE("NotificationService zero cooldown allows immediate re-show after dismissal", "[notification]") {
    NotificationService service;
    service.setCooldownDuration(std::chrono::minutes::zero());

    const Notice notice = makeNotice(NoticeType::SystemError);
    REQUIRE(service.showNotice(notice));

    service.dismissNotice(notice.type);

    CHECK_FALSE(service.isInCooldown(notice.type));
    CHECK(service.showNotice(notice));
}
