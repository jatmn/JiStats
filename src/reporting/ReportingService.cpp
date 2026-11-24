#include "reporting/ReportingService.hpp"
#include "reporting/HtmlExporter.hpp"
#include "gui/DashboardPresenter.hpp"

#include <chrono>
#include <cctype>
#include <ctime>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace JiStats {

namespace {

bool hasMeaningfulDateRange(const ReportDateRange& range) {
    return range.start.time_since_epoch().count() != 0
        || range.end.time_since_epoch().count() != 0;
}

std::string sanitizeFilenameComponent(const std::string& value) {
    std::string sanitized;
    sanitized.reserve(value.size());
    for (char ch : value) {
        if ((ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9')) {
            sanitized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        } else if (!sanitized.empty() && sanitized.back() != '_') {
            sanitized.push_back('_');
        }
    }

    while (!sanitized.empty() && sanitized.back() == '_') {
        sanitized.pop_back();
    }
    return sanitized;
}

std::filesystem::path makeUniqueOutputPath(const std::filesystem::path& requestedPath) {
    if (!std::filesystem::exists(requestedPath)) {
        return requestedPath;
    }

    const std::filesystem::path parent = requestedPath.parent_path();
    const std::string stem = requestedPath.stem().string();
    const std::string extension = requestedPath.extension().string();
    for (int attempt = 2; attempt < 1000; ++attempt) {
        const std::filesystem::path candidate =
            parent / (stem + "_" + std::to_string(attempt) + extension);
        if (!std::filesystem::exists(candidate)) {
            return candidate;
        }
    }

    return requestedPath;
}

bool providerMatchesFilter(const ProviderStatus& provider, const std::unordered_set<std::string>& filter) {
    return filter.find(provider.providerKey) != filter.end()
        || filter.find(provider.displayName) != filter.end();
}

DashboardViewModel applyReportRequestToViewModel(const DashboardViewModel& source, const ReportRequest& request) {
    DashboardViewModel viewModel = source;

    if (!request.providerFilter.empty()) {
        std::unordered_set<std::string> allowedProviders(
            request.providerFilter.begin(),
            request.providerFilter.end());

        std::vector<ProviderStatus> filteredProviders;
        filteredProviders.reserve(viewModel.providers.size());
        for (const auto& provider : viewModel.providers) {
            if (providerMatchesFilter(provider, allowedProviders)) {
                filteredProviders.push_back(provider);
            }
        }

        viewModel.providers = std::move(filteredProviders);
    }

    if (!request.accountFilter.empty()) {
        std::unordered_set<std::string> allowedAccounts(
            request.accountFilter.begin(),
            request.accountFilter.end());

        std::vector<ProviderStatus> filteredProviders;
        filteredProviders.reserve(viewModel.providers.size());
        for (const auto& provider : viewModel.providers) {
            if (allowedAccounts.find(provider.accountId) != allowedAccounts.end()) {
                filteredProviders.push_back(provider);
            }
        }

        viewModel.providers = std::move(filteredProviders);
    }

    return viewModel;
}

void stripProviderQuotasFromViewModel(DashboardViewModel& viewModel) {
    for (auto& provider : viewModel.providers) {
        provider.quotas.clear();
    }
}

} // namespace

ReportMetadata buildReportMetadata(const DashboardViewModel& viewModel, const ReportRequest& request) {
    ReportMetadata metadata;
    metadata.providerKeys = request.providerFilter;
    metadata.accountIds = request.accountFilter;
    metadata.accountScopeComplete = !metadata.accountIds.empty();
    std::set<std::string> seenProviderSurfaces;

    std::vector<ProviderState> providerStates;
    providerStates.reserve(viewModel.providers.size());
    for (const auto& provider : viewModel.providers) {
        if (provider.isEnabled) {
            providerStates.push_back(provider.state);
        }
    }

    metadata.runtimeState = deriveRuntimeState(providerStates);
    metadata.isSyncing = metadata.runtimeState == RuntimeState::CatchingUp;
    metadata.isOffline = metadata.runtimeState == RuntimeState::Offline;
    metadata.isStale = metadata.runtimeState == RuntimeState::Stale;

    for (const auto& provider : viewModel.providers) {
        std::time_t parsed = parseRuntimeTimestamp(provider.lastSyncTime);
        if (parsed <= 0 && provider.lastKnownQuota) {
            parsed = parseRuntimeTimestamp(provider.lastKnownQuota->lastSyncTime);
        }
        if (parsed > 0) {
            const auto syncTime = std::chrono::system_clock::from_time_t(parsed);
            if (!metadata.dataFreshnessTimestamp || syncTime > *metadata.dataFreshnessTimestamp) {
                metadata.dataFreshnessTimestamp = syncTime;
            }
        }
    }

    // Check for backfilled or unknown data in quotas using real reconciliation state
    // Per review follow-up: use quota.isBackfilled/hasUnknownGaps, not confidenceTier proxy
    for (const auto& provider : viewModel.providers) {
        for (const auto& quota : provider.quotas) {
            if (!quota.providerSurface.empty() &&
                seenProviderSurfaces.insert(quota.providerSurface).second) {
                metadata.providerSurfaces.push_back(quota.providerSurface);
            }
            if (quota.hasUnknownGaps) {
                metadata.containsUnknownGaps = true;
            }
            if (quota.isBackfilled) {
                metadata.containsBackfilledData = true;
            }
        }
    }

    metadata.mirrorPolicyVersion = "unknown";

    return metadata;
}

std::string ReportMetadata::freshnessLabel() const {
    return runtimeStateLabel(runtimeState);
}

std::string ReportMetadata::backfillLabel() const {
    if (containsBackfilledData && containsUnknownGaps) {
        return "Contains backfilled data and unknown gaps";
    }
    if (containsBackfilledData) return "Contains backfilled data";
    if (containsUnknownGaps) return "Contains unknown gaps";
    return "No backfill or gaps";
}

std::string ReportMetadata::confidenceLabel() const {
    return mirrorPolicyVersion.empty() ? "Unknown" : mirrorPolicyVersion;
}

std::string ReportMetadata::scopeLabel() const {
    if (accountIds.empty()) {
        return "All Accounts";
    }
    if (providerKeys.empty()) {
        return "Account scope";
    }
    return accountScopeComplete
        ? "Provider and account scope"
        : "Mixed provider/account scope";
}

struct ReportingService::Impl {
    std::string defaultOutputDir;
    std::string lastError;
    std::function<bool(DashboardViewModel&)> dashboardDataProvider;
    std::function<std::string(const DashboardViewModel&)> mirrorPolicyVersionProvider;
    std::function<ReportAggregateData(
        const ReportRequest&,
        const DashboardViewModel&,
        const ReportMetadata&)> aggregateDataProvider;
    
