// test_provider_data_path.cpp
// Tests for provider-fed data path in AppController
// Verifies that DashboardViewModel and TaskbarMetrics are built correctly from ProviderManager data

#define JISTATS_ENABLE_TEST_HOOKS 1

#include <catch_amalgamated.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <ctime>

#include "providers/ProviderManager.hpp"
#include "app/AppController.hpp"
#include "db/SettingsService.hpp"
#include "db/StorageService.hpp"
#include "gui/DashboardPresenter.hpp"
#include "runtime_test_support.hpp"
#include "sqlite_test_support.hpp"
#include "temp_test_directory.hpp"

using namespace JiStats;
using namespace JiStats::TestSupport;

namespace {

void seedProviderTrackingPreferences(
    const std::string& path,
    bool openaiEnabled,
    bool minimaxEnabled,
    bool claudeEnabled,
    bool kimiEnabled) {
    StorageService storage;
    REQUIRE(storage.init(path));

    SettingsService settings(storage);
    REQUIRE(settings.setBool("runtime.openai_subscription_tracking_enabled", openaiEnabled));
    REQUIRE(settings.setBool("runtime.minimax_subscription_tracking_enabled", minimaxEnabled));
    REQUIRE(settings.setBool("runtime.claude_code_enabled", claudeEnabled));
    REQUIRE(settings.setBool("runtime.kimi_code_enabled", kimiEnabled));

    storage.close();
}

void enableProviderTracking(
    AppController& controller,
    bool openaiEnabled,
    bool minimaxEnabled,
    bool claudeEnabled,
    bool kimiEnabled) {
    DashboardPreferences preferences = controller.getDashboardViewModelForTesting().preferences;
    preferences.openaiSubscriptionTrackingEnabled = openaiEnabled;
    preferences.miniMaxSubscriptionTrackingEnabled = minimaxEnabled;
    preferences.claudeCodeEnabled = claudeEnabled;
    preferences.kimiCodeEnabled = kimiEnabled;
    controller.applyDashboardPreferencesForTesting(preferences);
}

} // namespace

TEST_CASE("AppController provider path builds live and offline provider states correctly", "[provider_data_path]") {
    const std::time_t now = std::time(nullptr);
    auto liveProvider = makeCurrentQuotaProvider("openai", "OpenAI", 1305, 1500, now, "monthly");
    auto offlineProvider = makeOfflineQuotaProvider("minimax", "MiniMax", "Missing credentials");
    auto providerManager = makeProviderManager({liveProvider, offlineProvider});

    TempTestDatabase dbPath;
    AppController controller;
    controller.setDatabasePathForTesting(dbPath.path());
    controller.setHeadlessForTesting(true);
    controller.setProviderManagerForTesting(providerManager);
    REQUIRE(controller.init());
    enableProviderTracking(controller, true, true, false, false);

    const DashboardViewModel viewModel = controller.getDashboardViewModelForTesting();
    REQUIRE(viewModel.providers.size() == 2);
    CHECK(viewModel.runtimeState == RuntimeState::Offline);
    CHECK_FALSE(viewModel.isOnline);

    bool foundLiveProvider = false;
    bool foundOfflineProvider = false;
    for (const auto& provider : viewModel.providers) {
        if (provider.providerKey == "openai") {
            foundLiveProvider = true;
            CHECK(provider.state == ProviderState::Live);
            CHECK_FALSE(provider.lastSyncTime.empty());
            REQUIRE(provider.quotas.size() == 1);
            CHECK(provider.quotas[0].used == 1305);
            CHECK(provider.quotas[0].limit == 1500);
        } else if (provider.providerKey == "minimax") {
            foundOfflineProvider = true;
            CHECK(provider.state == ProviderState::Offline);
            CHECK(provider.lastSyncTime.empty());
            CHECK(provider.quotas.empty());
        }
    }

    CHECK(foundLiveProvider);
    CHECK(foundOfflineProvider);

    controller.shutdown();
}

TEST_CASE("AppController provider path does not fetch usage during startup when only quota data is needed", "[provider_data_path]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedProviderTrackingPreferences(dbPath.path(), true, true, false, false);
    const std::time_t now = std::time(nullptr);
    auto openaiProvider = makeCurrentQuotaProvider("openai", "OpenAI", 1305, 1500, now, "monthly");
    auto minimaxProvider = makeCurrentQuotaProvider("minimax", "MiniMax", 42000, 100000, now, "daily");
    auto providerManager = makeProviderManager({openaiProvider, minimaxProvider});

    AppController controller;
    controller.setHeadlessForTesting(true);
    controller.setDatabasePathForTesting(dbPath.path());
    controller.setProviderManagerForTesting(providerManager);
    REQUIRE(controller.init());

    CHECK(openaiProvider->getUsageFetchCount() == 0);
    CHECK(minimaxProvider->getUsageFetchCount() == 0);
    CHECK(openaiProvider->getQuotaFetchCount() >= 1);
    CHECK(minimaxProvider->getQuotaFetchCount() >= 1);

    controller.shutdown();
}

