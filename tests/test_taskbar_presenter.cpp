#define JISTATS_ENABLE_TEST_HOOKS 1

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <future>
#include <thread>
#include <utility>

#include <windows.h>

#include "runtime_test_support.hpp"
#include "taskbar/TaskbarPresenter.hpp"

using namespace JiStats;
using namespace JiStats::TestSupport;

namespace {

constexpr const char* kHealthyIcon = "\xE2\x97\x8F";  // ●
constexpr const char* kWatchIcon = "\xE2\x96\xB2";    // ▲
constexpr const char* kLowIcon = "\xE2\x97\x86";      // ◆
constexpr const char* kCriticalIcon = "\xE2\x96\xA0"; // ■
constexpr const char* kNeutralIcon = "\xE2\x97\x8B";  // ○

TaskbarMetrics makeMetrics(const std::string& providerKey, long long used, long long limit) {
    TaskbarMetrics metrics;
    metrics.providerKey = providerKey;
    metrics.providerDisplayName = providerKey;
    metrics.used = used;
    metrics.limit = limit;
    return metrics;
}

TaskbarMetrics makePersistedActivityMetrics(const std::string& providerKey) {
    TaskbarMetrics metrics;
    metrics.providerKey = providerKey;
    metrics.providerDisplayName = providerKey;
    metrics.kind = TaskbarMetricKind::PersistedActivity;
    metrics.summaryLabel = "OpenAI 30m";
    metrics.primaryMetric = "2x";
    metrics.secondaryMetric = "2 sessions, 12 mins active";
    metrics.statusIcon = "!";
    metrics.used = 2;
    return metrics;
}

TaskbarPresenter::GeometryRectForTesting makeRect(int left, int top, int right, int bottom) {
    return TaskbarPresenter::GeometryRectForTesting{left, top, right, bottom};
}

// Use a Win32 taskbar host we control so the test can move the tray anchor without touching Explorer.
class TaskbarWindowFixture {
  public:
    TaskbarWindowFixture() {
        std::promise<HWND> ready;
        auto window = ready.get_future();
        thread_ = std::thread([ready = std::move(ready)]() mutable {
            const HINSTANCE instance = GetModuleHandleW(nullptr);
            WNDCLASSW windowClass{};
            windowClass.hInstance = instance;
            windowClass.lpszClassName = L"Shell_TrayWnd";
            windowClass.lpfnWndProc = &windowProc;
            if (RegisterClassW(&windowClass) == 0) {
                ready.set_value(nullptr);
                return;
            }
            WNDCLASSW anchorClass{};
            anchorClass.hInstance = instance;
            anchorClass.lpszClassName = L"TrayNotifyWnd";
            anchorClass.lpfnWndProc = DefWindowProcW;
            if (RegisterClassW(&anchorClass) == 0) {
                UnregisterClassW(windowClass.lpszClassName, instance);
                ready.set_value(nullptr);
                return;
            }
            const HWND hwnd =
                CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOPMOST, windowClass.lpszClassName, L"", WS_POPUP | WS_VISIBLE,
                                20, 50, 640, 48, nullptr, nullptr, instance, nullptr);
            ready.set_value(hwnd);

            if (hwnd != nullptr) {
                MSG message{};
                while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                    DispatchMessageW(&message);
                }
            }
            UnregisterClassW(anchorClass.lpszClassName, instance);
            UnregisterClassW(windowClass.lpszClassName, instance);
        });
        window_ = window.get();
    }

    ~TaskbarWindowFixture() {
        if (thread_.joinable()) {
            if (window_ != nullptr) {
                PostMessageW(window_, WM_CLOSE, 0, 0);
            }
            thread_.join();
        }
    }

    HWND get() const {
        return window_;
    }

    bool addTrayAnchor() const {
        DWORD_PTR result = 0;
        return SendMessageTimeoutW(window_, kAddTrayAnchor, 0, 0, SMTO_ABORTIFHUNG, 2000, &result) != 0 && result != 0;
    }

  private:
    static constexpr UINT kAddTrayAnchor = WM_APP + 1;

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == kAddTrayAnchor) {
            return CreateWindowExW(0, L"TrayNotifyWnd", L"", WS_CHILD | WS_VISIBLE, 420, 0, 180, 48, hwnd, nullptr,
                                   GetModuleHandleW(nullptr), nullptr) != nullptr;
        }
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    HWND window_ = nullptr;
    std::thread thread_;
};

} // namespace

