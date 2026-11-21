#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace JiStats {

struct WindowsCredentialWriteRequest {
    std::string targetName;
    std::string secret;
    std::string userName;
    std::string comment;
};

struct WindowsCredentialSummary {
    std::string targetName;
    std::string userName;
    std::string comment;
};

class IWindowsCredentialBackend {
public:
    virtual ~IWindowsCredentialBackend() = default;

    virtual bool writeGenericCredential(
        const WindowsCredentialWriteRequest& request,
        std::string& error) = 0;

    virtual std::optional<std::string> readGenericCredential(
        const std::string& targetName,
        std::string& error) = 0;

    virtual bool deleteGenericCredential(
        const std::string& targetName,
        std::string& error) = 0;

    virtual std::vector<WindowsCredentialSummary> listGenericCredentials(
        const std::string& prefixFilter,
        std::string& error) = 0;
};

class WindowsCredentialStore {
public:
    WindowsCredentialStore();
    explicit WindowsCredentialStore(std::shared_ptr<IWindowsCredentialBackend> backend);

    bool writeGenericCredential(const WindowsCredentialWriteRequest& request);
    std::optional<std::string> readGenericCredential(const std::string& targetName);
    bool deleteGenericCredential(const std::string& targetName);
    std::vector<WindowsCredentialSummary> listGenericCredentials(
        const std::string& prefixFilter = "");

    std::string getLastError() const;
    void setBackend(std::shared_ptr<IWindowsCredentialBackend> backend);

private:
    std::shared_ptr<IWindowsCredentialBackend> backend_;
    std::string lastError_;
};

} // namespace JiStats
