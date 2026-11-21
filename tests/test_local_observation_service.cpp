#include <catch_amalgamated.hpp>

#include <ctime>
#include <vector>

#include "observation/LocalObservationService.hpp"

using namespace JiStats;

namespace {

ActivityEvent makeValidActivityEvent() {
    ActivityEvent event;
    event.providerKey = "openai";
    event.providerSurface = "openai_subscription";
    event.activityKind = ActivityKind::SessionActive;
    event.startedAt = 1713416400;
    event.endedAt = 1713416460;
    event.durationSeconds = 60;
    event.sourceRunId = "run-123";
    event.surfaceKey = "foreground_window";
    event.windowTitleHint = "ChatGPT";
    event.confidence = ActivityConfidence::LocalExact;
    event.rawReference = "window:chatgpt";
    return event;
}

} // namespace

TEST_CASE("LocalObservationService rejects activity before initialization", "[observation]") {
    LocalObservationService service;

    REQUIRE_FALSE(service.isInitialized());
    REQUIRE_FALSE(service.recordActivity(makeValidActivityEvent()));
    REQUIRE(service.getPendingActivityCount() == 0);
    CHECK(service.getLastError() == "LocalObservationService is not initialized");
}

TEST_CASE("LocalObservationService records and drains valid activities", "[observation]") {
    LocalObservationService service;
    REQUIRE(service.init());
    REQUIRE(service.isInitialized());

    const ActivityEvent event = makeValidActivityEvent();

    REQUIRE(service.recordActivity(event));
    CHECK(service.getLastError().empty());
    REQUIRE(service.getPendingActivityCount() == 1);

    const std::vector<ActivityEvent> pending = service.takePendingActivities();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].providerKey == "openai");
    CHECK(pending[0].providerSurface == "openai_subscription");
    CHECK(pending[0].activityKind == ActivityKind::SessionActive);
    CHECK(pending[0].startedAt == event.startedAt);
    CHECK(pending[0].endedAt == event.endedAt);
    CHECK(pending[0].durationSeconds == 60);

    CHECK(service.getPendingActivityCount() == 0);
    CHECK(service.takePendingActivities().empty());
}

TEST_CASE("LocalObservationService rejects invalid activities", "[observation]") {
    LocalObservationService service;
    REQUIRE(service.init());

    SECTION("missing provider key") {
        ActivityEvent event = makeValidActivityEvent();
        event.providerKey.clear();

        REQUIRE_FALSE(service.recordActivity(event));
        CHECK(service.getLastError() == "Activity event is missing providerKey");
    }

    SECTION("missing provider surface") {
        ActivityEvent event = makeValidActivityEvent();
        event.providerSurface.clear();

        REQUIRE_FALSE(service.recordActivity(event));
        CHECK(service.getLastError() == "Activity event is missing providerSurface");
    }

    SECTION("invalid timestamps") {
        ActivityEvent event = makeValidActivityEvent();
        event.endedAt = event.startedAt - 1;

        REQUIRE_FALSE(service.recordActivity(event));
        CHECK(service.getLastError() == "Activity event endedAt cannot be earlier than startedAt");
    }

    SECTION("negative duration") {
        ActivityEvent event = makeValidActivityEvent();
        event.durationSeconds = -1;

        REQUIRE_FALSE(service.recordActivity(event));
        CHECK(service.getLastError() == "Activity event durationSeconds cannot be negative");
    }

    CHECK(service.getPendingActivityCount() == 0);
}

TEST_CASE("LocalObservationService invokes the activity callback", "[observation]") {
    LocalObservationService service;
    REQUIRE(service.init());

    int callbackCount = 0;
    ActivityEvent callbackEvent;
    service.setActivityCallback([&](const ActivityEvent& event) {
        ++callbackCount;
        callbackEvent = event;
    });

    const ActivityEvent event = makeValidActivityEvent();
    REQUIRE(service.recordActivity(event));

    CHECK(callbackCount == 1);
    CHECK(callbackEvent.providerKey == "openai");
    CHECK(callbackEvent.surfaceKey == "foreground_window");
}

TEST_CASE("ActivityEvent helpers expose completion and enum text", "[observation]") {
    ActivityEvent completed = makeValidActivityEvent();
    CHECK(completed.isCompleted());

    ActivityEvent incomplete = makeValidActivityEvent();
    incomplete.endedAt = 0;
    CHECK_FALSE(incomplete.isCompleted());

    CHECK(toString(ActivityKind::SessionActive) == "session_active");
    CHECK(toString(ActivityKind::MessageSend) == "message_send");
    CHECK(toString(ActivityKind::ToolRun) == "tool_run");
    CHECK(toString(ActivityKind::UnknownActivity) == "unknown_activity");

    CHECK(toString(ActivityConfidence::LocalExact) == "LocalExact");
    CHECK(toString(ActivityConfidence::Imported) == "Imported");
    CHECK(toString(ActivityConfidence::Estimated) == "Estimated");
    CHECK(toString(ActivityConfidence::Unknown) == "Unknown");
}

TEST_CASE("LocalObservationService shutdown clears state", "[observation]") {
    LocalObservationService service;
    REQUIRE(service.init());
    REQUIRE(service.recordActivity(makeValidActivityEvent()));
    REQUIRE(service.getPendingActivityCount() == 1);

    service.shutdown();

    CHECK_FALSE(service.isInitialized());
    CHECK(service.getPendingActivityCount() == 0);
    CHECK(service.getLastError().empty());
    CHECK_FALSE(service.recordActivity(makeValidActivityEvent()));
}