TEST_CASE("TaskbarPresenter derives percentages and formatted metrics", "[taskbar]") {
    TaskbarPresenter presenter;

    SECTION("Derives remaining-percentage fields and compact defaults") {
        const TaskbarMetrics derived = presenter.deriveMetricsForTesting(
            makeMetrics("openai", 45, 90));

        REQUIRE(derived.usagePercentage == Catch::Approx(50.0));
        REQUIRE(derived.remainingPercentage == Catch::Approx(50.0));
        REQUIRE(derived.primaryMetric == "50%");
        REQUIRE(derived.secondaryMetric == "45 left");
        REQUIRE(derived.pressureBand == TaskbarPressureBand::Green);
        REQUIRE(derived.statusIcon.empty());
        REQUIRE_FALSE(derived.emphasizePrimary);
    }

    SECTION("Formats K/M compact remaining-value secondary metrics") {
        const TaskbarMetrics thousands = presenter.deriveMetricsForTesting(
            makeMetrics("openai", 1305, 1500));
        const TaskbarMetrics millions = presenter.deriveMetricsForTesting(
            makeMetrics("openai", 2500000, 4000000));

        REQUIRE(thousands.primaryMetric == "13%");
        REQUIRE(thousands.secondaryMetric == "195 left");
        REQUIRE(thousands.pressureBand == TaskbarPressureBand::Orange);
        REQUIRE(thousands.statusIcon.empty());
        REQUIRE(millions.secondaryMetric == "1M left");
    }

    SECTION("Handles zero limit without inventing quota pressure") {
        const TaskbarMetrics derived = presenter.deriveMetricsForTesting(
            makeMetrics("openai", 1200, 0));

        REQUIRE(derived.usagePercentage == Catch::Approx(0.0));
        REQUIRE(derived.remainingPercentage == Catch::Approx(0.0));
        REQUIRE(derived.primaryMetric == "0%");
        REQUIRE(derived.secondaryMetric == "0 left");
        REQUIRE(derived.pressureBand == TaskbarPressureBand::Neutral);
        REQUIRE(derived.statusIcon.empty());
        REQUIRE_FALSE(derived.emphasizePrimary);
    }

    SECTION("Preserves caller-provided primary text while ignoring legacy quota icons") {
        TaskbarMetrics metrics = makeMetrics("openai", 45, 90);
        metrics.statusIcon = "~";
        metrics.primaryMetric = "45%";

        const TaskbarMetrics derived = presenter.deriveMetricsForTesting(metrics);

        REQUIRE(derived.primaryMetric == "45%");
        REQUIRE(derived.secondaryMetric == "45 left");
        REQUIRE(derived.statusIcon == kWatchIcon);
        REQUIRE(derived.pressureBand == TaskbarPressureBand::Yellow);
    }
}

TEST_CASE("TaskbarPresenter maps remaining pressure bands to explicit icons", "[taskbar]") {
    TaskbarPresenter presenter;

    const TaskbarMetrics blue = presenter.deriveMetricsForTesting(makeMetrics("openai", 5, 100));
    const TaskbarMetrics green = presenter.deriveMetricsForTesting(makeMetrics("openai", 35, 100));
    const TaskbarMetrics yellow = presenter.deriveMetricsForTesting(makeMetrics("openai", 60, 100));
    const TaskbarMetrics orange = presenter.deriveMetricsForTesting(makeMetrics("openai", 89, 100));
    const TaskbarMetrics red = presenter.deriveMetricsForTesting(makeMetrics("openai", 95, 100));

    REQUIRE(blue.pressureBand == TaskbarPressureBand::Blue);
    REQUIRE(blue.statusIcon.empty());

    REQUIRE(green.pressureBand == TaskbarPressureBand::Green);
    REQUIRE(green.statusIcon.empty());

    REQUIRE(yellow.pressureBand == TaskbarPressureBand::Yellow);
    REQUIRE(yellow.statusIcon.empty());

    REQUIRE(orange.pressureBand == TaskbarPressureBand::Orange);
    REQUIRE(orange.statusIcon.empty());

    REQUIRE(red.pressureBand == TaskbarPressureBand::Red);
    REQUIRE(red.statusIcon.empty());
}

TEST_CASE("TaskbarPresenter emphasizes low-quota percentages at ten percent or under", "[taskbar]") {
    TaskbarPresenter presenter;

    const TaskbarMetrics tenPercent = presenter.deriveMetricsForTesting(makeMetrics("openai", 90, 100));
    const TaskbarMetrics elevenPercent = presenter.deriveMetricsForTesting(makeMetrics("openai", 89, 100));

    REQUIRE(tenPercent.primaryMetric == "10%");
    REQUIRE(tenPercent.emphasizePrimary);
    REQUIRE(tenPercent.pressureBand == TaskbarPressureBand::Red);

    REQUIRE(elevenPercent.primaryMetric == "11%");
    REQUIRE_FALSE(elevenPercent.emphasizePrimary);
    REQUIRE(elevenPercent.pressureBand == TaskbarPressureBand::Orange);
}

TEST_CASE("TaskbarPresenter honors explicit remaining semantics from presenter inputs", "[taskbar]") {
    TaskbarPresenter presenter;

    TaskbarMetrics metrics = makeMetrics("minimax", 992, 1000);
    metrics.usagePercentage = 0.8;
    metrics.remainingPercentage = 99.2;

    const TaskbarMetrics derived = presenter.deriveMetricsForTesting(metrics);

    REQUIRE(derived.usagePercentage == Catch::Approx(0.8));
    REQUIRE(derived.remainingPercentage == Catch::Approx(99.2));
    REQUIRE(derived.primaryMetric == "99%");
    REQUIRE(derived.secondaryMetric == "992 left");
    REQUIRE(derived.statusIcon.empty());
    REQUIRE(derived.pressureBand == TaskbarPressureBand::Blue);
    REQUIRE_FALSE(derived.emphasizePrimary);
}

