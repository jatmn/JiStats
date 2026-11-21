#pragma once

#include <functional>
#include <string>
#include <memory>
#include <optional>

namespace JiStats {

// Tray status indicator type
enum class TrayStatus {
    Normal,     // Default state - app running normally
    Offline,    // Provider connectivity issues
    Syncing,    // Currently syncing with providers
    Warning,    // Quota warning threshold reached
    Error       // Error state
};

// Tray menu action callback types
using TrayMenuCallback = std::function<void()>;

class TrayController {
public:
    TrayController();
    ~TrayController();

    // Initialize the tray icon
    // Returns true if successful, false otherwise
    bool init();

    // Shutdown and remove tray icon
    void shutdown();

    // Show the tray icon
    void showIcon();

    // Hide the tray icon
    void hideIcon();

    // Set the status indicator (affects icon tooltip and visual state)
    void setStatus(TrayStatus status);

    // Set the tooltip text
    void setTooltip(const std::string& text);

    // Atomically update both tray status and tooltip with a single publish.
    void setState(TrayStatus status, const std::string& text);

    // Show the context menu at the current cursor position
    void showMenu();

    // Menu item registration
    void setOnShowDashboardCallback(TrayMenuCallback callback);
    void setOnPreferencesCallback(TrayMenuCallback callback);
    void setOnRetryNowCallback(TrayMenuCallback callback);
    void setOnDismissOfflineNoticeCallback(TrayMenuCallback callback);
    void setOnExitCallback(TrayMenuCallback callback);

    // Check if tray is available and initialized
    bool isAvailable() const;

    // Get last error message
    std::string getLastError() const;

#ifdef JISTATS_ENABLE_TEST_HOOKS
    enum class TestOperation {
        CreateMessageWindow,
        StartMessagePump,
        AddTrayIcon,
        RemoveTrayIcon,
        StopMessagePump,
        UpdateIcon,
        ShowContextMenu,
        DispatchTrayEvent
    };

    using TestOperationHandler = std::function<std::optional<bool>(TestOperation operation,
                                                                   TrayStatus status,
                                                                   const std::string& tooltip,
                                                                   std::string& error)>;

    enum class TestEvent {
        TrayContextMenu,
        TrayDoubleClick,
        MenuShowDashboard,
        MenuPreferences,
        MenuRetryNow,
        MenuDismissOfflineNotice,
        MenuExit
    };

    struct TestState {
        bool initialized = false;
        bool iconVisible = false;
        TrayStatus status = TrayStatus::Normal;
        std::string tooltip = "JiStats";
        std::string lastError;
    };

    static void setTestOperationHandler(TestOperationHandler handler);
    static void clearTestOperationHandler();
    bool dispatchTestEvent(TestEvent event);
    TestState getTestState() const;
#endif

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace JiStats
