#define JISTATS_ENABLE_TEST_HOOKS 1

#include <catch_amalgamated.hpp>

#include <optional>
#include <string>
#include <vector>

#include "gui/AnalysisPresenter.hpp"

#include <commctrl.h>

using namespace JiStats;

namespace {

std::time_t makeUtcTime(int year, int month, int day, int hour = 0, int minute = 0, int second = 0) {
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
        hwnd_ =
            CreateWindowExW(0, L"STATIC", L"AnalysisPresenterTests", WS_OVERLAPPEDWINDOW | WS_VSCROLL, CW_USEDEFAULT,
                            CW_USEDEFAULT, 800, 640, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
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

HWND historyTabs(HWND parent) {
    return FindWindowExW(parent, nullptr, WC_TABCONTROLW, nullptr);
}

void selectPage(HWND parent, int index) {
    const HWND tabs = historyTabs(parent);
    REQUIRE(tabs != nullptr);
    REQUIRE(TabCtrl_GetItemCount(tabs) == 3);
    TabCtrl_SetCurSel(tabs, index);
    NMHDR notice{tabs, 0, TCN_SELCHANGE};
    SendMessageW(parent, WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&notice));
}

std::wstring tabTitle(HWND tabs, int index) {
    wchar_t title[32]{};
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = title;
    item.cchTextMax = static_cast<int>(std::size(title));
    SendMessageW(tabs, TCM_GETITEMW, index, reinterpret_cast<LPARAM>(&item));
    return title;
}

HWND findDescendant(HWND parent, int controlId) {
    for (HWND child = GetWindow(parent, GW_CHILD); child != nullptr; child = GetWindow(child, GW_HWNDNEXT)) {
        if (GetDlgCtrlID(child) == controlId) {
            return child;
        }
        if (HWND descendant = findDescendant(child, controlId)) {
            return descendant;
        }
    }
    return nullptr;
}

AnalysisViewModel makeViewModel() {
    AnalysisViewModel model;
    model.scope.selectedProviderId = "openai";
    model.scope.selectedAccountId = "acct-openai-primary";
    model.scope.dateRangePreset = AnalysisDateRangePreset::Last7Days;
    model.scope.customRangeStartUtc = makeUtcTime(2026, 4, 10, 0, 0, 0);
    model.scope.customRangeEndUtc = makeUtcTime(2026, 4, 12, 0, 0, 0);
    model.availableProviders = {{"", "All Providers"}, {"openai", "OpenAI"}, {"minimax", "MiniMax"}};
    model.availableAccounts = {{"", "All Accounts"},
                               {"acct-openai-primary", "OpenAI / acct-openai-primary"},
                               {"acct-minimax", "MiniMax / acct-minimax"}};
    model.scopeLabel = "OpenAI / OpenAI / acct-openai-primary / Last 7 Days";
    model.effectiveStartUtc = makeUtcTime(2026, 4, 10);
    model.effectiveEndUtc = makeUtcTime(2026, 4, 12, 9, 30);
    model.dailySummaries = {{"2026-04-10", 2, "OpenAI Codex-local", {}}, {"2026-04-11", 1, "OpenAI Local", {}}};
    model.providerBreakdown = {{"openai", "OpenAI", "", 3, "Mixed OpenAI provenance"}};
    model.accountBreakdown = {
        {"acct-openai-primary|OpenAI", "OpenAI / acct-openai-primary", "OpenAI", 3, "Mixed OpenAI provenance"}};
    model.trendPoints = {
        {"2026-04-10", 2, makeUtcTime(2026, 4, 10), makeUtcTime(2026, 4, 10, 23, 59), "OpenAI Codex-local"},
        {"2026-04-11", 1, makeUtcTime(2026, 4, 11), makeUtcTime(2026, 4, 11, 23, 59), "OpenAI Local"}};
    model.gapSummaryCounts.unknownDays = 1;
    model.dataQualityNote = "Rollup-backed daily history across the current analysis scope with 1 unknown gap day.";
    return model;
}

AnalysisViewModel makeQuotaTrendViewModel() {
    AnalysisViewModel model = makeViewModel();
    AnalysisQuotaTrendSeries series;
    series.key = "openai-plan|requests|5h";
    series.label = "OpenAI 5h";
    series.windowLabel = "5h";
    series.quotaDimension = "requests";
    series.points = {{"04-10 09:00", makeUtcTime(2026, 4, 10, 9, 0, 0), 82.0, 820, 1000, false, true},
                     {"04-10 14:00", makeUtcTime(2026, 4, 10, 14, 0, 0), 41.0, 410, 1000, false, true},
                     {"04-10 19:00", makeUtcTime(2026, 4, 10, 19, 0, 0), 95.0, 950, 1000, true, true}};
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
    secondary.points = {{"04-10", makeUtcTime(2026, 4, 10, 0, 0, 0), 64.0, 6400, 10000, false, true},
                        {"04-11", makeUtcTime(2026, 4, 11, 0, 0, 0), 52.0, 5200, 10000, false, true}};
    secondary.latestRemainingPercentage = 52.0;
    secondary.lowWaterRemainingPercentage = 52.0;
    secondary.latestRemainingValue = 5200;
    secondary.limitValue = 10000;
    secondary.resetCount = 0;
    model.quotaTrendSeries.push_back(secondary);
    return model;
}

} // namespace

TEST_CASE("AnalysisPresenter displays the effective scope and three history pages",
          "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);

    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    presenter.updateViewModel(makeViewModel());

    CHECK(presenter.getScopeLabelForTesting() == "Active filters: OpenAI / OpenAI / acct-openai-primary / Last 7 Days");
    CHECK(presenter.getStatusHeadlineForTesting() == "2026-04-10 00:00 to 2026-04-12 09:30 UTC");
    const HWND tabs = historyTabs(parent.get());
    REQUIRE(tabs != nullptr);
    REQUIRE(TabCtrl_GetItemCount(tabs) == 3);
    CHECK(tabTitle(tabs, 0) == L"Activity");
    CHECK(tabTitle(tabs, 1) == L"Quota history");
    CHECK(tabTitle(tabs, 2) == L"Coverage");
    CHECK(presenter.getProviderOptionsForTesting() == std::vector<std::string>{"All Providers", "OpenAI", "MiniMax"});
    CHECK(presenter.getAccountOptionsForTesting() ==
          std::vector<std::string>{"All Accounts", "OpenAI / acct-openai-primary", "MiniMax / acct-minimax"});
    CHECK(presenter.getRangeOptionsForTesting() == std::vector<std::string>{"Today", "Last 24 Hours", "Last 7 Days",
                                                                            "Current Billing/Quota Window",
                                                                            "This Month", "Custom"});

    presenter.shutdown();
}

TEST_CASE("AnalysisPresenter clears the account when the provider changes",
          "[analysis][presenter][analysis_presenter]") {
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

TEST_CASE("AnalysisPresenter applies a saved custom date range", "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);

    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    AnalysisViewModel model = makeViewModel();
    model.scope.selectedProviderId.clear();
    model.scope.selectedAccountId.clear();
    presenter.updateViewModel(model);

    std::optional<AnalysisScopeState> lastScope;
    presenter.setScopeChangedCallback([&](const AnalysisScopeState& state) { lastScope = state; });

    REQUIRE(presenter.selectRangePresetForTesting(AnalysisDateRangePreset::Custom));
    REQUIRE(lastScope.has_value());
    CHECK(lastScope->selectedProviderId.empty());
    CHECK(lastScope->selectedAccountId.empty());
    CHECK(lastScope->dateRangePreset == AnalysisDateRangePreset::Custom);
    CHECK(lastScope->customRangeStartUtc == model.scope.customRangeStartUtc);
    CHECK(lastScope->customRangeEndUtc == model.scope.customRangeEndUtc);

    presenter.shutdown();
}

TEST_CASE("AnalysisPresenter surfaces a pending refresh status while async analysis work is in flight",
          "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);

    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    presenter.updateViewModel(makeViewModel());
    presenter.setInteractionPending(true);

    CHECK(presenter.getStatusHeadlineForTesting() ==
          "Loading the selected range. The previous results are still shown.");
    CHECK_FALSE(presenter.areScopeInputsEnabledForTesting());

    presenter.updateViewModel(makeViewModel());
    CHECK(presenter.getStatusHeadlineForTesting() == "2026-04-10 00:00 to 2026-04-12 09:30 UTC");
    CHECK(presenter.areScopeInputsEnabledForTesting());

    presenter.shutdown();
}

TEST_CASE("AnalysisPresenter distinguishes missing activity buckets from recorded counts",
          "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);
    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    auto model = makeViewModel();
    model.trendPoints.push_back(
        {"2026-04-12", 0, makeUtcTime(2026, 4, 12), makeUtcTime(2026, 4, 12, 9, 30), "Unknown"});
    presenter.updateViewModel(model);
    CHECK(presenter.getTrendSummaryForTesting() == "3 records in the selected range");
    CHECK(presenter.getTrendDetailTextForTesting().find("No recorded count for this interval.") != std::string::npos);
    REQUIRE(presenter.selectTrendBucketForTesting(0));
    CHECK(presenter.getTrendDetailTextForTesting().find("2 records. Source: OpenAI Codex-local.") != std::string::npos);

    model.trendPoints.push_back(
        {"2026-04-13", 0, makeUtcTime(2026, 4, 13), makeUtcTime(2026, 4, 13, 9, 30), "Unknown"});
    presenter.updateViewModel(model);
    CHECK(presenter.getTrendDetailTextForTesting().find("2026-04-10 00:00") != std::string::npos);
}

TEST_CASE("AnalysisPresenter separates recent Codex rates from selected activity counts",
          "[analysis][presenter][analysis_presenter][codex]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);
    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    auto model = makeViewModel();
    model.codexThroughput.hasData = true;
    model.codexThroughput.tokensPerMinute5m = 60.0;
    model.codexThroughput.promptsPerMinute5m = 0.4;
    model.codexThroughput.tokensPerMinute30m = 9.8;
    model.codexThroughput.promptsPerMinute30m = 0.7;
    model.codexThroughput.confidenceLabel = "LocalExact";
    presenter.updateViewModel(model);

    CHECK(presenter.getTrendSummaryForTesting() == "3 records in the selected range");
    selectPage(parent.get(), 2);
    const auto recent = presenter.getCodexTextForTesting();
    CHECK(recent.find("Fixed windows ending at the last refresh") != std::string::npos);
    CHECK(recent.find("5 min: 60 tokens/min, 0.4 prompts/min.") != std::string::npos);
    CHECK(recent.find("30 min: 9.8 tokens/min, 0.7 prompts/min.") != std::string::npos);
}

TEST_CASE("AnalysisPresenter reports the quota low without inventing its timestamp",
          "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);
    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    auto model = makeQuotaTrendViewModel();
    model.quotaTrendSeries.front().lowWaterRemainingPercentage = 28.0;
    presenter.updateViewModel(model);
    selectPage(parent.get(), 1);
    CHECK(presenter.getTrendSummaryForTesting() ==
          "Latest in range: 95% (950 of 1,000 requests) remaining. Lowest recorded: 28%. 1 recorded reset.");
    CHECK(presenter.getTrendDetailTextForTesting().find("Recorded reset.") != std::string::npos);
}

TEST_CASE("AnalysisPresenter keeps quota series units distinct", "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);
    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    presenter.updateViewModel(makeMultiQuotaTrendViewModel());
    selectPage(parent.get(), 1);
    CHECK(presenter.getQuotaSeriesOptionsForTesting() == std::vector<std::string>{"OpenAI 5h", "MiniMax 1d"});
    CHECK(presenter.getTrendSummaryForTesting().find("950 of 1,000 requests") != std::string::npos);
    REQUIRE(presenter.selectQuotaSeriesForTesting("minimax-plan|tokens|1d"));
    CHECK(presenter.getTrendSummaryForTesting().find("5,200 of 10,000 tokens") != std::string::npos);
    CHECK(presenter.getTrendSummaryForTesting().find("Lowest recorded: 52%.") != std::string::npos);
}

TEST_CASE("AnalysisPresenter lays out the quota selector when a selected account gains history",
          "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);
    ShowWindow(parent.get(), SW_SHOWNA);
    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));
    presenter.updateViewModel(makeViewModel());
    presenter.layout(RECT{0, 0, 760, 520});
    selectPage(parent.get(), 1);

