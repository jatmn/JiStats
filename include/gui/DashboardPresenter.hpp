#pragma once

#include "gui/AnalysisViewModel.hpp"
#include "core/RuntimeStatus.hpp"

#include <functional>
#include <string>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace JiStats {

// Forward declarations
struct DashboardViewModel;
struct ProviderStatus;
struct QuotaInfo;
struct DashboardPreferences;
struct DashboardSummaryItem;
struct MiniMaxCredentialProfileSummary;
struct DashboardMiniMaxCredentialRequest;
struct DashboardMiniMaxCredentialResult;
struct DashboardOpenAIImportResult;
struct DashboardHtmlExportResult;
struct DashboardNoticeActionResult;
enum class DashboardShellCommand : int {
    FileSettings = 0,
    FileImportOpenAI = 1,
    FileExit = 2,
    HelpViewHelp = 3,
    HelpAbout = 4,
    FileExportHtml = 5
};

enum class DashboardShellDialog : int {
    Preferences = 0,
    Help = 1,
    About = 2
};

class DashboardPresenter {
public:
    DashboardPresenter();
    ~DashboardPresenter();
    
    // Initialize the dashboard
    bool init();

    // Shutdown the dashboard
    void shutdown();

    // Request the dashboard window to close without waiting for thread teardown
    void requestClose();
    
    // Render the dashboard UI
    void render();
    
    // Update the view model
    void updateViewModel(const DashboardViewModel& viewModel);
    
    // Get current view model
    DashboardViewModel getViewModel() const;
    
    // Show/hide dashboard
    void setVisible(bool visible);
    bool isVisible() const;
    bool isAnalysisWindowOpen() const;

    // Block until the dashboard window closes
    int waitForExit();

    // Get last error message
    std::string getLastError() const;

    // Subscribe to preference changes from the dashboard UI
    void setPreferencesChangedCallback(std::function<void(const DashboardPreferences&)> callback);
    void setCodexModelAttributionChangedCallback(std::function<void(bool, std::uint64_t)> callback);
    void setPreferencesOpenedCallback(std::function<void()> callback);
    void setPeriodicRefreshCallback(std::function<void()> callback);

    // Subscribe to MiniMax credential actions from the settings/accounts UI
    void setMiniMaxCredentialActionCallback(
        std::function<DashboardMiniMaxCredentialResult(const DashboardMiniMaxCredentialRequest&)> callback);

    // Subscribe to OpenAI export-import actions from the dashboard shell
    void setOpenAIImportActionCallback(
        std::function<DashboardOpenAIImportResult(const std::string&)> callback);

    // Subscribe to manual HTML export actions from the dashboard shell
    void setHtmlExportActionCallback(
        std::function<DashboardHtmlExportResult(const std::string&)> callback);

    // Subscribe to offline notice actions from the dashboard shell
    void setOfflineRetryActionCallback(
        std::function<DashboardNoticeActionResult()> callback);
    void setOfflineDismissActionCallback(
        std::function<DashboardNoticeActionResult()> callback);
    void requestOfflineRetry();
    void requestOfflineDismiss();

    // Subscribe to shell exit requests from the dashboard menu
    void setExitRequestedCallback(std::function<void()> callback);

