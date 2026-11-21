#define JISTATS_ENABLE_TEST_HOOKS 1

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "app/TrayController.hpp"

using namespace JiStats;

namespace {

class ScopedTrayTestHandler {
public:
    explicit ScopedTrayTestHandler(TrayController::TestOperationHandler handler) {
        TrayController::setTestOperationHandler(std::move(handler));
    }

    ~ScopedTrayTestHandler() {
        TrayController::clearTestOperationHandler();
    }
};

std::size_t opIndex(TrayController::TestOperation op) {
    return static_cast<std::size_t>(op);
}

constexpr std::size_t kTrayTestOperationCount =
    static_cast<std::size_t>(TrayController::TestOperation::DispatchTrayEvent) + 1;

} // namespace

TEST_CASE("TrayController uninitialized guards avoid tray operations", "[tray]") {
    std::array<int, kTrayTestOperationCount> operationCounts{};
    ScopedTrayTestHandler handler([&](TrayController::TestOperation operation,
                                      TrayStatus,
                                      const std::string&,
                                      std::string&) {
        ++operationCounts[opIndex(operation)];
        return true;
    });

    TrayController tray;

    tray.showIcon();
    tray.hideIcon();
    tray.showMenu();

    CHECK_FALSE(tray.isAvailable());
    const auto state = tray.getTestState();
    CHECK_FALSE(state.initialized);
    CHECK_FALSE(state.iconVisible);
    CHECK(operationCounts[opIndex(TrayController::TestOperation::AddTrayIcon)] == 0);
    CHECK(operationCounts[opIndex(TrayController::TestOperation::RemoveTrayIcon)] == 0);
    CHECK(operationCounts[opIndex(TrayController::TestOperation::ShowContextMenu)] == 0);
}

TEST_CASE("TrayController callback paths are invokable through test seam events", "[tray]") {
    std::array<int, kTrayTestOperationCount> operationCounts{};
    ScopedTrayTestHandler handler([&](TrayController::TestOperation operation,
                                      TrayStatus,
                                      const std::string&,
                                      std::string&) {
        ++operationCounts[opIndex(operation)];
        return true;
    });

    TrayController tray;
    REQUIRE(tray.init());

    int showDashboardCount = 0;
    int preferencesCount = 0;
    int retryNowCount = 0;
    int dismissOfflineNoticeCount = 0;
    int exitCount = 0;

    tray.setOnShowDashboardCallback([&]() { ++showDashboardCount; });
    tray.setOnPreferencesCallback([&]() { ++preferencesCount; });
    tray.setOnRetryNowCallback([&]() { ++retryNowCount; });
    tray.setOnDismissOfflineNoticeCallback([&]() { ++dismissOfflineNoticeCount; });
    tray.setOnExitCallback([&]() { ++exitCount; });

    CHECK(tray.dispatchTestEvent(TrayController::TestEvent::TrayDoubleClick));
    CHECK(tray.dispatchTestEvent(TrayController::TestEvent::MenuShowDashboard));
    CHECK(tray.dispatchTestEvent(TrayController::TestEvent::MenuPreferences));
    CHECK(tray.dispatchTestEvent(TrayController::TestEvent::MenuRetryNow));
    CHECK(tray.dispatchTestEvent(TrayController::TestEvent::MenuDismissOfflineNotice));
    CHECK(tray.dispatchTestEvent(TrayController::TestEvent::MenuExit));
    CHECK(tray.dispatchTestEvent(TrayController::TestEvent::TrayContextMenu));

    CHECK(showDashboardCount == 2);
    CHECK(preferencesCount == 1);
    CHECK(retryNowCount == 1);
    CHECK(dismissOfflineNoticeCount == 1);
    CHECK(exitCount == 1);
    CHECK(operationCounts[opIndex(TrayController::TestOperation::CreateMessageWindow)] == 1);
    CHECK(operationCounts[opIndex(TrayController::TestOperation::AddTrayIcon)] == 1);
    CHECK(operationCounts[opIndex(TrayController::TestOperation::ShowContextMenu)] == 1);

    tray.shutdown();
    CHECK(operationCounts[opIndex(TrayController::TestOperation::RemoveTrayIcon)] == 1);
}

