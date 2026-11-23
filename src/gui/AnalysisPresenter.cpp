#include "gui/AnalysisPresenter.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <cstring>

#ifdef _WIN32
#include <commctrl.h>
#include <windowsx.h>
#endif

namespace JiStats {

namespace {

struct ListColumnDefinition {
    const wchar_t* title;
    int minWidth;
    int weight;
};

struct AnalysisOverviewCells {
    std::string coverage;
    std::string breakdowns;
    std::string throughput;
};

constexpr ListColumnDefinition kBreakdownColumns[] = {
    {L"Name", 120, 30},
    {L"Context", 140, 34},
    {L"Count", 72, 16},
    {L"Source", 96, 20}
};

constexpr ListColumnDefinition kDailyColumns[] = {
    {L"Date", 88, 16},
    {L"Count", 74, 14},
    {L"Source", 96, 18},
    {L"Notes", 220, 52}
};

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

std::string getWindowTextUtf8(HWND hwnd) {
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

void setWindowTextUtf8(HWND hwnd, const std::string& value) {
    if (hwnd != nullptr) {
        SetWindowTextW(hwnd, utf8ToWide(value).c_str());
    }
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

std::optional<std::time_t> parseReportDay(const std::string& value) {
    if (value.empty()) {
        return std::nullopt;
    }
    std::tm tm{};
    std::istringstream stream(value);
    stream >> std::get_time(&tm, "%Y-%m-%d");
    if (stream.fail()) {
        return std::nullopt;
    }
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    return _mkgmtime(&tm);
}

std::string presetKeyFromEnum(AnalysisDateRangePreset preset) {
    switch (preset) {
    case AnalysisDateRangePreset::Today:
        return "today";
    case AnalysisDateRangePreset::Last24Hours:
        return "last24";
    case AnalysisDateRangePreset::Last7Days:
        return "last7";
    case AnalysisDateRangePreset::CurrentQuotaWindow:
        return "currentQuota";
    case AnalysisDateRangePreset::ThisMonth:
        return "month";
    case AnalysisDateRangePreset::Custom:
        return "custom";
    default:
        return "last7";
    }
}

AnalysisDateRangePreset presetFromKey(const std::string& key) {
    if (key == "today") return AnalysisDateRangePreset::Today;
    if (key == "last24") return AnalysisDateRangePreset::Last24Hours;
    if (key == "currentQuota") return AnalysisDateRangePreset::CurrentQuotaWindow;
    if (key == "month") return AnalysisDateRangePreset::ThisMonth;
    if (key == "custom") return AnalysisDateRangePreset::Custom;
    return AnalysisDateRangePreset::Last7Days;
}

template <typename Options>
std::string firstOptionKey(const Options& options) {
    if (options.empty()) {
        return "";
    }
    return options.front().key;
}

template <typename Options>
bool containsOptionKey(const Options& options, const std::string& key) {
    return std::any_of(options.begin(), options.end(), [&](const auto& option) {
        return option.key == key;
    });
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

std::string pluralize(long long value, const char* singular, const char* plural) {
    return value == 1 ? singular : plural;
}

std::string fallbackText(const std::string& value, const char* fallback) {
    return value.empty() ? std::string(fallback) : value;
}

std::string buildScopeSummaryText(const std::string& scopeLabel) {
    const std::string label = scopeLabel.empty()
        ? "All Providers / All Accounts / Last 7 Days"
        : scopeLabel;
    return "Active filters: " + label;
}

std::string buildWorkspaceGuideText() {
    return "Workspace guide: jump to the section you want to review next, then refine the provider, account, and date filters above.";
}

int sumValues(const std::vector<int>& values) {
    int total = 0;
    for (const int value : values) {
        total += value;
    }
    return total;
}

std::string trimToSingleLine(const std::string& value) {
    std::string flattened = value;
    std::replace(flattened.begin(), flattened.end(), '\r', ' ');
    std::replace(flattened.begin(), flattened.end(), '\n', ' ');
    while (flattened.find("  ") != std::string::npos) {
        flattened.replace(flattened.find("  "), 2, " ");
    }
    return flattened;
}

std::string gapClassificationText(const AnalysisGapDetail& detail) {
    if (!detail.classificationLabel.empty()) {
        return detail.classificationLabel;
    }

    switch (detail.classification) {
    case AnalysisGapClassification::KnownZero:
        return "Known Zero";
    case AnalysisGapClassification::Reconstructed:
        return "Reconstructed";
    case AnalysisGapClassification::Unknown:
        return "Unknown";
    case AnalysisGapClassification::None:
    default:
        return "";
    }
}

std::string buildDailyProvenanceText(const AnalysisDailySummary& summary) {
    std::ostringstream stream;
    const std::string classification = gapClassificationText(summary.gapDetail);
    if (!classification.empty()) {
        stream << classification;
    }
    if (!summary.gapDetail.basisSummary.empty()) {
        if (stream.tellp() > 0) {
            stream << " - ";
        }
        stream << trimToSingleLine(summary.gapDetail.basisSummary);
    }
    if (!summary.gapDetail.sourceKind.empty()) {
        if (stream.tellp() > 0) {
            stream << "; ";
        }
        stream << "source " << trimToSingleLine(summary.gapDetail.sourceKind);
    }
    if (stream.tellp() == 0) {
        stream << "Observed";
    }
    return stream.str();
}

long long totalActivityFromDaily(const std::vector<AnalysisDailySummary>& rows) {
    long long total = 0;
    for (const auto& row : rows) {
        total += row.activityCount;
    }
    return total;
}

long long totalActivityFromTrend(const std::vector<AnalysisTrendPoint>& rows) {
    long long total = 0;
    for (const auto& row : rows) {
        total += row.activityCount;
    }
    return total;
}

long long countActiveDays(const std::vector<AnalysisDailySummary>& rows) {
    return static_cast<long long>(std::count_if(rows.begin(), rows.end(), [](const AnalysisDailySummary& row) {
        return row.activityCount > 0;
    }));
}

long long countActiveTrendPoints(const std::vector<AnalysisTrendPoint>& rows) {
    return static_cast<long long>(std::count_if(rows.begin(), rows.end(), [](const AnalysisTrendPoint& row) {
        return row.activityCount > 0;
    }));
}

std::string topBreakdownSummary(
    const std::vector<AnalysisBreakdownRow>& rows,
    const char* labelPrefix) {
    if (rows.empty()) {
        return std::string(labelPrefix) + ": n/a.";
    }

    const AnalysisBreakdownRow& top = rows.front();
    std::ostringstream stream;
    stream << labelPrefix << ": " << fallbackText(top.label, "Unlabeled")
           << " (" << formatNumber(top.activityCount) << ").";
    return stream.str();
}

AnalysisOverviewCells buildOverviewCells(const AnalysisViewModel& model) {
    const long long reportDays = static_cast<long long>(model.dailySummaries.size());
    const long long trendPoints = static_cast<long long>(model.trendPoints.size());
    const long long activeDays = !model.dailySummaries.empty()
        ? countActiveDays(model.dailySummaries)
        : countActiveTrendPoints(model.trendPoints);
    const long long providerRows = static_cast<long long>(model.providerBreakdown.size());
    const long long accountRows = static_cast<long long>(model.accountBreakdown.size());

    AnalysisOverviewCells cells;
    std::ostringstream coverage;
    coverage << "Coverage"
             << "\r\nDaily: " << formatNumber(reportDays) << " rows"
             << "\r\nTrend: " << formatNumber(trendPoints) << " points"
             << "\r\nActive: " << formatNumber(activeDays) << " "
             << pluralize(activeDays, "day", "days");
    cells.coverage = coverage.str();

    std::ostringstream breakdowns;
    breakdowns << "Breakdowns"
               << "\r\nProviders: " << formatNumber(providerRows) << " rows"
               << "\r\nAccounts: " << formatNumber(accountRows) << " rows";
    if (!model.providerBreakdown.empty()) {
        breakdowns << "\r\nTop: " << fallbackText(model.providerBreakdown.front().label, "Unlabeled");
    } else {
        breakdowns << "\r\nTop: n/a";
    }
    cells.breakdowns = breakdowns.str();

    std::ostringstream throughput;
    throughput << "Codex";
    if (model.codexThroughput.hasData) {
        throughput << "\r\nLast 5m: "
                   << formatRate(model.codexThroughput.tokensPerMinute5m)
                   << " t/min, "
                   << formatRate(model.codexThroughput.promptsPerMinute5m)
                   << " p/min"
                   << "\r\nLast 30m: "
                   << formatRate(model.codexThroughput.tokensPerMinute30m)
                   << " t/min, "
                   << formatRate(model.codexThroughput.promptsPerMinute30m)
                   << " p/min"
                   << "\r\nSource: "
                   << fallbackText(model.codexThroughput.confidenceLabel, "Unknown");
    } else {
        throughput << "\r\nNo local throughput captured yet."
                   << "\r\nUse Codex, then refresh."
                   << "\r\nSource: Local tracking";
    }
    cells.throughput = throughput.str();

    return cells;
}

std::string buildOverviewHeadline(const AnalysisViewModel& model) {
    const long long reportDays = !model.dailySummaries.empty()
        ? static_cast<long long>(model.dailySummaries.size())
        : static_cast<long long>(model.trendPoints.size());
    const long long totalActivity = !model.dailySummaries.empty()
        ? totalActivityFromDaily(model.dailySummaries)
        : totalActivityFromTrend(model.trendPoints);
    const long long activeDays = !model.dailySummaries.empty()
        ? countActiveDays(model.dailySummaries)
        : countActiveTrendPoints(model.trendPoints);

    if (reportDays == 0) {
        return "No analysis history is available for the current scope yet.";
    }

    std::ostringstream stream;
    stream << formatNumber(totalActivity) << " activity across "
           << formatNumber(activeDays) << " active " << pluralize(activeDays, "day", "days")
           << " (" << formatNumber(reportDays) << " report " << pluralize(reportDays, "day", "days") << ").";
    return stream.str();
}

std::string buildOverviewStats(const AnalysisViewModel& model) {
    const long long reportDays = static_cast<long long>(model.dailySummaries.size());
    const long long trendPoints = static_cast<long long>(model.trendPoints.size());
    const long long providerRows = static_cast<long long>(model.providerBreakdown.size());
    const long long accountRows = static_cast<long long>(model.accountBreakdown.size());

    std::ostringstream stream;
    stream << "Coverage window: " << formatNumber(reportDays) << " daily rows, "
           << formatNumber(trendPoints) << " trend points.\r\n";
    stream << "Breakdowns: " << formatNumber(providerRows) << " provider rows, "
           << formatNumber(accountRows) << " account rows.\r\n";
    if (model.codexThroughput.hasData) {
        stream << "Codex throughput: "
               << formatRate(model.codexThroughput.tokensPerMinute5m)
               << " tokens/min and "
               << formatRate(model.codexThroughput.promptsPerMinute5m)
               << " prompts/min over 5m"
               << " (" << model.codexThroughput.confidenceLabel << ").\r\n";
    }
    stream << topBreakdownSummary(model.providerBreakdown, "Top provider") << " "
           << topBreakdownSummary(model.accountBreakdown, "Top account");
    return stream.str();
}

std::string deltaSummary(long long current, long long previous) {
    const long long delta = current - previous;
    if (delta > 0) {
        return "up " + formatNumber(delta);
    }
    if (delta < 0) {
        return "down " + formatNumber(-delta);
    }
    return "flat";
}

std::string formatPercentage(double value) {
    if (value < 0.0) {
        return "n/a";
    }

    std::ostringstream stream;
    stream << std::fixed << std::setprecision(value >= 10.0 ? 0 : 1) << value << "%";
    return stream.str();
}

std::string buildTrendSummary(
    const AnalysisViewModel& model,
    const AnalysisQuotaTrendSeries* selectedQuotaSeries = nullptr) {
    std::ostringstream stream;
    if (selectedQuotaSeries != nullptr) {
        const AnalysisQuotaTrendSeries& series = *selectedQuotaSeries;
        const auto lowWaterIt = std::min_element(
            series.points.begin(),
            series.points.end(),
            [](const AnalysisQuotaTrendPoint& left, const AnalysisQuotaTrendPoint& right) {
                return left.remainingPercentage < right.remainingPercentage;
            });
        stream << "Quota snapshot: " << fallbackText(series.label, "Scoped quota")
               << " at " << formatPercentage(series.latestRemainingPercentage)
               << " remaining";
        if (series.limitValue > 0) {
            stream << " (" << formatNumber(series.latestRemainingValue)
                   << " / " << formatNumber(series.limitValue) << ")";
        }
        stream << ".\r\nLow-water: " << formatPercentage(series.lowWaterRemainingPercentage)
               << " at " << fallbackText(lowWaterIt->label, "n/a")
               << ". Resets: " << formatNumber(series.resetCount) << ".";
        if (!model.trendPoints.empty()) {
            stream << "\r\n";
        } else {
            return stream.str();
        }
    }

    if (model.codexThroughput.hasData) {
        stream << "Codex throughput (5m): "
               << formatRate(model.codexThroughput.tokensPerMinute5m)
               << " tokens/min, "
               << formatRate(model.codexThroughput.promptsPerMinute5m)
               << " prompts/min"
               << " (" << model.codexThroughput.confidenceLabel << ").\r\n";
        stream << "Codex throughput (30m): "
               << formatRate(model.codexThroughput.tokensPerMinute30m)
               << " tokens/min, "
               << formatRate(model.codexThroughput.promptsPerMinute30m)
               << " prompts/min. Rates are averaged across the full window.\r\n";
    }

    if (model.trendPoints.empty()) {
        if (stream.tellp() > 0) {
            return stream.str();
        }
        return "No trend points are available for the selected scope yet.";
    }

    const auto peakIt = std::max_element(
        model.trendPoints.begin(),
        model.trendPoints.end(),
        [](const AnalysisTrendPoint& left, const AnalysisTrendPoint& right) {
            return left.activityCount < right.activityCount;
        });
    const auto lowIt = std::min_element(
        model.trendPoints.begin(),
        model.trendPoints.end(),
        [](const AnalysisTrendPoint& left, const AnalysisTrendPoint& right) {
            return left.activityCount < right.activityCount;
        });

    const AnalysisTrendPoint& latest = model.trendPoints.back();
    const long long pointCount = static_cast<long long>(model.trendPoints.size());
    const long long totalActivity = totalActivityFromTrend(model.trendPoints);
    const long long averageActivity = pointCount > 0 ? (totalActivity / pointCount) : 0;
    stream << "Window total: " << formatNumber(totalActivity)
           << ". Avg: " << formatNumber(averageActivity) << " per point."
           << "\r\nPeak: " << fallbackText(peakIt->dateLabel, "n/a")
           << " (" << formatNumber(peakIt->activityCount) << "). Low: "
           << fallbackText(lowIt->dateLabel, "n/a")
           << " (" << formatNumber(lowIt->activityCount) << ").";

    if (model.trendPoints.size() > 1) {
        const AnalysisTrendPoint& previous = model.trendPoints[model.trendPoints.size() - 2];
        stream << "\r\nLatest: " << fallbackText(latest.dateLabel, "n/a")
               << " (" << formatNumber(latest.activityCount) << "), "
               << deltaSummary(latest.activityCount, previous.activityCount)
               << " vs previous point.";
    } else {
        stream << "\r\nLatest: " << fallbackText(latest.dateLabel, "n/a")
               << " (" << formatNumber(latest.activityCount) << ").";
    }

    return stream.str();
}

std::string padRight(const std::string& value, size_t width) {
    if (value.size() >= width) {
        return value;
    }
    return value + std::string(width - value.size(), ' ');
}

std::string buildTrendChartText(
    const AnalysisViewModel& model,
    const AnalysisQuotaTrendSeries* selectedQuotaSeries = nullptr) {
    if (selectedQuotaSeries != nullptr) {
        const AnalysisQuotaTrendSeries& series = *selectedQuotaSeries;
        if (series.points.empty()) {
            return "No chartable quota trend history is available yet.";
        }

        const size_t maxVisiblePoints = 10;
        const size_t startIndex = series.points.size() > maxVisiblePoints
            ? series.points.size() - maxVisiblePoints
            : 0;
        std::ostringstream stream;
        if (startIndex > 0) {
            stream << "Showing latest "
                   << formatNumber(static_cast<long long>(series.points.size() - startIndex))
                   << " of " << formatNumber(static_cast<long long>(series.points.size()))
                   << " quota snapshots.\r\n";
        }

        constexpr int kMaxBarWidth = 24;
        for (size_t index = startIndex; index < series.points.size(); ++index) {
            const AnalysisQuotaTrendPoint& point = series.points[index];
            int barWidth = static_cast<int>((std::max(0.0, point.remainingPercentage) * kMaxBarWidth) / 100.0);
            if (point.remainingPercentage > 0.0 && barWidth == 0) {
                barWidth = 1;
            }

            stream << padRight(fallbackText(point.label, "-"), 10)
                   << " | "
                   << (barWidth > 0 ? std::string(static_cast<size_t>(barWidth), '#') : std::string("-"))
                   << " "
                   << formatPercentage(point.remainingPercentage);
            if (point.resetPoint) {
                stream << " reset";
            }

            if (index + 1 < series.points.size()) {
                stream << "\r\n";
            }
        }

        return stream.str();
    }

    const auto& points = model.trendPoints;
    if (points.empty()) {
        return "No chartable trend history is available yet.";
    }

    const size_t maxVisiblePoints = 8;
    const size_t startIndex = points.size() > maxVisiblePoints ? points.size() - maxVisiblePoints : 0;
    long long maxActivity = 0;
    for (size_t index = startIndex; index < points.size(); ++index) {
        maxActivity = std::max(maxActivity, points[index].activityCount);
    }
    maxActivity = std::max<long long>(maxActivity, 1);

    std::ostringstream stream;
    if (startIndex > 0) {
        stream << "Showing latest " << formatNumber(static_cast<long long>(points.size() - startIndex))
               << " of " << formatNumber(static_cast<long long>(points.size())) << " points.\r\n";
    }

    constexpr int kMaxBarWidth = 24;
    for (size_t index = startIndex; index < points.size(); ++index) {
        const AnalysisTrendPoint& point = points[index];
        int barWidth = static_cast<int>((point.activityCount * kMaxBarWidth) / maxActivity);
        if (point.activityCount > 0 && barWidth == 0) {
            barWidth = 1;
        }

        stream << padRight(fallbackText(point.dateLabel, "-"), 10)
               << " | "
               << (barWidth > 0 ? std::string(static_cast<size_t>(barWidth), '#') : std::string("-"))
               << " "
               << formatNumber(point.activityCount);

        if (index + 1 < points.size()) {
            stream << "\r\n";
        }
    }

    return stream.str();
}

std::string buildTrendComparisonText(
    const AnalysisViewModel& model,
    const AnalysisQuotaTrendSeries* selectedQuotaSeries = nullptr) {
    if (model.quotaTrendSeries.empty()) {
        return "Series comparison: activity-only scope. Click a bar in the chart to inspect a bucket.";
    }

    std::ostringstream stream;
    stream << "Series comparison:";
    const size_t limit = std::min<size_t>(model.quotaTrendSeries.size(), 3);
    for (size_t index = 0; index < limit; ++index) {
        const auto& series = model.quotaTrendSeries[index];
        stream << (index == 0 ? " " : " | ");
        if (selectedQuotaSeries != nullptr && series.key == selectedQuotaSeries->key) {
            stream << "[Selected] ";
        }
        stream << fallbackText(series.label, "Scoped quota")
               << " latest " << formatPercentage(series.latestRemainingPercentage)
               << ", low " << formatPercentage(series.lowWaterRemainingPercentage)
               << ", resets " << formatNumber(series.resetCount);
    }
    if (model.quotaTrendSeries.size() > limit) {
        stream << " | +" << formatNumber(static_cast<long long>(model.quotaTrendSeries.size() - limit))
               << " more";
    }
    stream << ".";
    return stream.str();
}

std::string formatUtcRangeLabel(std::time_t startUtc, std::time_t endUtc) {
    if (startUtc <= 0 || endUtc <= 0 || endUtc < startUtc) {
        return "n/a";
    }
    std::tm startTm{};
    gmtime_s(&startTm, &startUtc);
    std::tm endTm{};
    gmtime_s(&endTm, &endUtc);
    char startBuffer[32]{};
    char endBuffer[32]{};
    std::strftime(startBuffer, sizeof(startBuffer), "%Y-%m-%d %H:%M", &startTm);
    std::strftime(endBuffer, sizeof(endBuffer), "%Y-%m-%d %H:%M", &endTm);
    return std::string(startBuffer) + " to " + std::string(endBuffer) + " UTC";
}

std::string buildTrendDetailText(
    const AnalysisViewModel& model,
    const AnalysisQuotaTrendSeries* selectedQuotaSeries,
    int selectedActivityPointIndex,
    int selectedQuotaPointIndex) {
    if (selectedQuotaSeries != nullptr &&
        selectedQuotaPointIndex >= 0 &&
        selectedQuotaPointIndex < static_cast<int>(selectedQuotaSeries->points.size())) {
        const auto& point = selectedQuotaSeries->points[static_cast<size_t>(selectedQuotaPointIndex)];
        std::ostringstream stream;
        stream << "Selected quota point: " << fallbackText(selectedQuotaSeries->label, "Scoped quota")
               << " / " << fallbackText(point.label, "n/a")
               << ". Remaining " << formatPercentage(point.remainingPercentage);
        if (point.limitValue > 0) {
            stream << " (" << formatNumber(point.remainingValue)
                   << " / " << formatNumber(point.limitValue) << ")";
        }
        stream << ". Window " << fallbackText(selectedQuotaSeries->windowLabel, "n/a")
               << ". Reset marker: " << (point.resetPoint ? "yes" : "no")
               << ". Reconciled: " << (point.reconciled ? "yes" : "no") << ".";
        return stream.str();
    }

    if (selectedActivityPointIndex >= 0 &&
        selectedActivityPointIndex < static_cast<int>(model.trendPoints.size())) {
        const auto& point = model.trendPoints[static_cast<size_t>(selectedActivityPointIndex)];
        std::ostringstream stream;
        stream << "Selected activity bucket: " << fallbackText(point.dateLabel, "n/a")
               << ". Count " << formatNumber(point.activityCount)
               << ". Source " << fallbackText(point.sourceLabel, "Unknown")
               << ". Window " << formatUtcRangeLabel(point.bucketStartUtc, point.bucketEndUtc) << ".";
        return stream.str();
    }

    if (!model.trendPoints.empty()) {
        return "Click an activity bar to inspect its exact bucket range, count, and source mix.";
    }
    if (selectedQuotaSeries != nullptr && !selectedQuotaSeries->points.empty()) {
        return "Click a quota point to inspect remaining values, reset markers, and reconciliation state.";
    }
    return "No chart detail is available for the current scope yet.";
}

std::string buildSourceMixSummary(const std::vector<AnalysisDailySummary>& rows) {
    if (rows.empty()) {
        return "Daily sources: no daily source labels are available yet.";
    }

    std::map<std::string, long long> sourceCounts;
    for (const auto& row : rows) {
        sourceCounts[fallbackText(row.sourceLabel, "Unknown")] += 1;
    }

    std::vector<std::pair<std::string, long long>> entries(sourceCounts.begin(), sourceCounts.end());
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        if (left.second != right.second) {
            return left.second > right.second;
        }
        return left.first < right.first;
    });

    std::ostringstream stream;
    stream << "Daily sources: ";
    const size_t limit = std::min<size_t>(entries.size(), 3);
    for (size_t index = 0; index < limit; ++index) {
        if (index > 0) {
            stream << ", ";
        }
        stream << entries[index].first << " "
               << formatNumber(entries[index].second) << " "
               << pluralize(entries[index].second, "day", "days");
    }
    if (entries.size() > limit) {
        stream << ", +" << formatNumber(static_cast<long long>(entries.size() - limit)) << " more";
    }
    stream << ".";
    return stream.str();
}

std::string buildRecentAnnotationSummary(const std::vector<AnalysisDailySummary>& rows) {
    std::vector<std::string> highlights;
    highlights.reserve(2);

    for (auto it = rows.rbegin(); it != rows.rend() && highlights.size() < 2; ++it) {
        const std::string provenance = buildDailyProvenanceText(*it);
        if (provenance == "Observed") {
            continue;
        }
        highlights.push_back(fallbackText(it->dateLabel, "-") + " " + provenance);
    }

    if (highlights.empty()) {
        return "Recent notes: none beyond observed history.";
    }

    std::ostringstream stream;
    stream << "Recent notes: ";
    for (size_t index = 0; index < highlights.size(); ++index) {
        if (index > 0) {
            stream << "; ";
        }
        stream << highlights[index];
    }
    stream << ".";
    return stream.str();
}

bool isOperationalSentence(const std::string& sentence);
std::vector<std::string> extractDataQualitySentences(const std::string& text);

std::string buildDataQualityText(const AnalysisViewModel& model) {
    const long long reportDays = static_cast<long long>(model.dailySummaries.size());
    const long long activeDays = countActiveDays(model.dailySummaries);
    const auto qualitySentences = extractDataQualitySentences(model.dataQualityNote);

    std::ostringstream stream;
    if (qualitySentences.empty()) {
        stream << "Rollup-backed daily history across the current analysis scope.";
    } else {
        for (size_t index = 0; index < qualitySentences.size(); ++index) {
            if (index > 0) {
                stream << "\r\n";
            }
            stream << qualitySentences[index];
        }
    }
    stream << "\r\nCoverage in scope: " << formatNumber(reportDays)
           << " report " << pluralize(reportDays, "day", "days")
           << ", " << formatNumber(activeDays) << " active "
           << pluralize(activeDays, "day", "days") << ".";
    return stream.str();
}

std::string buildGapSummaryText(const AnalysisViewModel& model) {
    std::ostringstream stream;
    stream << "Gap mix: "
           << formatNumber(model.gapSummaryCounts.knownZeroDays) << " known zero, "
           << formatNumber(model.gapSummaryCounts.reconstructedDays) << " reconstructed, "
           << formatNumber(model.gapSummaryCounts.unknownDays) << " unknown, "
           << formatNumber(model.gapSummaryCounts.unknownSourceDays) << " unknown-source.\r\n";
    stream << buildSourceMixSummary(model.dailySummaries) << "\r\n";
    stream << buildRecentAnnotationSummary(model.dailySummaries);
    return stream.str();
}

std::vector<std::string> extractOperationalSentences(const std::string& text) {
    std::vector<std::string> matches;
    if (text.empty()) {
        return matches;
    }

    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('.', start);
        std::string sentence = text.substr(
            start,
            end == std::string::npos ? std::string::npos : (end - start + 1));
        sentence = trimToSingleLine(sentence);
        if (!sentence.empty() &&
            (sentence.find("Recovery checkpoints:") != std::string::npos ||
             sentence.find("Persisted quota history points in scope:") != std::string::npos ||
             sentence.find("Latest manual export:") != std::string::npos ||
             sentence.find("Latest scheduled export:") != std::string::npos)) {
            matches.push_back(sentence);
        }

        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
        while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
            ++start;
        }
    }

