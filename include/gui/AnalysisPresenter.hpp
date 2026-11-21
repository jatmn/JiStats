#pragma once

#include "gui/AnalysisViewModel.hpp"

#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace JiStats {

/**
 * Owns the Analysis tab scope controls and in-tab analysis surface.
 * DashboardPresenter hosts this as a sub-presenter, but the analysis
 * summaries, breakdowns, trend text, and provenance remain presenter-owned here.
 */
class AnalysisPresenter {
public:
    AnalysisPresenter();
    ~AnalysisPresenter();

    bool init(HWND parent);
    void shutdown();

    void updateViewModel(const AnalysisViewModel& viewModel);
    void setInteractionPending(bool pending);

    void setScopeChangedCallback(
        std::function<void(const AnalysisScopeState&)> callback);

    void setVisible(bool visible);
    void layout(const RECT& bounds);

    // Returns true if the command was handled.
    bool handleCommand(WORD id, WORD notifyCode = 0);
    bool handleTimer(UINT_PTR timerId);
    bool handleVScroll(WORD request, WORD thumbPosition = 0);
    bool handleMouseWheel(short wheelDelta);
    bool handleDrawItem(const DRAWITEMSTRUCT& drawItem);

#ifdef JISTATS_ENABLE_TEST_HOOKS
    std::string getScopeLabelForTesting() const;
    std::string getStatusHeadlineForTesting() const;
    std::string getOverviewStatsForTesting() const;
    std::string getTrendSummaryForTesting() const;
    std::vector<std::string> getSectionTitlesForTesting() const;
    std::vector<std::string> getProviderOptionsForTesting() const;
    std::vector<std::string> getAccountOptionsForTesting() const;
    std::vector<std::string> getRangeOptionsForTesting() const;
    std::vector<std::string> getQuotaSeriesOptionsForTesting() const;
    std::string getWorkspaceGuideTextForTesting() const;
    std::vector<std::string> getWorkspaceNavigationLabelsForTesting() const;
    std::string getTrendComparisonTextForTesting() const;
    std::string getTrendDetailTextForTesting() const;
    std::string getCustomDateHintForTesting() const;
    bool areScopeInputsEnabledForTesting() const;
    RECT getSectionBoundsForTesting(const std::string& sectionKey) const;
    int getScrollOffsetForTesting() const;
    int getContentHeightForTesting() const;
    int getViewportHeightForTesting() const;
    bool jumpToSectionForTesting(const std::string& sectionKey);
    bool selectTrendBucketForTesting(size_t index);
    bool selectQuotaTrendPointForTesting(size_t index);
    bool selectProviderForTesting(const std::string& key);
    bool selectAccountForTesting(const std::string& key);
    bool selectRangePresetForTesting(AnalysisDateRangePreset preset);
    bool selectQuotaSeriesForTesting(const std::string& key);
    void setCustomRangeTextForTesting(const std::string& startDate, const std::string& endDate);
#endif

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace JiStats