    std::string getDefaultDocumentsPath() {
#ifdef _WIN32
        wchar_t path[MAX_PATH] = {0};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, path))) {
            return std::filesystem::path(path).string();
        }
#endif
        return ".";
    }
    
    std::string formatTimestampForFilename(const std::chrono::system_clock::time_point& tp) {
        std::time_t time = std::chrono::system_clock::to_time_t(tp);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &time);
#else
        localtime_r(&time, &tm);
#endif
        std::ostringstream ss;
        ss << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S");
        return ss.str();
    }
};

ReportingService::ReportingService() : pImpl(new Impl()) {
}

ReportingService::~ReportingService() {
    delete pImpl;
}

ReportResult ReportingService::generateReport(const ReportRequest& request) {
    ReportResult result;
    pImpl->lastError.clear();
    
    // Determine output path
    std::string outputPath = request.outputPath;
    if (outputPath.empty()) {
        outputPath = suggestFilename(request);
        outputPath = makeUniqueOutputPath(std::filesystem::path(outputPath)).string();
    }
    
    // Ensure output directory exists
    std::filesystem::path path(outputPath);
    std::filesystem::path dir = path.parent_path();
    if (!dir.empty() && !std::filesystem::exists(dir)) {
        std::error_code ec;
        if (!std::filesystem::create_directories(dir, ec)) {
            result.errorMessage = "Failed to create output directory: " + ec.message();
            return result;
        }
    }
    
    // Generate report based on format
    switch (request.format) {
        case ReportFormat::Html: {
            HtmlExporter exporter;
            HtmlExportOptions options;
            options.title = "JiStats Usage Report";

            if (!pImpl->dashboardDataProvider) {
                result.errorMessage = "ReportingService has no dashboard data provider configured";
                return result;
            }

            DashboardViewModel sourceViewModel;
            bool gotViewModel = false;
            try {
                gotViewModel = pImpl->dashboardDataProvider(sourceViewModel);
            } catch (const std::exception& ex) {
                pImpl->lastError = ex.what();
            } catch (...) {
                pImpl->lastError = "unknown exception";
            }

            if (!gotViewModel) {
                if (pImpl->lastError.empty()) {
                    result.errorMessage = "Failed to build dashboard data for report";
                } else {
                    result.errorMessage = "Failed to build dashboard data for report: " + pImpl->lastError;
                }
                return result;
            }

            DashboardViewModel reportViewModel = applyReportRequestToViewModel(sourceViewModel, request);
            exporter.setReportTimestamp(std::chrono::system_clock::now());
            
            // Build report metadata from view model data
            ReportMetadata metadata = buildReportMetadata(reportViewModel, request);
            if (pImpl->mirrorPolicyVersionProvider) {
                const std::string version =
                    pImpl->mirrorPolicyVersionProvider(reportViewModel);
                if (!version.empty()) {
                    metadata.mirrorPolicyVersion = version;
                }
            }

            if (!request.includeProviderQuotas) {
                stripProviderQuotasFromViewModel(reportViewModel);
            }

            ReportAggregateData aggregateData;
            if (pImpl->aggregateDataProvider) {
                aggregateData = pImpl->aggregateDataProvider(
                    request,
                    reportViewModel,
                    metadata);
            }

            if (aggregateData.recoveryCoverage) {
                metadata.containsBackfilledData = aggregateData.recoveryCoverage->containsBackfilledData;
                metadata.containsUnknownGaps = aggregateData.recoveryCoverage->containsUnknownGaps;
            }

            if (!exporter.exportToFile(reportViewModel, metadata, aggregateData, outputPath, options)) {
                result.errorMessage = exporter.getLastError();
                return result;
            }
            
            result.metadata = metadata;
            result.aggregateData = aggregateData;
            break;
        }
        case ReportFormat::Csv:
            result.errorMessage = "CSV format not yet implemented";
            return result;
        case ReportFormat::Json:
            result.errorMessage = "JSON format not yet implemented";
            return result;
    }
    
    result.success = true;
    result.outputPath = outputPath;
    
    // Get file size
    std::error_code ec;
    auto fileSize = std::filesystem::file_size(outputPath, ec);
    if (!ec) {
        result.bytesWritten = fileSize;
    }
    
    return result;
}

