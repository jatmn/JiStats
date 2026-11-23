#include "gui/AnalysisPresenter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>

#include <commctrl.h>
#include <windowsx.h>

namespace JiStats {

namespace {

constexpr const wchar_t* kPageTitles[] = {L"Activity", L"Quota history", L"Coverage"};
enum class AnalysisPage { Activity, Quota, Coverage };

struct ListColumn {
    const wchar_t* title;
    int minWidth;
    int weight;
};

constexpr ListColumn kProviderColumns[] = {{L"Provider", 130, 3}, {L"Records", 76, 1}, {L"Source", 146, 3}};
constexpr ListColumn kAccountColumns[] = {
    {L"Account", 124, 3}, {L"Provider", 112, 2}, {L"Records", 76, 1}, {L"Source", 136, 3}};
constexpr ListColumn kDailyColumns[] = {
    {L"Date (UTC)", 98, 1}, {L"Records", 76, 1}, {L"Source", 148, 2}, {L"Notes", 172, 4}};

constexpr const wchar_t* kRangeLabels[] = {
    L"Today", L"Last 24 Hours", L"Last 7 Days", L"Current Billing/Quota Window", L"This Month", L"Custom"};
constexpr const wchar_t* kCustomDateInstruction = L"YYYY-MM-DD (UTC). Both dates are included.";
constexpr const char* kLoadingText = "Loading the selected range. The previous results are still shown.";

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

bool updateLayoutText(HWND hwnd, const std::string& text) {
    if (getWindowTextUtf8(hwnd) == text) {
        return false;
    }
    setWindowTextUtf8(hwnd, text);
    return true;
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
    if (value.size() != 10) {
        return std::nullopt;
    }
    std::tm tm{};
    std::istringstream stream(value);
    stream >> std::get_time(&tm, "%Y-%m-%d");
    if (stream.fail()) {
        return std::nullopt;
    }
    const auto date = _mkgmtime(&tm);
    return formatUtcDateOnly(date) == value ? std::optional<std::time_t>(date) : std::nullopt;
}

std::string formatUtcTime(std::time_t value) {
    if (value <= 0) {
        return {};
    }
    std::tm utc{};
    gmtime_s(&utc, &value);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &utc);
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
        stream << (summary.activityCount > 0 ? "Recorded" : "No activity evidence");
    }
    return stream.str();
}

RECT windowRectRelativeToParent(HWND parent, HWND child) {
    RECT rect{};
    if (parent == nullptr || child == nullptr || GetWindowRect(child, &rect) == 0) {
        return rect;
    }

    POINT points[2] = {{rect.left, rect.top}, {rect.right, rect.bottom}};
    MapWindowPoints(HWND_DESKTOP, parent, points, 2);
    rect.left = points[0].x;
    rect.top = points[0].y;
    rect.right = points[1].x;
    rect.bottom = points[1].y;
    return rect;
}

bool scopesMatch(const AnalysisScopeState& left, const AnalysisScopeState& right) {
    return left.selectedProviderId == right.selectedProviderId && left.selectedAccountId == right.selectedAccountId &&
           left.dateRangePreset == right.dateRangePreset && left.customRangeStartUtc == right.customRangeStartUtc &&
           left.customRangeEndUtc == right.customRangeEndUtc;
}

template <typename Option> bool sameOptions(const std::vector<Option>& first, const std::vector<Option>& second) {
    return std::equal(first.begin(), first.end(), second.begin(), second.end(),
                      [](const auto& a, const auto& b) { return a.key == b.key && a.label == b.label; });
}

std::string formatPercentage(double value) {
    if (value < 0.0) {
        return "Unavailable";
    }
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(value >= 10.0 ? 0 : 1) << value << "%";
    return stream.str();
}

std::string quotaAmount(const AnalysisQuotaTrendSeries& series, const AnalysisQuotaTrendPoint& point) {
    std::string text = formatPercentage(point.remainingPercentage);
    if (point.limitValue > 0 && series.quotaDimension != "percent" && !series.quotaDimension.empty()) {
        text += " (" + formatNumber(point.remainingValue) + " of " + formatNumber(point.limitValue) + " " +
                series.quotaDimension + ")";
    }
    return text;
}

std::string activitySummary(const AnalysisViewModel& model) {
    long long records = 0;
    for (const auto& point : model.trendPoints) {
        records += point.activityCount;
    }
    if (records == 0) {
        return "No activity records in this range.";
    }

    return formatNumber(records) + " " + pluralize(records, "record", "records") + " in the selected range";
}

std::string recentCodexUsage(const AnalysisCodexThroughput& usage) {
    if (!usage.hasData) {
        return {};
    }
    return "Recent Codex activity (" + fallbackText(usage.confidenceLabel, "source unknown") +
           "). Fixed windows ending at the last refresh:\r\n" + "5 min: " + formatRate(usage.tokensPerMinute5m) +
           " tokens/min, " + formatRate(usage.promptsPerMinute5m) + " prompts/min.\r\n" +
           "30 min: " + formatRate(usage.tokensPerMinute30m) + " tokens/min, " + formatRate(usage.promptsPerMinute30m) +
           " prompts/min.";
}

std::string coverageSummary(const AnalysisGapSummaryCounts& counts) {
    return "Coverage by day: " + formatNumber(counts.knownZeroDays) + " with a confirmed zero-usage gap, " +
           formatNumber(counts.reconstructedDays) + " with reconstructed data, " + formatNumber(counts.unknownDays) +
           " with an unresolved gap, and " + formatNumber(counts.unknownSourceDays) +
           " with an unknown source. A day can have a gap and still contain records.";
}

std::string scopeText(const AnalysisViewModel& model) {
    return "Active filters: " + fallbackText(model.scopeLabel, "All Providers / All Accounts / Last 7 Days");
}

void setControlFont(HWND window, HFONT font) {
    if (window != nullptr && font != nullptr) {
        SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    }
}

} // namespace

class AnalysisPresenter::Impl {
  public:
    ~Impl() {
        shutdown();
    }

    bool init(HWND owner) {
        if (owner != nullptr && parent == owner && tabs != nullptr) {
            return true;
        }
        shutdown();
        if (owner == nullptr) {
            return false;
        }
        parent = owner;
        INITCOMMONCONTROLSEX commonControls{sizeof(INITCOMMONCONTROLSEX), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES};
        InitCommonControlsEx(&commonControls);

        static std::once_flag chartClass;
        std::call_once(chartClass, []() {
            WNDCLASSW wc{};
            wc.lpfnWndProc = chartWindowProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = kChartClass;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            RegisterClassW(&wc);
        });

        scopeGroup = makeControl(parent, L"BUTTON", L"History filters", BS_GROUPBOX);
        providerLabel = makeLabel(parent, L"Provider");
        providerCombo = makeCombo(parent, kProviderComboId);
        accountLabel = makeLabel(parent, L"Account");
        accountCombo = makeCombo(parent, kAccountComboId);
        rangeLabel = makeLabel(parent, L"Date range");
        rangeCombo = makeCombo(parent, kRangeComboId);
        for (const wchar_t* title : kRangeLabels) {
            SendMessageW(rangeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(title));
        }

        customStartLabel = makeLabel(parent, L"From");
        customStartEdit =
            makeControl(parent, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kCustomStartEditId, WS_EX_CLIENTEDGE);
        customEndLabel = makeLabel(parent, L"To");
        customEndEdit =
            makeControl(parent, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kCustomEndEditId, WS_EX_CLIENTEDGE);
        SendMessageW(customStartEdit, EM_SETLIMITTEXT, 10, 0);
        SendMessageW(customEndEdit, EM_SETLIMITTEXT, 10, 0);
        customDateHint = makeLabel(parent, kCustomDateInstruction);
        scopeSummary = makeLabel(parent, L"");
        scopeStatus = makeLabel(parent, L"");

        tabs = makeControl(parent, WC_TABCONTROLW, L"History views", WS_TABSTOP | WS_CLIPSIBLINGS);
        for (int index = 0; index < static_cast<int>(std::size(kPageTitles)); ++index) {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<wchar_t*>(kPageTitles[index]);
            SendMessageW(tabs, TCM_INSERTITEMW, index, reinterpret_cast<LPARAM>(&item));
        }
        viewport = makeControl(parent, L"STATIC", L"", WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, WS_EX_CONTROLPARENT);
        body = makeControl(viewport, L"STATIC", L"", WS_CLIPCHILDREN, 0, WS_EX_CONTROLPARENT);
        if (!body || !tabs || !scopeGroup || !providerCombo || !accountCombo || !rangeCombo || !customStartEdit ||
            !customEndEdit || !SetWindowSubclass(parent, parentSubclass, 1, reinterpret_cast<DWORD_PTR>(this)) ||
            !SetWindowSubclass(body, bodySubclass, 1, reinterpret_cast<DWORD_PTR>(this))) {
            shutdown();
            return false;
        }

        chartGroup = makeControl(body, L"BUTTON", L"Recorded activity", BS_GROUPBOX);
        quotaSeriesLabel = makeLabel(body, L"Quota window");
        quotaSeriesCombo = makeCombo(body, kQuotaSeriesComboId);
        trendSummary = makeLabel(body, L"");
        chart = makeControl(body, kChartClass, L"History chart", WS_TABSTOP, 0, WS_EX_CLIENTEDGE, this);
        trendDetail = makeLabel(body, L"");
        activityExplanation = makeLabel(body, L"Local activity supplies counts when an account has no usage entry in "
                                              L"that bucket. Daily rows apply this per UTC day. Blank buckets have "
                                              L"no recorded count.");
        dailyGroup = makeControl(body, L"BUTTON", L"Daily activity", BS_GROUPBOX);
        dailyList = makeList(kDailyColumns);
        providerGroup = makeControl(body, L"BUTTON", L"Activity by provider", BS_GROUPBOX);
        providerList = makeList(kProviderColumns);
        accountGroup = makeControl(body, L"BUTTON", L"Activity by account", BS_GROUPBOX);
        accountList = makeList(kAccountColumns);

        quotaGroup = makeControl(body, L"BUTTON", L"Latest samples in this range", BS_GROUPBOX);
        quotaComparison = makeLabel(body, L"");
        coverageGroup = makeControl(body, L"BUTTON", L"Data coverage", BS_GROUPBOX);
        coverageText = makeLabel(body, L"");
        qualityText = makeLabel(body, L"");
        codexGroup = makeControl(body, L"BUTTON", L"Recent Codex throughput", BS_GROUPBOX);
        codexText = makeLabel(body, L"");
        if (!chart || !quotaSeriesCombo || !dailyList || !providerList || !accountList || !coverageGroup) {
            shutdown();
            return false;
        }

        applyFonts();
        updateViewModel(viewModel);
        syncPage();
        return true;
    }

