#include "gui/DashboardPresenter.hpp"
#include "gui/AnalysisPresenter.hpp"
#include "gui/DesktopTheme.hpp"
#include "db/StorageService.hpp"
#include "db/SettingsService.hpp"
#include "platform/WindowsAppAssets.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <iomanip>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
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
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#endif

namespace JiStats {

namespace {

std::string wideToUtf8Local(const std::wstring& value) {
    if (value.empty()) {
        return "";
    }

    const int count = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        std::string fallback;
        fallback.reserve(value.size());
        for (const wchar_t ch : value) {
            fallback.push_back((ch >= 0 && ch <= 0x7F) ? static_cast<char>(ch) : '?');
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

    std::wstring combined = std::wstring(dateBuffer) + L" " + std::wstring(timeBuffer);
    return wideToUtf8Local(combined);
}

std::string formatUtcDateOnly(std::time_t time) {
    if (time <= 0) {
        return "";
    }

    std::tm utcTm{};
    gmtime_s(&utcTm, &time);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &utcTm);
    return buffer;
}

std::string formatNumber(long long value) {
    std::string digits = std::to_string(value);
    const size_t firstGroup = digits.size() % 3;

    std::string formatted;
    formatted.reserve(digits.size() + (digits.size() / 3));
    for (size_t index = 0; index < digits.size(); ++index) {
        if (index > 0 && (index == firstGroup || (index > firstGroup && ((index - firstGroup) % 3 == 0)))) {
            formatted.push_back(',');
        }
        formatted.push_back(digits[index]);
    }

    return formatted;
}

std::string formatOptionalNumber(long long value) {
    return value > 0 ? formatNumber(value) : "-";
}

std::string formatRate(double value) {
    if (value < 0.0) {
        value = 0.0;
    }
    std::ostringstream stream;
    if (value >= 10.0) {
        stream << formatNumber(static_cast<long long>(value + 0.5));
    } else {
        stream << std::fixed << std::setprecision(1) << value;
    }
    return stream.str();
}

std::string formatTokenPromptTotal(long long tokens, long long prompts, const char* promptSuffix = "p") {
    std::ostringstream stream;
    stream << formatNumber(std::max<long long>(0, tokens))
           << " t, "
           << formatNumber(std::max<long long>(0, prompts))
           << " " << promptSuffix;
    return stream.str();
}

const wchar_t* codexAttributionSettingsInfo(CodexAttributionSaveError saveError) {
    switch (saveError) {
        case CodexAttributionSaveError::Revocation:
            return L"Codex attribution is paused in this window. JiStats could not save Off and will retry. "
                   L"You can also clear the checkbox again.";
        case CodexAttributionSaveError::Enable:
            return L"JiStats could not enable Codex model attribution. Select the checkbox to try again.";
        case CodexAttributionSaveError::None:
            return L"Subscription tracking reads Codex rollouts for quota and throughput. "
                   L"Model attribution reads model names and token counts from your local Codex state database.";
    }
    return L"";
}

std::string modelUsageOverviewText(const AnalysisProviderModelUsageChart& chart, bool attributionEnabled) {
    if (!chart.hasData) {
        return attributionEnabled ? "No new Codex SQLite usage observed yet. First scan establishes baseline."
                                  : "Enable Codex model attribution in File > Settings > OpenAI to start collecting.";
    }

    std::ostringstream stream;
    stream << chart.subtitle;
    const size_t count = std::min<size_t>(chart.series.size(), 6);
    for (size_t i = 0; i < count; ++i) {
        stream << "\r\n" << chart.series[i].modelKey << ": " << formatNumber(chart.series[i].totalTokens) << " tokens";
    }
    return stream.str();
}

struct CodexThroughputOverviewColumns {
    std::string recent;
    std::string averages;
    std::string totals;
};

CodexThroughputOverviewColumns codexThroughputOverviewColumns(const AnalysisCodexThroughput& throughput) {
    if (!throughput.hasData) {
        return {
            "Recent\r\nNo Codex throughput captured in the current 30-minute window yet.",
            "Averages\r\nEnable OpenAI / Codex tracking and use Codex, then refresh.",
            "Totals\r\nWaiting for local Codex rollout events."
        };
    }

    CodexThroughputOverviewColumns columns;
    std::ostringstream recent;
    recent << "Recent"
           << "\r\nLast 5m: " << formatRate(throughput.tokensPerMinute5m)
           << " t/min, " << formatRate(throughput.promptsPerMinute5m) << " p/min"
           << "\r\nLast 30m: " << formatRate(throughput.tokensPerMinute30m)
           << " t/min, " << formatRate(throughput.promptsPerMinute30m) << " p/min"
           << "\r\nLast 1h: " << formatTokenPromptTotal(throughput.tokensLast1h, throughput.promptsLast1h);
    columns.recent = recent.str();

    std::ostringstream averages;
    averages << "Averages"
             << "\r\nDaily: "
             << formatTokenPromptTotal(throughput.averageDailyTokens, throughput.averageDailyPrompts, "p/day")
             << "\r\nMonth/day: "
             << formatTokenPromptTotal(throughput.averageMonthlyTokens, throughput.averageMonthlyPrompts, "p/day")
             << "\r\nSource: "
             << (throughput.confidenceLabel.empty() ? "Unknown" : throughput.confidenceLabel);
    columns.averages = averages.str();

    std::ostringstream totals;
    totals << "Totals"
           << "\r\nCalendar month: "
           << formatTokenPromptTotal(throughput.calendarMonthTokens, throughput.calendarMonthPrompts)
           << "\r\nRolling 30d: "
           << formatTokenPromptTotal(throughput.rolling30DayTokens, throughput.rolling30DayPrompts)
           << "\r\nRates averaged over full windows";
    columns.totals = totals.str();
    return columns;
}

int parsePositiveIntegerOrDefault(const std::string& value, int fallback) {
    try {
        const int parsed = std::stoi(value);
        return parsed > 0 ? parsed : fallback;
    } catch (...) {
        return fallback;
    }
}

std::string formatPercentageLabel(double value) {
    const int rounded = static_cast<int>(std::clamp(value, 0.0, 100.0) + 0.5);
    return std::to_string(rounded) + "%";
}

int roundPercentValue(double value) {
    return static_cast<int>(std::clamp(value, 0.0, 100.0) + 0.5);
}

std::string aboutDialogVersionLabel(const DashboardViewModel& model) {
    return model.appVersion.empty() ? "unknown" : model.appVersion;
}

std::string aboutDialogSubtitleText() {
    return "Windows 11 desktop app for AI quota and usage tracking";
}

std::string buildAboutDialogBodyText(const DashboardViewModel& model) {
    std::ostringstream stream;
    stream << "Version: " << aboutDialogVersionLabel(model) << "\r\n";
    stream << "Status: Early implementation\r\n\r\n";
    stream << "Provider scope:\r\n";
    stream << "OpenAI / Codex, MiniMax, Claude Code, Kimi Code, Grok, and Cursor\r\n";
    stream << "All provider integrations are optional and disabled by default until enabled in Settings.\r\n";
    stream << "OpenAI and MiniMax ship with subscription-level support in v1.0.\r\n";
    stream << "OpenAI API-platform import and broader MiniMax PAYG support remain follow-on refinement.\r\n";
    stream << "Claude Code and Kimi Code ship as bounded local-first providers for v1.0.\r\n\r\n";
    stream << "License:\r\n";
    stream << "First-party code is Apache-2.0 with Commons Clause.\r\n";
    stream << "See LICENSE and THIRD_PARTY_NOTICES.md for details.";
    return stream.str();
}

std::string formatResetTimeLabel(std::time_t time) {
    if (time <= 0) {
        return "";
    }

    return formatSystemDateTime(time, TIME_NOSECONDS);
}

std::string formatChartBucketLabel(std::time_t time) {
    if (time <= 0) {
        return "";
    }

    return formatSystemDateTime(time, TIME_NOSECONDS);
}

bool tryParseTimestampWithFormat(const std::string& value,
                                 const char* format,
                                 std::tm& outTm) {
    std::istringstream stream(value);
    stream >> std::get_time(&outTm, format);
    return !stream.fail();
}

std::string localizeDisplayTimestamp(const std::string& value) {
    if (value.empty()) {
        return "";
    }

    std::tm utcTm{};
    if (tryParseTimestampWithFormat(value, "%Y-%m-%dT%H:%M:%SZ", utcTm)) {
        return formatSystemDateTime(_mkgmtime(&utcTm));
    }

    utcTm = {};
    if (tryParseTimestampWithFormat(value, "%Y-%m-%d %H:%M:%S UTC", utcTm)) {
        return formatSystemDateTime(_mkgmtime(&utcTm));
    }

    return value;
}

#ifdef _WIN32
bool tryGetMonitorWorkArea(HMONITOR monitor, RECT& workArea) {
    if (monitor == nullptr) {
        return false;
    }

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) == 0) {
        return false;
    }

    workArea = info.rcWork;
    return true;
}

bool tryGetMonitorWorkArea(const RECT& referenceRect, RECT& workArea) {
    return tryGetMonitorWorkArea(MonitorFromRect(&referenceRect, MONITOR_DEFAULTTONEAREST), workArea);
}

void clampRectToWorkArea(RECT& rect, const RECT& workArea) {
    const int width = std::max(0L, rect.right - rect.left);
    const int height = std::max(0L, rect.bottom - rect.top);
    const int workWidth = std::max(0L, workArea.right - workArea.left);
    const int workHeight = std::max(0L, workArea.bottom - workArea.top);

    if (width > workWidth) {
        rect.left = workArea.left;
        rect.right = workArea.right;
    } else {
        if (rect.left < workArea.left) {
            rect.left = workArea.left;
            rect.right = rect.left + width;
        }
        if (rect.right > workArea.right) {
            rect.right = workArea.right;
            rect.left = rect.right - width;
        }
    }

    if (height > workHeight) {
        rect.top = workArea.top;
        rect.bottom = workArea.bottom;
    } else {
        if (rect.top < workArea.top) {
            rect.top = workArea.top;
            rect.bottom = rect.top + height;
        }
        if (rect.bottom > workArea.bottom) {
            rect.bottom = workArea.bottom;
            rect.top = rect.bottom - height;
        }
    }
}

std::optional<RECT> centeredRectForOwner(HWND owner, int width, int height) {
    if (owner == nullptr || !IsWindow(owner) || width <= 0 || height <= 0) {
        return std::nullopt;
    }

    RECT ownerRect{};
    if (GetWindowRect(owner, &ownerRect) == 0) {
        return std::nullopt;
    }

    RECT targetRect{};
    targetRect.left = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    targetRect.top = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
    targetRect.right = targetRect.left + width;
    targetRect.bottom = targetRect.top + height;

    RECT workArea{};
    if (tryGetMonitorWorkArea(ownerRect, workArea)) {
        clampRectToWorkArea(targetRect, workArea);
    }

    return targetRect;
}
#endif

struct UsageProgressSummary {
    std::string label;
    std::string detail;
    std::string status;
    std::string overlayText;
    std::string resetText;
    int percentage = -1;
    std::string secondaryOverlayText;
    std::string secondaryResetText;
    int secondaryPercentage = -1;
};

struct ProviderQuotaRowData {
    std::vector<std::string> columns;
    std::string providerKey;
    std::string accountId;
};

struct ProviderQuotaColumnDefinition {
    const wchar_t* title;
    int width;
};

const ProviderQuotaColumnDefinition kProviderQuotaColumns[] = {
    {L"Provider", 120},
    {L"Status", 92},
    {L"Quota Surface", 150},
    {L"Reset Window", 98},
    {L"Used", 92},
    {L"Quota Limit", 96},
    {L"Remaining", 100},
    {L"Confidence", 90},
    {L"Last Sync", 170}
};

constexpr size_t kProviderQuotaColumnCount =
    sizeof(kProviderQuotaColumns) / sizeof(kProviderQuotaColumns[0]);

#ifdef _WIN32
int measureTextWidth(HWND hwnd, const std::wstring& text, HFONT font, int padding) {
    if (hwnd == nullptr) {
        return padding;
    }

    HDC dc = GetDC(hwnd);
    if (dc == nullptr) {
        return padding;
    }

    HGDIOBJ previousFont = nullptr;
    if (font != nullptr) {
        previousFont = SelectObject(dc, font);
    }

    SIZE size{};
    const BOOL measured = GetTextExtentPoint32W(
        dc,
        text.c_str(),
        static_cast<int>(text.size()),
        &size);

    if (previousFont != nullptr) {
        SelectObject(dc, previousFont);
    }
    ReleaseDC(hwnd, dc);

    return (measured ? size.cx : 0) + padding;
}
#endif

std::vector<std::string> dashboardApplicationFieldLabels() {
    return {
        "Connection",
        "Recovery",
        "Scheduled Export",
        "Last Updated"
    };
}

std::vector<std::string> providerQuotaColumnHeaders() {
    std::vector<std::string> headers;
    headers.reserve(sizeof(kProviderQuotaColumns) / sizeof(kProviderQuotaColumns[0]));
    for (const auto& column : kProviderQuotaColumns) {
        std::string header;
        for (const wchar_t ch : std::wstring(column.title)) {
            header.push_back((ch >= 0 && ch <= 0x7F) ? static_cast<char>(ch) : '?');
        }
        headers.push_back(header);
    }
    return headers;
}

std::vector<std::string> preferencesTabLabels() {
    return {"General", "OpenAI", "MiniMax", "Claude Code", "Kilo Code", "Kimi Code", "Grok / Cursor"};
}

bool isExternalSubscriptionQuota(const QuotaInfo& quota) {
    return quota.providerSurface == "grok_subscription_api" || quota.providerSurface == "cursor_subscription_api";
}

std::string normalizedQuotaPlanLabel(const ProviderStatus& provider, const QuotaInfo& quota) {
    if (quota.providerSurface == "openai_subscription_codex_local" ||
        quota.planName == "Codex Local Pressure") {
        return "Subscription";
    }
    if (quota.providerSurface == "minimax_subscription_coding_plan" ||
        quota.planName == "Coding Plan Remaining") {
        return "Coding Plan";
    }
    if (quota.providerSurface == "minimax_subscription_token_plan" ||
        quota.planName == "Token Plan Remaining") {
        return "Token Plan";
    }
    if (provider.providerKey == "openai" && quota.planName == "API Usage") {
        return "Subscription";
    }
    if (quota.providerSurface == "claude_code_local_stats") {
        return "Local Activity";
    }
    if (quota.providerSurface == "kimi_subscription_api") {
        return "Subscription";
    }
    return quota.planName;
}

std::string normalizedUsageSummaryLabel(const ProviderStatus& provider, const QuotaInfo& quota) {
    const std::string normalizedPlan = normalizedQuotaPlanLabel(provider, quota);
    if (provider.providerKey == "openai" && normalizedPlan == "Subscription") {
        return "OpenAI Subscription";
    }
    if (provider.providerKey == "kimi" && normalizedPlan == "Subscription") {
        return "Kimi Subscription";
    }
    if (isExternalSubscriptionQuota(quota)) {
        if (quota.windowType == "auto") {
            return provider.displayName + " Auto";
        }
        if (quota.windowType == "api") {
            return provider.displayName + " API";
        }
        return provider.displayName + " Subscription";
    }
    if (!normalizedPlan.empty()) {
        return provider.displayName + " - " + normalizedPlan;
    }
    return provider.displayName;
}

bool isOpenAISubscriptionQuota(const ProviderStatus& provider, const QuotaInfo& quota) {
    return provider.providerKey == "openai" &&
        quota.providerSurface == "openai_subscription_codex_local";
}

bool isMiniMaxCodingPlanQuota(const ProviderStatus& provider, const QuotaInfo& quota) {
    return provider.providerKey == "minimax" &&
        quota.providerSurface == "minimax_subscription_coding_plan";
}

bool isMiniMaxTokenPlanQuota(const ProviderStatus& provider, const QuotaInfo& quota) {
    return provider.providerKey == "minimax" &&
        quota.providerSurface == "minimax_subscription_token_plan";
}

bool isKimiSubscriptionQuota(const ProviderStatus& provider, const QuotaInfo& quota) {
    return provider.providerKey == "kimi" &&
        quota.providerSurface == "kimi_subscription_api";
}

bool isPairedSubscriptionQuotaSummary(const ProviderStatus& provider, const QuotaInfo& quota) {
    return (isOpenAISubscriptionQuota(provider, quota) || isKimiSubscriptionQuota(provider, quota)) &&
        quota.secondaryRemainingPercentage >= 0.0;
}

bool isStandaloneSecondaryQuotaRow(const ProviderStatus& provider,
                                   const QuotaInfo& quota,
                                   const std::vector<QuotaInfo>& quotas) {
    if (quota.windowType != "weekly") {
        return false;
    }

    return std::any_of(
        quotas.begin(),
        quotas.end(),
        [&](const QuotaInfo& candidate) {
            return candidate.providerSurface == quota.providerSurface &&
                candidate.windowType != quota.windowType &&
                isPairedSubscriptionQuotaSummary(provider, candidate);
        });
}

double normalizedUsedPercentage(const QuotaInfo& quota) {
    if (quota.limit <= 0) {
        return 0.0;
    }

    if (quota.usedPercentage > 0.0) {
        return quota.usedPercentage;
    }

    if (quota.remainingPercentage > 0.0) {
        return 100.0 - quota.remainingPercentage;
    }

    return (static_cast<double>(quota.used) / static_cast<double>(quota.limit)) * 100.0;
}

std::string quotaSourceLabel(const QuotaInfo& quota) {
    if (quota.isBackfilled) {
        return "Backfilled";
    }
    if (quota.hasUnknownGaps) {
        return "Estimated";
    }
    if (quota.providerSurface == "openai_subscription_local") {
        return "Local";
    }
    if (quota.providerSurface == "openai_subscription_codex_local") {
        return "Codex-local";
    }
    if (quota.providerSurface == "openai_subscription_export_import") {
        return "Export Import";
    }
    if (quota.providerSurface == "openai_api_import") {
        return "API Platform";
    }
    if (quota.providerSurface == "minimax_subscription_coding_plan" ||
        quota.providerSurface == "minimax_subscription_token_plan") {
        return "Subscription";
    }
    if (quota.providerSurface == "claude_code_local_stats") {
        return "Local";
    }
    if (quota.providerSurface == "kimi_subscription_api" || isExternalSubscriptionQuota(quota)) {
        return "Subscription";
    }
    return "Local";
}

std::string providerQuotaUsedValue(const QuotaInfo& quota) {
    if (quota.limit <= 0) {
        return "-";
    }

    return formatPercentageLabel(normalizedUsedPercentage(quota));
}

std::string providerQuotaLimitValue(const QuotaInfo& quota) {
    if (quota.providerSurface == "openai_subscription_codex_local" ||
        quota.providerSurface == "kimi_subscription_api" || isExternalSubscriptionQuota(quota)) {
        return "100%";
    }
    return formatOptionalNumber(quota.limit);
}

std::string providerQuotaRemainingValue(const QuotaInfo& quota) {
    if (quota.providerSurface == "openai_subscription_codex_local" ||
        quota.providerSurface == "kimi_subscription_api" || isExternalSubscriptionQuota(quota)) {
        return formatPercentageLabel(quota.remainingPercentage);
    }
    return formatOptionalNumber(quota.remaining);
}

std::string quotaSyncSourceLabel(const QuotaInfo& quota) {
    if (quota.providerSurface == "openai_api_import" || quota.providerSurface == "minimax_subscription_coding_plan" ||
        quota.providerSurface == "minimax_subscription_token_plan" ||
        quota.providerSurface == "kimi_subscription_api" || isExternalSubscriptionQuota(quota)) {
        return "API";
    }
    if (quota.providerSurface == "openai_subscription_export_import") {
        return "Import";
    }
    return "Local";
}

std::string providerQuotaDisplayLabel(
    const ProviderStatus& provider,
    const std::map<std::string, int>& providerCounts) {
    const auto countIt = providerCounts.find(provider.providerKey);
    if (countIt != providerCounts.end() && countIt->second > 1 && !provider.accountId.empty()) {
        return provider.displayName + " / " + provider.accountId;
    }
    return provider.displayName;
}

const std::string& dashboardQuotaTime(const ProviderStatus& provider) {
    return provider.lastKnownQuota ? provider.lastKnownQuota->lastSyncTime : provider.lastSyncTime;
}

const std::vector<QuotaInfo>& dashboardQuotas(const ProviderStatus& provider) {
    return provider.lastKnownQuota ? provider.lastKnownQuota->quotas : provider.quotas;
}

std::vector<ProviderQuotaRowData> buildProviderQuotaRows(const DashboardViewModel& model) {
    std::vector<ProviderQuotaRowData> rows;
    std::map<std::string, int> providerCounts;
    for (const auto& provider : model.providers) {
        providerCounts[provider.providerKey] += 1;
    }

    for (const auto& provider : model.providers) {
        const std::string providerState = provider.isEnabled
            ? providerStateLabel(provider.state)
            : "Disabled";
        const auto& lastSync = dashboardQuotaTime(provider);
        const std::string baseLastSync = lastSync.empty() ? "Pending" : formatRuntimeTimeAgo(lastSync);
        const std::string providerLabel = providerQuotaDisplayLabel(provider, providerCounts);

        const auto& quotas = dashboardQuotas(provider);
        if (quotas.empty()) {
            rows.push_back(ProviderQuotaRowData{{providerLabel, providerState,
                                                 (provider.providerKey == "grok" || provider.providerKey == "cursor") &&
                                                         !provider.detectionMessage.empty()
                                                     ? provider.detectionMessage
                                                     : "(no quota data)",
                                                 "", "", "", "", "", baseLastSync},
                                                provider.providerKey,
                                                provider.accountId});
            continue;
        }

        for (const auto& quota : quotas) {
            const std::string reading = baseLastSync + " (" + quotaSyncSourceLabel(quota) + ")";
            rows.push_back(ProviderQuotaRowData{
                {providerLabel, providerState, quotaSourceLabel(quota), quota.windowType, providerQuotaUsedValue(quota),
                 providerQuotaLimitValue(quota), providerQuotaRemainingValue(quota), quota.confidenceTier, reading},
                provider.providerKey,
                provider.accountId});
        }
    }

    return rows;
}

std::string connectionHelpText(RuntimeState runtimeState) {
    std::ostringstream stream;
    stream << "Connection shows the freshest shared runtime state across enabled providers.\r\n\r\n";
    stream << "Current: provider data is fresh and up to date.\r\n";
    stream << "Catching Up: JiStats is refreshing after startup or reconnect.\r\n";
    stream << "Stale: the last successful provider snapshot is older than the freshness window.\r\n";
    stream << "Offline: provider refresh failed or the network/provider is unreachable.\r\n";
    stream << "Unknown: JiStats does not have a usable runtime snapshot yet.\r\n\r\n";
    stream << "Current state: " << runtimeStateLabel(runtimeState);
    return stream.str();
}

const MiniMaxCredentialProfileSummary* findMiniMaxCredentialSummary(
    const DashboardViewModel& model,
    const std::string& planKey) {
    const auto it = std::find_if(
        model.minimaxCredentialProfiles.begin(),
        model.minimaxCredentialProfiles.end(),
        [&planKey](const MiniMaxCredentialProfileSummary& summary) {
            return summary.planKey == planKey;
        });
    return it == model.minimaxCredentialProfiles.end() ? nullptr : &(*it);
}

const ProviderStatus* findProviderStatusSummary(
    const DashboardViewModel& model,
    const std::string& providerKey) {
    const auto it = std::find_if(
        model.providers.begin(),
        model.providers.end(),
        [&providerKey](const ProviderStatus& provider) {
            return provider.providerKey == providerKey;
        });
    return it == model.providers.end() ? nullptr : &(*it);
}

std::string localProviderSettingsTitle(const std::string& providerKey) {
    if (providerKey == "claude_code") {
        return "Claude Code Local Tracking";
    }
    if (providerKey == "kimi") {
        return "Kimi Code Local Tracking";
    }
    return "Local Provider Tracking";
}

std::string localProviderToggleLabel(const std::string& providerKey) {
    if (providerKey == "claude_code") {
        return "Enable Claude Code local tracking";
    }
    if (providerKey == "kimi") {
        return "Enable Kimi Code local tracking";
    }
    return "Enable local tracking";
}

std::string localProviderSettingsInfoText(const std::string& providerKey) {
    if (providerKey == "claude_code") {
        return "JiStats reads only approved Claude Code aggregate/runtime artifacts. "
               "Local activity may be exact for what Claude recorded locally, but provider-exact "
               "quota truth and provider-mode classification still remain Estimated or Unknown "
               "unless Anthropic exposes a separately validated corroborating signal.";
    }
    if (providerKey == "kimi") {
        return "JiStats reads only approved Kimi runtime/task/notification metadata. "
               "That local evidence can support exact local activity summaries, but provider-exact "
               "quota truth still remains Estimated or Unknown unless a separately validated Kimi "
               "signal can corroborate it.";
    }
    return "JiStats uses approved local-first provider evidence only.";
}

std::string localProviderSettingsStatusText(
    const DashboardViewModel& model,
    const std::string& providerKey) {
    const bool enabled =
        providerKey == "claude_code"
            ? model.preferences.claudeCodeEnabled
            : model.preferences.kimiCodeEnabled;
    const bool detected =
        providerKey == "claude_code"
            ? model.claudeCodeLocallyDetected
            : model.kimiCodeLocallyDetected;

    std::ostringstream stream;
    stream << "Status: " << (enabled ? "Tracking enabled" : "Tracking disabled");
    if (detected) {
        stream << " | Local artifacts detected";
    } else {
        stream << " | Waiting for local artifacts";
    }

    const ProviderStatus* provider = findProviderStatusSummary(model, providerKey);
    if (provider != nullptr && enabled) {
        stream << " | Runtime " << providerStateLabel(provider->state);
    }

    return stream.str();
}

std::string localProviderSettingsDetailText(
    const DashboardViewModel& model,
    const std::string& providerKey) {
    const bool enabled =
        providerKey == "claude_code"
            ? model.preferences.claudeCodeEnabled
            : model.preferences.kimiCodeEnabled;
    const bool detected =
        providerKey == "claude_code"
            ? model.claudeCodeLocallyDetected
            : model.kimiCodeLocallyDetected;
    const std::string detectionMessage =
        providerKey == "claude_code"
            ? model.claudeCodeDetectionMessage
            : model.kimiCodeDetectionMessage;

    std::ostringstream stream;
    if (!detectionMessage.empty()) {
        stream << detectionMessage;
    } else if (detected) {
        stream << "Approved local artifacts were detected.";
    } else {
        stream << "Local artifacts have not been detected yet.";
    }

    stream << " ";
    if (enabled) {
        stream << "While enabled, this provider may appear in the dashboard, reports, and taskbar "
                  "when JiStats has approved local evidence.";
    } else {
        stream << "While disabled, JiStats hides this provider from dashboard and taskbar surfaces "
                  "without changing the bounded local-first parser rules.";
    }

    return stream.str();
}

std::string miniMaxCredentialStatusLabel(const MiniMaxCredentialProfileSummary* summary) {
    if (summary == nullptr || !summary->hasPersistedProfile) {
        return "Missing";
    }

    if (summary->validationState == "valid") {
        return "Validated";
    }
    if (summary->validationState == "pending") {
        return "Validating";
    }
    if (summary->validationState == "invalid") {
        return "Validation Failed";
    }
    if (summary->validationState == "unreachable") {
        return "Unavailable / Retry Needed";
    }
    return "Present";
}

std::string miniMaxCredentialDetailText(const MiniMaxCredentialProfileSummary* summary) {
    if (summary == nullptr || !summary->hasPersistedProfile) {
        return "No MiniMax subscription credential stored yet.";
    }

    std::ostringstream detail;
    if (!summary->accountDisplayName.empty()) {
        detail << summary->accountDisplayName;
    } else if (!summary->planDisplayName.empty()) {
        detail << summary->planDisplayName;
    } else {
        detail << "MiniMax subscription";
    }

    if (!summary->lastValidatedAtLabel.empty()) {
        detail << " | Checked " << summary->lastValidatedAtLabel;
    }
    if (!summary->lastValidationError.empty()) {
        detail << " | " << summary->lastValidationError;
    }

    return detail.str();
}

#ifdef _WIN32
std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return L"";
    }

    const int count = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (count <= 0) {
        return std::wstring(value.begin(), value.end());
    }

    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), count);
    result.resize(static_cast<size_t>(count - 1));
    return result;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return "";
    }

    const int count = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        std::string fallback;
        fallback.reserve(value.size());
        for (const wchar_t ch : value) {
            fallback.push_back((ch >= 0 && ch <= 0x7F) ? static_cast<char>(ch) : '?');
        }
        return fallback;
    }

    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), count, nullptr, nullptr);
    result.resize(static_cast<size_t>(count - 1));
    return result;
}

void setWindowText(HWND hwnd, const std::string& value) {
    if (hwnd != nullptr) {
        SetWindowTextW(hwnd, utf8ToWide(value).c_str());
    }
}

void setWindowText(HWND hwnd, const std::wstring& value) {
    if (hwnd != nullptr) {
        SetWindowTextW(hwnd, value.c_str());
    }
}

int insertListViewItem(HWND listView, int rowIndex, const std::wstring& text) {
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = rowIndex;
    item.pszText = const_cast<LPWSTR>(text.c_str());
    return static_cast<int>(SendMessageW(listView, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)));
}

void setListViewItemText(HWND listView, int rowIndex, int columnIndex, const std::wstring& text) {
    LVITEMW item{};
    item.iSubItem = columnIndex;
    item.pszText = const_cast<LPWSTR>(text.c_str());
    SendMessageW(listView, LVM_SETITEMTEXTW, static_cast<WPARAM>(rowIndex), reinterpret_cast<LPARAM>(&item));
}

bool isSubscriptionProvider(const ProviderStatus& provider) {
    const auto& key = provider.providerKey;
    return key == "openai" || key == "minimax" || key == "kimi" || key == "grok" || key == "cursor";
}

bool isSubscriptionQuota(const ProviderStatus& provider, const QuotaInfo& quota) {
    return isOpenAISubscriptionQuota(provider, quota) || isKimiSubscriptionQuota(provider, quota) ||
           isMiniMaxCodingPlanQuota(provider, quota) || isMiniMaxTokenPlanQuota(provider, quota) ||
           isExternalSubscriptionQuota(quota) || quota.providerSurface.empty();
}

std::string quotaWindowLabel(const ProviderStatus& provider, const QuotaInfo& quota, const std::string& window,
                             bool secondary = false) {
    // Codex secondary types also cover durations shorter than a week.
    if ((isOpenAISubscriptionQuota(provider, quota) && (window == "interval" || (secondary && window == "weekly"))) ||
        (isKimiSubscriptionQuota(provider, quota) && window == "interval")) {
        return secondary ? "Secondary window" : "Primary window";
    }
    if (window == "weekly") {
        return "7-day";
    }
    if (window == "daily") {
        return "Daily";
    }
    if (window == "monthly") {
        return "Monthly";
    }
    return "Plan";
}

std::string quotaResetLabel(std::time_t reset) {
    return reset > 0 ? "Resets " + formatResetTimeLabel(reset) : "Reset time unknown";
}

std::string quotaStatusLabel(const ProviderStatus& provider) {
    const auto sync = parseRuntimeTimestamp(dashboardQuotaTime(provider));
    if (sync <= 0) {
        return provider.state == ProviderState::Live ? "Update time unknown"
                                                     : providerStateLabel(provider.state) + " / Update time unknown";
    }
    const std::string state = provider.state == ProviderState::Live ? "Updated" : providerStateLabel(provider.state);
    return state + " / " + formatSystemDateTime(sync, TIME_NOSECONDS);
}

std::vector<UsageProgressSummary> buildUsageProgressSummaries(const DashboardViewModel& model) {
    std::vector<UsageProgressSummary> summaries;
    std::map<std::string, int> providerCounts;
    for (const auto& provider : model.providers) {
        if (provider.isEnabled) {
            ++providerCounts[provider.providerKey];
        }
    }

    for (const auto& provider : model.providers) {
        if (!provider.isEnabled || !isSubscriptionProvider(provider)) {
            continue;
        }
        const std::string providerLabel = providerQuotaDisplayLabel(provider, providerCounts);
        const std::string accountSuffix = providerLabel.substr(provider.displayName.size());
        const size_t start = summaries.size();
        const auto& quotas = dashboardQuotas(provider);
        for (const auto& quota : quotas) {
            if (!isSubscriptionQuota(provider, quota) || quota.limit <= 0 ||
                isStandaloneSecondaryQuotaRow(provider, quota, quotas)) {
                continue;
            }

            UsageProgressSummary summary;
            summary.label = normalizedUsageSummaryLabel(provider, quota) + accountSuffix;
            summary.status = quotaStatusLabel(provider);
            summary.percentage = static_cast<int>(std::clamp(quota.remainingPercentage, 0.0, 100.0) + 0.5);
            summary.overlayText = quotaWindowLabel(provider, quota, quota.windowType) + ": " +
                                  formatPercentageLabel(summary.percentage) + " remaining";
            summary.resetText = quotaResetLabel(quota.windowResetAt);

            std::ostringstream detail;
            detail << formatPercentageLabel(quota.remainingPercentage) << " remaining";
            if (isMiniMaxCodingPlanQuota(provider, quota) || isMiniMaxTokenPlanQuota(provider, quota)) {
                const char* units = isMiniMaxCodingPlanQuota(provider, quota) ? "prompts" : "tokens";
                detail << " (" << formatNumber(quota.remaining) << " of " << formatNumber(quota.limit) << " " << units
                       << " left)";
            }
            if (quota.windowResetAt > 0) {
                detail << " (resets " << formatResetTimeLabel(quota.windowResetAt) << ")";
            }
            if (isPairedSubscriptionQuotaSummary(provider, quota)) {
                summary.secondaryPercentage =
                    static_cast<int>(std::clamp(quota.secondaryRemainingPercentage, 0.0, 100.0) + 0.5);
                summary.secondaryOverlayText = quotaWindowLabel(provider, quota, quota.secondaryWindowType, true) +
                                               ": " + formatPercentageLabel(summary.secondaryPercentage) + " remaining";
                summary.secondaryResetText = quotaResetLabel(quota.secondaryWindowResetAt);
                detail << " | Weekly " << formatPercentageLabel(quota.secondaryRemainingPercentage) << " remaining";
                if (quota.secondaryWindowResetAt > 0) {
                    detail << " (resets " << formatResetTimeLabel(quota.secondaryWindowResetAt) << ")";
                }
            }
            detail << "\r\nSource: " << quotaSourceLabel(quota);
            if (!quota.confidenceTier.empty()) {
                detail << " | Confidence: " << quota.confidenceTier;
            }
            summary.detail = detail.str();
            if (quota.isBackfilled || quota.hasUnknownGaps) {
                summary.status += " / " + quotaSourceLabel(quota);
            }
            summaries.push_back(std::move(summary));
        }
        if (start == summaries.size()) {
            UsageProgressSummary summary;
            summary.label = providerLabel;
            summary.status = provider.state == ProviderState::Sample ? "" : providerStateLabel(provider.state);
            summary.overlayText = "Quota unavailable";
            summary.detail = provider.detectionMessage;
            summaries.push_back(std::move(summary));
        }
    }
    return summaries;
}
#endif

} // namespace

