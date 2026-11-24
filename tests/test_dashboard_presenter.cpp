#define JISTATS_ENABLE_TEST_HOOKS 1

#include <catch_amalgamated.hpp>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

#include <windows.h>
#include <commctrl.h>

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
#include <sqlite3.h>
#endif

#include "db/SettingsService.hpp"
#include "db/StorageService.hpp"
#include "gui/AnalysisPresenter.hpp"
#include "gui/DashboardPresenter.hpp"
#include "temp_test_directory.hpp"

using namespace JiStats;
using JiStats::TestSupport::TempTestDirectory;

namespace {

const std::filesystem::path kOpenAIExportFixturePath =
    std::filesystem::path("docs/fixtures/openai/chatgpt-export-redacted/conversations.json");

bool waitForCondition(const std::function<bool()>& predicate,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(1500)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
struct PlacementReadGate {
    std::promise<void> entered;
    std::shared_future<void> release;
};

std::atomic<PlacementReadGate*> analysisPlacementRead = nullptr;

int installAnalysisPlacementReadGate(sqlite3* db, char**, const sqlite3_api_routines*) {
    return sqlite3_trace_v2(
        db, SQLITE_TRACE_STMT,
        [](unsigned, void*, void* stmt, void*) {
            if (analysisPlacementRead.load() != nullptr) {
                char* sql = sqlite3_expanded_sql(static_cast<sqlite3_stmt*>(stmt));
                const bool isPlacementRead =
                    sql != nullptr &&
                    std::string_view(sql).find("WHERE key = 'ui.analysis_window_x'") != std::string_view::npos;
                sqlite3_free(sql);
                if (isPlacementRead) {
                    if (auto* gate = analysisPlacementRead.exchange(nullptr)) {
                        gate->entered.set_value();
                        gate->release.wait_for(std::chrono::seconds(10));
                    }
                }
            }
            return 0;
        },
        nullptr);
}
#endif

std::optional<DashboardSummaryItem> findSummary(
    const std::vector<DashboardSummaryItem>& items,
    const std::string& label) {
    const auto it = std::find_if(items.begin(), items.end(), [&label](const DashboardSummaryItem& item) {
        return item.label == label;
    });
    if (it == items.end()) {
        return std::nullopt;
    }
    return *it;
}

int rectCenterX(const std::array<int, 4>& rect) {
    return rect[0] + ((rect[2] - rect[0]) / 2);
}

int rectCenterY(const std::array<int, 4>& rect) {
    return rect[1] + ((rect[3] - rect[1]) / 2);
}

DashboardViewModel makeQuotaViewModel() {
    DashboardViewModel model;
    model.appVersion = "1.2.3";
    model.lastUpdateTime = "2026-04-18 18:00:00 UTC";
    model.runtimeState = RuntimeState::Stale;
    model.preferences.startupEnabled = true;
    model.preferences.scheduledExportEnabled = true;
    model.preferences.scheduledExportIntervalMinutes = 60;
    model.preferences.scheduledExportPath = "C:\\Reports\\JiStats";
    model.recoveryStatusSummary = "Backfilled windows in 1 provider";
    model.recoveryStatusDetail =
        "Current shared sync state: Stale. Backfilled windows are currently visible for OpenAI. "
        "Last recovery outcome: imported for OpenAI at 2026-04-18 18:00:00 UTC.";
    model.scheduledExportStatusSummary = "Every 60 mins";
    model.scheduledExportStatusDetail =
        "Scheduled HTML export is enabled every 60 minutes. Output folder: C:\\Reports\\JiStats. "
        "Last successful run: 2026-04-18 17:45:00 UTC.";
    model.scheduledExportLastRunTime = "2026-04-18 17:45:00 UTC";

    ProviderStatus openai;
    openai.providerKey = "openai";
    openai.displayName = "OpenAI";
    openai.accountId = "openai.primary";
    openai.isEnabled = true;
    openai.state = ProviderState::Live;
    openai.lastSyncTime = "2026-04-18T18:00:00Z";
    QuotaInfo openaiQuota;
    openaiQuota.planName = "Codex Local Pressure";
    openaiQuota.windowType = "interval";
    openaiQuota.used = 25;
    openaiQuota.limit = 100;
    openaiQuota.remaining = 75;
    openaiQuota.remainingPercentage = 75.0;
    openaiQuota.windowResetAt = 1776592800; // 2026-04-19 10:00 UTC
    openaiQuota.secondaryWindowType = "weekly";
    openaiQuota.secondaryRemainingPercentage = 62.0;
    openaiQuota.secondaryWindowResetAt = 1776816000; // 2026-04-22 00:00 UTC
    openaiQuota.providerSurface = "openai_subscription_codex_local";
    openai.quotas.push_back(openaiQuota);

    ProviderStatus minimax;
    minimax.providerKey = "minimax";
    minimax.displayName = "MiniMax";
    minimax.accountId = "minimax.subscription";
    minimax.isEnabled = true;
    minimax.state = ProviderState::Live;
    minimax.lastSyncTime = "2026-04-18T18:00:00Z";
    QuotaInfo minimaxQuota;
    minimaxQuota.planName = "Coding Plan Remaining";
    minimaxQuota.windowType = "interval";
    minimaxQuota.used = 800;
    minimaxQuota.limit = 1000;
    minimaxQuota.remaining = 200;
    minimaxQuota.remainingPercentage = 20.0;
    minimaxQuota.windowResetAt = 1776592800;
    minimaxQuota.providerSurface = "minimax_subscription_coding_plan";
    minimax.quotas.push_back(minimaxQuota);

    model.providers = {openai, minimax};
    return model;
}

DashboardViewModel makeMultiAccountQuotaViewModel() {
    DashboardViewModel model;
    model.appVersion = "1.2.3";
    model.lastUpdateTime = "2026-04-18 18:00:00 UTC";
    model.runtimeState = RuntimeState::Current;

    ProviderStatus openaiPrimary;
    openaiPrimary.providerKey = "openai";
    openaiPrimary.displayName = "OpenAI";
    openaiPrimary.accountId = "acct-openai-primary";
    openaiPrimary.isEnabled = true;
    openaiPrimary.state = ProviderState::Live;
    openaiPrimary.lastSyncTime = "2026-04-18T18:00:00Z";
    QuotaInfo primaryQuota;
    primaryQuota.planName = "Codex Local Pressure";
    primaryQuota.windowType = "interval";
    primaryQuota.used = 25;
    primaryQuota.limit = 100;
    primaryQuota.remaining = 75;
    primaryQuota.remainingPercentage = 75.0;
    primaryQuota.providerSurface = "openai_subscription_codex_local";
    openaiPrimary.quotas.push_back(primaryQuota);

    ProviderStatus openaiSecondary = openaiPrimary;
    openaiSecondary.accountId = "acct-openai-secondary";
    openaiSecondary.lastSyncTime = "2026-04-18T18:10:00Z";
    openaiSecondary.quotas.front().used = 80;
    openaiSecondary.quotas.front().remaining = 20;
    openaiSecondary.quotas.front().remainingPercentage = 20.0;

    ProviderStatus minimax;
    minimax.providerKey = "minimax";
    minimax.displayName = "MiniMax";
    minimax.accountId = "acct-minimax";
    minimax.isEnabled = true;
    minimax.state = ProviderState::Live;
    minimax.lastSyncTime = "2026-04-18T18:00:00Z";
    QuotaInfo minimaxQuota;
    minimaxQuota.planName = "Coding Plan Remaining";
    minimaxQuota.windowType = "interval";
    minimaxQuota.used = 300;
    minimaxQuota.limit = 1000;
    minimaxQuota.remaining = 700;
    minimaxQuota.remainingPercentage = 70.0;
    minimaxQuota.providerSurface = "minimax_subscription_coding_plan";
    minimax.quotas.push_back(minimaxQuota);

    model.providers = {openaiPrimary, openaiSecondary, minimax};
    return model;
}

DashboardViewModel makeKimiLocalRuntimeViewModel() {
    DashboardViewModel model;
    model.appVersion = "1.2.3";
    model.lastUpdateTime = "2026-04-20 18:00:00 UTC";
    model.runtimeState = RuntimeState::Current;
    model.preferences.kimiCodeEnabled = true;

    ProviderStatus kimi;
    kimi.providerKey = "kimi";
    kimi.displayName = "Kimi Code";
    kimi.accountId = "kimi.runtime";
    kimi.isEnabled = true;
    kimi.state = ProviderState::Sample;
    kimi.lastSyncTime = "2026-04-20T18:00:00Z";
    model.providers = {kimi};

    DashboardSummaryItem localSummary;
    localSummary.label = "Last 30 Minutes";
    localSummary.detail = "7 sessions, 18m active";
    localSummary.percentage = 60;
    localSummary.badge = "Local";
    model.runtimeSummaries.push_back(localSummary);
    return model;
}

DashboardViewModel makeKimiSubscriptionViewModel() {
    DashboardViewModel model;
    model.appVersion = "1.2.3";
    model.lastUpdateTime = "2026-04-20 18:00:00 UTC";
    model.runtimeState = RuntimeState::Current;
    model.preferences.kimiCodeEnabled = true;

    ProviderStatus kimi;
    kimi.providerKey = "kimi";
    kimi.displayName = "Kimi Code";
    kimi.accountId = "kimi.subscription";
    kimi.isEnabled = true;
    kimi.state = ProviderState::Live;
    kimi.lastSyncTime = "2026-04-20T18:00:00Z";

    QuotaInfo fiveHourQuota;
    fiveHourQuota.planName = "Subscription";
    fiveHourQuota.windowType = "interval";
    fiveHourQuota.used = 64;
    fiveHourQuota.limit = 100;
    fiveHourQuota.remaining = 36;
    fiveHourQuota.usedPercentage = 64.0;
    fiveHourQuota.remainingPercentage = 36.0;
    fiveHourQuota.secondaryWindowType = "weekly";
    fiveHourQuota.secondaryRemainingPercentage = 72.0;
    fiveHourQuota.providerSurface = "kimi_subscription_api";
    kimi.quotas.push_back(fiveHourQuota);

    QuotaInfo weeklyQuota = fiveHourQuota;
    weeklyQuota.windowType = "weekly";
    weeklyQuota.used = 28;
    weeklyQuota.remaining = 72;
    weeklyQuota.usedPercentage = 28.0;
    weeklyQuota.remainingPercentage = 72.0;
    weeklyQuota.secondaryWindowType.clear();
    weeklyQuota.secondaryRemainingPercentage = -1.0;
    kimi.quotas.push_back(weeklyQuota);

    model.providers = {kimi};
    return model;
}

} // namespace

TEST_CASE("DashboardPresenter normalizes dashboard shell labels and quota headers", "[dashboard_presenter]") {
    DashboardPresenter presenter;
    presenter.updateViewModel(makeQuotaViewModel());

    const auto summaries = presenter.getUsageProgressSummariesForTesting();
    const auto openaiSummary = findSummary(summaries, "OpenAI Subscription");
    REQUIRE(openaiSummary.has_value());
    CHECK(openaiSummary->percentage == 75);
    CHECK(openaiSummary->secondaryPercentage == 62);
    CHECK(openaiSummary->detail.find("75% remaining") != std::string::npos);
    CHECK(openaiSummary->detail.find("75 of 100 left") == std::string::npos);
    CHECK(openaiSummary->detail.find("25 used") == std::string::npos);
    CHECK(openaiSummary->detail.find("Weekly 62% remaining") != std::string::npos);
    CHECK(openaiSummary->detail.find("resets ") != std::string::npos);
    CHECK(openaiSummary->detail.find("2026-04-19 10:00 UTC") == std::string::npos);

    const auto minimaxSummary = findSummary(summaries, "MiniMax - Coding Plan");
    REQUIRE(minimaxSummary.has_value());
    CHECK(minimaxSummary->detail.find("20% remaining") != std::string::npos);
    CHECK(minimaxSummary->detail.find("200 of 1,000 prompts left") != std::string::npos);
    CHECK(minimaxSummary->detail.find("800 used") == std::string::npos);
    CHECK(minimaxSummary->detail.find("resets ") != std::string::npos);
    CHECK(minimaxSummary->detail.find("2026-04-19 10:00 UTC") == std::string::npos);

    const auto fieldLabels = presenter.getApplicationFieldLabelsForTesting();
    CHECK(std::find(fieldLabels.begin(), fieldLabels.end(), "Version") == fieldLabels.end());
    CHECK(std::find(fieldLabels.begin(), fieldLabels.end(), "Connection") != fieldLabels.end());
    CHECK(std::find(fieldLabels.begin(), fieldLabels.end(), "Recovery") != fieldLabels.end());
    CHECK(std::find(fieldLabels.begin(), fieldLabels.end(), "Scheduled Export") != fieldLabels.end());
    CHECK(std::find(fieldLabels.begin(), fieldLabels.end(), "Start With Windows") == fieldLabels.end());
    CHECK(std::find(fieldLabels.begin(), fieldLabels.end(), "Taskbar Overlay") == fieldLabels.end());

    const auto fieldValues = presenter.getApplicationFieldValuesForTesting();
    REQUIRE(fieldValues.size() == 4);
    CHECK(fieldValues[1] == "Backfilled windows in 1 provider");
    CHECK(fieldValues[2] == "Every 60 mins");

    const std::vector<std::string> expectedUsedValues = {
        "25%",
        "80%"
    };
    CHECK(presenter.getProviderQuotaUsedValuesForTesting() == expectedUsedValues);
    CHECK(presenter.getProviderQuotaLimitValuesForTesting() == std::vector<std::string>{"100%", "1,000"});
    CHECK(presenter.getProviderQuotaRemainingValuesForTesting() == std::vector<std::string>{"75%", "200"});
    const auto lastSyncValues = presenter.getProviderQuotaLastSyncValuesForTesting();
    REQUIRE(lastSyncValues.size() == 2);
    CHECK(lastSyncValues[0].find("(Local)") != std::string::npos);
    CHECK(lastSyncValues[1].find("(API)") != std::string::npos);

    const std::vector<std::string> expectedHeaders = {
        "Provider",
        "Status",
        "Quota Surface",
        "Reset Window",
        "Used",
        "Quota Limit",
        "Remaining",
        "Confidence",
        "Last Sync"
    };
    CHECK(presenter.getProviderQuotaColumnHeadersForTesting() == expectedHeaders);

    const std::string helpText = presenter.getConnectionHelpTextForTesting();
    CHECK(helpText.find("Current: provider data is fresh") != std::string::npos);
    CHECK(helpText.find("Catching Up: JiStats is refreshing") != std::string::npos);
    CHECK(helpText.find("Stale: the last successful provider snapshot") != std::string::npos);
    CHECK(helpText.find("Current state: Stale") != std::string::npos);
}

TEST_CASE("DashboardPresenter syncs scheduled export controls from the view model", "[dashboard_presenter][settings]") {
    DashboardPresenter presenter;
    DashboardViewModel model = makeQuotaViewModel();

    REQUIRE(presenter.init());
    presenter.updateViewModel(model);

    REQUIRE(presenter.dispatchShellCommandForTesting(DashboardShellCommand::FileSettings));
    REQUIRE(waitForCondition([&presenter]() {
        return presenter.isShellDialogOpenForTesting(DashboardShellDialog::Preferences);
    }));

    CHECK(presenter.isScheduledExportEnabledForTesting());
    CHECK(presenter.getScheduledExportIntervalMinutesForTesting() == 60);
    CHECK(presenter.getScheduledExportPathForTesting() == "C:\\Reports\\JiStats");
    CHECK_FALSE(presenter.getScheduledExportLastRunForTesting().empty());

    presenter.shutdown();
}

TEST_CASE("DashboardPresenter tabs through visible Settings controls", "[dashboard_presenter][settings]") {
    DashboardPresenter presenter;
    auto model = makeQuotaViewModel();
    model.preferences.startupEnabled = false;
    model.preferences.scheduledExportEnabled = false;
    presenter.updateViewModel(model);
    REQUIRE(presenter.init());
    REQUIRE(presenter.dispatchShellCommandForTesting(DashboardShellCommand::FileSettings));

    REQUIRE(
        waitForCondition([&]() { return presenter.isShellDialogOpenForTesting(DashboardShellDialog::Preferences); }));
    const HWND settings = FindWindowW(L"JiStatsPreferencesWindow", nullptr);
    REQUIRE(settings != nullptr);
    const DWORD settingsThread = GetWindowThreadProcessId(settings, nullptr);
    const auto focusedControl = [settingsThread]() -> HWND {
        GUITHREADINFO info{};
        info.cbSize = sizeof(info);
        return GetGUIThreadInfo(settingsThread, &info) ? info.hwndFocus : nullptr;
    };

    const HWND tab = FindWindowExW(settings, nullptr, WC_TABCONTROLW, nullptr);
    const HWND overlay = FindWindowExW(settings, nullptr, L"Button", L"Enable taskbar overlay");
    const HWND startup = FindWindowExW(settings, nullptr, L"Button", L"Start with Windows");
    const HWND scheduled = FindWindowExW(settings, nullptr, L"Button", L"Enable scheduled HTML export");
    const HWND close = FindWindowExW(settings, nullptr, L"Button", L"Close");
    REQUIRE(close != nullptr);
    REQUIRE(tab != nullptr);
    REQUIRE(overlay != nullptr);
    REQUIRE(startup != nullptr);
    REQUIRE(scheduled != nullptr);
    REQUIRE(waitForCondition([&]() { return focusedControl() == tab; }));

    const auto tabTo = [&](HWND current, HWND next) {
        return PostMessageW(current, WM_KEYDOWN, VK_TAB, 0) &&
               waitForCondition([&]() { return focusedControl() == next; });
    };
    const auto shiftTabTo = [&](HWND current, HWND next) {
        const DWORD testThread = GetCurrentThreadId();
        if (!AttachThreadInput(testThread, settingsThread, TRUE)) {
            return false;
        }
        BYTE keys[256]{};
        keys[VK_SHIFT] = 0x80;
        const bool moved = SetKeyboardState(keys) && tabTo(current, next);
        keys[VK_SHIFT] = 0;
        SetKeyboardState(keys);
        AttachThreadInput(testThread, settingsThread, FALSE);
        return moved;
    };

    SECTION("General skips disabled controls and wraps backwards") {
        REQUIRE(tabTo(tab, overlay));
        REQUIRE(tabTo(overlay, startup));
        REQUIRE(tabTo(startup, scheduled));
        REQUIRE(tabTo(scheduled, close));
        REQUIRE(tabTo(close, tab));
        REQUIRE(shiftTabTo(tab, close));
        CHECK(shiftTabTo(close, scheduled));
    }

    SECTION("MiniMax keeps focus when pending validation disables its action buttons") {
        REQUIRE(presenter.selectPreferencesTabForTesting(2));
        const HWND validate = FindWindowExW(settings, nullptr, L"Button", L"Validate");
        REQUIRE(validate != nullptr);
        REQUIRE(waitForCondition([&]() { return IsWindowVisible(validate) != FALSE; }));
        REQUIRE(shiftTabTo(tab, close));
        REQUIRE(shiftTabTo(close, validate));

        MiniMaxCredentialProfileSummary pending;
        pending.planKey = "coding_plan";
        pending.validationState = "pending";
        model.minimaxCredentialProfiles = {pending};
        presenter.updateViewModel(model);

        REQUIRE(waitForCondition([&]() { return !IsWindowEnabled(validate); }));
        CHECK(focusedControl() == close);
        CHECK(tabTo(close, tab));
    }
}

TEST_CASE("DashboardPresenter routes the separate Codex attribution checkbox",
          "[dashboard_presenter][settings][codex]") {
    std::atomic<int> requested{-1};
    DashboardPresenter presenter;
    presenter.setCodexModelAttributionChangedCallback(
        [&requested](bool enabled, std::uint64_t) { requested.store(enabled ? 1 : 0); });
    REQUIRE(presenter.init());
    presenter.updateViewModel(makeQuotaViewModel());

    REQUIRE(presenter.dispatchShellCommandForTesting(DashboardShellCommand::FileSettings));
    REQUIRE(waitForCondition(
        [&presenter]() { return presenter.isShellDialogOpenForTesting(DashboardShellDialog::Preferences); }));
    REQUIRE(presenter.selectPreferencesTabForTesting(1));

    const HWND settings = FindWindowW(L"JiStatsPreferencesWindow", nullptr);
    REQUIRE(settings != nullptr);
    const HWND checkbox = FindWindowExW(settings, nullptr, L"Button", L"Enable Codex model attribution");
    REQUIRE(checkbox != nullptr);
    REQUIRE(waitForCondition([checkbox]() { return IsWindowVisible(checkbox) != FALSE; }));
    CHECK(SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_UNCHECKED);
    SendMessageW(checkbox, BM_CLICK, 0, 0);
    CHECK(requested.load() == 1);
    SendMessageW(checkbox, BM_CLICK, 0, 0);
    CHECK(requested.load() == 0);
}

TEST_CASE("DashboardPresenter runs MiniMax credential actions asynchronously", "[dashboard_presenter][settings][minimax]") {
    DashboardPresenter presenter;
    DashboardViewModel model = makeQuotaViewModel();
    std::atomic<bool> callbackInvoked{false};

    presenter.setMiniMaxCredentialActionCallback([&](const DashboardMiniMaxCredentialRequest& request) {
        callbackInvoked.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        return DashboardMiniMaxCredentialResult{
            true,
            request.planKey,
            "Stored MiniMax credential in the background."
        };
    });

    REQUIRE(presenter.init());
    presenter.updateViewModel(model);

    REQUIRE(presenter.dispatchShellCommandForTesting(DashboardShellCommand::FileSettings));
    REQUIRE(waitForCondition([&presenter]() {
        return presenter.isShellDialogOpenForTesting(DashboardShellDialog::Preferences);
    }));

    DashboardMiniMaxCredentialRequest request;
    request.action = DashboardMiniMaxCredentialAction::Save;
    request.planKey = "coding_plan";
    request.accountDisplayName = "Async Coding Plan";
    request.secret = "sk-cp-async";

    const auto startedAt = std::chrono::steady_clock::now();
    REQUIRE(presenter.submitMiniMaxCredentialActionForTesting(request));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt);

