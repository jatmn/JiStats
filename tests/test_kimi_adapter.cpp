#define JISTATS_ENABLE_TEST_HOOKS 1

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>

#include "core/HttpClient.hpp"
#include "db/SchemaManager.hpp"
#include "db/StorageService.hpp"
#include "providers/kimi/KimiAdapter.hpp"
#include "sqlite_test_support.hpp"
#include "temp_test_directory.hpp"

using namespace JiStats;
using JiStats::TestSupport::requireSqlite;
using JiStats::TestSupport::TempTestDatabase;
using JiStats::TestSupport::TempTestDirectory;

namespace {

const std::string kFixtureRoot = "docs/fixtures/kimi/synthetic_local_runtime";

class TempFixtureDirectory {
public:
    std::filesystem::path makeFile(const std::filesystem::path& relativePath, const std::string& contents) const {
        const std::filesystem::path filePath = directory_.path() / relativePath;
        std::filesystem::create_directories(filePath.parent_path());
        std::ofstream output(filePath, std::ios::binary);
        if (!output.is_open()) {
            throw std::runtime_error("Failed to create temporary Kimi fixture file");
        }

        output << contents;
        return filePath;
    }

    const std::filesystem::path& path() const {
        return directory_.path();
    }

private:
    TempTestDirectory directory_{"jistats_kimi_fixture"};
};

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const char* name, const std::string& value)
        : name_(name) {
        char* existing = nullptr;
        std::size_t existingLength = 0;
        if (_dupenv_s(&existing, &existingLength, name) == 0 && existing != nullptr) {
            hadValue_ = true;
            previousValue_ = existing;
            free(existing);
        }
        _putenv_s(name, value.c_str());
    }

    ~ScopedEnvironmentVariable() {
        if (hadValue_) {
            _putenv_s(name_.c_str(), previousValue_.c_str());
        } else {
            _putenv_s(name_.c_str(), "");
        }
    }

private:
    std::string name_;
    std::string previousValue_;
    bool hadValue_ = false;
};

class ScopedWinsock {
public:
    ScopedWinsock() {
        const int result = WSAStartup(MAKEWORD(2, 2), &data_);
        if (result != 0) {
            throw std::runtime_error("Failed to initialize Winsock");
        }
    }

    ~ScopedWinsock() {
        WSACleanup();
    }

private:
    WSADATA data_{};
};

struct ExpectedHttpExchange {
    std::string method;
    std::string path;
    int statusCode = 200;
    std::string body;
    std::string contentType = "application/json";
    std::chrono::milliseconds responseDelay{0};
};

class LocalHttpSequenceServer {
public:
    explicit LocalHttpSequenceServer(std::vector<ExpectedHttpExchange> exchanges)
        : exchanges_(std::move(exchanges)) {
        listenSocket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket_ == INVALID_SOCKET) {
            throw std::runtime_error("Failed to create test HTTP socket");
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(listenSocket_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            closesocket(listenSocket_);
            throw std::runtime_error("Failed to bind test HTTP socket");
        }

        if (listen(listenSocket_, SOMAXCONN) == SOCKET_ERROR) {
            closesocket(listenSocket_);
            throw std::runtime_error("Failed to listen on test HTTP socket");
        }

        sockaddr_in bound{};
        int boundSize = sizeof(bound);
        if (getsockname(listenSocket_, reinterpret_cast<sockaddr*>(&bound), &boundSize) == SOCKET_ERROR) {
            closesocket(listenSocket_);
            throw std::runtime_error("Failed to read test HTTP socket address");
        }

        port_ = ntohs(bound.sin_port);
        thread_ = std::thread([this]() { run(); });
    }

    ~LocalHttpSequenceServer() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        responseWait_.notify_all();
        if (listenSocket_ != INVALID_SOCKET) {
            closesocket(listenSocket_);
            listenSocket_ = INVALID_SOCKET;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    std::string baseUrl() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    std::size_t handledRequestCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return handledRequestCount_;
    }

    std::string failure() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return failure_;
    }

    bool responseStarted() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return responseStarted_;
    }

