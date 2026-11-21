#pragma once

#include "providers/IProviderAdapter.hpp"
#include <string>
#include <memory>
#include <vector>
#include <ctime>

namespace JiStats {

// Forward declarations
struct MirrorInput;
struct MirrorOutput;
struct QuotaSnapshot;
struct UsageEvent;

// Mirror engine for computing quota mirrors
class MirrorEngine {
public:
    MirrorEngine();
    ~MirrorEngine();
    
    // Compute mirror for a given time window
    MirrorOutput computeMirror(const MirrorInput& input);
    
    // Reconcile local events with provider quota
    MirrorOutput reconcile(
        const std::vector<UsageEvent>& localEvents,
        const std::vector<UsageEvent>& importedEvents,
        const QuotaResponse& providerQuota,
        std::time_t windowStart,
        std::time_t windowEnd
    );
    
    // Get rolling window statistics
    struct RollingStats {
        long long totalUsage = 0;
        long long totalCost = 0;
        std::time_t windowStart = 0;
        std::time_t windowEnd = 0;
        int eventCount = 0;
        double confidenceScore = 0.0;
    };
    
    RollingStats getRollingWindow(
        const std::vector<UsageEvent>& events,
        std::time_t from,
        std::time_t to
    );
    
    // Classify confidence tier
    enum class ConfidenceTier {
        High,      // Exact provider signal
        Medium,    // Derived or estimated
        Low        // Guess based on partial data
    };
    
    static ConfidenceTier classifyConfidence(
        const QuotaResponse& providerQuota,
        size_t localEventCount,
        bool hasImportedData
    );
    
    // Get tier as string
    static std::string tierToString(ConfidenceTier tier);
    
private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

// Quota snapshot
struct QuotaSnapshot {
    std::time_t snapshotTime = 0;
    std::time_t windowStart = 0;
    std::time_t windowEnd = 0;
    long long used = 0;
    long long limit = 0;
    long long remaining = 0;
    std::string providerKey;
    std::string planKey;
    std::vector<UsageEvent> events;
};

// Input for mirror computation
struct MirrorInput {
    std::vector<UsageEvent> localEvents;
    std::vector<UsageEvent> importedEvents;
    std::time_t windowStart = 0;
    std::time_t windowEnd = 0;
    std::string providerKey;
};

// Output from mirror computation
struct MirrorOutput {
    QuotaSnapshot snapshot;
    MirrorEngine::ConfidenceTier tier;
    std::string basisSummary;
    bool success = false;
    std::string error;
};

} // namespace JiStats