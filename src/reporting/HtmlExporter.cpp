#include "reporting/HtmlExporter.hpp"
#include "reporting/ReportingService.hpp"
#include "gui/DashboardPresenter.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace JiStats {

namespace {

bool anyQuotaHasProviderSurface(const DashboardViewModel& viewModel) {
    for (const auto& provider : viewModel.providers) {
        for (const auto& quota : provider.quotas) {
            if (!quota.providerSurface.empty()) {
                return true;
            }
        }
    }

    return false;
}

std::string displaySourceLabel(const std::string& providerSurface) {
    if (providerSurface == "openai_subscription_local") {
        return "OpenAI Local Observation";
    }
    if (providerSurface == "openai_subscription_codex_local") {
        return "OpenAI Codex-local";
    }
    if (providerSurface == "openai_subscription_export_import") {
        return "OpenAI Export Import";
    }
    if (providerSurface == "openai_api_import") {
        return "OpenAI API Platform Import";
    }
    if (providerSurface == "claude_code_local_stats") {
        return "Claude Local";
    }
    if (providerSurface == "grok_subscription_api") {
        return "Grok Subscription";
    }
    if (providerSurface == "cursor_subscription_api") {
        return "Cursor Subscription";
    }
    if (providerSurface == "kimi_subscription_api") {
        return "Kimi Subscription";
    }
    if (providerSurface == "kimi_code_local" ||
        providerSurface == "kimi_local_runtime") {
        return "Kimi Local";
    }
    return providerSurface;
}

} // namespace

struct HtmlExporter::Impl {
    std::chrono::system_clock::time_point reportTimestamp;
    bool timestampSet = false;
    std::string lastError;
    
