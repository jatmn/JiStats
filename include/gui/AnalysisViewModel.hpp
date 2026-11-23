#pragma once

#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace JiStats {

enum class AnalysisDateRangePreset {
    Today = 0,
    Last24Hours = 1,
    Last7Days = 2,
    CurrentQuotaWindow = 3,
    ThisMonth = 4,
    Custom = 5
};

struct AnalysisScopeOption {
    std::string key;
    std::string label;
};

struct AnalysisScopeState {
    std::string selectedProviderId;
    std::string selectedAccountId;
    AnalysisDateRangePreset dateRangePreset = AnalysisDateRangePreset::Last7Days;
    std::time_t customRangeStartUtc = 0;
    std::time_t customRangeEndUtc = 0;
};

enum class AnalysisGapClassification {
    None = 0,
    KnownZero = 1,
    Reconstructed = 2,
    Unknown = 3
};

enum class AnalysisTrendBucketGranularity {
    Hourly = 0,
    Daily = 1,
    Weekly = 2,
    Monthly = 3
};

struct AnalysisGapDetail {
    AnalysisGapClassification classification = AnalysisGapClassification::None;
    std::string classificationLabel;
    std::string basisSummary;
    std::string sourceKind;
};

struct AnalysisDailySummary {
    std::string dateLabel;
    long long activityCount = 0;
    std::string sourceLabel = "Unknown";
    AnalysisGapDetail gapDetail;
};

struct AnalysisBreakdownRow {
    std::string key;
    std::string label;
    std::string secondaryLabel;
    long long activityCount = 0;
    std::string sourceLabel = "Unknown";
};

struct AnalysisTrendPoint {
    std::string dateLabel;
    long long activityCount = 0;
    std::time_t bucketStartUtc = 0;
    std::time_t bucketEndUtc = 0;
    std::string sourceLabel = "Unknown";
};

struct AnalysisQuotaTrendPoint {
    std::string label;
    std::time_t snapshotTime = 0;
    double remainingPercentage = -1.0;
    long long remainingValue = 0;
    long long limitValue = 0;
    bool resetPoint = false;
    bool reconciled = false;
    bool dailyRollup = false;
};

struct AnalysisQuotaTrendSeries {
    std::string key;
    std::string label;
    std::string quotaDimension = "requests";
    std::string windowLabel;
    std::vector<AnalysisQuotaTrendPoint> points;
    double latestRemainingPercentage = -1.0;
    double lowWaterRemainingPercentage = -1.0;
    long long latestRemainingValue = 0;
    long long limitValue = 0;
    int resetCount = 0;
};

struct AnalysisGapSummaryCounts {
    long long knownZeroDays = 0;
    long long reconstructedDays = 0;
    long long unknownDays = 0;
    long long unknownSourceDays = 0;
};

struct AnalysisCodexThroughput {
    double tokensPerMinute5m = 0.0;
    double promptsPerMinute5m = 0.0;
    double tokensPerMinute30m = 0.0;
    double promptsPerMinute30m = 0.0;
    long long tokensLast1h = 0;
    long long promptsLast1h = 0;
    long long averageDailyTokens = 0;
    long long averageDailyPrompts = 0;
    long long averageMonthlyTokens = 0;
    long long averageMonthlyPrompts = 0;
    long long calendarMonthTokens = 0;
    long long calendarMonthPrompts = 0;
    long long rolling30DayTokens = 0;
    long long rolling30DayPrompts = 0;
    std::string confidenceLabel = "Unknown";
    bool hasData = false;
};

struct AnalysisCodexMinutePoint {
    std::time_t snapshotTime = 0;
    double tokensPerMinute = 0.0;
    double promptsPerMinute = 0.0;
};

struct AnalysisCodexHourlyHistory {
    std::vector<AnalysisCodexMinutePoint> points;
    bool hasData = false;
};

struct AnalysisModelUsagePoint {
    std::time_t bucketStartUtc = 0;
    long long totalTokens = 0;
};

struct AnalysisModelUsageSeries {
    std::string modelKey;
    long long totalTokens = 0;
    std::vector<AnalysisModelUsagePoint> points;
};

struct AnalysisProviderModelUsageChart {
    std::string providerKey;
    std::string title;
    std::string subtitle;
    std::vector<AnalysisModelUsageSeries> series;
    bool hasData = false;
};

struct AnalysisViewModel {
    AnalysisScopeState scope;
    std::vector<AnalysisScopeOption> availableProviders;
    std::vector<AnalysisScopeOption> availableAccounts;
    std::vector<AnalysisDailySummary> dailySummaries;
    std::vector<AnalysisBreakdownRow> providerBreakdown;
    std::vector<AnalysisBreakdownRow> accountBreakdown;
    AnalysisTrendBucketGranularity trendBucketGranularity = AnalysisTrendBucketGranularity::Daily;
    std::string trendBucketLabel = "Daily";
    std::vector<AnalysisTrendPoint> trendPoints;
    std::vector<AnalysisQuotaTrendSeries> quotaTrendSeries;
    AnalysisGapSummaryCounts gapSummaryCounts;
    AnalysisCodexThroughput codexThroughput;
    AnalysisCodexHourlyHistory codexHourlyHistory;
    AnalysisProviderModelUsageChart openAiCodexModelUsage;
    AnalysisProviderModelUsageChart hicapCodexModelUsage;
    std::string scopeLabel;
    std::string dataQualityNote;
    // Echo the request separately from any provider, account, or date fallback.
    std::optional<AnalysisScopeState> requestedScope;
    std::time_t effectiveStartUtc = 0;
    std::time_t effectiveEndUtc = 0;
};

} // namespace JiStats