private:
    static std::string statusText(int statusCode) {
        switch (statusCode) {
        case 200: return "OK";
        case 401: return "Unauthorized";
        default: return "Test";
        }
    }

    static std::string readRequest(SOCKET socket) {
        std::string data;
        char buffer[4096];
        std::size_t expectedLength = 0;
        std::size_t headerEnd = std::string::npos;

        while (true) {
            const int received = recv(socket, buffer, sizeof(buffer), 0);
            if (received <= 0) {
                break;
            }
            data.append(buffer, static_cast<std::size_t>(received));
            headerEnd = data.find("\r\n\r\n");
            if (headerEnd == std::string::npos) {
                continue;
            }

            const std::string headers = data.substr(0, headerEnd);
            const std::size_t contentLengthPos = headers.find("Content-Length:");
            if (contentLengthPos != std::string::npos) {
                const std::size_t valueStart = contentLengthPos + std::string("Content-Length:").size();
                const std::size_t valueEnd = headers.find("\r\n", valueStart);
                expectedLength = static_cast<std::size_t>(std::stoul(headers.substr(valueStart, valueEnd - valueStart)));
            }

            if (data.size() >= headerEnd + 4 + expectedLength) {
                break;
            }
        }

        return data;
    }

    static bool sendAll(SOCKET socket, const std::string& data) {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const int sent = send(socket, data.data() + offset, static_cast<int>(data.size() - offset), 0);
            if (sent <= 0) {
                return false;
            }
            offset += static_cast<std::size_t>(sent);
        }
        return true;
    }

    void run() {
        for (std::size_t index = 0; index < exchanges_.size(); ++index) {
            sockaddr_in clientAddr{};
            int clientSize = sizeof(clientAddr);
            const SOCKET client = accept(listenSocket_, reinterpret_cast<sockaddr*>(&clientAddr), &clientSize);
            if (client == INVALID_SOCKET) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (failure_.empty()) {
                    failure_ = "accept failed";
                }
                return;
            }

            const std::string request = readRequest(client);
            const std::size_t lineEnd = request.find("\r\n");
            const std::string requestLine = lineEnd == std::string::npos ? request : request.substr(0, lineEnd);
            const std::string expectedPrefix = exchanges_[index].method + " " + exchanges_[index].path + " ";
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ++handledRequestCount_;
                if (failure_.empty() && requestLine.rfind(expectedPrefix, 0) != 0) {
                    failure_ = "unexpected request line: " + requestLine;
                }
                if (exchanges_[index].responseDelay.count() > 0 &&
                    responseWait_.wait_for(lock, exchanges_[index].responseDelay, [this] { return stopping_; })) {
                    closesocket(client);
                    return;
                }
                responseStarted_ = true;
            }

            std::ostringstream response;
            response << "HTTP/1.1 " << exchanges_[index].statusCode << " "
                     << statusText(exchanges_[index].statusCode) << "\r\n";
            response << "Content-Type: " << exchanges_[index].contentType << "\r\n";
            response << "Content-Length: " << exchanges_[index].body.size() << "\r\n";
            response << "Connection: close\r\n\r\n";
            response << exchanges_[index].body;
            sendAll(client, response.str());
            closesocket(client);
        }
    }

    SOCKET listenSocket_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    std::thread thread_;
    std::vector<ExpectedHttpExchange> exchanges_;
    mutable std::mutex mutex_;
    std::condition_variable responseWait_;
    std::size_t handledRequestCount_ = 0;
    bool responseStarted_ = false;
    bool stopping_ = false;
    std::string failure_;
};

std::string sampleKimiUsagesResponse() {
    return R"json({
  "usage": {
    "limit": "100",
    "used": "23",
    "remaining": "77",
    "resetTime": "2026-04-21T16:23:13.582922Z"
  },
  "limits": [
    {
      "window": { "duration": 300, "timeUnit": "TIME_UNIT_MINUTE" },
      "detail": {
        "limit": "100",
        "used": "40",
        "remaining": "60",
        "resetTime": "2026-04-20T23:23:13.582922Z"
      }
    }
  ]
})json";
}

} // namespace

TEST_CASE("WinHTTP requests report the configured receive timeout", "[http][timeout]") {
    constexpr unsigned int timeoutMs = 5000;
    constexpr auto withheldResponse = std::chrono::seconds(9);
    ScopedWinsock winsock;
    LocalHttpSequenceServer getServer({{"GET", "/slow", 200, "{}", "application/json", withheldResponse}});
    LocalHttpSequenceServer postServer({{"POST", "/slow", 200, "{}", "application/json", withheldResponse}});
    HttpClient getClient;
    HttpClient postClient;
    getClient.setTimeout(timeoutMs);
    postClient.setTimeout(timeoutMs);

    bool getResponseStarted = false;
    auto get = std::async(std::launch::async, [&]() {
        auto response = getClient.get(getServer.baseUrl() + "/slow");
        getResponseStarted = getServer.responseStarted();
        return response;
    });
    const auto postResponse = postClient.post(postServer.baseUrl() + "/slow", "{}");
    const bool postResponseStarted = postServer.responseStarted();
    const auto getResponse = get.get();

    const auto checkTimeout = [](const char* method, const LocalHttpSequenceServer& server, const HttpClient& client,
                                 const HttpResponse& response, bool responseStarted) {
        CAPTURE(method);
        CHECK(server.handledRequestCount() == 1);
        CHECK(server.failure().empty());
        CHECK_FALSE(responseStarted);
        CHECK_FALSE(response.success);
        CHECK(response.statusCode == 0);
        CHECK(response.error == "Failed to receive response: request timed out");
        CHECK(client.getLastError() == response.error);
    };
    checkTimeout("GET", getServer, getClient, getResponse, getResponseStarted);
    checkTimeout("POST", postServer, postClient, postResponse, postResponseStarted);
}

