#include <catch_amalgamated.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "reporting/ReportingService.hpp"
#include "gui/DashboardPresenter.hpp"
#include "temp_test_directory.hpp"

using namespace JiStats;
using JiStats::TestSupport::TempTestDirectory;

namespace {

QuotaInfo makeQuota(
    const std::string& planName,
    long long used,
    long long limit,
    const std::string& confidenceTier,
    const std::string& providerSurface = "") {
    QuotaInfo quota;
    quota.planName = planName;
    quota.windowType = "monthly";
    quota.used = used;
    quota.limit = limit;
    quota.remaining = (limit > used) ? (limit - used) : 0;
    quota.percentage = (limit > 0) ? (static_cast<double>(used) / static_cast<double>(limit)) * 100.0 : 0.0;
    quota.confidenceTier = confidenceTier;
    quota.providerSurface = providerSurface;
    return quota;
}

ProviderStatus makeProvider(const std::string& key, const std::string& displayName, std::vector<QuotaInfo> quotas) {
    ProviderStatus provider;
    provider.providerKey = key;
    provider.displayName = displayName;
    provider.isEnabled = true;
    provider.state = ProviderState::Live;
    provider.lastSyncTime = "2026-04-18 00:00:00 UTC";
    provider.quotas = std::move(quotas);
    return provider;
}

ProviderStatus makeProviderWithState(
    const std::string& key,
    const std::string& displayName,
    ProviderState state,
    const std::string& lastSyncTime,
    const std::string& accountId,
    std::vector<QuotaInfo> quotas) {
    ProviderStatus provider;
    provider.providerKey = key;
    provider.displayName = displayName;
    provider.accountId = accountId;
    provider.isEnabled = true;
    provider.state = state;
    provider.lastSyncTime = lastSyncTime;
    provider.quotas = std::move(quotas);
    return provider;
}

DashboardViewModel makeDashboardViewModel() {
    DashboardViewModel viewModel;
    viewModel.appVersion = "1.0.0-test";
    viewModel.lastUpdateTime = "2026-04-18 00:01:00 UTC";
    viewModel.isOnline = true;
    viewModel.providers = {
        makeProvider("openai", "OpenAI", {makeQuota("OpenAI Plan", 120, 1000, "high")}),
        makeProvider("minimax", "MiniMax", {makeQuota("MiniMax Plan", 85, 500, "medium")})
    };
    return viewModel;
}

ReportAggregateData makeAggregateData() {
    ReportAggregateData data;
    data.dateRangeLabel = "2026-04-12 to 2026-04-19 UTC";
    data.gapBackfillSummary = "1 provider recovery checkpoint imported provider history; 1 reconstructed gap marker";
    data.dailySummaries = {
        {"2026-04-18", "2026-04-18", 4, 400, 40},
        {"2026-04-19", "2026-04-19", 5, 500, 50}
    };
    data.weeklySummaries = {
        {"Week of 2026-04-14", "2026-04-14 to 2026-04-20 UTC", 9, 900, 90}
    };
    data.providerBreakdown = {
        {"openai", "OpenAI", "", 6, 600, 60},
        {"minimax", "MiniMax", "", 3, 300, 30}
    };
    data.accountBreakdown = {
        {"acct-openai", "OpenAI Account", "OpenAI", 6, 600, 60},
        {"acct-minimax", "MiniMax Account", "MiniMax", 3, 300, 30}
    };
    data.modelBreakdown = {
        {"gpt-4.1", "gpt-4.1", "", 5, 500, 50},
        {"minimax-m1", "minimax-m1", "", 4, 400, 40}
    };
    data.recentTrend = {
        {"04-18", 4, 400, 40},
        {"04-19", 5, 500, 50}
    };
    data.quotaSnapshots = {
        {"openai", "OpenAI", "acct-openai", "OpenAI Plan", "weekly", "2026-04-19 09:00:00 UTC", "high", "OpenAI Codex-local", 120, 1000, 880, 88.0, false, false},
        {"minimax", "MiniMax", "acct-minimax", "MiniMax Plan", "daily", "2026-04-19 09:05:00 UTC", "medium", "MiniMax Coding Plan", 85, 500, 415, 83.0, true, true}
    };
    return data;
}

std::string readFileContents(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open report file: " + path.string());
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

} // namespace

static_assert(!std::is_copy_constructible_v<ReportingService>, "ReportingService must stay non-copyable");
static_assert(!std::is_copy_assignable_v<ReportingService>, "ReportingService must stay non-assignable");

TEST_CASE("ReportingService constructs safely", "[reporting]") {
    ReportingService service;
    CHECK_FALSE(service.getDefaultOutputDirectory().empty());
}

TEST_CASE("ReportingService suggestFilename uses configured directory and format extension", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    service.setDefaultOutputDirectory(tempDir.path().string());

    const ReportRequest htmlRequest{ReportFormat::Html};
    const ReportRequest csvRequest{ReportFormat::Csv};
    const ReportRequest jsonRequest{ReportFormat::Json};

    const std::filesystem::path htmlPath(service.suggestFilename(htmlRequest));
    const std::filesystem::path csvPath(service.suggestFilename(csvRequest));
    const std::filesystem::path jsonPath(service.suggestFilename(jsonRequest));

    CHECK(htmlPath.parent_path() == tempDir.path());
    CHECK(csvPath.parent_path() == tempDir.path());
    CHECK(jsonPath.parent_path() == tempDir.path());
    CHECK(htmlPath.extension() == ".html");
    CHECK(csvPath.extension() == ".csv");
    CHECK(jsonPath.extension() == ".json");
    CHECK(htmlPath.filename().string().find("JiStats_Report_") != std::string::npos);
}

TEST_CASE("ReportingService generateReport fails when dashboard data provider is missing", "[reporting]") {
    ReportingService service;

    ReportRequest request;
    request.format = ReportFormat::Html;

    ReportResult result = service.generateReport(request);

    CHECK_FALSE(result.success);
    CHECK(result.outputPath.empty());
    CHECK(result.errorMessage == "ReportingService has no dashboard data provider configured");
}

TEST_CASE("ReportingService generateReport CSV returns not implemented error", "[reporting]") {
    ReportingService service;
    
    ReportRequest request;
    request.format = ReportFormat::Csv;
    request.outputPath = "test.csv";
    
    ReportResult result = service.generateReport(request);
    
    CHECK_FALSE(result.success);
    CHECK(result.errorMessage.find("not yet implemented") != std::string::npos);
}

TEST_CASE("ReportingService generateReport JSON returns not implemented error", "[reporting]") {
    ReportingService service;
    
    ReportRequest request;
    request.format = ReportFormat::Json;
    request.outputPath = "test.json";
    
    ReportResult result = service.generateReport(request);
    
    CHECK_FALSE(result.success);
    CHECK(result.errorMessage.find("not yet implemented") != std::string::npos);
}

TEST_CASE("ReportingService generateReport surfaces data-provider failure", "[reporting]") {
    ReportingService service;
    service.setDashboardDataProvider([](DashboardViewModel&) { return false; });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = "jistats_missing_data_provider_test.html";

    ReportResult result = service.generateReport(request);

    CHECK_FALSE(result.success);
    CHECK(result.errorMessage == "Failed to build dashboard data for report");
}

TEST_CASE("ReportingService generateReport captures provider exceptions", "[reporting]") {
    ReportingService service;
    service.setDashboardDataProvider([](DashboardViewModel&) -> bool {
        throw std::runtime_error("provider boom");
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = "jistats_provider_exception_test.html";

    ReportResult result = service.generateReport(request);

    CHECK_FALSE(result.success);
    CHECK(result.errorMessage.find("Failed to build dashboard data for report: provider boom") != std::string::npos);
}

TEST_CASE("ReportingService generateReport writes HTML report for valid dashboard data", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "report.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel = makeDashboardViewModel();
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();

    ReportResult result = service.generateReport(request);

    REQUIRE(result.success);
    CHECK(result.outputPath == outputPath.string());
    CHECK(result.bytesWritten > 0);
    CHECK(std::filesystem::exists(outputPath));

    const std::string html = readFileContents(outputPath);
    CHECK(html.find("JiStats Usage Report") != std::string::npos);
    CHECK(html.find("OpenAI") != std::string::npos);
    CHECK(html.find("MiniMax") != std::string::npos);
    CHECK(html.find("OpenAI Plan") != std::string::npos);
}

TEST_CASE("ReportingService generateReport with empty output path uses suggested filename", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    service.setDefaultOutputDirectory(tempDir.path().string());
    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel = makeDashboardViewModel();
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath.clear();

    ReportResult result = service.generateReport(request);

    REQUIRE(result.success);
    const std::filesystem::path reportPath(result.outputPath);
    CHECK(reportPath.parent_path() == tempDir.path());
    CHECK(reportPath.extension() == ".html");
    CHECK(std::filesystem::exists(reportPath));
}

TEST_CASE("ReportingService providerFilter applies by provider key", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "filter_by_key.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel = makeDashboardViewModel();
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();
    request.providerFilter = {"openai"};

    ReportResult result = service.generateReport(request);
    REQUIRE(result.success);

    const std::string html = readFileContents(outputPath);
    CHECK(html.find("OpenAI Plan") != std::string::npos);
    CHECK(html.find("MiniMax Plan") == std::string::npos);
}

TEST_CASE("ReportingService providerFilter applies by display name", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "filter_by_display_name.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel = makeDashboardViewModel();
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();
    request.providerFilter = {"MiniMax"};

    ReportResult result = service.generateReport(request);
    REQUIRE(result.success);

    const std::string html = readFileContents(outputPath);
    CHECK(html.find("MiniMax Plan") != std::string::npos);
    CHECK(html.find("OpenAI Plan") == std::string::npos);
}

TEST_CASE("ReportingService accountFilter applies to the selected account scope", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "filter_by_account.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel.appVersion = "1.0.0-test";
        viewModel.lastUpdateTime = "2026-04-18 00:01:00 UTC";
        viewModel.isOnline = true;
        viewModel.providers = {
            makeProviderWithState(
                "openai",
                "OpenAI",
                ProviderState::Live,
                "2026-04-18 00:00:00 UTC",
                "acct-openai-primary",
                {makeQuota("OpenAI Primary Plan", 120, 1000, "high")}),
            makeProviderWithState(
                "openai",
                "OpenAI",
                ProviderState::Live,
                "2026-04-18 00:00:00 UTC",
                "acct-openai-secondary",
                {makeQuota("OpenAI Secondary Plan", 80, 1000, "high")})
        };
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();
    request.providerFilter = {"openai"};
    request.accountFilter = {"acct-openai-secondary"};

    const ReportResult result = service.generateReport(request);
    REQUIRE(result.success);
    REQUIRE(result.metadata.accountIds.size() == 1);
    CHECK(result.metadata.accountIds.front() == "acct-openai-secondary");

    const std::string html = readFileContents(outputPath);
    CHECK(html.find("OpenAI Secondary Plan") != std::string::npos);
    CHECK(html.find("OpenAI Primary Plan") == std::string::npos);
    CHECK(html.find("acct-openai-secondary") != std::string::npos);
}

TEST_CASE("ReportingService providerFilter recomputes runtime state when hidden provider is offline", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "filter_runtime_hidden_offline.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel = makeDashboardViewModel();
        viewModel.runtimeState = RuntimeState::Offline;
        viewModel.providers[0].state = ProviderState::Live;
        viewModel.providers[1].state = ProviderState::Offline;
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();
    request.providerFilter = {"openai"};

    ReportResult result = service.generateReport(request);
    REQUIRE(result.success);

    CHECK(result.metadata.runtimeState == RuntimeState::Current);
    CHECK_FALSE(result.metadata.isOffline);
    CHECK_FALSE(result.metadata.isStale);
    CHECK_FALSE(result.metadata.isSyncing);
    CHECK(result.metadata.freshnessLabel() == "Current");

    const std::string html = readFileContents(outputPath);
    CHECK(html.find("Current") != std::string::npos);
    CHECK(html.find("Catching Up") == std::string::npos);
    CHECK(html.find("MiniMax") == std::string::npos);
}

TEST_CASE("ReportingService providerFilter recomputes runtime state when hidden provider is stale", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "filter_runtime_hidden_stale.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel = makeDashboardViewModel();
        viewModel.runtimeState = RuntimeState::Stale;
        viewModel.providers[0].state = ProviderState::Live;
        viewModel.providers[1].state = ProviderState::Stale;
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();
    request.providerFilter = {"openai"};

    ReportResult result = service.generateReport(request);
    REQUIRE(result.success);

    CHECK(result.metadata.runtimeState == RuntimeState::Current);
    CHECK_FALSE(result.metadata.isOffline);
    CHECK_FALSE(result.metadata.isStale);
    CHECK_FALSE(result.metadata.isSyncing);
    CHECK(result.metadata.freshnessLabel() == "Current");

    const std::string html = readFileContents(outputPath);
    CHECK(html.find("Current") != std::string::npos);
    CHECK(html.find("MiniMax") == std::string::npos);
}

TEST_CASE("ReportingService providerFilter recomputes runtime state when hidden provider is syncing", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "filter_runtime_hidden_syncing.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel = makeDashboardViewModel();
        viewModel.runtimeState = RuntimeState::CatchingUp;
        viewModel.providers[0].state = ProviderState::Live;
        viewModel.providers[1].state = ProviderState::Syncing;
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();
    request.providerFilter = {"openai"};

    ReportResult result = service.generateReport(request);
    REQUIRE(result.success);

    CHECK(result.metadata.runtimeState == RuntimeState::Current);
    CHECK_FALSE(result.metadata.isOffline);
    CHECK_FALSE(result.metadata.isStale);
    CHECK_FALSE(result.metadata.isSyncing);
    CHECK(result.metadata.freshnessLabel() == "Current");

    const std::string html = readFileContents(outputPath);
    CHECK(html.find("Current") != std::string::npos);
    CHECK(html.find("Catching Up") == std::string::npos);
    CHECK(html.find("MiniMax") == std::string::npos);
}

TEST_CASE("ReportingService includeProviderQuotas false strips quota rows from report", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "without_quotas.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel = makeDashboardViewModel();
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();
    request.includeProviderQuotas = false;

    ReportResult result = service.generateReport(request);
    REQUIRE(result.success);

    const std::string html = readFileContents(outputPath);
    CHECK(html.find("Provider Quotas") != std::string::npos);
    CHECK(html.find("OpenAI Plan") == std::string::npos);
    CHECK(html.find("MiniMax Plan") == std::string::npos);
}

TEST_CASE("ReportingService includeProviderQuotas false preserves source-label metadata", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "without_quotas_source_labels.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel.appVersion = "1.0.0-test";
        viewModel.lastUpdateTime = "2026-04-18 00:01:00 UTC";
        viewModel.isOnline = true;
        viewModel.providers = {
            makeProvider(
                "openai",
                "OpenAI",
                {makeQuota(
                    "Codex Local Pressure",
                    25,
                    100,
                    "high",
                    "openai_subscription_codex_local")}),
            makeProvider("minimax", "MiniMax", {makeQuota("MiniMax Plan", 85, 500, "medium")})
        };
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();
    request.includeProviderQuotas = false;

    ReportResult result = service.generateReport(request);
    REQUIRE(result.success);
    REQUIRE(result.metadata.providerSurfaces.size() == 1);
    CHECK(result.metadata.providerSurfaces.front() == "openai_subscription_codex_local");

    const std::string html = readFileContents(outputPath);
    CHECK(html.find("Source Labels") != std::string::npos);
    CHECK(html.find("OpenAI Codex-local") != std::string::npos);
    CHECK(html.find("<th>Source</th>") == std::string::npos);
    CHECK(html.find("Codex Local Pressure") == std::string::npos);
}

TEST_CASE("ReportingService renders Claude local source labels without leaking raw provider surfaces", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "claude_kimi_source_labels.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel.appVersion = "1.0.0-test";
        viewModel.lastUpdateTime = "2026-04-20 00:01:00 UTC";
        viewModel.isOnline = true;
        viewModel.providers = {
            makeProvider(
                "claude_code",
                "Claude Code",
                {makeQuota("Local Activity", 1200, 0, "Unknown", "claude_code_local_stats")})
        };
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();

    ReportResult result = service.generateReport(request);
    REQUIRE(result.success);
    REQUIRE(result.metadata.providerSurfaces.size() == 1);

    const std::string html = readFileContents(outputPath);
    CHECK(html.find("Claude Local") != std::string::npos);
    CHECK(html.find("claude_code_local_stats") == std::string::npos);
}

TEST_CASE("ReportingService renders a friendly Kimi subscription source label", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "kimi_subscription_source_label.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel.appVersion = "1.0.0-test";
        viewModel.lastUpdateTime = "2026-04-20 00:01:00 UTC";
        viewModel.isOnline = true;
        viewModel.providers = {
            makeProvider(
                "kimi",
                "Kimi Code",
                {makeQuota("Subscription", 64, 100, "high", "kimi_subscription_api")})
        };
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();

    ReportResult result = service.generateReport(request);
    REQUIRE(result.success);

    const std::string html = readFileContents(outputPath);
    CHECK(html.find("Kimi Subscription") != std::string::npos);
    CHECK(html.find("kimi_subscription_api") == std::string::npos);
}

TEST_CASE("ReportingService generateReport populates metadata with provider scope", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "metadata_scope.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel = makeDashboardViewModel();
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();

    ReportResult result = service.generateReport(request);
    REQUIRE(result.success);
    REQUIRE(result.success);
    
    // Check metadata was populated
    CHECK(result.metadata.providerKeys.size() == 2);
    CHECK(std::find(result.metadata.providerKeys.begin(), result.metadata.providerKeys.end(), "openai") != result.metadata.providerKeys.end());
    CHECK(std::find(result.metadata.providerKeys.begin(), result.metadata.providerKeys.end(), "minimax") != result.metadata.providerKeys.end());
    CHECK(result.metadata.scopeLabel() == "Provider scope only");
}

TEST_CASE("ReportingService generateReport metadata reflects live provider state", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "metadata_live.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel = makeDashboardViewModel();
        // Live state
        for (auto& provider : viewModel.providers) {
            provider.state = ProviderState::Live;
        }
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();

    ReportResult result = service.generateReport(request);
    REQUIRE(result.success);
    
    CHECK(result.metadata.isOffline == false);
    CHECK(result.metadata.isStale == false);
}

TEST_CASE("ReportingService generateReport metadata reflects offline provider state", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "metadata_offline.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel = makeDashboardViewModel();
        // Offline state
        for (auto& provider : viewModel.providers) {
            provider.state = ProviderState::Offline;
        }
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();

    ReportResult result = service.generateReport(request);
    REQUIRE(result.success);
    
    CHECK(result.metadata.isOffline == true);
    CHECK(result.metadata.isStale == false);
}

TEST_CASE("ReportingService generateReport metadata reflects stale provider state", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "metadata_stale.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel = makeDashboardViewModel();
        // Stale state
        for (auto& provider : viewModel.providers) {
            provider.state = ProviderState::Stale;
        }
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();

    ReportResult result = service.generateReport(request);
    REQUIRE(result.success);
    
    CHECK(result.metadata.isOffline == false);
    CHECK(result.metadata.isStale == true);
}

TEST_CASE("ReportingService generateReport metadata reflects syncing provider state", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "metadata_syncing.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel = makeDashboardViewModel();
        viewModel.providers[0].state = ProviderState::Offline;
        viewModel.providers[1].state = ProviderState::Syncing;
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();

    ReportResult result = service.generateReport(request);
    REQUIRE(result.success);

    CHECK(result.metadata.isSyncing == true);
    CHECK(result.metadata.freshnessLabel() == "Catching Up");

    const std::string html = readFileContents(outputPath);
    CHECK(html.find("Catching Up") != std::string::npos);
}

TEST_CASE("ReportingService generateReport metadata contains backfill label in HTML", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "metadata_backfill_label.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel = makeDashboardViewModel();
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();

    ReportResult result = service.generateReport(request);
    REQUIRE(result.success);

    const std::string html = readFileContents(outputPath);
    // Check that metadata card is present
    CHECK(html.find("Report Data Quality") != std::string::npos);
    CHECK(html.find("Data Freshness") != std::string::npos);
    CHECK(html.find("Backfill Status") != std::string::npos);
    CHECK(html.find("Contains backfilled data: No") != std::string::npos);
}

TEST_CASE("ReportingService generateReport metadata with backfilled data shows correct label", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "metadata_backfill_yes.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel = makeDashboardViewModel();
        // Set backfill state on quotas - per plan.md section 13.1
        for (auto& provider : viewModel.providers) {
            for (auto& quota : provider.quotas) {
                quota.isBackfilled = true;
                quota.hasUnknownGaps = true;
            }
        }
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();

    ReportResult result = service.generateReport(request);
    REQUIRE(result.success);
    CHECK(result.metadata.containsUnknownGaps);
    CHECK(result.metadata.containsBackfilledData);

    const std::string html = readFileContents(outputPath);
    CHECK(html.find("Contains backfilled data: Yes") != std::string::npos);
    CHECK(html.find("Contains unknown gaps: Yes") != std::string::npos);
}

TEST_CASE("ReportingService generateReport parses runtime UTC timestamps for freshness", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "metadata_runtime_timestamp.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel = makeDashboardViewModel();
        viewModel.providers[0].lastSyncTime = "2026-04-18 03:15:30 UTC";
        viewModel.providers[1].lastSyncTime = "2026-04-18T02:15:30Z";
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();

    ReportResult result = service.generateReport(request);
    REQUIRE(result.success);

    const std::time_t freshnessTime =
        std::chrono::system_clock::to_time_t(result.metadata.dataFreshnessTimestamp);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &freshnessTime);
#else
    gmtime_r(&freshnessTime, &tm);
#endif

    CHECK(tm.tm_year == 126);
    CHECK(tm.tm_mon == 3);
    CHECK(tm.tm_mday == 18);
    CHECK(tm.tm_hour == 3);
    CHECK(tm.tm_min == 15);
    CHECK(tm.tm_sec == 30);
}

TEST_CASE("ReportingService generateReport renders explicit account scope labels", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "metadata_account_scope.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel.appVersion = "1.0.0-test";
        viewModel.lastUpdateTime = "2026-04-18 00:01:00 UTC";
        viewModel.isOnline = true;
        viewModel.providers = {
            makeProviderWithState(
                "openai",
                "OpenAI",
                ProviderState::Live,
                "2026-04-18 00:00:00 UTC",
                "acct-openai",
                {makeQuota("OpenAI Plan", 120, 1000, "high")}),
            makeProviderWithState(
                "minimax",
                "MiniMax",
                ProviderState::Live,
                "2026-04-18 00:00:00 UTC",
                "acct-minimax",
                {makeQuota("MiniMax Plan", 85, 500, "medium")})
        };
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();

    ReportResult result = service.generateReport(request);
    REQUIRE(result.success);

    CHECK(result.metadata.accountScopeComplete == true);
    CHECK(result.metadata.scopeLabel() == "Provider and account scope");
    CHECK(result.metadata.accountIds.size() == 2);

    const std::string html = readFileContents(outputPath);
    CHECK(html.find("Account Scope") != std::string::npos);
    CHECK(html.find("Provider and account scope") != std::string::npos);
    CHECK(html.find("acct-openai") != std::string::npos);
    CHECK(html.find("acct-minimax") != std::string::npos);
}

TEST_CASE("ReportingService generateReport uses mirror policy version provider when available", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "metadata_mirror_policy.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel = makeDashboardViewModel();
        return true;
    });
    service.setMirrorPolicyVersionProvider([](const DashboardViewModel&) {
        return std::string("v7");
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();

    ReportResult result = service.generateReport(request);
    REQUIRE(result.success);

    CHECK(result.metadata.mirrorPolicyVersion == "v7");

    const std::string html = readFileContents(outputPath);
    CHECK(html.find("Mirror Policy") != std::string::npos);
    CHECK(html.find("v7") != std::string::npos);
}

TEST_CASE("ReportingService generateReport renders normalized OpenAI source labels when available", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "metadata_source_labels.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel.appVersion = "1.0.0-test";
        viewModel.lastUpdateTime = "2026-04-18 00:01:00 UTC";
        viewModel.isOnline = true;
        viewModel.providers = {
            makeProvider(
                "openai",
                "OpenAI",
                {makeQuota(
                    "Codex Local Pressure",
                    25,
                    100,
                    "high",
                    "openai_subscription_codex_local")}),
            makeProvider(
                "minimax",
                "MiniMax",
                {makeQuota("MiniMax Plan", 85, 500, "medium")})
        };
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();

    ReportResult result = service.generateReport(request);
    REQUIRE(result.success);
    CHECK(result.metadata.providerSurfaces.size() == 1);
    CHECK(result.metadata.providerSurfaces.front() == "openai_subscription_codex_local");

    const std::string html = readFileContents(outputPath);
    CHECK(html.find("Source Labels") != std::string::npos);
    CHECK(html.find("OpenAI Codex-local") != std::string::npos);
    CHECK(html.find("<th>Source</th>") != std::string::npos);
}

TEST_CASE("ReportingService generateReport renders OpenAI API-platform source labels with distinct user-facing wording", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "metadata_api_source_labels.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel.appVersion = "1.0.0-test";
        viewModel.lastUpdateTime = "2026-04-18 00:01:00 UTC";
        viewModel.isOnline = true;
        viewModel.providers = {
            makeProvider(
                "openai",
                "OpenAI",
                {makeQuota(
                    "API Platform Import",
                    25,
                    100,
                    "high",
                    "openai_api_import")})
        };
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();

    ReportResult result = service.generateReport(request);
    REQUIRE(result.success);
    REQUIRE(result.metadata.providerSurfaces.size() == 1);
    CHECK(result.metadata.providerSurfaces.front() == "openai_api_import");

    const std::string html = readFileContents(outputPath);
    CHECK(html.find("OpenAI API Platform Import") != std::string::npos);
    CHECK(html.find("openai_api_import") == std::string::npos);
}

TEST_CASE("ReportingService generateReport renders the Phase 6 HTML export sections when aggregate data is provided", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path outputPath = tempDir.path() / "phase6_sections.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel = makeDashboardViewModel();
        return true;
    });
    service.setAggregateDataProvider([](const ReportRequest&, const DashboardViewModel&, const ReportMetadata&) {
        return makeAggregateData();
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = outputPath.string();

    const ReportResult result = service.generateReport(request);
    REQUIRE(result.success);
    CHECK(result.aggregateData.dailySummaries.size() == 2);
    CHECK(result.aggregateData.providerBreakdown.size() == 2);

    const std::string html = readFileContents(outputPath);
    CHECK(html.find("Daily Summary") != std::string::npos);
    CHECK(html.find("Weekly Summary") != std::string::npos);
    CHECK(html.find("Provider Breakdown") != std::string::npos);
    CHECK(html.find("Account Breakdown") != std::string::npos);
    CHECK(html.find("Model Breakdown") != std::string::npos);
    CHECK(html.find("Recent Trend") != std::string::npos);
    CHECK(html.find("Current Quota Mirror Snapshots") != std::string::npos);
    CHECK(html.find("reflect current provider state at export time") != std::string::npos);
    CHECK(html.find("Recovery And Gap Summary") != std::string::npos);
    CHECK(html.find("OpenAI Account") != std::string::npos);
    CHECK(html.find("gpt-4.1") != std::string::npos);
}

TEST_CASE("ReportingService suggestFilename carries scope context when provider and date range are known", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    service.setDefaultOutputDirectory(tempDir.path().string());

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.providerFilter = {"openai"};
    request.accountFilter = {"acct-openai-secondary"};
    request.dateRange.start = std::chrono::system_clock::from_time_t(1715472000); // 2024-05-12 UTC
    request.dateRange.end = std::chrono::system_clock::from_time_t(1716076800);   // 2024-05-19 UTC

    const std::filesystem::path suggested(service.suggestFilename(request));
    CHECK(suggested.parent_path() == tempDir.path());
    CHECK(suggested.filename().string().find("openai") != std::string::npos);
    CHECK(suggested.filename().string().find("acct_openai_secondary") != std::string::npos);
    CHECK(suggested.filename().string().find("_to_") != std::string::npos);
}

TEST_CASE("ReportingService manual export request generates valid HTML file at explicit path", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path explicitPath = tempDir.path() / "manual-export.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel = makeDashboardViewModel();
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = explicitPath.string();

    const ReportResult result = service.generateReport(request);
    REQUIRE(result.success);
    CHECK(result.outputPath == explicitPath.string());
    CHECK(std::filesystem::exists(result.outputPath));
    CHECK(result.bytesWritten > 0);

    std::ifstream file(result.outputPath);
    REQUIRE(file.is_open());
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    CHECK(content.find("<!DOCTYPE html>") != std::string::npos);
    CHECK(content.find("JiStats") != std::string::npos);
}

TEST_CASE("ReportingService manual export respects provider and account scope filters", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path explicitPath = tempDir.path() / "scoped-export.html";

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel = makeDashboardViewModel();
        viewModel.providers.push_back(
            makeProvider("openai", "OpenAI", {makeQuota("Plus", 80, 100, "high")}));
        viewModel.providers.push_back(
            makeProvider("minimax", "MiniMax", {makeQuota("Coding", 40, 50, "medium")}));
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = explicitPath.string();
    request.providerFilter.push_back("openai");

    const ReportResult result = service.generateReport(request);
    REQUIRE(result.success);
    CHECK(result.metadata.providerKeys.size() >= 1);
}

TEST_CASE("ReportingService manual export preserves an explicit output path even when the file already exists", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    const std::filesystem::path requestedPath = tempDir.path() / "report.html";
    {
        std::ofstream existing(requestedPath);
        REQUIRE(existing.is_open());
        existing << "existing";
    }

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel = makeDashboardViewModel();
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;
    request.outputPath = requestedPath.string();

    const ReportResult result = service.generateReport(request);
    REQUIRE(result.success);
    CHECK(result.outputPath == requestedPath.string());
    CHECK(std::filesystem::exists(requestedPath));

    const std::string html = readFileContents(requestedPath);
    CHECK(html.find("<!DOCTYPE html>") != std::string::npos);
    CHECK(html.find("existing") == std::string::npos);
}

TEST_CASE("ReportingService auto-generated output path stays unique when the suggested filename already exists", "[reporting]") {
    ReportingService service;
    TempTestDirectory tempDir("jistats_report");
    service.setDefaultOutputDirectory(tempDir.path().string());

    service.setDashboardDataProvider([](DashboardViewModel& viewModel) {
        viewModel = makeDashboardViewModel();
        return true;
    });

    ReportRequest request;
    request.format = ReportFormat::Html;

    const std::filesystem::path suggestedPath(service.suggestFilename(request));
    {
        std::ofstream existing(suggestedPath);
        REQUIRE(existing.is_open());
        existing << "existing";
    }

    const ReportResult result = service.generateReport(request);
    REQUIRE(result.success);
    CHECK(result.outputPath != suggestedPath.string());
    CHECK(std::filesystem::exists(suggestedPath));
    CHECK(std::filesystem::exists(result.outputPath));
}
