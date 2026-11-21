#include "platform/WindowsCredentialStore.hpp"

#include <windows.h>
#include <wincred.h>

#include <cwchar>
#include <vector>

namespace JiStats {

namespace {

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return std::wstring();
    }

    const int required = MultiByteToWideChar(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (required <= 0) {
        return std::wstring();
    }

    std::wstring wideValue(static_cast<size_t>(required), L'\0');
    const int converted = MultiByteToWideChar(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(value.size()),
        wideValue.data(),
        required);
    if (converted != required) {
        return std::wstring();
    }

    return wideValue;
}

std::string wideToUtf8(const wchar_t* value, size_t length) {
    if (!value || length == 0) {
        return std::string();
    }

    const int required = WideCharToMultiByte(
        CP_UTF8,
        0,
        value,
        static_cast<int>(length),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return std::string();
    }

    std::string utf8Value(static_cast<size_t>(required), '\0');
    const int converted = WideCharToMultiByte(
        CP_UTF8,
        0,
        value,
        static_cast<int>(length),
        utf8Value.data(),
        required,
        nullptr,
        nullptr);
    if (converted != required) {
        return std::string();
    }

    return utf8Value;
}

std::string getWindowsErrorMessage(const std::string& prefix, DWORD errorCode = GetLastError()) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    std::string suffix;
    if (length > 0 && buffer != nullptr) {
        suffix = wideToUtf8(buffer, length);
        LocalFree(buffer);
    }

    while (!suffix.empty() && (suffix.back() == '\r' || suffix.back() == '\n')) {
        suffix.pop_back();
    }

    if (suffix.empty()) {
        suffix = "Win32 error " + std::to_string(errorCode);
    }

    return prefix + ": " + suffix;
}

class CredentialGuard {
public:
    CredentialGuard() = default;

    ~CredentialGuard() {
        if (credential_ != nullptr) {
            CredFree(credential_);
        }
    }

    PCREDENTIALW* out() {
        return &credential_;
    }

    PCREDENTIALW get() const {
        return credential_;
    }

private:
    PCREDENTIALW credential_ = nullptr;
};

class CredentialListGuard {
public:
    CredentialListGuard() = default;

    ~CredentialListGuard() {
        if (credentials_ != nullptr) {
            CredFree(credentials_);
        }
    }

    PCREDENTIALW** out() {
        return &credentials_;
    }

    PCREDENTIALW* get() const {
        return credentials_;
    }

private:
    PCREDENTIALW* credentials_ = nullptr;
};

class WindowsCredentialBackend final : public IWindowsCredentialBackend {
public:
    bool writeGenericCredential(
        const WindowsCredentialWriteRequest& request,
        std::string& error) override {
        if (request.targetName.empty()) {
            error = "Credential target name is required";
            return false;
        }
        if (request.secret.empty()) {
            error = "Credential secret is required";
            return false;
        }
        if (request.secret.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
            error = "Credential secret exceeds Windows Credential Manager size limit";
            return false;
        }

        const std::wstring targetName = utf8ToWide(request.targetName);
        if (targetName.empty()) {
            error = "Credential target name must be valid UTF-8";
            return false;
        }

        const std::wstring userName = utf8ToWide(request.userName);
        const std::wstring comment = utf8ToWide(request.comment);

        CREDENTIALW credential{};
        credential.Type = CRED_TYPE_GENERIC;
        credential.TargetName = const_cast<LPWSTR>(targetName.c_str());
        credential.CredentialBlobSize = static_cast<DWORD>(request.secret.size());
        credential.CredentialBlob =
            reinterpret_cast<LPBYTE>(const_cast<char*>(request.secret.data()));
        credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
        if (!userName.empty()) {
            credential.UserName = const_cast<LPWSTR>(userName.c_str());
        }
        if (!comment.empty()) {
            credential.Comment = const_cast<LPWSTR>(comment.c_str());
        }

        if (!CredWriteW(&credential, 0)) {
            error = getWindowsErrorMessage("Failed to write Windows credential");
            return false;
        }

        error.clear();
        return true;
    }

    std::optional<std::string> readGenericCredential(
        const std::string& targetName,
        std::string& error) override {
        if (targetName.empty()) {
            error = "Credential target name is required";
            return std::nullopt;
        }

        const std::wstring targetNameWide = utf8ToWide(targetName);
        if (targetNameWide.empty()) {
            error = "Credential target name must be valid UTF-8";
            return std::nullopt;
        }

        CredentialGuard credential;
        if (!CredReadW(targetNameWide.c_str(), CRED_TYPE_GENERIC, 0, credential.out())) {
            const DWORD lastError = GetLastError();
            if (lastError == ERROR_NOT_FOUND) {
                error = "Credential not found: " + targetName;
            } else {
                error = getWindowsErrorMessage("Failed to read Windows credential", lastError);
            }
            return std::nullopt;
        }

        const PCREDENTIALW stored = credential.get();
        if (stored == nullptr || stored->CredentialBlob == nullptr ||
            stored->CredentialBlobSize == 0) {
            error = "Credential has no secret payload: " + targetName;
            return std::nullopt;
        }

        error.clear();
        return std::string(
            reinterpret_cast<const char*>(stored->CredentialBlob),
            reinterpret_cast<const char*>(stored->CredentialBlob) + stored->CredentialBlobSize);
    }

