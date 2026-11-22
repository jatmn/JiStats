#pragma once

#include "gui/NotificationService.hpp"

#include <ctime>
#ifdef JISTATS_ENABLE_TEST_HOOKS
#include <functional>
#include <future>
#endif
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/TrayController.hpp"
#include "db/StorageService.hpp"
#include "platform/StartupManager.hpp"

namespace JiStats {

class ProviderManager; // Forward declare for test-injection path
class WindowsCredentialStore;
struct ActivityEvent;
struct DashboardViewModel;
struct DashboardPreferences;
struct DashboardSummaryItem;
struct DashboardMiniMaxCredentialRequest;
struct DashboardHtmlExportResult;
struct TaskbarMetrics;
enum class DashboardShellCommand : int;
enum class DashboardShellDialog : int;

class AppController {
public:
    AppController();
    ~AppController();

    // Initialize the application
    bool init(bool launchInBackground = false);

    // User-facing warning for a rejected database after initialization.
    std::string getStartupWarning() const;

    // Run the GUI lifecycle until shutdown is requested
    int run();

    // Request the application to stop running
    void requestShutdown();
    
    // Shutdown the application
    void shutdown();
    
    // Get application state
    std::string getState() const;

#ifdef JISTATS_ENABLE_TEST_HOOKS
    void setHeadlessForTesting(bool value);
    void setDisableTaskbarForTesting(bool value);
    void setTaskbarTestModeForTesting(bool value);
    void setForceDashboardInitFailureForTesting(bool value);
    void setDatabasePathForTesting(const std::string& dbPath);
    // Inject a test ProviderManager instance (for provider-driven data path)
    void setProviderManagerForTesting(std::shared_ptr<ProviderManager> providerManager);
    bool recordLocalActivityForTesting(const ActivityEvent& event);
    std::vector<PersistedActivityEvent> getPersistedActivitiesForTesting(
        const std::string& providerKey,
        const std::string& providerSurface,
        std::time_t startedAtFrom,
        std::time_t startedAtTo);
    std::optional<PersistedConnectivityState> getConnectivityStateForTesting(
        const std::string& providerKey) const;
    std::optional<PersistedSyncState> getSyncStateForTesting(
        const std::string& providerKey,
        const std::string& syncKind = "runtime_refresh") const;
    std::optional<PersistedRuntimeState> getPersistedRuntimeStateForTesting() const;
    std::vector<PersistedGapMarker> getGapMarkersForTesting(
        const std::string& providerKey,
        std::time_t periodStartFrom,
        std::time_t periodEndTo) const;
    DashboardViewModel getDashboardViewModelForTesting() const;
    std::vector<TaskbarMetrics> getTaskbarMetricsForTesting() const;
    bool isDashboardVisibleForTesting() const;
    bool dispatchTrayEventForTesting(TrayController::TestEvent event);
    bool dispatchDashboardShellCommandForTesting(DashboardShellCommand command);
    bool clickDashboardOfflineRetryForTesting();
    bool clickDashboardOfflineDismissForTesting();
    bool isDashboardShellDialogOpenForTesting(DashboardShellDialog dialog) const;
    std::string getDashboardShellDialogBodyTextForTesting(DashboardShellDialog dialog) const;
    bool submitMiniMaxCredentialActionForTesting(const DashboardMiniMaxCredentialRequest& request);
    std::string getMiniMaxSecretFieldTextForTesting() const;
    std::optional<std::string> getMiniMaxValidationStateForTesting(const std::string& planKey) const;
    std::vector<DashboardSummaryItem> getDashboardUsageProgressSummariesForTesting() const;
    TrayController::TestState getTrayStateForTesting() const;
    std::optional<Notice> getActiveNoticeForTesting(NoticeType type) const;
    bool isDashboardOfflineNoticeVisibleForTesting() const;
    std::string getDashboardOfflineNoticeTextForTesting() const;
    std::string getDashboardStatusTextForTesting() const;
    DashboardHtmlExportResult exportHtmlReportForTesting(const std::string& outputPath);
    void refreshRuntimeForTesting();
    void setBeforeRuntimePublicationForTesting(std::function<void()> callback);
    void applyDashboardPreferencesForTesting(const DashboardPreferences& preferences);
    void setMiniMaxCredentialStoreForTesting(std::shared_ptr<WindowsCredentialStore> store);
    void setMiniMaxValidationResponseForTesting(
        bool success,
        int statusCode,
        const std::string& body = "",
        const std::string& error = "");
    void setMiniMaxValidationReleaseForTesting(std::shared_future<void> release);
    void setOpenAICodexRefreshPollIntervalForTesting(int intervalMs);
    void setLocalProviderRefreshPollIntervalForTesting(int intervalMs);
    void setCurrentTimeForTesting(std::time_t now);
    void clearCurrentTimeForTesting();
    void setScheduledExportPollIntervalForTesting(int intervalMs);
    void setRetentionPollIntervalForTesting(int intervalMs);
    void setOpenAIImportPathForTesting(const std::string& inputPath);
    void setHtmlExportPathForTesting(const std::string& outputPath);
    void setStartupManagerTestHooks(const StartupManager::TestHooks& hooks);
    void clearStartupManagerTestHooks();
#endif
    
private:
    // Private implementation
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace JiStats