TEST_CASE("Kimi parser reads bounded synthetic fixture snapshots", "[kimi][fixtures]") {
    const KimiLocalParseResult result = KimiAdapter::parseLocalState(kFixtureRoot);

    REQUIRE(result.success);
    REQUIRE(result.error.empty());
    REQUIRE(result.defaultModelKey == "kimi-code/kimi-for-coding");
    REQUIRE(result.providerType == "kimi");
    REQUIRE(result.providerBaseUrl == "https://api.kimi.com/coding/v1");
    REQUIRE(result.sessionCount == 1);
    REQUIRE(result.taskCount == 2);
    REQUIRE(result.completedTaskCount == 1);
    REQUIRE(result.failedTaskCount == 1);
    REQUIRE(result.timedOutTaskCount == 1);
    REQUIRE(result.activitySnapshots.size() == 2);
    REQUIRE(result.normalizedActivityEvents.size() == 2);
    REQUIRE(result.latestObservedAt == 1776679530);

    const KimiLocalActivitySnapshot& completed = result.activitySnapshots[0];
    REQUIRE(completed.sessionId == "session-alpha");
    REQUIRE(completed.taskId == "task-success");
    REQUIRE(completed.planMode);
    REQUIRE(completed.approvalMode.yolo);
    REQUIRE_FALSE(completed.approvalMode.autoApproveActions);
    REQUIRE(completed.dynamicSubagentCount == 2);
    REQUIRE(completed.additionalDirCount == 2);
    REQUIRE(completed.taskRuntime.status == "completed");
    REQUIRE(completed.notificationSummary.type == "task.completed");
    REQUIRE(completed.notificationSummary.severity == "success");
    REQUIRE(completed.notificationSummary.deliveryStatus == "desktop:delivered,in_app:shown");
    REQUIRE(completed.workingDirScope == KimiAdapter::hashLocalPathScope("C:/Users/example/source/project-alpha"));
    REQUIRE(completed.workingDirScope.find("project-alpha") == std::string::npos);

    const ActivityEvent activityEvent = completed.toActivityEvent();
    REQUIRE(activityEvent.providerKey == "kimi");
    REQUIRE(activityEvent.providerSurface == "kimi_local_runtime");
    REQUIRE(activityEvent.confidence == ActivityConfidence::LocalExact);
    REQUIRE(activityEvent.sourceType == "local");
    REQUIRE(activityEvent.windowTitleHint == completed.workingDirScope);
    REQUIRE(activityEvent.rawReference == "kimi_local_runtime:session-alpha/task-success");

    const KimiLocalActivitySnapshot& timedOut = result.activitySnapshots[1];
    REQUIRE(timedOut.taskId == "task-timeout");
    REQUIRE(timedOut.taskRuntime.timedOut);
    REQUIRE(timedOut.notificationSummary.type == "task.failed");
    REQUIRE(timedOut.notificationSummary.payloadTimedOut);

    REQUIRE(result.rawSummary.find("C:/Users/example/source/project-alpha") == std::string::npos);
}

