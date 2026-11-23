#include <catch_amalgamated.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "app/AppController.hpp"
#include "app/TrayController.hpp"
#include "core/Logger.hpp"
#include "db/SettingsService.hpp"
#include "db/StorageService.hpp"
#include "gui/DashboardPresenter.hpp"
#include "platform/WindowsCredentialStore.hpp"
#include "providers/IProviderAdapter.hpp"
#include "providers/ProviderManager.hpp"
#include "providers/SubscriptionQuotaAdapter.hpp"
#include "providers/claude/ClaudeCodeAdapter.hpp"
#include "providers/kimi/KimiAdapter.hpp"
#include "providers/minimax/MiniMaxAdapter.hpp"
#include "providers/openai/OpenAIAdapter.hpp"
#include "providers/openai/OpenAIExportImport.hpp"
#include "taskbar/TaskbarPresenter.hpp"
#include "runtime_test_support.hpp"
#include "sqlite_test_support.hpp"
#include "temp_test_directory.hpp"

using namespace JiStats;
using namespace JiStats::TestSupport;

#ifdef JISTATS_ENABLE_TEST_HOOKS
namespace JiStats {
std::chrono::milliseconds dormantRefreshWatcherIntervalForTesting();
std::chrono::milliseconds adaptiveWatcherIntervalOrDormantForTesting(
    bool watcherActive,
    int baseIntervalMs,
    int maxIntervalMs,
    size_t unchangedPasses);
}
#endif

namespace {

const std::filesystem::path kOpenAIExportFixturePath =
    std::filesystem::path("docs/fixtures/openai/chatgpt-export-redacted/conversations.json");

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const char* name, const std::string& value)
        : name_(name ? name : "") {
        char* current = nullptr;
        std::size_t currentLength = 0;
        if (!name_.empty() &&
            _dupenv_s(&current, &currentLength, name_.c_str()) == 0 &&
            current != nullptr) {
            previousValue_ = current;
            free(current);
        }
        hadPreviousValue_ = previousValue_.has_value();

        if (!name_.empty()) {
            _putenv_s(name_.c_str(), value.c_str());
        }
    }

    ~ScopedEnvironmentVariable() {
        if (name_.empty()) {
            return;
        }

        if (hadPreviousValue_) {
            _putenv_s(name_.c_str(), previousValue_->c_str());
        } else {
            _putenv_s(name_.c_str(), "");
        }
    }

private:
    std::string name_;
    std::optional<std::string> previousValue_;
    bool hadPreviousValue_ = false;
};

void writeFile(const std::filesystem::path& path, const std::string& contents) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.is_open());
    output << contents;
}

std::string toUtcIso(std::time_t value) {
    std::tm utcTime{};
    gmtime_s(&utcTime, &value);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utcTime);
    return buffer;
}

std::string formatReportDayLabelForTest(std::time_t value) {
    std::tm utcTime{};
    gmtime_s(&utcTime, &value);
    char buffer[16]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &utcTime);
    return buffer;
}

void writeRecentKimiFixture(const std::filesystem::path& root, std::time_t now) {
    const std::time_t startedAt = std::max<std::time_t>(1, now - 120);
    const std::time_t updatedAt = std::max<std::time_t>(startedAt, now - 30);
    const std::time_t finishedAt = std::max<std::time_t>(updatedAt, now - 5);

    writeFile(
        root / "config.toml",
        R"toml(default_model = "kimi-code/kimi-for-coding"

[models."kimi-code/kimi-for-coding"]
type = "kimi"
base_url = "https://api.kimi.com/coding/v1"
)toml");
    writeFile(
        root / "kimi.json",
        R"json({
  "work_dirs": [
    {
      "path": "C:/Users/example/current-project",
      "last_session_id": "session-recent"
    }
  ]
})json");
    writeFile(
        root / "sessions/session-recent/state.json",
        R"json({
  "plan_mode": true,
  "approval": {
    "yolo": false,
    "auto_approve_actions": true
  },
  "dynamic_subagents": ["a"],
  "additional_dirs": []
})json");

    std::ostringstream runtimeJson;
    runtimeJson << "{\n"
                << "  \"status\": \"completed\",\n"
                << "  \"started_at\": \"" << toUtcIso(startedAt) << "\",\n"
                << "  \"updated_at\": \"" << toUtcIso(updatedAt) << "\",\n"
                << "  \"finished_at\": \"" << toUtcIso(finishedAt) << "\",\n"
                << "  \"exit_code\": 0,\n"
                << "  \"interrupted\": false,\n"
                << "  \"timed_out\": false\n"
                << "}\n";
    writeFile(
        root / "sessions/session-recent/tasks/task-recent/runtime.json",
        runtimeJson.str());
    writeFile(
        root / "sessions/session-recent/notifications/notify-recent/event.json",
        R"json({
  "category": "task",
  "type": "task.completed",
  "source_kind": "background_task",
  "severity": "success",
  "payload": {
    "status": "completed",
    "exit_code": 0,
    "interrupted": false,
    "timed_out": false
  }
})json");
}

void writeInstalledButIdleKimiFixture(const std::filesystem::path& root) {
    writeFile(
        root / "config.toml",
        R"toml(default_model = "kimi-code/kimi-for-coding"

[models."kimi-code/kimi-for-coding"]
type = "kimi"
base_url = "https://api.kimi.com/coding/v1"
)toml");
    writeFile(
        root / "kimi.json",
        R"json({
  "work_dirs": []
})json");
    std::error_code ec;
    std::filesystem::create_directories(root / "sessions", ec);
}

void writeNestedRecentKimiFixture(const std::filesystem::path& root, std::time_t now) {
    const std::time_t startedAt = std::max<std::time_t>(1, now - 180);
    const std::time_t updatedAt = std::max<std::time_t>(startedAt, now - 60);
    const std::time_t finishedAt = std::max<std::time_t>(updatedAt, now - 10);

    writeFile(
        root / "config.toml",
        R"toml(default_model = "kimi-code/kimi-for-coding"

[models."kimi-code/kimi-for-coding"]
type = "kimi"
base_url = "https://api.kimi.com/coding/v1"
)toml");
    writeFile(
        root / "kimi.json",
        R"json({
  "work_dirs": [
    {
      "path": "C:/Users/example/current-project",
      "last_session_id": null
    }
  ]
})json");
    writeFile(
        root / "sessions/root-session/run-recent/state.json",
        R"json({
  "plan_mode": true,
  "approval": {
    "yolo": false,
    "auto_approve_actions": true
  },
  "dynamic_subagents": ["a"],
  "additional_dirs": []
})json");

    std::ostringstream runtimeJson;
    runtimeJson << "{\n"
                << "  \"status\": \"completed\",\n"
                << "  \"started_at\": \"" << toUtcIso(startedAt) << "\",\n"
                << "  \"updated_at\": \"" << toUtcIso(updatedAt) << "\",\n"
                << "  \"finished_at\": \"" << toUtcIso(finishedAt) << "\",\n"
                << "  \"exit_code\": 0,\n"
                << "  \"interrupted\": false,\n"
                << "  \"timed_out\": false\n"
                << "}\n";
    writeFile(
        root / "sessions/root-session/run-recent/tasks/task-recent/runtime.json",
        runtimeJson.str());
    writeFile(
        root / "sessions/root-session/run-recent/notifications/notify-recent/event.json",
        R"json({
  "category": "task",
  "type": "task.completed",
  "source_kind": "background_task",
  "severity": "success",
  "payload": {
    "status": "completed",
    "exit_code": 0,
    "interrupted": false,
    "timed_out": false
  }
})json");
}

std::time_t utcDayStartForTest(std::time_t value) {
    std::tm tm{};
    gmtime_s(&tm, &value);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    return _mkgmtime(&tm);
}

std::optional<std::time_t> parseUtcIsoForTest(const std::string& value) {
    if (value.empty()) {
        return std::nullopt;
    }

    std::tm tm{};
    std::istringstream stream(value);
    stream >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    if (stream.fail()) {
        return std::nullopt;
    }
    return _mkgmtime(&tm);
}

AnalysisDateRangePreset parseAnalysisPresetForTest(const std::string& value) {
    if (value == "today") {
        return AnalysisDateRangePreset::Today;
    }
    if (value == "last_24_hours") {
        return AnalysisDateRangePreset::Last24Hours;
    }
    if (value == "this_month") {
        return AnalysisDateRangePreset::ThisMonth;
    }
    if (value == "custom") {
        return AnalysisDateRangePreset::Custom;
    }
    return AnalysisDateRangePreset::Last7Days;
}

struct FakeStartupHooksState {
    std::wstring executablePath = L"C:\\Program Files\\JiStats\\JiStats.exe";
    std::wstring registryValue;
    std::vector<std::wstring> writeAttempts;
    bool failWrite = false;
    std::string writeError = "Injected startup rewrite failure";
};

StartupManager::TestHooks makeStartupHooks(const std::shared_ptr<FakeStartupHooksState>& state) {
    StartupManager::TestHooks hooks;
    hooks.executablePathProvider = [state]() {
        return state->executablePath;
    };
    hooks.writeRegistryValue = [state](const std::wstring& value, std::string& error) {
        state->writeAttempts.push_back(value);
        if (state->failWrite) {
            error = state->writeError;
            return false;
        }
        state->registryValue = value;
        return true;
    };
    hooks.readRegistryValue = [state]() {
        return state->registryValue;
    };
    return hooks;
}

void seedStartupPreferences(
    const std::string& path,
    bool enabled,
    bool startBackground,
    bool openaiSubscriptionTrackingEnabled = false,
    bool miniMaxSubscriptionTrackingEnabled = false,
    bool claudeCodeEnabled = false,
    bool kimiCodeEnabled = false,
    bool scheduledExportEnabled = false,
    int scheduledExportIntervalMinutes = 1440,
    const std::string& scheduledExportPath = "",
    const std::string& lastSelectedProviderId = "",
    const std::string& lastSelectedAccountId = "",
    const std::string& analysisSelectedProviderId = "",
    const std::string& analysisSelectedAccountId = "",
    const std::string& analysisDateRangePreset = "last_7_days",
    const std::string& analysisCustomRangeStartUtc = "",
    const std::string& analysisCustomRangeEndUtc = "") {
    StorageService storage;
    REQUIRE(storage.init(path));

    const auto setSetting = [&storage](const std::string& key, const std::string& value) {
        return storage.execute(
            "INSERT OR REPLACE INTO settings ([key], value, updated_at_utc) "
            "VALUES (?, ?, strftime('%Y-%m-%dT%H:%M:%SZ', 'now'));",
            {key, value}).success();
    };

    REQUIRE(setSetting(
        "runtime.openai_subscription_tracking_enabled",
        openaiSubscriptionTrackingEnabled ? "true" : "false"));
    REQUIRE(setSetting(
        "runtime.minimax_subscription_tracking_enabled",
        miniMaxSubscriptionTrackingEnabled ? "true" : "false"));
    REQUIRE(setSetting("runtime.claude_code_enabled", claudeCodeEnabled ? "true" : "false"));
    REQUIRE(setSetting("runtime.kimi_code_enabled", kimiCodeEnabled ? "true" : "false"));
    REQUIRE(setSetting("runtime.startup_enabled", enabled ? "true" : "false"));
    REQUIRE(setSetting("runtime.startup_background", startBackground ? "true" : "false"));
    REQUIRE(setSetting(
        "reporting.scheduled_export_enabled",
        scheduledExportEnabled ? "true" : "false"));
    REQUIRE(setSetting(
        "reporting.scheduled_export_interval_minutes",
        std::to_string(scheduledExportIntervalMinutes)));
    REQUIRE(setSetting("reporting.scheduled_export_path", scheduledExportPath));
    REQUIRE(setSetting("ui.last_selected_provider_id", lastSelectedProviderId));
    REQUIRE(setSetting("ui.last_selected_account_id", lastSelectedAccountId));
    REQUIRE(setSetting("ui.analysis_selected_provider_id", analysisSelectedProviderId));
    REQUIRE(setSetting("ui.analysis_selected_account_id", analysisSelectedAccountId));
    REQUIRE(setSetting("ui.analysis_date_range_preset", analysisDateRangePreset));
    REQUIRE(setSetting("ui.analysis_custom_range_start_utc", analysisCustomRangeStartUtc));
    REQUIRE(setSetting("ui.analysis_custom_range_end_utc", analysisCustomRangeEndUtc));

    storage.close();
}

void seedAllProviderTrackingEnabled(const std::string& path) {
    seedStartupPreferences(path, false, false, true, true, true, true);
}

DashboardPreferences loadStartupPreferences(const std::string& path) {
    StorageService storage;
    DashboardPreferences preferences;
    const bool opened = storage.init(path);
    CHECK(opened);
    if (!opened) {
        return preferences;
    }

    SettingsService settings(storage);
    preferences.openaiSubscriptionTrackingEnabled =
        settings.getBool("runtime.openai_subscription_tracking_enabled").value_or(false);
    preferences.miniMaxSubscriptionTrackingEnabled =
        settings.getBool("runtime.minimax_subscription_tracking_enabled").value_or(false);
    preferences.claudeCodeEnabled =
        settings.getBool("runtime.claude_code_enabled").value_or(false);
    preferences.kimiCodeEnabled =
        settings.getBool("runtime.kimi_code_enabled").value_or(false);
    preferences.startupEnabled =
        settings.getBool("runtime.startup_enabled").value_or(false);
    preferences.startupStartInBackground =
        settings.getBool("runtime.startup_background").value_or(false);
    preferences.scheduledExportEnabled =
        settings.getBool("reporting.scheduled_export_enabled").value_or(false);
    preferences.scheduledExportIntervalMinutes =
        settings.getInt("reporting.scheduled_export_interval_minutes").value_or(1440);
    preferences.scheduledExportPath =
        settings.getString("reporting.scheduled_export_path").value_or("");
    preferences.lastSelectedProviderId =
        settings.getString("ui.last_selected_provider_id").value_or("");
    preferences.lastSelectedAccountId =
        settings.getString("ui.last_selected_account_id").value_or("");
    preferences.analysisSelectedProviderId =
        settings.getString("ui.analysis_selected_provider_id").value_or("");
    preferences.analysisSelectedAccountId =
        settings.getString("ui.analysis_selected_account_id").value_or("");
    preferences.analysisDateRangePreset = parseAnalysisPresetForTest(
        settings.getString("ui.analysis_date_range_preset").value_or("last_7_days"));
    if (const auto stored = settings.getString("ui.analysis_custom_range_start_utc")) {
        if (const auto parsed = parseUtcIsoForTest(*stored)) {
            preferences.analysisCustomRangeStartUtc = *parsed;
        }
    }
    if (const auto stored = settings.getString("ui.analysis_custom_range_end_utc")) {
        if (const auto parsed = parseUtcIsoForTest(*stored)) {
            preferences.analysisCustomRangeEndUtc = *parsed;
        }
    }

    storage.close();
    return preferences;
}

void enableAllProviderTracking(AppController& app) {
    DashboardPreferences preferences = app.getDashboardViewModelForTesting().preferences;
    preferences.openaiSubscriptionTrackingEnabled = true;
    preferences.miniMaxSubscriptionTrackingEnabled = true;
    preferences.claudeCodeEnabled = true;
    preferences.kimiCodeEnabled = true;
    app.applyDashboardPreferencesForTesting(preferences);
}

std::optional<int> loadProviderId(StorageService& storage, const std::string& providerKey) {
    const QueryResult result = storage.execute(
        "SELECT id FROM providers WHERE provider_key = ? LIMIT 1;",
        {providerKey});
    if (!result.success() || result.rowCount() == 0) {
        return std::nullopt;
    }

    return result.getInt(0, 0);
}

std::optional<int> loadRuntimeAccountId(StorageService& storage, const std::string& providerKey) {
    const auto providerId = loadProviderId(storage, providerKey);
    if (!providerId.has_value()) {
        return std::nullopt;
    }

    const QueryResult result = storage.execute(
        "SELECT id FROM accounts WHERE provider_id = ? AND account_key = ? LIMIT 1;",
        {std::to_string(*providerId), providerKey + ".runtime_status"});
    if (!result.success() || result.rowCount() == 0) {
        return std::nullopt;
    }

    return result.getInt(0, 0);
}

std::optional<PersistedConnectivityState> loadConnectivityStateForProvider(
    const std::string& path,
    const std::string& providerKey) {
    StorageService storage;
    REQUIRE(storage.init(path));
    const auto providerId = loadProviderId(storage, providerKey);
    const auto accountId = loadRuntimeAccountId(storage, providerKey);
    REQUIRE(providerId.has_value());
    REQUIRE(accountId.has_value());
    auto state = storage.getConnectivityState(*providerId, *accountId);
    storage.close();
    return state;
}

std::optional<PersistedSyncState> loadRuntimeSyncStateForProvider(
    const std::string& path,
    const std::string& providerKey) {
    StorageService storage;
    REQUIRE(storage.init(path));
    const auto providerId = loadProviderId(storage, providerKey);
    const auto accountId = loadRuntimeAccountId(storage, providerKey);
    REQUIRE(providerId.has_value());
    REQUIRE(accountId.has_value());
    auto state = storage.getSyncState(*providerId, *accountId, "runtime_refresh");
    storage.close();
    return state;
}

std::optional<PersistedRuntimeState> loadPersistedRuntimeState(const std::string& path) {
    StorageService storage;
    REQUIRE(storage.init(path));
    auto state = storage.getRuntimeState();
    storage.close();
    return state;
}

std::vector<PersistedGapMarker> loadRuntimeGapMarkersForProvider(
    const std::string& path,
    const std::string& providerKey,
    std::time_t from,
    std::time_t to) {
    StorageService storage;
    REQUIRE(storage.init(path));
    const auto providerId = loadProviderId(storage, providerKey);
    const auto accountId = loadRuntimeAccountId(storage, providerKey);
    REQUIRE(providerId.has_value());
    REQUIRE(accountId.has_value());
    auto markers = storage.getGapMarkers(*providerId, *accountId, from, to);
    storage.close();
    return markers;
}

std::string makeCodexRolloutPayload(double usedPercent, std::time_t eventTime, std::time_t resetsAt) {
    return "{\"timestamp\":\"" + toUtcIso(eventTime) +
        "\",\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"rate_limits\":"
        "{\"limit_id\":\"codex\",\"primary\":{\"used_percent\":" + std::to_string(usedPercent) +
        ",\"window_minutes\":300,\"resets_at\":" + std::to_string(resetsAt) +
        "},\"secondary\":null,\"plan_type\":\"prolite\"}}}\n";
}

void writeCodexRolloutFile(
    const std::filesystem::path& path,
    double usedPercent,
    std::time_t eventTime,
    std::time_t resetsAt) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    REQUIRE_FALSE(ec);

    std::ofstream out(path, std::ios::trunc);
    REQUIRE(out.is_open());
    out << makeCodexRolloutPayload(usedPercent, eventTime, resetsAt);
    out.close();
}

void seedRuntimeState(
    const std::string& path,
    std::time_t lastLaunchAt,
    std::time_t lastSuccessfulSyncAt,
    const std::string& recoveryMarker = "") {
    StorageService storage;
    REQUIRE(storage.init(path));

    PersistedRuntimeState state;
    state.lastLaunchAt = lastLaunchAt;
    state.lastSuccessfulSyncAt = lastSuccessfulSyncAt;
    state.recoveryMarker = recoveryMarker;
    REQUIRE(storage.upsertRuntimeState(state));
    storage.close();
}

UsageResponse makeBackfillUsageResponseWithOneEvent(std::time_t eventTime) {
    UsageResponse usage;
    usage.success = true;
    usage.windowStart = eventTime - 60;
    usage.windowEnd = eventTime + 60;

    UsageEvent event;
    event.eventTime = eventTime;
    event.modelKey = "gpt-5";
    event.requestType = "chat";
    event.inputTokens = 120;
    event.outputTokens = 80;
    event.totalTokens = 200;
    event.costMinorUnits = 15;
    event.sourceType = "api";
    event.sourceRunId = "recovery-backfill-1";
    event.rawReference = "evt-1";
    usage.events.push_back(event);

    usage.totalInputTokens = event.inputTokens;
    usage.totalOutputTokens = event.outputTokens;
    usage.totalCost = event.costMinorUnits;
    usage.isBackfilled = true;
    return usage;
}

long long requireSingleId(const QueryResult& result) {
    REQUIRE(result.success());
    REQUIRE(result.rowCount() == 1);
    return result.getInt64(0, 0);
}

int requireProviderId(StorageService& storage, const std::string& providerKey) {
    return static_cast<int>(requireSingleId(
        storage.execute(
            "SELECT id FROM providers WHERE provider_key = ? LIMIT 1;",
            {providerKey})));
}

int requireAccountId(StorageService& storage, int providerId, const std::string& accountKey) {
    return static_cast<int>(requireSingleId(
        storage.execute(
            "SELECT id FROM accounts WHERE provider_id = ? AND account_key = ? LIMIT 1;",
            {std::to_string(providerId), accountKey})));
}

int ensureProviderId(StorageService& storage, const std::string& providerKey, const std::string& displayName) {
    if (const auto existing = loadProviderId(storage, providerKey)) {
        return *existing;
    }

    auto insert = storage.execute(
        "INSERT INTO providers (provider_key, display_name) VALUES (?, ?);",
        {providerKey, displayName});
    REQUIRE(insert.success());
    return static_cast<int>(insert.lastInsertRowId());
}

int ensureRuntimeAccountId(
    StorageService& storage,
    const std::string& providerKey,
    const std::string& displayName) {
    const int providerId = ensureProviderId(storage, providerKey, displayName);
    if (const auto existing = loadRuntimeAccountId(storage, providerKey)) {
        return *existing;
    }

    auto insert = storage.execute(
        "INSERT INTO accounts (provider_id, account_key, display_name, scope_type, scope_identifier) "
        "VALUES (?, ?, ?, ?, ?);",
        {
            std::to_string(providerId),
            providerKey + ".runtime_status",
            displayName + " Runtime",
            "runtime",
            providerKey + ".runtime_status"
        });
    REQUIRE(insert.success());
    return static_cast<int>(insert.lastInsertRowId());
}

void seedRuntimeUsageEvent(
    const std::string& path,
    const std::string& providerKey,
    const std::string& displayName,
    std::time_t eventTime,
    int totalTokens,
    int costMinorUnits,
    const std::string& modelKey = "gpt-5") {
    StorageService storage;
    REQUIRE(storage.init(path));
    const int providerId = ensureProviderId(storage, providerKey, displayName);
    const int accountId = ensureRuntimeAccountId(storage, providerKey, displayName);
    const int inputTokens = totalTokens / 2;
    const int outputTokens = totalTokens - inputTokens;
    PersistedUsageEvent event;
    event.providerId = providerId;
    event.accountId = accountId;
    event.eventTime = eventTime;
    event.sourceType = "api";
    event.sourceRunId = providerKey + "-usage-" + std::to_string(eventTime);
    event.modelKey = modelKey;
    event.requestType = "chat";
    event.inputTokens = inputTokens;
    event.outputTokens = outputTokens;
    event.totalTokens = totalTokens;
    event.costMinorUnits = costMinorUnits;
    event.rawReference = providerKey + "://" + modelKey + "/" + std::to_string(eventTime);
    REQUIRE(storage.insertUsageEvents({event}));
    storage.close();
}

struct UsageStorageIds {
    int providerId = 0;
    int accountId = 0;
};

UsageStorageIds seedUsageProviderAccount(
    StorageService& storage,
    const std::string& providerKey,
    const std::string& accountKey,
    const std::string& accountDisplayName) {
    UsageStorageIds ids;
    ids.providerId = static_cast<int>(requireSingleId(
        storage.execute(
            "SELECT id FROM providers WHERE provider_key = ? LIMIT 1;",
            {providerKey})));

    auto accountInsert = storage.execute(
        "INSERT INTO accounts (provider_id, account_key, display_name, scope_type, scope_identifier) "
        "VALUES (?, ?, ?, ?, ?);",
        {
            std::to_string(ids.providerId),
            accountKey,
            accountDisplayName,
            "user",
            accountKey
        });
    REQUIRE(accountInsert.success());
    ids.accountId = static_cast<int>(accountInsert.lastInsertRowId());
    return ids;
}

void insertUsageEvent(
    StorageService& storage,
    int providerId,
    int accountId,
    const std::string& eventTimeUtc,
    const std::string& modelKey,
    int totalTokens,
    int costMinorUnits) {
    const int inputTokens = totalTokens / 2;
    const int outputTokens = totalTokens - inputTokens;
    const auto eventTime = parseUtcIsoForTest(eventTimeUtc);
    REQUIRE(eventTime.has_value());

    PersistedUsageEvent event;
    event.providerId = providerId;
    event.accountId = accountId;
    event.eventTime = *eventTime;
    event.sourceType = "api";
    event.sourceRunId = "seeded-usage-" + std::to_string(*eventTime);
    event.modelKey = modelKey;
    event.requestType = "chat";
    event.inputTokens = inputTokens;
    event.outputTokens = outputTokens;
    event.totalTokens = totalTokens;
    event.costMinorUnits = costMinorUnits;
    event.rawReference = "seed://usage/" + modelKey + "/" + std::to_string(*eventTime);
    REQUIRE(storage.insertUsageEvents({event}));
}

std::optional<DashboardSummaryItem> findUsageSummary(
    const std::vector<DashboardSummaryItem>& summaries,
    const std::string& label) {
    const auto it = std::find_if(
        summaries.begin(),
        summaries.end(),
        [&label](const DashboardSummaryItem& summary) {
            return summary.label == label;
        });
    if (it == summaries.end()) {
        return std::nullopt;
    }
    return *it;
}