    void shutdown() {
        cancelCustomDateEdit();
        if (parent != nullptr) {
            RemoveWindowSubclass(parent, parentSubclass, 1);
        }
        if (body != nullptr) {
            RemoveWindowSubclass(body, bodySubclass, 1);
        }
        if (viewport != nullptr) {
            DestroyWindow(viewport);
        }
        for (HWND window : headerControls()) {
            if (window != nullptr) {
                DestroyWindow(window);
            }
        }
        if (parent != nullptr && IsWindow(parent)) {
            ShowScrollBar(parent, SB_VERT, FALSE);
        }
        if (font != nullptr) {
            DeleteObject(font);
            font = nullptr;
        }
        parent = nullptr;
        viewport = nullptr;
        body = nullptr;
        tabs = nullptr;
        scopeGroup = nullptr;
        providerLabel = nullptr;
        providerCombo = nullptr;
        accountLabel = nullptr;
        accountCombo = nullptr;
        rangeLabel = nullptr;
        rangeCombo = nullptr;
        customStartLabel = nullptr;
        customStartEdit = nullptr;
        customEndLabel = nullptr;
        customEndEdit = nullptr;
        customDateHint = nullptr;
        scopeSummary = nullptr;
        scopeStatus = nullptr;
        chartGroup = nullptr;
        quotaSeriesLabel = nullptr;
        quotaSeriesCombo = nullptr;
        trendSummary = nullptr;
        chart = nullptr;
        trendDetail = nullptr;
        activityExplanation = nullptr;
        dailyGroup = nullptr;
        dailyList = nullptr;
        providerGroup = nullptr;
        providerList = nullptr;
        accountGroup = nullptr;
        accountList = nullptr;
        quotaGroup = nullptr;
        quotaComparison = nullptr;
        coverageGroup = nullptr;
        coverageText = nullptr;
        qualityText = nullptr;
        codexGroup = nullptr;
        codexText = nullptr;
        interactionPending = false;
        awaitingScope.reset();
        customDateError = false;
        customDateEditorSelected = false;
        shown = true;
        wheelRemainder = 0;
        hasBounds = false;
        scrollOffset = 0;
        contentHeight = 0;
        viewportHeight = 0;
        page = AnalysisPage::Activity;
    }

    void updateViewModel(const AnalysisViewModel& model) {
        if (parent == nullptr) {
            viewModel = model;
            currentScope = model.scope;
            return;
        }
        if (awaitingScope && model.requestedScope && !scopesMatch(*awaitingScope, *model.requestedScope)) {
            return;
        }
        if (awaitingScope && !model.requestedScope &&
            awaitingScope->dateRangePreset == AnalysisDateRangePreset::Custom &&
            (model.scope.customRangeStartUtc != awaitingScope->customRangeStartUtc ||
             model.scope.customRangeEndUtc != awaitingScope->customRangeEndUtc)) {
            return;
        }

        const bool hadCustomDates = showsCustomDates();
        const auto previousQuotaTime = selectedQuotaTime();
        const auto previousActivityTime = selectedActivityTime();
        const bool sameScope = scopesMatch(viewModel.scope, model.scope);
        const bool providerOptionsChanged = !sameOptions(viewModel.availableProviders, model.availableProviders);
        const bool accountOptionsChanged = !sameOptions(viewModel.availableAccounts, model.availableAccounts);
        const bool quotaOptionsChanged = !sameOptions(viewModel.quotaTrendSeries, model.quotaTrendSeries);

        currentScope = model.scope;
        viewModel = model;
        awaitingScope.reset();
        interactionPending = false;

        suppressNotification = true;
        bindCombo(providerCombo, viewModel.availableProviders, currentScope.selectedProviderId, providerOptionsChanged);
        bindCombo(accountCombo, viewModel.availableAccounts, currentScope.selectedAccountId, accountOptionsChanged);
        if (!customDateEditPending && !customDateError) {
            customDateEditorSelected = false;
            updateLayoutText(customStartEdit, formatUtcDateOnly(currentScope.customRangeStartUtc));
            updateLayoutText(customEndEdit, formatUtcDateOnly(currentScope.customRangeEndUtc));
        }
        const auto rangeIndex =
            static_cast<LRESULT>(showsCustomDates() ? AnalysisDateRangePreset::Custom : currentScope.dateRangePreset);
        if (SendMessageW(rangeCombo, CB_GETCURSEL, 0, 0) != rangeIndex) {
            SendMessageW(rangeCombo, CB_SETCURSEL, static_cast<WPARAM>(rangeIndex), 0);
        }
        if (!sameScope) {
            selectedQuotaSeriesKey.clear();
        }
        selectExistingQuotaSeries();
        bindCombo(quotaSeriesCombo, viewModel.quotaTrendSeries, selectedQuotaSeriesKey, quotaOptionsChanged);
        suppressNotification = false;

        selectedActivityPoint = pointIndex(viewModel.trendPoints, sameScope ? previousActivityTime : 0,
                                           [](const auto& point) { return point.bucketStartUtc; });
        if (const auto* series = selectedQuotaSeries()) {
            selectedQuotaPoint = pointIndex(series->points, sameScope ? previousQuotaTime : 0,
                                            [](const auto& point) { return point.snapshotTime; });
        } else {
            selectedQuotaPoint = -1;
        }

        refreshList(dailyList, viewModel.dailySummaries.size());
        refreshList(providerList, viewModel.providerBreakdown.size());
        refreshList(accountList, viewModel.accountBreakdown.size());
        updateLayoutText(scopeSummary, scopeText(viewModel));
        updateScopeStatus();
        updateScopeControls();
        bool coverageChanged = updateLayoutText(coverageText, coverageSummary(viewModel.gapSummaryCounts));
        coverageChanged |= updateLayoutText(qualityText, viewModel.dataQualityNote);
        coverageChanged |= updateLayoutText(codexText, recentCodexUsage(viewModel.codexThroughput));
        const bool trendLayoutChanged = refreshTrendControls();
        syncPageVisibility();

        if (hasBounds && (hadCustomDates != showsCustomDates() || trendLayoutChanged ||
                          (page == AnalysisPage::Coverage && coverageChanged))) {
            layout(bounds);
        }
    }

    void setInteractionPending(bool pending) {
        interactionPending = pending;
        if (!pending) {
            awaitingScope.reset();
        }
        updateScopeStatus();
        updateScopeControls();
    }

    void setVisible(bool visible) {
        shown = visible;
        for (HWND window : headerControls()) {
            toggle(window, visible);
        }
        toggle(viewport, visible);
        updateScopeControls();
    }