TEST_CASE("Kimi parser ignores excluded transcript-like files and keeps raw paths hashed", "[kimi][fixtures]") {
    TempFixtureDirectory fixture;
    fixture.makeFile("config.toml", R"toml(
default_model = "kimi-code/kimi-for-coding"

[models."kimi-code/kimi-for-coding"]
type = "kimi"
base_url = "https://api.kimi.com/coding/v1"
)toml");
    fixture.makeFile("kimi.json", R"json(
{
  "work_dirs": [
    {
      "path": "C:/Secret/Workspace",
      "last_session_id": "session-one"
    }
  ]
}
)json");
    fixture.makeFile("sessions/session-one/state.json", R"json(
{
  "version": 1,
  "plan_mode": false,
  "approval": {
    "yolo": false,
    "auto_approve_actions": true
  },
  "dynamic_subagents": [],
  "additional_dirs": ["D:/other"]
}
)json");
    fixture.makeFile("sessions/session-one/tasks/task-one/runtime.json", R"json(
{
  "status": "completed",
  "started_at": "2026-04-20T10:00:00Z",
  "updated_at": "2026-04-20T10:01:00Z",
  "finished_at": "2026-04-20T10:02:00Z",
  "exit_code": 0,
  "interrupted": false,
  "timed_out": false
}
)json");
    fixture.makeFile("sessions/session-one/notifications/notify-one/event.json", R"json(
{
  "category": "task",
  "type": "task.completed",
  "source_kind": "background_task",
  "severity": "success",
  "created_at": "2026-04-20T10:02:05Z",
  "payload": {
    "status": "completed",
    "exit_code": 0,
    "interrupted": false,
    "timed_out": false
  }
}
)json");
    fixture.makeFile("sessions/session-one/context.jsonl",
        R"jsonl({"type":"message","content":"very private prompt text"})jsonl");
    fixture.makeFile("sessions/session-one/wire.jsonl",
        R"jsonl({"msg":{"type":"ToolCall","payload":{"secret":"do-not-read"}}})jsonl");

    const KimiLocalParseResult result = KimiAdapter::parseLocalState(fixture.path().string());

    REQUIRE(result.success);
    REQUIRE(result.taskCount == 1);
    REQUIRE(result.activitySnapshots[0].workingDirScope == KimiAdapter::hashLocalPathScope("C:/Secret/Workspace"));
    REQUIRE(result.activitySnapshots[0].workingDirScope.find("Secret") == std::string::npos);
    REQUIRE(result.rawSummary.find("do-not-read") == std::string::npos);
    REQUIRE(result.rawSummary.find("very private prompt text") == std::string::npos);
}

TEST_CASE("Kimi parser reads nested session bucket layouts used by the local app", "[kimi][fixtures]") {
    TempFixtureDirectory fixture;
    fixture.makeFile("config.toml", R"toml(
default_model = "kimi-code/kimi-for-coding"

[models."kimi-code/kimi-for-coding"]
type = "kimi"
base_url = "https://api.kimi.com/coding/v1"
)toml");
    fixture.makeFile("kimi.json", R"json(
{
  "work_dirs": []
}
)json");
    fixture.makeFile("sessions/root-session/run-123/state.json", R"json(
{
  "plan_mode": true,
  "approval": {
    "yolo": false,
    "auto_approve_actions": true
  },
  "dynamic_subagents": ["a"],
  "additional_dirs": []
}
)json");
    fixture.makeFile("sessions/root-session/run-123/tasks/task-one/runtime.json", R"json(
{
  "status": "completed",
  "started_at": "2026-04-20T10:00:00Z",
  "updated_at": "2026-04-20T10:01:00Z",
  "finished_at": "2026-04-20T10:02:00Z",
  "exit_code": 0,
  "interrupted": false,
  "timed_out": false
}
)json");
    fixture.makeFile("sessions/root-session/run-123/notifications/notify-one/event.json", R"json(
{
  "category": "task",
  "type": "task.completed",
  "source_kind": "background_task",
  "severity": "success",
  "created_at": "2026-04-20T10:02:05Z",
  "payload": {
    "status": "completed",
    "exit_code": 0,
    "interrupted": false,
    "timed_out": false
  }
}
)json");

    const KimiLocalParseResult result = KimiAdapter::parseLocalState(fixture.path().string());

    REQUIRE(result.success);
    REQUIRE(result.sessionCount == 1);
    REQUIRE(result.taskCount == 1);
    REQUIRE(result.activitySnapshots.size() == 1);
    CHECK(result.activitySnapshots.front().sessionRootId == "root-session");
    CHECK(result.activitySnapshots.front().sessionId == "root-session/run-123");
    CHECK(result.activitySnapshots.front().taskId == "task-one");
    CHECK(result.normalizedActivityEvents.front().sourceRunId == "root-session/run-123");
}