std::optional<AnalysisDailySummary> findAnalysisDailySummary(
    const std::vector<AnalysisDailySummary>& rows,
    const std::string& dateLabel) {
    const auto it = std::find_if(rows.begin(), rows.end(), [&](const AnalysisDailySummary& row) {
        return row.dateLabel == dateLabel;
    });
    if (it == rows.end()) {
        return std::nullopt;
    }
    return *it;
}

std::optional<AnalysisBreakdownRow> findAnalysisBreakdownRow(
    const std::vector<AnalysisBreakdownRow>& rows,
    const std::string& label) {
    const auto it = std::find_if(rows.begin(), rows.end(), [&](const AnalysisBreakdownRow& row) {
        return row.label == label;
    });
    if (it == rows.end()) {
        return std::nullopt;
    }
    return *it;
}

std::optional<AnalysisTrendPoint> findAnalysisTrendPoint(
    const std::vector<AnalysisTrendPoint>& rows,
    const std::string& dateLabel) {
    const auto it = std::find_if(rows.begin(), rows.end(), [&](const AnalysisTrendPoint& row) {
        return row.dateLabel == dateLabel;
    });
    if (it == rows.end()) {
        return std::nullopt;
    }
    return *it;
}

std::vector<std::filesystem::path> listHtmlReports(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> reports;
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) {
        return reports;
    }

    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) {
            break;
        }
        if (entry.is_regular_file(ec) && entry.path().extension() == ".html") {
            reports.push_back(entry.path());
        }
    }

    std::sort(reports.begin(), reports.end());
    return reports;
}

const ProviderStatus* findProviderStatus(
    const DashboardViewModel& viewModel,
    const std::string& providerKey) {
    const auto it = std::find_if(
        viewModel.providers.begin(),
        viewModel.providers.end(),
        [&providerKey](const ProviderStatus& provider) {
            return provider.providerKey == providerKey;
        });
    return it == viewModel.providers.end() ? nullptr : &(*it);
}

class ScopedTrayTestHandler {
public:
    explicit ScopedTrayTestHandler(TrayController::TestOperationHandler handler) {
        TrayController::setTestOperationHandler(std::move(handler));
    }

    ~ScopedTrayTestHandler() {
        TrayController::clearTestOperationHandler();
    }
};

class FakeWindowsCredentialBackend final : public IWindowsCredentialBackend {
public:
    bool writeGenericCredential(
        const WindowsCredentialWriteRequest& request,
        std::string& error) override {
        if (request.targetName.empty() || request.secret.empty()) {
            error = "Credential target name and secret are required";
            return false;
        }

        secrets_[request.targetName] = request.secret;
        error.clear();
        return true;
    }

    std::optional<std::string> readGenericCredential(
        const std::string& targetName,
        std::string& error) override {
        const auto it = secrets_.find(targetName);
        if (it == secrets_.end()) {
            error = "Credential not found: " + targetName;
            return std::nullopt;
        }

        error.clear();
        return it->second;
    }

    bool deleteGenericCredential(
        const std::string& targetName,
        std::string& error) override {
        const auto it = secrets_.find(targetName);
        if (it == secrets_.end()) {
            error = "Credential not found: " + targetName;
            return false;
        }

        secrets_.erase(it);
        error.clear();
        return true;
    }

    std::vector<WindowsCredentialSummary> listGenericCredentials(
        const std::string& prefixFilter,
        std::string& error) override {
        std::vector<WindowsCredentialSummary> summaries;
        for (const auto& [targetName, secret] : secrets_) {
            if (!prefixFilter.empty() && targetName.rfind(prefixFilter, 0) != 0) {
                continue;
            }

            WindowsCredentialSummary summary;
            summary.targetName = targetName;
            summaries.push_back(std::move(summary));
        }
        error.clear();
        return summaries;
    }