    void layout(const RECT& newBounds) {
        if (parent == nullptr || layoutInProgress) {
            return;
        }
        layoutInProgress = true;
        bounds = newBounds;
        hasBounds = true;
        const int x = bounds.left;
        const int y = bounds.top;
        const int width = std::max(1L, bounds.right - bounds.left);
        const int height = std::max(1L, bounds.bottom - bounds.top);
        const int margin = px(10);
        const int labelHeight = px(18);
        const int fieldHeight = px(25);
        const int fieldWidth = std::max(1, (width - 4 * margin) / 3);
        const bool custom = showsCustomDates();
        const bool twoCustomRows = width < px(620);
        const int extraHeight = custom ? (twoCustomRows ? px(62) : px(38)) : 0;
        const int headerHeight = px(108) + extraHeight;
        position(scopeGroup, x, y, width, headerHeight);

        int fieldX = x + margin;
        const int labelY = y + px(20);
        const int fieldY = labelY + labelHeight;
        const HWND labels[] = {providerLabel, accountLabel, rangeLabel};
        const HWND fields[] = {providerCombo, accountCombo, rangeCombo};
        for (size_t i = 0; i < std::size(labels); ++i) {
            position(labels[i], fieldX, labelY, fieldWidth, labelHeight);
            position(fields[i], fieldX, fieldY, fieldWidth, px(225));
            fieldX += fieldWidth + margin;
        }
        if (custom) {
            const int customY = fieldY + fieldHeight + px(4);
            const int dateWidth = std::max(1, std::min(px(105), (width - px(110)) / 2));
            const int fromX = x + margin;
            const int startX = fromX + px(34);
            const int toX = startX + dateWidth + px(12);
            const int endX = toX + px(22);
            const int hintX = twoCustomRows ? fromX : endX + dateWidth + px(10);
            const int hintY = twoCustomRows ? customY + px(24) : customY;
            position(customStartLabel, fromX, customY, px(32), labelHeight);
            position(customStartEdit, startX, customY - px(2), dateWidth, fieldHeight);
            position(customEndLabel, toX, customY, px(20), labelHeight);
            position(customEndEdit, endX, customY - px(2), dateWidth, fieldHeight);
            position(customDateHint, hintX, hintY, std::max(1, x + width - margin - hintX), px(36));
        }

        const int summaryY = fieldY + fieldHeight + extraHeight + px(3);
        position(scopeSummary, x + margin, summaryY, std::max(1, width - 2 * margin), labelHeight);
        position(scopeStatus, x + margin, summaryY + labelHeight, std::max(1, width - 2 * margin), labelHeight);

        const int tabsY = y + headerHeight + px(8);
        const int tabsHeight = std::max(px(30), height - (tabsY - y));
        position(tabs, x, tabsY, width, tabsHeight);
        RECT pane{0, 0, width, tabsHeight};
        TabCtrl_AdjustRect(tabs, FALSE, &pane);
        const int paneWidth = std::max(1L, pane.right - pane.left);
        const int paneHeight = std::max(1L, pane.bottom - pane.top);
        SetWindowPos(viewport, HWND_TOP, x + pane.left, tabsY + pane.top, paneWidth, paneHeight, SWP_NOACTIVATE);
        layoutPage(paneWidth);

        viewportHeight = paneHeight;
        scrollOffset = std::clamp(scrollOffset, 0, maxScrollOffset());
        position(body, 0, -scrollOffset, paneWidth, std::max(viewportHeight, contentHeight));
        updateScrollBar();
        layoutInProgress = false;
    }

    bool handleCommand(WORD id, WORD notifyCode) {
        if (id == kCustomStartEditId || id == kCustomEndEditId) {
            if (!suppressNotification && (notifyCode == EN_CHANGE || notifyCode == EN_KILLFOCUS)) {
                onCustomDateChanged(notifyCode);
            }
            return true;
        }
        if (id != kProviderComboId && id != kAccountComboId && id != kRangeComboId && id != kQuotaSeriesComboId) {
            return false;
        }
        if (suppressNotification || notifyCode != CBN_SELCHANGE) {
            return true;
        }

        AnalysisScopeState scope = currentScope;
        switch (id) {
            case kProviderComboId:
                scope.selectedProviderId = selectedKey(providerCombo, viewModel.availableProviders);
                if (scope.selectedProviderId != currentScope.selectedProviderId) {
                    scope.selectedAccountId.clear();
                    SendMessageW(accountCombo, CB_SETCURSEL, 0, 0);
                }
                break;
            case kAccountComboId:
                scope.selectedAccountId = selectedKey(accountCombo, viewModel.availableAccounts);
                break;
            case kRangeComboId: {
                const LRESULT selection = SendMessageW(rangeCombo, CB_GETCURSEL, 0, 0);
                if (selection < 0 || selection >= static_cast<LRESULT>(std::size(kRangeLabels))) {
                    return true;
                }
                const bool hadCustomDates = showsCustomDates();
                cancelCustomDateEdit();
                customDateError = false;
                SetWindowTextW(customDateHint, kCustomDateInstruction);
                scope.dateRangePreset = static_cast<AnalysisDateRangePreset>(selection);
                customDateEditorSelected = scope.dateRangePreset == AnalysisDateRangePreset::Custom;
                if (scope.dateRangePreset == AnalysisDateRangePreset::Custom) {
                    if (!commitCustomDates(scope)) {
                        updateScopeControls();
                        if (hasBounds && !hadCustomDates) {
                            layout(bounds);
                        }
                        return true;
                    }
                }
                publishScope(scope);
                updateScopeControls();
                if (hasBounds && hadCustomDates != showsCustomDates()) {
                    layout(bounds);
                }
                return true;
            }
            case kQuotaSeriesComboId: {
                const std::string key = selectedKey(quotaSeriesCombo, viewModel.quotaTrendSeries);
                if (!key.empty() && key != selectedQuotaSeriesKey) {
                    selectedQuotaSeriesKey = key;
                    const auto* series = selectedQuotaSeries();
                    selectedQuotaPoint = series == nullptr ? -1 : static_cast<int>(series->points.size()) - 1;
                    if (refreshTrendControls() && hasBounds) {
                        layout(bounds);
                    }
                }
                return true;
            }
        }
        publishScope(scope);
        return true;
    }

    bool handleTimer(UINT_PTR id) {
        if (id != kCustomDateDebounceTimerId) {
            return false;
        }
        commitCustomDateEdit();
        return true;
    }

    bool handleVScroll(WORD request, WORD thumbPosition) {
        if (!hasBounds || contentHeight <= viewportHeight) {
            return false;
        }
        const int line = px(48);
        const int pageSize = std::max(line, viewportHeight - line);
        int target = scrollOffset;
        switch (request) {
            case SB_LINEUP:
                target -= line;
                break;
            case SB_LINEDOWN:
                target += line;
                break;
            case SB_PAGEUP:
                target -= pageSize;
                break;
            case SB_PAGEDOWN:
                target += pageSize;
                break;
            case SB_TOP:
                target = 0;
                break;
            case SB_BOTTOM:
                target = maxScrollOffset();
                break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: {
                SCROLLINFO info{sizeof(SCROLLINFO), SIF_TRACKPOS};
                target = GetScrollInfo(parent, SB_VERT, &info) ? info.nTrackPos : thumbPosition;
                break;
            }
            default:
                return false;
        }
        return setScrollOffset(target);
    }

    bool handleMouseWheel(short delta) {
        if (!hasBounds || contentHeight <= viewportHeight) {
            return false;
        }
        wheelRemainder += delta;
        const int steps = wheelRemainder / WHEEL_DELTA;
        wheelRemainder %= WHEEL_DELTA;
        if (steps != 0) {
            setScrollOffset(scrollOffset - steps * px(72));
        }
        return true;
    }

    std::function<void(const AnalysisScopeState&)> scopeChangedCallback;

  private:
    static constexpr const wchar_t* kChartClass = L"JiStatsHistoryChart";
    static constexpr int kProviderComboId = 2101;
    static constexpr int kAccountComboId = 2102;
    static constexpr int kRangeComboId = 2103;
    static constexpr int kCustomStartEditId = 2104;
    static constexpr int kCustomEndEditId = 2105;
    static constexpr UINT_PTR kCustomDateDebounceTimerId = 2106;
    static constexpr int kQuotaSeriesComboId = 2107;

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
    HWND scopeStatus = nullptr;
    HWND tabs = nullptr;
    HWND viewport = nullptr;
    HWND body = nullptr;
    HWND chartGroup = nullptr;
    HWND quotaSeriesLabel = nullptr;
    HWND quotaSeriesCombo = nullptr;
    HWND trendSummary = nullptr;
    HWND chart = nullptr;
    HWND trendDetail = nullptr;
    HWND activityExplanation = nullptr;
    HWND dailyGroup = nullptr;
    HWND dailyList = nullptr;
    HWND providerGroup = nullptr;
    HWND providerList = nullptr;
    HWND accountGroup = nullptr;
    HWND accountList = nullptr;
    HWND quotaGroup = nullptr;
    HWND quotaComparison = nullptr;
    HWND coverageGroup = nullptr;
    HWND coverageText = nullptr;
    HWND qualityText = nullptr;
    HWND codexGroup = nullptr;
    HWND codexText = nullptr;
    HFONT font = nullptr;
    UINT dpi = 96;