    CHECK(elapsed < std::chrono::milliseconds(150));
    CHECK(presenter.getMiniMaxSecretFieldTextForTesting() == "sk-cp-async");
    REQUIRE(waitForCondition([&]() {
        return callbackInvoked.load();
    }));
    REQUIRE(waitForCondition([&presenter]() {
        return presenter.getStatusTextForTesting().find("background") != std::string::npos;
    }));

    presenter.shutdown();
}

TEST_CASE("Dashboard, Settings and Analysis share panel and label backgrounds", "[dashboard_presenter][theme]") {
    AnalysisPresenter analysisPresenter;
    DashboardPresenter presenter;
    REQUIRE(presenter.init());
    presenter.setAnalysisPresenter(&analysisPresenter);
    REQUIRE(presenter.dispatchShellCommandForTesting(DashboardShellCommand::FileSettings));
    REQUIRE(
        waitForCondition([&]() { return presenter.isShellDialogOpenForTesting(DashboardShellDialog::Preferences); }));

    const HWND dashboard = FindWindowW(L"JiStatsDashboardWindow", nullptr);
    const HWND settings = FindWindowW(L"JiStatsPreferencesWindow", nullptr);
    REQUIRE(dashboard != nullptr);
    REQUIRE(settings != nullptr);
    const HWND checkbox = FindWindowExW(settings, nullptr, L"BUTTON", L"Enable taskbar overlay");
    const HWND dashboardTab = FindWindowExW(dashboard, nullptr, WC_TABCONTROLW, nullptr);
    const HWND settingsTab = FindWindowExW(settings, nullptr, WC_TABCONTROLW, nullptr);
    const HWND intervalField = FindWindowExW(settings, nullptr, L"EDIT", nullptr);
    const HWND history = FindWindowExW(dashboard, nullptr, L"BUTTON", L"&History...");
    REQUIRE(checkbox != nullptr);
    REQUIRE(dashboardTab != nullptr);
    REQUIRE(settingsTab != nullptr);
    REQUIRE(intervalField != nullptr);
    REQUIRE_FALSE(IsWindowEnabled(intervalField));
    REQUIRE(history != nullptr);

    SendMessageW(history, BM_CLICK, 0, 0);
    HWND analysis = nullptr;
    REQUIRE(waitForCondition(
        [&]() {
            analysis = FindWindowW(L"JiStatsAnalysisWindow", nullptr);
            return analysis != nullptr && IsWindowVisible(analysis);
        },
        std::chrono::milliseconds(4000)));
    const HWND label = FindWindowExW(analysis, nullptr, L"STATIC", L"Provider");
    const HWND analysisTab = FindWindowExW(analysis, nullptr, WC_TABCONTROLW, nullptr);
    REQUIRE(label != nullptr);
    REQUIRE(analysisTab != nullptr);

    RECT pane{};
    GetClientRect(settingsTab, &pane);
    TabCtrl_AdjustRect(settingsTab, FALSE, &pane);
    const HDC screen = GetDC(nullptr);
    const HDC dc = CreateCompatibleDC(screen);
    const HBITMAP bitmap = CreateCompatibleBitmap(screen, 1, 1);
    ReleaseDC(nullptr, screen);
    REQUIRE(dc != nullptr);
    REQUIRE(bitmap != nullptr);
    const HGDIOBJ previous = SelectObject(dc, bitmap);

    auto paintPixel = [&](HWND window, UINT message, LPARAM argument, POINT position, bool native = false) {
        SetWindowOrgEx(dc, position.x, position.y, nullptr);
        SetPixel(dc, position.x, position.y, RGB(255, 0, 255));
        const auto wParam = reinterpret_cast<WPARAM>(dc);
        const auto result = native ? DefWindowProcW(window, message, wParam, argument)
                                   : SendMessageW(window, message, wParam, argument);
        if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN) {
            const RECT pixel{position.x, position.y, position.x + 1, position.y + 1};
            FillRect(dc, &pixel, reinterpret_cast<HBRUSH>(result));
        }
        return GetPixel(dc, position.x, position.y);
    };

    const COLORREF dashboardPanel = paintPixel(dashboard, WM_ERASEBKGND, 0, {0, 100});
    const COLORREF settingsPanel = paintPixel(settings, WM_ERASEBKGND, 0, {0, 0});
    const COLORREF tabPane = paintPixel(settingsTab, WM_PRINTCLIENT, PRF_CLIENT, {pane.left + 12, pane.bottom - 12});
    const COLORREF checkboxPanel = paintPixel(settings, WM_CTLCOLORBTN, reinterpret_cast<LPARAM>(checkbox), {0, 0});
    const COLORREF analysisPanel = paintPixel(analysis, WM_ERASEBKGND, 0, {0, 0});
    const COLORREF labelPanel = paintPixel(analysis, WM_CTLCOLORSTATIC, reinterpret_cast<LPARAM>(label), {0, 0});
    const auto field = reinterpret_cast<LPARAM>(intervalField);
    const COLORREF fieldPanel = paintPixel(settings, WM_CTLCOLORSTATIC, field, {0, 0});
    const COLORREF nativeFieldPanel = paintPixel(settings, WM_CTLCOLORSTATIC, field, {0, 0}, true);
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                              (contrast.dwFlags & HCF_HIGHCONTRASTON);
    if (!highContrast) {
        constexpr COLORREF amber = RGB(232, 157, 43);
        for (HWND tab : {dashboardTab, settingsTab, analysisTab}) {
            RECT item{};
            bool visibleAccent = false;
            if (TabCtrl_GetItemRect(tab, TabCtrl_GetCurSel(tab), &item)) {
                const LONG x = item.left + (item.right - item.left) / 8;
                for (LONG y = item.top; y < item.top + (item.bottom - item.top) / 2 && !visibleAccent; ++y) {
                    visibleAccent = paintPixel(tab, WM_PRINTCLIENT, PRF_CLIENT, {x, y}) == amber;
                }
            }
            CHECK(visibleAccent);
        }

        const int dpi = GetDpiForWindow(settingsTab);
        const LONG protectedRows = MulDiv(2, dpi, 96);
        DRAWITEMSTRUCT draw{};
        draw.CtlType = ODT_TAB;
        draw.CtlID = GetDlgCtrlID(settingsTab);
        draw.itemID = TabCtrl_GetCurSel(settingsTab);
        draw.itemAction = ODA_DRAWENTIRE;
        draw.itemState = ODS_SELECTED;
        draw.hwndItem = settingsTab;
        draw.hDC = dc;
        draw.rcItem = {0, 0, MulDiv(80, dpi, 96), MulDiv(24, dpi, 96)};
        const POINT face{draw.rcItem.right / 8, protectedRows + 1};
        const int saved = SaveDC(dc);
        SetWindowOrgEx(dc, face.x, face.y, nullptr);
        // Some native tab controls pass the full item rectangle with its edge clipped from the DC.
        IntersectClipRect(dc, draw.rcItem.left, protectedRows, draw.rcItem.right, draw.rcItem.bottom);
        SetPixel(dc, face.x, face.y, RGB(255, 0, 255));
        SendMessageW(settings, WM_DRAWITEM, draw.CtlID, reinterpret_cast<LPARAM>(&draw));
        const auto clippedAccent = GetPixel(dc, face.x, face.y);
        RestoreDC(dc, saved);
        CHECK(clippedAccent == amber);
    }
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);

    CHECK(dashboardPanel == (highContrast ? GetSysColor(COLOR_BTNFACE) : RGB(246, 248, 251)));
    CHECK(settingsPanel == dashboardPanel);
    CHECK(tabPane == settingsPanel);
    CHECK(checkboxPanel == settingsPanel);
    CHECK(fieldPanel == nativeFieldPanel);
    CHECK(analysisPanel == dashboardPanel);
    CHECK(labelPanel == analysisPanel);
}