TEST_CASE("Kimi task activity does not become token usage or subscription quota", "[kimi]") {
    KimiAdapter adapter(kFixtureRoot);

    const QuotaResponse quota = adapter.fetchQuota();
    REQUIRE_FALSE(quota.success);
    REQUIRE(quota.providerSurface == "kimi_subscription_api");
    REQUIRE(quota.error == "Kimi quota API credentials are not available");

    const UsageResponse usage = adapter.fetchUsage(1776679500, 1776679530);
    REQUIRE_FALSE(usage.success);
    CHECK(usage.events.empty());
    CHECK(usage.totalInputTokens == 0);
    CHECK(usage.totalOutputTokens == 0);
    CHECK_FALSE(adapter.fetchLocalActivity().normalizedActivityEvents.empty());
}

TEST_CASE("Kimi adapter reports missing local roots without pretending quota truth", "[kimi]") {
    KimiAdapter adapter("nonexistent/kimi-root");

    const KimiLocalParseResult activity = adapter.fetchLocalActivity();
    REQUIRE_FALSE(activity.success);
    REQUIRE(activity.error == "Kimi local root path does not exist");

    const QuotaResponse quota = adapter.fetchQuota();
    REQUIRE_FALSE(quota.success);
    REQUIRE(quota.providerSurface == "kimi_subscription_api");
    REQUIRE(quota.error == "Kimi quota API credentials are not available");

    REQUIRE_FALSE(adapter.validateCredentials());
}

TEST_CASE("Kimi parser accepts fractional numeric timestamps in runtime.json", "[kimi]") {
    TempFixtureDirectory fixture;
    fixture.makeFile("config.toml", R"toml(
default_model = "kimi-code/kimi-for-coding"

[models."kimi-code/kimi-for-coding"]
type = "kimi"
base_url = "https://api.kimi.com/coding/v1"
)toml");
    fixture.makeFile("kimi.json", R"json(
{
  "work_dirs": []
}
)json");
    fixture.makeFile("sessions/session-one/state.json", R"json(
{
  "version": 1,
  "plan_mode": false,
  "approval": {
    "yolo": false,
    "auto_approve_actions": true
  },
  "dynamic_subagents": [],
  "additional_dirs": []
}
)json");
    fixture.makeFile("sessions/session-one/tasks/task-one/runtime.json", R"json(
{
  "status": "completed",
  "started_at": 1775246461.7841244,
  "updated_at": 1775246472.0858305,
  "finished_at": 1775246472.0858305,
  "exit_code": 0,
  "interrupted": false,
  "timed_out": false
}
)json");

    const KimiLocalParseResult result = KimiAdapter::parseLocalState(fixture.path().string());

    REQUIRE(result.success);
    REQUIRE(result.taskCount == 1);
    REQUIRE(result.activitySnapshots.size() == 1);
    REQUIRE(result.latestObservedAt == 1775246472);
    REQUIRE(result.activitySnapshots[0].taskRuntime.startedAt == 1775246461);
    REQUIRE(result.activitySnapshots[0].taskRuntime.updatedAt == 1775246472);
    REQUIRE(result.activitySnapshots[0].taskRuntime.finishedAt == 1775246472);
    REQUIRE(result.activitySnapshots[0].observedAt == 1775246472);
}

TEST_CASE("Kimi adapter detects an installed local root even before runtime snapshots exist", "[kimi]") {
    TempFixtureDirectory fixture;
    fixture.makeFile("config.toml", R"toml(
default_model = "kimi-code/kimi-for-coding"

[models."kimi-code/kimi-for-coding"]
type = "kimi"
base_url = "https://api.kimi.com/coding/v1"
)toml");
    fixture.makeFile("kimi.json", R"json(
{
  "work_dirs": []
}
)json");
    std::filesystem::create_directories(fixture.path() / "sessions");

    KimiAdapter adapter(fixture.path().string());
    CHECK(KimiAdapter::hasLocalInstallation(fixture.path().string()));
    CHECK(adapter.validateCredentials());

    const KimiLocalParseResult activity = adapter.fetchLocalActivity();
    REQUIRE_FALSE(activity.success);
    CHECK(activity.error == "No approved Kimi runtime snapshots were found");
}

TEST_CASE("Kimi importLocalActivity persists approved local runtime snapshots", "[kimi][persistence]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    SchemaManager schemaManager(storage);
    REQUIRE(schemaManager.migrate());

    REQUIRE(KimiAdapter::importLocalActivity(storage, kFixtureRoot));

    QueryResult result = storage.execute(
        "SELECT COUNT(*), MIN(source_type), MAX(provider_surface) FROM activity_events;");
    REQUIRE(result.success());
    REQUIRE(result.rowCount() == 1);
    CHECK(result.getInt64(0, 0) == 2);
    CHECK(result.getString(0, 1) == "local");
    CHECK(result.getString(0, 2) == "kimi_local_runtime");
}

