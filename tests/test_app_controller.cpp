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

#include <windows.h>
#include <sddl.h>

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
#include "app_controller_test_support.hpp"
#include "runtime_test_support.hpp"
#include "sqlite_test_support.hpp"
#include "temp_test_directory.hpp"

#include <windows.h>
#include <commctrl.h>

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

std::wstring readControlText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring result(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, result.data(), length + 1);
    result.resize(static_cast<size_t>(copied));
    return result;
}

const std::filesystem::path kOpenAIExportFixturePath =
    std::filesystem::path("docs/fixtures/openai/chatgpt-export-redacted/conversations.json");

void writeFile(const std::filesystem::path& path, const std::string& contents) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.is_open());
    output << contents;
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

} // namespace

void JiStats::TestSupport::seedStartupPreferences(
    const std::string& path, bool enabled, bool startBackground, bool openaiSubscriptionTrackingEnabled,
    bool miniMaxSubscriptionTrackingEnabled, bool claudeCodeEnabled, bool kimiCodeEnabled, bool scheduledExportEnabled,
    int scheduledExportIntervalMinutes, const std::string& scheduledExportPath,
    const std::string& lastSelectedProviderId, const std::string& lastSelectedAccountId,
    const std::string& analysisSelectedProviderId, const std::string& analysisSelectedAccountId,
    const std::string& analysisDateRangePreset, const std::string& analysisCustomRangeStartUtc,
    const std::string& analysisCustomRangeEndUtc) {
    StorageService storage;
    REQUIRE(storage.init(path));
    REQUIRE(storage.beginTransaction());

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

    REQUIRE(storage.commitTransaction());
    storage.close();
}

namespace {

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
    REQUIRE(storage
                .execute("DELETE FROM schema_version; INSERT INTO schema_version (version, description) VALUES (11, "
                         "'Unsupported fixture');")
                .success());
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

TEST_CASE("AppController applies a custom Analysis edit alongside a Settings change after an older provider refresh",
          "[app][analysis][navigation]") {
    requireSqlite();
    TempTestDatabase dbPath;
    const auto editedStart = parseUtcIsoForTest("2026-04-01T00:00:00Z");
    REQUIRE(editedStart.has_value());
    {
        StorageService storage;
        REQUIRE(storage.init(dbPath.path()));
        SettingsService settings(storage);
        REQUIRE(settings.setBool("ui.taskbar_overlay_enabled", true));
        REQUIRE(settings.setString("ui.analysis_date_range_preset", "custom"));
        REQUIRE(settings.setString("ui.analysis_custom_range_start_utc", "2026-04-02T00:00:00Z"));
        REQUIRE(settings.setString("ui.analysis_custom_range_end_utc", "2026-04-05T00:00:00Z"));
    }

    ScopedTrayTestHandler tray([](TrayController::TestOperation, TrayStatus, const std::string&,
                                  std::string&) -> std::optional<bool> { return true; });
    AppController app;
    app.setDisableTaskbarForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(std::make_shared<ProviderManager>());
    REQUIRE(app.init());

    const HWND dashboard = FindWindowW(L"JiStatsDashboardWindow", nullptr);
    REQUIRE(dashboard != nullptr);
    const HWND history = FindWindowExW(dashboard, nullptr, L"BUTTON", L"&History...");
    REQUIRE(history != nullptr);
    REQUIRE(PostMessageW(history, BM_CLICK, 0, 0));
    HWND startDate = nullptr;
    REQUIRE(waitForCondition(
        [&]() {
            startDate = GetDlgItem(FindWindowW(L"JiStatsAnalysisWindow", nullptr), 2104);
            return startDate != nullptr && IsWindowVisible(startDate) && IsWindowEnabled(startDate);
        },
        std::chrono::seconds(5)));

    REQUIRE(app.dispatchDashboardShellCommandForTesting(DashboardShellCommand::FileSettings));
    HWND overlay = nullptr;
    REQUIRE(waitForCondition([&]() {
        overlay = GetDlgItem(FindWindowW(L"JiStatsPreferencesWindow", nullptr), 1101);
        return overlay != nullptr;
    }));

    std::promise<void> reachedPublication;
    auto publicationReached = reachedPublication.get_future();
    std::promise<void> releasePublication;
    auto publicationReleased = releasePublication.get_future().share();
    app.setBeforeRuntimePublicationForTesting([&reachedPublication, publicationReleased]() {
        if (publicationReleased.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            reachedPublication.set_value();
            publicationReleased.wait();
        }
    });

    auto refresh = std::async(std::launch::async, [&app]() { app.refreshRuntimeForTesting(); });
    const bool refreshPaused = publicationReached.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    const bool editQueued = SetWindowTextW(startDate, L"2026-04-01") != FALSE &&
                            waitForCondition([&]() { return !IsWindowEnabled(startDate); });

    SendMessageW(overlay, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(GetParent(overlay), WM_COMMAND, MAKEWPARAM(1101, BN_CLICKED), reinterpret_cast<LPARAM>(overlay));
    releasePublication.set_value();
    refresh.get();

    CHECK(refreshPaused);
    CHECK(editQueued);
    REQUIRE(waitForCondition(
        [&]() {
            const auto model = app.getDashboardViewModelForTesting();
            return !model.preferences.taskbarOverlayEnabled &&
                   model.analysis.scope.customRangeStartUtc == *editedStart && IsWindowEnabled(startDate);
        },
        std::chrono::seconds(3)));
    CHECK(app.getDashboardViewModelForTesting().analysis.scope.dateRangePreset == AnalysisDateRangePreset::Custom);
    app.shutdown();

    StorageService storage;
    REQUIRE(storage.init(dbPath.path()));
    SettingsService settings(storage);
    CHECK(settings.getBool("ui.taskbar_overlay_enabled") == false);
    CHECK(settings.getString("ui.analysis_custom_range_start_utc") == "2026-04-01T00:00:00Z");
}

TEST_CASE("AppController keeps Settings edits through a stale provider refresh", "[app][shell][preferences]") {
    requireSqlite();
    TempTestDatabase dbPath;
    TempTestDirectory exportDirectory("jistats-settings-draft");
    seedStartupPreferences(dbPath.path(), false, false, true, false, false, false, true, 1440,
                           exportDirectory.path().string());
    {
        StorageService storage;
        REQUIRE(storage.init(dbPath.path()));
        SettingsService settings(storage);
        REQUIRE(settings.setBool("ui.taskbar_overlay_enabled", true));
    }
    const auto reportPath = exportDirectory.path() / "usage.html";
    const auto provider = std::make_shared<ControlledQuotaProviderAdapter>(
        "openai", "OpenAI",
        makeQuotaResponse(std::time(nullptr), 20, 100, "interval", "openai_subscription_codex_local"));
    auto providers = std::make_shared<ProviderManager>();
    providers->addProvider(provider);

    ScopedTrayTestHandler tray([](TrayController::TestOperation, TrayStatus, const std::string&,
                                  std::string&) -> std::optional<bool> { return true; });
    std::promise<void> settingsDequeued;
    auto dequeued = settingsDequeued.get_future();
    std::promise<void> releaseSettings;
    auto released = releaseSettings.get_future().share();
    std::atomic<bool> firstEdit{true};
    AppController app;
    app.setAfterSettingsDequeueForTesting([&]() {
        if (firstEdit.exchange(false)) {
            settingsDequeued.set_value();
            released.wait_for(std::chrono::seconds(6));
        }
    });
    app.setDisableTaskbarForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providers);
    REQUIRE(app.init());
    REQUIRE(app.dispatchDashboardShellCommandForTesting(DashboardShellCommand::FileSettings));

    HWND settingsWindow = nullptr;
    HWND overlay = nullptr;
    HWND exportPath = nullptr;
    REQUIRE(waitForCondition([&]() {
        settingsWindow = FindWindowW(L"JiStatsPreferencesWindow", nullptr);
        overlay = GetDlgItem(settingsWindow, 1101);
        exportPath = GetDlgItem(settingsWindow, 1115);
        return app.isDashboardShellDialogOpenForTesting(DashboardShellDialog::Preferences) && overlay != nullptr &&
               SendMessageW(overlay, BM_GETCHECK, 0, 0) == BST_CHECKED && exportPath != nullptr &&
               IsWindowEnabled(exportPath);
    }));

    SendMessageW(overlay, BM_CLICK, 0, 0);
    const bool queued = dequeued.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
    SendMessageW(exportPath, EM_SETSEL, 0, -1);
    SendMessageW(exportPath, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(reportPath.c_str()));

    // This refresh publishes the old persisted prefs while the Settings worker is paused.
    app.refreshRuntimeForTesting();
    const int callsAfterExplicitRefresh = provider->getQuotaFetchCount();
    const bool pendingChoicePreserved = !app.getDashboardViewModelForTesting().preferences.taskbarOverlayEnabled;
    // This posted selection follows the presenter's posted stale sync on the same UI thread.
    const bool selectionPosted = PostMessageW(exportPath, EM_SETSEL, 1, 2) != FALSE;
    const bool staleSyncProcessed = selectionPosted && waitForCondition([&]() {
                                        return SendMessageW(exportPath, EM_GETSEL, 0, 0) == MAKELONG(1, 2);
                                    });
    const std::wstring draftAfterRefresh = readControlText(exportPath);
    SendMessageW(settingsWindow, WM_COMMAND, MAKEWPARAM(1115, EN_KILLFOCUS), reinterpret_cast<LPARAM>(exportPath));
    releaseSettings.set_value();
    app.shutdown();

    CHECK(queued);
    CHECK(pendingChoicePreserved);
    CHECK(staleSyncProcessed);
    CHECK(draftAfterRefresh == reportPath.wstring());
    CHECK(provider->getQuotaFetchCount() == callsAfterExplicitRefresh);
    StorageService storage;
    REQUIRE(storage.init(dbPath.path()));
    SettingsService settings(storage);
    CHECK(settings.getBool("ui.taskbar_overlay_enabled") == false);
    CHECK(settings.getString("reporting.scheduled_export_path") == reportPath.string());
}

TEST_CASE("AppController reports and retries a rejected Settings save while keeping tracking paused",
          "[app][shell][preferences]") {
    requireSqlite();
    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, true);
    const auto provider = std::make_shared<ControlledQuotaProviderAdapter>(
        "openai", "OpenAI",
        makeQuotaResponse(std::time(nullptr), 20, 100, "interval", "openai_subscription_codex_local"));
    auto providers = std::make_shared<ProviderManager>();
    providers->addProvider(provider);
    ScopedTrayTestHandler tray([](TrayController::TestOperation, TrayStatus, const std::string&,
                                  std::string&) -> std::optional<bool> { return true; });

