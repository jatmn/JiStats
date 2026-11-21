#pragma once

#include <optional>
#include <string>

namespace JiStats {

enum class GapClassification {
    KnownZero,
    Reconstructed,
    Unknown
};

std::string toString(GapClassification classification);
std::optional<GapClassification> gapClassificationFromString(const std::string& value);

struct GapClassificationInput {
    bool appWasRunning = false;
    bool providerBackfillAvailable = false;
    bool providerConfirmedNoActivity = false;
    bool hasLocalActivity = false;
};

using GapClassificationInputs = GapClassificationInput;

struct GapClassificationResult {
    GapClassification classification = GapClassification::Unknown;
    std::string basisSummary;
};

class GapClassifier {
public:
    static GapClassificationResult classify(const GapClassificationInput& input);
};

GapClassification classifyGap(const GapClassificationInputs& input);

} // namespace JiStats
