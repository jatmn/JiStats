#include <catch_amalgamated.hpp>

#include <vector>

#include "core/RuntimeStatus.hpp"

using namespace JiStats;

TEST_CASE("RuntimeStatus derives aggregate runtime state with consistent priority", "[runtime_status]") {
    CHECK(deriveRuntimeState({}) == RuntimeState::Unknown);
    CHECK(deriveRuntimeState({ProviderState::Live}) == RuntimeState::Current);
    CHECK(deriveRuntimeState({ProviderState::Sample}) == RuntimeState::Current);
    CHECK(deriveRuntimeState({ProviderState::Stale, ProviderState::Live}) == RuntimeState::Stale);
    CHECK(deriveRuntimeState({ProviderState::Offline, ProviderState::Live}) == RuntimeState::Offline);
    CHECK(deriveRuntimeState({ProviderState::Offline, ProviderState::Syncing}) == RuntimeState::CatchingUp);
}

TEST_CASE("RuntimeStatus describes offline entry and recovery transitions", "[runtime_status]") {
    const RuntimeStatusTransition offline =
        describeRuntimeStateTransition(RuntimeState::Current, RuntimeState::Offline);
    CHECK(offline.enteredOffline);
    CHECK_FALSE(offline.exitedOffline);

    const RuntimeStatusTransition recovery =
        describeRuntimeStateTransition(RuntimeState::Offline, RuntimeState::Current);
    CHECK_FALSE(recovery.enteredOffline);
    CHECK(recovery.exitedOffline);
}

TEST_CASE("RuntimeStatus parses and formats runtime timestamps across supported formats", "[runtime_status]") {
    const std::time_t utcFormat = parseRuntimeTimestamp("2026-04-18 00:00:00 UTC");
    const std::time_t isoFormat = parseRuntimeTimestamp("2026-04-18T00:00:00Z");

    REQUIRE(utcFormat > 0);
    CHECK(utcFormat == isoFormat);

    CHECK(formatRuntimeTimeAgo("2026-04-18 00:00:00 UTC", utcFormat + 120) == "2 mins ago");
    CHECK(formatRuntimeTimeAgo("2026-04-18T00:00:00Z", isoFormat + 7200) == "2 hours ago");
}