    AnalysisViewModel viewModel;
    AnalysisPage page = AnalysisPage::Activity;
    AnalysisScopeState currentScope;
    std::optional<AnalysisScopeState> awaitingScope;
    std::string selectedQuotaSeriesKey;
    int selectedActivityPoint = -1;
    int selectedQuotaPoint = -1;
    bool interactionPending = false;
    bool customDateEditPending = false;
    bool customDateEditorSelected = false;
    bool customDateError = false;
    bool suppressNotification = false;
    bool shown = true;
    bool layoutInProgress = false;
    bool hasBounds = false;
    RECT bounds{};
    int scrollOffset = 0;
    int viewportHeight = 0;
    int contentHeight = 0;
    int wheelRemainder = 0;

    static HWND makeControl(HWND owner, const wchar_t* klass, const wchar_t* title, DWORD style, int id = 0,
                            DWORD extendedStyle = 0, void* arg = nullptr) {
        return CreateWindowExW(extendedStyle, klass, title, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, owner,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), arg);
    }

    static HWND makeLabel(HWND owner, const wchar_t* text) {
        return makeControl(owner, L"STATIC", text, SS_LEFT | SS_NOPREFIX);
    }

    static HWND makeCombo(HWND owner, int id) {
        return makeControl(owner, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, id);
    }

    template <size_t N> HWND makeList(const ListColumn (&columns)[N]) const {
        HWND list = makeControl(body, WC_LISTVIEWW, L"",
                                WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0,
                                WS_EX_CLIENTEDGE);
        if (list == nullptr) {
            return nullptr;
        }
        ListView_SetUnicodeFormat(list, TRUE);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        for (size_t i = 0; i < N; ++i) {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = const_cast<wchar_t*>(columns[i].title);
            column.cx = px(columns[i].minWidth);
            SendMessageW(list, LVM_INSERTCOLUMNW, i, reinterpret_cast<LPARAM>(&column));
        }
        return list;
    }

    static void position(HWND window, int x, int y, int width, int height) {
        SetWindowPos(window, nullptr, x, y, std::max(0, width), std::max(0, height), SWP_NOZORDER | SWP_NOACTIVATE);
    }

    static void toggle(HWND window, bool visible) {
        if (window != nullptr && ((GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0) != visible) {
            ShowWindow(window, visible ? SW_SHOWNA : SW_HIDE);
        }
    }

    std::array<HWND, 15> headerControls() const {
        return {scopeGroup,    providerLabel,  providerCombo,    accountLabel,    accountCombo,
                rangeLabel,    rangeCombo,     customStartLabel, customStartEdit, customEndLabel,
                customEndEdit, customDateHint, scopeSummary,     scopeStatus,     tabs};
    }

    std::array<HWND, 20> pageControls() const {
        return {chartGroup,    quotaSeriesLabel,    quotaSeriesCombo, trendSummary, chart,
                trendDetail,   activityExplanation, dailyGroup,       dailyList,    providerGroup,
                providerList,  accountGroup,        accountList,      quotaGroup,   quotaComparison,
                coverageGroup, coverageText,        qualityText,      codexGroup,   codexText};
    }

    int px(int value) const {
        return MulDiv(value, dpi, 96);
    }

    void applyFonts() {
        dpi = GetDpiForWindow(parent);
        if (dpi == 0) {
            dpi = 96;
        }
        HFONT previous = font;
        font = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Tahoma");
        const auto useFont = font != nullptr ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        for (HWND window : headerControls()) {
            setControlFont(window, useFont);
        }
        for (HWND window : pageControls()) {
            setControlFont(window, useFont);
        }
        if (previous != nullptr) {
            DeleteObject(previous);
        }
    }

    int textHeight(HWND control, int width, int minimum) const {
        HDC dc = GetDC(control);
        if (dc == nullptr) {
            return minimum;
        }
        const auto previous = font != nullptr ? SelectObject(dc, font) : nullptr;
        RECT rect{0, 0, width, 0};
        const std::wstring text = utf8ToWide(getWindowTextUtf8(control));
        DrawTextW(dc, text.c_str(), -1, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        if (previous != nullptr) {
            SelectObject(dc, previous);
        }
        ReleaseDC(control, dc);
        return std::max(minimum, static_cast<int>(rect.bottom) + px(4));
    }

    template <size_t N>
    void layoutList(HWND group, HWND list, const ListColumn (&columns)[N], int x, int y, int width, int height) const {
        const int side = px(8);
        const int top = px(18);
        position(group, x, y, width, height);
        position(list, x + side, y + top, width - 2 * side, height - top - side);
        RECT listClient{};
        GetClientRect(list, &listClient);
        int totalMinimum = 0;
        int totalWeight = 0;
        for (const auto& col : columns) {
            totalMinimum += px(col.minWidth);
            totalWeight += col.weight;
        }
        const int extra = std::max(0L, listClient.right - totalMinimum);
        for (size_t i = 0; i < N; ++i) {
            const int columnWidth = px(columns[i].minWidth) + extra * columns[i].weight / totalWeight;
            if (ListView_GetColumnWidth(list, i) != columnWidth) {
                ListView_SetColumnWidth(list, static_cast<int>(i), columnWidth);
            }
        }
    }

    void layoutPage(int bodyWidth) {
        const int outer = px(9);
        const int padding = px(10);
        const int gap = px(8);
        const int blockWidth = std::max(1, bodyWidth - 2 * outer);
        const int width = std::max(1, blockWidth - 2 * padding);
        const int left = outer + padding;
        int y = outer;

        if (page == AnalysisPage::Coverage) {
            const int coverageHeight = textHeight(coverageText, width, px(36));
            const int qualityHeight = textHeight(qualityText, width, px(36));
            const int groupHeight = px(22) + coverageHeight + gap + qualityHeight + padding;
            position(coverageGroup, outer, y, blockWidth, groupHeight);
            position(coverageText, left, y + px(20), width, coverageHeight);
            position(qualityText, left, y + px(20) + coverageHeight + gap, width, qualityHeight);
            y += groupHeight + gap;
            if (viewModel.codexThroughput.hasData) {
                const int codexHeight = textHeight(codexText, width, px(72));
                position(codexGroup, outer, y, blockWidth, codexHeight + px(30));
                position(codexText, left, y + px(20), width, codexHeight);
                y += codexHeight + px(30) + gap;
            }
            contentHeight = y;
            return;
        }

        const bool quota = page == AnalysisPage::Quota;
        const bool hasQuota = selectedQuotaSeries() != nullptr;
        int cursor = y + px(20);
        if (quota && hasQuota) {
            const int labelWidth = std::min(px(98), width / 3);
            position(quotaSeriesLabel, left, cursor + px(2), labelWidth, px(22));
            position(quotaSeriesCombo, left + labelWidth, cursor, std::min(px(340), width - labelWidth), px(230));
            cursor += px(29);
        }
        const int summaryHeight = textHeight(trendSummary, width, px(20));
        position(trendSummary, left, cursor, width, summaryHeight);
        cursor += summaryHeight + px(3);
        if (!quota || hasQuota) {
            if (!quota) {
                const int explanationHeight = textHeight(activityExplanation, width, px(23));
                position(activityExplanation, left, cursor, width, explanationHeight);
                cursor += explanationHeight;
            }
            const int chartHeight = px(quota ? 220 : 190);
            position(chart, left, cursor, width, chartHeight);
            cursor += chartHeight + px(5);
            const int detailHeight = px(44);
            position(trendDetail, left, cursor, width, detailHeight);
            cursor += detailHeight;
        }
        position(chartGroup, outer, y, blockWidth, cursor - y + padding);
        y = cursor + padding + gap;
        if (quota && hasQuota) {
            const int comparisonHeight = textHeight(quotaComparison, width, px(36));
            position(quotaGroup, outer, y, blockWidth, comparisonHeight + px(28));
            position(quotaComparison, left, y + px(20), width, comparisonHeight);
            y += comparisonHeight + px(28) + gap;
        } else if (!quota) {
            const int dailyHeight = px(178);
            layoutList(dailyGroup, dailyList, kDailyColumns, outer, y, blockWidth, dailyHeight);
            y += dailyHeight + gap;
            const int listHeight = px(165);
            const bool sideBySide = blockWidth >= px(910);
            const int half = (blockWidth - gap) / 2;
            layoutList(providerGroup, providerList, kProviderColumns, outer, y, sideBySide ? half : blockWidth,
                       listHeight);
            if (sideBySide) {
                layoutList(accountGroup, accountList, kAccountColumns, outer + half + gap, y, blockWidth - half - gap,
                           listHeight);
            } else {
                y += listHeight + gap;
                layoutList(accountGroup, accountList, kAccountColumns, outer, y, blockWidth, listHeight);
            }
            y += listHeight + gap;
        }
        contentHeight = y;
    }

    int maxScrollOffset() const {
        return std::max(0, contentHeight - viewportHeight);
    }

    void updateScrollBar() {
        SCROLLINFO info{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL};
        info.nMin = 0;
        info.nMax = std::max(0, contentHeight - 1);
        info.nPage = static_cast<UINT>(viewportHeight);
        info.nPos = scrollOffset;
        SetScrollInfo(parent, SB_VERT, &info, TRUE);
    }

    bool setScrollOffset(int value) {
        const int updated = std::clamp(value, 0, maxScrollOffset());
        if (updated == scrollOffset) {
            return false;
        }
        scrollOffset = updated;
        SetWindowPos(body, nullptr, 0, -scrollOffset, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        updateScrollBar();
        return true;
    }

    template <typename Option> static std::string selectedKey(HWND combo, const std::vector<Option>& options) {
        const LRESULT index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
        return index >= 0 && index < static_cast<LRESULT>(options.size()) ? options[index].key : std::string{};
    }

    template <typename Option>
    static void bindCombo(HWND combo, const std::vector<Option>& options, const std::string& selection, bool changed) {
        if (changed || SendMessageW(combo, CB_GETCOUNT, 0, 0) != static_cast<LRESULT>(options.size())) {
            SendMessageW(combo, CB_RESETCONTENT, 0, 0);
            for (const auto& option : options) {
                const auto label = utf8ToWide(option.label);
                SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            }
        }
        const auto selected =
            std::find_if(options.begin(), options.end(), [&](const auto& option) { return option.key == selection; });
        const auto index = selected == options.end() ? -1 : std::distance(options.begin(), selected);
        if (SendMessageW(combo, CB_GETCURSEL, 0, 0) != index) {
            SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
        }
    }

    void updateScopeStatus() const {
        if (interactionPending) {
            updateLayoutText(scopeStatus, kLoadingText);
            return;
        }
        const std::string start = formatUtcTime(viewModel.effectiveStartUtc);
        const std::string end = formatUtcTime(viewModel.effectiveEndUtc);
        updateLayoutText(scopeStatus, start.empty() || end.empty() ? "" : start + " to " + end + " UTC");
    }

    bool showsCustomDates() const {
        return customDateEditorSelected || customDateEditPending ||
               currentScope.dateRangePreset == AnalysisDateRangePreset::Custom;
    }

    void updateScopeControls() const {
        const bool custom = shown && showsCustomDates();
        const HWND customControls[] = {customStartLabel, customStartEdit, customEndLabel, customEndEdit,
                                       customDateHint};
        for (HWND control : customControls) {
            toggle(control, custom);
        }
        const HWND inputs[] = {providerCombo, accountCombo, rangeCombo, customStartEdit, customEndEdit};
        for (HWND input : inputs) {
            if (input != nullptr) {
                EnableWindow(input, !interactionPending);
            }
        }
    }

    void onCustomDateChanged(WORD notifyCode) {
        if (notifyCode == EN_KILLFOCUS) {
            commitCustomDateEdit();
            return;
        }
        const bool hadCustomDates = showsCustomDates();
        customDateEditPending = true;
        customDateEditorSelected = true;
        if (!hadCustomDates) {
            SendMessageW(rangeCombo, CB_SETCURSEL, static_cast<WPARAM>(AnalysisDateRangePreset::Custom), 0);
            updateScopeControls();
            if (hasBounds) {
                layout(bounds);
            }
        }
        if (SetTimer(parent, kCustomDateDebounceTimerId, 350, nullptr) == 0) {
            commitCustomDateEdit();
        }
    }

    void cancelCustomDateEdit() {
        customDateEditPending = false;
        if (parent != nullptr) {
            KillTimer(parent, kCustomDateDebounceTimerId);
        }
    }

    bool commitCustomDates(AnalysisScopeState& scope) {
        const auto start = parseReportDay(getWindowTextUtf8(customStartEdit));
        const auto end = parseReportDay(getWindowTextUtf8(customEndEdit));
        customDateError = !start || !end || *start > *end;
        SetWindowTextW(customDateHint, customDateError ? L"Enter valid dates with From on or before To (YYYY-MM-DD)."
                                                       : kCustomDateInstruction);
        if (customDateError) {
            return false;
        }

        scope.dateRangePreset = AnalysisDateRangePreset::Custom;
        scope.customRangeStartUtc = *start;
        scope.customRangeEndUtc = *end;
        return true;
    }

    void commitCustomDateEdit() {
        if (!customDateEditPending) {
            return;
        }
        cancelCustomDateEdit();
        AnalysisScopeState scope = currentScope;
        if (commitCustomDates(scope)) {
            publishScope(scope);
        }
    }

    void publishScope(const AnalysisScopeState& scope) {
        if (scopesMatch(scope, currentScope)) {
            return;
        }
        currentScope = scope;
        if (scopeChangedCallback) {
            awaitingScope = scope;
            scopeChangedCallback(scope);
        }
    }

    static void refreshList(HWND list, size_t count) {
        if (ListView_GetItemCount(list) != static_cast<int>(count)) {
            ListView_SetItemCountEx(list, static_cast<int>(count), LVSICF_NOSCROLL);
        }
        InvalidateRect(list, nullptr, FALSE);
    }

    std::string listCell(HWND list, int row, int column) const {
        const auto index = static_cast<size_t>(row);
        if (list == dailyList) {
            if (index >= viewModel.dailySummaries.size()) {
                return {};
            }
            const auto& day = viewModel.dailySummaries[index];
            switch (column) {
                case 0:
                    return day.dateLabel;
                case 1:
                    return day.activityCount > 0 || day.gapDetail.classification == AnalysisGapClassification::KnownZero
                               ? formatNumber(day.activityCount)
                               : "\xE2\x80\x94";
                case 2:
                    return day.sourceLabel;
                case 3:
                    return buildDailyProvenanceText(day);
                default:
                    return {};
            }
        }
        const bool account = list == accountList;
        if (!account && list != providerList) {
            return {};
        }
        const auto& rows = account ? viewModel.accountBreakdown : viewModel.providerBreakdown;
        if (index >= rows.size()) {
            return {};
        }
        const auto& datum = rows[index];
        if (column == 0) {
            return datum.label;
        }
        if (account && column == 1) {
            return datum.secondaryLabel;
        }
        return column == (account ? 2 : 1) ? formatNumber(datum.activityCount) : datum.sourceLabel;
    }

    void syncPage() {
        scrollOffset = 0;
        refreshTrendControls();
        syncPageVisibility();
        if (hasBounds) {
            layout(bounds);
        }
    }

    void syncPageVisibility() const {
        const bool activity = page == AnalysisPage::Activity;
        const bool quota = page == AnalysisPage::Quota;
        const bool coverage = page == AnalysisPage::Coverage;
        const bool hasQuota = selectedQuotaSeries() != nullptr;
        toggle(chartGroup, activity || quota);
        toggle(trendSummary, activity || quota);
        toggle(chart, activity || (quota && hasQuota));
        toggle(trendDetail, activity || (quota && hasQuota));
        toggle(quotaSeriesLabel, quota && hasQuota);
        toggle(quotaSeriesCombo, quota && hasQuota);
        toggle(quotaGroup, quota && hasQuota);
        toggle(quotaComparison, quota && hasQuota);
        const HWND activityControls[] = {activityExplanation, dailyGroup,   dailyList,  providerGroup,
                                         providerList,        accountGroup, accountList};
        for (HWND control : activityControls) {
            toggle(control, activity);
        }
        toggle(coverageGroup, coverage);
        toggle(coverageText, coverage);
        toggle(qualityText, coverage);
        toggle(codexGroup, coverage && viewModel.codexThroughput.hasData);
        toggle(codexText, coverage && viewModel.codexThroughput.hasData);
    }

    const AnalysisQuotaTrendSeries* selectedQuotaSeries() const {
        const auto it = std::find_if(
            viewModel.quotaTrendSeries.begin(), viewModel.quotaTrendSeries.end(),
            [this](const auto& series) { return series.key == selectedQuotaSeriesKey && !series.points.empty(); });
        return it == viewModel.quotaTrendSeries.end() ? nullptr : &*it;
    }

    void selectExistingQuotaSeries() {
        if (selectedQuotaSeries() != nullptr) {
            return;
        }
        const auto next = std::find_if(viewModel.quotaTrendSeries.begin(), viewModel.quotaTrendSeries.end(),
                                       [](const auto& series) { return !series.points.empty(); });
        selectedQuotaSeriesKey = next != viewModel.quotaTrendSeries.end() ? next->key : std::string{};
    }

    std::time_t selectedQuotaTime() const {
        const auto* series = selectedQuotaSeries();
        return series != nullptr && selectedQuotaPoint >= 0 &&
                       selectedQuotaPoint < static_cast<int>(series->points.size())
                   ? series->points[selectedQuotaPoint].snapshotTime
                   : 0;
    }

    std::time_t selectedActivityTime() const {
        return selectedActivityPoint >= 0 && selectedActivityPoint < static_cast<int>(viewModel.trendPoints.size())
                   ? viewModel.trendPoints[selectedActivityPoint].bucketStartUtc
                   : 0;
    }

    template <typename Point, typename ReadTime>
    static int pointIndex(const std::vector<Point>& points, std::time_t timestamp, ReadTime readTime) {
        if (points.empty()) {
            return -1;
        }
        if (timestamp > 0) {
            const auto retained =
                std::lower_bound(points.begin(), points.end(), timestamp,
                                 [&](const Point& point, auto time) { return readTime(point) < time; });
            if (retained != points.end() && readTime(*retained) == timestamp) {
                return static_cast<int>(std::distance(points.begin(), retained));
            }
        }
        return static_cast<int>(points.size()) - 1;
    }

    std::string quotaSummary() const {
        const auto* series = selectedQuotaSeries();
        if (series == nullptr) {
            if (viewModel.scope.selectedProviderId.empty() || viewModel.scope.selectedAccountId.empty()) {
                return "Choose a provider and an account to see recorded quota history.";
            }
            return "No quota samples were recorded for this account in the selected range.";
        }
        const auto& latest = series->points.back();
        return "Latest in range: " + quotaAmount(*series, latest) +
               " remaining. Lowest recorded: " + formatPercentage(series->lowWaterRemainingPercentage) + ". " +
               formatNumber(series->resetCount) + " " +
               pluralize(series->resetCount, "recorded reset.", "recorded resets.");
    }

    std::string quotaComparisonSummary() const {
        std::string result;
        for (const auto& series : viewModel.quotaTrendSeries) {
            if (series.points.empty()) {
                continue;
            }
            const auto& point = series.points.back();
            if (!result.empty()) {
                result += "\r\n";
            }
            result += series.label + ": " + quotaAmount(series, point) + " remaining";
            const std::string time = formatUtcTime(point.snapshotTime);
            if (!time.empty()) {
                result += " on " + time + " UTC";
            }
        }
        return result;
    }

    std::string selectedDetail() const {
        if (page == AnalysisPage::Quota) {
            const auto* series = selectedQuotaSeries();
            if (series == nullptr || selectedQuotaPoint < 0 ||
                selectedQuotaPoint >= static_cast<int>(series->points.size())) {
                return {};
            }
            const auto& point = series->points[selectedQuotaPoint];
            const std::string value = formatUtcTime(point.snapshotTime) + " UTC: " + quotaAmount(*series, point);
            if (point.dailyRollup) {
                return value + " remaining at the last capture. " +
                       (point.resetPoint ? "Reset recorded that day. " : "") +
                       (point.reconciled ? "Reconciled capture recorded that day." : "");
            }
            return value + " remaining. " + (point.resetPoint ? "Recorded reset. " : "") +
                   (point.reconciled ? "Reconciled sample." : "Unreconciled sample.");
        }
        if (selectedActivityPoint < 0 || selectedActivityPoint >= static_cast<int>(viewModel.trendPoints.size())) {
            return {};
        }
        const auto& point = viewModel.trendPoints[selectedActivityPoint];
        const std::string start = formatUtcTime(point.bucketStartUtc);
        const std::string finish = formatUtcTime(point.bucketEndUtc);
        const std::string source = fallbackText(point.sourceLabel, "Unknown");
        const std::string range = start.empty() || finish.empty() ? point.dateLabel : start + " to " + finish + " UTC";
        if (point.activityCount == 0) {
            return range + ": No recorded count for this interval.";
        }
        return range + ": " + formatNumber(point.activityCount) + " " +
               pluralize(point.activityCount, "record", "records") + ". Source: " + source + ".";
    }

    bool refreshTrendControls() const {
        if (page == AnalysisPage::Coverage) {
            return false;
        }
        const bool quota = page == AnalysisPage::Quota;
        updateLayoutText(chartGroup, quota ? "Subscription quota remaining" : "Recorded activity");
        bool layoutChanged = updateLayoutText(trendSummary, quota ? quotaSummary() : activitySummary(viewModel));
        updateLayoutText(trendDetail, selectedDetail());
        if (quota) {
            layoutChanged |= updateLayoutText(quotaComparison, quotaComparisonSummary());
        }
        InvalidateRect(chart, nullptr, FALSE);
        return layoutChanged;
    }

    bool selectPoint(int index) {
        const auto* series = selectedQuotaSeries();
        const bool quota = page == AnalysisPage::Quota;
        const int count = quota ? (series == nullptr ? 0 : static_cast<int>(series->points.size()))
                                : static_cast<int>(viewModel.trendPoints.size());
        if (index < 0 || index >= count) {
            return false;
        }
        (quota ? selectedQuotaPoint : selectedActivityPoint) = index;
        updateLayoutText(trendDetail, selectedDetail());
        InvalidateRect(chart, nullptr, FALSE);
        return true;
    }

    static LRESULT CALLBACK parentSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR,
                                           DWORD_PTR context) {
        auto& self = *reinterpret_cast<Impl*>(context);
        if (message == WM_NOTIFY && lParam != 0) {
            const auto& notification = *reinterpret_cast<const NMHDR*>(lParam);
            if (notification.hwndFrom == self.tabs && notification.code == TCN_SELCHANGE) {
                self.page = static_cast<AnalysisPage>(TabCtrl_GetCurSel(self.tabs));
                self.syncPage();
                return 0;
            }
        }
        if (message == WM_DPICHANGED && lParam != 0) {
            self.applyFonts();
            const auto& suggested = *reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested.left, suggested.top, suggested.right - suggested.left,
                         suggested.bottom - suggested.top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    static LRESULT CALLBACK bodySubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR,
                                         DWORD_PTR context) {
        auto& self = *reinterpret_cast<Impl*>(context);
        switch (message) {
            case WM_COMMAND:
                if (self.handleCommand(LOWORD(wParam), HIWORD(wParam))) {
                    return 0;
                }
                break;
            case WM_NOTIFY:
                if (lParam != 0) {
                    auto& info = *reinterpret_cast<NMLVDISPINFOW*>(lParam);
                    if (info.hdr.code == LVN_GETDISPINFOW && (info.item.mask & LVIF_TEXT) != 0 &&
                        info.item.pszText != nullptr && info.item.cchTextMax > 0) {
                        const auto text =
                            utf8ToWide(self.listCell(info.hdr.hwndFrom, info.item.iItem, info.item.iSubItem));
                        lstrcpynW(info.item.pszText, text.c_str(), info.item.cchTextMax);
                        return 0;
                    }
                    if (info.hdr.code == NM_SETFOCUS) {
                        self.ensureVisible(info.hdr.hwndFrom);
                    }
                }
                break;
            case WM_MOUSEWHEEL:
                if (self.handleMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam))) {
                    return 0;
                }
                break;
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    void ensureVisible(HWND control) {
        const RECT rect = windowRectRelativeToParent(viewport, control);
        if (rect.top < 0) {
            setScrollOffset(scrollOffset + rect.top - px(4));
        } else if (rect.bottom > viewportHeight) {
            const int distance =
                std::min(static_cast<int>(rect.top) - px(4), static_cast<int>(rect.bottom) - viewportHeight);
            setScrollOffset(scrollOffset + distance);
        }
    }

    static LRESULT CALLBACK chartWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
        }
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        return self == nullptr ? DefWindowProcW(window, message, wParam, lParam)
                               : self->handleChart(window, message, wParam, lParam);
    }

    RECT plotBounds(RECT client) const {
        client.left += px(55);
        client.right -= px(20);
        client.top += px(18);
        client.bottom -= px(32);
        return client;
    }

    int activityX(size_t index, const RECT& plot) const {
        const auto count = viewModel.trendPoints.size();
        const long long width = plot.right - plot.left;
        return plot.left +
               static_cast<int>(width * (static_cast<long long>(index) * 2 + 1) / (2 * static_cast<long long>(count)));
    }

    static int quotaX(const AnalysisQuotaTrendSeries& series, size_t index, const RECT& plot) {
        const long long width = plot.right - plot.left;
        const auto& points = series.points;
        const auto elapsed = points.back().snapshotTime - points.front().snapshotTime;
        return plot.left +
               (elapsed > 0
                    ? static_cast<int>(width * (points[index].snapshotTime - points.front().snapshotTime) / elapsed)
                    : static_cast<int>(width / 2));
    }

    static int plotY(double ratio, const RECT& plot) {
        return plot.bottom - static_cast<int>((plot.bottom - plot.top) * std::clamp(ratio, 0.0, 1.0));
    }

    static void drawAxisText(HDC dc, const std::string& label, RECT area, UINT flags) {
        const auto wide = utf8ToWide(label);
        DrawTextW(dc, wide.c_str(), -1, &area, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | flags);
    }

    void drawScales(HDC dc, const RECT& plot, bool quota, long long maxRecords, const std::string& first,
                    const std::string& last) const {
        const auto oldPen = SelectObject(dc, GetStockObject(DC_PEN));
        SetDCPenColor(dc, GetSysColor(COLOR_BTNSHADOW));
        MoveToEx(dc, plot.left, plot.top, nullptr);
        LineTo(dc, plot.left, plot.bottom);
        LineTo(dc, plot.right + 1, plot.bottom);
        SelectObject(dc, oldPen);

        const RECT topLabel{0, plot.top - px(8), plot.left - px(5), plot.top + px(12)};
        const RECT bottomLabel{0, plot.bottom - px(8), plot.left - px(5), plot.bottom + px(12)};
        drawAxisText(dc, quota ? "100%" : formatNumber(maxRecords), topLabel, DT_RIGHT);
        drawAxisText(dc, quota ? "0%" : "0", bottomLabel, DT_RIGHT);
        const int midpoint = plot.left + (plot.right - plot.left) / 2;
        drawAxisText(dc, first, RECT{plot.left, plot.bottom + px(5), midpoint, plot.bottom + px(24)}, DT_LEFT);
        drawAxisText(dc, last, RECT{midpoint, plot.bottom + px(5), plot.right, plot.bottom + px(24)}, DT_RIGHT);
    }

    void drawActivity(HDC dc, const RECT& plot) const {
        const auto& points = viewModel.trendPoints;
        const auto peak = std::max_element(points.begin(), points.end(), [](const auto& a, const auto& b) {
            return a.activityCount < b.activityCount;
        });
        if (peak == points.end() || peak->activityCount == 0) {
            return;
        }
        drawScales(dc, plot, false, peak->activityCount, points.front().dateLabel, points.back().dateLabel);
        const int slot = static_cast<int>((plot.right - plot.left) / static_cast<long long>(points.size()));
        const int halfBar = std::max(1, std::min(px(11), (slot - 2) / 2));
        const auto oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
        const auto barBrush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
        for (size_t index = 0; index < points.size(); ++index) {
            const int x = activityX(index, plot);
            const bool selected = static_cast<int>(index) == selectedActivityPoint;
            SetDCBrushColor(dc, GetSysColor(selected ? COLOR_WINDOWTEXT : COLOR_HIGHLIGHT));
            if (points[index].activityCount > 0) {
                const int y = plotY(static_cast<double>(points[index].activityCount) / peak->activityCount, plot);
                const RECT bar{x - halfBar + 1, std::min<int>(y, plot.bottom - 1), x + halfBar, plot.bottom};
                FillRect(dc, &bar, barBrush);
            }
            if (selected) {
                const RECT indicator{x - px(2), plot.bottom + 1, x + px(2) + 1, plot.bottom + px(5)};
                FillRect(dc, &indicator, barBrush);
            }
        }
        SelectObject(dc, oldBrush);
    }

    void drawQuota(HDC dc, const RECT& plot, const AnalysisQuotaTrendSeries& series) const {
        const auto& points = series.points;
        drawScales(dc, plot, true, 100, formatUtcTime(points.front().snapshotTime),
                   points.size() == 1 ? "" : formatUtcTime(points.back().snapshotTime));

        const auto oldPen = SelectObject(dc, GetStockObject(DC_PEN));
        for (size_t index = 0; index < points.size(); ++index) {
            if (points[index].resetPoint && !points[index].dailyRollup) {
                const int x = quotaX(series, index, plot);
                SetDCPenColor(dc, GetSysColor(COLOR_BTNSHADOW));
                MoveToEx(dc, x, plot.top, nullptr);
                LineTo(dc, x, plot.bottom);
            }
        }
        SetDCPenColor(dc, GetSysColor(COLOR_HIGHLIGHT));
        MoveToEx(dc, quotaX(series, 0, plot), plotY(points.front().remainingPercentage / 100.0, plot), nullptr);
        for (size_t index = 1; index < points.size(); ++index) {
            LineTo(dc, quotaX(series, index, plot), plotY(points[index].remainingPercentage / 100.0, plot));
        }
        SelectObject(dc, oldPen);

        const auto oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
        const auto markerBrush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
        for (size_t index = 0; index < points.size(); ++index) {
            const int x = quotaX(series, index, plot);
            const int y = plotY(points[index].remainingPercentage / 100.0, plot);
            const bool selected = static_cast<int>(index) == selectedQuotaPoint;
            const int radius = px(selected ? 4 : 2);
            SetDCBrushColor(dc, GetSysColor(selected ? COLOR_WINDOWTEXT : COLOR_HIGHLIGHT));
            const RECT marker{x - radius, y - radius, x + radius + 1, y + radius + 1};
            FillRect(dc, &marker, markerBrush);
        }
        SelectObject(dc, oldBrush);
    }

    LRESULT handleChart(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_GETDLGCODE:
                return DLGC_WANTARROWS;
            case WM_SETFOCUS:
                ensureVisible(window);
                [[fallthrough]];
            case WM_KILLFOCUS:
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            case WM_KEYDOWN:
                if (wParam == VK_LEFT || wParam == VK_RIGHT) {
                    const int current = page == AnalysisPage::Quota ? selectedQuotaPoint : selectedActivityPoint;
                    selectPoint(current + (wParam == VK_LEFT ? -1 : 1));
                    return 0;
                }
                break;
            case WM_LBUTTONDOWN: {
                SetFocus(window);
                RECT client{};
                GetClientRect(window, &client);
                const RECT plot = plotBounds(client);
                const int x = GET_X_LPARAM(lParam);
                if (plot.right <= plot.left || x < plot.left || x > plot.right) {
                    return 0;
                }
                if (page == AnalysisPage::Quota) {
                    if (const auto* series = selectedQuotaSeries()) {
                        size_t nearest = 0;
                        for (size_t i = 1; i < series->points.size(); ++i) {
                            if (std::abs(quotaX(*series, i, plot) - x) < std::abs(quotaX(*series, nearest, plot) - x)) {
                                nearest = i;
                            }
                        }
                        selectPoint(static_cast<int>(nearest));
                    }
                } else if (!viewModel.trendPoints.empty()) {
                    const auto count = static_cast<int>(viewModel.trendPoints.size());
                    const auto index =
                        static_cast<int>(static_cast<long long>(x - plot.left) * count / (plot.right - plot.left));
                    selectPoint(std::min(count - 1, index));
                }
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                const HDC dc = BeginPaint(window, &paint);
                RECT client{};
                GetClientRect(window, &client);
                FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));
                const auto oldFont = font != nullptr ? SelectObject(dc, font) : nullptr;
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
                const RECT plot = plotBounds(client);
                if (plot.right > plot.left && plot.bottom > plot.top) {
                    if (page == AnalysisPage::Quota) {
                        if (const auto* series = selectedQuotaSeries()) {
                            drawQuota(dc, plot, *series);
                        }
                    } else if (std::any_of(viewModel.trendPoints.begin(), viewModel.trendPoints.end(),
                                           [](const auto& point) { return point.activityCount > 0; })) {
                        drawActivity(dc, plot);
                    } else {
                        DrawTextW(dc, L"No recorded activity to plot", -1, &client,
                                  DT_VCENTER | DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
                    }
                }
                if (GetFocus() == window) {
                    InflateRect(&client, -px(2), -px(2));
                    DrawFocusRect(dc, &client);
                }
                if (oldFont != nullptr) {
                    SelectObject(dc, oldFont);
                }
                EndPaint(window, &paint);
                return 0;
            }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

#ifdef JISTATS_ENABLE_TEST_HOOKS
  public:
    std::string getScopeLabelForTesting() const {
        return getWindowTextUtf8(scopeSummary);
    }
    std::string getStatusHeadlineForTesting() const {
        return getWindowTextUtf8(scopeStatus);
    }
    std::string getTrendSummaryForTesting() const {
        return getWindowTextUtf8(trendSummary);
    }
    std::string getTrendComparisonTextForTesting() const {
        return getWindowTextUtf8(quotaComparison);
    }
    std::string getTrendDetailTextForTesting() const {
        return getWindowTextUtf8(trendDetail);
    }
    std::string getCodexTextForTesting() const {
        return getWindowTextUtf8(codexText);
    }
    std::string getCustomDateHintForTesting() const {
        return getWindowTextUtf8(customDateHint);
    }
    std::vector<std::string> getProviderOptionsForTesting() const {
        return comboLabels(providerCombo);
    }
    std::vector<std::string> getAccountOptionsForTesting() const {
        return comboLabels(accountCombo);
    }
    std::vector<std::string> getRangeOptionsForTesting() const {
        return comboLabels(rangeCombo);
    }
    std::vector<std::string> getQuotaSeriesOptionsForTesting() const {
        return comboLabels(quotaSeriesCombo);
    }
    bool areScopeInputsEnabledForTesting() const {
        return IsWindowEnabled(providerCombo) && IsWindowEnabled(accountCombo) && IsWindowEnabled(rangeCombo) &&
               (!showsCustomDates() || (IsWindowEnabled(customStartEdit) && IsWindowEnabled(customEndEdit)));
    }
    RECT getSectionBoundsForTesting(const std::string& section) const {
        HWND hwnd = nullptr;
        if (section == "scope")
            hwnd = scopeGroup;
        if (section == "trend")
            hwnd = chartGroup;
        if (section == "provider")
            hwnd = providerGroup;
        if (section == "account")
            hwnd = accountGroup;
        if (section == "daily")
            hwnd = dailyGroup;
        if (section == "provenance")
            hwnd = coverageGroup;
        return windowRectRelativeToParent(parent, hwnd);
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
    bool selectTrendBucketForTesting(size_t index) {
        return page == AnalysisPage::Activity && selectPoint(static_cast<int>(index));
    }
    bool selectQuotaTrendPointForTesting(size_t index) {
        return page == AnalysisPage::Quota && selectPoint(static_cast<int>(index));
    }
    bool selectProviderForTesting(const std::string& key) {
        return selectOption(providerCombo, kProviderComboId, viewModel.availableProviders, key);
    }
    bool selectAccountForTesting(const std::string& key) {
        return selectOption(accountCombo, kAccountComboId, viewModel.availableAccounts, key);
    }
    bool selectRangePresetForTesting(AnalysisDateRangePreset preset) {
        SendMessageW(rangeCombo, CB_SETCURSEL, static_cast<WPARAM>(preset), 0);
        return handleCommand(kRangeComboId, CBN_SELCHANGE);
    }
    bool selectQuotaSeriesForTesting(const std::string& key) {
        return page == AnalysisPage::Quota &&
               selectOption(quotaSeriesCombo, kQuotaSeriesComboId, viewModel.quotaTrendSeries, key);
    }
    void setCustomRangeTextForTesting(const std::string& startDate, const std::string& endDate) {
        suppressNotification = true;
        setWindowTextUtf8(customStartEdit, startDate);
        setWindowTextUtf8(customEndEdit, endDate);
        suppressNotification = false;
    }

  private:
    static std::vector<std::string> comboLabels(HWND combo) {
        std::vector<std::string> labels;
        const LRESULT count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
        for (LRESULT index = 0; index < count; ++index) {
            const auto length = SendMessageW(combo, CB_GETLBTEXTLEN, static_cast<WPARAM>(index), 0);
            if (length == CB_ERR) {
                continue;
            }
            std::wstring label(static_cast<size_t>(length + 1), L'\0');
            SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(label.data()));
            label.resize(length);
            labels.push_back(wideToUtf8(label));
        }
        return labels;
    }

    template <typename Option>
    bool selectOption(HWND combo, WORD id, const std::vector<Option>& options, const std::string& key) {
        const auto it =
            std::find_if(options.begin(), options.end(), [&](const auto& option) { return option.key == key; });
        if (it == options.end()) {
            return false;
        }
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(std::distance(options.begin(), it)), 0);
        return handleCommand(id, CBN_SELCHANGE);
    }