    const HWND selector = findDescendant(parent.get(), 2107);
    REQUIRE(selector != nullptr);
    CHECK_FALSE(IsWindowVisible(selector));
    CHECK(presenter.getTrendSummaryForTesting() ==
          "No quota samples were recorded for this account in the selected range.");

    presenter.updateViewModel(makeMultiQuotaTrendViewModel());
    REQUIRE(IsWindowVisible(selector));
    REQUIRE(IsWindowEnabled(selector));
    const RECT trend = presenter.getSectionBoundsForTesting("trend");
    RECT bounds{};
    REQUIRE(GetWindowRect(selector, &bounds));
    MapWindowPoints(HWND_DESKTOP, parent.get(), reinterpret_cast<POINT*>(&bounds), 2);
    CHECK(bounds.left > trend.left);
    CHECK(bounds.right <= trend.right);
    CHECK(bounds.top > trend.top);
    CHECK(bounds.bottom < trend.bottom);

    presenter.updateViewModel(makeQuotaTrendViewModel());
    CHECK(IsWindowVisible(selector));
    CHECK(presenter.getTrendSummaryForTesting().find("95%") != std::string::npos);
}

TEST_CASE("AnalysisPresenter exposes per-sample quota details and scope-aware empty states",
          "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);
    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    presenter.updateViewModel(makeMultiQuotaTrendViewModel());
    selectPage(parent.get(), 1);
    const auto comparison = presenter.getTrendComparisonTextForTesting();
    CHECK(comparison.find("OpenAI 5h: 95% (950 of 1,000 requests) remaining on 2026-04-10 19:00 UTC") !=
          std::string::npos);
    CHECK(comparison.find("MiniMax 1d: 52% (5,200 of 10,000 tokens) remaining on 2026-04-11 00:00 UTC") !=
          std::string::npos);
    REQUIRE(presenter.selectQuotaTrendPointForTesting(1));
    CHECK(presenter.getTrendDetailTextForTesting().find("2026-04-10 14:00 UTC: 41%") != std::string::npos);

    auto model = makeViewModel();
    model.scope = {};
    presenter.updateViewModel(model);
    CHECK(presenter.getTrendSummaryForTesting() == "Choose a provider and an account to see recorded quota history.");
}