    std::string formatTimestamp(const std::chrono::system_clock::time_point& tp) {
        std::time_t time = std::chrono::system_clock::to_time_t(tp);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &time);
#else
        gmtime_r(&time, &tm);
#endif
        std::ostringstream ss;
        ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S UTC");
        return ss.str();
    }
    
    std::string escapeHtml(const std::string& text) {
        std::string result;
        result.reserve(text.size());
        for (char c : text) {
            switch (c) {
                case '&': result += std::string("&") + "amp;"; break;
                case '<': result += std::string("&") + "lt;"; break;
                case '>': result += std::string("&") + "gt;"; break;
                case '"': result += std::string("&") + "quot;"; break;
                case '\'': result += std::string("&") + "#39;"; break;
                default: result += c; break;
            }
        }
        return result;
    }
    
    std::string providerStateToString(ProviderState state) {
        switch (state) {
            case ProviderState::Sample: return "Sample";
            case ProviderState::Offline: return "Offline";
            case ProviderState::Stale: return "Stale";
            case ProviderState::Syncing: return "Syncing";
            case ProviderState::Live: return "Live";
            default: return "Unknown";
        }
    }
    
    std::string getCssStyles(bool darkMode) {
        if (darkMode) {
            return ":root{--bg-color:#1a1a2e;--card-bg:#16213e;--text-color:#eaeaea;--text-muted:#a0a0a0;--border-color:#2a2a4a;--primary-color:#4a9eff;--success-color:#2ecc71;--warning-color:#f39c12;--danger-color:#e74c3c}";
        }
        return ":root{--bg-color:#f5f5f5;--card-bg:#ffffff;--text-color:#333333;--text-muted:#666666;--border-color:#e0e0e0;--primary-color:#0078d4;--success-color:#28a745;--warning-color:#ffc107;--danger-color:#dc3545}";
    }
    
    std::string getBaseStyles() {
        return "*{box-sizing:border-box;margin:0;padding:0}"
               "body{font-family:'Segoe UI',-apple-system,BlinkMacSystemFont,sans-serif;background-color:var(--bg-color);color:var(--text-color);line-height:1.6;padding:20px}"
               ".container{max-width:1200px;margin:0 auto}"
               ".header{text-align:center;margin-bottom:30px;padding-bottom:20px;border-bottom:1px solid var(--border-color)}"
               ".header h1{font-size:2rem;font-weight:600;margin-bottom:8px}"
               ".header .subtitle{color:var(--text-muted);font-size:0.9rem}"
               ".header .timestamp{color:var(--text-muted);font-size:0.85rem;margin-top:4px}"
               ".card{background:var(--card-bg);border:1px solid var(--border-color);border-radius:8px;padding:20px;margin-bottom:20px}"
               ".card h2{font-size:1.25rem;font-weight:600;margin-bottom:16px;padding-bottom:8px;border-bottom:1px solid var(--border-color)}"
               ".status-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:16px}"
               ".status-item{padding:12px;background:var(--bg-color);border-radius:6px}"
               ".status-item .label{font-size:0.85rem;color:var(--text-muted);margin-bottom:4px}"
               ".status-item .value{font-size:1.1rem;font-weight:500}"
               ".status-online{color:var(--success-color)}"
               ".status-offline{color:var(--danger-color)}"
               "table{width:100%;border-collapse:collapse}"
               "th,td{text-align:left;padding:12px;border-bottom:1px solid var(--border-color)}"
               "th{font-weight:600;background:var(--bg-color)}"
               ".footer{text-align:center;margin-top:30px;padding-top:20px;border-top:1px solid var(--border-color);color:var(--text-muted);font-size:0.85rem}"
               ".confidence-badge{display:inline-block;padding:2px 8px;border-radius:4px;font-size:0.75rem;font-weight:500}"
               ".confidence-high{background:var(--success-color);color:white}"
               ".confidence-medium{background:var(--warning-color);color:#333}"
               ".confidence-low{background:var(--danger-color);color:white}"
               ".metadata-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px}"
               ".metadata-item{padding:10px;background:var(--bg-color);border-radius:6px}"
               ".metadata-item .label{font-size:0.8rem;color:var(--text-muted);margin-bottom:4px}"
               ".metadata-item .value{font-size:0.95rem;font-weight:500}"
               ".summary-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px}"
               ".summary-card{padding:12px;background:var(--bg-color);border-radius:6px;border:1px solid var(--border-color)}"
               ".summary-card .label{font-size:0.8rem;color:var(--text-muted);margin-bottom:4px}"
               ".summary-card .value{font-size:1.15rem;font-weight:600}"
               ".summary-card .detail{font-size:0.82rem;color:var(--text-muted);margin-top:4px}"
               ".section-note{font-size:0.9rem;color:var(--text-muted);margin-bottom:12px}"
               ".callout{padding:12px 14px;background:var(--bg-color);border-left:4px solid var(--primary-color);border-radius:6px;margin-bottom:14px}"
               ".chart-wrap{display:flex;align-items:flex-end;gap:8px;height:180px;padding:12px;background:var(--bg-color);border-radius:6px;border:1px solid var(--border-color)}"
               ".chart-bar{flex:1;display:flex;flex-direction:column;align-items:center;justify-content:flex-end;min-width:0}"
               ".chart-bar-fill{width:100%;max-width:42px;background:var(--primary-color);border-radius:6px 6px 0 0;min-height:4px}"
               ".chart-bar-label{font-size:0.75rem;color:var(--text-muted);margin-top:8px;text-align:center}"
               ".snapshot-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}"
               ".snapshot-card{padding:14px;background:var(--bg-color);border-radius:6px;border:1px solid var(--border-color)}"
               ".snapshot-card h3{font-size:1rem;margin-bottom:6px}"
               ".snapshot-meta{font-size:0.8rem;color:var(--text-muted);margin-bottom:8px}"
               ".snapshot-value{font-size:1.2rem;font-weight:600}"
               ".snapshot-detail{font-size:0.82rem;color:var(--text-muted);margin-top:6px}"
               ".badge{display:inline-block;padding:2px 8px;border-radius:4px;font-size:0.75rem;font-weight:500}"
               ".badge-fresh{background:var(--success-color);color:white}"
               ".badge-stale{background:var(--warning-color);color:#333}"
               ".badge-catching-up{background:var(--primary-color);color:white}"
               ".badge-offline{background:var(--danger-color);color:white}"
                ".badge-backfill{background:var(--primary-color);color:white}"
                ".badge-unknown{background:var(--warning-color);color:#333}"
               ".badge-unknown{background:#888;color:white}";
    }
    
    std::string freshnessStateToBadgeClass(const ReportMetadata& metadata) {
        if (metadata.isSyncing) return "badge-catching-up";
        if (metadata.isOffline) return "badge-offline";
        if (metadata.isStale) return "badge-stale";
        return "badge-fresh";
    }
};

HtmlExporter::HtmlExporter() : pImpl(new Impl()) {
}

HtmlExporter::~HtmlExporter() {
    delete pImpl;
}

