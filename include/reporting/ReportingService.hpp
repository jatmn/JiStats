#pragma once

#include "core/RuntimeStatus.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace JiStats {

struct DashboardViewModel;

/**
 * Report format options.
 */
enum class ReportFormat {
    Html,
    Csv,
    Json
};

/**
 * Date range for report generation.
 */
struct ReportDateRange {
    std::chrono::system_clock::time_point start;
    std::chrono::system_clock::time_point end;
};

/**
 * Report request parameters.
 */
struct ReportRequest {
    ReportFormat format = ReportFormat::Html;
    ReportDateRange dateRange;
    std::string outputPath;
    bool includeProviderQuotas = true;
    bool includeUsageHistory = true;
    bool includeTrends = true;
    std::vector<std::string> providerFilter;  // Empty = all providers
    std::vector<std::string> accountFilter;   // Empty = all accounts
};

/**
 * Report metadata containing freshness, backfill, and confidence labeling.
 * Per docs/reference/reporting.md, reports must include:
 * - provider and account scope
 * - data freshness timestamp
 * - whether the report contains backfilled data
 * - whether the report contains unknown gaps
 * - the mirror confidence policy version used to generate quota sections
 */
struct ReportMetadata {
    // Provider/account scope
    std::vector<std::string> providerKeys;
    std::vector<std::string> accountIds;
    std::vector<std::string> providerSurfaces;
    bool accountScopeComplete = false;

    // Data freshness
    std::optional<std::chrono::system_clock::time_point> dataFreshnessTimestamp;
    RuntimeState runtimeState = RuntimeState::Unknown;
    bool isStale = false;
    bool isOffline = false;
    bool isSyncing = false;

    // Backfill state
    bool containsBackfilledData = false;
    bool containsUnknownGaps = false;

    // Mirror confidence policy version
    std::string mirrorPolicyVersion = "1.0";

    // Summary labels for UI display
    std::string freshnessLabel() const;
    std::string backfillLabel() const;
    std::string confidenceLabel() const;
    std::string scopeLabel() const;
};

struct ReportSummaryBucket {
    std::string label;
    std::string periodLabel;
    long long requestCount = 0;
    long long totalTokens = 0;
    long long totalCostMinorUnits = 0;
};

struct ReportBreakdownRow {
    std::string key;
    std::string label;
    std::string secondaryLabel;
    long long requestCount = 0;
    long long totalTokens = 0;
    long long totalCostMinorUnits = 0;
};

struct ReportTrendPoint {
    std::string label;
    long long requestCount = 0;
    long long totalTokens = 0;
    long long totalCostMinorUnits = 0;
};

struct ReportQuotaSnapshot {
    std::string providerKey;
    std::string providerDisplayName;
    std::string accountLabel;
    std::string planName;
    std::string windowLabel;
    std::string lastSyncLabel;
    std::string confidenceTier;
    std::string sourceLabel;
    long long used = 0;
    long long limit = 0;
    long long remaining = 0;
    double remainingPercentage = -1.0;
    bool isBackfilled = false;
    bool hasUnknownGaps = false;
};

struct ReportRecoveryCoverage {
    bool containsBackfilledData = false;
    bool containsUnknownGaps = false;
};

struct ReportAggregateData {
    std::string dateRangeLabel;
    std::string gapBackfillSummary;
    std::string quotaSnapshotTitle = "Current Quota Mirror Snapshots";
    std::string quotaSnapshotDetail =
        "These quota cards reflect current provider state at export time and may not match the historical report window.";
    std::vector<ReportSummaryBucket> dailySummaries;
    std::vector<ReportSummaryBucket> weeklySummaries;
    std::vector<ReportBreakdownRow> providerBreakdown;
    std::vector<ReportBreakdownRow> accountBreakdown;
    std::vector<ReportBreakdownRow> modelBreakdown;
    std::vector<ReportTrendPoint> recentTrend;
    std::vector<ReportQuotaSnapshot> quotaSnapshots;
    std::optional<ReportRecoveryCoverage> recoveryCoverage;
};

/**
 * Report generation result.
 */
struct ReportResult {
    bool success = false;
    std::string outputPath;
    std::string errorMessage;
    size_t bytesWritten = 0;
    ReportMetadata metadata;
    ReportAggregateData aggregateData;
};

/**
 * ReportingService handles report generation per docs/reference/reporting.md.
 *
 * Reports are export artifacts (not live viewing).
 * HTML is the primary v1.0 format.
 *
 * Per docs/reference/reporting.md:
 * - Reports include generation timestamp
 * - Data source attribution (provider, API, local)
 * - Confidence indicators for mirrored data
 * - Backfill labeling for reconstructed data
 */
class ReportingService {
public:
    ReportingService();
    ~ReportingService();
    
    // Non-copyable
    ReportingService(const ReportingService&) = delete;
    ReportingService& operator=(const ReportingService&) = delete;
    
    /**
     * Generate a report from the current dashboard data.
     * @param request Report parameters
     * @return Result indicating success/failure and output path
     */
    ReportResult generateReport(const ReportRequest& request);

    /**
     * Set the data provider used to populate report content.
     * The provider should populate viewModel and return true on success.
     */
    void setDashboardDataProvider(std::function<bool(DashboardViewModel&)> provider);

    /**
     * Set the mirror policy version provider used to populate export metadata.
     * The provider may return an empty string when no trustworthy version is available.
     */
    void setMirrorPolicyVersionProvider(
        std::function<std::string(const DashboardViewModel&)> provider);

    /**
     * Set the aggregate-data provider used to populate report sections beyond
     * the current dashboard snapshot.
     */
    void setAggregateDataProvider(
        std::function<ReportAggregateData(
            const ReportRequest&,
            const DashboardViewModel&,
            const ReportMetadata&)> provider);
    
    /**
     * Set the default output directory for reports.
     * Defaults to user's Documents folder if not set.
     */
    void setDefaultOutputDirectory(const std::string& path);
    
    /**
     * Get the default output directory.
     */
    std::string getDefaultOutputDirectory() const;
    
    /**
     * Generate a suggested filename based on report parameters.
     */
    std::string suggestFilename(const ReportRequest& request) const;

private:
    struct Impl;
    Impl* pImpl;
};

} // namespace JiStats
