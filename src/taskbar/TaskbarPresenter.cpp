// TaskbarPresenter.cpp - Windows taskbar overlay implementation
//
// Taskbar embedding approach based on TrafficMonitor by zhongyang219
// Reference: https://github.com/zhongyang219/TrafficMonitor
// Key patterns from TrafficMonitor/Win11TaskbarDlg.cpp and TaskBarDlg.cpp:
//   - SetParent() to embed window in taskbar hierarchy
//   - Transparent embedded rendering plus a dedicated hit target for reliable interaction
//   - Position detection to avoid overlapping system tray and other taskbar tools
//
// TrafficMonitor is released under the MIT License.
// Copyright (c) 2017 zhongyang219

#include "taskbar/TaskbarPresenter.hpp"
#include "core/Logger.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#endif

namespace JiStats {

namespace {

constexpr int kTaskbarMinimumX = 4;
constexpr int kTaskbarOuterGap = 8;
constexpr int kTaskbarBlockingGap = 6;
constexpr int kOverlayHorizontalPadding = 4;
constexpr int kOverlayRightPadding = 4;
constexpr int kOverlayColumnGap = 6;
constexpr int kOverlayGroupGap = 14;
constexpr int kOverlayMinimumWidth = 48;
constexpr int kOverlayMinimumHeight = 18;
constexpr int kOverlayRelaxedVerticalPadding = 2;
constexpr int kOverlayRelaxedRowGap = 2;
constexpr int kOverlayRelaxedLineHeight = 18;
constexpr int kOverlayRelaxedFontHeight = 12;
constexpr int kOverlayCompactVerticalPadding = 1;
constexpr int kOverlayCompactRowGap = 0;
constexpr int kOverlayCompactLineHeight = 16;
constexpr int kOverlayCompactFontHeight = 11;
constexpr UINT_PTR kOverlayHeartbeatTimerId = 1;
constexpr UINT kOverlayHeartbeatFastIntervalMs = 1000;
constexpr UINT kOverlayHeartbeatSlowIntervalMs = 5000;
constexpr int kTooltipHorizontalPadding = 10;
constexpr int kTooltipVerticalPadding = 8;
constexpr int kTooltipMaxWidth = 320;
constexpr int kTooltipFontHeight = 12;
constexpr UINT kTaskbarMenuShowDashboard = 1001;
constexpr UINT kTaskbarMenuPreferences = 1002;
constexpr UINT kTaskbarMenuRetryNow = 1003;
constexpr UINT kTaskbarMenuDismissOfflineNotice = 1004;
constexpr UINT kTaskbarMenuExit = 1005;

struct PlacementRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

struct OverlayRowLayout {
    std::wstring label;
    std::wstring primary;
    std::wstring secondaryPrimary;
    std::wstring icon;
    TaskbarPressureBand pressureBand = TaskbarPressureBand::Neutral;
    TaskbarPressureBand secondaryPressureBand = TaskbarPressureBand::Neutral;
    bool emphasizePrimary = false;
    bool emphasizeSecondary = false;
    int labelX = 0;
    int primaryX = 0;
    int separatorX = 0;
    int secondaryX = 0;
    int iconX = 0;
    int y = 0;
    int lineWidth = 0;
    int hitWidth = 0;
    int columnIndex = 0;
    int rowInColumn = 0;
};

struct OverlayScaleProfile {
    int verticalPadding = kOverlayCompactVerticalPadding;
    int rowGap = kOverlayCompactRowGap;
    int lineHeight = kOverlayCompactLineHeight;
    int fontHeight = kOverlayCompactFontHeight;
};

struct OverlayLayout {
    std::vector<OverlayRowLayout> rows;
    int width = 0;
    int height = 0;
    int labelColumnWidth = 0;
    int primaryColumnWidth = 0;
    int iconColumnWidth = 0;
    int lineHeight = kOverlayCompactLineHeight;
    int columnCount = 1;
};

bool rectsIntersect(const PlacementRect& a, const PlacementRect& b) {
    return a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top;
}

int rectWidth(const PlacementRect& rect) {
    return rect.right - rect.left;
}

int rectHeight(const PlacementRect& rect) {
    return rect.bottom - rect.top;
}

int overlayColumnCountForMetrics(size_t metricCount) {
    return metricCount <= 3 ? 1 : 2;
}

OverlayScaleProfile overlayScaleProfileForMetrics(size_t metricCount) {
    const int columnCount = overlayColumnCountForMetrics(metricCount);
    const int maxRowsPerColumn = columnCount <= 1
        ? static_cast<int>(metricCount)
        : static_cast<int>((metricCount + static_cast<size_t>(columnCount) - 1) / static_cast<size_t>(columnCount));
    if (maxRowsPerColumn <= 2) {
        return OverlayScaleProfile{
            kOverlayRelaxedVerticalPadding,
            kOverlayRelaxedRowGap,
            kOverlayRelaxedLineHeight,
            kOverlayRelaxedFontHeight};
    }

    return OverlayScaleProfile{
        kOverlayCompactVerticalPadding,
        kOverlayCompactRowGap,
        kOverlayCompactLineHeight,
        kOverlayCompactFontHeight};
}

bool taskbarMetricsEqual(const TaskbarMetrics& lhs, const TaskbarMetrics& rhs) {
    return lhs.providerKey == rhs.providerKey &&
        lhs.providerDisplayName == rhs.providerDisplayName &&
        lhs.kind == rhs.kind &&
        lhs.used == rhs.used &&
        lhs.limit == rhs.limit &&
        lhs.usagePercentage == rhs.usagePercentage &&
        lhs.remainingPercentage == rhs.remainingPercentage &&
        lhs.windowResetAt == rhs.windowResetAt &&
        lhs.secondaryWindowType == rhs.secondaryWindowType &&
        lhs.secondaryRemainingPercentage == rhs.secondaryRemainingPercentage &&
        lhs.secondaryWindowResetAt == rhs.secondaryWindowResetAt &&
        lhs.confidenceTier == rhs.confidenceTier &&
        lhs.providerSurface == rhs.providerSurface &&
        lhs.lastUpdated == rhs.lastUpdated &&
        lhs.summaryLabel == rhs.summaryLabel &&
        lhs.tooltipDetail == rhs.tooltipDetail &&
        lhs.primaryMetric == rhs.primaryMetric &&
        lhs.secondaryPrimaryMetric == rhs.secondaryPrimaryMetric &&
        lhs.secondaryMetric == rhs.secondaryMetric &&
        lhs.statusIcon == rhs.statusIcon &&
        lhs.pressureBand == rhs.pressureBand &&
        lhs.secondaryPressureBand == rhs.secondaryPressureBand &&
        lhs.emphasizePrimary == rhs.emphasizePrimary &&
        lhs.emphasizeSecondary == rhs.emphasizeSecondary;
}

bool taskbarMetricListsEqual(const std::vector<TaskbarMetrics>& lhs,
                             const std::vector<TaskbarMetrics>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t index = 0; index < lhs.size(); ++index) {
        if (!taskbarMetricsEqual(lhs[index], rhs[index])) {
            return false;
        }
    }
    return true;
}

std::wstring toLowerWide(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

std::wstring baseNameLowerFromPath(const std::wstring& value) {
    const size_t slash = value.find_last_of(L"\\/");
    const std::wstring baseName = slash == std::wstring::npos ? value : value.substr(slash + 1);
    return toLowerWide(baseName);
}

std::wstring widenUtf8(const std::string& value) {
#ifdef _WIN32
    if (value.empty()) {
        return L"";
    }

    const int count = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (count <= 0) {
        return std::wstring(value.begin(), value.end());
    }

    std::wstring wide(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, wide.data(), count);
    wide.resize(static_cast<size_t>(count - 1));
    return wide;
#else
    return std::wstring(value.begin(), value.end());
#endif
}

std::string wideToUtf8Local(const std::wstring& value) {
    if (value.empty()) {
        return "";
    }

    const int count = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        std::string fallback;
        fallback.reserve(value.size());
        for (const wchar_t ch : value) {
            fallback.push_back(ch <= 0x7f ? static_cast<char>(ch) : '?');
        }
        return fallback;
    }

    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), count, nullptr, nullptr);
    result.resize(static_cast<size_t>(count - 1));
    return result;
}

std::string formatSystemDateTime(std::time_t time, DWORD timeFlags = 0) {
    std::tm localTm{};
    localtime_s(&localTm, &time);

    SYSTEMTIME systemTime{};
    systemTime.wYear = static_cast<WORD>(localTm.tm_year + 1900);
    systemTime.wMonth = static_cast<WORD>(localTm.tm_mon + 1);
    systemTime.wDay = static_cast<WORD>(localTm.tm_mday);
    systemTime.wHour = static_cast<WORD>(localTm.tm_hour);
    systemTime.wMinute = static_cast<WORD>(localTm.tm_min);
    systemTime.wSecond = static_cast<WORD>(localTm.tm_sec);
    systemTime.wMilliseconds = 0;

    wchar_t dateBuffer[128]{};
    wchar_t timeBuffer[128]{};
    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &systemTime, nullptr,
                        dateBuffer, static_cast<int>(std::size(dateBuffer)), nullptr) == 0 ||
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, timeFlags, &systemTime, nullptr,
                        timeBuffer, static_cast<int>(std::size(timeBuffer))) == 0) {
        std::ostringstream fallback;
        fallback << std::put_time(&localTm, "%x %X");
        return fallback.str();
    }

    return wideToUtf8Local(std::wstring(dateBuffer) + L" " + std::wstring(timeBuffer));
}

std::string formatResetTimeLabel(std::time_t time) {
    if (time <= 0) {
        return "";
    }
    return formatSystemDateTime(time, TIME_NOSECONDS);
}

std::string narrowWideForTesting(const std::wstring& value) {
    std::string result;
    result.reserve(value.size());
    for (wchar_t ch : value) {
        result.push_back(ch <= 0x7f ? static_cast<char>(ch) : '?');
    }
    return result;
}

#ifdef JISTATS_ENABLE_TEST_HOOKS
PlacementRect toPlacementRect(const TaskbarPresenter::GeometryRectForTesting& rect) {
    return PlacementRect{rect.left, rect.top, rect.right, rect.bottom};
}
#endif

#ifdef _WIN32
constexpr COLORREF kOverlayLayeredBackgroundColor = RGB(1, 1, 1);
constexpr BYTE kOverlayLayeredBackgroundAlpha = 1;

std::wstring processImageNameLowerForWindow(HWND hwnd) {
    if (hwnd == nullptr) {
        return L"";
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0) {
        return L"";
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return L"";
    }

    std::wstring path(1024, L'\0');
    DWORD pathLength = static_cast<DWORD>(path.size());
    if (QueryFullProcessImageNameW(process, 0, path.data(), &pathLength) == 0 || pathLength == 0) {
        CloseHandle(process);
        return L"";
    }

    CloseHandle(process);
    path.resize(pathLength);
    return baseNameLowerFromPath(path);
}
#endif

} // namespace

class TaskbarPresenter::Impl {
public:
  std::atomic<bool> initialized{false};
  bool available = false;
  std::string lastError;
  std::vector<TaskbarMetrics> currentMetrics;
  std::mutex mutex;
  bool overlayEnabled = true;
  std::function<void()> onShowDashboardCallback;
  std::function<void()> onPreferencesCallback;
  std::function<void()> onRetryNowCallback;
  std::function<void()> onDismissOfflineNoticeCallback;
  std::function<void()> onExitCallback;
  bool offlineRetryAvailable = false;
  bool offlineDismissAvailable = false;
#ifdef JISTATS_ENABLE_TEST_HOOKS
    std::uint64_t refreshRequestCountForTesting = 0;
#endif

#ifdef _WIN32
    std::thread uiThread;
    std::condition_variable readyCv;
    HWND controllerWindow = nullptr;
    HWND surfaceWindow = nullptr;
    HWND tooltipWindow = nullptr;
    HWND taskbarWindow = nullptr;
    UINT taskbarCreatedMessage = 0;
    bool threadReady = false;
    std::atomic<bool> stopRequested{false};
    bool embedded = false;
    std::wstring displayText = L"JiStats";
    std::wstring lastPaintedText;
    OverlayLayout overlayLayout;
    COLORREF textColor = RGB(255, 255, 255);
    static constexpr UINT WM_JISTATS_REFRESH = WM_APP + 1;
    int lastSurfaceX = std::numeric_limits<int>::min();
    int lastSurfaceY = std::numeric_limits<int>::min();
    int lastSurfaceWidth = 0;
    int lastSurfaceHeight = 0;
    int hoveredRowIndex = -1;
    bool tooltipVisible = false;
    std::wstring tooltipText;
    POINT lastTooltipAnchor{std::numeric_limits<LONG>::min(), std::numeric_limits<LONG>::min()};
    SIZE lastTooltipSize{0, 0};
    bool rightButtonCaptured = false;
    bool contextMenuOpen = false;
    UINT currentHeartbeatTimerIntervalMs = 0;
    bool trackingMouseLeave = false;
#endif

    bool shouldRecreateSurface(HWND discoveredTaskbar) const {
#ifdef _WIN32
        return surfaceWindow != nullptr &&
               taskbarWindow != nullptr &&
               discoveredTaskbar != nullptr &&
               discoveredTaskbar != taskbarWindow;
#else
        (void)discoveredTaskbar;
        return false;
#endif
    }