TEST_CASE("TrayController showMenu routes through the tray dispatch path", "[tray]") {
    std::array<int, kTrayTestOperationCount> operationCounts{};
    ScopedTrayTestHandler handler([&](TrayController::TestOperation operation,
                                      TrayStatus,
                                      const std::string&,
                                      std::string&) {
        ++operationCounts[opIndex(operation)];
        return true;
    });

    TrayController tray;
    REQUIRE(tray.init());

    tray.showMenu();

    CHECK(operationCounts[opIndex(TrayController::TestOperation::DispatchTrayEvent)] == 1);
    CHECK(operationCounts[opIndex(TrayController::TestOperation::ShowContextMenu)] == 1);
}

TEST_CASE("TrayController showMenu waits for the tray-thread menu handler before returning", "[tray]") {
    bool showMenuReturned = false;
    bool menuHandlerObservedReturn = false;

    ScopedTrayTestHandler handler([&](TrayController::TestOperation operation,
                                      TrayStatus,
                                      const std::string&,
                                      std::string&) {
        if (operation == TrayController::TestOperation::ShowContextMenu) {
            menuHandlerObservedReturn = showMenuReturned;
        }
        return true;
    });

    TrayController tray;
    REQUIRE(tray.init());

    tray.showMenu();
    showMenuReturned = true;

    CHECK_FALSE(menuHandlerObservedReturn);
}

TEST_CASE("TrayController tracks status and tooltip updates deterministically", "[tray]") {
    std::array<int, kTrayTestOperationCount> operationCounts{};
    std::vector<std::tuple<TrayStatus, std::string>> updateSnapshots;

    ScopedTrayTestHandler handler([&](TrayController::TestOperation operation,
                                      TrayStatus status,
                                      const std::string& tooltip,
                                      std::string&) {
        ++operationCounts[opIndex(operation)];
        if (operation == TrayController::TestOperation::UpdateIcon) {
            updateSnapshots.emplace_back(status, tooltip);
        }
        return true;
    });

    TrayController tray;
    REQUIRE(tray.init());

    tray.setStatus(TrayStatus::Syncing);
    tray.setTooltip("Syncing provider quotas");
    tray.setStatus(TrayStatus::Warning);

    const auto activeState = tray.getTestState();
    CHECK(activeState.status == TrayStatus::Warning);
    CHECK(activeState.tooltip == "Syncing provider quotas");
    CHECK(operationCounts[opIndex(TrayController::TestOperation::UpdateIcon)] == 3);

    REQUIRE(updateSnapshots.size() == 3);
    CHECK(std::get<0>(updateSnapshots[0]) == TrayStatus::Syncing);
    CHECK(std::get<1>(updateSnapshots[0]) == "JiStats");
    CHECK(std::get<0>(updateSnapshots[1]) == TrayStatus::Syncing);
    CHECK(std::get<1>(updateSnapshots[1]) == "Syncing provider quotas");
    CHECK(std::get<0>(updateSnapshots[2]) == TrayStatus::Warning);
    CHECK(std::get<1>(updateSnapshots[2]) == "Syncing provider quotas");

    tray.hideIcon();
    tray.setTooltip("Hidden tray icon");
    tray.setStatus(TrayStatus::Error);

    const auto hiddenState = tray.getTestState();
    CHECK(hiddenState.status == TrayStatus::Error);
    CHECK(hiddenState.tooltip == "Hidden tray icon");
    CHECK(operationCounts[opIndex(TrayController::TestOperation::UpdateIcon)] == 3);

    tray.showIcon();
    tray.setState(TrayStatus::Warning, "Quota pressure rising");
    tray.setState(TrayStatus::Warning, "Quota pressure rising");

    const auto mergedState = tray.getTestState();
    CHECK(mergedState.status == TrayStatus::Warning);
    CHECK(mergedState.tooltip == "Quota pressure rising");
    CHECK(operationCounts[opIndex(TrayController::TestOperation::UpdateIcon)] == 4);
}

