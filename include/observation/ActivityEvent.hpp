#pragma once

#include <ctime>
#include <string>

namespace JiStats {

enum class ActivityKind {
    SessionActive,
    MessageSend,
    ToolRun,
    UnknownActivity
};

enum class ActivityConfidence {
    LocalExact,
    Imported,
    Estimated,
    Unknown
};

struct ActivityEvent {
    std::string providerKey;
    std::string providerSurface;
    ActivityKind activityKind = ActivityKind::UnknownActivity;
    std::time_t startedAt = 0;
    std::time_t endedAt = 0;
    long long durationSeconds = 0;
    std::string sourceType = "local";
    std::string sourceRunId;
    std::string surfaceKey;
    std::string windowTitleHint;
    ActivityConfidence confidence = ActivityConfidence::LocalExact;
    std::string rawReference;

    bool isCompleted() const {
        return endedAt >= startedAt && startedAt > 0;
    }
};

std::string toString(ActivityKind kind);
std::string toString(ActivityConfidence confidence);

} // namespace JiStats