    // Format percentage for display
    std::string formatPercentage(double value) const {
        const double clamped = std::clamp(value, 0.0, 100.0);
        if (clamped >= 99.5) {
            return "100%";
        }
        if (clamped >= 10.0) {
            return std::to_string(static_cast<int>(std::round(clamped))) + "%";
        }
        if (clamped >= 1.0) {
            return std::to_string(static_cast<int>(std::round(clamped))) + "%";
        }
        if (clamped > 0.0) {
            std::ostringstream stream;
            stream << std::fixed << std::setprecision(1) << clamped;
            return stream.str() + "%";
        }
        return "0%";
    }
    
    // Format large numbers for display (K, M)
    std::string formatNumber(long long value) const {
        if (value >= 1000000) {
            return std::to_string(value / 1000000) + "M";
        } else if (value >= 1000) {
            return std::to_string(value / 1000) + "K";
        }
        return std::to_string(value);
    }

    bool hasExplicitRemainingPercentage(const TaskbarMetrics& metrics) const {
        return metrics.remainingPercentage > 0.0 || metrics.usagePercentage > 0.0;
    }

    bool hasSecondaryRemainingPercentage(const TaskbarMetrics& metrics) const {
        return metrics.secondaryRemainingPercentage >= 0.0;
    }

    double deriveRemainingPercentage(const TaskbarMetrics& metrics) const {
        if (hasExplicitRemainingPercentage(metrics)) {
            return std::clamp(metrics.remainingPercentage, 0.0, 100.0);
        }
        if (metrics.limit > 0) {
            return std::clamp(
                (static_cast<double>(std::max(0LL, metrics.limit - metrics.used)) / metrics.limit) * 100.0,
                0.0,
                100.0);
        }
        if (metrics.usagePercentage > 0.0) {
            return std::clamp(100.0 - metrics.usagePercentage, 0.0, 100.0);
        }
        return 0.0;
    }

    long long deriveRemainingValue(const TaskbarMetrics& metrics, double remainingPercentage) const {
        if (metrics.limit <= 0) {
            return 0;
        }
        if (hasExplicitRemainingPercentage(metrics)) {
            return std::clamp(
                static_cast<long long>(std::llround(
                    (remainingPercentage / 100.0) * static_cast<double>(metrics.limit))),
                0LL,
                metrics.limit);
        }
        return std::max(0LL, metrics.limit - metrics.used);
    }

    TaskbarPressureBand pressureBandForRemaining(double remainingPercentage) const {
        const double clamped = std::clamp(remainingPercentage, 0.0, 100.0);
        if (clamped >= 80.0) {
            return TaskbarPressureBand::Blue;
        }
        if (clamped >= 50.0) {
            return TaskbarPressureBand::Green;
        }
        if (clamped >= 25.0) {
            return TaskbarPressureBand::Yellow;
        }
        if (clamped > 10.0) {
            return TaskbarPressureBand::Orange;
        }
        return TaskbarPressureBand::Red;
    }

    TaskbarPressureBand pressureBandForStateIcon(const std::string& icon) const {
        if (icon == "■") {
            return TaskbarPressureBand::Red;
        }
        if (icon == "◆") {
            return TaskbarPressureBand::Orange;
        }
        if (icon == "▲") {
            return TaskbarPressureBand::Yellow;
        }
        return TaskbarPressureBand::Neutral;
    }

    int pressureBandSeverity(TaskbarPressureBand band) const {
        switch (band) {
            case TaskbarPressureBand::Blue: return 1;
            case TaskbarPressureBand::Green: return 2;
            case TaskbarPressureBand::Yellow: return 3;
            case TaskbarPressureBand::Orange: return 4;
            case TaskbarPressureBand::Red: return 5;
            default: return 0;
        }
    }

    TaskbarPressureBand mergePressureBands(TaskbarPressureBand first,
                                           TaskbarPressureBand second) const {
        return pressureBandSeverity(first) >= pressureBandSeverity(second) ? first : second;
    }

    std::string iconForBand(TaskbarPressureBand band) const {
        switch (band) {
            case TaskbarPressureBand::Blue:
            case TaskbarPressureBand::Green:
                return "\xE2\x97\x8F"; // ●
            case TaskbarPressureBand::Yellow:
                return "\xE2\x96\xB2"; // ▲
            case TaskbarPressureBand::Orange:
                return "\xE2\x97\x86"; // ◆
            case TaskbarPressureBand::Red:
                return "\xE2\x96\xA0"; // ■
            default:
                return "\xE2\x97\x8B"; // ○
        }
    }

    std::string normalizeStatusIcon(const std::string& icon) const {
        if (icon.empty()) {
            return "";
        }
        if (icon == "+" || icon == "●") {
            return "\xE2\x97\x8F"; // ●
        }
        if (icon == "~" || icon == "▲") {
            return "\xE2\x96\xB2"; // ▲
        }
        if (icon == "!" || icon == "■") {
            return "\xE2\x96\xA0"; // ■
        }
        if (icon == "?" || icon == "◆") {
            return "\xE2\x97\x86"; // ◆
        }
        return icon;
    }

    std::string displayNameForMetric(const TaskbarMetrics& metrics) const {
        if (!metrics.providerDisplayName.empty()) {
            return metrics.providerDisplayName;
        }
        return metrics.providerKey;
    }

    bool isKimiSubscriptionMetric(const TaskbarMetrics& metrics) const {
        return metrics.providerKey == "kimi" &&
            metrics.providerSurface == "kimi_subscription_api" &&
            metrics.limit > 0;
    }

    std::string primaryWindowDescription(const TaskbarMetrics& metrics) const {
        if (metrics.kind == TaskbarMetricKind::PersistedActivity) {
            return "Local activity summary";
        }
        if (isKimiSubscriptionMetric(metrics) && metrics.secondaryRemainingPercentage >= 0.0) {
            return "5h remaining";
        }
        if (metrics.providerKey == "openai" && metrics.secondaryRemainingPercentage >= 0.0) {
            return "5h remaining";
        }
        if (metrics.providerKey == "minimax") {
            return "Current plan remaining";
        }
        return "Current window remaining";
    }

    std::string secondaryWindowDescription(const TaskbarMetrics& metrics) const {
        if (isKimiSubscriptionMetric(metrics) && !metrics.secondaryPrimaryMetric.empty()) {
            return metrics.secondaryWindowType == "weekly"
                ? "7d remaining"
                : "Secondary remaining";
        }
        if (metrics.secondaryRemainingPercentage < 0.0) {
            return "";
        }
        if (metrics.secondaryWindowType == "weekly") {
            return "Weekly remaining";
        }
        if (metrics.secondaryWindowType == "interval") {
            return "Secondary interval remaining";
        }
        if (!metrics.secondaryWindowType.empty()) {
            return metrics.secondaryWindowType + " remaining";
        }
        return "Secondary remaining";
    }

    std::string remainingUnitsDescription(const TaskbarMetrics& metrics) const {
        if (metrics.providerSurface == "minimax_subscription_coding_plan") {
            return "Prompts left";
        }
        if (metrics.providerSurface == "minimax_subscription_token_plan") {
            return "Tokens left";
        }
        return "Approx units left";
    }

    std::string exactRemainingUnitsText(const TaskbarMetrics& metrics) const {
        const double remainingPercentage = deriveRemainingPercentage(metrics);
        const long long remainingValue = deriveRemainingValue(metrics, remainingPercentage);
        return std::to_string(remainingValue) + " left";
    }

    std::wstring buildHoverTooltipText(const std::vector<TaskbarMetrics>& metricsList,
                                       size_t rowIndex) const {
        if (rowIndex >= metricsList.size()) {
            return L"";
        }

        const TaskbarMetrics metrics = deriveMetrics(metricsList[rowIndex]);
        std::ostringstream stream;
        stream << displayNameForMetric(metrics);

        if (metrics.kind == TaskbarMetricKind::PersistedActivity) {
            if (!metrics.summaryLabel.empty()) {
                stream << "\n" << metrics.summaryLabel;
            }
            if (!metrics.primaryMetric.empty()) {
                stream << "\nCurrent summary: " << metrics.primaryMetric;
            }
            if (!metrics.secondaryMetric.empty()) {
                stream << "\n" << metrics.secondaryMetric;
            }
            return widenUtf8(stream.str());
        }

        stream << "\n" << primaryWindowDescription(metrics) << ": " << metrics.primaryMetric;
        if (metrics.windowResetAt > 0) {
            stream << " (resets " << formatResetTimeLabel(metrics.windowResetAt) << ")";
        }
        if (metrics.providerKey == "minimax" && metrics.limit > 0) {
            stream << "\n" << remainingUnitsDescription(metrics) << ": "
                   << exactRemainingUnitsText(metrics);
        } else if (metrics.providerKey != "openai" &&
                   metrics.providerKey != "kimi" &&
                   !metrics.secondaryMetric.empty()) {
            stream << "\n" << remainingUnitsDescription(metrics) << ": " << metrics.secondaryMetric;
        }

        const std::string secondaryDescription = secondaryWindowDescription(metrics);
        if (!secondaryDescription.empty() && !metrics.secondaryPrimaryMetric.empty()) {
            stream << "\n" << secondaryDescription << ": " << metrics.secondaryPrimaryMetric;
            if (metrics.secondaryWindowResetAt > 0) {
                stream << " (resets " << formatResetTimeLabel(metrics.secondaryWindowResetAt) << ")";
            }
        }
        if (!metrics.tooltipDetail.empty()) {
            stream << "\n" << metrics.tooltipDetail;
        }

        return widenUtf8(stream.str());
    }

    POINT computeTooltipAnchor(const RECT& surfaceRect,
                               const RECT& taskbarRect,
                               const POINT& cursor,
                               const SIZE& bubbleSize) const {
        RECT monitorRect = taskbarRect;
        const HMONITOR monitor = MonitorFromRect(&surfaceRect, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo) != 0) {
            monitorRect = monitorInfo.rcMonitor;
        }

        const int margin = 4;
        const int bubbleWidth = std::max(1L, bubbleSize.cx);
        const int bubbleHeight = std::max(1L, bubbleSize.cy);

        const int preferredX = std::max(surfaceRect.left, cursor.x - (bubbleWidth / 2));
        const int minX = monitorRect.left + margin;
        const int maxX = std::max(minX, static_cast<int>(monitorRect.right) - bubbleWidth - margin);
        const int x = std::clamp(preferredX, minX, maxX);

        const int preferredY = taskbarRect.top - bubbleHeight - margin;
        const int minY = monitorRect.top + margin;
        const int maxY = std::max(minY, static_cast<int>(surfaceRect.top) - bubbleHeight - margin);
        const int y = std::clamp(preferredY, minY, maxY);

        return POINT{x, y};
    }

    int measureTextWidthApprox(const std::wstring& text, bool bold) const {
        int width = 0;
        for (wchar_t ch : text) {
            if (ch == L' ') {
                width += 4;
            } else if (ch >= L'0' && ch <= L'9') {
                width += 7;
            } else if (ch == L'%') {
                width += 7;
            } else if (ch > 127) {
                width += 10;
            } else if (std::iswupper(ch) != 0) {
                width += 8;
            } else {
                width += 7;
            }
        }
        if (bold) {
            width += static_cast<int>(text.size());
        }
        return width;
    }

#ifdef _WIN32
    HFONT createOverlayFont(bool bold, int fontHeight = kOverlayCompactFontHeight) const {
        HFONT baseFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        LOGFONTW logFont{};
        if (GetObjectW(baseFont, sizeof(logFont), &logFont) == 0) {
            logFont.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
            wcscpy_s(logFont.lfFaceName, L"Segoe UI");
        } else {
            logFont.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
        }
        logFont.lfHeight = -fontHeight;
        return CreateFontIndirectW(&logFont);
    }

    int measureTextWidth(const std::wstring& text, bool bold, HDC dc, int fontHeight) const {
        if (dc == nullptr || text.empty()) {
            return measureTextWidthApprox(text, bold);
        }

        HFONT font = createOverlayFont(bold, fontHeight);
        HGDIOBJ oldFont = SelectObject(dc, font);
        SIZE textSize{};
        if (GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &textSize) == 0) {
            textSize.cx = measureTextWidthApprox(text, bold);
        }
        SelectObject(dc, oldFont);
        DeleteObject(font);
        return textSize.cx;
    }