TEST_CASE("Kimi task imports preserve one activity across lifecycle updates", "[kimi][persistence]") {
    requireSqlite();
    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    TempFixtureDirectory fixture;
    const auto runtimePath = "sessions/session-one/tasks/task-one/runtime.json";
    const auto starting = R"json({
        "status": "starting",
        "started_at": "2026-04-20T10:01:00Z",
        "updated_at": "2026-04-20T10:01:00Z"
    })json";
    const auto running = R"json({
        "status": "running",
        "started_at": "2026-04-20T10:01:00Z",
        "updated_at": "2026-04-20T10:01:10Z"
    })json";
    const auto completed = R"json({
        "status": "completed",
        "started_at": "2026-04-20T10:01:00Z",
        "updated_at": "2026-04-20T10:02:30Z",
        "finished_at": "2026-04-20T10:02:30Z"
    })json";
    const auto awaitingApproval = R"json({
        "status": "awaiting_approval",
        "started_at": "2026-04-20T10:01:00Z",
        "updated_at": "2026-04-20T10:03:00Z"
    })json";
    const auto loadActivity = [&] {
        return storage.execute(
            "SELECT id, surface_key, ended_at_utc, duration_seconds, raw_reference FROM activity_events;");
    };

    fixture.makeFile(runtimePath, starting);
    REQUIRE(KimiAdapter::importLocalActivity(storage, fixture.path().string()));
    const auto first = loadActivity();
    REQUIRE(first.success());
    REQUIRE(first.rowCount() == 1);
    CHECK(first.getString(0, 1) == "starting");

    fixture.makeFile(runtimePath, running);
    REQUIRE(KimiAdapter::importLocalActivity(storage, fixture.path().string()));
    REQUIRE(KimiAdapter::importLocalActivity(storage, fixture.path().string()));
    const auto initial = loadActivity();
    REQUIRE(initial.success());
    REQUIRE(initial.rowCount() == 1);
    CHECK(initial.getInt64(0, 0) == first.getInt64(0, 0));
    CHECK(initial.getString(0, 1) == "running");
    CHECK(initial.getInt64(0, 3) == 10);

    fixture.makeFile(runtimePath, completed);
    REQUIRE(KimiAdapter::importLocalActivity(storage, fixture.path().string()));
    REQUIRE(KimiAdapter::importLocalActivity(storage, fixture.path().string()));
    fixture.makeFile(runtimePath, running);
    REQUIRE(KimiAdapter::importLocalActivity(storage, fixture.path().string()));

    fixture.makeFile(runtimePath, awaitingApproval);
    REQUIRE(KimiAdapter::importLocalActivity(storage, fixture.path().string()));

    const auto final = loadActivity();
    REQUIRE(final.success());
    REQUIRE(final.rowCount() == 1);
    CHECK(final.getInt64(0, 0) == initial.getInt64(0, 0));
    CHECK(final.getString(0, 1) == "completed");
    CHECK(final.getString(0, 2) == "2026-04-20T10:02:30Z");
    CHECK(final.getInt64(0, 3) == 90);
    CHECK(final.getString(0, 4) == "kimi_local_runtime:session-one/task-one");
}

TEST_CASE("Kimi adapter parses provider usages API response", "[kimi]") {
    const std::time_t before = std::time(nullptr);
    const std::string apiResponse = R"json({
  "user": {
    "userId": "test-user",
    "region": "REGION_OVERSEA",
    "membership": { "level": "LEVEL_BASIC" }
  },
  "usage": {
    "limit": "100",
    "used": "23",
    "remaining": "77",
    "resetTime": "2026-04-21T16:23:13.582922Z"
  },
  "limits": [
    {
      "window": {
        "duration": 300,
        "timeUnit": "TIME_UNIT_MINUTE"
      },
      "detail": {
        "limit": "100",
        "used": "40",
        "remaining": "60",
        "resetTime": "2026-04-20T23:23:13.582922Z"
      }
    }
  ],
  "parallel": { "limit": "10" }
})json";

    const QuotaResponse quota = KimiAdapter::parseUsagesResponseForTesting(apiResponse);
    REQUIRE(quota.success);
    REQUIRE(quota.providerSurface == "kimi_subscription_api");
    REQUIRE(quota.windowType == "interval");
    REQUIRE(quota.used == 40);
    REQUIRE(quota.limit == 100);
    REQUIRE(quota.remaining == 60);
    REQUIRE(quota.secondaryWindowType == "weekly");
    REQUIRE(quota.secondaryUsed == 23);
    REQUIRE(quota.secondaryLimit == 100);
    REQUIRE(quota.secondaryRemaining == 77);
    REQUIRE(quota.snapshotTime >= before);
    REQUIRE(quota.snapshotTime <= std::time(nullptr));
}

