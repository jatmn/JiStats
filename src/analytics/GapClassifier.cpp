#include "analytics/GapClassifier.hpp"

namespace JiStats {

std::string toString(GapClassification classification) {
    switch (classification) {
    case GapClassification::KnownZero:
        return "known_zero";
    case GapClassification::Reconstructed:
        return "reconstructed";
    case GapClassification::Unknown:
    default:
        return "unknown";
    }
}

std::optional<GapClassification> gapClassificationFromString(const std::string& value) {
    if (value == "known_zero") {
        return GapClassification::KnownZero;
    }
    if (value == "reconstructed") {
        return GapClassification::Reconstructed;
    }
    if (value == "unknown") {
        return GapClassification::Unknown;
    }
    return std::nullopt;
}

GapClassificationResult GapClassifier::classify(const GapClassificationInput& input) {
    if (input.providerBackfillAvailable) {
        return {GapClassification::Reconstructed,
                "Provider backfill is available for the missing period"};
    }

    if (input.appWasRunning && input.providerConfirmedNoActivity && !input.hasLocalActivity) {
        return {GapClassification::KnownZero,
                "App was running and provider confirmed no activity"};
    }

    if (input.hasLocalActivity) {
        return {GapClassification::Unknown,
                "Local activity exists without provider backfill"};
    }

    if (!input.appWasRunning) {
        return {GapClassification::Unknown,
                "App was not running and no provider backfill is available"};
    }

    return {GapClassification::Unknown,
            "Provider did not confirm a known-zero period"};
}

GapClassification classifyGap(const GapClassificationInputs& input) {
    return GapClassifier::classify(input).classification;
}

} // namespace JiStats