#endif

    int measureTextWidth(const std::wstring& text, bool bold, int fontHeight = kOverlayCompactFontHeight) const {
#ifdef _WIN32
        return measureTextWidth(text, bold, nullptr, fontHeight);
#else
        (void)fontHeight;
        return measureTextWidthApprox(text, bold);
#endif
    }

    TaskbarMetrics deriveMetrics(const TaskbarMetrics& metrics) const {
        TaskbarMetrics derived = metrics;
        if (metrics.kind == TaskbarMetricKind::PersistedActivity) {
            derived.usagePercentage = 0.0;
            derived.remainingPercentage = 0.0;
            derived.secondaryRemainingPercentage = -1.0;
            derived.secondaryPrimaryMetric.clear();
            if (derived.primaryMetric.empty()) {
                derived.primaryMetric = formatNumber(metrics.used);
            }
            if (derived.secondaryMetric.empty()) {
                derived.secondaryMetric = formatNumber(metrics.used);
            }
            derived.statusIcon = normalizeStatusIcon(derived.statusIcon);
            if (derived.statusIcon.empty()) {
                derived.statusIcon = iconForBand(TaskbarPressureBand::Green);
            }
            derived.pressureBand = mergePressureBands(
                derived.pressureBand,
                pressureBandForStateIcon(derived.statusIcon));
            derived.emphasizePrimary = false;
            return derived;
        }

        derived.remainingPercentage = deriveRemainingPercentage(metrics);
        derived.usagePercentage = (metrics.limit > 0)
            ? std::clamp(100.0 - derived.remainingPercentage, 0.0, 100.0)
            : std::clamp(metrics.usagePercentage, 0.0, 100.0);
        const long long remainingValue = deriveRemainingValue(metrics, derived.remainingPercentage);
        if (derived.primaryMetric.empty()) {
            derived.primaryMetric = formatPercentage(derived.remainingPercentage);
        }
        if (hasSecondaryRemainingPercentage(metrics) && derived.secondaryPrimaryMetric.empty()) {
            derived.secondaryPrimaryMetric =
                formatPercentage(std::clamp(metrics.secondaryRemainingPercentage, 0.0, 100.0));
        }

        if (derived.secondaryMetric.empty()) {
            if (metrics.limit > 0) {
                derived.secondaryMetric = formatNumber(remainingValue) + " left";
            } else {
                derived.secondaryMetric = formatNumber(remainingValue) + " left";
            }
        }

        derived.pressureBand = (metrics.limit > 0)
            ? pressureBandForRemaining(derived.remainingPercentage)
            : TaskbarPressureBand::Neutral;
        derived.secondaryPressureBand = hasSecondaryRemainingPercentage(metrics)
            ? pressureBandForRemaining(metrics.secondaryRemainingPercentage)
            : TaskbarPressureBand::Neutral;
        derived.statusIcon = normalizeStatusIcon(derived.statusIcon);
        if (!derived.statusIcon.empty()) {
            derived.pressureBand = mergePressureBands(
                derived.pressureBand,
                pressureBandForStateIcon(derived.statusIcon));
        }
        derived.emphasizePrimary = metrics.limit > 0 && derived.remainingPercentage <= 10.0;
        derived.emphasizeSecondary =
            hasSecondaryRemainingPercentage(metrics) && metrics.secondaryRemainingPercentage <= 10.0;
        return derived;
    }

    OverlayLayout buildOverlayLayout(const std::vector<TaskbarMetrics>& metricsList) const {
        OverlayLayout layout;
        if (metricsList.empty()) {
            return layout;
        }

        const OverlayScaleProfile profile = overlayScaleProfileForMetrics(metricsList.size());
        const int columnCount = overlayColumnCountForMetrics(metricsList.size());
        const int rowsPerRightColumn = columnCount <= 1
            ? static_cast<int>(metricsList.size())
            : static_cast<int>((metricsList.size() + 1) / 2);
        layout.lineHeight = profile.lineHeight;
        layout.columnCount = columnCount;
        layout.rows.reserve(metricsList.size());
        std::vector<int> columnLabelWidths(static_cast<size_t>(columnCount), 0);
        std::vector<int> columnWidths(static_cast<size_t>(columnCount), 0);

        for (const auto& metric : metricsList) {
            OverlayRowLayout row;
            row.pressureBand = metric.pressureBand;
            row.secondaryPressureBand = metric.secondaryPressureBand;
            row.emphasizePrimary = metric.emphasizePrimary;
            row.emphasizeSecondary = metric.emphasizeSecondary;

            std::wstring provider = widenUtf8(metric.providerDisplayName);
            if (provider.empty()) {
                provider = widenUtf8(metric.providerKey);
            }
            if (metric.kind == TaskbarMetricKind::PersistedActivity && !metric.summaryLabel.empty()) {
                provider = widenUtf8(metric.summaryLabel);
            } else if (metric.kind == TaskbarMetricKind::QuotaUsage && !provider.empty()) {
                provider += L":";
            }

            row.label = provider;
            row.primary = widenUtf8(metric.primaryMetric);
            row.secondaryPrimary = widenUtf8(metric.secondaryPrimaryMetric);
            if (metric.kind == TaskbarMetricKind::PersistedActivity) {
                row.icon = widenUtf8(metric.statusIcon);
            }
            layout.rows.push_back(row);
        }

        int widestLine = 0;
        for (size_t index = 0; index < layout.rows.size(); ++index) {
            auto& row = layout.rows[index];
            row.columnIndex = columnCount <= 1
                ? 0
                : (static_cast<int>(index) < rowsPerRightColumn ? 1 : 0);
            row.rowInColumn = columnCount <= 1
                ? static_cast<int>(index)
                : (row.columnIndex == 1 ? static_cast<int>(index)
                                        : static_cast<int>(index) - rowsPerRightColumn);

            const int labelWidth = measureTextWidth(row.label, false, profile.fontHeight);
            const int primaryWidth = measureTextWidth(row.primary, row.emphasizePrimary, profile.fontHeight);
            const int iconWidth = measureTextWidth(row.icon, false, profile.fontHeight);

            columnLabelWidths[static_cast<size_t>(row.columnIndex)] = std::max(
                columnLabelWidths[static_cast<size_t>(row.columnIndex)],
                labelWidth);
            layout.labelColumnWidth = std::max(layout.labelColumnWidth, labelWidth);
            layout.primaryColumnWidth = std::max(layout.primaryColumnWidth, primaryWidth);
            layout.iconColumnWidth = std::max(layout.iconColumnWidth, iconWidth);
        }

        if (columnCount == 2) {
            for (size_t index = 0; index < layout.rows.size(); ++index) {
                const auto& row = layout.rows[index];
                const int labelColumnWidth = columnLabelWidths[static_cast<size_t>(row.columnIndex)];
                const int primaryWidth = measureTextWidth(row.primary, row.emphasizePrimary, profile.fontHeight);
                const int secondaryWidth = measureTextWidth(row.secondaryPrimary, row.emphasizeSecondary, profile.fontHeight);
                const int iconWidth = measureTextWidth(row.icon, false, profile.fontHeight);

                int rowWidth = labelColumnWidth;
                if (!row.primary.empty()) {
                    rowWidth += kOverlayColumnGap + primaryWidth;
                }
                if (!row.secondaryPrimary.empty()) {
                    rowWidth += 8 + measureTextWidth(L"|", false, profile.fontHeight) + secondaryWidth;
                }
                if (!row.icon.empty()) {
                    rowWidth += kOverlayColumnGap + iconWidth;
                }
                columnWidths[static_cast<size_t>(row.columnIndex)] = std::max(
                    columnWidths[static_cast<size_t>(row.columnIndex)],
                    rowWidth);
            }
        }

        std::vector<int> columnStartX(static_cast<size_t>(columnCount), kOverlayHorizontalPadding);
        if (columnCount == 2) {
            columnStartX[1] = kOverlayHorizontalPadding + columnWidths[0] + kOverlayGroupGap;
        }

        for (size_t index = 0; index < layout.rows.size(); ++index) {
            auto& row = layout.rows[index];
            const int columnIndex = row.columnIndex;
            const int columnStart = columnStartX[static_cast<size_t>(columnIndex)];
            const int labelColumnWidth = columnLabelWidths[static_cast<size_t>(columnIndex)];
            const int primaryWidth = measureTextWidth(row.primary, row.emphasizePrimary, profile.fontHeight);
            const int secondaryWidth = measureTextWidth(row.secondaryPrimary, row.emphasizeSecondary, profile.fontHeight);
            const int iconWidth = measureTextWidth(row.icon, false, profile.fontHeight);

            row.labelX = columnStart;
            row.primaryX = columnStart + labelColumnWidth + kOverlayColumnGap;
            row.y = profile.verticalPadding +
                row.rowInColumn * (profile.lineHeight + profile.rowGap);
            row.lineWidth = labelColumnWidth;
            if (!row.primary.empty()) {
                row.lineWidth += kOverlayColumnGap + primaryWidth;
            }
            row.separatorX = row.primaryX + primaryWidth + 4;
            row.secondaryX = row.separatorX + measureTextWidth(L"|", false, profile.fontHeight) + 4;
            if (!row.secondaryPrimary.empty()) {
                row.lineWidth += 8 + measureTextWidth(L"|", false, profile.fontHeight) + secondaryWidth;
            }
            if (!row.icon.empty()) {
                row.iconX = row.secondaryPrimary.empty()
                    ? (row.primaryX + primaryWidth + kOverlayColumnGap)
                    : (row.secondaryX + secondaryWidth + kOverlayColumnGap);
                row.lineWidth += kOverlayColumnGap + iconWidth;
            }
            columnWidths[static_cast<size_t>(columnIndex)] = std::max(
                columnWidths[static_cast<size_t>(columnIndex)],
                row.lineWidth);
            row.hitWidth = columnWidths[static_cast<size_t>(columnIndex)];
        }

        for (auto& row : layout.rows) {
            row.hitWidth = columnWidths[static_cast<size_t>(row.columnIndex)];
        }

        for (const int columnWidth : columnWidths) {
            if (columnWidth > 0) {
                widestLine += columnWidth;
            }
        }
        if (columnCount == 2 && columnWidths[0] > 0 && columnWidths[1] > 0) {
            widestLine += kOverlayGroupGap;
        }

        layout.width = std::max(
            kOverlayMinimumWidth,
            kOverlayHorizontalPadding + widestLine + kOverlayRightPadding);
        layout.height = std::max(
            kOverlayMinimumHeight,
            (profile.verticalPadding * 2) +
                (std::max(0, columnCount <= 1
                    ? static_cast<int>(layout.rows.size())
                    : rowsPerRightColumn) * profile.lineHeight) +
                (std::max(0, (columnCount <= 1
                    ? static_cast<int>(layout.rows.size())
                    : rowsPerRightColumn) - 1) * profile.rowGap));
        return layout;
    }