    AppController app;
    app.setDisableTaskbarForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providers);
    REQUIRE(app.init());
    StorageService storage;
    REQUIRE(storage.init(dbPath.path()));
    SettingsService settings(storage);
    REQUIRE(settings.remove("ui.taskbar_overlay_enabled"));
    REQUIRE(storage
                .execute("CREATE TRIGGER reject_provider_opt_out BEFORE INSERT ON settings "
                         "WHEN new.key='runtime.openai_subscription_tracking_enabled' AND new.value='false' "
                         "BEGIN SELECT RAISE(FAIL, 'settings write rejected'); END;")
                .success());

    REQUIRE(app.dispatchDashboardShellCommandForTesting(DashboardShellCommand::FileSettings));
    REQUIRE(waitForCondition(
        [&app]() { return app.isDashboardShellDialogOpenForTesting(DashboardShellDialog::Preferences); }));
    REQUIRE(app.selectDashboardPreferencesTabForTesting(1));
    const HWND window = FindWindowW(L"JiStatsPreferencesWindow", nullptr);
    const HWND tracking = GetDlgItem(window, 1102);
    const HWND saveStatus = GetDlgItem(window, 1122);
    const HWND retry = GetDlgItem(window, 1123);
    REQUIRE(tracking != nullptr);
    REQUIRE(saveStatus != nullptr);
    REQUIRE(retry != nullptr);
    REQUIRE(SendMessageW(tracking, BM_GETCHECK, 0, 0) == BST_CHECKED);

    SendMessageW(tracking, BM_CLICK, 0, 0);
    REQUIRE(waitForCondition([&]() { return readControlText(saveStatus) == L"Could not save changes."; }));
    CHECK(IsWindowVisible(retry) != FALSE);
    CHECK(settings.getBool("runtime.openai_subscription_tracking_enabled") == true);
    CHECK_FALSE(settings.exists("ui.taskbar_overlay_enabled"));

    const int callsAfterFailure = provider->getQuotaFetchCount();
    app.refreshRuntimeForTesting();
    CHECK(provider->getQuotaFetchCount() == callsAfterFailure);
    CHECK_FALSE(provider->isEnabled());
    CHECK(SendMessageW(tracking, BM_GETCHECK, 0, 0) == BST_UNCHECKED);

    REQUIRE(storage.execute("DROP TRIGGER reject_provider_opt_out;").success());
    SendMessageW(retry, BM_CLICK, 0, 0);
    REQUIRE(waitForCondition([&]() {
        return settings.getBool("runtime.openai_subscription_tracking_enabled") == false &&
               readControlText(saveStatus).empty() && IsWindowVisible(retry) == FALSE;
    }));
    CHECK_FALSE(provider->isEnabled());
    app.shutdown();
}