TEST_CASE("TaskbarPresenter preserves explicit MiniMax remaining semantics alongside OpenAI defaults", "[taskbar]") {
    TaskbarPresenter presenter;
    presenter.setRuntimeStateForTesting(true, true);

    TaskbarMetrics openai = makeMetrics("openai", 25, 100);
    TaskbarMetrics minimax = makeMetrics("minimax", 992, 1000);
    minimax.usagePercentage = 0.8;
    minimax.remainingPercentage = 99.2;

    presenter.updateMetrics({openai, minimax});

    const std::vector<TaskbarMetrics> metrics = presenter.getCurrentMetrics();
    REQUIRE(metrics.size() == 2);

    const auto openaiIt = std::find_if(metrics.begin(), metrics.end(), [](const TaskbarMetrics& metric) {
        return metric.providerKey == "openai";
    });
    REQUIRE(openaiIt != metrics.end());
    CHECK(openaiIt->usagePercentage == Catch::Approx(25.0));
    CHECK(openaiIt->remainingPercentage == Catch::Approx(75.0));
    CHECK(openaiIt->primaryMetric == "75%");
    CHECK(openaiIt->secondaryMetric == "75 left");
    CHECK(openaiIt->statusIcon.empty());
    CHECK(openaiIt->pressureBand == TaskbarPressureBand::Green);

    const auto minimaxIt = std::find_if(metrics.begin(), metrics.end(), [](const TaskbarMetrics& metric) {
        return metric.providerKey == "minimax";
    });
    REQUIRE(minimaxIt != metrics.end());
    CHECK(minimaxIt->usagePercentage == Catch::Approx(0.8));
    CHECK(minimaxIt->remainingPercentage == Catch::Approx(99.2));
    CHECK(minimaxIt->primaryMetric == "99%");
    CHECK(minimaxIt->secondaryMetric == "992 left");
    CHECK(minimaxIt->statusIcon.empty());
    CHECK(minimaxIt->pressureBand == TaskbarPressureBand::Blue);
}

TEST_CASE("TaskbarPresenter skips no-op refresh requests for unchanged metrics and overlay state", "[taskbar]") {
    TaskbarPresenter presenter;
    presenter.setRuntimeStateForTesting(true, true);

    const std::vector<TaskbarMetrics> metrics = {
        makeMetrics("openai", 25, 100),
        makeMetrics("minimax", 992, 1000)
    };

    presenter.replaceMetrics(metrics);
    const std::uint64_t refreshCountAfterFirstReplace = presenter.getRefreshRequestCountForTesting();
    REQUIRE(refreshCountAfterFirstReplace == 1);

    presenter.replaceMetrics(metrics);
    CHECK(presenter.getRefreshRequestCountForTesting() == refreshCountAfterFirstReplace);

    presenter.setOverlayEnabled(true);
    CHECK(presenter.getRefreshRequestCountForTesting() == refreshCountAfterFirstReplace);

    presenter.clearMetrics();
    const std::uint64_t refreshCountAfterClear = presenter.getRefreshRequestCountForTesting();
    CHECK(refreshCountAfterClear == refreshCountAfterFirstReplace + 1);

    presenter.clearMetrics();
    CHECK(presenter.getRefreshRequestCountForTesting() == refreshCountAfterClear);
}

TEST_CASE("TaskbarPresenter preserves caller-owned persisted activity summaries", "[taskbar]") {
    TaskbarPresenter presenter;

    const TaskbarMetrics derived = presenter.deriveMetricsForTesting(
        makePersistedActivityMetrics("openai"));

    REQUIRE(derived.kind == TaskbarMetricKind::PersistedActivity);
    REQUIRE(derived.summaryLabel == "OpenAI 30m");
    REQUIRE(derived.primaryMetric == "2x");
    REQUIRE(derived.secondaryMetric == "2 sessions, 12 mins active");
    REQUIRE(derived.statusIcon == kCriticalIcon);
    REQUIRE(derived.pressureBand == TaskbarPressureBand::Red);
    REQUIRE(derived.usagePercentage == Catch::Approx(0.0));
    REQUIRE(derived.remainingPercentage == Catch::Approx(0.0));
}

TEST_CASE("TaskbarPresenter preserves compact multi-provider helper ordering", "[taskbar]") {
    TaskbarPresenter presenter;
    presenter.setRuntimeStateForTesting(true, true);

    presenter.updateMetrics(makeCompactMultiProviderTaskbarMetrics(std::time(nullptr)));

    const std::vector<TaskbarMetrics> metrics = presenter.getCurrentMetrics();
    REQUIRE(metrics.size() == 2);
    CHECK(metrics[0].providerKey == "openai");
    CHECK(metrics[0].primaryMetric == "50%");
    CHECK(metrics[1].providerKey == "minimax");
    CHECK(metrics[1].primaryMetric == "58%");
}

