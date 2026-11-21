#include "app/TrayController.hpp"

#include "core/Logger.hpp"
#include "platform/WindowsAppAssets.hpp"

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <string>

namespace JiStats {

// Unique message ID for tray icon events
constexpr UINT WM_TRAYICON = WM_USER + 1;
constexpr UINT WM_TRAY_DISPATCH = WM_APP + 12;
constexpr UINT TRAY_ICON_ID = 1;

#ifdef JISTATS_ENABLE_TEST_HOOKS
namespace {

    TrayController::TestOperationHandler& getTestOperationHandler() {
    static TrayController::TestOperationHandler handler;
    return handler;
}

std::optional<bool> runTestOperation(
    TrayController::TestOperation operation,
    TrayStatus status,
    const std::string& tooltip,
    std::string& error) {
    auto& handler = getTestOperationHandler();
    if (!handler) {
        return std::nullopt;
    }

    std::string operationError;
    const std::optional<bool> success = handler(operation, status, tooltip, operationError);
    if (success.has_value() && !success.value()) {
        error = operationError;
    }
    return success;
}

} // namespace
#endif

class TrayController::Impl {
public:
    bool initialized = false;
    bool iconVisible = false;
    TrayStatus currentStatus = TrayStatus::Normal;
    std::string tooltip = "JiStats";
    std::string lastError;

    TrayMenuCallback onShowDashboard;
    TrayMenuCallback onPreferences;
    TrayMenuCallback onRetryNow;
    TrayMenuCallback onDismissOfflineNotice;
    TrayMenuCallback onExit;

    NOTIFYICONDATAW nid = {};
    HWND hwndMessage = nullptr;
    HMENU hMenu = nullptr;
    std::thread messagePumpThread;
    std::mutex messagePumpMutex;
    std::condition_variable messagePumpCv;
    bool messagePumpReady = false;
    bool messagePumpFailed = false;
    bool messagePumpRunning = false;
    std::string messagePumpError;
    DWORD messagePumpThreadId = 0;
    bool usingTestMessageWindow = false;
    HICON appIcon = nullptr;

    bool queueTrayDispatch(UINT_PTR action) {
#ifdef JISTATS_ENABLE_TEST_HOOKS
        const bool hasFakeMessageWindow =
            usingTestMessageWindow && hwndMessage == reinterpret_cast<HWND>(1);
        if (hasFakeMessageWindow) {
            std::string seamError;
            const std::optional<bool> seamResult = runTestOperation(
                TrayController::TestOperation::DispatchTrayEvent,
                currentStatus,
                tooltip,
                seamError);
            if (seamResult.has_value()) {
                if (!seamResult.value()) {
                    lastError = seamError.empty() ? "Failed to dispatch tray callback (test seam)" : seamError;
                    JISTATS_LOG_ERROR(LogCategory::General, "TrayController: " + lastError);
                    return false;
                }
                return true;
            }
        }
#endif

        if (PostMessageW(hwndMessage, WM_TRAY_DISPATCH, action, 0) != 0) {
            return true;
        }

        lastError = "Failed to post tray callback dispatch message, error: " +
            std::to_string(GetLastError());
        return false;
    }

    bool handleTrayIconEvent(UINT eventCode) {
        switch (eventCode) {
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
                showContextMenu();
                return true;
            case WM_LBUTTONDBLCLK:
                if (onShowDashboard) {
                    onShowDashboard();
                }
                return true;
            default:
                return false;
        }
    }

    bool handleTrayDispatchMessage(UINT_PTR action) {
        if (handleTrayIconEvent(static_cast<UINT>(action))) {
            return true;
        }
        return handleMenuCommand(static_cast<UINT>(action));
    }