class DashboardPresenter::Impl {
public:
    bool initialized = false;
    bool visible = true;
    bool shutdownRequested = false;
    std::string lastError;
    DashboardViewModel viewModel;
    std::function<void(const DashboardPreferences&)> preferencesChangedCallback;
    std::uint64_t preferencesRequest = 0;
    std::function<void(bool, std::uint64_t)> codexModelAttributionChangedCallback;
    std::uint64_t codexAttributionRequest = 0;
    bool requestedCodexAttributionEnabled = false;
    std::function<void()> preferencesOpenedCallback;
    std::function<void()> periodicRefreshCallback;
    std::function<DashboardMiniMaxCredentialResult(const DashboardMiniMaxCredentialRequest&)>
        miniMaxCredentialActionCallback;
    std::function<DashboardOpenAIImportResult(const std::string&)> openAIImportActionCallback;
    std::function<DashboardHtmlExportResult(const std::string&)> htmlExportActionCallback;
    std::function<DashboardNoticeActionResult()> offlineRetryActionCallback;
    std::function<DashboardNoticeActionResult()> offlineDismissActionCallback;
    std::function<void()> exitRequestedCallback;
    mutable std::mutex mutex;
    std::mutex threadJoinMutex;
    std::mutex openAIImportMutex;
    std::mutex htmlExportMutex;
    std::mutex miniMaxActionMutex;
    std::mutex offlineRetryMutex;
    std::mutex offlineDismissMutex;
    std::vector<std::thread> openAIImportThreads;
    std::vector<std::thread> htmlExportThreads;
    std::vector<std::thread> miniMaxActionThreads;
    std::vector<std::thread> offlineRetryThreads;
    std::vector<std::thread> offlineDismissThreads;
    bool openAIImportInFlight = false;
    bool htmlExportInFlight = false;
    bool miniMaxActionInFlight = false;
    bool offlineRetryInFlight = false;
    bool offlineDismissInFlight = false;
    std::wstring lastHtmlExportStatusText;

    struct MiniMaxActionCompletionParams {
        DashboardMiniMaxCredentialResult result;
        std::string submittedAccountName;
        std::uint64_t submittedAccountEditRevision;
    };

    struct OfflineRetryCompletionParams {
        DashboardNoticeActionResult result;
    };

    struct OfflineDismissCompletionParams {
        DashboardNoticeActionResult result;
    };

#ifdef _WIN32
    struct ModelChartPointHit {
        int seriesIndex = -1;
        int pointIndex = -1;
        POINT position{};
        bool onMarker = false;

        bool operator==(const ModelChartPointHit& other) const {
            return seriesIndex == other.seriesIndex && pointIndex == other.pointIndex &&
                   position.x == other.position.x && position.y == other.position.y && onMarker == other.onMarker;
        }
    };

    std::thread uiThread;
    std::condition_variable readyCv;
    HWND window = nullptr;
    HWND preferencesWindow = nullptr;
    HWND analysisWindow = nullptr;
    HWND titleLabel = nullptr;
    HWND dashboardViewport = nullptr;
    HWND appGroup = nullptr;
    HWND mainTab = nullptr;
    HWND historyButton = nullptr;
    HWND dashboardSettingsButton = nullptr;
    HWND connectionLabel = nullptr;
    HWND connectionValue = nullptr;
    HWND offlineRetryButton = nullptr;
    HWND offlineNoticeTitle = nullptr;
    HWND offlineNoticeMessage = nullptr;
    HWND offlineDismissButton = nullptr;
    HWND connectionTooltip = nullptr;
    HWND usageTooltip = nullptr;
    HWND recoveryLabel = nullptr;
    HWND recoveryValue = nullptr;
    HWND scheduledExportLabel = nullptr;
    HWND scheduledExportValue = nullptr;
    HWND updatedLabel = nullptr;
    HWND updatedValue = nullptr;
    HWND usageGroup = nullptr;
    HWND quotaEmptyText = nullptr;
    HWND activityGroup = nullptr;
    HWND activityText = nullptr;
    HWND codexThroughputGroup = nullptr;
    HWND codexThroughputText = nullptr;
    HWND codexThroughputAveragesText = nullptr;
    HWND codexThroughputTotalsText = nullptr;
    HWND codexThroughputChart = nullptr;
    HWND openAiUsageGroup = nullptr;
    HWND openAiUsageText = nullptr;
    HWND openAiUsageChart = nullptr;
    HWND hicapUsageGroup = nullptr;
    HWND hicapUsageText = nullptr;
    HWND hicapUsageChart = nullptr;
    int codexChartHoverIndex = -1;
    bool codexChartTrackingMouse = false;
    HWND modelChartHoverWindow = nullptr;
    POINT modelChartHoverCursor{};
    ModelChartPointHit modelChartHoverHit;
    bool modelChartTrackingMouse = false;
    ULONGLONG nextDashboardChartRefreshTick = 0;
    HWND chartCountdownHoverWindow = nullptr;
    HWND providersGroup = nullptr;
    HWND providerList = nullptr;
    HWND statusBar = nullptr;
    AnalysisPresenter* analysisPresenter = nullptr;
    HWND preferencesTab = nullptr;
    HWND preferencesGeneralGroup = nullptr;
    HWND preferencesInfo = nullptr;
    HWND overlayCheckbox = nullptr;
    HWND scheduledExportGroup = nullptr;
    HWND scheduledExportInfo = nullptr;
    HWND scheduledExportCheckbox = nullptr;
    HWND scheduledExportIntervalLabel = nullptr;
    HWND scheduledExportIntervalEdit = nullptr;
    HWND scheduledExportIntervalSuffix = nullptr;
    HWND scheduledExportPathLabel = nullptr;
    HWND scheduledExportPathEdit = nullptr;
    HWND scheduledExportLastRunLabel = nullptr;
    HWND scheduledExportLastRunValue = nullptr;
    HWND openAISettingsGroup = nullptr;
    HWND openAISettingsInfo = nullptr;
    HWND openaiSubscriptionCheckbox = nullptr;
    HWND codexModelAttributionCheckbox = nullptr;
    HWND startupCheckbox = nullptr;
    HWND startupBackgroundCheckbox = nullptr;
    HWND miniMaxAccountsGroup = nullptr;
    HWND miniMaxAccountsInfo = nullptr;
    HWND miniMaxSubscriptionCheckbox = nullptr;
    HWND miniMaxCodingStatus = nullptr;
    HWND miniMaxCodingDetail = nullptr;
    HWND miniMaxTokenStatus = nullptr;
    HWND miniMaxTokenDetail = nullptr;
    HWND miniMaxPlanLabel = nullptr;
    HWND miniMaxPlanCombo = nullptr;
    HWND miniMaxAccountLabel = nullptr;
    HWND miniMaxAccountEdit = nullptr;
    std::atomic<std::uint64_t> miniMaxAccountEditRevision{0};
    HWND miniMaxSecretLabel = nullptr;
    HWND miniMaxSecretEdit = nullptr;
    HWND miniMaxSaveButton = nullptr;
    HWND miniMaxValidateButton = nullptr;
    HWND miniMaxFeedbackLabel = nullptr;
    HWND claudeCodeGroup = nullptr;
    HWND claudeCodeInfo = nullptr;
    HWND claudeCodeEnableCheckbox = nullptr;
    HWND claudeCodeStatus = nullptr;
    HWND claudeCodeDetail = nullptr;
    HWND futureProviderGroup = nullptr;
    HWND futureProviderInfo = nullptr;
    HWND externalSubscriptionsGroup = nullptr;
    HWND externalSubscriptionsInfo = nullptr;
    HWND grokSubscriptionCheckbox = nullptr;
    HWND cursorSubscriptionCheckbox = nullptr;
    HWND kimiCodeGroup = nullptr;
    HWND kimiCodeInfo = nullptr;
    HWND kimiCodeEnableCheckbox = nullptr;
    HWND kimiCodeStatus = nullptr;
    HWND kimiCodeDetail = nullptr;
    HWND preferencesSaveStatus = nullptr;
    HWND preferencesRetryButton = nullptr;
    HWND preferencesCloseButton = nullptr;
    std::string miniMaxFeedbackMessage;
    bool threadReady = false;
    bool viewModelSyncPending = false;
    DesktopTheme desktopTheme;
    HFONT titleFont = nullptr;
    HFONT bodyFont = nullptr;
    HFONT dashboardBodyFont = nullptr;
    HFONT dashboardLabelFont = nullptr;
    HFONT dashboardSectionFont = nullptr;
    HFONT dashboardTitleFont = nullptr;
    std::array<RECT, 6> overviewCards{};
    RECT noticeRect{};
    RECT otherPageCard{};
    int overviewScroll = 0;
    int wheelDelta = 0;
    size_t quotaRowCount = 0;
    bool mainControlsLaidOut = false;
    struct OverviewLayout {
        std::wstring offlineNoticeMessage;
        bool offlineNoticeVisible = false;
        bool offlineRetryAvailable = false;
        int openAiModelRows = 0;
        int hicapModelRows = 0;
        int activityRows = 0;

        bool operator==(const OverviewLayout& other) const {
            return offlineNoticeMessage == other.offlineNoticeMessage &&
                   offlineNoticeVisible == other.offlineNoticeVisible &&
                   offlineRetryAvailable == other.offlineRetryAvailable && openAiModelRows == other.openAiModelRows &&
                   hicapModelRows == other.hicapModelRows && activityRows == other.activityRows;
        }
    };
    OverviewLayout overviewLayout;
    HICON appIconLarge = nullptr;
    HICON appIconSmall = nullptr;
    HMENU mainMenu = nullptr;
    HMENU fileMenu = nullptr;
    HMENU helpMenu = nullptr;
    std::wstring persistentStatusText = L"Ready";
#ifdef JISTATS_ENABLE_TEST_HOOKS
    std::string openAIImportPathForTesting;
    std::string htmlExportPathForTesting;
    std::uint64_t viewModelSyncCountForTesting = 0;
#endif
    struct UsageProgressRow {
        HWND label = nullptr;
        HWND status = nullptr;
        HWND bar = nullptr;
        int percentage = -1;
        int secondaryPercentage = -1;
        COLORREF barColor = RGB(46, 139, 87);
        COLORREF secondaryBarColor = RGB(46, 139, 87);
        std::wstring overlayText;
        std::wstring secondaryOverlayText;
        std::wstring resetText;
        std::wstring secondaryResetText;
        std::wstring detailTooltipText;
    };
    struct ShellDialogState {
        HWND window = nullptr;
        HWND headerIcon = nullptr;
        HWND headerTitle = nullptr;
        HWND headerSubtitle = nullptr;
        HWND bodyLabel = nullptr;
        HWND closeButton = nullptr;
    };
    std::vector<UsageProgressRow> usageRows;
    std::vector<ProviderQuotaRowData> providerQuotaRows;
    ShellDialogState helpDialog;
    ShellDialogState aboutDialog;
    std::wstring connectionTooltipBody;
    std::wstring recoveryTooltipBody;
    std::wstring scheduledExportTooltipBody;
    std::wstring updatedTooltipBody;
    StorageService placementStorage;
    RECT initialMainWindowRect{}; // Read and written by the UI thread.
    std::optional<RECT> deferredStartupMainPlacement;
    std::optional<RECT> savedAnalysisWindowRect;
    std::optional<RECT> initialAnalysisFallbackRect;
    std::thread placementWriter;
    std::mutex placementMutex;
    std::condition_variable placementCv;
    bool stopPlacementWriter = false;
    std::optional<RECT> pendingMainPlacement;
    std::optional<RECT> pendingAnalysisPlacement;

    static constexpr UINT WM_JISTATS_SYNC_VIEWMODEL = WM_APP + 32;
    static constexpr UINT WM_JISTATS_SYNC_PREFERENCES = WM_APP + 33;
    static constexpr UINT WM_JISTATS_TEST_MINIMAX_ACTION = WM_APP + 34;
    static constexpr UINT WM_JISTATS_OPENAI_IMPORT_COMPLETE = WM_APP + 35;
    static constexpr UINT WM_JISTATS_HTML_EXPORT_COMPLETE = WM_APP + 36;
    static constexpr UINT WM_JISTATS_ATTACH_ANALYSIS = WM_APP + 37;
    static constexpr UINT WM_JISTATS_OPEN_ANALYSIS_WINDOW = WM_APP + 38;
    static constexpr UINT WM_JISTATS_TEST_SELECT_MAIN_TAB = WM_APP + 39;
    static constexpr UINT WM_JISTATS_MINIMAX_ACTION_COMPLETE = WM_APP + 40;
    static constexpr UINT WM_JISTATS_OFFLINE_RETRY_COMPLETE = WM_APP + 41;
    static constexpr UINT WM_JISTATS_OFFLINE_DISMISS_COMPLETE = WM_APP + 42;
    static constexpr UINT WM_JISTATS_RESTORE_MAIN_PLACEMENT = WM_APP + 43;
    static constexpr int kMenuFileSettingsId = 1001;
    static constexpr int kMenuFileImportOpenAIId = 1002;
    static constexpr int kMenuFileExportHtmlId = 1010;
    static constexpr int kMenuFileExitId = 1003;
    static constexpr int kMenuHelpViewHelpId = 1004;
    static constexpr int kMenuHelpAboutId = 1005;
    static constexpr int kMainTabId = 1006;
    static constexpr int kOpenAnalysisWindowButtonId = 1007;
    static constexpr int kOfflineRetryButtonId = 1011;
    static constexpr int kOfflineDismissButtonId = 1012;
    static constexpr int kPreferencesOverlayCheckboxId = 1101;
    static constexpr int kPreferencesOpenAISubscriptionCheckboxId = 1102;
    static constexpr int kPreferencesStartupCheckboxId = 1103;
    static constexpr int kPreferencesStartupBackgroundCheckboxId = 1104;
    static constexpr int kPreferencesMiniMaxSubscriptionCheckboxId = 1118;
    static constexpr int kPreferencesClaudeCodeCheckboxId = 1116;
    static constexpr int kPreferencesKimiCodeCheckboxId = 1117;
    static constexpr int kPreferencesGrokSubscriptionCheckboxId = 1119;
    static constexpr int kPreferencesCursorSubscriptionCheckboxId = 1120;
    static constexpr int kPreferencesCodexModelAttributionCheckboxId = 1121;
    static constexpr int kPreferencesSaveStatusId = 1122;
    static constexpr int kPreferencesRetryButtonId = 1123;
    static constexpr int kPreferencesScheduledExportCheckboxId = 1108;
    static constexpr int kPreferencesScheduledExportIntervalEditId = 1109;
    static constexpr int kPreferencesCloseButtonId = 1106;
    static constexpr int kPreferencesTabId = 1107;
    static constexpr int kPreferencesScheduledExportPathEditId = 1115;
    static constexpr int kMiniMaxPlanComboId = 1110;
    static constexpr int kMiniMaxAccountEditId = 1111;
    static constexpr int kMiniMaxSecretEditId = 1112;
    static constexpr int kMiniMaxSaveButtonId = 1113;
    static constexpr int kMiniMaxValidateButtonId = 1114;
    static constexpr int kShellDialogCloseButtonId = 1105;
    static constexpr int kUsageBarBaseId = 1200;
    static constexpr UINT_PTR kDashboardChartRefreshTimerId = 1300;
    static constexpr UINT kDashboardChartCountdownTimerMs = 1000;
    static constexpr UINT kDashboardChartRefreshIntervalMs = 60 * 1000;
    static constexpr int kDefaultWidth = 1040;
    static constexpr int kDefaultHeight = 820;
    static constexpr int kMinWidth = 620;
    static constexpr int kMinHeight = 460;
    static constexpr int kAnalysisDefaultWidth = 1360;
    static constexpr int kAnalysisDefaultHeight = 960;
    static constexpr int kAnalysisMinWidth = 1100;
    static constexpr int kAnalysisMinHeight = 820;
    static constexpr const wchar_t* kShellDialogKindProperty = L"JiStatsShellDialogKind";
    static constexpr const char* kWindowPosXKey = "ui.dashboard_window_x";
    static constexpr const char* kWindowPosYKey = "ui.dashboard_window_y";
    static constexpr const char* kWindowWidthKey = "ui.dashboard_window_width";
    static constexpr const char* kWindowHeightKey = "ui.dashboard_window_height";
    static constexpr const char* kAnalysisWindowPosXKey = "ui.analysis_window_x";
    static constexpr const char* kAnalysisWindowPosYKey = "ui.analysis_window_y";
    static constexpr const char* kAnalysisWindowWidthKey = "ui.analysis_window_width";
    static constexpr const char* kAnalysisWindowHeightKey = "ui.analysis_window_height";
    int selectedMainTab = 0;
    int selectedPreferencesTab = 0;
    bool suppressProviderSelectionNotification = false;

    static const wchar_t* windowClassName() {
        return L"JiStatsDashboardWindow";
    }

    static const wchar_t* viewportClassName() {
        return L"JiStatsDashboardViewport";
    }

    static const wchar_t* preferencesWindowClassName() {
        return L"JiStatsPreferencesWindow";
    }

    static const wchar_t* analysisWindowClassName() {
        return L"JiStatsAnalysisWindow";
    }

    static const wchar_t* shellDialogWindowClassName() {
        return L"JiStatsShellDialogWindow";
    }

    void seedDefaultsIfNeeded() {
        std::lock_guard<std::mutex> lock(mutex);
        if (viewModel.startupPending || !viewModel.lastUpdateTime.empty()) {
            return;
        }

        viewModel.appVersion = "1.0.0";
        viewModel.lastUpdateTime = formatSystemDateTime(std::time(nullptr));
        viewModel.isOnline = true;
        viewModel.preferences.taskbarOverlayEnabled = true;
        viewModel.preferences.openaiSubscriptionTrackingEnabled = false;
        viewModel.preferences.miniMaxSubscriptionTrackingEnabled = false;
        viewModel.preferences.claudeCodeEnabled = false;
        viewModel.preferences.kimiCodeEnabled = false;
        viewModel.preferences.startupEnabled = false;
        viewModel.preferences.startupStartInBackground = true;
    }

    bool ensurePlacementStorage() {
        return placementStorage.isOpen() || placementStorage.init();
    }

    std::optional<RECT> loadSavedMainWindowRect() {
        SettingsService settings(placementStorage);
        const auto x = settings.getInt(kWindowPosXKey);
        const auto y = settings.getInt(kWindowPosYKey);
        const auto width = settings.getInt(kWindowWidthKey);
        const auto height = settings.getInt(kWindowHeightKey);
        if (!x.has_value() || !y.has_value() || !width.has_value() || !height.has_value()) {
            return std::nullopt;
        }
        if (*width < kMinWidth || *height < kMinHeight) {
            return std::nullopt;
        }

        RECT rect{*x, *y, *x + *width, *y + *height};
#ifdef _WIN32
        RECT workArea{};
        if (tryGetMonitorWorkArea(rect, workArea)) {
            clampRectToWorkArea(rect, workArea);
        }
#endif
        return rect;
    }

    SIZE dashboardMinimumSize(HWND hwnd, const RECT& workArea) const {
        SIZE minimum{scaledPixels(kMinWidth, hwnd), scaledPixels(kMinHeight, hwnd)};
        if (!IsRectEmpty(&workArea)) {
            minimum.cx = std::min(minimum.cx, workArea.right - workArea.left);
            minimum.cy = std::min(minimum.cy, workArea.bottom - workArea.top);
        }
        return minimum;
    }

    void fitInitialDashboardBounds(HWND hwnd) {
        RECT bounds{};
        RECT workArea{};
        if (!GetWindowRect(hwnd, &bounds) ||
            !tryGetMonitorWorkArea(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), workArea)) {
            return;
        }