bool HtmlExporter::exportToFile(const DashboardViewModel& viewModel,
                                 const std::string& outputPath,
                                 const HtmlExportOptions& options) {
    std::string html = generateHtml(viewModel, options);
    if (html.empty()) {
        pImpl->lastError = "Failed to generate HTML content";
        return false;
    }
    
    std::ofstream file(outputPath, std::ios::binary);
    if (!file.is_open()) {
        pImpl->lastError = "Failed to open file for writing: " + outputPath;
        return false;
    }
    
    file << html;
    file.close();
    
    if (file.fail()) {
        pImpl->lastError = "Failed to write to file: " + outputPath;
        return false;
    }
    
    return true;
}

bool HtmlExporter::exportToFile(const DashboardViewModel& viewModel,
                                 const ReportMetadata& metadata,
                                 const ReportAggregateData& aggregateData,
                                 const std::string& outputPath,
                                 const HtmlExportOptions& options) {
    std::string html = generateHtml(viewModel, metadata, aggregateData, options);
    if (html.empty()) {
        pImpl->lastError = "Failed to generate HTML content";
        return false;
    }
    
    std::ofstream file(outputPath, std::ios::binary);
    if (!file.is_open()) {
        pImpl->lastError = "Failed to open file for writing: " + outputPath;
        return false;
    }
    
    file << html;
    file.close();
    
    if (file.fail()) {
        pImpl->lastError = "Failed to write to file: " + outputPath;
        return false;
    }
    
    return true;
}

std::string HtmlExporter::generateHtml(const DashboardViewModel& viewModel,
                                        const HtmlExportOptions& options) {
    std::ostringstream html;
    
    auto timestamp = pImpl->timestampSet ? pImpl->reportTimestamp 
                                         : std::chrono::system_clock::now();
    std::string timestampStr = pImpl->formatTimestamp(timestamp);
    
    html << "<!DOCTYPE html>\n";
    html << "<html lang=\"en\">\n";
    html << "<head>\n";
    html << "    <meta charset=\"UTF-8\">\n";
    html << "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n";
    html << "    <title>" << pImpl->escapeHtml(options.title) << "</title>\n";
    
    if (options.includeStyles) {
        html << "    <style>\n";
        html << pImpl->getCssStyles(options.darkMode) << "\n";
        html << pImpl->getBaseStyles() << "\n";
        if (!options.customCss.empty()) {
            html << options.customCss << "\n";
        }
        html << "    </style>\n";
    }
    
    html << "</head>\n";
    html << "<body>\n";
    html << "    <div class=\"container\">\n";
    
    html << "        <div class=\"header\">\n";
    html << "            <h1>" << pImpl->escapeHtml(options.title) << "</h1>\n";
    html << "            <div class=\"subtitle\">AI Provider Usage Tracking Report</div>\n";
    html << "            <div class=\"timestamp\">Generated: " << timestampStr << "</div>\n";
    html << "        </div>\n";
    
    html << "        <div class=\"card\">\n";
    html << "            <h2>Application Status</h2>\n";
    html << "            <div class=\"status-grid\">\n";
    html << "                <div class=\"status-item\">\n";
    html << "                    <div class=\"label\">Version</div>\n";
    html << "                    <div class=\"value\">" << pImpl->escapeHtml(viewModel.appVersion) << "</div>\n";
    html << "                </div>\n";
    html << "                <div class=\"status-item\">\n";
    html << "                    <div class=\"label\">Connection</div>\n";
    std::string statusClass = viewModel.isOnline ? "status-online" : "status-offline";
    std::string statusText = viewModel.isOnline ? "Online" : "Offline";
    html << "                    <div class=\"value " << statusClass << "\">" << statusText << "</div>\n";
    html << "                </div>\n";
    html << "                <div class=\"status-item\">\n";
    html << "                    <div class=\"label\">Last Updated</div>\n";
    html << "                    <div class=\"value\">" << pImpl->escapeHtml(viewModel.lastUpdateTime) << "</div>\n";
    html << "                </div>\n";
    html << "            </div>\n";
    html << "        </div>\n";
    
    html << "        <div class=\"card\">\n";
    html << "            <h2>Provider Quotas</h2>\n";
    if (viewModel.providers.empty()) {
        html << "            <p>No provider data available.</p>\n";
    } else {
        const bool showSourceColumn = anyQuotaHasProviderSurface(viewModel);
        html << "            <table>\n";
        html << "                <thead>\n";
        html << "                    <tr>\n";
        html << "                        <th>Provider</th>\n";
        html << "                        <th>State</th>\n";
        html << "                        <th>Plan</th>\n";
        if (showSourceColumn) {
            html << "                        <th>Source</th>\n";
        }
        html << "                        <th>Used</th>\n";
        html << "                        <th>Limit</th>\n";
        html << "                        <th>Remaining</th>\n";
        html << "                        <th>Confidence</th>\n";
        html << "                    </tr>\n";
        html << "                </thead>\n";
        html << "                <tbody>\n";
        
        for (const auto& provider : viewModel.providers) {
            for (const auto& quota : provider.quotas) {
                html << "                    <tr>\n";
                html << "                        <td>" << pImpl->escapeHtml(provider.displayName) << "</td>\n";
                html << "                        <td>" << pImpl->escapeHtml(pImpl->providerStateToString(provider.state)) << "</td>\n";
                html << "                        <td>" << pImpl->escapeHtml(quota.planName) << "</td>\n";
                if (showSourceColumn) {
                    std::string sourceBadgeClass = "badge-fresh";
                    std::string sourceLabel = displaySourceLabel(quota.providerSurface);
                    if (quota.isBackfilled) {
                        sourceBadgeClass = "badge-backfill";
                        sourceLabel = "Backfilled";
                    } else if (quota.hasUnknownGaps) {
                        sourceBadgeClass = "badge-unknown";
                        sourceLabel = "Estimated";
                    }
                    html << "                        <td><span class=\"" << sourceBadgeClass << "\">" << pImpl->escapeHtml(sourceLabel) << "</span></td>\n";
                }
                html << "                        <td>" << quota.used << "</td>\n";
                html << "                        <td>" << quota.limit << "</td>\n";
                html << "                        <td>" << quota.remaining << "</td>\n";
                std::string confClass = quota.confidenceTier == "high" ? "high" : 
                                        quota.confidenceTier == "medium" ? "medium" : "low";
                html << "                        <td><span class=\"confidence-badge confidence-" << confClass << "\">" << pImpl->escapeHtml(quota.confidenceTier) << "</span></td>\n";
                html << "                    </tr>\n";
            }
        }
        
        html << "                </tbody>\n";
        html << "            </table>\n";
    }
    html << "        </div>\n";

    html << "        <div class=\"footer\">\n";
    html << "            <p>Generated by JiStats v" << pImpl->escapeHtml(viewModel.appVersion) << "</p>\n";
    html << "            <p>Data source attribution and confidence indicators are shown where available.</p>\n";
    html << "        </div>\n";
    
    html << "    </div>\n";
    html << "</body>\n";
    html << "</html>\n";
    
    return html.str();
}