#ifdef _WIN32
    OverlayLayout buildOverlayLayout(const std::vector<TaskbarMetrics>& metricsList, HDC dc) const {
        if (dc == nullptr) {
            return buildOverlayLayout(metricsList);
        }

        OverlayLayout layout;
        if (metricsList.empty()) {
            return layout;
        }

        const OverlayScaleProfile profile = overlayScaleProfileForMetrics(metricsList.size());
        const int columnCount = overlayColumnCountForMetrics(metricsList.size());
        const int rowsPerRightColumn = columnCount <= 1
            ? static_cast<int>(metricsList.size())
            : static_cast<int>((metricsList.size() + 1) / 2);
        layout.lineHeight = profile.lineHeight;
        layout.columnCount = columnCount;
        layout.rows.reserve(metricsList.size());
        std::vector<int> columnLabelWidths(static_cast<size_t>(columnCount), 0);
        std::vector<int> columnWidths(static_cast<size_t>(columnCount), 0);
        for (const auto& metric : metricsList) {
            OverlayRowLayout row;
            row.pressureBand = metric.pressureBand;
            row.secondaryPressureBand = metric.secondaryPressureBand;
            row.emphasizePrimary = metric.emphasizePrimary;
            row.emphasizeSecondary = metric.emphasizeSecondary;

            std::wstring provider = widenUtf8(metric.providerDisplayName);
            if (provider.empty()) {
                provider = widenUtf8(metric.providerKey);
            }
            if (metric.kind == TaskbarMetricKind::PersistedActivity && !metric.summaryLabel.empty()) {
                provider = widenUtf8(metric.summaryLabel);
            } else if (metric.kind == TaskbarMetricKind::QuotaUsage && !provider.empty()) {
                provider += L":";
            }

            row.label = provider;
            row.primary = widenUtf8(metric.primaryMetric);
            row.secondaryPrimary = widenUtf8(metric.secondaryPrimaryMetric);
            if (metric.kind == TaskbarMetricKind::PersistedActivity) {
                row.icon = widenUtf8(metric.statusIcon);
            }
            row.columnIndex = columnCount <= 1
                ? 0
                : (static_cast<int>(layout.rows.size()) < rowsPerRightColumn ? 1 : 0);
            row.rowInColumn = columnCount <= 1
                ? static_cast<int>(layout.rows.size())
                : (row.columnIndex == 1 ? static_cast<int>(layout.rows.size())
                                        : static_cast<int>(layout.rows.size()) - rowsPerRightColumn);

            const int labelWidth = measureTextWidth(row.label, false, dc, profile.fontHeight);
            const int primaryWidth = measureTextWidth(row.primary, row.emphasizePrimary, dc, profile.fontHeight);
            const int iconWidth = measureTextWidth(row.icon, false, dc, profile.fontHeight);

            columnLabelWidths[static_cast<size_t>(row.columnIndex)] = std::max(
                columnLabelWidths[static_cast<size_t>(row.columnIndex)],
                labelWidth);
            layout.labelColumnWidth = std::max(layout.labelColumnWidth, labelWidth);
            layout.primaryColumnWidth = std::max(layout.primaryColumnWidth, primaryWidth);
            layout.iconColumnWidth = std::max(layout.iconColumnWidth, iconWidth);
            layout.rows.push_back(row);
        }

        std::vector<int> columnStartX(static_cast<size_t>(columnCount), kOverlayHorizontalPadding);
        int widestLine = 0;
        if (columnCount == 2) {
            for (size_t index = 0; index < layout.rows.size(); ++index) {
                auto& row = layout.rows[index];
                const int labelColumnWidth = columnLabelWidths[static_cast<size_t>(row.columnIndex)];
                const int primaryWidth = measureTextWidth(row.primary, row.emphasizePrimary, dc, profile.fontHeight);
                const int secondaryWidth = measureTextWidth(row.secondaryPrimary, row.emphasizeSecondary, dc, profile.fontHeight);
                const int iconWidth = measureTextWidth(row.icon, false, dc, profile.fontHeight);

                int rowWidth = labelColumnWidth;
                if (!row.primary.empty()) {
                    rowWidth += kOverlayColumnGap + primaryWidth;
                }
                if (!row.secondaryPrimary.empty()) {
                    rowWidth += 8 + measureTextWidth(L"|", false, dc, profile.fontHeight) + secondaryWidth;
                }
                if (!row.icon.empty()) {
                    rowWidth += kOverlayColumnGap + iconWidth;
                }
                columnWidths[static_cast<size_t>(row.columnIndex)] = std::max(
                    columnWidths[static_cast<size_t>(row.columnIndex)],
                    rowWidth);
            }
            columnStartX[1] = kOverlayHorizontalPadding + columnWidths[0] + kOverlayGroupGap;
        }

        for (size_t index = 0; index < layout.rows.size(); ++index) {
            auto& row = layout.rows[index];
            const int columnIndex = row.columnIndex;
            const int columnStart = columnStartX[static_cast<size_t>(columnIndex)];
            const int labelColumnWidth = columnLabelWidths[static_cast<size_t>(columnIndex)];
            const int primaryWidth = measureTextWidth(row.primary, row.emphasizePrimary, dc, profile.fontHeight);
            const int secondaryWidth = measureTextWidth(row.secondaryPrimary, row.emphasizeSecondary, dc, profile.fontHeight);
            const int iconWidth = measureTextWidth(row.icon, false, dc, profile.fontHeight);

            row.labelX = columnStart;
            row.primaryX = columnStart + labelColumnWidth + kOverlayColumnGap;
            row.y = profile.verticalPadding +
                row.rowInColumn * (profile.lineHeight + profile.rowGap);
            row.lineWidth = labelColumnWidth;
            if (!row.primary.empty()) {
                row.lineWidth += kOverlayColumnGap + primaryWidth;
            }
            row.separatorX = row.primaryX + primaryWidth + 4;
            row.secondaryX = row.separatorX + measureTextWidth(L"|", false, dc, profile.fontHeight) + 4;
            if (!row.secondaryPrimary.empty()) {
                row.lineWidth += 8 + measureTextWidth(L"|", false, dc, profile.fontHeight) + secondaryWidth;
            }
            if (!row.icon.empty()) {
                row.iconX = row.secondaryPrimary.empty()
                    ? (row.primaryX + primaryWidth + kOverlayColumnGap)
                    : (row.secondaryX + secondaryWidth + kOverlayColumnGap);
                row.lineWidth += kOverlayColumnGap + iconWidth;
            }
            columnWidths[static_cast<size_t>(columnIndex)] = std::max(
                columnWidths[static_cast<size_t>(columnIndex)],
                row.lineWidth);
        }

        for (auto& row : layout.rows) {
            row.hitWidth = columnWidths[static_cast<size_t>(row.columnIndex)];
        }
        for (const int columnWidth : columnWidths) {
            if (columnWidth > 0) {
                widestLine += columnWidth;
            }
        }
        if (columnCount == 2 && columnWidths[0] > 0 && columnWidths[1] > 0) {
            widestLine += kOverlayGroupGap;
        }
        layout.width = std::max(
            kOverlayMinimumWidth,
            kOverlayHorizontalPadding + widestLine + kOverlayRightPadding);
        layout.height = std::max(
            kOverlayMinimumHeight,
            (profile.verticalPadding * 2) +
                (std::max(0, columnCount <= 1
                    ? static_cast<int>(layout.rows.size())
                    : rowsPerRightColumn) * profile.lineHeight) +
                (std::max(0, (columnCount <= 1
                    ? static_cast<int>(layout.rows.size())
                    : rowsPerRightColumn) - 1) * profile.rowGap));
        return layout;
    }
#endif

    std::wstring buildDisplayText(const OverlayLayout& layout) const {
        if (layout.rows.empty()) {
            return L"JiStats";
        }

        std::wostringstream stream;
        for (size_t index = 0; index < layout.rows.size(); ++index) {
            const auto& row = layout.rows[index];
            if (index > 0) {
                stream << L"\r\n";
            }
            stream << row.label;
            if (!row.primary.empty()) {
                stream << L" " << row.primary;
            }
            if (!row.secondaryPrimary.empty()) {
                stream << L" | " << row.secondaryPrimary;
            }
            if (!row.icon.empty()) {
                stream << L" " << row.icon;
            }
        }
        return stream.str();
    }

    std::wstring buildDisplayTextLocked() const {
        if (currentMetrics.empty()) {
            return L"JiStats";
        }
        return buildDisplayText(buildOverlayLayout(currentMetrics));
    }

    bool shouldSuppressTooltipInteractions() const {
        return rightButtonCaptured || contextMenuOpen;
    }

    static bool shouldCaptureOverlayRightClick(bool menuOpen, bool insideSurface) {
        return !menuOpen && insideSurface;
    }

    static bool shouldRecoverOverlayAfterTaskbarClick(
        bool menuOpen,
        bool insideSurface,
        bool insideTaskbar) {
        return !menuOpen && !insideSurface && insideTaskbar;
    }

    bool shouldPreferTopmostSurface() const {
        return !embedded;
    }

    void hideTooltipWindow() {
#ifdef _WIN32
        if (tooltipWindow != nullptr) {
            ShowWindow(tooltipWindow, SW_HIDE);
        }
        tooltipVisible = false;
        hoveredRowIndex = -1;
        lastTooltipAnchor = POINT{std::numeric_limits<LONG>::min(), std::numeric_limits<LONG>::min()};
#endif
    }

    static const wchar_t* tooltipWindowClassName() {
        return L"JiStatsTaskbarTooltipWindow";
    }

    void ensureTooltipWindow() {
#ifdef _WIN32
        if (tooltipWindow != nullptr) {
            return;
        }

        tooltipWindow = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            tooltipWindowClassName(),
            L"",
            WS_POPUP,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            0,
            0,
            nullptr,
            nullptr,
            GetModuleHandleW(nullptr),
            this);

        if (tooltipWindow == nullptr) {
            return;
        }
#endif
    }

#ifdef _WIN32
    HWND preferredSurfaceInsertAfter() const {
        if (embedded) {
            return nullptr;
        }
        return shouldPreferTopmostSurface() ? HWND_TOPMOST : HWND_NOTOPMOST;
    }

    void applySurfaceZOrder(bool showWindow) {
        if (surfaceWindow == nullptr) {
            return;
        }

        if (embedded) {
            if (showWindow) {
                ShowWindow(surfaceWindow, SW_SHOWNA);
            }
            return;
        }

        UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
        if (showWindow) {
            flags |= SWP_SHOWWINDOW;
        }

        SetWindowPos(surfaceWindow, preferredSurfaceInsertAfter(), 0, 0, 0, 0, flags);
    }
#endif

    void beginContextMenuInteraction() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            contextMenuOpen = true;
        }
        rightButtonCaptured = false;
        hideTooltipWindow();
    }

    void endContextMenuInteraction() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            contextMenuOpen = false;
        }
        rightButtonCaptured = false;
#ifdef _WIN32
        if (surfaceWindow != nullptr) {
            updateSurfaceWindow();
            ensureSurfaceVisible();
        }
#endif
    }

    SIZE measureTooltipSize(const std::wstring& text) const {
#ifdef _WIN32
        if (text.empty()) {
            return SIZE{
                kTooltipMaxWidth,
                kOverlayCompactLineHeight + (kTooltipVerticalPadding * 2)
            };
        }

        const HWND measureWindow = controllerWindow != nullptr ? controllerWindow : surfaceWindow;
        HDC dc = measureWindow != nullptr ? GetDC(measureWindow) : nullptr;
        if (dc == nullptr) {
            return SIZE{
                kTooltipMaxWidth,
                (kOverlayCompactLineHeight * 2) + (kTooltipVerticalPadding * 2)
            };
        }

        HFONT font = createOverlayFont(false, kTooltipFontHeight);
        HFONT previousFont = static_cast<HFONT>(SelectObject(dc, font));
        RECT rect{
            0,
            0,
            kTooltipMaxWidth - (kTooltipHorizontalPadding * 2),
            0
        };
        DrawTextW(
            dc,
            text.c_str(),
            -1,
            &rect,
            DT_CALCRECT | DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, previousFont);
        DeleteObject(font);
        ReleaseDC(measureWindow, dc);

        return SIZE{
            rect.right - rect.left + (kTooltipHorizontalPadding * 2),
            rect.bottom - rect.top + (kTooltipVerticalPadding * 2)
        };
#else
        static_cast<void>(text);
        return SIZE{0, 0};
#endif
    }

    void updateHoverTooltipForScreenPoint(POINT cursor) {
#ifdef _WIN32
        const HWND hoverWindow = interactionWindow();
        if (stopRequested || hoverWindow == nullptr) {
            hideTooltipWindow();
            return;
        }

        bool enabled = true;
        {
            std::lock_guard<std::mutex> lock(mutex);
            enabled = overlayEnabled;
        }
        if (!enabled || !IsWindowVisible(hoverWindow)) {
            hideTooltipWindow();
            return;
        }

        RECT surfaceRect{};
        if (GetWindowRect(hoverWindow, &surfaceRect) == 0) {
            hideTooltipWindow();
            return;
        }

        if (!PtInRect(&surfaceRect, cursor)) {
            hideTooltipWindow();
            return;
        }

        POINT clientPoint = cursor;
        ScreenToClient(hoverWindow, &clientPoint);

        int rowIndex = -1;
        std::wstring text;
        bool suppressTooltip = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            suppressTooltip = contextMenuOpen;
            for (size_t index = 0; index < overlayLayout.rows.size() && index < currentMetrics.size(); ++index) {
                const auto& row = overlayLayout.rows[index];
                if (clientPoint.y >= row.y &&
                    clientPoint.y < row.y + overlayLayout.lineHeight &&
                    clientPoint.x >= row.labelX &&
                    clientPoint.x < row.labelX + row.hitWidth) {
                    rowIndex = static_cast<int>(index);
                    break;
                }
            }

            if (rowIndex >= 0) {
                text = buildHoverTooltipText(currentMetrics, static_cast<size_t>(rowIndex));
            }
        }

        if (suppressTooltip) {
            hideTooltipWindow();
            return;
        }

        if (rowIndex < 0 || text.empty()) {
            hideTooltipWindow();
            return;
        }

        ensureTooltipWindow();
        if (tooltipWindow == nullptr) {
            return;
        }

        RECT taskbarRect{};
        if (taskbarWindow == nullptr || GetWindowRect(taskbarWindow, &taskbarRect) == 0) {
            taskbarRect = surfaceRect;
        }

        const SIZE bubbleSize = measureTooltipSize(text);
        const POINT anchor = computeTooltipAnchor(surfaceRect, taskbarRect, cursor, bubbleSize);
        const bool textChanged = tooltipText != text;
        const bool anchorChanged = anchor.x != lastTooltipAnchor.x || anchor.y != lastTooltipAnchor.y;
        const bool sizeChanged = bubbleSize.cx != lastTooltipSize.cx || bubbleSize.cy != lastTooltipSize.cy;
        tooltipText = text;

        if (!tooltipVisible || hoveredRowIndex != rowIndex || textChanged || anchorChanged || sizeChanged) {
            SetWindowPos(
                tooltipWindow,
                HWND_TOPMOST,
                anchor.x,
                anchor.y,
                bubbleSize.cx,
                bubbleSize.cy,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
            InvalidateRect(tooltipWindow, nullptr, TRUE);
        }

        lastTooltipAnchor = anchor;
        lastTooltipSize = bubbleSize;
        tooltipVisible = true;
        hoveredRowIndex = rowIndex;
#endif
    }

    void ensureSurfaceVisible() {
#ifdef _WIN32
        if (stopRequested || surfaceWindow == nullptr) {
            return;
        }

        bool enabled = true;
        {
            std::lock_guard<std::mutex> lock(mutex);
            enabled = overlayEnabled;
        }
        if (!enabled) {
            ShowWindow(surfaceWindow, SW_HIDE);
            return;
        }

        applySurfaceZOrder(true);
#endif
    }

#ifdef _WIN32
    void setHeartbeatTimerInterval(HWND controller, UINT intervalMs) {
        if (currentHeartbeatTimerIntervalMs != intervalMs &&
            SetTimer(controller, kOverlayHeartbeatTimerId, intervalMs, nullptr) != 0) {
            currentHeartbeatTimerIntervalMs = intervalMs;
        }
    }
#endif