        const RECT original = bounds;
        const auto minimum = dashboardMinimumSize(hwnd, workArea);
        bounds.right = bounds.left + std::max(bounds.right - bounds.left, minimum.cx);
        bounds.bottom = bounds.top + std::max(bounds.bottom - bounds.top, minimum.cy);
        clampRectToWorkArea(bounds, workArea);
        if (!EqualRect(&bounds, &original)) {
            SetWindowPos(hwnd, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    std::optional<RECT> loadSavedAnalysisWindowRect() {
        SettingsService settings(placementStorage);
        const auto x = settings.getInt(kAnalysisWindowPosXKey);
        if (!x) {
            return std::nullopt;
        }
        const auto y = settings.getInt(kAnalysisWindowPosYKey);
        if (!y) {
            return std::nullopt;
        }
        const auto width = settings.getInt(kAnalysisWindowWidthKey);
        if (!width || *width < kAnalysisMinWidth) {
            return std::nullopt;
        }
        const auto height = settings.getInt(kAnalysisWindowHeightKey);
        if (!height || *height < kAnalysisMinHeight) {
            return std::nullopt;
        }
        return RECT{*x, *y, *x + *width, *y + *height};
    }

    void restoreMainPlacementWhenWindowed(HWND hwnd) {
        if (!deferredStartupMainPlacement || IsIconic(hwnd) || IsZoomed(hwnd)) {
            return;
        }
        const RECT saved = *deferredStartupMainPlacement;
        deferredStartupMainPlacement.reset();
        RECT current{};
        if (GetWindowRect(hwnd, &current) && EqualRect(&current, &initialMainWindowRect)) {
            SetWindowPos(hwnd, nullptr, saved.left, saved.top, saved.right - saved.left, saved.bottom - saved.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            fitInitialDashboardBounds(hwnd);
        }
    }

    void persistMainWindowPlacement(HWND hwnd) {
        if (hwnd == nullptr || !IsWindow(hwnd) || IsIconic(hwnd) || deferredStartupMainPlacement) {
            return;
        }

        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        RECT rect{};
        if (IsZoomed(hwnd) && GetWindowPlacement(hwnd, &placement) != 0 && !IsRectEmpty(&placement.rcNormalPosition)) {
            rect = placement.rcNormalPosition;
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
                OffsetRect(&rect, monitor.rcWork.left - monitor.rcMonitor.left,
                           monitor.rcWork.top - monitor.rcMonitor.top);
            }
        } else if (GetWindowRect(hwnd, &rect) == 0 || IsRectEmpty(&rect)) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(placementMutex);
            pendingMainPlacement = rect;
        }
        placementCv.notify_one();
    }

    static std::optional<RECT> analysisWindowPlacement(HWND hwnd) {
        if (hwnd == nullptr || !IsWindow(hwnd) || IsIconic(hwnd)) {
            return std::nullopt;
        }

        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        RECT rect{};
        if (GetWindowPlacement(hwnd, &placement) != 0 &&
            !IsRectEmpty(&placement.rcNormalPosition)) {
            rect = placement.rcNormalPosition;
        } else if (GetWindowRect(hwnd, &rect) == 0 || IsRectEmpty(&rect)) {
            return std::nullopt;
        }

        return rect;
    }

    void persistAnalysisWindowPlacement(HWND hwnd) {
        const auto rect = analysisWindowPlacement(hwnd);
        if (!rect || (initialAnalysisFallbackRect && EqualRect(&*rect, &*initialAnalysisFallbackRect))) {
            return;
        }

        initialAnalysisFallbackRect.reset();
        {
            std::lock_guard<std::mutex> lock(placementMutex);
            savedAnalysisWindowRect = rect;
            pendingAnalysisPlacement = rect;
        }
        placementCv.notify_one();
    }

    void savePlacement(const std::optional<RECT>& rect, const char* xKey, const char* yKey, const char* widthKey,
                       const char* heightKey) {
        if (!rect || !ensurePlacementStorage() ||
            !placementStorage.beginTransaction(StorageService::TransactionMode::Immediate)) {
            return;
        }
        SettingsService settings(placementStorage);
        if (!(settings.setInt(xKey, rect->left) && settings.setInt(yKey, rect->top) &&
              settings.setInt(widthKey, rect->right - rect->left) &&
              settings.setInt(heightKey, rect->bottom - rect->top) && placementStorage.commitTransaction())) {
            placementStorage.rollbackTransaction();
        }
    }

    void runPlacementWriter() {
        if (placementStorage.isOpen()) {
            const auto storedRect = loadSavedAnalysisWindowRect();
            std::lock_guard<std::mutex> lock(placementMutex);
            if (!savedAnalysisWindowRect) {
                savedAnalysisWindowRect = storedRect;
            }
        }
        for (;;) {
            std::optional<RECT> mainPlacement;
            std::optional<RECT> analysisPlacement;
            {
                std::unique_lock<std::mutex> lock(placementMutex);
                placementCv.wait(
                    lock, [this]() { return stopPlacementWriter || pendingMainPlacement || pendingAnalysisPlacement; });
                if (!pendingMainPlacement && !pendingAnalysisPlacement) {
                    return;
                }
                mainPlacement = pendingMainPlacement;
                analysisPlacement = pendingAnalysisPlacement;
                pendingMainPlacement.reset();
                pendingAnalysisPlacement.reset();
            }
            savePlacement(mainPlacement, kWindowPosXKey, kWindowPosYKey, kWindowWidthKey, kWindowHeightKey);
            savePlacement(analysisPlacement, kAnalysisWindowPosXKey, kAnalysisWindowPosYKey, kAnalysisWindowWidthKey,
                          kAnalysisWindowHeightKey);
        }
    }

    void joinPlacementWriter() {
        {
            std::lock_guard<std::mutex> lock(placementMutex);
            stopPlacementWriter = true;
        }
        placementCv.notify_one();
        placementWriter.join();
    }

    void centerWindowOnOwner(HWND hwnd, HWND owner) {
        if (hwnd == nullptr || owner == nullptr || !IsWindow(hwnd) || !IsWindow(owner)) {
            return;
        }

        RECT ownerRect{};
        RECT windowRect{};
        if (GetWindowRect(owner, &ownerRect) == 0 || GetWindowRect(hwnd, &windowRect) == 0) {
            return;
        }

        const int width = windowRect.right - windowRect.left;
        const int height = windowRect.bottom - windowRect.top;
        RECT targetRect{};
        targetRect.left = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
        targetRect.top = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
        targetRect.right = targetRect.left + width;
        targetRect.bottom = targetRect.top + height;

#ifdef _WIN32
        RECT workArea{};
        if (tryGetMonitorWorkArea(ownerRect, workArea)) {
            clampRectToWorkArea(targetRect, workArea);
        }
#endif

        SetWindowPos(
            hwnd,
            nullptr,
            targetRect.left,
            targetRect.top,
            0,
            0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void ensureFonts() {
        if (titleFont != nullptr && bodyFont != nullptr) {
            return;
        }

        // Use Segoe UI for modern Windows look
        LOGFONTW bodyLogFont{};
        bodyLogFont.lfHeight = -12;  // 9pt
        bodyLogFont.lfWeight = FW_NORMAL;
        bodyLogFont.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(bodyLogFont.lfFaceName, L"Segoe UI");
        bodyFont = CreateFontIndirectW(&bodyLogFont);

        // Title font - larger and semibold
        LOGFONTW titleLogFont{};
        titleLogFont.lfHeight = -20;  // 15pt
        titleLogFont.lfWeight = FW_SEMIBOLD;
        titleLogFont.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(titleLogFont.lfFaceName, L"Segoe UI Semibold");
        titleFont = CreateFontIndirectW(&titleLogFont);
    }

    void releaseFonts() {
        if (titleFont != nullptr) {
            DeleteObject(titleFont);
            titleFont = nullptr;
        }
        if (bodyFont != nullptr && bodyFont != GetStockObject(DEFAULT_GUI_FONT)) {
            DeleteObject(bodyFont);
        }
        bodyFont = nullptr;
        releaseDashboardFonts();
    }

    void releaseDashboardFonts() {
        const HFONT fonts[] = {dashboardBodyFont, dashboardLabelFont, dashboardSectionFont, dashboardTitleFont};
        for (HFONT font : fonts) {
            if (font != nullptr) {
                DeleteObject(font);
            }
        }
        dashboardBodyFont = dashboardLabelFont = dashboardSectionFont = dashboardTitleFont = nullptr;
    }

    void applyDashboardFonts() {
        const auto makeFont = [this](int pixels, int weight) {
            LOGFONTW font{};
            font.lfHeight = -scaledPixels(pixels);
            font.lfWeight = weight;
            font.lfQuality = CLEARTYPE_QUALITY;
            wcscpy_s(font.lfFaceName, L"Tahoma");
            return CreateFontIndirectW(&font);
        };
        const HFONT oldFonts[] = {dashboardBodyFont, dashboardLabelFont, dashboardSectionFont, dashboardTitleFont};
        dashboardBodyFont = makeFont(13, FW_NORMAL);
        dashboardLabelFont = makeFont(13, FW_SEMIBOLD);
        dashboardSectionFont = makeFont(14, FW_BOLD);
        dashboardTitleFont = makeFont(22, FW_BOLD);

        const auto setFont = [](HWND control, HFONT font) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        };
        const HWND bodyControls[] = {historyButton,      dashboardSettingsButton,
                                     recoveryValue,      scheduledExportValue,
                                     updatedValue,       offlineNoticeMessage,
                                     offlineRetryButton, offlineDismissButton,
                                     quotaEmptyText,     activityText,
                                     openAiUsageText,    hicapUsageText,
                                     providerList,       statusBar};
        for (HWND control : bodyControls) {
            setFont(control, dashboardBodyFont);
        }
        const HWND labels[] = {connectionLabel, recoveryLabel,   scheduledExportLabel,
                               updatedLabel,    connectionValue, offlineNoticeTitle};
        for (HWND control : labels) {
            setFont(control, dashboardLabelFont);
        }
        const HWND sections[] = {appGroup,         usageGroup,      activityGroup, codexThroughputGroup,
                                 openAiUsageGroup, hicapUsageGroup, providersGroup};
        for (HWND control : sections) {
            setFont(control, dashboardSectionFont);
        }
        for (const auto& row : usageRows) {
            setFont(row.label, dashboardLabelFont);
            setFont(row.status, dashboardBodyFont);
            setFont(row.bar, dashboardBodyFont);
        }
        setFont(titleLabel, dashboardTitleFont);
        setFont(mainTab, dashboardLabelFont);
        SendMessageW(mainTab, TCM_SETITEMSIZE, 0, MAKELPARAM(scaledPixels(134), scaledPixels(26)));
        if (HWND header = ListView_GetHeader(providerList)) {
            setFont(header, dashboardLabelFont);
        }
        for (HFONT font : oldFonts) {
            if (font != nullptr) {
                DeleteObject(font);
            }
        }
    }

    void ensureAppIcons() {
        if (appIconLarge == nullptr) {
            appIconLarge = WindowsAppAssets::loadAppIcon(GetSystemMetrics(SM_CXICON));
        }
        if (appIconSmall == nullptr) {
            appIconSmall = WindowsAppAssets::loadAppIcon(GetSystemMetrics(SM_CXSMICON));
        }
    }

    void releaseAppIcons() {
        if (appIconLarge != nullptr) {
            DestroyIcon(appIconLarge);
            appIconLarge = nullptr;
        }
        if (appIconSmall != nullptr) {
            DestroyIcon(appIconSmall);
            appIconSmall = nullptr;
        }
    }

    void applyWindowIcons(HWND hwnd) {
        if (hwnd == nullptr) {
            return;
        }

        ensureAppIcons();
        if (appIconLarge != nullptr) {
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIconLarge));
        }
        if (appIconSmall != nullptr) {
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIconSmall));
        }
    }

    void applyBodyFont(HWND hwnd) {
        if (hwnd != nullptr && bodyFont != nullptr) {
            SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont), TRUE);
        }
    }

    void applyTitleFont(HWND hwnd) {
        if (hwnd != nullptr && titleFont != nullptr) {
            SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont), TRUE);
        }
    }

    int scaledPixels(int value, HWND hwnd = nullptr) const {
#ifdef _WIN32
        UINT dpi = 96;
        if (hwnd != nullptr) {
            dpi = GetDpiForWindow(hwnd);
        } else if (window != nullptr) {
            dpi = GetDpiForWindow(window);
        }
        return MulDiv(value, dpi == 0 ? 96 : static_cast<int>(dpi), 96);
#else
        (void)hwnd;
        return value;
#endif
    }

    void initializeCommonControls() {
        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_WIN95_CLASSES;
        InitCommonControlsEx(&icc);
    }

    void configureTooltipTool(HWND target) {
        if (connectionTooltip == nullptr || target == nullptr) {
            return;
        }

        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = window;
        tool.uId = reinterpret_cast<UINT_PTR>(target);
        tool.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(connectionTooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }

    void ensureConnectionTooltip() {
        if (window == nullptr || connectionTooltip != nullptr) {
            return;
        }

        connectionTooltip = CreateWindowExW(
            WS_EX_TOPMOST,
            TOOLTIPS_CLASSW,
            nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            window,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        if (connectionTooltip == nullptr) {
            return;
        }

        SendMessageW(connectionTooltip, TTM_SETMAXTIPWIDTH, 0, 360);
        SetWindowPos(
            connectionTooltip,
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SendMessageW(connectionTooltip, TTM_ACTIVATE, TRUE, 0);
        configureTooltipTool(connectionLabel);
        configureTooltipTool(connectionValue);
        configureTooltipTool(recoveryLabel);
        configureTooltipTool(recoveryValue);
        configureTooltipTool(scheduledExportLabel);
        configureTooltipTool(scheduledExportValue);
        configureTooltipTool(updatedValue);
    }

    void configureUsageTooltipTool(HWND target) {
        if (usageTooltip == nullptr || target == nullptr) {
            return;
        }

        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = window;
        tool.uId = reinterpret_cast<UINT_PTR>(target);
        tool.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(usageTooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }

    void ensureUsageTooltip() {
        if (window == nullptr || usageTooltip != nullptr) {
            return;
        }

        usageTooltip = CreateWindowExW(
            WS_EX_TOPMOST,
            TOOLTIPS_CLASSW,
            nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            window,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        if (usageTooltip == nullptr) {
            return;
        }

        SendMessageW(usageTooltip, TTM_SETMAXTIPWIDTH, 0, 360);
        SetWindowPos(
            usageTooltip,
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SendMessageW(usageTooltip, TTM_ACTIVATE, TRUE, 0);
    }

    void dismissUsageTooltip() {
        if (usageTooltip != nullptr) {
            SendMessageW(usageTooltip, TTM_POP, 0, 0);
        }
    }

    void configureProviderListColumns() {
        if (providerList == nullptr) {
            return;
        }

        HWND header = ListView_GetHeader(providerList);
        if (header != nullptr && dashboardLabelFont != nullptr) {
            SendMessageW(header, WM_SETFONT, reinterpret_cast<WPARAM>(dashboardLabelFont), TRUE);
        }

        const int widthPadding = scaledPixels(28, providerList);
        for (int index = 0;
             index < static_cast<int>(sizeof(kProviderQuotaColumns) / sizeof(kProviderQuotaColumns[0]));
             ++index) {
            const std::wstring title = kProviderQuotaColumns[index].title;
            const int minimumWidth = scaledPixels(kProviderQuotaColumns[index].width, providerList);
            const int measuredWidth =
                measureTextWidth(header != nullptr ? header : providerList, title, dashboardLabelFont, widthPadding);
            const int resolvedWidth = std::max(minimumWidth, measuredWidth);
            ListView_SetColumnWidth(providerList, index, resolvedWidth);
        }
    }

    void setComboSelectionByIndex(HWND combo, int index) {
        if (combo == nullptr) {
            return;
        }
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(std::max(0, index)), 0);
    }

    void syncConnectionTooltip(RuntimeState runtimeState) {
        connectionTooltipBody = utf8ToWide(connectionHelpText(runtimeState));
    }

    void syncRecoveryTooltip(const DashboardViewModel& model) {
        recoveryTooltipBody = utf8ToWide(model.recoveryStatusDetail);
    }

    void syncScheduledExportTooltip(const DashboardViewModel& model) {
        scheduledExportTooltipBody = utf8ToWide(model.scheduledExportStatusDetail);
    }

    const std::wstring* tooltipTextForTarget(UINT_PTR targetId) const {
        const HWND target = reinterpret_cast<HWND>(targetId);
        if (target == connectionLabel || target == connectionValue) {
            return &connectionTooltipBody;
        }
        if (target == recoveryLabel || target == recoveryValue) {
            return &recoveryTooltipBody;
        }
        if (target == scheduledExportLabel || target == scheduledExportValue) {
            return &scheduledExportTooltipBody;
        }
        if (target == updatedValue) {
            return &updatedTooltipBody;
        }

        for (const auto& row : usageRows) {
            if (target == row.label || target == row.status || target == row.bar) {
                return &row.detailTooltipText;
            }
        }

        return nullptr;
    }

    void closeAuxiliaryWindows() {
        HWND prefs = nullptr;
        HWND analysis = nullptr;
        HWND help = nullptr;
        HWND about = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            prefs = preferencesWindow;
            analysis = analysisWindow;
            help = helpDialog.window;
            about = aboutDialog.window;
        }

        const HWND windowsToClose[] = {prefs, analysis, help, about};
        for (HWND target : windowsToClose) {
            if (target != nullptr && IsWindow(target)) {
                DestroyWindow(target);
            }
        }
    }

    void layoutAnalysisWindowControls(HWND hwnd) {
        if (analysisPresenter == nullptr || hwnd == nullptr) {
            return;
        }

        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        const int gutter = scaledPixels(16, hwnd);
        RECT bounds{};
        bounds.left = gutter;
        bounds.top = gutter;
        bounds.right = std::max(bounds.left + 1, clientRect.right - gutter);
        bounds.bottom = std::max(bounds.top + 1, clientRect.bottom - gutter);
        analysisPresenter->layout(bounds);
    }

    void initializeAnalysisControls(HWND hwnd) {
        if (analysisPresenter == nullptr) {
            return;
        }

        AnalysisViewModel model;
        {
            std::lock_guard<std::mutex> lock(mutex);
            model = viewModel.analysis;
        }
        analysisPresenter->init(hwnd);
        analysisPresenter->setVisible(true);
        analysisPresenter->updateViewModel(model);
        layoutAnalysisWindowControls(hwnd);
    }

    void openAnalysisWindow(bool bringToFront = true) {
        HWND existing = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            existing = analysisWindow;
        }

        if (existing != nullptr && IsWindow(existing)) {
            ShowWindow(existing, IsIconic(existing) ? SW_RESTORE : SW_SHOW);
            if (bringToFront) {
                SetForegroundWindow(existing);
            }
            return;
        }

        std::optional<RECT> savedRect;
        {
            std::lock_guard<std::mutex> lock(placementMutex);
            savedRect = savedAnalysisWindowRect;
        }
        RECT rect{};
        if (savedRect) {
            rect = *savedRect;
            RECT workArea{};
            if (tryGetMonitorWorkArea(rect, workArea)) {
                clampRectToWorkArea(rect, workArea);
            }
        } else {
            rect.left = CW_USEDEFAULT;
            rect.top = CW_USEDEFAULT;
            rect.right = kAnalysisDefaultWidth;
            rect.bottom = kAnalysisDefaultHeight;
        }

        HWND owner = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            owner = window;
        }

        HWND hwnd = CreateWindowExW(
            WS_EX_APPWINDOW,
            analysisWindowClassName(),
            L"JiStats Analysis",
            WS_OVERLAPPEDWINDOW | WS_VSCROLL,
            rect.left,
            rect.top,
            rect.left == CW_USEDEFAULT ? kAnalysisDefaultWidth : rect.right - rect.left,
            rect.top == CW_USEDEFAULT ? kAnalysisDefaultHeight : rect.bottom - rect.top,
            owner,
            nullptr,
            GetModuleHandleW(nullptr),
            this);
        if (hwnd == nullptr) {
            return;
        }

        applyWindowIcons(hwnd);
        if (rect.left == CW_USEDEFAULT || rect.top == CW_USEDEFAULT) {
            centerWindowOnOwner(hwnd, owner);
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            analysisWindow = hwnd;
        }

        initialAnalysisFallbackRect = savedRect ? std::nullopt : analysisWindowPlacement(hwnd);
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        if (bringToFront) {
            SetForegroundWindow(hwnd);
        }
    }

    void activateMainPage(int index) {
        selectedMainTab = index;
        layoutMainControls();
    }

    void syncMainTabVisibility() {
        const bool overview = selectedMainTab == 0;
        const HWND overviewControls[] = {appGroup,
                                         connectionLabel,
                                         connectionValue,
                                         recoveryLabel,
                                         recoveryValue,
                                         scheduledExportLabel,
                                         scheduledExportValue,
                                         updatedLabel,
                                         updatedValue,
                                         usageGroup,
                                         codexThroughputGroup,
                                         codexThroughputText,
                                         codexThroughputAveragesText,
                                         codexThroughputTotalsText,
                                         codexThroughputChart,
                                         openAiUsageGroup,
                                         openAiUsageText,
                                         openAiUsageChart,
                                         hicapUsageGroup,
                                         hicapUsageText,
                                         hicapUsageChart};
        for (HWND control : overviewControls) {
            ShowWindow(control, overview ? SW_SHOWNA : SW_HIDE);
        }
        ShowWindow(offlineRetryButton, overview && overviewLayout.offlineRetryAvailable ? SW_SHOWNA : SW_HIDE);
        for (HWND control : {offlineNoticeTitle, offlineNoticeMessage, offlineDismissButton}) {
            ShowWindow(control, overview && overviewLayout.offlineNoticeVisible ? SW_SHOWNA : SW_HIDE);
        }
        for (size_t index = 0; index < usageRows.size(); ++index) {
            const int flag = overview && index < quotaRowCount ? SW_SHOWNA : SW_HIDE;
            ShowWindow(usageRows[index].label, flag);
            ShowWindow(usageRows[index].status, flag);
            ShowWindow(usageRows[index].bar, flag);
        }
        ShowWindow(quotaEmptyText, overview && quotaRowCount == 0 ? SW_SHOWNA : SW_HIDE);
        for (HWND control : {activityGroup, activityText}) {
            ShowWindow(control, overview && overviewLayout.activityRows > 0 ? SW_SHOWNA : SW_HIDE);
        }
        for (HWND control : {providersGroup, providerList}) {
            ShowWindow(control, overview ? SW_HIDE : SW_SHOWNA);
        }
    }

    std::wstring futureProviderPlaceholderText() const {
        const auto labels = preferencesTabLabels();
        const size_t index = static_cast<size_t>(std::clamp(selectedPreferencesTab, 0, static_cast<int>(labels.size()) - 1));
        const std::string providerName = labels[index];
        return utf8ToWide(providerName + " subscription tracking is planned.");
    }

    void syncPreferencesTabVisibility() {
        const int generalFlag = selectedPreferencesTab == 0 ? SW_SHOW : SW_HIDE;
        const int openAIFlag = selectedPreferencesTab == 1 ? SW_SHOW : SW_HIDE;
        const int miniMaxFlag = selectedPreferencesTab == 2 ? SW_SHOW : SW_HIDE;
        const int claudeFlag = selectedPreferencesTab == 3 ? SW_SHOW : SW_HIDE;
        const int futureFlag = selectedPreferencesTab == 4 ? SW_SHOW : SW_HIDE;
        const int kimiFlag = selectedPreferencesTab == 5 ? SW_SHOW : SW_HIDE;
        const int externalSubscriptionsFlag = selectedPreferencesTab == 6 ? SW_SHOW : SW_HIDE;

        const HWND generalControls[] = {
            preferencesGeneralGroup,
            preferencesInfo,
            overlayCheckbox,
            startupCheckbox,
            startupBackgroundCheckbox,
            scheduledExportGroup,
            scheduledExportInfo,
            scheduledExportCheckbox,
            scheduledExportIntervalLabel,
            scheduledExportIntervalEdit,
            scheduledExportIntervalSuffix,
            scheduledExportPathLabel,
            scheduledExportPathEdit,
            scheduledExportLastRunLabel,
            scheduledExportLastRunValue
        };
        for (HWND control : generalControls) {
            if (control != nullptr) {
                ShowWindow(control, generalFlag);
            }
        }

        const HWND openAIControls[] = {openAISettingsGroup, openAISettingsInfo, openaiSubscriptionCheckbox,
                                       codexModelAttributionCheckbox};
        for (HWND control : openAIControls) {
            if (control != nullptr) {
                ShowWindow(control, openAIFlag);
            }
        }

        const HWND miniMaxControls[] = {
            miniMaxAccountsGroup,
            miniMaxAccountsInfo,
            miniMaxSubscriptionCheckbox,
            miniMaxCodingStatus,
            miniMaxCodingDetail,
            miniMaxTokenStatus,
            miniMaxTokenDetail,
            miniMaxPlanLabel,
            miniMaxPlanCombo,
            miniMaxAccountLabel,
            miniMaxAccountEdit,
            miniMaxSecretLabel,
            miniMaxSecretEdit,
            miniMaxSaveButton,
            miniMaxValidateButton,
            miniMaxFeedbackLabel
        };
        for (HWND control : miniMaxControls) {
            if (control != nullptr) {
                ShowWindow(control, miniMaxFlag);
            }
        }

        const HWND claudeControls[] = {
            claudeCodeGroup,
            claudeCodeInfo,
            claudeCodeEnableCheckbox,
            claudeCodeStatus,
            claudeCodeDetail
        };
        for (HWND control : claudeControls) {
            if (control != nullptr) {
                ShowWindow(control, claudeFlag);
            }
        }

        if (futureProviderGroup != nullptr) {
            ShowWindow(futureProviderGroup, futureFlag);
        }
        if (futureProviderInfo != nullptr) {
            ShowWindow(futureProviderInfo, futureFlag);
            if (futureFlag == SW_SHOW) {
                const auto labels = preferencesTabLabels();
                const size_t index = static_cast<size_t>(std::clamp(selectedPreferencesTab, 0, static_cast<int>(labels.size()) - 1));
                setWindowText(futureProviderGroup, utf8ToWide(labels[index] + " Subscription"));
                setWindowText(futureProviderInfo, futureProviderPlaceholderText());
            }
        }

        const HWND externalSubscriptionControls[] = {externalSubscriptionsGroup, externalSubscriptionsInfo,
                                                     grokSubscriptionCheckbox, cursorSubscriptionCheckbox};
        for (const auto control : externalSubscriptionControls) {
            if (control != nullptr) {
                ShowWindow(control, externalSubscriptionsFlag);
            }
        }

        const HWND kimiControls[] = {
            kimiCodeGroup,
            kimiCodeInfo,
            kimiCodeEnableCheckbox,
            kimiCodeStatus,
            kimiCodeDetail
        };
        for (HWND control : kimiControls) {
            if (control != nullptr) {
                ShowWindow(control, kimiFlag);
            }
        }
    }

    void layoutPreferencesControls(HWND hwnd) {
        if (preferencesTab == nullptr) {
            return;
        }

        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        const int margin = 12;
        const int buttonWidth = 88;
        const int buttonHeight = 28;
        const int closeBottomMargin = 16;
        const int tabTop = margin;
        const int contentWidth = clientRect.right - (margin * 2);
        const int tabHeight = clientRect.bottom - tabTop - buttonHeight - closeBottomMargin - (margin * 2);
        MoveWindow(preferencesTab, margin, tabTop, contentWidth, tabHeight, TRUE);

        RECT tabRect{};
        GetClientRect(preferencesTab, &tabRect);
        TabCtrl_AdjustRect(preferencesTab, FALSE, &tabRect);
        const int pageLeft = margin + tabRect.left;
        const int pageTop = tabTop + tabRect.top;
        const int pageWidth = tabRect.right - tabRect.left;
        const int pageHeight = tabRect.bottom - tabRect.top;

        const int innerLeft = pageLeft + 14;
        const int innerTop = pageTop + 18;
        const int innerWidth = pageWidth - 28;
        const int rowHeight = 22;

        if (preferencesGeneralGroup != nullptr) {
            MoveWindow(preferencesGeneralGroup, pageLeft, pageTop, pageWidth, 170, TRUE);
        }
        if (preferencesInfo != nullptr) {
            MoveWindow(preferencesInfo, innerLeft, innerTop + 8, innerWidth - 12, 36, TRUE);
        }
        if (overlayCheckbox != nullptr) {
            MoveWindow(overlayCheckbox, innerLeft, innerTop + 56, 240, rowHeight, TRUE);
        }
        if (startupCheckbox != nullptr) {
            MoveWindow(startupCheckbox, innerLeft, innerTop + 86, 220, rowHeight, TRUE);
        }
        if (startupBackgroundCheckbox != nullptr) {
            MoveWindow(startupBackgroundCheckbox, innerLeft + 20, innerTop + 114, 320, rowHeight, TRUE);
        }
        if (scheduledExportGroup != nullptr) {
            MoveWindow(scheduledExportGroup, pageLeft, pageTop + 182, pageWidth, 194, TRUE);
        }
        if (scheduledExportInfo != nullptr) {
            MoveWindow(scheduledExportInfo, innerLeft, innerTop + 190, innerWidth - 12, 36, TRUE);
        }
        if (scheduledExportCheckbox != nullptr) {
            MoveWindow(scheduledExportCheckbox, innerLeft, innerTop + 226, 260, rowHeight, TRUE);
        }
        if (scheduledExportIntervalLabel != nullptr) {
            MoveWindow(scheduledExportIntervalLabel, innerLeft, innerTop + 256, 60, 20, TRUE);
        }
        if (scheduledExportIntervalEdit != nullptr) {
            MoveWindow(scheduledExportIntervalEdit, innerLeft + 66, innerTop + 252, 72, 22, TRUE);
        }
        if (scheduledExportIntervalSuffix != nullptr) {
            MoveWindow(scheduledExportIntervalSuffix, innerLeft + 146, innerTop + 256, 70, 20, TRUE);
        }
        if (scheduledExportPathLabel != nullptr) {
            MoveWindow(scheduledExportPathLabel, innerLeft, innerTop + 286, 86, 20, TRUE);
        }
        if (scheduledExportPathEdit != nullptr) {
            MoveWindow(scheduledExportPathEdit, innerLeft + 88, innerTop + 282, innerWidth - 106, 22, TRUE);
        }
        if (scheduledExportLastRunLabel != nullptr) {
            MoveWindow(scheduledExportLastRunLabel, innerLeft, innerTop + 316, 70, 20, TRUE);
        }
        if (scheduledExportLastRunValue != nullptr) {
            MoveWindow(scheduledExportLastRunValue, innerLeft + 88, innerTop + 316, innerWidth - 106, 20, TRUE);
        }

        if (openAISettingsGroup != nullptr) {
            MoveWindow(openAISettingsGroup, pageLeft, pageTop, pageWidth, pageHeight, TRUE);
        }
        if (openAISettingsInfo != nullptr) {
            MoveWindow(openAISettingsInfo, innerLeft, innerTop + 8, innerWidth - 12, 72, TRUE);
        }
        if (openaiSubscriptionCheckbox != nullptr) {
            MoveWindow(openaiSubscriptionCheckbox, innerLeft, innerTop + 92, 320, rowHeight, TRUE);
        }
        if (codexModelAttributionCheckbox != nullptr) {
            MoveWindow(codexModelAttributionCheckbox, innerLeft, innerTop + 122, 320, rowHeight, TRUE);
        }

        if (miniMaxAccountsGroup != nullptr) {
            MoveWindow(miniMaxAccountsGroup, pageLeft, pageTop, pageWidth, pageHeight, TRUE);
        }
        if (miniMaxAccountsInfo != nullptr) {
            MoveWindow(miniMaxAccountsInfo, innerLeft, innerTop + 8, innerWidth - 12, 36, TRUE);
        }
        if (miniMaxSubscriptionCheckbox != nullptr) {
            MoveWindow(miniMaxSubscriptionCheckbox, innerLeft, innerTop + 50, 320, rowHeight, TRUE);
        }
        if (miniMaxCodingStatus != nullptr) {
            MoveWindow(miniMaxCodingStatus, innerLeft, innerTop + 86, innerWidth - 12, 18, TRUE);
        }
        if (miniMaxCodingDetail != nullptr) {
            MoveWindow(miniMaxCodingDetail, innerLeft + 16, innerTop + 106, innerWidth - 28, 18, TRUE);
        }
        if (miniMaxTokenStatus != nullptr) {
            MoveWindow(miniMaxTokenStatus, innerLeft, innerTop + 134, innerWidth - 12, 18, TRUE);
        }
        if (miniMaxTokenDetail != nullptr) {
            MoveWindow(miniMaxTokenDetail, innerLeft + 16, innerTop + 154, innerWidth - 28, 18, TRUE);
        }
        if (miniMaxPlanLabel != nullptr) {
            MoveWindow(miniMaxPlanLabel, innerLeft, innerTop + 190, 64, 20, TRUE);
        }
        if (miniMaxPlanCombo != nullptr) {
            MoveWindow(miniMaxPlanCombo, innerLeft + 74, innerTop + 186, 170, 300, TRUE);
        }
        if (miniMaxAccountLabel != nullptr) {
            MoveWindow(miniMaxAccountLabel, innerLeft, innerTop + 222, 86, 20, TRUE);
        }
        if (miniMaxAccountEdit != nullptr) {
            MoveWindow(miniMaxAccountEdit, innerLeft + 92, innerTop + 218, 250, 22, TRUE);
        }
        if (miniMaxSecretLabel != nullptr) {
            MoveWindow(miniMaxSecretLabel, innerLeft, innerTop + 254, 96, 20, TRUE);
        }
        if (miniMaxSecretEdit != nullptr) {
            MoveWindow(miniMaxSecretEdit, innerLeft + 102, innerTop + 250, 240, 22, TRUE);
        }
        if (miniMaxSaveButton != nullptr) {
            MoveWindow(miniMaxSaveButton, pageLeft + pageWidth - 118, innerTop + 216, 92, 26, TRUE);
        }
        if (miniMaxValidateButton != nullptr) {
            MoveWindow(miniMaxValidateButton, pageLeft + pageWidth - 118, innerTop + 248, 92, 26, TRUE);
        }
        if (miniMaxFeedbackLabel != nullptr) {
            MoveWindow(miniMaxFeedbackLabel, innerLeft, innerTop + 286, innerWidth - 12, 42, TRUE);
        }

        if (claudeCodeGroup != nullptr) {
            MoveWindow(claudeCodeGroup, pageLeft, pageTop, pageWidth, pageHeight, TRUE);
        }
        if (claudeCodeInfo != nullptr) {
            MoveWindow(claudeCodeInfo, innerLeft, innerTop + 8, innerWidth - 12, 84, TRUE);
        }
        if (claudeCodeEnableCheckbox != nullptr) {
            MoveWindow(claudeCodeEnableCheckbox, innerLeft, innerTop + 106, 280, rowHeight, TRUE);
        }
        if (claudeCodeStatus != nullptr) {
            MoveWindow(claudeCodeStatus, innerLeft, innerTop + 138, innerWidth - 12, 20, TRUE);
        }
        if (claudeCodeDetail != nullptr) {
            MoveWindow(claudeCodeDetail, innerLeft, innerTop + 166, innerWidth - 12, 84, TRUE);
        }

        if (futureProviderGroup != nullptr) {
            MoveWindow(futureProviderGroup, pageLeft, pageTop, pageWidth, pageHeight, TRUE);
        }
        if (futureProviderInfo != nullptr) {
            MoveWindow(futureProviderInfo, innerLeft, innerTop + 18, innerWidth - 12, 120, TRUE);
        }

        if (externalSubscriptionsGroup != nullptr) {
            MoveWindow(externalSubscriptionsGroup, pageLeft, pageTop, pageWidth, pageHeight, TRUE);
            MoveWindow(externalSubscriptionsInfo, innerLeft, innerTop + 8, innerWidth - 12, 80, TRUE);
            MoveWindow(grokSubscriptionCheckbox, innerLeft, innerTop + 100, 300, rowHeight, TRUE);
            MoveWindow(cursorSubscriptionCheckbox, innerLeft, innerTop + 132, 300, rowHeight, TRUE);
        }

        if (kimiCodeGroup != nullptr) {
            MoveWindow(kimiCodeGroup, pageLeft, pageTop, pageWidth, pageHeight, TRUE);
        }
        if (kimiCodeInfo != nullptr) {
            MoveWindow(kimiCodeInfo, innerLeft, innerTop + 8, innerWidth - 12, 84, TRUE);
        }
        if (kimiCodeEnableCheckbox != nullptr) {
            MoveWindow(kimiCodeEnableCheckbox, innerLeft, innerTop + 106, 280, rowHeight, TRUE);
        }
        if (kimiCodeStatus != nullptr) {
            MoveWindow(kimiCodeStatus, innerLeft, innerTop + 138, innerWidth - 12, 20, TRUE);
        }
        if (kimiCodeDetail != nullptr) {
            MoveWindow(kimiCodeDetail, innerLeft, innerTop + 166, innerWidth - 12, 84, TRUE);
        }

        const int buttonsTop = clientRect.bottom - closeBottomMargin - buttonHeight;
        if (preferencesSaveStatus != nullptr) {
            MoveWindow(preferencesSaveStatus, margin, buttonsTop + 5, contentWidth - 2 * (buttonWidth + margin), 20,
                       TRUE);
        }
        if (preferencesRetryButton != nullptr) {
            MoveWindow(preferencesRetryButton, clientRect.right - 2 * (margin + buttonWidth), buttonsTop, buttonWidth,
                       buttonHeight, TRUE);
        }
        if (preferencesCloseButton != nullptr) {
            MoveWindow(preferencesCloseButton,
                clientRect.right - margin - buttonWidth,
                clientRect.bottom - closeBottomMargin - buttonHeight,
                buttonWidth,
                buttonHeight,
                TRUE);
        }

        syncPreferencesTabVisibility();
    }

    bool createMainMenuBar(HWND hwnd) {
        mainMenu = CreateMenu();
        fileMenu = CreatePopupMenu();
        helpMenu = CreatePopupMenu();
        if (mainMenu == nullptr || fileMenu == nullptr || helpMenu == nullptr) {
            lastError = "Failed to create dashboard menu bar";
            if (helpMenu != nullptr) {
                DestroyMenu(helpMenu);
            }
            if (fileMenu != nullptr) {
                DestroyMenu(fileMenu);
            }
            if (mainMenu != nullptr) {
                DestroyMenu(mainMenu);
            }
            mainMenu = nullptr;
            fileMenu = nullptr;
            helpMenu = nullptr;
            return false;
        }

        AppendMenuW(fileMenu, MF_STRING, kMenuFileSettingsId, L"&Settings...");
        AppendMenuW(fileMenu, MF_STRING, kMenuFileImportOpenAIId, L"Import OpenAI &Export...");
        AppendMenuW(fileMenu, MF_STRING, kMenuFileExportHtmlId, L"Export HTML &Report...");
        AppendMenuW(fileMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(fileMenu, MF_STRING, kMenuFileExitId, L"E&xit");
        AppendMenuW(helpMenu, MF_STRING, kMenuHelpViewHelpId, L"View &Help");
        AppendMenuW(helpMenu, MF_STRING, kMenuHelpAboutId, L"&About JiStats");
        AppendMenuW(mainMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), L"&File");
        AppendMenuW(mainMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(helpMenu), L"&Help");
        SetMenu(hwnd, mainMenu);
        DrawMenuBar(hwnd);
        return true;
    }

    void releaseMenuBar() {
        if (window != nullptr) {
            SetMenu(window, nullptr);
        }
        if (mainMenu != nullptr) {
            DestroyMenu(mainMenu);
        }
        mainMenu = nullptr;
        fileMenu = nullptr;
        helpMenu = nullptr;
    }

    void setStatusText(const std::wstring& text) {
        HWND statusTarget = nullptr;
        std::wstring persistentText;
        {
            std::lock_guard<std::mutex> lock(mutex);
            persistentStatusText = text;
            persistentText = persistentStatusText;
            statusTarget = statusBar;
        }

        if (statusTarget != nullptr) {
            SendMessageW(statusTarget, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(persistentText.c_str()));
        }
    }

    std::optional<std::string> promptForOpenAIImportPath(bool& canceled, std::wstring& errorText) {
        canceled = false;
        errorText.clear();
#ifdef JISTATS_ENABLE_TEST_HOOKS
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!openAIImportPathForTesting.empty()) {
                return openAIImportPathForTesting;
            }
        }
#endif

        wchar_t filePath[MAX_PATH] = L"";
        OPENFILENAMEW openFile{};
        openFile.lStructSize = sizeof(openFile);
        openFile.hwndOwner = window;
        openFile.lpstrFilter =
            L"ChatGPT export conversations.json\0conversations.json\0"
            L"JSON files (*.json)\0*.json\0"
            L"All files (*.*)\0*.*\0";
        openFile.lpstrFile = filePath;
        openFile.nMaxFile = MAX_PATH;
        openFile.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
        openFile.lpstrTitle = L"Select extracted ChatGPT export conversations.json";
        openFile.lpstrDefExt = L"json";

        if (GetOpenFileNameW(&openFile) == FALSE) {
            const DWORD dialogError = CommDlgExtendedError();
            if (dialogError == 0) {
                canceled = true;
                return std::nullopt;
            }

            std::wostringstream stream;
            stream << L"OpenAI export import picker failed (0x"
                   << std::hex << std::uppercase << dialogError << L").";
            errorText = stream.str();
            return std::nullopt;
        }

        return wideToUtf8(filePath);
    }

    void notifyOpenAIImportRequested() {
        {
            std::lock_guard<std::mutex> lock(openAIImportMutex);
            if (openAIImportInFlight) {
                setStatusText(L"OpenAI export import is already running...");
                return;
            }
        }

        bool canceled = false;
        std::wstring pickerError;
        const auto inputPath = promptForOpenAIImportPath(canceled, pickerError);
        if (!inputPath.has_value()) {
            setStatusText(canceled ? L"OpenAI export import canceled." : pickerError);
            return;
        }

        std::function<DashboardOpenAIImportResult(const std::string&)> callback;
        {
            std::lock_guard<std::mutex> lock(mutex);
            callback = openAIImportActionCallback;
        }

        if (!callback) {
            setStatusText(L"OpenAI export import is unavailable.");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(openAIImportMutex);
            if (openAIImportInFlight) {
                setStatusText(L"OpenAI export import is already running...");
                return;
            }
            openAIImportInFlight = true;
        }

        setStatusText(L"OpenAI export import is running...");
        const std::string selectedPath = *inputPath;
        std::lock_guard<std::mutex> lock(openAIImportMutex);
        openAIImportThreads.emplace_back([this, callback, selectedPath]() {
            DashboardOpenAIImportResult result = callback(selectedPath);
            HWND hwnd = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex);
                hwnd = window;
            }

            auto* payload = new DashboardOpenAIImportResult(std::move(result));
            if (hwnd == nullptr ||
                PostMessageW(hwnd, WM_JISTATS_OPENAI_IMPORT_COMPLETE, 0, reinterpret_cast<LPARAM>(payload)) == FALSE) {
                delete payload;
                std::lock_guard<std::mutex> lock(openAIImportMutex);
                openAIImportInFlight = false;
            }
        });
    }

    void joinOpenAIImportThreads() {
        std::vector<std::thread> threads;
        {
            std::lock_guard<std::mutex> lock(openAIImportMutex);
            threads.swap(openAIImportThreads);
        }

        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    void handleOpenAIImportCompletion(DashboardOpenAIImportResult* result) {
        std::unique_ptr<DashboardOpenAIImportResult> ownedResult(result);
        {
            std::lock_guard<std::mutex> lock(openAIImportMutex);
            openAIImportInFlight = false;
        }

        if (!ownedResult) {
            setStatusText(L"OpenAI export import finished without a result.");
            return;
        }

        std::wstring statusText = utf8ToWide(ownedResult->message);
        if (statusText.empty()) {
            statusText = ownedResult->success
                ? L"OpenAI export import completed."
                : L"OpenAI export import failed.";
        }
        setStatusText(statusText);
    }

    std::optional<std::string> promptForHtmlExportPath(bool& canceled, std::wstring& errorText) {
        canceled = false;
        errorText.clear();
#ifdef JISTATS_ENABLE_TEST_HOOKS
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!htmlExportPathForTesting.empty()) {
                return htmlExportPathForTesting;
            }
        }
