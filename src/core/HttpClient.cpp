#include "core/HttpClient.hpp"
#include <windows.h>
#include <winhttp.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace JiStats {

namespace {

class WinHttpHandle {
public:
    explicit WinHttpHandle(HINTERNET handle = nullptr) : handle_(handle) {}

    ~WinHttpHandle() {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
    }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    WinHttpHandle(WinHttpHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) {
            if (handle_ != nullptr) {
                WinHttpCloseHandle(handle_);
            }
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    HINTERNET get() const {
        return handle_;
    }

private:
    HINTERNET handle_;
};

static std::wstring stringToWstring(const std::string& str) {
    return std::wstring(str.begin(), str.end());
}

static std::string getLastErrorMessage(const std::string& prefix) {
    DWORD error = GetLastError();
    char errorMsg[256] = {};
    DWORD charsWritten = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, error, 0, errorMsg,
                                        static_cast<DWORD>(sizeof(errorMsg)), nullptr);
    if (charsWritten == 0) {
        return prefix + " (WinHTTP error " + std::to_string(error) + ")";
    }

    std::string message(errorMsg);
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n')) {
        message.pop_back();
    }
    return prefix + ": " + message;
}

static std::wstring buildRequestPath(const URL_COMPONENTS& urlComp) {
    std::wstring path;
    if (urlComp.lpszUrlPath != nullptr && urlComp.dwUrlPathLength > 0) {
        path.assign(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);
    }

    if (path.empty()) {
        path = L"/";
    }

    if (urlComp.lpszExtraInfo != nullptr && urlComp.dwExtraInfoLength > 0) {
        std::wstring extraInfo(urlComp.lpszExtraInfo, urlComp.dwExtraInfoLength);
        const size_t fragmentOffset = extraInfo.find(L'#');
        if (fragmentOffset != std::wstring::npos) {
            extraInfo.erase(fragmentOffset);
        }
        path.append(extraInfo);
    }

    return path;
}

static bool receiveResponse(HINTERNET request, HttpResponse& response) {
    if (!WinHttpReceiveResponse(request, nullptr)) {
        response.error = getLastErrorMessage("Failed to receive response");
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &statusCode,
                             &statusCodeSize, WINHTTP_NO_HEADER_INDEX)) {
        response.error = getLastErrorMessage("Failed to query status code");
        return false;
    }
    response.statusCode = static_cast<int>(statusCode);

    constexpr DWORD bufferSize = 65536;
    std::vector<char> buffer(bufferSize);
    DWORD bytesRead = 0;

    while (true) {
        if (!WinHttpReadData(request, buffer.data(), bufferSize, &bytesRead)) {
            response.error = getLastErrorMessage("Failed to read response body");
            return false;
        }

        if (bytesRead == 0) {
            break;
        }

        response.body.append(buffer.data(), bytesRead);
    }

    response.success = (statusCode >= 200 && statusCode < 300);
    if (!response.success) {
        response.error = "HTTP " + std::to_string(statusCode);
    }

    return true;
}

} // namespace

class HttpClient::Impl {
public:
    std::string apiKey;
    std::string groupId;
    unsigned int timeoutMs = 30000;
    std::string lastError;
    std::string userAgent = "JiStats/1.0";
    std::string contentType = "application/json";
    std::vector<std::pair<std::string, std::string>> extraHeaders;
};

HttpClient::HttpClient()
    : pImpl(std::make_unique<Impl>()) {
}

HttpClient::~HttpClient() = default;

void HttpClient::setApiKey(const std::string& apiKey) {
    pImpl->apiKey = apiKey;
}

void HttpClient::setGroupId(const std::string& groupId) {
    pImpl->groupId = groupId;
}

void HttpClient::setTimeout(unsigned int timeoutMs) {
    pImpl->timeoutMs = timeoutMs;
}

void HttpClient::setUserAgent(const std::string& userAgent) {
    pImpl->userAgent = userAgent.empty() ? "JiStats/1.0" : userAgent;
}

void HttpClient::setContentType(const std::string& contentType) {
    pImpl->contentType = contentType.empty() ? "application/json" : contentType;
}

void HttpClient::setHeader(const std::string& name, const std::string& value) {
    for (auto& header : pImpl->extraHeaders) {
        if (header.first == name) {
            header.second = value;
            return;
        }
    }

    pImpl->extraHeaders.emplace_back(name, value);
}

void HttpClient::clearHeaders() {
    pImpl->extraHeaders.clear();
}

