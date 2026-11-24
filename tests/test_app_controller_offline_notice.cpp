#include "app_controller_test_support.hpp"

#include <memory>

#include "app/AppController.hpp"
#include "providers/ProviderManager.hpp"
#include "providers/claude/ClaudeCodeAdapter.hpp"
#include "providers/openai/OpenAIAdapter.hpp"
#include "sqlite_test_support.hpp"
#include "temp_test_directory.hpp"

using namespace JiStats;
using namespace JiStats::TestSupport;

TEST_CASE("AppController dashboard dismiss and tray retry keep offline notice low-noise across recovery transitions",
          "[app][runtime][offline_notice]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedAllProviderTrackingEnabled(dbPath.path());
    const std::time_t now = std::time(nullptr);
    ScopedTrayTestHandler handler([](TrayController::TestOperation, TrayStatus, const std::string&,
                                     std::string&) -> std::optional<bool> { return true; });

    AppController app;
    std::future<void> refresh;
    std::promise<void> releaseQuota;
    auto provider = std::make_shared<ControlledQuotaProviderAdapter>(
        "minimax", "MiniMax", makeFailedQuotaResponse("network unreachable"), releaseQuota.get_future().share(), 2);
    auto providerManager = std::make_shared<ProviderManager>();
    providerManager->addProvider(provider);
    app.setTaskbarTestModeForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);

    REQUIRE(app.init());
    REQUIRE(waitForCondition([&app]() { return app.isDashboardOfflineNoticeVisibleForTesting(); },
                             std::chrono::milliseconds(4000)));

    // The initial fetch makes the notice visible. Hold the next refresh's response.
    refresh = std::async(std::launch::async, [&app]() { app.refreshRuntimeForTesting(); });
    REQUIRE(waitForCondition([&provider]() { return provider->getQuotaFetchCount() > 1; }, std::chrono::seconds(4)));
    REQUIRE(provider->getCompletedQuotaFetchCount() == 1);

    REQUIRE(app.clickDashboardOfflineDismissForTesting());
    REQUIRE(waitForCondition(
        [&app]() {
            return !app.isDashboardOfflineNoticeVisibleForTesting() &&
                   !app.getActiveNoticeForTesting(NoticeType::Offline).has_value();
        },
        std::chrono::milliseconds(4000)));
    REQUIRE(provider->getCompletedQuotaFetchCount() == 1);
    CHECK(app.getTrayStateForTesting().status == TrayStatus::Offline);

    const auto dismissedConnectivity = app.getConnectivityStateForTesting("minimax");
    REQUIRE(dismissedConnectivity.has_value());
    CHECK(dismissedConnectivity->status == "offline");
    CHECK(dismissedConnectivity->noticeDismissedAt > 0);

    releaseQuota.set_value();
    refresh.get();
    CHECK_FALSE(app.isDashboardOfflineNoticeVisibleForTesting());
    CHECK_FALSE(app.getActiveNoticeForTesting(NoticeType::Offline).has_value());
    const auto stillOffline = app.getConnectivityStateForTesting("minimax");
    REQUIRE(stillOffline.has_value());
    CHECK(stillOffline->status == "offline");
    CHECK(stillOffline->noticeDismissedAt == dismissedConnectivity->noticeDismissedAt);

    provider->setQuotaResponse(makeQuotaResponse(now, 120, 1000, "daily", "minimax_subscription_coding_plan"));
    REQUIRE(app.clickDashboardOfflineRetryForTesting());
    REQUIRE(waitForCondition(
        [&app]() {
            const auto connectivity = app.getConnectivityStateForTesting("minimax");
            const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
            return connectivity.has_value() && connectivity->status == "online" &&
                   viewModel.runtimeState != RuntimeState::Offline && !viewModel.offlineNoticeVisible &&
                   !app.getActiveNoticeForTesting(NoticeType::Offline).has_value();
        },
        std::chrono::milliseconds(4000)));

    const auto recoveredConnectivity = app.getConnectivityStateForTesting("minimax");
    REQUIRE(recoveredConnectivity.has_value());
    CHECK(recoveredConnectivity->status == "online");
    CHECK(recoveredConnectivity->noticeDismissedAt == 0);
    CHECK_FALSE(app.isDashboardOfflineNoticeVisibleForTesting());

    provider->setQuotaResponse(makeFailedQuotaResponse("offline"));
    app.refreshRuntimeForTesting();
    REQUIRE(waitForCondition(
        [&app]() {
            return app.isDashboardOfflineNoticeVisibleForTesting() &&
                   app.getActiveNoticeForTesting(NoticeType::Offline).has_value();
        },
        std::chrono::milliseconds(4000)));

    const DashboardViewModel finalViewModel = app.getDashboardViewModelForTesting();
    CHECK(finalViewModel.runtimeState == RuntimeState::Offline);
    CHECK(finalViewModel.offlineRetryAvailable);
    CHECK(finalViewModel.offlineNoticeVisible);

    app.shutdown();
}