#endif

        wchar_t filePath[MAX_PATH] = L"";

        // Pre-fill with a suggested filename
        const std::wstring suggested = utf8ToWide("jistats-report.html");
        if (!suggested.empty() && suggested.size() < MAX_PATH) {
            wcscpy_s(filePath, suggested.c_str());
        }

        OPENFILENAMEW saveFile{};
        saveFile.lStructSize = sizeof(saveFile);
        saveFile.hwndOwner = window;
        saveFile.lpstrFilter =
            L"HTML files (*.html)\0*.html\0"
            L"All files (*.*)\0*.*\0";
        saveFile.lpstrFile = filePath;
        saveFile.nMaxFile = MAX_PATH;
        saveFile.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
        saveFile.lpstrTitle = L"Export HTML Report";
        saveFile.lpstrDefExt = L"html";

        if (GetSaveFileNameW(&saveFile) == FALSE) {
            const DWORD dialogError = CommDlgExtendedError();
            if (dialogError == 0) {
                canceled = true;
                return std::nullopt;
            }
            errorText = L"Could not open save dialog.";
            return std::nullopt;
        }

        const std::string narrowPath = wideToUtf8(filePath);
        if (narrowPath.empty()) {
            errorText = L"Selected export path is invalid.";
            return std::nullopt;
        }
        return narrowPath;
    }

    void joinHtmlExportThreads() {
        std::vector<std::thread> threads;
        {
            std::lock_guard<std::mutex> lock(htmlExportMutex);
            threads.swap(htmlExportThreads);
        }

        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    void joinMiniMaxActionThreads() {
        std::vector<std::thread> threads;
        {
            std::lock_guard<std::mutex> lock(miniMaxActionMutex);
            threads.swap(miniMaxActionThreads);
        }

        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    void joinOfflineRetryThreads() {
        std::vector<std::thread> threads;
        {
            std::lock_guard<std::mutex> lock(offlineRetryMutex);
            threads.swap(offlineRetryThreads);
        }

        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    void joinOfflineDismissThreads() {
        std::vector<std::thread> threads;
        {
            std::lock_guard<std::mutex> lock(offlineDismissMutex);
            threads.swap(offlineDismissThreads);
        }

        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    void notifyHtmlExportRequested() {
        {
            std::lock_guard<std::mutex> lock(htmlExportMutex);
            if (htmlExportInFlight) {
                setStatusText(L"HTML export is already running...");
                return;
            }
        }

        bool canceled = false;
        std::wstring pickerError;
        const auto outputPath = promptForHtmlExportPath(canceled, pickerError);
        if (!outputPath.has_value()) {
            setStatusText(canceled ? L"HTML export canceled." : pickerError);
            return;
        }

        std::function<DashboardHtmlExportResult(const std::string&)> callback;
        {
            std::lock_guard<std::mutex> lock(mutex);
            callback = htmlExportActionCallback;
        }

        if (!callback) {
            setStatusText(L"HTML export is unavailable.");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(htmlExportMutex);
            if (htmlExportInFlight) {
                setStatusText(L"HTML export is already running...");
                return;
            }
            htmlExportInFlight = true;
        }

        setStatusText(L"Exporting HTML report...");
        const std::string selectedPath = *outputPath;
        std::lock_guard<std::mutex> lock(htmlExportMutex);
        htmlExportThreads.emplace_back([this, callback, selectedPath]() {
            DashboardHtmlExportResult result = callback(selectedPath);
            HWND hwnd = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex);
                hwnd = window;
            }

            auto* payload = new DashboardHtmlExportResult(std::move(result));
            if (hwnd == nullptr ||
                PostMessageW(hwnd, WM_JISTATS_HTML_EXPORT_COMPLETE, 0, reinterpret_cast<LPARAM>(payload)) == FALSE) {
                delete payload;
                std::lock_guard<std::mutex> lock(htmlExportMutex);
                htmlExportInFlight = false;
            }
        });
    }

    void handleHtmlExportCompletion(DashboardHtmlExportResult* result) {
        std::unique_ptr<DashboardHtmlExportResult> ownedResult(result);
        {
            std::lock_guard<std::mutex> lock(htmlExportMutex);
            htmlExportInFlight = false;
        }

        if (!ownedResult) {
            setStatusText(L"HTML export finished without a result.");
            return;
        }

        std::wstring statusText = utf8ToWide(ownedResult->message);
        if (statusText.empty()) {
            statusText = ownedResult->success
                ? L"HTML export completed."
                : L"HTML export failed.";
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            lastHtmlExportStatusText = statusText;
        }
        setStatusText(statusText);
    }

    void notifyOfflineRetryRequested() {
        std::function<DashboardNoticeActionResult()> callback;
        {
            std::lock_guard<std::mutex> lock(mutex);
            callback = offlineRetryActionCallback;
        }

        if (!callback) {
            setStatusText(L"Offline retry is unavailable.");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(offlineRetryMutex);
            if (offlineRetryInFlight) {
                setStatusText(L"Offline retry is already running...");
                return;
            }
            offlineRetryInFlight = true;
        }

        setStatusText(L"Retrying provider connectivity...");
        std::lock_guard<std::mutex> lock(offlineRetryMutex);
        offlineRetryThreads.emplace_back([this, callback]() {
            DashboardNoticeActionResult result = callback();
            HWND hwnd = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex);
                hwnd = window;
            }

            auto* payload = new OfflineRetryCompletionParams{std::move(result)};
            if (hwnd == nullptr ||
                PostMessageW(hwnd, WM_JISTATS_OFFLINE_RETRY_COMPLETE, 0, reinterpret_cast<LPARAM>(payload)) == FALSE) {
                delete payload;
                std::lock_guard<std::mutex> retryLock(offlineRetryMutex);
                offlineRetryInFlight = false;
            }
        });
    }

    void handleOfflineRetryCompletion(OfflineRetryCompletionParams* params) {
        std::unique_ptr<OfflineRetryCompletionParams> ownedParams(params);
        {
            std::lock_guard<std::mutex> lock(offlineRetryMutex);
            offlineRetryInFlight = false;
        }

        if (!ownedParams) {
            setStatusText(L"Offline retry finished without a result.");
            return;
        }

        std::wstring statusText = utf8ToWide(ownedParams->result.message);
        if (statusText.empty()) {
            statusText = ownedParams->result.success
                ? L"Retry requested."
                : L"Retry could not be started.";
        }
        setStatusText(statusText);
    }

    void notifyOfflineDismissRequested() {
        std::function<DashboardNoticeActionResult()> callback;
        {
            std::lock_guard<std::mutex> lock(mutex);
            callback = offlineDismissActionCallback;
        }

        if (!callback) {
            setStatusText(L"Offline notice dismissal is unavailable.");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(offlineDismissMutex);
            if (offlineDismissInFlight) {
                setStatusText(L"Offline notice dismissal is already running...");
                return;
            }
            offlineDismissInFlight = true;
        }

        setStatusText(L"Dismissing offline notice...");
        std::lock_guard<std::mutex> lock(offlineDismissMutex);
        offlineDismissThreads.emplace_back([this, callback]() {
            DashboardNoticeActionResult result = callback();
            HWND hwnd = nullptr;
            {
                std::lock_guard<std::mutex> stateLock(mutex);
                hwnd = window;
            }

            auto* payload = new OfflineDismissCompletionParams{std::move(result)};
            if (hwnd == nullptr ||
                PostMessageW(hwnd, WM_JISTATS_OFFLINE_DISMISS_COMPLETE, 0, reinterpret_cast<LPARAM>(payload)) == FALSE) {
                delete payload;
                std::lock_guard<std::mutex> dismissLock(offlineDismissMutex);
                offlineDismissInFlight = false;
            }
        });
    }

    void handleOfflineDismissCompletion(OfflineDismissCompletionParams* params) {
        std::unique_ptr<OfflineDismissCompletionParams> ownedParams(params);
        {
            std::lock_guard<std::mutex> lock(offlineDismissMutex);
            offlineDismissInFlight = false;
        }

        if (!ownedParams) {
            setStatusText(L"Offline notice dismissal finished without a result.");
            return;
        }

        std::wstring statusText = utf8ToWide(ownedParams->result.message);
        if (statusText.empty()) {
            statusText = ownedParams->result.success
                ? L"Offline notice dismissed."
                : L"Offline notice dismissal failed.";
        }
        setStatusText(statusText);
    }

    void createMainControls(HWND hwnd) {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        auto add = [instance](HWND parent, LPCWSTR type, LPCWSTR text, DWORD style = 0, int id = 0) {
            return CreateWindowExW(0, type, text, WS_CHILD | style, 0, 0, 0, 0, parent,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        };

        titleLabel = add(hwnd, L"STATIC", L"JiStats", WS_VISIBLE);
        historyButton =
            add(hwnd, L"BUTTON", L"&History...", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kOpenAnalysisWindowButtonId);
        dashboardSettingsButton =
            add(hwnd, L"BUTTON", L"&Settings...", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kMenuFileSettingsId);
        mainTab =
            add(hwnd, WC_TABCONTROLW, L"", WS_VISIBLE | WS_TABSTOP | TCS_FIXEDWIDTH | TCS_OWNERDRAWFIXED, kMainTabId);
        desktopTheme.styleTab(mainTab);
        const wchar_t* pageLabels[] = {L"Overview", L"Provider Details"};
        for (int index = 0; index < static_cast<int>(std::size(pageLabels)); ++index) {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<LPWSTR>(pageLabels[index]);
            SendMessageW(mainTab, TCM_INSERTITEMW, index, reinterpret_cast<LPARAM>(&item));
        }
        TabCtrl_SetCurSel(mainTab, selectedMainTab);

        dashboardViewport = CreateWindowExW(WS_EX_CONTROLPARENT, viewportClassName(), L"",
                                            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_VSCROLL, 0, 0, 0, 0, hwnd,
                                            nullptr, instance, this);
        const HWND page = dashboardViewport;
        appGroup = add(page, L"STATIC", L"System status");
        connectionLabel = add(page, L"STATIC", L"Connection", SS_NOTIFY);
        connectionValue = add(page, L"STATIC", L"", SS_NOTIFY | SS_ENDELLIPSIS);
        recoveryLabel = add(page, L"STATIC", L"Recovery", SS_NOTIFY);
        recoveryValue = add(page, L"STATIC", L"", SS_NOTIFY | SS_ENDELLIPSIS | SS_NOPREFIX);
        scheduledExportLabel = add(page, L"STATIC", L"Scheduled export", SS_NOTIFY);
        scheduledExportValue = add(page, L"STATIC", L"", SS_NOTIFY | SS_ENDELLIPSIS | SS_NOPREFIX);
        updatedLabel = add(page, L"STATIC", L"Last updated", SS_NOTIFY);
        updatedValue = add(page, L"STATIC", L"", SS_NOTIFY | SS_ENDELLIPSIS);
        offlineNoticeTitle = add(page, L"STATIC", L"", SS_NOPREFIX);
        offlineNoticeMessage = add(page, L"STATIC", L"", SS_NOPREFIX);
        offlineRetryButton =
            add(page, L"BUTTON", L"Retry Now", WS_TABSTOP | BS_PUSHBUTTON | BS_NOTIFY, kOfflineRetryButtonId);
        offlineDismissButton =
            add(page, L"BUTTON", L"Dismiss", WS_TABSTOP | BS_PUSHBUTTON | BS_NOTIFY, kOfflineDismissButtonId);
        usageGroup = add(page, L"STATIC", L"Subscription quota");
        quotaEmptyText =
            add(page, L"STATIC", L"No subscription quota reported yet. Manage provider tracking in Settings.");
        activityGroup = add(page, L"STATIC", L"Recorded activity");
        activityText = add(page, L"STATIC", L"", SS_NOPREFIX);

        codexThroughputGroup = add(page, L"STATIC", L"Codex throughput");
        codexThroughputText = add(page, L"STATIC", L"", SS_OWNERDRAW);
        codexThroughputAveragesText = add(page, L"STATIC", L"", SS_OWNERDRAW);
        codexThroughputTotalsText = add(page, L"STATIC", L"", SS_OWNERDRAW);
        openAiUsageGroup = add(page, L"STATIC", L"OpenAI Codex usage by model");
        openAiUsageText = add(page, L"STATIC", L"", SS_OWNERDRAW | SS_NOPREFIX);
        hicapUsageGroup = add(page, L"STATIC", L"HICAP Codex usage by model");
        hicapUsageText = add(page, L"STATIC", L"", SS_OWNERDRAW | SS_NOPREFIX);

        ensureCodexChartClass();
        auto addChart = [&]() {
            return CreateWindowExW(0, L"JistatsCodexChart", L"", WS_CHILD, 0, 0, 0, 0, page, nullptr, instance, this);
        };
        codexThroughputChart = addChart();
        openAiUsageChart = addChart();
        hicapUsageChart = addChart();

        providersGroup = add(page, L"STATIC", L"Provider details");
        providerList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                       WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0, 0, 0,
                                       0, page, nullptr, instance, nullptr);
        ListView_SetExtendedListViewStyle(providerList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        statusBar = add(hwnd, STATUSCLASSNAMEW, L"", WS_VISIBLE);
        applyDashboardFonts();

        for (int index = 0;
             index < static_cast<int>(sizeof(kProviderQuotaColumns) / sizeof(kProviderQuotaColumns[0]));
             ++index) {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
            column.fmt = LVCFMT_LEFT;
            column.pszText = const_cast<LPWSTR>(kProviderQuotaColumns[index].title);
            column.cx = scaledPixels(kProviderQuotaColumns[index].width, providerList);
            column.iSubItem = index;
            SendMessageW(
                providerList,
                LVM_INSERTCOLUMNW,
                static_cast<WPARAM>(index),
                reinterpret_cast<LPARAM>(&column));
        }

        configureProviderListColumns();
        ensureConnectionTooltip();
        ensureUsageTooltip();
    }

    void layoutMainControls() {
        if (dashboardViewport == nullptr) {
            return;
        }

        const auto px = [this](int value) { return scaledPixels(value); };
        RECT client{};
        GetClientRect(window, &client);
        const int width = client.right;
        const int margin = px(12);
        const int navTop = px(54);
        const int viewportTop = px(90);
        const int actionWidth = std::min(px(98), std::max(0, (width - 4 * margin) / 3));
        const int settingsLeft = width - margin - actionWidth;
        const int historyLeft = settingsLeft - margin - actionWidth;

        SendMessageW(statusBar, WM_SIZE, 0, 0);
        RECT status{};
        GetClientRect(statusBar, &status);
        const int viewportHeight = std::max(0, static_cast<int>(client.bottom - status.bottom) - viewportTop);
        MoveWindow(titleLabel, margin, px(12), std::max(0, historyLeft - margin * 2), px(28), FALSE);
        MoveWindow(historyButton, historyLeft, px(13), actionWidth, px(26), FALSE);
        MoveWindow(dashboardSettingsButton, settingsLeft, px(13), actionWidth, px(26), FALSE);
        MoveWindow(mainTab, margin, navTop, std::min(px(282), std::max(0, width - margin * 2)), px(30), FALSE);
        MoveWindow(dashboardViewport, 0, viewportTop, width, viewportHeight, FALSE);

        if (selectedMainTab == 0) {
            layoutOverview(width, viewportHeight);
        } else {
            ShowScrollBar(dashboardViewport, SB_VERT, FALSE);
            const int panelWidth = std::max(0, width - margin * 2);
            const int inset = px(10);
            const int innerWidth = std::max(0, panelWidth - inset * 2);
            const int panelHeight = std::max(0, viewportHeight - px(12));
            otherPageCard = {margin, px(2), margin + panelWidth, px(2) + panelHeight};
            MoveWindow(providersGroup, margin + inset, px(12), innerWidth, px(18), FALSE);
            MoveWindow(providerList, margin + inset, px(34), innerWidth, std::max(0, panelHeight - px(44)), FALSE);
        }
        syncMainTabVisibility();
        mainControlsLaidOut = true;
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    void layoutOverview(int width, int height) {
        const auto px = [this](int value) { return scaledPixels(value); };
        const int left = px(12);
        const int gap = px(10);
        const int inset = px(12);
        const int cardWidth =
            std::max(0, width - left * 2 - GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(window)));
        const int innerWidth = std::max(0, cardWidth - inset * 2);
        const int innerLeft = left + inset;
        const bool notice = overviewLayout.offlineNoticeVisible;
        const bool retry = overviewLayout.offlineRetryAvailable;
        const int buttonWidth = px(86);
        const int noticeTextWidth = std::max(0, innerWidth - (retry ? 2 : 1) * (buttonWidth + gap) - px(24));
        int noticeHeight = 0;
        if (notice) {
            HDC dc = GetDC(dashboardViewport);
            HGDIOBJ oldFont = SelectObject(dc, dashboardBodyFont);
            RECT measured{0, 0, noticeTextWidth, 0};
            DrawTextW(dc, overviewLayout.offlineNoticeMessage.c_str(), -1, &measured,
                      DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(dc, oldFont);
            ReleaseDC(dashboardViewport, dc);
            noticeHeight = std::max(px(56), px(33) + static_cast<int>(measured.bottom));
        }

        const bool stackQuota = innerWidth < px(540);
        const int quotaRowHeight = px(stackQuota ? 83 : 56);
        const int quotaTop = px(3) + (notice ? noticeHeight + gap : 0);
        const int quotaHeight = px(45) + static_cast<int>(quotaRowCount) * quotaRowHeight;
        const int activityTop = quotaTop + quotaHeight + gap;
        const bool showActivity = overviewLayout.activityRows > 0;
        const int activityHeight = px(34 + 17 * overviewLayout.activityRows);
        const int statusTop = activityTop + (showActivity ? activityHeight + gap : 0);
        const int statusColumns = innerWidth >= px(720) ? 4 : 2;
        const int statusRowsHeight = (4 / statusColumns) * px(40);
        const int statusHeight = px(39) + statusRowsHeight + (!notice && retry ? px(30) : 0);
        const int throughputTop = statusTop + statusHeight + gap;
        const int summaryColumns = innerWidth >= px(680) ? 3 : 2;
        const int summaryHeight = px(96);
        const int summaryBlockHeight = summaryColumns == 3 ? summaryHeight : 2 * summaryHeight + gap;
        const int throughputHeight = px(181) + summaryBlockHeight;
        const bool twoColumns = cardWidth >= px(830);
        const auto summaryTextHeight = [&](int rows) { return px(std::max(34, (rows + 1) * 16 + 4)); };
        int openAiSummaryHeight = summaryTextHeight(overviewLayout.openAiModelRows);
        int hicapSummaryHeight = summaryTextHeight(overviewLayout.hicapModelRows);
        if (twoColumns) {
            openAiSummaryHeight = hicapSummaryHeight = std::max(openAiSummaryHeight, hicapSummaryHeight);
        }
        const int openAiModelHeight = px(190) + openAiSummaryHeight;
        const int hicapModelHeight = px(190) + hicapSummaryHeight;
        const int modelWidth = twoColumns ? (cardWidth - gap) / 2 : cardWidth;
        const int modelTop = throughputTop + throughputHeight + gap;
        const int secondModelTop = twoColumns ? modelTop : modelTop + openAiModelHeight + gap;
        const int contentHeight = secondModelTop + hicapModelHeight + px(14);

        overviewScroll = std::clamp(overviewScroll, 0, std::max(0, contentHeight - height));
        SCROLLINFO scroll{};
        scroll.cbSize = sizeof(scroll);
        scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        scroll.nMax = contentHeight - 1;
        scroll.nPage = static_cast<UINT>(height);
        scroll.nPos = overviewScroll;
        SetScrollInfo(dashboardViewport, SB_VERT, &scroll, TRUE);
        ShowScrollBar(dashboardViewport, SB_VERT, contentHeight > height);

        const auto move = [&](HWND control, int x, int y, int w, int h) {
            MoveWindow(control, x, y - overviewScroll, w, h, FALSE);
        };
        const auto card = [&](size_t index, HWND title, int x, int y, int w, int h) {
            overviewCards[index] = {x, y - overviewScroll, x + w, y + h - overviewScroll};
            move(title, x + inset, y + px(8), std::max(0, w - inset * 2), px(20));
        };

        noticeRect = {};
        const int buttonX = innerLeft + innerWidth - buttonWidth;
        if (notice) {
            const int noticeTop = px(3);
            const int buttonY = noticeTop + (noticeHeight - px(26)) / 2;
            noticeRect = {left, noticeTop - overviewScroll, left + cardWidth,
                          noticeTop + noticeHeight - overviewScroll};
            move(offlineNoticeTitle, innerLeft, noticeTop + px(7), noticeTextWidth, px(18));
            move(offlineNoticeMessage, innerLeft, noticeTop + px(26), noticeTextWidth, noticeHeight - px(27));
            move(offlineDismissButton, buttonX, buttonY, buttonWidth, px(26));
            if (retry) {
                move(offlineRetryButton, buttonX - gap - buttonWidth, buttonY, buttonWidth, px(26));
            }
        } else if (retry) {
            move(offlineRetryButton, buttonX, statusTop + px(26) + statusRowsHeight, buttonWidth, px(26));
        }

        card(0, usageGroup, left, quotaTop, cardWidth, quotaHeight);
        move(quotaEmptyText, innerLeft, quotaTop + px(25), innerWidth, px(16));
        const int quotaLabelWidth = stackQuota ? innerWidth : std::clamp(innerWidth / 3, px(177), px(310));
        const int barOffset = stackQuota ? 0 : quotaLabelWidth + gap;
        for (size_t index = 0; index < quotaRowCount; ++index) {
            const auto& row = usageRows[index];
            const int y = quotaTop + px(32) + static_cast<int>(index) * quotaRowHeight;
            move(row.label, innerLeft, y, quotaLabelWidth, px(17));
            move(row.status, innerLeft, y + px(18), quotaLabelWidth, px(16));
            move(row.bar, innerLeft + barOffset, y + (stackQuota ? px(35) : 0), innerWidth - barOffset, px(45));
        }

        overviewCards[1] = {};
        if (showActivity) {
            card(1, activityGroup, left, activityTop, cardWidth, activityHeight);
            move(activityText, innerLeft, activityTop + px(27), innerWidth, activityHeight - px(30));
        }

        card(2, appGroup, left, statusTop, cardWidth, statusHeight);
        const HWND labels[] = {connectionLabel, recoveryLabel, scheduledExportLabel, updatedLabel};
        const HWND values[] = {connectionValue, recoveryValue, scheduledExportValue, updatedValue};
        const int columnWidth = innerWidth / statusColumns;
        for (int index = 0; index < 4; ++index) {
            const int x = innerLeft + (index % statusColumns) * columnWidth;
            const int y = statusTop + px(33) + (index / statusColumns) * px(40);
            move(labels[index], x, y, std::max(0, columnWidth - px(8)), px(18));
            move(values[index], x, y + px(18), std::max(0, columnWidth - px(8)), px(18));
        }

        card(3, codexThroughputGroup, left, throughputTop, cardWidth, throughputHeight);
        const HWND summaries[] = {codexThroughputText, codexThroughputAveragesText, codexThroughputTotalsText};
        const int summaryWidth = std::max(0, (innerWidth - (summaryColumns - 1) * gap) / summaryColumns);
        for (int index = 0; index < 3; ++index) {
            const int x = innerLeft + (index % summaryColumns) * (summaryWidth + gap);
            const int y = throughputTop + px(31) + (index / summaryColumns) * (summaryHeight + gap);
            const int w = (index == 2 || index == summaryColumns - 1) ? innerLeft + innerWidth - x : summaryWidth;
            move(summaries[index], x, y, std::max(0, w), summaryHeight);
        }
        move(codexThroughputChart, innerLeft, throughputTop + px(38) + summaryBlockHeight, innerWidth, px(132));

        const int secondX = twoColumns ? left + cardWidth - modelWidth : left;
        const auto modelCard = [&](size_t index, HWND title, HWND text, HWND chart, int x, int y, int summarySize,
                                   int cardHeight) {
            card(index, title, x, y, modelWidth, cardHeight);
            move(text, x + inset, y + px(31), innerWidth - (cardWidth - modelWidth), summarySize);
            move(chart, x + inset, y + px(31) + summarySize, innerWidth - (cardWidth - modelWidth), px(144));
        };
        modelCard(4, openAiUsageGroup, openAiUsageText, openAiUsageChart, left, modelTop, openAiSummaryHeight,
                  openAiModelHeight);
        modelCard(5, hicapUsageGroup, hicapUsageText, hicapUsageChart, secondX, secondModelTop, hicapSummaryHeight,
                  hicapModelHeight);
    }

    void scrollOverview(UINT command) {
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_ALL;
        if (selectedMainTab != 0 || !GetScrollInfo(dashboardViewport, SB_VERT, &info)) {
            return;
        }
        int next = overviewScroll;
        switch (command) {
            case SB_LINEUP:
                next -= scaledPixels(32);
                break;
            case SB_LINEDOWN:
                next += scaledPixels(32);
                break;
            case SB_PAGEUP:
                next -= static_cast<int>(info.nPage);
                break;
            case SB_PAGEDOWN:
                next += static_cast<int>(info.nPage);
                break;
            case SB_THUMBTRACK:
                next = info.nTrackPos;
                break;
            case SB_TOP:
                next = 0;
                break;
            case SB_BOTTOM:
                next = info.nMax;
                break;
            default:
                return;
        }
        setOverviewScroll(next, info);
    }

    void scrollOverviewWheel(int delta) {
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_RANGE | SIF_PAGE;
        if (!GetScrollInfo(dashboardViewport, SB_VERT, &info)) {
            return;
        }
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        wheelDelta += delta;
        const int ticks = wheelDelta / WHEEL_DELTA;
        wheelDelta %= WHEEL_DELTA;
        const int distance =
            lines == WHEEL_PAGESCROLL ? static_cast<int>(info.nPage) : scaledPixels(24) * static_cast<int>(lines);
        setOverviewScroll(overviewScroll - ticks * distance, info);
    }

    void setOverviewScroll(int next, const SCROLLINFO& info) {
        next = std::clamp(next, 0, std::max(0, info.nMax - static_cast<int>(info.nPage) + 1));
        const int offset = overviewScroll - next;
        if (offset == 0) {
            return;
        }

        overviewScroll = next;
        dismissUsageTooltip();
        HDWP positions = BeginDeferWindowPos(32);
        for (HWND child = GetWindow(dashboardViewport, GW_CHILD); child != nullptr && positions != nullptr;
             child = GetWindow(child, GW_HWNDNEXT)) {
            if (!(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) {
                continue;
            }
            RECT rect{};
            GetWindowRect(child, &rect);
            MapWindowPoints(HWND_DESKTOP, dashboardViewport, reinterpret_cast<POINT*>(&rect), 2);
            positions = DeferWindowPos(positions, child, nullptr, rect.left, rect.top + offset, 0, 0,
                                       SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
        }
        if (positions == nullptr || !EndDeferWindowPos(positions)) {
            layoutMainControls();
            return;
        }

        for (auto& card : overviewCards) {
            OffsetRect(&card, 0, offset);
        }
        OffsetRect(&noticeRect, 0, offset);
        SCROLLINFO position{};
        position.cbSize = sizeof(position);
        position.fMask = SIF_POS;
        position.nPos = next;
        SetScrollInfo(dashboardViewport, SB_VERT, &position, TRUE);
        RedrawWindow(dashboardViewport, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    void revealOverviewControl(HWND control) {
        if (selectedMainTab != 0 || control == nullptr) {
            return;
        }
        RECT rect{};
        RECT viewport{};
        GetWindowRect(control, &rect);
        GetClientRect(dashboardViewport, &viewport);
        MapWindowPoints(HWND_DESKTOP, dashboardViewport, reinterpret_cast<POINT*>(&rect), 2);
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_RANGE | SIF_PAGE;
        if (GetScrollInfo(dashboardViewport, SB_VERT, &info)) {
            const int padding = scaledPixels(8);
            if (rect.top < padding) {
                setOverviewScroll(overviewScroll + rect.top - padding, info);
            } else if (rect.bottom > viewport.bottom - padding) {
                setOverviewScroll(overviewScroll + rect.bottom - viewport.bottom + padding, info);
            }
        }
    }

    DashboardViewModel copyViewModel() const {
        std::lock_guard<std::mutex> lock(mutex);
        return viewModel;
    }

    void populateProviderList(const DashboardViewModel& model) {
        if (providerList == nullptr) {
            return;
        }

        auto rows = buildProviderQuotaRows(model);
        const bool sameRows =
            rows.size() == providerQuotaRows.size() &&
            std::equal(rows.begin(), rows.end(), providerQuotaRows.begin(),
                       [](const ProviderQuotaRowData& left, const ProviderQuotaRowData& right) {
                           return left.providerKey == right.providerKey && left.accountId == right.accountId;
                       });
        if (!sameRows) {
            ListView_DeleteAllItems(providerList);
        }
        for (int rowIndex = 0; rowIndex < static_cast<int>(rows.size()); ++rowIndex) {
            const auto& row = rows[static_cast<size_t>(rowIndex)];
            int columnIndex = 0;
            if (!sameRows) {
                insertListViewItem(providerList, rowIndex, utf8ToWide(row.columns[0]));
                ++columnIndex;
            }

            for (; columnIndex < static_cast<int>(row.columns.size()); ++columnIndex) {
                if (!sameRows || row.columns[columnIndex] != providerQuotaRows[rowIndex].columns[columnIndex]) {
                    setListViewItemText(providerList, rowIndex, columnIndex, utf8ToWide(row.columns[columnIndex]));
                }
            }
        }
        providerQuotaRows = std::move(rows);

        restoreProviderListSelection(model.preferences);
    }

    int findProviderQuotaRowIndex(const DashboardPreferences& preferences) const {
        if (preferences.lastSelectedProviderId.empty()) {
            return -1;
        }

        for (size_t index = 0; index < providerQuotaRows.size(); ++index) {
            const auto& row = providerQuotaRows[index];
            if (row.providerKey != preferences.lastSelectedProviderId) {
                continue;
            }
            if (!preferences.lastSelectedAccountId.empty() &&
                row.accountId != preferences.lastSelectedAccountId) {
                continue;
            }
            return static_cast<int>(index);
        }

        return -1;
    }

    void restoreProviderListSelection(const DashboardPreferences& preferences) {
        if (providerList == nullptr) {
            return;
        }

        const int matchIndex = findProviderQuotaRowIndex(preferences);
        const int selected = ListView_GetNextItem(providerList, -1, LVNI_SELECTED);
        if (selected == matchIndex ||
            (selected >= 0 && matchIndex >= 0 &&
             providerQuotaRows[selected].providerKey == providerQuotaRows[matchIndex].providerKey &&
             providerQuotaRows[selected].accountId == providerQuotaRows[matchIndex].accountId)) {
            return;
        }

        suppressProviderSelectionNotification = true;
        ListView_SetItemState(
            providerList,
            -1,
            0,
            LVIS_SELECTED | LVIS_FOCUSED);
        if (matchIndex >= 0) {
            ListView_SetItemState(providerList, matchIndex, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(providerList, matchIndex, FALSE);
        }
        suppressProviderSelectionNotification = false;
    }

    void handleProviderQuotaRowSelection(int rowIndex) {
        if (rowIndex < 0 || rowIndex >= static_cast<int>(providerQuotaRows.size())) {
            return;
        }
        if (suppressProviderSelectionNotification) {
            return;
        }

        const auto& selectedRow = providerQuotaRows[static_cast<size_t>(rowIndex)];
        editPreferences(
            [&selectedRow](DashboardPreferences& preferences) {
                if (preferences.lastSelectedProviderId == selectedRow.providerKey &&
                    preferences.lastSelectedAccountId == selectedRow.accountId) {
                    return false;
                }
                preferences.lastSelectedProviderId = selectedRow.providerKey;
                preferences.lastSelectedAccountId = selectedRow.accountId;
                return true;
            },
            true);
    }

    void syncPreferenceEditor(HWND editor, const std::string& value) {
        if (SendMessageW(editor, EM_GETMODIFY, 0, 0) == FALSE && getWindowTextUtf8(editor) != value) {
            setWindowText(editor, utf8ToWide(value));
        }
    }

    void syncPreferencesWindowState() {
        DashboardViewModel model;
        bool preferencesPending = false;
        bool preferencesFailed = false;
        HWND overlay = nullptr;
        HWND openaiSubscription = nullptr;
        HWND codexModelAttribution = nullptr;
        bool attributionChecked = false;
        HWND openaiInfo = nullptr;
        HWND startup = nullptr;
        HWND startupBackground = nullptr;
        HWND scheduledExportToggle = nullptr;
        HWND scheduledExportInterval = nullptr;
        HWND scheduledExportPath = nullptr;
        HWND scheduledExportLastRun = nullptr;
        HWND miniMaxSubscription = nullptr;
        HWND codingStatus = nullptr;
        HWND codingDetail = nullptr;
        HWND tokenStatus = nullptr;
        HWND tokenDetail = nullptr;
        HWND claudeToggle = nullptr;
        HWND claudeStatusLabel = nullptr;
        HWND claudeDetailLabel = nullptr;
        HWND kimiToggle = nullptr;
        HWND kimiStatusLabel = nullptr;
        HWND kimiDetailLabel = nullptr;
        HWND planCombo = nullptr;
        HWND accountEdit = nullptr;
        HWND saveButton = nullptr;
        HWND validateButton = nullptr;
        HWND feedbackLabel = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            model = viewModel;
            preferencesPending = model.completedPreferencesRequest < preferencesRequest;
            preferencesFailed = preferencesPending && model.failedPreferencesRequest == preferencesRequest;
            overlay = overlayCheckbox;
            openaiSubscription = openaiSubscriptionCheckbox;
            codexModelAttribution = codexModelAttributionCheckbox;
            attributionChecked = model.completedCodexAttributionRequest < codexAttributionRequest
                                     ? requestedCodexAttributionEnabled
                                     : model.preferences.codexModelAttributionEnabled;
            openaiInfo = openAISettingsInfo;
            startup = startupCheckbox;
            startupBackground = startupBackgroundCheckbox;
            scheduledExportToggle = scheduledExportCheckbox;
            scheduledExportInterval = scheduledExportIntervalEdit;
            scheduledExportPath = scheduledExportPathEdit;
            scheduledExportLastRun = scheduledExportLastRunValue;
            miniMaxSubscription = miniMaxSubscriptionCheckbox;
            codingStatus = miniMaxCodingStatus;
            codingDetail = miniMaxCodingDetail;
            tokenStatus = miniMaxTokenStatus;
            tokenDetail = miniMaxTokenDetail;
            claudeToggle = claudeCodeEnableCheckbox;
            claudeStatusLabel = claudeCodeStatus;
            claudeDetailLabel = claudeCodeDetail;
            kimiToggle = kimiCodeEnableCheckbox;
            kimiStatusLabel = kimiCodeStatus;
            kimiDetailLabel = kimiCodeDetail;
            planCombo = miniMaxPlanCombo;
            accountEdit = miniMaxAccountEdit;
            saveButton = miniMaxSaveButton;
            validateButton = miniMaxValidateButton;
            feedbackLabel = miniMaxFeedbackLabel;
        }

        const wchar_t* saveStatus = L"";
        if (preferencesFailed) {
            saveStatus = L"Could not save changes.";
        } else if (preferencesPending) {
            saveStatus = L"Applying changes...";
        } else if (model.preferencesSessionOnly) {
            saveStatus = L"Storage unavailable. Changes apply for this session.";
        }
        setWindowText(preferencesSaveStatus, saveStatus);
        if (preferencesRetryButton != nullptr) {
            ShowWindow(preferencesRetryButton, preferencesFailed ? SW_SHOWNA : SW_HIDE);
        }
        if (overlay != nullptr) {
            SendMessageW(
                overlay,
                BM_SETCHECK,
                model.preferences.taskbarOverlayEnabled ? BST_CHECKED : BST_UNCHECKED,
                0);
        }
        if (openaiSubscription != nullptr) {
            SendMessageW(
                openaiSubscription,
                BM_SETCHECK,
                model.preferences.openaiSubscriptionTrackingEnabled ? BST_CHECKED : BST_UNCHECKED,
                0);
        }
        if (openaiInfo != nullptr) {
            setWindowText(openaiInfo, codexAttributionSettingsInfo(model.codexAttributionSaveError));
        }
        if (codexModelAttribution != nullptr) {
            SendMessageW(codexModelAttribution, BM_SETCHECK, attributionChecked ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        if (miniMaxSubscription != nullptr) {
            SendMessageW(
                miniMaxSubscription,
                BM_SETCHECK,
                model.preferences.miniMaxSubscriptionTrackingEnabled ? BST_CHECKED : BST_UNCHECKED,
                0);
        }
        if (startup != nullptr) {
            SendMessageW(
                startup,
                BM_SETCHECK,
                model.preferences.startupEnabled ? BST_CHECKED : BST_UNCHECKED,
                0);
        }
        if (startupBackground != nullptr) {
            SendMessageW(startupBackground, BM_SETCHECK,
                model.preferences.startupStartInBackground ? BST_CHECKED : BST_UNCHECKED,
                0);
            EnableWindow(startupBackground, model.preferences.startupEnabled ? TRUE : FALSE);
        }
        if (claudeToggle != nullptr) {
            SendMessageW(
                claudeToggle,
                BM_SETCHECK,
                model.preferences.claudeCodeEnabled ? BST_CHECKED : BST_UNCHECKED,
                0);
        }
        if (claudeStatusLabel != nullptr) {
            setWindowText(
                claudeStatusLabel,
                utf8ToWide(localProviderSettingsStatusText(model, "claude_code")));
        }
        if (claudeDetailLabel != nullptr) {
            setWindowText(
                claudeDetailLabel,
                utf8ToWide(localProviderSettingsDetailText(model, "claude_code")));
        }
        if (grokSubscriptionCheckbox != nullptr) {
            SendMessageW(grokSubscriptionCheckbox, BM_SETCHECK,
                         model.preferences.grokSubscriptionTrackingEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        if (cursorSubscriptionCheckbox != nullptr) {
            SendMessageW(cursorSubscriptionCheckbox, BM_SETCHECK,
                         model.preferences.cursorSubscriptionTrackingEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        if (kimiToggle != nullptr) {
            SendMessageW(
                kimiToggle,
                BM_SETCHECK,
                model.preferences.kimiCodeEnabled ? BST_CHECKED : BST_UNCHECKED,
                0);
        }
        if (kimiStatusLabel != nullptr) {
            setWindowText(
                kimiStatusLabel,
                utf8ToWide(localProviderSettingsStatusText(model, "kimi")));
        }
        if (kimiDetailLabel != nullptr) {
            setWindowText(
                kimiDetailLabel,
                utf8ToWide(localProviderSettingsDetailText(model, "kimi")));
        }
        if (scheduledExportToggle != nullptr) {
            SendMessageW(
                scheduledExportToggle,
                BM_SETCHECK,
                model.preferences.scheduledExportEnabled ? BST_CHECKED : BST_UNCHECKED,
                0);
        }
        if (scheduledExportInterval != nullptr) {
            syncPreferenceEditor(scheduledExportInterval,
                                 std::to_string(std::max(1, model.preferences.scheduledExportIntervalMinutes)));
            EnableWindow(scheduledExportInterval, model.preferences.scheduledExportEnabled ? TRUE : FALSE);
        }
        if (scheduledExportPath != nullptr) {
            syncPreferenceEditor(scheduledExportPath, model.preferences.scheduledExportPath);
            EnableWindow(scheduledExportPath, model.preferences.scheduledExportEnabled ? TRUE : FALSE);
        }
        if (scheduledExportLastRun != nullptr) {
            const std::string lastRun =
                model.scheduledExportLastRunTime.empty()
                    ? "Never"
                    : localizeDisplayTimestamp(model.scheduledExportLastRunTime);
            setWindowText(scheduledExportLastRun, utf8ToWide(lastRun));
            EnableWindow(scheduledExportLastRun, model.preferences.scheduledExportEnabled ? TRUE : FALSE);
        }

        const auto* codingSummary = findMiniMaxCredentialSummary(model, "coding_plan");
        const auto* tokenSummary = findMiniMaxCredentialSummary(model, "token_plan");
        if (codingStatus != nullptr) {
            setWindowText(
                codingStatus,
                utf8ToWide("Coding Plan: " + miniMaxCredentialStatusLabel(codingSummary)));
        }
        if (codingDetail != nullptr) {
            setWindowText(codingDetail, utf8ToWide(miniMaxCredentialDetailText(codingSummary)));
        }
        if (tokenStatus != nullptr) {
            setWindowText(
                tokenStatus,
                utf8ToWide("Token Plan: " + miniMaxCredentialStatusLabel(tokenSummary)));
        }
        if (tokenDetail != nullptr) {
            setWindowText(tokenDetail, utf8ToWide(miniMaxCredentialDetailText(tokenSummary)));
        }

        if (planCombo != nullptr) {
            LRESULT selection = SendMessageW(planCombo, CB_GETCURSEL, 0, 0);
            if (selection == CB_ERR) {
                selection = 0;
                SendMessageW(planCombo, CB_SETCURSEL, selection, 0);
            }

            const char* selectedPlanKey = selection == 1 ? "token_plan" : "coding_plan";
            const auto* selectedSummary = findMiniMaxCredentialSummary(model, selectedPlanKey);
            if (accountEdit != nullptr) {
                const std::string accountDisplayName =
                    selectedSummary != nullptr ? selectedSummary->accountDisplayName : "";
                if (getWindowTextUtf8(accountEdit) == accountDisplayName) {
                    SendMessageW(accountEdit, EM_SETMODIFY, FALSE, 0);
                }
                syncPreferenceEditor(accountEdit, accountDisplayName);
            }
            const bool validationPending =
                selectedSummary != nullptr && selectedSummary->validationState == "pending";
            if (const HWND focus = GetFocus();
                validationPending && focus != nullptr && (focus == saveButton || focus == validateButton)) {
                SetFocus(preferencesCloseButton);
            }
            if (saveButton != nullptr) {
                EnableWindow(saveButton, validationPending ? FALSE : TRUE);
            }
            if (validateButton != nullptr) {
                EnableWindow(validateButton, validationPending ? FALSE : TRUE);
            }
        }

        if (feedbackLabel != nullptr) {
            std::string feedback;
            {
                std::lock_guard<std::mutex> lock(mutex);
                feedback = miniMaxFeedbackMessage;
            }
            setWindowText(feedbackLabel, utf8ToWide(feedback));
        }
    }

    std::string selectedMiniMaxPlanKey() const {
        if (miniMaxPlanCombo == nullptr) {
            return "coding_plan";
        }

        const LRESULT selection = SendMessageW(miniMaxPlanCombo, CB_GETCURSEL, 0, 0);
        return selection == 1 ? "token_plan" : "coding_plan";
    }

    std::string getWindowTextUtf8(HWND hwnd) const {
        if (hwnd == nullptr) {
            return "";
        }

        const int length = GetWindowTextLengthW(hwnd);
        std::wstring text(static_cast<size_t>(length + 1), L'\0');
        if (length > 0) {
            GetWindowTextW(hwnd, text.data(), length + 1);
        }
        text.resize(static_cast<size_t>(length));
        return wideToUtf8(text);
    }

    void queueMiniMaxCredentialAction(const DashboardMiniMaxCredentialRequest& request) {
        std::function<DashboardMiniMaxCredentialResult(const DashboardMiniMaxCredentialRequest&)> callback;
        {
            std::lock_guard<std::mutex> lock(mutex);
            callback = miniMaxCredentialActionCallback;
        }

        if (!callback) {
            setStatusText(L"MiniMax credential actions are unavailable.");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(miniMaxActionMutex);
            if (miniMaxActionInFlight) {
                setStatusText(L"MiniMax credential action is already running...");
                return;
            }
            miniMaxActionInFlight = true;
        }

        const std::wstring inFlightStatus =
            request.action == DashboardMiniMaxCredentialAction::Save
                ? L"Saving MiniMax credential..."
                : L"Validating MiniMax credential...";
        {
            std::lock_guard<std::mutex> lock(mutex);
            miniMaxFeedbackMessage = wideToUtf8(inFlightStatus);
        }
        requestViewModelSync();
        setStatusText(inFlightStatus);

        const auto submittedRevision = miniMaxAccountEditRevision.load();
        std::lock_guard<std::mutex> lock(miniMaxActionMutex);
        miniMaxActionThreads.emplace_back([this, callback, request, submittedRevision]() {
            DashboardMiniMaxCredentialResult result = callback(request);
            HWND hwnd = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex);
                hwnd = window;
            }

            auto* payload =
                new MiniMaxActionCompletionParams{std::move(result), request.accountDisplayName, submittedRevision};
            if (hwnd == nullptr ||
                PostMessageW(hwnd, WM_JISTATS_MINIMAX_ACTION_COMPLETE, 0, reinterpret_cast<LPARAM>(payload)) == FALSE) {
                delete payload;
                std::lock_guard<std::mutex> actionLock(miniMaxActionMutex);
                miniMaxActionInFlight = false;
            }
        });
    }

    void handleMiniMaxCredentialCompletion(MiniMaxActionCompletionParams* params) {
        std::unique_ptr<MiniMaxActionCompletionParams> ownedParams(params);
        {
            std::lock_guard<std::mutex> lock(miniMaxActionMutex);
            miniMaxActionInFlight = false;
        }

        if (!ownedParams) {
            setStatusText(L"MiniMax credential action finished without a result.");
            return;
        }

        const auto& result = ownedParams->result;
        if (result.success && result.accountDisplayName && miniMaxAccountEdit != nullptr &&
            miniMaxAccountEditRevision.load() == ownedParams->submittedAccountEditRevision &&
            selectedMiniMaxPlanKey() == result.planKey &&
            getWindowTextUtf8(miniMaxAccountEdit) == ownedParams->submittedAccountName) {
            setWindowText(miniMaxAccountEdit, *result.accountDisplayName);
            SendMessageW(miniMaxAccountEdit, EM_SETMODIFY, FALSE, 0);
        }

        std::wstring statusText = utf8ToWide(ownedParams->result.message);
        if (statusText.empty()) {
            statusText = ownedParams->result.success
                ? L"MiniMax credential action completed."
                : L"MiniMax credential action failed.";
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            miniMaxFeedbackMessage = ownedParams->result.message;
        }
        requestViewModelSync();
        setStatusText(statusText);
    }

    void handleMiniMaxCredentialAction(DashboardMiniMaxCredentialAction action) {
        DashboardMiniMaxCredentialRequest request;
        request.action = action;
        request.planKey = selectedMiniMaxPlanKey();
        request.accountDisplayName = getWindowTextUtf8(miniMaxAccountEdit);
        request.secret = getWindowTextUtf8(miniMaxSecretEdit);

        queueMiniMaxCredentialAction(request);
    }

    ShellDialogState* dialogStateFor(DashboardShellDialog dialog) {
        switch (dialog) {
        case DashboardShellDialog::Help:
            return &helpDialog;
        case DashboardShellDialog::About:
            return &aboutDialog;
        default:
            return nullptr;
        }
    }

    const ShellDialogState* dialogStateFor(DashboardShellDialog dialog) const {
        switch (dialog) {
        case DashboardShellDialog::Help:
            return &helpDialog;
        case DashboardShellDialog::About:
            return &aboutDialog;
        default:
            return nullptr;
        }
    }

    std::wstring shellDialogTitle(DashboardShellDialog dialog) const {
        switch (dialog) {
        case DashboardShellDialog::Help:
            return L"JiStats Help";
        case DashboardShellDialog::About:
            return L"About JiStats";
        default:
            return L"JiStats";
        }
    }

    std::wstring shellDialogBody(DashboardShellDialog dialog) const {
        DashboardViewModel model = copyViewModel();
        switch (dialog) {
        case DashboardShellDialog::Help:
            return L"Use File > Settings to change runtime preferences.\r\n\r\n"
                L"Close the dashboard window to hide it to the tray, or use File > Exit when you want to stop JiStats.";
        case DashboardShellDialog::About:
            return utf8ToWide(buildAboutDialogBodyText(model));
        default:
            return L"";
        }
    }

    void syncShellDialogContent(DashboardShellDialog dialog) {
        const ShellDialogState* state = dialogStateFor(dialog);
        if (state == nullptr || state->window == nullptr) {
            return;
        }

        setWindowText(state->window, shellDialogTitle(dialog));
        if (dialog == DashboardShellDialog::About) {
            setWindowText(state->headerTitle, L"JiStats");
            setWindowText(state->headerSubtitle, utf8ToWide(aboutDialogSubtitleText()));
        }
        setWindowText(state->bodyLabel, shellDialogBody(dialog));
    }

    void notifyExitRequested() {
        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> lock(mutex);
            callback = exitRequestedCallback;
        }

        if (callback) {
            callback();
        }
    }

    void ensureUsageRows(size_t count) {
        const auto create = [this](DWORD style, int id = 0) {
            return CreateWindowExW(0, L"STATIC", L"", WS_CHILD | style, 0, 0, 0, 0, dashboardViewport,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr),
                                   nullptr);
        };
        while (usageRows.size() < count) {
            UsageProgressRow row;
            row.label = create(SS_NOTIFY | SS_NOPREFIX | SS_ENDELLIPSIS);
            row.status = create(SS_NOTIFY | SS_NOPREFIX | SS_ENDELLIPSIS);
            row.bar = create(SS_OWNERDRAW | SS_NOTIFY, kUsageBarBaseId + static_cast<int>(usageRows.size()));
            if (row.label == nullptr || row.status == nullptr || row.bar == nullptr) {
                for (HWND control : {row.label, row.status, row.bar}) {
                    if (control != nullptr) {
                        DestroyWindow(control);
                    }
                }
                return;
            }
            SendMessageW(row.label, WM_SETFONT, reinterpret_cast<WPARAM>(dashboardLabelFont), FALSE);
            SendMessageW(row.status, WM_SETFONT, reinterpret_cast<WPARAM>(dashboardBodyFont), FALSE);
            SendMessageW(row.bar, WM_SETFONT, reinterpret_cast<WPARAM>(dashboardBodyFont), FALSE);
            usageRows.push_back(std::move(row));
            for (HWND control : {usageRows.back().label, usageRows.back().status, usageRows.back().bar}) {
                configureUsageTooltipTool(control);
            }
        }
    }

    bool syncUsageProgress(const DashboardViewModel& model) {
        const auto summaries = buildUsageProgressSummaries(model);
        ensureUsageRows(summaries.size());
        const size_t previousCount = quotaRowCount;
        quotaRowCount = std::min(summaries.size(), usageRows.size());

        const auto colorForPercentage = [](int percentage) {
            if (percentage >= 50) {
                return RGB(54, 128, 28);
            }
            if (percentage >= 20) {
                return RGB(173, 105, 14);
            }
            return RGB(181, 49, 33);
        };
        for (size_t index = 0; index < quotaRowCount; ++index) {
            auto& row = usageRows[index];
            const auto& summary = summaries[index];
            setWindowText(row.label, summary.label);
            setWindowText(row.status, summary.status);
            row.overlayText = utf8ToWide(summary.overlayText);
            row.secondaryOverlayText = utf8ToWide(summary.secondaryOverlayText);
            row.resetText = utf8ToWide(summary.resetText);
            row.secondaryResetText = utf8ToWide(summary.secondaryResetText);
            row.detailTooltipText = utf8ToWide(summary.detail);
            row.percentage = summary.percentage;
            row.secondaryPercentage = summary.secondaryPercentage;
            row.barColor = colorForPercentage(row.percentage);
            row.secondaryBarColor = colorForPercentage(row.secondaryPercentage);

            std::wstring caption = row.overlayText;
            if (!row.resetText.empty()) {
                caption += L" / " + row.resetText;
            }
            if (row.secondaryPercentage >= 0) {
                caption += L" | " + row.secondaryOverlayText + L" / " + row.secondaryResetText;
            }
            setWindowText(row.bar, caption);
            InvalidateRect(row.bar, nullptr, FALSE);
        }
        return quotaRowCount != previousCount;
    }

    void syncRecordedActivity(const DashboardViewModel& model) {
        std::ostringstream activity;
        for (const auto& summary : model.runtimeSummaries) {
            if (activity.tellp() > 0) {
                activity << "\r\n";
            }
            activity << summary.label;
            if (summary.label == "Today") {
                activity << " (UTC)";
            }
            activity << "   " << summary.detail;
        }
        setWindowText(activityText, activity.str());
    }

    void syncViewModelToControls() {
        const DashboardViewModel model = copyViewModel();

#ifdef JISTATS_ENABLE_TEST_HOOKS
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++viewModelSyncCountForTesting;
        }
#endif

        {
            const std::string title = model.appVersion.empty()
                ? "JiStats"
                : ("JiStats | v" + model.appVersion);
            setWindowText(window, utf8ToWide(title));
        }
        setWindowText(connectionValue,
                      utf8ToWide(model.startupPending ? "Starting..." : runtimeStateLabel(model.runtimeState)));
        if (const HMENU menu = GetMenu(window)) {
            const UINT availability = MF_BYCOMMAND | (model.startupPending ? MF_GRAYED : MF_ENABLED);
            EnableMenuItem(menu, kMenuFileSettingsId, availability);
            EnableMenuItem(menu, kMenuFileImportOpenAIId, availability);
            EnableMenuItem(menu, kMenuFileExportHtmlId, availability);
        }
        EnableWindow(historyButton, !model.startupPending);
        EnableWindow(dashboardSettingsButton, !model.startupPending);
        setWindowText(
            offlineNoticeTitle,
            utf8ToWide(model.offlineNoticeTitle.empty()
                ? "Connection Lost"
                : model.offlineNoticeTitle));
        const auto modelRows = [](const AnalysisProviderModelUsageChart& chart) {
            return chart.hasData ? static_cast<int>(std::min<size_t>(chart.series.size(), 6)) : 0;
        };
        OverviewLayout layout{utf8ToWide(model.offlineNoticeMessage),
                              model.offlineNoticeVisible,
                              model.offlineRetryAvailable,
                              modelRows(model.analysis.openAiCodexModelUsage),
                              modelRows(model.analysis.hicapCodexModelUsage),
                              static_cast<int>(model.runtimeSummaries.size())};
        const bool layoutChanged = !(overviewLayout == layout);
        overviewLayout = std::move(layout);
        setWindowText(offlineNoticeMessage, overviewLayout.offlineNoticeMessage);
        setWindowText(
            recoveryValue,
            utf8ToWide(model.recoveryStatusSummary.empty()
                ? "No recent recovery activity"
                : model.recoveryStatusSummary));
        setWindowText(
            scheduledExportValue,
            utf8ToWide(model.scheduledExportStatusSummary.empty()
                ? "Disabled"
                : model.scheduledExportStatusSummary));
        syncConnectionTooltip(model.runtimeState);
        syncRecoveryTooltip(model);
        syncScheduledExportTooltip(model);
        const CodexThroughputOverviewColumns codexThroughputColumns =
            codexThroughputOverviewColumns(model.analysis.codexThroughput);
        setWindowText(codexThroughputText, utf8ToWide(codexThroughputColumns.recent));
        setWindowText(codexThroughputAveragesText, utf8ToWide(codexThroughputColumns.averages));
        setWindowText(codexThroughputTotalsText, utf8ToWide(codexThroughputColumns.totals));
        const bool attributionEnabled = model.preferences.codexModelAttributionEnabled;
        setWindowText(openAiUsageText,
                      utf8ToWide(modelUsageOverviewText(model.analysis.openAiCodexModelUsage, attributionEnabled)));
        setWindowText(hicapUsageText,
                      utf8ToWide(modelUsageOverviewText(model.analysis.hicapCodexModelUsage, attributionEnabled)));
        updatedTooltipBody = utf8ToWide(model.lastUpdateTime.empty() ? "Waiting for first update"
                                                                     : localizeDisplayTimestamp(model.lastUpdateTime));
        setWindowText(updatedValue, updatedTooltipBody);
        const bool quotaRowsChanged = syncUsageProgress(model);
        syncRecordedActivity(model);
        if (!mainControlsLaidOut || layoutChanged || quotaRowsChanged) {
            layoutMainControls();
        } else {
            invalidateDashboardChartCanvases();
        }
        populateProviderList(model);
        if (analysisPresenter != nullptr && analysisWindow != nullptr) {
            analysisPresenter->updateViewModel(model.analysis);
        }

        std::wstring statusText;
        {
            std::lock_guard<std::mutex> lock(mutex);
            statusText = persistentStatusText;
        }
        SendMessageW(statusBar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(statusText.c_str()));

        if (preferencesWindow != nullptr) {
            syncPreferencesWindowState();
        }
        syncShellDialogContent(DashboardShellDialog::Help);
        syncShellDialogContent(DashboardShellDialog::About);
    }

    void invalidateDashboardChartCanvases() {
        if (selectedMainTab != 0 || !IsWindowVisible(dashboardViewport)) {
            return;
        }
        RECT viewport{};
        GetWindowRect(dashboardViewport, &viewport);
        for (HWND chart : {codexThroughputChart, openAiUsageChart, hicapUsageChart}) {
            RECT bounds{};
            RECT intersection{};
            if (GetWindowRect(chart, &bounds) && IntersectRect(&intersection, &bounds, &viewport)) {
                InvalidateRect(chart, nullptr, FALSE);
            }
        }
    }

    // Call with mutex held.
    bool shouldSyncMainWindow() const {
        return visible || analysisWindow != nullptr || helpDialog.window != nullptr || aboutDialog.window != nullptr;
    }

    void requestViewModelSync() {
        std::lock_guard<std::mutex> lock(mutex);
        if (window != nullptr && !viewModelSyncPending && (shouldSyncMainWindow() || preferencesWindow != nullptr)) {
            viewModelSyncPending = PostMessageW(window, WM_JISTATS_SYNC_VIEWMODEL, 0, 0) != FALSE;
        }
    }

    template <typename Edit>
    bool editPreferences(Edit edit, bool syncDashboard = false, bool affectsProviders = false) {
        DashboardPreferences preferences;
        std::function<void(const DashboardPreferences&)> callback;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!edit(viewModel.preferences)) {
                return false;
            }
            callback = preferencesChangedCallback;
            if (callback) {
                viewModel.preferences.changeRequest = ++preferencesRequest;
                if (affectsProviders) {
                    viewModel.preferences.providerChangeRequest = preferencesRequest;
                }
            }
            preferences = viewModel.preferences;
        }

        if (callback) {
            callback(preferences);
        }
        if (syncDashboard) {
            requestViewModelSync();
        } else {
            syncPreferencesWindowState();
        }
        return true;
    }

    void setPreference(bool DashboardPreferences::*field, bool enabled, bool affectsProviders = false) {
        editPreferences(
            [field, enabled](DashboardPreferences& preferences) {
                return std::exchange(preferences.*field, enabled) != enabled;
            },
            false, affectsProviders);
    }

    void commitScheduledExportSettingsFromControls() {
        if (scheduledExportCheckbox == nullptr) {
            return;
        }

        const bool enabled = SendMessageW(scheduledExportCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const std::string intervalText = getWindowTextUtf8(scheduledExportIntervalEdit);
        const std::string path = getWindowTextUtf8(scheduledExportPathEdit);
        SendMessageW(scheduledExportIntervalEdit, EM_SETMODIFY, FALSE, 0);
        SendMessageW(scheduledExportPathEdit, EM_SETMODIFY, FALSE, 0);
        const bool changed = editPreferences([&](DashboardPreferences& preferences) {
            const int interval =
                parsePositiveIntegerOrDefault(intervalText, std::max(1, preferences.scheduledExportIntervalMinutes));
            const bool changed = preferences.scheduledExportEnabled != enabled ||
                                 preferences.scheduledExportIntervalMinutes != interval ||
                                 preferences.scheduledExportPath != path;
            preferences.scheduledExportEnabled = enabled;
            preferences.scheduledExportIntervalMinutes = interval;
            preferences.scheduledExportPath = path;
            return changed;
        });
        // Restore the accepted interval if invalid text resolved to the existing value.
        if (!changed) {
            std::string effectiveInterval;
            {
                std::lock_guard<std::mutex> lock(mutex);
                effectiveInterval = std::to_string(viewModel.preferences.scheduledExportIntervalMinutes);
            }
            syncPreferenceEditor(scheduledExportIntervalEdit, effectiveInterval);
        }
    }

    void createPreferencesControls(HWND hwnd) {
        preferencesTab = CreateWindowExW(
            0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | TCS_OWNERDRAWFIXED, 12, 12,
            520, 470, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPreferencesTabId)), GetModuleHandleW(nullptr),
            nullptr);
        desktopTheme.styleTab(preferencesTab);
        const auto tabLabels = preferencesTabLabels();
        for (int index = 0; index < static_cast<int>(tabLabels.size()); ++index) {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            std::wstring wideLabel = utf8ToWide(tabLabels[static_cast<size_t>(index)]);
            item.pszText = wideLabel.data();
            SendMessageW(preferencesTab, TCM_INSERTITEMW, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&item));
        }
        TabCtrl_SetCurSel(preferencesTab, selectedPreferencesTab);

        preferencesGeneralGroup = CreateWindowExW(
            0, L"BUTTON", L"General Settings",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 10, 484, 170,
            hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);

        preferencesInfo = CreateWindowExW(
            0,
            L"STATIC",
            L"Taskbar overlay and startup behavior are app-wide settings. Provider-specific subscription controls live on their own tabs.",
            WS_CHILD | WS_VISIBLE,
            24,
            34,
            440,
            36,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);

        overlayCheckbox = CreateWindowExW(
            0,
            L"BUTTON",
            L"Enable taskbar overlay",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            24,
            62,
            220,
            22,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPreferencesOverlayCheckboxId)),
            GetModuleHandleW(nullptr),
            nullptr);

        startupCheckbox = CreateWindowExW(
            0,
            L"BUTTON",
            L"Start with Windows",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            24,
            114,
            220,
            22,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPreferencesStartupCheckboxId)),
            GetModuleHandleW(nullptr),
            nullptr);

        startupBackgroundCheckbox = CreateWindowExW(
            0,
            L"BUTTON",
            L"Start in background (--background)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            44,
            140,
            300,
            22,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPreferencesStartupBackgroundCheckboxId)),
            GetModuleHandleW(nullptr),
            nullptr);

        scheduledExportGroup = CreateWindowExW(
            0, L"BUTTON", L"Scheduled HTML Export",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 188, 484, 190,
            hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);

        scheduledExportInfo = CreateWindowExW(
            0,
            L"STATIC",
            L"JiStats can write a recurring HTML report from the current dashboard scope without relying on Windows Task Scheduler.",
            WS_CHILD | WS_VISIBLE,
            24,
            212,
            440,
            36,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);

        scheduledExportCheckbox = CreateWindowExW(
            0,
            L"BUTTON",
            L"Enable scheduled HTML export",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            24,
            246,
            240,
            22,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPreferencesScheduledExportCheckboxId)),
            GetModuleHandleW(nullptr),
            nullptr);

        scheduledExportIntervalLabel = CreateWindowExW(
            0,
            L"STATIC",
            L"Interval:",
            WS_CHILD | WS_VISIBLE,
            24,
            276,
            60,
            20,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        scheduledExportIntervalEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            90,
            272,
            72,
            22,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPreferencesScheduledExportIntervalEditId)),
            GetModuleHandleW(nullptr),
            nullptr);
        scheduledExportIntervalSuffix = CreateWindowExW(
            0,
            L"STATIC",
            L"minutes",
            WS_CHILD | WS_VISIBLE,
            170,
            276,
            70,
            20,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);

        scheduledExportPathLabel = CreateWindowExW(
            0,
            L"STATIC",
            L"Output Folder:",
            WS_CHILD | WS_VISIBLE,
            24,
            306,
            86,
            20,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        scheduledExportPathEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            112,
            302,
            330,
            22,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPreferencesScheduledExportPathEditId)),
            GetModuleHandleW(nullptr),
            nullptr);

        scheduledExportLastRunLabel = CreateWindowExW(
            0,
            L"STATIC",
            L"Last Run:",
            WS_CHILD | WS_VISIBLE,
            24,
            336,
            70,
            20,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        scheduledExportLastRunValue = CreateWindowExW(
            0,
            L"STATIC",
            L"Never",
            WS_CHILD | WS_VISIBLE,
            112,
            336,
            330,
            20,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);

        openAISettingsGroup = CreateWindowExW(
            0, L"BUTTON", L"OpenAI Subscription",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 10, 484, 170,
            hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        openAISettingsInfo =
            CreateWindowExW(0, L"STATIC", codexAttributionSettingsInfo(CodexAttributionSaveError::None),
                            WS_CHILD | WS_VISIBLE, 24, 34, 440, 54, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        openaiSubscriptionCheckbox = CreateWindowExW(
            0,
            L"BUTTON",
            L"Enable OpenAI subscription tracking/status",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            24,
            96,
            300,
            22,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPreferencesOpenAISubscriptionCheckboxId)),
            GetModuleHandleW(nullptr),
            nullptr);

        codexModelAttributionCheckbox =
            CreateWindowExW(0, L"BUTTON", L"Enable Codex model attribution",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 24, 126, 300, 22, hwnd,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPreferencesCodexModelAttributionCheckboxId)),
                            GetModuleHandleW(nullptr), nullptr);

        claudeCodeGroup = CreateWindowExW(
            0, L"BUTTON", L"Claude Code Local Tracking",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 10, 484, 170,
            hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        claudeCodeInfo = CreateWindowExW(
            0,
            L"STATIC",
            utf8ToWide(localProviderSettingsInfoText("claude_code")).c_str(),
            WS_CHILD | WS_VISIBLE,
            24,
            34,
            440,
            84,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        claudeCodeEnableCheckbox = CreateWindowExW(
            0,
            L"BUTTON",
            utf8ToWide(localProviderToggleLabel("claude_code")).c_str(),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            24,
            122,
            300,
            22,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPreferencesClaudeCodeCheckboxId)),
            GetModuleHandleW(nullptr),
            nullptr);
        claudeCodeStatus = CreateWindowExW(
            0,
            L"STATIC",
            L"",
            WS_CHILD | WS_VISIBLE,
            24,
            152,
            440,
            20,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        claudeCodeDetail = CreateWindowExW(
            0,
            L"STATIC",
            L"",
            WS_CHILD | WS_VISIBLE,
            24,
            178,
            440,
            84,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);

        miniMaxAccountsGroup = CreateWindowExW(
            0, L"BUTTON", L"MiniMax Subscription Accounts",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 192, 484, 240,
            hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);

        miniMaxAccountsInfo = CreateWindowExW(
            0,
            L"STATIC",
            L"Secrets stay in Windows Credential Manager. JiStats stores only references and validation timestamps locally.",
            WS_CHILD | WS_VISIBLE,
            24,
            216,
            446,
            32,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        miniMaxSubscriptionCheckbox = CreateWindowExW(
            0,
            L"BUTTON",
            L"Enable MiniMax subscription tracking/status",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            24,
            254,
            320,
            22,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPreferencesMiniMaxSubscriptionCheckboxId)),
            GetModuleHandleW(nullptr),
            nullptr);

        miniMaxCodingStatus = CreateWindowExW(
            0,
            L"STATIC",
            L"Coding Plan: Missing",
            WS_CHILD | WS_VISIBLE,
            24,
            286,
            446,
            18,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        miniMaxCodingDetail = CreateWindowExW(
            0,
            L"STATIC",
            L"",
            WS_CHILD | WS_VISIBLE,
            40,
            306,
            430,
            18,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        miniMaxTokenStatus = CreateWindowExW(
            0,
            L"STATIC",
            L"Token Plan: Missing",
            WS_CHILD | WS_VISIBLE,
            24,
            334,
            446,
            18,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        miniMaxTokenDetail = CreateWindowExW(
            0,
            L"STATIC",
            L"",
            WS_CHILD | WS_VISIBLE,
            40,
            354,
            430,
            18,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);

        miniMaxPlanLabel = CreateWindowExW(
            0,
            L"STATIC",
            L"Surface:",
            WS_CHILD | WS_VISIBLE,
            24,
            350,
            60,
            20,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        miniMaxPlanCombo = CreateWindowExW(
            0,
            WC_COMBOBOXW,
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            96,
            346,
            180,
            120,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMiniMaxPlanComboId)),
            GetModuleHandleW(nullptr),
            nullptr);
        SendMessageW(miniMaxPlanCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Coding Plan"));
        SendMessageW(miniMaxPlanCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Token Plan"));
        SendMessageW(miniMaxPlanCombo, CB_SETCURSEL, 0, 0);

        miniMaxAccountLabel = CreateWindowExW(
            0,
            L"STATIC",
            L"Account Label:",
            WS_CHILD | WS_VISIBLE,
            24,
            380,
            80,
            20,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        miniMaxAccountEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            110,
            376,
            240,
            22,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMiniMaxAccountEditId)),
            GetModuleHandleW(nullptr),
            nullptr);

        miniMaxSecretLabel = CreateWindowExW(
            0,
            L"STATIC",
            L"Subscription Key:",
            WS_CHILD | WS_VISIBLE,
            24,
            408,
            92,
            20,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        miniMaxSecretEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD,
            126,
            404,
            224,
            22,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMiniMaxSecretEditId)),
            GetModuleHandleW(nullptr),
            nullptr);

        miniMaxSaveButton = CreateWindowExW(
            0,
            L"BUTTON",
            L"Save",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            364,
            374,
            96,
            26,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMiniMaxSaveButtonId)),
            GetModuleHandleW(nullptr),
            nullptr);
        miniMaxValidateButton = CreateWindowExW(
            0,
            L"BUTTON",
            L"Validate",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            364,
            404,
            96,
            26,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMiniMaxValidateButtonId)),
            GetModuleHandleW(nullptr),
            nullptr);

        futureProviderGroup = CreateWindowExW(
            0, L"BUTTON", L"Future Provider Subscription",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 10, 484, 170,
            hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        futureProviderInfo = CreateWindowExW(
            0,
            L"STATIC",
            L"",
            WS_CHILD | WS_VISIBLE,
            24,
            42,
            440,
            96,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);

        externalSubscriptionsGroup = CreateWindowExW(0, L"BUTTON", L"Subscription Quota", WS_CHILD | BS_GROUPBOX, 0, 0,
                                                     0, 0, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        externalSubscriptionsInfo = CreateWindowExW(
            0, L"STATIC",
            L"Grok uses the account signed in with grok login. Cursor uses CURSOR_API_KEY or CURSOR_AUTH_TOKEN "
            L"from JiStats' environment. Restart JiStats after changing a Cursor environment variable.",
            WS_CHILD, 0, 0, 0, 0, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        auto makeSubscriptionCheckbox = [&](const wchar_t* label, int id) {
            return CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr),
                                   nullptr);
        };
        grokSubscriptionCheckbox =
            makeSubscriptionCheckbox(L"Track Grok subscription quota", kPreferencesGrokSubscriptionCheckboxId);
        cursorSubscriptionCheckbox =
            makeSubscriptionCheckbox(L"Track Cursor subscription quota", kPreferencesCursorSubscriptionCheckboxId);

        kimiCodeGroup = CreateWindowExW(
            0, L"BUTTON", L"Kimi Code Local Tracking",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 10, 484, 170,
            hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        kimiCodeInfo = CreateWindowExW(
            0,
            L"STATIC",
            utf8ToWide(localProviderSettingsInfoText("kimi")).c_str(),
            WS_CHILD | WS_VISIBLE,
            24,
            34,
            440,
            84,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        kimiCodeEnableCheckbox = CreateWindowExW(
            0,
            L"BUTTON",
            utf8ToWide(localProviderToggleLabel("kimi")).c_str(),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            24,
            122,
            300,
            22,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPreferencesKimiCodeCheckboxId)),
            GetModuleHandleW(nullptr),
            nullptr);
        kimiCodeStatus = CreateWindowExW(
            0,
            L"STATIC",
            L"",
            WS_CHILD | WS_VISIBLE,
            24,
            152,
            440,
            20,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        kimiCodeDetail = CreateWindowExW(
            0,
            L"STATIC",
            L"",
            WS_CHILD | WS_VISIBLE,
            24,
            178,
            440,
            84,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);

        miniMaxFeedbackLabel = CreateWindowExW(
            0,
            L"STATIC",
            L"",
            WS_CHILD | WS_VISIBLE,
            24,
            438,
            446,
            36,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);

        preferencesSaveStatus = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPreferencesSaveStatusId)),
                                                GetModuleHandleW(nullptr), nullptr);
        preferencesRetryButton =
            CreateWindowExW(0, L"BUTTON", L"Retry save", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPreferencesRetryButtonId)),
                            GetModuleHandleW(nullptr), nullptr);
        preferencesCloseButton = CreateWindowExW(
            0,
            L"BUTTON",
            L"Close",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            404,
            482,
            80,
            26,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPreferencesCloseButtonId)),
            GetModuleHandleW(nullptr),
            nullptr);

        for (const HWND control : {overlayCheckbox, startupCheckbox, startupBackgroundCheckbox, scheduledExportCheckbox,
                                   openaiSubscriptionCheckbox, claudeCodeEnableCheckbox, miniMaxSubscriptionCheckbox,
                                   kimiCodeEnableCheckbox, preferencesCloseButton}) {
            if (control != nullptr) {
                SetWindowLongPtrW(control, GWL_STYLE, GetWindowLongPtrW(control, GWL_STYLE) | WS_TABSTOP);
            }
        }

        applyBodyFont(preferencesTab);
        applyBodyFont(preferencesGeneralGroup);
        applyBodyFont(preferencesInfo);
        applyBodyFont(overlayCheckbox);
        applyBodyFont(startupCheckbox);
        applyBodyFont(startupBackgroundCheckbox);
        applyBodyFont(scheduledExportGroup);
        applyBodyFont(scheduledExportInfo);
        applyBodyFont(scheduledExportCheckbox);
        applyBodyFont(scheduledExportIntervalLabel);
        applyBodyFont(scheduledExportIntervalEdit);
        applyBodyFont(scheduledExportIntervalSuffix);
        applyBodyFont(scheduledExportPathLabel);
        applyBodyFont(scheduledExportPathEdit);
        applyBodyFont(scheduledExportLastRunLabel);
        applyBodyFont(scheduledExportLastRunValue);
        applyBodyFont(openAISettingsGroup);
        applyBodyFont(openAISettingsInfo);
        applyBodyFont(openaiSubscriptionCheckbox);
        applyBodyFont(codexModelAttributionCheckbox);
        applyBodyFont(miniMaxSubscriptionCheckbox);
        applyBodyFont(claudeCodeGroup);
        applyBodyFont(claudeCodeInfo);
        applyBodyFont(claudeCodeEnableCheckbox);
        applyBodyFont(claudeCodeStatus);
        applyBodyFont(claudeCodeDetail);
        applyBodyFont(miniMaxAccountsGroup);
        applyBodyFont(miniMaxAccountsInfo);
        applyBodyFont(miniMaxCodingStatus);
        applyBodyFont(miniMaxCodingDetail);
        applyBodyFont(miniMaxTokenStatus);
        applyBodyFont(miniMaxTokenDetail);
        applyBodyFont(miniMaxPlanLabel);
        applyBodyFont(miniMaxPlanCombo);
        applyBodyFont(miniMaxAccountLabel);
        applyBodyFont(miniMaxAccountEdit);
        applyBodyFont(miniMaxSecretLabel);
        applyBodyFont(miniMaxSecretEdit);
        applyBodyFont(miniMaxSaveButton);
        applyBodyFont(miniMaxValidateButton);
        applyBodyFont(miniMaxFeedbackLabel);
        applyBodyFont(futureProviderGroup);
        applyBodyFont(futureProviderInfo);
        applyBodyFont(externalSubscriptionsGroup);
        applyBodyFont(externalSubscriptionsInfo);
        applyBodyFont(grokSubscriptionCheckbox);
        applyBodyFont(cursorSubscriptionCheckbox);
        applyBodyFont(kimiCodeGroup);
        applyBodyFont(kimiCodeInfo);
        applyBodyFont(kimiCodeEnableCheckbox);
        applyBodyFont(kimiCodeStatus);
        applyBodyFont(kimiCodeDetail);
        applyBodyFont(preferencesSaveStatus);
        applyBodyFont(preferencesRetryButton);
        applyBodyFont(preferencesCloseButton);
        layoutPreferencesControls(hwnd);
        syncPreferencesWindowState();
    }

    void openPreferencesWindow() {
        HWND existing = nullptr;
        HWND owner = nullptr;
        std::function<void()> onOpened;
        {
            std::lock_guard<std::mutex> lock(mutex);
            existing = preferencesWindow;
            owner = window;
            onOpened = preferencesOpenedCallback;
        }

        if (existing != nullptr) {
            ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
            if (onOpened) {
                onOpened();
            }
            return;
        }

        HWND hwnd = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW,
            preferencesWindowClassName(),
            L"JiStats Settings",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            560,
            620,
            owner,
            nullptr,
            GetModuleHandleW(nullptr),
            this);

        if (hwnd != nullptr) {
            applyWindowIcons(hwnd);
            centerWindowOnOwner(hwnd, owner);
            {
                std::lock_guard<std::mutex> lock(mutex);
                preferencesWindow = hwnd;
            }
            ShowWindow(hwnd, SW_SHOW);
            SetFocus(preferencesTab);
            UpdateWindow(hwnd);
            if (onOpened) {
                onOpened();
            }
        }
    }

    struct ShellDialogCreateParams {
        Impl* owner = nullptr;
        DashboardShellDialog dialog = DashboardShellDialog::Help;
    };

    struct MiniMaxActionMessageParams {
        DashboardMiniMaxCredentialRequest request;
    };

    static DashboardShellDialog shellDialogFromWindow(HWND hwnd) {
        const auto value = reinterpret_cast<INT_PTR>(GetPropW(hwnd, kShellDialogKindProperty));
        return static_cast<DashboardShellDialog>(value);
    }

    void createShellDialogControls(HWND hwnd, DashboardShellDialog dialog) {
        ShellDialogState* state = dialogStateFor(dialog);
        if (state == nullptr) {
            return;
        }

        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        const int margin = 18;
        const int buttonWidth = 80;
        const int buttonHeight = 28;
        const int buttonBottomMargin = 18;
        const bool isAboutDialog = dialog == DashboardShellDialog::About;
        const int iconSize = scaledPixels(32, hwnd);
        const int headerGap = 12;
        const int headerTitleHeight = 22;
        const int headerSubtitleHeight = 20;
        const int headerHeight = isAboutDialog ? std::max(iconSize, headerTitleHeight + headerSubtitleHeight + 4) : 0;
        const int headerBottomGap = isAboutDialog ? 14 : 0;
        const int bodyWidth = static_cast<int>(clientRect.right) - (margin * 2);
        const int bodyHeight = std::max<int>(
            80,
            static_cast<int>(clientRect.bottom) - margin - headerHeight - headerBottomGap -
                buttonHeight - buttonBottomMargin - 16);
        const int closeLeft = static_cast<int>(clientRect.right) - margin - buttonWidth;
        const int closeTop = static_cast<int>(clientRect.bottom) - buttonBottomMargin - buttonHeight;
        const int bodyTop = margin + headerHeight + headerBottomGap;

        state->window = hwnd;
        applyWindowIcons(hwnd);
        if (isAboutDialog) {
            ensureAppIcons();
            state->headerIcon = CreateWindowExW(
                0,
                L"STATIC",
                nullptr,
                WS_CHILD | WS_VISIBLE | SS_ICON,
                margin,
                margin,
                iconSize,
                iconSize,
                hwnd,
                nullptr,
                GetModuleHandleW(nullptr),
                nullptr);
            if (state->headerIcon != nullptr && appIconLarge != nullptr) {
                SendMessageW(state->headerIcon, STM_SETIMAGE, IMAGE_ICON, reinterpret_cast<LPARAM>(appIconLarge));
            }

            const int headerTextLeft = margin + iconSize + headerGap;
            const int headerTextWidth = std::max(120, bodyWidth - iconSize - headerGap);
            state->headerTitle = CreateWindowExW(
                0,
                L"STATIC",
                L"",
                WS_CHILD | WS_VISIBLE,
                headerTextLeft,
                margin,
                headerTextWidth,
                headerTitleHeight,
                hwnd,
                nullptr,
                GetModuleHandleW(nullptr),
                nullptr);
            state->headerSubtitle = CreateWindowExW(
                0,
                L"STATIC",
                L"",
                WS_CHILD | WS_VISIBLE,
                headerTextLeft,
                margin + headerTitleHeight,
                headerTextWidth,
                headerSubtitleHeight,
                hwnd,
                nullptr,
                GetModuleHandleW(nullptr),
                nullptr);
        }
        state->bodyLabel = CreateWindowExW(
            0,
            L"STATIC",
            L"",
            WS_CHILD | WS_VISIBLE,
            margin,
            bodyTop,
            bodyWidth,
            bodyHeight,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        state->closeButton = CreateWindowExW(
            0,
            L"BUTTON",
            L"Close",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            closeLeft,
            closeTop,
            buttonWidth,
            buttonHeight,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kShellDialogCloseButtonId)),
            GetModuleHandleW(nullptr),
            nullptr);

        applyTitleFont(state->headerTitle);
        applyBodyFont(state->headerSubtitle);
        applyBodyFont(state->bodyLabel);
        applyBodyFont(state->closeButton);
        syncShellDialogContent(dialog);
    }

    void clearShellDialogState(DashboardShellDialog dialog) {
        ShellDialogState* state = dialogStateFor(dialog);
        if (state == nullptr) {
            return;
        }

        state->window = nullptr;
        state->headerIcon = nullptr;
        state->headerTitle = nullptr;
        state->headerSubtitle = nullptr;
        state->bodyLabel = nullptr;
        state->closeButton = nullptr;
    }

    void openShellDialogWindow(DashboardShellDialog dialog) {
        ShellDialogState* state = nullptr;
        HWND owner = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            state = dialogStateFor(dialog);
            owner = window;
        }

        if (state == nullptr) {
            return;
        }

        if (state->window != nullptr) {
            ShowWindow(state->window, SW_SHOW);
            SetForegroundWindow(state->window);
            return;
        }

        auto createParams = std::make_unique<ShellDialogCreateParams>();
        createParams->owner = this;
        createParams->dialog = dialog;
        ShellDialogCreateParams* rawCreateParams = createParams.release();

        int dialogWidth = 400;
        int dialogHeight = 220;
        if (dialog == DashboardShellDialog::About) {
            dialogWidth = 520;
            dialogHeight = 420;
        }

        int dialogLeft = CW_USEDEFAULT;
        int dialogTop = CW_USEDEFAULT;
        if (const auto centeredRect = centeredRectForOwner(owner, dialogWidth, dialogHeight)) {
            dialogLeft = centeredRect->left;
            dialogTop = centeredRect->top;
        }

        HWND hwnd = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW,
            shellDialogWindowClassName(),
            shellDialogTitle(dialog).c_str(),
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
            dialogLeft,
            dialogTop,
            dialogWidth,
            dialogHeight,
            owner,
            nullptr,
            GetModuleHandleW(nullptr),
            rawCreateParams);

        if (hwnd == nullptr) {
            delete rawCreateParams;
        }

        if (hwnd != nullptr) {
            applyWindowIcons(hwnd);
            ShowWindow(hwnd, SW_SHOW);
            UpdateWindow(hwnd);
        }
    }

    LRESULT handlePreferencesMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (const auto result = desktopTheme.panelMessage(hwnd, msg, wParam, lParam)) {
            return *result;
        }
        switch (msg) {
            case WM_SETTINGCHANGE:
            case WM_SYSCOLORCHANGE:
            case WM_THEMECHANGED:
                desktopTheme.refreshWindow(hwnd, msg);
                return DefWindowProcW(hwnd, msg, wParam, lParam);
            case WM_CREATE:
                createPreferencesControls(hwnd);
                return 0;
            case WM_DRAWITEM: {
                const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
                if (draw != nullptr && draw->hwndItem == preferencesTab) {
                    desktopTheme.drawTab(*draw);
                    return TRUE;
                }
                return DefWindowProcW(hwnd, msg, wParam, lParam);
            }
        case WM_SIZE:
            layoutPreferencesControls(hwnd);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case kPreferencesOverlayCheckboxId:
                if (HIWORD(wParam) == BN_CLICKED) {
                    const bool checked = (SendMessageW(overlayCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    setPreference(&DashboardPreferences::taskbarOverlayEnabled, checked);
                }
                return 0;
            case kPreferencesOpenAISubscriptionCheckboxId:
                if (HIWORD(wParam) == BN_CLICKED) {
                    const bool checked =
                        (SendMessageW(openaiSubscriptionCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    setPreference(&DashboardPreferences::openaiSubscriptionTrackingEnabled, checked, true);
                }
                return 0;
            case kPreferencesCodexModelAttributionCheckboxId:
                if (HIWORD(wParam) == BN_CLICKED) {
                    const bool enabled = SendMessageW(codexModelAttributionCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    std::function<void(bool, std::uint64_t)> callback;
                    std::uint64_t request = 0;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        callback = codexModelAttributionChangedCallback;
                        if (callback) {
                            requestedCodexAttributionEnabled = enabled;
                            request = ++codexAttributionRequest;
                        }
                    }
                    if (callback) {
                        callback(enabled, request);
                    }
                }
                return 0;
            case kPreferencesMiniMaxSubscriptionCheckboxId:
                if (HIWORD(wParam) == BN_CLICKED) {
                    const bool checked =
                        (SendMessageW(miniMaxSubscriptionCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    setPreference(&DashboardPreferences::miniMaxSubscriptionTrackingEnabled, checked, true);
                }
                return 0;
            case kPreferencesClaudeCodeCheckboxId:
                if (HIWORD(wParam) == BN_CLICKED) {
                    const bool checked =
                        (SendMessageW(claudeCodeEnableCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    setPreference(&DashboardPreferences::claudeCodeEnabled, checked, true);
                }
                return 0;
            case kPreferencesGrokSubscriptionCheckboxId:
            case kPreferencesCursorSubscriptionCheckboxId:
                if (HIWORD(wParam) == BN_CLICKED) {
                    const bool isGrok = LOWORD(wParam) == kPreferencesGrokSubscriptionCheckboxId;
                    const HWND checkbox = isGrok ? grokSubscriptionCheckbox : cursorSubscriptionCheckbox;
                    const auto field = isGrok ? &DashboardPreferences::grokSubscriptionTrackingEnabled
                                              : &DashboardPreferences::cursorSubscriptionTrackingEnabled;
                    setPreference(field, SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED, true);
                }
                return 0;
            case kPreferencesKimiCodeCheckboxId:
                if (HIWORD(wParam) == BN_CLICKED) {
                    const bool checked =
                        (SendMessageW(kimiCodeEnableCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    setPreference(&DashboardPreferences::kimiCodeEnabled, checked, true);
                }
                return 0;
            case kPreferencesStartupCheckboxId:
                if (HIWORD(wParam) == BN_CLICKED) {
                    const bool checked = (SendMessageW(startupCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    setPreference(&DashboardPreferences::startupEnabled, checked);
                }
                return 0;
            case kPreferencesStartupBackgroundCheckboxId:
                if (HIWORD(wParam) == BN_CLICKED) {
                    const bool checked =
                        (SendMessageW(startupBackgroundCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    setPreference(&DashboardPreferences::startupStartInBackground, checked);
                }
                return 0;
            case kPreferencesScheduledExportCheckboxId:
                if (HIWORD(wParam) == BN_CLICKED) {
                    commitScheduledExportSettingsFromControls();
                }
                return 0;
            case kPreferencesScheduledExportIntervalEditId:
            case kPreferencesScheduledExportPathEditId:
                if (HIWORD(wParam) == EN_KILLFOCUS &&
                    SendMessageW(reinterpret_cast<HWND>(lParam), EM_GETMODIFY, 0, 0) != FALSE) {
                    commitScheduledExportSettingsFromControls();
                }
                return 0;
            case kPreferencesCloseButtonId:
                if (HIWORD(wParam) == BN_CLICKED) {
                    commitScheduledExportSettingsFromControls();
                    DestroyWindow(hwnd);
                }
                return 0;
            case kPreferencesRetryButtonId:
                if (HIWORD(wParam) == BN_CLICKED) {
                    editPreferences([this](DashboardPreferences&) {
                        return preferencesRequest > viewModel.completedPreferencesRequest &&
                               viewModel.failedPreferencesRequest == preferencesRequest;
                    });
                }
                return 0;
            case kMiniMaxAccountEditId:
                if (HIWORD(wParam) == EN_CHANGE) {
                    ++miniMaxAccountEditRevision;
                }
                return 0;
            case kMiniMaxPlanComboId:
                if (HIWORD(wParam) == CBN_SELCHANGE) {
                    ++miniMaxAccountEditRevision;
                    SendMessageW(miniMaxAccountEdit, EM_SETMODIFY, FALSE, 0);
                    syncPreferencesWindowState();
                }
                return 0;
            case kMiniMaxSaveButtonId:
                if (HIWORD(wParam) == BN_CLICKED) {
                    handleMiniMaxCredentialAction(DashboardMiniMaxCredentialAction::Save);
                }
                return 0;
            case kMiniMaxValidateButtonId:
                if (HIWORD(wParam) == BN_CLICKED) {
                    handleMiniMaxCredentialAction(DashboardMiniMaxCredentialAction::Validate);
                }
                return 0;
            default:
                return DefWindowProcW(hwnd, msg, wParam, lParam);
            }
        case WM_NOTIFY:
            {
                const auto* header = reinterpret_cast<NMHDR*>(lParam);
                if (header != nullptr && header->hwndFrom == preferencesTab &&
                    header->code == TCN_SELCHANGE) {
                    commitScheduledExportSettingsFromControls();
                    selectedPreferencesTab = std::max(0, TabCtrl_GetCurSel(preferencesTab));
                    layoutPreferencesControls(hwnd);
                    syncPreferencesWindowState();
                    return 0;
                }
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_JISTATS_SYNC_PREFERENCES:
            syncPreferencesWindowState();
            return 0;
        case WM_CLOSE:
            commitScheduledExportSettingsFromControls();
            DestroyWindow(hwnd);
            return 0;
        case WM_JISTATS_TEST_MINIMAX_ACTION:
            {
                std::unique_ptr<MiniMaxActionMessageParams> params(
                    reinterpret_cast<MiniMaxActionMessageParams*>(lParam));
                if (!params) {
                    return 0;
                }

                if (miniMaxPlanCombo != nullptr) {
                    const int selection = params->request.planKey == "token_plan" ? 1 : 0;
                    SendMessageW(miniMaxPlanCombo, CB_SETCURSEL, selection, 0);
                }
                if (miniMaxAccountEdit != nullptr) {
                    setWindowText(miniMaxAccountEdit, utf8ToWide(params->request.accountDisplayName));
                }
                if (miniMaxSecretEdit != nullptr) {
                    setWindowText(miniMaxSecretEdit, utf8ToWide(params->request.secret));
                }
                queueMiniMaxCredentialAction(params->request);
            }
            return 0;
        case WM_DESTROY:
            {
                std::lock_guard<std::mutex> lock(mutex);
                preferencesWindow = nullptr;
                preferencesTab = nullptr;
                preferencesGeneralGroup = nullptr;
                preferencesInfo = nullptr;
                overlayCheckbox = nullptr;
                scheduledExportGroup = nullptr;
                scheduledExportInfo = nullptr;
                scheduledExportCheckbox = nullptr;
                scheduledExportIntervalLabel = nullptr;
                scheduledExportIntervalEdit = nullptr;
                scheduledExportIntervalSuffix = nullptr;
                scheduledExportPathLabel = nullptr;
                scheduledExportPathEdit = nullptr;
                scheduledExportLastRunLabel = nullptr;
                scheduledExportLastRunValue = nullptr;
                openAISettingsGroup = nullptr;
                openAISettingsInfo = nullptr;
                openaiSubscriptionCheckbox = nullptr;
                codexModelAttributionCheckbox = nullptr;
                miniMaxSubscriptionCheckbox = nullptr;
                claudeCodeGroup = nullptr;
                claudeCodeInfo = nullptr;
                claudeCodeEnableCheckbox = nullptr;
                claudeCodeStatus = nullptr;
                claudeCodeDetail = nullptr;
                startupCheckbox = nullptr;
                startupBackgroundCheckbox = nullptr;
                miniMaxAccountsGroup = nullptr;
                miniMaxAccountsInfo = nullptr;
                miniMaxCodingStatus = nullptr;
                miniMaxCodingDetail = nullptr;
                miniMaxTokenStatus = nullptr;
                miniMaxTokenDetail = nullptr;
                miniMaxPlanLabel = nullptr;
                miniMaxPlanCombo = nullptr;
                miniMaxAccountLabel = nullptr;
                miniMaxAccountEdit = nullptr;
                ++miniMaxAccountEditRevision;
                miniMaxSecretLabel = nullptr;
                miniMaxSecretEdit = nullptr;
                miniMaxSaveButton = nullptr;
                miniMaxValidateButton = nullptr;
                miniMaxFeedbackLabel = nullptr;
                futureProviderGroup = nullptr;
                futureProviderInfo = nullptr;
                externalSubscriptionsGroup = nullptr;
                externalSubscriptionsInfo = nullptr;
                grokSubscriptionCheckbox = nullptr;
                cursorSubscriptionCheckbox = nullptr;
                kimiCodeGroup = nullptr;
                kimiCodeInfo = nullptr;
                kimiCodeEnableCheckbox = nullptr;
                kimiCodeStatus = nullptr;
                kimiCodeDetail = nullptr;
                preferencesSaveStatus = nullptr;
                preferencesRetryButton = nullptr;
                preferencesCloseButton = nullptr;
            }
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    LRESULT handleShellDialogMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (const auto result = desktopTheme.panelMessage(hwnd, msg, wParam, lParam)) {
            return *result;
        }
        const DashboardShellDialog dialog = shellDialogFromWindow(hwnd);
        switch (msg) {
            case WM_SETTINGCHANGE:
            case WM_SYSCOLORCHANGE:
            case WM_THEMECHANGED:
                desktopTheme.refreshWindow(hwnd, msg);
                return DefWindowProcW(hwnd, msg, wParam, lParam);
            case WM_CREATE:
                createShellDialogControls(hwnd, dialog);
                centerWindowOnOwner(hwnd, GetWindow(hwnd, GW_OWNER));
                return 0;
            case WM_COMMAND:
                if (LOWORD(wParam) == kShellDialogCloseButtonId && HIWORD(wParam) == BN_CLICKED) {
                    DestroyWindow(hwnd);
                    return 0;
                }
                return DefWindowProcW(hwnd, msg, wParam, lParam);
            case WM_CLOSE:
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                RemovePropW(hwnd, kShellDialogKindProperty);
                clearShellDialogState(dialog);
                return 0;
            default:
                return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    LRESULT handleAnalysisMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (const auto result = desktopTheme.panelMessage(hwnd, msg, wParam, lParam)) {
            return *result;
        }
        switch (msg) {
            case WM_CREATE: {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    analysisWindow = hwnd;
                }
                initializeAnalysisControls(hwnd);
                return 0;
            }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = kAnalysisMinWidth;
            info->ptMinTrackSize.y = kAnalysisMinHeight;
            return 0;
        }
        case WM_SIZE:
            layoutAnalysisWindowControls(hwnd);
            return 0;
        case WM_COMMAND:
            if (analysisPresenter != nullptr &&
                analysisPresenter->handleCommand(
                    static_cast<WORD>(LOWORD(wParam)),
                    static_cast<WORD>(HIWORD(wParam)))) {
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_TIMER:
            if (analysisPresenter != nullptr &&
                analysisPresenter->handleTimer(static_cast<UINT_PTR>(wParam))) {
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_VSCROLL:
            if (analysisPresenter != nullptr &&
                analysisPresenter->handleVScroll(
                    static_cast<WORD>(LOWORD(wParam)),
                    static_cast<WORD>(HIWORD(wParam)))) {
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_MOUSEWHEEL:
            if (analysisPresenter != nullptr &&
                analysisPresenter->handleMouseWheel(static_cast<short>(HIWORD(wParam)))) {
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_JISTATS_ATTACH_ANALYSIS:
            initializeAnalysisControls(hwnd);
            return 0;
        case WM_EXITSIZEMOVE:
            persistAnalysisWindowPlacement(hwnd);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            persistAnalysisWindowPlacement(hwnd);
            if (analysisPresenter != nullptr) {
                analysisPresenter->shutdown();
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                analysisWindow = nullptr;
            }
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    bool isCodexThroughputSummaryControl(HWND control) const {
        return control == codexThroughputText ||
            control == codexThroughputAveragesText ||
            control == codexThroughputTotalsText;
    }

    std::vector<std::wstring> splitOwnerDrawLines(const std::wstring& text) const {
        std::vector<std::wstring> lines;
        size_t start = 0;
        while (start <= text.size()) {
            size_t end = text.find(L'\n', start);
            std::wstring line = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            if (!line.empty() && line.back() == L'\r') {
                line.pop_back();
            }
            lines.push_back(std::move(line));
            if (end == std::wstring::npos) {
                break;
            }
            start = end + 1;
        }
        return lines;
    }

    void drawCodexThroughputSummaryCell(const DRAWITEMSTRUCT& draw) {
        HDC dc = draw.hDC;
        RECT rect = draw.rcItem;
        DesktopTheme::fill(dc, rect, dashboardColor(RGB(240, 244, 250), COLOR_WINDOW));
        if (desktopTheme.usesHighContrast()) {
            FrameRect(dc, &rect, GetSysColorBrush(COLOR_WINDOWTEXT));
        }

        const int length = GetWindowTextLengthW(draw.hwndItem);
        std::wstring text(static_cast<size_t>(length + 1), L'\0');
        if (length > 0) {
            GetWindowTextW(draw.hwndItem, text.data(), length + 1);
        }
        text.resize(static_cast<size_t>(length));

        RECT contentRect = rect;
        InflateRect(&contentRect, -scaledPixels(12), -scaledPixels(8));
        const COLORREF textColor = dashboardColor(RGB(27, 40, 61), COLOR_WINDOWTEXT);
        const int oldBkMode = SetBkMode(dc, TRANSPARENT);
        const COLORREF oldColor = SetTextColor(dc, textColor);
        HGDIOBJ oldFont =
            SelectObject(dc, dashboardBodyFont != nullptr ? dashboardBodyFont : GetStockObject(DEFAULT_GUI_FONT));

        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        const int lineHeight = metrics.tmHeight + scaledPixels(3);
        int y = contentRect.top;
        const auto lines = splitOwnerDrawLines(text);
        for (size_t index = 0; index < lines.size(); ++index) {
            if (y >= contentRect.bottom) {
                break;
            }

            const std::wstring& line = lines[index];
            RECT lineRect{
                contentRect.left,
                static_cast<LONG>(y),
                contentRect.right,
                std::min<LONG>(contentRect.bottom, static_cast<LONG>(y + lineHeight + 2))
            };
            if (index == 0) {
                SelectObject(dc, dashboardLabelFont);
                DrawTextW(dc, line.c_str(), -1, &lineRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            } else if (line.rfind(L"Source:", 0) == 0 || line.rfind(L"Rates averaged", 0) == 0) {
                SelectObject(dc, dashboardBodyFont);
                DrawTextW(dc, line.c_str(), -1, &lineRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            } else {
                const size_t colon = line.find(L':');
                if (colon != std::wstring::npos) {
                    const std::wstring label = line.substr(0, colon + 1);
                    const std::wstring value = line.substr(colon + 1);
                    SelectObject(dc, dashboardLabelFont);
                    TextOutW(dc, lineRect.left, lineRect.top, label.c_str(), static_cast<int>(label.size()));
                    SIZE labelSize{};
                    GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &labelSize);
                    RECT valueRect = lineRect;
                    valueRect.left = std::min(valueRect.right, valueRect.left + labelSize.cx + 3);
                    SelectObject(dc,
                                 dashboardBodyFont != nullptr ? dashboardBodyFont : GetStockObject(DEFAULT_GUI_FONT));
                    DrawTextW(dc, value.c_str(), -1, &valueRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                } else {
                    SelectObject(dc,
                                 dashboardBodyFont != nullptr ? dashboardBodyFont : GetStockObject(DEFAULT_GUI_FONT));
                    lineRect.bottom = contentRect.bottom;
                    DrawTextW(dc, line.c_str(), -1, &lineRect, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
                }
            }
            y += lineHeight;
        }

        if (oldFont != nullptr) {
            SelectObject(dc, oldFont);
        }
        SetTextColor(dc, oldColor);
        SetBkMode(dc, oldBkMode);
    }

    LRESULT handleMainMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_CREATE:
            window = hwnd;
            fitInitialDashboardBounds(hwnd);
            nextDashboardChartRefreshTick = GetTickCount64() + kDashboardChartRefreshIntervalMs;
            ensureFonts();
            desktopTheme.refresh();
            createMainMenuBar(hwnd);
            createMainControls(hwnd);
            syncViewModelToControls();
            SetTimer(hwnd, kDashboardChartRefreshTimerId, kDashboardChartCountdownTimerMs, nullptr);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            RECT workArea{};
            tryGetMonitorWorkArea(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), workArea);
            const auto minimum = dashboardMinimumSize(hwnd, workArea);
            info->ptMinTrackSize = {minimum.cx, minimum.cy};
            return 0;
        }
        case WM_DPICHANGED: {
            applyDashboardFonts();
            configureProviderListColumns();
            const auto& bounds = *reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layoutMainControls();
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_SYSCOLORCHANGE:
        case WM_THEMECHANGED:
            desktopTheme.refreshWindow(hwnd, msg);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_ERASEBKGND: {
            RECT rect{};
            GetClientRect(hwnd, &rect);
            HDC dc = reinterpret_cast<HDC>(wParam);
            DesktopTheme::fill(dc, rect, desktopTheme.panelColor());
            RECT banner{0, 0, rect.right, scaledPixels(46)};
            DesktopTheme::fill(dc, banner, desktopTheme.accentColor());
            RECT bevel{0, banner.bottom, rect.right, banner.bottom + scaledPixels(3)};
            DesktopTheme::fill(dc, bevel, dashboardColor(RGB(108, 160, 226), COLOR_3DSHADOW));
            return 1;
        }
        case WM_MOUSEWHEEL:
            if (selectedMainTab == 0) {
                scrollOverviewWheel(GET_WHEEL_DELTA_WPARAM(wParam));
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                ShowWindow(hwnd, SW_HIDE);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    visible = false;
                }
                return 0;
            }
            if (wParam == SIZE_RESTORED) {
                restoreMainPlacementWhenWindowed(hwnd);
            }
            layoutMainControls();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case kMenuFileSettingsId:
                openPreferencesWindow();
                return 0;
            case kMenuFileImportOpenAIId:
                notifyOpenAIImportRequested();
                return 0;
            case kMenuFileExportHtmlId:
                notifyHtmlExportRequested();
                return 0;
            case kMenuFileExitId:
                notifyExitRequested();
                return 0;
            case kMenuHelpViewHelpId:
                openShellDialogWindow(DashboardShellDialog::Help);
                return 0;
            case kMenuHelpAboutId:
                openShellDialogWindow(DashboardShellDialog::About);
                return 0;
            case kOpenAnalysisWindowButtonId:
                if (HIWORD(wParam) == BN_CLICKED) {
                    openAnalysisWindow(true);
                }
                return 0;
            case kOfflineRetryButtonId:
                if (HIWORD(wParam) == BN_SETFOCUS) {
                    revealOverviewControl(offlineRetryButton);
                }
                if (HIWORD(wParam) == BN_CLICKED) {
                    notifyOfflineRetryRequested();
                }
                return 0;
            case kOfflineDismissButtonId:
                if (HIWORD(wParam) == BN_SETFOCUS) {
                    revealOverviewControl(offlineDismissButton);
                }
                if (HIWORD(wParam) == BN_CLICKED) {
                    notifyOfflineDismissRequested();
                }
                return 0;
            default:
                return DefWindowProcW(hwnd, msg, wParam, lParam);
            }
        case WM_TIMER:
            if (wParam == kDashboardChartRefreshTimerId) {
                std::function<void()> callback;
                bool shouldRefresh = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    callback = periodicRefreshCallback;
                    shouldRefresh = visible && !shutdownRequested;
                }
                const ULONGLONG nowTick = GetTickCount64();
                if (nextDashboardChartRefreshTick == 0) {
                    nextDashboardChartRefreshTick = nowTick + kDashboardChartRefreshIntervalMs;
                }
                if (!shouldRefresh) {
                    nextDashboardChartRefreshTick = nowTick + kDashboardChartRefreshIntervalMs;
                } else if (nowTick >= nextDashboardChartRefreshTick) {
                    nextDashboardChartRefreshTick = nowTick + kDashboardChartRefreshIntervalMs;
                    if (callback) {
                        callback();
                    }
                }
                invalidateDashboardChartCanvases();
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_NOTIFY:
            {
                const auto* header = reinterpret_cast<NMHDR*>(lParam);
                if (header != nullptr &&
                    (header->code == TTN_GETDISPINFOW || header->code == TTN_NEEDTEXTW)) {
                    auto* tooltipInfo = reinterpret_cast<NMTTDISPINFOW*>(lParam);
                    if (tooltipInfo != nullptr) {
                        const std::wstring* text = tooltipTextForTarget(header->idFrom);
                        if (text != nullptr) {
                            tooltipInfo->lpszText = text->empty()
                                ? const_cast<LPWSTR>(L"")
                                : const_cast<LPWSTR>(text->c_str());
                            return TRUE;
                        }
                    }
                }
                if (header != nullptr && header->hwndFrom == mainTab &&
                    header->code == TCN_SELCHANGE) {
                    activateMainPage(std::max(0, TabCtrl_GetCurSel(mainTab)));
                    return 0;
                }
                if (header != nullptr && header->hwndFrom == providerList &&
                    header->code == LVN_ITEMCHANGED) {
                    const auto* item = reinterpret_cast<const NMLISTVIEW*>(lParam);
                    if (item != nullptr &&
                        item->iItem >= 0 &&
                        (item->uChanged & LVIF_STATE) != 0 &&
                        (item->uNewState & LVIS_SELECTED) != 0 &&
                        (item->uOldState & LVIS_SELECTED) == 0) {
                        handleProviderQuotaRowSelection(item->iItem);
                    }
                    return 0;
                }

            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
            case WM_DRAWITEM: {
                const auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
                if (draw == nullptr) {
                    return FALSE;
                }
                if (draw->hwndItem == mainTab) {
                    desktopTheme.drawTab(*draw);
                    return TRUE;
                }
                if (isCodexThroughputSummaryControl(draw->hwndItem)) {
                    drawCodexThroughputSummaryCell(*draw);
                    return TRUE;
                }
                if (draw->hwndItem == openAiUsageText || draw->hwndItem == hicapUsageText) {
                    drawModelUsageSummary(*draw);
                    return TRUE;
                }
                if (draw->CtlID >= kUsageBarBaseId && draw->CtlID - kUsageBarBaseId < usageRows.size()) {
                    const auto& row = usageRows[draw->CtlID - kUsageBarBaseId];
                    if (draw->hwndItem == row.bar) {
                        drawQuotaBar(*draw, row);
                        return TRUE;
                    }
                }
                return DefWindowProcW(hwnd, msg, wParam, lParam);
            }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            HWND control = reinterpret_cast<HWND>(lParam);
            const bool header = control == titleLabel;
            const bool notice = control == offlineNoticeTitle || control == offlineNoticeMessage;
            const COLORREF background = notice   ? dashboardColor(RGB(255, 248, 216), COLOR_WINDOW)
                                        : header ? desktopTheme.accentColor()
                                                 : GetSysColor(COLOR_WINDOW);
            COLORREF foreground =
                header ? dashboardColor(RGB(255, 255, 255), COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_WINDOWTEXT);
            if (control == connectionLabel || control == recoveryLabel || control == scheduledExportLabel ||
                control == updatedLabel || control == quotaEmptyText || control == openAiUsageText ||
                control == hicapUsageText) {
                foreground = dashboardColor(RGB(71, 88, 113), COLOR_WINDOWTEXT);
            }
            if (control == connectionValue) {
                std::lock_guard<std::mutex> lock(mutex);
                switch (viewModel.runtimeState) {
                    case RuntimeState::Current:
                        foreground = dashboardColor(RGB(16, 108, 80), COLOR_WINDOWTEXT);
                        break;
                    case RuntimeState::CatchingUp:
                        foreground = dashboardColor(RGB(35, 82, 174), COLOR_WINDOWTEXT);
                        break;
                    case RuntimeState::Stale:
                        foreground = dashboardColor(RGB(135, 77, 13), COLOR_WINDOWTEXT);
                        break;
                    case RuntimeState::Offline:
                        foreground = dashboardColor(RGB(172, 39, 57), COLOR_WINDOWTEXT);
                        break;
                    default:
                        break;
                }
            }
            SetTextColor(dc, foreground);
            SetBkMode(dc, TRANSPARENT);
            SetDCBrushColor(dc, background);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        case WM_JISTATS_SYNC_VIEWMODEL: {
            bool syncMainWindow;
            {
                std::lock_guard<std::mutex> lock(mutex);
                viewModelSyncPending = false;
                syncMainWindow = shouldSyncMainWindow();
            }
            if (syncMainWindow) {
                syncViewModelToControls();
            } else if (preferencesWindow != nullptr) {
                syncPreferencesWindowState();
            }
            return 0;
        }
        case WM_JISTATS_RESTORE_MAIN_PLACEMENT:
            deferredStartupMainPlacement = *reinterpret_cast<const RECT*>(lParam);
            restoreMainPlacementWhenWindowed(hwnd);
            return 0;
        case WM_JISTATS_ATTACH_ANALYSIS:
            if (analysisWindow != nullptr && analysisPresenter != nullptr) {
                PostMessageW(analysisWindow, WM_JISTATS_ATTACH_ANALYSIS, 0, 0);
            }
            return 0;
        case WM_JISTATS_OPEN_ANALYSIS_WINDOW:
            openAnalysisWindow(true);
            return 0;
        case WM_JISTATS_TEST_SELECT_MAIN_TAB: {
            const int index = std::clamp(static_cast<int>(wParam), 0, TabCtrl_GetItemCount(mainTab) - 1);
            TabCtrl_SetCurSel(mainTab, index);
            activateMainPage(index);
            return 0;
        }
        case WM_JISTATS_OPENAI_IMPORT_COMPLETE:
            handleOpenAIImportCompletion(reinterpret_cast<DashboardOpenAIImportResult*>(lParam));
            return 0;
        case WM_JISTATS_HTML_EXPORT_COMPLETE:
            handleHtmlExportCompletion(reinterpret_cast<DashboardHtmlExportResult*>(lParam));
            return 0;
        case WM_JISTATS_MINIMAX_ACTION_COMPLETE:
            handleMiniMaxCredentialCompletion(reinterpret_cast<MiniMaxActionCompletionParams*>(lParam));
            return 0;
        case WM_JISTATS_OFFLINE_RETRY_COMPLETE:
            handleOfflineRetryCompletion(reinterpret_cast<OfflineRetryCompletionParams*>(lParam));
            return 0;
        case WM_JISTATS_OFFLINE_DISMISS_COMPLETE:
            handleOfflineDismissCompletion(reinterpret_cast<OfflineDismissCompletionParams*>(lParam));
            return 0;
        case WM_EXITSIZEMOVE:
            persistMainWindowPlacement(hwnd);
            return 0;
        case WM_SHOWWINDOW:
            {
                std::lock_guard<std::mutex> lock(mutex);
                visible = (wParam != FALSE);
            }
            if (wParam == FALSE) {
                dismissUsageTooltip();
            }
            return 0;
        case WM_CLOSE:
            {
                bool shouldHide = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (!shutdownRequested) {
                        visible = false;
                        shouldHide = true;
                    }
                }
                if (shouldHide) {
                    if (!copyViewModel().startupPending) {
                        persistMainWindowPlacement(hwnd);
                    }
                    closeAuxiliaryWindows();
                    ShowWindow(hwnd, SW_HIDE);
                    return 0;
                }
            }
            closeAuxiliaryWindows();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (!copyViewModel().startupPending) {
                persistMainWindowPlacement(hwnd);
            }
            KillTimer(hwnd, kDashboardChartRefreshTimerId);
            dismissUsageTooltip();
            if (usageTooltip != nullptr && IsWindow(usageTooltip)) {
                DestroyWindow(usageTooltip);
            }
            if (connectionTooltip != nullptr && IsWindow(connectionTooltip)) {
                DestroyWindow(connectionTooltip);
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                preferencesWindow = nullptr;
                preferencesInfo = nullptr;
                overlayCheckbox = nullptr;
                scheduledExportGroup = nullptr;
                scheduledExportInfo = nullptr;
                scheduledExportCheckbox = nullptr;
                scheduledExportIntervalLabel = nullptr;
                scheduledExportIntervalEdit = nullptr;
                scheduledExportIntervalSuffix = nullptr;
                scheduledExportPathLabel = nullptr;
                scheduledExportPathEdit = nullptr;
                scheduledExportLastRunLabel = nullptr;
                scheduledExportLastRunValue = nullptr;
                openaiSubscriptionCheckbox = nullptr;
                codexModelAttributionCheckbox = nullptr;
                miniMaxSubscriptionCheckbox = nullptr;
                startupCheckbox = nullptr;
                startupBackgroundCheckbox = nullptr;
                miniMaxAccountsInfo = nullptr;
                miniMaxCodingStatus = nullptr;
                miniMaxCodingDetail = nullptr;
                miniMaxTokenStatus = nullptr;
                miniMaxTokenDetail = nullptr;
                miniMaxPlanLabel = nullptr;
                miniMaxPlanCombo = nullptr;
                miniMaxAccountLabel = nullptr;
                miniMaxAccountEdit = nullptr;
                ++miniMaxAccountEditRevision;
                miniMaxSecretLabel = nullptr;
                miniMaxSecretEdit = nullptr;
                miniMaxSaveButton = nullptr;
                miniMaxValidateButton = nullptr;
                miniMaxFeedbackLabel = nullptr;
                preferencesSaveStatus = nullptr;
                preferencesRetryButton = nullptr;
                preferencesCloseButton = nullptr;
                helpDialog = {};
                aboutDialog = {};
                mainTab = nullptr;
                dashboardViewport = nullptr;
                codexThroughputGroup = nullptr;
                codexThroughputText = nullptr;
                codexThroughputAveragesText = nullptr;
                codexThroughputTotalsText = nullptr;
                codexThroughputChart = nullptr;
                openAiUsageGroup = nullptr;
                openAiUsageText = nullptr;
                openAiUsageChart = nullptr;
                hicapUsageGroup = nullptr;
                hicapUsageText = nullptr;
                hicapUsageChart = nullptr;
                providerList = nullptr;
                historyButton = nullptr;
                dashboardSettingsButton = nullptr;
                usageRows.clear();
                quotaRowCount = 0;
                mainControlsLaidOut = false;
                providerQuotaRows.clear();
                offlineRetryButton = nullptr;
                offlineNoticeTitle = nullptr;
                offlineNoticeMessage = nullptr;
                offlineDismissButton = nullptr;
                connectionTooltip = nullptr;
                usageTooltip = nullptr;
                recoveryLabel = nullptr;
                recoveryValue = nullptr;
                scheduledExportLabel = nullptr;
                scheduledExportValue = nullptr;
            }
            releaseMenuBar();
            releaseFonts();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    COLORREF dashboardColor(COLORREF color, int systemColor) const {
        return desktopTheme.color(color, systemColor);
    }

    void drawQuotaBar(const DRAWITEMSTRUCT& draw, const UsageProgressRow& row) {
        HDC dc = draw.hDC;
        const int saved = SaveDC(dc);
        const RECT rect = draw.rcItem;
        DesktopTheme::fill(dc, rect, GetSysColor(COLOR_WINDOW));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));

        const auto segment = [&](LONG left, LONG right, int percentage, COLORREF color, const std::wstring& text,
                                 const std::wstring& reset) {
            RECT label{left, rect.top, right, rect.top + scaledPixels(16)};
            SelectObject(dc, dashboardLabelFont);
            DrawTextW(dc, text.c_str(), -1, &label, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (percentage < 0 || right <= left) {
                return;
            }

            RECT track{left, rect.top + scaledPixels(17), right, rect.top + scaledPixels(28)};
            DesktopTheme::fill(dc, track, dashboardColor(RGB(239, 241, 246), COLOR_WINDOW));
            if (desktopTheme.usesHighContrast()) {
                FrameRect(dc, &track, GetSysColorBrush(COLOR_WINDOWTEXT));
                InflateRect(&track, -1, -1);
            } else {
                DrawEdge(dc, &track, EDGE_SUNKEN, BF_RECT | BF_ADJUST);
            }
            track.right = track.left + MulDiv(percentage, track.right - track.left, 100);
            DesktopTheme::fill(dc, track, dashboardColor(color, COLOR_HIGHLIGHT));

            RECT resetBounds{left, rect.top + scaledPixels(30), right, rect.bottom};
            SelectObject(dc, dashboardBodyFont);
            DrawTextW(dc, reset.c_str(), -1, &resetBounds, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        };

        const bool paired = row.secondaryPercentage >= 0;
        const int gap = scaledPixels(10);
        const int middle = (rect.left + rect.right) / 2;
        segment(rect.left, paired ? middle - gap / 2 : rect.right, row.percentage, row.barColor, row.overlayText,
                row.resetText);
        if (paired) {
            segment(middle + gap / 2, rect.right, row.secondaryPercentage, row.secondaryBarColor,
                    row.secondaryOverlayText, row.secondaryResetText);
        }
        RestoreDC(dc, saved);
    }

    void paintDashboardViewport(HWND hwnd) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        DesktopTheme::fill(dc, paint.rcPaint, desktopTheme.panelColor());
        const auto drawCard = [&](const RECT& card) {
            RECT visible{};
            if (!IntersectRect(&visible, &card, &paint.rcPaint)) {
                return;
            }
            DesktopTheme::fill(dc, card, GetSysColor(COLOR_WINDOW));
            if (desktopTheme.usesHighContrast()) {
                FrameRect(dc, &card, GetSysColorBrush(COLOR_WINDOWTEXT));
            } else {
                RECT edge = card;
                DrawEdge(dc, &edge, EDGE_ETCHED, BF_RECT);
            }
        };
        if (selectedMainTab == 0) {
            for (const auto& card : overviewCards) {
                drawCard(card);
            }
            if (!IsRectEmpty(&noticeRect)) {
                DesktopTheme::fill(dc, noticeRect, dashboardColor(RGB(255, 248, 216), COLOR_WINDOW));
                RECT accent{noticeRect.left, noticeRect.top, noticeRect.left + scaledPixels(3), noticeRect.bottom};
                DesktopTheme::fill(dc, accent, dashboardColor(RGB(186, 114, 31), COLOR_WINDOWTEXT));
            }
        } else {
            drawCard(otherPageCard);
        }
        EndPaint(hwnd, &paint);
    }

    static LRESULT CALLBACK dashboardViewportProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self == nullptr) {
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        switch (msg) {
            case WM_PAINT:
                self->paintDashboardViewport(hwnd);
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_VSCROLL:
                self->scrollOverview(LOWORD(wParam));
                return 0;
            case WM_CTLCOLORSTATIC:
            case WM_DRAWITEM:
            case WM_NOTIFY:
            case WM_COMMAND:
                return SendMessageW(GetParent(hwnd), msg, wParam, lParam);
            default:
                return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    static LRESULT CALLBACK mainWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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

        return self->handleMainMessage(hwnd, msg, wParam, lParam);
    }

    static LRESULT CALLBACK preferencesWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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

        return self->handlePreferencesMessage(hwnd, msg, wParam, lParam);
    }

    static LRESULT CALLBACK analysisWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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

        return self->handleAnalysisMessage(hwnd, msg, wParam, lParam);
    }

    static LRESULT CALLBACK shellDialogWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* params = static_cast<ShellDialogCreateParams*>(create->lpCreateParams);
            if (params == nullptr || params->owner == nullptr) {
                return FALSE;
            }

            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(params->owner));
            SetPropW(
                hwnd,
                kShellDialogKindProperty,
                reinterpret_cast<HANDLE>(static_cast<INT_PTR>(params->dialog)));
            delete params;
            return TRUE;
        }

        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self == nullptr) {
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        return self->handleShellDialogMessage(hwnd, msg, wParam, lParam);
    }

    static LRESULT CALLBACK codexChartWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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
        return self->handleCodexChartMessage(hwnd, msg, wParam, lParam);
    }

    LRESULT handleCodexChartMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (hdc != nullptr) {
                RECT rect;
                GetClientRect(hwnd, &rect);
                DashboardViewModel vm;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    vm = viewModel;
                }
                if (hwnd == openAiUsageChart || hwnd == hicapUsageChart) {
                    const auto& chart =
                        hwnd == openAiUsageChart ? vm.analysis.openAiCodexModelUsage : vm.analysis.hicapCodexModelUsage;
                    const bool hovered = modelChartHoverWindow == hwnd;
                    const auto cursor = hovered && chartCountdownHoverWindow != hwnd
                                            ? std::optional<POINT>{modelChartHoverCursor}
                                            : std::nullopt;
                    const auto hit = drawModelUsageChart(hdc, rect, chart, cursor);
                    if (hovered) {
                        modelChartHoverHit = hit;
                    }
                } else {
                    drawCodexThroughputChart(hdc, rect, vm.analysis.codexHourlyHistory, codexChartHoverIndex);
                }
                drawChartRefreshCountdown(hdc, rect, hwnd);
                EndPaint(hwnd, &ps);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (hwnd == codexThroughputChart) {
                if (!codexChartTrackingMouse) {
                    TRACKMOUSEEVENT tme{};
                    tme.cbSize = sizeof(tme);
                    tme.dwFlags = TME_LEAVE;
                    tme.hwndTrack = hwnd;
                    TrackMouseEvent(&tme);
                    codexChartTrackingMouse = true;
                }
                POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                const bool countdownHovered = updateChartCountdownHover(hwnd, pt, rectFromHwnd(hwnd));
                AnalysisCodexHourlyHistory hist;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    hist = viewModel.analysis.codexHourlyHistory;
                }
                int newHover = countdownHovered ? -1 : codexChartHitTest(pt, rectFromHwnd(hwnd), hist);
                if (newHover != codexChartHoverIndex) {
                    codexChartHoverIndex = newHover;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }

            if (hwnd == openAiUsageChart || hwnd == hicapUsageChart) {
                if (!modelChartTrackingMouse || modelChartHoverWindow != hwnd) {
                    TRACKMOUSEEVENT tme{};
                    tme.cbSize = sizeof(tme);
                    tme.dwFlags = TME_LEAVE;
                    tme.hwndTrack = hwnd;
                    TrackMouseEvent(&tme);
                    modelChartTrackingMouse = true;
                }
                const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                const RECT rect = rectFromHwnd(hwnd);
                const bool countdownHovered = updateChartCountdownHover(hwnd, pt, rect);
                AnalysisProviderModelUsageChart chart;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    chart = hwnd == openAiUsageChart
                        ? viewModel.analysis.openAiCodexModelUsage
                        : viewModel.analysis.hicapCodexModelUsage;
                }
                ModelChartPointHit hit;
                if (!countdownHovered && chart.hasData && PtInRect(&rect, pt)) {
                    hit = modelChartHitTest(pt, modelChartPlot(rect, chart));
                }
                modelChartHoverCursor = pt;
                if (modelChartHoverWindow != hwnd || !(hit == modelChartHoverHit)) {
                    HWND previous = modelChartHoverWindow;
                    modelChartHoverWindow = hwnd;
                    modelChartHoverHit = hit;
                    if (previous != nullptr && previous != hwnd) {
                        InvalidateRect(previous, nullptr, FALSE);
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }

            return 0;
        }
        case WM_MOUSELEAVE: {
            if (hwnd == codexThroughputChart) {
                codexChartTrackingMouse = false;
                clearChartCountdownHover(hwnd);
                if (codexChartHoverIndex != -1) {
                    codexChartHoverIndex = -1;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }

            if (hwnd == openAiUsageChart || hwnd == hicapUsageChart) {
                modelChartTrackingMouse = false;
                clearChartCountdownHover(hwnd);
                if (modelChartHoverWindow == hwnd) {
                    modelChartHoverWindow = nullptr;
                    modelChartHoverHit = {};
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }

            if (codexChartHoverIndex != -1) {
                codexChartHoverIndex = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    RECT rectFromHwnd(HWND hwnd) {
        RECT r{};
        GetClientRect(hwnd, &r);
        return r;
    }

    LONG throughputPointX(const RECT& plot, const AnalysisCodexHourlyHistory& history, size_t index) const {
        const auto start = history.points.front().snapshotTime;
        const auto end = history.points.back().snapshotTime;
        const LONG width = plot.right - plot.left;
        if (end <= start) {
            return plot.left + width / 2;
        }
        return plot.left + static_cast<LONG>(static_cast<double>(history.points[index].snapshotTime - start) /
                                             (end - start) * width);
    }

    int codexChartHitTest(POINT pt, const RECT& rect, const AnalysisCodexHourlyHistory& history) {
        if (!history.hasData || history.points.empty()) {
            return -1;
        }
        const RECT plot = dashboardPlotRect(rect, true);
        if (pt.x < plot.left || pt.x > plot.right || pt.y < plot.top || pt.y > plot.bottom) {
            return -1;
        }
        int nearest = -1;
        LONG nearestDistance = history.points.size() == 1 ? scaledPixels(14) : plot.right - plot.left + 1;
        for (size_t index = 0; index < history.points.size(); ++index) {
            const LONG distance = std::abs(throughputPointX(plot, history, index) - pt.x);
            if (distance < nearestDistance) {
                nearestDistance = distance;
                nearest = static_cast<int>(index);
            }
        }
        return nearest;
    }

    int dashboardChartRefreshSecondsRemaining() const {
        if (nextDashboardChartRefreshTick == 0) {
            return static_cast<int>(kDashboardChartRefreshIntervalMs / 1000);
        }

        const ULONGLONG nowTick = GetTickCount64();
        if (nowTick >= nextDashboardChartRefreshTick) {
            return 0;
        }

        const ULONGLONG remainingMs = nextDashboardChartRefreshTick - nowTick;
        return static_cast<int>((remainingMs + 999) / 1000);
    }

    RECT chartRefreshCountdownRect(const RECT& rect) const {
        return RECT{std::max(rect.left + scaledPixels(8), rect.right - scaledPixels(50)), rect.top + scaledPixels(2),
                    rect.right - scaledPixels(8), rect.top + scaledPixels(21)};
    }

    bool updateChartCountdownHover(HWND hwnd, POINT pt, const RECT& rect) {
        RECT countdownRect = chartRefreshCountdownRect(rect);
        const bool hovered = PtInRect(&countdownRect, pt) != 0;
        HWND previous = chartCountdownHoverWindow;
        HWND next = hovered ? hwnd : nullptr;
        if (previous != next) {
            chartCountdownHoverWindow = next;
            if (previous != nullptr) {
                InvalidateRect(previous, nullptr, FALSE);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return hovered;
    }

    void clearChartCountdownHover(HWND hwnd) {
        if (chartCountdownHoverWindow == hwnd) {
            chartCountdownHoverWindow = nullptr;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }

    void drawChartRefreshCountdown(HDC hdc, const RECT& rect, HWND hwnd) {
        const int remainingSeconds = dashboardChartRefreshSecondsRemaining();
        std::wostringstream label;
        label << (remainingSeconds / 60)
              << L":"
              << std::setw(2)
              << std::setfill(L'0')
              << (remainingSeconds % 60);

        const int saved = SaveDC(hdc);
        SelectObject(hdc, dashboardBodyFont);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, dashboardColor(RGB(71, 88, 113), COLOR_WINDOWTEXT));

        RECT textRect = chartRefreshCountdownRect(rect);
        const std::wstring text = label.str();
        DrawTextW(hdc, text.c_str(), -1, &textRect, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

        if (chartCountdownHoverWindow == hwnd) {
            drawTooltipBox(
                hdc,
                L"Graph refresh timer\nInterval: 1 minute",
                textRect.right,
                textRect.bottom,
                rect.left + 8,
                rect.top + 3,
                rect.right - 8,
                rect.bottom - 8);
        }
        RestoreDC(hdc, saved);
    }

    void drawTooltipBox(
        HDC hdc,
        const std::wstring& tipText,
        int anchorX,
        int anchorY,
        int chartLeft,
        int chartTop,
        int chartRight,
        int chartBottom) {
        SIZE lineSize{};
        int lineCount = 0;
        int maxLineW = 0;
        int lineHeight = 0;
        size_t pos = 0;
        while (pos <= tipText.size()) {
            size_t end = tipText.find(L'\n', pos);
            if (end == std::wstring::npos) end = tipText.size();
            GetTextExtentPoint32W(hdc, tipText.c_str() + pos, static_cast<int>(end - pos), &lineSize);
            maxLineW = std::max(maxLineW, static_cast<int>(lineSize.cx));
            lineHeight = std::max(lineHeight, static_cast<int>(lineSize.cy));
            ++lineCount;
            if (end == tipText.size()) break;
            pos = end + 1;
        }
        lineHeight = std::max(14, lineHeight);

        const int boxPad = 6;
        const int boxW = maxLineW + boxPad * 2;
        const int boxH = lineHeight * lineCount + boxPad * 2;
        int boxX = anchorX + 8;
        int boxY = anchorY - boxH - 8;
        if (boxX + boxW > chartRight) boxX = anchorX - boxW - 8;
        if (boxY < chartTop) boxY = anchorY + 8;
        if (boxY + boxH > chartBottom) boxY = chartBottom - boxH;
        if (boxX < chartLeft) boxX = chartLeft;

        RECT boxRect{ boxX, boxY, boxX + boxW, boxY + boxH };
        HBRUSH tipBrush = CreateSolidBrush(dashboardColor(RGB(245, 248, 255), COLOR_WINDOW));
        FillRect(hdc, &boxRect, tipBrush);
        DeleteObject(tipBrush);
        HPEN tipPen = CreatePen(PS_SOLID, 1, dashboardColor(RGB(169, 187, 217), COLOR_WINDOWTEXT));
        HGDIOBJ oldPen = SelectObject(hdc, tipPen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, boxRect.left, boxRect.top, boxRect.right, boxRect.bottom);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(tipPen);

        SetTextColor(hdc, dashboardColor(RGB(29, 42, 65), COLOR_WINDOWTEXT));
        SetBkMode(hdc, TRANSPARENT);
        int ty = boxY + boxPad;
        pos = 0;
        while (pos <= tipText.size()) {
            size_t end = tipText.find(L'\n', pos);
            if (end == std::wstring::npos) end = tipText.size();
            TextOutW(hdc, boxX + boxPad, ty, tipText.c_str() + pos, static_cast<int>(end - pos));
            ty += lineHeight;
            if (end == tipText.size()) break;
            pos = end + 1;
        }
    }

    RECT dashboardPlotRect(const RECT& rect, bool throughput) const {
        return {rect.left + scaledPixels(55), rect.top + scaledPixels(35),
                rect.right - scaledPixels(throughput ? 44 : 10), rect.bottom - scaledPixels(24)};
    }

    std::wstring chartScaleLabel(double value) const {
        for (const auto& [unit, suffix] :
             {std::pair{1e12, L"T"}, std::pair{1e9, L"B"}, std::pair{1e6, L"M"}, std::pair{1e3, L"K"}}) {
            if (value >= unit) {
                std::wostringstream label;
                label << std::fixed << std::setprecision(std::fmod(value, unit) == 0.0 ? 0 : 1) << value / unit
                      << suffix;
                return label.str();
            }
        }
        if (std::floor(value) != value) {
            std::wostringstream label;
            label << std::fixed << std::setprecision(2) << value;
            std::wstring text = label.str();
            if (text.back() == L'0') {
                text.pop_back();
            }
            return text;
        }
        return std::to_wstring(static_cast<long long>(value));
    }

    void drawChartAxes(HDC dc, const RECT& plot, double leftScale, double rightScale = 0) {
        const int gap = scaledPixels(7);
        const int labelHeight = scaledPixels(17);
        SelectObject(dc, dashboardBodyFont);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, dashboardColor(RGB(71, 88, 113), COLOR_WINDOWTEXT));
        for (int tick = 0; tick <= 4; ++tick) {
            const int y = plot.bottom - (plot.bottom - plot.top) * tick / 4;
            RECT grid{plot.left, y, plot.right, y + 1};
            DesktopTheme::fill(dc, grid,
                               dashboardColor(tick == 0 ? RGB(185, 199, 221) : RGB(229, 235, 244), COLOR_WINDOWTEXT));
            RECT left{plot.left - scaledPixels(55), y - labelHeight / 2, plot.left - gap, y + labelHeight / 2};
            const auto leftLabel = chartScaleLabel(leftScale * tick / 4);
            DrawTextW(dc, leftLabel.c_str(), -1, &left, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (rightScale > 0) {
                RECT right{plot.right + gap, left.top, plot.right + scaledPixels(44), left.bottom};
                const auto rightLabel = chartScaleLabel(rightScale * tick / 4);
                DrawTextW(dc, rightLabel.c_str(), -1, &right, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
        }
    }

    void drawChartTimeLabels(HDC dc, const RECT& rect, const RECT& plot, std::time_t first, std::time_t last) {
        const LONG top = plot.bottom + scaledPixels(5);
        SetTextColor(dc, dashboardColor(RGB(71, 88, 113), COLOR_WINDOWTEXT));
        const auto firstLabel = utf8ToWide(formatChartBucketLabel(first));
        if (first == last) {
            RECT label{plot.left, top, plot.right, rect.bottom};
            DrawTextW(dc, firstLabel.c_str(), -1, &label, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
        } else {
            const LONG middle = (plot.left + plot.right) / 2;
            RECT left{plot.left, top, middle, rect.bottom};
            RECT right{middle, top, plot.right, rect.bottom};
            const auto lastLabel = utf8ToWide(formatChartBucketLabel(last));
            DrawTextW(dc, firstLabel.c_str(), -1, &left, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            DrawTextW(dc, lastLabel.c_str(), -1, &right, DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }

    void drawEmptyChart(HDC dc, RECT rect, const wchar_t* text) {
        DesktopTheme::fill(dc, rect, dashboardColor(RGB(245, 247, 251), COLOR_WINDOW));
        if (desktopTheme.usesHighContrast()) {
            FrameRect(dc, &rect, GetSysColorBrush(COLOR_WINDOWTEXT));
        }
        SelectObject(dc, dashboardBodyFont);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, dashboardColor(RGB(71, 88, 113), COLOR_WINDOWTEXT));
        DrawTextW(dc, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    COLORREF modelSeriesColor(size_t index) const {
        const COLORREF colors[] = {RGB(42, 99, 205),  RGB(168, 79, 31), RGB(19, 122, 99),
                                   RGB(120, 72, 173), RGB(9, 111, 147), RGB(146, 62, 111)};
        return dashboardColor(colors[index % std::size(colors)], COLOR_WINDOWTEXT);
    }

    static constexpr int kModelMarkerRadius = 5;

    HRGN createModelMarkerRegion(POINT center, size_t series, int size) const {
        const LONG radius = scaledPixels(size);
        const LONG x = center.x;
        const LONG y = center.y;
        switch (series % 6) {
            case 0:
                return CreateEllipticRgn(x - radius, y - radius, x + radius + 1, y + radius + 1);
            case 1:
                return CreateRectRgn(x - radius, y - radius, x + radius + 1, y + radius + 1);
            case 2: {
                const POINT triangle[] = {{x, y - radius}, {x + radius, y + radius}, {x - radius, y + radius}};
                return CreatePolygonRgn(triangle, static_cast<int>(std::size(triangle)), WINDING);
            }
            case 3: {
                const POINT diamond[] = {{x, y - radius}, {x + radius, y}, {x, y + radius}, {x - radius, y}};
                return CreatePolygonRgn(diamond, static_cast<int>(std::size(diamond)), WINDING);
            }
            case 4: {
                const POINT triangle[] = {{x - radius, y - radius}, {x + radius, y - radius}, {x, y + radius}};
                return CreatePolygonRgn(triangle, static_cast<int>(std::size(triangle)), WINDING);
            }
            default: {
                HRGN bar = CreateRectRgn(x - radius, y, x + radius + 1, y + 1);
                HRGN stem = CreateRectRgn(x, y - radius, x + 1, y + radius + 1);
                const bool joined = bar != nullptr && stem != nullptr && CombineRgn(bar, bar, stem, RGN_OR) != ERROR;
                if (stem != nullptr) {
                    DeleteObject(stem);
                }
                if (!joined && bar != nullptr) {
                    DeleteObject(bar);
                }
                return joined ? bar : nullptr;
            }
        }
    }

    void drawModelMarker(HDC dc, POINT center, size_t series, int size = kModelMarkerRadius) {
        HRGN shape = createModelMarkerRegion(center, series, size);
        if (shape == nullptr) {
            return;
        }

        const auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
        const COLORREF previous = SetDCBrushColor(dc, modelSeriesColor(series));
        const int saved = desktopTheme.usesHighContrast() ? SaveDC(dc) : 0;
        if (saved != 0 && ExtSelectClipRgn(dc, shape, RGN_AND) != ERROR) {
            FillRgn(dc, shape, GetSysColorBrush(COLOR_WINDOW));
            FrameRgn(dc, shape, brush, 1, 1);
        } else {
            FillRgn(dc, shape, brush);
        }
        if (saved != 0) {
            RestoreDC(dc, saved);
        }
        SetDCBrushColor(dc, previous);
        DeleteObject(shape);
    }

    void drawModelUsageSummary(const DRAWITEMSTRUCT& draw) {
        HDC dc = draw.hDC;
        const int saved = SaveDC(dc);
        DesktopTheme::fill(dc, draw.rcItem, GetSysColor(COLOR_WINDOW));
        SelectObject(dc, dashboardBodyFont);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, dashboardColor(RGB(71, 88, 113), COLOR_WINDOWTEXT));
        const auto lines = splitOwnerDrawLines(utf8ToWide(getWindowTextUtf8(draw.hwndItem)));
        const int lineHeight = scaledPixels(16);
        for (size_t index = 0; index < lines.size(); ++index) {
            RECT line = draw.rcItem;
            line.top += static_cast<int>(index) * lineHeight;
            if (index == 0) {
                DrawTextW(dc, lines[index].c_str(), -1, &line, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            } else {
                drawModelMarker(dc, {line.left + scaledPixels(5), line.top + scaledPixels(8)}, index - 1);
                line.left += scaledPixels(15);
                SetTextColor(dc, dashboardColor(RGB(27, 40, 61), COLOR_WINDOWTEXT));
                DrawTextW(dc, lines[index].c_str(), -1, &line, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        }
        RestoreDC(dc, saved);
    }

    struct ModelChartMarker {
        POINT center;
        size_t seriesIndex;
        size_t pointIndex;
    };

    struct ModelChartPlot {
        RECT bounds;
        double maxTokens = 4;
        std::time_t first = 0;
        std::time_t last = 0;
        std::vector<std::vector<POINT>> seriesPoints;
        std::vector<ModelChartMarker> markers;

        POINT position(std::time_t time, long long tokens) const {
            const LONG width = bounds.right - bounds.left;
            const LONG height = bounds.bottom - bounds.top;
            const LONG x = last == first
                               ? width / 2
                               : static_cast<LONG>(static_cast<double>(time - first) / (last - first) * width);
            return {bounds.left + x,
                    bounds.bottom - static_cast<LONG>(static_cast<double>(tokens) / maxTokens * height)};
        }
    };

    ModelChartPlot modelChartPlot(const RECT& rect, const AnalysisProviderModelUsageChart& chart) const {
        ModelChartPlot plot{};
        plot.bounds = dashboardPlotRect(rect, false);
        for (const auto& series : chart.series) {
            for (const auto& point : series.points) {
                plot.maxTokens = std::max(plot.maxTokens, static_cast<double>(point.totalTokens));
                plot.first = plot.first == 0 ? point.bucketStartUtc : std::min(plot.first, point.bucketStartUtc);
                plot.last = std::max(plot.last, point.bucketStartUtc);
            }
        }
        // Four equal intervals with whole-number tick labels.
        const double step = std::pow(10.0, std::floor(std::log10(plot.maxTokens / 4)));
        plot.maxTokens = std::ceil(plot.maxTokens / (4 * step)) * 4 * step;
        plot.seriesPoints.resize(chart.series.size());
        for (size_t index = 0; index < chart.series.size(); ++index) {
            auto& points = plot.seriesPoints[index];
            const auto& samples = chart.series[index].points;
            points.reserve(samples.size());
            for (const auto& sample : samples) {
                points.push_back(plot.position(sample.bucketStartUtc, sample.totalTokens));
            }
        }
        if (desktopTheme.usesHighContrast()) {
            plot.markers = contrastModelMarkers(plot.bounds, plot.seriesPoints);
        } else {
            for (size_t index = 0; index < plot.seriesPoints.size(); ++index) {
                const auto& points = plot.seriesPoints[index];
                if (points.size() == 1) {
                    plot.markers.push_back({points.front(), index, 0});
                }
            }
        }
        return plot;
    }

    ModelChartPointHit modelChartHitTest(POINT pt, const ModelChartPlot& plot) const {
        const int markerRadius = scaledPixels(kModelMarkerRadius);
        // The box filters out distant markers. Only the painted region gets priority over samples.
        for (auto marker = plot.markers.rbegin(); marker != plot.markers.rend(); ++marker) {
            if (std::abs(marker->center.x - pt.x) > markerRadius || std::abs(marker->center.y - pt.y) > markerRadius) {
                continue;
            }
            HRGN shape = createModelMarkerRegion(marker->center, marker->seriesIndex, kModelMarkerRadius);
            if (shape == nullptr) {
                continue;
            }
            const bool inside = PtInRegion(shape, pt.x, pt.y) != FALSE;
            DeleteObject(shape);
            if (inside) {
                return {static_cast<int>(marker->seriesIndex), static_cast<int>(marker->pointIndex), marker->center,
                        true};
            }
        }
        ModelChartPointHit hit;
        const int radius = scaledPixels(14);
        int nearest = radius * radius;
        for (size_t series = 0; series < plot.seriesPoints.size(); ++series) {
            const auto& points = plot.seriesPoints[series];
            for (size_t index = 0; index < points.size(); ++index) {
                const POINT point = points[index];
                const int dx = point.x - pt.x;
                const int dy = point.y - pt.y;
                const int distance = dx * dx + dy * dy;
                if (distance < nearest) {
                    nearest = distance;
                    hit = {static_cast<int>(series), static_cast<int>(index), point};
                }
            }
        }
        return hit;
    }

    void drawChartSeries(HDC dc, const std::vector<POINT>& points, COLORREF color) {
        if (points.size() == 1) {
            const auto point = points.front();
            const int radius = scaledPixels(3);
            const int saved = SaveDC(dc);
            SelectObject(dc, GetStockObject(NULL_PEN));
            SelectObject(dc, GetStockObject(DC_BRUSH));
            SetDCBrushColor(dc, color);
            Ellipse(dc, point.x - radius, point.y - radius, point.x + radius, point.y + radius);
            RestoreDC(dc, saved);
        } else if (!points.empty()) {
            HPEN pen = CreatePen(PS_SOLID, scaledPixels(2), color);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            Polyline(dc, points.data(), static_cast<int>(points.size()));
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }
    }

    std::vector<ModelChartMarker> contrastModelMarkers(const RECT& plot,
                                                       const std::vector<std::vector<POINT>>& seriesPoints) const {
        std::vector<ModelChartMarker> markers;
        const LONG markerGap = 2L * scaledPixels(kModelMarkerRadius) + 2;
        const auto isClear = [&](POINT center) {
            return std::none_of(markers.begin(), markers.end(), [&](const ModelChartMarker& marker) {
                return std::abs(center.x - marker.center.x) < markerGap &&
                       std::abs(center.y - marker.center.y) < markerGap;
            });
        };

        // Favor a point that can be distinguished from the other series, including on short traces.
        const size_t shared = seriesPoints.size();
        std::map<std::pair<LONG, LONG>, size_t> owners;
        for (size_t index = 0; index < seriesPoints.size(); ++index) {
            for (const auto point : seriesPoints[index]) {
                auto [entry, inserted] = owners.try_emplace(std::make_pair(point.x, point.y), index);
                if (!inserted && entry->second != index) {
                    entry->second = shared;
                }
            }
        }
        for (size_t index = 0; index < seriesPoints.size(); ++index) {
            const auto& points = seriesPoints[index];
            if (points.empty()) {
                continue;
            }
            const auto distinct = std::find_if(points.rbegin(), points.rend(),
                                               [&](POINT point) { return owners.at({point.x, point.y}) == index; });
            const size_t sampleIndex = distinct == points.rend()
                                           ? points.size() - 1
                                           : static_cast<size_t>(distinct.base() - points.begin() - 1);
            const POINT anchor = points[sampleIndex];
            POINT center = anchor;
            // Keep the sample at its actual position. Move only its marker when the shapes would overlap.
            const LONG span = plot.right - plot.left;
            for (LONG offset = markerGap; !isClear(center) && offset <= span; offset += markerGap) {
                const POINT left{anchor.x - offset, anchor.y};
                const POINT right{anchor.x + offset, anchor.y};
                if (left.x >= plot.left && isClear(left)) {
                    center = left;
                } else if (right.x <= plot.right && isClear(right)) {
                    center = right;
                }
            }
            markers.push_back({center, index, sampleIndex});
        }

        // Reserve every series identifier before adding the repeated markers on longer traces.
        const int spacing = scaledPixels(32);
        for (size_t index = 0; index < seriesPoints.size(); ++index) {
            LONG nextX = plot.left;
            const auto& points = seriesPoints[index];
            for (size_t pointIndex = 0; pointIndex < points.size(); ++pointIndex) {
                const POINT point = points[pointIndex];
                if (point.x >= nextX && isClear(point)) {
                    markers.push_back({point, index, pointIndex});
                    nextX = point.x + spacing;
                }
            }
        }
        return markers;
    }

    void drawModelChartMarkers(HDC dc, const ModelChartPlot& plot) {
        if (desktopTheme.usesHighContrast()) {
            const int saved = SaveDC(dc);
            SelectObject(dc, GetStockObject(DC_PEN));
            SetDCPenColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            for (const auto& marker : plot.markers) {
                const POINT anchor = plot.seriesPoints[marker.seriesIndex][marker.pointIndex];
                if (anchor.x != marker.center.x) {
                    MoveToEx(dc, anchor.x, anchor.y, nullptr);
                    LineTo(dc, marker.center.x, marker.center.y);
                }
            }
            RestoreDC(dc, saved);
        }
        for (const auto& marker : plot.markers) {
            drawModelMarker(dc, marker.center, marker.seriesIndex);
        }
    }

    ModelChartPointHit drawModelUsageChart(HDC dc, const RECT& rect, const AnalysisProviderModelUsageChart& chart,
                                           std::optional<POINT> cursor) {
        const int saved = SaveDC(dc);
        if (!chart.hasData || chart.series.empty()) {
            drawEmptyChart(dc, rect, L"Waiting for new model usage");
            RestoreDC(dc, saved);
            return {};
        }

        DesktopTheme::fill(dc, rect, GetSysColor(COLOR_WINDOW));
        const ModelChartPlot plot = modelChartPlot(rect, chart);
        drawChartAxes(dc, plot.bounds, plot.maxTokens);
        RECT units{rect.left, rect.top + scaledPixels(4), plot.bounds.right, plot.bounds.top};
        DrawTextW(dc, L"Tokens per 5-minute bucket", -1, &units, DT_SINGLELINE | DT_NOPREFIX);
        drawChartTimeLabels(dc, rect, plot.bounds, plot.first, plot.last);

        for (size_t index = 0; index < plot.seriesPoints.size(); ++index) {
            const auto& points = plot.seriesPoints[index];
            if (points.size() > 1) {
                drawChartSeries(dc, points, modelSeriesColor(index));
            }
        }
        drawModelChartMarkers(dc, plot);

        ModelChartPointHit hit;
        if (cursor && PtInRect(&rect, *cursor)) {
            hit = modelChartHitTest(*cursor, plot);
        }
        if (hit.seriesIndex >= 0) {
            const auto& series = chart.series[static_cast<size_t>(hit.seriesIndex)];
            const auto& value = series.points[static_cast<size_t>(hit.pointIndex)];
            const POINT point = hit.position;
            if (!hit.onMarker) {
                drawModelMarker(dc, point, static_cast<size_t>(hit.seriesIndex), 7);
            }
            std::wostringstream tooltip;
            tooltip << utf8ToWide(series.modelKey.empty() ? "unknown" : series.modelKey) << L"\n"
                    << L"Tokens: " << utf8ToWide(formatNumber(value.totalTokens)) << L"\n"
                    << L"Bucket: " << utf8ToWide(formatChartBucketLabel(value.bucketStartUtc));
            drawTooltipBox(dc, tooltip.str(), point.x, point.y, rect.left, rect.top, rect.right, rect.bottom);
        }
        RestoreDC(dc, saved);
        return hit;
    }

    void drawCodexThroughputChart(HDC dc, const RECT& rect, const AnalysisCodexHourlyHistory& history, int hoverIndex) {
        const int saved = SaveDC(dc);
        if (!history.hasData || history.points.empty()) {
            drawEmptyChart(dc, rect, L"No Codex throughput in the last 30 minutes");
            RestoreDC(dc, saved);
            return;
        }

        DesktopTheme::fill(dc, rect, GetSysColor(COLOR_WINDOW));
        const RECT plot = dashboardPlotRect(rect, true);
        const LONG height = plot.bottom - plot.top;
        double maxTokens = 1.0;
        double maxPrompts = 1.0;
        for (const auto& point : history.points) {
            maxTokens = std::max(maxTokens, point.tokensPerMinute);
            maxPrompts = std::max(maxPrompts, point.promptsPerMinute);
        }
        maxTokens = std::pow(2.0, std::ceil(std::log2(maxTokens)));
        maxPrompts = std::pow(2.0, std::ceil(std::log2(maxPrompts)));
        drawChartAxes(dc, plot, maxTokens, maxPrompts);
        drawChartTimeLabels(dc, rect, plot, history.points.front().snapshotTime, history.points.back().snapshotTime);

        const COLORREF tokenColor = modelSeriesColor(0);
        const COLORREF promptColor = dashboardColor(RGB(168, 79, 31), COLOR_HIGHLIGHT);
        RECT tokenUnits{rect.left, rect.top + scaledPixels(4), rect.right, plot.top};
        SetTextColor(dc, tokenColor);
        DrawTextW(dc, L"Tokens / min", -1, &tokenUnits, DT_SINGLELINE | DT_NOPREFIX);
        RECT promptUnits = tokenUnits;
        promptUnits.right -= scaledPixels(57);
        SetTextColor(dc, promptColor);
        DrawTextW(dc, L"Prompts / min", -1, &promptUnits, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);

        std::vector<POINT> tokens;
        std::vector<POINT> prompts;
        const size_t count = history.points.size();
        tokens.reserve(count);
        prompts.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            const auto& point = history.points[index];
            const LONG x = throughputPointX(plot, history, index);
            tokens.push_back({x, plot.bottom - static_cast<LONG>(point.tokensPerMinute / maxTokens * height)});
            prompts.push_back({x, plot.bottom - static_cast<LONG>(point.promptsPerMinute / maxPrompts * height)});
        }
        drawChartSeries(dc, tokens, tokenColor);
        drawChartSeries(dc, prompts, promptColor);

        if (hoverIndex >= 0 && static_cast<size_t>(hoverIndex) < count) {
            const size_t index = static_cast<size_t>(hoverIndex);
            drawChartSeries(dc, {tokens[index]}, tokenColor);
            drawChartSeries(dc, {prompts[index]}, promptColor);
            const auto& point = history.points[index];
            std::wostringstream tooltip;
            tooltip << L"Tokens: " << utf8ToWide(formatRate(point.tokensPerMinute)) << L"/min\n"
                    << L"Prompts: " << utf8ToWide(formatRate(point.promptsPerMinute)) << L"/min\n"
                    << L"Bucket: " << utf8ToWide(formatChartBucketLabel(point.snapshotTime));
            drawTooltipBox(dc, tooltip.str(), tokens[index].x, std::min(tokens[index].y, prompts[index].y), rect.left,
                           rect.top, rect.right, rect.bottom);
        }
        RestoreDC(dc, saved);
    }

    void ensureCodexChartClass() {
        static std::once_flag flag;
        std::call_once(flag, [&]() {
            WNDCLASSEXW wc = {};
            wc.cbSize = sizeof(WNDCLASSEXW);
            wc.lpfnWndProc = codexChartWindowProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = static_cast<HCURSOR>(LoadImageW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW), IMAGE_CURSOR, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
            wc.hbrBackground = nullptr;
            wc.lpszClassName = L"JistatsCodexChart";
            wc.hIcon = reinterpret_cast<HICON>(LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_SHARED));
            RegisterClassExW(&wc);
        });
    }

    void runUiThread(const std::optional<RECT>& savedRect) {
        initializeCommonControls();
        ensureAppIcons();
        HINSTANCE instance = GetModuleHandleW(nullptr);

        WNDCLASSW mainClass{};
        mainClass.lpfnWndProc = &Impl::mainWindowProc;
        mainClass.hInstance = instance;
        mainClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        mainClass.lpszClassName = windowClassName();
        RegisterClassW(&mainClass);

        WNDCLASSW viewportClass{};
        viewportClass.lpfnWndProc = &Impl::dashboardViewportProc;
        viewportClass.hInstance = instance;
        viewportClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        viewportClass.lpszClassName = viewportClassName();
        RegisterClassW(&viewportClass);

        WNDCLASSW preferencesClass{};
        preferencesClass.lpfnWndProc = &Impl::preferencesWindowProc;
        preferencesClass.hInstance = instance;
        preferencesClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        preferencesClass.lpszClassName = preferencesWindowClassName();
        RegisterClassW(&preferencesClass);

        WNDCLASSW analysisClass{};
        analysisClass.lpfnWndProc = &Impl::analysisWindowProc;
        analysisClass.hInstance = instance;
        analysisClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        analysisClass.lpszClassName = analysisWindowClassName();
        RegisterClassW(&analysisClass);

        WNDCLASSW shellDialogClass{};
        shellDialogClass.lpfnWndProc = &Impl::shellDialogWindowProc;
        shellDialogClass.hInstance = instance;
        shellDialogClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        shellDialogClass.lpszClassName = shellDialogWindowClassName();
        RegisterClassW(&shellDialogClass);

        RECT bounds{0, 0, MulDiv(kDefaultWidth, GetDpiForSystem(), 96), MulDiv(kDefaultHeight, GetDpiForSystem(), 96)};
        if (savedRect) {
            bounds = *savedRect;
        } else {
            RECT workArea{};
            if (tryGetMonitorWorkArea(bounds, workArea)) {
                OffsetRect(&bounds, workArea.left + (workArea.right - workArea.left - bounds.right) / 2,
                           workArea.top + (workArea.bottom - workArea.top - bounds.bottom) / 2);
                clampRectToWorkArea(bounds, workArea);
            }
        }

        HWND hwnd = CreateWindowExW(0, windowClassName(), L"JiStats", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                    bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                                    nullptr, nullptr, instance, this);

        {
            std::lock_guard<std::mutex> lock(mutex);
            window = hwnd;
            threadReady = true;
            if (hwnd == nullptr) {
                lastError = "Failed to create dashboard window";
            }
        }
        readyCv.notify_all();

        if (hwnd == nullptr) {
            releaseAppIcons();
            return;
        }

        applyWindowIcons(hwnd);
        GetWindowRect(hwnd, &initialMainWindowRect);
        ShowWindow(hwnd, visible ? SW_SHOW : SW_HIDE);
        UpdateWindow(hwnd);

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.hwnd == hwnd || IsChild(hwnd, msg.hwnd)) {
                if (msg.hwnd == mainTab && msg.message == WM_KEYDOWN &&
                    (msg.wParam == VK_LEFT || msg.wParam == VK_RIGHT)) {
                    // Button-style tabs move only focus by default. Move the focus and selection together.
                    const int direction = msg.wParam == VK_RIGHT ? 1 : -1;
                    const int index = selectedMainTab + direction;
                    if (index >= 0 && index < TabCtrl_GetItemCount(mainTab)) {
                        TabCtrl_SetCurSel(mainTab, index);
                        TabCtrl_SetCurFocus(mainTab, index);
                        activateMainPage(index);
                    }
                    continue;
                }
                if (msg.message == WM_KEYDOWN && selectedMainTab == 0 &&
                    (msg.wParam == VK_PRIOR || msg.wParam == VK_NEXT)) {
                    scrollOverview(msg.wParam == VK_PRIOR ? SB_PAGEUP : SB_PAGEDOWN);
                    continue;
                }
                if (IsDialogMessageW(hwnd, &msg)) {
                    continue;
                }
            } else if (preferencesWindow != nullptr &&
                       (msg.hwnd == preferencesWindow || IsChild(preferencesWindow, msg.hwnd)) &&
                       IsDialogMessageW(preferencesWindow, &msg)) {
                continue;
            } else if (analysisWindow != nullptr && (msg.hwnd == analysisWindow || IsChild(analysisWindow, msg.hwnd)) &&
                       IsDialogMessageW(analysisWindow, &msg)) {
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        UnregisterClassW(shellDialogWindowClassName(), instance);
        UnregisterClassW(analysisWindowClassName(), instance);
        UnregisterClassW(preferencesWindowClassName(), instance);
        UnregisterClassW(viewportClassName(), instance);
        UnregisterClassW(windowClassName(), instance);

        std::lock_guard<std::mutex> lock(mutex);
        window = nullptr;
        preferencesWindow = nullptr;
        analysisWindow = nullptr;
        viewModelSyncPending = false;
        clearShellDialogState(DashboardShellDialog::Help);
        clearShellDialogState(DashboardShellDialog::About);
        visible = false;
        initialized = false;
        releaseAppIcons();
    }
#endif
};

DashboardPresenter::DashboardPresenter() : pImpl(std::make_unique<Impl>()) {
}

DashboardPresenter::~DashboardPresenter() {
    shutdown();
}

bool DashboardPresenter::init(bool deferPlacement) {
    if (pImpl->initialized) {
        return true;
    }

    pImpl->seedDefaultsIfNeeded();
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        pImpl->shutdownRequested = false;
    }

#ifdef _WIN32
    std::optional<RECT> savedMainRect;
    pImpl->savedAnalysisWindowRect.reset();
    pImpl->deferredStartupMainPlacement.reset();
    if (!deferPlacement) {
        if (pImpl->ensurePlacementStorage()) {
            savedMainRect = pImpl->loadSavedMainWindowRect();
        }
        pImpl->stopPlacementWriter = false;
        pImpl->placementWriter = std::thread([this]() { pImpl->runPlacementWriter(); });
    }
    pImpl->threadReady = false;
    pImpl->uiThread = std::thread([this, savedMainRect]() { pImpl->runUiThread(savedMainRect); });

    std::unique_lock<std::mutex> lock(pImpl->mutex);
    pImpl->readyCv.wait(lock, [this]() {
        return pImpl->threadReady;
    });

    pImpl->initialized = (pImpl->window != nullptr);
    if (!pImpl->initialized && pImpl->lastError.empty()) {
        pImpl->lastError = "Dashboard initialization failed";
    }
    return pImpl->initialized;
#else
    pImpl->lastError = "Dashboard presenter is only implemented on Windows";
    return false;
#endif
}

void DashboardPresenter::restoreStartupPlacement() {
#ifdef _WIN32
    if (pImpl->placementWriter.joinable()) {
        return;
    }

    if (pImpl->ensurePlacementStorage()) {
        if (const auto saved = pImpl->loadSavedMainWindowRect()) {
            HWND hwnd;
            {
                std::lock_guard<std::mutex> lock(pImpl->mutex);
                hwnd = pImpl->window;
            }
            if (hwnd != nullptr) {
                SendMessageW(hwnd, Impl::WM_JISTATS_RESTORE_MAIN_PLACEMENT, 0, reinterpret_cast<LPARAM>(&*saved));
            }
        }
    }
    pImpl->stopPlacementWriter = false;
    pImpl->placementWriter = std::thread([this]() { pImpl->runPlacementWriter(); });
#endif
}

void DashboardPresenter::requestClose() {
#ifdef _WIN32
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        pImpl->shutdownRequested = true;
        hwnd = pImpl->window;
    }

    if (hwnd != nullptr) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
#endif
}

void DashboardPresenter::shutdown() {
    requestClose();
    waitForExit();
}

int DashboardPresenter::waitForExit() {
#ifdef _WIN32
    std::lock_guard<std::mutex> joinLock(pImpl->threadJoinMutex);
    if (pImpl->uiThread.joinable() && pImpl->uiThread.get_id() != std::this_thread::get_id()) {
        pImpl->uiThread.join();
    }
    if (!pImpl->uiThread.joinable() && pImpl->placementWriter.joinable()) {
        pImpl->joinPlacementWriter();
    }
#endif
    pImpl->joinOpenAIImportThreads();
    pImpl->joinHtmlExportThreads();
    pImpl->joinMiniMaxActionThreads();
    pImpl->joinOfflineRetryThreads();
    pImpl->joinOfflineDismissThreads();

    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->initialized = false;
    pImpl->visible = false;
    pImpl->shutdownRequested = false;
    return 0;
}

std::string DashboardPresenter::getLastError() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->lastError;
}

void DashboardPresenter::setPreferencesChangedCallback(std::function<void(const DashboardPreferences&)> callback) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->preferencesChangedCallback = std::move(callback);
}

void DashboardPresenter::setCodexModelAttributionChangedCallback(std::function<void(bool, std::uint64_t)> callback) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->codexModelAttributionChangedCallback = std::move(callback);
}

void DashboardPresenter::setPreferencesOpenedCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->preferencesOpenedCallback = std::move(callback);
}

void DashboardPresenter::setPeriodicRefreshCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->periodicRefreshCallback = std::move(callback);
}

void DashboardPresenter::setMiniMaxCredentialActionCallback(
    std::function<DashboardMiniMaxCredentialResult(const DashboardMiniMaxCredentialRequest&)> callback) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->miniMaxCredentialActionCallback = std::move(callback);
}

void DashboardPresenter::setOpenAIImportActionCallback(
    std::function<DashboardOpenAIImportResult(const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->openAIImportActionCallback = std::move(callback);
}

void DashboardPresenter::setHtmlExportActionCallback(
    std::function<DashboardHtmlExportResult(const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->htmlExportActionCallback = std::move(callback);
}

void DashboardPresenter::setOfflineRetryActionCallback(
    std::function<DashboardNoticeActionResult()> callback) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->offlineRetryActionCallback = std::move(callback);
}

void DashboardPresenter::setOfflineDismissActionCallback(
    std::function<DashboardNoticeActionResult()> callback) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->offlineDismissActionCallback = std::move(callback);
}

void DashboardPresenter::requestOfflineRetry() {
#ifdef _WIN32
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        hwnd = pImpl->window;
    }
    if (hwnd != nullptr) {
        PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(Impl::kOfflineRetryButtonId, BN_CLICKED), 0);
    }
#endif
}

void DashboardPresenter::requestOfflineDismiss() {
#ifdef _WIN32
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        hwnd = pImpl->window;
    }
    if (hwnd != nullptr) {
        PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(Impl::kOfflineDismissButtonId, BN_CLICKED), 0);
    }
#endif
}

void DashboardPresenter::setExitRequestedCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->exitRequestedCallback = std::move(callback);
}

#ifdef JISTATS_ENABLE_TEST_HOOKS
bool DashboardPresenter::selectMainTabForTesting(int tabIndex) {
#ifdef _WIN32
    HWND hwnd = nullptr;
    HWND tab = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        hwnd = pImpl->window;
        tab = pImpl->mainTab;
    }
    if (hwnd == nullptr || tab == nullptr) {
        return false;
    }

    const int count = TabCtrl_GetItemCount(tab);
    if (tabIndex < 0 || tabIndex >= count) {
        return false;
    }

    return PostMessageW(
        hwnd,
        Impl::WM_JISTATS_TEST_SELECT_MAIN_TAB,
        static_cast<WPARAM>(tabIndex),
        0) != FALSE;
#else
    (void)tabIndex;
    return false;
#endif
}

bool DashboardPresenter::dispatchShellCommandForTesting(DashboardShellCommand command) {
#ifdef _WIN32
    HWND hwnd = nullptr;
    int commandId = 0;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        hwnd = pImpl->window;
    }

    switch (command) {
    case DashboardShellCommand::FileSettings:
        commandId = Impl::kMenuFileSettingsId;
        break;
    case DashboardShellCommand::FileImportOpenAI:
        commandId = Impl::kMenuFileImportOpenAIId;
        break;
    case DashboardShellCommand::FileExit:
        commandId = Impl::kMenuFileExitId;
        break;
    case DashboardShellCommand::HelpViewHelp:
        commandId = Impl::kMenuHelpViewHelpId;
        break;
    case DashboardShellCommand::HelpAbout:
        commandId = Impl::kMenuHelpAboutId;
        break;
    case DashboardShellCommand::FileExportHtml:
        commandId = Impl::kMenuFileExportHtmlId;
        break;
    default:
        return false;
    }

    return hwnd != nullptr && PostMessageW(hwnd, WM_COMMAND, static_cast<WPARAM>(commandId), 0) != FALSE;
#else
    static_cast<void>(command);
    return false;
#endif
}

bool DashboardPresenter::isAnalysisWindowOpenForTesting() const {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->analysisWindow != nullptr;
#else
    return false;
#endif
}

bool DashboardPresenter::isShellDialogOpenForTesting(DashboardShellDialog dialog) const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    switch (dialog) {
    case DashboardShellDialog::Preferences:
        return pImpl->preferencesWindow != nullptr;
    case DashboardShellDialog::Help:
        return pImpl->helpDialog.window != nullptr;
    case DashboardShellDialog::About:
        return pImpl->aboutDialog.window != nullptr;
    default:
        return false;
    }
}

std::string DashboardPresenter::getShellDialogBodyTextForTesting(DashboardShellDialog dialog) const {
#ifdef _WIN32
    return wideToUtf8(pImpl->shellDialogBody(dialog));
#else
    static_cast<void>(dialog);
    return "";
#endif
}

std::array<int, 4> DashboardPresenter::getMainWindowRectForTesting() const {
#ifdef _WIN32
    std::array<int, 4> rect{0, 0, 0, 0};
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        hwnd = pImpl->window;
    }
    RECT windowRect{};
    if (hwnd != nullptr && GetWindowRect(hwnd, &windowRect) != 0) {
        rect = {windowRect.left, windowRect.top, windowRect.right, windowRect.bottom};
    }
    return rect;
#else
    return {0, 0, 0, 0};
#endif
}

std::array<int, 4> DashboardPresenter::getAnalysisWindowRectForTesting() const {
#ifdef _WIN32
    std::array<int, 4> rect{0, 0, 0, 0};
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        hwnd = pImpl->analysisWindow;
    }
    RECT windowRect{};
    if (hwnd != nullptr && GetWindowRect(hwnd, &windowRect) != 0) {
        rect = {windowRect.left, windowRect.top, windowRect.right, windowRect.bottom};
    }
    return rect;
#else
    return {0, 0, 0, 0};
#endif
}

std::array<int, 4> DashboardPresenter::getShellDialogRectForTesting(DashboardShellDialog dialog) const {
#ifdef _WIN32
    std::array<int, 4> rect{0, 0, 0, 0};
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        switch (dialog) {
        case DashboardShellDialog::Help:
            hwnd = pImpl->helpDialog.window;
            break;
        case DashboardShellDialog::About:
            hwnd = pImpl->aboutDialog.window;
            break;
        default:
            hwnd = nullptr;
            break;
        }
    }
    RECT windowRect{};
    if (hwnd != nullptr && GetWindowRect(hwnd, &windowRect) != 0) {
        rect = {windowRect.left, windowRect.top, windowRect.right, windowRect.bottom};
    }
    return rect;
#else
    static_cast<void>(dialog);
    return {0, 0, 0, 0};
#endif
}

bool DashboardPresenter::submitMiniMaxCredentialActionForTesting(
    const DashboardMiniMaxCredentialRequest& request) {
#ifdef _WIN32
    HWND hwnd = nullptr;
    HWND planCombo = nullptr;
    HWND accountEdit = nullptr;
    HWND secretEdit = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        hwnd = pImpl->preferencesWindow;
        planCombo = pImpl->miniMaxPlanCombo;
        accountEdit = pImpl->miniMaxAccountEdit;
        secretEdit = pImpl->miniMaxSecretEdit;
    }

    if (hwnd == nullptr) {
        return false;
    }

    if (planCombo != nullptr) {
        const int selection = request.planKey == "token_plan" ? 1 : 0;
        SendMessageW(planCombo, CB_SETCURSEL, selection, 0);
    }
    if (accountEdit != nullptr) {
        setWindowText(accountEdit, utf8ToWide(request.accountDisplayName));
    }
    if (secretEdit != nullptr) {
        setWindowText(secretEdit, utf8ToWide(request.secret));
    }

    pImpl->queueMiniMaxCredentialAction(request);
    return true;
#else
    static_cast<void>(request);
    return false;
#endif
}

bool DashboardPresenter::selectProviderQuotaRowForTesting(int rowIndex) {
#ifdef _WIN32
    HWND list = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        list = pImpl->providerList;
    }
    if (list == nullptr || rowIndex < 0) {
        return false;
    }

    ListView_SetItemState(
        list,
        -1,
        0,
        LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(
        list,
        rowIndex,
        LVIS_SELECTED | LVIS_FOCUSED,
        LVIS_SELECTED | LVIS_FOCUSED);
    pImpl->handleProviderQuotaRowSelection(rowIndex);
    return true;
#else
    static_cast<void>(rowIndex);
    return false;
#endif
}

std::string DashboardPresenter::getSelectedProviderIdForTesting() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->viewModel.preferences.lastSelectedProviderId;
}

std::string DashboardPresenter::getSelectedAccountIdForTesting() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->viewModel.preferences.lastSelectedAccountId;
}

std::string DashboardPresenter::getMiniMaxSecretFieldTextForTesting() const {
#ifdef _WIN32
    HWND secretEdit = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        secretEdit = pImpl->miniMaxSecretEdit;
    }
    return pImpl->getWindowTextUtf8(secretEdit);
#else
    return "";
#endif
}

std::vector<DashboardSummaryItem> DashboardPresenter::getUsageProgressSummariesForTesting() const {
    const DashboardViewModel model = getViewModel();
    const std::vector<UsageProgressSummary> summaries = buildUsageProgressSummaries(model);
    std::vector<DashboardSummaryItem> items;
    items.reserve(summaries.size() + model.runtimeSummaries.size());
    for (const auto& summary : summaries) {
        if (summary.percentage < 0) {
            continue;
        }
        DashboardSummaryItem item;
        item.label = summary.label;
        item.detail = summary.detail;
        item.percentage = summary.percentage;
        item.badge = summary.overlayText;
        item.secondaryPercentage = summary.secondaryPercentage;
        item.secondaryBadge = summary.secondaryOverlayText;
        items.push_back(item);
    }
    items.insert(items.end(), model.runtimeSummaries.begin(), model.runtimeSummaries.end());
    return items;
}

std::vector<std::string> DashboardPresenter::getApplicationFieldLabelsForTesting() const {
    return dashboardApplicationFieldLabels();
}

std::vector<std::string> DashboardPresenter::getApplicationFieldValuesForTesting() const {
    const DashboardViewModel model = getViewModel();
    return {
        runtimeStateLabel(model.runtimeState),
        model.recoveryStatusSummary.empty() ? "No recent recovery activity" : model.recoveryStatusSummary,
        model.scheduledExportStatusSummary.empty() ? "Disabled" : model.scheduledExportStatusSummary,
        model.lastUpdateTime.empty() ? "" : localizeDisplayTimestamp(model.lastUpdateTime)
    };
}

std::vector<std::string> DashboardPresenter::getPreferencesTabLabelsForTesting() const {
#ifdef _WIN32
    HWND tab = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        tab = pImpl->preferencesTab;
    }
    if (tab == nullptr) {
        return preferencesTabLabels();
    }

    const int count = TabCtrl_GetItemCount(tab);
    std::vector<std::string> labels;
    labels.reserve(static_cast<size_t>(std::max(0, count)));
    wchar_t buffer[128]{};
    for (int index = 0; index < count; ++index) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = buffer;
        item.cchTextMax = static_cast<int>(std::size(buffer));
        if (SendMessageW(tab, TCM_GETITEMW, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&item)) != FALSE) {
            labels.push_back(wideToUtf8(std::wstring(buffer)));
        }
    }
    return labels;
