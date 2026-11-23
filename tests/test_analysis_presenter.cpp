#define JISTATS_ENABLE_TEST_HOOKS 1

#include <catch_amalgamated.hpp>

#include <optional>
#include <string>
#include <vector>

#include "gui/AnalysisPresenter.hpp"

using namespace JiStats;

namespace {

std::time_t makeUtcTime(
    int year,
    int month,
    int day,
    int hour = 0,
    int minute = 0,
    int second = 0) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    return _mkgmtime(&tm);
}

class ScopedParentWindow {
public:
    ScopedParentWindow() {
        hwnd_ = CreateWindowExW(
            0,
            L"STATIC",
            L"AnalysisPresenterTests",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            640,
            240,
            nullptr,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
    }

    ~ScopedParentWindow() {
        if (hwnd_ != nullptr) {
            DestroyWindow(hwnd_);
        }
    }

    HWND get() const {
        return hwnd_;
    }

private:
    HWND hwnd_ = nullptr;
};

AnalysisViewModel makeViewModel() {
    AnalysisViewModel model;
    model.scope.selectedProviderId = "openai";
    model.scope.selectedAccountId = "acct-openai-primary";
    model.scope.dateRangePreset = AnalysisDateRangePreset::Last7Days;
    model.scope.customRangeStartUtc = makeUtcTime(2026, 4, 10, 0, 0, 0);
    model.scope.customRangeEndUtc = makeUtcTime(2026, 4, 12, 0, 0, 0);
    model.availableProviders = {
        {"", "All Providers"},
        {"openai", "OpenAI"},
        {"minimax", "MiniMax"}
    };
    model.availableAccounts = {
        {"", "All Accounts"},
        {"acct-openai-primary", "OpenAI / acct-openai-primary"},
        {"acct-minimax", "MiniMax / acct-minimax"}
    };
    model.scopeLabel = "OpenAI / OpenAI / acct-openai-primary / Last 7 Days";
    model.dailySummaries = {
        {"2026-04-10", 2, "OpenAI Codex-local", {}},
        {"2026-04-11", 1, "OpenAI Local", {}}
    };
    model.providerBreakdown = {
        {"openai", "OpenAI", "", 3, "Mixed OpenAI provenance"}
    };
    model.accountBreakdown = {
        {"acct-openai-primary|OpenAI", "OpenAI / acct-openai-primary", "OpenAI", 3, "Mixed OpenAI provenance"}
    };
    model.trendPoints = {
        {"2026-04-10", 2},
        {"2026-04-11", 1}
    };
    model.gapSummaryCounts.unknownDays = 1;
    model.dataQualityNote =
        "Rollup-backed daily history across the current analysis scope with 1 unknown gap day.";
    return model;
}

AnalysisViewModel makeQuotaTrendViewModel() {
    AnalysisViewModel model = makeViewModel();
    AnalysisQuotaTrendSeries series;
    series.key = "openai-plan|requests|5h";
    series.label = "OpenAI 5h";
    series.windowLabel = "5h";
    series.quotaDimension = "requests";
    series.points = {
        {"04-10 09:00", makeUtcTime(2026, 4, 10, 9, 0, 0), 82.0, 820, 1000, false, true},
        {"04-10 14:00", makeUtcTime(2026, 4, 10, 14, 0, 0), 41.0, 410, 1000, false, true},
        {"04-10 19:00", makeUtcTime(2026, 4, 10, 19, 0, 0), 95.0, 950, 1000, true, true}
    };
    series.latestRemainingPercentage = 95.0;
    series.lowWaterRemainingPercentage = 41.0;
    series.latestRemainingValue = 950;
    series.limitValue = 1000;
    series.resetCount = 1;
    model.quotaTrendSeries.push_back(series);
    return model;
}

AnalysisViewModel makeMultiQuotaTrendViewModel() {
    AnalysisViewModel model = makeQuotaTrendViewModel();
    AnalysisQuotaTrendSeries secondary;
    secondary.key = "minimax-plan|tokens|1d";
    secondary.label = "MiniMax 1d";
    secondary.windowLabel = "1d";
    secondary.quotaDimension = "tokens";
    secondary.points = {
        {"04-10", makeUtcTime(2026, 4, 10, 0, 0, 0), 64.0, 6400, 10000, false, true},
        {"04-11", makeUtcTime(2026, 4, 11, 0, 0, 0), 52.0, 5200, 10000, false, true}
    };
    secondary.latestRemainingPercentage = 52.0;
    secondary.lowWaterRemainingPercentage = 52.0;
    secondary.latestRemainingValue = 5200;
    secondary.limitValue = 10000;
    secondary.resetCount = 0;
    model.quotaTrendSeries.push_back(secondary);
    return model;
}

} // namespace