HttpResponse HttpClient::get(const std::string& url) {
    HttpResponse response;
    response.success = false;
    pImpl->lastError.clear();

    URL_COMPONENTS urlComp = {};
    urlComp.dwStructSize = sizeof(urlComp);
    urlComp.dwSchemeLength = static_cast<DWORD>(-1);
    urlComp.dwHostNameLength = static_cast<DWORD>(-1);
    urlComp.dwUrlPathLength = static_cast<DWORD>(-1);
    urlComp.dwExtraInfoLength = static_cast<DWORD>(-1);

    std::wstring urlW = stringToWstring(url);
    if (!WinHttpCrackUrl(urlW.c_str(), static_cast<DWORD>(urlW.length()), 0, &urlComp)) {
        response.error = "Failed to parse URL";
        pImpl->lastError = response.error;
        return response;
    }

    WinHttpHandle session(WinHttpOpen(stringToWstring(pImpl->userAgent).c_str(),
                                      WINHTTP_ACCESS_TYPE_NO_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.get()) {
        response.error = getLastErrorMessage("Failed to open WinHTTP session");
        pImpl->lastError = response.error;
        return response;
    }

    std::wstring hostName(urlComp.lpszHostName, urlComp.dwHostNameLength);
    WinHttpHandle connect(WinHttpConnect(session.get(), hostName.c_str(), urlComp.nPort, 0));
    if (!connect.get()) {
        response.error = getLastErrorMessage("Failed to connect to host");
        pImpl->lastError = response.error;
        return response;
    }

    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    std::wstring path = buildRequestPath(urlComp);
    WinHttpHandle request(WinHttpOpenRequest(connect.get(), L"GET", path.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request.get()) {
        response.error = getLastErrorMessage("Failed to open request");
        pImpl->lastError = response.error;
        return response;
    }

    std::wstring headers;
    if (!pImpl->apiKey.empty()) {
        headers += L"Authorization: Bearer " + stringToWstring(pImpl->apiKey) + L"\r\n";
    }
    if (!pImpl->groupId.empty()) {
        headers += L"GroupId: " + stringToWstring(pImpl->groupId) + L"\r\n";
    }
    for (const auto& header : pImpl->extraHeaders) {
        headers += stringToWstring(header.first) + L": " + stringToWstring(header.second) + L"\r\n";
    }
    headers += L"Content-Type: " + stringToWstring(pImpl->contentType) + L"\r\n";

    if (!WinHttpSendRequest(request.get(), headers.c_str(),
                            static_cast<DWORD>(headers.length()),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        response.error = getLastErrorMessage("Failed to send request");
        pImpl->lastError = response.error;
        return response;
    }

    if (!receiveResponse(request.get(), response)) {
        pImpl->lastError = response.error;
        return response;
    }

    pImpl->lastError = response.error;
    return response;
}

HttpResponse HttpClient::post(const std::string& url, const std::string& body) {
    HttpResponse response;
    response.success = false;
    pImpl->lastError.clear();

    URL_COMPONENTS urlComp = {};
    urlComp.dwStructSize = sizeof(urlComp);
    urlComp.dwSchemeLength = static_cast<DWORD>(-1);
    urlComp.dwHostNameLength = static_cast<DWORD>(-1);
    urlComp.dwUrlPathLength = static_cast<DWORD>(-1);
    urlComp.dwExtraInfoLength = static_cast<DWORD>(-1);

    std::wstring urlW = stringToWstring(url);
    if (!WinHttpCrackUrl(urlW.c_str(), static_cast<DWORD>(urlW.length()), 0, &urlComp)) {
        response.error = "Failed to parse URL";
        pImpl->lastError = response.error;
        return response;
    }

    WinHttpHandle session(WinHttpOpen(stringToWstring(pImpl->userAgent).c_str(),
                                      WINHTTP_ACCESS_TYPE_NO_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.get()) {
        response.error = getLastErrorMessage("Failed to open WinHTTP session");
        pImpl->lastError = response.error;
        return response;
    }

    std::wstring hostName(urlComp.lpszHostName, urlComp.dwHostNameLength);
    WinHttpHandle connect(WinHttpConnect(session.get(), hostName.c_str(), urlComp.nPort, 0));
    if (!connect.get()) {
        response.error = getLastErrorMessage("Failed to connect to host");
        pImpl->lastError = response.error;
        return response;
    }

    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    std::wstring path = buildRequestPath(urlComp);
    WinHttpHandle request(WinHttpOpenRequest(connect.get(), L"POST", path.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request.get()) {
        response.error = getLastErrorMessage("Failed to open request");
        pImpl->lastError = response.error;
        return response;
    }

    std::wstring headers;
    if (!pImpl->apiKey.empty()) {
        headers += L"Authorization: Bearer " + stringToWstring(pImpl->apiKey) + L"\r\n";
    }
    if (!pImpl->groupId.empty()) {
        headers += L"GroupId: " + stringToWstring(pImpl->groupId) + L"\r\n";
    }
    for (const auto& header : pImpl->extraHeaders) {
        headers += stringToWstring(header.first) + L": " + stringToWstring(header.second) + L"\r\n";
    }
    headers += L"Content-Type: " + stringToWstring(pImpl->contentType) + L"\r\n";

    LPVOID requestBody = body.empty()
        ? WINHTTP_NO_REQUEST_DATA
        : static_cast<LPVOID>(const_cast<char*>(body.data()));
    DWORD bodyLength = body.empty() ? 0 : static_cast<DWORD>(body.length());

    if (!WinHttpSendRequest(request.get(), headers.c_str(),
                            static_cast<DWORD>(headers.length()),
                            requestBody,
                            bodyLength,
                            bodyLength, 0)) {
        response.error = getLastErrorMessage("Failed to send request");
        pImpl->lastError = response.error;
        return response;
    }

    if (!receiveResponse(request.get(), response)) {
        pImpl->lastError = response.error;
        return response;
    }

    pImpl->lastError = response.error;
    return response;
}

std::string HttpClient::getLastError() const {
    return pImpl->lastError;
}

} // namespace JiStats