#else
    return preferencesTabLabels();
#endif
}

bool DashboardPresenter::selectPreferencesTabForTesting(int tabIndex) {
#ifdef _WIN32
    HWND prefs = nullptr;
    HWND tab = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        prefs = pImpl->preferencesWindow;
        tab = pImpl->preferencesTab;
    }
    if (prefs == nullptr || tab == nullptr) {
        return false;
    }

    const int count = TabCtrl_GetItemCount(tab);
    if (tabIndex < 0 || tabIndex >= count) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        pImpl->selectedPreferencesTab = tabIndex;
    }
    SendMessageW(tab, TCM_SETCURSEL, static_cast<WPARAM>(tabIndex), 0);
    SendMessageW(prefs, WM_SIZE, 0, 0);
    SendMessageW(prefs, Impl::WM_JISTATS_SYNC_PREFERENCES, 0, 0);
    return true;
#else
    (void)tabIndex;
    return false;
#endif
}

bool DashboardPresenter::clickClaudeCodeToggleForTesting() {
#ifdef _WIN32
    HWND prefs = nullptr;
    HWND checkbox = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        prefs = pImpl->preferencesWindow;
        checkbox = pImpl->claudeCodeEnableCheckbox;
    }
    return prefs != nullptr &&
        checkbox != nullptr &&
        PostMessageW(
            prefs,
            WM_COMMAND,
            MAKEWPARAM(Impl::kPreferencesClaudeCodeCheckboxId, BN_CLICKED),
            reinterpret_cast<LPARAM>(checkbox)) != FALSE;