TEST_CASE("AppController persists dismissal when the quota check after catch-up is offline",
          "[app][runtime][offline_notice][recovery]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, false, true, false, false);
    const std::time_t now = std::time(nullptr);
    seedRuntimeState(dbPath.path(), now - 3 * 3600, now - 3 * 3600);
    ScopedTrayTestHandler handler([](TrayController::TestOperation, TrayStatus, const std::string&,
                                     std::string&) -> std::optional<bool> { return true; });

    AppController app;
    std::promise<void> releaseQuota;
    auto provider = std::make_shared<ControlledQuotaProviderAdapter>(
        "minimax", "MiniMax", makeFailedQuotaResponse("network unreachable"), releaseQuota.get_future().share(), 3,
        makeQuotaResponse(now, 120, 1000, "daily", "minimax_subscription_coding_plan"));
    auto providerManager = std::make_shared<ProviderManager>();
    providerManager->addProvider(provider);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setDisableTaskbarForTesting(true);

    REQUIRE(app.init());
    // The first response allowed catch-up and the second showed the notice.
    // Hold the deferred startup refresh so it cannot replace the saved result.
    REQUIRE(waitForCondition([&provider]() { return provider->getQuotaFetchCount() == 3; }, std::chrono::seconds(4)));
    REQUIRE(provider->getCompletedQuotaFetchCount() == 2);
    REQUIRE(app.isDashboardOfflineNoticeVisibleForTesting());

    const auto connectivity = app.getConnectivityStateForTesting("minimax");
    REQUIRE(connectivity.has_value());
    CHECK(connectivity->status == "offline");
    CHECK(connectivity->noticeDismissedAt == 0);

    REQUIRE(app.clickDashboardOfflineDismissForTesting());
    REQUIRE(waitForCondition([&app]() { return !app.isDashboardOfflineNoticeVisibleForTesting(); },
                             std::chrono::seconds(4)));
    REQUIRE(provider->getCompletedQuotaFetchCount() == 2);
    const auto dismissed = app.getConnectivityStateForTesting("minimax");
    REQUIRE(dismissed.has_value());
    REQUIRE(dismissed->noticeDismissedAt > 0);

    releaseQuota.set_value();
    app.refreshRuntimeForTesting();
    const auto refreshed = app.getConnectivityStateForTesting("minimax");
    REQUIRE(refreshed.has_value());
    CHECK(refreshed->status == "offline");
    CHECK(refreshed->noticeDismissedAt == dismissed->noticeDismissedAt);
    CHECK_FALSE(app.isDashboardOfflineNoticeVisibleForTesting());

    app.shutdown();
}

