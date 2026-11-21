#define JISTATS_ENABLE_TEST_HOOKS 1
#include <catch_amalgamated.hpp>

#include <memory>
#include <string>
#include <vector>

#include "platform/StartupManager.hpp"

using namespace JiStats;

namespace {

struct FakeStartupEnvironment {
    std::wstring executablePath = L"C:\\Program Files\\JiStats\\JiStats.exe";
    std::wstring registryValue;
    std::vector<std::wstring> writeAttempts;
    bool failWrite = false;
    std::string writeError = "Injected registry write failure";
};

StartupManager::TestHooks makeHooks(const std::shared_ptr<FakeStartupEnvironment>& env) {
    StartupManager::TestHooks hooks;

    hooks.executablePathProvider = [env]() {
        return env->executablePath;
    };

    hooks.writeRegistryValue = [env](const std::wstring& value, std::string& error) {
        env->writeAttempts.push_back(value);
        if (env->failWrite) {
            error = env->writeError;
            return false;
        }

        env->registryValue = value;
        return true;
    };

    hooks.readRegistryValue = [env]() {
        return env->registryValue;
    };

    return hooks;
}

} // namespace

TEST_CASE("StartupManager enable builds expected startup command", "[startup]") {
    auto env = std::make_shared<FakeStartupEnvironment>();
    StartupManager manager;
    manager.setTestHooks(makeHooks(env));

    REQUIRE(manager.enable());
    REQUIRE(env->writeAttempts.size() == 1);
    CHECK(env->writeAttempts[0] == L"\"C:\\Program Files\\JiStats\\JiStats.exe\"");
    CHECK(manager.isEnabled());

    StartupManager minimizedManager;
    minimizedManager.setTestHooks(makeHooks(env));
    minimizedManager.disable();
    minimizedManager.setStartMinimized(true);

    REQUIRE(minimizedManager.enable());
    REQUIRE(env->writeAttempts.size() >= 3);
    CHECK(env->writeAttempts.back() == L"\"C:\\Program Files\\JiStats\\JiStats.exe\" --background");
}

TEST_CASE("StartupManager disable clears startup state", "[startup]") {
    auto env = std::make_shared<FakeStartupEnvironment>();
    StartupManager manager;
    manager.setTestHooks(makeHooks(env));

    REQUIRE(manager.enable());
    REQUIRE_FALSE(env->registryValue.empty());
    REQUIRE(manager.isEnabled());

    REQUIRE(manager.disable());
    CHECK(env->registryValue.empty());
    CHECK_FALSE(manager.isEnabled());
}

TEST_CASE("StartupManager setStartMinimized rewrites command when startup is enabled", "[startup]") {
    auto env = std::make_shared<FakeStartupEnvironment>();
    StartupManager manager;
    manager.setTestHooks(makeHooks(env));

    REQUIRE(manager.setStartMinimized(true));
    CHECK(env->writeAttempts.empty());

    REQUIRE(manager.enable());
    REQUIRE(env->writeAttempts.size() == 1);
    CHECK(env->writeAttempts[0] == L"\"C:\\Program Files\\JiStats\\JiStats.exe\" --background");

    REQUIRE(manager.setStartMinimized(false));
    REQUIRE(env->writeAttempts.size() == 2);
    CHECK(env->writeAttempts[1] == L"\"C:\\Program Files\\JiStats\\JiStats.exe\"");
}

TEST_CASE("StartupManager reads start-minimized mode from startup command", "[startup]") {
    auto env = std::make_shared<FakeStartupEnvironment>();
    StartupManager manager;
    manager.setTestHooks(makeHooks(env));

    env->registryValue = L"\"C:\\Program Files\\JiStats\\JiStats.exe\" --background";
    REQUIRE(manager.isEnabled());
    CHECK(manager.isStartMinimized());

    env->registryValue = L"\"C:\\Program Files\\JiStats\\JiStats.exe\"";
    REQUIRE(manager.isEnabled());
    CHECK_FALSE(manager.isStartMinimized());
}

TEST_CASE("StartupManager propagates errors from startup operations", "[startup]") {
    SECTION("enable fails when executable path cannot be resolved") {
        auto env = std::make_shared<FakeStartupEnvironment>();
        env->executablePath.clear();

        StartupManager manager;
        manager.setTestHooks(makeHooks(env));

        REQUIRE_FALSE(manager.enable());
        CHECK(manager.getLastError() == "Failed to get executable path");
    }

    SECTION("enable fails when registry write fails") {
        auto env = std::make_shared<FakeStartupEnvironment>();
        env->failWrite = true;
        env->writeError = "Registry denied";

        StartupManager manager;
        manager.setTestHooks(makeHooks(env));

        REQUIRE_FALSE(manager.enable());
        CHECK(manager.getLastError() == "Registry denied");
    }

    SECTION("setStartMinimized surfaces rewrite failures after startup is enabled") {
        auto env = std::make_shared<FakeStartupEnvironment>();

        StartupManager manager;
        manager.setTestHooks(makeHooks(env));
        REQUIRE(manager.enable());

        env->failWrite = true;
        env->writeError = "Rewrite failed";

        REQUIRE_FALSE(manager.setStartMinimized(true));
        CHECK(manager.getLastError() == "Rewrite failed");
    }

    SECTION("successful no-op clears stale error text when startup command already matches") {
        auto env = std::make_shared<FakeStartupEnvironment>();

        StartupManager manager;
        manager.setTestHooks(makeHooks(env));

        env->executablePath.clear();
        REQUIRE_FALSE(manager.enable());
        CHECK(manager.getLastError() == "Failed to get executable path");

        env->executablePath = L"C:\\Program Files\\JiStats\\JiStats.exe";
        env->registryValue = L"\"C:\\Program Files\\JiStats\\JiStats.exe\" --background";
        env->writeAttempts.clear();

        REQUIRE(manager.setStartMinimized(true));
        CHECK(env->writeAttempts.empty());
        CHECK(manager.getLastError().empty());
    }
}