#else
    return false;
#endif
}

bool DashboardPresenter::clickKimiCodeToggleForTesting() {
#ifdef _WIN32
    HWND prefs = nullptr;
    HWND checkbox = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        prefs = pImpl->preferencesWindow;
        checkbox = pImpl->kimiCodeEnableCheckbox;
    }
    return prefs != nullptr &&
        checkbox != nullptr &&
        PostMessageW(
            prefs,
            WM_COMMAND,
            MAKEWPARAM(Impl::kPreferencesKimiCodeCheckboxId, BN_CLICKED),
            reinterpret_cast<LPARAM>(checkbox)) != FALSE;
#else
    return false;
#endif
}

bool DashboardPresenter::isClaudeCodeEnabledForTesting() const {
#ifdef _WIN32
    HWND checkbox = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        checkbox = pImpl->claudeCodeEnableCheckbox;
    }
    if (checkbox != nullptr) {
        return SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
#endif
    return getViewModel().preferences.claudeCodeEnabled;
}

bool DashboardPresenter::isKimiCodeEnabledForTesting() const {
#ifdef _WIN32
    HWND checkbox = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        checkbox = pImpl->kimiCodeEnableCheckbox;
    }
    if (checkbox != nullptr) {
        return SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
#endif
    return getViewModel().preferences.kimiCodeEnabled;
}

