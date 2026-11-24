// test_provider_data_path.cpp
// Tests for provider-fed data path in AppController
// Verifies that DashboardViewModel and TaskbarMetrics are built correctly from ProviderManager data

#define JISTATS_ENABLE_TEST_HOOKS 1

#include <catch_amalgamated.hpp>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <vector>
#include <ctime>

#include "providers/ProviderManager.hpp"
#include "providers/SubscriptionQuotaAdapter.hpp"
#include "app/AppController.hpp"
#include "core/HttpClient.hpp"
#include "db/SettingsService.hpp"
#include "db/StorageService.hpp"
#include "gui/DashboardPresenter.hpp"
#include "app_controller_test_support.hpp"
#include "runtime_test_support.hpp"
#include "sqlite_test_support.hpp"
#include "temp_test_directory.hpp"

using namespace JiStats;
using namespace JiStats::TestSupport;

namespace {

QuotaResponse subscriptionFixture(SubscriptionProvider provider, const std::string& body, std::time_t now) {
    return SubscriptionQuotaAdapter::parseQuotaResponse(provider, HttpResponse{true, 200, body, {}}, now);
}

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
    const bool hasSourceDate = GENERATE(false, true);
    const std::time_t sourceDate = hasSourceDate ? now - 86400 : 0;
    const std::time_t legacyPollTime = now - 300;
    std::promise<void> releaseFirstQuota;
    if (hasSourceDate) {
        seedPersistedQuotaSnapshot(dbPath.path(), "claude_code", "claude_code.runtime_status",
                                   "Claude Code Runtime Status", "claude_code_local_stats:local_aggregate",
                                   "Local Activity", "local_aggregate", legacyPollTime, 41, 0, 0, "Unknown");
    }
    auto claudeProvider = std::make_shared<ControlledQuotaProviderAdapter>(
        "claude_code", "Claude Code",
        makeQuotaResponse(sourceDate, 31415, 0, "local_aggregate", "claude_code_local_stats"),
        hasSourceDate ? releaseFirstQuota.get_future().share() : std::shared_future<void>{});
    auto providerManager = std::make_shared<ProviderManager>();
    providerManager->addProvider(claudeProvider);

    AppController controller;
    controller.setHeadlessForTesting(true);
    controller.setDatabasePathForTesting(dbPath.path());
    controller.setProviderManagerForTesting(providerManager);
    const bool started = controller.init();
    const auto startup = controller.getDashboardViewModelForTesting();
    if (hasSourceDate) {
        releaseFirstQuota.set_value();
    }
    REQUIRE(started);
    if (hasSourceDate) {
        REQUIRE(startup.providers.size() == 1);
        CHECK(startup.providers.front().state == ProviderState::Unknown);
        CHECK(startup.providers.front().lastSyncTime.empty());
        REQUIRE(startup.providers.front().quotas.size() == 1);
        CHECK(startup.providers.front().quotas.front().used == 41);
        controller.refreshRuntimeForTesting();
    }

    const DashboardViewModel viewModel = controller.getDashboardViewModelForTesting();
    REQUIRE(viewModel.providers.size() == 1);
    const auto& status = viewModel.providers.front();
    REQUIRE(status.quotas.size() == 1);
    CHECK(status.providerKey == "claude_code");
    CHECK(status.state == (hasSourceDate ? ProviderState::Stale : ProviderState::Unknown));
    CHECK(parseRuntimeTimestamp(status.lastSyncTime) == sourceDate);
    CHECK(status.quotas.front().planName == "Local Activity");
    CHECK(status.quotas.front().providerSurface == "claude_code_local_stats");
    CHECK(status.quotas.front().confidenceTier == "Unknown");
    CHECK(status.quotas.front().used == 31415);

    const auto connectivity = controller.getConnectivityStateForTesting("claude_code");
    REQUIRE(connectivity.has_value());
    CHECK(connectivity->lastCheckedAt >= now);

    controller.refreshRuntimeForTesting();
    if (hasSourceDate) {
        claudeProvider->setQuotaResponse(
            makeQuotaResponse(sourceDate, 42, 0, "local_aggregate", "claude_code_local_stats"));
        controller.refreshRuntimeForTesting();
        controller.refreshRuntimeForTesting();
    }
    controller.shutdown();

    StorageService storage;
    REQUIRE(storage.init(dbPath.path()));
    const auto snapshots = storage.execute("SELECT snapshot_time_utc, used_value FROM quota_snapshots ORDER BY id");
    REQUIRE(snapshots.success());
    REQUIRE(snapshots.rowCount() == (hasSourceDate ? 3 : 0));
    if (!hasSourceDate) {
        return;
    }
    CHECK(parseRuntimeTimestamp(snapshots.getString(0, 0)) == legacyPollTime);
    CHECK(parseRuntimeTimestamp(snapshots.getString(1, 0)) == sourceDate);
    CHECK(parseRuntimeTimestamp(snapshots.getString(2, 0)) == sourceDate);
    CHECK(snapshots.getInt64(0, 1) == 41);
    CHECK(snapshots.getInt64(1, 1) == 31415);
    CHECK(snapshots.getInt64(2, 1) == 42);
    storage.close();

