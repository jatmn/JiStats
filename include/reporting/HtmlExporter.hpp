#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace JiStats {

// Forward declarations
struct DashboardViewModel;
struct ProviderStatus;
struct UsageProgressSummary;
struct ReportMetadata;
struct ReportAggregateData;

/**
 * HTML export options.
 */
struct HtmlExportOptions {
    bool includeStyles = true;        // Embed CSS styles
    bool includeScripts = false;      // Embed JavaScript (for interactivity)
    bool darkMode = false;            // Use dark color scheme
    std::string title = "JiStats Report";
    std::string customCss;            // Additional CSS to inject
};

/**
 * Data freshness state labels.
 */
enum class DataFreshnessState {
    Current,     // Data is fresh
    Stale,       // Data is stale (sync threshold exceeded)
    CatchingUp,  // Currently syncing/backfilling
    Offline,     // No provider connectivity
    Unknown      // Freshness cannot be determined
};

/**
 * Confidence level for displayed data.
 */
enum class DataConfidenceLevel {
    Exact,          // Exact match with provider
    HighConfidence, // High confidence estimate
    Estimated,      // Estimated based on local observation
    Unknown         // Confidence cannot be determined
};

/**
 * HtmlExporter generates HTML reports from dashboard data.
 * 
 * Per docs/plan.md section 13:
 * - Reports are export-only (not for live viewing)
 * - Include generation timestamp
 * - Data source attribution
 * - Confidence indicators
 */
class HtmlExporter {
public:
    HtmlExporter();
    ~HtmlExporter();
    
    /**
     * Export dashboard data to HTML file.
     * @param viewModel Dashboard data to export
     * @param outputPath Target file path
     * @param options Export options
     * @return true on success
     */
    bool exportToFile(const DashboardViewModel& viewModel,
                      const std::string& outputPath,
                      const HtmlExportOptions& options = HtmlExportOptions());
    
    /**
     * Export dashboard data to HTML file with report metadata.
     * @param viewModel Dashboard data to export
     * @param metadata Report metadata for freshness/backfill labeling
     * @param outputPath Target file path
     * @param options Export options
     * @return true on success
     */
    bool exportToFile(const DashboardViewModel& viewModel,
                      const ReportMetadata& metadata,
                      const ReportAggregateData& aggregateData,
                      const std::string& outputPath,
                      const HtmlExportOptions& options = HtmlExportOptions());
    
    /**
     * Generate HTML content as a string.
     * Useful for preview or custom file handling.
     */
    std::string generateHtml(const DashboardViewModel& viewModel,
                             const HtmlExportOptions& options = HtmlExportOptions());
    
    /**
     * Generate HTML content as a string with report metadata.
     * Includes freshness and backfill labeling per docs/plan.md section 13.1.
     */
    std::string generateHtml(const DashboardViewModel& viewModel,
                             const ReportMetadata& metadata,
                             const ReportAggregateData& aggregateData,
                             const HtmlExportOptions& options = HtmlExportOptions());
    
    /**
     * Set the timestamp to use for the report.
     * Defaults to current time if not set.
     */
    void setReportTimestamp(const std::chrono::system_clock::time_point& timestamp);
    
    /**
     * Get the last error message if export failed.
     */
    std::string getLastError() const;

private:
    struct Impl;
    Impl* pImpl;
};

} // namespace JiStats