#endif
};

AnalysisPresenter::AnalysisPresenter() : pImpl(std::make_unique<Impl>()) {}
AnalysisPresenter::~AnalysisPresenter() = default;

bool AnalysisPresenter::init(HWND parent) {
    return pImpl->init(parent);
}
void AnalysisPresenter::shutdown() {
    pImpl->shutdown();
}
void AnalysisPresenter::updateViewModel(const AnalysisViewModel& viewModel) {
    pImpl->updateViewModel(viewModel);
}
void AnalysisPresenter::setInteractionPending(bool pending) {
    pImpl->setInteractionPending(pending);
}
void AnalysisPresenter::setScopeChangedCallback(std::function<void(const AnalysisScopeState&)> callback) {
    pImpl->scopeChangedCallback = std::move(callback);
}
void AnalysisPresenter::setVisible(bool visible) {
    pImpl->setVisible(visible);
}
void AnalysisPresenter::layout(const RECT& bounds) {
    pImpl->layout(bounds);
}
bool AnalysisPresenter::handleCommand(WORD id, WORD notifyCode) {
    return pImpl->handleCommand(id, notifyCode);
}
bool AnalysisPresenter::handleTimer(UINT_PTR timerId) {
    return pImpl->handleTimer(timerId);
}
bool AnalysisPresenter::handleVScroll(WORD request, WORD thumbPosition) {
    return pImpl->handleVScroll(request, thumbPosition);
}
bool AnalysisPresenter::handleMouseWheel(short wheelDelta) {
    return pImpl->handleMouseWheel(wheelDelta);
}

