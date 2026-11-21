#pragma once

#include "gui/AnalysisViewModel.hpp"
#include "reporting/ReportingService.hpp"

#include <ctime>
#include <string>

namespace JiStats {

struct DashboardViewModel;
struct DashboardPreferences;
class StorageService;

struct ResolvedAnalysisWindow {
    std::time_t start = 0;
    std::time_t end = 0;
    std::string label;
    AnalysisDateRangePreset effectivePreset = AnalysisDateRangePreset::Last7Days;
    ReportDateRange reportDateRange;
};

/**
 * Owns scope resolution and analysis view-model building for the active Analysis tab path.
 * The engine keeps scope handling, persisted-history queries, and export-window resolution in
 * one place so the rebuilt Analysis surface does not need to revive legacy AppController logic.
 */
class AnalysisQueryEngine {
public:
    explicit AnalysisQueryEngine(StorageService& storage);

    AnalysisViewModel buildViewModel(
        const DashboardViewModel& sourceViewModel,
        const DashboardPreferences& preferences,
        std::time_t now) const;

    ResolvedAnalysisWindow resolveWindow(
        const DashboardPreferences& preferences,
        std::time_t now) const;

    // Helpers exposed so AppController can use them for manual export
    static const char* dateRangePresetSettingValue(AnalysisDateRangePreset preset);
    static AnalysisDateRangePreset parseDateRangePreset(const std::string& value);
    static std::string dateRangePresetLabel(AnalysisDateRangePreset preset);

private:
    StorageService* storage_;
};

} // namespace JiStats