TEST_CASE("DashboardPresenter handles native close safely", "[dashboard_presenter]") {
    DashboardPresenter presenter;
    REQUIRE(presenter.init());
    presenter.updateViewModel(makeQuotaViewModel());

    REQUIRE(waitForCondition([&presenter]() { return presenter.getUsageProgressBarWidthForTesting() > 0; }));

    const HWND dashboard = FindWindowW(L"JiStatsDashboardWindow", nullptr);
    REQUIRE(dashboard != nullptr);
    const HWND settings = FindWindowExW(dashboard, nullptr, L"BUTTON", L"&Settings...");
    REQUIRE(settings != nullptr);
    REQUIRE(IsWindowVisible(settings));
    SendMessageW(settings, BM_CLICK, 0, 0);
    REQUIRE(waitForCondition([&presenter]() {
        return presenter.isShellDialogOpenForTesting(DashboardShellDialog::Preferences);
    }));
    const std::vector<std::string> expectedPreferenceTabs = {"General",   "OpenAI",    "MiniMax",      "Claude Code",
                                                             "Kilo Code", "Kimi Code", "Grok / Cursor"};
    CHECK(presenter.getPreferencesTabLabelsForTesting() == expectedPreferenceTabs);
    REQUIRE(presenter.dispatchShellCommandForTesting(DashboardShellCommand::HelpAbout));
    REQUIRE(waitForCondition([&presenter]() {
        return presenter.isShellDialogOpenForTesting(DashboardShellDialog::About);
    }));

    REQUIRE(presenter.postNativeCloseForTesting());
    REQUIRE(waitForCondition([&presenter]() {
        return !presenter.isVisible() &&
            !presenter.isShellDialogOpenForTesting(DashboardShellDialog::Preferences) &&
            !presenter.isShellDialogOpenForTesting(DashboardShellDialog::About);
    }));

    presenter.shutdown();
}

