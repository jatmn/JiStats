#pragma once

#include "analytics/MirrorEngine.hpp"
#include <string>
#include <memory>
#include <vector>
#include <ctime>

namespace JiStats {

// Analytics engine for computing metrics and trends
class AnalyticsEngine {
public:
    AnalyticsEngine();
    ~AnalyticsEngine();
    
    // Compute daily usage totals
    struct DailyUsage {
        std::string date; // YYYY-MM-DD
        long long totalTokens = 0;
        long long totalCost = 0;
        int requestCount = 0;
        std::string providerKey;
    };
    
    std::vector<DailyUsage> computeDailyUsage(
        const std::vector<UsageEvent>& events,
        const std::string& providerKey,
        std::time_t from,
        std::time_t to
    );
    
    // Compute usage by model
    struct ModelUsage {
        std::string modelKey;
        long long totalTokens = 0;
        long long totalCost = 0;
        int requestCount = 0;
    };
    
    std::vector<ModelUsage> computeModelUsage(
        const std::vector<UsageEvent>& events,
        std::time_t from,
        std::time_t to
    );
    
    // Compute cost breakdown
    struct CostBreakdown {
        long long totalCost = 0;
        long long inputCost = 0;
        long long outputCost = 0;
        double inputPercentage = 0.0;
        double outputPercentage = 0.0;
    };
    
    CostBreakdown computeCostBreakdown(
        const std::vector<UsageEvent>& events,
        std::time_t from,
        std::time_t to
    );
    
    // Get usage trend
    enum class Trend {
        Increasing,
        Decreasing,
        Stable
    };
    
    Trend getUsageTrend(
        const std::vector<UsageEvent>& events,
        std::time_t from,
        std::time_t to,
        std::time_t compareFrom,
        std::time_t compareTo
    );
    
    // Compute peak usage hours
    struct HourlyUsage {
        int hour = 0; // 0-23
        int requestCount = 0;
        long long totalTokens = 0;
    };
    
    std::vector<HourlyUsage> computePeakHours(
        const std::vector<UsageEvent>& events,
        std::time_t from,
        std::time_t to
    );
    
    // Estimate monthly cost
    long long estimateMonthlyCost(
        const std::vector<UsageEvent>& events,
        std::time_t from,
        std::time_t to
    );
    
private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace JiStats