TEST_CASE("AnalysisPresenter stacks activity detail tables in a narrow window",
          "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);
    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    presenter.updateViewModel(makeViewModel());
    presenter.layout(RECT{0, 0, 760, 980});
    const RECT trend = presenter.getSectionBoundsForTesting("trend");
    const RECT daily = presenter.getSectionBoundsForTesting("daily");
    const RECT provider = presenter.getSectionBoundsForTesting("provider");
    const RECT account = presenter.getSectionBoundsForTesting("account");
    CHECK(daily.left == trend.left);
    CHECK(provider.left == trend.left);
    CHECK(account.left == trend.left);
    CHECK(daily.top >= trend.bottom);
    CHECK(provider.top >= daily.bottom);
    CHECK(account.top >= provider.bottom);
}

TEST_CASE("AnalysisPresenter scrolls activity details with scope controls fixed",
          "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);
    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    presenter.updateViewModel(makeViewModel());
    presenter.layout(RECT{0, 0, 760, 520});
    REQUIRE(presenter.getContentHeightForTesting() > presenter.getViewportHeightForTesting());
    const RECT scopeBefore = presenter.getSectionBoundsForTesting("scope");
    const RECT dailyBefore = presenter.getSectionBoundsForTesting("daily");
    REQUIRE(presenter.handleVScroll(SB_PAGEDOWN));
    const RECT dailyAfter = presenter.getSectionBoundsForTesting("daily");
    const RECT scopeAfter = presenter.getSectionBoundsForTesting("scope");
    CHECK(EqualRect(&scopeBefore, &scopeAfter));
    CHECK(dailyBefore.top - dailyAfter.top == presenter.getScrollOffsetForTesting());

    REQUIRE(presenter.handleVScroll(SB_BOTTOM));
    CHECK(presenter.getScrollOffsetForTesting() ==
          presenter.getContentHeightForTesting() - presenter.getViewportHeightForTesting());
    REQUIRE(presenter.handleVScroll(SB_TOP));
    const RECT dailyRestored = presenter.getSectionBoundsForTesting("daily");
    CHECK(EqualRect(&dailyBefore, &dailyRestored));
}