#ifdef _WIN32
    COLORREF primaryColorForBand(TaskbarPressureBand band) const {
        switch (band) {
            case TaskbarPressureBand::Blue:
                return RGB(96, 170, 255);
            case TaskbarPressureBand::Green:
                return RGB(92, 214, 117);
            case TaskbarPressureBand::Yellow:
                return RGB(240, 208, 88);
            case TaskbarPressureBand::Orange:
                return RGB(240, 152, 63);
            case TaskbarPressureBand::Red:
                return RGB(232, 92, 92);
            default:
                return RGB(240, 240, 240);
        }
    }

    COLORREF labelColor() const {
        return RGB(235, 235, 235);
    }
#endif

#ifdef _WIN32
    static const wchar_t* controllerClassName() {
        return L"JiStatsTaskbarControllerWindow";
    }

    static const wchar_t* surfaceClassName() {
        return L"JiStatsTaskbarSurfaceWindow";
    }

    HWND interactionWindow() const {
        return surfaceWindow;
    }

    bool pointInsideSurface(POINT point) {
        const HWND targetWindow = interactionWindow();
        if (targetWindow == nullptr || !IsWindowVisible(targetWindow)) {
            return false;
        }

        bool enabled = true;
        {
            std::lock_guard<std::mutex> lock(mutex);
            enabled = overlayEnabled;
        }
        if (!enabled) {
            return false;
        }

        RECT rect{};
        if (GetWindowRect(targetWindow, &rect) == 0) {
            return false;
        }
        return PtInRect(&rect, point) != FALSE;
    }

    void beginMouseLeaveTracking() {
        const HWND targetWindow = interactionWindow();
        if (targetWindow == nullptr || trackingMouseLeave) {
            return;
        }

        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = targetWindow;
        if (TrackMouseEvent(&track) != 0) {
            trackingMouseLeave = true;
        }
    }

    bool handleContextMenuCommand(UINT commandId) {
        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> lock(mutex);
            switch (commandId) {
                case kTaskbarMenuShowDashboard:
                    callback = onShowDashboardCallback;
                    break;
                case kTaskbarMenuPreferences:
                    callback = onPreferencesCallback;
                    break;
                case kTaskbarMenuRetryNow:
                    callback = offlineRetryAvailable ? onRetryNowCallback : std::function<void()>{};
                    break;
                case kTaskbarMenuDismissOfflineNotice:
                    callback = offlineDismissAvailable ? onDismissOfflineNoticeCallback : std::function<void()>{};
                    break;
                case kTaskbarMenuExit:
                    callback = onExitCallback;
                    break;
                default:
                    break;
            }
        }

        if (!callback) {
            return false;
        }

        callback();
        return true;
    }

    void showContextMenuAt(POINT screenPoint) {
        beginContextMenuInteraction();

        HMENU menu = CreatePopupMenu();
        if (menu == nullptr) {
            endContextMenuInteraction();
            return;
        }

        std::function<void()> onRetryNow;
        std::function<void()> onDismissOfflineNotice;
        bool showRetry = false;
        bool showDismiss = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            onRetryNow = onRetryNowCallback;
            onDismissOfflineNotice = onDismissOfflineNoticeCallback;
            showRetry = offlineRetryAvailable && static_cast<bool>(onRetryNow);
            showDismiss = offlineDismissAvailable && static_cast<bool>(onDismissOfflineNotice);
        }

        AppendMenuW(menu, MF_STRING, kTaskbarMenuShowDashboard, L"Open Main Window");
        if (showRetry) {
            AppendMenuW(menu, MF_STRING, kTaskbarMenuRetryNow, L"Retry Now");
        }
        if (showDismiss) {
            AppendMenuW(menu, MF_STRING, kTaskbarMenuDismissOfflineNotice, L"Dismiss Offline Notice");
        }
        AppendMenuW(menu, MF_STRING, kTaskbarMenuPreferences, L"Preferences...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kTaskbarMenuExit, L"Exit");

        HWND foregroundWindow = controllerWindow != nullptr ? controllerWindow : surfaceWindow;
        if (foregroundWindow != nullptr) {
            SetForegroundWindow(foregroundWindow);
        }

        const UINT command = TrackPopupMenu(
            menu,
            TPM_BOTTOMALIGN | TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
            screenPoint.x,
            screenPoint.y,
            0,
            foregroundWindow,
            nullptr);
        if (command != 0) {
            handleContextMenuCommand(command);
        }
        if (foregroundWindow != nullptr) {
            PostMessageW(foregroundWindow, WM_NULL, 0, 0);
        }

        DestroyMenu(menu);
        endContextMenuInteraction();
    }

    void drawOverlayContent(HDC dc, const RECT& rect, const OverlayLayout& layout, std::wstring text) {
        SetBkMode(dc, TRANSPARENT);
        const int overlayFontHeight =
            layout.lineHeight <= kOverlayCompactLineHeight ? kOverlayCompactFontHeight : kOverlayRelaxedFontHeight;
        HFONT regularFont = createOverlayFont(false, overlayFontHeight);
        HFONT boldFont = createOverlayFont(true, overlayFontHeight);

        if (!layout.rows.empty()) {
            for (const auto& row : layout.rows) {
                RECT labelRect{row.labelX, row.y, rect.right - kOverlayHorizontalPadding, row.y + layout.lineHeight};
                SelectObject(dc, regularFont);
                SetTextColor(dc, labelColor());
                DrawTextW(dc, row.label.c_str(), -1, &labelRect,
                    DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

                if (!row.primary.empty()) {
                    RECT primaryRect{row.primaryX, row.y, rect.right - kOverlayHorizontalPadding, row.y + layout.lineHeight};
                    SelectObject(dc, row.emphasizePrimary ? boldFont : regularFont);
                    SetTextColor(dc, primaryColorForBand(row.pressureBand));
                    DrawTextW(dc, row.primary.c_str(), -1, &primaryRect,
                        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                }

                if (!row.secondaryPrimary.empty()) {
                    RECT separatorRect{row.separatorX, row.y, rect.right - kOverlayHorizontalPadding, row.y + layout.lineHeight};
                    SelectObject(dc, regularFont);
                    SetTextColor(dc, labelColor());
                    DrawTextW(dc, L"|", -1, &separatorRect,
                        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

                    RECT secondaryRect{row.secondaryX, row.y, rect.right - kOverlayHorizontalPadding, row.y + layout.lineHeight};
                    SelectObject(dc, row.emphasizeSecondary ? boldFont : regularFont);
                    SetTextColor(dc, primaryColorForBand(row.secondaryPressureBand));
                    DrawTextW(dc, row.secondaryPrimary.c_str(), -1, &secondaryRect,
                        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                }

                if (!row.icon.empty()) {
                    RECT iconRect{row.iconX, row.y, rect.right - kOverlayHorizontalPadding, row.y + layout.lineHeight};
                    SelectObject(dc, regularFont);
                    SetTextColor(dc, primaryColorForBand(row.pressureBand));
                    DrawTextW(dc, row.icon.c_str(), -1, &iconRect,
                        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                }
            }
        } else {
            if (text.empty()) {
                text = L"JiStats";
            }

            SelectObject(dc, regularFont);
            SetTextColor(dc, textColor);
            RECT textRect = rect;
            textRect.left += kOverlayHorizontalPadding;
            DrawTextW(dc, text.c_str(), -1, &textRect,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        }

        DeleteObject(regularFont);
        DeleteObject(boldFont);
    }

    bool renderSurfaceLayered(const OverlayLayout& layout, const std::wstring& text, int width, int height) {
        if (surfaceWindow == nullptr || width <= 0 || height <= 0) {
            return false;
        }

        HDC screenDc = GetDC(nullptr);
        if (screenDc == nullptr) {
            return false;
        }

        HDC memoryDc = CreateCompatibleDC(screenDc);
        if (memoryDc == nullptr) {
            ReleaseDC(nullptr, screenDc);
            return false;
        }

        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
        bitmapInfo.bmiHeader.biWidth = width;
        bitmapInfo.bmiHeader.biHeight = -height;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        void* bitmapBits = nullptr;
        HBITMAP bitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &bitmapBits, nullptr, 0);
        if (bitmap == nullptr || bitmapBits == nullptr) {
            if (bitmap != nullptr) {
                DeleteObject(bitmap);
            }
            DeleteDC(memoryDc);
            ReleaseDC(nullptr, screenDc);
            return false;
        }

        HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
        RECT rect{0, 0, width, height};
        HBRUSH background = CreateSolidBrush(kOverlayLayeredBackgroundColor);
        FillRect(memoryDc, &rect, background);
        DeleteObject(background);

        drawOverlayContent(memoryDc, rect, layout, text);

        const std::uint32_t backgroundBgr =
            static_cast<std::uint32_t>(kOverlayLayeredBackgroundColor) & 0x00ffffffu;
        auto* pixels = static_cast<std::uint32_t*>(bitmapBits);
        const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
        for (size_t index = 0; index < pixelCount; ++index) {
            const std::uint32_t color = pixels[index] & 0x00ffffffu;
            const std::uint32_t alpha = (color == backgroundBgr)
                ? static_cast<std::uint32_t>(kOverlayLayeredBackgroundAlpha)
                : 0xffu;
            pixels[index] = color | (alpha << 24);
        }

        POINT source{0, 0};
        SIZE size{width, height};
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;

        UPDATELAYEREDWINDOWINFO updateInfo{};
        updateInfo.cbSize = sizeof(updateInfo);
        updateInfo.psize = &size;
        updateInfo.hdcSrc = memoryDc;
        updateInfo.pptSrc = &source;
        updateInfo.pblend = &blend;
        updateInfo.dwFlags = ULW_ALPHA;

        BOOL updateSucceeded = UpdateLayeredWindowIndirect(surfaceWindow, &updateInfo);
        if (updateSucceeded == FALSE) {
            updateSucceeded = UpdateLayeredWindow(
                surfaceWindow,
                screenDc,
                nullptr,
                &size,
                memoryDc,
                &source,
                0,
                &blend,
                ULW_ALPHA);
        }

        SelectObject(memoryDc, previousBitmap);
        DeleteObject(bitmap);
        DeleteDC(memoryDc);
        ReleaseDC(nullptr, screenDc);
        if (updateSucceeded == FALSE) {
            const DWORD error = GetLastError();
            JISTATS_LOG_WARN(
                LogCategory::Taskbar,
                "UpdateLayeredWindow failed for embedded taskbar surface; error=" + std::to_string(error));
        }
        return updateSucceeded != FALSE;
    }

    static std::wstring utf8ToWide(const std::string& value) {
        if (value.empty()) {
            return L"";
        }

        const int count = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
        if (count <= 0) {
            return std::wstring(value.begin(), value.end());
        }

        std::wstring wide(static_cast<size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, wide.data(), count);
        wide.resize(static_cast<size_t>(count - 1));
        return wide;
    }

    static bool shouldSkipBlockingRect(const PlacementRect& childRect,
                                       const PlacementRect& taskbarClientRect) {
        const int childWidth = rectWidth(childRect);
        const int childHeight = rectHeight(childRect);
        if (childWidth <= 0 || childHeight <= 0) {
            return true;
        }

        const int taskbarWidth = rectWidth(taskbarClientRect);
        if (childRect.right <= taskbarClientRect.left ||
            childRect.left >= taskbarClientRect.right) {
            return true;
        }

        // Ignore giant container windows that represent the general task list area.
        if (childWidth >= (taskbarWidth * 3) / 4 &&
            childRect.left <= taskbarClientRect.left + (taskbarWidth / 4)) {
            return true;
        }

        return false;
    }

    static int shiftLeftUntilClearAgainstRects(int proposedX,
                                               int y,
                                               int width,
                                               int height,
                                               const PlacementRect& taskbarClientRect,
                                               const std::vector<PlacementRect>& blockingRects) {
        PlacementRect candidate{proposedX, y, proposedX + width, y + height};
        for (int attempt = 0; attempt < 16; ++attempt) {
            bool moved = false;
            for (const auto& childRect : blockingRects) {
                if (shouldSkipBlockingRect(childRect, taskbarClientRect)) {
                    continue;
                }

                // Focus avoidance on taskbar content clustered near the notification area.
                if (childRect.left <
                    taskbarClientRect.left + (rectWidth(taskbarClientRect) / 2)) {
                    continue;
                }

                if (!rectsIntersect(candidate, childRect)) {
                    continue;
                }

                proposedX = std::max(kTaskbarMinimumX, childRect.left - width - kTaskbarBlockingGap);
                candidate.left = proposedX;
                candidate.right = proposedX + width;
                moved = true;
                break;
            }

            if (!moved) {
                break;
            }
        }

        return proposedX;
    }

    #ifdef JISTATS_ENABLE_TEST_HOOKS
    static TaskbarPresenter::PlacementResultForTesting computePlacement(
        const TaskbarPresenter::PlacementInputForTesting& input) {
        TaskbarPresenter::PlacementResultForTesting result;
        result.width = input.width;
        result.height = input.height;

        const PlacementRect taskbarRect = toPlacementRect(input.taskbarRect);
        const PlacementRect taskbarClientRect = toPlacementRect(input.taskbarClientRect);

        if (input.embedded) {
            int x = std::max(
                kTaskbarMinimumX,
                static_cast<int>(taskbarClientRect.right) - input.width - kTaskbarOuterGap);
            const int y = std::max(0, (taskbarClientRect.bottom - input.height) / 2);

            if (input.hasNotificationArea) {
                const PlacementRect notificationRect = toPlacementRect(input.notificationAreaRect);
                x = std::max(kTaskbarMinimumX, notificationRect.left - input.width - kTaskbarOuterGap);
            }

            std::vector<PlacementRect> blockingRects;
            blockingRects.reserve(input.blockingRects.size());
            for (const auto& rect : input.blockingRects) {
                blockingRects.push_back(toPlacementRect(rect));
            }
            x = shiftLeftUntilClearAgainstRects(
                x,
                y,
                input.width,
                input.height,
                taskbarClientRect,
                blockingRects);

            result.x = x;
            result.y = y;
            return result;
        }

        int x = taskbarRect.right - input.width - kTaskbarOuterGap;
        if (input.hasNotificationArea) {
            const PlacementRect notificationRect = toPlacementRect(input.notificationAreaRect);
            x = notificationRect.left - input.width - kTaskbarOuterGap;
        }

        if (input.hasTrafficMonitor) {
            const PlacementRect trafficMonitorRect = toPlacementRect(input.trafficMonitorRect);
            x = std::min(x, trafficMonitorRect.left - input.width - kTaskbarOuterGap);
        }

        result.x = std::max(kTaskbarMinimumX, x);
        result.y = taskbarRect.top +
            std::max(0, (rectHeight(taskbarRect) - input.height) / 2);
        return result;
    }
    #endif

    int shiftLeftUntilClear(int proposedX, int y, int width, int height, const RECT& taskbarClientRect) const {
        if (taskbarWindow == nullptr) {
            return proposedX;
        }

        PlacementRect clientRect{
            taskbarClientRect.left,
            taskbarClientRect.top,
            taskbarClientRect.right,
            taskbarClientRect.bottom};
        std::vector<PlacementRect> blockingRects;
        for (HWND child = GetWindow(taskbarWindow, GW_CHILD);
             child != nullptr;
             child = GetWindow(child, GW_HWNDNEXT)) {
            if (child == surfaceWindow || child == controllerWindow) {
                continue;
            }
            if (!IsWindowVisible(child)) {
                continue;
            }

            RECT childScreenRect{};
            GetWindowRect(child, &childScreenRect);

            POINT topLeft{childScreenRect.left, childScreenRect.top};
            POINT bottomRight{childScreenRect.right, childScreenRect.bottom};
            ScreenToClient(taskbarWindow, &topLeft);
            ScreenToClient(taskbarWindow, &bottomRight);

            blockingRects.push_back(PlacementRect{
                topLeft.x,
                topLeft.y,
                bottomRight.x,
                bottomRight.y});
        }

        return shiftLeftUntilClearAgainstRects(
            proposedX,
            y,
            width,
            height,
            clientRect,
            blockingRects);
    }

    static bool isLikelyTrafficMonitorWindow(HWND hwnd) {
        if (hwnd == nullptr) {
            return false;
        }

        wchar_t className[256]{};
        if (GetClassNameW(hwnd, className, static_cast<int>(sizeof(className) / sizeof(className[0]))) <= 0) {
            className[0] = L'\0';
        }
        std::wstring classNameLower = toLowerWide(className);
        if (classNameLower.find(L"trafficmonitor") != std::wstring::npos) {
            return true;
        }

        wchar_t title[256]{};
        GetWindowTextW(hwnd, title, static_cast<int>(sizeof(title) / sizeof(title[0])));
        std::wstring titleLower = toLowerWide(title);
        if (titleLower.find(L"trafficmonitor") != std::wstring::npos) {
            return true;
        }

        const std::wstring processNameLower = processImageNameLowerForWindow(hwnd);
        return processNameLower.find(L"trafficmonitor") != std::wstring::npos;
    }

    HWND findTrafficMonitorWindow() const {
        HWND trafficMonitor = FindWindowW(L"TrafficMonitor_TaskBarWnd", nullptr);
        if (trafficMonitor != nullptr && IsWindowVisible(trafficMonitor)) {
            return trafficMonitor;
        }

        struct Context {
            const Impl* self = nullptr;
            PlacementRect taskbarRect;
            bool hasTaskbarRect = false;
            HWND match = nullptr;
            int bestScore = std::numeric_limits<int>::min();
        } context{};
        context.self = this;

        RECT taskbarRect{};
        context.hasTaskbarRect = taskbarWindow != nullptr && GetWindowRect(taskbarWindow, &taskbarRect) != 0;
        if (context.hasTaskbarRect) {
            context.taskbarRect = PlacementRect{
                taskbarRect.left,
                taskbarRect.top,
                taskbarRect.right,
                taskbarRect.bottom};
        }

        if (taskbarWindow != nullptr) {
            EnumChildWindows(taskbarWindow, [](HWND hwnd, LPARAM lParam) -> BOOL {
                auto* context = reinterpret_cast<Context*>(lParam);
                if (hwnd == nullptr ||
                    hwnd == context->self->surfaceWindow ||
                    hwnd == context->self->controllerWindow ||
                    hwnd == context->self->taskbarWindow) {
                    return TRUE;
                }
                if (!IsWindowVisible(hwnd) || !isLikelyTrafficMonitorWindow(hwnd)) {
                    return TRUE;
                }

                RECT windowRect{};
                if (GetWindowRect(hwnd, &windowRect) == 0) {
                    return TRUE;
                }

                PlacementRect candidate{
                    windowRect.left,
                    windowRect.top,
                    windowRect.right,
                    windowRect.bottom};

                int score = rectWidth(candidate);
                if (context->hasTaskbarRect && rectsIntersect(candidate, context->taskbarRect)) {
                    score += 100000;
                }
                score += candidate.left;

                if (score > context->bestScore) {
                    context->bestScore = score;
                    context->match = hwnd;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&context));
        }

        if (context.match != nullptr) {
            return context.match;
        }
        if (!context.hasTaskbarRect) {
            return nullptr;
        }

        EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
            auto* context = reinterpret_cast<Context*>(lParam);
            if (hwnd == nullptr ||
                hwnd == context->self->surfaceWindow ||
                hwnd == context->self->controllerWindow ||
                hwnd == context->self->taskbarWindow) {
                return TRUE;
            }
            if (!IsWindowVisible(hwnd)) {
                return TRUE;
            }

            RECT windowRect{};
            if (GetWindowRect(hwnd, &windowRect) == 0) {
                return TRUE;
            }

            PlacementRect candidate{
                windowRect.left,
                windowRect.top,
                windowRect.right,
                windowRect.bottom};

            if (!rectsIntersect(candidate, context->taskbarRect) || !isLikelyTrafficMonitorWindow(hwnd)) {
                return TRUE;
            }
            const int score = rectWidth(candidate) + candidate.left;

            if (score > context->bestScore) {
                context->bestScore = score;
                context->match = hwnd;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&context));

        return context.match;
    }

    std::vector<PlacementRect> collectPopupBlockingRects(const PlacementRect& taskbarRect) const {
        std::vector<PlacementRect> blockingRects;

        if (taskbarWindow != nullptr) {
            for (HWND child = GetWindow(taskbarWindow, GW_CHILD);
                 child != nullptr;
                 child = GetWindow(child, GW_HWNDNEXT)) {
                if (child == surfaceWindow || child == controllerWindow) {
                    continue;
                }
                if (!IsWindowVisible(child)) {
                    continue;
                }

                RECT childRect{};
                if (GetWindowRect(child, &childRect) == 0) {
                    continue;
                }

                PlacementRect candidate{
                    childRect.left,
                    childRect.top,
                    childRect.right,
                    childRect.bottom};
                if (shouldSkipBlockingRect(candidate, taskbarRect)) {
                    continue;
                }
                if (!rectsIntersect(candidate, taskbarRect)) {
                    continue;
                }

                blockingRects.push_back(candidate);
            }
        }

        struct Context {
            const Impl* self = nullptr;
            PlacementRect taskbarRect;
            std::vector<PlacementRect>* rects = nullptr;
        } context{this, taskbarRect, nullptr};
        context.rects = &blockingRects;

        EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
            auto* context = reinterpret_cast<Context*>(lParam);
            if (hwnd == nullptr ||
                hwnd == context->self->surfaceWindow ||
                hwnd == context->self->controllerWindow ||
                hwnd == context->self->taskbarWindow) {
                return TRUE;
            }
            if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
                return TRUE;
            }

            const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
            if ((style & WS_CHILD) != 0) {
                return TRUE;
            }

            RECT windowRect{};
            if (GetWindowRect(hwnd, &windowRect) == 0) {
                return TRUE;
            }

            PlacementRect candidate{
                windowRect.left,
                windowRect.top,
                windowRect.right,
                windowRect.bottom};

            if (shouldSkipBlockingRect(candidate, context->taskbarRect)) {
                return TRUE;
            }
            if (!rectsIntersect(candidate, context->taskbarRect)) {
                return TRUE;
            }

            context->rects->push_back(candidate);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&context));

        return blockingRects;
    }

    void runUiThread() {
        HINSTANCE instance = GetModuleHandleW(nullptr);

        WNDCLASSW controllerClass{};
        controllerClass.lpfnWndProc = &Impl::controllerWindowProc;
        controllerClass.hInstance = instance;
        controllerClass.lpszClassName = controllerClassName();
        RegisterClassW(&controllerClass);

        WNDCLASSW surfaceClass{};
        surfaceClass.lpfnWndProc = &Impl::surfaceWindowProc;
        surfaceClass.hInstance = instance;
        surfaceClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        surfaceClass.hbrBackground = nullptr;
        surfaceClass.lpszClassName = surfaceClassName();
        RegisterClassW(&surfaceClass);

        WNDCLASSW tooltipClass{};
        tooltipClass.lpfnWndProc = &Impl::tooltipWindowProc;
        tooltipClass.hInstance = instance;
        tooltipClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        tooltipClass.hbrBackground = nullptr;
        tooltipClass.lpszClassName = tooltipWindowClassName();
        RegisterClassW(&tooltipClass);

        HWND hwnd = CreateWindowExW(
            0,
            controllerClassName(),
            L"JiStats Taskbar Controller",
            WS_OVERLAPPED,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            nullptr,
            nullptr,
            instance,
            this);

        {
            std::lock_guard<std::mutex> lock(mutex);
            controllerWindow = hwnd;
            threadReady = true;
            if (hwnd == nullptr) {
                lastError = "Failed to create taskbar controller window";
            }
        }
        readyCv.notify_all();

        if (hwnd == nullptr) {
            return;
        }

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        UnregisterClassW(tooltipWindowClassName(), instance);
        UnregisterClassW(surfaceClassName(), instance);
        UnregisterClassW(controllerClassName(), instance);
    }

    void requestRefresh() {
        HWND hwnd = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopRequested) {
                return;
            }
#ifdef JISTATS_ENABLE_TEST_HOOKS
            ++refreshRequestCountForTesting;
#endif
            hwnd = controllerWindow;
        }

        if (hwnd != nullptr) {
            PostMessageW(hwnd, WM_JISTATS_REFRESH, 0, 0);
        }
    }

    void reportSurfaceUnavailable(const std::string& error) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            available = false;
            if (lastError == error) {
                return;
            }
            lastError = error;
        }
        JISTATS_LOG_ERROR(LogCategory::Taskbar, error);
    }

    void ensureSurfaceWindow() {
        if (stopRequested) {
            return;
        }
        HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (taskbar == nullptr) {
            reportSurfaceUnavailable("Shell_TrayWnd not available");
            destroySurfaceWindow();
            taskbarWindow = nullptr;
            return;
        }

        if (surfaceWindow != nullptr && !IsWindow(surfaceWindow)) {
            destroySurfaceWindow();
        } else if (shouldRecreateSurface(taskbar)) {
            JISTATS_LOG_INFO(LogCategory::Taskbar, "Taskbar window changed, recreating overlay surface");
            destroySurfaceWindow();
        }

        taskbarWindow = taskbar;

        // Only create and embed a surface when the previous one is gone.
        if (surfaceWindow == nullptr) {
            HINSTANCE instance = GetModuleHandleW(nullptr);
            surfaceWindow = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, surfaceClassName(), L"", WS_POPUP, 0, 0,
                                            160, 24, nullptr, nullptr, instance, this);

            if (surfaceWindow == nullptr) {
                reportSurfaceUnavailable("Failed to create taskbar surface window");
                return;
            }
            JISTATS_LOG_INFO(LogCategory::Taskbar, "Surface window created");

            SetLastError(ERROR_SUCCESS);
            const HWND previousParent = SetParent(surfaceWindow, taskbarWindow);
            const DWORD parentError = GetLastError();
            const bool embeddedSuccessfully =
                previousParent != nullptr || parentError == ERROR_SUCCESS;

            if (embeddedSuccessfully) {
                embedded = true;
                JISTATS_LOG_INFO(
                    LogCategory::Taskbar,
                    "Embedded taskbar surface into Shell_TrayWnd using TrafficMonitor-style parenting");
            } else {
                reportSurfaceUnavailable("Taskbar embedding failed, error: " + std::to_string(parentError));
                destroySurfaceWindow();
                return;
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            available = true;
            lastError.clear();
        }
        updateSurfaceWindow();
        ensureSurfaceVisible();
    }

    void destroySurfaceWindow() {
        if (const HWND surface = std::exchange(surfaceWindow, nullptr)) {
            DestroyWindow(surface);
        }
        if (tooltipWindow != nullptr) {
            DestroyWindow(tooltipWindow);
            tooltipWindow = nullptr;
        }
        embedded = false;
        tooltipVisible = false;
        hoveredRowIndex = -1;
        trackingMouseLeave = false;
        lastSurfaceX = std::numeric_limits<int>::min();
        lastSurfaceY = std::numeric_limits<int>::min();
        lastSurfaceWidth = 0;
        lastSurfaceHeight = 0;
        lastPaintedText.clear();
        lastTooltipAnchor = POINT{std::numeric_limits<LONG>::min(), std::numeric_limits<LONG>::min()};
        lastTooltipSize = SIZE{0, 0};
    }

    void updateSurfaceWindow() {
        if (stopRequested) {
            return;
        }
        if (surfaceWindow == nullptr) {
            JISTATS_LOG_WARN(LogCategory::Taskbar, "updateSurfaceWindow: surfaceWindow is null");
            return;
        }

        bool enabled = true;
        {
            std::lock_guard<std::mutex> lock(mutex);
            enabled = overlayEnabled;
        }
        if (!enabled) {
            ShowWindow(surfaceWindow, SW_HIDE);
            hideTooltipWindow();
            return;
        }

        HDC dc = GetDC(surfaceWindow);
        OverlayLayout layout;
        std::wstring text;
        {
            std::lock_guard<std::mutex> lock(mutex);
            overlayLayout = (dc != nullptr)
                ? buildOverlayLayout(currentMetrics, dc)
                : buildOverlayLayout(currentMetrics);
            if (overlayLayout.width == 0) {
                overlayLayout.width = kOverlayMinimumWidth;
                overlayLayout.height = kOverlayMinimumHeight;
            }
            displayText = buildDisplayText(overlayLayout);
            layout = overlayLayout;
            text = displayText;
        }
        if (dc != nullptr) {
            ReleaseDC(surfaceWindow, dc);
        }

        if (text.empty()) {
            text = L"JiStats";
        }
        const int width = std::max(kOverlayMinimumWidth, layout.width);
        const int height = std::max(kOverlayMinimumHeight, layout.height);

        RECT taskbarRect{};
        GetWindowRect(taskbarWindow, &taskbarRect);

        int x = 8;
        int y = 0;

        if (embedded) {
            RECT clientRect{};
            GetClientRect(taskbarWindow, &clientRect);

            int notifyX = std::max(
                kTaskbarMinimumX,
                static_cast<int>(clientRect.right) - width + 2);
            int startHeight = std::max(0, static_cast<int>(clientRect.bottom) - static_cast<int>(clientRect.top));

            HWND notify = FindWindowExW(taskbarWindow, nullptr, L"TrayNotifyWnd", nullptr);
            if (notify != nullptr) {
                RECT notifyRect{};
                GetWindowRect(notify, &notifyRect);
                POINT topLeft{notifyRect.left, notifyRect.top};
                ScreenToClient(taskbarWindow, &topLeft);
                notifyX = static_cast<int>(topLeft.x) - width + 2;
            }

            HWND startWindow = FindWindowExW(taskbarWindow, nullptr, L"Start", nullptr);
            if (startWindow != nullptr) {
                RECT startRect{};
                GetWindowRect(startWindow, &startRect);
                POINT topLeft{startRect.left, startRect.top};
                POINT bottomRight{startRect.right, startRect.bottom};
                ScreenToClient(taskbarWindow, &topLeft);
                ScreenToClient(taskbarWindow, &bottomRight);
                startHeight = std::max(0, static_cast<int>(bottomRight.y - topLeft.y));
            }

            x = std::max(kTaskbarMinimumX, notifyX);

            HWND trafficMonitor = findTrafficMonitorWindow();
            if (trafficMonitor != nullptr && IsWindowVisible(trafficMonitor)) {
                RECT tmRect{};
                GetWindowRect(trafficMonitor, &tmRect);
                POINT topLeft{tmRect.left, tmRect.top};
                ScreenToClient(taskbarWindow, &topLeft);
                x = std::max(kTaskbarMinimumX, std::min(x, static_cast<int>(topLeft.x) - width - 2));
            }

            const int taskbarHeight = std::max(0, static_cast<int>(clientRect.bottom) - static_cast<int>(clientRect.top));
            const int effectiveStartHeight = startHeight > 0 ? startHeight : taskbarHeight;
            y = std::max(
                0,
                ((effectiveStartHeight - height) / 2) + (taskbarHeight - effectiveStartHeight));
        } else {
            // Non-embedded mode: position to the LEFT of system tray and TrafficMonitor
            // Start from the right edge of taskbar and shift left to avoid overlaps
            
            // Find TrayNotifyWnd (system tray area with clock)
            HWND trayNotify = FindWindowExW(taskbarWindow, nullptr, L"TrayNotifyWnd", nullptr);
            if (trayNotify != nullptr) {
                RECT trayRect{};
                GetWindowRect(trayNotify, &trayRect);
                x = trayRect.left - width - kTaskbarOuterGap;  // Position to the LEFT of tray
            } else {
                x = taskbarRect.right - width - kTaskbarOuterGap;
            }
            
            // Check for TrafficMonitor window and position to the LEFT of it
            HWND trafficMonitor = findTrafficMonitorWindow();
            if (trafficMonitor != nullptr && IsWindowVisible(trafficMonitor)) {
                RECT tmRect{};
                GetWindowRect(trafficMonitor, &tmRect);
                // Position to the LEFT of TrafficMonitor
                x = std::min(x, static_cast<int>(tmRect.left - width - kTaskbarOuterGap));
            }
            
            y = static_cast<int>(taskbarRect.top) +
                std::max(0, (static_cast<int>(taskbarRect.bottom - taskbarRect.top) - height) / 2);

            // Ensure we don't go off the left edge
            x = std::max(kTaskbarMinimumX, x);
        }

        const bool geometryChanged =
            x != lastSurfaceX || y != lastSurfaceY || width != lastSurfaceWidth || height != lastSurfaceHeight;
        const bool contentChanged = text != lastPaintedText;
        if (geometryChanged) {
            if (embedded) {
                MoveWindow(surfaceWindow, x, y, width, height, TRUE);
                ShowWindow(surfaceWindow, SW_SHOWNA);
                JISTATS_LOG_INFO(
                    LogCategory::Taskbar,
                    "Embedded surface bounds x=" + std::to_string(x) +
                        ", y=" + std::to_string(y) +
                        ", width=" + std::to_string(width) +
                        ", height=" + std::to_string(height));
            } else {
                SetWindowPos(
                    surfaceWindow,
                    preferredSurfaceInsertAfter(),
                    x,
                    y,
                    width,
                    height,
                    SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
            lastSurfaceX = x;
            lastSurfaceY = y;
            lastSurfaceWidth = width;
            lastSurfaceHeight = height;
        }

        if (geometryChanged || contentChanged) {
            lastPaintedText = text;
            if (!renderSurfaceLayered(layout, text, width, height)) {
                InvalidateRect(surfaceWindow, nullptr, FALSE);
            }
        }
    }

    LRESULT handleControllerMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == taskbarCreatedMessage) {
            destroySurfaceWindow();
            ensureSurfaceWindow();
            return 0;
        }

        switch (msg) {
        case WM_CREATE:
            currentHeartbeatTimerIntervalMs = 0;
            setHeartbeatTimerInterval(hwnd, kOverlayHeartbeatFastIntervalMs);
            ensureSurfaceWindow();
            return 0;
        case WM_TIMER:
            if (wParam == kOverlayHeartbeatTimerId) {
                if (!stopRequested) {
                    if (surfaceWindow == nullptr || !IsWindow(surfaceWindow) ||
                        taskbarWindow == nullptr || !IsWindow(taskbarWindow)) {
                        setHeartbeatTimerInterval(hwnd, kOverlayHeartbeatFastIntervalMs);
                        ensureSurfaceWindow();
                    } else {
                        setHeartbeatTimerInterval(hwnd, kOverlayHeartbeatSlowIntervalMs);
                        // The tray and adjacent windows may finish reattaching after the overlay.
                        updateSurfaceWindow();
                        ensureSurfaceVisible();
                    }
                }
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
        case WM_JISTATS_REFRESH:
            ensureSurfaceWindow();
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kOverlayHeartbeatTimerId);
            currentHeartbeatTimerIntervalMs = 0;
            destroySurfaceWindow();
            {
                std::lock_guard<std::mutex> lock(mutex);
                controllerWindow = nullptr;
            }
            taskbarWindow = nullptr;
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    LRESULT handleSurfaceMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_NCDESTROY && surfaceWindow == hwnd) {
            surfaceWindow = nullptr;
            destroySurfaceWindow();
            std::lock_guard<std::mutex> lock(mutex);
            available = false;
        }

        switch (msg) {
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MOUSEMOVE: {
            beginMouseLeaveTracking();
            POINT screenPoint{};
            screenPoint.x = static_cast<SHORT>(LOWORD(lParam));
            screenPoint.y = static_cast<SHORT>(HIWORD(lParam));
            ClientToScreen(hwnd, &screenPoint);
            updateHoverTooltipForScreenPoint(screenPoint);
            return 0;
        }
        case WM_MOUSELEAVE:
            trackingMouseLeave = false;
            hideTooltipWindow();
            return 0;
        case WM_RBUTTONUP: {
            POINT screenPoint{};
            screenPoint.x = static_cast<SHORT>(LOWORD(lParam));
            screenPoint.y = static_cast<SHORT>(HIWORD(lParam));
            ClientToScreen(hwnd, &screenPoint);
            showContextMenuAt(screenPoint);
            return 0;
        }
        case WM_CONTEXTMENU: {
            POINT screenPoint{
                static_cast<SHORT>(LOWORD(lParam)),
                static_cast<SHORT>(HIWORD(lParam))
            };
            if (screenPoint.x == -1 && screenPoint.y == -1) {
                RECT rect{};
                GetWindowRect(hwnd, &rect);
                screenPoint.x = rect.left;
                screenPoint.y = rect.bottom;
            }
            showContextMenuAt(screenPoint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);

            RECT rect{};
            GetClientRect(hwnd, &rect);
            HBRUSH brush = CreateSolidBrush(kOverlayLayeredBackgroundColor);
            FillRect(dc, &rect, brush);
            DeleteObject(brush);

            OverlayLayout layout;
            std::wstring text;
            {
                std::lock_guard<std::mutex> lock(mutex);
                layout = overlayLayout;
                text = displayText;
            }
            drawOverlayContent(dc, rect, layout, text);

            if (text.empty() && layout.rows.empty()) {
                text = L"JiStats";
            }
            EndPaint(hwnd, &paint);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    LRESULT handleTooltipMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);

            RECT rect{};
            GetClientRect(hwnd, &rect);
            HBRUSH background = CreateSolidBrush(RGB(36, 39, 46));
            FillRect(dc, &rect, background);
            DeleteObject(background);
            FrameRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(245, 245, 245));
            HFONT font = createOverlayFont(false, kTooltipFontHeight);
            HFONT previousFont = static_cast<HFONT>(SelectObject(dc, font));

            RECT textRect{
                kTooltipHorizontalPadding,
                kTooltipVerticalPadding,
                rect.right - kTooltipHorizontalPadding,
                rect.bottom - kTooltipVerticalPadding
            };
            DrawTextW(dc, tooltipText.c_str(), -1, &textRect,
                      DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);

            SelectObject(dc, previousFont);
            DeleteObject(font);
            EndPaint(hwnd, &paint);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    static LRESULT CALLBACK controllerWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return TRUE;
        }

        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self == nullptr) {
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        return self->handleControllerMessage(hwnd, msg, wParam, lParam);
    }

    static LRESULT CALLBACK surfaceWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return TRUE;
        }

        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self == nullptr) {
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        return self->handleSurfaceMessage(hwnd, msg, wParam, lParam);
    }

    static LRESULT CALLBACK tooltipWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return TRUE;
        }

        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self == nullptr) {
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        return self->handleTooltipMessage(hwnd, msg, wParam, lParam);
    }