TEST_CASE("TaskbarPresenter preserves runtime-only helper fixture labels", "[taskbar]") {
    TaskbarPresenter presenter;

    const TaskbarMetrics derived = presenter.deriveMetricsForTesting(
        makeRuntimeOnlyTaskbarMetric(RuntimeState::Offline, std::time(nullptr)));

    CHECK(derived.providerKey == "runtime");
    CHECK(derived.kind == TaskbarMetricKind::PersistedActivity);
    CHECK(derived.summaryLabel == "Runtime");
    CHECK(derived.primaryMetric == "Off");
    CHECK(derived.secondaryMetric == "Offline");
    CHECK(derived.statusIcon == kCriticalIcon);
}

TEST_CASE("TaskbarPresenter updates and replaces metrics by provider key", "[taskbar]") {
    TaskbarPresenter presenter;
    presenter.setRuntimeStateForTesting(true, true);

    presenter.updateMetrics(makeMetrics("openai", 10, 100));
    presenter.updateMetrics(makeMetrics("minimax", 20, 200));

    std::vector<TaskbarMetrics> metrics = presenter.getCurrentMetrics();
    REQUIRE(metrics.size() == 2);

    presenter.updateMetrics(makeMetrics("openai", 95, 100));
    metrics = presenter.getCurrentMetrics();

    REQUIRE(metrics.size() == 2);
    const auto openai = std::find_if(metrics.begin(), metrics.end(), [](const TaskbarMetrics& metric) {
        return metric.providerKey == "openai";
    });
    REQUIRE(openai != metrics.end());
    REQUIRE(openai->primaryMetric == "5%");
    REQUIRE(openai->statusIcon.empty());
    REQUIRE(openai->emphasizePrimary);
}

TEST_CASE("TaskbarPresenter clearMetrics removes all provider entries", "[taskbar]") {
    TaskbarPresenter presenter;
    presenter.setRuntimeStateForTesting(true, true);

    presenter.updateMetrics(makeMetrics("openai", 10, 100));
    presenter.updateMetrics(makeMetrics("minimax", 20, 200));
    REQUIRE(presenter.getCurrentMetrics().size() == 2);

    presenter.clearMetrics();

    REQUIRE(presenter.getCurrentMetrics().empty());
}

TEST_CASE("TaskbarPresenter setProgress updates first metric state under test seam", "[taskbar]") {
    TaskbarPresenter presenter;
    presenter.setRuntimeStateForTesting(true, true);
    presenter.updateMetrics(makeMetrics("openai", 10, 100));

    presenter.setProgress(66.0, TaskbarPresenter::ProgressState::Normal);
    auto metrics = presenter.getCurrentMetrics();
    REQUIRE(metrics.size() == 1);
    REQUIRE(metrics.front().primaryMetric == "66%");
    REQUIRE(metrics.front().statusIcon == kHealthyIcon);

    presenter.setProgress(77.0, TaskbarPresenter::ProgressState::Paused);
    metrics = presenter.getCurrentMetrics();
    REQUIRE(metrics.front().primaryMetric == "77%");
    REQUIRE(metrics.front().statusIcon == kWatchIcon);

    presenter.setProgress(95.0, TaskbarPresenter::ProgressState::Error);
    metrics = presenter.getCurrentMetrics();
    REQUIRE(metrics.front().primaryMetric == "95%");
    REQUIRE(metrics.front().statusIcon == kCriticalIcon);

    presenter.setRuntimeStateForTesting(true, false);
    presenter.setProgress(10.0, TaskbarPresenter::ProgressState::Normal);
    metrics = presenter.getCurrentMetrics();
    REQUIRE(metrics.front().primaryMetric == "95%");
    REQUIRE(metrics.front().statusIcon == kCriticalIcon);
}

TEST_CASE("TaskbarPresenter derives aligned overlay layout from formatted rows", "[taskbar]") {
    TaskbarPresenter presenter;

    const auto layout = presenter.deriveOverlayLayoutForTesting({
        makeMetrics("openai", 12, 100),
        makeMetrics("minimax", 95, 100)
    });

    REQUIRE(layout.rows.size() == 2);
    CHECK(layout.rows[0].label == "openai:");
    CHECK(layout.rows[0].primaryMetric == "88%");
    CHECK(layout.rows[0].statusIcon.empty());
    CHECK(layout.rows[1].primaryMetric == "5%");
    CHECK(layout.rows[1].statusIcon.empty());
    CHECK(layout.rows[1].emphasizePrimary);
    CHECK(layout.rows[0].primaryX == layout.rows[1].primaryX);
    CHECK(layout.transparentBackground);
    CHECK(layout.columnCount == 1);
    CHECK(layout.lineHeight == 18);
    CHECK(layout.height == 42);
}