TEST_CASE("AnalysisPresenter renders Slice B scope labels and options from the view model", "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);

    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    presenter.updateViewModel(makeViewModel());

    CHECK(presenter.getScopeLabelForTesting() == "Active filters: OpenAI / OpenAI / acct-openai-primary / Last 7 Days");
    CHECK(presenter.getSectionTitlesForTesting() == std::vector<std::string>{
        "Filters & Scope",
        "Overview",
        "Activity Trend",
        "Operational Signals",
        "By Provider",
        "By Account",
        "Daily Activity",
        "Coverage & Provenance"
    });
    CHECK(presenter.getCustomDateHintForTesting() == "Use YYYY-MM-DD dates. Changes apply after you pause typing.");
    CHECK(presenter.getWorkspaceGuideTextForTesting() ==
        "Workspace guide: jump to the section you want to review next, then refine the provider, account, and date filters above.");
    CHECK(presenter.getWorkspaceNavigationLabelsForTesting() == std::vector<std::string>{
        "Overview",
        "Trend",
        "Operational Signals",
        "Breakdowns",
        "Daily Activity",
        "Coverage & Provenance"
    });
    CHECK(presenter.getProviderOptionsForTesting() == std::vector<std::string>{
        "All Providers",
        "OpenAI",
        "MiniMax"
    });
    CHECK(presenter.getAccountOptionsForTesting() == std::vector<std::string>{
        "All Accounts",
        "OpenAI / acct-openai-primary",
        "MiniMax / acct-minimax"
    });
    CHECK(presenter.getRangeOptionsForTesting() == std::vector<std::string>{
        "Today",
        "Last 24 Hours",
        "Last 7 Days",
        "Current Billing/Quota Window",
        "This Month",
        "Custom"
    });

    presenter.shutdown();
}

TEST_CASE("AnalysisPresenter emits provider scope changes through the Slice B callback seam", "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);

    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    AnalysisViewModel model = makeViewModel();
    presenter.updateViewModel(model);

    std::optional<AnalysisScopeState> lastScope;
    int callbackCount = 0;
    presenter.setScopeChangedCallback([&](const AnalysisScopeState& state) {
        lastScope = state;
        ++callbackCount;
    });

    REQUIRE(presenter.selectProviderForTesting("minimax"));
    REQUIRE(lastScope.has_value());
    CHECK(callbackCount == 1);
    CHECK(lastScope->selectedProviderId == "minimax");
    CHECK(lastScope->selectedAccountId.empty());
    CHECK(lastScope->dateRangePreset == AnalysisDateRangePreset::Last7Days);

    presenter.shutdown();
}

TEST_CASE("AnalysisPresenter emits date-range preset changes without inventing Slice C data", "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);

    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    AnalysisViewModel model = makeViewModel();
    model.scope.selectedProviderId.clear();
    model.scope.selectedAccountId.clear();
    presenter.updateViewModel(model);

    std::optional<AnalysisScopeState> lastScope;
    presenter.setScopeChangedCallback([&](const AnalysisScopeState& state) {
        lastScope = state;
    });

    REQUIRE(presenter.selectRangePresetForTesting(AnalysisDateRangePreset::Custom));
    REQUIRE(lastScope.has_value());
    CHECK(lastScope->selectedProviderId.empty());
    CHECK(lastScope->selectedAccountId.empty());
    CHECK(lastScope->dateRangePreset == AnalysisDateRangePreset::Custom);
    CHECK(lastScope->customRangeStartUtc == model.scope.customRangeStartUtc);
    CHECK(lastScope->customRangeEndUtc == model.scope.customRangeEndUtc);

    presenter.shutdown();
}

TEST_CASE("AnalysisPresenter surfaces a pending refresh status while async analysis work is in flight", "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);

    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    presenter.updateViewModel(makeViewModel());
    presenter.setInteractionPending(true);

    CHECK(presenter.getStatusHeadlineForTesting() == "Refreshing analysis for the updated scope...");
    CHECK_FALSE(presenter.areScopeInputsEnabledForTesting());

    presenter.updateViewModel(makeViewModel());
    CHECK(presenter.getStatusHeadlineForTesting() ==
        "No scoped recovery, quota-history, or export metadata is available yet.");
    CHECK(presenter.areScopeInputsEnabledForTesting());

    presenter.shutdown();
}