#ifdef JISTATS_ENABLE_TEST_HOOKS
std::string AnalysisPresenter::getScopeLabelForTesting() const {
    return pImpl->getScopeLabelForTesting();
}
std::string AnalysisPresenter::getStatusHeadlineForTesting() const {
    return pImpl->getStatusHeadlineForTesting();
}
std::string AnalysisPresenter::getCodexTextForTesting() const {
    return pImpl->getCodexTextForTesting();
}
std::string AnalysisPresenter::getTrendSummaryForTesting() const {
    return pImpl->getTrendSummaryForTesting();
}
std::vector<std::string> AnalysisPresenter::getProviderOptionsForTesting() const {
    return pImpl->getProviderOptionsForTesting();
}
std::vector<std::string> AnalysisPresenter::getAccountOptionsForTesting() const {
    return pImpl->getAccountOptionsForTesting();
}
std::vector<std::string> AnalysisPresenter::getRangeOptionsForTesting() const {
    return pImpl->getRangeOptionsForTesting();
}
std::vector<std::string> AnalysisPresenter::getQuotaSeriesOptionsForTesting() const {
    return pImpl->getQuotaSeriesOptionsForTesting();
}
std::string AnalysisPresenter::getTrendComparisonTextForTesting() const {
    return pImpl->getTrendComparisonTextForTesting();
}
std::string AnalysisPresenter::getTrendDetailTextForTesting() const {
    return pImpl->getTrendDetailTextForTesting();
}
std::string AnalysisPresenter::getCustomDateHintForTesting() const {
    return pImpl->getCustomDateHintForTesting();
}
bool AnalysisPresenter::areScopeInputsEnabledForTesting() const {
    return pImpl->areScopeInputsEnabledForTesting();
}
RECT AnalysisPresenter::getSectionBoundsForTesting(const std::string& sectionKey) const {
    return pImpl->getSectionBoundsForTesting(sectionKey);
}
int AnalysisPresenter::getScrollOffsetForTesting() const {
    return pImpl->getScrollOffsetForTesting();
}
int AnalysisPresenter::getContentHeightForTesting() const {
    return pImpl->getContentHeightForTesting();
}
int AnalysisPresenter::getViewportHeightForTesting() const {
    return pImpl->getViewportHeightForTesting();
}
bool AnalysisPresenter::selectTrendBucketForTesting(size_t index) {
    return pImpl->selectTrendBucketForTesting(index);
}
bool AnalysisPresenter::selectQuotaTrendPointForTesting(size_t index) {
    return pImpl->selectQuotaTrendPointForTesting(index);
}
bool AnalysisPresenter::selectProviderForTesting(const std::string& key) {
    return pImpl->selectProviderForTesting(key);
}
bool AnalysisPresenter::selectAccountForTesting(const std::string& key) {
    return pImpl->selectAccountForTesting(key);
}
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