#endif
};

TaskbarPresenter::TaskbarPresenter() : pImpl(std::make_unique<Impl>()) {
}

TaskbarPresenter::~TaskbarPresenter() {
    shutdown();
}

bool TaskbarPresenter::init() {
    if (pImpl->initialized) {
        return true;
    }

#ifdef _WIN32
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        pImpl->taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
        pImpl->threadReady = false;
        pImpl->stopRequested = false;
        pImpl->lastError.clear();
        pImpl->controllerWindow = nullptr;
        pImpl->surfaceWindow = nullptr;
        pImpl->taskbarWindow = nullptr;
        pImpl->available = false;
    }
    pImpl->uiThread = std::thread([this]() {
        pImpl->runUiThread();
    });

    std::unique_lock<std::mutex> lock(pImpl->mutex);
    pImpl->readyCv.wait(lock, [this]() {
        return pImpl->threadReady;
    });

    pImpl->initialized = (pImpl->controllerWindow != nullptr);
    if (!pImpl->initialized && pImpl->lastError.empty()) {
        pImpl->lastError = "Taskbar presenter initialization failed";
    }
#else
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        pImpl->available = false;
        pImpl->lastError = "Taskbar presenter is only available on Windows";
    }
#endif

        if (pImpl->initialized) {
            JISTATS_LOG_INFO(LogCategory::Taskbar, "TaskbarPresenter initialized");
        }
        return pImpl->initialized;
}

