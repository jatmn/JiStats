#pragma once

#include <string>
#include <vector>
#include <memory>

namespace JiStats {

/**
 * Simple HTTP client using WinHTTP for making API calls.
 * 
 * Per AGENTS.md, we avoid heavy frameworks. WinHTTP is a native Windows API
 * that doesn't require additional dependencies.
 */

class HttpResponse {
public:
    bool success = false;
    int statusCode = 0;
    std::string body;
    std::string error;
};

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    // Disable copy
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    /**
     * Set the API key for authenticated requests.
     */
    void setApiKey(const std::string& apiKey);

    /**
     * Set the Group ID for MiniMax API.
     */
    void setGroupId(const std::string& groupId);

    /**
     * Set request timeout in milliseconds (default: 30000).
     */
    void setTimeout(unsigned int timeoutMs);

    /**
     * Set the WinHTTP user agent string for new requests.
     */
    void setUserAgent(const std::string& userAgent);

    /**
     * Set the Content-Type header used for requests with bodies.
     */
    void setContentType(const std::string& contentType);

    /**
     * Add or replace an extra request header.
     */
    void setHeader(const std::string& name, const std::string& value);

    /**
     * Remove all extra request headers.
     */
    void clearHeaders();

    /**
     * Perform a GET request.
     */
    HttpResponse get(const std::string& url);

    /**
     * Perform a POST request with optional body.
     */
    HttpResponse post(const std::string& url, const std::string& body = "");

    /**
     * Get the last error message.
     */
    std::string getLastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace JiStats