    // Host an AnalysisPresenter for the Analysis tab. DashboardPresenter does not own it.
    void setAnalysisPresenter(class AnalysisPresenter* presenter);

#ifdef JISTATS_ENABLE_TEST_HOOKS
    bool selectMainTabForTesting(int tabIndex);
    bool dispatchShellCommandForTesting(DashboardShellCommand command);
    bool isAnalysisWindowOpenForTesting() const;
    bool isShellDialogOpenForTesting(DashboardShellDialog dialog) const;
    std::string getShellDialogBodyTextForTesting(DashboardShellDialog dialog) const;
    std::array<int, 4> getMainWindowRectForTesting() const;
    std::array<int, 4> getAnalysisWindowRectForTesting() const;
    std::array<int, 4> getShellDialogRectForTesting(DashboardShellDialog dialog) const;
    bool selectPreferencesTabForTesting(int tabIndex);
    bool clickClaudeCodeToggleForTesting();
    bool clickKimiCodeToggleForTesting();
    bool isClaudeCodeEnabledForTesting() const;
    bool isKimiCodeEnabledForTesting() const;
    std::string getClaudeCodeSettingsStatusTextForTesting() const;
    std::string getClaudeCodeSettingsDetailTextForTesting() const;
    std::string getKimiCodeSettingsStatusTextForTesting() const;
    std::string getKimiCodeSettingsDetailTextForTesting() const;
    bool submitMiniMaxCredentialActionForTesting(
        const DashboardMiniMaxCredentialRequest& request);
    bool selectProviderQuotaRowForTesting(int rowIndex);
    std::string getSelectedProviderIdForTesting() const;
    std::string getSelectedAccountIdForTesting() const;
    std::string getMiniMaxSecretFieldTextForTesting() const;
    std::vector<DashboardSummaryItem> getUsageProgressSummariesForTesting() const;
    std::string getCodexThroughputTextForTesting() const;
    std::string getOpenAiCodexModelUsageTextForTesting() const;
    std::string getHicapCodexModelUsageTextForTesting() const;
    std::array<int, 4> getCodexThroughputChartRectForTesting() const;
    std::vector<std::string> getApplicationFieldLabelsForTesting() const;
    std::vector<std::string> getApplicationFieldValuesForTesting() const;
    std::vector<std::string> getPreferencesTabLabelsForTesting() const;
    std::vector<std::string> getProviderQuotaColumnHeadersForTesting() const;
    std::vector<std::string> getProviderQuotaUsedValuesForTesting() const;
    std::vector<std::string> getProviderQuotaLimitValuesForTesting() const;
    std::vector<std::string> getProviderQuotaRemainingValuesForTesting() const;
    std::vector<std::string> getProviderQuotaLastSyncValuesForTesting() const;
    std::string getConnectionHelpTextForTesting() const;
    bool isScheduledExportEnabledForTesting() const;
    int getScheduledExportIntervalMinutesForTesting() const;
    std::string getScheduledExportPathForTesting() const;
    std::string getScheduledExportLastRunForTesting() const;
    std::string getStatusTextForTesting() const;
    int getUsageProgressBarWidthForTesting() const;
    bool postNativeCloseForTesting();
    void setOpenAIImportPathForTesting(const std::string& inputPath);
    void setHtmlExportPathForTesting(const std::string& outputPath);
    std::string getLastHtmlExportStatusForTesting() const;
    bool isOfflineNoticeVisibleForTesting() const;
    std::string getOfflineNoticeTextForTesting() const;
    bool clickOfflineRetryForTesting();
    bool clickOfflineDismissForTesting();
    bool clickOpenAnalysisWindowForTesting();
    std::string getAnalysisScopeLabelForTesting() const;
    std::uint64_t getViewModelSyncCountForTesting() const;
#endif
    
private:
    // Render sections
    void renderHeader();
    void renderProviderStatus();
    void renderQuotaCards();
    void renderUsageChart();
    void renderRecentActivity();
    
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

struct DashboardPreferences {
    bool taskbarOverlayEnabled = true;
    bool openaiSubscriptionTrackingEnabled = false;
    bool codexModelAttributionEnabled = false;
    bool miniMaxSubscriptionTrackingEnabled = false;
    bool claudeCodeEnabled = false;
    bool kimiCodeEnabled = false;
    bool grokSubscriptionTrackingEnabled = false;
    bool cursorSubscriptionTrackingEnabled = false;
    bool startupEnabled = false;
    bool startupStartInBackground = true;
    bool scheduledExportEnabled = false;
    int scheduledExportIntervalMinutes = 1440;
    std::string scheduledExportPath;
    // Last selected provider/account for dashboard persistence
    std::string lastSelectedProviderId;
    std::string lastSelectedAccountId;
    std::string analysisSelectedProviderId;
    std::string analysisSelectedAccountId;
    AnalysisDateRangePreset analysisDateRangePreset = AnalysisDateRangePreset::Last7Days;
    std::time_t analysisCustomRangeStartUtc = 0;
    std::time_t analysisCustomRangeEndUtc = 0;
    // Transient dashboard edit identifiers.
    std::uint64_t changeRequest = 0;
    std::uint64_t providerChangeRequest = 0;
};

enum class DashboardMiniMaxCredentialAction : int {
    Save = 0,
    Validate = 1
};

struct MiniMaxCredentialProfileSummary {
    std::string planKey;
    std::string planDisplayName;
    std::string profileName;
    std::string accountKey;
    std::string accountDisplayName;
    std::string secretRef;
    std::string validationState = "missing";
    std::string lastValidationError;
    std::string lastValidatedAtLabel;
    bool hasPersistedProfile = false;
};

struct DashboardMiniMaxCredentialRequest {
    DashboardMiniMaxCredentialAction action = DashboardMiniMaxCredentialAction::Save;
    std::string planKey;
    std::string accountDisplayName;
    std::string secret;
};

struct DashboardMiniMaxCredentialResult {
    bool success = false;
    std::string planKey;
    std::string message;
    std::optional<std::string> accountDisplayName = std::nullopt;
};

struct DashboardOpenAIImportResult {
    bool success = false;
    std::size_t insertedCount = 0;
    std::string inputPath;
    std::string message;
};

struct DashboardHtmlExportResult {
    bool success = false;
    std::string outputPath;
    std::string message;
};

struct DashboardNoticeActionResult {
    bool success = false;
    std::string message;
};

struct DashboardSummaryItem {
    std::string label;
    std::string detail;
    int percentage = 0;
    std::string badge = "Local";
    int secondaryPercentage = -1;
    std::string secondaryBadge;
};

enum class CodexAttributionSaveError { None, Enable, Revocation };

// Dashboard view model
struct DashboardViewModel {
    std::string appVersion = "1.0.0";
    std::string lastUpdateTime;
    bool isOnline = true;
    RuntimeState runtimeState = RuntimeState::Unknown;
    DashboardPreferences preferences;
    std::uint64_t completedPreferencesRequest = 0;
    std::uint64_t failedPreferencesRequest = 0;
    bool preferencesSessionOnly = false;
    std::uint64_t completedCodexAttributionRequest = 0;
    CodexAttributionSaveError codexAttributionSaveError = CodexAttributionSaveError::None;
    bool claudeCodeLocallyDetected = false;
    std::string claudeCodeDetectionMessage;
    bool kimiCodeLocallyDetected = false;
    std::string kimiCodeDetectionMessage;
    std::vector<MiniMaxCredentialProfileSummary> minimaxCredentialProfiles;
    std::vector<DashboardSummaryItem> runtimeSummaries;
    std::vector<ProviderStatus> providers;
    AnalysisViewModel analysis;
    std::string recoveryStatusSummary;
    std::string recoveryStatusDetail;
    std::string scheduledExportStatusSummary;
    std::string scheduledExportStatusDetail;
    std::string scheduledExportLastRunTime;
    bool offlineNoticeVisible = false;
    bool offlineRetryAvailable = false;
    std::string offlineNoticeTitle;
    std::string offlineNoticeMessage;
};

// Provider status
struct ProviderStatus {
    std::string providerKey;
    std::string displayName;
    std::string accountId;  // Provider account identifier for scope tracking
    bool isEnabled = false;
    ProviderState state = ProviderState::Sample; // Explicit state instead of just isConnected
    std::string lastSyncTime;  // ISO8601 timestamp of last successful sync
    std::vector<QuotaInfo> quotas;
    bool locallyDetected = false;  // Whether local artifacts were found at startup
    std::string detectionMessage;  // Human-readable discovery status
};

// Quota information
struct QuotaInfo {
    std::string planName;
    std::string windowType; // "daily", "monthly", "weekly"
    long long used = 0;
    long long limit = 0;
    long long remaining = 0;
    double percentage = 0.0; // Backward-compatible alias of usedPercentage
    double usedPercentage = 0.0;
    double remainingPercentage = 0.0;
    std::time_t windowResetAt = 0;
    std::string secondaryWindowType;
    double secondaryRemainingPercentage = -1.0;
    std::time_t secondaryWindowResetAt = 0;
    std::string confidenceTier; // "high", "medium", "low"
    std::string providerSurface; // normalized provenance label when known

    // Backfill and gap state (per docs/reference/reporting.md)
    // Set by reconciliation logic when data is reconstructed from multiple sources
    bool isBackfilled = false;
    // Set when there are gaps in the data that could not be filled
    bool hasUnknownGaps = false;
};

} // namespace JiStats
