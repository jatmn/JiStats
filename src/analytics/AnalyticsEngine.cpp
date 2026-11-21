#include "analytics/AnalyticsEngine.hpp"
#include <algorithm>
#include <map>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace JiStats {

class AnalyticsEngine::Impl {
public:
    // Convert time_t to date string YYYY-MM-DD
    static std::string timeToDate(std::time_t time) {
        std::tm tm;
        gmtime_s(&tm, &time);
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d");
        return oss.str();
    }
    
    // Get hour from time_t
    static int getHour(std::time_t time) {
        std::tm tm;
        gmtime_s(&tm, &time);
        return tm.tm_hour;
    }
};

AnalyticsEngine::AnalyticsEngine() : pImpl(std::make_unique<Impl>()) {
}

AnalyticsEngine::~AnalyticsEngine() = default;

std::vector<AnalyticsEngine::DailyUsage> AnalyticsEngine::computeDailyUsage(
    const std::vector<UsageEvent>& events,
    const std::string& providerKey,
    std::time_t from,
    std::time_t to
) {
    std::map<std::string, DailyUsage> dailyMap;
    
    // Filter and aggregate by day
    for (const auto& e : events) {
        if (e.eventTime >= from && e.eventTime <= to) {
            std::string date = pImpl->timeToDate(e.eventTime);
            auto& usage = dailyMap[date];
            usage.date = date;
            usage.providerKey = providerKey;
            usage.totalTokens += e.totalTokens;
            usage.totalCost += e.costMinorUnits;
            usage.requestCount++;
        }
    }
    
    // Convert to vector and sort by date
    std::vector<DailyUsage> result;
    for (auto& pair : dailyMap) {
        result.push_back(pair.second);
    }
    
    std::sort(result.begin(), result.end(),
        [](const DailyUsage& a, const DailyUsage& b) {
            return a.date < b.date;
        });
    
    return result;
}

std::vector<AnalyticsEngine::ModelUsage> AnalyticsEngine::computeModelUsage(
    const std::vector<UsageEvent>& events,
    std::time_t from,
    std::time_t to
) {
    std::map<std::string, ModelUsage> modelMap;
    
    // Aggregate by model
    for (const auto& e : events) {
        if (e.eventTime >= from && e.eventTime <= to && !e.modelKey.empty()) {
            auto& usage = modelMap[e.modelKey];
            usage.modelKey = e.modelKey;
            usage.totalTokens += e.totalTokens;
            usage.totalCost += e.costMinorUnits;
            usage.requestCount++;
        }
    }
    
    // Convert to vector
    std::vector<ModelUsage> result;
    for (auto& pair : modelMap) {
        result.push_back(pair.second);
    }
    
    // Sort by token usage descending
    std::sort(result.begin(), result.end(),
        [](const ModelUsage& a, const ModelUsage& b) {
            return a.totalTokens > b.totalTokens;
        });
    
    return result;
}

AnalyticsEngine::CostBreakdown AnalyticsEngine::computeCostBreakdown(
    const std::vector<UsageEvent>& events,
    std::time_t from,
    std::time_t to
) {
    CostBreakdown breakdown;
    
    for (const auto& e : events) {
        if (e.eventTime >= from && e.eventTime <= to) {
            breakdown.totalCost += e.costMinorUnits;
            breakdown.inputCost += e.inputTokens;
            breakdown.outputCost += e.outputTokens;
        }
    }
    
    long long totalTokens = breakdown.inputCost + breakdown.outputCost;
    if (totalTokens > 0) {
        breakdown.inputPercentage = (static_cast<double>(breakdown.inputCost) / totalTokens) * 100.0;
        breakdown.outputPercentage = (static_cast<double>(breakdown.outputCost) / totalTokens) * 100.0;
    }
    
    return breakdown;
}

AnalyticsEngine::Trend AnalyticsEngine::getUsageTrend(
    const std::vector<UsageEvent>& events,
    std::time_t from,
    std::time_t to,
    std::time_t compareFrom,
    std::time_t compareTo
) {
    long long currentUsage = 0;
    long long compareUsage = 0;
    
    // Sum usage for current period
    for (const auto& e : events) {
        if (e.eventTime >= from && e.eventTime <= to) {
            currentUsage += e.totalTokens;
        }
    }
    
    // Sum usage for comparison period
    for (const auto& e : events) {
        if (e.eventTime >= compareFrom && e.eventTime <= compareTo) {
            compareUsage += e.totalTokens;
        }
    }
    
    // Calculate trend
    double changePercent = 0.0;
    if (compareUsage > 0) {
        changePercent = (static_cast<double>(currentUsage - compareUsage) / compareUsage) * 100.0;
    }
    
    // Threshold for trend detection (10%)
    if (changePercent > 10.0) {
        return Trend::Increasing;
    } else if (changePercent < -10.0) {
        return Trend::Decreasing;
    }
    
    return Trend::Stable;
}

std::vector<AnalyticsEngine::HourlyUsage> AnalyticsEngine::computePeakHours(
    const std::vector<UsageEvent>& events,
    std::time_t from,
    std::time_t to
) {
    std::map<int, HourlyUsage> hourMap;
    
    // Initialize all hours
    for (int hour = 0; hour < 24; hour++) {
        hourMap[hour].hour = hour;
    }
    
    // Aggregate by hour
    for (const auto& e : events) {
        if (e.eventTime >= from && e.eventTime <= to) {
            int hour = pImpl->getHour(e.eventTime);
            auto& usage = hourMap[hour];
            usage.requestCount++;
            usage.totalTokens += e.totalTokens;
        }
    }
    
    // Convert to vector
    std::vector<HourlyUsage> result;
    for (int hour = 0; hour < 24; hour++) {
        result.push_back(hourMap[hour]);
    }
    
    // Sort by request count descending
    std::sort(result.begin(), result.end(),
        [](const HourlyUsage& a, const HourlyUsage& b) {
            return a.requestCount > b.requestCount;
        });
    
    return result;
}

long long AnalyticsEngine::estimateMonthlyCost(
    const std::vector<UsageEvent>& events,
    std::time_t from,
    std::time_t to
) {
    long long periodCost = 0;
    long long periodSeconds = to - from;
    
    // Sum cost for the period
    for (const auto& e : events) {
        if (e.eventTime >= from && e.eventTime <= to) {
            periodCost += e.costMinorUnits;
        }
    }
    
    // Estimate for 30 days
    if (periodSeconds > 0) {
        long long secondsPerMonth = 30 * 24 * 60 * 60;
        return (periodCost * secondsPerMonth) / periodSeconds;
    }
    
    return 0;
}

} // namespace JiStats