    std::optional<std::string> peek(const std::string& targetName) const {
        const auto it = secrets_.find(targetName);
        if (it == secrets_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

private:
    std::map<std::string, std::string> secrets_;
};

std::shared_ptr<FakeWindowsCredentialBackend> makeFakeCredentialBackend() {
    return std::make_shared<FakeWindowsCredentialBackend>();
}

std::optional<MiniMaxCredentialProfileSummary> findMiniMaxSummary(
    const DashboardViewModel& viewModel,
    const std::string& planKey) {
    const auto it = std::find_if(
        viewModel.minimaxCredentialProfiles.begin(),
        viewModel.minimaxCredentialProfiles.end(),
        [&planKey](const MiniMaxCredentialProfileSummary& summary) {
            return summary.planKey == planKey;
        });
    if (it == viewModel.minimaxCredentialProfiles.end()) {
        return std::nullopt;
    }
    return *it;
}

std::string formatTimeUtcIsoForTest(std::time_t time) {
    std::tm tm{};
    gmtime_s(&tm, &time);

    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

void seedMiniMaxCredentialMetadata(
    const std::string& path,
    const std::string& planKey,
    const std::string& accountDisplayName) {
    StorageService storage;
    REQUIRE(storage.init(path));

    const auto providerResult = storage.execute(
        "SELECT id FROM providers WHERE provider_key = 'minimax' LIMIT 1;");
    REQUIRE(providerResult.success());
    REQUIRE(providerResult.rowCount() == 1);
    const int providerId = providerResult.getInt(0, 0);

    const std::string accountKey = planKey + ".default";
    const auto accountInsert = storage.execute(
        "INSERT INTO accounts (provider_id, account_key, display_name, scope_type, scope_identifier) "
        "VALUES (?, ?, ?, ?, ?);",
        {
            std::to_string(providerId),
            accountKey,
            accountDisplayName,
            "subscription",
            accountKey
        });
    REQUIRE(accountInsert.success());
    const int accountId = static_cast<int>(accountInsert.lastInsertRowId());

    PersistedCredentialProfile profile;
    profile.providerId = providerId;
    profile.accountId = accountId;
    profile.profileName = planKey == "token_plan" ? "MiniMax Token Plan" : "MiniMax Coding Plan";
    profile.secretRef = MiniMaxAdapter::buildSecretRef(MiniMaxSubscriptionCredentialProfile{
        "minimax",
        accountKey,
        accountDisplayName,
        "subscription",
        accountKey,
        profile.profileName,
        planKey,
        profile.profileName,
        "subscription_key",
        "",
        planKey == "token_plan" ? "token_plan_remains" : "coding_plan_remains",
        "unknown",
        0,
        ""
    });
    profile.credentialType = "subscription_key";
    profile.lastValidatedAt = 1713900000;
    REQUIRE(storage.upsertCredentialProfile(profile));
    storage.close();
}

void seedPersistedQuotaSnapshot(
    const std::string& path,
    const std::string& providerKey,
    const std::string& accountKey,
    const std::string& accountDisplayName,
    const std::string& planKey,
    const std::string& planDisplayName,
    const std::string& quotaWindow,
    std::time_t snapshotTime,
    long long usedValue,
    long long limitValue,
    long long remainingValue,
    const std::string& confidenceTier = "high") {
    StorageService storage;
    REQUIRE(storage.init(path));

    const QueryResult providerResult = storage.execute(
        "SELECT id FROM providers WHERE provider_key = ? LIMIT 1;",
        {providerKey});
    REQUIRE(providerResult.success());
    REQUIRE(providerResult.rowCount() == 1);
    const int providerId = providerResult.getInt(0, 0);

    const auto accountInsert = storage.execute(
        "INSERT INTO accounts (provider_id, account_key, display_name, scope_type, scope_identifier) "
        "VALUES (?, ?, ?, ?, ?);",
        {
            std::to_string(providerId),
            accountKey,
            accountDisplayName,
            "subscription",
            accountKey
        });
    REQUIRE(accountInsert.success());
    const int accountId = static_cast<int>(accountInsert.lastInsertRowId());

    const auto planInsert = storage.execute(
        "INSERT INTO plans (account_id, plan_key, plan_type, display_name, quota_window, limit_value) "
        "VALUES (?, ?, ?, ?, ?, ?);",
        {
            std::to_string(accountId),
            planKey,
            "subscription",
            planDisplayName,
            quotaWindow,
            std::to_string(limitValue)
        });
    REQUIRE(planInsert.success());
    const int planId = static_cast<int>(planInsert.lastInsertRowId());

    REQUIRE(storage.execute(
        "INSERT INTO quota_snapshots ("
        "provider_id, account_id, plan_id, snapshot_time_utc, window_start_utc, window_end_utc, "
        "used_value, limit_value, confidence_tier, basis_summary, quota_dimension, remaining_value, is_reconciled) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
        {
            std::to_string(providerId),
            std::to_string(accountId),
            std::to_string(planId),
            formatTimeUtcIsoForTest(snapshotTime),
            formatTimeUtcIsoForTest(snapshotTime - 3600),
            formatTimeUtcIsoForTest(snapshotTime),
            std::to_string(usedValue),
            std::to_string(limitValue),
            confidenceTier,
            "Persisted test snapshot",
            "requests",
            std::to_string(remainingValue),
            "0"
        }).success());

    storage.close();
}

class ControlledQuotaProviderAdapter final : public IProviderAdapter {
public:
    ControlledQuotaProviderAdapter(
        std::string providerKey,
        std::string displayName,
        QuotaResponse quotaResponse,
        std::shared_future<void> releaseQuota = {},
        int firstBlockedFetch = 1,
        std::optional<QuotaResponse> firstQuotaResponse = std::nullopt)
        : providerKey_(std::move(providerKey)),
          displayName_(std::move(displayName)),
          quotaResponse_(std::move(quotaResponse)),
          releaseQuota_(std::move(releaseQuota)),
          firstBlockedFetch_(firstBlockedFetch),
          firstQuotaResponse_(std::move(firstQuotaResponse)) {}

    void setQuotaResponse(QuotaResponse response) {
        std::lock_guard<std::mutex> lock(quotaMutex_);
        quotaResponse_ = std::move(response);
    }

    UsageResponse fetchUsage(std::time_t from, std::time_t to) override {
        (void)from;
        (void)to;
        return makeUsageResponse(std::time(nullptr));
    }

    QuotaResponse fetchQuota() override {
        const int fetchNumber = ++quotaFetchCount_;
        const bool released = fetchNumber < firstBlockedFetch_ || !releaseQuota_.valid() ||
            releaseQuota_.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        ++completedQuotaFetchCount_;
        if (!released) {
            return makeFailedQuotaResponse("Timed out waiting for the test to release quota");
        }
        if (fetchNumber == 1 && firstQuotaResponse_.has_value()) {
            return *firstQuotaResponse_;
        }
        std::lock_guard<std::mutex> lock(quotaMutex_);
        return quotaResponse_;
    }

    bool validateCredentials() override {
        return true;
    }

    std::vector<Capability> getCapabilities() override {
        return {{"quota_api", "Quota API access", true, ""}};
    }

    std::string getProviderKey() const override {
        return providerKey_;
    }

    std::string getDisplayName() const override {
        return displayName_;
    }

    bool isEnabled() const override {
        return enabled_;
    }

    void setEnabled(bool enabled) override {
        enabled_ = enabled;
    }

    int getQuotaFetchCount() const {
        return quotaFetchCount_.load();
    }

    int getCompletedQuotaFetchCount() const {
        return completedQuotaFetchCount_.load();
    }

private:
    std::string providerKey_;
    std::string displayName_;
    std::mutex quotaMutex_;
    QuotaResponse quotaResponse_;
    std::shared_future<void> releaseQuota_;
    const int firstBlockedFetch_;
    const std::optional<QuotaResponse> firstQuotaResponse_;
    bool enabled_ = true;
    std::atomic<int> quotaFetchCount_{0};
    std::atomic<int> completedQuotaFetchCount_{0};
};

class CountingDetectionProviderAdapter final : public IProviderAdapter {
public:
    CountingDetectionProviderAdapter(
        std::string providerKey,
        std::string displayName,
        bool validateResult)
        : providerKey_(std::move(providerKey)),
          displayName_(std::move(displayName)),
          validateResult_(validateResult) {}

    UsageResponse fetchUsage(std::time_t from, std::time_t to) override {
        (void)from;
        (void)to;
        return makeUsageResponse(std::time(nullptr));
    }

    QuotaResponse fetchQuota() override {
        ++quotaFetchCount_;
        return makeQuotaResponse(std::time(nullptr), 0, 0, "local", providerKey_ + "_local");
    }

    bool validateCredentials() override {
        ++validateCredentialsCount_;
        return validateResult_;
    }

    std::vector<Capability> getCapabilities() override {
        return {{"local_detection", "Local detection", true, ""}};
    }

    std::string getProviderKey() const override {
        return providerKey_;
    }

    std::string getDisplayName() const override {
        return displayName_;
    }

    bool isEnabled() const override {
        return enabled_;
    }

    void setEnabled(bool enabled) override {
        enabled_ = enabled;
    }

    int getValidateCredentialsCount() const {
        return validateCredentialsCount_.load();
    }

    int getQuotaFetchCount() const {
        return quotaFetchCount_.load();
    }

private:
    std::string providerKey_;
    std::string displayName_;
    bool validateResult_ = false;
    bool enabled_ = true;
    std::atomic<int> validateCredentialsCount_{0};
    std::atomic<int> quotaFetchCount_{0};
};

bool waitForCondition(const std::function<bool()>& predicate,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

} // namespace

TEST_CASE("AppController cleans up logger state after forced startup failure", "[app][startup]") {
    if (Logger::isInitialized()) {
        Logger::shutdown();
    }

    TempTestDatabase dbPath;
    AppController app;
    app.setDatabasePathForTesting(dbPath.path());
    app.setHeadlessForTesting(true);
    app.setForceDashboardInitFailureForTesting(true);

    REQUIRE_FALSE(app.init());
    REQUIRE(app.getState() == "error");
    REQUIRE_FALSE(Logger::isInitialized());

    app.shutdown();
    REQUIRE_FALSE(Logger::isInitialized());
}

TEST_CASE("AppController honors launch-in-background startup mode", "[app][startup]") {
    auto startupState = std::make_shared<FakeStartupHooksState>();

    TempTestDatabase dbPath;
    AppController app;
    app.setDatabasePathForTesting(dbPath.path());
    app.setHeadlessForTesting(true);
    app.setStartupManagerTestHooks(makeStartupHooks(startupState));
    REQUIRE(app.init(true));

    CHECK_FALSE(app.isDashboardVisibleForTesting());

    app.shutdown();
}

TEST_CASE("AppController explains a rejected database during background startup", "[app][startup]") {
    requireSqlite();

    TempTestDatabase dbPath;
    StorageService storage;
    REQUIRE(storage.init(dbPath.path()));
    REQUIRE(storage.execute("UPDATE schema_version SET version = 11;").success());
    storage.close();

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    REQUIRE(app.init(true));

    const auto warning = app.getStartupWarning();
    CHECK(warning.find(dbPath.path()) != std::string::npos);
    CHECK(warning.find("version 11") != std::string::npos);
    CHECK(warning.find("persistence is disabled") != std::string::npos);
    CHECK(warning.find("move this database") != std::string::npos);
    app.shutdown();
}

TEST_CASE("AppController applies persisted startup preferences through StartupManager", "[app][startup]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), true, true);

    auto startupState = std::make_shared<FakeStartupHooksState>();

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setStartupManagerTestHooks(makeStartupHooks(startupState));

    REQUIRE(app.init());

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    CHECK(viewModel.preferences.startupEnabled);
    CHECK(viewModel.preferences.startupStartInBackground);

    REQUIRE_FALSE(startupState->writeAttempts.empty());
    CHECK(startupState->writeAttempts.back().find(L"--background") != std::wstring::npos);

    app.shutdown();
}

TEST_CASE("AppController rewrites an enabled startup entry when persisted background mode changes", "[app][startup]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), true, true);

    auto startupState = std::make_shared<FakeStartupHooksState>();
    startupState->registryValue = L"\"C:\\Program Files\\JiStats\\JiStats.exe\"";

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setStartupManagerTestHooks(makeStartupHooks(startupState));

    REQUIRE(app.init());

    REQUIRE(startupState->writeAttempts.size() == 1);
    CHECK(startupState->writeAttempts.back() == L"\"C:\\Program Files\\JiStats\\JiStats.exe\" --background");
    CHECK(startupState->registryValue.find(L"--background") != std::wstring::npos);

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    CHECK(viewModel.preferences.startupEnabled);
    CHECK(viewModel.preferences.startupStartInBackground);

    app.shutdown();
}

TEST_CASE("AppController keeps registry startup background state when rewrite fails", "[app][startup]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), true, true);

    auto startupState = std::make_shared<FakeStartupHooksState>();
    startupState->registryValue = L"\"C:\\Program Files\\JiStats\\JiStats.exe\"";
    startupState->failWrite = true;
    startupState->writeError = "Rewrite failed";

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setStartupManagerTestHooks(makeStartupHooks(startupState));

    REQUIRE(app.init());

    REQUIRE(startupState->writeAttempts.size() == 1);
    CHECK(startupState->registryValue == L"\"C:\\Program Files\\JiStats\\JiStats.exe\"");

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    CHECK(viewModel.preferences.startupEnabled);
    CHECK_FALSE(viewModel.preferences.startupStartInBackground);

    const DashboardPreferences storedPreferences = loadStartupPreferences(dbPath.path());
    CHECK(storedPreferences.startupEnabled);
    CHECK_FALSE(storedPreferences.startupStartInBackground);

    app.shutdown();
}

TEST_CASE("AppController preserves requested startup enable preference when enable fails", "[app][startup]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), true, true);

    auto startupState = std::make_shared<FakeStartupHooksState>();
    startupState->registryValue.clear();
    startupState->failWrite = true;
    startupState->writeError = "Enable failed";

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setStartupManagerTestHooks(makeStartupHooks(startupState));

    REQUIRE(app.init());

    REQUIRE(startupState->writeAttempts.size() == 1);
    CHECK(startupState->registryValue.empty());

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    CHECK(viewModel.preferences.startupEnabled);
    CHECK(viewModel.preferences.startupStartInBackground);

    const DashboardPreferences storedPreferences = loadStartupPreferences(dbPath.path());
    CHECK(storedPreferences.startupEnabled);
    CHECK(storedPreferences.startupStartInBackground);

    app.shutdown();
}

TEST_CASE("AppController preserves requested startup disable preference when disable fails", "[app][startup]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false);

    auto startupState = std::make_shared<FakeStartupHooksState>();
    startupState->registryValue = L"\"C:\\Program Files\\JiStats\\JiStats.exe\"";
    startupState->failWrite = true;
    startupState->writeError = "Disable failed";

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setStartupManagerTestHooks(makeStartupHooks(startupState));

    REQUIRE(app.init());

    REQUIRE(startupState->writeAttempts.size() == 1);
    CHECK(startupState->registryValue == L"\"C:\\Program Files\\JiStats\\JiStats.exe\"");

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    CHECK_FALSE(viewModel.preferences.startupEnabled);
    CHECK_FALSE(viewModel.preferences.startupStartInBackground);

    const DashboardPreferences storedPreferences = loadStartupPreferences(dbPath.path());
    CHECK_FALSE(storedPreferences.startupEnabled);
    CHECK_FALSE(storedPreferences.startupStartInBackground);

    app.shutdown();
}

TEST_CASE("AppController disables startup without a background rewrite when the Run entry is mismatched", "[app][startup]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, true);

    auto startupState = std::make_shared<FakeStartupHooksState>();
    startupState->registryValue = L"\"C:\\Program Files\\JiStats\\JiStats.exe\"";

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setStartupManagerTestHooks(makeStartupHooks(startupState));

    REQUIRE(app.init());

    REQUIRE(startupState->writeAttempts.size() == 1);
    CHECK(startupState->writeAttempts.front().empty());
    CHECK(startupState->registryValue.empty());

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    CHECK_FALSE(viewModel.preferences.startupEnabled);
    CHECK(viewModel.preferences.startupStartInBackground);

    const DashboardPreferences storedPreferences = loadStartupPreferences(dbPath.path());
    CHECK_FALSE(storedPreferences.startupEnabled);
    CHECK(storedPreferences.startupStartInBackground);

    app.shutdown();
}

TEST_CASE("AppController falls back to a visible dashboard when background tray init fails", "[app][tray]") {
    auto providerManager = std::make_shared<ProviderManager>();
    ScopedTrayTestHandler handler([](TrayController::TestOperation operation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string& error) -> std::optional<bool> {
        switch (operation) {
        case TrayController::TestOperation::CreateMessageWindow:
            return true;
        case TrayController::TestOperation::AddTrayIcon:
            error = "test seam tray init failure";
            return false;
        case TrayController::TestOperation::StopMessagePump:
            return true;
        default:
            return true;
        }
    });

    TempTestDatabase dbPath;
    AppController app;
    app.setDatabasePathForTesting(dbPath.path());
    app.setDisableTaskbarForTesting(true);
    app.setProviderManagerForTesting(providerManager);

    REQUIRE(app.init(true));
    CHECK(app.isDashboardVisibleForTesting());

    app.shutdown();
}

TEST_CASE("AppController tray show dashboard action does not toggle visibility off", "[app][tray]") {
    auto providerManager = std::make_shared<ProviderManager>();
    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

    TempTestDatabase dbPath;
    AppController app;
    app.setDatabasePathForTesting(dbPath.path());
    app.setDisableTaskbarForTesting(true);
    app.setProviderManagerForTesting(providerManager);

    REQUIRE(app.init(true));
    CHECK_FALSE(app.isDashboardVisibleForTesting());

    REQUIRE(app.dispatchTrayEventForTesting(TrayController::TestEvent::MenuShowDashboard));
    CHECK(app.isDashboardVisibleForTesting());

    REQUIRE(app.dispatchTrayEventForTesting(TrayController::TestEvent::MenuShowDashboard));
    CHECK(app.isDashboardVisibleForTesting());

    app.shutdown();
}

TEST_CASE("AppController tray navigation stays responsive during a runtime refresh", "[app][tray][navigation]") {
    auto providerManager = std::make_shared<ProviderManager>();
    ScopedTrayTestHandler handler([](TrayController::TestOperation, TrayStatus, const std::string&,
                                     std::string&) -> std::optional<bool> { return true; });

    TempTestDatabase dbPath;
    AppController app;
    app.setDatabasePathForTesting(dbPath.path());
    app.setDisableTaskbarForTesting(true);
    app.setProviderManagerForTesting(providerManager);
    REQUIRE(app.init(true));

    std::promise<void> reachedPublication;
    auto publicationReached = reachedPublication.get_future();
    std::promise<void> releasePublication;
    auto publicationReleased = releasePublication.get_future().share();
    app.setBeforeRuntimePublicationForTesting([&reachedPublication, publicationReleased]() {
        reachedPublication.set_value();
        publicationReleased.wait();
    });

    auto refresh = std::async(std::launch::async, [&app]() { app.refreshRuntimeForTesting(); });
    const bool refreshBlocked = publicationReached.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    auto openDashboard = std::async(std::launch::async, [&app]() {
        return app.dispatchTrayEventForTesting(TrayController::TestEvent::MenuShowDashboard);
    });
    const bool trayResponsive = openDashboard.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    const bool helpOpenedDuringRefresh =
        app.dispatchDashboardShellCommandForTesting(DashboardShellCommand::HelpViewHelp) &&
        waitForCondition([&app]() { return app.isDashboardShellDialogOpenForTesting(DashboardShellDialog::Help); });

    releasePublication.set_value();
    refresh.get();
    CHECK(openDashboard.get());
    CHECK(refreshBlocked);
    CHECK(trayResponsive);
    CHECK(helpOpenedDuringRefresh);
    CHECK(app.isDashboardVisibleForTesting());
    app.shutdown();
}

TEST_CASE("AppController dashboard shell menu opens settings help and about windows", "[app][shell]") {
    auto providerManager = std::make_shared<ProviderManager>();
    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

    TempTestDatabase dbPath;
    AppController app;
    app.setDatabasePathForTesting(dbPath.path());
    app.setDisableTaskbarForTesting(true);
    app.setProviderManagerForTesting(providerManager);

    REQUIRE(app.init());

    REQUIRE(app.dispatchDashboardShellCommandForTesting(DashboardShellCommand::FileSettings));
    REQUIRE(waitForCondition([&app]() {
        return app.isDashboardShellDialogOpenForTesting(DashboardShellDialog::Preferences);
    }));

    REQUIRE(app.dispatchDashboardShellCommandForTesting(DashboardShellCommand::HelpViewHelp));
    REQUIRE(waitForCondition([&app]() {
        return app.isDashboardShellDialogOpenForTesting(DashboardShellDialog::Help);
    }));

    REQUIRE(app.dispatchDashboardShellCommandForTesting(DashboardShellCommand::HelpAbout));
    REQUIRE(waitForCondition([&app]() {
        return app.isDashboardShellDialogOpenForTesting(DashboardShellDialog::About);
    }));

    app.shutdown();
}

TEST_CASE("AppController imports OpenAI export history from the dashboard shell", "[app][shell][openai]") {
    requireSqlite();

    TempTestDatabase dbPath;
    auto providerManager = std::make_shared<ProviderManager>();
    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

    AppController app;
    app.setDisableTaskbarForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setOpenAIImportPathForTesting(kOpenAIExportFixturePath.string());

    REQUIRE(app.init());
    REQUIRE(app.dispatchDashboardShellCommandForTesting(DashboardShellCommand::FileImportOpenAI));

    REQUIRE(waitForCondition([&app]() {
        const std::string status = app.getDashboardStatusTextForTesting();
        return status.find("OpenAI export import completed") != std::string::npos;
    }));

    StorageService storage;
    REQUIRE(storage.init(dbPath.path()));

    const QueryResult providerResult = storage.execute(
        "SELECT id FROM providers WHERE provider_key = ? LIMIT 1;",
        {"openai"});
    REQUIRE(providerResult.success());
    REQUIRE(providerResult.rowCount() == 1);
    const int providerId = providerResult.getInt(0, 0);

    const QueryResult accountResult = storage.execute(
        "SELECT id FROM accounts WHERE provider_id = ? AND account_key = ? LIMIT 1;",
        {
            std::to_string(providerId),
            "imported-history:openai_subscription_export_import"
        });
    REQUIRE(accountResult.success());
    REQUIRE(accountResult.rowCount() == 1);
    const int accountId = accountResult.getInt(0, 0);

    const auto syncState = storage.getSyncState(providerId, accountId, OpenAIExportImport::kSyncKind);
    REQUIRE(syncState.has_value());
    CHECK(syncState->cursorValue.find("\"artifact_name\":\"chatgpt-export-redacted\"") != std::string::npos);
    CHECK(syncState->cursorValue.find(OpenAIExportImport::kProviderSurface) != std::string::npos);
    CHECK(syncState->cursorValue.find("\"source_type\":\"chatgpt_export\"") != std::string::npos);

    const auto events = storage.getActivityEvents(providerId, accountId, 0, std::time(nullptr) + 1);
    REQUIRE(events.size() == 3);
    for (const auto& event : events) {
        CHECK(event.activity.providerSurface == OpenAIExportImport::kProviderSurface);
        CHECK(event.activity.sourceType == "imported");
    }

    storage.close();
    app.shutdown();
}

TEST_CASE("AppController About dialog reflects the current about-content contract", "[app][shell]") {
    auto providerManager = std::make_shared<ProviderManager>();
    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

    TempTestDatabase dbPath;
    AppController app;
    app.setDatabasePathForTesting(dbPath.path());
    app.setDisableTaskbarForTesting(true);
    app.setProviderManagerForTesting(providerManager);

    REQUIRE(app.init());
    REQUIRE(app.dispatchDashboardShellCommandForTesting(DashboardShellCommand::HelpAbout));
    REQUIRE(waitForCondition([&app]() {
        return app.isDashboardShellDialogOpenForTesting(DashboardShellDialog::About);
    }));

    const std::string aboutBody =
        app.getDashboardShellDialogBodyTextForTesting(DashboardShellDialog::About);
    CHECK(aboutBody.find("Version:") != std::string::npos);
    CHECK(aboutBody.find("Status: Early implementation") != std::string::npos);
    CHECK(aboutBody.find("Provider scope:") != std::string::npos);
    CHECK(aboutBody.find("OpenAI / Codex, MiniMax, Claude Code, Kimi Code, Grok, and Cursor") != std::string::npos);
    CHECK(aboutBody.find("All provider integrations are optional and disabled by default until enabled in Settings.") != std::string::npos);
    CHECK(aboutBody.find("OpenAI and MiniMax ship with subscription-level support in v1.0.") != std::string::npos);
    CHECK(aboutBody.find("OpenAI API-platform import and broader MiniMax PAYG support remain follow-on refinement.") != std::string::npos);
    CHECK(aboutBody.find("Claude Code and Kimi Code ship as bounded local-first providers for v1.0.") != std::string::npos);
    CHECK(aboutBody.find("License:") != std::string::npos);
    CHECK(aboutBody.find("Primary goal:") == std::string::npos);
    CHECK(aboutBody.find("Platform:") == std::string::npos);
    CHECK(aboutBody.find("Stack:") == std::string::npos);
    CHECK(aboutBody.find("Storage:") == std::string::npos);
    CHECK(aboutBody.find("Secret storage:") == std::string::npos);
    CHECK(aboutBody.find("Local docs:") == std::string::npos);
    CHECK(aboutBody.find("AGENTS.md") == std::string::npos);
    CHECK(aboutBody.find("docs/plan.md") == std::string::npos);
    CHECK(aboutBody.find("docs/progress.md") == std::string::npos);
    CHECK(aboutBody.find("LICENSE") != std::string::npos);
    CHECK(aboutBody.find("THIRD_PARTY_NOTICES.md") != std::string::npos);

    app.shutdown();
}

TEST_CASE("AppController dashboard File Exit command requests shutdown with shell dialogs open", "[app][shell]") {
    auto providerManager = std::make_shared<ProviderManager>();
    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

    TempTestDatabase dbPath;
    AppController app;
    app.setDatabasePathForTesting(dbPath.path());
    app.setDisableTaskbarForTesting(true);
    app.setProviderManagerForTesting(providerManager);

    REQUIRE(app.init());
    REQUIRE(app.dispatchDashboardShellCommandForTesting(DashboardShellCommand::HelpViewHelp));
    REQUIRE(waitForCondition([&app]() {
        return app.isDashboardShellDialogOpenForTesting(DashboardShellDialog::Help);
    }));
    REQUIRE(app.dispatchDashboardShellCommandForTesting(DashboardShellCommand::HelpAbout));
    REQUIRE(waitForCondition([&app]() {
        return app.isDashboardShellDialogOpenForTesting(DashboardShellDialog::About);
    }));

    auto runFuture = std::async(std::launch::async, [&app]() {
        return app.run();
    });

    REQUIRE(waitForCondition([&app]() {
        return app.getState() == "ui-active";
    }));

    auto exitFuture = std::async(std::launch::async, [&app]() {
        return app.dispatchDashboardShellCommandForTesting(DashboardShellCommand::FileExit);
    });

    CHECK(exitFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    REQUIRE(exitFuture.get());

    CHECK(runFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(runFuture.get() == 0);
    CHECK_FALSE(app.isDashboardShellDialogOpenForTesting(DashboardShellDialog::Help));
    CHECK_FALSE(app.isDashboardShellDialogOpenForTesting(DashboardShellDialog::About));

    app.shutdown();
}

TEST_CASE("AppController saves MiniMax credentials from the settings shell and persists only metadata", "[app][shell][minimax]") {
    TempTestDatabase dbPath;
    auto backend = makeFakeCredentialBackend();
    auto store = std::make_shared<WindowsCredentialStore>(backend);
    auto providerManager = std::make_shared<ProviderManager>();
    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

    AppController app;
    app.setDisableTaskbarForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setMiniMaxCredentialStoreForTesting(store);

    REQUIRE(app.init());
    REQUIRE(app.dispatchDashboardShellCommandForTesting(DashboardShellCommand::FileSettings));
    REQUIRE(waitForCondition([&app]() {
        return app.isDashboardShellDialogOpenForTesting(DashboardShellDialog::Preferences);
    }));

    DashboardMiniMaxCredentialRequest request;
    request.action = DashboardMiniMaxCredentialAction::Save;
    request.planKey = "coding_plan";
    request.accountDisplayName = "Work Coding Plan";
    request.secret = "sk-cp-shell-save";
    REQUIRE(app.submitMiniMaxCredentialActionForTesting(request));
    REQUIRE(waitForCondition([&app]() {
        const auto summary = findMiniMaxSummary(app.getDashboardViewModelForTesting(), "coding_plan");
        return summary.has_value() &&
            summary->hasPersistedProfile &&
            summary->accountDisplayName == "Work Coding Plan";
    }));

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    const auto summary = findMiniMaxSummary(viewModel, "coding_plan");
    REQUIRE(summary.has_value());
    CHECK(summary->validationState == "pending");
    CHECK(summary->accountDisplayName == "Work Coding Plan");
    CHECK(summary->secretRef == "JiStats/minimax/subscription/coding_plan/coding_plan.default");
    CHECK(summary->lastValidationError.empty());
    REQUIRE(backend->peek(summary->secretRef).has_value());
    CHECK(backend->peek(summary->secretRef).value() == "sk-cp-shell-save");

    if (sqliteAvailable()) {
        StorageService storage;
        REQUIRE(storage.init(dbPath.path()));
        const auto accountResult = storage.execute(
            "SELECT display_name FROM accounts WHERE account_key = ? LIMIT 1;",
            {"coding_plan.default"});
        REQUIRE(accountResult.success());
        REQUIRE(accountResult.rowCount() == 1);
        CHECK(accountResult.getString(0, 0) == "Work Coding Plan");

        const auto profileResult = storage.execute(
            "SELECT secret_ref, credential_type FROM credential_profiles WHERE profile_name = ? LIMIT 1;",
            {"MiniMax Coding Plan"});
        REQUIRE(profileResult.success());
        REQUIRE(profileResult.rowCount() == 1);
        CHECK(profileResult.getString(0, 0) == "JiStats/minimax/subscription/coding_plan/coding_plan.default");
        CHECK(profileResult.getString(0, 1) == "subscription_key");
        storage.close();
    }

    app.shutdown();
}

TEST_CASE("AppController updates an existing MiniMax credential profile from the settings shell", "[app][shell][minimax]") {
    TempTestDatabase dbPath;
    auto backend = makeFakeCredentialBackend();
    auto store = std::make_shared<WindowsCredentialStore>(backend);
    auto providerManager = std::make_shared<ProviderManager>();
    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

    AppController app;
    app.setDisableTaskbarForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setMiniMaxCredentialStoreForTesting(store);
    REQUIRE(app.init());
    REQUIRE(app.dispatchDashboardShellCommandForTesting(DashboardShellCommand::FileSettings));
    REQUIRE(waitForCondition([&app]() {
        return app.isDashboardShellDialogOpenForTesting(DashboardShellDialog::Preferences);
    }));

    DashboardMiniMaxCredentialRequest request;
    request.action = DashboardMiniMaxCredentialAction::Save;
    request.planKey = "coding_plan";
    request.accountDisplayName = "Work Coding Plan";
    request.secret = "sk-cp-shell-save";
    REQUIRE(app.submitMiniMaxCredentialActionForTesting(request));
    REQUIRE(waitForCondition([&app]() {
        const auto summary = findMiniMaxSummary(app.getDashboardViewModelForTesting(), "coding_plan");
        return summary.has_value() && summary->accountDisplayName == "Work Coding Plan";
    }, std::chrono::milliseconds(4000)));

    request.accountDisplayName = "Renamed Coding Plan";
    request.secret = "sk-cp-updated";
    REQUIRE(app.submitMiniMaxCredentialActionForTesting(request));
    REQUIRE(waitForCondition([&app]() {
        const auto summary = findMiniMaxSummary(app.getDashboardViewModelForTesting(), "coding_plan");
        return summary.has_value() && summary->accountDisplayName == "Renamed Coding Plan";
    }, std::chrono::milliseconds(4000)));

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    const auto summary = findMiniMaxSummary(viewModel, "coding_plan");
    REQUIRE(summary.has_value());
    CHECK(summary->validationState == "pending");
    REQUIRE(backend->peek(summary->secretRef).has_value());
    CHECK(backend->peek(summary->secretRef).value() == "sk-cp-updated");

    if (sqliteAvailable()) {
        StorageService storage;
        REQUIRE(storage.init(dbPath.path()));
        const auto accountResult = storage.execute(
            "SELECT display_name FROM accounts WHERE account_key = ? LIMIT 1;",
            {"coding_plan.default"});
        REQUIRE(accountResult.success());
        REQUIRE(accountResult.rowCount() == 1);
        CHECK(accountResult.getString(0, 0) == "Renamed Coding Plan");
        storage.close();
    }

    app.shutdown();
}

TEST_CASE("AppController surfaces MiniMax validation failures through the settings shell", "[app][shell][minimax]") {
    TempTestDatabase dbPath;
    auto backend = makeFakeCredentialBackend();
    auto store = std::make_shared<WindowsCredentialStore>(backend);
    auto providerManager = std::make_shared<ProviderManager>();
    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

    AppController app;
    app.setDisableTaskbarForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setMiniMaxCredentialStoreForTesting(store);
    app.setMiniMaxValidationResponseForTesting(false, 403, "", "forbidden");
    REQUIRE(app.init());
    REQUIRE(app.dispatchDashboardShellCommandForTesting(DashboardShellCommand::FileSettings));
    REQUIRE(waitForCondition([&app]() {
        return app.isDashboardShellDialogOpenForTesting(DashboardShellDialog::Preferences);
    }));

    DashboardMiniMaxCredentialRequest save;
    save.action = DashboardMiniMaxCredentialAction::Save;
    save.planKey = "token_plan";
    save.accountDisplayName = "Token Plan";
    save.secret = "sk-tp-shell-save";
    REQUIRE(app.submitMiniMaxCredentialActionForTesting(save));
    REQUIRE(waitForCondition([&app]() {
        const auto summary = findMiniMaxSummary(app.getDashboardViewModelForTesting(), "token_plan");
        return summary.has_value() && summary->hasPersistedProfile;
    }));

    DashboardMiniMaxCredentialRequest validate;
    validate.action = DashboardMiniMaxCredentialAction::Validate;
    validate.planKey = "token_plan";
    REQUIRE(app.submitMiniMaxCredentialActionForTesting(validate));
    REQUIRE(waitForCondition([&app]() {
        return app.getMiniMaxValidationStateForTesting("token_plan") ==
            std::optional<std::string>("invalid");
    }, std::chrono::milliseconds(4000)));

    app.refreshRuntimeForTesting();

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    const auto summary = findMiniMaxSummary(viewModel, "token_plan");
    REQUIRE(summary.has_value());
    CHECK(summary->validationState == "invalid");
    CHECK(summary->lastValidationError == "MiniMax subscription key rejected");
    CHECK(summary->lastValidatedAtLabel.find("UTC") != std::string::npos);

    app.shutdown();
}

TEST_CASE("AppController keeps MiniMax validation asynchronous and preserves the masked secret field", "[app][shell][minimax]") {
    requireSqlite();
    TempTestDatabase dbPath;
    auto backend = makeFakeCredentialBackend();
    auto store = std::make_shared<WindowsCredentialStore>(backend);
    auto providerManager = std::make_shared<ProviderManager>();
    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

    AppController app;
    std::promise<void> releaseValidation;
    app.setDisableTaskbarForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setMiniMaxCredentialStoreForTesting(store);
    app.setMiniMaxValidationResponseForTesting(true, 200, "{}", "");
    app.setMiniMaxValidationReleaseForTesting(releaseValidation.get_future().share());
    REQUIRE(app.init());
    REQUIRE(app.dispatchDashboardShellCommandForTesting(DashboardShellCommand::FileSettings));
    REQUIRE(waitForCondition([&app]() {
        return app.isDashboardShellDialogOpenForTesting(DashboardShellDialog::Preferences);
    }));

    DashboardMiniMaxCredentialRequest validate;
    validate.action = DashboardMiniMaxCredentialAction::Validate;
    validate.planKey = "coding_plan";
    validate.accountDisplayName = "Async Coding Plan";
    validate.secret = "sk-cp-async";

    REQUIRE(app.submitMiniMaxCredentialActionForTesting(validate));

    REQUIRE(waitForCondition([&app]() {
        const auto summary = findMiniMaxSummary(app.getDashboardViewModelForTesting(), "coding_plan");
        return summary.has_value() && summary->validationState == "pending";
    }, std::chrono::seconds(5)));
    CHECK(app.getMiniMaxSecretFieldTextForTesting() == "sk-cp-async");

    releaseValidation.set_value();
    REQUIRE(waitForCondition([&app]() {
        const auto summary = findMiniMaxSummary(app.getDashboardViewModelForTesting(), "coding_plan");
        return summary.has_value() && summary->validationState == "valid";
    }, std::chrono::seconds(5)));

    const auto summary = findMiniMaxSummary(app.getDashboardViewModelForTesting(), "coding_plan");
    REQUIRE(summary.has_value());
    CHECK(summary->accountDisplayName == "Async Coding Plan");
    CHECK(summary->lastValidationError.empty());
    CHECK(app.getMiniMaxSecretFieldTextForTesting() == "sk-cp-async");

    app.shutdown();
}

TEST_CASE("AppController reports missing MiniMax secrets from hydrated metadata in the settings shell", "[app][shell][minimax]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedMiniMaxCredentialMetadata(dbPath.path(), "coding_plan", "Hydrated Coding Plan");

    auto backend = makeFakeCredentialBackend();
    auto store = std::make_shared<WindowsCredentialStore>(backend);
    auto providerManager = std::make_shared<ProviderManager>();
    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

    AppController app;
    app.setDisableTaskbarForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setMiniMaxCredentialStoreForTesting(store);
    REQUIRE(app.init());

    const auto hydrated = findMiniMaxSummary(app.getDashboardViewModelForTesting(), "coding_plan");
    REQUIRE(hydrated.has_value());
    CHECK(hydrated->hasPersistedProfile);
    CHECK(hydrated->validationState == "unknown");
    CHECK(hydrated->accountDisplayName == "Hydrated Coding Plan");

    REQUIRE(app.dispatchDashboardShellCommandForTesting(DashboardShellCommand::FileSettings));
    REQUIRE(waitForCondition([&app]() {
        return app.isDashboardShellDialogOpenForTesting(DashboardShellDialog::Preferences);
    }));

    DashboardMiniMaxCredentialRequest validate;
    validate.action = DashboardMiniMaxCredentialAction::Validate;
    validate.planKey = "coding_plan";
    REQUIRE(app.submitMiniMaxCredentialActionForTesting(validate));
    REQUIRE(waitForCondition([&app]() {
        const auto summary = findMiniMaxSummary(app.getDashboardViewModelForTesting(), "coding_plan");
        return summary.has_value() && summary->validationState == "invalid";
    }));

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    const auto summary = findMiniMaxSummary(viewModel, "coding_plan");
    REQUIRE(summary.has_value());
    CHECK(summary->lastValidationError ==
        "Stored MiniMax credential is missing from Windows Credential Manager");

    app.shutdown();
}

TEST_CASE("AppController testing MiniMax action helper returns the direct fallback result", "[app][shell][minimax]") {
    TempTestDatabase dbPath;
    auto backend = makeFakeCredentialBackend();
    auto store = std::make_shared<WindowsCredentialStore>(backend);
    auto providerManager = std::make_shared<ProviderManager>();
    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

    AppController app;
    app.setDisableTaskbarForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setMiniMaxCredentialStoreForTesting(store);
    REQUIRE(app.init());

    DashboardMiniMaxCredentialRequest request;
    request.action = DashboardMiniMaxCredentialAction::Save;
    request.planKey = "coding_plan";
    request.accountDisplayName = "Fallback Save";
    request.secret.clear();

    CHECK_FALSE(app.submitMiniMaxCredentialActionForTesting(request));

    const auto summary = findMiniMaxSummary(app.getDashboardViewModelForTesting(), "coding_plan");
    REQUIRE(summary.has_value());
    CHECK(summary->validationState == "invalid");
    CHECK_FALSE(summary->lastValidationError.empty());

    app.shutdown();
}

TEST_CASE("AppController keeps dashboard, tray, taskbar, and offline notice in sync", "[app][runtime]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, true, true, false, false);
    const std::time_t now = std::time(nullptr);
    auto liveProvider = makeCurrentQuotaProvider("openai", "OpenAI", 1305, 1500, now, "monthly");
    auto offlineProvider = makeOfflineQuotaProvider("minimax", "MiniMax");
    auto providerManager = makeProviderManager({liveProvider, offlineProvider});

    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

    AppController app;
    app.setTaskbarTestModeForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);

    REQUIRE(app.init());

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    CHECK(viewModel.runtimeState == RuntimeState::Offline);
    CHECK_FALSE(viewModel.isOnline);
    CHECK(app.isDashboardOfflineNoticeVisibleForTesting());
    CHECK(app.getDashboardOfflineNoticeTextForTesting().find("Provider connectivity is unavailable") != std::string::npos);

    const auto trayState = app.getTrayStateForTesting();
    CHECK(trayState.status == TrayStatus::Offline);
    CHECK(trayState.tooltip.find("Offline") != std::string::npos);

    const auto offlineNotice = app.getActiveNoticeForTesting(NoticeType::Offline);
    REQUIRE(offlineNotice.has_value());
    CHECK(offlineNotice->title == NotificationService::getDefaultTitle(NoticeType::Offline));

    const std::vector<TaskbarMetrics> taskbarMetrics = app.getTaskbarMetricsForTesting();
    const TaskbarMetrics* openaiMetric = findTaskbarMetric(taskbarMetrics, "openai");
    REQUIRE(openaiMetric != nullptr);
    CHECK(openaiMetric->kind == TaskbarMetricKind::QuotaUsage);
    CHECK(findTaskbarMetric(taskbarMetrics, "minimax") == nullptr);

    StorageService storage;
    REQUIRE(storage.init(dbPath.path()));

    const int openaiProviderId = requireProviderId(storage, "openai");
    const int minimaxProviderId = requireProviderId(storage, "minimax");
    const int openaiRuntimeAccountId =
        requireAccountId(storage, openaiProviderId, "openai.runtime_status");
    const int minimaxRuntimeAccountId =
        requireAccountId(storage, minimaxProviderId, "minimax.runtime_status");

    const auto openaiConnectivity =
        storage.getConnectivityState(openaiProviderId, openaiRuntimeAccountId);
    REQUIRE(openaiConnectivity.has_value());
    CHECK(openaiConnectivity->status == "online");
    CHECK(openaiConnectivity->failureCategory.empty());
    CHECK(openaiConnectivity->nextRetryAt == 0);

    const auto minimaxConnectivity =
        storage.getConnectivityState(minimaxProviderId, minimaxRuntimeAccountId);
    REQUIRE(minimaxConnectivity.has_value());
    CHECK(minimaxConnectivity->status == "offline");
    CHECK(minimaxConnectivity->failureCategory == "provider_unreachable");
    CHECK(minimaxConnectivity->nextRetryAt > now);

    const auto openaiRefreshSync =
        storage.getSyncState(openaiProviderId, openaiRuntimeAccountId, "runtime_refresh");
    REQUIRE(openaiRefreshSync.has_value());
    CHECK(openaiRefreshSync->lastAttemptAt > 0);
    CHECK(openaiRefreshSync->lastSuccessAt > 0);
    CHECK(openaiRefreshSync->lastFailureAt == 0);
    CHECK(openaiRefreshSync->lastFailureCategory.empty());

    const auto minimaxRefreshSync =
        storage.getSyncState(minimaxProviderId, minimaxRuntimeAccountId, "runtime_refresh");
    REQUIRE(minimaxRefreshSync.has_value());
    CHECK(minimaxRefreshSync->lastAttemptAt > 0);
    CHECK(minimaxRefreshSync->lastSuccessAt == 0);
    CHECK(minimaxRefreshSync->lastFailureAt > 0);
    CHECK(minimaxRefreshSync->lastFailureCategory == "provider_unreachable");

    const auto runtimeState = storage.getRuntimeState();
    REQUIRE(runtimeState.has_value());
    CHECK(runtimeState->lastLaunchAt > 0);
    CHECK(runtimeState->lastSuccessfulSyncAt == 0);
    CHECK_FALSE(runtimeState->recoveryMarker.empty());

    storage.close();

    app.shutdown();
}

TEST_CASE("AppController dashboard dismiss and tray retry keep offline notice low-noise across recovery transitions", "[app][runtime][offline_notice]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedAllProviderTrackingEnabled(dbPath.path());
    const std::time_t now = std::time(nullptr);
    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

    AppController app;
    std::future<void> refresh;
    std::promise<void> releaseQuota;
    auto provider = std::make_shared<ControlledQuotaProviderAdapter>(
        "minimax", "MiniMax", makeFailedQuotaResponse("network unreachable"),
        releaseQuota.get_future().share(), 2);
    auto providerManager = std::make_shared<ProviderManager>();
    providerManager->addProvider(provider);
    app.setTaskbarTestModeForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);

    REQUIRE(app.init());
    REQUIRE(waitForCondition([&app]() {
        return app.isDashboardOfflineNoticeVisibleForTesting();
    }, std::chrono::milliseconds(4000)));

    // The initial fetch makes the notice visible. Hold the next refresh's response.
    refresh = std::async(std::launch::async, [&app]() { app.refreshRuntimeForTesting(); });
    REQUIRE(waitForCondition([&provider]() {
        return provider->getQuotaFetchCount() > 1;
    }, std::chrono::seconds(4)));
    REQUIRE(provider->getCompletedQuotaFetchCount() == 1);

    REQUIRE(app.clickDashboardOfflineDismissForTesting());
    REQUIRE(waitForCondition([&app]() {
        return !app.isDashboardOfflineNoticeVisibleForTesting() &&
            !app.getActiveNoticeForTesting(NoticeType::Offline).has_value();
    }, std::chrono::milliseconds(4000)));
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

    provider->setQuotaResponse(
        makeQuotaResponse(now, 120, 1000, "daily", "minimax_subscription_coding_plan"));
    REQUIRE(app.clickDashboardOfflineRetryForTesting());
    REQUIRE(waitForCondition([&app]() {
        const auto connectivity = app.getConnectivityStateForTesting("minimax");
        const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
        return connectivity.has_value() &&
            connectivity->status == "online" &&
            viewModel.runtimeState != RuntimeState::Offline &&
            !viewModel.offlineNoticeVisible &&
            !app.getActiveNoticeForTesting(NoticeType::Offline).has_value();
    }, std::chrono::milliseconds(4000)));

    const auto recoveredConnectivity = app.getConnectivityStateForTesting("minimax");
    REQUIRE(recoveredConnectivity.has_value());
    CHECK(recoveredConnectivity->status == "online");
    CHECK(recoveredConnectivity->noticeDismissedAt == 0);
    CHECK_FALSE(app.isDashboardOfflineNoticeVisibleForTesting());

    provider->setQuotaResponse(makeFailedQuotaResponse("offline"));
    app.refreshRuntimeForTesting();
    REQUIRE(waitForCondition([&app]() {
        return app.isDashboardOfflineNoticeVisibleForTesting() &&
            app.getActiveNoticeForTesting(NoticeType::Offline).has_value();
    }, std::chrono::milliseconds(4000)));

    const DashboardViewModel finalViewModel = app.getDashboardViewModelForTesting();
    CHECK(finalViewModel.runtimeState == RuntimeState::Offline);
    CHECK(finalViewModel.offlineRetryAvailable);
    CHECK(finalViewModel.offlineNoticeVisible);

    app.shutdown();
}

TEST_CASE("AppController persists dismissal when the quota check after catch-up is offline", "[app][runtime][offline_notice][recovery]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, false, true, false, false);
    const std::time_t now = std::time(nullptr);
    seedRuntimeState(dbPath.path(), now - 3 * 3600, now - 3 * 3600);
    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

    AppController app;
    std::promise<void> releaseQuota;
    auto provider = std::make_shared<ControlledQuotaProviderAdapter>(
        "minimax", "MiniMax", makeFailedQuotaResponse("network unreachable"),
        releaseQuota.get_future().share(), 3,
        makeQuotaResponse(now, 120, 1000, "daily", "minimax_subscription_coding_plan"));
    auto providerManager = std::make_shared<ProviderManager>();
    providerManager->addProvider(provider);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setDisableTaskbarForTesting(true);

    REQUIRE(app.init());
    // The first response allowed catch-up and the second showed the notice.
    // Hold the deferred startup refresh so it cannot replace the saved result.
    REQUIRE(waitForCondition([&provider]() {
        return provider->getQuotaFetchCount() == 3;
    }, std::chrono::seconds(4)));
    REQUIRE(provider->getCompletedQuotaFetchCount() == 2);
    REQUIRE(app.isDashboardOfflineNoticeVisibleForTesting());

    const auto connectivity = app.getConnectivityStateForTesting("minimax");
    REQUIRE(connectivity.has_value());
    CHECK(connectivity->status == "offline");
    CHECK(connectivity->noticeDismissedAt == 0);

    REQUIRE(app.clickDashboardOfflineDismissForTesting());
    REQUIRE(waitForCondition([&app]() {
        return !app.isDashboardOfflineNoticeVisibleForTesting();
    }, std::chrono::seconds(4)));
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

TEST_CASE("AppController keeps an offline dismissal across a provider handoff and restart", "[app][runtime][offline_notice]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, true, true, false, false);
    const std::time_t now = std::time(nullptr);
    auto openai = std::make_shared<ControlledQuotaProviderAdapter>(
        "openai", "OpenAI", makeQuotaResponse(now, 25, 100, "interval", "openai_subscription_codex_local"),
        std::shared_future<void>{}, 1, makeFailedQuotaResponse("network unreachable"));
    auto miniMax = std::make_shared<ControlledQuotaProviderAdapter>(
        "minimax", "MiniMax", makeFailedQuotaResponse("network unreachable"),
        std::shared_future<void>{}, 1,
        makeQuotaResponse(now, 120, 1000, "daily", "minimax_subscription_coding_plan"));
    auto providerManager = std::make_shared<ProviderManager>();
    providerManager->addProvider(openai);
    providerManager->addProvider(miniMax);
    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

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
    REQUIRE(waitForCondition([&app]() {
        return !app.isDashboardOfflineNoticeVisibleForTesting();
    }, std::chrono::seconds(5)));
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
        "openai", "OpenAI", makeFailedQuotaResponse("network unreachable"),
        std::shared_future<void>{}, 1,
        makeQuotaResponse(now, 25, 100, "interval", "openai_subscription_codex_local"));
    auto providerManager = std::make_shared<ProviderManager>();
    providerManager->addProvider(openai);
    providerManager->addProvider(miniMax);
    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

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
    REQUIRE(waitForCondition([&app]() {
        return !app.isDashboardOfflineNoticeVisibleForTesting();
    }, std::chrono::seconds(5)));
    const auto dismissed = app.getConnectivityStateForTesting("minimax");
    REQUIRE(dismissed.has_value());
    REQUIRE(dismissed->status == "offline");
    REQUIRE(dismissed->noticeDismissedAt > 0);

    {
        StorageService storage;
        REQUIRE(storage.init(dbPath.path()));
        const int openaiAccountId =
            requireAccountId(storage, requireProviderId(storage, "openai"), "openai.runtime_status");
        REQUIRE(storage.execute(
            "CREATE TRIGGER reject_openai_offline_row BEFORE INSERT ON connectivity_state "
            "WHEN NEW.status = 'offline' AND NEW.account_id = " + std::to_string(openaiAccountId) + " "
            "BEGIN SELECT RAISE(ABORT, 'test rejects the handoff write'); END;").success());
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

TEST_CASE("AppController keeps an offline dismissal when the newly offline provider has no saved row", "[app][runtime][offline_notice]") {
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
        std::shared_future<void>{}, 1,
        makeQuotaResponse(now, 120, 1000, "daily", "unregistered_quota"));
    auto providerManager = std::make_shared<ProviderManager>();
    providerManager->addProvider(openai);
    providerManager->addProvider(unregistered);
    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

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
    REQUIRE(waitForCondition([&app]() {
        return !app.isDashboardOfflineNoticeVisibleForTesting();
    }, std::chrono::seconds(5)));
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

TEST_CASE("AppController keeps an omitted provider's dismissal when a local provider recovers after restart", "[app][runtime][offline_notice]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, true, true, false, false);
    auto openai = std::make_shared<ControlledQuotaProviderAdapter>(
        "openai", "OpenAI", makeFailedQuotaResponse("network unreachable"));
    auto miniMax = makeOfflineQuotaProvider("minimax", "MiniMax");
    auto providerManager = std::make_shared<ProviderManager>();
    providerManager->addProvider(openai);
    providerManager->addProvider(miniMax);
    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

    AppController app;
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setDisableTaskbarForTesting(true);
    REQUIRE(app.init());
    REQUIRE(app.isDashboardOfflineNoticeVisibleForTesting());
    REQUIRE(app.clickDashboardOfflineDismissForTesting());
    REQUIRE(waitForCondition([&app]() {
        return !app.isDashboardOfflineNoticeVisibleForTesting();
    }, std::chrono::seconds(5)));

    auto preferences = app.getDashboardViewModelForTesting().preferences;
    preferences.miniMaxSubscriptionTrackingEnabled = false;
    app.applyDashboardPreferencesForTesting(preferences);
    REQUIRE(app.getDashboardViewModelForTesting().runtimeState == RuntimeState::Offline);
    const auto dismissed = app.getConnectivityStateForTesting("minimax");
    REQUIRE(dismissed.has_value());
    REQUIRE(dismissed->noticeDismissedAt > 0);
    app.shutdown();

    const std::time_t now = std::time(nullptr);
    seedPersistedQuotaSnapshot(
        dbPath.path(), "openai", "acct-openai-cached", "OpenAI Cached Account",
        "openai_subscription", "OpenAI Subscription", "interval", now - 60, 60, 100, 40);
    openai->setQuotaResponse(
        makeQuotaResponse(now, 25, 100, "interval", "openai_subscription_codex_local"));

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

TEST_CASE("AppController preserves saved offline dismissal through inconclusive quota checks", "[app][runtime][offline_notice][kimi]") {
    requireSqlite();

    TempTestDirectory profile("jistats_offline_profile");
    ScopedEnvironmentVariable userProfile("USERPROFILE", profile.path().string());
    // This Codex rollout predates the dismissal and does not change.
    const std::time_t rolloutTime = std::time(nullptr) - 30 * 60;
    writeCodexRolloutFile(
        profile.path() / ".codex" / "sessions" / "2026" / "04" / "19" / "rollout-existing.jsonl",
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
    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

    AppController app;
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setDisableTaskbarForTesting(true);
    REQUIRE(app.init());
    REQUIRE(app.isDashboardOfflineNoticeVisibleForTesting());
    REQUIRE(app.clickDashboardOfflineDismissForTesting());
    REQUIRE(waitForCondition([&app]() {
        return !app.isDashboardOfflineNoticeVisibleForTesting();
    }, std::chrono::seconds(5)));
    const auto dismissed = app.getConnectivityStateForTesting("kimi");
    REQUIRE(dismissed.has_value());
    REQUIRE(dismissed->noticeDismissedAt > 0);

    kimi->setQuotaResponse(makeFailedQuotaResponse("Kimi quota API credentials are not available"));
    app.refreshRuntimeForTesting();
    CHECK(app.getDashboardViewModelForTesting().runtimeState == RuntimeState::Unknown);
    const auto withoutCredentials = app.getConnectivityStateForTesting("kimi");
    REQUIRE(withoutCredentials.has_value());
    CHECK(withoutCredentials->noticeDismissedAt == dismissed->noticeDismissedAt);

    auto preferences = app.getDashboardViewModelForTesting().preferences;
    preferences.claudeCodeEnabled = true;
    preferences.openaiSubscriptionTrackingEnabled = true;
    app.applyDashboardPreferencesForTesting(preferences);
    const DashboardViewModel localOnly = app.getDashboardViewModelForTesting();
    CHECK(localOnly.runtimeState != RuntimeState::Offline);
    CHECK(findProviderStatus(localOnly, "kimi") == nullptr);
    const auto* localClaude = findProviderStatus(localOnly, "claude_code");
    REQUIRE(localClaude != nullptr);
    CHECK(localClaude->state == ProviderState::Live);
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

TEST_CASE("AppController keeps offline dismissal when remaining quota comes from the polling cache", "[app][runtime][offline_notice][polling]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, false, true, false, true);
    {
        StorageService storage;
        REQUIRE(storage.init(dbPath.path()));
        REQUIRE(ensureProviderId(storage, "kimi", "Kimi Code") > 0);
    }
    const std::time_t now = std::time(nullptr);
    auto miniMax = makeCurrentQuotaProvider(
        "minimax", "MiniMax", 120, 1000, now, "daily", "minimax_subscription_coding_plan");
    miniMax->setDirectQuotaApiConfigured(true, "minimax:coding_plan_remains");
    auto kimi = makeOfflineQuotaProvider("kimi", "Kimi Code");
    auto providerManager = makeProviderManager({miniMax, kimi});
    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

    AppController app;
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setDisableTaskbarForTesting(true);
    REQUIRE(app.init());
    REQUIRE(app.isDashboardOfflineNoticeVisibleForTesting());
    REQUIRE(miniMax->getQuotaFetchCount() == 1);
    REQUIRE(app.clickDashboardOfflineDismissForTesting());
    REQUIRE(waitForCondition([&app]() {
        return !app.isDashboardOfflineNoticeVisibleForTesting();
    }, std::chrono::seconds(5)));
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

TEST_CASE("AppController surfaces stale provider runtime state for taskbar validation", "[app][runtime]") {
    TempTestDatabase dbPath;
    const std::time_t now = std::time(nullptr);
    auto staleProvider = makeStaleQuotaProvider("openai", "OpenAI", 1305, 1500, now, "monthly");
    auto providerManager = makeProviderManager({staleProvider});

    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

    AppController app;
    app.setTaskbarTestModeForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);

    REQUIRE(app.init());
    enableAllProviderTracking(app);

    const DashboardViewModel initialViewModel = app.getDashboardViewModelForTesting();
    CHECK((
        initialViewModel.runtimeState == RuntimeState::CatchingUp ||
        initialViewModel.runtimeState == RuntimeState::Stale));

    REQUIRE(waitForCondition([&]() {
        const auto runtimeState = app.getPersistedRuntimeStateForTesting();
        if (!runtimeState.has_value() || !runtimeState->recoveryMarker.empty()) {
            return false;
        }

        const DashboardViewModel settledViewModel = app.getDashboardViewModelForTesting();
        return settledViewModel.runtimeState == RuntimeState::Stale;
    }, std::chrono::milliseconds(4000)));

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    CHECK(viewModel.runtimeState == RuntimeState::Stale);
    CHECK(viewModel.isOnline);

    const auto trayState = app.getTrayStateForTesting();
    CHECK(trayState.status == TrayStatus::Warning);
    CHECK(trayState.tooltip.find("Stale") != std::string::npos);
    CHECK_FALSE(app.getActiveNoticeForTesting(NoticeType::Offline).has_value());

    const std::vector<TaskbarMetrics> taskbarMetrics = app.getTaskbarMetricsForTesting();
    REQUIRE(taskbarMetrics.size() == 1);
    CHECK(taskbarMetrics.front().providerKey == "openai");
    CHECK(taskbarMetrics.front().statusIcon == "\xE2\x96\xB2");
    CHECK_FALSE(taskbarMetrics.front().primaryMetric.empty());

    app.shutdown();
}

TEST_CASE("AppController persists offline retry metadata and recovery markers for runtime refreshes", "[app][runtime][recovery]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedAllProviderTrackingEnabled(dbPath.path());
    auto offlineProvider = makeOfflineQuotaProvider("minimax", "MiniMax");
    auto providerManager = makeProviderManager({offlineProvider});

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);

    REQUIRE(app.init());

    const auto connectivity = app.getConnectivityStateForTesting("minimax");
    REQUIRE(connectivity.has_value());
    CHECK(connectivity->status == "offline");
    CHECK(connectivity->failureCategory == "provider_unreachable");
    CHECK(connectivity->nextRetryAt > connectivity->lastCheckedAt);

    const auto syncState = app.getSyncStateForTesting("minimax");
    REQUIRE(syncState.has_value());
    CHECK(syncState->lastAttemptAt > 0);
    CHECK(syncState->lastFailureAt > 0);
    CHECK(syncState->lastFailureCategory == "provider_unreachable");

    const auto runtimeState = app.getPersistedRuntimeStateForTesting();
    REQUIRE(runtimeState.has_value());
    CHECK(runtimeState->lastLaunchAt > 0);
    CHECK_FALSE(runtimeState->recoveryMarker.empty());

    app.shutdown();
}

TEST_CASE("AppController runs catch-up recovery and records runtime gap markers after a launch gap", "[app][runtime][recovery]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedAllProviderTrackingEnabled(dbPath.path());
    const std::time_t now = std::time(nullptr);
    seedRuntimeState(
        dbPath.path(),
        now - (3 * 3600),
        now - (3 * 3600));

    auto liveProvider = makeCurrentQuotaProvider("openai", "OpenAI", 25, 100, now);
    auto providerManager = makeProviderManager({liveProvider});

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);

    REQUIRE(app.init());

    const DashboardViewModel initialViewModel = app.getDashboardViewModelForTesting();
    const ProviderStatus* openaiStatus = findProviderStatus(initialViewModel, "openai");
    REQUIRE(openaiStatus != nullptr);
    CHECK((
        initialViewModel.runtimeState == RuntimeState::CatchingUp ||
        initialViewModel.runtimeState == RuntimeState::Current));
    if (initialViewModel.runtimeState == RuntimeState::CatchingUp) {
        CHECK((
            initialViewModel.recoveryStatusSummary == "Checking for missed history" ||
            initialViewModel.recoveryStatusSummary == "Unknown gaps in 1 provider"));
        CHECK(initialViewModel.recoveryStatusDetail.find("Current shared sync state: Catching Up") != std::string::npos);
        CHECK(openaiStatus->state == ProviderState::Syncing);
    } else {
        CHECK(openaiStatus->state == ProviderState::Live);
    }

    REQUIRE(waitForCondition([&]() {
        const auto runtimeState = app.getPersistedRuntimeStateForTesting();
        return runtimeState.has_value() && runtimeState->recoveryMarker.empty();
    }, std::chrono::milliseconds(1500)));

    const auto refreshedRuntimeState = app.getPersistedRuntimeStateForTesting();
    REQUIRE(refreshedRuntimeState.has_value());
    CHECK(refreshedRuntimeState->lastSuccessfulSyncAt > 0);
    CHECK(refreshedRuntimeState->recoveryMarker.empty());

    const DashboardViewModel settledViewModel = app.getDashboardViewModelForTesting();
    CHECK(settledViewModel.runtimeState == RuntimeState::Current);
    const ProviderStatus* settledOpenAIStatus = findProviderStatus(settledViewModel, "openai");
    REQUIRE(settledOpenAIStatus != nullptr);
    CHECK(settledOpenAIStatus->state == ProviderState::Live);

    const auto connectivity = app.getConnectivityStateForTesting("openai");
    REQUIRE(connectivity.has_value());
    CHECK(connectivity->status == "online");
    CHECK(connectivity->nextRetryAt == 0);

    const auto syncState = app.getSyncStateForTesting("openai");
    REQUIRE(syncState.has_value());
    CHECK(syncState->lastAttemptAt > 0);
    CHECK(syncState->lastSuccessAt > 0);
    CHECK(syncState->lastFailureCategory.empty());

    StorageService storage;
    REQUIRE(storage.init(dbPath.path()));
    const int openaiProviderId = requireProviderId(storage, "openai");
    const int openaiRuntimeAccountId =
        requireAccountId(storage, openaiProviderId, "openai.runtime_status");

    const auto backfillCheckpoint =
        storage.getLatestRecoveryCheckpoint(openaiProviderId, openaiRuntimeAccountId, "runtime_backfill");
    REQUIRE(backfillCheckpoint.has_value());
    CHECK(backfillCheckpoint->attemptAt > 0);
    CHECK(backfillCheckpoint->outcome == "no_data");
    CHECK(backfillCheckpoint->fetchedEventCount == 0);
    CHECK(backfillCheckpoint->comparisonResult == "no_local_evidence");
    CHECK(backfillCheckpoint->failureCategory.empty());
    storage.close();

    const auto gapMarkers = app.getGapMarkersForTesting(
        "openai",
        now - (4 * 3600),
        std::time(nullptr));
    REQUIRE(gapMarkers.size() == 1);
    CHECK(gapMarkers.front().classification == GapClassification::Unknown);
    CHECK(gapMarkers.front().sourceKind == "runtime_recovery");

    REQUIRE(waitForCondition([&app]() {
        return app.getDashboardViewModelForTesting().runtimeState == RuntimeState::Current;
    }, std::chrono::milliseconds(1500)));

    const DashboardViewModel recoveredViewModel = app.getDashboardViewModelForTesting();
    const ProviderStatus* recoveredOpenAI = findProviderStatus(recoveredViewModel, "openai");
    REQUIRE(recoveredOpenAI != nullptr);
    REQUIRE(recoveredOpenAI->quotas.size() == 1);
    CHECK(recoveredOpenAI->quotas.front().hasUnknownGaps);
    CHECK(recoveredViewModel.recoveryStatusSummary == "Unknown gaps in 1 provider");
    CHECK(
        recoveredViewModel.recoveryStatusDetail.find("Provider returned no recovery history") != std::string::npos);

    app.shutdown();
}

TEST_CASE("AppController persists imported recovery backfill checkpoints per provider", "[app][runtime][recovery]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedAllProviderTrackingEnabled(dbPath.path());
    const std::time_t now = std::time(nullptr);
    seedRuntimeState(
        dbPath.path(),
        now - (3 * 3600),
        now - (3 * 3600));

    auto liveProvider = makeCurrentQuotaProvider("openai", "OpenAI", 25, 100, now);
    liveProvider->setUsageResponse(makeBackfillUsageResponseWithOneEvent(now - 1800));
    auto providerManager = makeProviderManager({liveProvider});

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);

    REQUIRE(app.init());

    REQUIRE(waitForCondition([&]() {
        const auto runtimeState = app.getPersistedRuntimeStateForTesting();
        return runtimeState.has_value() && runtimeState->recoveryMarker.empty();
    }, std::chrono::milliseconds(1500)));

    StorageService storage;
    REQUIRE(storage.init(dbPath.path()));
    const int openaiProviderId = requireProviderId(storage, "openai");
    const int openaiRuntimeAccountId =
        requireAccountId(storage, openaiProviderId, "openai.runtime_status");

    const auto checkpoints = storage.getRecoveryCheckpoints(
        openaiProviderId,
        openaiRuntimeAccountId,
        "runtime_backfill",
        now - (4 * 3600),
        now + 60);
    REQUIRE(checkpoints.size() == 1);
    CHECK(checkpoints.front().attemptAt > 0);
    CHECK(checkpoints.front().outcome == "imported");
    CHECK(checkpoints.front().fetchedEventCount == 1);
    CHECK(checkpoints.front().comparisonResult == "no_local_evidence");
    CHECK(checkpoints.front().failureCategory.empty());

    const auto usageEvents = storage.getUsageEvents(
        openaiProviderId,
        openaiRuntimeAccountId,
        now - (4 * 3600),
        now);
    REQUIRE(usageEvents.size() == 1);
    CHECK(usageEvents.front().isBackfilled);
    CHECK(usageEvents.front().sourceType == "provider_backfill");
    storage.close();

    app.shutdown();
}

TEST_CASE("AppController persists no-mismatch recovery summaries when provider backfill matches local evidence", "[app][runtime][recovery]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedAllProviderTrackingEnabled(dbPath.path());
    const std::time_t now = std::time(nullptr);
    seedRuntimeState(
        dbPath.path(),
        now - (3 * 3600),
        now - (3 * 3600));
    seedRuntimeUsageEvent(
        dbPath.path(),
        "openai",
        "OpenAI",
        now - 1800,
        200,
        15);

    auto liveProvider = makeCurrentQuotaProvider("openai", "OpenAI", 25, 100, now);
    liveProvider->setUsageResponse(makeBackfillUsageResponseWithOneEvent(now - 1800));
    auto providerManager = makeProviderManager({liveProvider});

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);

    REQUIRE(app.init());

    REQUIRE(waitForCondition([&]() {
        const auto runtimeState = app.getPersistedRuntimeStateForTesting();
        return runtimeState.has_value() && runtimeState->recoveryMarker.empty();
    }, std::chrono::milliseconds(1500)));

    StorageService storage;
    REQUIRE(storage.init(dbPath.path()));
    const int openaiProviderId = requireProviderId(storage, "openai");
    const int openaiRuntimeAccountId =
        requireAccountId(storage, openaiProviderId, "openai.runtime_status");

    const auto backfillCheckpoint =
        storage.getLatestRecoveryCheckpoint(openaiProviderId, openaiRuntimeAccountId, "runtime_backfill");
    REQUIRE(backfillCheckpoint.has_value());
    CHECK(backfillCheckpoint->outcome == "imported");
    CHECK(backfillCheckpoint->comparisonResult == "matched_local_evidence");
    CHECK(backfillCheckpoint->localEventCount == 1);
    CHECK(backfillCheckpoint->localTotalTokens == 200);
    CHECK(backfillCheckpoint->importedTotalTokens == 200);
    storage.close();

    const auto gapMarkers = app.getGapMarkersForTesting(
        "openai",
        now - (4 * 3600),
        std::time(nullptr));
    REQUIRE(gapMarkers.size() == 1);
    CHECK(gapMarkers.front().classification == GapClassification::Reconstructed);
    CHECK(gapMarkers.front().basisSummary.find("matched local quantified evidence") != std::string::npos);

    app.shutdown();
}

TEST_CASE("AppController persists mismatch recovery summaries when provider backfill differs from local evidence", "[app][runtime][recovery]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedAllProviderTrackingEnabled(dbPath.path());
    const std::time_t now = std::time(nullptr);
    seedRuntimeState(
        dbPath.path(),
        now - (3 * 3600),
        now - (3 * 3600));
    seedRuntimeUsageEvent(
        dbPath.path(),
        "openai",
        "OpenAI",
        now - 1800,
        120,
        5);

    auto liveProvider = makeCurrentQuotaProvider("openai", "OpenAI", 25, 100, now);
    liveProvider->setUsageResponse(makeBackfillUsageResponseWithOneEvent(now - 1800));
    auto providerManager = makeProviderManager({liveProvider});

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);

    REQUIRE(app.init());

    REQUIRE(waitForCondition([&]() {
        const auto runtimeState = app.getPersistedRuntimeStateForTesting();
        return runtimeState.has_value() && runtimeState->recoveryMarker.empty();
    }, std::chrono::milliseconds(1500)));

    StorageService storage;
    REQUIRE(storage.init(dbPath.path()));
    const int openaiProviderId = requireProviderId(storage, "openai");
    const int openaiRuntimeAccountId =
        requireAccountId(storage, openaiProviderId, "openai.runtime_status");

    const auto backfillCheckpoint =
        storage.getLatestRecoveryCheckpoint(openaiProviderId, openaiRuntimeAccountId, "runtime_backfill");
    REQUIRE(backfillCheckpoint.has_value());
    CHECK(backfillCheckpoint->outcome == "imported");
    CHECK(backfillCheckpoint->comparisonResult == "mismatch");
    CHECK(backfillCheckpoint->localEventCount == 1);
    CHECK(backfillCheckpoint->localTotalTokens == 120);
    CHECK(backfillCheckpoint->importedTotalTokens == 200);
    storage.close();

    const auto gapMarkers = app.getGapMarkersForTesting(
        "openai",
        now - (4 * 3600),
        std::time(nullptr));
    REQUIRE(gapMarkers.size() == 1);
    CHECK(gapMarkers.front().classification == GapClassification::Reconstructed);
    CHECK(gapMarkers.front().basisSummary.find("differs from local quantified evidence") != std::string::npos);

    app.shutdown();
}

TEST_CASE("AppController persists failed recovery backfill checkpoints and leaves the gap marked unknown", "[app][runtime][recovery]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedAllProviderTrackingEnabled(dbPath.path());
    const std::time_t now = std::time(nullptr);
    seedRuntimeState(
        dbPath.path(),
        now - (3 * 3600),
        now - (3 * 3600));

    auto liveProvider = makeCurrentQuotaProvider("openai", "OpenAI", 25, 100, now);
    UsageResponse failedBackfill;
    failedBackfill.success = false;
    failedBackfill.error = "network unreachable";
    liveProvider->setUsageResponse(failedBackfill);
    class TestKimiQuota : public KimiAdapter {
        QuotaResponse fetchQuota() override {
            return makeQuotaResponse(std::time(nullptr), 25, 100, "interval", "kimi_subscription_api");
        }
    };
    class TestSubscriptionQuota : public SubscriptionQuotaAdapter {
      public:
        using SubscriptionQuotaAdapter::SubscriptionQuotaAdapter;

        QuotaResponse fetchQuota() override {
            return makeQuotaResponse(std::time(nullptr), 25, 100, "weekly", getProviderKey() + "_subscription_api");
        }
    };
    auto providerManager = makeProviderManager({liveProvider});
    providerManager->addProvider(std::make_shared<TestKimiQuota>());
    providerManager->addProvider(std::make_shared<TestSubscriptionQuota>(SubscriptionProvider::Grok));
    providerManager->addProvider(std::make_shared<TestSubscriptionQuota>(SubscriptionProvider::Cursor));
    {
        StorageService storage;
        REQUIRE(storage.init(dbPath.path()));
        SettingsService settings(storage);
        REQUIRE(settings.setBool("runtime.grok_subscription_tracking_enabled", true));
        REQUIRE(settings.setBool("runtime.cursor_subscription_tracking_enabled", true));
    }

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);

    REQUIRE(app.init());

    StorageService storage;
    REQUIRE(storage.init(dbPath.path()));
    const int openaiProviderId = requireProviderId(storage, "openai");
    const int openaiRuntimeAccountId =
        requireAccountId(storage, openaiProviderId, "openai.runtime_status");

    const auto backfillCheckpoint =
        storage.getLatestRecoveryCheckpoint(openaiProviderId, openaiRuntimeAccountId, "runtime_backfill");
    REQUIRE(backfillCheckpoint.has_value());
    CHECK(backfillCheckpoint->attemptAt > 0);
    CHECK(backfillCheckpoint->outcome == "fetch_failed");
    CHECK(backfillCheckpoint->fetchedEventCount == 0);
    CHECK(backfillCheckpoint->comparisonResult == "not_available");
    CHECK(backfillCheckpoint->failureCategory == "provider_backfill_fetch_failed");
    for (const auto* key : {"kimi", "grok", "cursor"}) {
        const auto providerId = requireProviderId(storage, key);
        const auto account = requireAccountId(storage, providerId, std::string(key) + ".runtime_status");
        CHECK_FALSE(storage.getLatestRecoveryCheckpoint(providerId, account, "runtime_backfill").has_value());
        const auto gaps = app.getGapMarkersForTesting(key, now - 4 * 3600, std::time(nullptr));
        REQUIRE(gaps.size() == 1);
        CHECK(gaps.front().classification == GapClassification::Unknown);
    }
    storage.close();

    const auto runtimeState = app.getPersistedRuntimeStateForTesting();
    REQUIRE(runtimeState.has_value());
    CHECK(runtimeState->recoveryMarker.empty());

    const auto gapMarkers = app.getGapMarkersForTesting(
        "openai",
        now - (4 * 3600),
        std::time(nullptr));
    REQUIRE(gapMarkers.size() == 1);
    CHECK(gapMarkers.front().classification == GapClassification::Unknown);
    CHECK(gapMarkers.front().sourceKind == "runtime_recovery");

    app.shutdown();
}

TEST_CASE("AppController handles long-gap mixed provider recovery outcomes without duplicating imported backfill on repeat runs", "[app][runtime][recovery]") {
    requireSqlite();

    const std::time_t providerNow = std::time(nullptr);
    const auto makeMixedOutcomeProviderManager = [](std::time_t now) {
        auto openaiProvider = makeCurrentQuotaProvider("openai", "OpenAI", 25, 100, now);
        openaiProvider->setUsageResponse(makeBackfillUsageResponseWithOneEvent(now - 1800));

        auto minimaxProvider = makeCurrentQuotaProvider(
            "minimax",
            "MiniMax",
            300,
            1000,
            now,
            "interval",
            "minimax_subscription_coding_plan");

        auto kimiProvider = makeCurrentQuotaProvider("kimi", "Kimi Code", 10, 100, now);
        UsageResponse failedBackfill;
        failedBackfill.success = false;
        failedBackfill.error = "temporary upstream failure";
        kimiProvider->setUsageResponse(failedBackfill);

        return makeProviderManager({openaiProvider, minimaxProvider, kimiProvider});
    };

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, true, true, false, true);
    const std::time_t recoveryStart = std::time(nullptr) - (5 * 24 * 3600);
    seedRuntimeState(
        dbPath.path(),
        recoveryStart,
        recoveryStart);
    {
        StorageService storage;
        REQUIRE(storage.init(dbPath.path()));
        (void)ensureRuntimeAccountId(storage, "kimi", "Kimi Code");
        storage.close();
    }

    {
        AppController app;
        app.setHeadlessForTesting(true);
        app.setDatabasePathForTesting(dbPath.path());
        app.setProviderManagerForTesting(makeMixedOutcomeProviderManager(providerNow));

        REQUIRE(app.init());

        REQUIRE(waitForCondition([&]() {
            const auto runtimeState = app.getPersistedRuntimeStateForTesting();
            return runtimeState.has_value() && runtimeState->recoveryMarker.empty();
        }, std::chrono::milliseconds(1500)));

        StorageService storage;
        REQUIRE(storage.init(dbPath.path()));

        const int openaiProviderId = requireProviderId(storage, "openai");
        const int openaiRuntimeAccountId =
            requireAccountId(storage, openaiProviderId, "openai.runtime_status");
        const auto openaiBackfill =
            storage.getLatestRecoveryCheckpoint(openaiProviderId, openaiRuntimeAccountId, "runtime_backfill");
        REQUIRE(openaiBackfill.has_value());
        CHECK(openaiBackfill->outcome == "imported");
        CHECK(openaiBackfill->comparisonResult == "no_local_evidence");

        const int minimaxProviderId = requireProviderId(storage, "minimax");
        const int minimaxRuntimeAccountId =
            requireAccountId(storage, minimaxProviderId, "minimax.runtime_status");
        const auto minimaxBackfill =
            storage.getLatestRecoveryCheckpoint(minimaxProviderId, minimaxRuntimeAccountId, "runtime_backfill");
        REQUIRE(minimaxBackfill.has_value());
        CHECK(minimaxBackfill->outcome == "no_data");
        CHECK(minimaxBackfill->comparisonResult == "no_local_evidence");

        const int kimiProviderId = requireProviderId(storage, "kimi");
        const int kimiRuntimeAccountId =
            requireAccountId(storage, kimiProviderId, "kimi.runtime_status");
        const auto kimiBackfill =
            storage.getLatestRecoveryCheckpoint(kimiProviderId, kimiRuntimeAccountId, "runtime_backfill");
        REQUIRE(kimiBackfill.has_value());
        CHECK(kimiBackfill->outcome == "fetch_failed");
        CHECK(kimiBackfill->comparisonResult == "not_available");

        const auto openaiUsageEvents = storage.getUsageEvents(
            openaiProviderId,
            openaiRuntimeAccountId,
            recoveryStart,
            std::time(nullptr));
        REQUIRE(openaiUsageEvents.size() == 1);
        CHECK(openaiUsageEvents.front().isBackfilled);
        storage.close();

        app.shutdown();
    }

    seedRuntimeState(
        dbPath.path(),
        recoveryStart,
        recoveryStart);

    {
        AppController repeatedApp;
        repeatedApp.setHeadlessForTesting(true);
        repeatedApp.setDatabasePathForTesting(dbPath.path());
        repeatedApp.setProviderManagerForTesting(makeMixedOutcomeProviderManager(providerNow));

        REQUIRE(repeatedApp.init());

        REQUIRE(waitForCondition([&]() {
            const auto runtimeState = repeatedApp.getPersistedRuntimeStateForTesting();
            return runtimeState.has_value() && runtimeState->recoveryMarker.empty();
        }, std::chrono::milliseconds(1500)));

        StorageService storage;
        REQUIRE(storage.init(dbPath.path()));
        const int openaiProviderId = requireProviderId(storage, "openai");
        const int openaiRuntimeAccountId =
            requireAccountId(storage, openaiProviderId, "openai.runtime_status");
        const auto openaiUsageEvents = storage.getUsageEvents(
            openaiProviderId,
            openaiRuntimeAccountId,
            recoveryStart,
            std::time(nullptr));
        CHECK(openaiUsageEvents.size() == 1);

        const auto openaiGapMarkers = repeatedApp.getGapMarkersForTesting(
            "openai",
            recoveryStart,
            std::time(nullptr));
        CHECK(std::any_of(
            openaiGapMarkers.begin(),
            openaiGapMarkers.end(),
            [](const PersistedGapMarker& marker) {
                return marker.classification == GapClassification::Reconstructed;
            }));

        const auto minimaxGapMarkers = repeatedApp.getGapMarkersForTesting(
            "minimax",
            recoveryStart,
            std::time(nullptr));
        CHECK(std::any_of(
            minimaxGapMarkers.begin(),
            minimaxGapMarkers.end(),
            [](const PersistedGapMarker& marker) {
                return marker.classification == GapClassification::Unknown;
            }));

        const auto kimiGapMarkers = repeatedApp.getGapMarkersForTesting(
            "kimi",
            recoveryStart,
            std::time(nullptr));
        CHECK(std::any_of(
            kimiGapMarkers.begin(),
            kimiGapMarkers.end(),
            [](const PersistedGapMarker& marker) {
                return marker.classification == GapClassification::Unknown;
            }));

        storage.close();
        repeatedApp.shutdown();
    }
}

TEST_CASE("AppController refreshes OpenAI Codex quotas when the local rollout snapshot changes",
          "[app][runtime][openai][codex]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedAllProviderTrackingEnabled(dbPath.path());
    TempTestDirectory userProfileRoot("jistats_codex_refresh");
    const auto rolloutPath =
        userProfileRoot.path() / ".codex" / "sessions" / "2026" / "04" / "19" / "rollout-live.jsonl";
    const std::time_t now = std::time(nullptr);

    writeCodexRolloutFile(rolloutPath, 25.0, now, now + (5 * 3600));
    ScopedEnvironmentVariable codexHome(
        "CODEX_HOME",
        (userProfileRoot.path() / ".codex").string());

    auto openaiProvider = makeCurrentQuotaProvider(
        "openai",
        "OpenAI",
        25,
        100,
        now,
        "interval",
        "openai_subscription_codex_local");
    auto providerManager = makeProviderManager({openaiProvider});

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDisableTaskbarForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setOpenAICodexRefreshPollIntervalForTesting(25);

    REQUIRE(app.init());
    const int initialQuotaFetchCount = openaiProvider->getQuotaFetchCount();

    const ProviderStatus* initialOpenAI =
        findProviderStatus(app.getDashboardViewModelForTesting(), "openai");
    REQUIRE(initialOpenAI != nullptr);

    writeCodexRolloutFile(rolloutPath, 40.0, now + 60, now + (5 * 3600));
    std::error_code updateTimestampError;
    std::filesystem::last_write_time(
        rolloutPath,
        std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2),
        updateTimestampError);
    REQUIRE_FALSE(updateTimestampError);
    openaiProvider->setQuotaResponse(
        makeQuotaResponse(now + 60, 40, 100, "interval", "openai_subscription_codex_local"));