TEST_CASE("TaskbarPresenter keeps transparent overlay bounds compact", "[taskbar]") {
    TaskbarPresenter presenter;

    const auto layout = presenter.deriveOverlayLayoutForTesting({
        makeMetrics("openai", 10, 100)
    });

    REQUIRE(layout.rows.size() == 1);
    CHECK(layout.transparentBackground);
    CHECK(layout.rows.front().labelX == 4);
    CHECK(layout.rows.front().primaryX == layout.labelColumnWidth + 10);
    CHECK(layout.columnCount == 1);
    CHECK(layout.lineHeight == 18);
    CHECK(layout.height == 22);
}

TEST_CASE("TaskbarPresenter switches to compact single-column layout for three rows", "[taskbar]") {
    TaskbarPresenter presenter;

    const auto layout = presenter.deriveOverlayLayoutForTesting({
        makeMetrics("openai", 10, 100),
        makeMetrics("minimax", 20, 100),
        makeMetrics("kimi", 30, 100)
    });

    REQUIRE(layout.rows.size() == 3);
    CHECK(layout.columnCount == 1);
    CHECK(layout.lineHeight == 16);
    CHECK(layout.height == 50);
    CHECK(layout.rows[0].columnIndex == 0);
    CHECK(layout.rows[2].rowInColumn == 2);
}

TEST_CASE("TaskbarPresenter splits four rows into two relaxed columns", "[taskbar]") {
    TaskbarPresenter presenter;

    const auto layout = presenter.deriveOverlayLayoutForTesting({
        makeMetrics("openai", 10, 100),
        makeMetrics("minimax", 20, 100),
        makeMetrics("claude", 30, 100),
        makeMetrics("kimi", 40, 100)
    });

    REQUIRE(layout.rows.size() == 4);
    CHECK(layout.columnCount == 2);
    CHECK(layout.lineHeight == 18);
    CHECK(layout.height == 42);
    CHECK(layout.rows[0].columnIndex == 1);
    CHECK(layout.rows[1].columnIndex == 1);
    CHECK(layout.rows[2].columnIndex == 0);
    CHECK(layout.rows[3].columnIndex == 0);
    CHECK(layout.rows[0].labelX > layout.rows[2].labelX);
    CHECK(layout.rows[0].y == layout.rows[2].y);
    CHECK(layout.rows[1].y == layout.rows[3].y);
}

TEST_CASE("TaskbarPresenter splits five rows into two compact columns", "[taskbar]") {
    TaskbarPresenter presenter;

    const auto layout = presenter.deriveOverlayLayoutForTesting({
        makeMetrics("openai", 10, 100),
        makeMetrics("minimax", 20, 100),
        makeMetrics("claude", 30, 100),
        makeMetrics("kimi", 40, 100),
        makeMetrics("runtime", 0, 0)
    });

    REQUIRE(layout.rows.size() == 5);
    CHECK(layout.columnCount == 2);
    CHECK(layout.lineHeight == 16);
    CHECK(layout.height == 50);
    CHECK(layout.rows[0].columnIndex == 1);
    CHECK(layout.rows[2].columnIndex == 1);
    CHECK(layout.rows[3].columnIndex == 0);
    CHECK(layout.rows[4].columnIndex == 0);
    CHECK(layout.rows[2].rowInColumn == 2);
    CHECK(layout.rows[4].rowInColumn == 1);
}

TEST_CASE("TaskbarPresenter renders OpenAI dual-window quota rows without a trailing icon", "[taskbar]") {
    TaskbarPresenter presenter;

    TaskbarMetrics metrics = makeMetrics("openai", 9, 100);
    metrics.providerDisplayName = "OpenAI";
    metrics.secondaryWindowType = "weekly";
    metrics.secondaryRemainingPercentage = 65.0;
    metrics.tooltipDetail = "Codex throughput 5m: 60 tok/min, 0.4 prm/min";

    const auto layout = presenter.deriveOverlayLayoutForTesting({metrics});

    REQUIRE(layout.rows.size() == 1);
    CHECK(layout.rows.front().label == "OpenAI:");
    CHECK(layout.rows.front().primaryMetric == "91%");
    CHECK(layout.rows.front().secondaryPrimaryMetric == "65%");
    CHECK(layout.rows.front().statusIcon.empty());
    CHECK(layout.rows.front().separatorX > layout.rows.front().primaryX);
    CHECK(layout.rows.front().secondaryX > layout.rows.front().separatorX);
    CHECK(layout.rows.front().pressureBand == TaskbarPressureBand::Blue);
    CHECK(layout.rows.front().secondaryPressureBand == TaskbarPressureBand::Green);
}