TEST_CASE("AppController provider path suppresses Kimi local-only quota state", "[provider_data_path][kimi]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedProviderTrackingPreferences(dbPath.path(), false, false, false, true);
    const std::time_t now = std::time(nullptr);
    auto kimiProvider = makeCurrentQuotaProvider(
        "kimi",
        "Kimi Code",
        42,
        0,
        now,
        "interval",
        "kimi_code_local");
    auto providerManager = makeProviderManager({kimiProvider});

    AppController controller;
    controller.setHeadlessForTesting(true);
    controller.setDatabasePathForTesting(dbPath.path());
    controller.setProviderManagerForTesting(providerManager);
    REQUIRE(controller.init());

    const DashboardViewModel viewModel = controller.getDashboardViewModelForTesting();
    REQUIRE(viewModel.providers.size() == 1);
    CHECK(viewModel.runtimeState == RuntimeState::Current);

    const ProviderStatus& provider = viewModel.providers.front();
    CHECK(provider.providerKey == "kimi");
    CHECK(provider.displayName == "Kimi Code");
    CHECK(provider.state == ProviderState::Sample);
    CHECK(provider.lastSyncTime.empty());
    CHECK(provider.quotas.empty());

    controller.shutdown();
}

TEST_CASE("AppController provider path gives Claude local quotas a readable plan label and unknown confidence", "[provider_data_path][claude]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedProviderTrackingPreferences(dbPath.path(), false, false, true, false);
    const std::time_t now = std::time(nullptr);
    auto claudeProvider = makeCurrentQuotaProvider(
        "claude_code",
        "Claude Code",
        31415,
        0,
        now,
        "interval",
        "claude_code_local_stats");
    auto providerManager = makeProviderManager({claudeProvider});

    AppController controller;
    controller.setHeadlessForTesting(true);
    controller.setDatabasePathForTesting(dbPath.path());
    controller.setProviderManagerForTesting(providerManager);
    REQUIRE(controller.init());

    const DashboardViewModel viewModel = controller.getDashboardViewModelForTesting();
    REQUIRE(viewModel.providers.size() == 1);
    REQUIRE(viewModel.providers.front().quotas.size() == 1);
    CHECK(viewModel.providers.front().providerKey == "claude_code");
    CHECK(viewModel.providers.front().quotas.front().planName == "Local Activity");
    CHECK(viewModel.providers.front().quotas.front().providerSurface == "claude_code_local_stats");
    CHECK(viewModel.providers.front().quotas.front().confidenceTier == "Unknown");

    controller.shutdown();
}

TEST_CASE("ProviderManager handles empty providers gracefully", "[provider_data_path]") {
    auto providerManager = std::make_shared<ProviderManager>();
    
    // Empty provider manager
    REQUIRE(providerManager->getProviderCount() == 0);
    
    auto quotaResponses = providerManager->fetchAllQuota();
    CHECK(quotaResponses.empty());
    
    auto usageResponses = providerManager->fetchAllUsage(0, std::time(nullptr));
    CHECK(usageResponses.empty());
}

TEST_CASE("ProviderManager keeps other quota responses when a provider fails", "[provider_data_path]") {
    const auto failed = makeOfflineQuotaProvider("minimax", "MiniMax", "API error");
    const auto healthy = makeCurrentQuotaProvider("openai", "OpenAI", 25, 100, std::time(nullptr));
    const auto providerManager = makeProviderManager({failed, healthy});

    const auto quotaResponses = providerManager->fetchAllQuota();
    REQUIRE(quotaResponses.size() == 2);
    CHECK_FALSE(quotaResponses[0].success);
    CHECK(quotaResponses[0].error == "API error");
    CHECK(quotaResponses[1].success);
    CHECK(quotaResponses[1].used == 25);
    CHECK(failed->getQuotaFetchCount() == 1);
    CHECK(healthy->getQuotaFetchCount() == 1);
}

TEST_CASE("ProviderManager respects enabled/disabled state", "[provider_data_path]") {
    auto providerManager = std::make_shared<ProviderManager>();
    
    const std::time_t now = std::time(nullptr);
    auto enabledProvider = makeCurrentQuotaProvider("enabled", "Enabled Provider", 100, 200, now);
    auto disabledProvider = makeCurrentQuotaProvider("disabled", "Disabled Provider", 300, 400, now);
    
    disabledProvider->setEnabled(false);
    
    providerManager->addProvider(enabledProvider);
    providerManager->addProvider(disabledProvider);
    
    // Only enabled providers should be returned
    auto enabledProviders = providerManager->getEnabledProviders();
    REQUIRE(enabledProviders.size() == 1);
    CHECK(enabledProviders[0]->getProviderKey() == "enabled");
    
    // fetchAllQuota only queries enabled providers
    auto quotaResponses = providerManager->fetchAllQuota();
    REQUIRE(quotaResponses.size() == 1);
    CHECK(quotaResponses[0].used == 100);
}