TEST_CASE("AnalysisPresenter trend summary surfaces average and low-water hints", "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);

    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    presenter.updateViewModel(makeViewModel());

    const std::string trendSummary = presenter.getTrendSummaryForTesting();
    CHECK(trendSummary.find("Avg: 1 per point.") != std::string::npos);
    CHECK(trendSummary.find("Low: 2026-04-11 (1).") != std::string::npos);
    CHECK(trendSummary.find("Latest: 2026-04-11 (1), down 1 vs previous point.") != std::string::npos);

    presenter.shutdown();
}

TEST_CASE("AnalysisPresenter surfaces Codex throughput in overview and trend text", "[analysis][presenter][analysis_presenter][codex]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);

    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    AnalysisViewModel model = makeViewModel();
    model.codexThroughput.hasData = true;
    model.codexThroughput.tokensPerMinute5m = 60.0;
    model.codexThroughput.promptsPerMinute5m = 0.4;
    model.codexThroughput.tokensPerMinute30m = 9.8;
    model.codexThroughput.promptsPerMinute30m = 0.7;
    model.codexThroughput.confidenceLabel = "LocalExact";

    presenter.updateViewModel(model);

    const std::string overviewStats = presenter.getOverviewStatsForTesting();
    CHECK(overviewStats.find("Codex throughput: 60 tokens/min and 0.4 prompts/min over 5m") != std::string::npos);
    CHECK(overviewStats.find("(LocalExact)") != std::string::npos);

    const std::string trendSummary = presenter.getTrendSummaryForTesting();
    CHECK(trendSummary.find("Codex throughput (5m): 60 tokens/min, 0.4 prompts/min") != std::string::npos);
    CHECK(trendSummary.find("Codex throughput (30m): 9.8 tokens/min, 0.7 prompts/min") != std::string::npos);

    presenter.shutdown();
}

TEST_CASE("AnalysisPresenter surfaces scoped quota trend low-water and reset hints", "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);

    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    presenter.updateViewModel(makeQuotaTrendViewModel());

    const std::string trendSummary = presenter.getTrendSummaryForTesting();
    CHECK(trendSummary.find("Quota snapshot: OpenAI 5h at 95% remaining (950 / 1,000).") != std::string::npos);
    CHECK(trendSummary.find("Low-water: 41% at 04-10 14:00.") != std::string::npos);
    CHECK(trendSummary.find("Resets: 1.") != std::string::npos);

    presenter.shutdown();
}

TEST_CASE("AnalysisPresenter lets the user switch between scoped quota trend series", "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);

    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    presenter.updateViewModel(makeMultiQuotaTrendViewModel());

    CHECK(presenter.getQuotaSeriesOptionsForTesting() == std::vector<std::string>{
        "OpenAI 5h",
        "MiniMax 1d"
    });
    CHECK(presenter.getTrendSummaryForTesting().find("Quota snapshot: OpenAI 5h at 95% remaining") != std::string::npos);

    REQUIRE(presenter.selectQuotaSeriesForTesting("minimax-plan|tokens|1d"));
    CHECK(presenter.getTrendSummaryForTesting().find("Quota snapshot: MiniMax 1d at 52% remaining") != std::string::npos);
    CHECK(presenter.getTrendSummaryForTesting().find("Low-water: 52% at 04-11.") != std::string::npos);

    presenter.shutdown();
}

TEST_CASE("AnalysisPresenter exposes trend comparison and drill-down detail text", "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);

    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    presenter.updateViewModel(makeMultiQuotaTrendViewModel());

    const std::string comparison = presenter.getTrendComparisonTextForTesting();
    CHECK(comparison.find("Series comparison:") != std::string::npos);
    CHECK(comparison.find("OpenAI 5h") != std::string::npos);
    CHECK(comparison.find("MiniMax 1d") != std::string::npos);

    const std::string defaultDetail = presenter.getTrendDetailTextForTesting();
    CHECK(defaultDetail.find("OpenAI 5h") != std::string::npos);
    CHECK(defaultDetail.find("Remaining 95%") != std::string::npos);

    REQUIRE(presenter.selectQuotaTrendPointForTesting(1));
    const std::string selectedQuotaDetail = presenter.getTrendDetailTextForTesting();
    CHECK(selectedQuotaDetail.find("OpenAI 5h") != std::string::npos);
    CHECK(selectedQuotaDetail.find("Remaining 41%") != std::string::npos);

    REQUIRE(presenter.selectTrendBucketForTesting(0));
    const std::string selectedActivityDetail = presenter.getTrendDetailTextForTesting();
    CHECK(selectedActivityDetail.find("2026-04-10") != std::string::npos);
    CHECK(selectedActivityDetail.find("Count 2") != std::string::npos);

    presenter.shutdown();
}