TEST_CASE("DashboardPresenter centers Help and About dialogs on the main window", "[dashboard_presenter][shell]") {
    DashboardPresenter presenter;
    REQUIRE(presenter.init());
    presenter.updateViewModel(makeQuotaViewModel());

    REQUIRE(waitForCondition([&presenter]() {
        const auto rect = presenter.getMainWindowRectForTesting();
        return rect[2] > rect[0] && rect[3] > rect[1];
    }));

    const auto mainRect = presenter.getMainWindowRectForTesting();

    REQUIRE(presenter.dispatchShellCommandForTesting(DashboardShellCommand::HelpViewHelp));
    REQUIRE(waitForCondition([&presenter]() {
        return presenter.isShellDialogOpenForTesting(DashboardShellDialog::Help);
    }));
    REQUIRE(waitForCondition([&presenter]() {
        const auto rect = presenter.getShellDialogRectForTesting(DashboardShellDialog::Help);
        return rect[2] > rect[0] && rect[3] > rect[1];
    }));
    const auto helpRect = presenter.getShellDialogRectForTesting(DashboardShellDialog::Help);
    CHECK(std::abs(rectCenterX(mainRect) - rectCenterX(helpRect)) <= 24);
    CHECK(std::abs(rectCenterY(mainRect) - rectCenterY(helpRect)) <= 24);

    REQUIRE(presenter.dispatchShellCommandForTesting(DashboardShellCommand::HelpAbout));
    REQUIRE(waitForCondition([&presenter]() {
        return presenter.isShellDialogOpenForTesting(DashboardShellDialog::About);
    }));
    REQUIRE(waitForCondition([&presenter]() {
        const auto rect = presenter.getShellDialogRectForTesting(DashboardShellDialog::About);
        return rect[2] > rect[0] && rect[3] > rect[1];
    }));
    const auto aboutRect = presenter.getShellDialogRectForTesting(DashboardShellDialog::About);
    CHECK(std::abs(rectCenterX(mainRect) - rectCenterX(aboutRect)) <= 24);
    CHECK(std::abs(rectCenterY(mainRect) - rectCenterY(aboutRect)) <= 24);

    presenter.shutdown();
}

TEST_CASE("DashboardPresenter routes File Import OpenAI shell commands through the configured callback", "[dashboard_presenter][shell]") {
    DashboardPresenter presenter;
    std::atomic<int> callbackCount{0};
    std::string importedPath;

    presenter.setOpenAIImportActionCallback([&](const std::string& inputPath) {
        importedPath = inputPath;
        callbackCount.fetch_add(1);

        DashboardOpenAIImportResult result;
        result.success = true;
        result.insertedCount = 3;
        result.inputPath = inputPath;
        result.message = "Imported 3 historical messages";
        return result;
    });
    presenter.setOpenAIImportPathForTesting(kOpenAIExportFixturePath.string());

    REQUIRE(presenter.init());
    REQUIRE(presenter.dispatchShellCommandForTesting(DashboardShellCommand::FileImportOpenAI));
    REQUIRE(waitForCondition([&]() {
        return callbackCount.load() == 1;
    }));
    CHECK(importedPath == kOpenAIExportFixturePath.string());

    presenter.shutdown();
}

TEST_CASE("DashboardPresenter runs OpenAI import asynchronously", "[dashboard_presenter][shell][async]") {
    DashboardPresenter presenter;
    std::atomic<bool> callbackInvoked{false};

    presenter.setOpenAIImportActionCallback([&](const std::string& inputPath) {
        callbackInvoked.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        DashboardOpenAIImportResult result;
        result.success = true;
        result.insertedCount = 4;
        result.inputPath = inputPath;
        result.message = "Imported 4 historical messages in the background.";
        return result;
    });
    presenter.setOpenAIImportPathForTesting(kOpenAIExportFixturePath.string());

    REQUIRE(presenter.init());

    const auto startedAt = std::chrono::steady_clock::now();
    REQUIRE(presenter.dispatchShellCommandForTesting(DashboardShellCommand::FileImportOpenAI));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt);

    CHECK(elapsed < std::chrono::milliseconds(150));
    REQUIRE(waitForCondition([&]() {
        return callbackInvoked.load();
    }));
    REQUIRE(waitForCondition([&presenter]() {
        return presenter.getStatusTextForTesting().find("background") != std::string::npos;
    }));

    presenter.shutdown();
}

TEST_CASE("DashboardPresenter persists provider snapshot selection through the preferences callback", "[dashboard_presenter][selection]") {
    DashboardPresenter presenter;
    DashboardPreferences lastPreferences;
    std::mutex callbackMutex;
    std::atomic<int> callbackCount{0};

    presenter.setPreferencesChangedCallback([&](const DashboardPreferences& preferences) {
        std::lock_guard<std::mutex> lock(callbackMutex);
        lastPreferences = preferences;
        callbackCount.fetch_add(1);
    });

    REQUIRE(presenter.init());
    presenter.updateViewModel(makeMultiAccountQuotaViewModel());

    REQUIRE(waitForCondition([&]() {
        if (callbackCount.load() == 0) {
            presenter.selectProviderQuotaRowForTesting(1);
        }
        return callbackCount.load() == 1;
    }));

    {
        std::lock_guard<std::mutex> lock(callbackMutex);
        CHECK(lastPreferences.lastSelectedProviderId == "openai");
        CHECK(lastPreferences.lastSelectedAccountId == "acct-openai-secondary");
    }
    CHECK(presenter.getSelectedProviderIdForTesting() == "openai");
    CHECK(presenter.getSelectedAccountIdForTesting() == "acct-openai-secondary");

    presenter.shutdown();
}