TEST_CASE("TrayController availability and error reporting follow init result", "[tray]") {
    std::array<int, kTrayTestOperationCount> operationCounts{};

    SECTION("successful init reports availability") {
        ScopedTrayTestHandler handler([&](TrayController::TestOperation op,
                                         TrayStatus,
                                         const std::string&,
                                         std::string&) {
            ++operationCounts[opIndex(op)];
            return true;
        });

        TrayController tray;
        REQUIRE(tray.init());
        CHECK(tray.isAvailable());
        CHECK(tray.getLastError().empty());

        tray.hideIcon();
        CHECK_FALSE(tray.isAvailable());

        tray.showIcon();
        CHECK(tray.isAvailable());
    }

    SECTION("init failure propagates deterministic seam error") {
        ScopedTrayTestHandler handler([&](TrayController::TestOperation op,
                                         TrayStatus,
                                         const std::string&,
                                         std::string& error) {
            ++operationCounts[opIndex(op)];
            if (op == TrayController::TestOperation::CreateMessageWindow) {
                error = "test seam create message window failure";
                return false;
            }
            return true;
        });

        TrayController tray;
        CHECK_FALSE(tray.init());
        CHECK_FALSE(tray.isAvailable());
        CHECK(tray.getLastError() == "test seam create message window failure");

        const auto state = tray.getTestState();
        CHECK_FALSE(state.initialized);
        CHECK_FALSE(state.iconVisible);
        CHECK(state.lastError == "test seam create message window failure");
    }
}

TEST_CASE("TrayController callback dispatch goes through queue staging", "[tray]") {
    std::array<int, kTrayTestOperationCount> operationCounts{};

    ScopedTrayTestHandler handler([&](TrayController::TestOperation operation,
                                      TrayStatus,
                                      const std::string&,
                                      std::string&) {
        ++operationCounts[opIndex(operation)];
        return true;
    });

    TrayController tray;
    REQUIRE(tray.init());

    int showDashboardCount = 0;
    tray.setOnShowDashboardCallback([&]() { ++showDashboardCount; });

    CHECK(tray.dispatchTestEvent(TrayController::TestEvent::TrayDoubleClick));
    CHECK(tray.dispatchTestEvent(TrayController::TestEvent::MenuShowDashboard));
    CHECK(tray.dispatchTestEvent(TrayController::TestEvent::TrayContextMenu));

    CHECK(showDashboardCount == 2);
    CHECK(operationCounts[opIndex(TrayController::TestOperation::DispatchTrayEvent)] == 3);
}

TEST_CASE("TrayController shutdown removes the icon before stopping the message pump", "[tray]") {
    std::vector<TrayController::TestOperation> operations;

    SECTION("fake tray window path keeps deterministic remove-before-stop ordering") {
        ScopedTrayTestHandler handler([&](TrayController::TestOperation operation,
                                          TrayStatus,
                                          const std::string&,
                                          std::string&) -> std::optional<bool> {
            operations.push_back(operation);
            return true;
        });

        TrayController tray;
        REQUIRE(tray.init());
        tray.shutdown();

        REQUIRE(operations.size() >= 4);
        CHECK(operations[0] == TrayController::TestOperation::CreateMessageWindow);
        CHECK(operations[1] == TrayController::TestOperation::AddTrayIcon);
        CHECK(operations[2] == TrayController::TestOperation::RemoveTrayIcon);
        CHECK(operations[3] == TrayController::TestOperation::StopMessagePump);
    }

    SECTION("real message pump path still shuts down cleanly on the owning thread") {
        ScopedTrayTestHandler handler([&](TrayController::TestOperation operation,
                                          TrayStatus,
                                          const std::string&,
                                          std::string&) -> std::optional<bool> {
            operations.push_back(operation);
            switch (operation) {
            case TrayController::TestOperation::CreateMessageWindow:
            case TrayController::TestOperation::StartMessagePump:
            case TrayController::TestOperation::StopMessagePump:
                return std::nullopt;
            case TrayController::TestOperation::AddTrayIcon:
            case TrayController::TestOperation::RemoveTrayIcon:
                return true;
            default:
                return std::nullopt;
            }
        });

        TrayController tray;
        REQUIRE(tray.init());
        tray.shutdown();

        const auto state = tray.getTestState();
        CHECK_FALSE(state.initialized);
        CHECK_FALSE(state.iconVisible);

        REQUIRE(operations.size() >= 4);
        CHECK(operations[0] == TrayController::TestOperation::CreateMessageWindow);
        CHECK(operations[1] == TrayController::TestOperation::StartMessagePump);
        CHECK(operations[2] == TrayController::TestOperation::AddTrayIcon);
        CHECK(operations[3] == TrayController::TestOperation::RemoveTrayIcon);
        CHECK(std::find(operations.begin(), operations.end(),
                        TrayController::TestOperation::StopMessagePump) != operations.end());
    }
}