TEST_CASE("AnalysisPresenter stacks sections cleanly in a narrow analysis window", "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);

    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    presenter.updateViewModel(makeViewModel());
    presenter.layout(RECT{0, 0, 760, 980});

    const RECT summary = presenter.getSectionBoundsForTesting("summary");
    const RECT trend = presenter.getSectionBoundsForTesting("trend");
    const RECT status = presenter.getSectionBoundsForTesting("status");
    const RECT provider = presenter.getSectionBoundsForTesting("provider");
    const RECT account = presenter.getSectionBoundsForTesting("account");
    const RECT daily = presenter.getSectionBoundsForTesting("daily");
    const RECT provenance = presenter.getSectionBoundsForTesting("provenance");

    CHECK(trend.left == summary.left);
    CHECK(status.left == summary.left);
    CHECK(provider.left == summary.left);
    CHECK(account.left == summary.left);
    CHECK(daily.left == summary.left);
    CHECK(provenance.left == summary.left);

    CHECK(trend.top >= summary.bottom);
    CHECK(status.top >= trend.bottom);
    CHECK(provider.top >= status.bottom);
    CHECK(account.top >= provider.bottom);
    CHECK(daily.top >= account.bottom);
    CHECK(provenance.top >= daily.bottom);
    CHECK(presenter.getContentHeightForTesting() >= presenter.getViewportHeightForTesting());

    presenter.shutdown();
}

TEST_CASE("AnalysisPresenter uses vertical scrolling instead of over-compressing small windows", "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);

    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    presenter.updateViewModel(makeMultiQuotaTrendViewModel());
    presenter.layout(RECT{0, 0, 760, 520});

    CHECK(presenter.getContentHeightForTesting() > presenter.getViewportHeightForTesting());
    CHECK(presenter.getScrollOffsetForTesting() == 0);

    const RECT provenanceBefore = presenter.getSectionBoundsForTesting("provenance");
    REQUIRE(presenter.handleVScroll(SB_PAGEDOWN));
    CHECK(presenter.getScrollOffsetForTesting() > 0);

    const RECT provenanceAfter = presenter.getSectionBoundsForTesting("provenance");
    CHECK(provenanceBefore.top - provenanceAfter.top == presenter.getScrollOffsetForTesting());
    CHECK(provenanceBefore.bottom - provenanceAfter.bottom == presenter.getScrollOffsetForTesting());

    REQUIRE(presenter.handleVScroll(SB_BOTTOM));
    CHECK(presenter.getScrollOffsetForTesting() ==
          presenter.getContentHeightForTesting() - presenter.getViewportHeightForTesting());
    REQUIRE(presenter.handleVScroll(SB_TOP));
    const RECT provenanceRestored = presenter.getSectionBoundsForTesting("provenance");
    CHECK(EqualRect(&provenanceRestored, &provenanceBefore));

    presenter.shutdown();
}

TEST_CASE("AnalysisPresenter workspace navigation jumps to later sections in the dedicated window", "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);

    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    presenter.updateViewModel(makeMultiQuotaTrendViewModel());
    presenter.layout(RECT{0, 0, 760, 520});

    const RECT dailyBefore = presenter.getSectionBoundsForTesting("daily");
    REQUIRE(presenter.jumpToSectionForTesting("daily"));

    CHECK(presenter.getScrollOffsetForTesting() > 0);
    const RECT dailyAfter = presenter.getSectionBoundsForTesting("daily");
    CHECK(dailyAfter.top < dailyBefore.top);
    CHECK(dailyAfter.top <= presenter.getViewportHeightForTesting() / 3);

    presenter.shutdown();
}

TEST_CASE("AnalysisPresenter debounces custom date edits until commit notifications", "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);

    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    AnalysisViewModel model = makeViewModel();
    presenter.updateViewModel(model);

    std::vector<AnalysisScopeState> scopes;
    presenter.setScopeChangedCallback([&](const AnalysisScopeState& state) {
        scopes.push_back(state);
    });

    presenter.setCustomRangeTextForTesting("2026-04-01", "2026-04-05");
    REQUIRE(presenter.handleCommand(2104, EN_CHANGE));
    CHECK(scopes.empty());

    REQUIRE(presenter.handleCommand(2105, EN_KILLFOCUS));
    REQUIRE(scopes.size() == 1);
    CHECK(scopes.front().dateRangePreset == AnalysisDateRangePreset::Custom);
    CHECK(scopes.front().customRangeStartUtc == makeUtcTime(2026, 4, 1));
    CHECK(scopes.front().customRangeEndUtc == makeUtcTime(2026, 4, 5));

    presenter.shutdown();
}