TEST_CASE("DashboardPresenter keeps all enabled quota accounts in Overview after restoring a detail selection",
          "[dashboard_presenter][selection]") {
    DashboardPresenter presenter;
    DashboardViewModel model = makeMultiAccountQuotaViewModel();
    model.preferences.lastSelectedProviderId = "openai";
    model.preferences.lastSelectedAccountId = "acct-openai-secondary";

    for (int index = 0; index < 3; ++index) {
        auto account = model.providers.front();
        account.accountId = "openai.extra-" + std::to_string(index);
        model.providers.push_back(std::move(account));
    }
    auto& lastQuota = model.providers.back().quotas.front();
    lastQuota.remainingPercentage = 39.0;
    lastQuota.windowResetAt = 1776592800;
    lastQuota.secondaryWindowType = "weekly";
    lastQuota.secondaryRemainingPercentage = 65.0;
    lastQuota.secondaryWindowResetAt = 1776816000;
    model.providers.back().state = ProviderState::Stale;

    auto disabled = model.providers.front();
    disabled.accountId = "openai.disabled";
    disabled.isEnabled = false;
    model.providers.push_back(std::move(disabled));

    auto apiQuota = model.providers.front().quotas.front();
    apiQuota.providerSurface = "openai_api_import";
    model.providers.front().quotas.push_back(std::move(apiQuota));

    auto kimi = makeKimiLocalRuntimeViewModel();
    model.providers.push_back(kimi.providers.front());
    model.runtimeSummaries = std::move(kimi.runtimeSummaries);
    REQUIRE(presenter.init());
    presenter.updateViewModel(model);

    const HWND dashboard = FindWindowW(L"JiStatsDashboardWindow", nullptr);
    REQUIRE(dashboard != nullptr);
    const HWND viewport = FindWindowExW(dashboard, nullptr, L"JiStatsDashboardViewport", nullptr);
    REQUIRE(viewport != nullptr);
    const HWND quotaSection = FindWindowExW(viewport, nullptr, L"STATIC", L"Subscription quota");
    const HWND activitySection = FindWindowExW(viewport, nullptr, L"STATIC", L"Recorded activity");
    const HWND statusSection = FindWindowExW(viewport, nullptr, L"STATIC", L"System status");
    REQUIRE(quotaSection != nullptr);
    REQUIRE(activitySection != nullptr);
    REQUIRE(statusSection != nullptr);
    REQUIRE(waitForCondition([&]() {
        return IsWindowVisible(activitySection) &&
               FindWindowExW(viewport, nullptr, L"STATIC", L"OpenAI Subscription / openai.extra-2") != nullptr;
    }));

    RECT quotaBounds{};
    RECT activityBounds{};
    RECT statusBounds{};
    REQUIRE(GetWindowRect(quotaSection, &quotaBounds));
    REQUIRE(GetWindowRect(activitySection, &activityBounds));
    REQUIRE(GetWindowRect(statusSection, &statusBounds));
    CHECK(quotaBounds.top < activityBounds.top);
    CHECK(activityBounds.top < statusBounds.top);

    SendMessageW(viewport, WM_VSCROLL, SB_BOTTOM, 0);
    CHECK(FindWindowExW(viewport, nullptr, L"STATIC", L"Quota unavailable") != nullptr);
    CHECK(FindWindowExW(viewport, nullptr, L"STATIC", L"OpenAI Subscription / openai.disabled") == nullptr);

    const auto summaries = presenter.getUsageProgressSummariesForTesting();
    REQUIRE(summaries.size() == 7);
    const auto selected = findSummary(summaries, "OpenAI Subscription / acct-openai-secondary");
    REQUIRE(selected.has_value());
    CHECK(selected->percentage == 20);
    const auto last = findSummary(summaries, "OpenAI Subscription / openai.extra-2");
    REQUIRE(last.has_value());
    CHECK(last->badge == "Primary window: 39% remaining");
    CHECK(last->secondaryBadge == "Secondary window: 65% remaining");
    CHECK(last->detail.find("resets ") != std::string::npos);
    CHECK(findSummary(summaries, "MiniMax - Coding Plan").has_value());
    CHECK(findSummary(summaries, "Last 30 Minutes").has_value());
    CHECK(presenter.getSelectedAccountIdForTesting() == "acct-openai-secondary");

    presenter.shutdown();
}

TEST_CASE("DashboardPresenter keeps recorded activity separate when a provider has no quota",
          "[dashboard_presenter][local]") {
    DashboardPresenter presenter;
    presenter.updateViewModel(makeKimiLocalRuntimeViewModel());

    const auto summaries = presenter.getUsageProgressSummariesForTesting();
    REQUIRE(summaries.size() == 1);
    CHECK(summaries.front().label == "Last 30 Minutes");
    CHECK(summaries.front().detail.find("7 sessions") != std::string::npos);
    CHECK(summaries.front().percentage == 60);
    CHECK(presenter.getProviderQuotaUsedValuesForTesting() == std::vector<std::string>{""});
    CHECK(presenter.getProviderQuotaLimitValuesForTesting() == std::vector<std::string>{""});
    CHECK(presenter.getProviderQuotaRemainingValuesForTesting() == std::vector<std::string>{""});
    CHECK(presenter.getProviderQuotaLastSyncValuesForTesting().size() == 1);
}

TEST_CASE("DashboardPresenter keeps Kimi subscription summaries remaining-first while showing used percentages in the quota table", "[dashboard_presenter][kimi]") {
    DashboardPresenter presenter;
    presenter.updateViewModel(makeKimiSubscriptionViewModel());

    const auto summaries = presenter.getUsageProgressSummariesForTesting();
    REQUIRE(summaries.size() == 1);
    const auto kimiSummary = findSummary(summaries, "Kimi Subscription");
    REQUIRE(kimiSummary.has_value());
    CHECK(kimiSummary->percentage == 36);
    CHECK(kimiSummary->secondaryPercentage == 72);
    CHECK(kimiSummary->detail.find("36% remaining") != std::string::npos);
    CHECK(kimiSummary->detail.find("Weekly 72% remaining") != std::string::npos);
    CHECK(kimiSummary->detail.find("64% used") == std::string::npos);

    CHECK(presenter.getProviderQuotaUsedValuesForTesting() == std::vector<std::string>{"64%", "28%"});
    CHECK(presenter.getProviderQuotaLimitValuesForTesting() == std::vector<std::string>{"100%", "100%"});
    CHECK(presenter.getProviderQuotaRemainingValuesForTesting() == std::vector<std::string>{"36%", "72%"});
    const auto lastSyncValues = presenter.getProviderQuotaLastSyncValuesForTesting();
    REQUIRE(lastSyncValues.size() == 2);
    CHECK(lastSyncValues[0].find("(API)") != std::string::npos);
    CHECK(lastSyncValues[1].find("(API)") != std::string::npos);
}

TEST_CASE("DashboardPresenter surfaces recovery and scheduled export status in the main shell", "[dashboard_presenter][status]") {
    DashboardPresenter presenter;
    DashboardViewModel model = makeQuotaViewModel();
    model.recoveryStatusSummary = "Backfilled windows in 1 provider";
    model.recoveryStatusDetail =
        "Current shared sync state: Catching Up. Backfilled windows are currently visible for OpenAI.";
    model.preferences.scheduledExportEnabled = true;
    model.preferences.scheduledExportIntervalMinutes = 60;
    model.preferences.scheduledExportPath = "C:/Reports/JiStats";
    model.scheduledExportStatusSummary = "Every 60 mins";
    model.scheduledExportStatusDetail =
        "Scheduled HTML export is enabled every 60 minutes. Output folder: C:/Reports/JiStats.";
    model.scheduledExportLastRunTime = "2026-04-19 18:15:00 UTC";

    REQUIRE(presenter.init());
    presenter.updateViewModel(model);

    const auto fieldValues = presenter.getApplicationFieldValuesForTesting();
    REQUIRE(fieldValues.size() == 4);
    CHECK(fieldValues[1] == "Backfilled windows in 1 provider");
    CHECK(fieldValues[2] == "Every 60 mins");

    REQUIRE(presenter.dispatchShellCommandForTesting(DashboardShellCommand::FileSettings));
    REQUIRE(waitForCondition([&presenter]() {
        return presenter.isShellDialogOpenForTesting(DashboardShellDialog::Preferences);
    }));

    CHECK(presenter.isScheduledExportEnabledForTesting());
    CHECK(presenter.getScheduledExportIntervalMinutesForTesting() == 60);
    CHECK(presenter.getScheduledExportPathForTesting() == "C:/Reports/JiStats");
    CHECK(presenter.getScheduledExportLastRunForTesting().find("2026") != std::string::npos);
    CHECK(presenter.getScheduledExportLastRunForTesting().find("18:15:00 UTC") == std::string::npos);

    presenter.shutdown();
}

TEST_CASE("DashboardPresenter shows offline notice content and routes retry and dismiss actions", "[dashboard_presenter][offline]") {
    DashboardPresenter presenter;
    DashboardViewModel model = makeQuotaViewModel();
    model.runtimeState = RuntimeState::Offline;
    model.isOnline = false;
    model.offlineRetryAvailable = true;
    model.offlineNoticeVisible = true;
    model.offlineNoticeTitle = "Connection Lost";
    model.offlineNoticeMessage =
        "Provider connectivity is unavailable. JiStats will keep the last known state and retry when connectivity returns.";

    std::atomic<int> retryCount{0};
    std::atomic<int> dismissCount{0};

    presenter.setOfflineRetryActionCallback([&]() {
        retryCount.fetch_add(1);
        return DashboardNoticeActionResult{true, "Retry attempted."};
    });
    presenter.setOfflineDismissActionCallback([&]() {
        dismissCount.fetch_add(1);
        return DashboardNoticeActionResult{true, "Offline notice dismissed."};
    });

    REQUIRE(presenter.init());
    presenter.updateViewModel(model);

    REQUIRE(waitForCondition([&presenter]() {
        return presenter.isOfflineNoticeVisibleForTesting();
    }));

    const std::string noticeText = presenter.getOfflineNoticeTextForTesting();
    CHECK(noticeText.find("Connection Lost") != std::string::npos);
    CHECK(noticeText.find("Provider connectivity is unavailable") != std::string::npos);

    REQUIRE(presenter.clickOfflineRetryForTesting());
    REQUIRE(waitForCondition([&]() {
        return retryCount.load() == 1;
    }));
    REQUIRE(waitForCondition([&presenter]() {
        return presenter.getStatusTextForTesting().find("Retry attempted") != std::string::npos;
    }));

    REQUIRE(presenter.clickOfflineDismissForTesting());
    REQUIRE(waitForCondition([&]() {
        return dismissCount.load() == 1;
    }));
    REQUIRE(waitForCondition([&presenter]() {
        return presenter.getStatusTextForTesting().find("dismissed") != std::string::npos;
    }));

    presenter.shutdown();
}

