#include <catch_amalgamated.hpp>

#include <ctime>
#include <filesystem>
#include <string>

#include "app/AppController.hpp"
#include "db/StorageService.hpp"
#include "gui/DashboardPresenter.hpp"
#include "observation/ActivityEvent.hpp"
#include "runtime_test_support.hpp"
#include "taskbar/TaskbarPresenter.hpp"

using namespace JiStats;
using namespace JiStats::TestSupport;

namespace {

class TempDbPath {
public:
    TempDbPath()
        : path_(std::filesystem::temp_directory_path() / "jistats_app_observation_test.db") {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    ~TempDbPath() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    std::string string() const {
        return path_.string();
    }

private:
    std::filesystem::path path_;
};

ActivityEvent makeObservedActivity() {
    const std::time_t now = std::time(nullptr);

    ActivityEvent event;
    event.providerKey = "openai";
    event.providerSurface = "openai_subscription";
    event.activityKind = ActivityKind::SessionActive;
    event.startedAt = now - 60;
    event.endedAt = now;
    event.durationSeconds = 60;
    event.sourceType = "local";
    event.sourceRunId = "app-run-1";
    event.surfaceKey = "foreground_window";
    event.windowTitleHint = "ChatGPT";
    event.confidence = ActivityConfidence::LocalExact;
    event.rawReference = "window:chatgpt";
    return event;
}

bool sqliteAvailable() {
    TempDbPath probePath;
    StorageService storage;
    const bool available = storage.init(probePath.string());
    if (available) {
        storage.close();
    }
    return available;
}

void applyProviderTrackingPreferences(
    AppController& app,
    bool openaiEnabled,
    bool minimaxEnabled,
    bool claudeEnabled = false,
    bool kimiEnabled = false) {
    DashboardPreferences preferences = app.getDashboardViewModelForTesting().preferences;
    preferences.openaiSubscriptionTrackingEnabled = openaiEnabled;
    preferences.miniMaxSubscriptionTrackingEnabled = minimaxEnabled;
    preferences.claudeCodeEnabled = claudeEnabled;
    preferences.kimiCodeEnabled = kimiEnabled;
    app.applyDashboardPreferencesForTesting(preferences);
}

std::string formatTimeUtcIsoForTest(std::time_t time) {
    std::tm tm{};
    gmtime_s(&tm, &time);

    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

std::time_t utcHourStartForTest(std::time_t time) {
    std::tm tm{};
    gmtime_s(&tm, &time);
    tm.tm_min = 0;
    tm.tm_sec = 0;
    return _mkgmtime(&tm);
}

std::time_t utcDayStartForTest(std::time_t time) {
    std::tm tm{};
    gmtime_s(&tm, &time);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    return _mkgmtime(&tm);
}

long long requireSingleId(const QueryResult& result) {
    REQUIRE(result.success());
    REQUIRE(result.rowCount() == 1);
    return result.getInt64(0, 0);
}

struct UsageSeedIds {
    int openaiProviderId = 0;
    int minimaxProviderId = 0;
    int openaiAccountId = 0;
    int minimaxAccountId = 0;
};

UsageSeedIds seedUsageAccounts(StorageService& storage) {
    UsageSeedIds ids;
    ids.openaiProviderId = static_cast<int>(requireSingleId(
        storage.execute("SELECT id FROM providers WHERE provider_key = 'openai';")));
    ids.minimaxProviderId = static_cast<int>(requireSingleId(
        storage.execute("SELECT id FROM providers WHERE provider_key = 'minimax';")));

    auto openaiInsert = storage.execute(
        "INSERT INTO accounts (provider_id, account_key, display_name, scope_type, scope_identifier) "
        "VALUES (?, ?, ?, ?, ?);",
        {
            std::to_string(ids.openaiProviderId),
            "acct-openai-rollup",
            "OpenAI Rollup Account",
            "user",
            "user-openai-rollup"
        });
    REQUIRE(openaiInsert.success());
    ids.openaiAccountId = static_cast<int>(openaiInsert.lastInsertRowId());

    auto minimaxInsert = storage.execute(
        "INSERT INTO accounts (provider_id, account_key, display_name, scope_type, scope_identifier) "
        "VALUES (?, ?, ?, ?, ?);",
        {
            std::to_string(ids.minimaxProviderId),
            "acct-minimax-rollup",
            "MiniMax Rollup Account",
            "user",
            "user-minimax-rollup"
        });
    REQUIRE(minimaxInsert.success());
    ids.minimaxAccountId = static_cast<int>(minimaxInsert.lastInsertRowId());

    return ids;
}

void insertUsageEvent(
    StorageService& storage,
    int providerId,
    int accountId,
    const std::string& eventTimeUtc,
    const std::string& modelKey,
    int inputTokens,
    int outputTokens,
    int totalTokens,
    int costMinorUnits) {
    auto insert = storage.execute(
        "INSERT INTO usage_events ("
        "provider_id, account_id, event_time_utc, source_type, model_key, request_type, "
        "input_tokens, output_tokens, total_tokens, cost_minor_units"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
        {
            std::to_string(providerId),
            std::to_string(accountId),
            eventTimeUtc,
            "api",
            modelKey,
            "chat",
            std::to_string(inputTokens),
            std::to_string(outputTokens),
            std::to_string(totalTokens),
            std::to_string(costMinorUnits)
        });
    REQUIRE(insert.success());
}

} // namespace

TEST_CASE("AppController persists local observation events through the runtime path", "[app][observation]") {
    if (!sqliteAvailable()) {
        SUCCEED("SQLite not available in this build; runtime persistence path cannot be exercised here.");
        return;
    }

    TempDbPath dbPath;
    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.string());