TEST_CASE("AnalysisPresenter changes pages without hiding the selected filters",
          "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);
    ShowWindow(parent.get(), SW_SHOWNA);
    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));
    presenter.updateViewModel(makeMultiQuotaTrendViewModel());
    presenter.layout(RECT{0, 0, 760, 520});

    REQUIRE(presenter.handleVScroll(SB_PAGEDOWN));
    selectPage(parent.get(), 1);
    CHECK(presenter.getScrollOffsetForTesting() == 0);
    CHECK(presenter.getTrendSummaryForTesting().find("remaining") != std::string::npos);
    CHECK(IsWindowVisible(GetDlgItem(parent.get(), 2101)));
    selectPage(parent.get(), 2);
    CHECK_FALSE(IsWindowVisible(findDescendant(parent.get(), 2107)));
    CHECK(IsWindowVisible(GetDlgItem(parent.get(), 2101)));
}

TEST_CASE("AnalysisPresenter debounces custom date edits until commit notifications",
          "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);

    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    AnalysisViewModel model = makeViewModel();
    presenter.updateViewModel(model);

    std::vector<AnalysisScopeState> scopes;
    presenter.setScopeChangedCallback([&](const AnalysisScopeState& state) { scopes.push_back(state); });

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

TEST_CASE("AnalysisPresenter keeps custom date input through a refresh and its debounce",
          "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);

    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));
    AnalysisViewModel model = makeViewModel();
    model.scope.dateRangePreset = AnalysisDateRangePreset::Custom;
    presenter.updateViewModel(model);

    std::vector<AnalysisScopeState> scopes;
    presenter.setScopeChangedCallback([&](const AnalysisScopeState& state) {
        scopes.push_back(state);
        presenter.setInteractionPending(true);
    });

    presenter.setCustomRangeTextForTesting("2026-04-01", "2026-04-05");
    REQUIRE(presenter.handleCommand(2104, EN_CHANGE));
    presenter.updateViewModel(model);
    CHECK(scopes.empty());

    MSG timer{};
    const ULONGLONG deadline = GetTickCount64() + 2000;
    while (!PeekMessageW(&timer, parent.get(), WM_TIMER, WM_TIMER, PM_REMOVE) && GetTickCount64() < deadline) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_TIMER);
    }
    REQUIRE(timer.message == WM_TIMER);
    REQUIRE(presenter.handleTimer(timer.wParam));
    REQUIRE(scopes.size() == 1);
    CHECK(scopes.front().dateRangePreset == AnalysisDateRangePreset::Custom);
    CHECK(scopes.front().customRangeStartUtc == makeUtcTime(2026, 4, 1));
    CHECK(scopes.front().customRangeEndUtc == makeUtcTime(2026, 4, 5));

    presenter.updateViewModel(model);
    CHECK_FALSE(presenter.areScopeInputsEnabledForTesting());
    model.scope = scopes.front();
    presenter.updateViewModel(model);
    CHECK(presenter.areScopeInputsEnabledForTesting());
}