TEST_CASE("DashboardPresenter runs offline notice actions asynchronously", "[dashboard_presenter][offline][async]") {
    DashboardPresenter presenter;
    DashboardViewModel model = makeQuotaViewModel();
    model.runtimeState = RuntimeState::Offline;
    model.isOnline = false;
    model.offlineRetryAvailable = true;
    model.offlineNoticeVisible = true;
    model.offlineNoticeTitle = "Connection Lost";
    model.offlineNoticeMessage = "Async offline notice actions should not block the dashboard thread.";

    std::atomic<int> retryCount{0};
    std::atomic<int> dismissCount{0};

    presenter.setOfflineRetryActionCallback([&]() {
        retryCount.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        return DashboardNoticeActionResult{true, "Retry attempted in the background."};
    });
    presenter.setOfflineDismissActionCallback([&]() {
        dismissCount.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        return DashboardNoticeActionResult{true, "Offline notice dismissed in the background."};
    });

    REQUIRE(presenter.init());
    presenter.updateViewModel(model);

    REQUIRE(waitForCondition([&presenter]() {
        return presenter.isOfflineNoticeVisibleForTesting();
    }));

    const auto retryStartedAt = std::chrono::steady_clock::now();
    REQUIRE(presenter.clickOfflineRetryForTesting());
    const auto retryElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - retryStartedAt);
    CHECK(retryElapsed < std::chrono::milliseconds(150));
    REQUIRE(waitForCondition([&]() {
        return retryCount.load() == 1;
    }));
    REQUIRE(waitForCondition([&presenter]() {
        return presenter.getStatusTextForTesting().find("background") != std::string::npos;
    }));

    const auto dismissStartedAt = std::chrono::steady_clock::now();
    REQUIRE(presenter.clickOfflineDismissForTesting());
    const auto dismissElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - dismissStartedAt);
    CHECK(dismissElapsed < std::chrono::milliseconds(150));
    REQUIRE(waitForCondition([&]() {
        return dismissCount.load() == 1;
    }));
    REQUIRE(waitForCondition([&presenter]() {
        return presenter.getStatusTextForTesting().find("dismissed in the background") != std::string::npos;
    }));

    presenter.shutdown();
}

TEST_CASE("DashboardPresenter releases completed retry workers while it stays open",
          "[dashboard_presenter][offline][async]") {
    std::atomic<int> retries{0};
    DashboardPresenter presenter;
    DashboardViewModel model = makeQuotaViewModel();
    model.runtimeState = RuntimeState::Offline;
    model.isOnline = false;
    model.offlineRetryAvailable = true;
    model.offlineNoticeVisible = true;
    model.offlineNoticeTitle = "Connection Lost";
    model.offlineNoticeMessage = "Retry can be requested again after the previous action completes.";
    presenter.setOfflineRetryActionCallback(
        [&]() { return DashboardNoticeActionResult{true, "Completed retry " + std::to_string(++retries)}; });

    REQUIRE(presenter.init());
    presenter.updateViewModel(model);
    REQUIRE(waitForCondition([&]() { return presenter.isOfflineNoticeVisibleForTesting(); }));

    const auto retry = [&](int expected) {
        REQUIRE(presenter.clickOfflineRetryForTesting());
        REQUIRE(waitForCondition(
            [&]() { return presenter.getStatusTextForTesting() == "Completed retry " + std::to_string(expected); }));
    };
    retry(1);

    DWORD handlesBefore = 0;
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &handlesBefore));
    for (int expected = 2; expected <= 25; ++expected) {
        retry(expected);
    }
    DWORD handlesAfter = 0;
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &handlesAfter));
    // Allow incidental process handles. Retaining one worker for every action adds at least 24.
    CHECK(handlesAfter <= handlesBefore + 8);
}

TEST_CASE("DashboardPresenter carries rebuilt analysis data through the shell view model", "[dashboard_presenter][analysis]") {
    DashboardPresenter presenter;
    DashboardViewModel model = makeQuotaViewModel();
    model.analysis.scope.selectedProviderId = "openai";
    model.analysis.scope.selectedAccountId = "openai.primary";
    model.analysis.scope.dateRangePreset = AnalysisDateRangePreset::Last7Days;
    model.analysis.scopeLabel = "OpenAI / openai.primary / Last 7 Days";
    model.analysis.dailySummaries = {
        {"2026-04-17", 2, "OpenAI Codex-local", {}},
        {"2026-04-18", 1, "OpenAI Local", {}}
    };
    model.analysis.providerBreakdown = {
        {"openai", "OpenAI", "", 3, "Mixed OpenAI provenance"}
    };
    model.analysis.accountBreakdown = {
        {"openai.primary|OpenAI", "OpenAI / openai.primary", "OpenAI", 3, "Mixed OpenAI provenance"}
    };
    model.analysis.trendPoints = {
        {"2026-04-17", 2},
        {"2026-04-18", 1}
    };
    model.analysis.gapSummaryCounts.unknownDays = 1;
    model.analysis.dataQualityNote =
        "Rollup-backed daily history across the current analysis scope with 1 unknown gap day.";

    presenter.updateViewModel(model);

    CHECK(presenter.getViewModel().analysis.scopeLabel == "OpenAI / openai.primary / Last 7 Days");
    REQUIRE(presenter.getViewModel().analysis.dailySummaries.size() == 2);
    CHECK(presenter.getViewModel().analysis.dailySummaries.front().dateLabel == "2026-04-17");
    CHECK(presenter.getViewModel().analysis.dailySummaries.front().sourceLabel == "OpenAI Codex-local");
    REQUIRE(presenter.getViewModel().analysis.providerBreakdown.size() == 1);
    CHECK(presenter.getViewModel().analysis.providerBreakdown.front().label == "OpenAI");
    REQUIRE(presenter.getViewModel().analysis.accountBreakdown.size() == 1);
    CHECK(presenter.getViewModel().analysis.accountBreakdown.front().secondaryLabel == "OpenAI");
    REQUIRE(presenter.getViewModel().analysis.trendPoints.size() == 2);
    CHECK(presenter.getViewModel().analysis.trendPoints.back().activityCount == 1);
    CHECK(presenter.getViewModel().analysis.gapSummaryCounts.unknownDays == 1);
    CHECK(presenter.getViewModel().analysis.dataQualityNote.find("unknown gap day") != std::string::npos);
}

TEST_CASE("DashboardPresenter shows Codex throughput on the main overview tab", "[dashboard_presenter][analysis][codex]") {
    DashboardPresenter presenter;
    REQUIRE(presenter.init());

    DashboardViewModel model = makeQuotaViewModel();
    model.analysis.codexThroughput.hasData = true;
    model.analysis.codexThroughput.tokensPerMinute5m = 60.0;
    model.analysis.codexThroughput.promptsPerMinute5m = 0.4;
    model.analysis.codexThroughput.tokensPerMinute30m = 9.8;
    model.analysis.codexThroughput.promptsPerMinute30m = 0.7;
    model.analysis.codexThroughput.confidenceLabel = "LocalExact";
    model.preferences.codexModelAttributionEnabled = true;
    model.analysis.openAiCodexModelUsage.title = "OpenAI Codex Usage by Model";
    model.analysis.openAiCodexModelUsage.subtitle = "Observed local Codex usage since JiStats baseline";
    model.analysis.openAiCodexModelUsage.hasData = true;
    AnalysisModelUsageSeries openAiSeries;
    openAiSeries.modelKey = "gpt-5.4";
    openAiSeries.totalTokens = 12345;
    openAiSeries.points.push_back({1700000000, 12345});
    model.analysis.openAiCodexModelUsage.series.push_back(openAiSeries);
    model.analysis.hicapCodexModelUsage.title = "HICAP Codex Usage by Model";
    model.analysis.hicapCodexModelUsage.subtitle = "Observed local Codex usage since JiStats baseline";

    presenter.updateViewModel(model);

    REQUIRE(waitForCondition([&presenter]() {
        return presenter.getCodexThroughputTextForTesting().find("Last 5m: 60 t/min, 0.4 p/min") != std::string::npos;
    }));
    const std::string text = presenter.getCodexThroughputTextForTesting();
    CHECK(text.find("Last 30m: 9.8 t/min, 0.7 p/min") != std::string::npos);
    CHECK(text.find("Averages") != std::string::npos);
    CHECK(text.find("Totals") != std::string::npos);
    CHECK(text.find("Source: LocalExact") != std::string::npos);
    CHECK(presenter.getOpenAiCodexModelUsageTextForTesting().find("gpt-5.4: 12,345 tokens") != std::string::npos);
    CHECK(presenter.getHicapCodexModelUsageTextForTesting().find("No new Codex SQLite usage observed yet") != std::string::npos);

    model.preferences.codexModelAttributionEnabled = false;
    presenter.updateViewModel(model);
    REQUIRE(waitForCondition([&presenter]() {
        return presenter.getHicapCodexModelUsageTextForTesting().find("Enable Codex model attribution") !=
               std::string::npos;
    }));
    CHECK(presenter.getOpenAiCodexModelUsageTextForTesting().find("gpt-5.4: 12,345 tokens") != std::string::npos);

    const auto chartRect = presenter.getCodexThroughputChartRectForTesting();
    CHECK(chartRect[0] == 0);
    CHECK(chartRect[1] > 100);
    CHECK((chartRect[2] - chartRect[0]) > 300);
    CHECK((chartRect[3] - chartRect[1]) >= 120);

    presenter.shutdown();
}

