#include "analytics/MirrorEngine.hpp"
#include "providers/IProviderAdapter.hpp"
#include <algorithm>
#include <numeric>
#include <iterator>

namespace JiStats {

class MirrorEngine::Impl {
public:
    // Count usage events in time range
    static size_t countEventsInRange(
        const std::vector<UsageEvent>& events,
        std::time_t from,
        std::time_t to
    ) {
        return std::count_if(events.begin(), events.end(),
            [from, to](const UsageEvent& e) {
                return e.eventTime >= from && e.eventTime <= to;
            });
    }
    
    // Sum tokens in time range
    static long long sumTokensInRange(
        const std::vector<UsageEvent>& events,
        std::time_t from,
        std::time_t to
    ) {
        long long sum = 0;
        for (const auto& e : events) {
            if (e.eventTime >= from && e.eventTime <= to) {
                sum += e.totalTokens;
            }
        }
        return sum;
    }
    
    // Sum cost in time range
    static long long sumCostInRange(
        const std::vector<UsageEvent>& events,
        std::time_t from,
        std::time_t to
    ) {
        long long sum = 0;
        for (const auto& e : events) {
            if (e.eventTime >= from && e.eventTime <= to) {
                sum += e.costMinorUnits;
            }
        }
        return sum;
    }
};

MirrorEngine::MirrorEngine() : pImpl(std::make_unique<Impl>()) {
}

MirrorEngine::~MirrorEngine() = default;

MirrorOutput MirrorEngine::computeMirror(const MirrorInput& input) {
    MirrorOutput output;
    output.success = false;
    
    if (input.windowStart >= input.windowEnd) {
        output.error = "Invalid time window";
        return output;
    }
    
    // Combine local and imported events
    std::vector<UsageEvent> allEvents;
    allEvents.insert(allEvents.end(), input.localEvents.begin(), input.localEvents.end());
    allEvents.insert(allEvents.end(), input.importedEvents.begin(), input.importedEvents.end());
    
    // Sort by time
    std::sort(allEvents.begin(), allEvents.end(),
        [](const UsageEvent& a, const UsageEvent& b) {
            return a.eventTime < b.eventTime;
        });
    
    // Compute local totals
    long long localUsage = pImpl->sumTokensInRange(
        allEvents, input.windowStart, input.windowEnd);
    
    // Classify confidence
    output.tier = ConfidenceTier::Medium; // Default to medium
    
    // If provider quota is available, use it
    if (!output.snapshot.providerKey.empty()) {
        output.snapshot.used = localUsage;
        output.snapshot.remaining = output.snapshot.limit - output.snapshot.used;
        output.tier = ConfidenceTier::High;
        output.basisSummary = "Provider quota signal with local events";
    } else {
        output.basisSummary = "Local events only (no provider quota)";
    }
    
    output.success = true;
    return output;
}

MirrorOutput MirrorEngine::reconcile(
    const std::vector<UsageEvent>& localEvents,
    const std::vector<UsageEvent>& importedEvents,
    const QuotaResponse& providerQuota,
    std::time_t windowStart,
    std::time_t windowEnd
) {
    MirrorOutput output;
    
    // Combine events
    std::vector<UsageEvent> allEvents;
    allEvents.insert(allEvents.end(), localEvents.begin(), localEvents.end());
    allEvents.insert(allEvents.end(), importedEvents.begin(), importedEvents.end());
    
    // Sort by time
    std::sort(allEvents.begin(), allEvents.end(),
        [](const UsageEvent& a, const UsageEvent& b) {
            return a.eventTime < b.eventTime;
        });
    
    // Compute local totals
    long long localUsage = pImpl->sumTokensInRange(allEvents, windowStart, windowEnd);
    [[maybe_unused]] long long localCost = pImpl->sumCostInRange(allEvents, windowStart, windowEnd);
    
    // Build snapshot
    output.snapshot.snapshotTime = std::time(nullptr);
    output.snapshot.windowStart = windowStart;
    output.snapshot.windowEnd = windowEnd;
    output.snapshot.events = allEvents;
    
    if (providerQuota.success) {
        // Use provider quota as source of truth
        output.snapshot.used = providerQuota.used;
        output.snapshot.limit = providerQuota.limit;
        output.snapshot.remaining = providerQuota.remaining;
        
        // Check if local events add to provider data
        // (Provider might not have all local events)
        if (localUsage > providerQuota.used) {
            // Local events exceed provider - might be newer data
            output.snapshot.used = localUsage;
            output.snapshot.remaining = output.snapshot.limit - output.snapshot.used;
        }
        
        output.tier = ConfidenceTier::High;
        output.basisSummary = "Provider quota reconciled with local events";
    } else {
        // Fall back to local events only
        output.snapshot.used = localUsage;
        output.snapshot.limit = 0;
        output.snapshot.remaining = 0;
        
        output.tier = localEvents.empty() ? ConfidenceTier::Low : ConfidenceTier::Medium;
        output.basisSummary = localEvents.empty() 
            ? "No provider quota, no local events" 
            : "Local events only, no provider quota";
    }
    
    output.success = true;
    return output;
}

MirrorEngine::RollingStats MirrorEngine::getRollingWindow(
    const std::vector<UsageEvent>& events,
    std::time_t from,
    std::time_t to
) {
    RollingStats stats;
    stats.windowStart = from;
    stats.windowEnd = to;
    
    // Filter events in range
    std::vector<UsageEvent> windowEvents;
    std::copy_if(events.begin(), events.end(), std::back_inserter(windowEvents),
        [from, to](const UsageEvent& e) {
            return e.eventTime >= from && e.eventTime <= to;
        });
    
    // Compute stats
    stats.eventCount = static_cast<int>(windowEvents.size());
    stats.totalUsage = pImpl->sumTokensInRange(windowEvents, from, to);
    stats.totalCost = pImpl->sumCostInRange(windowEvents, from, to);
    
    // Compute confidence score (simple heuristic)
    if (stats.eventCount == 0) {
        stats.confidenceScore = 0.0;
    } else if (stats.eventCount < 5) {
        stats.confidenceScore = 0.5;
    } else if (stats.eventCount < 20) {
        stats.confidenceScore = 0.75;
    } else {
        stats.confidenceScore = 1.0;
    }
    
    return stats;
}

MirrorEngine::ConfidenceTier MirrorEngine::classifyConfidence(
    const QuotaResponse& providerQuota,
    size_t localEventCount,
    bool hasImportedData
) {
    if (providerQuota.success) {
        return ConfidenceTier::High;
    }
    
    if (hasImportedData && localEventCount > 0) {
        return ConfidenceTier::Medium;
    }
    
    if (localEventCount > 0) {
        return ConfidenceTier::Medium;
    }
    
    return ConfidenceTier::Low;
}

std::string MirrorEngine::tierToString(ConfidenceTier tier) {
    switch (tier) {
        case ConfidenceTier::High:
            return "High";
        case ConfidenceTier::Medium:
            return "Medium";
        case ConfidenceTier::Low:
            return "Low";
        default:
            return "Unknown";
    }
}

} // namespace JiStats