TEST_CASE("Kimi adapter requires API credentials for quota mirroring", "[kimi]") {
    TempFixtureDirectory fixture;
    fixture.makeFile("config.toml", R"toml(
default_model = "kimi-code/kimi-for-coding"

[models."kimi-code/kimi-for-coding"]
type = "kimi"
base_url = "https://api.kimi.com/coding/v1"
)toml");
    fixture.makeFile("kimi.json", R"json({"work_dirs": []})json");
    fixture.makeFile("sessions/session-one/state.json", R"json(
{
  "version": 1,
  "plan_mode": false,
  "approval": { "yolo": false, "auto_approve_actions": true },
  "dynamic_subagents": [],
  "additional_dirs": []
}
)json");
    fixture.makeFile("sessions/session-one/tasks/task-one/runtime.json", R"json(
{
  "status": "completed",
  "started_at": "2026-04-20T10:00:00Z",
  "updated_at": "2026-04-20T10:01:00Z",
  "finished_at": "2026-04-20T10:02:00Z",
  "exit_code": 0,
  "interrupted": false,
  "timed_out": false
}
)json");

    KimiAdapter adapter(fixture.path().string());
    const QuotaResponse quota = adapter.fetchQuota();
    REQUIRE_FALSE(quota.success);
    REQUIRE(quota.providerSurface == "kimi_subscription_api");
    REQUIRE(quota.error == "Kimi quota API credentials are not available");
}

TEST_CASE("Kimi adapter advertises provider_exact_quota when credentials exist", "[kimi]") {
    TempFixtureDirectory fixture;
    fixture.makeFile("config.toml", R"toml(
default_model = "kimi-code/kimi-for-coding"
)toml");
    fixture.makeFile("credentials/kimi-code.json", R"json({
  "access_token": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.test",
  "token_type": "Bearer",
  "expires_at": 9999999999.0
})json");

    KimiAdapter adapter(fixture.path().string());
    const auto capabilities = adapter.getCapabilities();

    const auto exactQuota = std::find_if(capabilities.begin(), capabilities.end(),
        [](const Capability& cap) { return cap.name == "provider_exact_quota"; });
    const auto localEstimate = std::find_if(capabilities.begin(), capabilities.end(),
        [](const Capability& cap) { return cap.name == "local_runtime_quota_estimate"; });
    REQUIRE(exactQuota != capabilities.end());
    CHECK(exactQuota->supported);
    CHECK(localEstimate == capabilities.end());
}

TEST_CASE("Kimi adapter advertises provider_exact_quota when only a refresh token can recover access", "[kimi]") {
    TempFixtureDirectory fixture;
    fixture.makeFile("config.toml", R"toml(
default_model = "kimi-code/kimi-for-coding"
)toml");
    fixture.makeFile("credentials/kimi-code.json", R"json({
  "access_token": "expired-token",
  "refresh_token": "refresh-token",
  "scope": "openid profile",
  "token_type": "Bearer",
  "expires_in": 900,
  "expires_at": 1.0
})json");

    KimiAdapter adapter(fixture.path().string());
    const auto capabilities = adapter.getCapabilities();

    const auto exactQuota = std::find_if(capabilities.begin(), capabilities.end(),
        [](const Capability& cap) { return cap.name == "provider_exact_quota"; });
    REQUIRE(exactQuota != capabilities.end());
    CHECK(exactQuota->supported);
    CHECK(adapter.hasDirectQuotaApiConfigured());
}

TEST_CASE("Kimi adapter does not advertise provider_exact_quota without credentials", "[kimi]") {
    TempFixtureDirectory fixture;
    fixture.makeFile("config.toml", R"toml(
default_model = "kimi-code/kimi-for-coding"
)toml");

    KimiAdapter adapter(fixture.path().string());
    const auto capabilities = adapter.getCapabilities();

    const auto exactQuota = std::find_if(capabilities.begin(), capabilities.end(),
        [](const Capability& cap) { return cap.name == "provider_exact_quota"; });
    const auto localEstimate = std::find_if(capabilities.begin(), capabilities.end(),
        [](const Capability& cap) { return cap.name == "local_runtime_quota_estimate"; });
    REQUIRE(exactQuota != capabilities.end());
    CHECK_FALSE(exactQuota->supported);
    CHECK(localEstimate == capabilities.end());
}