TEST_CASE("AppController keeps an offline dismissal across a provider handoff and restart",
          "[app][runtime][offline_notice]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, true, true, false, false);
    const std::time_t now = std::time(nullptr);
    auto openai = std::make_shared<ControlledQuotaProviderAdapter>(
        "openai", "OpenAI", makeQuotaResponse(now, 25, 100, "interval", "openai_subscription_codex_local"),
        std::shared_future<void>{}, 1, makeFailedQuotaResponse("network unreachable"));
    auto miniMax = std::make_shared<ControlledQuotaProviderAdapter>(
        "minimax", "MiniMax", makeFailedQuotaResponse("network unreachable"), std::shared_future<void>{}, 1,
        makeQuotaResponse(now, 120, 1000, "daily", "minimax_subscription_coding_plan"));
    auto providerManager = std::make_shared<ProviderManager>();
    providerManager->addProvider(openai);
    providerManager->addProvider(miniMax);
    ScopedTrayTestHandler handler([](TrayController::TestOperation, TrayStatus, const std::string&,
                                     std::string&) -> std::optional<bool> { return true; });

    std::promise<void> reachedPublication;
    auto publicationReached = reachedPublication.get_future();
    AppController app;
    std::promise<void> releasePublication;
    auto publicationReleased = releasePublication.get_future().share();
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setDisableTaskbarForTesting(true);
    app.setBeforeRuntimePublicationForTesting([&reachedPublication, publicationReleased]() {
        if (publicationReleased.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            reachedPublication.set_value();
            publicationReleased.wait();
        }
    });
    REQUIRE(app.init());
    // The deferred startup refresh has resolved both quota responses. Hold it
    // before it publishes the new provider set to the dashboard.
    REQUIRE(publicationReached.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const DashboardViewModel before = app.getDashboardViewModelForTesting();
    REQUIRE(before.offlineNoticeVisible);
    const auto* beforeOpenAI = findProviderStatus(before, "openai");
    const auto* beforeMiniMax = findProviderStatus(before, "minimax");
    REQUIRE(beforeOpenAI != nullptr);
    REQUIRE(beforeMiniMax != nullptr);
    CHECK(beforeOpenAI->state == ProviderState::Offline);
    CHECK(beforeMiniMax->state == ProviderState::Live);

    REQUIRE(app.clickDashboardOfflineDismissForTesting());
    REQUIRE(waitForCondition([&app]() { return !app.isDashboardOfflineNoticeVisibleForTesting(); },
                             std::chrono::seconds(5)));
    const auto dismissed = app.getConnectivityStateForTesting("openai");
    REQUIRE(dismissed.has_value());
    CHECK(dismissed->status == "offline");
    CHECK(dismissed->noticeDismissedAt > 0);

    releasePublication.set_value();
    app.refreshRuntimeForTesting();
    CHECK(app.getDashboardViewModelForTesting().runtimeState == RuntimeState::Offline);
    CHECK_FALSE(app.isDashboardOfflineNoticeVisibleForTesting());
    const auto recovered = app.getConnectivityStateForTesting("openai");
    const auto stillOffline = app.getConnectivityStateForTesting("minimax");
    REQUIRE(recovered.has_value());
    REQUIRE(stillOffline.has_value());
    CHECK(recovered->status == "online");
    CHECK(recovered->noticeDismissedAt == 0);
    CHECK(stillOffline->status == "offline");
    CHECK(stillOffline->noticeDismissedAt == dismissed->noticeDismissedAt);
    app.shutdown();

    AppController restarted;
    restarted.setDatabasePathForTesting(dbPath.path());
    restarted.setProviderManagerForTesting(providerManager);
    restarted.setDisableTaskbarForTesting(true);
    REQUIRE(restarted.init());
    restarted.refreshRuntimeForTesting();
    CHECK(restarted.getDashboardViewModelForTesting().runtimeState == RuntimeState::Offline);
    CHECK_FALSE(restarted.isDashboardOfflineNoticeVisibleForTesting());
    CHECK_FALSE(restarted.getActiveNoticeForTesting(NoticeType::Offline).has_value());

    // OpenAI recovered earlier and has no direct quota API, so its result
    // does not confirm that MiniMax's outage ended.
    auto preferences = restarted.getDashboardViewModelForTesting().preferences;
    preferences.miniMaxSubscriptionTrackingEnabled = false;
    restarted.applyDashboardPreferencesForTesting(preferences);
    REQUIRE(restarted.getDashboardViewModelForTesting().runtimeState == RuntimeState::Current);
    const auto disabled = restarted.getConnectivityStateForTesting("minimax");
    REQUIRE(disabled.has_value());
    CHECK(disabled->status == "offline");
    CHECK(disabled->noticeDismissedAt == dismissed->noticeDismissedAt);

    preferences.miniMaxSubscriptionTrackingEnabled = true;
    restarted.applyDashboardPreferencesForTesting(preferences);
    CHECK(restarted.getDashboardViewModelForTesting().runtimeState == RuntimeState::Offline);
    CHECK_FALSE(restarted.isDashboardOfflineNoticeVisibleForTesting());
    CHECK_FALSE(restarted.getActiveNoticeForTesting(NoticeType::Offline).has_value());
    restarted.shutdown();
}

TEST_CASE("AppController keeps an offline dismissal when the handoff write fails", "[app][runtime][offline_notice]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, true, true, false, false);
    const std::time_t now = std::time(nullptr);
    auto miniMax = std::make_shared<ControlledQuotaProviderAdapter>(
        "minimax", "MiniMax", makeQuotaResponse(now, 120, 1000, "daily", "minimax_subscription_coding_plan"),
        std::shared_future<void>{}, 1, makeFailedQuotaResponse("network unreachable"));
    auto openai = std::make_shared<ControlledQuotaProviderAdapter>(
        "openai", "OpenAI", makeFailedQuotaResponse("network unreachable"), std::shared_future<void>{}, 1,
        makeQuotaResponse(now, 25, 100, "interval", "openai_subscription_codex_local"));
    auto providerManager = std::make_shared<ProviderManager>();
    providerManager->addProvider(openai);
    providerManager->addProvider(miniMax);
    ScopedTrayTestHandler handler([](TrayController::TestOperation, TrayStatus, const std::string&,
                                     std::string&) -> std::optional<bool> { return true; });

    std::promise<void> reachedPublication;
    auto publicationReached = reachedPublication.get_future();
    AppController app;
    std::promise<void> releasePublication;
    auto publicationReleased = releasePublication.get_future().share();
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setDisableTaskbarForTesting(true);
    app.setBeforeRuntimePublicationForTesting([&reachedPublication, publicationReleased]() {
        if (publicationReleased.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            reachedPublication.set_value();
            publicationReleased.wait();
        }
    });
    REQUIRE(app.init());
    // Hold the deferred refresh in which MiniMax recovers and OpenAI goes offline.
    REQUIRE(publicationReached.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(app.isDashboardOfflineNoticeVisibleForTesting());
    REQUIRE(app.clickDashboardOfflineDismissForTesting());
    REQUIRE(waitForCondition([&app]() { return !app.isDashboardOfflineNoticeVisibleForTesting(); },
                             std::chrono::seconds(5)));
    const auto dismissed = app.getConnectivityStateForTesting("minimax");
    REQUIRE(dismissed.has_value());
    REQUIRE(dismissed->status == "offline");
    REQUIRE(dismissed->noticeDismissedAt > 0);

    {
        StorageService storage;
        REQUIRE(storage.init(dbPath.path()));
        const int openaiAccountId =
            requireAccountId(storage, requireProviderId(storage, "openai"), "openai.runtime_status");
        REQUIRE(storage
                    .execute("CREATE TRIGGER reject_openai_offline_row BEFORE INSERT ON connectivity_state "
                             "WHEN NEW.status = 'offline' AND NEW.account_id = " +
                             std::to_string(openaiAccountId) +
                             " "
                             "BEGIN SELECT RAISE(ABORT, 'test rejects the handoff write'); END;")
                    .success());
    }

    releasePublication.set_value();
    app.refreshRuntimeForTesting();
    CHECK(app.getDashboardViewModelForTesting().runtimeState == RuntimeState::Offline);
    CHECK_FALSE(app.isDashboardOfflineNoticeVisibleForTesting());
    app.shutdown();

    {
        StorageService storage;
        REQUIRE(storage.init(dbPath.path()));
        const int miniMaxProviderId = requireProviderId(storage, "minimax");
        const auto source = storage.getConnectivityState(
            miniMaxProviderId, requireAccountId(storage, miniMaxProviderId, "minimax.runtime_status"));
        REQUIRE(source.has_value());
        CHECK(source->status == "online");
        CHECK(source->noticeDismissedAt == dismissed->noticeDismissedAt);
        REQUIRE(storage.execute("DROP TRIGGER reject_openai_offline_row;").success());
    }

    AppController restarted;
    restarted.setDatabasePathForTesting(dbPath.path());
    restarted.setProviderManagerForTesting(providerManager);
    restarted.setDisableTaskbarForTesting(true);
    REQUIRE(restarted.init());
    restarted.refreshRuntimeForTesting();
    CHECK(restarted.getDashboardViewModelForTesting().runtimeState == RuntimeState::Offline);
    CHECK_FALSE(restarted.isDashboardOfflineNoticeVisibleForTesting());
    CHECK_FALSE(restarted.getActiveNoticeForTesting(NoticeType::Offline).has_value());
    const auto carried = restarted.getConnectivityStateForTesting("openai");
    REQUIRE(carried.has_value());
    CHECK(carried->status == "offline");
    CHECK(carried->noticeDismissedAt == dismissed->noticeDismissedAt);
    restarted.shutdown();
}

TEST_CASE("AppController keeps an offline dismissal when the newly offline provider has no saved row",
          "[app][runtime][offline_notice]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, true, false, false, false);
    const std::time_t now = std::time(nullptr);
    auto openai = std::make_shared<ControlledQuotaProviderAdapter>(
        "openai", "OpenAI", makeQuotaResponse(now, 25, 100, "interval", "openai_subscription_codex_local"),
        std::shared_future<void>{}, 1, makeFailedQuotaResponse("network unreachable"));
    // No providers row exists for this key, so its connectivity cannot be saved.
    auto unregistered = std::make_shared<ControlledQuotaProviderAdapter>(
        "unregistered_provider", "Unregistered", makeFailedQuotaResponse("network unreachable"),
        std::shared_future<void>{}, 1, makeQuotaResponse(now, 120, 1000, "daily", "unregistered_quota"));
    auto providerManager = std::make_shared<ProviderManager>();
    providerManager->addProvider(openai);
    providerManager->addProvider(unregistered);
    ScopedTrayTestHandler handler([](TrayController::TestOperation, TrayStatus, const std::string&,
                                     std::string&) -> std::optional<bool> { return true; });

    std::promise<void> reachedPublication;
    auto publicationReached = reachedPublication.get_future();
    AppController app;
    std::promise<void> releasePublication;
    auto publicationReleased = releasePublication.get_future().share();
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setDisableTaskbarForTesting(true);
    app.setBeforeRuntimePublicationForTesting([&reachedPublication, publicationReleased]() {
        if (publicationReleased.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            reachedPublication.set_value();
            publicationReleased.wait();
        }
    });
    REQUIRE(app.init());
    // Hold the deferred refresh in which OpenAI recovers and the unregistered provider goes offline.
    REQUIRE(publicationReached.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(app.isDashboardOfflineNoticeVisibleForTesting());
    REQUIRE(app.clickDashboardOfflineDismissForTesting());
    REQUIRE(waitForCondition([&app]() { return !app.isDashboardOfflineNoticeVisibleForTesting(); },
                             std::chrono::seconds(5)));
    const auto dismissed = app.getConnectivityStateForTesting("openai");
    REQUIRE(dismissed.has_value());
    REQUIRE(dismissed->status == "offline");
    REQUIRE(dismissed->noticeDismissedAt > 0);

    releasePublication.set_value();
    app.refreshRuntimeForTesting();
    CHECK(app.getDashboardViewModelForTesting().runtimeState == RuntimeState::Offline);
    CHECK_FALSE(app.isDashboardOfflineNoticeVisibleForTesting());
    CHECK_FALSE(app.getConnectivityStateForTesting("unregistered_provider").has_value());
    const auto recovered = app.getConnectivityStateForTesting("openai");
    REQUIRE(recovered.has_value());
    CHECK(recovered->status == "online");
    CHECK(recovered->noticeDismissedAt == dismissed->noticeDismissedAt);
    app.shutdown();

    AppController restarted;
    restarted.setDatabasePathForTesting(dbPath.path());
    restarted.setProviderManagerForTesting(providerManager);
    restarted.setDisableTaskbarForTesting(true);
    REQUIRE(restarted.init());
    restarted.refreshRuntimeForTesting();
    CHECK(restarted.getDashboardViewModelForTesting().runtimeState == RuntimeState::Offline);
    CHECK_FALSE(restarted.isDashboardOfflineNoticeVisibleForTesting());
    CHECK_FALSE(restarted.getActiveNoticeForTesting(NoticeType::Offline).has_value());
    restarted.shutdown();
}

TEST_CASE("AppController keeps an omitted provider's dismissal when a local provider recovers after restart",
          "[app][runtime][offline_notice]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, true, true, false, false);
    auto openai = std::make_shared<ControlledQuotaProviderAdapter>("openai", "OpenAI",
                                                                   makeFailedQuotaResponse("network unreachable"));
    auto miniMax = makeOfflineQuotaProvider("minimax", "MiniMax");
    auto providerManager = std::make_shared<ProviderManager>();
    providerManager->addProvider(openai);
    providerManager->addProvider(miniMax);
    ScopedTrayTestHandler handler([](TrayController::TestOperation, TrayStatus, const std::string&,
                                     std::string&) -> std::optional<bool> { return true; });

    AppController app;
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setDisableTaskbarForTesting(true);
    REQUIRE(app.init());
    REQUIRE(app.isDashboardOfflineNoticeVisibleForTesting());
    REQUIRE(app.clickDashboardOfflineDismissForTesting());
    REQUIRE(waitForCondition([&app]() { return !app.isDashboardOfflineNoticeVisibleForTesting(); },
                             std::chrono::seconds(5)));

    auto preferences = app.getDashboardViewModelForTesting().preferences;
    preferences.miniMaxSubscriptionTrackingEnabled = false;
    app.applyDashboardPreferencesForTesting(preferences);
    REQUIRE(app.getDashboardViewModelForTesting().runtimeState == RuntimeState::Offline);
    const auto dismissed = app.getConnectivityStateForTesting("minimax");
    REQUIRE(dismissed.has_value());
    REQUIRE(dismissed->noticeDismissedAt > 0);
    app.shutdown();

    const std::time_t now = std::time(nullptr);
    seedPersistedQuotaSnapshot(dbPath.path(), "openai", "acct-openai-cached", "OpenAI Cached Account",
                               "openai_subscription", "OpenAI Subscription", "interval", now - 60, 60, 100, 40);
    openai->setQuotaResponse(makeQuotaResponse(now, 25, 100, "interval", "openai_subscription_codex_local"));

    std::promise<void> reachedPublication;
    auto publicationReached = reachedPublication.get_future();
    AppController restarted;
    std::promise<void> releasePublication;
    auto publicationReleased = releasePublication.get_future().share();
    restarted.setDatabasePathForTesting(dbPath.path());
    restarted.setProviderManagerForTesting(providerManager);
    restarted.setDisableTaskbarForTesting(true);
    restarted.setBeforeRuntimePublicationForTesting([&reachedPublication, publicationReleased]() {
        if (publicationReleased.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            reachedPublication.set_value();
            publicationReleased.wait();
        }
    });
    REQUIRE(restarted.init());
    REQUIRE(publicationReached.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const DashboardViewModel cached = restarted.getDashboardViewModelForTesting();
    CHECK(cached.runtimeState != RuntimeState::Offline);
    const auto* cachedOpenAI = findProviderStatus(cached, "openai");
    REQUIRE(cachedOpenAI != nullptr);
    REQUIRE_FALSE(cachedOpenAI->quotas.empty());
    CHECK(cachedOpenAI->quotas.front().used == 60);
    const auto beforeRecovery = restarted.getConnectivityStateForTesting("minimax");
    REQUIRE(beforeRecovery.has_value());
    CHECK(beforeRecovery->noticeDismissedAt == dismissed->noticeDismissedAt);

    // OpenAI has no provider quota API, so its recovery ends only its own outage.
    releasePublication.set_value();
    restarted.refreshRuntimeForTesting();
    REQUIRE(restarted.getDashboardViewModelForTesting().runtimeState == RuntimeState::Current);
    const auto recoveredOpenAI = restarted.getConnectivityStateForTesting("openai");
    REQUIRE(recoveredOpenAI.has_value());
    CHECK(recoveredOpenAI->status == "online");
    CHECK(recoveredOpenAI->noticeDismissedAt == 0);
    const auto afterRecovery = restarted.getConnectivityStateForTesting("minimax");
    REQUIRE(afterRecovery.has_value());
    CHECK(afterRecovery->status == "offline");
    CHECK(afterRecovery->noticeDismissedAt == dismissed->noticeDismissedAt);

    preferences = restarted.getDashboardViewModelForTesting().preferences;
    preferences.miniMaxSubscriptionTrackingEnabled = true;
    restarted.applyDashboardPreferencesForTesting(preferences);
    CHECK(restarted.getDashboardViewModelForTesting().runtimeState == RuntimeState::Offline);
    CHECK_FALSE(restarted.isDashboardOfflineNoticeVisibleForTesting());
    CHECK_FALSE(restarted.getActiveNoticeForTesting(NoticeType::Offline).has_value());
    restarted.shutdown();
}

TEST_CASE("AppController preserves saved offline dismissal through inconclusive quota checks",
          "[app][runtime][offline_notice][kimi]") {
    requireSqlite();

    TempTestDirectory profile("jistats_offline_profile");
    ScopedEnvironmentVariable userProfile("USERPROFILE", profile.path().string());
    // This Codex rollout predates the dismissal and does not change.
    const std::time_t rolloutTime = std::time(nullptr) - 30 * 60;
    writeCodexRolloutFile(profile.path() / ".codex" / "sessions" / "2026" / "04" / "19" / "rollout-existing.jsonl",
                          25.0, rolloutTime, rolloutTime + 5 * 3600);
    ScopedEnvironmentVariable codexHome("CODEX_HOME", (profile.path() / ".codex").string());
    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, false, false, false, true);
    {
        StorageService storage;
        REQUIRE(storage.init(dbPath.path()));
        REQUIRE(ensureProviderId(storage, "kimi", "Kimi Code") > 0);
    }
    const auto offline = makeFailedQuotaResponse("network unreachable");
    auto kimi = std::make_shared<ControlledQuotaProviderAdapter>("kimi", "Kimi Code", offline);
    auto claude = std::make_shared<ClaudeCodeAdapter>();
    claude->setMockStatsCachePath("docs/fixtures/claude/stats-cache.redacted.json");
    auto openai = std::make_shared<OpenAIAdapter>();
    auto providerManager = std::make_shared<ProviderManager>();
    providerManager->addProvider(kimi);
    providerManager->addProvider(claude);
    providerManager->addProvider(openai);
    ScopedTrayTestHandler handler([](TrayController::TestOperation, TrayStatus, const std::string&,
                                     std::string&) -> std::optional<bool> { return true; });

    AppController app;
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setDisableTaskbarForTesting(true);
    REQUIRE(app.init());
    REQUIRE(app.isDashboardOfflineNoticeVisibleForTesting());
    REQUIRE(app.clickDashboardOfflineDismissForTesting());
    REQUIRE(waitForCondition([&app]() { return !app.isDashboardOfflineNoticeVisibleForTesting(); },
                             std::chrono::seconds(5)));
    // The offline startup also queues one background refresh. Let it finish before measuring the next check.
    REQUIRE(waitForCondition([&]() { return kimi->getCompletedQuotaFetchCount() >= 2; }, std::chrono::seconds(5)));
    app.refreshRuntimeForTesting();
    const auto dismissed = app.getConnectivityStateForTesting("kimi");
    REQUIRE(dismissed.has_value());
    REQUIRE(dismissed->noticeDismissedAt > 0);

    kimi->setQuotaResponse(makeFailedQuotaResponse("Kimi quota API credentials are not available"));
    app.refreshRuntimeForTesting();
    CHECK(app.getDashboardViewModelForTesting().runtimeState == RuntimeState::Unknown);
    const auto withoutCredentials = app.getConnectivityStateForTesting("kimi");
    REQUIRE(withoutCredentials.has_value());
    CHECK(withoutCredentials->noticeDismissedAt == dismissed->noticeDismissedAt);
    CHECK(withoutCredentials->status == dismissed->status);
    CHECK(withoutCredentials->nextRetryAt == dismissed->nextRetryAt);
    CHECK(withoutCredentials->lastCheckedAt == dismissed->lastCheckedAt);

    auto preferences = app.getDashboardViewModelForTesting().preferences;
    preferences.claudeCodeEnabled = true;
    preferences.openaiSubscriptionTrackingEnabled = true;
    app.applyDashboardPreferencesForTesting(preferences);
    const DashboardViewModel localOnly = app.getDashboardViewModelForTesting();
    CHECK(localOnly.runtimeState != RuntimeState::Offline);
    const auto* localKimi = findProviderStatus(localOnly, "kimi");
    REQUIRE(localKimi != nullptr);
    CHECK(localKimi->state == ProviderState::Sample);
    CHECK(localKimi->quotas.empty());
    const auto* localClaude = findProviderStatus(localOnly, "claude_code");
    REQUIRE(localClaude != nullptr);
    CHECK(localClaude->state == ProviderState::Stale);
    REQUIRE_FALSE(localClaude->quotas.empty());
    CHECK(localClaude->quotas.front().used > 0);
    const auto* localCodex = findProviderStatus(localOnly, "openai");
    REQUIRE(localCodex != nullptr);
    CHECK(localCodex->state == ProviderState::Live);
    REQUIRE_FALSE(localCodex->quotas.empty());
    CHECK(localCodex->quotas.front().limit == 100);
    const auto afterLocalStats = app.getConnectivityStateForTesting("kimi");
    REQUIRE(afterLocalStats.has_value());
    CHECK(afterLocalStats->noticeDismissedAt == dismissed->noticeDismissedAt);

    kimi->setQuotaResponse(offline);
    app.refreshRuntimeForTesting();
    CHECK(app.getDashboardViewModelForTesting().runtimeState == RuntimeState::Offline);
    CHECK_FALSE(app.isDashboardOfflineNoticeVisibleForTesting());

    const std::time_t now = std::time(nullptr);
    kimi->setQuotaResponse(makeQuotaResponse(now, 4, 10, "interval", "kimi_code_local"));
    app.refreshRuntimeForTesting();
    const DashboardViewModel sample = app.getDashboardViewModelForTesting();
    const auto* sampleKimi = findProviderStatus(sample, "kimi");
    REQUIRE(sampleKimi != nullptr);
    CHECK(sampleKimi->state == ProviderState::Sample);
    const auto withSample = app.getConnectivityStateForTesting("kimi");
    REQUIRE(withSample.has_value());
    CHECK(withSample->noticeDismissedAt == dismissed->noticeDismissedAt);

    kimi->setQuotaResponse(offline);
    app.refreshRuntimeForTesting();
    CHECK(app.getDashboardViewModelForTesting().runtimeState == RuntimeState::Offline);
    CHECK_FALSE(app.isDashboardOfflineNoticeVisibleForTesting());

    kimi->setQuotaResponse(makeQuotaResponse(now, 4, 10, "interval", "kimi_subscription_api"));
    app.refreshRuntimeForTesting();
    CHECK(app.getDashboardViewModelForTesting().runtimeState == RuntimeState::Current);
    const auto recovered = app.getConnectivityStateForTesting("kimi");
    REQUIRE(recovered.has_value());
    CHECK(recovered->noticeDismissedAt == 0);

    kimi->setQuotaResponse(offline);
    app.refreshRuntimeForTesting();
    CHECK(app.getDashboardViewModelForTesting().runtimeState == RuntimeState::Offline);
    CHECK(app.isDashboardOfflineNoticeVisibleForTesting());
    app.shutdown();
}

TEST_CASE("AppController keeps offline dismissal when remaining quota comes from the polling cache",
          "[app][runtime][offline_notice][polling]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, false, true, false, true);
    {
        StorageService storage;
        REQUIRE(storage.init(dbPath.path()));
        REQUIRE(ensureProviderId(storage, "kimi", "Kimi Code") > 0);
    }
    const std::time_t now = std::time(nullptr);
    auto miniMax =
        makeCurrentQuotaProvider("minimax", "MiniMax", 120, 1000, now, "daily", "minimax_subscription_coding_plan");
    miniMax->setDirectQuotaApiConfigured(true, "minimax:coding_plan_remains");
    auto kimi = makeOfflineQuotaProvider("kimi", "Kimi Code");
    auto providerManager = makeProviderManager({miniMax, kimi});
    ScopedTrayTestHandler handler([](TrayController::TestOperation, TrayStatus, const std::string&,
                                     std::string&) -> std::optional<bool> { return true; });

    AppController app;
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setDisableTaskbarForTesting(true);
    REQUIRE(app.init());
    REQUIRE(app.isDashboardOfflineNoticeVisibleForTesting());
    REQUIRE(miniMax->getQuotaFetchCount() == 1);
    REQUIRE(app.clickDashboardOfflineDismissForTesting());
    REQUIRE(waitForCondition([&app]() { return !app.isDashboardOfflineNoticeVisibleForTesting(); },
                             std::chrono::seconds(5)));
    const auto dismissed = app.getConnectivityStateForTesting("kimi");
    REQUIRE(dismissed.has_value());
    REQUIRE(dismissed->noticeDismissedAt > 0);

    auto preferences = app.getDashboardViewModelForTesting().preferences;
    preferences.kimiCodeEnabled = false;
    app.applyDashboardPreferencesForTesting(preferences);
    const DashboardViewModel cached = app.getDashboardViewModelForTesting();
    CHECK(cached.runtimeState == RuntimeState::Current);
    CHECK(miniMax->getQuotaFetchCount() == 1);
    const auto* cachedMiniMax = findProviderStatus(cached, "minimax");
    REQUIRE(cachedMiniMax != nullptr);
    REQUIRE_FALSE(cachedMiniMax->quotas.empty());
    CHECK(cachedMiniMax->quotas.front().used == 120);
    const auto afterCache = app.getConnectivityStateForTesting("kimi");
    REQUIRE(afterCache.has_value());
    CHECK(afterCache->noticeDismissedAt == dismissed->noticeDismissedAt);

    preferences.kimiCodeEnabled = true;
    app.applyDashboardPreferencesForTesting(preferences);
    CHECK(app.getDashboardViewModelForTesting().runtimeState == RuntimeState::Offline);
    CHECK_FALSE(app.isDashboardOfflineNoticeVisibleForTesting());

    preferences.kimiCodeEnabled = false;
    app.applyDashboardPreferencesForTesting(preferences);
    CHECK(miniMax->getQuotaFetchCount() == 1);
    app.setCurrentTimeForTesting(now + 301);
    app.refreshRuntimeForTesting();
    REQUIRE(miniMax->getQuotaFetchCount() == 2);
    const auto afterFetch = app.getConnectivityStateForTesting("kimi");
    REQUIRE(afterFetch.has_value());
    CHECK(afterFetch->noticeDismissedAt == 0);
    app.shutdown();
}
