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
#include "providers/SubscriptionQuotaAdapter.hpp"
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

TEST_CASE("Subscription quota reads provider percentages and skips unsupported balances",
          "[provider_data_path][subscription]") {
    const auto now = std::time(nullptr);
    SECTION("Grok reports used allowance and reset time") {
        const auto quota = SubscriptionQuotaAdapter::parseQuotaResponse(SubscriptionProvider::Grok, R"json({
          "config": {
            "creditUsagePercent": 37.6,
            "currentPeriod": {"type":"USAGE_PERIOD_TYPE_WEEKLY","start":"2026-04-20T12:00:00Z","end":"2026-04-27T14:00:00+02:00"}
          }
        })json",
                                                                        now);
        REQUIRE(quota.success);
        CHECK(quota.used == 38);
        CHECK(quota.remaining == 62);
        CHECK(quota.windowType == "weekly");
        CHECK(quota.windowEnd - quota.windowStart == 7 * 86400);
        CHECK(quota.providerSurface == "grok_subscription_api");
        CHECK(quota.rawResponse.empty());
    }
    SECTION("Grok only infers an omitted zero with a complete recognized period") {
        const auto quota = SubscriptionQuotaAdapter::parseQuotaResponse(SubscriptionProvider::Grok, R"json({
          "config": {"currentPeriod": {"type":"WEEKLY","start":"2026-04-20T12:00:00Z","end":"2026-04-27T12:00:00Z"}}
        })json",
                                                                        now);
        REQUIRE(quota.success);
        CHECK(quota.remaining == 100);
        const auto invalid = SubscriptionQuotaAdapter::parseQuotaResponse(SubscriptionProvider::Grok, R"json({
          "config": {"currentPeriod": {"type":"WEEKLY","start":"2026-02-30T12:00:00Z","end":"2026-04-27T12:00:00Z"}}
        })json",
                                                                          now);
        CHECK_FALSE(invalid.success);
        CHECK(invalid.unavailable);
    }
    SECTION("Cursor uses the provider's percentages rather than on-demand spend") {
        const auto quota = SubscriptionQuotaAdapter::parseQuotaResponse(SubscriptionProvider::Cursor, R"json({
            "billingCycleStart": "1776686400000", "billingCycleEnd": 1779278400000,
            "planUsage": {"totalPercentUsed": 22}, "onDemandUsage": {"totalSpend": 42}
        })json",
                                                                        now);
        REQUIRE(quota.success);
        CHECK(quota.used == 22);
        CHECK(quota.remaining == 78);
        CHECK(quota.windowType == "monthly");
        CHECK(quota.windowEnd == 1779278400);
        CHECK(quota.providerSurface == "cursor_subscription_api");
        const auto unsupported = SubscriptionQuotaAdapter::parseQuotaResponse(
            SubscriptionProvider::Cursor, R"({"onDemandUsage":{"totalSpend":42}})", now);
        CHECK_FALSE(unsupported.success);
        CHECK(unsupported.unavailable);
    }
}

TEST_CASE("Grok and Cursor quota follow independent tracking preferences", "[provider_data_path][subscription]") {
    requireSqlite();
    const auto now = std::time(nullptr);
    const auto grok = SubscriptionQuotaAdapter::parseQuotaResponse(SubscriptionProvider::Grok,
                                                                   R"({"config":{"creditUsagePercent":42}})", now);
    const auto cursor = SubscriptionQuotaAdapter::parseQuotaResponse(
        SubscriptionProvider::Cursor, R"({"planUsage":{"autoPercentUsed":10,"apiPercentUsed":25}})", now);
    REQUIRE(grok.success);
    REQUIRE(cursor.success);
    auto grokProvider = makeQuotaProvider("grok", "Grok", grok);
    auto cursorProvider = makeQuotaProvider("cursor", "Cursor", cursor);
    grokProvider->setDirectQuotaApiConfigured(true);
    cursorProvider->setDirectQuotaApiConfigured(true);
    auto providerManager = makeProviderManager({grokProvider, cursorProvider});
    TempTestDatabase dbPath;
    AppController controller;
    controller.setDatabasePathForTesting(dbPath.path());
    controller.setHeadlessForTesting(true);
    controller.setProviderManagerForTesting(providerManager);
    REQUIRE(controller.init());
    auto preferences = controller.getDashboardViewModelForTesting().preferences;
    CHECK_FALSE(preferences.grokSubscriptionTrackingEnabled);
    CHECK_FALSE(preferences.cursorSubscriptionTrackingEnabled);
    CHECK(controller.getDashboardViewModelForTesting().providers.empty());

    preferences.grokSubscriptionTrackingEnabled = true;
    controller.applyDashboardPreferencesForTesting(preferences);
    auto providers = controller.getDashboardViewModelForTesting().providers;
    REQUIRE(providers.size() == 1);
    CHECK(providers.front().providerKey == "grok");
    REQUIRE(providers.front().quotas.size() == 1);
    CHECK(providers.front().quotas.front().remaining == 58);
    controller.refreshRuntimeForTesting();

    preferences.grokSubscriptionTrackingEnabled = false;
    preferences.cursorSubscriptionTrackingEnabled = true;
    controller.applyDashboardPreferencesForTesting(preferences);
    providers = controller.getDashboardViewModelForTesting().providers;
    REQUIRE(providers.size() == 1);
    CHECK(providers.front().providerKey == "cursor");
    REQUIRE(providers.front().quotas.size() == 2);
    CHECK(providers.front().quotas[0].remaining == 90);
    CHECK(providers.front().quotas[1].remaining == 75);
    controller.refreshRuntimeForTesting();

    controller.shutdown();
    StorageService storage;
    REQUIRE(storage.init(dbPath.path()));
    SettingsService settings(storage);
    CHECK(settings.getBool("runtime.grok_subscription_tracking_enabled") == false);
    CHECK(settings.getBool("runtime.cursor_subscription_tracking_enabled") == true);
    const auto history = storage.execute(
        "SELECT p.provider_key, COUNT(*) FROM quota_snapshots qs JOIN providers p ON qs.provider_id = p.id "
        "WHERE p.provider_key IN ('cursor', 'grok') AND qs.quota_dimension = 'percent' "
        "GROUP BY p.provider_key ORDER BY p.provider_key;");
    REQUIRE(history.success());
    REQUIRE(history.rowCount() == 2);
    CHECK(history.getString(0, 0) == "cursor");
    CHECK(history.getInt(0, 1) == 2);
    CHECK(history.getString(1, 0) == "grok");
    CHECK(history.getInt(1, 1) == 1);
}
