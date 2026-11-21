#pragma once

#include <functional>
#include <string>

namespace JiStats {

/**
 * StartupManager handles Windows run-on-startup behavior.
 * 
 * Uses registry-based startup (HKCU\Software\Microsoft\Windows\CurrentVersion\Run).
 * Opt-in by default - user must explicitly enable startup behavior.
 * 
 * Per docs/plan.md section 10.4:
 * - Opt-in (disabled by default)
 * - --background flag support for minimized startup
 * - Registry-based for reliability
 */
class StartupManager {
public:
    StartupManager();
    ~StartupManager();
    
    // Non-copyable
    StartupManager(const StartupManager&) = delete;
    StartupManager& operator=(const StartupManager&) = delete;
    
    /**
     * Enable run-on-startup.
     * @return true if successfully enabled
     */
    bool enable();
    
    /**
     * Disable run-on-startup.
     * @return true if successfully disabled
     */
    bool disable();
    
    /**
     * Check if run-on-startup is currently enabled.
     * @return true if enabled
     */
    bool isEnabled() const;
    
    /**
     * Set whether to start minimized (background mode).
     * Rewrites the startup command immediately when startup is already enabled.
     * @param minimized true to start minimized to tray
     * @return true when the preference is cached, already matches the enabled command, or the enabled startup entry is updated successfully
     */
    bool setStartMinimized(bool minimized);
    
    /**
     * Check if start-minimized mode is set.
     * @return true if configured to start minimized
     */
    bool isStartMinimized() const;
    
    /**
     * Get the last error message if an operation failed.
     * @return error message or empty string if no error
     */
    std::string getLastError() const;

#ifdef JISTATS_ENABLE_TEST_HOOKS
    struct TestHooks {
        std::function<std::wstring()> executablePathProvider;
        std::function<bool(const std::wstring& value, std::string& error)> writeRegistryValue;
        std::function<std::wstring()> readRegistryValue;
    };

    void setTestHooks(const TestHooks& hooks);
    void clearTestHooks();
#endif

private:
    struct Impl;
    Impl* pImpl;
};

} // namespace JiStats