TEST_CASE("AppController schedules exports only after an accepted Settings save", "[app][shell][preferences]") {
    requireSqlite();
    TempTestDatabase dbPath;
    TempTestDirectory exportDirectory("jistats-rejected-schedule");
    seedStartupPreferences(dbPath.path(), false, false, false, false, false, false, false, 1440,
                           exportDirectory.path().string());
    ScopedTrayTestHandler tray([](TrayController::TestOperation, TrayStatus, const std::string&,
                                  std::string&) -> std::optional<bool> { return true; });
    std::promise<void> firstEditDequeued;
    auto dequeued = firstEditDequeued.get_future();
    std::promise<void> releaseFirstEdit;
    auto released = releaseFirstEdit.get_future().share();
    std::atomic<bool> firstEdit{true};
    AppController app;
    app.setAfterSettingsDequeueForTesting([&]() {
        if (firstEdit.exchange(false)) {
            firstEditDequeued.set_value();
            released.wait_for(std::chrono::seconds(6));
        }
    });
    app.setDisableTaskbarForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(std::make_shared<ProviderManager>());
    REQUIRE(app.init());

    StorageService storage;
    REQUIRE(storage.init(dbPath.path()));
    SettingsService settings(storage);
    REQUIRE(storage
                .execute("CREATE TRIGGER reject_schedule BEFORE INSERT ON settings "
                         "WHEN new.key='reporting.scheduled_export_enabled' AND new.value='true' "
                         "BEGIN SELECT RAISE(FAIL, 'schedule write rejected'); END;")
                .success());
    const auto attempts = [&]() {
        const auto result = storage.execute("SELECT COUNT(*) FROM export_runs WHERE run_kind = 'scheduled_html'");
        REQUIRE(result.success());
        return result.getInt(0, 0);
    };

    REQUIRE(app.dispatchDashboardShellCommandForTesting(DashboardShellCommand::FileSettings));
    REQUIRE(waitForCondition(
        [&]() { return app.isDashboardShellDialogOpenForTesting(DashboardShellDialog::Preferences); }));
    const HWND window = FindWindowW(L"JiStatsPreferencesWindow", nullptr);
    const HWND overlay = GetDlgItem(window, 1101);
    const HWND schedule = GetDlgItem(window, 1108);
    const HWND saveStatus = GetDlgItem(window, 1122);
    const HWND retry = GetDlgItem(window, 1123);
    REQUIRE(overlay != nullptr);
    REQUIRE(schedule != nullptr);
    REQUIRE(saveStatus != nullptr);
    REQUIRE(retry != nullptr);

    SendMessageW(overlay, BM_CLICK, 0, 0);
    const bool paused = dequeued.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
    SendMessageW(schedule, BM_CLICK, 0, 0);
    releaseFirstEdit.set_value();

    CHECK(paused);
    REQUIRE(waitForCondition([&]() { return readControlText(saveStatus) == L"Could not save changes."; },
                             std::chrono::seconds(5)));
    CHECK(settings.getBool("reporting.scheduled_export_enabled") == false);
    CHECK(attempts() == 0);
    CHECK(std::filesystem::is_empty(exportDirectory.path()));

    REQUIRE(storage.execute("DROP TRIGGER reject_schedule;").success());
    SendMessageW(retry, BM_CLICK, 0, 0);
    REQUIRE(waitForCondition([&]() { return attempts() > 0; }, std::chrono::seconds(5)));
    CHECK(settings.getBool("reporting.scheduled_export_enabled") == true);
    CHECK_FALSE(std::filesystem::is_empty(exportDirectory.path()));
    app.shutdown();
}

TEST_CASE("AppController restores quota after queued provider tracking is turned off and back on",
          "[app][shell][preferences]") {
    requireSqlite();
    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, true);
    {
        StorageService storage;
        REQUIRE(storage.init(dbPath.path()));
        SettingsService settings(storage);
        REQUIRE(settings.setBool("ui.taskbar_overlay_enabled", true));
    }
    auto providers = makeProviderManager({makeCurrentQuotaProvider("openai", "OpenAI", 20, 100, std::time(nullptr))});
    ScopedTrayTestHandler tray([](TrayController::TestOperation, TrayStatus, const std::string&,
                                  std::string&) -> std::optional<bool> { return true; });
    std::promise<void> settingsDequeued;
    auto dequeued = settingsDequeued.get_future();
    std::promise<void> releaseSettings;
    auto released = releaseSettings.get_future().share();
    std::atomic<bool> firstEdit{true};
    AppController app;
    app.setAfterSettingsDequeueForTesting([&]() {
        if (firstEdit.exchange(false)) {
            settingsDequeued.set_value();
            released.wait_for(std::chrono::seconds(6));
        }
    });
    app.setDisableTaskbarForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providers);
    REQUIRE(app.init());
    const auto showsOpenAI = [&]() {
        const auto view = app.getDashboardViewModelForTesting();
        return std::any_of(view.providers.begin(), view.providers.end(), [](const auto& provider) {
            return provider.providerKey == "openai" && !provider.quotas.empty();
        });
    };
    REQUIRE(showsOpenAI());
    REQUIRE(app.dispatchDashboardShellCommandForTesting(DashboardShellCommand::FileSettings));

    HWND settings = nullptr;
    HWND overlay = nullptr;
    HWND tracking = nullptr;
    REQUIRE(waitForCondition([&]() {
        settings = FindWindowW(L"JiStatsPreferencesWindow", nullptr);
        overlay = GetDlgItem(settings, 1101);
        tracking = GetDlgItem(settings, 1102);
        return app.isDashboardShellDialogOpenForTesting(DashboardShellDialog::Preferences) && overlay != nullptr &&
               SendMessageW(overlay, BM_GETCHECK, 0, 0) == BST_CHECKED && tracking != nullptr &&
               SendMessageW(tracking, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }));

    SendMessageW(overlay, BM_CLICK, 0, 0);
    const bool queued = dequeued.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
    const bool switchedToProviderTab = app.selectDashboardPreferencesTabForTesting(1);
    const bool trackingVisible = waitForCondition([&]() { return IsWindowVisible(tracking) != FALSE; });
    SendMessageW(tracking, BM_CLICK, 0, 0);
    app.refreshRuntimeForTesting();
    const bool removedDuringOptOut = !showsOpenAI();
    SendMessageW(tracking, BM_CLICK, 0, 0);
    const bool requestedReenable = app.getDashboardViewModelForTesting().preferences.openaiSubscriptionTrackingEnabled;
    releaseSettings.set_value();

    CHECK(queued);
    CHECK(switchedToProviderTab);
    CHECK(trackingVisible);
    CHECK(removedDuringOptOut);
    CHECK(requestedReenable);
    CHECK(waitForCondition(showsOpenAI, std::chrono::seconds(5)));
    app.shutdown();
}

