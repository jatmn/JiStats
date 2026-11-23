#include <catch_amalgamated.hpp>

#include <string>
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
    constexpr std::time_t midnightUtc = 1776470400; // 2026-04-18T00:00:00Z
    const std::string utc = "2026-04-18 00:00:00 UTC";
    const std::string iso = "2026-04-18T00:00:00Z";

    CHECK(parseRuntimeTimestamp(utc) == midnightUtc);
    CHECK(parseRuntimeTimestamp(iso) == midnightUtc);

    CHECK(formatRuntimeTimeAgo(utc, midnightUtc + 120) == "2 mins ago");
    CHECK(formatRuntimeTimeAgo(iso, midnightUtc + 7200) == "2 hours ago");
}
