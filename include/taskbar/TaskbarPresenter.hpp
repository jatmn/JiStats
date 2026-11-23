#pragma once

#include <string>
#include <memory>
#include <vector>
#include <ctime>
#include <cstdint>
#include <functional>

namespace JiStats {

enum class TaskbarMetricKind {
    QuotaUsage,
    PersistedActivity
};

enum class TaskbarPressureBand {
    Neutral,
    Blue,
    Green,
    Yellow,
    Orange,
    Red
};

// Taskbar metrics structure (per docs/reference/presentation.md)
struct TaskbarMetrics {
    std::string providerKey;
    std::string providerDisplayName;
    TaskbarMetricKind kind = TaskbarMetricKind::QuotaUsage;
    long long used = 0;               // Actual usage value
    long long limit = 0;             // Quota limit
    double usagePercentage = 0.0;     // Usage as percentage of quota
    double remainingPercentage = 0.0; // Remaining as percentage of quota
    std::time_t windowResetAt = 0;
    std::string secondaryWindowType;
    double secondaryRemainingPercentage = -1.0;
    std::time_t secondaryWindowResetAt = 0;
    std::string confidenceTier;       // "high", "medium", "low"
    std::string providerSurface;
    std::time_t lastUpdated = 0;

    // Optional override label for precomputed runtime summaries.
    std::string summaryLabel;
    std::string tooltipDetail;

    // Primary metric display (4-char max for taskbar)
    std::string primaryMetric;        // e.g., "85%" or "1.5K"
    std::string secondaryPrimaryMetric;

    // Secondary info
    std::string secondaryMetric;      // e.g., "1305/1500"
    std::string statusIcon;           // e.g., "🔵", "🟡", "🔴"
    TaskbarPressureBand pressureBand = TaskbarPressureBand::Neutral;
    TaskbarPressureBand secondaryPressureBand = TaskbarPressureBand::Neutral;
    bool emphasizePrimary = false;
    bool emphasizeSecondary = false;
};

// Taskbar presenter for Windows 11 taskbar integration
class TaskbarPresenter {
public:
    TaskbarPresenter();
    ~TaskbarPresenter();
    
    // Initialize taskbar integration
    bool init();
    
    // Shutdown taskbar integration
    void shutdown();
    
    // Update metrics display
    void updateMetrics(const TaskbarMetrics& metrics);
    
    // Update multiple provider metrics
    void updateMetrics(const std::vector<TaskbarMetrics>& metricsList);

    // Atomically replace the current metrics collection.
    void replaceMetrics(const std::vector<TaskbarMetrics>& metricsList);
    
    // Clear current display
    void clearMetrics();

    // Show or hide the overlay surface without tearing down the presenter thread.
    void setOverlayEnabled(bool enabled);

    // Register dedicated context-menu actions for the overlay surface.
    void setOnShowDashboardCallback(std::function<void()> callback);
    void setOnPreferencesCallback(std::function<void()> callback);
    void setOnRetryNowCallback(std::function<void()> callback);
    void setOnDismissOfflineNoticeCallback(std::function<void()> callback);
    void setOnExitCallback(std::function<void()> callback);
    void setOfflineMenuState(bool retryAvailable, bool dismissAvailable);
    
    // Set taskbar progress state
    enum class ProgressState {
        None,
        Normal,
        Paused,
        Error
    };
    
    void setProgress(double percentage, ProgressState state);
    
    // Get current metrics
    std::vector<TaskbarMetrics> getCurrentMetrics() const;

    // The presenter can accept metrics while the shell surface is being recovered.
    bool isInitialized() const;

    // Check if taskbar integration is available
    bool isAvailable() const;
    
    // Get last error message
    std::string getLastError() const;

#ifdef JISTATS_ENABLE_TEST_HOOKS
    struct GeometryRectForTesting {
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
    };

    struct PlacementInputForTesting {
        bool embedded = false;
        int width = 0;
        int height = 0;
        GeometryRectForTesting taskbarRect;
        GeometryRectForTesting taskbarClientRect;
        bool hasNotificationArea = false;
        GeometryRectForTesting notificationAreaRect;
        bool hasTrafficMonitor = false;
        GeometryRectForTesting trafficMonitorRect;
        std::vector<GeometryRectForTesting> blockingRects;
    };

    struct PlacementResultForTesting {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };

    struct OverlayRowForTesting {
        std::string label;
        std::string primaryMetric;
        std::string secondaryPrimaryMetric;
        std::string statusIcon;
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
        int columnIndex = 0;
        int rowInColumn = 0;
    };

    struct OverlayLayoutForTesting {
        std::vector<OverlayRowForTesting> rows;
        int width = 0;
        int height = 0;
        int labelColumnWidth = 0;
        int primaryColumnWidth = 0;
        int iconColumnWidth = 0;
        int lineHeight = 0;
        int columnCount = 1;
        bool transparentBackground = false;
    };

    struct InteractionStateForTesting {
        bool tooltipVisible = false;
        int hoveredRowIndex = -1;
        bool rightButtonCaptured = false;
        bool contextMenuOpen = false;
        bool hoverSuppressed = false;
        bool preferTopmostSurface = false;
        int tooltipFontHeight = 0;
    };

    enum class ContextMenuActionForTesting {
        ShowDashboard,
        Preferences,
        RetryNow,
        DismissOfflineNotice,
        Exit
    };

    // Test seam: expose deterministic metric derivation without UI dependencies.
    TaskbarMetrics deriveMetricsForTesting(const TaskbarMetrics& metrics) const;
    // Test seam: expose deterministic overlay formatting and layout math.
    OverlayLayoutForTesting deriveOverlayLayoutForTesting(
        const std::vector<TaskbarMetrics>& metricsList) const;
    // Test seam: expose row-specific hover-detail text without UI dependencies.
    std::string deriveHoverTooltipForTesting(
        const std::vector<TaskbarMetrics>& metricsList,
        size_t rowIndex) const;
    // Test seam: isolate placement math from live Win32 window discovery.
    PlacementResultForTesting derivePlacementForTesting(const PlacementInputForTesting& input) const;
    // Test seam: capture when shell/taskbar lifecycle should force overlay recreation.
    bool shouldRecreateSurfaceForTesting(
        std::uintptr_t existingTaskbarToken,
        std::uintptr_t discoveredTaskbarToken,
        bool hasSurfaceWindow) const;
    // Test seam: control runtime state to exercise update/progress logic.
    void setRuntimeStateForTesting(bool initialized, bool available);
    // Test seam: seed presenter interaction state without a live Win32 window.
    void setInteractionStateForTesting(bool tooltipVisible,
                                       int hoveredRowIndex,
                                       bool rightButtonCaptured,
                                       bool contextMenuOpen,
                                       bool embedded);
    // Test seam: inspect tooltip/menu/topmost interaction policy.
    InteractionStateForTesting getInteractionStateForTesting() const;
    // Test seam: exercise the same menu lifecycle used by overlay right click.
    void beginContextMenuInteractionForTesting();
    void endContextMenuInteractionForTesting();
    bool shouldCaptureOverlayRightClickForTesting(bool contextMenuOpen, bool pointInsideSurface) const;
    bool shouldRecoverOverlayAfterTaskbarClickForTesting(
        bool contextMenuOpen,
        bool pointInsideSurface,
        bool pointInsideTaskbar) const;
    bool dispatchContextMenuActionForTesting(ContextMenuActionForTesting action);
    std::uint64_t getRefreshRequestCountForTesting() const;
#endif
    
private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace JiStats
