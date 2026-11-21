#include "core/RuntimeStatus.hpp"

#include <iomanip>
#include <sstream>

namespace JiStats {

namespace {

std::time_t utcTmToTimeT(std::tm& tm) {
#ifdef _WIN32
    return _mkgmtime(&tm);
#else
    return timegm(&tm);
#endif
}

} // namespace

RuntimeState deriveRuntimeState(const std::vector<ProviderState>& providerStates) {
    if (providerStates.empty()) {
        return RuntimeState::Unknown;
    }

    bool anyOffline = false;
    bool anyStale = false;
    bool anySyncing = false;
    bool anyCurrent = false;

    for (const ProviderState state : providerStates) {
        switch (state) {
            case ProviderState::Syncing:
                anySyncing = true;
                break;
            case ProviderState::Offline:
                anyOffline = true;
                break;
            case ProviderState::Stale:
                anyStale = true;
                break;
            case ProviderState::Sample:
            case ProviderState::Live:
                anyCurrent = true;
                break;
            default:
                break;
        }
    }

    if (anySyncing) {
        return RuntimeState::CatchingUp;
    }
    if (anyOffline) {
        return RuntimeState::Offline;
    }
    if (anyStale) {
        return RuntimeState::Stale;
    }
    if (anyCurrent) {
        return RuntimeState::Current;
    }
    return RuntimeState::Unknown;
}

RuntimeStatusTransition describeRuntimeStateTransition(RuntimeState previous, RuntimeState current) {
    RuntimeStatusTransition transition;
    transition.previous = previous;
    transition.current = current;
    transition.enteredOffline =
        previous != RuntimeState::Offline && current == RuntimeState::Offline;
    transition.exitedOffline =
        previous == RuntimeState::Offline && current != RuntimeState::Offline;
    return transition;
}

std::string providerStateLabel(ProviderState state) {
    switch (state) {
        case ProviderState::Sample: return "Sample";
        case ProviderState::Offline: return "Offline";
        case ProviderState::Stale: return "Stale";
        case ProviderState::Syncing: return "Syncing";
        case ProviderState::Live: return "Live";
        default: return "Unknown";
    }
}

std::string providerStateCompactLabel(ProviderState state) {
    switch (state) {
        case ProviderState::Sample: return "Smp";
        case ProviderState::Offline: return "Off";
        case ProviderState::Stale: return "Stl";
        case ProviderState::Syncing: return "Sync";
        case ProviderState::Live: return "Live";
        default: return "Unk";
    }
}

std::string providerStateIndicator(ProviderState state) {
    switch (state) {
        case ProviderState::Offline:
            return "!";
        case ProviderState::Stale:
        case ProviderState::Syncing:
            return "~";
        case ProviderState::Sample:
        case ProviderState::Live:
            return "+";
        default:
            return "?";
    }
}

std::string runtimeStateLabel(RuntimeState state) {
    switch (state) {
        case RuntimeState::Current: return "Current";
        case RuntimeState::Stale: return "Stale";
        case RuntimeState::CatchingUp: return "Catching Up";
        case RuntimeState::Offline: return "Offline";
        default: return "Unknown";
    }
}

std::string runtimeStateCompactLabel(RuntimeState state) {
    switch (state) {
        case RuntimeState::Current: return "Now";
        case RuntimeState::Stale: return "Stl";
        case RuntimeState::CatchingUp: return "Sync";
        case RuntimeState::Offline: return "Off";
        default: return "Unk";
    }
}

std::string runtimeStateIndicator(RuntimeState state) {
    switch (state) {
        case RuntimeState::Offline:
            return "!";
        case RuntimeState::Stale:
        case RuntimeState::CatchingUp:
            return "~";
        case RuntimeState::Current:
            return "+";
        default:
            return "?";
    }
}

bool runtimeStateIsOnline(RuntimeState state) {
    return state == RuntimeState::Current ||
           state == RuntimeState::Stale ||
           state == RuntimeState::CatchingUp;
}

std::time_t parseRuntimeTimestamp(const std::string& timestamp) {
    if (timestamp.empty()) {
        return 0;
    }

    const char* const formats[] = {
        "%Y-%m-%dT%H:%M:%SZ",
        "%Y-%m-%d %H:%M:%S UTC"
    };

    for (const char* format : formats) {
        std::tm tm{};
        std::istringstream stream(timestamp);
        stream >> std::get_time(&tm, format);
        if (stream.fail()) {
            continue;
        }

        const std::time_t parsed = utcTmToTimeT(tm);
        if (parsed != static_cast<std::time_t>(-1)) {
            return parsed;
        }
    }

    return 0;
}

bool isRuntimeTimestampStale(
    const std::string& timestamp,
    std::time_t now,
    std::time_t staleThresholdSeconds) {
    const std::time_t parsed = parseRuntimeTimestamp(timestamp);
    if (parsed == 0) {
        return true;
    }
    return (now - parsed) > staleThresholdSeconds;
}

std::string formatRuntimeTimeAgo(const std::string& timestamp, std::time_t now) {
    if (timestamp.empty()) {
        return "Never";
    }

    const std::time_t parsed = parseRuntimeTimestamp(timestamp);
    if (parsed == 0) {
        return "Unknown";
    }

    if (now == 0) {
        now = std::time(nullptr);
    }

    const int elapsed = static_cast<int>(now - parsed);
    if (elapsed <= 60) {
        return "Just now";
    }
    if (elapsed < 3600) {
        const int mins = elapsed / 60;
        return std::to_string(mins) + (mins == 1 ? " min ago" : " mins ago");
    }
    if (elapsed < 86400) {
        const int hours = elapsed / 3600;
        return std::to_string(hours) + (hours == 1 ? " hour ago" : " hours ago");
    }

    const int days = elapsed / 86400;
    return std::to_string(days) + (days == 1 ? " day ago" : " days ago");
}

} // namespace JiStats