    return matches;
}

bool isOperationalSentence(const std::string& sentence) {
    return sentence.find("Recovery checkpoints:") != std::string::npos ||
        sentence.find("Persisted quota history points in scope:") != std::string::npos ||
        sentence.find("Latest manual export:") != std::string::npos ||
        sentence.find("Latest scheduled export:") != std::string::npos;
}

std::vector<std::string> extractDataQualitySentences(const std::string& text) {
    std::vector<std::string> matches;
    if (text.empty()) {
        return matches;
    }

    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('.', start);
        std::string sentence = text.substr(
            start,
            end == std::string::npos ? std::string::npos : (end - start + 1));
        sentence = trimToSingleLine(sentence);
        if (!sentence.empty() && !isOperationalSentence(sentence)) {
            matches.push_back(sentence);
        }

        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
        while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
            ++start;
        }
    }

    return matches;
}

std::string buildOperationalStatusHeadline(const AnalysisViewModel& model) {
    const auto sentences = extractOperationalSentences(model.dataQualityNote);
    const bool hasRecovery = std::any_of(sentences.begin(), sentences.end(), [](const std::string& sentence) {
        return sentence.find("Recovery checkpoints:") != std::string::npos;
    });
    const bool hasExport = std::any_of(sentences.begin(), sentences.end(), [](const std::string& sentence) {
        return sentence.find("Latest manual export:") != std::string::npos ||
            sentence.find("Latest scheduled export:") != std::string::npos;
    });
    const bool hasQuota = std::any_of(sentences.begin(), sentences.end(), [](const std::string& sentence) {
        return sentence.find("Persisted quota history points in scope:") != std::string::npos;
    });

    std::ostringstream stream;
    if (!hasRecovery && !hasExport && !hasQuota) {
        stream << "No scoped recovery, quota-history, or export metadata is available yet.";
    } else {
        stream << "Scoped operational context: ";
        if (hasRecovery) {
            stream << "recovery";
        }
        if (hasRecovery && (hasQuota || hasExport)) {
            stream << ", ";
        }
        if (hasQuota) {
            stream << "quota history";
        }
        if (hasQuota && hasExport) {
            stream << ", ";
        }
        if (hasExport) {
            stream << "export history";
        }
        stream << ".";
    }
    return stream.str();
}

std::string buildOperationalStatusDetail(const AnalysisViewModel& model) {
    const auto sentences = extractOperationalSentences(model.dataQualityNote);
    if (sentences.empty()) {
        return "Recovery, quota-history, and export notes will appear here once scoped metadata exists.";
    }

    std::ostringstream stream;
    for (size_t index = 0; index < sentences.size(); ++index) {
        if (index > 0) {
            stream << "\r\n";
        }
        stream << sentences[index];
    }
    return stream.str();
}

std::string buildPendingStatusHeadline() {
    return "Refreshing analysis for the updated scope...";
}

std::string buildPendingStatusDetail() {
    return "JiStats is applying the latest Analysis filters and refreshing persisted summaries in the background.";
}

RECT windowRectRelativeToParent(HWND parent, HWND child) {
    RECT rect{};
    if (parent == nullptr || child == nullptr || GetWindowRect(child, &rect) == 0) {
        return rect;
    }

    POINT points[2] = {
        {rect.left, rect.top},
        {rect.right, rect.bottom}
    };
    MapWindowPoints(HWND_DESKTOP, parent, points, 2);
    rect.left = points[0].x;
    rect.top = points[0].y;
    rect.right = points[1].x;
    rect.bottom = points[1].y;
    return rect;
}

void setListItemText(HWND list, int rowIndex, int columnIndex, const std::string& value) {
    std::wstring wide = utf8ToWide(value);
    LVITEMW item{};
    item.iSubItem = columnIndex;
    item.pszText = wide.data();
    SendMessageW(list, LVM_SETITEMTEXTW, static_cast<WPARAM>(rowIndex), reinterpret_cast<LPARAM>(&item));
}

void resizeListColumns(HWND list, const ListColumnDefinition* columns, size_t count, int scale) {
    if (list == nullptr) {
        return;
    }

    RECT rect{};
    if (GetClientRect(list, &rect) == 0) {
        return;
    }

    int availableWidth = std::max<int>(0, static_cast<int>(rect.right - rect.left));
    int totalMinimum = 0;
    int totalWeight = 0;
    for (size_t index = 0; index < count; ++index) {
        totalMinimum += MulDiv(columns[index].minWidth, scale, 96);
        totalWeight += columns[index].weight;
    }

    int extraWidth = std::max(0, availableWidth - totalMinimum);
    int assignedWidth = 0;
    for (size_t index = 0; index < count; ++index) {
        int columnWidth = MulDiv(columns[index].minWidth, scale, 96);
        if (extraWidth > 0 && totalWeight > 0) {
            if (index + 1 == count) {
                columnWidth = std::max(columnWidth, availableWidth - assignedWidth);
            } else {
                columnWidth += (extraWidth * columns[index].weight) / totalWeight;
            }
        }

        ListView_SetColumnWidth(list, static_cast<int>(index), columnWidth);
        assignedWidth += columnWidth;
    }
}

void initializeListColumns(HWND list, const ListColumnDefinition* columns, size_t count, int scale) {
    if (list == nullptr) {
        return;
    }

    while (ListView_DeleteColumn(list, 0) != FALSE) {
    }

    for (size_t index = 0; index < count; ++index) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
        column.fmt = LVCFMT_LEFT;
        column.pszText = const_cast<LPWSTR>(columns[index].title);
        column.cx = MulDiv(columns[index].minWidth, scale, 96);
        column.iSubItem = static_cast<int>(index);
        SendMessageW(
            list,
            LVM_INSERTCOLUMNW,
            static_cast<WPARAM>(index),
            reinterpret_cast<LPARAM>(&column));
    }
}

const wchar_t* analysisTrendChartWindowClassName() {
    return L"JiStatsAnalysisTrendChart";
}

} // namespace

class AnalysisPresenter::Impl {
public:
    HWND parent = nullptr;
    HWND scopeGroup = nullptr;
    HWND providerLabel = nullptr;
    HWND providerCombo = nullptr;
    HWND accountLabel = nullptr;
    HWND accountCombo = nullptr;
    HWND rangeLabel = nullptr;
    HWND rangeCombo = nullptr;
    HWND customStartLabel = nullptr;
    HWND customStartEdit = nullptr;
    HWND customEndLabel = nullptr;
    HWND customEndEdit = nullptr;
    HWND customDateHint = nullptr;
    HWND scopeSummary = nullptr;
    HWND workspaceGuide = nullptr;
    HWND navOverviewButton = nullptr;
    HWND navTrendButton = nullptr;
    HWND navOperationsButton = nullptr;
    HWND navBreakdownsButton = nullptr;
    HWND navDailyButton = nullptr;
    HWND navCoverageButton = nullptr;

    HWND summaryGroup = nullptr;
    HWND summaryHeadline = nullptr;
    HWND summaryStats = nullptr;
    HWND summaryCoverageCell = nullptr;
    HWND summaryBreakdownsCell = nullptr;
    HWND summaryThroughputCell = nullptr;

    HWND trendGroup = nullptr;
    HWND quotaSeriesLabel = nullptr;
    HWND quotaSeriesCombo = nullptr;
    HWND trendSummary = nullptr;
    HWND trendChart = nullptr;
    HWND trendComparison = nullptr;
    HWND trendDetail = nullptr;
    HWND statusGroup = nullptr;
    HWND statusHeadline = nullptr;
    HWND statusDetail = nullptr;

    HWND providerGroup = nullptr;
    HWND providerList = nullptr;
    HWND accountGroup = nullptr;
    HWND accountList = nullptr;
    HWND dailyGroup = nullptr;
    HWND dailyList = nullptr;

    HWND provenanceGroup = nullptr;
    HWND dataQuality = nullptr;
    HWND gapSummary = nullptr;

    HFONT bodyFont = nullptr;
    HFONT sectionFont = nullptr;
    HFONT bodyBoldFont = nullptr;
    HFONT bodyItalicFont = nullptr;
    HFONT monoFont = nullptr;
    HBRUSH overviewCoverageBrush = nullptr;
    HBRUSH overviewBreakdownsBrush = nullptr;
    HBRUSH overviewThroughputBrush = nullptr;
    HBRUSH scopeBrush = nullptr;
    HBRUSH trendBrush = nullptr;
    HBRUSH statusBrush = nullptr;
    HBRUSH provenanceBrush = nullptr;

    AnalysisViewModel viewModel;
    AnalysisScopeState currentScope;
    std::string selectedQuotaSeriesKey;
    int selectedActivityPointIndex = -1;
    int selectedQuotaPointIndex = -1;
    std::function<void(const AnalysisScopeState&)> scopeChangedCallback;
    bool interactionPending = false;

    bool suppressNotification = false;
    RECT lastLayoutBounds{};
    bool hasLastLayoutBounds = false;
    int scrollOffset = 0;
    int contentHeight = 0;
    int viewportHeight = 0;

    static constexpr int kProviderComboId = 2101;
    static constexpr int kAccountComboId = 2102;
    static constexpr int kRangeComboId = 2103;
    static constexpr int kCustomStartEditId = 2104;
    static constexpr int kCustomEndEditId = 2105;
    static constexpr UINT_PTR kCustomDateDebounceTimerId = 2106;
    static constexpr int kQuotaSeriesComboId = 2107;
    static constexpr int kNavOverviewButtonId = 2108;
    static constexpr int kNavTrendButtonId = 2109;
    static constexpr int kNavOperationsButtonId = 2110;
    static constexpr int kNavBreakdownsButtonId = 2111;
    static constexpr int kNavDailyButtonId = 2112;
    static constexpr int kNavCoverageButtonId = 2113;
    static constexpr const char* presetKeys[] = {"today", "last24", "last7", "currentQuota", "month", "custom"};