TEST_CASE("TaskbarPresenter explains hover detail text for OpenAI dual-window quota rows", "[taskbar]") {
    TaskbarPresenter presenter;

    TaskbarMetrics metrics = makeMetrics("openai", 9, 100);
    metrics.providerDisplayName = "OpenAI";
    metrics.providerSurface = "openai_subscription_codex_local";
    metrics.windowResetAt = 1776592800;
    metrics.secondaryWindowType = "weekly";
    metrics.secondaryRemainingPercentage = 65.0;
    metrics.secondaryWindowResetAt = 1776816000;
    metrics.tooltipDetail =
        "Codex throughput 5m: 60 tok/min, 0.4 prm/min\n"
        "Codex throughput 30m: 10 tok/min, 0.1 prm/min";

    const std::string tooltip = presenter.deriveHoverTooltipForTesting({metrics}, 0);

    CHECK(tooltip.find("OpenAI") != std::string::npos);
    CHECK(tooltip.find("5h remaining: 91%") != std::string::npos);
    CHECK(tooltip.find("resets ") != std::string::npos);
    CHECK(tooltip.find("2026-04-19 10:00 UTC") == std::string::npos);
    CHECK(tooltip.find("Weekly remaining: 65%") != std::string::npos);
    CHECK(tooltip.find("Codex throughput 5m: 60 tok/min, 0.4 prm/min") != std::string::npos);
    CHECK(tooltip.find("Codex throughput 30m: 10 tok/min, 0.1 prm/min") != std::string::npos);
    CHECK(tooltip.find("Approx units left:") == std::string::npos);
}

TEST_CASE("TaskbarPresenter keeps Kimi subscription windows remaining-first like other providers", "[taskbar]") {
    TaskbarPresenter presenter;

    TaskbarMetrics metrics;
    metrics.providerKey = "kimi";
    metrics.providerDisplayName = "Kimi Code";
    metrics.providerSurface = "kimi_subscription_api";
    metrics.used = 64;
    metrics.limit = 100;
    metrics.remainingPercentage = 36.0;
    metrics.usagePercentage = 64.0;
    metrics.secondaryWindowType = "weekly";
    metrics.secondaryRemainingPercentage = 72.0;

    const TaskbarMetrics derived = presenter.deriveMetricsForTesting(metrics);
    CHECK(derived.primaryMetric == "36%");
    CHECK(derived.secondaryPrimaryMetric == "72%");
    CHECK(derived.secondaryMetric == "36 left");
    CHECK(derived.pressureBand == TaskbarPressureBand::Yellow);
    CHECK(derived.secondaryPressureBand == TaskbarPressureBand::Green);

    const std::string tooltip = presenter.deriveHoverTooltipForTesting({metrics}, 0);
    CHECK(tooltip.find("Kimi Code") != std::string::npos);
    CHECK(tooltip.find("5h remaining: 36%") != std::string::npos);
    CHECK(tooltip.find("7d remaining: 72%") != std::string::npos);
    CHECK(tooltip.find("Approx units left:") == std::string::npos);
    CHECK(tooltip.find("left") == std::string::npos);
}

TEST_CASE("TaskbarPresenter explains hover detail text for MiniMax remaining rows", "[taskbar]") {
    TaskbarPresenter presenter;

    TaskbarMetrics metrics = makeMetrics("minimax", 750, 2000);
    metrics.providerDisplayName = "MiniMax";
    metrics.providerSurface = "minimax_subscription_coding_plan";
    metrics.remainingPercentage = 62.5;
    metrics.usagePercentage = 37.5;
    metrics.windowResetAt = 1776592800;

    const TaskbarMetrics derived = presenter.deriveMetricsForTesting(metrics);
    CHECK(derived.secondaryMetric == "1K left");

    const std::string tooltip = presenter.deriveHoverTooltipForTesting({metrics}, 0);

    CHECK(tooltip.find("MiniMax") != std::string::npos);
    CHECK(tooltip.find("Current plan remaining: 63%") != std::string::npos);
    CHECK(tooltip.find("Prompts left: 1250 left") != std::string::npos);
    CHECK(tooltip.find("Prompts left: 1K left") == std::string::npos);
    CHECK(tooltip.find("resets ") != std::string::npos);
    CHECK(tooltip.find("2026-04-19 10:00 UTC") == std::string::npos);
}