    REQUIRE(app.init());
    applyProviderTrackingPreferences(app, true, false);

    const ActivityEvent event = makeObservedActivity();
    REQUIRE(app.recordLocalActivityForTesting(event));

    const std::vector<PersistedActivityEvent> persisted =
        app.getPersistedActivitiesForTesting(
            "openai",
            "openai_subscription",
            event.startedAt - 5,
            event.startedAt + 120);

    REQUIRE(persisted.size() == 1);
    CHECK(persisted[0].activity.providerKey == "openai");
    CHECK(persisted[0].activity.providerSurface == "openai_subscription");
    CHECK(persisted[0].activity.activityKind == ActivityKind::SessionActive);
    CHECK(persisted[0].activity.startedAt == event.startedAt);
    CHECK(persisted[0].activity.endedAt == event.endedAt);
    CHECK(persisted[0].activity.durationSeconds == 60);
    CHECK(persisted[0].activity.sourceRunId == "app-run-1");
    CHECK(persisted[0].activity.surfaceKey == "foreground_window");
    CHECK(persisted[0].activity.windowTitleHint == "ChatGPT");
    CHECK(persisted[0].activity.confidence == ActivityConfidence::LocalExact);
    CHECK(persisted[0].planId.has_value() == false);

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    REQUIRE(viewModel.runtimeSummaries.size() == 4);
    CHECK(viewModel.runtimeSummaries[0].label == "Last 5 Minutes");
    CHECK(viewModel.runtimeSummaries[0].detail.find("1 session") != std::string::npos);
    CHECK(viewModel.runtimeSummaries[0].detail.find("1 min active") != std::string::npos);
    CHECK(viewModel.runtimeSummaries[0].percentage > 0);

    app.shutdown();
}

TEST_CASE("AppController drops new OpenAI local observations while OpenAI tracking is disabled", "[app][observation]") {
    if (!sqliteAvailable()) {
        SUCCEED("SQLite not available in this build; disabled observation gating cannot be exercised here.");
        return;
    }

    TempDbPath dbPath;
    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.string());

    REQUIRE(app.init());
    applyProviderTrackingPreferences(app, false, false);

    const ActivityEvent event = makeObservedActivity();
    CHECK_FALSE(app.recordLocalActivityForTesting(event));

    const std::vector<PersistedActivityEvent> persisted =
        app.getPersistedActivitiesForTesting(
            "openai",
            "openai_subscription",
            event.startedAt - 5,
            event.startedAt + 120);

    CHECK(persisted.empty());

    app.shutdown();
}