void TaskbarPresenter::shutdown() {
    if (!pImpl->initialized) {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        pImpl->currentMetrics.clear();
#ifdef _WIN32
        pImpl->displayText = L"JiStats";
        pImpl->overlayLayout = {};
#endif
        return;
    }

#ifdef _WIN32
    HWND controller = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        controller = pImpl->controllerWindow;
        pImpl->stopRequested = true;
    }

    if (controller != nullptr) {
        PostMessageW(controller, WM_CLOSE, 0, 0);
    }
    if (pImpl->uiThread.joinable()) {
        pImpl->uiThread.join();
    }
#endif

    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        pImpl->currentMetrics.clear();
        pImpl->available = false;
#ifdef _WIN32
        pImpl->displayText = L"JiStats";
        pImpl->overlayLayout = {};
        pImpl->overlayEnabled = true;
        pImpl->controllerWindow = nullptr;
        pImpl->surfaceWindow = nullptr;
        pImpl->taskbarWindow = nullptr;
        pImpl->threadReady = false;
        pImpl->stopRequested = false;
#endif
    }

    pImpl->initialized = false;
    JISTATS_LOG_INFO(LogCategory::Taskbar, "TaskbarPresenter shutdown");
}

void TaskbarPresenter::updateMetrics(const TaskbarMetrics& metrics) {
    if (!pImpl->initialized) {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        pImpl->lastError = "TaskbarPresenter not initialized";
        return;
    }

    TaskbarMetrics m = pImpl->deriveMetrics(metrics);
    
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);

        auto it = std::find_if(pImpl->currentMetrics.begin(), pImpl->currentMetrics.end(),
            [&metrics](const TaskbarMetrics& current) {
                return current.providerKey == metrics.providerKey;
            });

        if (it != pImpl->currentMetrics.end()) {
            if (taskbarMetricsEqual(*it, m)) {
                return;
            }
            *it = m;
        } else {
            pImpl->currentMetrics.push_back(m);
        }