    bool handleMenuCommand(UINT commandId) {
        switch (commandId) {
            case ID_TRAY_SHOW_DASHBOARD:
                if (onShowDashboard) {
                    onShowDashboard();
                }
                return true;
            case ID_TRAY_PREFERENCES:
                if (onPreferences) {
                    onPreferences();
                }
                return true;
            case ID_TRAY_RETRY_NOW:
                if (onRetryNow) {
                    onRetryNow();
                }
                return true;
            case ID_TRAY_DISMISS_OFFLINE_NOTICE:
                if (onDismissOfflineNotice) {
                    onDismissOfflineNotice();
                }
                return true;
            case ID_TRAY_EXIT:
                if (onExit) {
                    onExit();
                }
                return true;
            default:
                return false;
        }
    }

#ifdef JISTATS_ENABLE_TEST_HOOKS
    bool dispatchTestEvent(TrayController::TestEvent event) {
        const auto dispatch = [this](UINT action) {
            const bool isTestWindow = usingTestMessageWindow && (hwndMessage == reinterpret_cast<HWND>(1));
            if (queueTrayDispatch(action)) {
                return isTestWindow ? handleTrayDispatchMessage(action) : true;
            }
            return handleTrayDispatchMessage(action);
        };

        switch (event) {
            case TrayController::TestEvent::TrayContextMenu:
                return dispatch(WM_CONTEXTMENU);
            case TrayController::TestEvent::TrayDoubleClick:
                return dispatch(WM_LBUTTONDBLCLK);
            case TrayController::TestEvent::MenuShowDashboard:
                return dispatch(ID_TRAY_SHOW_DASHBOARD);
            case TrayController::TestEvent::MenuPreferences:
                return dispatch(ID_TRAY_PREFERENCES);
            case TrayController::TestEvent::MenuRetryNow:
                return dispatch(ID_TRAY_RETRY_NOW);
            case TrayController::TestEvent::MenuDismissOfflineNotice:
                return dispatch(ID_TRAY_DISMISS_OFFLINE_NOTICE);
            case TrayController::TestEvent::MenuExit:
                return dispatch(ID_TRAY_EXIT);
            default:
                return false;
        }
    }
#endif

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        if (uMsg == WM_TRAY_DISPATCH && self) {
            if (self->handleTrayDispatchMessage(static_cast<UINT_PTR>(wParam))) {
                return 0;
            }
        }

        if (uMsg == WM_TRAYICON && self) {
            if (self->queueTrayDispatch(LOWORD(lParam))) {
                return 0;
            }
            if (self->handleTrayIconEvent(LOWORD(lParam))) {
                return 0;
            }
        }

        if (uMsg == WM_COMMAND && self) {
            if (self->queueTrayDispatch(LOWORD(wParam))) {
                return 0;
            }
            if (self->handleMenuCommand(LOWORD(wParam))) {
                return 0;
            }
        }

        if (uMsg == WM_NCDESTROY && self) {
            PostQuitMessage(0);
            return 0;
        }
        
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    void showContextMenu() {
#ifdef JISTATS_ENABLE_TEST_HOOKS
        std::string seamError;
        const std::optional<bool> seamResult = runTestOperation(
            TrayController::TestOperation::ShowContextMenu,
            currentStatus,
            tooltip,
            seamError);
        if (seamResult.has_value()) {
            if (!seamResult.value()) {
                lastError = seamError.empty() ? "Failed to show context menu (test seam)" : seamError;
                JISTATS_LOG_ERROR(LogCategory::General, "TrayController: " + lastError);
            }
            return;
        }
#endif

        if (hMenu) {
            DestroyMenu(hMenu);
            hMenu = nullptr;
        }

        hMenu = CreatePopupMenu();
        if (!hMenu) {
            lastError = "Failed to create popup menu";
            JISTATS_LOG_ERROR(LogCategory::General, "TrayController: " + lastError);
            return;
        }
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_SHOW_DASHBOARD, L"Open Main Window");
        if (currentStatus == TrayStatus::Offline) {
            if (onRetryNow) {
                AppendMenuW(hMenu, MF_STRING, ID_TRAY_RETRY_NOW, L"Retry Now");
            }
            if (onDismissOfflineNotice) {
                AppendMenuW(hMenu, MF_STRING, ID_TRAY_DISMISS_OFFLINE_NOTICE, L"Dismiss Offline Notice");
            }
        }
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_PREFERENCES, L"Preferences...");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");