TEST_CASE("AppController keeps taskbar overlay on subscription quota when local activity exists", "[app][observation]") {
    if (!sqliteAvailable()) {
        SUCCEED("SQLite not available in this build; persisted taskbar overlay coverage cannot be exercised here.");
        return;
    }

    TempDbPath dbPath;
    const std::time_t now = std::time(nullptr);
    auto openaiProvider = makeCurrentQuotaProvider("openai", "OpenAI", 25, 100, now);
    auto minimaxProvider = makeOfflineQuotaProvider("minimax", "MiniMax");

    AppController app;
    app.setHeadlessForTesting(true);
    app.setTaskbarTestModeForTesting(true);
    app.setDatabasePathForTesting(dbPath.string());
    app.setProviderManagerForTesting(makeProviderManager({openaiProvider, minimaxProvider}));

    REQUIRE(app.init());
    applyProviderTrackingPreferences(app, true, true);
    REQUIRE(app.recordLocalActivityForTesting(makeObservedActivity()));

    const std::vector<TaskbarMetrics> taskbarMetrics = app.getTaskbarMetricsForTesting();
    // Quota-only overlay: the OpenAI subscription row stays, and offline MiniMax is omitted.
    REQUIRE(taskbarMetrics.size() == 1);

    const TaskbarMetrics* openaiMetric = findTaskbarMetric(taskbarMetrics, "openai");
    REQUIRE(openaiMetric != nullptr);
    CHECK(openaiMetric->kind == TaskbarMetricKind::QuotaUsage);
    CHECK(openaiMetric->used == 25);
    CHECK(openaiMetric->limit == 100);
    CHECK(findTaskbarMetric(taskbarMetrics, "minimax") == nullptr);

    app.shutdown();
}

TEST_CASE("AppController prefers rollup-backed quantified summaries when usage rollups exist", "[app][observation][rollups]") {
    if (!sqliteAvailable()) {
        SUCCEED("SQLite not available in this build; rollup-backed summary path cannot be exercised here.");
        return;
    }

    TempDbPath dbPath;
    StorageService storage;
    REQUIRE(storage.init(dbPath.string()));

    const auto ids = seedUsageAccounts(storage);
    const std::time_t now = std::time(nullptr);
    const std::time_t utcDayStart = utcDayStartForTest(now);
    const std::time_t secondsIntoUtcDay = std::max<std::time_t>(0, now - utcDayStart);
    if (secondsIntoUtcDay <= (31 * 60)) {
        SUCCEED("Current UTC time is too close to midnight to keep 'Today' distinct from 'Last 30 Minutes'.");
        return;
    }

    const std::time_t openaiRecent = now - 120;
    const std::time_t minimaxRecent = now - (20 * 60);
    const std::time_t openaiOlder = now - std::min<std::time_t>(3 * 60 * 60, secondsIntoUtcDay - 60);

    insertUsageEvent(storage, ids.openaiProviderId, ids.openaiAccountId, formatTimeUtcIsoForTest(openaiRecent), "gpt-4.1", 90, 110, 200, 25);
    insertUsageEvent(storage, ids.minimaxProviderId, ids.minimaxAccountId, formatTimeUtcIsoForTest(minimaxRecent), "MiniMax-Text-01", 120, 180, 300, 35);
    insertUsageEvent(storage, ids.openaiProviderId, ids.openaiAccountId, formatTimeUtcIsoForTest(openaiOlder), "gpt-4.1", 360, 440, 800, 70);

    const std::time_t rollupFrom = utcHourStartForTest(openaiOlder);
    const std::time_t rollupTo = utcHourStartForTest(now) + 3600;
    REQUIRE(storage.rebuildUsageRollups(
        UsageRollupResolution::Hourly,
        ids.openaiProviderId,
        ids.openaiAccountId,
        rollupFrom,
        rollupTo));
    REQUIRE(storage.rebuildUsageRollups(
        UsageRollupResolution::Hourly,
        ids.minimaxProviderId,
        ids.minimaxAccountId,
        rollupFrom,
        rollupTo));
    storage.close();

    AppController app;
    app.setHeadlessForTesting(true);
    app.setTaskbarTestModeForTesting(true);
    app.setDatabasePathForTesting(dbPath.string());
    REQUIRE(app.init());
    app.refreshRuntimeForTesting();

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    REQUIRE(viewModel.runtimeSummaries.size() == 4);
    CHECK(viewModel.runtimeSummaries[0].label == "Last 5 Minutes");
    CHECK(viewModel.runtimeSummaries[0].detail == "1 request, 200 tokens");
    CHECK(viewModel.runtimeSummaries[0].badge == "Usage");
    CHECK(viewModel.runtimeSummaries[1].label == "Last 30 Minutes");
    CHECK(viewModel.runtimeSummaries[1].detail == "2 requests, 500 tokens across 2 providers");
    CHECK(viewModel.runtimeSummaries[2].label == "Last 5 Hours");
    CHECK(viewModel.runtimeSummaries[2].detail == "3 requests, 1300 tokens across 2 providers");
    CHECK(viewModel.runtimeSummaries[3].label == "Today");
    CHECK(viewModel.runtimeSummaries[3].detail == "3 requests, 1300 tokens across 2 providers");

    app.shutdown();
}