    std::promise<void> releaseQuota;
    auto deferredProvider = std::make_shared<ControlledQuotaProviderAdapter>(
        "claude_code", "Claude Code",
        makeQuotaResponse(sourceDate, 100, 0, "local_aggregate", "claude_code_local_stats"),
        releaseQuota.get_future().share());
    auto restartedProviders = std::make_shared<ProviderManager>();
    restartedProviders->addProvider(deferredProvider);
    AppController restarted;
    restarted.setHeadlessForTesting(true);
    restarted.setDatabasePathForTesting(dbPath.path());
    restarted.setProviderManagerForTesting(restartedProviders);
    const bool initialized = restarted.init();
    const auto hydrated = restarted.getDashboardViewModelForTesting();
    releaseQuota.set_value();
    restarted.shutdown();

    REQUIRE(initialized);
    REQUIRE(hydrated.providers.size() == 1);
    CHECK(hydrated.providers.front().state == ProviderState::Stale);
    CHECK(parseRuntimeTimestamp(hydrated.providers.front().lastSyncTime) == sourceDate);
    REQUIRE(hydrated.providers.front().quotas.size() == 1);
    CHECK(hydrated.providers.front().quotas.front().used == 42);
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
        const auto quota = subscriptionFixture(SubscriptionProvider::Grok, R"json({
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
        const auto quota = subscriptionFixture(SubscriptionProvider::Grok, R"json({
          "config": {"currentPeriod": {"type":"WEEKLY","start":"2026-04-20T12:00:00Z","end":"2026-04-27T12:00:00Z"}}
        })json",
                                               now);
        REQUIRE(quota.success);
        CHECK(quota.remaining == 100);
        const auto invalid = subscriptionFixture(SubscriptionProvider::Grok, R"json({
          "config": {"currentPeriod": {"type":"WEEKLY","start":"2026-02-30T12:00:00Z","end":"2026-04-27T12:00:00Z"}}
        })json",
                                                 now);
        CHECK_FALSE(invalid.success);
        CHECK(invalid.unavailable);
    }
    SECTION("Grok rejects a supplied negative value even with a complete period") {
        const auto quota = subscriptionFixture(SubscriptionProvider::Grok, R"json({
          "config": {
            "creditUsagePercent": -0.25,
            "currentPeriod": {"type":"WEEKLY","start":"2026-04-20T12:00:00Z","end":"2026-04-27T12:00:00Z"}
          }
        })json",
                                               now);
        CHECK_FALSE(quota.success);
        CHECK(quota.unavailable);
    }
    SECTION("Grok accepts measured zero without period metadata") {
        const auto quota =
            subscriptionFixture(SubscriptionProvider::Grok, R"({"config":{"creditUsagePercent":0}})", now);
        REQUIRE(quota.success);
        CHECK(quota.used == 0);
        CHECK(quota.remaining == 100);
    }
    SECTION("A rejected Grok sign-in returns an actionable quota state") {
        const auto expired = SubscriptionQuotaAdapter::parseQuotaResponse(
            SubscriptionProvider::Grok, HttpResponse{false, 401, "private response", {}}, now);
        CHECK_FALSE(expired.success);
        CHECK(expired.unavailable);
        CHECK(expired.error == "Run grok login again");
        CHECK(expired.rawResponse.empty());

        const auto outage = SubscriptionQuotaAdapter::parseQuotaResponse(SubscriptionProvider::Grok,
                                                                         HttpResponse{false, 503, {}, {}}, now);
        CHECK_FALSE(outage.success);
        CHECK_FALSE(outage.unavailable);
        CHECK(outage.error.find("503") != std::string::npos);
    }
    SECTION("Cursor uses the provider's percentages rather than on-demand spend") {
        const auto quota = subscriptionFixture(SubscriptionProvider::Cursor, R"json({
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
        const auto unsupported =
            subscriptionFixture(SubscriptionProvider::Cursor, R"({"onDemandUsage":{"totalSpend":42}})", now);
        CHECK_FALSE(unsupported.success);
        CHECK(unsupported.unavailable);
    }
    SECTION("Cursor skips negative percentages and retains valid split usage") {
        const auto unsupported =
            subscriptionFixture(SubscriptionProvider::Cursor, R"({"planUsage":{"totalPercentUsed":-1}})", now);
        CHECK_FALSE(unsupported.success);
        CHECK(unsupported.unavailable);

        const auto quota = subscriptionFixture(
            SubscriptionProvider::Cursor,
            R"({"planUsage":{"totalPercentUsed":-1,"autoPercentUsed":20,"apiPercentUsed":-1}})", now);
        REQUIRE(quota.success);
        CHECK(quota.windowType == "auto");
        CHECK(quota.remaining == 80);
        CHECK(quota.secondaryLimit == 0);
    }
}

TEST_CASE("Grok and Cursor quota follow independent tracking preferences", "[provider_data_path][subscription]") {
    requireSqlite();
    const auto now = std::time(nullptr);
    const auto grok = subscriptionFixture(SubscriptionProvider::Grok, R"({"config":{"creditUsagePercent":42}})", now);
    const auto cursor = subscriptionFixture(SubscriptionProvider::Cursor,
                                            R"({"planUsage":{"autoPercentUsed":10,"apiPercentUsed":25}})", now);
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
    const auto syntheticBounds =
        storage.execute("SELECT COUNT(*) FROM quota_snapshots WHERE quota_dimension = 'percent' "
                        "AND (window_start_utc <> '' OR window_end_utc <> '');");
    REQUIRE(syntheticBounds.success());
    CHECK(syntheticBounds.getInt(0, 0) == 0);
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