TEST_CASE("Kimi adapter refreshes expired OAuth credentials before requesting quota", "[kimi]") {
    ScopedWinsock winsock;
    LocalHttpSequenceServer server({
        {"POST", "/api/oauth/token", 200, R"json({
  "access_token": "fresh-access-token",
  "refresh_token": "fresh-refresh-token",
  "expires_in": 900,
  "scope": "openid profile",
  "token_type": "Bearer"
})json"},
        {"GET", "/coding/v1/usages", 200, sampleKimiUsagesResponse()}
    });
    ScopedEnvironmentVariable oauthUrl(
        "JISTATS_KIMI_OAUTH_TOKEN_URL",
        server.baseUrl() + "/api/oauth/token");
    ScopedEnvironmentVariable usagesUrl(
        "JISTATS_KIMI_USAGES_URL",
        server.baseUrl() + "/coding/v1/usages");

    TempFixtureDirectory fixture;
    fixture.makeFile("config.toml", "default_model = \"kimi-code/kimi-for-coding\"\n");
    fixture.makeFile("credentials/kimi-code.json", R"json({
  "access_token": "expired-access-token",
  "refresh_token": "refresh-token",
  "scope": "openid profile",
  "token_type": "Bearer",
  "expires_in": 900,
  "expires_at": 1.0
})json");

    KimiAdapter adapter(fixture.path().string());
    const QuotaResponse quota = adapter.fetchQuota();

    REQUIRE(server.failure().empty());
    REQUIRE(server.handledRequestCount() == 2);
    REQUIRE(quota.success);
    CHECK(quota.used == 40);
    CHECK(quota.limit == 100);
    CHECK(quota.secondaryUsed == 23);

    std::string refreshedCredentials;
    REQUIRE(std::ifstream(fixture.path() / "credentials" / "kimi-code.json").good());
    {
        std::ifstream input(fixture.path() / "credentials" / "kimi-code.json", std::ios::binary);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        refreshedCredentials = buffer.str();
    }
    CHECK(refreshedCredentials.find("fresh-access-token") != std::string::npos);
    CHECK(refreshedCredentials.find("fresh-refresh-token") != std::string::npos);
}

TEST_CASE("Kimi adapter refreshes OAuth credentials after a 401 usages response and retries quota", "[kimi]") {
    ScopedWinsock winsock;
    LocalHttpSequenceServer server({
        {"GET", "/coding/v1/usages", 401, R"json({"code":"unauthenticated"})json"},
        {"POST", "/api/oauth/token", 200, R"json({
  "access_token": "retry-access-token",
  "refresh_token": "retry-refresh-token",
  "expires_in": 900,
  "scope": "openid profile",
  "token_type": "Bearer"
})json"},
        {"GET", "/coding/v1/usages", 200, sampleKimiUsagesResponse()}
    });
    ScopedEnvironmentVariable oauthUrl(
        "JISTATS_KIMI_OAUTH_TOKEN_URL",
        server.baseUrl() + "/api/oauth/token");
    ScopedEnvironmentVariable usagesUrl(
        "JISTATS_KIMI_USAGES_URL",
        server.baseUrl() + "/coding/v1/usages");

    TempFixtureDirectory fixture;
    fixture.makeFile("config.toml", "default_model = \"kimi-code/kimi-for-coding\"\n");
    fixture.makeFile("credentials/kimi-code.json", R"json({
  "access_token": "stale-access-token",
  "refresh_token": "refresh-token",
  "scope": "openid profile",
  "token_type": "Bearer",
  "expires_in": 900,
  "expires_at": 9999999999.0
})json");

    KimiAdapter adapter(fixture.path().string());
    const QuotaResponse quota = adapter.fetchQuota();

    REQUIRE(server.failure().empty());
    REQUIRE(server.handledRequestCount() == 3);
    REQUIRE(quota.success);
    CHECK(quota.used == 40);
    CHECK(quota.limit == 100);
    CHECK(quota.secondaryRemaining == 77);
}

TEST_CASE("Kimi adapter resolves the effective root path from KIMI_HOME when no explicit root is configured", "[kimi]") {
    TempFixtureDirectory fixture;
    fixture.makeFile("config.toml", "default_model = \"kimi-code/kimi-for-coding\"\n");

    ScopedEnvironmentVariable kimiHome("KIMI_HOME", fixture.path().string());

    const std::string resolved = KimiAdapter::resolveEffectiveRootPathForTesting();
    CHECK(resolved == fixture.path().string());
}