std::string DashboardPresenter::getClaudeCodeSettingsStatusTextForTesting() const {
#ifdef _WIN32
    HWND label = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        label = pImpl->claudeCodeStatus;
    }
    if (label != nullptr) {
        return pImpl->getWindowTextUtf8(label);
    }
#endif
    return localProviderSettingsStatusText(getViewModel(), "claude_code");
}

std::string DashboardPresenter::getClaudeCodeSettingsDetailTextForTesting() const {
#ifdef _WIN32
    HWND label = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        label = pImpl->claudeCodeDetail;
    }
    if (label != nullptr) {
        return pImpl->getWindowTextUtf8(label);
    }
#endif
    return localProviderSettingsDetailText(getViewModel(), "claude_code");
}

std::string DashboardPresenter::getKimiCodeSettingsStatusTextForTesting() const {
#ifdef _WIN32
    HWND label = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        label = pImpl->kimiCodeStatus;
    }
    if (label != nullptr) {
        return pImpl->getWindowTextUtf8(label);
    }
#endif
    return localProviderSettingsStatusText(getViewModel(), "kimi");
}

std::string DashboardPresenter::getKimiCodeSettingsDetailTextForTesting() const {
#ifdef _WIN32
    HWND label = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        label = pImpl->kimiCodeDetail;
    }
    if (label != nullptr) {
        return pImpl->getWindowTextUtf8(label);
    }
