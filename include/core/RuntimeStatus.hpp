#pragma once

#include <ctime>
#include <string>
#include <vector>

namespace JiStats {

enum class ProviderState { Sample, Offline, Stale, Syncing, Live, Unknown };

enum class RuntimeState {
    Current,
    Stale,
    CatchingUp,
    Offline,
    Unknown
};

struct RuntimeStatusTransition {
    RuntimeState previous = RuntimeState::Unknown;
    RuntimeState current = RuntimeState::Unknown;
    bool enteredOffline = false;
    bool exitedOffline = false;
};

RuntimeState deriveRuntimeState(const std::vector<ProviderState>& providerStates);
RuntimeStatusTransition describeRuntimeStateTransition(RuntimeState previous, RuntimeState current);

std::string providerStateLabel(ProviderState state);
std::string providerStateCompactLabel(ProviderState state);
std::string providerStateIndicator(ProviderState state);

std::string runtimeStateLabel(RuntimeState state);
std::string runtimeStateCompactLabel(RuntimeState state);
std::string runtimeStateIndicator(RuntimeState state);
bool runtimeStateIsOnline(RuntimeState state);

std::time_t parseRuntimeTimestamp(const std::string& timestamp);
bool isRuntimeTimestampStale(
    const std::string& timestamp,
    std::time_t now,
    std::time_t staleThresholdSeconds = 3600);
std::string formatRuntimeTimeAgo(const std::string& timestamp, std::time_t now = 0);

} // namespace JiStats
