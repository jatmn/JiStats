#include "platform/StartupManager.hpp"

#ifdef JISTATS_ENABLE_TEST_HOOKS
#include <atomic>
#include <mutex>
#else
#include <windows.h>
#endif
#include <optional>
#include <string>

namespace JiStats {

struct StartupManager::Impl {
    std::string lastError;

#ifdef JISTATS_ENABLE_TEST_HOOKS
    // Test executables must never read or write the user's startup entry.
    std::atomic<bool> startMinimized{false};
    mutable std::mutex startupCommandMutexForTesting;
    std::wstring startupCommandForTesting;
    std::optional<StartupManager::TestHooks> testHooks;
#else
    bool startMinimized = false;
    std::wstring appName = L"JiStats";

    // Registry path for startup entries
    static constexpr const wchar_t* RUN_KEY_PATH = 
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
#endif

    void clearLastError() {
        lastError.clear();
    }
    
    std::wstring getExecutablePath() const {
#ifdef JISTATS_ENABLE_TEST_HOOKS
        if (testHooks.has_value() && testHooks->executablePathProvider) {
            return testHooks->executablePathProvider();
        }
        return L"JiStats.exe";
#else
        wchar_t buffer[MAX_PATH] = {0};
        DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
            return L"";
        }
        return std::wstring(buffer, length);
#endif
    }
    
    bool writeRegistryValue(const std::wstring& value) {
#ifdef JISTATS_ENABLE_TEST_HOOKS
        if (testHooks.has_value() && testHooks->writeRegistryValue) {
            std::string error;
            if (!testHooks->writeRegistryValue(value, error)) {
                if (!error.empty()) {
                    lastError = error;
                }
                return false;
            }
            return true;
        }
        const std::lock_guard<std::mutex> lock(startupCommandMutexForTesting);
        startupCommandForTesting = value;
        return true;
#else
        HKEY hKey = nullptr;
        LONG result = RegOpenKeyExW(
            HKEY_CURRENT_USER,
            RUN_KEY_PATH,
            0,
            KEY_SET_VALUE,
            &hKey
        );
        
        if (result != ERROR_SUCCESS) {
            lastError = "Failed to open registry key";
            return false;
        }
        
        // If value is empty, delete the entry
        if (value.empty()) {
            result = RegDeleteValueW(hKey, appName.c_str());
            RegCloseKey(hKey);
            
            if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
                lastError = "Failed to delete registry value";
                return false;
            }
            return true;
        }
        
        // Write the startup command
        result = RegSetKeyValueW(
            hKey,
            nullptr,
            appName.c_str(),
            REG_SZ,
            value.c_str(),
            static_cast<DWORD>((value.length() + 1) * sizeof(wchar_t))
        );
        
        RegCloseKey(hKey);
        
        if (result != ERROR_SUCCESS) {
            lastError = "Failed to write registry value";
            return false;
        }
        
        return true;
#endif
    }
    
    std::wstring readRegistryValue() const {
#ifdef JISTATS_ENABLE_TEST_HOOKS
        if (testHooks.has_value() && testHooks->readRegistryValue) {
            return testHooks->readRegistryValue();
        }
        const std::lock_guard<std::mutex> lock(startupCommandMutexForTesting);
        return startupCommandForTesting;
#else
        HKEY hKey = nullptr;
        LONG result = RegOpenKeyExW(
            HKEY_CURRENT_USER,
            RUN_KEY_PATH,
            0,
            KEY_READ,
            &hKey
        );
        
        if (result != ERROR_SUCCESS) {
            return L"";
        }
        
        wchar_t buffer[MAX_PATH * 2] = {0};
        DWORD bufferSize = sizeof(buffer);
        DWORD type = 0;
        
        result = RegQueryValueExW(
            hKey,
            appName.c_str(),
            nullptr,
            &type,
            reinterpret_cast<LPBYTE>(buffer),
            &bufferSize
        );
        
        RegCloseKey(hKey);
        
        if (result != ERROR_SUCCESS || type != REG_SZ) {
            return L"";
        }
        
        return std::wstring(buffer);
#endif
    }

    std::wstring buildStartupCommand() const {
        const std::wstring exePath = getExecutablePath();
        if (exePath.empty()) {
            return L"";
        }

        std::wstring command = L"\"" + exePath + L"\"";
        if (startMinimized) {
            command += L" --background";
        }

        return command;
    }
};

StartupManager::StartupManager() : pImpl(new Impl()) {
}

StartupManager::~StartupManager() {
    delete pImpl;
}

bool StartupManager::enable() {
    const std::wstring command = pImpl->buildStartupCommand();
    if (command.empty()) {
        pImpl->lastError = "Failed to get executable path";
        return false;
    }

    if (pImpl->readRegistryValue() == command) {
        pImpl->clearLastError();
        return true;
    }

    if (!pImpl->writeRegistryValue(command)) {
        return false;
    }

    pImpl->clearLastError();
    return true;
}

bool StartupManager::disable() {
    if (!isEnabled()) {
        pImpl->clearLastError();
        return true;
    }

    if (!pImpl->writeRegistryValue(L"")) {
        return false;
    }

    pImpl->clearLastError();
    return true;
}

bool StartupManager::isEnabled() const {
    return !pImpl->readRegistryValue().empty();
}

bool StartupManager::setStartMinimized(bool minimized) {
    pImpl->startMinimized = minimized;

    // If already enabled, update the registry entry
    if (!isEnabled()) {
        pImpl->clearLastError();
        return true;
    }

    return enable(); // Re-write with new setting
}

bool StartupManager::isStartMinimized() const {
    const std::wstring startupCommand = pImpl->readRegistryValue();
    if (!startupCommand.empty()) {
        return startupCommand.find(L"--background") != std::wstring::npos;
    }

    return pImpl->startMinimized;
}

std::string StartupManager::getLastError() const {
    return pImpl->lastError;
}

#ifdef JISTATS_ENABLE_TEST_HOOKS
void StartupManager::setTestHooks(const TestHooks& hooks) {
    pImpl->testHooks = hooks;
}

void StartupManager::clearTestHooks() {
    pImpl->testHooks.reset();
}
#endif

} // namespace JiStats