    bool init(HWND parentWindow) {
        if (scopeGroup != nullptr && parent == parentWindow) {
            return true;
        }
        parent = parentWindow;
        if (parent == nullptr) {
            return false;
        }

        initializeCommonControls();
        ensureFonts();
        ensureTrendChartClass();

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        const DWORD comboStyle = WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL;
        const DWORD editStyle = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL;
        const DWORD singleLineStaticStyle = WS_CHILD | WS_VISIBLE | SS_LEFT;
        const DWORD multilineStaticStyle = WS_CHILD | WS_VISIBLE | SS_LEFT;
        const DWORD groupStyle = WS_CHILD | WS_VISIBLE | BS_GROUPBOX;

        scopeGroup = CreateWindowExW(
            0, L"BUTTON", L"Filters & Scope",
            groupStyle,
            0, 0, 100, 100,
            parent, nullptr, instance, nullptr);

        providerLabel = CreateWindowExW(
            0, L"STATIC", L"Provider:",
            singleLineStaticStyle,
            0, 0, 60, 20,
            parent, nullptr, instance, nullptr);
        providerCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            comboStyle,
            0, 0, 140, 240,
            parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kProviderComboId)), instance, nullptr);

        accountLabel = CreateWindowExW(
            0, L"STATIC", L"Account:",
            singleLineStaticStyle,
            0, 0, 52, 20,
            parent, nullptr, instance, nullptr);
        accountCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            comboStyle,
            0, 0, 140, 240,
            parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kAccountComboId)), instance, nullptr);

        rangeLabel = CreateWindowExW(
            0, L"STATIC", L"Range:",
            singleLineStaticStyle,
            0, 0, 44, 20,
            parent, nullptr, instance, nullptr);
        rangeCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            comboStyle,
            0, 0, 140, 220,
            parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kRangeComboId)), instance, nullptr);

        customStartLabel = CreateWindowExW(
            0, L"STATIC", L"From:",
            singleLineStaticStyle,
            0, 0, 36, 20,
            parent, nullptr, instance, nullptr);
        customStartEdit = CreateWindowExW(
            0, L"EDIT", nullptr,
            editStyle,
            0, 0, 90, 20,
            parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kCustomStartEditId)), instance, nullptr);

        customEndLabel = CreateWindowExW(
            0, L"STATIC", L"To:",
            singleLineStaticStyle,
            0, 0, 24, 20,
            parent, nullptr, instance, nullptr);
        customEndEdit = CreateWindowExW(
            0, L"EDIT", nullptr,
            editStyle,
            0, 0, 90, 20,
            parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kCustomEndEditId)), instance, nullptr);
        customDateHint = CreateWindowExW(
            0, L"STATIC", L"Use YYYY-MM-DD dates. Changes apply after you pause typing.",
            multilineStaticStyle,
            0, 0, 100, 24,
            parent, nullptr, instance, nullptr);

        scopeSummary = CreateWindowExW(
            0, L"STATIC", L"Active filters: All Providers / All Accounts / Last 7 Days",
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            0, 0, 100, 20,
            parent, nullptr, instance, nullptr);
        workspaceGuide = CreateWindowExW(
            0, L"STATIC", utf8ToWide(buildWorkspaceGuideText()).c_str(),
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            0, 0, 100, 28,
            parent, nullptr, instance, nullptr);
        navOverviewButton = CreateWindowExW(
            0, L"BUTTON", L"Overview",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 100, 24,
            parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kNavOverviewButtonId)), instance, nullptr);
        navTrendButton = CreateWindowExW(
            0, L"BUTTON", L"Trend",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 100, 24,
            parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kNavTrendButtonId)), instance, nullptr);
        navOperationsButton = CreateWindowExW(
            0, L"BUTTON", L"Operational Signals",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 100, 24,
            parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kNavOperationsButtonId)), instance, nullptr);
        navBreakdownsButton = CreateWindowExW(
            0, L"BUTTON", L"Breakdowns",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 100, 24,
            parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kNavBreakdownsButtonId)), instance, nullptr);
        navDailyButton = CreateWindowExW(
            0, L"BUTTON", L"Daily Activity",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 100, 24,
            parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kNavDailyButtonId)), instance, nullptr);
        navCoverageButton = CreateWindowExW(
            0, L"BUTTON", L"Coverage & Provenance",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 100, 24,
            parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kNavCoverageButtonId)), instance, nullptr);

        summaryGroup = CreateWindowExW(
            0, L"BUTTON", L"Overview",
            groupStyle,
            0, 0, 100, 100,
            parent, nullptr, instance, nullptr);
        summaryHeadline = CreateWindowExW(
            0, L"STATIC", L"No analysis history is available for the current scope yet.",
            multilineStaticStyle,
            0, 0, 100, 36,
            parent, nullptr, instance, nullptr);
        summaryStats = CreateWindowExW(
            0, L"STATIC", L"Coverage window: 0 daily rows, 0 trend points.",
            WS_CHILD,
            0, 0, 100, 52,
            parent, nullptr, instance, nullptr);
        summaryCoverageCell = CreateWindowExW(
            0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            0, 0, 100, 70,
            parent, nullptr, instance, nullptr);
        summaryBreakdownsCell = CreateWindowExW(
            0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            0, 0, 100, 70,
            parent, nullptr, instance, nullptr);
        summaryThroughputCell = CreateWindowExW(
            0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            0, 0, 100, 70,
            parent, nullptr, instance, nullptr);

        trendGroup = CreateWindowExW(
            0, L"BUTTON", L"Activity Trend",
            groupStyle,
            0, 0, 100, 100,
            parent, nullptr, instance, nullptr);
        quotaSeriesLabel = CreateWindowExW(
            0, L"STATIC", L"Quota series:",
            singleLineStaticStyle,
            0, 0, 78, 20,
            parent, nullptr, instance, nullptr);
        quotaSeriesCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            comboStyle,
            0, 0, 180, 220,
            parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kQuotaSeriesComboId)), instance, nullptr);
        trendSummary = CreateWindowExW(
            0, L"STATIC", L"No trend points are available for the selected scope yet.",
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            0, 0, 100, 40,
            parent, nullptr, instance, nullptr);
        trendChart = CreateWindowExW(
            0, analysisTrendChartWindowClassName(), L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            0, 0, 100, 80,
            parent, nullptr, instance, this);
        trendComparison = CreateWindowExW(
            0, L"STATIC", L"Series comparison: no quota series are available for this scope yet.",
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            0, 0, 100, 40,
            parent, nullptr, instance, nullptr);
        trendDetail = CreateWindowExW(
            0, L"STATIC", L"No chart detail is available for the current scope yet.",
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            0, 0, 100, 40,
            parent, nullptr, instance, nullptr);

        statusGroup = CreateWindowExW(
            0, L"BUTTON", L"Operational Signals",
            groupStyle,
            0, 0, 100, 100,
            parent, nullptr, instance, nullptr);
        statusHeadline = CreateWindowExW(
            0, L"STATIC", L"No scoped recovery, quota-history, or export metadata is available yet.",
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            0, 0, 100, 40,
            parent, nullptr, instance, nullptr);
        statusDetail = CreateWindowExW(
            0, L"STATIC", L"Recovery, quota-history, and export notes will appear here once scoped metadata exists.",
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            0, 0, 100, 80,
            parent, nullptr, instance, nullptr);

        providerGroup = CreateWindowExW(
            0, L"BUTTON", L"By Provider",
            groupStyle,
            0, 0, 100, 100,
            parent, nullptr, instance, nullptr);
        providerList = createListView(instance);

        accountGroup = CreateWindowExW(
            0, L"BUTTON", L"By Account",
            groupStyle,
            0, 0, 100, 100,
            parent, nullptr, instance, nullptr);
        accountList = createListView(instance);

        dailyGroup = CreateWindowExW(
            0, L"BUTTON", L"Daily Activity",
            groupStyle,
            0, 0, 100, 100,
            parent, nullptr, instance, nullptr);
        dailyList = createListView(instance);

        provenanceGroup = CreateWindowExW(
            0, L"BUTTON", L"Coverage & Provenance",
            groupStyle,
            0, 0, 100, 100,
            parent, nullptr, instance, nullptr);
        dataQuality = CreateWindowExW(
            0, L"STATIC", L"Rollup-backed daily history across the current analysis scope.",
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            0, 0, 100, 40,
            parent, nullptr, instance, nullptr);
        gapSummary = CreateWindowExW(
            0, L"STATIC", L"Gap mix: 0 known zero, 0 reconstructed, 0 unknown, 0 unknown-source.",
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            0, 0, 100, 70,
            parent, nullptr, instance, nullptr);

        applyFonts();
        configureListView(providerList, kBreakdownColumns, std::size(kBreakdownColumns));
        configureListView(accountList, kBreakdownColumns, std::size(kBreakdownColumns));
        configureListView(dailyList, kDailyColumns, std::size(kDailyColumns));

        populateRangeCombo();
        updateViewModel(viewModel);

        return scopeGroup != nullptr &&
            providerCombo != nullptr &&
            accountCombo != nullptr &&
            rangeCombo != nullptr &&
            customDateHint != nullptr &&
            workspaceGuide != nullptr &&
            navOverviewButton != nullptr &&
            navTrendButton != nullptr &&
            navOperationsButton != nullptr &&
            navBreakdownsButton != nullptr &&
            navDailyButton != nullptr &&
            navCoverageButton != nullptr &&
            summaryGroup != nullptr &&
            summaryCoverageCell != nullptr &&
            summaryBreakdownsCell != nullptr &&
            summaryThroughputCell != nullptr &&
            trendGroup != nullptr &&
            quotaSeriesLabel != nullptr &&
            quotaSeriesCombo != nullptr &&
            trendComparison != nullptr &&
            trendDetail != nullptr &&
            statusGroup != nullptr &&
            providerList != nullptr &&
            accountList != nullptr &&
            dailyList != nullptr &&
            provenanceGroup != nullptr;
    }

    void shutdown() {
        if (parent != nullptr) {
            KillTimer(parent, kCustomDateDebounceTimerId);
            SCROLLINFO info{};
            info.cbSize = sizeof(info);
            info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
            info.nMin = 0;
            info.nMax = 0;
            info.nPage = 0;
            info.nPos = 0;
            SetScrollInfo(parent, SB_VERT, &info, TRUE);
            ShowScrollBar(parent, SB_VERT, FALSE);
        }
        destroyWindow(scopeGroup);
        destroyWindow(providerLabel);
        destroyWindow(providerCombo);
        destroyWindow(accountLabel);
        destroyWindow(accountCombo);
        destroyWindow(rangeLabel);
        destroyWindow(rangeCombo);
        destroyWindow(customStartLabel);
        destroyWindow(customStartEdit);
        destroyWindow(customEndLabel);
        destroyWindow(customEndEdit);
        destroyWindow(customDateHint);
        destroyWindow(scopeSummary);
        destroyWindow(workspaceGuide);
        destroyWindow(navOverviewButton);
        destroyWindow(navTrendButton);
        destroyWindow(navOperationsButton);
        destroyWindow(navBreakdownsButton);
        destroyWindow(navDailyButton);
        destroyWindow(navCoverageButton);

        destroyWindow(summaryGroup);
        destroyWindow(summaryHeadline);
        destroyWindow(summaryStats);
        destroyWindow(summaryCoverageCell);
        destroyWindow(summaryBreakdownsCell);
        destroyWindow(summaryThroughputCell);

        destroyWindow(trendGroup);
        destroyWindow(quotaSeriesLabel);
        destroyWindow(quotaSeriesCombo);
        destroyWindow(trendSummary);
        destroyWindow(trendChart);
        destroyWindow(trendComparison);
        destroyWindow(trendDetail);
        destroyWindow(statusGroup);
        destroyWindow(statusHeadline);
        destroyWindow(statusDetail);

        destroyWindow(providerGroup);
        destroyWindow(providerList);
        destroyWindow(accountGroup);
        destroyWindow(accountList);
        destroyWindow(dailyGroup);
        destroyWindow(dailyList);

        destroyWindow(provenanceGroup);
        destroyWindow(dataQuality);
        destroyWindow(gapSummary);

        releaseFonts();
        releaseBrushes();
        parent = nullptr;
        interactionPending = false;
        scrollOffset = 0;
        contentHeight = 0;
        viewportHeight = 0;
        hasLastLayoutBounds = false;
        std::memset(&lastLayoutBounds, 0, sizeof(lastLayoutBounds));
    }

    void populateRangeCombo() {
        if (rangeCombo == nullptr) return;
        SendMessageW(rangeCombo, CB_RESETCONTENT, 0, 0);
        struct PresetOption {
            std::string key;
            std::wstring label;
        };
        const PresetOption options[] = {
            {"today", L"Today"},
            {"last24", L"Last 24 Hours"},
            {"last7", L"Last 7 Days"},
            {"currentQuota", L"Current Billing/Quota Window"},
            {"month", L"This Month"},
            {"custom", L"Custom"}
        };
        for (const auto& opt : options) {
            SendMessageW(rangeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(opt.label.c_str()));
        }
    }

    void populateCombo(HWND combo, const std::vector<AnalysisScopeOption>& options, const std::string& selectedKey) {
        if (combo == nullptr) return;
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        int selectedIndex = -1;
        for (int i = 0; i < static_cast<int>(options.size()); ++i) {
            const auto& opt = options[static_cast<size_t>(i)];
            const std::wstring label = utf8ToWide(opt.label);
            const int added = static_cast<int>(SendMessageW(
                combo, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(label.c_str())));
            if (added >= 0 && opt.key == selectedKey) {
                selectedIndex = added;
            }
        }
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(selectedIndex), 0);
    }

    const AnalysisQuotaTrendSeries* selectedQuotaSeries() const {
        if (viewModel.quotaTrendSeries.empty()) {
            return nullptr;
        }
        if (!selectedQuotaSeriesKey.empty()) {
            for (const auto& series : viewModel.quotaTrendSeries) {
                if (series.key == selectedQuotaSeriesKey) {
                    return &series;
                }
            }
        }
        return &viewModel.quotaTrendSeries.front();
    }

    void populateQuotaSeriesCombo() {
        if (quotaSeriesCombo == nullptr) {
            return;
        }

        SendMessageW(quotaSeriesCombo, CB_RESETCONTENT, 0, 0);
        int selectedIndex = -1;
        for (int i = 0; i < static_cast<int>(viewModel.quotaTrendSeries.size()); ++i) {
            const auto& series = viewModel.quotaTrendSeries[static_cast<size_t>(i)];
            const std::wstring label = utf8ToWide(series.label);
            const int added = static_cast<int>(SendMessageW(
                quotaSeriesCombo,
                CB_ADDSTRING,
                0,
                reinterpret_cast<LPARAM>(label.c_str())));
            if (added >= 0 && series.key == selectedQuotaSeriesKey) {
                selectedIndex = added;
            }
        }

        if (selectedIndex < 0 && !viewModel.quotaTrendSeries.empty()) {
            selectedQuotaSeriesKey = viewModel.quotaTrendSeries.front().key;
            selectedIndex = 0;
        }

        SendMessageW(quotaSeriesCombo, CB_SETCURSEL, static_cast<WPARAM>(selectedIndex), 0);
        EnableWindow(quotaSeriesCombo, viewModel.quotaTrendSeries.size() > 1 ? TRUE : FALSE);
        ShowWindow(quotaSeriesCombo, viewModel.quotaTrendSeries.empty() ? SW_HIDE : SW_SHOW);
        if (quotaSeriesLabel != nullptr) {
            ShowWindow(quotaSeriesLabel, viewModel.quotaTrendSeries.size() > 1 ? SW_SHOW : SW_HIDE);
        }
    }

    std::string currentTrendSummaryText() const {
        return buildTrendSummary(viewModel, selectedQuotaSeries());
    }

    std::string currentTrendChartText() const {
        return buildTrendChartText(viewModel, selectedQuotaSeries());
    }

    std::string currentTrendComparisonText() const {
        return buildTrendComparisonText(viewModel, selectedQuotaSeries());
    }

    std::string currentTrendDetailText() const {
        return buildTrendDetailText(
            viewModel,
            selectedQuotaSeries(),
            selectedActivityPointIndex,
            selectedQuotaPointIndex);
    }

    static LRESULT CALLBACK trendChartWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self == nullptr) {
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        return self->handleTrendChartMessage(hwnd, msg, wParam, lParam);
    }

    void ensureTrendChartClass() const {
        static std::once_flag once;
        std::call_once(once, []() {
            WNDCLASSW wc{};
            wc.lpfnWndProc = &Impl::trendChartWndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = analysisTrendChartWindowClassName();
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            RegisterClassW(&wc);
        });
    }

    bool selectActivityPoint(int index) {
        if (index < 0 || index >= static_cast<int>(viewModel.trendPoints.size())) {
            return false;
        }
        selectedActivityPointIndex = index;
        selectedQuotaPointIndex = -1;
        setWindowTextUtf8(trendDetail, currentTrendDetailText());
        if (trendChart != nullptr) {
            InvalidateRect(trendChart, nullptr, TRUE);
            UpdateWindow(trendChart);
        }
        return true;
    }

    bool selectQuotaPoint(int index) {
        const auto* series = selectedQuotaSeries();
        if (series == nullptr || index < 0 || index >= static_cast<int>(series->points.size())) {
            return false;
        }
        selectedQuotaPointIndex = index;
        selectedActivityPointIndex = -1;
        setWindowTextUtf8(trendDetail, currentTrendDetailText());
        if (trendChart != nullptr) {
            InvalidateRect(trendChart, nullptr, TRUE);
            UpdateWindow(trendChart);
        }
        return true;
    }

    void syncDefaultTrendSelection() {
        const auto* series = selectedQuotaSeries();
        if (series != nullptr && !series->points.empty()) {
            selectedQuotaPointIndex = static_cast<int>(series->points.size()) - 1;
            selectedActivityPointIndex = -1;
            return;
        }
        if (!viewModel.trendPoints.empty()) {
            selectedActivityPointIndex = static_cast<int>(viewModel.trendPoints.size()) - 1;
            selectedQuotaPointIndex = -1;
            return;
        }
        selectedActivityPointIndex = -1;
        selectedQuotaPointIndex = -1;
    }

    RECT trendActivityChartRect(const RECT& clientRect) const {
        RECT rect = clientRect;
        InflateRect(&rect, -scaledPixels(8), -scaledPixels(8));
        if (selectedQuotaSeries() != nullptr && !selectedQuotaSeries()->points.empty() &&
            !viewModel.trendPoints.empty()) {
            rect.bottom = rect.top + ((rect.bottom - rect.top) * 55 / 100);
        }
        return rect;
    }

    RECT trendQuotaChartRect(const RECT& clientRect) const {
        RECT rect = clientRect;
        InflateRect(&rect, -scaledPixels(8), -scaledPixels(8));
        const auto* series = selectedQuotaSeries();
        if (series == nullptr || series->points.empty()) {
            rect.bottom = rect.top;
            return rect;
        }
        if (!viewModel.trendPoints.empty()) {
            const int splitTop = rect.top + ((rect.bottom - rect.top) * 55 / 100) + scaledPixels(10);
            rect.top = splitTop;
        }
        return rect;
    }

    void drawActivityChart(HDC dc, const RECT& rect) const {
        if (viewModel.trendPoints.empty() || rect.bottom <= rect.top || rect.right <= rect.left) {
            return;
        }

        HBRUSH axisBrush = CreateSolidBrush(RGB(210, 214, 220));
        RECT axisRect{rect.left, rect.bottom - 1, rect.right, rect.bottom};
        FillRect(dc, &axisRect, axisBrush);
        DeleteObject(axisBrush);

        long long maxCount = 1;
        for (const auto& point : viewModel.trendPoints) {
            maxCount = std::max(maxCount, point.activityCount);
        }

        const int count = static_cast<int>(viewModel.trendPoints.size());
        const int rectWidth = std::max(0, static_cast<int>(rect.right - rect.left));
        const int slotWidth = std::max(1, rectWidth / std::max(1, count));
        const int barWidth = std::max(3, std::min(slotWidth - scaledPixels(3), scaledPixels(22)));
        for (int index = 0; index < count; ++index) {
            const auto& point = viewModel.trendPoints[static_cast<size_t>(index)];
            const int left = rect.left + (index * slotWidth) + std::max(0, (slotWidth - barWidth) / 2);
            const int rectHeight = std::max(0, static_cast<int>(rect.bottom - rect.top));
            const int barHeight = point.activityCount <= 0
                ? 0
                : std::max(2, static_cast<int>(((rectHeight - scaledPixels(6)) * point.activityCount) / maxCount));
            RECT barRect{left, rect.bottom - barHeight - 1, left + barWidth, rect.bottom - 1};
            HBRUSH brush = CreateSolidBrush(
                index == selectedActivityPointIndex ? RGB(36, 104, 190) : RGB(96, 154, 220));
            FillRect(dc, &barRect, brush);
            DeleteObject(brush);
        }
    }

    void drawQuotaChart(HDC dc, const RECT& rect) const {
        const auto* series = selectedQuotaSeries();
        if (series == nullptr || series->points.empty() || rect.bottom <= rect.top || rect.right <= rect.left) {
            return;
        }

        HPEN axisPen = CreatePen(PS_SOLID, 1, RGB(210, 214, 220));
        HPEN seriesPen = CreatePen(PS_SOLID, 2, RGB(42, 126, 74));
        HPEN resetPen = CreatePen(PS_DOT, 1, RGB(196, 74, 74));
        HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(dc, axisPen));
        MoveToEx(dc, rect.left, rect.bottom - 1, nullptr);
        LineTo(dc, rect.right, rect.bottom - 1);
        LineTo(dc, rect.right, rect.top);
        SelectObject(dc, seriesPen);

        const int count = static_cast<int>(series->points.size());
        const int width = std::max(1, static_cast<int>(rect.right - rect.left));
        const int height = std::max(1, static_cast<int>(rect.bottom - rect.top) - scaledPixels(4));
        auto pointX = [&](int index) {
            if (count <= 1) {
                return rect.left + (width / 2);
            }
            return rect.left + (index * width / (count - 1));
        };
        auto pointY = [&](double percentage) {
            const double clamped = std::clamp(percentage, 0.0, 100.0);
            return rect.bottom - 1 - static_cast<int>((clamped / 100.0) * height);
        };

        for (int index = 0; index < count; ++index) {
            const auto& point = series->points[static_cast<size_t>(index)];
            const int x = pointX(index);
            const int y = pointY(point.remainingPercentage);
            if (index == 0) {
                MoveToEx(dc, x, y, nullptr);
            } else {
                LineTo(dc, x, y);
            }
            if (point.resetPoint) {
                SelectObject(dc, resetPen);
                MoveToEx(dc, x, rect.top, nullptr);
                LineTo(dc, x, rect.bottom);
                SelectObject(dc, seriesPen);
                MoveToEx(dc, x, y, nullptr);
            }
            RECT marker{x - 3, y - 3, x + 4, y + 4};
            HBRUSH markerBrush = CreateSolidBrush(
                index == selectedQuotaPointIndex ? RGB(34, 83, 54) : RGB(64, 166, 96));
            FillRect(dc, &marker, markerBrush);
            DeleteObject(markerBrush);
        }

        SelectObject(dc, oldPen);
        DeleteObject(axisPen);
        DeleteObject(seriesPen);
        DeleteObject(resetPen);
    }

    int nearestActivityPointIndex(int x, const RECT& rect) const {
        if (viewModel.trendPoints.empty() || rect.right <= rect.left) {
            return -1;
        }
        const int count = static_cast<int>(viewModel.trendPoints.size());
        const int rectWidth = std::max(0, static_cast<int>(rect.right - rect.left));
        const int slotWidth = std::max(1, rectWidth / std::max(1, count));
        int bestIndex = 0;
        int bestDistance = std::numeric_limits<int>::max();
        for (int index = 0; index < count; ++index) {
            const int center = rect.left + (index * slotWidth) + (slotWidth / 2);
            const int distance = std::abs(x - center);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestIndex = index;
            }
        }
        return bestIndex;
    }

    int nearestQuotaPointIndex(int x, const RECT& rect) const {
        const auto* series = selectedQuotaSeries();
        if (series == nullptr || series->points.empty() || rect.right <= rect.left) {
            return -1;
        }
        const int count = static_cast<int>(series->points.size());
        int bestIndex = 0;
        int bestDistance = std::numeric_limits<int>::max();
        for (int index = 0; index < count; ++index) {
            const int center = count <= 1
                ? rect.left + (static_cast<int>(rect.right - rect.left) / 2)
                : rect.left + (index * std::max(1, static_cast<int>(rect.right - rect.left)) / (count - 1));
            const int distance = std::abs(x - center);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestIndex = index;
            }
        }
        return bestIndex;
    }

    LRESULT handleTrendChartMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_LBUTTONDOWN: {
            RECT clientRect{};
            GetClientRect(hwnd, &clientRect);
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            const RECT quotaRect = trendQuotaChartRect(clientRect);
            if (y >= quotaRect.top && y <= quotaRect.bottom && selectQuotaPoint(nearestQuotaPointIndex(x, quotaRect))) {
                SetFocus(hwnd);
                return 0;
            }
            const RECT activityRect = trendActivityChartRect(clientRect);
            if (y >= activityRect.top && y <= activityRect.bottom && selectActivityPoint(nearestActivityPointIndex(x, activityRect))) {
                SetFocus(hwnd);
                return 0;
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT clientRect{};
            GetClientRect(hwnd, &clientRect);
            FillRect(dc, &clientRect, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            FrameRect(dc, &clientRect, reinterpret_cast<HBRUSH>(COLOR_WINDOWFRAME + 1));
            if (viewModel.trendPoints.empty() &&
                (selectedQuotaSeries() == nullptr || selectedQuotaSeries()->points.empty())) {
                DrawTextW(
                    dc,
                    L"No chartable trend history is available yet.",
                    -1,
                    &clientRect,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                drawActivityChart(dc, trendActivityChartRect(clientRect));
                drawQuotaChart(dc, trendQuotaChartRect(clientRect));
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    void updateViewModel(const AnalysisViewModel& model) {
        viewModel = model;
        currentScope = normalizeScope(model.scope);
        interactionPending = false;
        if (!selectedQuotaSeriesKey.empty()) {
            bool foundSelectedSeries = false;
            for (const auto& series : viewModel.quotaTrendSeries) {
                if (series.key == selectedQuotaSeriesKey) {
                    foundSelectedSeries = true;
                    break;
                }
            }
            if (!foundSelectedSeries) {
                selectedQuotaSeriesKey.clear();
            }
        }
        if (parent != nullptr) {
            KillTimer(parent, kCustomDateDebounceTimerId);
        }

        suppressNotification = true;
        populateCombo(providerCombo, model.availableProviders, currentScope.selectedProviderId);
        populateCombo(accountCombo, model.availableAccounts, currentScope.selectedAccountId);

        if (rangeCombo != nullptr) {
            const std::string key = presetKeyFromEnum(currentScope.dateRangePreset);
            int rangeIndex = 2;
            for (int i = 0; i < static_cast<int>(std::size(presetKeys)); ++i) {
                if (presetKeys[i] == key) {
                    rangeIndex = i;
                    break;
                }
            }
            SendMessageW(rangeCombo, CB_SETCURSEL, static_cast<WPARAM>(rangeIndex), 0);
        }

        setWindowTextUtf8(customStartEdit, formatUtcDateOnly(currentScope.customRangeStartUtc));
        setWindowTextUtf8(customEndEdit, formatUtcDateOnly(currentScope.customRangeEndUtc));
        applyCustomDateEnabledState(currentScope.dateRangePreset == AnalysisDateRangePreset::Custom);

        setWindowTextUtf8(
            scopeSummary,
            buildScopeSummaryText(model.scopeLabel));
        populateQuotaSeriesCombo();
        syncDefaultTrendSelection();
        setWindowTextUtf8(summaryHeadline, buildOverviewHeadline(model));
        setWindowTextUtf8(summaryStats, buildOverviewStats(model));
        const AnalysisOverviewCells overviewCells = buildOverviewCells(model);
        setWindowTextUtf8(summaryCoverageCell, overviewCells.coverage);
        setWindowTextUtf8(summaryBreakdownsCell, overviewCells.breakdowns);
        setWindowTextUtf8(summaryThroughputCell, overviewCells.throughput);
        setWindowTextUtf8(trendSummary, currentTrendSummaryText());
        setWindowTextUtf8(trendComparison, currentTrendComparisonText());
        setWindowTextUtf8(trendDetail, currentTrendDetailText());
        if (trendChart != nullptr) {
            InvalidateRect(trendChart, nullptr, TRUE);
        }
        setWindowTextUtf8(statusHeadline, currentStatusHeadline());
        setWindowTextUtf8(statusDetail, currentStatusDetail());
        setWindowTextUtf8(dataQuality, buildDataQualityText(model));
        setWindowTextUtf8(gapSummary, buildGapSummaryText(model));
        applyScopeInputEnabledState();

        populateBreakdownList(providerList, model.providerBreakdown, "No provider breakdown is available for this scope yet.");
        populateBreakdownList(accountList, model.accountBreakdown, "No account breakdown is available for this scope yet.");
        populateDailyList(model.dailySummaries);

        suppressNotification = false;
    }

    void setVisible(bool visible) {
        const int flag = visible ? SW_SHOW : SW_HIDE;
        const HWND controls[] = {
            scopeGroup, providerLabel, providerCombo,
            accountLabel, accountCombo, rangeLabel, rangeCombo,
            customDateHint, scopeSummary, workspaceGuide,
            navOverviewButton, navTrendButton, navOperationsButton,
            navBreakdownsButton, navDailyButton, navCoverageButton,
            summaryGroup, summaryHeadline, summaryCoverageCell, summaryBreakdownsCell, summaryThroughputCell,
            trendGroup, quotaSeriesLabel, quotaSeriesCombo, trendSummary, trendChart, trendComparison, trendDetail,
            statusGroup, statusHeadline, statusDetail,
            providerGroup, providerList, accountGroup, accountList,
            dailyGroup, dailyList,
            provenanceGroup, dataQuality, gapSummary
        };
        for (HWND hwnd : controls) {
            if (hwnd != nullptr) {
                ShowWindow(hwnd, flag);
            }
        }
        applyCustomDateEnabledState(visible && currentScope.dateRangePreset == AnalysisDateRangePreset::Custom);
    }

    void layout(const RECT& bounds) {
        lastLayoutBounds = bounds;
        hasLastLayoutBounds = true;

        const int padding = scaledPixels(14);
        const int groupGap = scaledPixels(14);
        const int groupTextTop = scaledPixels(24);
        const int groupTextSide = scaledPixels(12);
        const int rowHeight = scaledPixels(24);
        const int comboWidth = scaledPixels(180);
        const int compactComboHeight = scaledPixels(220);
        const int editWidth = scaledPixels(110);
        const int singleLineHeight = scaledPixels(20);
        const bool showCustomDateInputs = currentScope.dateRangePreset == AnalysisDateRangePreset::Custom;
        const bool showQuotaSeriesSelector = viewModel.quotaTrendSeries.size() > 1;
        const int customDateHintHeight = showCustomDateInputs ? scaledPixels(26) : 0;

        const int totalWidth = std::max<int>(0, static_cast<int>(bounds.right - bounds.left));
        const int totalHeight = std::max<int>(0, static_cast<int>(bounds.bottom - bounds.top));
        const int innerLeft = bounds.left + padding;
        const int innerTop = bounds.top + padding;
        const int innerWidth = std::max(0, totalWidth - (padding * 2));
        const bool narrowScopeLayout = innerWidth < scaledPixels(980);
        const bool singleColumnContent = innerWidth < scaledPixels(860);
        const bool compactTopLayout = !singleColumnContent && innerWidth < scaledPixels(1260);

        int y = innerTop + scaledPixels(8);
        if (narrowScopeLayout) {
            const int labelWidth = scaledPixels(60);
            const int fieldWidth = std::max(scaledPixels(120), innerWidth - labelWidth - scaledPixels(6));

            MoveWindow(providerLabel, innerLeft, y, labelWidth, singleLineHeight, TRUE);
            MoveWindow(providerCombo,
                       innerLeft + labelWidth,
                       y - scaledPixels(2),
                       fieldWidth,
                       compactComboHeight,
                       TRUE);
            y += rowHeight;

            MoveWindow(accountLabel, innerLeft, y, labelWidth, singleLineHeight, TRUE);
            MoveWindow(accountCombo,
                       innerLeft + labelWidth,
                       y - scaledPixels(2),
                       fieldWidth,
                       compactComboHeight,
                       TRUE);
            y += rowHeight;

            MoveWindow(rangeLabel, innerLeft, y, labelWidth, singleLineHeight, TRUE);
            MoveWindow(rangeCombo,
                       innerLeft + labelWidth,
                       y - scaledPixels(2),
                       fieldWidth,
                       compactComboHeight,
                       TRUE);
            y += rowHeight;

            if (showCustomDateInputs) {
                MoveWindow(customStartLabel, innerLeft, y, labelWidth, singleLineHeight, TRUE);
                MoveWindow(customStartEdit,
                           innerLeft + labelWidth,
                           y - scaledPixels(2),
                           std::min(editWidth, fieldWidth),
                           scaledPixels(22),
                           TRUE);
                y += rowHeight;

                MoveWindow(customEndLabel, innerLeft, y, labelWidth, singleLineHeight, TRUE);
                MoveWindow(customEndEdit,
                           innerLeft + labelWidth,
                           y - scaledPixels(2),
                           std::min(editWidth, fieldWidth),
                           scaledPixels(22),
                           TRUE);
                y += rowHeight;
                MoveWindow(customDateHint, innerLeft, y, innerWidth, customDateHintHeight, TRUE);
                y += customDateHintHeight;
            }
        } else {
            int x = innerLeft;
            MoveWindow(providerLabel, x, y, scaledPixels(56), singleLineHeight, TRUE);
            x += scaledPixels(60);
            MoveWindow(providerCombo, x, y - scaledPixels(2), comboWidth, compactComboHeight, TRUE);
            x += comboWidth + scaledPixels(16);

            MoveWindow(accountLabel, x, y, scaledPixels(54), singleLineHeight, TRUE);
            x += scaledPixels(58);
            MoveWindow(accountCombo, x, y - scaledPixels(2), comboWidth, compactComboHeight, TRUE);

            x = innerLeft;
            y += rowHeight;

            MoveWindow(rangeLabel, x, y, scaledPixels(46), singleLineHeight, TRUE);
            x += scaledPixels(50);
            MoveWindow(rangeCombo, x, y - scaledPixels(2), comboWidth, compactComboHeight, TRUE);
            if (showCustomDateInputs) {
                x += comboWidth + scaledPixels(16);
                MoveWindow(customStartLabel, x, y, scaledPixels(38), singleLineHeight, TRUE);
                x += scaledPixels(42);
                MoveWindow(customStartEdit, x, y - scaledPixels(2), editWidth, scaledPixels(22), TRUE);
                x += editWidth + scaledPixels(10);

                MoveWindow(customEndLabel, x, y, scaledPixels(26), singleLineHeight, TRUE);
                x += scaledPixels(30);
                MoveWindow(customEndEdit, x, y - scaledPixels(2), editWidth, scaledPixels(22), TRUE);
                y += rowHeight;
                MoveWindow(customDateHint, innerLeft, y, innerWidth, customDateHintHeight, TRUE);
            } else {
                MoveWindow(customDateHint, innerLeft, y, innerWidth, 0, TRUE);
            }

            y += rowHeight;
        }

        y += scaledPixels(10);
        const std::string scopeSummaryText = buildScopeSummaryText(viewModel.scopeLabel);
        const int scopeSummaryHeight = measureMultilineTextHeight(
            scopeSummary,
            scopeSummaryText,
            bodyFont,
            innerWidth,
            singleLineHeight,
            narrowScopeLayout ? scaledPixels(52) : scaledPixels(40));
        const std::string workspaceGuideText = buildWorkspaceGuideText();
        const int workspaceGuideHeight = measureMultilineTextHeight(
            workspaceGuide,
            workspaceGuideText,
            bodyFont,
            innerWidth,
            scaledPixels(34),
            narrowScopeLayout ? scaledPixels(56) : scaledPixels(44));
        const int navButtonHeight = scaledPixels(26);
        const int navButtonGap = scaledPixels(8);
        const int navButtonRowGap = scaledPixels(8);
        const int navButtonWidths[] = {
            scaledPixels(104),
            scaledPixels(96),
            scaledPixels(154),
            scaledPixels(116),
            scaledPixels(118),
            scaledPixels(172)
        };
        const HWND navButtons[] = {
            navOverviewButton,
            navTrendButton,
            navOperationsButton,
            navBreakdownsButton,
            navDailyButton,
            navCoverageButton
        };
        int navX = innerLeft;
        int navY = y + scopeSummaryHeight + scaledPixels(8) + workspaceGuideHeight + scaledPixels(8);
        int navRows = 1;
        for (size_t index = 0; index < std::size(navButtons); ++index) {
            const int width = std::min(navButtonWidths[index], innerWidth);
            if (navX > innerLeft && navX + width > innerLeft + innerWidth) {
                navX = innerLeft;
                navY += navButtonHeight + navButtonRowGap;
                ++navRows;
            }
            MoveWindow(navButtons[index], navX, navY, width, navButtonHeight, TRUE);
            navX += width + navButtonGap;
        }
        const int navButtonsHeight = (navRows * navButtonHeight) + ((navRows - 1) * navButtonRowGap);
        const int minimumScopeGroupHeight = narrowScopeLayout ? scaledPixels(252) : scaledPixels(214);
        const int scopeGroupHeight = std::max(
            minimumScopeGroupHeight,
            y - static_cast<int>(bounds.top) + scopeSummaryHeight + scaledPixels(8) +
                workspaceGuideHeight + scaledPixels(8) + navButtonsHeight + scaledPixels(16));
        const int contentTop = bounds.top + scopeGroupHeight + groupGap;

        const int headlineHeight = measureMultilineTextHeight(
            summaryHeadline,
            buildOverviewHeadline(viewModel),
            sectionFont,
            std::max(0, innerWidth - (groupTextSide * 2)),
            scaledPixels(34),
            scaledPixels(74));
        const int overviewCellHeight = scaledPixels(92);
        const int trendSummaryHeight = measureMultilineTextHeight(
            trendSummary,
            currentTrendSummaryText(),
            bodyFont,
            std::max(0, innerWidth - (groupTextSide * 2)),
            scaledPixels(44),
            scaledPixels(74));
        const int trendComparisonHeight = measureMultilineTextHeight(
            trendComparison,
            currentTrendComparisonText(),
            bodyFont,
            std::max(0, innerWidth - (groupTextSide * 2)),
            scaledPixels(44),
            scaledPixels(72));
        const int trendChartHeight = std::max(scaledPixels(140), innerWidth < scaledPixels(900) ? scaledPixels(160) : scaledPixels(180));
        const int trendDetailHeight = measureMultilineTextHeight(
            trendDetail,
            currentTrendDetailText(),
            bodyFont,
            std::max(0, innerWidth - (groupTextSide * 2)),
            scaledPixels(52),
            scaledPixels(96));
        const int statusHeadlineHeight = measureMultilineTextHeight(
            statusHeadline,
            currentStatusHeadline(),
            sectionFont,
            std::max(0, innerWidth - (groupTextSide * 2)),
            scaledPixels(42),
            scaledPixels(76));
        const int statusDetailHeight = measureMultilineTextHeight(
            statusDetail,
            currentStatusDetail(),
            bodyFont,
            std::max(0, innerWidth - (groupTextSide * 2)),
            scaledPixels(88),
            scaledPixels(136));
        const int provenanceTopHeight = measureMultilineTextHeight(
            dataQuality,
            buildDataQualityText(viewModel),
            bodyFont,
            std::max(0, innerWidth - (groupTextSide * 2)),
            scaledPixels(60),
            scaledPixels(110));
        const int gapSummaryHeight = measureMultilineTextHeight(
            gapSummary,
            buildGapSummaryText(viewModel),
            bodyFont,
            std::max(0, innerWidth - (groupTextSide * 2)),
            scaledPixels(80),
            scaledPixels(126));

        const int listTopInset = groupTextTop + scaledPixels(12);
        const int listSideInset = scaledPixels(10);

        if (singleColumnContent) {
            const int fullWidth = innerWidth;
            const int fullTextWidth = std::max(0, fullWidth - (groupTextSide * 2));
            const int preferredSummaryHeight = std::max(
                scaledPixels(360),
                headlineHeight + (overviewCellHeight * 3) + groupTextTop + scaledPixels(44));
            const int preferredTrendHeight = std::max(
                scaledPixels(286),
                (showQuotaSeriesSelector ? rowHeight + scaledPixels(8) : 0) +
                    trendSummaryHeight + scaledPixels(6) +
                    trendComparisonHeight + scaledPixels(6) +
                    trendChartHeight + scaledPixels(6) +
                    trendDetailHeight + groupTextTop + scaledPixels(24));
            const int preferredStatusHeight = std::max(
                scaledPixels(148),
                statusHeadlineHeight + statusDetailHeight + groupTextTop + scaledPixels(24));
            const int preferredProviderHeight = scaledPixels(156);
            const int preferredAccountHeight = scaledPixels(156);
            const int preferredDailyHeight = scaledPixels(196);
            const int preferredProvenanceHeight = std::max(
                scaledPixels(188),
                provenanceTopHeight + gapSummaryHeight + groupTextTop + scaledPixels(24));

            std::vector<int> heights = {
                preferredSummaryHeight,
                preferredTrendHeight,
                preferredStatusHeight,
                preferredProviderHeight,
                preferredAccountHeight,
                preferredDailyHeight,
                preferredProvenanceHeight
            };
            int currentTop = contentTop;
            auto placeFullWidthGroup = [&](HWND group, int height) -> RECT {
                RECT rect{innerLeft, currentTop, innerLeft + fullWidth, currentTop + height};
                MoveWindow(group, rect.left, rect.top, fullWidth, height, TRUE);
                currentTop += height + groupGap;
                return rect;
            };

            const RECT summaryRect = placeFullWidthGroup(summaryGroup, heights[0]);
            const RECT trendRect = placeFullWidthGroup(trendGroup, heights[1]);
            const RECT statusRect = placeFullWidthGroup(statusGroup, heights[2]);
            const RECT providerRect = placeFullWidthGroup(providerGroup, heights[3]);
            const RECT accountRect = placeFullWidthGroup(accountGroup, heights[4]);
            const RECT dailyRect = placeFullWidthGroup(dailyGroup, heights[5]);
            const RECT provenanceRect = placeFullWidthGroup(provenanceGroup, heights[6]);

            const int summaryHeadlineSpace = std::max(0, heights[0] - groupTextTop - scaledPixels(18));
            const int summaryHeadlineDrawHeight = std::min(headlineHeight, summaryHeadlineSpace);
            MoveWindow(summaryHeadline,
                       summaryRect.left + groupTextSide,
                       summaryRect.top + groupTextTop,
                       fullTextWidth,
                       summaryHeadlineDrawHeight,
                       TRUE);
            const int summaryCellTop = summaryRect.top + groupTextTop + summaryHeadlineDrawHeight + scaledPixels(8);
            MoveWindow(summaryCoverageCell,
                       summaryRect.left + groupTextSide,
                       summaryCellTop,
                       fullTextWidth,
                       overviewCellHeight,
                       TRUE);
            MoveWindow(summaryBreakdownsCell,
                       summaryRect.left + groupTextSide,
                       summaryCellTop + overviewCellHeight + scaledPixels(8),
                       fullTextWidth,
                       overviewCellHeight,
                       TRUE);
            MoveWindow(summaryThroughputCell,
                       summaryRect.left + groupTextSide,
                       summaryCellTop + ((overviewCellHeight + scaledPixels(8)) * 2),
                       fullTextWidth,
                       overviewCellHeight,
                       TRUE);

            const int trendComboTop = trendRect.top + groupTextTop;
            if (showQuotaSeriesSelector) {
                const int quotaLabelWidth = std::min(scaledPixels(104), fullTextWidth);
                MoveWindow(quotaSeriesCombo,
                           trendRect.left + groupTextSide + quotaLabelWidth + scaledPixels(6),
                           trendComboTop - scaledPixels(2),
                           std::max(
                               scaledPixels(150),
                               std::min(
                                   scaledPixels(280),
                                   fullTextWidth - quotaLabelWidth - scaledPixels(6))),
                           compactComboHeight,
                           TRUE);
                MoveWindow(quotaSeriesLabel,
                           trendRect.left + groupTextSide,
                           trendComboTop,
                           quotaLabelWidth,
                           singleLineHeight,
                           TRUE);
            }
            const int trendContentTop =
                trendComboTop + (showQuotaSeriesSelector ? rowHeight + scaledPixels(6) : 0);
            const int trendAvailableHeight = std::max(
                0,
                heights[1] - groupTextTop - scaledPixels(18) -
                    (showQuotaSeriesSelector ? rowHeight + scaledPixels(8) : 0));
            const int fixedTrendHeight =
                trendSummaryHeight + scaledPixels(6) +
                trendComparisonHeight + scaledPixels(6) +
                trendDetailHeight + scaledPixels(6);
            const int trendChartDrawHeight = std::max(
                scaledPixels(110),
                trendAvailableHeight - fixedTrendHeight);
            MoveWindow(
                trendSummary,
                trendRect.left + groupTextSide,
                trendContentTop,
                fullTextWidth,
                trendSummaryHeight,
                TRUE);
            MoveWindow(
                trendComparison,
                trendRect.left + groupTextSide,
                trendContentTop + trendSummaryHeight + scaledPixels(6),
                fullTextWidth,
                trendComparisonHeight,
                TRUE);
            MoveWindow(
                trendChart,
                trendRect.left + groupTextSide,
                trendContentTop + trendSummaryHeight + scaledPixels(6) +
                    trendComparisonHeight + scaledPixels(6),
                fullTextWidth,
                trendChartDrawHeight,
                TRUE);
            MoveWindow(
                trendDetail,
                trendRect.left + groupTextSide,
                trendContentTop + trendSummaryHeight + scaledPixels(6) +
                    trendComparisonHeight + scaledPixels(6) +
                    trendChartDrawHeight + scaledPixels(6),
                fullTextWidth,
                trendDetailHeight,
                TRUE);

            const int statusHeadlineSpace = std::max(0, heights[2] - groupTextTop - scaledPixels(18));
            const int statusHeadlineDrawHeight = std::min(statusHeadlineHeight, statusHeadlineSpace);
            MoveWindow(statusHeadline,
                       statusRect.left + groupTextSide,
                       statusRect.top + groupTextTop,
                       fullTextWidth,
                       statusHeadlineDrawHeight,
                       TRUE);
            MoveWindow(statusDetail,
                       statusRect.left + groupTextSide,
                       statusRect.top + groupTextTop + statusHeadlineDrawHeight + scaledPixels(6),
                       fullTextWidth,
                       std::max(0, heights[2] - groupTextTop - statusHeadlineDrawHeight - scaledPixels(18)),
                       TRUE);

            MoveWindow(providerList,
                       providerRect.left + listSideInset,
                       providerRect.top + listTopInset,
                       std::max(0, fullWidth - (listSideInset * 2)),
                       std::max(0, heights[3] - listTopInset - scaledPixels(10)),
                       TRUE);
            MoveWindow(accountList,
                       accountRect.left + listSideInset,
                       accountRect.top + listTopInset,
                       std::max(0, fullWidth - (listSideInset * 2)),
                       std::max(0, heights[4] - listTopInset - scaledPixels(10)),
                       TRUE);
            MoveWindow(dailyList,
                       dailyRect.left + listSideInset,
                       dailyRect.top + listTopInset,
                       std::max(0, fullWidth - (listSideInset * 2)),
                       std::max(0, heights[5] - listTopInset - scaledPixels(10)),
                       TRUE);

            const int provenanceHeadlineDrawHeight = std::min(
                provenanceTopHeight,
                std::max(0, heights[6] - groupTextTop - scaledPixels(18)));
            MoveWindow(dataQuality,
                       provenanceRect.left + groupTextSide,
                       provenanceRect.top + groupTextTop,
                       fullTextWidth,
                       provenanceHeadlineDrawHeight,
                       TRUE);
            MoveWindow(gapSummary,
                       provenanceRect.left + groupTextSide,
                       provenanceRect.top + groupTextTop + provenanceHeadlineDrawHeight + scaledPixels(6),
                       fullTextWidth,
                       std::max(0, heights[6] - groupTextTop - provenanceHeadlineDrawHeight - scaledPixels(18)),
                       TRUE);

            const int scale = dpiScale();
            resizeListColumns(providerList, kBreakdownColumns, std::size(kBreakdownColumns), scale);
            resizeListColumns(accountList, kBreakdownColumns, std::size(kBreakdownColumns), scale);
            resizeListColumns(dailyList, kDailyColumns, std::size(kDailyColumns), scale);
            MoveWindow(scopeGroup, bounds.left, bounds.top, totalWidth, std::min(scopeGroupHeight, totalHeight), TRUE);
            MoveWindow(scopeSummary, innerLeft, y, innerWidth, scopeSummaryHeight, TRUE);
            MoveWindow(
                workspaceGuide,
                innerLeft,
                y + scopeSummaryHeight + scaledPixels(8),
                innerWidth,
                workspaceGuideHeight,
                TRUE);
            updateVerticalScroll(totalHeight, static_cast<int>(provenanceRect.bottom - bounds.top) + padding);
            offsetControls(-scrollOffset);
            return;
        }

        const int topColumnWidth = std::max(0, (innerWidth - (groupGap * 2)) / 3);
        const int topRightWidth = std::max(0, innerWidth - (topColumnWidth * 2) - (groupGap * 2));
        const int splitWidth = std::max(0, (innerWidth - groupGap) / 2);
        const int dailyWidth = std::max(0, (innerWidth - groupGap) * 60 / 100);
        const int provenanceWidth = std::max(0, innerWidth - dailyWidth - groupGap);
        const int summaryTrendWidth = compactTopLayout
            ? std::max(0, (innerWidth - groupGap) / 2)
            : topColumnWidth;
        const int statusBandWidth = compactTopLayout ? innerWidth : topRightWidth;
        const int provenanceTextWidth = std::max(0, provenanceWidth - (groupTextSide * 2));

        const bool stackSummaryCellsForBand =
            std::max(0, summaryTrendWidth - (groupTextSide * 2)) < scaledPixels(560);
        const int summaryBandHeight = std::max(
            stackSummaryCellsForBand ? scaledPixels(350) : scaledPixels(206),
            headlineHeight +
                (stackSummaryCellsForBand ? (scaledPixels(84) * 3) + scaledPixels(16) : overviewCellHeight) +
                groupTextTop + scaledPixels(30));
        const int trendBandHeight = std::max(
            scaledPixels(286),
            (showQuotaSeriesSelector ? rowHeight + scaledPixels(8) : 0) +
                trendSummaryHeight + scaledPixels(6) +
                trendComparisonHeight + scaledPixels(6) +
                trendChartHeight + scaledPixels(6) +
                trendDetailHeight + groupTextTop + scaledPixels(24));
        const int statusBandHeight = std::max(
            scaledPixels(156),
            statusHeadlineHeight + statusDetailHeight + groupTextTop + scaledPixels(24));
        const int preferredTopRowHeight = std::max(summaryBandHeight, trendBandHeight);
        const int preferredBottomBandHeight = std::max(
            scaledPixels(196),
            provenanceTopHeight + gapSummaryHeight + groupTextTop + scaledPixels(24));
        int topRowHeight = preferredTopRowHeight;
        int middleBandHeight = scaledPixels(192);
        int bottomBandHeight = preferredBottomBandHeight;
        int statusRowHeight = compactTopLayout ? statusBandHeight : 0;
        if (compactTopLayout) {
            topRowHeight = preferredTopRowHeight;
            statusRowHeight = statusBandHeight;
        } else {
            topRowHeight = std::max({summaryBandHeight, trendBandHeight, statusBandHeight});
        }

        const int statusTop = compactTopLayout ? contentTop + topRowHeight + groupGap : contentTop;
        MoveWindow(summaryGroup, innerLeft, contentTop, summaryTrendWidth, topRowHeight, TRUE);
        MoveWindow(
            trendGroup,
            compactTopLayout ? innerLeft + summaryTrendWidth + groupGap : innerLeft + topColumnWidth + groupGap,
            contentTop,
            compactTopLayout ? std::max(0, innerWidth - summaryTrendWidth - groupGap) : topColumnWidth,
            topRowHeight,
            TRUE);
        MoveWindow(
            statusGroup,
            compactTopLayout ? innerLeft : innerLeft + (topColumnWidth * 2) + (groupGap * 2),
            statusTop,
            statusBandWidth,
            compactTopLayout ? statusRowHeight : topRowHeight,
            TRUE);

        RECT summaryRect{
            innerLeft,
            contentTop,
            innerLeft + summaryTrendWidth,
            contentTop + topRowHeight
        };
        RECT trendRect{
            compactTopLayout ? innerLeft + summaryTrendWidth + groupGap : innerLeft + topColumnWidth + groupGap,
            contentTop,
            compactTopLayout
                ? innerLeft + innerWidth
                : innerLeft + topColumnWidth + groupGap + topColumnWidth,
            contentTop + topRowHeight
        };
        RECT statusRect{
            compactTopLayout ? innerLeft : innerLeft + (topColumnWidth * 2) + (groupGap * 2),
            statusTop,
            compactTopLayout
                ? innerLeft + innerWidth
                : innerLeft + (topColumnWidth * 2) + (groupGap * 2) + topRightWidth,
            statusTop + (compactTopLayout ? statusRowHeight : topRowHeight)
        };

        const int summaryContentWidth = std::max<int>(0, static_cast<int>(summaryRect.right - summaryRect.left) - (groupTextSide * 2));
        const int trendContentWidth = std::max<int>(0, static_cast<int>(trendRect.right - trendRect.left) - (groupTextSide * 2));
        const int statusContentWidth = std::max<int>(0, static_cast<int>(statusRect.right - statusRect.left) - (groupTextSide * 2));
        const int summaryHeadlineDrawHeight = std::min(
            measureMultilineTextHeight(summaryHeadline, buildOverviewHeadline(viewModel), sectionFont, summaryContentWidth, scaledPixels(24), scaledPixels(74)),
            std::max(0, static_cast<int>(summaryRect.bottom - summaryRect.top) - groupTextTop - scaledPixels(18)));
        MoveWindow(summaryHeadline,
                   summaryRect.left + groupTextSide,
                   summaryRect.top + groupTextTop,
                   summaryContentWidth,
                   summaryHeadlineDrawHeight,
                   TRUE);
        const int summaryCellTop = summaryRect.top + groupTextTop + summaryHeadlineDrawHeight + scaledPixels(8);
        const bool stackSummaryCells = summaryContentWidth < scaledPixels(560);
        if (stackSummaryCells) {
            const int stackedCellHeight = scaledPixels(84);
            MoveWindow(summaryCoverageCell,
                       summaryRect.left + groupTextSide,
                       summaryCellTop,
                       summaryContentWidth,
                       stackedCellHeight,
                       TRUE);
            MoveWindow(summaryBreakdownsCell,
                       summaryRect.left + groupTextSide,
                       summaryCellTop + stackedCellHeight + scaledPixels(8),
                       summaryContentWidth,
                       stackedCellHeight,
                       TRUE);
            MoveWindow(summaryThroughputCell,
                       summaryRect.left + groupTextSide,
                       summaryCellTop + ((stackedCellHeight + scaledPixels(8)) * 2),
                       summaryContentWidth,
                       stackedCellHeight,
                       TRUE);
        } else {
            const int cellGap = scaledPixels(10);
            const int firstCellWidth = std::max(0, (summaryContentWidth - (cellGap * 2)) / 3);
            const int secondCellLeft = summaryRect.left + groupTextSide + firstCellWidth + cellGap;
            const int thirdCellLeft = secondCellLeft + firstCellWidth + cellGap;
            MoveWindow(summaryCoverageCell,
                       summaryRect.left + groupTextSide,
                       summaryCellTop,
                       firstCellWidth,
                       overviewCellHeight,
                       TRUE);
            MoveWindow(summaryBreakdownsCell,
                       secondCellLeft,
                       summaryCellTop,
                       firstCellWidth,
                       overviewCellHeight,
                       TRUE);
            MoveWindow(summaryThroughputCell,
                       thirdCellLeft,
                       summaryCellTop,
                       summaryRect.left + groupTextSide + summaryContentWidth - thirdCellLeft,
                       overviewCellHeight,
                       TRUE);
        }

        const int trendComboTop = trendRect.top + groupTextTop;
        if (showQuotaSeriesSelector) {
            const int quotaLabelWidth = std::min(scaledPixels(92), trendContentWidth);
            MoveWindow(quotaSeriesCombo,
                       trendRect.left + groupTextSide + quotaLabelWidth + scaledPixels(6),
                       trendComboTop - scaledPixels(2),
                       std::max(
                           scaledPixels(140),
                           std::min(
                               scaledPixels(280),
                               trendContentWidth - quotaLabelWidth - scaledPixels(6))),
                       compactComboHeight,
                       TRUE);
            MoveWindow(quotaSeriesLabel,
                       trendRect.left + groupTextSide,
                       trendComboTop,
                       quotaLabelWidth,
                       singleLineHeight,
                       TRUE);
        }
        const int trendContentTop =
            trendComboTop + (showQuotaSeriesSelector ? rowHeight + scaledPixels(6) : 0);
        const int trendAvailableHeight = std::max(
            0,
            static_cast<int>(trendRect.bottom - trendRect.top) - groupTextTop - scaledPixels(18) -
                (showQuotaSeriesSelector ? rowHeight + scaledPixels(8) : 0));
        const int fixedTrendHeight =
            trendSummaryHeight + scaledPixels(6) +
            trendComparisonHeight + scaledPixels(6) +
            trendDetailHeight + scaledPixels(6);
        const int trendChartDrawHeight = std::max(
            scaledPixels(110),
            trendAvailableHeight - fixedTrendHeight);
        MoveWindow(trendSummary,
                   trendRect.left + groupTextSide,
                   trendContentTop,
                   trendContentWidth,
                   trendSummaryHeight,
                   TRUE);
        MoveWindow(trendComparison,
                   trendRect.left + groupTextSide,
                   trendContentTop + trendSummaryHeight + scaledPixels(6),
                   trendContentWidth,
                   trendComparisonHeight,
                   TRUE);
        MoveWindow(trendChart,
                   trendRect.left + groupTextSide,
                   trendContentTop + trendSummaryHeight + scaledPixels(6) +
                       trendComparisonHeight + scaledPixels(6),
                   trendContentWidth,
                   trendChartDrawHeight,
                   TRUE);
        MoveWindow(trendDetail,
                   trendRect.left + groupTextSide,
                   trendContentTop + trendSummaryHeight + scaledPixels(6) +
                       trendComparisonHeight + scaledPixels(6) +
                       trendChartDrawHeight + scaledPixels(6),
                   trendContentWidth,
                   trendDetailHeight,
                   TRUE);
        const int statusHeadlineDrawHeight = std::min(
            measureMultilineTextHeight(statusHeadline, currentStatusHeadline(), sectionFont, statusContentWidth, scaledPixels(24), scaledPixels(76)),
            std::max(0, static_cast<int>(statusRect.bottom - statusRect.top) - groupTextTop - scaledPixels(18)));
        MoveWindow(statusHeadline,
                   statusRect.left + groupTextSide,
                   statusRect.top + groupTextTop,
                   statusContentWidth,
                   statusHeadlineDrawHeight,
                   TRUE);
        MoveWindow(statusDetail,
                   statusRect.left + groupTextSide,
                   statusRect.top + groupTextTop + statusHeadlineDrawHeight + scaledPixels(6),
                   statusContentWidth,
                   std::max(
                       0,
                       static_cast<int>(statusRect.bottom - statusRect.top) - groupTextTop -
                           statusHeadlineDrawHeight - scaledPixels(18)),
                   TRUE);

        const int middleTop = compactTopLayout
            ? statusTop + statusRowHeight + groupGap
            : contentTop + topRowHeight + groupGap;
        MoveWindow(providerGroup, innerLeft, middleTop, splitWidth, middleBandHeight, TRUE);
        MoveWindow(accountGroup, innerLeft + splitWidth + groupGap, middleTop,
                   std::max(0, innerWidth - splitWidth - groupGap), middleBandHeight, TRUE);

        RECT providerRect{
            innerLeft,
            middleTop,
            innerLeft + splitWidth,
            middleTop + middleBandHeight
        };
        RECT accountRect{
            innerLeft + splitWidth + groupGap,
            middleTop,
            innerLeft + splitWidth + groupGap + std::max(0, innerWidth - splitWidth - groupGap),
            middleTop + middleBandHeight
        };

        MoveWindow(providerList,
                   providerRect.left + listSideInset,
                   providerRect.top + listTopInset,
                   std::max<int>(0, static_cast<int>(providerRect.right - providerRect.left) - (listSideInset * 2)),
                   std::max(0, middleBandHeight - listTopInset - scaledPixels(10)),
                   TRUE);
        MoveWindow(accountList,
                   accountRect.left + listSideInset,
                   accountRect.top + listTopInset,
                   std::max<int>(0, static_cast<int>(accountRect.right - accountRect.left) - (listSideInset * 2)),
                   std::max(0, middleBandHeight - listTopInset - scaledPixels(10)),
                   TRUE);

        const int bottomTop = middleTop + middleBandHeight + groupGap;

        MoveWindow(dailyGroup, innerLeft, bottomTop, dailyWidth, bottomBandHeight, TRUE);
        MoveWindow(provenanceGroup, innerLeft + dailyWidth + groupGap, bottomTop,
                   provenanceWidth, bottomBandHeight, TRUE);

        RECT dailyRect{
            innerLeft,
            bottomTop,
            innerLeft + dailyWidth,
            bottomTop + bottomBandHeight
        };
        RECT provenanceRect{
            innerLeft + dailyWidth + groupGap,
            bottomTop,
            innerLeft + dailyWidth + groupGap + provenanceWidth,
            bottomTop + bottomBandHeight
        };

        MoveWindow(dailyList,
                   dailyRect.left + listSideInset,
                   dailyRect.top + listTopInset,
                   std::max<int>(0, static_cast<int>(dailyRect.right - dailyRect.left) - (listSideInset * 2)),
                   std::max<int>(0, bottomBandHeight - listTopInset - scaledPixels(10)),
                   TRUE);
        MoveWindow(dataQuality,
                   provenanceRect.left + groupTextSide,
                   provenanceRect.top + groupTextTop,
                   provenanceTextWidth,
                   std::min(
                       measureMultilineTextHeight(dataQuality, buildDataQualityText(viewModel), bodyFont, provenanceTextWidth, scaledPixels(24), scaledPixels(110)),
                       std::max(0, bottomBandHeight - groupTextTop - scaledPixels(18))),
                   TRUE);
        const int dataQualityDrawHeight = std::min(
            measureMultilineTextHeight(dataQuality, buildDataQualityText(viewModel), bodyFont, provenanceTextWidth, scaledPixels(24), scaledPixels(110)),
            std::max(0, bottomBandHeight - groupTextTop - scaledPixels(18)));
        MoveWindow(gapSummary,
                   provenanceRect.left + groupTextSide,
                   provenanceRect.top + groupTextTop + dataQualityDrawHeight + scaledPixels(6),
                   provenanceTextWidth,
                   std::max(0, bottomBandHeight - groupTextTop - dataQualityDrawHeight - scaledPixels(16)),
                   TRUE);

        const int scale = dpiScale();
        resizeListColumns(providerList, kBreakdownColumns, std::size(kBreakdownColumns), scale);
        resizeListColumns(accountList, kBreakdownColumns, std::size(kBreakdownColumns), scale);
        resizeListColumns(dailyList, kDailyColumns, std::size(kDailyColumns), scale);
        MoveWindow(scopeGroup, bounds.left, bounds.top, totalWidth, std::min(scopeGroupHeight, totalHeight), TRUE);
        MoveWindow(scopeSummary, innerLeft, y, innerWidth, scopeSummaryHeight, TRUE);
        MoveWindow(
            workspaceGuide,
            innerLeft,
            y + scopeSummaryHeight + scaledPixels(8),
            innerWidth,
            workspaceGuideHeight,
            TRUE);
        updateVerticalScroll(totalHeight, static_cast<int>(provenanceRect.bottom - bounds.top) + padding);
        offsetControls(-scrollOffset);
    }

    bool isStyledTextCell(HWND control) const {
        return control == summaryCoverageCell ||
            control == summaryBreakdownsCell ||
            control == summaryThroughputCell ||
            control == scopeSummary ||
            control == workspaceGuide ||
            control == trendSummary ||
            control == trendComparison ||
            control == trendDetail ||
            control == statusHeadline ||
            control == statusDetail ||
            control == dataQuality ||
            control == gapSummary;
    }

    HBRUSH styledTextCellBrush(HWND control) const {
        if (control == summaryCoverageCell) {
            return overviewCoverageBrush;
        }
        if (control == summaryBreakdownsCell) {
            return overviewBreakdownsBrush;
        }
        if (control == summaryThroughputCell) {
            return overviewThroughputBrush;
        }
        if (control == scopeSummary || control == workspaceGuide) {
            return scopeBrush;
        }
        if (control == trendSummary || control == trendComparison || control == trendDetail) {
            return trendBrush;
        }
        if (control == statusHeadline || control == statusDetail) {
            return statusBrush;
        }
        if (control == dataQuality || control == gapSummary) {
            return provenanceBrush;
        }
        return reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    }

    COLORREF styledTextCellTextColor(HWND control) const {
        if (control == summaryCoverageCell) {
            return RGB(24, 92, 169);
        }
        if (control == summaryBreakdownsCell) {
            return RGB(36, 122, 72);
        }
        if (control == summaryThroughputCell) {
            return RGB(142, 93, 0);
        }
        if (control == trendSummary || control == trendComparison || control == trendDetail) {
            return RGB(24, 92, 169);
        }
        if (control == statusHeadline || control == statusDetail) {
            return RGB(142, 93, 0);
        }
        if (control == dataQuality || control == gapSummary) {
            return RGB(36, 122, 72);
        }
        if (control == scopeSummary || control == workspaceGuide) {
            return RGB(60, 66, 76);
        }
        return GetSysColor(COLOR_WINDOWTEXT);
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

    bool handleDrawItem(const DRAWITEMSTRUCT& draw) {
        if (!isStyledTextCell(draw.hwndItem)) {
            return false;
        }

        HDC dc = draw.hDC;
        RECT rect = draw.rcItem;
        FillRect(dc, &rect, styledTextCellBrush(draw.hwndItem));

        const int length = GetWindowTextLengthW(draw.hwndItem);
        std::wstring text(static_cast<size_t>(length + 1), L'\0');
        if (length > 0) {
            GetWindowTextW(draw.hwndItem, text.data(), length + 1);
        }
        text.resize(static_cast<size_t>(length));

        RECT contentRect = rect;
        InflateRect(&contentRect, -scaledPixels(8), -scaledPixels(5));
        const int oldBkMode = SetBkMode(dc, TRANSPARENT);
        const COLORREF oldColor = SetTextColor(dc, styledTextCellTextColor(draw.hwndItem));
        HGDIOBJ oldFont = SelectObject(dc, bodyFont != nullptr ? bodyFont : GetStockObject(DEFAULT_GUI_FONT));

        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        const int lineHeight = metrics.tmHeight + scaledPixels(4);
        int y = contentRect.top;
        const auto lines = splitOwnerDrawLines(text);
        for (size_t index = 0; index < lines.size() && y < contentRect.bottom; ++index) {
            const std::wstring& line = lines[index];
            RECT lineRect{
                contentRect.left,
                static_cast<LONG>(y),
                contentRect.right,
                std::min<LONG>(contentRect.bottom, static_cast<LONG>(y + lineHeight + scaledPixels(2)))
            };
            if (index == 0 && (draw.hwndItem == summaryCoverageCell ||
                               draw.hwndItem == summaryBreakdownsCell ||
                               draw.hwndItem == summaryThroughputCell)) {
                SelectObject(dc, bodyBoldFont != nullptr ? bodyBoldFont : sectionFont);
                DrawTextW(dc, line.c_str(), -1, &lineRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            } else if (line.rfind(L"Source:", 0) == 0) {
                SelectObject(dc, bodyItalicFont != nullptr ? bodyItalicFont : bodyFont);
                DrawTextW(dc, line.c_str(), -1, &lineRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            } else {
                const size_t colon = line.find(L':');
                if (colon != std::wstring::npos) {
                    const std::wstring label = line.substr(0, colon + 1);
                    const std::wstring value = line.substr(colon + 1);
                    SelectObject(dc, bodyBoldFont != nullptr ? bodyBoldFont : sectionFont);
                    TextOutW(dc, lineRect.left, lineRect.top, label.c_str(), static_cast<int>(label.size()));
                    SIZE labelSize{};
                    GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &labelSize);
                    RECT valueRect = lineRect;
                    valueRect.left = std::min(valueRect.right, valueRect.left + labelSize.cx + scaledPixels(3));
                    SelectObject(dc, bodyFont != nullptr ? bodyFont : GetStockObject(DEFAULT_GUI_FONT));
                    DrawTextW(dc, value.c_str(), -1, &valueRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                } else {
                    SelectObject(dc, bodyFont != nullptr ? bodyFont : GetStockObject(DEFAULT_GUI_FONT));
                    DrawTextW(dc, line.c_str(), -1, &lineRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
            }
            y += lineHeight;
        }

        if (oldFont != nullptr) {
            SelectObject(dc, oldFont);
        }
        SetTextColor(dc, oldColor);
        SetBkMode(dc, oldBkMode);
        return true;
    }

    bool handleCommand(WORD id) {
        return handleCommand(id, 0);
    }

    bool jumpToSection(const std::string& sectionKey) {
        if (!hasLastLayoutBounds || parent == nullptr) {
            return false;
        }

        HWND section = nullptr;
        if (sectionKey == "summary") {
            section = summaryGroup;
        } else if (sectionKey == "trend") {
            section = trendGroup;
        } else if (sectionKey == "status") {
            section = statusGroup;
        } else if (sectionKey == "provider") {
            section = providerGroup;
        } else if (sectionKey == "account") {
            section = accountGroup;
        } else if (sectionKey == "daily") {
            section = dailyGroup;
        } else if (sectionKey == "provenance") {
            section = provenanceGroup;
        } else {
            return false;
        }

        const RECT sectionRect = windowRectRelativeToParent(parent, section);
        const int viewportMargin = scaledPixels(8);
        const int contentTop = sectionRect.top + scrollOffset;
        return setScrollOffset(contentTop - lastLayoutBounds.top - viewportMargin);
    }

    bool handleCommand(WORD id, WORD notifyCode) {
        if (id == kProviderComboId) {
            onProviderChanged();
            return true;
        }
        if (id == kAccountComboId) {
            onAccountChanged();
            return true;
        }
        if (id == kRangeComboId) {
            onRangeChanged();
            return true;
        }
        if (id == kQuotaSeriesComboId) {
            onQuotaSeriesChanged();
            return true;
        }
        if (id == kCustomStartEditId || id == kCustomEndEditId) {
            onCustomDateChanged(notifyCode);
            return true;
        }
        if (id == kNavOverviewButtonId) {
            return jumpToSection("summary");
        }
        if (id == kNavTrendButtonId) {
            return jumpToSection("trend");
        }
        if (id == kNavOperationsButtonId) {
            return jumpToSection("status");
        }
        if (id == kNavBreakdownsButtonId) {
            return jumpToSection("provider");
        }
        if (id == kNavDailyButtonId) {
            return jumpToSection("daily");
        }
        if (id == kNavCoverageButtonId) {
            return jumpToSection("provenance");
        }
        return false;
    }

    bool handleTimer(UINT_PTR timerId) {
        if (timerId != kCustomDateDebounceTimerId) {
            return false;
        }
        if (parent != nullptr) {
            KillTimer(parent, kCustomDateDebounceTimerId);
        }
        commitCustomDateChange();
        return true;
    }

    bool handleVScroll(WORD request, WORD thumbPosition) {
        if (parent == nullptr || contentHeight <= viewportHeight) {
            return false;
        }

        const int lineStep = scaledPixels(48);
        const int pageStep = std::max(lineStep * 2, viewportHeight - scaledPixels(72));
        int targetOffset = scrollOffset;
        switch (request) {
        case SB_LINEUP:
            targetOffset -= lineStep;
            break;
        case SB_LINEDOWN:
            targetOffset += lineStep;
            break;
        case SB_PAGEUP:
            targetOffset -= pageStep;
            break;
        case SB_PAGEDOWN:
            targetOffset += pageStep;
            break;
        case SB_TOP:
            targetOffset = 0;
            break;
        case SB_BOTTOM:
            targetOffset = maxScrollOffset();
            break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
            targetOffset = thumbPosition;
            break;
        default:
            return false;
        }

        return setScrollOffset(targetOffset);
    }

    bool handleMouseWheel(short wheelDelta) {
        if (parent == nullptr || contentHeight <= viewportHeight || wheelDelta == 0) {
            return false;
        }

        const int step = scaledPixels(72);
        const int notches = wheelDelta / WHEEL_DELTA;
        if (notches == 0) {
            return false;
        }

        return setScrollOffset(scrollOffset - (notches * step));
    }

    void onProviderChanged() {
        if (suppressNotification || providerCombo == nullptr) return;
        const LRESULT sel = SendMessageW(providerCombo, CB_GETCURSEL, 0, 0);
        if (sel < 0 || sel >= static_cast<LRESULT>(viewModel.availableProviders.size())) return;
        AnalysisScopeState state = currentScope;
        state.selectedProviderId = viewModel.availableProviders[static_cast<size_t>(sel)].key;
        if (!state.selectedAccountId.empty()) {
            state.selectedAccountId.clear();
            if (accountCombo != nullptr) {
                SendMessageW(accountCombo, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
            }
        }
        fireScopeChanged(normalizeScope(state));
    }

    void onAccountChanged() {
        if (suppressNotification || accountCombo == nullptr) return;
        const LRESULT sel = SendMessageW(accountCombo, CB_GETCURSEL, 0, 0);
        AnalysisScopeState state = currentScope;
        if (sel >= 0 && sel < static_cast<LRESULT>(viewModel.availableAccounts.size())) {
            state.selectedAccountId = viewModel.availableAccounts[static_cast<size_t>(sel)].key;
        } else {
            state.selectedAccountId.clear();
        }
        fireScopeChanged(normalizeScope(state));
    }

    void onRangeChanged() {
        if (suppressNotification || rangeCombo == nullptr) return;
        const LRESULT sel = SendMessageW(rangeCombo, CB_GETCURSEL, 0, 0);
        if (sel < 0 || sel >= static_cast<LRESULT>(std::size(presetKeys))) return;
        AnalysisScopeState state = currentScope;
        state.dateRangePreset = presetFromKey(presetKeys[static_cast<size_t>(sel)]);
        if (state.dateRangePreset == AnalysisDateRangePreset::Custom) {
            if (const auto startOpt = parseReportDay(getWindowTextUtf8(customStartEdit))) {
                state.customRangeStartUtc = *startOpt;
            }
            if (const auto endOpt = parseReportDay(getWindowTextUtf8(customEndEdit))) {
                state.customRangeEndUtc = *endOpt;
            }
        }
        applyCustomDateEnabledState(state.dateRangePreset == AnalysisDateRangePreset::Custom);
        fireScopeChanged(normalizeScope(state));
    }

    void onQuotaSeriesChanged() {
        if (suppressNotification || quotaSeriesCombo == nullptr) {
            return;
        }

        const LRESULT sel = SendMessageW(quotaSeriesCombo, CB_GETCURSEL, 0, 0);
        if (sel < 0 || sel >= static_cast<LRESULT>(viewModel.quotaTrendSeries.size())) {
            return;
        }

        selectedQuotaSeriesKey = viewModel.quotaTrendSeries[static_cast<size_t>(sel)].key;
        syncDefaultTrendSelection();
        setWindowTextUtf8(trendSummary, currentTrendSummaryText());
        setWindowTextUtf8(trendComparison, currentTrendComparisonText());
        setWindowTextUtf8(trendDetail, currentTrendDetailText());
        if (parent != nullptr) {
            RECT bounds{};
            if (GetClientRect(parent, &bounds) != 0) {
                layout(bounds);
            }
        }
        if (trendChart != nullptr) {
            InvalidateRect(trendChart, nullptr, TRUE);
        }
    }

    void onCustomDateChanged(WORD notifyCode) {
        if (suppressNotification) return;
        applyCustomDateEditingState();
        if (notifyCode == EN_CHANGE) {
            if (parent != nullptr) {
                SetTimer(parent, kCustomDateDebounceTimerId, 350, nullptr);
            }
            return;
        }
        if (notifyCode == EN_KILLFOCUS && parent != nullptr) {
            KillTimer(parent, kCustomDateDebounceTimerId);
        }
        commitCustomDateChange();
    }

    void commitCustomDateChange() {
        AnalysisScopeState state = currentScope;
        state.dateRangePreset = AnalysisDateRangePreset::Custom;
        const auto startOpt = parseReportDay(getWindowTextUtf8(customStartEdit));
        const auto endOpt = parseReportDay(getWindowTextUtf8(customEndEdit));
        if (startOpt.has_value()) {
            state.customRangeStartUtc = *startOpt;
        }
        if (endOpt.has_value()) {
            state.customRangeEndUtc = *endOpt;
        }
        fireScopeChanged(normalizeScope(state));
    }

    void applyCustomDateEditingState() {
        if (rangeCombo != nullptr) {
            SendMessageW(rangeCombo, CB_SETCURSEL, static_cast<WPARAM>(std::size(presetKeys) - 1), 0);
        }
        applyCustomDateEnabledState(true);
    }

    void fireScopeChanged(const AnalysisScopeState& state) {
        if (state.selectedProviderId == currentScope.selectedProviderId &&
            state.selectedAccountId == currentScope.selectedAccountId &&
            state.dateRangePreset == currentScope.dateRangePreset &&
            state.customRangeStartUtc == currentScope.customRangeStartUtc &&
            state.customRangeEndUtc == currentScope.customRangeEndUtc) {
            return;
        }

        currentScope = state;
        if (scopeChangedCallback) {
            scopeChangedCallback(state);
        }
    }

    void setInteractionPending(bool pending) {
        if (interactionPending == pending) {
            return;
        }
        interactionPending = pending;
        setWindowTextUtf8(statusHeadline, currentStatusHeadline());
        setWindowTextUtf8(statusDetail, currentStatusDetail());
        applyScopeInputEnabledState();
        if (parent != nullptr) {
            RECT bounds{};
            if (GetClientRect(parent, &bounds) != 0) {
                layout(bounds);
            }
        }
    }

    std::string currentStatusHeadline() const {
        return interactionPending
            ? buildPendingStatusHeadline()
            : buildOperationalStatusHeadline(viewModel);
    }

    std::string currentStatusDetail() const {
        return interactionPending
            ? buildPendingStatusDetail()
            : buildOperationalStatusDetail(viewModel);
    }

private:
    void applyScopeInputEnabledState() const {
        const bool enableScopeInputs = !interactionPending;
        if (providerCombo != nullptr) {
            EnableWindow(providerCombo, enableScopeInputs ? TRUE : FALSE);
        }
        if (accountCombo != nullptr) {
            EnableWindow(accountCombo, enableScopeInputs ? TRUE : FALSE);
        }
        if (rangeCombo != nullptr) {
            EnableWindow(rangeCombo, enableScopeInputs ? TRUE : FALSE);
        }
        applyCustomDateEnabledState(currentScope.dateRangePreset == AnalysisDateRangePreset::Custom);
    }

    static void destroyWindow(HWND& hwnd) {
        if (hwnd != nullptr) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
    }

    void initializeCommonControls() const {
        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_WIN95_CLASSES;
        InitCommonControlsEx(&icc);
    }

    HWND createListView(HINSTANCE instance) const {
        return CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_LISTVIEWW,
            L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 0, 0,
            parent,
            nullptr,
            instance,
            nullptr);
    }

    void configureListView(HWND list, const ListColumnDefinition* columns, size_t count) {
        if (list == nullptr) {
            return;
        }

        ListView_SetExtendedListViewStyle(
            list,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        initializeListColumns(list, columns, count, dpiScale());

        HWND header = ListView_GetHeader(list);
        if (header != nullptr && bodyFont != nullptr) {
            SendMessageW(header, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont), TRUE);
        }
    }

    void ensureFonts() {
        if (bodyFont != nullptr &&
            sectionFont != nullptr &&
            bodyBoldFont != nullptr &&
            bodyItalicFont != nullptr &&
            monoFont != nullptr &&
            overviewCoverageBrush != nullptr &&
            overviewBreakdownsBrush != nullptr &&
            overviewThroughputBrush != nullptr &&
            scopeBrush != nullptr &&
            trendBrush != nullptr &&
            statusBrush != nullptr &&
            provenanceBrush != nullptr) {
            return;
        }

        if (overviewCoverageBrush == nullptr) {
            overviewCoverageBrush = CreateSolidBrush(RGB(235, 244, 255));
        }
        if (overviewBreakdownsBrush == nullptr) {
            overviewBreakdownsBrush = CreateSolidBrush(RGB(235, 248, 240));
        }
        if (overviewThroughputBrush == nullptr) {
            overviewThroughputBrush = CreateSolidBrush(RGB(255, 247, 226));
        }
        if (scopeBrush == nullptr) {
            scopeBrush = CreateSolidBrush(RGB(245, 247, 250));
        }
        if (trendBrush == nullptr) {
            trendBrush = CreateSolidBrush(RGB(235, 244, 255));
        }
        if (statusBrush == nullptr) {
            statusBrush = CreateSolidBrush(RGB(255, 247, 226));
        }
        if (provenanceBrush == nullptr) {
            provenanceBrush = CreateSolidBrush(RGB(235, 248, 240));
        }

        LOGFONTW bodyLogFont{};
        bodyLogFont.lfHeight = -12;
        bodyLogFont.lfWeight = FW_NORMAL;
        bodyLogFont.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(bodyLogFont.lfFaceName, L"Segoe UI");
        bodyFont = CreateFontIndirectW(&bodyLogFont);
        if (bodyFont == nullptr) {
            bodyFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        }

        LOGFONTW sectionLogFont{};
        sectionLogFont.lfHeight = -15;
        sectionLogFont.lfWeight = FW_SEMIBOLD;
        sectionLogFont.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(sectionLogFont.lfFaceName, L"Segoe UI Semibold");
        sectionFont = CreateFontIndirectW(&sectionLogFont);
        if (sectionFont == nullptr) {
            sectionFont = bodyFont;
        }

        LOGFONTW boldLogFont = bodyLogFont;
        boldLogFont.lfWeight = FW_SEMIBOLD;
        wcscpy_s(boldLogFont.lfFaceName, L"Segoe UI Semibold");
        bodyBoldFont = CreateFontIndirectW(&boldLogFont);
        if (bodyBoldFont == nullptr) {
            bodyBoldFont = sectionFont;
        }

        LOGFONTW italicLogFont = bodyLogFont;
        italicLogFont.lfItalic = TRUE;
        bodyItalicFont = CreateFontIndirectW(&italicLogFont);
        if (bodyItalicFont == nullptr) {
            bodyItalicFont = bodyFont;
        }

        LOGFONTW monoLogFont{};
        monoLogFont.lfHeight = -12;
        monoLogFont.lfWeight = FW_NORMAL;
        monoLogFont.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(monoLogFont.lfFaceName, L"Consolas");
        monoFont = CreateFontIndirectW(&monoLogFont);
        if (monoFont == nullptr) {
            monoFont = bodyFont;
        }
    }

    void releaseFont(HFONT& font) {
        if (font != nullptr &&
            font != reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)) &&
            font != bodyFont &&
            font != sectionFont &&
            font != monoFont) {
            DeleteObject(font);
        }
        font = nullptr;
    }

    void releaseFonts() {
        HFONT ownedBody = bodyFont;
        HFONT ownedSection = sectionFont;
        HFONT ownedBold = bodyBoldFont;
        HFONT ownedItalic = bodyItalicFont;
        HFONT ownedMono = monoFont;

        bodyFont = nullptr;
        sectionFont = nullptr;
        bodyBoldFont = nullptr;
        bodyItalicFont = nullptr;
        monoFont = nullptr;

        if (ownedMono != nullptr &&
            ownedMono != ownedBody &&
            ownedMono != ownedSection &&
            ownedMono != ownedBold &&
            ownedMono != ownedItalic &&
            ownedMono != reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT))) {
            DeleteObject(ownedMono);
        }
        if (ownedItalic != nullptr &&
            ownedItalic != ownedBody &&
            ownedItalic != ownedSection &&
            ownedItalic != ownedBold &&
            ownedItalic != reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT))) {
            DeleteObject(ownedItalic);
        }
        if (ownedBold != nullptr &&
            ownedBold != ownedBody &&
            ownedBold != ownedSection &&
            ownedBold != reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT))) {
            DeleteObject(ownedBold);
        }
        if (ownedSection != nullptr &&
            ownedSection != ownedBody &&
            ownedSection != reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT))) {
            DeleteObject(ownedSection);
        }
        if (ownedBody != nullptr &&
            ownedBody != reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT))) {
            DeleteObject(ownedBody);
        }
    }

    void releaseBrush(HBRUSH& brush) {
        if (brush != nullptr) {
            DeleteObject(brush);
            brush = nullptr;
        }
    }

    void releaseBrushes() {
        releaseBrush(overviewCoverageBrush);
        releaseBrush(overviewBreakdownsBrush);
        releaseBrush(overviewThroughputBrush);
        releaseBrush(scopeBrush);
        releaseBrush(trendBrush);
        releaseBrush(statusBrush);
        releaseBrush(provenanceBrush);
    }

    void applyFont(HWND hwnd, HFONT font) const {
        if (hwnd != nullptr && font != nullptr) {
            SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
    }

    int measureMultilineTextHeight(
        HWND hwnd,
        const std::string& text,
        HFONT font,
        int width,
        int minimumHeight,
        int maximumHeight) const {
        if (hwnd == nullptr || width <= 0) {
            return minimumHeight;
        }

        HDC dc = GetDC(hwnd);
        if (dc == nullptr) {
            return minimumHeight;
        }

        HGDIOBJ previousFont = nullptr;
        if (font != nullptr) {
            previousFont = SelectObject(dc, font);
        }

        RECT textRect{0, 0, width, 0};
        std::wstring wideText = utf8ToWide(text.empty() ? " " : text);
        DrawTextW(dc,
                  wideText.c_str(),
                  -1,
                  &textRect,
                  DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_EDITCONTROL);

        if (previousFont != nullptr) {
            SelectObject(dc, previousFont);
        }
        ReleaseDC(hwnd, dc);

        const int measuredTextHeight = static_cast<int>(textRect.bottom - textRect.top);
        const int verticalPadding = (GetWindowLongPtrW(hwnd, GWL_STYLE) & SS_OWNERDRAW) != 0
            ? scaledPixels(14)
            : scaledPixels(4);
        const int measuredHeight = std::max(minimumHeight, measuredTextHeight + verticalPadding);
        return std::min(measuredHeight, maximumHeight);
    }

    void applyFonts() {
        const HWND bodyControls[] = {
            scopeGroup, providerLabel, providerCombo, accountLabel, accountCombo,
            rangeLabel, rangeCombo, customStartLabel, customStartEdit, customEndLabel, customEndEdit,
            customDateHint, scopeSummary, workspaceGuide,
            navOverviewButton, navTrendButton, navOperationsButton,
            navBreakdownsButton, navDailyButton, navCoverageButton,
            summaryGroup, summaryStats, summaryCoverageCell, summaryBreakdownsCell, summaryThroughputCell,
            trendGroup, quotaSeriesLabel, quotaSeriesCombo, trendSummary, trendComparison, trendDetail,
            statusGroup, statusDetail,
            providerGroup, providerList, accountGroup, accountList, dailyGroup, dailyList,
            provenanceGroup, dataQuality, gapSummary
        };
        for (HWND hwnd : bodyControls) {
            applyFont(hwnd, bodyFont);
        }
        applyFont(summaryHeadline, sectionFont);
        applyFont(statusHeadline, sectionFont);
        applyFont(trendChart, monoFont);
    }

    int dpiScale() const {
        UINT dpi = 96;
        if (parent != nullptr) {
            dpi = GetDpiForWindow(parent);
        }
        return static_cast<int>(dpi);
    }

    int scaledPixels(int value) const {
        return MulDiv(value, dpiScale(), 96);
    }

    int maxScrollOffset() const {
        return std::max(0, contentHeight - viewportHeight);
    }

    void updateVerticalScroll(int viewportHeightValue, int contentHeightValue) {
        viewportHeight = std::max(0, viewportHeightValue);
        contentHeight = std::max(viewportHeight, contentHeightValue);
        scrollOffset = std::clamp(scrollOffset, 0, maxScrollOffset());

        if (parent == nullptr) {
            return;
        }

        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        info.nMin = 0;
        info.nMax = std::max(0, contentHeight - 1);
        info.nPage = static_cast<UINT>(std::max(0, viewportHeight));
        info.nPos = scrollOffset;
        SetScrollInfo(parent, SB_VERT, &info, TRUE);
        ShowScrollBar(parent, SB_VERT, contentHeight > viewportHeight);
    }

    bool setScrollOffset(int newOffset) {
        if (!hasLastLayoutBounds) {
            return false;
        }

        const int clamped = std::clamp(newOffset, 0, maxScrollOffset());
        const int offset = scrollOffset - clamped;
        if (offset == 0) {
            return false;
        }

        scrollOffset = clamped;
        offsetControls(offset);
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_POS;
        info.nPos = scrollOffset;
        SetScrollInfo(parent, SB_VERT, &info, TRUE);
        return true;
    }

    void offsetControls(int offset) const {
        if (parent == nullptr || offset == 0) {
            return;
        }

        const HWND controls[] = {
            scopeGroup, providerLabel, providerCombo, accountLabel, accountCombo,
            rangeLabel, rangeCombo, customStartLabel, customStartEdit, customEndLabel, customEndEdit,
            customDateHint, scopeSummary, workspaceGuide,
            navOverviewButton, navTrendButton, navOperationsButton,
            navBreakdownsButton, navDailyButton, navCoverageButton,
            summaryGroup, summaryHeadline, summaryStats,
            summaryCoverageCell, summaryBreakdownsCell, summaryThroughputCell,
            trendGroup, quotaSeriesLabel, quotaSeriesCombo, trendSummary, trendChart, trendComparison, trendDetail,
            statusGroup, statusHeadline, statusDetail,
            providerGroup, providerList, accountGroup, accountList,
            dailyGroup, dailyList, provenanceGroup, dataQuality, gapSummary
        };

        for (HWND hwnd : controls) {
            if (hwnd == nullptr) {
                continue;
            }
            RECT rect = windowRectRelativeToParent(parent, hwnd);
            SetWindowPos(hwnd, nullptr, rect.left, rect.top + offset, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
        }
        RedrawWindow(parent, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    AnalysisScopeState normalizeScope(const AnalysisScopeState& state) const {
        AnalysisScopeState normalized = state;

        if (!viewModel.availableProviders.empty() &&
            !normalized.selectedProviderId.empty() &&
            !containsOptionKey(viewModel.availableProviders, normalized.selectedProviderId)) {
            normalized.selectedProviderId = firstOptionKey(viewModel.availableProviders);
        }

        if (!viewModel.availableAccounts.empty() &&
            !normalized.selectedAccountId.empty() &&
            !containsOptionKey(viewModel.availableAccounts, normalized.selectedAccountId)) {
            normalized.selectedAccountId = firstOptionKey(viewModel.availableAccounts);
        }

        if (viewModel.availableAccounts.empty()) {
            normalized.selectedAccountId.clear();
        }

        return normalized;
    }

    void applyCustomDateEnabledState(bool enabled) const {
        const bool interactive = enabled && !interactionPending;
        if (customStartEdit != nullptr) {
            EnableWindow(customStartEdit, interactive ? TRUE : FALSE);
            ShowWindow(customStartEdit, enabled ? SW_SHOW : SW_HIDE);
        }
        if (customEndEdit != nullptr) {
            EnableWindow(customEndEdit, interactive ? TRUE : FALSE);
            ShowWindow(customEndEdit, enabled ? SW_SHOW : SW_HIDE);
        }
        if (customStartLabel != nullptr) {
            ShowWindow(customStartLabel, enabled ? SW_SHOW : SW_HIDE);
        }
        if (customEndLabel != nullptr) {
            ShowWindow(customEndLabel, enabled ? SW_SHOW : SW_HIDE);
        }
        if (customDateHint != nullptr) {
            ShowWindow(customDateHint, enabled ? SW_SHOW : SW_HIDE);
        }
    }

    void populateBreakdownList(
        HWND list,
        const std::vector<AnalysisBreakdownRow>& rows,
        const char* emptyMessage) {
        if (list == nullptr) {
            return;
        }

        ListView_DeleteAllItems(list);
        if (rows.empty()) {
            insertBreakdownRow(list, emptyMessage, "Adjust the scope or wait for history sync.", "", "");
            return;
        }

        for (const auto& row : rows) {
            insertBreakdownRow(
                list,
                fallbackText(row.label, "Unlabeled"),
                row.secondaryLabel,
                formatNumber(row.activityCount),
                fallbackText(row.sourceLabel, "Unknown"));
        }
    }

    void insertBreakdownRow(
        HWND list,
        const std::string& label,
        const std::string& context,
        const std::string& activity,
        const std::string& source) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = ListView_GetItemCount(list);
        std::wstring wideLabel = utf8ToWide(label);
        item.pszText = wideLabel.data();
        const int rowIndex = ListView_InsertItem(list, &item);
        if (rowIndex < 0) {
            return;
        }

        setListItemText(list, rowIndex, 1, context);
        setListItemText(list, rowIndex, 2, activity);
        setListItemText(list, rowIndex, 3, source);
    }

    void populateDailyList(const std::vector<AnalysisDailySummary>& rows) {
        if (dailyList == nullptr) {
            return;
        }

        ListView_DeleteAllItems(dailyList);
        if (rows.empty()) {
            insertDailyRow("No daily history is available for this scope yet.", "", "", "Adjust the scope or wait for history sync.");
            return;
        }

        for (const auto& row : rows) {
            insertDailyRow(
                fallbackText(row.dateLabel, "-"),
                formatNumber(row.activityCount),
                fallbackText(row.sourceLabel, "Unknown"),
                buildDailyProvenanceText(row));
        }
    }

    void insertDailyRow(
        const std::string& dateLabel,
        const std::string& activity,
        const std::string& source,
        const std::string& provenance) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = ListView_GetItemCount(dailyList);
        std::wstring wideDate = utf8ToWide(dateLabel);
        item.pszText = wideDate.data();
        const int rowIndex = ListView_InsertItem(dailyList, &item);
        if (rowIndex < 0) {
            return;
        }

        setListItemText(dailyList, rowIndex, 1, activity);
        setListItemText(dailyList, rowIndex, 2, source);
        setListItemText(dailyList, rowIndex, 3, provenance);
    }

#ifdef JISTATS_ENABLE_TEST_HOOKS
public:
    std::string getScopeLabelForTesting() const {
        if (scopeSummary == nullptr) return "";
        return getWindowTextUtf8(scopeSummary);
    }

    std::string getStatusHeadlineForTesting() const {
        return currentStatusHeadline();
    }

    std::string getOverviewStatsForTesting() const {
        if (summaryStats == nullptr) return "";
        return getWindowTextUtf8(summaryStats);
    }

    std::string getTrendSummaryForTesting() const {
        return currentTrendSummaryText();
    }

    std::vector<std::string> getSectionTitlesForTesting() const {
        return {
            getWindowTextUtf8(scopeGroup),
            getWindowTextUtf8(summaryGroup),
            getWindowTextUtf8(trendGroup),
            getWindowTextUtf8(statusGroup),
            getWindowTextUtf8(providerGroup),
            getWindowTextUtf8(accountGroup),
            getWindowTextUtf8(dailyGroup),
            getWindowTextUtf8(provenanceGroup)
        };
    }

    std::vector<std::string> getProviderOptionsForTesting() const {
        if (providerCombo == nullptr) return {};
        std::vector<std::string> result;
        const LRESULT count = SendMessageW(providerCombo, CB_GETCOUNT, 0, 0);
        for (LRESULT i = 0; i < count; ++i) {
            const LRESULT len = SendMessageW(providerCombo, CB_GETLBTEXTLEN, static_cast<WPARAM>(i), 0);
            if (len <= 0) continue;
            std::wstring text(static_cast<size_t>(len + 1), L'\0');
            SendMessageW(providerCombo, CB_GETLBTEXT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(text.data()));
            text.resize(static_cast<size_t>(len));
            result.push_back(wideToUtf8(text));
        }
        return result;
    }

    std::vector<std::string> getRangeOptionsForTesting() const {
        if (rangeCombo == nullptr) return {};
        std::vector<std::string> result;
        const LRESULT count = SendMessageW(rangeCombo, CB_GETCOUNT, 0, 0);
        for (LRESULT i = 0; i < count; ++i) {
            const LRESULT len = SendMessageW(rangeCombo, CB_GETLBTEXTLEN, static_cast<WPARAM>(i), 0);
            if (len <= 0) continue;
            std::wstring text(static_cast<size_t>(len + 1), L'\0');
            SendMessageW(rangeCombo, CB_GETLBTEXT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(text.data()));
            text.resize(static_cast<size_t>(len));
            result.push_back(wideToUtf8(text));
        }
        return result;
    }

    bool areScopeInputsEnabledForTesting() const {
        const bool providerEnabled = providerCombo != nullptr && IsWindowEnabled(providerCombo) != FALSE;
        const bool accountEnabled = accountCombo != nullptr && IsWindowEnabled(accountCombo) != FALSE;
        const bool rangeEnabled = rangeCombo != nullptr && IsWindowEnabled(rangeCombo) != FALSE;
        const bool customEnabled =
            currentScope.dateRangePreset != AnalysisDateRangePreset::Custom ||
            ((customStartEdit != nullptr && IsWindowEnabled(customStartEdit) != FALSE) &&
             (customEndEdit != nullptr && IsWindowEnabled(customEndEdit) != FALSE));
        return providerEnabled && accountEnabled && rangeEnabled && customEnabled;
    }

    RECT getSectionBoundsForTesting(const std::string& sectionKey) const {
        HWND section = nullptr;
        if (sectionKey == "scope") {
            section = scopeGroup;
        } else if (sectionKey == "summary") {
            section = summaryGroup;
        } else if (sectionKey == "trend") {
            section = trendGroup;
        } else if (sectionKey == "status") {
            section = statusGroup;
        } else if (sectionKey == "provider") {
            section = providerGroup;
        } else if (sectionKey == "account") {
            section = accountGroup;
        } else if (sectionKey == "daily") {
            section = dailyGroup;
        } else if (sectionKey == "provenance") {
            section = provenanceGroup;
        }
        return windowRectRelativeToParent(parent, section);
    }

    int getScrollOffsetForTesting() const {
        return scrollOffset;
    }

    int getContentHeightForTesting() const {
        return contentHeight;
    }

    int getViewportHeightForTesting() const {
        return viewportHeight;
    }

    std::vector<std::string> getAccountOptionsForTesting() const {
        if (accountCombo == nullptr) return {};
        std::vector<std::string> result;
        const LRESULT count = SendMessageW(accountCombo, CB_GETCOUNT, 0, 0);
        for (LRESULT i = 0; i < count; ++i) {
            const LRESULT len = SendMessageW(accountCombo, CB_GETLBTEXTLEN, static_cast<WPARAM>(i), 0);
            if (len <= 0) continue;
            std::wstring text(static_cast<size_t>(len + 1), L'\0');
            SendMessageW(accountCombo, CB_GETLBTEXT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(text.data()));
            text.resize(static_cast<size_t>(len));
            result.push_back(wideToUtf8(text));
        }
        return result;
    }

    std::vector<std::string> getQuotaSeriesOptionsForTesting() const {
        if (quotaSeriesCombo == nullptr) return {};
        std::vector<std::string> result;
        const LRESULT count = SendMessageW(quotaSeriesCombo, CB_GETCOUNT, 0, 0);
        for (LRESULT i = 0; i < count; ++i) {
            const LRESULT len = SendMessageW(quotaSeriesCombo, CB_GETLBTEXTLEN, static_cast<WPARAM>(i), 0);
            if (len <= 0) continue;
            std::wstring text(static_cast<size_t>(len + 1), L'\0');
            SendMessageW(quotaSeriesCombo, CB_GETLBTEXT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(text.data()));
            text.resize(static_cast<size_t>(len));
            result.push_back(wideToUtf8(text));
        }
        return result;
    }

    std::string getTrendComparisonTextForTesting() const {
        if (trendComparison == nullptr) return {};
        return getWindowTextUtf8(trendComparison);
    }

    std::string getTrendDetailTextForTesting() const {
        if (trendDetail == nullptr) return {};
        return getWindowTextUtf8(trendDetail);
    }

    std::string getWorkspaceGuideTextForTesting() const {
        if (workspaceGuide == nullptr) return {};
        return getWindowTextUtf8(workspaceGuide);
    }

    std::vector<std::string> getWorkspaceNavigationLabelsForTesting() const {
        const HWND buttons[] = {
            navOverviewButton,
            navTrendButton,
            navOperationsButton,
            navBreakdownsButton,
            navDailyButton,
            navCoverageButton
        };
        std::vector<std::string> result;
        for (HWND button : buttons) {
            if (button != nullptr) {
                result.push_back(getWindowTextUtf8(button));
            }
        }
        return result;
    }

    std::string getCustomDateHintForTesting() const {
        if (customDateHint == nullptr) return {};
        return getWindowTextUtf8(customDateHint);
    }

    bool jumpToSectionForTesting(const std::string& sectionKey) {
        return jumpToSection(sectionKey);
    }

    bool selectTrendBucketForTesting(size_t index) {
        return selectActivityPoint(static_cast<int>(index));
    }

    bool selectQuotaTrendPointForTesting(size_t index) {
        return selectQuotaPoint(static_cast<int>(index));
    }

    bool selectProviderForTesting(const std::string& key) {
        if (providerCombo == nullptr) return false;
        for (size_t i = 0; i < viewModel.availableProviders.size(); ++i) {
            if (viewModel.availableProviders[i].key == key) {
                SendMessageW(providerCombo, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
                onProviderChanged();
                return true;
            }
        }
        return false;
    }

    bool selectAccountForTesting(const std::string& key) {
        if (accountCombo == nullptr) return false;
        for (size_t i = 0; i < viewModel.availableAccounts.size(); ++i) {
            if (viewModel.availableAccounts[i].key == key) {
                SendMessageW(accountCombo, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
                onAccountChanged();
                return true;
            }
        }
        return false;
    }

    bool selectRangePresetForTesting(AnalysisDateRangePreset preset) {
        if (rangeCombo == nullptr) return false;
        const std::string key = presetKeyFromEnum(preset);
        for (size_t i = 0; i < std::size(presetKeys); ++i) {
            if (presetKeys[i] == key) {
                SendMessageW(rangeCombo, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
                onRangeChanged();
                return true;
            }
        }
        return false;
    }

    bool selectQuotaSeriesForTesting(const std::string& key) {
        if (quotaSeriesCombo == nullptr) return false;
        for (size_t i = 0; i < viewModel.quotaTrendSeries.size(); ++i) {
            if (viewModel.quotaTrendSeries[i].key == key) {
                SendMessageW(quotaSeriesCombo, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
                onQuotaSeriesChanged();
                return true;
            }
        }
        return false;
    }

    void setCustomRangeTextForTesting(const std::string& startDate, const std::string& endDate) {
        setWindowTextUtf8(customStartEdit, startDate);
        setWindowTextUtf8(customEndEdit, endDate);
    }
#endif
};

AnalysisPresenter::AnalysisPresenter() : pImpl(std::make_unique<Impl>()) {}
AnalysisPresenter::~AnalysisPresenter() = default;

bool AnalysisPresenter::init(HWND parent) { return pImpl->init(parent); }
void AnalysisPresenter::shutdown() { pImpl->shutdown(); }
void AnalysisPresenter::updateViewModel(const AnalysisViewModel& viewModel) { pImpl->updateViewModel(viewModel); }
void AnalysisPresenter::setInteractionPending(bool pending) { pImpl->setInteractionPending(pending); }
void AnalysisPresenter::setScopeChangedCallback(std::function<void(const AnalysisScopeState&)> callback) {
    pImpl->scopeChangedCallback = std::move(callback);
}
void AnalysisPresenter::setVisible(bool visible) { pImpl->setVisible(visible); }
void AnalysisPresenter::layout(const RECT& bounds) { pImpl->layout(bounds); }
bool AnalysisPresenter::handleCommand(WORD id, WORD notifyCode) { return pImpl->handleCommand(id, notifyCode); }
bool AnalysisPresenter::handleTimer(UINT_PTR timerId) { return pImpl->handleTimer(timerId); }
bool AnalysisPresenter::handleVScroll(WORD request, WORD thumbPosition) { return pImpl->handleVScroll(request, thumbPosition); }
bool AnalysisPresenter::handleMouseWheel(short wheelDelta) { return pImpl->handleMouseWheel(wheelDelta); }
bool AnalysisPresenter::handleDrawItem(const DRAWITEMSTRUCT& drawItem) { return pImpl->handleDrawItem(drawItem); }

#ifdef JISTATS_ENABLE_TEST_HOOKS
std::string AnalysisPresenter::getScopeLabelForTesting() const { return pImpl->getScopeLabelForTesting(); }
std::string AnalysisPresenter::getStatusHeadlineForTesting() const { return pImpl->getStatusHeadlineForTesting(); }
std::string AnalysisPresenter::getOverviewStatsForTesting() const { return pImpl->getOverviewStatsForTesting(); }
std::string AnalysisPresenter::getTrendSummaryForTesting() const { return pImpl->getTrendSummaryForTesting(); }
std::vector<std::string> AnalysisPresenter::getSectionTitlesForTesting() const { return pImpl->getSectionTitlesForTesting(); }
std::vector<std::string> AnalysisPresenter::getProviderOptionsForTesting() const { return pImpl->getProviderOptionsForTesting(); }
std::vector<std::string> AnalysisPresenter::getAccountOptionsForTesting() const { return pImpl->getAccountOptionsForTesting(); }
std::vector<std::string> AnalysisPresenter::getRangeOptionsForTesting() const {
    return pImpl->getRangeOptionsForTesting();
}
std::vector<std::string> AnalysisPresenter::getQuotaSeriesOptionsForTesting() const {
    return pImpl->getQuotaSeriesOptionsForTesting();
}
std::string AnalysisPresenter::getWorkspaceGuideTextForTesting() const {
    return pImpl->getWorkspaceGuideTextForTesting();
}
std::vector<std::string> AnalysisPresenter::getWorkspaceNavigationLabelsForTesting() const {
    return pImpl->getWorkspaceNavigationLabelsForTesting();
}
std::string AnalysisPresenter::getTrendComparisonTextForTesting() const {
    return pImpl->getTrendComparisonTextForTesting();
}
std::string AnalysisPresenter::getTrendDetailTextForTesting() const {
    return pImpl->getTrendDetailTextForTesting();
}
std::string AnalysisPresenter::getCustomDateHintForTesting() const { return pImpl->getCustomDateHintForTesting(); }
bool AnalysisPresenter::areScopeInputsEnabledForTesting() const { return pImpl->areScopeInputsEnabledForTesting(); }
RECT AnalysisPresenter::getSectionBoundsForTesting(const std::string& sectionKey) const {
    return pImpl->getSectionBoundsForTesting(sectionKey);
}
int AnalysisPresenter::getScrollOffsetForTesting() const { return pImpl->getScrollOffsetForTesting(); }
int AnalysisPresenter::getContentHeightForTesting() const { return pImpl->getContentHeightForTesting(); }
int AnalysisPresenter::getViewportHeightForTesting() const { return pImpl->getViewportHeightForTesting(); }
bool AnalysisPresenter::jumpToSectionForTesting(const std::string& sectionKey) {
    return pImpl->jumpToSectionForTesting(sectionKey);
}
bool AnalysisPresenter::selectTrendBucketForTesting(size_t index) {
    return pImpl->selectTrendBucketForTesting(index);
}
bool AnalysisPresenter::selectQuotaTrendPointForTesting(size_t index) {
    return pImpl->selectQuotaTrendPointForTesting(index);
}
bool AnalysisPresenter::selectProviderForTesting(const std::string& key) { return pImpl->selectProviderForTesting(key); }
bool AnalysisPresenter::selectAccountForTesting(const std::string& key) { return pImpl->selectAccountForTesting(key); }
bool AnalysisPresenter::selectRangePresetForTesting(AnalysisDateRangePreset preset) {
    return pImpl->selectRangePresetForTesting(preset);
}
bool AnalysisPresenter::selectQuotaSeriesForTesting(const std::string& key) {
    return pImpl->selectQuotaSeriesForTesting(key);
}
void AnalysisPresenter::setCustomRangeTextForTesting(const std::string& startDate, const std::string& endDate) {
    pImpl->setCustomRangeTextForTesting(startDate, endDate);
}
#endif

} // namespace JiStats