TEST_CASE("TaskbarPresenter exposes deterministic placement math for overlap repros", "[taskbar]") {
    TaskbarPresenter presenter;

    SECTION("Embedded placement shifts left to clear a nearby right-edge blocker") {
        TaskbarPresenter::PlacementInputForTesting input;
        input.embedded = true;
        input.width = 120;
        input.height = 24;
        input.taskbarClientRect = makeRect(0, 0, 1000, 40);
        input.hasNotificationArea = true;
        input.notificationAreaRect = makeRect(860, 0, 1000, 40);
        input.blockingRects.push_back(makeRect(720, 0, 810, 40));

        const auto placement = presenter.derivePlacementForTesting(input);

        REQUIRE(placement.width == 120);
        REQUIRE(placement.height == 24);
        CHECK(placement.x == 594);
        CHECK(placement.y == 8);
    }

    SECTION("Embedded placement ignores left-half and giant task-list blockers") {
        TaskbarPresenter::PlacementInputForTesting input;
        input.embedded = true;
        input.width = 120;
        input.height = 24;
        input.taskbarClientRect = makeRect(0, 0, 1000, 40);
        input.hasNotificationArea = true;
        input.notificationAreaRect = makeRect(860, 0, 1000, 40);
        input.blockingRects.push_back(makeRect(100, 0, 280, 40));
        input.blockingRects.push_back(makeRect(0, 0, 850, 40));

        const auto placement = presenter.derivePlacementForTesting(input);

        CHECK(placement.x == 732);
        CHECK(placement.y == 8);
    }

    SECTION("Popup placement anchors left of TrafficMonitor when present") {
        TaskbarPresenter::PlacementInputForTesting input;
        input.width = 120;
        input.height = 24;
        input.taskbarRect = makeRect(0, 1000, 1200, 1040);
        input.hasNotificationArea = true;
        input.notificationAreaRect = makeRect(1050, 1000, 1200, 1040);
        input.hasTrafficMonitor = true;
        input.trafficMonitorRect = makeRect(900, 1000, 980, 1040);

        const auto placement = presenter.derivePlacementForTesting(input);

        CHECK(placement.x == 772);
        CHECK(placement.y == 1008);
    }

    SECTION("Popup placement ignores transient right-edge blockers without a TrafficMonitor hint") {
        TaskbarPresenter::PlacementInputForTesting input;
        input.width = 120;
        input.height = 24;
        input.taskbarRect = makeRect(0, 1000, 1200, 1040);
        input.hasNotificationArea = true;
        input.notificationAreaRect = makeRect(1050, 1000, 1200, 1040);
        input.blockingRects.push_back(makeRect(900, 1000, 980, 1040));

        const auto placement = presenter.derivePlacementForTesting(input);

        CHECK(placement.x == 922);
        CHECK(placement.y == 1008);
    }
}

TEST_CASE("TaskbarPresenter flags shell/taskbar handle changes that require overlay recreation", "[taskbar]") {
    TaskbarPresenter presenter;

    CHECK_FALSE(presenter.shouldRecreateSurfaceForTesting(0, 0, false));
    CHECK_FALSE(presenter.shouldRecreateSurfaceForTesting(100, 100, true));
    CHECK_FALSE(presenter.shouldRecreateSurfaceForTesting(100, 200, false));
    CHECK_FALSE(presenter.shouldRecreateSurfaceForTesting(0, 200, true));
    CHECK(presenter.shouldRecreateSurfaceForTesting(100, 200, true));
}