#ifdef _WIN32
        pImpl->displayText = pImpl->buildDisplayTextLocked();
#endif
    }

#ifdef _WIN32
    pImpl->requestRefresh();
#endif
    JISTATS_LOG_DEBUG(LogCategory::Taskbar, "Updated metrics for " + metrics.providerKey);
}

void TaskbarPresenter::updateMetrics(const std::vector<TaskbarMetrics>& metricsList) {
    replaceMetrics(metricsList);
}

void TaskbarPresenter::replaceMetrics(const std::vector<TaskbarMetrics>& metricsList) {
    if (!pImpl->initialized) {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        pImpl->lastError = "TaskbarPresenter not initialized";
        return;
    }

    std::vector<TaskbarMetrics> replaced;
    replaced.reserve(metricsList.size());
    for (const auto& metrics : metricsList) {
        replaced.push_back(pImpl->deriveMetrics(metrics));
    }

    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        if (taskbarMetricListsEqual(pImpl->currentMetrics, replaced)) {
            return;
        }
        pImpl->currentMetrics = std::move(replaced);
#ifdef _WIN32
        pImpl->displayText = pImpl->buildDisplayTextLocked();
#endif
    }

#ifdef _WIN32
    pImpl->requestRefresh();
#endif
}

void TaskbarPresenter::clearMetrics() {
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        const bool alreadyCleared = pImpl->currentMetrics.empty()
#ifdef _WIN32
            && pImpl->displayText == L"JiStats"
#endif
            ;
        if (alreadyCleared) {
            return;
        }
        pImpl->currentMetrics.clear();
#ifdef _WIN32
        pImpl->displayText = L"JiStats";
        pImpl->overlayLayout = {};
#endif
    }

#ifdef _WIN32
    pImpl->requestRefresh();
#endif
}

void TaskbarPresenter::setOverlayEnabled(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        if (pImpl->overlayEnabled == enabled) {
            return;
        }
        pImpl->overlayEnabled = enabled;
    }

#ifdef _WIN32
    pImpl->requestRefresh();
#endif
}

void TaskbarPresenter::setOnShowDashboardCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->onShowDashboardCallback = std::move(callback);
}

void TaskbarPresenter::setOnPreferencesCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->onPreferencesCallback = std::move(callback);
}

void TaskbarPresenter::setOnRetryNowCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->onRetryNowCallback = std::move(callback);
}

void TaskbarPresenter::setOnDismissOfflineNoticeCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->onDismissOfflineNoticeCallback = std::move(callback);
}

void TaskbarPresenter::setOnExitCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->onExitCallback = std::move(callback);
}

void TaskbarPresenter::setOfflineMenuState(bool retryAvailable, bool dismissAvailable) {
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        pImpl->offlineRetryAvailable = retryAvailable;
        pImpl->offlineDismissAvailable = dismissAvailable;
    }

#ifdef _WIN32
    pImpl->requestRefresh();
#endif
}

void TaskbarPresenter::setProgress(double percentage, ProgressState state) {
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        if (!pImpl->initialized || !pImpl->available) {
            return;
        }
        if (!pImpl->currentMetrics.empty()) {
            auto& metric = pImpl->currentMetrics.front();
            metric.primaryMetric = pImpl->formatPercentage(percentage);
            metric.statusIcon = pImpl->normalizeStatusIcon(
                (state == ProgressState::Error)
                    ? "!"
                    : (state == ProgressState::Paused ? "~" : "+"));
            metric.pressureBand = (state == ProgressState::Error)
                ? TaskbarPressureBand::Red
                : (state == ProgressState::Paused
                    ? TaskbarPressureBand::Yellow
                    : TaskbarPressureBand::Green);
            metric.emphasizePrimary = false;
#ifdef _WIN32
            pImpl->displayText = pImpl->buildDisplayTextLocked();
#endif
        }
    }

#ifdef _WIN32
    pImpl->requestRefresh();
#endif
}

std::vector<TaskbarMetrics> TaskbarPresenter::getCurrentMetrics() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->currentMetrics;
}

bool TaskbarPresenter::isInitialized() const {
    return pImpl->initialized.load();
}

bool TaskbarPresenter::isAvailable() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->available;
}

std::string TaskbarPresenter::getLastError() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->lastError;
}

#ifdef JISTATS_ENABLE_TEST_HOOKS
TaskbarMetrics TaskbarPresenter::deriveMetricsForTesting(const TaskbarMetrics& metrics) const {
    return pImpl->deriveMetrics(metrics);
}

TaskbarPresenter::OverlayLayoutForTesting TaskbarPresenter::deriveOverlayLayoutForTesting(
    const std::vector<TaskbarMetrics>& metricsList) const {
    std::vector<TaskbarMetrics> derivedMetrics;
    derivedMetrics.reserve(metricsList.size());
    for (const auto& metric : metricsList) {
        derivedMetrics.push_back(pImpl->deriveMetrics(metric));
    }

    const OverlayLayout layout = pImpl->buildOverlayLayout(derivedMetrics);
    OverlayLayoutForTesting result;
    result.width = layout.width;
    result.height = layout.height;
    result.labelColumnWidth = layout.labelColumnWidth;
    result.primaryColumnWidth = layout.primaryColumnWidth;
    result.iconColumnWidth = layout.iconColumnWidth;
    result.lineHeight = layout.lineHeight;
    result.columnCount = layout.columnCount;
    result.transparentBackground = true;
    result.rows.reserve(layout.rows.size());
    for (size_t index = 0; index < layout.rows.size(); ++index) {
        const auto& row = layout.rows[index];
        const auto& derivedMetric = derivedMetrics[index];
        OverlayRowForTesting testingRow;
        testingRow.label = narrowWideForTesting(row.label);
        testingRow.primaryMetric = derivedMetric.primaryMetric;
        testingRow.secondaryPrimaryMetric = derivedMetric.secondaryPrimaryMetric;
        testingRow.statusIcon = derivedMetric.statusIcon;
        testingRow.pressureBand = row.pressureBand;
        testingRow.secondaryPressureBand = row.secondaryPressureBand;
        testingRow.emphasizePrimary = row.emphasizePrimary;
        testingRow.emphasizeSecondary = row.emphasizeSecondary;
        testingRow.labelX = row.labelX;
        testingRow.primaryX = row.primaryX;
        testingRow.separatorX = row.separatorX;
        testingRow.secondaryX = row.secondaryX;
        testingRow.iconX = row.iconX;
        testingRow.y = row.y;
        testingRow.lineWidth = row.lineWidth;
        testingRow.columnIndex = row.columnIndex;
        testingRow.rowInColumn = row.rowInColumn;
        result.rows.push_back(testingRow);
    }
    return result;
}

TaskbarPresenter::PlacementResultForTesting TaskbarPresenter::derivePlacementForTesting(
    const PlacementInputForTesting& input) const {
    return Impl::computePlacement(input);
}

std::string TaskbarPresenter::deriveHoverTooltipForTesting(
    const std::vector<TaskbarMetrics>& metricsList,
    size_t rowIndex) const {
    return narrowWideForTesting(pImpl->buildHoverTooltipText(metricsList, rowIndex));
}

bool TaskbarPresenter::shouldRecreateSurfaceForTesting(
    std::uintptr_t existingTaskbarToken,
    std::uintptr_t discoveredTaskbarToken,
    bool hasSurfaceWindow) const {
#ifdef _WIN32
    return hasSurfaceWindow &&
           existingTaskbarToken != 0 &&
           discoveredTaskbarToken != 0 &&
           existingTaskbarToken != discoveredTaskbarToken;
#else
    (void)existingTaskbarToken;
    (void)discoveredTaskbarToken;
    (void)hasSurfaceWindow;
    return false;
#endif
}

void TaskbarPresenter::setRuntimeStateForTesting(bool initialized, bool available) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->initialized = initialized;
    pImpl->available = available;
}

void TaskbarPresenter::setInteractionStateForTesting(bool tooltipVisible,
                                                     int hoveredRowIndex,
                                                     bool rightButtonCaptured,
                                                     bool contextMenuOpen,
                                                     bool embedded) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->tooltipVisible = tooltipVisible;
    pImpl->hoveredRowIndex = hoveredRowIndex;
    pImpl->rightButtonCaptured = rightButtonCaptured;
    pImpl->contextMenuOpen = contextMenuOpen;
    pImpl->embedded = embedded;
}

TaskbarPresenter::InteractionStateForTesting TaskbarPresenter::getInteractionStateForTesting() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    InteractionStateForTesting state;
    state.tooltipVisible = pImpl->tooltipVisible;
    state.hoveredRowIndex = pImpl->hoveredRowIndex;
    state.rightButtonCaptured = pImpl->rightButtonCaptured;
    state.contextMenuOpen = pImpl->contextMenuOpen;
    state.hoverSuppressed = pImpl->rightButtonCaptured || pImpl->contextMenuOpen;
    state.preferTopmostSurface = !pImpl->embedded;
    state.tooltipFontHeight = kTooltipFontHeight;
    return state;
}

void TaskbarPresenter::beginContextMenuInteractionForTesting() {
    pImpl->beginContextMenuInteraction();
}

void TaskbarPresenter::endContextMenuInteractionForTesting() {
    pImpl->endContextMenuInteraction();
}

bool TaskbarPresenter::shouldCaptureOverlayRightClickForTesting(bool contextMenuOpen, bool pointInsideSurface) const {
    return pImpl->shouldCaptureOverlayRightClick(contextMenuOpen, pointInsideSurface);
}

bool TaskbarPresenter::shouldRecoverOverlayAfterTaskbarClickForTesting(
    bool contextMenuOpen,
    bool pointInsideSurface,
    bool pointInsideTaskbar) const {
    return pImpl->shouldRecoverOverlayAfterTaskbarClick(
        contextMenuOpen,
        pointInsideSurface,
        pointInsideTaskbar);
}

bool TaskbarPresenter::dispatchContextMenuActionForTesting(ContextMenuActionForTesting action) {
    UINT commandId = 0;
    switch (action) {
        case ContextMenuActionForTesting::ShowDashboard:
            commandId = kTaskbarMenuShowDashboard;
            break;
        case ContextMenuActionForTesting::Preferences:
            commandId = kTaskbarMenuPreferences;
            break;
        case ContextMenuActionForTesting::RetryNow:
            commandId = kTaskbarMenuRetryNow;
            break;
        case ContextMenuActionForTesting::DismissOfflineNotice:
            commandId = kTaskbarMenuDismissOfflineNotice;
            break;
        case ContextMenuActionForTesting::Exit:
            commandId = kTaskbarMenuExit;
            break;
    }
    return pImpl->handleContextMenuCommand(commandId);
}

std::uint64_t TaskbarPresenter::getRefreshRequestCountForTesting() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->refreshRequestCountForTesting;
}
#endif

} // namespace JiStats