    REQUIRE(waitForCondition([&openaiProvider, initialQuotaFetchCount]() {
        return openaiProvider->getQuotaFetchCount() > initialQuotaFetchCount;
    }, std::chrono::milliseconds(1500)));

    app.shutdown();
}

TEST_CASE("AppController applies taskbar overlay preference toggles without leaving stale taskbar metrics", "[app][runtime]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedAllProviderTrackingEnabled(dbPath.path());
    const std::time_t now = std::time(nullptr);
    auto liveProvider = makeCurrentQuotaProvider("openai", "OpenAI", 1305, 1500, now, "monthly");
    auto providerManager = makeProviderManager({liveProvider});

    AppController app;
    app.setTaskbarTestModeForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);

    REQUIRE(app.init());

    DashboardPreferences preferences = app.getDashboardViewModelForTesting().preferences;
    REQUIRE(preferences.taskbarOverlayEnabled);
    REQUIRE_FALSE(app.getTaskbarMetricsForTesting().empty());

    preferences.taskbarOverlayEnabled = false;
    app.applyDashboardPreferencesForTesting(preferences);

    const DashboardViewModel disabledViewModel = app.getDashboardViewModelForTesting();
    CHECK_FALSE(disabledViewModel.preferences.taskbarOverlayEnabled);
    CHECK(app.getTaskbarMetricsForTesting().empty());

    preferences.taskbarOverlayEnabled = true;
    app.applyDashboardPreferencesForTesting(preferences);

    const DashboardViewModel reenabledViewModel = app.getDashboardViewModelForTesting();
    CHECK(reenabledViewModel.preferences.taskbarOverlayEnabled);
    const std::vector<TaskbarMetrics> taskbarMetrics = app.getTaskbarMetricsForTesting();
    REQUIRE_FALSE(taskbarMetrics.empty());
    CHECK(taskbarMetrics.front().providerKey == "openai");

    app.shutdown();
}

