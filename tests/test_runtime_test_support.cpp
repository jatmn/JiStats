#include <catch_amalgamated.hpp>

#include <ctime>

#include "runtime_test_support.hpp"

using namespace JiStats;
using namespace JiStats::TestSupport;

TEST_CASE("Runtime test support builds current, stale, and offline provider inputs", "[runtime_test_support]") {
    const std::time_t now = std::time(nullptr);

    auto currentProvider = makeCurrentQuotaProvider(
        "openai",
        "OpenAI",
        25,
        100,
        now,
        "interval",
        "openai_subscription_codex_local");
    auto staleProvider = makeStaleQuotaProvider("minimax", "MiniMax", 60, 100, now, "daily");
    auto offlineProvider = makeOfflineQuotaProvider("claude", "Claude", "timeout");

    const QuotaResponse currentQuota = currentProvider->fetchQuota();
    const QuotaResponse staleQuota = staleProvider->fetchQuota();
    const QuotaResponse offlineQuota = offlineProvider->fetchQuota();

    REQUIRE(currentQuota.success);
    CHECK(currentQuota.snapshotTime == now);
    CHECK(currentQuota.windowStart == now - kDefaultQuotaWindowSeconds);
    CHECK(currentQuota.providerSurface == "openai_subscription_codex_local");
    CHECK(currentQuota.remaining == 75);

    REQUIRE(staleQuota.success);
    CHECK(staleQuota.snapshotTime == now - kDefaultStaleQuotaAgeSeconds);
    CHECK(staleQuota.remaining == 40);

    CHECK_FALSE(offlineQuota.success);
    CHECK(offlineQuota.error == "timeout");
}

TEST_CASE("Runtime test support builds provider managers without changing provider fetch semantics", "[runtime_test_support]") {
    const std::time_t now = std::time(nullptr);

    auto openaiProvider = makeCurrentQuotaProvider("openai", "OpenAI", 10, 100, now, "daily");
    auto minimaxProvider = makeOfflineQuotaProvider("minimax", "MiniMax", "offline");
    auto providerManager = makeProviderManager({openaiProvider, minimaxProvider});

    REQUIRE(providerManager->getProviderCount() == 2);

    auto quotaResponses = providerManager->fetchAllQuota();
    REQUIRE(quotaResponses.size() == 2);
    CHECK(openaiProvider->getQuotaFetchCount() == 1);
    CHECK(minimaxProvider->getQuotaFetchCount() == 1);
    const auto successCount = std::count_if(
        quotaResponses.begin(),
        quotaResponses.end(),
        [](const QuotaResponse& response) { return response.success; });
    CHECK(successCount == 1);
    CHECK((quotaResponses[0].error == "offline" || quotaResponses[1].error == "offline"));
}

TEST_CASE("Runtime test support preserves provider-manager ordering for provider-fed seams", "[runtime_test_support]") {
    const std::time_t now = std::time(nullptr);

    auto openaiProvider = makeCurrentQuotaProvider("openai", "OpenAI", 10, 100, now, "daily");
    auto minimaxProvider = makeCurrentQuotaProvider("minimax", "MiniMax", 20, 100, now, "daily");
    auto providerManager = makeProviderManager({openaiProvider, minimaxProvider});

    const auto enabledProviders = providerManager->getEnabledProviders();
    REQUIRE(enabledProviders.size() == 2);
    CHECK(enabledProviders[0]->getProviderKey() == "minimax");
    CHECK(enabledProviders[1]->getProviderKey() == "openai");
}

TEST_CASE("Runtime test support builds overlay preference toggles deterministically", "[runtime_test_support]") {
    const DashboardPreferences enabled = makeOverlayEnabledPreferences();
    const DashboardPreferences disabled = makeOverlayDisabledPreferences();

    CHECK(enabled.taskbarOverlayEnabled);
    CHECK_FALSE(disabled.taskbarOverlayEnabled);
    CHECK(enabled.startupEnabled == disabled.startupEnabled);
    CHECK(enabled.startupStartInBackground == disabled.startupStartInBackground);
}

TEST_CASE("Runtime test support builds compact taskbar fixtures for multi-provider and runtime-only paths", "[runtime_test_support]") {
    const std::time_t now = std::time(nullptr);

    const std::vector<TaskbarMetrics> providerMetrics = makeCompactMultiProviderTaskbarMetrics(now);
    REQUIRE(providerMetrics.size() == 2);
    CHECK(providerMetrics[0].providerKey == "openai");
    CHECK(providerMetrics[0].providerDisplayName == "OpenAI");
    CHECK(providerMetrics[0].used == 45);
    CHECK(providerMetrics[0].limit == 90);
    CHECK(providerMetrics[1].providerKey == "minimax");
    CHECK(providerMetrics[1].providerDisplayName == "MiniMax");
    CHECK(providerMetrics[1].used == 42000);
    CHECK(providerMetrics[1].limit == 100000);

    const TaskbarMetrics runtimeMetric = makeRuntimeOnlyTaskbarMetric(RuntimeState::Offline, now);
    CHECK(runtimeMetric.providerKey == "runtime");
    CHECK(runtimeMetric.kind == TaskbarMetricKind::PersistedActivity);
    CHECK(runtimeMetric.summaryLabel == "Runtime");
    CHECK(runtimeMetric.primaryMetric == "Off");
    CHECK(runtimeMetric.secondaryMetric == "Offline");
    CHECK(runtimeMetric.statusIcon == "!");
}