        // Get cursor position for menu placement
        POINT pt;
        GetCursorPos(&pt);

        // Required for the menu to close when clicking outside
        SetForegroundWindow(hwndMessage);

        const UINT flags = TPM_BOTTOMALIGN | TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY;
        const UINT command = TrackPopupMenu(
            hMenu,
            flags,
            pt.x,
            pt.y,
            0,
            hwndMessage,
            nullptr);
        if (command != 0) {
            handleMenuCommand(command);
        }
        PostMessageW(hwndMessage, WM_NULL, 0, 0);
    }

    void runMessagePump() {
        std::string localError;
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"JiStatsTrayMessageClass";

        if (!RegisterClassExW(&wc)) {
            const DWORD errorCode = GetLastError();
            if (errorCode != ERROR_CLASS_ALREADY_EXISTS) {
                localError = "Failed to register message window class, error: " + std::to_string(errorCode);
            }
        }

        if (localError.empty()) {
            hwndMessage = CreateWindowExW(
                WS_EX_TOOLWINDOW,
                L"JiStatsTrayMessageClass",
                L"JiStatsTray",
                WS_POPUP,
                0,
                0,
                0,
                0,
                nullptr,
                nullptr,
                GetModuleHandleW(nullptr),
                nullptr);
            if (!hwndMessage) {
                localError = "Failed to create message window, error: " + std::to_string(GetLastError());
            } else {
                SetWindowLongPtrW(hwndMessage, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            }
        }

        {
            std::lock_guard<std::mutex> lock(messagePumpMutex);
            messagePumpFailed = !localError.empty();
            messagePumpError = localError;
            messagePumpReady = true;
            messagePumpRunning = !localError.empty() ? false : true;
        }
        messagePumpCv.notify_all();

        if (localError.empty()) {
            messagePumpThreadId = GetCurrentThreadId();
            messagePumpRunning = true;
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        {
            std::lock_guard<std::mutex> lock(messagePumpMutex);
            messagePumpRunning = false;
            hwndMessage = nullptr;
        }
    }

    bool createMessageWindow() {
#ifdef JISTATS_ENABLE_TEST_HOOKS
        std::string seamError;
        const std::optional<bool> seamResult = runTestOperation(
            TrayController::TestOperation::CreateMessageWindow,
            currentStatus,
            tooltip,
            seamError);
        if (seamResult.has_value()) {
            if (!seamResult.value()) {
                lastError = seamError.empty()
                    ? "Failed to create message window (test seam)" 
                    : seamError;
                JISTATS_LOG_ERROR(LogCategory::General, "TrayController: " + lastError);
                return false;
            }

            hwndMessage = reinterpret_cast<HWND>(1);
            messagePumpRunning = true;
            usingTestMessageWindow = true;
            return true;
        }
#endif

        messagePumpError.clear();
        messagePumpFailed = false;
        messagePumpReady = false;

#ifdef JISTATS_ENABLE_TEST_HOOKS
        {
            std::string startSeamError;
            const std::optional<bool> startResult = runTestOperation(
                TrayController::TestOperation::StartMessagePump,
                currentStatus,
                tooltip,
                startSeamError);
            if (startResult.has_value() && !startResult.value()) {
                lastError = startSeamError.empty()
                    ? "Failed to start tray message pump (test seam)"
                    : startSeamError;
                JISTATS_LOG_ERROR(LogCategory::General, "TrayController: " + lastError);
                return false;
            }
        }
#endif

        messagePumpThread = std::thread([this]() {
            runMessagePump();
        });

        std::unique_lock<std::mutex> lock(messagePumpMutex);
        messagePumpCv.wait(lock, [this]() {
            return messagePumpReady;
        });

        if (messagePumpFailed) {
            lastError = messagePumpError;
            if (messagePumpThread.joinable()) {
                messagePumpThread.join();
            }
            return false;
        }

#ifdef JISTATS_ENABLE_TEST_HOOKS
        usingTestMessageWindow = false;
#endif
        return true;
    }

    bool addTrayIcon() {
#ifdef JISTATS_ENABLE_TEST_HOOKS
        std::string seamError;
        const std::optional<bool> seamResult = runTestOperation(
            TrayController::TestOperation::AddTrayIcon,
            currentStatus,
            tooltip,
            seamError);
        if (seamResult.has_value()) {
            if (!seamResult.value()) {
                lastError = seamError.empty() ? "Failed to add tray icon (test seam)" : seamError;
                JISTATS_LOG_ERROR(LogCategory::General, "TrayController: " + lastError);
                return false;
            }

            iconVisible = true;
            return true;
        }
#endif

        ZeroMemory(&nid, sizeof(nid));
        nid.cbSize = sizeof(NOTIFYICONDATAW);
        nid.hWnd = hwndMessage;
        nid.uID = TRAY_ICON_ID;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_TRAYICON;
        nid.hIcon = loadIconForStatus(currentStatus);
        
        // Set tooltip
        std::wstring wtooltip = toWideString(tooltip);
        wcsncpy_s(nid.szTip, wtooltip.c_str(), _TRUNCATE);

        if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
            lastError = "Failed to add tray icon, error: " + std::to_string(GetLastError());
            JISTATS_LOG_ERROR(LogCategory::General, "TrayController: " + lastError);
            return false;
        }

        iconVisible = true;
        return true;
    }

    void removeTrayIcon() {
        if (!iconVisible) {
            return;
        }

#ifdef JISTATS_ENABLE_TEST_HOOKS
        std::string seamError;
        const std::optional<bool> seamResult = runTestOperation(
            TrayController::TestOperation::RemoveTrayIcon,
            currentStatus,
            tooltip,
            seamError);
        if (seamResult.has_value()) {
            if (!seamResult.value()) {
                lastError = seamError.empty() ? "Failed to remove tray icon (test seam)" : seamError;
                JISTATS_LOG_ERROR(LogCategory::General, "TrayController: " + lastError);
            }
            iconVisible = false;
            return;
        }
#endif

        Shell_NotifyIconW(NIM_DELETE, &nid);
        iconVisible = false;
    }

    void stopMessagePump() {
#ifdef JISTATS_ENABLE_TEST_HOOKS
        {
            std::string seamError;
            const std::optional<bool> seamResult = runTestOperation(
                TrayController::TestOperation::StopMessagePump,
                currentStatus,
                tooltip,
                seamError);
            if (seamResult.has_value() && !seamResult.value()) {
                lastError = seamError.empty() ? "Failed to stop tray message pump (test seam)" : seamError;
                JISTATS_LOG_ERROR(LogCategory::General, "TrayController: " + lastError);
            }
        }

        if (usingTestMessageWindow) {
            usingTestMessageWindow = false;
            hwndMessage = nullptr;
            messagePumpRunning = false;
            messagePumpReady = false;
            messagePumpFailed = false;
            messagePumpError.clear();
            return;
        }
#endif

        if (hwndMessage != nullptr) {
            if (PostMessageW(hwndMessage, WM_CLOSE, 0, 0) == 0) {
                lastError = "Failed to post tray window close message, error: " +
                    std::to_string(GetLastError());
                JISTATS_LOG_ERROR(LogCategory::General, "TrayController: " + lastError);
            }
        }

        if (messagePumpThread.joinable()) {
            if (messagePumpThread.get_id() != std::this_thread::get_id()) {
                messagePumpThread.join();
            }
        }
    }

    void updateIcon() {
        if (!iconVisible) return;

#ifdef JISTATS_ENABLE_TEST_HOOKS
        std::string seamError;
        const std::optional<bool> seamResult = runTestOperation(
            TrayController::TestOperation::UpdateIcon,
            currentStatus,
            tooltip,
            seamError);
        if (seamResult.has_value()) {
            if (!seamResult.value()) {
                lastError = seamError.empty() ? "Failed to update tray icon (test seam)" : seamError;
                JISTATS_LOG_ERROR(LogCategory::General, "TrayController: " + lastError);
            }
            return;
        }
#endif

        nid.hIcon = loadIconForStatus(currentStatus);
        std::wstring wtooltip = toWideString(tooltip);
        wcsncpy_s(nid.szTip, wtooltip.c_str(), _TRUNCATE);

        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    HICON loadIconForStatus(TrayStatus status) {
        if (appIcon == nullptr) {
            appIcon = WindowsAppAssets::loadAppIcon(GetSystemMetrics(SM_CXSMICON));
        }
        if (appIcon != nullptr) {
            return appIcon;
        }

        // Fall back to standard Windows icons if the custom asset is unavailable.
        WORD iconId = 32512; // IDI_APPLICATION
        
        switch (status) {
            case TrayStatus::Normal:
                iconId = 32512; // IDI_APPLICATION
                break;
            case TrayStatus::Offline:
                iconId = 32513; // IDI_ERROR
                break;
            case TrayStatus::Syncing:
                iconId = 32512; // IDI_APPLICATION (could use custom sync icon)
                break;
            case TrayStatus::Warning:
                iconId = 32515; // IDI_WARNING
                break;
            case TrayStatus::Error:
                iconId = 32513; // IDI_ERROR
                break;
        }

        return LoadIconW(nullptr, MAKEINTRESOURCEW(iconId));
    }

    std::wstring toWideString(const std::string& str) {
        if (str.empty()) return L"";
        
        int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
        std::wstring result(size - 1, 0);
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &result[0], size);
        return result;
    }

    void cleanup() {
        removeTrayIcon();
        stopMessagePump();
        
        if (hMenu) {
            DestroyMenu(hMenu);
            hMenu = nullptr;
        }
        if (appIcon != nullptr) {
            DestroyIcon(appIcon);
            appIcon = nullptr;
        }
        
        initialized = false;
    }

private:
    // Menu item IDs
    static constexpr UINT ID_TRAY_SHOW_DASHBOARD = 1001;
    static constexpr UINT ID_TRAY_PREFERENCES = 1002;
    static constexpr UINT ID_TRAY_EXIT = 1003;
    static constexpr UINT ID_TRAY_RETRY_NOW = 1004;
    static constexpr UINT ID_TRAY_DISMISS_OFFLINE_NOTICE = 1005;
};