TEST_CASE("AppController labels Codex-local quota rows without generic API wording", "[app][runtime]") {
    const std::time_t now = std::time(nullptr);
    auto codexProvider = makeCurrentQuotaProvider(
        "openai",
        "OpenAI",
        25,
        100,
        now,
        "interval",
        "openai_subscription_codex_local");
    auto providerManager = makeProviderManager({codexProvider});

    TempTestDatabase dbPath;
    AppController app;
    app.setDatabasePathForTesting(dbPath.path());
    app.setHeadlessForTesting(true);
    app.setProviderManagerForTesting(providerManager);
    REQUIRE(app.init());
    enableAllProviderTracking(app);

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    REQUIRE(viewModel.providers.size() == 1);
    REQUIRE(viewModel.providers.front().quotas.size() == 1);
    CHECK(viewModel.providers.front().quotas.front().planName == "Subscription");
    CHECK(viewModel.providers.front().quotas.front().providerSurface == "openai_subscription_codex_local");

    app.shutdown();
}

TEST_CASE("AppController keeps OpenAI API-platform imported quota rows distinct from subscription wording", "[app][runtime]") {
    const std::time_t now = std::time(nullptr);
    auto apiImportProvider = makeCurrentQuotaProvider(
        "openai",
        "OpenAI",
        25,
        100,
        now,
        "daily",
        "openai_api_import");
    auto providerManager = makeProviderManager({apiImportProvider});

    TempTestDatabase dbPath;
    AppController app;
    app.setDatabasePathForTesting(dbPath.path());
    app.setHeadlessForTesting(true);
    app.setProviderManagerForTesting(providerManager);
    REQUIRE(app.init());
    enableAllProviderTracking(app);

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    REQUIRE(viewModel.providers.size() == 1);
    REQUIRE(viewModel.providers.front().quotas.size() == 1);
    CHECK(viewModel.providers.front().quotas.front().planName == "API Platform Import");
    CHECK(viewModel.providers.front().quotas.front().providerSurface == "openai_api_import");

    app.shutdown();
}

TEST_CASE("AppController suppresses Kimi local-only quota snapshots and relies on activity surfaces instead", "[app][runtime][kimi]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedAllProviderTrackingEnabled(dbPath.path());
    const std::time_t now = std::time(nullptr);
    QuotaResponse kimiQuota = makeQuotaResponse(
        now,
        42,
        0,
        "interval",
        "kimi_code_local");
    kimiQuota.secondaryWindowType = "weekly";
    kimiQuota.secondaryWindowEnd = now;
    kimiQuota.secondaryUsed = 84;
    auto kimiProvider = makeQuotaProvider("kimi", "Kimi Code", kimiQuota, now);
    auto providerManager = makeProviderManager({kimiProvider});

    AppController app;
    app.setHeadlessForTesting(true);
    app.setTaskbarTestModeForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    REQUIRE(app.init());

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    REQUIRE(viewModel.providers.size() == 1);
    const ProviderStatus* kimiStatus = findProviderStatus(viewModel, "kimi");
    REQUIRE(kimiStatus != nullptr);
    CHECK(kimiStatus->state == ProviderState::Sample);
    CHECK(kimiStatus->quotas.empty());

    const std::vector<TaskbarMetrics> metrics = app.getTaskbarMetricsForTesting();
    CHECK(findTaskbarMetric(metrics, "kimi") == nullptr);

    app.shutdown();
}

TEST_CASE("AppController surfaces Kimi provider-exact quota windows as remaining-first taskbar metrics", "[app][runtime][kimi]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedAllProviderTrackingEnabled(dbPath.path());
    const std::time_t now = std::time(nullptr);

    QuotaResponse kimiQuota = makeQuotaResponse(
        now,
        64,
        100,
        "interval",
        "kimi_subscription_api");
    kimiQuota.secondaryWindowType = "weekly";
    kimiQuota.secondaryWindowEnd = now;
    kimiQuota.secondaryUsed = 28;
    kimiQuota.secondaryLimit = 100;
    kimiQuota.secondaryRemaining = 72;
    auto kimiProvider = makeQuotaProvider("kimi", "Kimi Code", kimiQuota, now);
    auto providerManager = makeProviderManager({kimiProvider});

    AppController app;
    app.setHeadlessForTesting(true);
    app.setTaskbarTestModeForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    REQUIRE(app.init());

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    const ProviderStatus* kimiStatus = findProviderStatus(viewModel, "kimi");
    REQUIRE(kimiStatus != nullptr);
    REQUIRE(kimiStatus->quotas.size() == 2);
    CHECK_FALSE(kimiStatus->lastSyncTime.empty());
    CHECK(kimiStatus->quotas.front().planName == "Subscription");
    CHECK(kimiStatus->quotas.front().providerSurface == "kimi_subscription_api");
    CHECK(kimiStatus->quotas.front().used == 64);
    CHECK(kimiStatus->quotas.front().remaining == 36);
    CHECK(kimiStatus->quotas.front().secondaryRemainingPercentage == Catch::Approx(72.0));
    CHECK(kimiStatus->quotas[1].windowType == "weekly");
    CHECK(kimiStatus->quotas[1].used == 28);
    CHECK(kimiStatus->quotas[1].remaining == 72);

    const auto kimiSummary =
        findUsageSummary(app.getDashboardUsageProgressSummariesForTesting(), "Kimi Subscription");
    REQUIRE(kimiSummary.has_value());
    CHECK(kimiSummary->detail.find("36% remaining") != std::string::npos);
    CHECK(kimiSummary->detail.find("Weekly 72% remaining") != std::string::npos);

    const std::vector<TaskbarMetrics> metrics = app.getTaskbarMetricsForTesting();
    const TaskbarMetrics* kimiMetric = findTaskbarMetric(metrics, "kimi");
    REQUIRE(kimiMetric != nullptr);
    CHECK(kimiMetric->providerSurface == "kimi_subscription_api");
    CHECK(kimiMetric->primaryMetric == "36%");
    CHECK(kimiMetric->secondaryPrimaryMetric == "72%");
    CHECK(kimiMetric->secondaryMetric == "36 left");

    app.shutdown();
}

TEST_CASE("AppController throttles direct quota APIs per API surface with adaptive polling windows", "[app][runtime][polling]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, false, true, false, true);

    const std::time_t baseNow = std::time(nullptr);
    auto minimaxProvider = makeCurrentQuotaProvider(
        "minimax",
        "MiniMax",
        300,
        1000,
        baseNow,
        "interval",
        "minimax_subscription_coding_plan");
    minimaxProvider->setDirectQuotaApiConfigured(true, "minimax:coding_plan_remains");

    QuotaResponse kimiQuota = makeQuotaResponse(
        baseNow,
        64,
        100,
        "interval",
        "kimi_subscription_api");
    kimiQuota.secondaryWindowType = "weekly";
    kimiQuota.secondaryWindowEnd = baseNow;
    kimiQuota.secondaryUsed = 28;
    kimiQuota.secondaryLimit = 100;
    kimiQuota.secondaryRemaining = 72;
    auto kimiProvider = makeQuotaProvider("kimi", "Kimi Code", kimiQuota, baseNow);
    kimiProvider->setDirectQuotaApiConfigured(true, "kimi:/usages");

    auto providerManager = makeProviderManager({minimaxProvider, kimiProvider});

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDisableTaskbarForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    REQUIRE(app.init());

    const std::time_t base = std::time(nullptr);
    const int initialMiniMaxFetches = minimaxProvider->getQuotaFetchCount();
    const int initialKimiFetches = kimiProvider->getQuotaFetchCount();
    REQUIRE(initialMiniMaxFetches >= 1);
    REQUIRE(initialKimiFetches >= 1);

    app.setCurrentTimeForTesting(base + 60);
    app.refreshRuntimeForTesting();
    CHECK(minimaxProvider->getQuotaFetchCount() == initialMiniMaxFetches);
    CHECK(kimiProvider->getQuotaFetchCount() == initialKimiFetches);

    app.setCurrentTimeForTesting(base + 301);
    app.refreshRuntimeForTesting();
    CHECK(minimaxProvider->getQuotaFetchCount() == initialMiniMaxFetches + 1);
    CHECK(kimiProvider->getQuotaFetchCount() == initialKimiFetches + 1);

    app.setCurrentTimeForTesting(base + 1801);
    app.refreshRuntimeForTesting();
    CHECK(minimaxProvider->getQuotaFetchCount() == initialMiniMaxFetches + 1);
    CHECK(kimiProvider->getQuotaFetchCount() == initialKimiFetches + 1);

    app.setCurrentTimeForTesting(base + 2105);
    app.refreshRuntimeForTesting();
    CHECK(minimaxProvider->getQuotaFetchCount() == initialMiniMaxFetches + 2);
    CHECK(kimiProvider->getQuotaFetchCount() == initialKimiFetches + 2);

    QuotaResponse changedKimiQuota = kimiQuota;
    changedKimiQuota.snapshotTime = base + 3906;
    changedKimiQuota.windowEnd = base + 3906;
    changedKimiQuota.windowStart = changedKimiQuota.windowEnd - kDefaultQuotaWindowSeconds;
    changedKimiQuota.used = 65;
    changedKimiQuota.remaining = 35;
    kimiProvider->setQuotaResponse(changedKimiQuota);

    app.setCurrentTimeForTesting(base + 3906);
    app.refreshRuntimeForTesting();
    CHECK(minimaxProvider->getQuotaFetchCount() == initialMiniMaxFetches + 3);
    CHECK(kimiProvider->getQuotaFetchCount() == initialKimiFetches + 3);

    app.setCurrentTimeForTesting(base + 4207);
    app.refreshRuntimeForTesting();
    CHECK(minimaxProvider->getQuotaFetchCount() == initialMiniMaxFetches + 3);
    CHECK(kimiProvider->getQuotaFetchCount() == initialKimiFetches + 4);

    app.clearCurrentTimeForTesting();
    app.shutdown();
}