TEST_CASE("DashboardPresenter scrolls to the final chart after a native resize", "[dashboard_presenter][layout]") {
    DashboardPresenter presenter;
    REQUIRE(presenter.init());
    presenter.setVisible(true);

    const HWND dashboard = FindWindowW(L"JiStatsDashboardWindow", nullptr);
    REQUIRE(dashboard != nullptr);
    DWORD process = 0;
    GetWindowThreadProcessId(dashboard, &process);
    REQUIRE(process == GetCurrentProcessId());

    RECT original{};
    REQUIRE(GetWindowRect(dashboard, &original));
    const UINT dpi = GetDpiForWindow(dashboard);
    const auto resize = [dashboard, dpi](int width, int height) {
        return SetWindowPos(dashboard, nullptr, 0, 0, MulDiv(width, dpi, 96), MulDiv(height, dpi, 96),
                            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    };
    const HWND viewport = FindWindowExW(dashboard, nullptr, L"JiStatsDashboardViewport", nullptr);
    REQUIRE(viewport != nullptr);
    const HWND throughput = FindWindowExW(viewport, nullptr, L"JistatsCodexChart", nullptr);
    const HWND openAi = FindWindowExW(viewport, throughput, L"JistatsCodexChart", nullptr);
    const HWND hicap = FindWindowExW(viewport, openAi, L"JistatsCodexChart", nullptr);
    REQUIRE(throughput != nullptr);
    REQUIRE(openAi != nullptr);
    REQUIRE(hicap != nullptr);

    RECT viewportBounds{};
    RECT openAiBounds{};
    RECT hicapBounds{};
    REQUIRE(resize(1040, 500));
    REQUIRE(GetWindowRect(openAi, &openAiBounds));
    REQUIRE(GetWindowRect(hicap, &hicapBounds));
    CHECK(openAiBounds.top == hicapBounds.top);
    CHECK(openAiBounds.right < hicapBounds.left);

    REQUIRE(resize(680, 500));
    REQUIRE(GetWindowRect(viewport, &viewportBounds));
    REQUIRE(GetWindowRect(openAi, &openAiBounds));
    REQUIRE(GetWindowRect(hicap, &hicapBounds));
    CHECK(openAiBounds.bottom < hicapBounds.top);
    CHECK(hicapBounds.top > viewportBounds.bottom);

    SendMessageW(viewport, WM_VSCROLL, SB_BOTTOM, 0);
    REQUIRE(GetWindowRect(hicap, &hicapBounds));
    CHECK(hicapBounds.top >= viewportBounds.top);
    CHECK(hicapBounds.bottom < viewportBounds.bottom);

    REQUIRE(PostMessageW(dashboard, WM_KEYDOWN, VK_PRIOR, 0));
    REQUIRE(waitForCondition(
        [&]() { return GetWindowRect(hicap, &hicapBounds) && hicapBounds.bottom > viewportBounds.bottom; }));

    SendMessageW(viewport, WM_VSCROLL, SB_TOP, 0);
    UINT wheelLines = 0;
    if (SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &wheelLines, 0) && wheelLines != 0) {
        SendMessageW(throughput, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
        SCROLLINFO scroll{};
        scroll.cbSize = sizeof(scroll);
        scroll.fMask = SIF_POS;
        REQUIRE(GetScrollInfo(viewport, SB_VERT, &scroll));
        CHECK(scroll.nPos > 0);
    }
    REQUIRE(SetWindowPos(dashboard, nullptr, original.left, original.top, original.right - original.left,
                         original.bottom - original.top, SWP_NOZORDER | SWP_NOACTIVATE));
    presenter.shutdown();
}

TEST_CASE("DashboardPresenter restores startup placement after being minimized", "[dashboard_presenter][navigation]") {
    StorageService storage;
    REQUIRE(storage.init());
    SettingsService settings(storage);
    const std::array<std::string, 4> keys = {"ui.dashboard_window_x", "ui.dashboard_window_y",
                                             "ui.dashboard_window_width", "ui.dashboard_window_height"};
    const std::array<int, 4> expected = {20, 30, 720, 560};
    std::array<std::optional<std::string>, 4> original;
    for (size_t index = 0; index < keys.size(); ++index) {
        original[index] = settings.getString(keys[index]);
        CHECK(settings.setInt(keys[index], expected[index]));
    }

    DashboardPresenter presenter;
    presenter.setVisible(true);
    const bool initialized = presenter.init(true);
    const HWND dashboard = FindWindowW(L"JiStatsDashboardWindow", nullptr);
    bool minimized = false;
    bool restored = false;
    if (initialized && dashboard != nullptr) {
        ShowWindow(dashboard, SW_MINIMIZE);
        minimized = waitForCondition([&]() { return IsIconic(dashboard) != FALSE; });
        presenter.restoreStartupPlacement();
        presenter.setVisible(true);
        restored = waitForCondition([&]() {
            RECT rect{};
            return GetWindowRect(dashboard, &rect) && rect.left == expected[0] && rect.top == expected[1] &&
                   rect.right - rect.left == expected[2] && rect.bottom - rect.top == expected[3];
        });
    }
    presenter.shutdown();
    for (size_t index = 0; index < keys.size(); ++index) {
        if (original[index]) {
            CHECK(settings.setString(keys[index], *original[index]));
        } else {
            settings.remove(keys[index]);
        }
    }
    CHECK(initialized);
    CHECK(minimized);
    CHECK(restored);
}

TEST_CASE("DashboardPresenter shows the dashboard while saved Analysis placement is loading",
          "[dashboard_presenter][navigation]") {
#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    StorageService storage;
    REQUIRE(storage.init());
    SettingsService settings(storage);
    const std::array<std::string, 4> keys = {"ui.analysis_window_x", "ui.analysis_window_y", "ui.analysis_window_width",
                                             "ui.analysis_window_height"};
    const std::array<int, 4> saved = {20, 30, 1173, 839};
    std::array<std::optional<std::string>, 4> original;
    for (size_t index = 0; index < keys.size(); ++index) {
        original[index] = settings.getString(keys[index]);
    }

    bool moveBeforeClosing = false;
    SECTION("keep saved bounds when the fallback window is untouched") {}
    SECTION("persist a deliberate resize while the saved bounds load") {
        moveBeforeClosing = true;
    }

    DashboardPresenter presenter;
    std::promise<void> release;
    PlacementReadGate gate{std::promise<void>{}, release.get_future().share()};
    auto entered = gate.entered.get_future();
    const auto extension = reinterpret_cast<void (*)()>(installAnalysisPlacementReadGate);
    REQUIRE(sqlite3_auto_extension(extension) == SQLITE_OK);
    for (size_t index = 0; index < keys.size(); ++index) {
        CHECK(settings.setInt(keys[index], saved[index]));
    }
    analysisPlacementRead.store(&gate);

    auto startup = std::async(std::launch::async, [&]() { return presenter.init(); });
    const bool placementReadStarted = entered.wait_for(std::chrono::seconds(4)) == std::future_status::ready;
    const bool startedWhileBlocked = startup.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    const bool analysisOpenedWhileBlocked =
        startedWhileBlocked && presenter.clickOpenAnalysisWindowForTesting() &&
        waitForCondition([&]() { return presenter.isAnalysisWindowOpenForTesting(); });

    std::array<int, 4> expected = saved;
    bool moved = false;
    bool closedWhileBlocked = false;
    if (analysisOpenedWhileBlocked) {
        const HWND analysis = FindWindowW(L"JiStatsAnalysisWindow", nullptr);
        if (moveBeforeClosing && SetWindowPos(analysis, nullptr, 50, 70, 1205, 855, SWP_NOACTIVATE | SWP_NOZORDER)) {
            WINDOWPLACEMENT placement{};
            placement.length = sizeof(placement);
            moved = GetWindowPlacement(analysis, &placement) != FALSE;
            const RECT& rect = placement.rcNormalPosition;
            expected = {rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top};
        }
        closedWhileBlocked = PostMessageW(analysis, WM_CLOSE, 0, 0) &&
                             waitForCondition([&]() { return !presenter.isAnalysisWindowOpenForTesting(); });
    }

    release.set_value();
    const bool initialized = startup.get();
    analysisPlacementRead.store(nullptr);
    sqlite3_cancel_auto_extension(extension);

    CHECK(placementReadStarted);
    CHECK(initialized);
    CHECK(startedWhileBlocked);
    CHECK(analysisOpenedWhileBlocked);
    CHECK(closedWhileBlocked);
    if (moveBeforeClosing) {
        CHECK(moved);
    }
    presenter.shutdown();
    for (size_t index = 0; index < keys.size(); ++index) {
        CHECK(settings.getInt(keys[index]) == expected[index]);
        if (original[index]) {
            CHECK(settings.setString(keys[index], *original[index]));
        } else {
            settings.remove(keys[index]);
        }
    }
#else
    SKIP("SQLite is not compiled into this test build");
#endif
}

TEST_CASE("DashboardPresenter switches pages with arrow keys and populates the dedicated Analysis window",
          "[dashboard_presenter][analysis]") {
    DashboardPresenter presenter;
    AnalysisPresenter analysisPresenter;

    REQUIRE(presenter.init());
    presenter.setAnalysisPresenter(&analysisPresenter);

    DashboardViewModel model = makeQuotaViewModel();
    model.analysis.scope.selectedProviderId = "openai";
    model.analysis.scope.selectedAccountId = "openai.primary";
    model.analysis.scope.dateRangePreset = AnalysisDateRangePreset::Last7Days;
    model.analysis.scopeLabel = "OpenAI / openai.primary / Last 7 Days";
    model.analysis.dailySummaries = {
        {"2026-04-17", 2, "OpenAI Codex-local", {}},
        {"2026-04-18", 1, "OpenAI Local", {}}
    };
    model.analysis.providerBreakdown = {
        {"openai", "OpenAI", "", 3, "Mixed OpenAI provenance"}
    };
    model.analysis.accountBreakdown = {
        {"openai.primary|OpenAI", "OpenAI / openai.primary", "OpenAI", 3, "Mixed OpenAI provenance"}
    };
    model.analysis.trendPoints = {
        {"2026-04-17", 2},
        {"2026-04-18", 1}
    };
    model.analysis.gapSummaryCounts.unknownDays = 1;
    model.analysis.dataQualityNote =
        "Rollup-backed daily history across the current analysis scope with 1 unknown gap day. "
        "Recovery checkpoints: 1 imported, 0 mismatch, 0 failed. "
        "Latest manual export: 2026-04-18T18:00:00Z (success).";

    presenter.updateViewModel(model);

    const HWND dashboard = FindWindowW(L"JiStatsDashboardWindow", nullptr);
    REQUIRE(dashboard != nullptr);
    const HWND tabs = FindWindowExW(dashboard, nullptr, WC_TABCONTROLW, nullptr);
    const HWND viewport = FindWindowExW(dashboard, nullptr, L"JiStatsDashboardViewport", nullptr);
    REQUIRE(tabs != nullptr);
    CHECK_FALSE(GetWindowLongPtrW(tabs, GWL_STYLE) & TCS_BUTTONS);
    REQUIRE(viewport != nullptr);
    const HWND providers = FindWindowExW(viewport, nullptr, WC_LISTVIEWW, nullptr);
    REQUIRE(providers != nullptr);
    REQUIRE(waitForCondition([&]() { return IsWindowVisible(dashboard) && ListView_GetItemCount(providers) > 0; }));

    RECT overviewTab{};
    REQUIRE(TabCtrl_GetItemRect(tabs, 0, &overviewTab));
    const LPARAM center =
        MAKELPARAM((overviewTab.left + overviewTab.right) / 2, (overviewTab.top + overviewTab.bottom) / 2);
    SendMessageW(tabs, WM_LBUTTONDOWN, MK_LBUTTON, center);
    SendMessageW(tabs, WM_LBUTTONUP, 0, center);
    GUITHREADINFO input{};
    input.cbSize = sizeof(input);
    REQUIRE(GetGUIThreadInfo(GetWindowThreadProcessId(tabs, nullptr), &input));
    REQUIRE(input.hwndFocus == tabs);
    const std::uint64_t syncCount = presenter.getViewModelSyncCountForTesting();

    REQUIRE(PostMessageW(tabs, WM_KEYDOWN, VK_RIGHT, 0));
    REQUIRE(waitForCondition([&]() { return TabCtrl_GetCurSel(tabs) == 1 && IsWindowVisible(providers); }));
    REQUIRE(PostMessageW(tabs, WM_KEYDOWN, VK_LEFT, 0));
    REQUIRE(waitForCondition([&]() { return TabCtrl_GetCurSel(tabs) == 0 && !IsWindowVisible(providers); }));
    const HWND history = FindWindowExW(dashboard, nullptr, L"BUTTON", L"&History...");
    REQUIRE(history != nullptr);
    REQUIRE(IsWindowVisible(history));
    SendMessageW(history, BM_CLICK, 0, 0);
    REQUIRE(waitForCondition([&]() { return presenter.isAnalysisWindowOpenForTesting(); },
                             std::chrono::milliseconds(4000)));
    CHECK(TabCtrl_GetCurSel(tabs) == 0);
    REQUIRE(waitForCondition([&presenter]() {
        return presenter.getAnalysisScopeLabelForTesting() == "Active filters: OpenAI / openai.primary / Last 7 Days";
    }, std::chrono::milliseconds(4000)));

    const HWND analysis = FindWindowW(L"JiStatsAnalysisWindow", nullptr);
    REQUIRE(analysis != nullptr);
    CHECK(GetWindow(analysis, GW_OWNER) == dashboard);
    CHECK(presenter.getViewModelSyncCountForTesting() == syncCount);
    const auto analysisRect = presenter.getAnalysisWindowRectForTesting();
    CHECK(analysisRect[2] > analysisRect[0]);
    CHECK(analysisRect[3] > analysisRect[1]);

    presenter.shutdown();
}

TEST_CASE("DashboardPresenter skips hidden main-window sync until the UI is shown again", "[dashboard_presenter][background]") {
    DashboardPresenter presenter;
    REQUIRE(presenter.init());

    DashboardViewModel model = makeQuotaViewModel();
    presenter.updateViewModel(model);
    REQUIRE(waitForCondition([&presenter]() {
        return presenter.getViewModelSyncCountForTesting() > 0;
    }));

    const std::uint64_t syncCountBeforeHide = presenter.getViewModelSyncCountForTesting();
    presenter.setVisible(false);
    REQUIRE(waitForCondition([&presenter]() {
        return !presenter.isVisible();
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const std::uint64_t syncCountWhileHidden = presenter.getViewModelSyncCountForTesting();
    CHECK(syncCountWhileHidden >= syncCountBeforeHide);

    DashboardViewModel hiddenUpdate = model;
    hiddenUpdate.lastUpdateTime = "2026-04-19 03:15:00 UTC";
    hiddenUpdate.recoveryStatusSummary = "No recent recovery activity";
    presenter.updateViewModel(hiddenUpdate);

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(presenter.getViewModel().lastUpdateTime == hiddenUpdate.lastUpdateTime);
    CHECK(presenter.getViewModelSyncCountForTesting() == syncCountWhileHidden);

    presenter.setVisible(true);
    REQUIRE(waitForCondition([&presenter, syncCountWhileHidden]() {
        return presenter.getViewModelSyncCountForTesting() > syncCountWhileHidden;
    }));

    presenter.shutdown();
}

TEST_CASE("DashboardPresenter dispatches FileExportHtml shell command without callback", "[dashboard_presenter]") {
    TempTestDirectory tempDir("jistats_dispatch_test");
    DashboardPresenter presenter;
    const std::string outputPath = (tempDir.path() / "dispatch-test.html").string();

    REQUIRE(presenter.init());
    presenter.setHtmlExportPathForTesting(outputPath);

    // Without a callback, the command dispatches but reports unavailable via status text
    REQUIRE(presenter.dispatchShellCommandForTesting(DashboardShellCommand::FileExportHtml));

    REQUIRE(waitForCondition([&presenter]() {
        return presenter.getStatusTextForTesting().find("unavailable") != std::string::npos;
    }));

    CHECK(presenter.getStatusTextForTesting().find("HTML export") != std::string::npos);

    presenter.shutdown();
}

TEST_CASE("DashboardPresenter manual HTML export routes through callback and reports status", "[dashboard_presenter]") {
    TempTestDirectory tempDir("jistats_export_test");
    const std::string outputPath = (tempDir.path() / "test-export.html").string();

    std::atomic<bool> callbackInvoked{false};
    std::string receivedPath;

    DashboardPresenter presenter;
    presenter.setHtmlExportActionCallback([&](const std::string& path) {
        receivedPath = path;
        callbackInvoked.store(true);
        DashboardHtmlExportResult result;
        result.success = true;
        result.outputPath = path;
        result.message = "HTML report exported to " + path;
        return result;
    });

    REQUIRE(presenter.init());
    presenter.setHtmlExportPathForTesting(outputPath);
    REQUIRE(presenter.dispatchShellCommandForTesting(DashboardShellCommand::FileExportHtml));

    REQUIRE(waitForCondition([&]() {
        return callbackInvoked.load();
    }));

    CHECK(receivedPath == outputPath);

    // Wait for the completion message to be processed
    REQUIRE(waitForCondition([&presenter]() {
        return !presenter.getLastHtmlExportStatusForTesting().empty();
    }));

    const std::string status = presenter.getLastHtmlExportStatusForTesting();
    CHECK(status.find("exported") != std::string::npos);

    presenter.shutdown();
}

TEST_CASE("DashboardPresenter manual HTML export shows failure status on error", "[dashboard_presenter]") {
    TempTestDirectory tempDir("jistats_export_fail_test");
    const std::string outputPath = (tempDir.path() / "fail-export.html").string();

    std::atomic<bool> callbackInvoked{false};

    DashboardPresenter presenter;
    presenter.setHtmlExportActionCallback([&](const std::string&) {
        callbackInvoked.store(true);
        DashboardHtmlExportResult result;
        result.success = false;
        result.message = "Export failed: disk full";
        return result;
    });

    REQUIRE(presenter.init());
    presenter.setHtmlExportPathForTesting(outputPath);
    REQUIRE(presenter.dispatchShellCommandForTesting(DashboardShellCommand::FileExportHtml));

    REQUIRE(waitForCondition([&]() {
        return callbackInvoked.load();
    }));

    REQUIRE(waitForCondition([&presenter]() {
        return !presenter.getLastHtmlExportStatusForTesting().empty();
    }));

    const std::string status = presenter.getLastHtmlExportStatusForTesting();
    CHECK(status.find("disk full") != std::string::npos);

    presenter.shutdown();
}

TEST_CASE("DashboardPresenter rejects a second manual HTML export request without blocking the UI", "[dashboard_presenter]") {
    TempTestDirectory tempDir("jistats_export_busy_test");
    const std::string outputPath = (tempDir.path() / "busy-export.html").string();

    std::atomic<bool> callbackStarted{false};
    DashboardPresenter presenter;
    std::promise<void> allowCompletion;

    presenter.setHtmlExportActionCallback(
        [&callbackStarted, release = allowCompletion.get_future().share()](const std::string& path) {
            callbackStarted.store(true);
            if (release.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
                DashboardHtmlExportResult timedOut;
                timedOut.message = "Timed out waiting for the test to release export";
                return timedOut;
            }

            DashboardHtmlExportResult result;
            result.success = true;
            result.outputPath = path;
            result.message = "HTML report exported to " + path;
            return result;
        });

    REQUIRE(presenter.init());
    presenter.setHtmlExportPathForTesting(outputPath);
    REQUIRE(presenter.dispatchShellCommandForTesting(DashboardShellCommand::FileExportHtml));

    REQUIRE(waitForCondition([&]() {
        return callbackStarted.load();
    }));

    REQUIRE(presenter.dispatchShellCommandForTesting(DashboardShellCommand::FileExportHtml));
    REQUIRE(waitForCondition([&presenter]() {
        return presenter.getStatusTextForTesting().find("already running") != std::string::npos;
    }));

    allowCompletion.set_value();
    REQUIRE(waitForCondition([&presenter]() {
        return presenter.getLastHtmlExportStatusForTesting().find("exported") != std::string::npos;
    }));

    presenter.shutdown();
}