void ReportingService::setDashboardDataProvider(std::function<bool(DashboardViewModel&)> provider) {
    pImpl->dashboardDataProvider = std::move(provider);
}

void ReportingService::setMirrorPolicyVersionProvider(
    std::function<std::string(const DashboardViewModel&)> provider) {
    pImpl->mirrorPolicyVersionProvider = std::move(provider);
}

void ReportingService::setAggregateDataProvider(
    std::function<ReportAggregateData(
        const ReportRequest&,
        const DashboardViewModel&,
        const ReportMetadata&)> provider) {
    pImpl->aggregateDataProvider = std::move(provider);
}

void ReportingService::setDefaultOutputDirectory(const std::string& path) {
    pImpl->defaultOutputDir = path;
}

std::string ReportingService::getDefaultOutputDirectory() const {
    if (!pImpl->defaultOutputDir.empty()) {
        return pImpl->defaultOutputDir;
    }
    return pImpl->getDefaultDocumentsPath();
}

std::string ReportingService::suggestFilename(const ReportRequest& request) const {
    auto now = std::chrono::system_clock::now();
    std::string timestamp = pImpl->formatTimestampForFilename(now);
    
    std::string extension;
    switch (request.format) {
        case ReportFormat::Html:
            extension = ".html";
            break;
        case ReportFormat::Csv:
            extension = ".csv";
            break;
        case ReportFormat::Json:
            extension = ".json";
            break;
    }
    
    std::string baseName = "JiStats_Report";
    if (request.providerFilter.size() == 1) {
        const std::string providerSlug = sanitizeFilenameComponent(request.providerFilter.front());
        if (!providerSlug.empty()) {
            baseName += "_" + providerSlug;
        }
    } else if (request.providerFilter.size() > 1) {
        baseName += "_multi_provider";
    }

    if (request.accountFilter.size() == 1) {
        const std::string accountSlug = sanitizeFilenameComponent(request.accountFilter.front());
        if (!accountSlug.empty()) {
            baseName += "_" + accountSlug;
        }
    } else if (request.accountFilter.size() > 1) {
        baseName += "_multi_account";
    }

    if (hasMeaningfulDateRange(request.dateRange)) {
        const std::time_t start = std::chrono::system_clock::to_time_t(request.dateRange.start);
        const std::time_t end = std::chrono::system_clock::to_time_t(request.dateRange.end);
        std::tm startTm{};
        std::tm endTm{};
#ifdef _WIN32
        localtime_s(&startTm, &start);
        localtime_s(&endTm, &end);
#else
        localtime_r(&start, &startTm);
        localtime_r(&end, &endTm);
#endif
        std::ostringstream rangeLabel;
        rangeLabel << std::put_time(&startTm, "%Y-%m-%d")
                   << "_to_"
                   << std::put_time(&endTm, "%Y-%m-%d");
        baseName += "_" + rangeLabel.str();
    } else {
        baseName += "_rolling";
    }

    baseName += "_" + timestamp;
    
    std::filesystem::path outputDir = getDefaultOutputDirectory();
    return (outputDir / (baseName + extension)).string();
}

} // namespace JiStats