TEST_CASE("AppController local-provider watcher does not drag MiniMax direct API quota fetches onto the local refresh cadence", "[app][runtime][polling]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, false, true, false, true);

    const std::time_t now = std::time(nullptr);
    auto minimaxProvider = makeCurrentQuotaProvider(
        "minimax",
        "MiniMax",
        300,
        1000,
        now,
        "interval",
        "minimax_subscription_coding_plan");
    minimaxProvider->setDirectQuotaApiConfigured(true, "minimax:coding_plan_remains");

    auto providerManager = makeProviderManager({minimaxProvider});

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDisableTaskbarForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setLocalProviderRefreshPollIntervalForTesting(50);
    REQUIRE(app.init());

    const int initialFetchCount = minimaxProvider->getQuotaFetchCount();
    REQUIRE(initialFetchCount >= 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    CHECK(minimaxProvider->getQuotaFetchCount() == initialFetchCount);

    app.shutdown();
}

TEST_CASE("AppController local-provider watcher stays idle until Claude or Kimi local artifacts actually change", "[app][runtime][polling]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, false, false, false, true);
    TempTestDirectory homeRoot("jistats_kimi_quiet_watch_home");
    TempTestDirectory emptyUserRoot("jistats_kimi_quiet_watch_empty_user");
    writeInstalledButIdleKimiFixture(homeRoot.path());

    ScopedEnvironmentVariable kimiHome("KIMI_HOME", homeRoot.path().string());
    ScopedEnvironmentVariable userProfile("USERPROFILE", emptyUserRoot.path().string());
    ScopedEnvironmentVariable home("HOME", emptyUserRoot.path().string());

    const std::time_t now = std::time(nullptr);
    auto kimiProvider = makeCurrentQuotaProvider(
        "kimi",
        "Kimi Code",
        64,
        100,
        now,
        "interval",
        "kimi_subscription_api");
    auto providerManager = makeProviderManager({kimiProvider});

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDisableTaskbarForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setLocalProviderRefreshPollIntervalForTesting(50);
    REQUIRE(app.init());

    const int initialFetchCount = kimiProvider->getQuotaFetchCount();
    REQUIRE(initialFetchCount >= 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    CHECK(kimiProvider->getQuotaFetchCount() == initialFetchCount);

    app.shutdown();
}

#ifdef JISTATS_ENABLE_TEST_HOOKS
TEST_CASE("AppController background watchers switch to a dormant interval when their lane is inactive", "[app][runtime][polling]") {
    const auto dormantInterval = JiStats::dormantRefreshWatcherIntervalForTesting();

    CHECK(
        JiStats::adaptiveWatcherIntervalOrDormantForTesting(false, 2000, 30000, 0) ==
        dormantInterval);
    CHECK(
        JiStats::adaptiveWatcherIntervalOrDormantForTesting(false, 15000, 60000, 9) ==
        dormantInterval);

    CHECK(
        JiStats::adaptiveWatcherIntervalOrDormantForTesting(true, 2000, 30000, 0) ==
        std::chrono::milliseconds(2000));
    CHECK(
        JiStats::adaptiveWatcherIntervalOrDormantForTesting(true, 2000, 30000, 3) ==
        std::chrono::milliseconds(4000));
}
#endif

TEST_CASE("AppController imports Kimi local runtime snapshots into persisted runtime summaries", "[app][runtime][kimi]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedAllProviderTrackingEnabled(dbPath.path());
    TempTestDirectory homeRoot("jistats_kimi_home");
    TempTestDirectory emptyUserRoot("jistats_kimi_empty_user");
    const std::time_t now = std::time(nullptr);
    writeRecentKimiFixture(homeRoot.path(), now);

    ScopedEnvironmentVariable kimiHome("KIMI_HOME", homeRoot.path().string());
    ScopedEnvironmentVariable userProfile("USERPROFILE", emptyUserRoot.path().string());
    ScopedEnvironmentVariable home("HOME", emptyUserRoot.path().string());

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    REQUIRE(app.init());

    const auto persisted = app.getPersistedActivitiesForTesting(
        "kimi",
        "kimi_local_runtime",
        now - 300,
        now + 60);
    REQUIRE_FALSE(persisted.empty());
    CHECK(std::all_of(
        persisted.begin(),
        persisted.end(),
        [](const PersistedActivityEvent& event) {
            return event.activity.providerKey == "kimi" &&
                   event.activity.providerSurface == "kimi_local_runtime" &&
                   event.activity.sourceType == "local";
        }));

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    REQUIRE_FALSE(viewModel.runtimeSummaries.empty());
    CHECK(std::any_of(
        viewModel.runtimeSummaries.begin(),
        viewModel.runtimeSummaries.end(),
        [](const DashboardSummaryItem& item) {
            return item.badge == "Local" && item.percentage > 0;
        }));

    app.shutdown();
}

TEST_CASE("AppController detects installed Kimi without surfacing a false offline state when no runtime snapshots exist", "[app][runtime][kimi]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, false, false, false, true);
    TempTestDirectory homeRoot("jistats_kimi_idle_home");
    TempTestDirectory emptyUserRoot("jistats_kimi_idle_empty_user");
    writeInstalledButIdleKimiFixture(homeRoot.path());

    ScopedEnvironmentVariable kimiHome("KIMI_HOME", homeRoot.path().string());
    ScopedEnvironmentVariable userProfile("USERPROFILE", emptyUserRoot.path().string());
    ScopedEnvironmentVariable home("HOME", emptyUserRoot.path().string());

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    REQUIRE(app.init());

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    CHECK(viewModel.kimiCodeLocallyDetected);
    CHECK(viewModel.kimiCodeDetectionMessage.find("Detected at ~/.kimi") != std::string::npos);
    CHECK(findProviderStatus(viewModel, "kimi") == nullptr);
    CHECK(viewModel.runtimeState != RuntimeState::Offline);
    CHECK(viewModel.isOnline);
    CHECK_FALSE(viewModel.offlineNoticeVisible);

    app.shutdown();
}

TEST_CASE("AppController enabling Kimi after startup imports nested local runtime activity and shows live Kimi data", "[app][runtime][kimi]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, false, false, false, false);
    TempTestDirectory homeRoot("jistats_kimi_nested_home");
    TempTestDirectory emptyUserRoot("jistats_kimi_nested_empty_user");
    const std::time_t now = std::time(nullptr);
    writeNestedRecentKimiFixture(homeRoot.path(), now);

    ScopedEnvironmentVariable kimiHome("KIMI_HOME", homeRoot.path().string());
    ScopedEnvironmentVariable userProfile("USERPROFILE", emptyUserRoot.path().string());
    ScopedEnvironmentVariable home("HOME", emptyUserRoot.path().string());

    AppController app;
    app.setHeadlessForTesting(true);
    app.setTaskbarTestModeForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    auto providerManager = std::make_shared<ProviderManager>();
    providerManager->addProvider(std::make_shared<KimiAdapter>(homeRoot.path().string()));
    app.setProviderManagerForTesting(providerManager);
    REQUIRE(app.init());

    DashboardViewModel initialViewModel = app.getDashboardViewModelForTesting();
    CHECK_FALSE(initialViewModel.preferences.kimiCodeEnabled);
    CHECK(findProviderStatus(initialViewModel, "kimi") == nullptr);

    DashboardPreferences preferences = initialViewModel.preferences;
    preferences.kimiCodeEnabled = true;
    app.applyDashboardPreferencesForTesting(preferences);

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    CHECK(viewModel.preferences.kimiCodeEnabled);
    CHECK(viewModel.kimiCodeLocallyDetected);
    CHECK(findProviderStatus(viewModel, "kimi") == nullptr);

    const auto persisted = app.getPersistedActivitiesForTesting(
        "kimi",
        "kimi_local_runtime",
        now - 600,
        now + 60);
    REQUIRE_FALSE(persisted.empty());

    const std::vector<TaskbarMetrics> metrics = app.getTaskbarMetricsForTesting();
    CHECK(findTaskbarMetric(metrics, "kimi") == nullptr);

    app.shutdown();
}

TEST_CASE("AppController local-provider watcher refreshes Kimi activity after local files change", "[app][runtime][kimi]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, false, false, false, true);
    TempTestDirectory homeRoot("jistats_kimi_watch_home");
    TempTestDirectory emptyUserRoot("jistats_kimi_watch_empty_user");
    writeInstalledButIdleKimiFixture(homeRoot.path());

    ScopedEnvironmentVariable kimiHome("KIMI_HOME", homeRoot.path().string());
    ScopedEnvironmentVariable userProfile("USERPROFILE", emptyUserRoot.path().string());
    ScopedEnvironmentVariable home("HOME", emptyUserRoot.path().string());

    AppController app;
    app.setHeadlessForTesting(true);
    app.setTaskbarTestModeForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setLocalProviderRefreshPollIntervalForTesting(50);
    auto providerManager = std::make_shared<ProviderManager>();
    providerManager->addProvider(std::make_shared<KimiAdapter>(homeRoot.path().string()));
    app.setProviderManagerForTesting(providerManager);
    REQUIRE(app.init());

    const DashboardViewModel initialViewModel = app.getDashboardViewModelForTesting();
    CHECK(initialViewModel.kimiCodeLocallyDetected);
    CHECK(findProviderStatus(initialViewModel, "kimi") == nullptr);

    const std::time_t now = std::time(nullptr);
    writeNestedRecentKimiFixture(homeRoot.path(), now);

    REQUIRE(waitForCondition([&app, now]() {
        const auto persisted = app.getPersistedActivitiesForTesting(
            "kimi",
            "kimi_local_runtime",
            now - 600,
            now + 60);
        return !persisted.empty();
    }, std::chrono::milliseconds(2500)));

    const DashboardViewModel refreshedViewModel = app.getDashboardViewModelForTesting();
    CHECK(findProviderStatus(refreshedViewModel, "kimi") == nullptr);

    const std::vector<TaskbarMetrics> metrics = app.getTaskbarMetricsForTesting();
    CHECK(findTaskbarMetric(metrics, "kimi") == nullptr);

    app.shutdown();
}

TEST_CASE("AppController normalizes quota fields and dashboard summaries to remaining semantics", "[app][runtime]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedAllProviderTrackingEnabled(dbPath.path());
    const std::time_t now = std::time(nullptr);
    auto openaiProvider = makeCurrentQuotaProvider(
        "openai",
        "OpenAI",
        25,
        100,
        now,
        "interval",
        "openai_subscription_codex_local");
    auto minimaxProvider = makeCurrentQuotaProvider(
        "minimax",
        "MiniMax",
        42000,
        100000,
        now,
        "interval",
        "minimax_subscription_coding_plan");
    auto providerManager = makeProviderManager({openaiProvider, minimaxProvider});

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    REQUIRE(app.init());

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    REQUIRE(viewModel.providers.size() == 2);

    const ProviderStatus* openaiStatus = findProviderStatus(viewModel, "openai");
    REQUIRE(openaiStatus != nullptr);
    REQUIRE(openaiStatus->quotas.size() == 1);
    const auto& openaiQuota = openaiStatus->quotas.front();
    CHECK(openaiQuota.planName == "Subscription");
    CHECK(openaiQuota.used == 25);
    CHECK(openaiQuota.remaining == 75);
    CHECK(openaiQuota.usedPercentage == Catch::Approx(25.0));
    CHECK(openaiQuota.remainingPercentage == Catch::Approx(75.0));

    const ProviderStatus* minimaxStatus = findProviderStatus(viewModel, "minimax");
    REQUIRE(minimaxStatus != nullptr);
    REQUIRE(minimaxStatus->quotas.size() == 1);
    const auto& minimaxQuota = minimaxStatus->quotas.front();
    CHECK(minimaxQuota.planName == "Coding Plan");
    CHECK(minimaxQuota.used == 42000);
    CHECK(minimaxQuota.remaining == 58000);
    CHECK(minimaxQuota.usedPercentage == Catch::Approx(42.0));
    CHECK(minimaxQuota.remainingPercentage == Catch::Approx(58.0));

    const std::vector<DashboardSummaryItem> summaries =
        app.getDashboardUsageProgressSummariesForTesting();
    const auto openaiSummary = findUsageSummary(summaries, "OpenAI Subscription");
    REQUIRE(openaiSummary.has_value());
    CHECK(openaiSummary->detail.find("75% remaining") != std::string::npos);
    CHECK(openaiSummary->detail.find("75 of 100 left") == std::string::npos);
    CHECK(openaiSummary->percentage == 75);

    const auto minimaxSummary = findUsageSummary(summaries, "MiniMax - Coding Plan");
    REQUIRE(minimaxSummary.has_value());
    CHECK(minimaxSummary->detail.find("58% remaining") != std::string::npos);
    CHECK(minimaxSummary->detail.find("58,000 of 100,000 prompts left") != std::string::npos);
    CHECK(minimaxSummary->percentage == 58);

    app.shutdown();
}

TEST_CASE("AppController startup hydration shows persisted quota state while live refresh is blocked", "[app][runtime]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, true, false, false, false);
    const std::time_t snapshotTime = std::time(nullptr) - 300;
    seedPersistedQuotaSnapshot(
        dbPath.path(),
        "openai",
        "acct-openai-startup",
        "OpenAI Startup Account",
        "openai_subscription",
        "OpenAI Subscription",
        "interval",
        snapshotTime,
        60,
        100,
        40);

    AppController app;
    std::promise<void> releaseQuota;
    auto provider = std::make_shared<ControlledQuotaProviderAdapter>(
        "openai",
        "OpenAI",
        makeQuotaResponse(std::time(nullptr), 25, 100, "interval", "openai_subscription_codex_local"),
        releaseQuota.get_future().share());
    auto providerManager = std::make_shared<ProviderManager>();
    providerManager->addProvider(provider);

    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);

    REQUIRE(app.init());
    REQUIRE(waitForCondition([&provider]() {
        return provider->getQuotaFetchCount() > 0;
    }, std::chrono::seconds(5)));
    REQUIRE(provider->getCompletedQuotaFetchCount() == 0);

    const DashboardViewModel hydratedViewModel = app.getDashboardViewModelForTesting();
    REQUIRE(hydratedViewModel.providers.size() == 1);
    REQUIRE(hydratedViewModel.providers.front().quotas.size() == 1);
    CHECK(hydratedViewModel.providers.front().quotas.front().planName == "Subscription");
    CHECK(hydratedViewModel.providers.front().quotas.front().used == 60);
    CHECK(hydratedViewModel.providers.front().quotas.front().remaining == 40);

    const auto hydratedSummary =
        findUsageSummary(app.getDashboardUsageProgressSummariesForTesting(), "OpenAI Subscription");
    REQUIRE(hydratedSummary.has_value());
    CHECK(hydratedSummary->detail.find("40% remaining") != std::string::npos);

    releaseQuota.set_value();
    REQUIRE(waitForCondition([&app]() {
        const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
        const ProviderStatus* openaiStatus = findProviderStatus(viewModel, "openai");
        return openaiStatus != nullptr &&
               !openaiStatus->quotas.empty() &&
               openaiStatus->quotas.front().remaining == 75;
    }, std::chrono::seconds(5)));

    const DashboardViewModel refreshedViewModel = app.getDashboardViewModelForTesting();
    const ProviderStatus* refreshedOpenAI = findProviderStatus(refreshedViewModel, "openai");
    REQUIRE(refreshedOpenAI != nullptr);
    REQUIRE(refreshedOpenAI->quotas.size() == 1);
    CHECK(refreshedOpenAI->quotas.front().used == 25);
    CHECK(refreshedOpenAI->quotas.front().remaining == 75);
    CHECK(provider->getCompletedQuotaFetchCount() >= 1);

    app.shutdown();
}

TEST_CASE("AppController uses persisted MiniMax quota at startup until an explicit refresh", "[app][runtime]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, false, true, false, false);
    const std::time_t snapshotTime = std::time(nullptr) - 180;
    seedPersistedQuotaSnapshot(
        dbPath.path(),
        "minimax",
        "acct-minimax-startup",
        "MiniMax Startup Account",
        "coding_plan",
        "Coding Plan",
        "interval",
        snapshotTime,
        420,
        1000,
        580);

    QuotaResponse liveQuota = makeQuotaResponse(
        std::time(nullptr),
        150,
        1000,
        "interval",
        "minimax_subscription_coding_plan");
    liveQuota.remaining = 850;
    auto provider = std::make_shared<ControlledQuotaProviderAdapter>("minimax", "MiniMax", liveQuota);
    auto providerManager = std::make_shared<ProviderManager>();
    providerManager->addProvider(provider);

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);

    REQUIRE(app.init());
    CHECK(provider->getQuotaFetchCount() == 0);

    const DashboardViewModel hydratedViewModel = app.getDashboardViewModelForTesting();
    REQUIRE(hydratedViewModel.providers.size() == 1);
    REQUIRE(hydratedViewModel.providers.front().quotas.size() == 1);
    CHECK(hydratedViewModel.providers.front().quotas.front().used == 420);
    CHECK(hydratedViewModel.providers.front().quotas.front().remaining == 580);

    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    CHECK(provider->getQuotaFetchCount() == 0);

    app.refreshRuntimeForTesting();
    REQUIRE(waitForCondition([&app]() {
        const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
        const ProviderStatus* minimaxStatus = findProviderStatus(viewModel, "minimax");
        return minimaxStatus != nullptr &&
               !minimaxStatus->quotas.empty() &&
               minimaxStatus->quotas.front().remaining == 850;
    }, std::chrono::milliseconds(1500)));

    app.shutdown();
}

TEST_CASE("AppController defers provider discovery until Settings opens after hydrated startup", "[app][shell][startup]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, true, false, true, false);
    const std::time_t snapshotTime = std::time(nullptr) - 180;
    seedPersistedQuotaSnapshot(
        dbPath.path(),
        "openai",
        "acct-openai-startup",
        "OpenAI Startup Account",
        "openai_subscription",
        "OpenAI Subscription",
        "interval",
        snapshotTime,
        60,
        100,
        40);

    auto claudeProvider = std::make_shared<CountingDetectionProviderAdapter>(
        "claude_code",
        "Claude Code",
        true);
    auto providerManager = std::make_shared<ProviderManager>();
    providerManager->addProvider(claudeProvider);

    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

    AppController app;
    app.setDisableTaskbarForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);

    REQUIRE(app.init());
    CHECK(claudeProvider->getValidateCredentialsCount() == 0);

    REQUIRE(app.dispatchDashboardShellCommandForTesting(DashboardShellCommand::FileSettings));
    REQUIRE(waitForCondition([&app]() {
        return app.isDashboardShellDialogOpenForTesting(DashboardShellDialog::Preferences);
    }));
    REQUIRE(waitForCondition([&claudeProvider]() {
        return claudeProvider->getValidateCredentialsCount() >= 1;
    }, std::chrono::milliseconds(1500)));

    app.shutdown();
}

TEST_CASE("AppController uses quantified usage rollups for dashboard fallback and tray tooltip", "[app][runtime][rollups]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedAllProviderTrackingEnabled(dbPath.path());
    const std::time_t now = utcDayStartForTest(std::time(nullptr) - 12 * 3600) + 12 * 3600;
    const std::time_t rollupFrom = now - (6 * 60 * 60);

    StorageService storage;
    REQUIRE(storage.init(dbPath.path()));

    const auto openaiIds = seedUsageProviderAccount(
        storage,
        "openai",
        "acct-openai-rollup",
        "OpenAI Rollup Account");
    const auto minimaxIds = seedUsageProviderAccount(
        storage,
        "minimax",
        "acct-minimax-rollup",
        "MiniMax Rollup Account");

    insertUsageEvent(
        storage,
        openaiIds.providerId,
        openaiIds.accountId,
        toUtcIso(now - (70 * 60)),
        "gpt-4o",
        200,
        20);
    insertUsageEvent(
        storage,
        openaiIds.providerId,
        openaiIds.accountId,
        toUtcIso(now - (12 * 60)),
        "gpt-4o",
        240,
        24);
    insertUsageEvent(
        storage,
        openaiIds.providerId,
        openaiIds.accountId,
        toUtcIso(now - (2 * 60)),
        "gpt-4o",
        120,
        12);
    insertUsageEvent(
        storage,
        minimaxIds.providerId,
        minimaxIds.accountId,
        toUtcIso(now - (20 * 60)),
        "MiniMax-M1",
        500,
        50);

    REQUIRE(storage.rebuildUsageRollups(
        UsageRollupResolution::Hourly,
        openaiIds.providerId,
        openaiIds.accountId,
        rollupFrom,
        now));
    REQUIRE(storage.rebuildUsageRollups(
        UsageRollupResolution::Hourly,
        minimaxIds.providerId,
        minimaxIds.accountId,
        rollupFrom,
        now));
    storage.close();

    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

    AppController app;
    app.setTaskbarTestModeForTesting(true);
    app.setCurrentTimeForTesting(now);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(std::make_shared<ProviderManager>());

    REQUIRE(app.init());

    const std::vector<DashboardSummaryItem> summaries =
        app.getDashboardUsageProgressSummariesForTesting();
    REQUIRE(summaries.size() == 4);

    const auto lastFiveMinutes = findUsageSummary(summaries, "Last 5 Minutes");
    REQUIRE(lastFiveMinutes.has_value());
    CHECK(lastFiveMinutes->detail == "1 request, 120 tokens");
    CHECK(lastFiveMinutes->percentage == 25);

    const auto lastThirtyMinutes = findUsageSummary(summaries, "Last 30 Minutes");
    REQUIRE(lastThirtyMinutes.has_value());
    CHECK(lastThirtyMinutes->detail == "3 requests, 860 tokens across 2 providers");
    CHECK(lastThirtyMinutes->percentage == 75);

    const auto lastFiveHours = findUsageSummary(summaries, "Last 5 Hours");
    REQUIRE(lastFiveHours.has_value());
    CHECK(lastFiveHours->detail == "4 requests, 1060 tokens across 2 providers");
    CHECK(lastFiveHours->percentage == 100);

    const auto today = findUsageSummary(summaries, "Today");
    REQUIRE(today.has_value());
    CHECK(today->detail == "4 requests, 1060 tokens across 2 providers");

    const auto trayState = app.getTrayStateForTesting();
    CHECK(trayState.tooltip.find("3 req / 860 tok in 30m") != std::string::npos);

    app.shutdown();
}

TEST_CASE("AppController keeps raw quantified usage for providers missing hourly rollups", "[app][runtime][rollups]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedAllProviderTrackingEnabled(dbPath.path());
    const std::time_t now = std::time(nullptr);
    const std::time_t rollupFrom = now - (6 * 60 * 60);

    StorageService storage;
    REQUIRE(storage.init(dbPath.path()));

    const auto openaiIds = seedUsageProviderAccount(
        storage,
        "openai",
        "acct-openai-partial-rollup",
        "OpenAI Partial Rollup Account");
    const auto minimaxIds = seedUsageProviderAccount(
        storage,
        "minimax",
        "acct-minimax-partial-rollup",
        "MiniMax Partial Rollup Account");

    insertUsageEvent(
        storage,
        openaiIds.providerId,
        openaiIds.accountId,
        toUtcIso(now - (3 * 60 * 60)),
        "gpt-4o",
        180,
        18);
    insertUsageEvent(
        storage,
        minimaxIds.providerId,
        minimaxIds.accountId,
        toUtcIso(now - (2 * 60 * 60)),
        "MiniMax-M1",
        270,
        27);

    REQUIRE(storage.rebuildUsageRollups(
        UsageRollupResolution::Hourly,
        openaiIds.providerId,
        openaiIds.accountId,
        rollupFrom,
        now));
    storage.close();

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(std::make_shared<ProviderManager>());

    REQUIRE(app.init());

    const auto lastFiveHours =
        findUsageSummary(app.getDashboardUsageProgressSummariesForTesting(), "Last 5 Hours");
    REQUIRE(lastFiveHours.has_value());
    CHECK(lastFiveHours->detail == "2 requests, 450 tokens across 2 providers");

    app.shutdown();
}

TEST_CASE("AppController counts providers from hourly rollups after raw usage retention", "[app][runtime][rollups]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedAllProviderTrackingEnabled(dbPath.path());
    const std::time_t now = std::time(nullptr);
    const std::time_t rollupFrom = now - (6 * 60 * 60);

    StorageService storage;
    REQUIRE(storage.init(dbPath.path()));

    const auto openaiIds = seedUsageProviderAccount(
        storage,
        "openai",
        "acct-openai-retained-rollup",
        "OpenAI Retained Rollup Account");
    const auto minimaxIds = seedUsageProviderAccount(
        storage,
        "minimax",
        "acct-minimax-retained-rollup",
        "MiniMax Retained Rollup Account");

    insertUsageEvent(
        storage,
        openaiIds.providerId,
        openaiIds.accountId,
        toUtcIso(now - (4 * 60 * 60)),
        "gpt-4o",
        160,
        16);
    insertUsageEvent(
        storage,
        minimaxIds.providerId,
        minimaxIds.accountId,
        toUtcIso(now - (2 * 60 * 60)),
        "MiniMax-M1",
        290,
        29);

    REQUIRE(storage.rebuildUsageRollups(
        UsageRollupResolution::Hourly,
        openaiIds.providerId,
        openaiIds.accountId,
        rollupFrom,
        now));
    REQUIRE(storage.rebuildUsageRollups(
        UsageRollupResolution::Hourly,
        minimaxIds.providerId,
        minimaxIds.accountId,
        rollupFrom,
        now));
    REQUIRE(storage.execute("DELETE FROM usage_events;").success());
    storage.close();

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(std::make_shared<ProviderManager>());

    REQUIRE(app.init());

    const auto lastFiveHours =
        findUsageSummary(app.getDashboardUsageProgressSummariesForTesting(), "Last 5 Hours");
    REQUIRE(lastFiveHours.has_value());
    CHECK(lastFiveHours->detail == "2 requests, 450 tokens across 2 providers");

    app.shutdown();
}

TEST_CASE("AppController keeps taskbar overlay on subscription quota when Codex local usage exists", "[app][runtime][taskbar][codex]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedAllProviderTrackingEnabled(dbPath.path());
    const std::time_t now = std::time(nullptr);

    StorageService storage;
    REQUIRE(storage.init(dbPath.path()));
    const auto openaiIds = seedUsageProviderAccount(
        storage,
        "openai",
        "openai.codex_local_sqlite",
        "OpenAI Codex Local");
    PersistedUsageEvent codexEvent;
    codexEvent.providerId = openaiIds.providerId;
    codexEvent.accountId = openaiIds.accountId;
    codexEvent.eventTime = now - 60;
    codexEvent.sourceType = "codex_local_sqlite";
    codexEvent.sourceRunId = "codex-state:test-thread";
    codexEvent.modelKey = "gpt-5.5";
    codexEvent.requestType = "codex_thread_delta";
    codexEvent.totalTokens = 2500;
    codexEvent.rawReference = "codex-state:test-thread:0-2500";
    REQUIRE(storage.insertUsageEvents({codexEvent}));
    storage.close();

    auto openaiProvider = makeCurrentQuotaProvider(
        "openai",
        "OpenAI",
        25,
        100,
        now,
        "interval",
        "openai_subscription_codex_local");

    AppController app;
    app.setHeadlessForTesting(true);
    app.setTaskbarTestModeForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(makeProviderManager({openaiProvider}));
    REQUIRE(app.init());

    const std::vector<TaskbarMetrics> taskbarMetrics = app.getTaskbarMetricsForTesting();
    const TaskbarMetrics* openaiMetric = findTaskbarMetric(taskbarMetrics, "openai");
    REQUIRE(openaiMetric != nullptr);
    CHECK(openaiMetric->kind == TaskbarMetricKind::QuotaUsage);
    CHECK(openaiMetric->providerSurface == "openai_subscription_codex_local");
    CHECK(openaiMetric->used == 25);
    CHECK(openaiMetric->limit == 100);
    CHECK(openaiMetric->secondaryMetric.find("tok") == std::string::npos);

    app.shutdown();
}