TrayController::TrayController() : pImpl(std::make_unique<Impl>()) {
}

TrayController::~TrayController() {
    shutdown();
}

bool TrayController::init() {
    if (pImpl->initialized) {
        return true;
    }

    JISTATS_LOG_INFO(LogCategory::Startup, "TrayController initializing");

    // Create message window for receiving tray events
    if (!pImpl->createMessageWindow()) {
        return false;
    }

    // Add the tray icon
    if (!pImpl->addTrayIcon()) {
        pImpl->cleanup();
        return false;
    }

    pImpl->initialized = true;
    JISTATS_LOG_INFO(LogCategory::Startup, "TrayController initialized");
    return true;
}

void TrayController::shutdown() {
    if (!pImpl->initialized) {
        return;
    }

    JISTATS_LOG_INFO(LogCategory::Startup, "TrayController shutting down");
    pImpl->cleanup();
}

void TrayController::showIcon() {
    if (!pImpl->initialized) {
        return;
    }

    if (!pImpl->iconVisible) {
        pImpl->addTrayIcon();
    }
}

void TrayController::hideIcon() {
    if (!pImpl->initialized) {
        return;
    }

    pImpl->removeTrayIcon();
}

void TrayController::setStatus(TrayStatus status) {
    if (pImpl->currentStatus == status) {
        return;
    }
    pImpl->currentStatus = status;
    pImpl->updateIcon();
}