TEST_CASE("AppController skips new quota requests after Settings dequeues an opt-out", "[app][runtime][polling]") {
    requireSqlite();
    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, true, false, true);
    TempTestDirectory emptyProviderHome("jistats-opt-out-provider-home");
    ScopedEnvironmentVariable codexHome("CODEX_HOME", emptyProviderHome.path().string());
    ScopedEnvironmentVariable claudeHome("CLAUDE_CONFIG_DIR", emptyProviderHome.path().string());

    const auto now = std::time(nullptr);
    std::promise<void> releaseFirst;
    auto first = std::make_shared<ControlledQuotaProviderAdapter>(
        "claude_code", "Claude Code", makeQuotaResponse(now, 0, 100, "local", "claude_code_local"),
        releaseFirst.get_future().share(), 2);
    auto openai = std::make_shared<ControlledQuotaProviderAdapter>(
        "openai", "OpenAI", makeQuotaResponse(now, 20, 100, "interval", "openai_subscription_codex_local"));
    auto providers = std::make_shared<ProviderManager>();
    providers->addProvider(first);
    providers->addProvider(openai);

    ScopedTrayTestHandler tray([](TrayController::TestOperation, TrayStatus, const std::string&,
                                  std::string&) -> std::optional<bool> { return true; });
    std::promise<void> settingsDequeued;
    auto dequeued = settingsDequeued.get_future();
    std::promise<void> releaseSettings;
    auto settingsReleased = releaseSettings.get_future();
    AppController app;
    app.setAfterSettingsDequeueForTesting([&]() {
        settingsDequeued.set_value();
        settingsReleased.wait_for(std::chrono::seconds(15));
    });
    app.setDisableTaskbarForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providers);
    REQUIRE(app.init());
    REQUIRE(app.dispatchDashboardShellCommandForTesting(DashboardShellCommand::FileSettings));

    HWND checkbox = nullptr;
    REQUIRE(waitForCondition([&]() {
        checkbox = GetDlgItem(FindWindowW(L"JiStatsPreferencesWindow", nullptr), 1102);
        return checkbox != nullptr && SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }));
    const int fetchedBeforeOptOut = openai->getQuotaFetchCount();
    REQUIRE(fetchedBeforeOptOut > 0);

    auto underway = std::async(std::launch::async, [&]() { app.refreshRuntimeForTesting(); });
    const bool heldBeforeOpenAI =
        waitForCondition([&]() { return first->getQuotaFetchCount() > first->getCompletedQuotaFetchCount(); });

    SendMessageW(checkbox, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(GetParent(checkbox), WM_COMMAND, MAKEWPARAM(1102, BN_CLICKED), reinterpret_cast<LPARAM>(checkbox));
    const bool optOutDequeued = dequeued.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    auto competing = std::async(std::launch::async, [&]() { app.refreshRuntimeForTesting(); });
    releaseFirst.set_value();
    underway.get();
    competing.get();

    // Both refreshes finished while the opt-out was active and the old setting was still persisted.
    const int fetchedBeforeSettingsSave = openai->getQuotaFetchCount();
    StorageService storage;
    const bool opened = storage.init(dbPath.path());
    SettingsService settings(storage);
    const auto optedInBeforeSettingsSave =
        opened ? settings.getBool("runtime.openai_subscription_tracking_enabled") : std::nullopt;
    releaseSettings.set_value();
    app.shutdown();

    CHECK(heldBeforeOpenAI);
    CHECK(optOutDequeued);
    CHECK(fetchedBeforeSettingsSave == fetchedBeforeOptOut);
    CHECK(optedInBeforeSettingsSave == true);
    REQUIRE(opened);
    CHECK(settings.getBool("runtime.openai_subscription_tracking_enabled") == false);
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

TEST_CASE("AppController shows the accepted MiniMax account name after saving a blank draft", "[app][shell][minimax]") {
    requireSqlite();
    TempTestDatabase dbPath;
    seedMiniMaxCredentialMetadata(dbPath.path(), "coding_plan", "Retained Coding Plan");
    auto store = std::make_shared<WindowsCredentialStore>(makeFakeCredentialBackend());
    ScopedTrayTestHandler tray([](TrayController::TestOperation, TrayStatus, const std::string&,
                                  std::string&) -> std::optional<bool> { return true; });

    AppController app;
    app.setDisableTaskbarForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(std::make_shared<ProviderManager>());
    app.setMiniMaxCredentialStoreForTesting(store);
    REQUIRE(app.init());
    REQUIRE(app.dispatchDashboardShellCommandForTesting(DashboardShellCommand::FileSettings));
    REQUIRE(waitForCondition(
        [&app]() { return app.isDashboardShellDialogOpenForTesting(DashboardShellDialog::Preferences); }));
    REQUIRE(app.selectDashboardPreferencesTabForTesting(2));
    const HWND window = FindWindowW(L"JiStatsPreferencesWindow", nullptr);
    const HWND name = GetDlgItem(window, 1111);
    const HWND secret = GetDlgItem(window, 1112);
    const HWND save = GetDlgItem(window, 1113);
    REQUIRE(name != nullptr);
    REQUIRE(secret != nullptr);
    REQUIRE(save != nullptr);
    REQUIRE(readControlText(name) == L"Retained Coding Plan");
    SendMessageW(name, EM_SETSEL, 0, -1);
    SendMessageW(name, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
    REQUIRE(readControlText(name).empty());
    REQUIRE(SendMessageW(name, EM_GETMODIFY, 0, 0) != FALSE);
    REQUIRE(SetWindowTextW(secret, L"sk-cp-test"));
    SendMessageW(save, BM_CLICK, 0, 0);

    REQUIRE(waitForCondition([&]() { return readControlText(name) == L"Retained Coding Plan"; }));
    CHECK(SendMessageW(name, EM_GETMODIFY, 0, 0) == FALSE);
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
    TempTestDirectory userProfileRoot("jistats_codex_refresh");
    const auto rolloutPath =
        userProfileRoot.path() / ".codex" / "sessions" / "2026" / "04" / "19" / "rollout-live.jsonl";
    const std::time_t now = std::time(nullptr);

    if (GENERATE(true, false)) {
        writeCodexRolloutFile(rolloutPath, 25.0, now, now + (5 * 3600));
    }
    ScopedEnvironmentVariable codexHome(
        "CODEX_HOME",
        (userProfileRoot.path() / ".codex").string());

    auto openaiProvider = std::make_shared<ControlledQuotaProviderAdapter>(
        "openai", "OpenAI", makeQuotaResponse(now, 25, 100, "interval", "openai_subscription_codex_local"));
    auto providerManager = std::make_shared<ProviderManager>();
    providerManager->addProvider(openaiProvider);
    std::atomic<int> rolloutSamples{0};

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDisableTaskbarForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(providerManager);
    app.setOpenAICodexRefreshPollIntervalForTesting(25);
    app.setBeforeCodexRolloutSampleForTesting([&]() { ++rolloutSamples; });

    REQUIRE(app.init());
    REQUIRE_FALSE(app.getDashboardViewModelForTesting().preferences.openaiSubscriptionTrackingEnabled);
    CHECK(openaiProvider->getQuotaFetchCount() == 0);
    CHECK(rolloutSamples.load() == 0);

    auto preferences = app.getDashboardViewModelForTesting().preferences;
    preferences.openaiSubscriptionTrackingEnabled = true;
    app.applyDashboardPreferencesForTesting(preferences);

    auto hasQuota = [&app](long long used) {
        const auto model = app.getDashboardViewModelForTesting();
        const auto* provider = findProviderStatus(model, "openai");
        return provider && !provider->quotas.empty() && provider->quotas.front().used == used;
    };
    REQUIRE(hasQuota(25));
    REQUIRE(waitForCondition([&]() { return rolloutSamples.load() >= 3; }));
    CHECK(openaiProvider->getQuotaFetchCount() == 1);
    const auto refreshQuota = [&](const std::filesystem::path& changedPath, long long used, int offsetSeconds) {
        openaiProvider->setQuotaResponse(
            makeQuotaResponse(now + offsetSeconds, used, 100, "interval", "openai_subscription_codex_local"));
        writeCodexRolloutFile(changedPath, static_cast<double>(used), now + offsetSeconds, now + (5 * 3600));
        std::error_code error;
        std::filesystem::last_write_time(
            changedPath, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(offsetSeconds), error);
        REQUIRE_FALSE(error);
        REQUIRE(waitForCondition([&]() { return hasQuota(used); }, std::chrono::milliseconds(1500)));
    };
    refreshQuota(rolloutPath, 40, 60);
    refreshQuota(rolloutPath.parent_path().parent_path() / "20" / "rollout-new.jsonl", 65, 120);

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

TEST_CASE("AppController retains successful Dashboard quota readings through an offline refresh",
          "[app][runtime][quota_retention]") {
    requireSqlite();

    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, true, true);
    const std::time_t now = std::time(nullptr);
    auto reading = makeQuotaResponse(now - 60, 25, 100, "interval", "openai_subscription_codex_local");
    reading.windowEnd = now + 3600;
    reading.secondaryWindowType = "weekly";
    reading.secondaryWindowEnd = now + 7 * 86400;
    reading.secondaryUsed = 38;
    reading.secondaryLimit = 100;
    reading.secondaryRemaining = 62;

    auto openai = std::make_shared<ControlledQuotaProviderAdapter>("openai", "OpenAI", reading);
    auto minimax = std::make_shared<ControlledQuotaProviderAdapter>(
        "minimax", "MiniMax", makeQuotaResponse(now, 20, 100, "interval", "minimax_subscription_coding_plan"));
    auto manager = std::make_shared<ProviderManager>();
    manager->addProvider(openai);
    manager->addProvider(minimax);

    AppController app;
    app.setHeadlessForTesting(true);
    app.setTaskbarTestModeForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(manager);
    REQUIRE(app.init());
    const auto original = app.getDashboardViewModelForTesting();
    const auto* originalProvider = findProviderStatus(original, "openai");
    REQUIRE(originalProvider != nullptr);
    REQUIRE(originalProvider->state == ProviderState::Live);
    REQUIRE(parseRuntimeTimestamp(originalProvider->lastSyncTime) == reading.snapshotTime);
    CHECK_FALSE(originalProvider->lastKnownQuota.has_value());
    const auto originalMetrics = app.getTaskbarMetricsForTesting();
    REQUIRE(findTaskbarMetric(originalMetrics, "openai") != nullptr);

    openai->setQuotaResponse(makeFailedQuotaResponse("network unreachable"));
    for (int attempt = 0; attempt < 2; ++attempt) {
        app.refreshRuntimeForTesting();
        const auto offline = app.getDashboardViewModelForTesting();
        const auto* offlineProvider = findProviderStatus(offline, "openai");
        REQUIRE(offlineProvider != nullptr);
        CHECK(offline.runtimeState == RuntimeState::Offline);
        CHECK(offlineProvider->state == ProviderState::Offline);
        CHECK(offlineProvider->accountId == originalProvider->accountId);
        CHECK(offlineProvider->quotas.empty());
        REQUIRE(offlineProvider->lastKnownQuota.has_value());
        const auto& retained = *offlineProvider->lastKnownQuota;
        CHECK(retained.lastSyncTime == originalProvider->lastSyncTime);
        REQUIRE(retained.quotas.size() == 1);
        const auto& quota = retained.quotas.front();
        CHECK(quota.remainingPercentage == Catch::Approx(75.0));
        CHECK(quota.windowResetAt == reading.windowEnd);
        CHECK(quota.secondaryRemainingPercentage == Catch::Approx(62.0));
        CHECK(quota.secondaryWindowResetAt == reading.secondaryWindowEnd);
        CHECK(quota.providerSurface == reading.providerSurface);

        const auto summaries = app.getDashboardUsageProgressSummariesForTesting();
        const auto summary = findUsageSummary(summaries, "OpenAI Subscription");
        REQUIRE(summary.has_value());
        CHECK(summary->percentage == 75);
        CHECK(summary->secondaryPercentage == 62);
        CHECK(findTaskbarMetric(app.getTaskbarMetricsForTesting(), "openai") == nullptr);
    }

    auto failedMiniMax = makeFailedQuotaResponse("network unreachable");
    failedMiniMax.providerSurface = "minimax_subscription_coding_plan";
    minimax->setQuotaResponse(failedMiniMax);
    app.refreshRuntimeForTesting();
    const auto samePlan = app.getDashboardViewModelForTesting();
    const auto* sameMiniMax = findProviderStatus(samePlan, "minimax");
    REQUIRE(sameMiniMax != nullptr);
    REQUIRE(sameMiniMax->lastKnownQuota.has_value());
    CHECK(sameMiniMax->lastKnownQuota->quotas.front().providerSurface == failedMiniMax.providerSurface);

    failedMiniMax.providerSurface = "minimax_subscription_token_plan";
    minimax->setQuotaResponse(failedMiniMax);
    app.refreshRuntimeForTesting();
    const auto switchedPlan = app.getDashboardViewModelForTesting();
    const auto* switchedMiniMax = findProviderStatus(switchedPlan, "minimax");
    REQUIRE(switchedMiniMax != nullptr);
    CHECK(switchedMiniMax->state == ProviderState::Offline);
    CHECK(switchedMiniMax->quotas.empty());
    CHECK_FALSE(switchedMiniMax->lastKnownQuota.has_value());

    auto preferences = original.preferences;
    preferences.openaiSubscriptionTrackingEnabled = false;
    app.applyDashboardPreferencesForTesting(preferences);
    const auto disabled = app.getDashboardViewModelForTesting();
    CHECK(findProviderStatus(disabled, "openai") == nullptr);

    preferences.openaiSubscriptionTrackingEnabled = true;
    app.applyDashboardPreferencesForTesting(preferences);
    const auto reenabled = app.getDashboardViewModelForTesting();
    const auto* unavailable = findProviderStatus(reenabled, "openai");
    REQUIRE(unavailable != nullptr);
    CHECK(unavailable->state == ProviderState::Offline);
    CHECK(unavailable->quotas.empty());
    CHECK_FALSE(unavailable->lastKnownQuota.has_value());
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

TEST_CASE("AppController shows an enabled Kimi account without OAuth quota credentials", "[app][runtime][kimi]") {
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
    app.setTaskbarTestModeForTesting(true);
    auto providerManager = std::make_shared<ProviderManager>();
    providerManager->addProvider(std::make_shared<KimiAdapter>(homeRoot.path().string()));
    app.setProviderManagerForTesting(providerManager);
    REQUIRE(app.init());

    const DashboardViewModel viewModel = app.getDashboardViewModelForTesting();
    CHECK(viewModel.kimiCodeLocallyDetected);
    CHECK(viewModel.kimiCodeDetectionMessage.find("Detected at ~/.kimi") != std::string::npos);
    const auto* kimi = findProviderStatus(viewModel, "kimi");
    REQUIRE(kimi != nullptr);
    CHECK(kimi->isEnabled);
    CHECK(kimi->accountId == "kimi.runtime_status");
    CHECK(kimi->state == ProviderState::Sample);
    CHECK(kimi->quotas.empty());
    CHECK_FALSE(kimi->lastKnownQuota.has_value());
    CHECK(findTaskbarMetric(app.getTaskbarMetricsForTesting(), "kimi") == nullptr);
    CHECK(viewModel.runtimeState != RuntimeState::Offline);
    CHECK(viewModel.runtimeState == RuntimeState::Unknown);
    CHECK_FALSE(viewModel.isOnline);
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
    const auto* kimi = findProviderStatus(viewModel, "kimi");
    REQUIRE(kimi != nullptr);
    CHECK(kimi->state == ProviderState::Sample);
    CHECK(kimi->quotas.empty());

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
    const auto* initialKimi = findProviderStatus(initialViewModel, "kimi");
    REQUIRE(initialKimi != nullptr);
    CHECK(initialKimi->state == ProviderState::Sample);
    CHECK(initialKimi->quotas.empty());

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
    const auto* refreshedKimi = findProviderStatus(refreshedViewModel, "kimi");
    REQUIRE(refreshedKimi != nullptr);
    CHECK(refreshedKimi->state == ProviderState::Sample);
    CHECK(refreshedKimi->quotas.empty());

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

    std::atomic<bool> sampling{false};
    std::promise<void> releaseSample;
    auto released = releaseSample.get_future().share();
    app.setBeforeCodexRolloutSampleForTesting([&]() {
        if (!sampling.exchange(true)) {
            released.wait_for(std::chrono::seconds(5));
        }
    });

    auto startup = std::async(std::launch::async, [&]() { return app.init(); });
    const bool sampleHeld = waitForCondition([&]() { return sampling.load(); }, std::chrono::seconds(2));
    const bool initFinished = startup.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    const auto beforeSampling = app.getDashboardViewModelForTesting();
    const int fetchesBeforeSample = provider->getQuotaFetchCount();
    releaseSample.set_value();

    REQUIRE(startup.get());
    CHECK(sampleHeld);
    CHECK(initFinished);
    CHECK(fetchesBeforeSample == 0);
    const auto* cached = findProviderStatus(beforeSampling, "openai");
    REQUIRE(cached != nullptr);
    REQUIRE_FALSE(cached->quotas.empty());
    CHECK(cached->quotas.front().used == 60);
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

TEST_CASE("AppController retains only matching MiniMax quota after a failed startup refresh",
          "[app][runtime][quota_retention]") {
    requireSqlite();
    const std::string plan =
        GENERATE("minimax_subscription_coding_plan:interval", "minimax_subscription_token_plan:interval", "minimax");
    CAPTURE(plan);
    TempTestDatabase dbPath;
    seedStartupPreferences(dbPath.path(), false, false, false, true, false, false);
    const auto snapshotTime = std::time(nullptr) - 180;
    seedPersistedQuotaSnapshot(dbPath.path(), "minimax", "acct-minimax-startup", "MiniMax Startup Account", plan,
                               "MiniMax", "interval", snapshotTime, 420, 1000, 580);

    auto failure = makeFailedQuotaResponse("network unreachable");
    failure.providerSurface = "minimax_subscription_coding_plan";
    auto provider = std::make_shared<ControlledQuotaProviderAdapter>("minimax", "MiniMax", failure);
    auto manager = std::make_shared<ProviderManager>();
    manager->addProvider(provider);

    AppController app;
    app.setHeadlessForTesting(true);
    app.setDisableTaskbarForTesting(true);
    app.setDatabasePathForTesting(dbPath.path());
    app.setProviderManagerForTesting(manager);
    REQUIRE(app.init());
    CHECK(provider->getQuotaFetchCount() == 0);

    app.refreshRuntimeForTesting();
    const auto refreshed = app.getDashboardViewModelForTesting();
    const auto* status = findProviderStatus(refreshed, "minimax");
    REQUIRE(status != nullptr);
    CHECK(status->state == ProviderState::Offline);
    CHECK(status->quotas.empty());
    if (plan == "minimax_subscription_coding_plan:interval") {
        REQUIRE(status->lastKnownQuota.has_value());
        CHECK(parseRuntimeTimestamp(status->lastKnownQuota->lastSyncTime) == snapshotTime);
        REQUIRE(status->lastKnownQuota->quotas.size() == 1);
        CHECK(status->lastKnownQuota->quotas.front().remaining == 580);
        CHECK(status->lastKnownQuota->quotas.front().providerSurface == failure.providerSurface);
    } else {
        CHECK_FALSE(status->lastKnownQuota.has_value());
    }
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

namespace {

constexpr const char* kCodexAttributionSetting = "runtime.codex_model_attribution_generation";

class CodexAttributionTestProfile {
  public:
    CodexAttributionTestProfile() {
        REQUIRE(source.init((codexHome.path() / "state_5.sqlite").string()));
        // Each fixture update appends a WAL frame even when the main file timestamp is unchanged.
        const auto journal = source.execute("PRAGMA journal_mode = WAL;");
        REQUIRE(journal.success());
        REQUIRE(journal.getString(0, 0) == "wal");
        REQUIRE(
            source
                .execute("CREATE TABLE threads (id TEXT PRIMARY KEY, model_provider TEXT, model TEXT, "
                         "reasoning_effort TEXT, tokens_used INTEGER, created_at_ms INTEGER, updated_at_ms INTEGER);")
                .success());
        const std::string createdAtMs = std::to_string((std::time(nullptr) - 60) * 1000LL);
        REQUIRE(source
                    .execute("INSERT INTO threads VALUES ('thread-1', 'hicap-ai', 'gpt-test', 'high', 100, ?, ?);",
                             {createdAtMs, createdAtMs})
                    .success());
        REQUIRE(storage.init(database.path()));
    }

    int generation() {
        return SettingsService(storage).getInt(kCodexAttributionSetting).value_or(0);
    }

    int sourceCount() {
        const auto result = storage.execute("SELECT COUNT(*) FROM codex_local_sources;");
        REQUIRE(result.success());
        return result.getInt(0, 0);
    }

    long long tokens() {
        const auto result = storage.execute("SELECT COALESCE(SUM(total_tokens), 0) FROM usage_events "
                                            "WHERE source_type = 'codex_local_sqlite';");
        REQUIRE(result.success());
        return result.getInt64(0, 0);
    }

    void updateSource(int tokens) {
        REQUIRE(source
                    .execute("UPDATE threads SET tokens_used = ?, updated_at_ms = updated_at_ms + 1000 "
                             "WHERE id = 'thread-1';",
                             {std::to_string(tokens)})
                    .success());
    }

    void startController(AppController& app, bool headless = true) {
        app.setHeadlessForTesting(headless);
        app.setDisableTaskbarForTesting(true);
        app.setDatabasePathForTesting(database.path());
        REQUIRE(app.init());
    }

    TempTestDatabase database;
    StorageService storage;

  private:
    TempTestDirectory codexHome{"jistats_codex_attribution"};
    ScopedEnvironmentVariable codexHomeVariable{"CODEX_HOME", codexHome.path().string()};
    StorageService source;
};

} // namespace

TEST_CASE("AppController requires explicit consent to read Codex SQLite model attribution", "[app][runtime][codex]") {
    requireSqlite();

    CodexAttributionTestProfile profile;
    seedStartupPreferences(profile.database.path(), false, false, true);

    AppController app;
    profile.startController(app);
    DashboardPreferences preferences = app.getDashboardViewModelForTesting().preferences;
    REQUIRE(preferences.openaiSubscriptionTrackingEnabled);
    CHECK_FALSE(preferences.codexModelAttributionEnabled);
    CHECK(profile.sourceCount() == 0);

    profile.updateSource(200);
    app.refreshRuntimeForTesting();
    CHECK(profile.sourceCount() == 0);
    CHECK(profile.tokens() == 0);

    preferences.openaiSubscriptionTrackingEnabled = false;
    preferences.codexModelAttributionEnabled = true;
    app.applyDashboardPreferencesForTesting(preferences);
    REQUIRE(app.getDashboardViewModelForTesting().preferences.codexModelAttributionEnabled);
    REQUIRE(profile.sourceCount() == 1);
    CHECK(profile.tokens() == 0);

    profile.updateSource(250);
    app.refreshRuntimeForTesting();
    CHECK(profile.tokens() == 50);
    app.shutdown();

    profile.updateSource(300);
    AppController restarted;
    profile.startController(restarted);
    preferences = restarted.getDashboardViewModelForTesting().preferences;
    CHECK_FALSE(preferences.openaiSubscriptionTrackingEnabled);
    REQUIRE(preferences.codexModelAttributionEnabled);
    CHECK(profile.sourceCount() == 1);
    CHECK(profile.tokens() == 100);

    AppController peer;
    profile.startController(peer);
    REQUIRE(peer.getDashboardViewModelForTesting().preferences.codexModelAttributionEnabled);

    preferences.codexModelAttributionEnabled = false;
    restarted.applyDashboardPreferencesForTesting(preferences);
    CHECK_FALSE(restarted.getDashboardViewModelForTesting().preferences.codexModelAttributionEnabled);
    profile.updateSource(900);
    restarted.refreshRuntimeForTesting();
    peer.refreshRuntimeForTesting();
    CHECK_FALSE(peer.getDashboardViewModelForTesting().preferences.codexModelAttributionEnabled);
    CHECK(profile.tokens() == 100);
    peer.shutdown();
    restarted.shutdown();

    AppController disabled;
    profile.startController(disabled);
    preferences = disabled.getDashboardViewModelForTesting().preferences;
    REQUIRE_FALSE(preferences.codexModelAttributionEnabled);
    CHECK(profile.tokens() == 100);

    preferences.codexModelAttributionEnabled = true;
    disabled.applyDashboardPreferencesForTesting(preferences);
    REQUIRE(profile.sourceCount() == 2);
    CHECK(profile.tokens() == 100);
    profile.updateSource(950);
    disabled.refreshRuntimeForTesting();
    CHECK(profile.tokens() == 150);
}

TEST_CASE("AppController serializes a peer Codex scan with profile revocation", "[app][runtime][codex]") {
    requireSqlite();

    CodexAttributionTestProfile profile;
    REQUIRE(SettingsService(profile.storage).setInt(kCodexAttributionSetting, 1));
    AppController revoking;
    profile.startController(revoking);
    AppController scanning;
    profile.startController(scanning);
    REQUIRE(profile.sourceCount() == 1);

    std::promise<void> aboutToRead;
    auto sourceReadReached = aboutToRead.get_future();
    std::promise<void> releaseRead;
    auto sourceReadReleased = releaseRead.get_future();
    scanning.setBeforeCodexSourceReadForTesting([&]() {
        aboutToRead.set_value();
        sourceReadReleased.wait_for(std::chrono::seconds(5));
    });
    profile.updateSource(150);
    auto peerScan = std::async(std::launch::async, [&]() { scanning.refreshRuntimeForTesting(); });
    REQUIRE(sourceReadReached.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    auto preferences = revoking.getDashboardViewModelForTesting().preferences;
    preferences.codexModelAttributionEnabled = false;
    std::promise<void> attemptOff;
    auto offStarted = attemptOff.get_future();
    auto peerRevocation = std::async(std::launch::async, [&]() {
        attemptOff.set_value();
        revoking.applyDashboardPreferencesForTesting(preferences);
    });
    REQUIRE(offStarted.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const bool savedBeforeRead =
        waitForCondition([&]() { return profile.generation() == -1; }, std::chrono::milliseconds(600));
    CHECK_FALSE(savedBeforeRead);
    if (savedBeforeRead) {
        // Reproduce a new counter arriving after a durable Off while the old scan is paused.
        profile.updateSource(250);
    }
    releaseRead.set_value();
    peerScan.get();
    peerRevocation.get();
    scanning.setBeforeCodexSourceReadForTesting({});

    REQUIRE(profile.generation() == -1);
    CHECK(profile.tokens() == 50);
    profile.updateSource(300);
    scanning.refreshRuntimeForTesting();
    CHECK(profile.tokens() == 50);
    CHECK_FALSE(scanning.getDashboardViewModelForTesting().preferences.codexModelAttributionEnabled);
}

TEST_CASE("AppController revokes Codex consent with restricted profile mutex access", "[app][runtime][codex]") {
    requireSqlite();

    CodexAttributionTestProfile profile;
    REQUIRE(SettingsService(profile.storage).setInt(kCodexAttributionSetting, 1));
    AppController app;
    profile.startController(app);
    REQUIRE(profile.sourceCount() == 1);

    const auto path = std::filesystem::path(profile.database.path()).wstring();
    const HANDLE database =
        CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(database != INVALID_HANDLE_VALUE);
    BY_HANDLE_FILE_INFORMATION identity{};
    const bool identified = GetFileInformationByHandle(database, &identity) != FALSE;
    CloseHandle(database);
    REQUIRE(identified);
    const std::wstring name = L"Global\\JiStats.CodexAttribution." + std::to_wstring(identity.dwVolumeSerialNumber) +
                              L"." + std::to_wstring(identity.nFileIndexHigh) + L"." +
                              std::to_wstring(identity.nFileIndexLow);

    // Model a peer that can use the lock but cannot request all access.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;SD;;;WD)(A;;0x00100001;;;WD)",
                                                                 SDDL_REVISION_1, &descriptor, nullptr));
    const std::unique_ptr<void, decltype(&LocalFree)> security(descriptor, LocalFree);
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security.get(), FALSE};
    const std::unique_ptr<void, decltype(&CloseHandle)> held(
        CreateMutexExW(&attributes, name.c_str(), CREATE_MUTEX_INITIAL_OWNER, SYNCHRONIZE | MUTEX_MODIFY_STATE),
        CloseHandle);
    const DWORD creationStatus = GetLastError();
    REQUIRE(held);
    REQUIRE(creationStatus != ERROR_ALREADY_EXISTS);

    const std::unique_ptr<void, decltype(&CloseHandle)> fullAccess(OpenMutexW(MUTEX_ALL_ACCESS, FALSE, name.c_str()),
                                                                   CloseHandle);
    const DWORD accessStatus = GetLastError();
    REQUIRE_FALSE(fullAccess);
    REQUIRE(accessStatus == ERROR_ACCESS_DENIED);

    auto preferences = app.getDashboardViewModelForTesting().preferences;
    preferences.codexModelAttributionEnabled = false;
    std::promise<void> attemptingOff;
    auto offStarted = attemptingOff.get_future();
    auto off = std::async(std::launch::async, [&]() {
        attemptingOff.set_value();
        app.applyDashboardPreferencesForTesting(preferences);
    });
    CHECK(offStarted.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK_FALSE(waitForCondition([&]() { return profile.generation() == -1; }, std::chrono::milliseconds(200)));
    REQUIRE(ReleaseMutex(held.get()));
    off.get();

    CHECK(profile.generation() == -1);
    CHECK_FALSE(app.getDashboardViewModelForTesting().preferences.codexModelAttributionEnabled);
    const DWORD wait = WaitForSingleObject(held.get(), 0);
    CHECK(wait == WAIT_OBJECT_0);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        CHECK(ReleaseMutex(held.get()));
    }
}

TEST_CASE("AppController preserves queued Codex revocation and retries a failed save", "[app][runtime][codex]") {
    requireSqlite();

    CodexAttributionTestProfile profile;
    REQUIRE(SettingsService(profile.storage).setInt(kCodexAttributionSetting, 1));
    ScopedTrayTestHandler tray([](TrayController::TestOperation, TrayStatus, const std::string&,
                                  std::string&) -> std::optional<bool> { return true; });
    std::promise<void> settingsDequeued;
    auto dequeued = settingsDequeued.get_future();
    std::promise<void> releaseSettings;
    const auto settingsReleased = releaseSettings.get_future();

    AppController app;
    app.setStartupManagerTestHooks(makeStartupHooks(std::make_shared<FakeStartupHooksState>()));
    app.setAfterSettingsDequeueForTesting([&]() {
        settingsDequeued.set_value();
        settingsReleased.wait_for(std::chrono::seconds(15));
    });
    profile.startController(app, false);
    REQUIRE(profile.sourceCount() == 1);
    REQUIRE(app.dispatchDashboardShellCommandForTesting(DashboardShellCommand::FileSettings));
    REQUIRE(waitForCondition(
        [&app]() { return app.isDashboardShellDialogOpenForTesting(DashboardShellDialog::Preferences); }));
    const HWND settings = FindWindowW(L"JiStatsPreferencesWindow", nullptr);
    REQUIRE(settings != nullptr);
    const HWND startup = FindWindowExW(settings, nullptr, L"Button", L"Start with Windows");
    REQUIRE(startup != nullptr);

    const auto selectAttributionTab = [&]() {
        REQUIRE(app.selectDashboardPreferencesTabForTesting(1));
        const HWND checkbox = FindWindowExW(settings, nullptr, L"Button", L"Enable Codex model attribution");
        REQUIRE(checkbox != nullptr);
        REQUIRE(waitForCondition([checkbox]() { return IsWindowVisible(checkbox) != FALSE; }));
        return checkbox;
    };

    SECTION("Off and On are queued while the interaction worker is busy") {
        SendMessageW(startup, BM_CLICK, 0, 0);
        REQUIRE(dequeued.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

        const HWND checkbox = selectAttributionTab();
        REQUIRE(SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED);
        SendMessageW(checkbox, BM_CLICK, 0, 0);
        // Redraw Settings while the worker is still blocked and the saved choice is still On.
        REQUIRE(app.selectDashboardPreferencesTabForTesting(1));
        CHECK(SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_UNCHECKED);
        SendMessageW(checkbox, BM_CLICK, 0, 0);

        profile.updateSource(900);
        app.refreshRuntimeForTesting();
        CHECK(profile.generation() == 1);
        CHECK(profile.tokens() == 0);
        releaseSettings.set_value();

        const bool newBaseline = waitForCondition(
            [&]() { return profile.generation() == 2 && profile.sourceCount() == 2; }, std::chrono::seconds(5));
        INFO("Saved generation: " << profile.generation() << ", sources: " << profile.sourceCount());
        REQUIRE(newBaseline);
        CHECK(profile.tokens() == 0);
        profile.updateSource(950);
        app.refreshRuntimeForTesting();
        CHECK(profile.tokens() == 50);
    }

    SECTION("SQLite rejects the Off write and an explicit retry persists it") {
        const HWND checkbox = selectAttributionTab();
        REQUIRE(
            profile.storage
                .execute(
                    "CREATE TRIGGER fail_codex_revocation_insert BEFORE INSERT ON settings "
                    "WHEN NEW.key = 'runtime.codex_model_attribution_generation' AND CAST(NEW.value AS INTEGER) < 0 "
                    "BEGIN SELECT RAISE(ABORT, 'test revocation write failure'); END;"
                    "CREATE TRIGGER fail_codex_revocation_update BEFORE UPDATE ON settings "
                    "WHEN NEW.key = 'runtime.codex_model_attribution_generation' AND CAST(NEW.value AS INTEGER) < 0 "
                    "BEGIN SELECT RAISE(ABORT, 'test revocation write failure'); END;")
                .success());
        SendMessageW(checkbox, BM_CLICK, 0, 0);
        REQUIRE(waitForCondition([&]() {
            return app.getDashboardViewModelForTesting().codexAttributionSaveError ==
                   CodexAttributionSaveError::Revocation;
        }));
        CHECK(profile.generation() == 1);
        CHECK(app.getDashboardViewModelForTesting().preferences.codexModelAttributionEnabled);
        profile.updateSource(1200);
        app.refreshRuntimeForTesting();
        CHECK(profile.tokens() == 0);

        REQUIRE(profile.storage
                    .execute("DROP TRIGGER fail_codex_revocation_insert; DROP TRIGGER fail_codex_revocation_update;")
                    .success());
        REQUIRE(waitForCondition([&]() { return SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED; }));
        SendMessageW(checkbox, BM_CLICK, 0, 0);
        REQUIRE(waitForCondition([&]() { return profile.generation() == -1; }));
        app.shutdown();

        AppController restarted;
        profile.startController(restarted);
        CHECK_FALSE(restarted.getDashboardViewModelForTesting().preferences.codexModelAttributionEnabled);
        CHECK(profile.tokens() == 0);
    }
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
    CHECK(findUsageSummary(app.getDashboardUsageProgressSummariesForTesting(), "OpenAI Subscription").has_value());

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
    CHECK(findUsageSummary(rehydratedApp.getDashboardUsageProgressSummariesForTesting(), "OpenAI Subscription")
              .has_value());

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