TEST_CASE("AppController prefers explicit MiniMax remaining values without regressing OpenAI summaries or taskbar metrics", "[app][runtime]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedAllProviderTrackingEnabled(dbPath.path());
    const std::time_t now = std::time(nullptr);

    QuotaResponse openaiQuota =
        makeQuotaResponse(now, 25, 100, "interval", "openai_subscription_codex_local");
    openaiQuota.secondaryUsed = 35;
    openaiQuota.secondaryLimit = 100;
    openaiQuota.secondaryRemaining = 65;
    openaiQuota.secondaryWindowType = "weekly";
    auto openaiProvider = makeQuotaProvider("openai", "OpenAI", openaiQuota, now);

    QuotaResponse minimaxQuota =
        makeQuotaResponse(now, 992, 1000, "interval", "minimax_subscription_coding_plan");
    minimaxQuota.remaining = 992;
    auto minimaxProvider = makeQuotaProvider("minimax", "MiniMax", minimaxQuota, now);

    auto providerManager = makeProviderManager({openaiProvider, minimaxProvider});

    AppController app;
    app.setHeadlessForTesting(true);
    app.setTaskbarTestModeForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    REQUIRE(app.init());

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    const ProviderStatus* openaiStatus = findProviderStatus(viewModel, "openai");
    REQUIRE(openaiStatus != nullptr);
    REQUIRE(openaiStatus->quotas.size() == 1);
    CHECK(openaiStatus->quotas.front().used == 25);
    CHECK(openaiStatus->quotas.front().remaining == 75);

    const ProviderStatus* minimaxStatus = findProviderStatus(viewModel, "minimax");
    REQUIRE(minimaxStatus != nullptr);
    REQUIRE(minimaxStatus->quotas.size() == 1);
    CHECK(minimaxStatus->quotas.front().used == 8);
    CHECK(minimaxStatus->quotas.front().remaining == 992);
    CHECK(minimaxStatus->quotas.front().remainingPercentage == Catch::Approx(99.2));

    const std::vector<DashboardSummaryItem> summaries =
        app.getDashboardUsageProgressSummariesForTesting();
    const auto openaiSummary = findUsageSummary(summaries, "OpenAI Subscription");
    REQUIRE(openaiSummary.has_value());
    CHECK(openaiSummary->percentage == 75);

    const auto minimaxSummary = findUsageSummary(summaries, "MiniMax - Coding Plan");
    REQUIRE(minimaxSummary.has_value());
    CHECK(minimaxSummary->percentage == 99);
    CHECK(minimaxSummary->detail.find("99% remaining") != std::string::npos);
    CHECK(minimaxSummary->detail.find("992 of 1,000 prompts left") != std::string::npos);

    const std::vector<TaskbarMetrics> taskbarMetrics = app.getTaskbarMetricsForTesting();
    const TaskbarMetrics* openaiMetric = findTaskbarMetric(taskbarMetrics, "openai");
    REQUIRE(openaiMetric != nullptr);
    CHECK(openaiMetric->usagePercentage == Catch::Approx(25.0));
    CHECK(openaiMetric->remainingPercentage == Catch::Approx(75.0));
    CHECK(openaiMetric->primaryMetric == "75%");
    CHECK(openaiMetric->secondaryRemainingPercentage == Catch::Approx(65.0));
    CHECK(openaiMetric->secondaryPrimaryMetric == "65%");
    CHECK(openaiMetric->secondaryMetric == "75 left");
    CHECK(openaiMetric->statusIcon.empty());
    CHECK(openaiMetric->pressureBand == TaskbarPressureBand::Green);
    CHECK(openaiMetric->secondaryPressureBand == TaskbarPressureBand::Green);

    const TaskbarMetrics* minimaxMetric = findTaskbarMetric(taskbarMetrics, "minimax");
    REQUIRE(minimaxMetric != nullptr);
    CHECK(minimaxMetric->usagePercentage == Catch::Approx(0.8));
    CHECK(minimaxMetric->remainingPercentage == Catch::Approx(99.2));
    CHECK(minimaxMetric->primaryMetric == "99%");
    CHECK(minimaxMetric->secondaryMetric == "992 left");
    CHECK(minimaxMetric->statusIcon.empty());
    CHECK(minimaxMetric->pressureBand == TaskbarPressureBand::Blue);

    app.shutdown();
}

TEST_CASE("AppController defaults all shipped provider tracking to disabled on a fresh install", "[app][runtime]") {
    requireSqlite();

    TempTestDatabase dbPath;
    const std::time_t now = std::time(nullptr);
    auto openaiProvider = makeCurrentQuotaProvider("openai", "OpenAI", 25, 100, now);
    auto minimaxProvider = makeCurrentQuotaProvider(
        "minimax",
        "MiniMax",
        42000,
        100000,
        now,
        "interval",
        "minimax_subscription_coding_plan");
    auto claudeProvider = makeCurrentQuotaProvider("claude_code", "Claude Code", 12, 0, now);
    auto kimiProvider = makeCurrentQuotaProvider("kimi", "Kimi Code", 7, 0, now);
    auto providerManager = makeProviderManager({openaiProvider, minimaxProvider, claudeProvider, kimiProvider});

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    REQUIRE(app.init());

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    CHECK_FALSE(viewModel.preferences.openaiSubscriptionTrackingEnabled);
    CHECK_FALSE(viewModel.preferences.miniMaxSubscriptionTrackingEnabled);
    CHECK_FALSE(viewModel.preferences.claudeCodeEnabled);
    CHECK_FALSE(viewModel.preferences.kimiCodeEnabled);
    CHECK(viewModel.providers.empty());

    const DashboardPreferences storedPreferences = loadStartupPreferences(dbPath.path());
    CHECK_FALSE(storedPreferences.openaiSubscriptionTrackingEnabled);
    CHECK_FALSE(storedPreferences.miniMaxSubscriptionTrackingEnabled);
    CHECK_FALSE(storedPreferences.claudeCodeEnabled);
    CHECK_FALSE(storedPreferences.kimiCodeEnabled);

    app.shutdown();
}

TEST_CASE("AppController rehydrates persisted provider tracking preferences", "[app][runtime]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, false, true);

    const std::time_t now = std::time(nullptr);
    auto openaiProvider = makeCurrentQuotaProvider("openai", "OpenAI", 25, 100, now);
    auto minimaxProvider = makeCurrentQuotaProvider(
        "minimax",
        "MiniMax",
        42000,
        100000,
        now,
        "interval",
        "minimax_subscription_coding_plan");
    auto providerManager = makeProviderManager({openaiProvider, minimaxProvider});

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    REQUIRE(app.init());

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    CHECK_FALSE(viewModel.preferences.openaiSubscriptionTrackingEnabled);
    CHECK(viewModel.preferences.miniMaxSubscriptionTrackingEnabled);
    REQUIRE(viewModel.providers.size() == 1);
    CHECK(viewModel.providers.front().providerKey == "minimax");
    CHECK(openaiProvider->getQuotaFetchCount() == 0);

    const DashboardPreferences storedPreferences = loadStartupPreferences(dbPath.path());
    CHECK_FALSE(storedPreferences.openaiSubscriptionTrackingEnabled);
    CHECK(storedPreferences.miniMaxSubscriptionTrackingEnabled);

    app.shutdown();
}

TEST_CASE("AppController toggles OpenAI subscription tracking at runtime", "[app][runtime]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, true, true);
    const std::time_t now = std::time(nullptr);
    auto openaiProvider = makeCurrentQuotaProvider("openai", "OpenAI", 25, 100, now);
    auto minimaxProvider = makeCurrentQuotaProvider(
        "minimax",
        "MiniMax",
        42000,
        100000,
        now,
        "interval",
        "minimax_subscription_coding_plan");
    auto providerManager = makeProviderManager({openaiProvider, minimaxProvider});

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    REQUIRE(app.init());

    DashboardPreferences preferences = app.getDashboardViewModelForTesting().preferences;
    REQUIRE(preferences.openaiSubscriptionTrackingEnabled);
    REQUIRE(preferences.miniMaxSubscriptionTrackingEnabled);

    preferences.openaiSubscriptionTrackingEnabled = false;
    app.applyDashboardPreferencesForTesting(preferences);

    DashboardViewModel disabledViewModel = app.getDashboardViewModelForTesting();
    CHECK_FALSE(disabledViewModel.preferences.openaiSubscriptionTrackingEnabled);
    REQUIRE(disabledViewModel.providers.size() == 1);
    CHECK(disabledViewModel.providers.front().providerKey == "minimax");

    preferences.openaiSubscriptionTrackingEnabled = true;
    app.applyDashboardPreferencesForTesting(preferences);

    DashboardViewModel reenabledViewModel = app.getDashboardViewModelForTesting();
    CHECK(reenabledViewModel.preferences.openaiSubscriptionTrackingEnabled);
    REQUIRE(reenabledViewModel.providers.size() == 2);
    CHECK(findProviderStatus(reenabledViewModel, "openai") != nullptr);
    CHECK(findProviderStatus(reenabledViewModel, "minimax") != nullptr);

    app.shutdown();
}

TEST_CASE("AppController persists and rehydrates the last selected provider scope", "[app][runtime][selection]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedAllProviderTrackingEnabled(dbPath.path());
    const std::time_t now = std::time(nullptr);
    auto openaiProvider = makeCurrentQuotaProvider("openai", "OpenAI", 25, 100, now);
    auto minimaxProvider = makeCurrentQuotaProvider(
        "minimax",
        "MiniMax",
        300,
        1000,
        now,
        "interval",
        "minimax_subscription_coding_plan");
    auto providerManager = makeProviderManager({openaiProvider, minimaxProvider});

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    REQUIRE(app.init());

    DashboardPreferences preferences = app.getDashboardViewModelForTesting().preferences;
    preferences.lastSelectedProviderId = "minimax";
    preferences.lastSelectedAccountId = "minimax.runtime_status";
    app.applyDashboardPreferencesForTesting(preferences);

    const auto selectedSummary =
        findUsageSummary(app.getDashboardUsageProgressSummariesForTesting(), "MiniMax - Coding Plan");
    REQUIRE(selectedSummary.has_value());
    CHECK(findUsageSummary(app.getDashboardUsageProgressSummariesForTesting(), "OpenAI Subscription") == std::nullopt);

    const DashboardPreferences storedPreferences = loadStartupPreferences(dbPath.path());
    CHECK(storedPreferences.lastSelectedProviderId == "minimax");
    CHECK(storedPreferences.lastSelectedAccountId == "minimax.runtime_status");

    app.shutdown();

    AppController rehydratedApp;
    rehydratedApp.setHeadlessForTesting(true);
    rehydratedApp.setDatabasePathForTesting(dbPath.path());
    rehydratedApp.setProviderManagerForTesting(providerManager);
    REQUIRE(rehydratedApp.init());

    const DashboardViewModel rehydratedViewModel = rehydratedApp.getDashboardViewModelForTesting();
    CHECK(rehydratedViewModel.preferences.lastSelectedProviderId == "minimax");
    CHECK(rehydratedViewModel.preferences.lastSelectedAccountId == "minimax.runtime_status");
    CHECK(findUsageSummary(
              rehydratedApp.getDashboardUsageProgressSummariesForTesting(),
              "OpenAI Subscription") == std::nullopt);

    rehydratedApp.shutdown();
}

TEST_CASE("AppController persists and rehydrates analysis scope preferences", "[app][analysis][preferences]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, true, true, false, false);
    const std::time_t now = std::time(nullptr);
    auto openaiProvider = makeCurrentQuotaProvider("openai", "OpenAI", 25, 100, now);
    auto minimaxProvider = makeCurrentQuotaProvider(
        "minimax",
        "MiniMax",
        300,
        1000,
        now,
        "interval",
        "minimax_subscription_coding_plan");
    auto providerManager = makeProviderManager({openaiProvider, minimaxProvider});

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    REQUIRE(app.init());

    DashboardPreferences preferences = app.getDashboardViewModelForTesting().preferences;
    preferences.analysisSelectedProviderId = "openai";
    preferences.analysisSelectedAccountId = "openai.runtime_status";
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Custom;
    preferences.analysisCustomRangeStartUtc = now - (3 * 24 * 3600);
    preferences.analysisCustomRangeEndUtc = now - (2 * 24 * 3600);
    app.applyDashboardPreferencesForTesting(preferences);

    const DashboardPreferences storedPreferences = loadStartupPreferences(dbPath.path());
    CHECK(storedPreferences.analysisSelectedProviderId == "openai");
    CHECK(storedPreferences.analysisSelectedAccountId == "openai.runtime_status");
    CHECK(storedPreferences.analysisDateRangePreset == AnalysisDateRangePreset::Custom);
    CHECK(storedPreferences.analysisCustomRangeStartUtc == preferences.analysisCustomRangeStartUtc);
    CHECK(storedPreferences.analysisCustomRangeEndUtc == preferences.analysisCustomRangeEndUtc);

    app.shutdown();

    AppController rehydratedApp;
    rehydratedApp.setHeadlessForTesting(true);
    rehydratedApp.setDatabasePathForTesting(dbPath.path());
    rehydratedApp.setProviderManagerForTesting(providerManager);
    REQUIRE(rehydratedApp.init());

    const DashboardViewModel rehydrated = rehydratedApp.getDashboardViewModelForTesting();
    CHECK(rehydrated.preferences.analysisSelectedProviderId == "openai");
    CHECK(rehydrated.preferences.analysisSelectedAccountId == "openai.runtime_status");
    CHECK(rehydrated.preferences.analysisDateRangePreset == AnalysisDateRangePreset::Custom);
    CHECK(rehydrated.preferences.analysisCustomRangeStartUtc == preferences.analysisCustomRangeStartUtc);
    CHECK(rehydrated.preferences.analysisCustomRangeEndUtc == preferences.analysisCustomRangeEndUtc);

    rehydratedApp.shutdown();
}

TEST_CASE("AppController builds analysis daily summaries breakdowns and gap messaging from persisted usage", "[app][analysis]") {
    requireSqlite();

    TempTestDatabase dbPath;
    const std::time_t now = std::time(nullptr);
    const std::time_t openaiDayOne = now - (2 * 24 * 3600);
    const std::time_t openaiDayTwo = now - (1 * 24 * 3600);

    seedRuntimeUsageEvent(dbPath.path(), "openai", "OpenAI", openaiDayOne, 240, 18, "gpt-4.1");
    seedRuntimeUsageEvent(dbPath.path(), "openai", "OpenAI", openaiDayTwo, 180, 14, "gpt-4.1");
    seedRuntimeUsageEvent(dbPath.path(), "minimax", "MiniMax", openaiDayTwo, 120, 9, "minimax-m1");

    {
        StorageService storage;
        REQUIRE(storage.init(dbPath.path()));
        const auto openaiProviderId = loadProviderId(storage, "openai");
        const auto openaiAccountId = loadRuntimeAccountId(storage, "openai");
        const auto minimaxProviderId = loadProviderId(storage, "minimax");
        const auto minimaxAccountId = loadRuntimeAccountId(storage, "minimax");
        REQUIRE(openaiProviderId.has_value());
        REQUIRE(openaiAccountId.has_value());
        REQUIRE(minimaxProviderId.has_value());
        REQUIRE(minimaxAccountId.has_value());
        REQUIRE(storage.rebuildUsageRollups(
            UsageRollupResolution::Daily,
            *openaiProviderId,
            *openaiAccountId,
            openaiDayOne - 86400,
            now));
        REQUIRE(storage.rebuildUsageRollups(
            UsageRollupResolution::Daily,
            *minimaxProviderId,
            *minimaxAccountId,
            openaiDayOne - 86400,
            now));

        PersistedGapMarker gap;
        gap.providerId = *openaiProviderId;
        gap.accountId = *openaiAccountId;
        gap.periodStart = utcDayStartForTest(openaiDayOne);
        gap.periodEnd = utcDayStartForTest(openaiDayOne) + 3600;
        gap.classification = GapClassification::Unknown;
        gap.basisSummary = "analysis test gap";
        gap.sourceKind = "test";
        REQUIRE(storage.upsertGapMarker(gap));
        storage.close();
    }

    auto openaiProvider = makeCurrentQuotaProvider("openai", "OpenAI", 25, 100, now);
    auto minimaxProvider = makeCurrentQuotaProvider(
        "minimax",
        "MiniMax",
        300,
        1000,
        now,
        "interval",
        "minimax_subscription_coding_plan");
    auto providerManager = makeProviderManager({openaiProvider, minimaxProvider});

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    REQUIRE(app.init());

    DashboardPreferences preferences = app.getDashboardViewModelForTesting().preferences;
    preferences.analysisSelectedProviderId = "openai";
    preferences.analysisSelectedAccountId = "openai.runtime_status";
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Last7Days;
    app.applyDashboardPreferencesForTesting(preferences);

    const DashboardViewModel model = app.getDashboardViewModelForTesting();
    const std::string openaiDayOneLabel = formatReportDayLabelForTest(utcDayStartForTest(openaiDayOne));
    const std::string openaiDayTwoLabel = formatReportDayLabelForTest(utcDayStartForTest(openaiDayTwo));
    CHECK_FALSE(model.analysis.scopeLabel.empty());
    CHECK(model.analysis.scopeLabel.find("OpenAI") != std::string::npos);
    CHECK(model.analysis.scopeLabel.find("Last 7 Days") != std::string::npos);
    CHECK(std::any_of(
        model.analysis.availableProviders.begin(),
        model.analysis.availableProviders.end(),
        [](const AnalysisScopeOption& option) { return option.key == "openai"; }));
    CHECK(std::any_of(
        model.analysis.availableAccounts.begin(),
        model.analysis.availableAccounts.end(),
        [](const AnalysisScopeOption& option) { return option.key == "openai.runtime_status"; }));

    const auto openaiDayOneSummary = findAnalysisDailySummary(model.analysis.dailySummaries, openaiDayOneLabel);
    const auto openaiDayTwoSummary = findAnalysisDailySummary(model.analysis.dailySummaries, openaiDayTwoLabel);
    REQUIRE(openaiDayOneSummary.has_value());
    REQUIRE(openaiDayTwoSummary.has_value());
    CHECK(openaiDayOneSummary->activityCount >= 1);
    CHECK(openaiDayTwoSummary->activityCount >= 1);
    CHECK(openaiDayOneSummary->sourceLabel != "Unknown");
    CHECK(openaiDayTwoSummary->sourceLabel != "Unknown");
    CHECK(openaiDayOneSummary->gapDetail.classification != AnalysisGapClassification::None);
    CHECK_FALSE(openaiDayOneSummary->gapDetail.classificationLabel.empty());
    CHECK(openaiDayOneSummary->gapDetail.basisSummary.find("analysis test gap") != std::string::npos);

    const auto openaiProviderRow = findAnalysisBreakdownRow(model.analysis.providerBreakdown, "OpenAI");
    REQUIRE(openaiProviderRow.has_value());
    CHECK(openaiProviderRow->activityCount >= 2);
    CHECK(openaiProviderRow->sourceLabel != "Unknown");

    const auto openaiAccountRow = findAnalysisBreakdownRow(model.analysis.accountBreakdown, "OpenAI Runtime");
    REQUIRE(openaiAccountRow.has_value());
    CHECK(openaiAccountRow->activityCount >= 2);
    CHECK(openaiAccountRow->secondaryLabel == "OpenAI");
    CHECK(openaiAccountRow->sourceLabel != "Unknown");

    const auto openaiDayOneTrend = findAnalysisTrendPoint(model.analysis.trendPoints, openaiDayOneLabel);
    const auto openaiDayTwoTrend = findAnalysisTrendPoint(model.analysis.trendPoints, openaiDayTwoLabel);
    REQUIRE(openaiDayOneTrend.has_value());
    REQUIRE(openaiDayTwoTrend.has_value());
    CHECK(openaiDayOneTrend->activityCount == openaiDayOneSummary->activityCount);
    CHECK(openaiDayTwoTrend->activityCount == openaiDayTwoSummary->activityCount);

    CHECK(model.analysis.gapSummaryCounts.unknownDays >= 1);
    CHECK(model.analysis.dataQualityNote.find("Rollup") != std::string::npos);
    CHECK(model.analysis.dataQualityNote.find("gap") != std::string::npos);

    app.shutdown();
}

TEST_CASE("AppController builds analysis history and provenance messaging from persisted local activity", "[app][analysis]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, false, false, true, false);
    const std::time_t now = std::time(nullptr);
    const std::time_t dayOne = utcDayStartForTest(now) - (2 * 24 * 3600) + 3600;
    const std::time_t dayTwo = utcDayStartForTest(now) - (1 * 24 * 3600) + 7200;

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    REQUIRE(app.init());

    ActivityEvent firstEvent;
    firstEvent.providerKey = "claude_code";
    firstEvent.providerSurface = "claude_code_local_stats";
    firstEvent.activityKind = ActivityKind::SessionActive;
    firstEvent.startedAt = dayOne;
    firstEvent.endedAt = dayOne + 300;
    firstEvent.durationSeconds = 300;
    firstEvent.sourceType = "local";
    firstEvent.sourceRunId = "claude-local-1";
    firstEvent.surfaceKey = "workspace";
    firstEvent.windowTitleHint = "Claude Code";
    firstEvent.confidence = ActivityConfidence::LocalExact;
    firstEvent.rawReference = "local://claude/run-1";
    REQUIRE(app.recordLocalActivityForTesting(firstEvent));

    ActivityEvent secondEvent = firstEvent;
    secondEvent.startedAt = dayTwo;
    secondEvent.endedAt = dayTwo + 420;
    secondEvent.durationSeconds = 420;
    secondEvent.sourceRunId = "claude-local-2";
    secondEvent.rawReference = "local://claude/run-2";
    REQUIRE(app.recordLocalActivityForTesting(secondEvent));

    REQUIRE(waitForCondition([&app, now]() {
        return app.getPersistedActivitiesForTesting(
                   "claude_code",
                   "claude_code_local_stats",
                   now - (4 * 24 * 3600),
                   now + 60)
                   .size() >= 2;
    }));

    DashboardPreferences preferences = app.getDashboardViewModelForTesting().preferences;
    preferences.analysisSelectedProviderId = "claude_code";
    preferences.analysisSelectedAccountId = "claude_code.runtime_status";
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Last7Days;
    app.applyDashboardPreferencesForTesting(preferences);

    const DashboardViewModel model = app.getDashboardViewModelForTesting();
    const std::string dayOneLabel = formatReportDayLabelForTest(utcDayStartForTest(dayOne));
    const std::string dayTwoLabel = formatReportDayLabelForTest(utcDayStartForTest(dayTwo));
    CHECK(model.analysis.scope.selectedAccountId.empty());
    CHECK(model.analysis.scopeLabel.find("Claude Code") != std::string::npos);
    CHECK(model.analysis.scopeLabel.find("All Accounts") != std::string::npos);
    CHECK(std::any_of(
        model.analysis.availableProviders.begin(),
        model.analysis.availableProviders.end(),
        [](const AnalysisScopeOption& option) { return option.key == "claude_code"; }));
    CHECK(std::any_of(
        model.analysis.availableAccounts.begin(),
        model.analysis.availableAccounts.end(),
        [](const AnalysisScopeOption& option) { return option.label.find("Claude Code /") != std::string::npos; }));

    const auto dayOneSummary = findAnalysisDailySummary(model.analysis.dailySummaries, dayOneLabel);
    const auto dayTwoSummary = findAnalysisDailySummary(model.analysis.dailySummaries, dayTwoLabel);
    REQUIRE(dayOneSummary.has_value());
    REQUIRE(dayTwoSummary.has_value());
    CHECK(dayOneSummary->activityCount >= 1);
    CHECK(dayTwoSummary->activityCount >= 1);
    CHECK(dayOneSummary->sourceLabel != "Unknown");
    CHECK(dayTwoSummary->sourceLabel != "Unknown");
    CHECK(dayOneSummary->gapDetail.classification == AnalysisGapClassification::None);
    CHECK(dayTwoSummary->gapDetail.classification == AnalysisGapClassification::None);

    const auto providerRow = findAnalysisBreakdownRow(model.analysis.providerBreakdown, "Claude Code");
    REQUIRE(providerRow.has_value());
    CHECK(providerRow->activityCount >= 2);
    CHECK(providerRow->sourceLabel != "Unknown");

    const auto accountRow = std::find_if(
        model.analysis.accountBreakdown.begin(),
        model.analysis.accountBreakdown.end(),
        [](const AnalysisBreakdownRow& row) {
            return row.secondaryLabel == "Claude Code" &&
                row.label.find("Local Observation") != std::string::npos;
        });
    REQUIRE(accountRow != model.analysis.accountBreakdown.end());
    CHECK(accountRow->activityCount >= 2);
    CHECK(accountRow->secondaryLabel == "Claude Code");
    CHECK(accountRow->sourceLabel != "Unknown");

    const auto dayOneTrend = findAnalysisTrendPoint(model.analysis.trendPoints, dayOneLabel);
    const auto dayTwoTrend = findAnalysisTrendPoint(model.analysis.trendPoints, dayTwoLabel);
    REQUIRE(dayOneTrend.has_value());
    REQUIRE(dayTwoTrend.has_value());
    CHECK(dayOneTrend->activityCount == dayOneSummary->activityCount);
    CHECK(dayTwoTrend->activityCount == dayTwoSummary->activityCount);

    CHECK(model.analysis.gapSummaryCounts.knownZeroDays == 0);
    CHECK(model.analysis.gapSummaryCounts.reconstructedDays == 0);
    CHECK(model.analysis.gapSummaryCounts.unknownDays == 0);
    CHECK(model.analysis.dataQualityNote.find("local") != std::string::npos);

    app.shutdown();
}