void TrayController::setTooltip(const std::string& text) {
    if (pImpl->tooltip == text) {
        return;
    }
    pImpl->tooltip = text;
    pImpl->updateIcon();
}

void TrayController::setState(TrayStatus status, const std::string& text) {
    const bool statusChanged = pImpl->currentStatus != status;
    const bool tooltipChanged = pImpl->tooltip != text;
    if (!statusChanged && !tooltipChanged) {
        return;
    }

    pImpl->currentStatus = status;
    pImpl->tooltip = text;
    pImpl->updateIcon();
}

void TrayController::showMenu() {
    if (!pImpl->initialized) {
        return;
    }

    const bool onTrayThread =
        pImpl->messagePumpThreadId != 0 &&
        GetCurrentThreadId() == pImpl->messagePumpThreadId;
    if (onTrayThread) {
        pImpl->showContextMenu();
        return;
    }

#ifdef JISTATS_ENABLE_TEST_HOOKS
    const bool isTestWindow = pImpl->usingTestMessageWindow &&
        pImpl->hwndMessage == reinterpret_cast<HWND>(1);
    if (isTestWindow) {
        if (pImpl->queueTrayDispatch(WM_CONTEXTMENU)) {
            pImpl->handleTrayDispatchMessage(WM_CONTEXTMENU);
            return;
        }
    } else if (pImpl->hwndMessage != nullptr) {
        SendMessageW(pImpl->hwndMessage, WM_TRAY_DISPATCH, WM_CONTEXTMENU, 0);
        return;
    }
#else
    if (pImpl->hwndMessage != nullptr) {
        SendMessageW(pImpl->hwndMessage, WM_TRAY_DISPATCH, WM_CONTEXTMENU, 0);
        return;
    }
#endif

    pImpl->showContextMenu();
}