std::string HtmlExporter::generateHtml(const DashboardViewModel& viewModel,
                                        const ReportMetadata& metadata,
                                        const ReportAggregateData& aggregateData,
                                        const HtmlExportOptions& options) {
    std::ostringstream html;
    
    auto timestamp = pImpl->timestampSet ? pImpl->reportTimestamp 
                                         : std::chrono::system_clock::now();
    std::string timestampStr = pImpl->formatTimestamp(timestamp);
    
    html << "<!DOCTYPE html>\n";
    html << "<html lang=\"en\">\n";
    html << "<head>\n";
    html << "    <meta charset=\"UTF-8\">\n";
    html << "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n";
    html << "    <title>" << pImpl->escapeHtml(options.title) << "</title>\n";
    
    if (options.includeStyles) {
        html << "    <style>\n";
        html << pImpl->getCssStyles(options.darkMode) << "\n";
        html << pImpl->getBaseStyles() << "\n";
        if (!options.customCss.empty()) {
            html << options.customCss << "\n";
        }
        html << "    </style>\n";
    }
    
    html << "</head>\n";
    html << "<body>\n";
    html << "    <div class=\"container\">\n";
    
    html << "        <div class=\"header\">\n";
    html << "            <h1>" << pImpl->escapeHtml(options.title) << "</h1>\n";
    html << "            <div class=\"subtitle\">AI Provider Usage Tracking Report</div>\n";
    html << "            <div class=\"timestamp\">Generated: " << timestampStr << "</div>\n";
    html << "        </div>\n";

    // Report metadata card (per docs/reference/reporting.md)
    html << "        <div class=\"card\">\n";
    html << "            <h2>Report Data Quality</h2>\n";
    html << "            <div class=\"metadata-grid\">\n";

    html << "                <div class=\"metadata-item\">\n";
    html << "                    <div class=\"label\">Report Generated</div>\n";
    html << "                    <div class=\"value\">" << timestampStr << "</div>\n";
    html << "                </div>\n";

    if (!aggregateData.dateRangeLabel.empty()) {
        html << "                <div class=\"metadata-item\">\n";
        html << "                    <div class=\"label\">Report Window</div>\n";
        html << "                    <div class=\"value\">" << pImpl->escapeHtml(aggregateData.dateRangeLabel) << "</div>\n";
        html << "                </div>\n";
    }
    
    // Freshness state
    html << "                <div class=\"metadata-item\">\n";
    html << "                    <div class=\"label\">Data Freshness</div>\n";
    std::string freshnessBadgeClass = pImpl->freshnessStateToBadgeClass(metadata);
    std::string freshnessLabel = metadata.freshnessLabel();
    html << "                    <div class=\"value\"><span class=\"badge " << freshnessBadgeClass << "\">" << freshnessLabel << "</span></div>\n";
    html << "                </div>\n";
    
    // Backfill status
    html << "                <div class=\"metadata-item\">\n";
    html << "                    <div class=\"label\">Backfill Status</div>\n";
    const char* backfillBadgeClass = "badge-fresh";
    if (metadata.containsUnknownGaps) {
        backfillBadgeClass = "badge-unknown";
    } else if (metadata.containsBackfilledData) {
        backfillBadgeClass = "badge-backfill";
    }
    std::string backfillLabel = metadata.backfillLabel();
    html << "                    <div class=\"value\"><span class=\"badge " << backfillBadgeClass << "\">" << pImpl->escapeHtml(backfillLabel) << "</span></div>\n";
    html << "                </div>\n";
    
    // Data timestamp
    html << "                <div class=\"metadata-item\">\n";
    html << "                    <div class=\"label\">Data Timestamp</div>\n";
    const std::string dataTimestampStr =
        metadata.dataFreshnessTimestamp ? pImpl->formatTimestamp(*metadata.dataFreshnessTimestamp) : "Unknown";
    html << "                    <div class=\"value\">" << dataTimestampStr << "</div>\n";
    html << "                </div>\n";
    
    // Mirror policy version
    html << "                <div class=\"metadata-item\">\n";
    html << "                    <div class=\"label\">Mirror Policy</div>\n";
    html << "                    <div class=\"value\">" << pImpl->escapeHtml(metadata.mirrorPolicyVersion) << "</div>\n";
    html << "                </div>\n";

    html << "                <div class=\"metadata-item\">\n";
    html << "                    <div class=\"label\">Providers</div>\n";
    html << "                    <div class=\"value\">";
    if (metadata.providerKeys.empty()) {
        html << "All Providers";
    } else {
        for (size_t i = 0; i < metadata.providerKeys.size(); ++i) {
            if (i > 0) {
                html << ", ";
            }
            html << pImpl->escapeHtml(metadata.providerKeys[i]);
        }
    }
    html << "</div>\n";
    html << "                </div>\n";

    html << "                <div class=\"metadata-item\">\n";
    html << "                    <div class=\"label\">Account Scope</div>\n";
    html << "                    <div class=\"value\">" << pImpl->escapeHtml(metadata.scopeLabel()) << "</div>\n";
    html << "                </div>\n";

    if (!metadata.accountIds.empty()) {
        html << "                <div class=\"metadata-item\">\n";
        html << "                    <div class=\"label\">Accounts</div>\n";
        html << "                    <div class=\"value\">";
        for (size_t i = 0; i < metadata.accountIds.size(); ++i) {
            if (i > 0) html << ", ";
            html << pImpl->escapeHtml(metadata.accountIds[i]);
        }
        html << "</div>\n";
        html << "                </div>\n";
    }

    if (!metadata.providerSurfaces.empty()) {
        html << "                <div class=\"metadata-item\">\n";
        html << "                    <div class=\"label\">Source Labels</div>\n";
        html << "                    <div class=\"value\">";
        for (size_t i = 0; i < metadata.providerSurfaces.size(); ++i) {
            if (i > 0) html << ", ";
            html << pImpl->escapeHtml(displaySourceLabel(metadata.providerSurfaces[i]));
        }
        html << "</div>\n";
        html << "                </div>\n";
    }
    
    html << "            </div>\n";
    html << "        </div>\n";
    
    html << "        <div class=\"card\">\n";
    html << "            <h2>Application Status</h2>\n";
    html << "            <div class=\"status-grid\">\n";
    html << "                <div class=\"status-item\">\n";
    html << "                    <div class=\"label\">Version</div>\n";
    html << "                    <div class=\"value\">" << pImpl->escapeHtml(viewModel.appVersion) << "</div>\n";
    html << "                </div>\n";
    html << "                <div class=\"status-item\">\n";
    html << "                    <div class=\"label\">Connection</div>\n";
    std::string statusClass = viewModel.isOnline ? "status-online" : "status-offline";
    std::string statusText = viewModel.isOnline ? "Online" : "Offline";
    html << "                    <div class=\"value " << statusClass << "\">" << statusText << "</div>\n";
    html << "                </div>\n";
    html << "                <div class=\"status-item\">\n";
    html << "                    <div class=\"label\">Last Updated</div>\n";
    html << "                    <div class=\"value\">" << pImpl->escapeHtml(viewModel.lastUpdateTime) << "</div>\n";
    html << "                </div>\n";
    html << "            </div>\n";
    html << "        </div>\n";
    
    html << "        <div class=\"card\">\n";
    html << "            <h2>Provider Quotas</h2>\n";
    if (viewModel.providers.empty()) {
        html << "            <p>No provider data available.</p>\n";
    } else {
        const bool showSourceColumn = anyQuotaHasProviderSurface(viewModel);
        html << "            <table>\n";
        html << "                <thead>\n";
        html << "                    <tr>\n";
        html << "                        <th>Provider</th>\n";
        html << "                        <th>State</th>\n";
        html << "                        <th>Plan</th>\n";
        if (showSourceColumn) {
            html << "                        <th>Source</th>\n";
        }
        html << "                        <th>Used</th>\n";
        html << "                        <th>Limit</th>\n";
        html << "                        <th>Remaining</th>\n";
        html << "                        <th>Confidence</th>\n";
        html << "                    </tr>\n";
        html << "                </thead>\n";
        html << "                <tbody>\n";
        
        for (const auto& provider : viewModel.providers) {
            for (const auto& quota : provider.quotas) {
                html << "                    <tr>\n";
                html << "                        <td>" << pImpl->escapeHtml(provider.displayName) << "</td>\n";
                html << "                        <td>" << pImpl->escapeHtml(pImpl->providerStateToString(provider.state)) << "</td>\n";
                html << "                        <td>" << pImpl->escapeHtml(quota.planName) << "</td>\n";
                if (showSourceColumn) {
                    std::string sourceBadgeClass = "badge-fresh";
                    std::string sourceLabel = displaySourceLabel(quota.providerSurface);
                    if (quota.isBackfilled) {
                        sourceBadgeClass = "badge-backfill";
                        sourceLabel = "Backfilled";
                    } else if (quota.hasUnknownGaps) {
                        sourceBadgeClass = "badge-unknown";
                        sourceLabel = "Estimated";
                    }
                    html << "                        <td><span class=\"" << sourceBadgeClass << "\">" << pImpl->escapeHtml(sourceLabel) << "</span></td>\n";
                }
                html << "                        <td>" << quota.used << "</td>\n";
                html << "                        <td>" << quota.limit << "</td>\n";
                html << "                        <td>" << quota.remaining << "</td>\n";
                std::string confClass = quota.confidenceTier == "high" ? "high" : 
                                        quota.confidenceTier == "medium" ? "medium" : "low";
                html << "                        <td><span class=\"confidence-badge confidence-" << confClass << "\">" << pImpl->escapeHtml(quota.confidenceTier) << "</span></td>\n";
                html << "                    </tr>\n";
            }
        }
        
        html << "                </tbody>\n";
        html << "            </table>\n";
    }
    html << "        </div>\n";

    if (!aggregateData.gapBackfillSummary.empty()) {
        html << "        <div class=\"card\">\n";
        html << "            <h2>Recovery And Gap Summary</h2>\n";
        html << "            <div class=\"callout\">" << pImpl->escapeHtml(aggregateData.gapBackfillSummary) << "</div>\n";
        html << "        </div>\n";
    }

    if (!aggregateData.dailySummaries.empty()) {
        html << "        <div class=\"card\">\n";
        html << "            <h2>Daily Summary</h2>\n";
        html << "            <div class=\"summary-grid\">\n";
        for (const auto& bucket : aggregateData.dailySummaries) {
            html << "                <div class=\"summary-card\">\n";
            html << "                    <div class=\"label\">" << pImpl->escapeHtml(bucket.label) << "</div>\n";
            html << "                    <div class=\"value\">" << bucket.totalTokens << " tokens</div>\n";
            html << "                    <div class=\"detail\">" << bucket.requestCount << " requests";
            if (!bucket.periodLabel.empty()) {
                html << " | " << pImpl->escapeHtml(bucket.periodLabel);
            }
            html << "</div>\n";
            html << "                </div>\n";
        }
        html << "            </div>\n";
        html << "        </div>\n";
    }

    if (!aggregateData.weeklySummaries.empty()) {
        html << "        <div class=\"card\">\n";
        html << "            <h2>Weekly Summary</h2>\n";
        html << "            <div class=\"summary-grid\">\n";
        for (const auto& bucket : aggregateData.weeklySummaries) {
            html << "                <div class=\"summary-card\">\n";
            html << "                    <div class=\"label\">" << pImpl->escapeHtml(bucket.label) << "</div>\n";
            html << "                    <div class=\"value\">" << bucket.totalTokens << " tokens</div>\n";
            html << "                    <div class=\"detail\">" << bucket.requestCount << " requests";
            if (!bucket.periodLabel.empty()) {
                html << " | " << pImpl->escapeHtml(bucket.periodLabel);
            }
            html << "</div>\n";
            html << "                </div>\n";
        }
        html << "            </div>\n";
        html << "        </div>\n";
    }

    if (!aggregateData.providerBreakdown.empty()) {
        html << "        <div class=\"card\">\n";
        html << "            <h2>Provider Breakdown</h2>\n";
        html << "            <table>\n";
        html << "                <thead><tr><th>Provider</th><th>Requests</th><th>Tokens</th><th>Cost (minor units)</th></tr></thead>\n";
        html << "                <tbody>\n";
        for (const auto& row : aggregateData.providerBreakdown) {
            html << "                    <tr><td>" << pImpl->escapeHtml(row.label) << "</td><td>"
                 << row.requestCount << "</td><td>" << row.totalTokens << "</td><td>"
                 << row.totalCostMinorUnits << "</td></tr>\n";
        }
        html << "                </tbody>\n";
        html << "            </table>\n";
        html << "        </div>\n";
    }

    if (!aggregateData.accountBreakdown.empty()) {
        html << "        <div class=\"card\">\n";
        html << "            <h2>Account Breakdown</h2>\n";
        html << "            <table>\n";
        html << "                <thead><tr><th>Account</th><th>Provider</th><th>Requests</th><th>Tokens</th><th>Cost (minor units)</th></tr></thead>\n";
        html << "                <tbody>\n";
        for (const auto& row : aggregateData.accountBreakdown) {
            html << "                    <tr><td>" << pImpl->escapeHtml(row.label) << "</td><td>"
                 << pImpl->escapeHtml(row.secondaryLabel) << "</td><td>" << row.requestCount
                 << "</td><td>" << row.totalTokens << "</td><td>" << row.totalCostMinorUnits
                 << "</td></tr>\n";
        }
        html << "                </tbody>\n";
        html << "            </table>\n";
        html << "        </div>\n";
    }

    if (!aggregateData.modelBreakdown.empty()) {
        html << "        <div class=\"card\">\n";
        html << "            <h2>Model Breakdown</h2>\n";
        html << "            <table>\n";
        html << "                <thead><tr><th>Model</th><th>Requests</th><th>Tokens</th><th>Cost (minor units)</th></tr></thead>\n";
        html << "                <tbody>\n";
        for (const auto& row : aggregateData.modelBreakdown) {
            html << "                    <tr><td>" << pImpl->escapeHtml(row.label) << "</td><td>"
                 << row.requestCount << "</td><td>" << row.totalTokens << "</td><td>"
                 << row.totalCostMinorUnits << "</td></tr>\n";
        }
        html << "                </tbody>\n";
        html << "            </table>\n";
        html << "        </div>\n";
    }

    if (!aggregateData.recentTrend.empty()) {
        long long maxTrendTokens = 0;
        for (const auto& point : aggregateData.recentTrend) {
            maxTrendTokens = std::max(maxTrendTokens, point.totalTokens);
        }

        html << "        <div class=\"card\">\n";
        html << "            <h2>Recent Trend</h2>\n";
        html << "            <div class=\"section-note\">Tokens by recent day across the current report scope.</div>\n";
        html << "            <div class=\"chart-wrap\">\n";
        for (const auto& point : aggregateData.recentTrend) {
            const long long height =
                maxTrendTokens > 0 ? std::max<long long>(8LL, (point.totalTokens * 140LL) / maxTrendTokens) : 8LL;
            html << "                <div class=\"chart-bar\">\n";
            html << "                    <div class=\"chart-bar-fill\" style=\"height:" << height << "px\"></div>\n";
            html << "                    <div class=\"chart-bar-label\">" << pImpl->escapeHtml(point.label) << "</div>\n";
            html << "                </div>\n";
        }
        html << "            </div>\n";
        html << "        </div>\n";
    }

    if (!aggregateData.quotaSnapshots.empty()) {
        html << "        <div class=\"card\">\n";
        html << "            <h2>" << pImpl->escapeHtml(aggregateData.quotaSnapshotTitle) << "</h2>\n";
        if (!aggregateData.quotaSnapshotDetail.empty()) {
            html << "            <div class=\"section-note\">"
                 << pImpl->escapeHtml(aggregateData.quotaSnapshotDetail)
                 << "</div>\n";
        }
        html << "            <div class=\"snapshot-grid\">\n";
        for (const auto& snapshot : aggregateData.quotaSnapshots) {
            html << "                <div class=\"snapshot-card\">\n";
            html << "                    <h3>" << pImpl->escapeHtml(snapshot.providerDisplayName) << "</h3>\n";
            html << "                    <div class=\"snapshot-meta\">" << pImpl->escapeHtml(snapshot.planName);
            if (!snapshot.accountLabel.empty()) {
                html << " | " << pImpl->escapeHtml(snapshot.accountLabel);
            }
            html << "</div>\n";
            html << "                    <div class=\"snapshot-value\">" << snapshot.remaining;
            if (snapshot.limit > 0) {
                html << " / " << snapshot.limit;
            }
            html << " remaining</div>\n";
            html << "                    <div class=\"snapshot-detail\">Used: " << snapshot.used
                 << " | Confidence: " << pImpl->escapeHtml(snapshot.confidenceTier);
            if (!snapshot.sourceLabel.empty()) {
                html << " | Source: " << pImpl->escapeHtml(snapshot.sourceLabel);
            }
            html << "</div>\n";
            if (!snapshot.lastSyncLabel.empty()) {
                html << "                    <div class=\"snapshot-detail\">Last sync: "
                     << pImpl->escapeHtml(snapshot.lastSyncLabel) << "</div>\n";
            }
            if (snapshot.isBackfilled || snapshot.hasUnknownGaps) {
                html << "                    <div class=\"snapshot-detail\">";
                if (snapshot.isBackfilled) {
                    html << "Backfilled";
                }
                if (snapshot.isBackfilled && snapshot.hasUnknownGaps) {
                    html << " | ";
                }
                if (snapshot.hasUnknownGaps) {
                    html << "Unknown gaps";
                }
                html << "</div>\n";
            }
            html << "                </div>\n";
        }
        html << "            </div>\n";
        html << "        </div>\n";
    }
    
    html << "        <div class=\"footer\">\n";
    html << "            <p>Generated by JiStats v" << pImpl->escapeHtml(viewModel.appVersion) << "</p>\n";
    html << "            <p>Data source attribution and confidence indicators are shown where available.</p>\n";
    html << "            <p>Contains backfilled data: " << (metadata.containsBackfilledData ? "Yes" : "No") << " | Contains unknown gaps: " << (metadata.containsUnknownGaps ? "Yes" : "No") << "</p>\n";
    html << "        </div>\n";
    
    html << "    </div>\n";
    html << "</body>\n";
    html << "</html>\n";
    
    return html.str();
}

void HtmlExporter::setReportTimestamp(const std::chrono::system_clock::time_point& timestamp) {
    pImpl->reportTimestamp = timestamp;
    pImpl->timestampSet = true;
}

std::string HtmlExporter::getLastError() const {
    return pImpl->lastError;
}

} // namespace JiStats