TEST_CASE("TaskbarPresenter recreates and repositions a lost surface without a refresh", "[taskbar]") {
    const TaskbarWindowFixture taskbar;
    REQUIRE(taskbar.get() != nullptr);
    REQUIRE(FindWindowW(L"Shell_TrayWnd", nullptr) == taskbar.get());

    TaskbarPresenter presenter;
    REQUIRE(presenter.init());

    const auto findSurface = [&taskbar]() {
        return FindWindowExW(taskbar.get(), nullptr, L"JiStatsTaskbarSurfaceWindow", nullptr);
    };
    const HWND initialSurface = findSurface();
    REQUIRE(initialSurface != nullptr);

    DWORD processId = 0;
    GetWindowThreadProcessId(initialSurface, &processId);
    REQUIRE(processId == GetCurrentProcessId());

    RECT initialBounds{};
    REQUIRE(GetWindowRect(initialSurface, &initialBounds) != 0);
    DWORD_PTR result = 0;
    REQUIRE(SendMessageTimeoutW(initialSurface, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 2000, &result) != 0);
    REQUIRE_FALSE(IsWindow(initialSurface));

    const auto waitForSurface = [&](const auto& matches) {
        // The heartbeat runs at 1 s during recovery and 5 s while healthy.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        do {
            const HWND candidate = findSurface();
            RECT bounds{};
            if (candidate != nullptr && GetWindowRect(candidate, &bounds) != 0 && matches(bounds)) {
                return candidate;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (std::chrono::steady_clock::now() < deadline);
        return static_cast<HWND>(nullptr);
    };

    const HWND replacement =
        waitForSurface([&](const RECT& bounds) { return EqualRect(&initialBounds, &bounds) != 0; });
    REQUIRE(replacement != nullptr);
    REQUIRE(taskbar.addTrayAnchor());
    REQUIRE(waitForSurface([&](const RECT& bounds) { return bounds.left < initialBounds.left; }) == replacement);

    REQUIRE(SendMessageTimeoutW(replacement, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 2000, &result) != 0);
    REQUIRE_FALSE(IsWindow(replacement));
    REQUIRE(presenter.isInitialized());
    presenter.replaceMetrics({makeMetrics("openai", 25, 100)});
    REQUIRE(waitForSurface([&](const RECT& bounds) { return bounds.left < initialBounds.left; }) != nullptr);
    const auto metrics = presenter.getCurrentMetrics();
    REQUIRE(metrics.size() == 1);
    CHECK(metrics[0].providerKey == "openai");
    CHECK(metrics[0].primaryMetric == "75%");
}

TEST_CASE("TaskbarPresenter keeps hover suppressed and preserves popup visibility while context menu is active", "[taskbar]") {
    TaskbarPresenter presenter;

    presenter.setInteractionStateForTesting(true, 2, true, false, false);

    const auto rightClickState = presenter.getInteractionStateForTesting();
    CHECK(rightClickState.tooltipVisible);
    CHECK(rightClickState.hoveredRowIndex == 2);
    CHECK(rightClickState.rightButtonCaptured);
    CHECK(rightClickState.hoverSuppressed);
    CHECK(rightClickState.preferTopmostSurface);
    CHECK(rightClickState.tooltipFontHeight == 12);

    presenter.beginContextMenuInteractionForTesting();

    const auto menuOpenState = presenter.getInteractionStateForTesting();
    CHECK_FALSE(menuOpenState.tooltipVisible);
    CHECK(menuOpenState.hoveredRowIndex == -1);
    CHECK_FALSE(menuOpenState.rightButtonCaptured);
    CHECK(menuOpenState.contextMenuOpen);
    CHECK(menuOpenState.hoverSuppressed);
    CHECK(menuOpenState.preferTopmostSurface);

    presenter.endContextMenuInteractionForTesting();

    const auto menuClosedState = presenter.getInteractionStateForTesting();
    CHECK_FALSE(menuClosedState.contextMenuOpen);
    CHECK_FALSE(menuClosedState.hoverSuppressed);
    CHECK(menuClosedState.preferTopmostSurface);

    presenter.setInteractionStateForTesting(false, -1, false, false, true);
    const auto embeddedState = presenter.getInteractionStateForTesting();
    CHECK_FALSE(embeddedState.preferTopmostSurface);
}

TEST_CASE("TaskbarPresenter does not treat overlay right clicks as a new menu request while a menu is already open", "[taskbar]") {
    TaskbarPresenter presenter;

    CHECK(presenter.shouldCaptureOverlayRightClickForTesting(false, true));
    CHECK_FALSE(presenter.shouldCaptureOverlayRightClickForTesting(true, true));
    CHECK_FALSE(presenter.shouldCaptureOverlayRightClickForTesting(false, false));
}

TEST_CASE("TaskbarPresenter requests overlay recovery for taskbar clicks outside the overlay surface", "[taskbar]") {
    TaskbarPresenter presenter;

    CHECK(presenter.shouldRecoverOverlayAfterTaskbarClickForTesting(false, false, true));
    CHECK_FALSE(presenter.shouldRecoverOverlayAfterTaskbarClickForTesting(true, false, true));
    CHECK_FALSE(presenter.shouldRecoverOverlayAfterTaskbarClickForTesting(false, true, true));
    CHECK_FALSE(presenter.shouldRecoverOverlayAfterTaskbarClickForTesting(false, false, false));
}

TEST_CASE("TaskbarPresenter dispatches its own context menu actions without the tray bridge", "[taskbar]") {
    TaskbarPresenter presenter;

    int showDashboardCalls = 0;
    int preferencesCalls = 0;
    int retryCalls = 0;
    int dismissCalls = 0;
    int exitCalls = 0;

    presenter.setOnShowDashboardCallback([&showDashboardCalls]() { ++showDashboardCalls; });
    presenter.setOnPreferencesCallback([&preferencesCalls]() { ++preferencesCalls; });
    presenter.setOnRetryNowCallback([&retryCalls]() { ++retryCalls; });
    presenter.setOnDismissOfflineNoticeCallback([&dismissCalls]() { ++dismissCalls; });
    presenter.setOnExitCallback([&exitCalls]() { ++exitCalls; });

    CHECK(presenter.dispatchContextMenuActionForTesting(
        TaskbarPresenter::ContextMenuActionForTesting::ShowDashboard));
    CHECK(presenter.dispatchContextMenuActionForTesting(
        TaskbarPresenter::ContextMenuActionForTesting::Preferences));
    CHECK_FALSE(presenter.dispatchContextMenuActionForTesting(
        TaskbarPresenter::ContextMenuActionForTesting::RetryNow));
    CHECK_FALSE(presenter.dispatchContextMenuActionForTesting(
        TaskbarPresenter::ContextMenuActionForTesting::DismissOfflineNotice));
    CHECK(presenter.dispatchContextMenuActionForTesting(
        TaskbarPresenter::ContextMenuActionForTesting::Exit));

    presenter.setOfflineMenuState(true, true);

    CHECK(presenter.dispatchContextMenuActionForTesting(
        TaskbarPresenter::ContextMenuActionForTesting::RetryNow));
    CHECK(presenter.dispatchContextMenuActionForTesting(
        TaskbarPresenter::ContextMenuActionForTesting::DismissOfflineNotice));

    CHECK(showDashboardCalls == 1);
    CHECK(preferencesCalls == 1);
    CHECK(retryCalls == 1);
    CHECK(dismissCalls == 1);
    CHECK(exitCalls == 1);
}