void TrayController::setOnShowDashboardCallback(TrayMenuCallback callback) {
    pImpl->onShowDashboard = std::move(callback);
}

void TrayController::setOnPreferencesCallback(TrayMenuCallback callback) {
    pImpl->onPreferences = std::move(callback);
}

void TrayController::setOnRetryNowCallback(TrayMenuCallback callback) {
    pImpl->onRetryNow = std::move(callback);
}

void TrayController::setOnDismissOfflineNoticeCallback(TrayMenuCallback callback) {
    pImpl->onDismissOfflineNotice = std::move(callback);
}

void TrayController::setOnExitCallback(TrayMenuCallback callback) {
    pImpl->onExit = std::move(callback);
}

bool TrayController::isAvailable() const {
    return pImpl->initialized && pImpl->iconVisible;
}

std::string TrayController::getLastError() const {
    return pImpl->lastError;
}

#ifdef JISTATS_ENABLE_TEST_HOOKS
void TrayController::setTestOperationHandler(TestOperationHandler handler) {
    getTestOperationHandler() = std::move(handler);
}

void TrayController::clearTestOperationHandler() {
    getTestOperationHandler() = nullptr;
}

bool TrayController::dispatchTestEvent(TestEvent event) {
    if (!pImpl->initialized) {
        return false;
    }
    return pImpl->dispatchTestEvent(event);
}

TrayController::TestState TrayController::getTestState() const {
    TestState state;
    state.initialized = pImpl->initialized;
    state.iconVisible = pImpl->iconVisible;
    state.status = pImpl->currentStatus;
    state.tooltip = pImpl->tooltip;
    state.lastError = pImpl->lastError;
    return state;
}
#endif

} // namespace JiStats