#endif
    return localProviderSettingsDetailText(getViewModel(), "kimi");
}

std::vector<std::string> DashboardPresenter::getProviderQuotaColumnHeadersForTesting() const {
    return providerQuotaColumnHeaders();
}

std::vector<std::string> DashboardPresenter::getProviderQuotaUsedValuesForTesting() const {
    const DashboardViewModel model = getViewModel();
    const auto rows = buildProviderQuotaRows(model);
    std::vector<std::string> values;
    values.reserve(rows.size());
    for (const auto& row : rows) {
        if (row.columns.size() > 4) {
            values.push_back(row.columns[4]);
        }
    }
    return values;
}

std::vector<std::string> DashboardPresenter::getProviderQuotaLimitValuesForTesting() const {
    const DashboardViewModel model = getViewModel();
    const auto rows = buildProviderQuotaRows(model);
    std::vector<std::string> values;
    values.reserve(rows.size());
    for (const auto& row : rows) {
        if (row.columns.size() > 5) {
            values.push_back(row.columns[5]);
        }
    }
    return values;
}

std::vector<std::string> DashboardPresenter::getProviderQuotaRemainingValuesForTesting() const {
    const DashboardViewModel model = getViewModel();
    const auto rows = buildProviderQuotaRows(model);
    std::vector<std::string> values;
    values.reserve(rows.size());
    for (const auto& row : rows) {
        if (row.columns.size() > 6) {
            values.push_back(row.columns[6]);
        }
    }
    return values;
}

std::vector<std::string> DashboardPresenter::getProviderQuotaLastSyncValuesForTesting() const {
    const DashboardViewModel model = getViewModel();
    const auto rows = buildProviderQuotaRows(model);
    std::vector<std::string> values;
    values.reserve(rows.size());
    for (const auto& row : rows) {
        if (row.columns.size() > 8) {
            values.push_back(row.columns[8]);
        }
    }
    return values;
}

std::string DashboardPresenter::getConnectionHelpTextForTesting() const {
    const DashboardViewModel model = getViewModel();
    return connectionHelpText(model.runtimeState);
}

bool DashboardPresenter::isScheduledExportEnabledForTesting() const {
#ifdef _WIN32
    HWND checkbox = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        checkbox = pImpl->scheduledExportCheckbox;
    }
    if (checkbox != nullptr) {
        return SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
#endif
    return getViewModel().preferences.scheduledExportEnabled;
}

int DashboardPresenter::getScheduledExportIntervalMinutesForTesting() const {
#ifdef _WIN32
    HWND edit = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        edit = pImpl->scheduledExportIntervalEdit;
    }
    if (edit != nullptr) {
        return parsePositiveIntegerOrDefault(
            pImpl->getWindowTextUtf8(edit),
            std::max(1, getViewModel().preferences.scheduledExportIntervalMinutes));
    }
#endif
    return std::max(1, getViewModel().preferences.scheduledExportIntervalMinutes);
}

std::string DashboardPresenter::getScheduledExportPathForTesting() const {
#ifdef _WIN32
    HWND edit = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        edit = pImpl->scheduledExportPathEdit;
    }
    if (edit != nullptr) {
        return pImpl->getWindowTextUtf8(edit);
    }
#endif
    return getViewModel().preferences.scheduledExportPath;
}

std::string DashboardPresenter::getScheduledExportLastRunForTesting() const {
#ifdef _WIN32
    HWND label = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        label = pImpl->scheduledExportLastRunValue;
    }
    if (label != nullptr) {
        return pImpl->getWindowTextUtf8(label);
    }
#endif
    return getViewModel().scheduledExportLastRunTime;
}

std::string DashboardPresenter::getStatusTextForTesting() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return wideToUtf8(pImpl->persistentStatusText);
}

std::string DashboardPresenter::getCodexThroughputTextForTesting() const {
#ifdef _WIN32
    HWND recentText = nullptr;
    HWND averagesText = nullptr;
    HWND totalsText = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        recentText = pImpl->codexThroughputText;
        averagesText = pImpl->codexThroughputAveragesText;
        totalsText = pImpl->codexThroughputTotalsText;
    }
    if (recentText == nullptr) {
        return {};
    }
    std::string text = pImpl->getWindowTextUtf8(recentText);
    if (averagesText != nullptr) {
        text += "\r\n" + pImpl->getWindowTextUtf8(averagesText);
    }
    if (totalsText != nullptr) {
        text += "\r\n" + pImpl->getWindowTextUtf8(totalsText);
    }
    return text;
#else
    return {};
#endif
}

std::string DashboardPresenter::getOpenAiCodexModelUsageTextForTesting() const {
#ifdef _WIN32
    HWND text = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        text = pImpl->openAiUsageText;
    }
    if (text == nullptr) return {};
    return pImpl->getWindowTextUtf8(text);
#else
    return {};
#endif
}

std::string DashboardPresenter::getHicapCodexModelUsageTextForTesting() const {
#ifdef _WIN32
    HWND text = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        text = pImpl->hicapUsageText;
    }
    if (text == nullptr) return {};
    return pImpl->getWindowTextUtf8(text);
#else
    return {};
#endif
}

std::array<int, 4> DashboardPresenter::getCodexThroughputChartRectForTesting() const {
#ifdef _WIN32
    HWND chart = nullptr;
    HWND group = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        chart = pImpl->codexThroughputChart;
        group = pImpl->codexThroughputGroup;
    }

    RECT rect{};
    if (chart == nullptr || group == nullptr || GetWindowRect(chart, &rect) == 0) {
        return {0, 0, 0, 0};
    }

    POINT points[2] = {
        {rect.left, rect.top},
        {rect.right, rect.bottom}
    };
    MapWindowPoints(HWND_DESKTOP, group, points, 2);
    return {points[0].x, points[0].y, points[1].x, points[1].y};
#else
    return {0, 0, 0, 0};
#endif
}

int DashboardPresenter::getUsageProgressBarWidthForTesting() const {
#ifdef _WIN32
    HWND bar = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        if (pImpl->dashboardViewport != nullptr) {
            bar = GetDlgItem(pImpl->dashboardViewport, Impl::kUsageBarBaseId);
        }
    }
    if (bar == nullptr) {
        return 0;
    }

    RECT rect{};
    if (GetWindowRect(bar, &rect) == 0) {
        return 0;
    }
    return rect.right - rect.left;
#else
    return 0;
#endif
}

bool DashboardPresenter::postNativeCloseForTesting() {
#ifdef _WIN32
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        hwnd = pImpl->window;
    }
    return hwnd != nullptr && PostMessageW(hwnd, WM_CLOSE, 0, 0) != FALSE;
#else
    return false;
#endif
}

void DashboardPresenter::setOpenAIImportPathForTesting(const std::string& inputPath) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->openAIImportPathForTesting = inputPath;
}

void DashboardPresenter::setHtmlExportPathForTesting(const std::string& outputPath) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->htmlExportPathForTesting = outputPath;
}

std::string DashboardPresenter::getLastHtmlExportStatusForTesting() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return wideToUtf8(pImpl->lastHtmlExportStatusText);
}

bool DashboardPresenter::isOfflineNoticeVisibleForTesting() const {
    return getViewModel().offlineNoticeVisible;
}

std::string DashboardPresenter::getOfflineNoticeTextForTesting() const {
    const DashboardViewModel model = getViewModel();
    if (!model.offlineNoticeVisible) {
        return "";
    }

    if (model.offlineNoticeTitle.empty()) {
        return model.offlineNoticeMessage;
    }

    if (model.offlineNoticeMessage.empty()) {
        return model.offlineNoticeTitle;
    }

    return model.offlineNoticeTitle + "\n" + model.offlineNoticeMessage;
}

bool DashboardPresenter::clickOfflineRetryForTesting() {
#ifdef _WIN32
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        hwnd = pImpl->window;
    }
    return hwnd != nullptr &&
        PostMessageW(
            hwnd,
            WM_COMMAND,
            MAKEWPARAM(Impl::kOfflineRetryButtonId, BN_CLICKED),
            0) != FALSE;
#else
    return false;
#endif
}

bool DashboardPresenter::clickOfflineDismissForTesting() {
#ifdef _WIN32
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        hwnd = pImpl->window;
    }
    return hwnd != nullptr &&
        PostMessageW(
            hwnd,
            WM_COMMAND,
            MAKEWPARAM(Impl::kOfflineDismissButtonId, BN_CLICKED),
            0) != FALSE;
#else
    return false;
#endif
}

bool DashboardPresenter::clickOpenAnalysisWindowForTesting() {
#ifdef _WIN32
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        hwnd = pImpl->window;
    }
    return hwnd != nullptr &&
        PostMessageW(
            hwnd,
            WM_COMMAND,
            MAKEWPARAM(Impl::kOpenAnalysisWindowButtonId, BN_CLICKED),
            0) != FALSE;
#else
    return false;
#endif
}

std::string DashboardPresenter::getAnalysisScopeLabelForTesting() const {
    AnalysisPresenter* analysisPresenter = nullptr;
    std::string fallbackScopeLabel;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        analysisPresenter = pImpl->analysisPresenter;
        fallbackScopeLabel = pImpl->viewModel.analysis.scopeLabel;
    }
    if (analysisPresenter != nullptr) {
        return analysisPresenter->getScopeLabelForTesting();
    }
    return fallbackScopeLabel;
}

std::uint64_t DashboardPresenter::getViewModelSyncCountForTesting() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->viewModelSyncCountForTesting;
}
#endif

void DashboardPresenter::render() {
#ifdef _WIN32
    pImpl->requestViewModelSync();
#endif
}

void DashboardPresenter::renderHeader() {
}

void DashboardPresenter::renderProviderStatus() {
}

void DashboardPresenter::renderQuotaCards() {
}

void DashboardPresenter::renderUsageChart() {
}

void DashboardPresenter::renderRecentActivity() {
}

void DashboardPresenter::updateViewModel(const DashboardViewModel& viewModel) {
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        auto& current = pImpl->viewModel;
        if (viewModel.completedPreferencesRequest < pImpl->preferencesRequest) {
            auto preferences = std::move(current.preferences);
            const auto completedRequest = current.completedPreferencesRequest;
            const auto failedRequest = current.failedPreferencesRequest;
            current = viewModel;
            // Runtime publications also carry the independently saved Analysis scope and attribution consent.
            preferences.codexModelAttributionEnabled = viewModel.preferences.codexModelAttributionEnabled;
            preferences.analysisSelectedProviderId = viewModel.preferences.analysisSelectedProviderId;
            preferences.analysisSelectedAccountId = viewModel.preferences.analysisSelectedAccountId;
            preferences.analysisDateRangePreset = viewModel.preferences.analysisDateRangePreset;
            preferences.analysisCustomRangeStartUtc = viewModel.preferences.analysisCustomRangeStartUtc;
            preferences.analysisCustomRangeEndUtc = viewModel.preferences.analysisCustomRangeEndUtc;
            current.preferences = std::move(preferences);
            current.completedPreferencesRequest = std::max(completedRequest, viewModel.completedPreferencesRequest);
            current.failedPreferencesRequest = std::max(failedRequest, viewModel.failedPreferencesRequest);
        } else {
            current = viewModel;
        }
    }
    render();
}

DashboardViewModel DashboardPresenter::getViewModel() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->viewModel;
}

void DashboardPresenter::setVisible(bool visible) {
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        pImpl->visible = visible;
        hwnd = pImpl->window;
    }

#ifdef _WIN32
    if (hwnd != nullptr) {
        if (visible) {
            ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(hwnd);
            UpdateWindow(hwnd);
            render();
        } else {
            ShowWindow(hwnd, SW_HIDE);
        }
    }
#endif
}

bool DashboardPresenter::isVisible() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->visible;
}

bool DashboardPresenter::isAnalysisWindowOpen() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    return pImpl->analysisWindow != nullptr;
}

void DashboardPresenter::setAnalysisPresenter(AnalysisPresenter* presenter) {
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(pImpl->mutex);
        pImpl->analysisPresenter = presenter;
        hwnd = pImpl->analysisWindow != nullptr ? pImpl->analysisWindow : pImpl->window;
    }
    if (hwnd != nullptr && presenter != nullptr) {
        PostMessageW(hwnd, Impl::WM_JISTATS_ATTACH_ANALYSIS, 0, 0);
    }
}

} // namespace JiStats