    bool deleteGenericCredential(
        const std::string& targetName,
        std::string& error) override {
        if (targetName.empty()) {
            error = "Credential target name is required";
            return false;
        }

        const std::wstring targetNameWide = utf8ToWide(targetName);
        if (targetNameWide.empty()) {
            error = "Credential target name must be valid UTF-8";
            return false;
        }

        if (!CredDeleteW(targetNameWide.c_str(), CRED_TYPE_GENERIC, 0)) {
            const DWORD lastError = GetLastError();
            if (lastError == ERROR_NOT_FOUND) {
                error = "Credential not found: " + targetName;
            } else {
                error = getWindowsErrorMessage("Failed to delete Windows credential", lastError);
            }
            return false;
        }

        error.clear();
        return true;
    }

    std::vector<WindowsCredentialSummary> listGenericCredentials(
        const std::string& prefixFilter,
        std::string& error) override {
        std::wstring filterPattern;
        LPCWSTR filter = nullptr;
        if (!prefixFilter.empty()) {
            filterPattern = utf8ToWide(prefixFilter + "*");
            if (filterPattern.empty()) {
                error = "Credential prefix filter must be valid UTF-8";
                return {};
            }
            filter = filterPattern.c_str();
        }

        DWORD count = 0;
        CredentialListGuard credentials;
        if (!CredEnumerateW(filter, 0, &count, credentials.out())) {
            const DWORD lastError = GetLastError();
            if (lastError == ERROR_NOT_FOUND) {
                error.clear();
                return {};
            }

            error = getWindowsErrorMessage(
                "Failed to enumerate Windows credentials",
                lastError);
            return {};
        }

        std::vector<WindowsCredentialSummary> summaries;
        summaries.reserve(static_cast<size_t>(count));
        PCREDENTIALW* listed = credentials.get();
        for (DWORD index = 0; index < count; ++index) {
            const PCREDENTIALW credential = listed[index];
            if (!credential || credential->Type != CRED_TYPE_GENERIC) {
                continue;
            }

            WindowsCredentialSummary summary;
            if (credential->TargetName) {
                summary.targetName = wideToUtf8(
                    credential->TargetName,
                    wcslen(credential->TargetName));
            }
            if (credential->UserName) {
                summary.userName = wideToUtf8(
                    credential->UserName,
                    wcslen(credential->UserName));
            }
            if (credential->Comment) {
                summary.comment = wideToUtf8(
                    credential->Comment,
                    wcslen(credential->Comment));
            }
            summaries.push_back(std::move(summary));
        }

        error.clear();
        return summaries;
    }
};

} // namespace

WindowsCredentialStore::WindowsCredentialStore()
    : backend_(std::make_shared<WindowsCredentialBackend>()) {}

WindowsCredentialStore::WindowsCredentialStore(std::shared_ptr<IWindowsCredentialBackend> backend)
    : backend_(backend ? std::move(backend) : std::make_shared<WindowsCredentialBackend>()) {}

bool WindowsCredentialStore::writeGenericCredential(const WindowsCredentialWriteRequest& request) {
    if (!backend_) {
        lastError_ = "Windows credential backend is not configured";
        return false;
    }

    if (!backend_->writeGenericCredential(request, lastError_)) {
        return false;
    }

    lastError_.clear();
    return true;
}

std::optional<std::string> WindowsCredentialStore::readGenericCredential(
    const std::string& targetName) {
    if (!backend_) {
        lastError_ = "Windows credential backend is not configured";
        return std::nullopt;
    }

    const std::optional<std::string> secret = backend_->readGenericCredential(targetName, lastError_);
    if (!secret.has_value()) {
        return std::nullopt;
    }

    lastError_.clear();
    return secret;
}

bool WindowsCredentialStore::deleteGenericCredential(const std::string& targetName) {
    if (!backend_) {
        lastError_ = "Windows credential backend is not configured";
        return false;
    }

    if (!backend_->deleteGenericCredential(targetName, lastError_)) {
        return false;
    }

    lastError_.clear();
    return true;
}

std::vector<WindowsCredentialSummary> WindowsCredentialStore::listGenericCredentials(
    const std::string& prefixFilter) {
    if (!backend_) {
        lastError_ = "Windows credential backend is not configured";
        return {};
    }

    std::vector<WindowsCredentialSummary> credentials =
        backend_->listGenericCredentials(prefixFilter, lastError_);
    if (!lastError_.empty()) {
        return {};
    }

    lastError_.clear();
    return credentials;
}

std::string WindowsCredentialStore::getLastError() const {
    return lastError_;
}

void WindowsCredentialStore::setBackend(std::shared_ptr<IWindowsCredentialBackend> backend) {
    backend_ = backend;
    lastError_.clear();
}

} // namespace JiStats