TEST_CASE("AppController analysis scope combines persisted usage and local activity providers into shared summaries", "[app][analysis]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, true, false, true, false);
    const std::time_t now = std::time(nullptr);
    const std::time_t openaiDay = utcDayStartForTest(now) - (2 * 24 * 3600) + 3600;
    const std::time_t claudeDay = utcDayStartForTest(now) - (1 * 24 * 3600) + 5400;

    seedRuntimeUsageEvent(dbPath.path(), "openai", "OpenAI", openaiDay, 240, 18, "gpt-4.1");

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    REQUIRE(app.init());

    ActivityEvent claudeEvent;
    claudeEvent.providerKey = "claude_code";
    claudeEvent.providerSurface = "claude_code_local_stats";
    claudeEvent.activityKind = ActivityKind::SessionActive;
    claudeEvent.startedAt = claudeDay;
    claudeEvent.endedAt = claudeDay + 240;
    claudeEvent.durationSeconds = 240;
    claudeEvent.sourceType = "local";
    claudeEvent.sourceRunId = "claude-local-mixed";
    claudeEvent.surfaceKey = "workspace";
    claudeEvent.windowTitleHint = "Claude Code";
    claudeEvent.confidence = ActivityConfidence::LocalExact;
    claudeEvent.rawReference = "local://claude/mixed";
    REQUIRE(app.recordLocalActivityForTesting(claudeEvent));

    REQUIRE(waitForCondition([&app, now]() {
        return !app.getPersistedActivitiesForTesting(
                    "claude_code",
                    "claude_code_local_stats",
                    now - (4 * 24 * 3600),
                    now + 60)
                    .empty();
    }));

    DashboardPreferences preferences = app.getDashboardViewModelForTesting().preferences;
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Last7Days;
    app.applyDashboardPreferencesForTesting(preferences);

    const DashboardViewModel model = app.getDashboardViewModelForTesting();
    const std::string openaiDayLabel = formatReportDayLabelForTest(utcDayStartForTest(openaiDay));
    const std::string claudeDayLabel = formatReportDayLabelForTest(utcDayStartForTest(claudeDay));
    CHECK(std::any_of(
        model.analysis.availableProviders.begin(),
        model.analysis.availableProviders.end(),
        [](const AnalysisScopeOption& option) { return option.key == "openai"; }));
    CHECK(std::any_of(
        model.analysis.availableProviders.begin(),
        model.analysis.availableProviders.end(),
        [](const AnalysisScopeOption& option) { return option.key == "claude_code"; }));
    CHECK(std::any_of(
        model.analysis.availableAccounts.begin(),
        model.analysis.availableAccounts.end(),
        [](const AnalysisScopeOption& option) { return option.key == "openai.runtime_status"; }));
    CHECK(std::any_of(
        model.analysis.availableAccounts.begin(),
        model.analysis.availableAccounts.end(),
        [](const AnalysisScopeOption& option) { return option.label.find("Claude Code /") != std::string::npos; }));

    const auto openaiSummary = findAnalysisDailySummary(model.analysis.dailySummaries, openaiDayLabel);
    const auto claudeSummary = findAnalysisDailySummary(model.analysis.dailySummaries, claudeDayLabel);
    REQUIRE(openaiSummary.has_value());
    REQUIRE(claudeSummary.has_value());
    CHECK(openaiSummary->activityCount >= 1);
    CHECK(claudeSummary->activityCount >= 1);
    CHECK(openaiSummary->sourceLabel != "Unknown");
    CHECK(claudeSummary->sourceLabel != "Unknown");

    const auto openaiProviderRow = findAnalysisBreakdownRow(model.analysis.providerBreakdown, "OpenAI");
    const auto claudeProviderRow = findAnalysisBreakdownRow(model.analysis.providerBreakdown, "Claude Code");
    REQUIRE(openaiProviderRow.has_value());
    REQUIRE(claudeProviderRow.has_value());
    CHECK(openaiProviderRow->activityCount >= 1);
    CHECK(claudeProviderRow->activityCount >= 1);
    CHECK(openaiProviderRow->sourceLabel != "Unknown");
    CHECK(claudeProviderRow->sourceLabel != "Unknown");

    const auto openaiAccountRow = findAnalysisBreakdownRow(model.analysis.accountBreakdown, "OpenAI Runtime");
    const auto claudeAccountRow = std::find_if(
        model.analysis.accountBreakdown.begin(),
        model.analysis.accountBreakdown.end(),
        [](const AnalysisBreakdownRow& row) {
            return row.secondaryLabel == "Claude Code" &&
                row.label.find("Local Observation") != std::string::npos;
        });
    REQUIRE(openaiAccountRow.has_value());
    REQUIRE(claudeAccountRow != model.analysis.accountBreakdown.end());
    CHECK(openaiAccountRow->secondaryLabel == "OpenAI");
    CHECK(claudeAccountRow->secondaryLabel == "Claude Code");

    const auto openaiTrend = findAnalysisTrendPoint(model.analysis.trendPoints, openaiDayLabel);
    const auto claudeTrend = findAnalysisTrendPoint(model.analysis.trendPoints, claudeDayLabel);
    REQUIRE(openaiTrend.has_value());
    REQUIRE(claudeTrend.has_value());
    CHECK(openaiTrend->activityCount == openaiSummary->activityCount);
    CHECK(claudeTrend->activityCount == claudeSummary->activityCount);

    CHECK((model.analysis.dataQualityNote.find("OpenAI") != std::string::npos ||
           model.analysis.dataQualityNote.find("Claude") != std::string::npos ||
           model.analysis.dataQualityNote.find("local") != std::string::npos));

    app.shutdown();
}

TEST_CASE("AppController manual HTML export uses the current analysis scope", "[app][analysis][reporting]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, true, true, false, false);
    TempTestDirectory exportDir("jistats_analysis_manual_export");
    const std::time_t now = std::time(nullptr);
    const std::time_t openaiOldDay = now - (3 * 24 * 3600);
    const std::time_t openaiScopedDay = now - (1 * 24 * 3600);

    seedRuntimeUsageEvent(dbPath.path(), "openai", "OpenAI", openaiOldDay, 240, 18, "gpt-4.1");
    seedRuntimeUsageEvent(dbPath.path(), "openai", "OpenAI", openaiScopedDay, 180, 14, "gpt-4.1");
    seedRuntimeUsageEvent(dbPath.path(), "minimax", "MiniMax", openaiScopedDay, 500, 40, "minimax-m1");

    auto openaiProvider = makeCurrentQuotaProvider("openai", "OpenAI", 25, 100, now);
    auto minimaxProvider = makeCurrentQuotaProvider(
        "minimax",
        "MiniMax",
        300,
        1000,
        now,
        "interval",
        "minimax_subscription_coding_plan");
    auto providerManager = makeProviderManager({openaiProvider, minimaxProvider});

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    REQUIRE(app.init());

    DashboardPreferences preferences = app.getDashboardViewModelForTesting().preferences;
    preferences.analysisSelectedProviderId = "openai";
    preferences.analysisSelectedAccountId = "openai.runtime_status";
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Custom;
    preferences.analysisCustomRangeStartUtc = openaiScopedDay;
    preferences.analysisCustomRangeEndUtc = openaiScopedDay;
    app.applyDashboardPreferencesForTesting(preferences);

    const std::filesystem::path outputPath = exportDir.path() / "analysis-scope.html";
    const DashboardHtmlExportResult exportResult = app.exportHtmlReportForTesting(outputPath.string());
    REQUIRE(exportResult.success);
    CHECK(exportResult.outputPath == outputPath.string());

    std::ifstream report(outputPath);
    REQUIRE(report.is_open());
    const std::string reportBody((std::istreambuf_iterator<char>(report)),
                                 std::istreambuf_iterator<char>());
    CHECK(reportBody.find("OpenAI") != std::string::npos);
    CHECK(reportBody.find("MiniMax") == std::string::npos);
    CHECK(reportBody.find("180 tokens") != std::string::npos);
    CHECK(reportBody.find("240 tokens") == std::string::npos);

    {
        StorageService storage;
        REQUIRE(storage.init(dbPath.path()));
        const auto exportRun = storage.getLatestExportRun("manual_html");
        REQUIRE(exportRun.has_value());
        CHECK(exportRun->success);
        CHECK(exportRun->format == "html");
        CHECK(exportRun->outputPath == outputPath.string());
        CHECK(exportRun->providerFilterCsv == "openai");
        CHECK(exportRun->accountFilterCsv == "openai.runtime_status");
        CHECK(exportRun->rangeStart > openaiOldDay);
        CHECK(exportRun->rangeEnd >= exportRun->rangeStart);
    }

    app.shutdown();
}

TEST_CASE("AppController writes a scheduled HTML report when the export seam is enabled", "[app][reporting][scheduled]") {
    requireSqlite();

    TempTestDatabase dbPath;
    TempTestDirectory exportDir("jistats_scheduled_export");
    const std::time_t now = std::time(nullptr);

    seedStartupPreferences(
        dbPath.path(),
        false,
        false,
        true,
        false,
        false,
        false,
        true,
        60,
        exportDir.path().string(),
        "openai",
        "openai.runtime_status");
    seedRuntimeUsageEvent(
        dbPath.path(),
        "openai",
        "OpenAI",
        now - 1800,
        240,
        18,
        "gpt-4.1");

    auto openaiProvider = makeCurrentQuotaProvider("openai", "OpenAI", 25, 100, now);
    auto minimaxProvider = makeCurrentQuotaProvider(
        "minimax",
        "MiniMax",
        300,
        1000,
        now,
        "interval",
        "minimax_subscription_coding_plan");
    auto providerManager = makeProviderManager({openaiProvider, minimaxProvider});

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setScheduledExportPollIntervalForTesting(50);
    REQUIRE(app.init());

    REQUIRE(waitForCondition([&]() {
        return !listHtmlReports(exportDir.path()).empty();
    }, std::chrono::milliseconds(4000)));

    const auto reports = listHtmlReports(exportDir.path());
    REQUIRE(reports.size() == 1);
    CHECK(reports.front().filename().string().find("openai_runtime_status") != std::string::npos);

    std::ifstream report(reports.front());
    REQUIRE(report.is_open());
    const std::string reportBody((std::istreambuf_iterator<char>(report)),
                                 std::istreambuf_iterator<char>());
    CHECK(reportBody.find("OpenAI") != std::string::npos);
    CHECK(reportBody.find("MiniMax") == std::string::npos);
    CHECK(reportBody.find("Daily Summary") != std::string::npos);
    CHECK(reportBody.find("Weekly Summary") != std::string::npos);
    CHECK(reportBody.find("Provider Breakdown") != std::string::npos);
    CHECK(reportBody.find("Account Breakdown") != std::string::npos);
    CHECK(reportBody.find("Model Breakdown") != std::string::npos);
    CHECK(reportBody.find("Recent Trend") != std::string::npos);
    CHECK(reportBody.find("Current Quota Mirror Snapshots") != std::string::npos);
    CHECK(reportBody.find("gpt-4.1") != std::string::npos);

    const DashboardPreferences storedPreferences = loadStartupPreferences(dbPath.path());
    CHECK(storedPreferences.scheduledExportEnabled);
    CHECK(storedPreferences.scheduledExportPath == exportDir.path().string());

    REQUIRE(waitForCondition([&app]() {
        return !app.getDashboardViewModelForTesting().scheduledExportLastRunTime.empty();
    }, std::chrono::milliseconds(4000)));

    const DashboardViewModel scheduledViewModel = app.getDashboardViewModelForTesting();
    CHECK(scheduledViewModel.scheduledExportStatusSummary == "Every 60 mins");
    CHECK(scheduledViewModel.scheduledExportStatusDetail.find(exportDir.path().string()) != std::string::npos);
    CHECK(scheduledViewModel.scheduledExportStatusDetail.find("Latest attempt:") != std::string::npos);

    {
        StorageService storage;
        REQUIRE(storage.init(dbPath.path()));
        const auto exportRun = storage.getLatestExportRun("scheduled_html");
        REQUIRE(exportRun.has_value());
        CHECK(exportRun->success);
        CHECK(exportRun->format == "html");
        CHECK(exportRun->providerFilterCsv == "openai");
        CHECK(exportRun->accountFilterCsv == "openai.runtime_status");
        CHECK(exportRun->outputPath.find(exportDir.path().string()) != std::string::npos);
        CHECK(exportRun->completedAt >= exportRun->requestedAt);
    }

    app.shutdown();
}

TEST_CASE("AppController scheduled HTML export uses retained daily rollups after raw usage retention", "[app][reporting][scheduled]") {
    requireSqlite();

    TempTestDatabase dbPath;
    TempTestDirectory exportDir("jistats_scheduled_export_rollups");
    const std::time_t now = std::time(nullptr);

    seedStartupPreferences(
        dbPath.path(),
        false,
        false,
        true,
        false,
        false,
        false,
        true,
        60,
        exportDir.path().string(),
        "openai",
        "openai.runtime_status");
    seedRuntimeUsageEvent(
        dbPath.path(),
        "openai",
        "OpenAI",
        now - 1800,
        240,
        18,
        "gpt-4.1");

    {
        StorageService storage;
        REQUIRE(storage.init(dbPath.path()));
        const auto providerId = loadProviderId(storage, "openai");
        const auto accountId = loadRuntimeAccountId(storage, "openai");
        REQUIRE(providerId.has_value());
        REQUIRE(accountId.has_value());
        REQUIRE(storage.rebuildUsageRollups(
            UsageRollupResolution::Daily,
            *providerId,
            *accountId,
            now - 86400,
            now));
        REQUIRE(storage.execute("DELETE FROM usage_events;").success());
        storage.close();
    }

    auto openaiProvider = makeCurrentQuotaProvider("openai", "OpenAI", 25, 100, now);
    auto providerManager = makeProviderManager({openaiProvider});

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setScheduledExportPollIntervalForTesting(50);
    REQUIRE(app.init());

    REQUIRE(waitForCondition([&]() {
        return !listHtmlReports(exportDir.path()).empty();
    }, std::chrono::milliseconds(4000)));

    const auto reports = listHtmlReports(exportDir.path());
    REQUIRE(reports.size() == 1);

    std::ifstream report(reports.front());
    REQUIRE(report.is_open());
    const std::string reportBody((std::istreambuf_iterator<char>(report)),
                                 std::istreambuf_iterator<char>());
    CHECK(reportBody.find("Daily Summary") != std::string::npos);
    CHECK(reportBody.find("Provider Breakdown") != std::string::npos);
    CHECK(reportBody.find("Model Breakdown") != std::string::npos);
    CHECK(reportBody.find("240 tokens") != std::string::npos);
    CHECK(reportBody.find("gpt-4.1") != std::string::npos);

    app.shutdown();
}

TEST_CASE("AppController surfaces recovery and scheduled export status in the dashboard view model", "[app][runtime][status]") {
    requireSqlite();

    TempTestDatabase dbPath;
    TempTestDirectory exportDir("jistats_dashboard_status");
    const std::time_t now = std::time(nullptr);

    seedStartupPreferences(
        dbPath.path(),
        false,
        false,
        true,
        false,
        false,
        false,
        true,
        60,
        exportDir.path().string(),
        "openai",
        "openai.runtime_status");
    seedRuntimeState(
        dbPath.path(),
        now - (3 * 3600),
        now - (3 * 3600));
    seedRuntimeUsageEvent(
        dbPath.path(),
        "openai",
        "OpenAI",
        now - 1800,
        200,
        15);

    auto liveProvider = makeCurrentQuotaProvider("openai", "OpenAI", 25, 100, now);
    liveProvider->setUsageResponse(makeBackfillUsageResponseWithOneEvent(now - 1800));
    auto providerManager = makeProviderManager({liveProvider});

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setScheduledExportPollIntervalForTesting(50);
    REQUIRE(app.init());

    REQUIRE(waitForCondition([&]() {
        const DashboardViewModel model = app.getDashboardViewModelForTesting();
        return !model.scheduledExportLastRunTime.empty() &&
               model.recoveryStatusDetail.find("matched 1 local event") != std::string::npos;
    }, std::chrono::milliseconds(4000)));

    const DashboardViewModel model = app.getDashboardViewModelForTesting();
    CHECK(model.recoveryStatusDetail.find("matched 1 local event") != std::string::npos);
    CHECK(model.recoveryStatusDetail.find("matched 1 local event") != std::string::npos);
    CHECK(model.scheduledExportStatusSummary == "Every 60 mins");
    CHECK(model.scheduledExportStatusDetail.find(exportDir.path().string()) != std::string::npos);
    CHECK(model.scheduledExportStatusDetail.find("Latest attempt:") != std::string::npos);

    {
        StorageService storage;
        REQUIRE(storage.init(dbPath.path()));

        const auto openaiProviderId = loadProviderId(storage, "openai");
        const auto openaiAccountId = loadRuntimeAccountId(storage, "openai");
        REQUIRE(openaiProviderId.has_value());
        REQUIRE(openaiAccountId.has_value());

        const auto recovery = storage.getLatestRecoveryCheckpoint(
            *openaiProviderId,
            *openaiAccountId,
            "runtime_backfill");
        REQUIRE(recovery.has_value());
        CHECK(recovery->outcome == "imported");
        CHECK(recovery->comparisonResult == "matched_local_evidence");
        CHECK(recovery->fetchedEventCount == 1);
        CHECK(recovery->localEventCount == 1);

        const auto exportRun = storage.getLatestExportRun("scheduled_html");
        REQUIRE(exportRun.has_value());
        CHECK(exportRun->success);
        CHECK(exportRun->providerFilterCsv == "openai");
        CHECK(exportRun->accountFilterCsv == "openai.runtime_status");
    }

    app.shutdown();
}

TEST_CASE("AppController tray exit requests shutdown without hanging the run loop", "[app][tray]") {
    auto providerManager = std::make_shared<ProviderManager>();
    ScopedTrayTestHandler handler([](TrayController::TestOperation,
                                     TrayStatus,
                                     const std::string&,
                                     std::string&) -> std::optional<bool> {
        return true;
    });

    TempTestDatabase dbPath;
    AppController app;
    app.setDatabasePathForTesting(dbPath.path());
    app.setDisableTaskbarForTesting(true);
    app.setProviderManagerForTesting(providerManager);

    REQUIRE(app.init());

    auto runFuture = std::async(std::launch::async, [&app]() {
        return app.run();
    });

    REQUIRE(waitForCondition([&app]() {
        return app.getState() == "ui-active";
    }));

    auto exitFuture = std::async(std::launch::async, [&app]() {
        return app.dispatchTrayEventForTesting(TrayController::TestEvent::MenuExit);
    });

    CHECK(exitFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    REQUIRE(exitFuture.get());

    CHECK(runFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(runFuture.get() == 0);

    app.shutdown();
}

TEST_CASE("AppController runs retention pass during lifecycle without regressing rollup-backed summaries", "[app][retention]") {
    requireSqlite();

    TempTestDatabase dbPath;
    const std::time_t now = std::time(nullptr);
    constexpr std::time_t kSecondsPerDay = 24 * 3600;
    const std::time_t oldEventTime = now - (120 * kSecondsPerDay);
    const std::time_t recentEventTime = now - (2 * kSecondsPerDay);

    StorageService storage;
    REQUIRE(storage.init(dbPath.path()));

    const auto openaiIds = seedUsageProviderAccount(
        storage,
        "openai",
        "acct-openai-retention",
        "OpenAI Retention Account");

    insertUsageEvent(
        storage,
        openaiIds.providerId,
        openaiIds.accountId,
        toUtcIso(oldEventTime),
        "gpt-4o",
        200,
        20);
    insertUsageEvent(
        storage,
        openaiIds.providerId,
        openaiIds.accountId,
        toUtcIso(recentEventTime),
        "gpt-4o",
        300,
        30);

    REQUIRE(storage.rebuildUsageRollups(
        UsageRollupResolution::Hourly,
        openaiIds.providerId,
        openaiIds.accountId,
        oldEventTime - 3600,
        now));
    REQUIRE(storage.rebuildUsageRollups(
        UsageRollupResolution::Daily,
        openaiIds.providerId,
        openaiIds.accountId,
        oldEventTime - kSecondsPerDay,
        now));

    PersistedGapMarker oldGap;
    oldGap.providerId = openaiIds.providerId;
    oldGap.accountId = openaiIds.accountId;
    oldGap.periodStart = oldEventTime - (2 * kSecondsPerDay);
    oldGap.periodEnd = oldEventTime - kSecondsPerDay;
    oldGap.classification = GapClassification::Unknown;
    oldGap.basisSummary = "test old gap";
    oldGap.sourceKind = "test";
    REQUIRE(storage.upsertGapMarker(oldGap));

    PersistedGapMarker recentGap;
    recentGap.providerId = openaiIds.providerId;
    recentGap.accountId = openaiIds.accountId;
    recentGap.periodStart = recentEventTime - 3600;
    recentGap.periodEnd = recentEventTime;
    recentGap.classification = GapClassification::KnownZero;
    recentGap.basisSummary = "test recent gap";
    recentGap.sourceKind = "test";
    REQUIRE(storage.upsertGapMarker(recentGap));

    storage.close();

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(std::make_shared<ProviderManager>());
    app.setRetentionPollIntervalForTesting(50);
    REQUIRE(app.init());

    REQUIRE(waitForCondition([&dbPath]() {
        StorageService check;
        if (!check.init(dbPath.path())) {
            return false;
        }
        SettingsService settings(check);
        const bool ran = settings.getString("retention.last_run_utc").has_value();
        check.close();
        return ran;
    }, std::chrono::milliseconds(4000)));

    {
        StorageService verify;
        REQUIRE(verify.init(dbPath.path()));

        auto oldEvents = verify.execute(
            "SELECT COUNT(*) FROM usage_events WHERE event_time_utc < ?;",
            {toUtcIso(now - (90 * kSecondsPerDay))});
        REQUIRE(oldEvents.success());
        CHECK(oldEvents.getInt(0, 0) == 0);

        auto recentEvents = verify.execute(
            "SELECT COUNT(*) FROM usage_events WHERE event_time_utc >= ?;",
            {toUtcIso(now - (90 * kSecondsPerDay))});
        REQUIRE(recentEvents.success());
        CHECK(recentEvents.getInt(0, 0) >= 1);

        const auto recentGaps = verify.getGapMarkers(
            openaiIds.providerId,
            openaiIds.accountId,
            recentEventTime - 7200,
            recentEventTime + 3600);
        CHECK(recentGaps.size() == 1);

        const auto oldGaps = verify.getGapMarkers(
            openaiIds.providerId,
            openaiIds.accountId,
            oldEventTime - (3 * kSecondsPerDay),
            oldEventTime);
        CHECK(oldGaps.empty());

        verify.close();
    }

    const auto summaries = app.getDashboardUsageProgressSummariesForTesting();
    const auto last24h = findUsageSummary(summaries, "Last 24 Hours");
    if (last24h.has_value()) {
        CHECK_FALSE(last24h->detail.empty());
    }

    app.shutdown();
}

TEST_CASE("AppController retention pass preserves recovery metadata", "[app][retention]") {
    requireSqlite();

    TempTestDatabase dbPath;
    const std::time_t now = std::time(nullptr);
    constexpr std::time_t kSecondsPerDay = 24 * 3600;

    StorageService storage;
    REQUIRE(storage.init(dbPath.path()));

    const auto openaiIds = seedUsageProviderAccount(
        storage,
        "openai",
        "acct-openai-retention-meta",
        "OpenAI Retention Meta Account");

    insertUsageEvent(
        storage,
        openaiIds.providerId,
        openaiIds.accountId,
        toUtcIso(now - (120 * kSecondsPerDay)),
        "gpt-4o",
        400,
        40);

    REQUIRE(storage.rebuildUsageRollups(
        UsageRollupResolution::Daily,
        openaiIds.providerId,
        openaiIds.accountId,
        now - (125 * kSecondsPerDay),
        now));

    PersistedRuntimeState runtimeState;
    runtimeState.lastLaunchAt = now - 3600;
    runtimeState.lastSuccessfulSyncAt = now - 1800;
    runtimeState.lastLocalObservationAt = now - 900;
    runtimeState.recoveryMarker = "test_recovery_marker";
    REQUIRE(storage.upsertRuntimeState(runtimeState));

    storage.close();

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(std::make_shared<ProviderManager>());
    app.setRetentionPollIntervalForTesting(50);
    REQUIRE(app.init());

    REQUIRE(waitForCondition([&dbPath]() {
        StorageService check;
        if (!check.init(dbPath.path())) {
            return false;
        }
        SettingsService settings(check);
        const bool ran = settings.getString("retention.last_run_utc").has_value();
        check.close();
        return ran;
    }, std::chrono::milliseconds(4000)));

    const auto recoveredState = app.getPersistedRuntimeStateForTesting();
    REQUIRE(recoveredState.has_value());
    CHECK(recoveredState->lastLaunchAt > 0);
    CHECK(recoveredState->lastSuccessfulSyncAt > 0);
    CHECK(recoveredState->lastLocalObservationAt > 0);

    {
        StorageService verify;
        REQUIRE(verify.init(dbPath.path()));

        auto oldEvents = verify.execute(
            "SELECT COUNT(*) FROM usage_events WHERE event_time_utc < ?;",
            {toUtcIso(now - (90 * kSecondsPerDay))});
        REQUIRE(oldEvents.success());
        CHECK(oldEvents.getInt(0, 0) == 0);

        const auto dailyRollups = verify.getUsageRollups(
            UsageRollupResolution::Daily,
            openaiIds.providerId,
            openaiIds.accountId,
            now - (365 * kSecondsPerDay),
            now);
        CHECK_FALSE(dailyRollups.empty());

        verify.close();
    }

    app.shutdown();
}