TEST_CASE("AnalysisPresenter waits for the selected scope when an older refresh arrives",
          "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);
    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));

    auto model = makeViewModel();
    model.requestedScope = model.scope;
    presenter.updateViewModel(model);
    std::optional<AnalysisScopeState> requested;
    presenter.setScopeChangedCallback([&](const AnalysisScopeState& state) {
        requested = state;
        presenter.setInteractionPending(true);
    });
    REQUIRE(presenter.selectProviderForTesting("minimax"));
    REQUIRE(requested.has_value());

    presenter.updateViewModel(model);
    CHECK_FALSE(presenter.areScopeInputsEnabledForTesting());

    model.scope = *requested;
    model.requestedScope = *requested;
    model.scopeLabel = "MiniMax / All Accounts / Last 7 Days";
    presenter.updateViewModel(model);
    CHECK(presenter.areScopeInputsEnabledForTesting());
    CHECK(presenter.getScopeLabelForTesting() == "Active filters: MiniMax / All Accounts / Last 7 Days");
}

TEST_CASE("AnalysisPresenter keeps invalid custom dates out of the saved scope",
          "[analysis][presenter][analysis_presenter]") {
    ScopedParentWindow parent;
    REQUIRE(parent.get() != nullptr);
    AnalysisPresenter presenter;
    REQUIRE(presenter.init(parent.get()));
    presenter.updateViewModel(makeViewModel());
    int updates = 0;
    presenter.setScopeChangedCallback([&](const AnalysisScopeState&) { ++updates; });

    presenter.setCustomRangeTextForTesting("2026-04-31", "2026-05-02");
    REQUIRE(presenter.handleCommand(2104, EN_CHANGE));
    REQUIRE(presenter.handleCommand(2104, EN_KILLFOCUS));
    CHECK(updates == 0);
    CHECK(presenter.getCustomDateHintForTesting().find("Enter valid dates") != std::string::npos);

    presenter.setCustomRangeTextForTesting("2026-05-01", "2026-05-02");
    REQUIRE(presenter.handleCommand(2104, EN_CHANGE));
    REQUIRE(presenter.handleCommand(2104, EN_KILLFOCUS));
    CHECK(updates == 1);
}
