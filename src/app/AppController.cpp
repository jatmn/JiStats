#include "app/AppController.hpp"
#include "app/TrayController.hpp"

#include "analytics/GapClassifier.hpp"
#include "analytics/MirrorEngine.hpp"
#include "core/Logger.hpp"
#include "core/RuntimeStatus.hpp"
#include "db/StorageService.hpp"
#include "db/SettingsService.hpp"
#include "gui/DashboardPresenter.hpp"
#include "gui/NotificationService.hpp"
#include "observation/LocalObservationService.hpp"
#include "observation/ObservationPersistenceBridge.hpp"
#include "platform/StartupManager.hpp"
#include "providers/ProviderManager.hpp"
#include "providers/SubscriptionQuotaAdapter.hpp"
#include "providers/openai/OpenAIAdapter.hpp"
#include "providers/openai/OpenAIExportImport.hpp"
#include "providers/openai/CodexLocalSqliteReader.hpp"
#include "providers/openai/CodexLocalUsageImporter.hpp"
#include "providers/minimax/MiniMaxAdapter.hpp"
#include "providers/claude/ClaudeCodeAdapter.hpp"
#include "providers/claude/ClaudeCodeLocalParser.hpp"
#if __has_include("providers/kimi/KimiAdapter.hpp")
#include "providers/kimi/KimiAdapter.hpp"
#define JISTATS_HAS_KIMI_ADAPTER 1
#else
#define JISTATS_HAS_KIMI_ADAPTER 0
#endif
#include "reporting/AnalysisQueryEngine.hpp"
#include "reporting/ReportingService.hpp"
#include "gui/AnalysisPresenter.hpp"
#include "taskbar/TaskbarPresenter.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace JiStats {

namespace {

constexpr const char* kJiStatsAppVersion = "1.0.0";

std::string formatTimeUtc(std::time_t time) {
    std::tm tm{};
    gmtime_s(&tm, &time);

    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S UTC", &tm);
    return buffer;
}

std::string formatTimeUtcIso(std::time_t time) {
    std::tm tm{};
    gmtime_s(&tm, &time);

    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

std::optional<std::time_t> parseTimeUtcIso(const std::string& value) {
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

constexpr std::time_t kRecoveryGapThresholdSeconds = 3600;
constexpr std::time_t kGapAnnotationLookbackSeconds = 7 * 24 * 3600;
constexpr std::time_t kRetryDelaySeconds = 5 * 60;
constexpr std::time_t kRetryDelayEscalatedSeconds = 15 * 60;
constexpr std::time_t kOfflineNoticeCooldownSeconds = 60 * 60;
constexpr const char* kRuntimeRefreshSyncKind = "runtime_refresh";
constexpr const char* kRuntimeBackfillSyncKind = "runtime_backfill";
constexpr auto kOpenAICodexRefreshPollInterval = std::chrono::seconds(2);
constexpr auto kMiniMaxRefreshPollInterval = std::chrono::seconds(45);
constexpr auto kTaskbarRefreshPollInterval = std::chrono::minutes(5);
constexpr auto kTaskbarTrayOnlyRefreshPollInterval = std::chrono::minutes(30);
constexpr auto kDormantRefreshWatcherInterval = std::chrono::hours(6);
constexpr auto kOpenAICodexRefreshMaxBackoffInterval = std::chrono::seconds(30);
constexpr auto kLocalProviderRefreshMaxBackoffInterval = std::chrono::seconds(60);
constexpr size_t kWatcherBackoffStepUnchangedPasses = 3;
constexpr std::time_t kDirectApiQuotaActivePollIntervalSeconds = 5 * 60;
constexpr std::time_t kDirectApiQuotaStableWindowSeconds = 30 * 60;
constexpr std::time_t kDirectApiQuotaStablePollIntervalSeconds = 30 * 60;
constexpr std::time_t kRetentionRawEventDays = 90;
constexpr std::time_t kRetentionHourlyRollupDays = 30;
constexpr std::time_t kRetentionDailyRollupDays = 365;
constexpr std::time_t kRetentionGapMarkerDays = 90;
constexpr std::time_t kRetentionIntervalSeconds = 24 * 3600;
constexpr auto kRetentionCheckInterval = std::chrono::hours(1);

bool isProviderTrackingEnabled(
    const DashboardPreferences& preferences,
    const std::string& providerKey) {
    if (providerKey == "openai") {
        return preferences.openaiSubscriptionTrackingEnabled;
    }
    if (providerKey == "minimax") {
        return preferences.miniMaxSubscriptionTrackingEnabled;
    }
    if (providerKey == "claude_code") {
        return preferences.claudeCodeEnabled;
    }
    if (providerKey == "kimi") {
        return preferences.kimiCodeEnabled;
    }
    if (providerKey == "grok") {
        return preferences.grokSubscriptionTrackingEnabled;
    }
    if (providerKey == "cursor") {
        return preferences.cursorSubscriptionTrackingEnabled;
    }
    return true;
}

// Consent writes and source scans share the same lock across JiStats processes.
class CodexProfileLock {
  public:
    CodexProfileLock(const std::string& databasePath, DWORD waitMs) {
        const auto path = std::filesystem::path(databasePath).wstring();
        const HANDLE database =
            CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (database == INVALID_HANDLE_VALUE) {
            return;
        }
        BY_HANDLE_FILE_INFORMATION identity{};
        const bool identified = GetFileInformationByHandle(database, &identity) != FALSE;
        CloseHandle(database);
        if (!identified) {
            return;
        }

        // The file identity also matches when a peer opened the database through a path alias.
        const std::wstring name =
            L"Global\\JiStats.CodexAttribution." + std::to_wstring(identity.dwVolumeSerialNumber) + L"." +
            std::to_wstring(identity.nFileIndexHigh) + L"." + std::to_wstring(identity.nFileIndexLow);
        // An existing mutex may allow synchronization without granting security-control rights.
        const HANDLE mutex = CreateMutexExW(nullptr, name.c_str(), 0, SYNCHRONIZE | MUTEX_MODIFY_STATE);
        if (mutex != nullptr) {
            const DWORD status = WaitForSingleObject(mutex, waitMs);
            if (status == WAIT_OBJECT_0 || status == WAIT_ABANDONED) {
                mutex_ = mutex;
            } else {
                CloseHandle(mutex);
            }
        }
    }

    ~CodexProfileLock() {
        if (mutex_ != nullptr) {
            ReleaseMutex(mutex_);
            CloseHandle(mutex_);
        }
    }

    CodexProfileLock(const CodexProfileLock&) = delete;
    CodexProfileLock& operator=(const CodexProfileLock&) = delete;

    bool ownsLock() const {
        return mutex_ != nullptr;
    }

  private:
    HANDLE mutex_ = nullptr;
};

struct RefreshWatcherFileSample {
    std::filesystem::path path;
    std::filesystem::file_time_type lastWriteTime{};
};

std::chrono::milliseconds adaptiveWatcherInterval(std::chrono::milliseconds baseInterval,
                                                  std::chrono::milliseconds maxInterval,
                                                  size_t unchangedPasses) {
    const size_t backoffStep = unchangedPasses / kWatcherBackoffStepUnchangedPasses;
    if (backoffStep == 0) {
        return baseInterval;
    }

    std::chrono::milliseconds interval = baseInterval;
    for (size_t index = 0; index < backoffStep; ++index) {
        if (interval >= maxInterval / 2) {
            return maxInterval;
        }
        interval *= 2;
    }
    return std::min(interval, maxInterval);
}

std::chrono::milliseconds dormantRefreshWatcherInterval() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(kDormantRefreshWatcherInterval);
}

std::chrono::milliseconds adaptiveWatcherIntervalOrDormant(
    bool watcherActive,
    std::chrono::milliseconds baseInterval,
    std::chrono::milliseconds maxInterval,
    size_t unchangedPasses) {
    if (!watcherActive) {
        return dormantRefreshWatcherInterval();
    }

    return adaptiveWatcherInterval(baseInterval, maxInterval, unchangedPasses);
}

std::string tryGetEnvironmentVariable(const char* name) {
    if (!name || *name == '\0') {
        return {};
    }

    char* value = nullptr;
    std::size_t valueLength = 0;
    if (_dupenv_s(&value, &valueLength, name) != 0 || value == nullptr) {
        return {};
    }

    const std::string resolvedValue(value);
    free(value);
    return resolvedValue;
}

std::string resolveCodexRolloutDirectoryForRefreshWatcher() {
    const std::string codexHome = tryGetEnvironmentVariable("CODEX_HOME");
    if (!codexHome.empty()) {
        return (std::filesystem::path(codexHome) / "sessions").string();
    }

    const std::string userProfile = tryGetEnvironmentVariable("USERPROFILE");
    if (!userProfile.empty()) {
        return (std::filesystem::path(userProfile) / ".codex" / "sessions").string();
    }

    const std::string home = tryGetEnvironmentVariable("HOME");
    if (!home.empty()) {
        return (std::filesystem::path(home) / ".codex" / "sessions").string();
    }

    return {};
}

bool isCodexRolloutPath(const std::filesystem::path& path) {
    const std::string filename = path.filename().string();
    return filename.rfind("rollout-", 0) == 0 && path.extension() == ".jsonl";
}

bool shouldReplaceRefreshWatcherSample(
    const std::optional<RefreshWatcherFileSample>& current,
    const RefreshWatcherFileSample& candidate) {
    return !current.has_value() ||
        candidate.lastWriteTime > current->lastWriteTime ||
        (candidate.lastWriteTime == current->lastWriteTime &&
         candidate.path.string() > current->path.string());
}

void considerRefreshWatcherFileSample(
    std::optional<RefreshWatcherFileSample>& latest,
    const std::filesystem::path& candidatePath,
    std::error_code& ec) {
    if (!std::filesystem::exists(candidatePath, ec) ||
        !std::filesystem::is_regular_file(candidatePath, ec)) {
        ec.clear();
        return;
    }

    const auto writeTime = std::filesystem::last_write_time(candidatePath, ec);
    if (ec) {
        ec.clear();
        return;
    }

    const RefreshWatcherFileSample candidate{candidatePath, writeTime};
    if (shouldReplaceRefreshWatcherSample(latest, candidate)) {
        latest = candidate;
    }
}

std::optional<RefreshWatcherFileSample> sampleLatestCodexRolloutFileForRefreshWatcher() {
    const std::string directoryPath = resolveCodexRolloutDirectoryForRefreshWatcher();
    if (directoryPath.empty()) {
        return std::nullopt;
    }

    std::error_code ec;
    const std::filesystem::path root(directoryPath);
    if (!std::filesystem::exists(root, ec) || ec) {
        return std::nullopt;
    }

    std::optional<RefreshWatcherFileSample> latest;
    for (std::filesystem::recursive_directory_iterator it(
             root,
             std::filesystem::directory_options::skip_permission_denied,
             ec);
         !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        const auto& entry = *it;
        if (!entry.is_regular_file(ec) || ec || !isCodexRolloutPath(entry.path())) {
            ec.clear();
            continue;
        }

        const auto writeTime = entry.last_write_time(ec);
        if (ec) {
            ec.clear();
            continue;
        }

        const RefreshWatcherFileSample candidate{entry.path(), writeTime};
        if (shouldReplaceRefreshWatcherSample(latest, candidate)) {
            latest = candidate;
        }
    }

    return latest;
}

std::optional<RefreshWatcherFileSample> sampleClaudeLocalArtifactForRefreshWatcher() {
    const std::string statsCachePath = ClaudeCodeLocalParser::resolveDefaultStatsCachePath();
    if (statsCachePath.empty()) {
        return std::nullopt;
    }

    std::error_code ec;
    std::optional<RefreshWatcherFileSample> latest;
    considerRefreshWatcherFileSample(latest, std::filesystem::path(statsCachePath), ec);
    return latest;
}

bool isKimiRefreshWatcherArtifactPath(
    const std::filesystem::path& root,
    const std::filesystem::path& candidatePath) {
    const std::filesystem::path normalizedRoot = root.lexically_normal();
    const std::filesystem::path normalizedCandidate = candidatePath.lexically_normal();
    const std::filesystem::path relativePath = normalizedCandidate.lexically_relative(normalizedRoot);
    const auto relativeBegin = relativePath.begin();
    if (relativePath.empty() ||
        (relativeBegin != relativePath.end() &&
         *relativeBegin == std::filesystem::path(".."))) {
        return false;
    }

    if (relativePath == std::filesystem::path("config.toml") ||
        relativePath == std::filesystem::path("kimi.json") ||
        relativePath == std::filesystem::path("credentials") / "kimi-code.json") {
        return true;
    }

    const std::string filename = normalizedCandidate.filename().string();
    return filename == "state.json" ||
        filename == "runtime.json" ||
        filename == "event.json" ||
        filename == "delivery.json";
}

std::optional<RefreshWatcherFileSample> sampleLatestKimiLocalArtifactForRefreshWatcher() {
    const std::string rootPath = KimiAdapter::resolveDefaultRootPath();
    if (rootPath.empty()) {
        return std::nullopt;
    }

    std::error_code ec;
    const std::filesystem::path root(rootPath);
    if (!std::filesystem::exists(root, ec) || ec) {
        return std::nullopt;
    }

    std::optional<RefreshWatcherFileSample> latest;
    considerRefreshWatcherFileSample(latest, root / "config.toml", ec);
    considerRefreshWatcherFileSample(latest, root / "kimi.json", ec);
    considerRefreshWatcherFileSample(latest, root / "credentials" / "kimi-code.json", ec);

    const std::filesystem::path sessionsRoot = root / "sessions";
    if (!std::filesystem::exists(sessionsRoot, ec) || ec) {
        ec.clear();
        return latest;
    }

    for (std::filesystem::recursive_directory_iterator it(
             sessionsRoot,
             std::filesystem::directory_options::skip_permission_denied,
             ec);
         !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        const auto& entry = *it;
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }

        if (!isKimiRefreshWatcherArtifactPath(root, entry.path())) {
            continue;
        }

        considerRefreshWatcherFileSample(latest, entry.path(), ec);
    }

    return latest;
}

std::optional<RefreshWatcherFileSample> sampleLatestLocalProviderArtifactForRefreshWatcher(
    bool includeClaudeCode,
    bool includeKimiCode) {
    std::optional<RefreshWatcherFileSample> latest;
    if (includeClaudeCode) {
        if (const auto claudeSample = sampleClaudeLocalArtifactForRefreshWatcher();
            claudeSample.has_value() &&
            shouldReplaceRefreshWatcherSample(latest, *claudeSample)) {
            latest = claudeSample;
        }
    }
    if (includeKimiCode) {
        if (const auto kimiSample = sampleLatestKimiLocalArtifactForRefreshWatcher();
            kimiSample.has_value() &&
            shouldReplaceRefreshWatcherSample(latest, *kimiSample)) {
            latest = kimiSample;
        }
    }
    return latest;
}

std::time_t utcDayStart(std::time_t time) {
    std::tm tm{};
    gmtime_s(&tm, &time);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    return _mkgmtime(&tm);
}

std::time_t utcHourStart(std::time_t time) {
    std::tm tm{};
    gmtime_s(&tm, &time);
    tm.tm_min = 0;
    tm.tm_sec = 0;
    return _mkgmtime(&tm);
}

std::string joinCsvValues(const std::vector<std::string>& values) {
    std::ostringstream stream;
    for (size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            stream << ",";
        }
        stream << values[index];
    }
    return stream.str();
}

std::string buildRecoveryBackfillCursorValue(
    std::time_t recoveryStart,
    std::time_t recoveryEnd,
    const std::string& outcome,
    std::size_t fetchedEventCount,
    const std::string& comparisonResult,
    long long localEventCount,
    long long localTotalTokens,
    long long localTotalCostMinorUnits,
    long long importedTotalTokens,
    long long importedTotalCostMinorUnits) {
    std::ostringstream oss;
    oss << "{"
        << "\"recovery_start\":\"" << formatTimeUtcIso(recoveryStart) << "\","
        << "\"recovery_end\":\"" << formatTimeUtcIso(recoveryEnd) << "\","
        << "\"outcome\":\"" << outcome << "\","
        << "\"fetched_event_count\":" << fetchedEventCount << ","
        << "\"comparison_result\":\"" << comparisonResult << "\","
        << "\"local_event_count\":" << localEventCount << ","
        << "\"local_total_tokens\":" << localTotalTokens << ","
        << "\"local_total_cost_minor_units\":" << localTotalCostMinorUnits << ","
        << "\"imported_total_tokens\":" << importedTotalTokens << ","
        << "\"imported_total_cost_minor_units\":" << importedTotalCostMinorUnits
        << "}";
    return oss.str();
}

std::string extractCursorStringField(const std::string& cursorValue, const char* fieldName) {
    if (fieldName == nullptr || *fieldName == '\0') {
        return {};
    }

    const std::string needle = "\"" + std::string(fieldName) + "\":\"";
    const std::size_t start = cursorValue.find(needle);
    if (start == std::string::npos) {
        return {};
    }

    const std::size_t valueStart = start + needle.size();
    const std::size_t valueEnd = cursorValue.find('"', valueStart);
    if (valueEnd == std::string::npos || valueEnd <= valueStart) {
        return {};
    }

    return cursorValue.substr(valueStart, valueEnd - valueStart);
}

std::optional<std::string> extractJsonStringField(
    const std::string& cursorValue,
    const char* fieldName) {
    const std::string value = extractCursorStringField(cursorValue, fieldName);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

long long extractCursorIntField(const std::string& cursorValue, const char* fieldName) {
    if (fieldName == nullptr || *fieldName == '\0') {
        return 0;
    }

    const std::string needle = "\"" + std::string(fieldName) + "\":";
    const std::size_t start = cursorValue.find(needle);
    if (start == std::string::npos) {
        return 0;
    }

    const std::size_t valueStart = start + needle.size();
    const std::size_t valueEnd = cursorValue.find_first_of(",}", valueStart);
    const std::string rawValue = cursorValue.substr(
        valueStart,
        valueEnd == std::string::npos ? std::string::npos : valueEnd - valueStart);

    try {
        return std::stoll(rawValue);
    } catch (...) {
        return 0;
    }
}

std::optional<long long> extractJsonInt64Field(
    const std::string& cursorValue,
    const char* fieldName) {
    if (fieldName == nullptr || *fieldName == '\0') {
        return std::nullopt;
    }

    const std::string needle = "\"" + std::string(fieldName) + "\":";
    const std::size_t start = cursorValue.find(needle);
    if (start == std::string::npos) {
        return std::nullopt;
    }

    return extractCursorIntField(cursorValue, fieldName);
}

struct RecoveryBackfillCursorSummary {
    std::string outcome;
    std::string comparisonResult;
    std::time_t recoveryEnd = 0;
    long long fetchedEventCount = 0;
    long long localEventCount = 0;
    long long localTotalTokens = 0;
    long long importedTotalTokens = 0;
};

RecoveryBackfillCursorSummary parseRecoveryBackfillCursorSummary(const std::string& cursorValue) {
    RecoveryBackfillCursorSummary summary;
    summary.outcome = extractCursorStringField(cursorValue, "outcome");
    summary.comparisonResult = extractCursorStringField(cursorValue, "comparison_result");
    summary.fetchedEventCount = extractCursorIntField(cursorValue, "fetched_event_count");
    summary.localEventCount = extractCursorIntField(cursorValue, "local_event_count");
    summary.localTotalTokens = extractCursorIntField(cursorValue, "local_total_tokens");
    summary.importedTotalTokens = extractCursorIntField(cursorValue, "imported_total_tokens");
    if (const auto parsedRecoveryEnd =
            parseTimeUtcIso(extractCursorStringField(cursorValue, "recovery_end"))) {
        summary.recoveryEnd = *parsedRecoveryEnd;
    }
    return summary;
}

struct RecoveryBackfillComparisonSummary {
    std::string comparisonResult = "not_available";
    long long localEventCount = 0;
    long long localTotalTokens = 0;
    long long localTotalCostMinorUnits = 0;
    long long importedTotalTokens = 0;
    long long importedTotalCostMinorUnits = 0;
};

std::optional<RecoveryBackfillComparisonSummary> summarizeRecoveryBackfillAgainstLocalEvidence(
    StorageService& storageService,
    int providerId,
    int accountId,
    std::time_t recoveryStart,
    std::time_t recoveryEnd,
    const std::vector<PersistedUsageEvent>& importedEvents) {
    RecoveryBackfillComparisonSummary summary;
    for (const auto& event : importedEvents) {
        summary.importedTotalTokens += event.totalTokens;
        summary.importedTotalCostMinorUnits += event.costMinorUnits;
    }

    QueryResult localEvidence = storageService.execute(
        "SELECT COUNT(*), COALESCE(SUM(total_tokens), 0), COALESCE(SUM(cost_minor_units), 0) "
        "FROM usage_events WHERE provider_id = ? AND account_id = ? "
        "AND event_time_utc >= ? AND event_time_utc <= ? AND is_backfilled = 0 "
        "AND source_type != 'codex_local_rollout';",
        {
            std::to_string(providerId),
            std::to_string(accountId),
            formatTimeUtcIso(recoveryStart),
            formatTimeUtcIso(recoveryEnd)
        });
    if (!localEvidence.success() || localEvidence.rowCount() == 0) {
        return std::nullopt;
    }

    summary.localEventCount = localEvidence.getInt64(0, 0);
    summary.localTotalTokens = localEvidence.getInt64(0, 1);
    summary.localTotalCostMinorUnits = localEvidence.getInt64(0, 2);

    const bool hasLocalEvidence =
        summary.localEventCount > 0 ||
        summary.localTotalTokens > 0 ||
        summary.localTotalCostMinorUnits > 0;

    if (!hasLocalEvidence) {
        summary.comparisonResult = "no_local_evidence";
    } else if (summary.localEventCount == static_cast<long long>(importedEvents.size()) &&
               summary.localTotalTokens == summary.importedTotalTokens &&
               summary.localTotalCostMinorUnits == summary.importedTotalCostMinorUnits) {
        summary.comparisonResult = "matched_local_evidence";
    } else {
        summary.comparisonResult = "mismatch";
    }

    return summary;
}

std::string buildRecoveryGapBasisSummary(
    const std::string& backfillOutcome,
    const RecoveryBackfillComparisonSummary& comparison) {
    if (backfillOutcome == "imported") {
        if (comparison.comparisonResult == "matched_local_evidence") {
            return "Recovered usage from provider-imported backfill; matched local quantified evidence";
        }
        if (comparison.comparisonResult == "mismatch") {
            return "Recovered usage from provider-imported backfill; differs from local quantified evidence";
        }
        return "Recovered usage from provider-imported backfill; no local quantified evidence was available for comparison";
    }

    if (backfillOutcome == "no_data") {
        if (comparison.comparisonResult == "mismatch") {
            return "Provider backfill returned no history, but JiStats has local quantified evidence for the same recovery window";
        }
        return "No provider-imported history or local quantified evidence was available for the recovery window";
    }

    if (backfillOutcome == "fetch_failed") {
        return "JiStats could not retrieve provider backfill during the recovery window";
    }

    return "JiStats was not fully observing provider state during the recovery window";
}

bool persistRecoveryBackfillSyncState(
    StorageService& storageService,
    int providerId,
    int accountId,
    std::time_t recoveryStart,
    std::time_t recoveryEnd,
    std::time_t attemptAt,
    const std::string& outcome,
    std::size_t fetchedEventCount,
    const std::string& failureCategory,
    const RecoveryBackfillComparisonSummary& comparison) {
    PersistedRecoveryCheckpoint checkpoint;
    checkpoint.providerId = providerId;
    checkpoint.accountId = accountId;
    checkpoint.syncKind = kRuntimeBackfillSyncKind;
    checkpoint.recoveryStart = recoveryStart;
    checkpoint.recoveryEnd = recoveryEnd;
    checkpoint.attemptAt = attemptAt;
    checkpoint.outcome = outcome;
    checkpoint.fetchedEventCount = static_cast<long long>(fetchedEventCount);
    checkpoint.comparisonResult = comparison.comparisonResult;
    checkpoint.localEventCount = comparison.localEventCount;
    checkpoint.localTotalTokens = comparison.localTotalTokens;
    checkpoint.localTotalCostMinorUnits = comparison.localTotalCostMinorUnits;
    checkpoint.importedTotalTokens = comparison.importedTotalTokens;
    checkpoint.importedTotalCostMinorUnits = comparison.importedTotalCostMinorUnits;
    checkpoint.failureCategory = failureCategory;
    if (!storageService.insertRecoveryCheckpoint(checkpoint)) {
        return false;
    }

    PersistedSyncState syncState;
    if (const auto existing =
            storageService.getSyncState(providerId, accountId, kRuntimeBackfillSyncKind)) {
        syncState = *existing;
    }

    syncState.providerId = providerId;
    syncState.accountId = accountId;
    syncState.syncKind = kRuntimeBackfillSyncKind;
    syncState.cursorValue = buildRecoveryBackfillCursorValue(
        recoveryStart,
        recoveryEnd,
        outcome,
        fetchedEventCount,
        comparison.comparisonResult,
        comparison.localEventCount,
        comparison.localTotalTokens,
        comparison.localTotalCostMinorUnits,
        comparison.importedTotalTokens,
        comparison.importedTotalCostMinorUnits);
    syncState.lastAttemptAt = attemptAt;

    if (failureCategory.empty()) {
        syncState.lastSuccessAt = attemptAt;
        syncState.lastFailureAt = 0;
        syncState.lastFailureCategory.clear();
    } else {
        syncState.lastFailureAt = attemptAt;
        syncState.lastFailureCategory = failureCategory;
    }

    return storageService.upsertSyncState(syncState);
}

std::string pluralize(long long count, const char* singular, const char* plural) {
    return std::to_string(count) + " " + (count == 1 ? singular : plural);
}

long long roundUpMinutes(long long seconds) {
    if (seconds <= 0) {
        return 0;
    }

    return (seconds + 59) / 60;
}

std::string formatActiveMinutes(long long activeSeconds) {
    return pluralize(roundUpMinutes(activeSeconds), "min active", "mins active");
}

int computeWindowOccupancyPercentage(long long activeSeconds, long long windowSeconds) {
    if (windowSeconds <= 0 || activeSeconds <= 0) {
        return 0;
    }

    double percentage =
        (static_cast<double>(activeSeconds) / static_cast<double>(windowSeconds)) * 100.0;
    percentage = std::clamp(percentage, 0.0, 100.0);
    return static_cast<int>(percentage + 0.5);
}

std::string buildDashboardActivityDetail(
    long long sessionCount,
    long long activeSeconds,
    long long providerCount) {
    std::ostringstream detail;
    detail << pluralize(sessionCount, "session", "sessions");
    detail << ", " << formatActiveMinutes(activeSeconds);
    if (providerCount > 1) {
        detail << " across " << pluralize(providerCount, "provider", "providers");
    }
    return detail.str();
}

std::string formatCompactSessionCount(long long sessionCount) {
    if (sessionCount > 99) {
        return "99+";
    }
    return std::to_string(sessionCount) + "x";
}

std::string buildTaskbarActivityDetail(long long sessionCount, long long activeSeconds) {
    std::ostringstream detail;
    detail << pluralize(sessionCount, "session", "sessions");
    detail << ", " << formatActiveMinutes(activeSeconds);
    return detail.str();
}

struct DashboardActivityWindowSpec {
    const char* label;
    long long windowSeconds;
    bool useUtcDayStart = false;
};

struct NormalizedQuotaValues {
    long long used = 0;
    long long limit = 0;
    long long remaining = 0;
};

struct DirectApiQuotaPollState {
    bool hasResponse = false;
    QuotaResponse response;
    std::time_t lastAttemptAt = 0;
    std::time_t stableSince = 0;
    std::uint64_t fetchedInRefresh = 0;
};

bool quotaUsageFingerprintMatches(const QuotaResponse& lhs, const QuotaResponse& rhs) {
    return lhs.success == rhs.success &&
        lhs.providerSurface == rhs.providerSurface &&
        lhs.windowType == rhs.windowType &&
        lhs.windowEnd == rhs.windowEnd &&
        lhs.used == rhs.used &&
        lhs.limit == rhs.limit &&
        lhs.remaining == rhs.remaining &&
        lhs.secondaryWindowType == rhs.secondaryWindowType &&
        lhs.secondaryWindowEnd == rhs.secondaryWindowEnd &&
        lhs.secondaryUsed == rhs.secondaryUsed &&
        lhs.secondaryLimit == rhs.secondaryLimit &&
        lhs.secondaryRemaining == rhs.secondaryRemaining;
}

void applyRuntimeStateToViewModel(DashboardViewModel& viewModel);
ProviderState determineProviderState(const QuotaResponse& quota, std::time_t now);

std::string formatCompactRequestCount(long long requestCount) {
    if (requestCount > 99) {
        return "99+";
    }
    return std::to_string(requestCount) + "x";
}

std::string formatCompactTokenCount(long long totalTokens) {
    if (totalTokens >= 1000000) {
        return std::to_string(totalTokens / 1000000) + "M";
    }
    if (totalTokens >= 1000) {
        return std::to_string(totalTokens / 1000) + "K";
    }
    return std::to_string(totalTokens);
}

struct QuantifiedUsageAggregate {
    long long requestCount = 0;
    long long totalTokens = 0;
    long long totalCostMinorUnits = 0;
    long long providerCount = 0;

    bool hasUsage() const {
        return requestCount > 0 || totalTokens > 0 || totalCostMinorUnits > 0;
    }

    void merge(const QuantifiedUsageAggregate& other) {
        requestCount += other.requestCount;
        totalTokens += other.totalTokens;
        totalCostMinorUnits += other.totalCostMinorUnits;
    }
};

struct QuantifiedProviderUsageAggregate {
    std::string providerKey;
    std::string providerDisplayName;
    long long requestCount = 0;
    long long totalTokens = 0;
    long long totalCostMinorUnits = 0;
    long long hotWindowRequests = 0;
};

struct PersistedMetricSnapshot {
    std::vector<DashboardSummaryItem> dashboardSummaries;
    std::vector<TaskbarMetrics> taskbarMetrics;
    std::string trayTooltipSuffix;

    bool hasUsageData() const {
        return !dashboardSummaries.empty() || !taskbarMetrics.empty() || !trayTooltipSuffix.empty();
    }
};

enum class PersistedMetricSnapshotMode {
    Full,
    TrayOnly
};

QuantifiedUsageAggregate queryRawUsageAggregate(
    StorageService& storage,
    std::time_t from,
    std::time_t to) {
    QuantifiedUsageAggregate aggregate;
    if (!storage.isOpen() || from > to) {
        return aggregate;
    }

    QueryResult result = storage.execute(
        "SELECT COUNT(*), COALESCE(SUM(total_tokens), 0), COALESCE(SUM(cost_minor_units), 0) "
        "FROM usage_events WHERE event_time_utc >= ? AND event_time_utc <= ? "
        "AND source_type != 'codex_local_rollout';",
        {formatTimeUtcIso(from), formatTimeUtcIso(to)});
    if (!result.success() || result.rowCount() == 0) {
        return {};
    }

    aggregate.requestCount = result.getInt64(0, 0);
    aggregate.totalTokens = result.getInt64(0, 1);
    aggregate.totalCostMinorUnits = result.getInt64(0, 2);
    return aggregate;
}

QuantifiedUsageAggregate queryHourlyRollupAggregate(
    StorageService& storage,
    std::time_t fromHourStart,
    std::time_t toHourStart) {
    QuantifiedUsageAggregate aggregate;
    if (!storage.isOpen() || fromHourStart > toHourStart) {
        return aggregate;
    }

    QueryResult result = storage.execute(
        "SELECT COALESCE(SUM(request_count), 0), COALESCE(SUM(total_tokens), 0), "
        "COALESCE(SUM(total_cost_minor_units), 0) "
        "FROM usage_rollups_hourly WHERE rollup_period_start_utc >= ? AND rollup_period_start_utc <= ?;",
        {formatTimeUtcIso(fromHourStart), formatTimeUtcIso(toHourStart)});
    if (!result.success() || result.rowCount() == 0) {
        return {};
    }

    aggregate.requestCount = result.getInt64(0, 0);
    aggregate.totalTokens = result.getInt64(0, 1);
    aggregate.totalCostMinorUnits = result.getInt64(0, 2);
    return aggregate;
}

QuantifiedUsageAggregate queryRawUsageAggregateForMissingHourlyRollups(
    StorageService& storage,
    std::time_t fromHourStart,
    std::time_t toHourStart) {
    QuantifiedUsageAggregate aggregate;
    if (!storage.isOpen() || fromHourStart > toHourStart) {
        return aggregate;
    }

    QueryResult result = storage.execute(
        "SELECT COUNT(*), COALESCE(SUM(ue.total_tokens), 0), COALESCE(SUM(ue.cost_minor_units), 0) "
        "FROM usage_events ue "
        "WHERE ue.event_time_utc >= ? AND ue.event_time_utc <= ? "
        "AND ue.source_type != 'codex_local_rollout' "
        "AND NOT EXISTS ("
        "    SELECT 1 FROM usage_rollups_hourly r "
        "    WHERE r.provider_id = ue.provider_id "
        "      AND r.account_id = ue.account_id "
        "      AND r.model_key = COALESCE(ue.model_key, '') "
        "      AND r.rollup_period_start_utc = substr(ue.event_time_utc, 1, 13) || ':00:00Z'"
        ");",
        {formatTimeUtcIso(fromHourStart), formatTimeUtcIso(toHourStart)});
    if (!result.success() || result.rowCount() == 0) {
        return {};
    }

    aggregate.requestCount = result.getInt64(0, 0);
    aggregate.totalTokens = result.getInt64(0, 1);
    aggregate.totalCostMinorUnits = result.getInt64(0, 2);
    return aggregate;
}

long long queryRawUsageProviderCount(
    StorageService& storage,
    std::time_t from,
    std::time_t to) {
    if (!storage.isOpen() || from > to) {
        return 0;
    }

    QueryResult result = storage.execute(
        "SELECT COUNT(DISTINCT provider_id) FROM usage_events "
        "WHERE event_time_utc >= ? AND event_time_utc <= ? "
        "AND source_type != 'codex_local_rollout';",
        {formatTimeUtcIso(from), formatTimeUtcIso(to)});
    if (!result.success() || result.rowCount() == 0) {
        return 0;
    }

    return result.getInt64(0, 0);
}

long long queryHybridUsageProviderCount(
    StorageService& storage,
    std::time_t from,
    std::time_t to,
    const std::optional<std::pair<std::time_t, std::time_t>>& fullHours) {
    if (!storage.isOpen() || from > to) {
        return 0;
    }

    if (!fullHours.has_value()) {
        return queryRawUsageProviderCount(storage, from, to);
    }

    std::set<long long> providerIds;
    auto appendProviderIds = [&providerIds](const QueryResult& result) {
        if (!result.success()) {
            return false;
        }

        for (size_t row = 0; row < result.rowCount(); ++row) {
            providerIds.insert(result.getInt64(row, 0));
        }
        return true;
    };

    QueryResult rollupProviders = storage.execute(
        "SELECT DISTINCT provider_id FROM usage_rollups_hourly "
        "WHERE rollup_period_start_utc >= ? AND rollup_period_start_utc <= ?;",
        {
            formatTimeUtcIso(fullHours->first),
            formatTimeUtcIso(fullHours->second)
        });
    if (!appendProviderIds(rollupProviders)) {
        return 0;
    }

    QueryResult missingRollupProviders = storage.execute(
        "SELECT DISTINCT ue.provider_id FROM usage_events ue "
        "WHERE ue.event_time_utc >= ? AND ue.event_time_utc <= ? "
        "AND ue.source_type != 'codex_local_rollout' "
        "AND NOT EXISTS ("
        "    SELECT 1 FROM usage_rollups_hourly r "
        "    WHERE r.provider_id = ue.provider_id "
        "      AND r.account_id = ue.account_id "
        "      AND r.model_key = COALESCE(ue.model_key, '') "
        "      AND r.rollup_period_start_utc = substr(ue.event_time_utc, 1, 13) || ':00:00Z'"
        ");",
        {
            formatTimeUtcIso(fullHours->first),
            formatTimeUtcIso(fullHours->second + 3599)
        });
    if (!appendProviderIds(missingRollupProviders)) {
        return 0;
    }

    const std::time_t trailingStart = fullHours->second + 3600;
    if (from < fullHours->first) {
        QueryResult leadingProviders = storage.execute(
            "SELECT DISTINCT provider_id FROM usage_events "
            "WHERE event_time_utc >= ? AND event_time_utc <= ? "
            "AND source_type != 'codex_local_rollout';",
            {
                formatTimeUtcIso(from),
                formatTimeUtcIso(fullHours->first - 1)
            });
        if (!appendProviderIds(leadingProviders)) {
            return 0;
        }
    }

    if (trailingStart <= to) {
        QueryResult trailingProviders = storage.execute(
            "SELECT DISTINCT provider_id FROM usage_events "
            "WHERE event_time_utc >= ? AND event_time_utc <= ? "
            "AND source_type != 'codex_local_rollout';",
            {
                formatTimeUtcIso(trailingStart),
                formatTimeUtcIso(to)
            });
        if (!appendProviderIds(trailingProviders)) {
            return 0;
        }
    }

    return static_cast<long long>(providerIds.size());
}

std::optional<std::pair<std::time_t, std::time_t>> fullyCoveredHourlyRange(
    std::time_t from,
    std::time_t to) {
    if (from > to) {
        return std::nullopt;
    }

    std::time_t startHour = utcHourStart(from);
    if (from > startHour) {
        startHour += 3600;
    }

    std::time_t endHour = utcHourStart(to);
    if (to == endHour) {
        endHour -= 3600;
    }

    if (endHour < startHour) {
        return std::nullopt;
    }

    return std::make_pair(startHour, endHour);
}

QuantifiedUsageAggregate queryHybridUsageAggregate(
    StorageService& storage,
    std::time_t from,
    std::time_t to) {
    QuantifiedUsageAggregate aggregate;
    if (!storage.isOpen() || from > to) {
        return aggregate;
    }

    const auto fullHours = fullyCoveredHourlyRange(from, to);
    if (!fullHours.has_value()) {
        aggregate = queryRawUsageAggregate(storage, from, to);
        aggregate.providerCount = queryRawUsageProviderCount(storage, from, to);
        return aggregate;
    }

    if (from < fullHours->first) {
        aggregate.merge(queryRawUsageAggregate(storage, from, fullHours->first - 1));
    }

    aggregate.merge(queryHourlyRollupAggregate(storage, fullHours->first, fullHours->second));
    aggregate.merge(queryRawUsageAggregateForMissingHourlyRollups(
        storage,
        fullHours->first,
        fullHours->second + 3599));

    const std::time_t trailingStart = fullHours->second + 3600;
    if (trailingStart <= to) {
        aggregate.merge(queryRawUsageAggregate(storage, trailingStart, to));
    }

    aggregate.providerCount = queryHybridUsageProviderCount(storage, from, to, fullHours);
    return aggregate;
}

void accumulateRawProviderUsage(
    StorageService& storage,
    std::time_t from,
    std::time_t to,
    std::map<std::string, QuantifiedProviderUsageAggregate>& aggregates) {
    if (!storage.isOpen() || from > to) {
        return;
    }

    QueryResult result = storage.execute(
        "SELECT p.provider_key, p.display_name, COUNT(*), COALESCE(SUM(ue.total_tokens), 0), "
        "COALESCE(SUM(ue.cost_minor_units), 0) "
        "FROM usage_events ue "
        "JOIN providers p ON p.id = ue.provider_id "
        "WHERE ue.event_time_utc >= ? AND ue.event_time_utc <= ? "
        "AND ue.source_type != 'codex_local_rollout' "
        "GROUP BY p.provider_key, p.display_name;",
        {formatTimeUtcIso(from), formatTimeUtcIso(to)});
    if (!result.success()) {
        return;
    }

    for (size_t row = 0; row < result.rowCount(); ++row) {
        const std::string providerKey = result.getString(row, 0);
        auto& aggregate = aggregates[providerKey];
        aggregate.providerKey = providerKey;
        aggregate.providerDisplayName = result.getString(row, 1);
        aggregate.requestCount += result.getInt64(row, 2);
        aggregate.totalTokens += result.getInt64(row, 3);
        aggregate.totalCostMinorUnits += result.getInt64(row, 4);
    }
}

void accumulateHourlyRollupProviderUsage(
    StorageService& storage,
    std::time_t fromHourStart,
    std::time_t toHourStart,
    std::map<std::string, QuantifiedProviderUsageAggregate>& aggregates) {
    if (!storage.isOpen() || fromHourStart > toHourStart) {
        return;
    }

    QueryResult result = storage.execute(
        "SELECT p.provider_key, p.display_name, COALESCE(SUM(r.request_count), 0), "
        "COALESCE(SUM(r.total_tokens), 0), COALESCE(SUM(r.total_cost_minor_units), 0) "
        "FROM usage_rollups_hourly r "
        "JOIN providers p ON p.id = r.provider_id "
        "WHERE r.rollup_period_start_utc >= ? AND r.rollup_period_start_utc <= ? "
        "GROUP BY p.provider_key, p.display_name;",
        {formatTimeUtcIso(fromHourStart), formatTimeUtcIso(toHourStart)});
    if (!result.success()) {
        return;
    }

    for (size_t row = 0; row < result.rowCount(); ++row) {
        const std::string providerKey = result.getString(row, 0);
        auto& aggregate = aggregates[providerKey];
        aggregate.providerKey = providerKey;
        aggregate.providerDisplayName = result.getString(row, 1);
        aggregate.requestCount += result.getInt64(row, 2);
        aggregate.totalTokens += result.getInt64(row, 3);
        aggregate.totalCostMinorUnits += result.getInt64(row, 4);
    }
}

void accumulateRawProviderUsageForMissingHourlyRollups(
    StorageService& storage,
    std::time_t fromHourStart,
    std::time_t toHourEnd,
    std::map<std::string, QuantifiedProviderUsageAggregate>& aggregates) {
    if (!storage.isOpen() || fromHourStart > toHourEnd) {
        return;
    }

    QueryResult result = storage.execute(
        "SELECT p.provider_key, p.display_name, COUNT(*), COALESCE(SUM(ue.total_tokens), 0), "
        "COALESCE(SUM(ue.cost_minor_units), 0) "
        "FROM usage_events ue "
        "JOIN providers p ON p.id = ue.provider_id "
        "WHERE ue.event_time_utc >= ? AND ue.event_time_utc <= ? "
        "AND ue.source_type != 'codex_local_rollout' "
        "AND NOT EXISTS ("
        "    SELECT 1 FROM usage_rollups_hourly r "
        "    WHERE r.provider_id = ue.provider_id "
        "      AND r.account_id = ue.account_id "
        "      AND r.model_key = COALESCE(ue.model_key, '') "
        "      AND r.rollup_period_start_utc = substr(ue.event_time_utc, 1, 13) || ':00:00Z'"
        ") "
        "GROUP BY p.provider_key, p.display_name;",
        {formatTimeUtcIso(fromHourStart), formatTimeUtcIso(toHourEnd)});
    if (!result.success()) {
        return;
    }

    for (size_t row = 0; row < result.rowCount(); ++row) {
        const std::string providerKey = result.getString(row, 0);
        auto& aggregate = aggregates[providerKey];
        aggregate.providerKey = providerKey;
        aggregate.providerDisplayName = result.getString(row, 1);
        aggregate.requestCount += result.getInt64(row, 2);
        aggregate.totalTokens += result.getInt64(row, 3);
        aggregate.totalCostMinorUnits += result.getInt64(row, 4);
    }
}

std::vector<QuantifiedProviderUsageAggregate> queryHybridProviderUsage(
    StorageService& storage,
    std::time_t from,
    std::time_t to) {
    std::map<std::string, QuantifiedProviderUsageAggregate> aggregates;
    if (!storage.isOpen() || from > to) {
        return {};
    }

    const auto fullHours = fullyCoveredHourlyRange(from, to);
    if (!fullHours.has_value()) {
        accumulateRawProviderUsage(storage, from, to, aggregates);
    } else {
        if (from < fullHours->first) {
            accumulateRawProviderUsage(storage, from, fullHours->first - 1, aggregates);
        }

        accumulateHourlyRollupProviderUsage(storage, fullHours->first, fullHours->second, aggregates);
        accumulateRawProviderUsageForMissingHourlyRollups(
            storage,
            fullHours->first,
            fullHours->second + 3599,
            aggregates);

        const std::time_t trailingStart = fullHours->second + 3600;
        if (trailingStart <= to) {
            accumulateRawProviderUsage(storage, trailingStart, to, aggregates);
        }
    }

    std::vector<QuantifiedProviderUsageAggregate> results;
    results.reserve(aggregates.size());
    for (auto& entry : aggregates) {
        results.push_back(std::move(entry.second));
    }
    return results;
}

std::string buildQuantifiedUsageDetail(
    long long requestCount,
    long long totalTokens,
    long long providerCount) {
    std::ostringstream detail;
    detail << pluralize(requestCount, "request", "requests");
    detail << ", " << totalTokens << " tokens";
    if (providerCount > 1) {
        detail << " across " << pluralize(providerCount, "provider", "providers");
    }
    return detail.str();
}

struct ResolvedReportWindow {
    std::time_t start = 0;
    std::time_t end = 0;
    std::string label;
};



std::string reportSourceLabelForSurface(const std::string& providerSurface) {
    if (providerSurface == "openai_subscription_local") {
        return "OpenAI Local Observation";
    }
    if (providerSurface == "openai_subscription_codex_local") {
        return "OpenAI Codex-local";
    }
    if (providerSurface == "openai_subscription_export_import") {
        return "OpenAI Export Import";
    }
    if (providerSurface == "openai_api_import") {
        return "OpenAI API Platform Import";
    }
    if (providerSurface == "minimax_subscription_coding_plan") {
        return "MiniMax Coding Plan";
    }
    if (providerSurface == "minimax_subscription_token_plan") {
        return "MiniMax Token Plan";
    }
    if (providerSurface == "claude_code_local_stats") {
        return "Claude Local";
    }
    if (providerSurface == "grok_subscription_api") {
        return "Grok Subscription";
    }
    if (providerSurface == "cursor_subscription_api") {
        return "Cursor Subscription";
    }
    if (providerSurface == "kimi_subscription_api") {
        return "Kimi Subscription";
    }
    if (providerSurface == "kimi_code_local" ||
        providerSurface == "kimi_local_runtime") {
        return "Kimi Local";
    }
    return providerSurface;
}

std::optional<std::time_t> parseReportDay(const std::string& value) {
    if (value.empty()) {
        return std::nullopt;
    }

    std::tm tm{};
    std::istringstream stream(value);
    stream >> std::get_time(&tm, "%Y-%m-%d");
    if (stream.fail()) {
        return std::nullopt;
    }
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    return _mkgmtime(&tm);
}

std::time_t utcWeekStart(std::time_t value) {
    std::tm tm{};
    gmtime_s(&tm, &value);
    const int dayOffset = (tm.tm_wday + 6) % 7; // Monday-start week.
    return utcDayStart(value) - static_cast<std::time_t>(dayOffset) * 86400;
}

std::string formatReportDayLabel(std::time_t value) {
    std::tm tm{};
    gmtime_s(&tm, &value);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &tm);
    return buffer;
}

std::string formatReportWindowLabel(std::time_t start, std::time_t end) {
    return formatReportDayLabel(start) + " to " + formatReportDayLabel(end) + " UTC";
}

std::string analysisDateRangePresetLabel(AnalysisDateRangePreset preset) {
    switch (preset) {
    case AnalysisDateRangePreset::Today:
        return "Today";
    case AnalysisDateRangePreset::Last24Hours:
        return "Last 24 Hours";
    case AnalysisDateRangePreset::Last7Days:
        return "Last 7 Days";
    case AnalysisDateRangePreset::CurrentQuotaWindow:
        return "Current Billing/Quota Window";
    case AnalysisDateRangePreset::ThisMonth:
        return "This Month";
    case AnalysisDateRangePreset::Custom:
        return "Custom";
    default:
        return "Last 7 Days";
    }
}

const char* analysisDateRangePresetSettingValue(AnalysisDateRangePreset preset) {
    switch (preset) {
    case AnalysisDateRangePreset::Today:
        return "today";
    case AnalysisDateRangePreset::Last24Hours:
        return "last_24_hours";
    case AnalysisDateRangePreset::Last7Days:
        return "last_7_days";
    case AnalysisDateRangePreset::CurrentQuotaWindow:
        return "current_quota_window";
    case AnalysisDateRangePreset::ThisMonth:
        return "this_month";
    case AnalysisDateRangePreset::Custom:
        return "custom";
    default:
        return "last_7_days";
    }
}

AnalysisDateRangePreset parseAnalysisDateRangePreset(const std::string& value) {
    if (value == "today") {
        return AnalysisDateRangePreset::Today;
    }
    if (value == "last_24_hours") {
        return AnalysisDateRangePreset::Last24Hours;
    }
    if (value == "current_quota_window") {
        return AnalysisDateRangePreset::CurrentQuotaWindow;
    }
    if (value == "this_month") {
        return AnalysisDateRangePreset::ThisMonth;
    }
    if (value == "custom") {
        return AnalysisDateRangePreset::Custom;
    }
    return AnalysisDateRangePreset::Last7Days;
}

ResolvedReportWindow resolveReportWindow(const ReportRequest& request, std::time_t now) {
    ResolvedReportWindow window;
    if (request.dateRange.end.time_since_epoch().count() != 0) {
        window.end = std::chrono::system_clock::to_time_t(request.dateRange.end);
    } else {
        window.end = now;
    }

    if (request.dateRange.start.time_since_epoch().count() != 0) {
        window.start = std::chrono::system_clock::to_time_t(request.dateRange.start);
    } else {
        window.start = std::max<std::time_t>(0, utcDayStart(window.end) - (13 * 86400));
    }

    if (window.start > window.end) {
        std::swap(window.start, window.end);
    }

    window.label = formatReportWindowLabel(window.start, window.end);
    return window;
}

std::vector<std::string> collectReportProviderKeys(const DashboardViewModel& viewModel) {
    std::vector<std::string> providerKeys;
    providerKeys.reserve(viewModel.providers.size());
    for (const auto& provider : viewModel.providers) {
        if (!provider.providerKey.empty()) {
            providerKeys.push_back(provider.providerKey);
        }
    }
    return providerKeys;
}

std::vector<std::string> collectReportAccountIds(const DashboardViewModel& viewModel) {
    std::vector<std::string> accountIds;
    accountIds.reserve(viewModel.providers.size());
    for (const auto& provider : viewModel.providers) {
        if (!provider.accountId.empty()) {
            accountIds.push_back(provider.accountId);
        }
    }
    return accountIds;
}

void appendProviderFilterClause(
    std::string& sql,
    std::vector<std::string>& params,
    const std::vector<std::string>& providerKeys,
    const char* columnName) {
    if (providerKeys.empty() || columnName == nullptr || *columnName == '\0') {
        return;
    }

    sql += " AND ";
    sql += columnName;
    sql += " IN (";
    for (size_t index = 0; index < providerKeys.size(); ++index) {
        if (index > 0) {
            sql += ", ";
        }
        sql += "?";
        params.push_back(providerKeys[index]);
    }
    sql += ")";
}

void appendAccountFilterClause(
    std::string& sql,
    std::vector<std::string>& params,
    const std::vector<std::string>& accountIds,
    const char* columnName) {
    if (accountIds.empty() || columnName == nullptr || *columnName == '\0') {
        return;
    }

    sql += " AND ";
    sql += columnName;
    sql += " IN (";
    for (size_t index = 0; index < accountIds.size(); ++index) {
        if (index > 0) {
            sql += ", ";
        }
        sql += "?";
        params.push_back(accountIds[index]);
    }
    sql += ")";
}

std::optional<std::pair<std::time_t, std::time_t>> fullyCoveredDailyRange(
    std::time_t from,
    std::time_t to) {
    if (from > to) {
        return std::nullopt;
    }

    std::time_t startDay = utcDayStart(from);
    if (from > startDay) {
        startDay += 86400;
    }

    std::time_t endDay = utcDayStart(to);
    if (to == endDay) {
        endDay -= 86400;
    }

    if (endDay < startDay) {
        return std::nullopt;
    }

    return std::make_pair(startDay, endDay);
}

ReportAggregateData buildReportAggregateData(
    StorageService& storage,
    const ReportRequest& request,
    const DashboardViewModel& viewModel,
    const ReportMetadata& metadata,
    std::time_t now) {
    ReportAggregateData aggregateData;
    const ResolvedReportWindow window = resolveReportWindow(request, now);
    aggregateData.dateRangeLabel = window.label;

    for (const auto& provider : viewModel.providers) {
        for (const auto& quota : provider.quotas) {
            ReportQuotaSnapshot snapshot;
            snapshot.providerKey = provider.providerKey;
            snapshot.providerDisplayName = provider.displayName;
            snapshot.accountLabel = provider.accountId;
            snapshot.planName = quota.planName;
            snapshot.windowLabel = quota.windowType;
            snapshot.lastSyncLabel = provider.lastSyncTime;
            snapshot.confidenceTier = quota.confidenceTier;
            snapshot.sourceLabel = reportSourceLabelForSurface(quota.providerSurface);
            snapshot.used = quota.used;
            snapshot.limit = quota.limit;
            snapshot.remaining = quota.remaining;
            snapshot.remainingPercentage = quota.remainingPercentage;
            snapshot.isBackfilled = quota.isBackfilled;
            snapshot.hasUnknownGaps = quota.hasUnknownGaps;
            aggregateData.quotaSnapshots.push_back(std::move(snapshot));
        }
    }

    const std::vector<std::string> providerKeys = collectReportProviderKeys(viewModel);
    const std::vector<std::string> accountIds = collectReportAccountIds(viewModel);
    if (!storage.isOpen()) {
        if (metadata.containsBackfilledData || metadata.containsUnknownGaps) {
            std::ostringstream summary;
            summary << "Report metadata indicates ";
            if (metadata.containsBackfilledData) {
                summary << "backfilled data";
            }
            if (metadata.containsBackfilledData && metadata.containsUnknownGaps) {
                summary << " and ";
            }
            if (metadata.containsUnknownGaps) {
                summary << "unknown gaps";
            }
            summary << " within the current scope.";
            aggregateData.gapBackfillSummary = summary.str();
        }
        return aggregateData;
    }

    const std::string fromIso = formatTimeUtcIso(window.start);
    const std::string toIso = formatTimeUtcIso(window.end);
    auto appendScopeFilters = [&](std::string& sql,
                                  std::vector<std::string>& params,
                                  const char* providerColumn,
                                  const char* accountColumn) {
        appendProviderFilterClause(sql, params, providerKeys, providerColumn);
        appendAccountFilterClause(sql, params, accountIds, accountColumn);
    };

    auto appendDailyBucket = [](std::map<std::string, ReportSummaryBucket>& buckets,
                                const std::string& dayLabel,
                                long long requestCount,
                                long long totalTokens,
                                long long totalCostMinorUnits) {
        auto& bucket = buckets[dayLabel];
        bucket.label = dayLabel;
        bucket.periodLabel = dayLabel;
        bucket.requestCount += requestCount;
        bucket.totalTokens += totalTokens;
        bucket.totalCostMinorUnits += totalCostMinorUnits;
    };

    auto accumulateDailyBuckets = [&](const QueryResult& result,
                                      std::map<std::string, ReportSummaryBucket>& buckets) {
        if (!result.success()) {
            return;
        }
        for (size_t row = 0; row < result.rowCount(); ++row) {
            appendDailyBucket(
                buckets,
                result.getString(row, 0),
                result.getInt64(row, 1),
                result.getInt64(row, 2),
                result.getInt64(row, 3));
        }
    };

    auto queryRawDailyBuckets = [&](std::time_t from,
                                    std::time_t to,
                                    bool missingDailyRollupsOnly,
                                    std::map<std::string, ReportSummaryBucket>& buckets) {
        if (from > to) {
            return;
        }

        std::string sql =
            "SELECT substr(ue.event_time_utc, 1, 10), COUNT(*), "
            "COALESCE(SUM(ue.total_tokens), 0), COALESCE(SUM(ue.cost_minor_units), 0) "
            "FROM usage_events ue "
            "JOIN providers p ON p.id = ue.provider_id "
            "JOIN accounts a ON a.id = ue.account_id "
            "WHERE ue.event_time_utc >= ? AND ue.event_time_utc <= ?";
        std::vector<std::string> params{formatTimeUtcIso(from), formatTimeUtcIso(to)};
        appendScopeFilters(sql, params, "p.provider_key", "a.account_key");
        sql += " AND ue.source_type != 'codex_local_rollout'";
        if (missingDailyRollupsOnly) {
            sql +=
                " AND NOT EXISTS ("
                "    SELECT 1 FROM usage_rollups_daily r "
                "    WHERE r.provider_id = ue.provider_id "
                "      AND r.account_id = ue.account_id "
                "      AND r.model_key = COALESCE(ue.model_key, '') "
                "      AND r.rollup_period_start_utc = substr(ue.event_time_utc, 1, 10) || 'T00:00:00Z'"
                ")";
        }
        sql += " GROUP BY substr(ue.event_time_utc, 1, 10) ORDER BY substr(ue.event_time_utc, 1, 10) ASC;";
        accumulateDailyBuckets(storage.execute(sql, params), buckets);
    };

    auto queryDailyRollupBuckets = [&](std::time_t fromDayStart,
                                       std::time_t toDayStart,
                                       std::map<std::string, ReportSummaryBucket>& buckets) {
        if (fromDayStart > toDayStart) {
            return;
        }

        std::string sql =
            "SELECT substr(r.rollup_period_start_utc, 1, 10), "
            "COALESCE(SUM(r.request_count), 0), "
            "COALESCE(SUM(r.total_tokens), 0), "
            "COALESCE(SUM(r.total_cost_minor_units), 0) "
            "FROM usage_rollups_daily r "
            "JOIN providers p ON p.id = r.provider_id "
            "JOIN accounts a ON a.id = r.account_id "
            "WHERE r.rollup_period_start_utc >= ? AND r.rollup_period_start_utc <= ?";
        std::vector<std::string> params{formatTimeUtcIso(fromDayStart), formatTimeUtcIso(toDayStart)};
        appendScopeFilters(sql, params, "p.provider_key", "a.account_key");
        sql += " GROUP BY substr(r.rollup_period_start_utc, 1, 10) ORDER BY substr(r.rollup_period_start_utc, 1, 10) ASC;";
        accumulateDailyBuckets(storage.execute(sql, params), buckets);
    };

    std::map<std::string, ReportSummaryBucket> dailyBuckets;
    const auto fullDays = fullyCoveredDailyRange(window.start, window.end);
    if (!fullDays.has_value()) {
        queryRawDailyBuckets(window.start, window.end, false, dailyBuckets);
    } else {
        if (window.start < fullDays->first) {
            queryRawDailyBuckets(window.start, fullDays->first - 1, false, dailyBuckets);
        }
        queryDailyRollupBuckets(fullDays->first, fullDays->second, dailyBuckets);
        queryRawDailyBuckets(fullDays->first, fullDays->second + 86399, true, dailyBuckets);
        const std::time_t trailingStart = fullDays->second + 86400;
        if (trailingStart <= window.end) {
            queryRawDailyBuckets(trailingStart, window.end, false, dailyBuckets);
        }
    }

    std::map<std::time_t, ReportSummaryBucket> weeklyBuckets;
    for (const auto& entry : dailyBuckets) {
        aggregateData.dailySummaries.push_back(entry.second);
        if (const auto parsedDay = parseReportDay(entry.first)) {
            const std::time_t weekStart = utcWeekStart(*parsedDay);
            auto& weekly = weeklyBuckets[weekStart];
            weekly.label = "Week of " + formatReportDayLabel(weekStart);
            weekly.periodLabel = formatReportWindowLabel(weekStart, weekStart + (6 * 86400));
            weekly.requestCount += entry.second.requestCount;
            weekly.totalTokens += entry.second.totalTokens;
            weekly.totalCostMinorUnits += entry.second.totalCostMinorUnits;
        }
    }

    for (const auto& entry : weeklyBuckets) {
        aggregateData.weeklySummaries.push_back(entry.second);
    }

    aggregateData.recentTrend.reserve(aggregateData.dailySummaries.size());
    for (const auto& bucket : aggregateData.dailySummaries) {
        ReportTrendPoint point;
        point.label = bucket.label.size() > 5 ? bucket.label.substr(5) : bucket.label;
        point.requestCount = bucket.requestCount;
        point.totalTokens = bucket.totalTokens;
        point.totalCostMinorUnits = bucket.totalCostMinorUnits;
        aggregateData.recentTrend.push_back(std::move(point));
    }

    auto sortBreakdowns = [](std::vector<ReportBreakdownRow>& rows) {
        std::sort(
            rows.begin(),
            rows.end(),
            [](const ReportBreakdownRow& lhs, const ReportBreakdownRow& rhs) {
                if (lhs.totalTokens != rhs.totalTokens) {
                    return lhs.totalTokens > rhs.totalTokens;
                }
                return lhs.label < rhs.label;
            });
    };

    {
        std::map<std::string, ReportBreakdownRow> rows;
        auto appendRow = [&rows](const std::string& key,
                                 const std::string& label,
                                 const std::string& secondaryLabel,
                                 long long requestCount,
                                 long long totalTokens,
                                 long long totalCostMinorUnits) {
            auto& row = rows[key];
            row.key = key;
            row.label = label;
            row.secondaryLabel = secondaryLabel;
            row.requestCount += requestCount;
            row.totalTokens += totalTokens;
            row.totalCostMinorUnits += totalCostMinorUnits;
        };
        auto accumulateProviderBreakdown = [&](const QueryResult& result) {
            if (!result.success()) {
                return;
            }
            for (size_t rowIndex = 0; rowIndex < result.rowCount(); ++rowIndex) {
                appendRow(
                    result.getString(rowIndex, 0),
                    result.getString(rowIndex, 1),
                    "",
                    result.getInt64(rowIndex, 2),
                    result.getInt64(rowIndex, 3),
                    result.getInt64(rowIndex, 4));
            }
        };
        auto queryProviderBreakdownRaw = [&](std::time_t from,
                                             std::time_t to,
                                             bool missingDailyRollupsOnly) {
            if (from > to) {
                return;
            }
            std::string sql =
                "SELECT p.provider_key, p.display_name, COUNT(*), "
                "COALESCE(SUM(ue.total_tokens), 0), COALESCE(SUM(ue.cost_minor_units), 0) "
                "FROM usage_events ue "
                "JOIN providers p ON p.id = ue.provider_id "
                "JOIN accounts a ON a.id = ue.account_id "
                "WHERE ue.event_time_utc >= ? AND ue.event_time_utc <= ?";
            std::vector<std::string> params{formatTimeUtcIso(from), formatTimeUtcIso(to)};
            appendScopeFilters(sql, params, "p.provider_key", "a.account_key");
            sql += " AND ue.source_type != 'codex_local_rollout'";
            if (missingDailyRollupsOnly) {
                sql +=
                    " AND NOT EXISTS ("
                    "    SELECT 1 FROM usage_rollups_daily r "
                    "    WHERE r.provider_id = ue.provider_id "
                    "      AND r.account_id = ue.account_id "
                    "      AND r.model_key = COALESCE(ue.model_key, '') "
                    "      AND r.rollup_period_start_utc = substr(ue.event_time_utc, 1, 10) || 'T00:00:00Z'"
                    ")";
            }
            sql += " GROUP BY p.provider_key, p.display_name;";
            accumulateProviderBreakdown(storage.execute(sql, params));
        };
        auto queryProviderBreakdownRollups = [&](std::time_t fromDayStart, std::time_t toDayStart) {
            if (fromDayStart > toDayStart) {
                return;
            }
            std::string sql =
                "SELECT p.provider_key, p.display_name, COALESCE(SUM(r.request_count), 0), "
                "COALESCE(SUM(r.total_tokens), 0), COALESCE(SUM(r.total_cost_minor_units), 0) "
                "FROM usage_rollups_daily r "
                "JOIN providers p ON p.id = r.provider_id "
                "JOIN accounts a ON a.id = r.account_id "
                "WHERE r.rollup_period_start_utc >= ? AND r.rollup_period_start_utc <= ?";
            std::vector<std::string> params{formatTimeUtcIso(fromDayStart), formatTimeUtcIso(toDayStart)};
            appendScopeFilters(sql, params, "p.provider_key", "a.account_key");
            sql += " GROUP BY p.provider_key, p.display_name;";
            accumulateProviderBreakdown(storage.execute(sql, params));
        };

        if (!fullDays.has_value()) {
            queryProviderBreakdownRaw(window.start, window.end, false);
        } else {
            if (window.start < fullDays->first) {
                queryProviderBreakdownRaw(window.start, fullDays->first - 1, false);
            }
            queryProviderBreakdownRollups(fullDays->first, fullDays->second);
            queryProviderBreakdownRaw(fullDays->first, fullDays->second + 86399, true);
            const std::time_t trailingStart = fullDays->second + 86400;
            if (trailingStart <= window.end) {
                queryProviderBreakdownRaw(trailingStart, window.end, false);
            }
        }

        for (auto& entry : rows) {
            aggregateData.providerBreakdown.push_back(std::move(entry.second));
        }
        sortBreakdowns(aggregateData.providerBreakdown);
    }

    {
        std::map<std::string, ReportBreakdownRow> rows;
        auto appendRow = [&rows](const std::string& key,
                                 const std::string& label,
                                 const std::string& secondaryLabel,
                                 long long requestCount,
                                 long long totalTokens,
                                 long long totalCostMinorUnits) {
            auto& row = rows[key];
            row.key = key;
            row.label = label;
            row.secondaryLabel = secondaryLabel;
            row.requestCount += requestCount;
            row.totalTokens += totalTokens;
            row.totalCostMinorUnits += totalCostMinorUnits;
        };
        auto accumulateAccountBreakdown = [&](const QueryResult& result) {
            if (!result.success()) {
                return;
            }
            for (size_t rowIndex = 0; rowIndex < result.rowCount(); ++rowIndex) {
                const std::string accountKey = result.getString(rowIndex, 0);
                const std::string providerLabel = result.getString(rowIndex, 2);
                appendRow(
                    accountKey + "|" + providerLabel,
                    result.getString(rowIndex, 1),
                    providerLabel,
                    result.getInt64(rowIndex, 3),
                    result.getInt64(rowIndex, 4),
                    result.getInt64(rowIndex, 5));
            }
        };
        auto queryAccountBreakdownRaw = [&](std::time_t from,
                                            std::time_t to,
                                            bool missingDailyRollupsOnly) {
            if (from > to) {
                return;
            }
            std::string sql =
                "SELECT a.account_key, a.display_name, p.display_name, COUNT(*), "
                "COALESCE(SUM(ue.total_tokens), 0), COALESCE(SUM(ue.cost_minor_units), 0) "
                "FROM usage_events ue "
                "JOIN providers p ON p.id = ue.provider_id "
                "JOIN accounts a ON a.id = ue.account_id "
                "WHERE ue.event_time_utc >= ? AND ue.event_time_utc <= ?";
            std::vector<std::string> params{formatTimeUtcIso(from), formatTimeUtcIso(to)};
            appendScopeFilters(sql, params, "p.provider_key", "a.account_key");
            sql += " AND ue.source_type != 'codex_local_rollout'";
            if (missingDailyRollupsOnly) {
                sql +=
                    " AND NOT EXISTS ("
                    "    SELECT 1 FROM usage_rollups_daily r "
                    "    WHERE r.provider_id = ue.provider_id "
                    "      AND r.account_id = ue.account_id "
                    "      AND r.model_key = COALESCE(ue.model_key, '') "
                    "      AND r.rollup_period_start_utc = substr(ue.event_time_utc, 1, 10) || 'T00:00:00Z'"
                    ")";
            }
            sql += " GROUP BY a.account_key, a.display_name, p.display_name;";
            accumulateAccountBreakdown(storage.execute(sql, params));
        };
        auto queryAccountBreakdownRollups = [&](std::time_t fromDayStart, std::time_t toDayStart) {
            if (fromDayStart > toDayStart) {
                return;
            }
            std::string sql =
                "SELECT a.account_key, a.display_name, p.display_name, COALESCE(SUM(r.request_count), 0), "
                "COALESCE(SUM(r.total_tokens), 0), COALESCE(SUM(r.total_cost_minor_units), 0) "
                "FROM usage_rollups_daily r "
                "JOIN providers p ON p.id = r.provider_id "
                "JOIN accounts a ON a.id = r.account_id "
                "WHERE r.rollup_period_start_utc >= ? AND r.rollup_period_start_utc <= ?";
            std::vector<std::string> params{formatTimeUtcIso(fromDayStart), formatTimeUtcIso(toDayStart)};
            appendScopeFilters(sql, params, "p.provider_key", "a.account_key");
            sql += " GROUP BY a.account_key, a.display_name, p.display_name;";
            accumulateAccountBreakdown(storage.execute(sql, params));
        };

        if (!fullDays.has_value()) {
            queryAccountBreakdownRaw(window.start, window.end, false);
        } else {
            if (window.start < fullDays->first) {
                queryAccountBreakdownRaw(window.start, fullDays->first - 1, false);
            }
            queryAccountBreakdownRollups(fullDays->first, fullDays->second);
            queryAccountBreakdownRaw(fullDays->first, fullDays->second + 86399, true);
            const std::time_t trailingStart = fullDays->second + 86400;
            if (trailingStart <= window.end) {
                queryAccountBreakdownRaw(trailingStart, window.end, false);
            }
        }

        for (auto& entry : rows) {
            aggregateData.accountBreakdown.push_back(std::move(entry.second));
        }
        sortBreakdowns(aggregateData.accountBreakdown);
    }

    {
        std::map<std::string, ReportBreakdownRow> rows;
        auto appendRow = [&rows](const std::string& key,
                                 long long requestCount,
                                 long long totalTokens,
                                 long long totalCostMinorUnits) {
            auto& row = rows[key];
            row.key = key;
            row.label = key;
            row.requestCount += requestCount;
            row.totalTokens += totalTokens;
            row.totalCostMinorUnits += totalCostMinorUnits;
        };
        auto accumulateModelBreakdown = [&](const QueryResult& result) {
            if (!result.success()) {
                return;
            }
            for (size_t rowIndex = 0; rowIndex < result.rowCount(); ++rowIndex) {
                const std::string modelKey = result.getString(rowIndex, 0);
                appendRow(
                    modelKey,
                    result.getInt64(rowIndex, 1),
                    result.getInt64(rowIndex, 2),
                    result.getInt64(rowIndex, 3));
            }
        };
        auto queryModelBreakdownRaw = [&](std::time_t from,
                                          std::time_t to,
                                          bool missingDailyRollupsOnly) {
            if (from > to) {
                return;
            }
            std::string sql =
                "SELECT COALESCE(NULLIF(ue.model_key, ''), '(unknown)'), COUNT(*), "
                "COALESCE(SUM(ue.total_tokens), 0), COALESCE(SUM(ue.cost_minor_units), 0) "
                "FROM usage_events ue "
                "JOIN providers p ON p.id = ue.provider_id "
                "JOIN accounts a ON a.id = ue.account_id "
                "WHERE ue.event_time_utc >= ? AND ue.event_time_utc <= ?";
            std::vector<std::string> params{formatTimeUtcIso(from), formatTimeUtcIso(to)};
            appendScopeFilters(sql, params, "p.provider_key", "a.account_key");
            sql += " AND ue.source_type != 'codex_local_rollout'";
            if (missingDailyRollupsOnly) {
                sql +=
                    " AND NOT EXISTS ("
                    "    SELECT 1 FROM usage_rollups_daily r "
                    "    WHERE r.provider_id = ue.provider_id "
                    "      AND r.account_id = ue.account_id "
                    "      AND r.model_key = COALESCE(ue.model_key, '') "
                    "      AND r.rollup_period_start_utc = substr(ue.event_time_utc, 1, 10) || 'T00:00:00Z'"
                    ")";
            }
            sql += " GROUP BY COALESCE(NULLIF(ue.model_key, ''), '(unknown)');";
            accumulateModelBreakdown(storage.execute(sql, params));
        };
        auto queryModelBreakdownRollups = [&](std::time_t fromDayStart, std::time_t toDayStart) {
            if (fromDayStart > toDayStart) {
                return;
            }
            std::string sql =
                "SELECT COALESCE(NULLIF(r.model_key, ''), '(unknown)'), COALESCE(SUM(r.request_count), 0), "
                "COALESCE(SUM(r.total_tokens), 0), COALESCE(SUM(r.total_cost_minor_units), 0) "
                "FROM usage_rollups_daily r "
                "JOIN providers p ON p.id = r.provider_id "
                "JOIN accounts a ON a.id = r.account_id "
                "WHERE r.rollup_period_start_utc >= ? AND r.rollup_period_start_utc <= ?";
            std::vector<std::string> params{formatTimeUtcIso(fromDayStart), formatTimeUtcIso(toDayStart)};
            appendScopeFilters(sql, params, "p.provider_key", "a.account_key");
            sql += " GROUP BY COALESCE(NULLIF(r.model_key, ''), '(unknown)');";
            accumulateModelBreakdown(storage.execute(sql, params));
        };

        if (!fullDays.has_value()) {
            queryModelBreakdownRaw(window.start, window.end, false);
        } else {
            if (window.start < fullDays->first) {
                queryModelBreakdownRaw(window.start, fullDays->first - 1, false);
            }
            queryModelBreakdownRollups(fullDays->first, fullDays->second);
            queryModelBreakdownRaw(fullDays->first, fullDays->second + 86399, true);
            const std::time_t trailingStart = fullDays->second + 86400;
            if (trailingStart <= window.end) {
                queryModelBreakdownRaw(trailingStart, window.end, false);
            }
        }

        for (auto& entry : rows) {
            aggregateData.modelBreakdown.push_back(std::move(entry.second));
        }
        sortBreakdowns(aggregateData.modelBreakdown);
    }

    long long reconstructedGapCount = 0;
    long long unknownGapCount = 0;
    {
        std::string sql =
            "SELECT "
            "COALESCE(SUM(CASE WHEN gm.classification = 'reconstructed' THEN 1 ELSE 0 END), 0), "
            "COALESCE(SUM(CASE WHEN gm.classification = 'unknown' THEN 1 ELSE 0 END), 0) "
            "FROM gap_markers gm "
            "JOIN providers p ON p.id = gm.provider_id "
            "JOIN accounts a ON a.id = gm.account_id "
            "WHERE gm.period_end_utc >= ? AND gm.period_start_utc <= ?";
        std::vector<std::string> params{fromIso, toIso};
        appendProviderFilterClause(sql, params, providerKeys, "p.provider_key");
        appendAccountFilterClause(sql, params, accountIds, "a.account_key");
        QueryResult result = storage.execute(sql, params);
        if (result.success() && result.rowCount() > 0) {
            reconstructedGapCount = result.getInt64(0, 0);
            unknownGapCount = result.getInt64(0, 1);
        }
    }

    long long importedBackfillCount = 0;
    long long mismatchCount = 0;
    long long failedBackfillCount = 0;
    {
        std::string sql =
            "SELECT "
            "COALESCE(SUM(CASE WHEN rc.outcome = 'imported' THEN 1 ELSE 0 END), 0), "
            "COALESCE(SUM(CASE WHEN rc.comparison_result = 'mismatch' THEN 1 ELSE 0 END), 0), "
            "COALESCE(SUM(CASE WHEN rc.outcome IN ('fetch_failed', 'persist_failed') THEN 1 ELSE 0 END), 0) "
            "FROM recovery_checkpoints rc "
            "JOIN providers p ON p.id = rc.provider_id "
            "JOIN accounts a ON a.id = rc.account_id "
            "WHERE rc.sync_kind = ?";
        std::vector<std::string> params{kRuntimeBackfillSyncKind};
        appendProviderFilterClause(sql, params, providerKeys, "p.provider_key");
        appendAccountFilterClause(sql, params, accountIds, "a.account_key");
        QueryResult result = storage.execute(sql, params);
        if (result.success() && result.rowCount() > 0) {
            importedBackfillCount = result.getInt64(0, 0);
            mismatchCount = result.getInt64(0, 1);
            failedBackfillCount = result.getInt64(0, 2);
        }
    }

    std::ostringstream summary;
    bool wroteSummary = false;
    if (importedBackfillCount > 0) {
        summary << importedBackfillCount << " provider recovery checkpoint";
        if (importedBackfillCount != 1) {
            summary << "s";
        }
        summary << " imported provider history";
        wroteSummary = true;
    }
    if (mismatchCount > 0) {
        if (wroteSummary) {
            summary << "; ";
        }
        summary << mismatchCount << " checkpoint";
        if (mismatchCount != 1) {
            summary << "s";
        }
        summary << " recorded imported-versus-local mismatches";
        wroteSummary = true;
    }
    if (failedBackfillCount > 0) {
        if (wroteSummary) {
            summary << "; ";
        }
        summary << failedBackfillCount << " recovery fetch failure";
        if (failedBackfillCount != 1) {
            summary << "s";
        }
        wroteSummary = true;
    }
    if (reconstructedGapCount > 0) {
        if (wroteSummary) {
            summary << "; ";
        }
        summary << reconstructedGapCount << " reconstructed gap marker";
        if (reconstructedGapCount != 1) {
            summary << "s";
        }
        wroteSummary = true;
    }
    if (unknownGapCount > 0) {
        if (wroteSummary) {
            summary << "; ";
        }
        summary << unknownGapCount << " unknown gap marker";
        if (unknownGapCount != 1) {
            summary << "s";
        }
        wroteSummary = true;
    }
    if (!wroteSummary) {
        if (metadata.containsBackfilledData || metadata.containsUnknownGaps) {
            summary << "Current quotas indicate backfilled or gap-affected data within the report scope.";
        } else {
            summary << "No persisted gap or recovery warnings were found for the current report scope.";
        }
    }
    aggregateData.gapBackfillSummary = summary.str();

    return aggregateData;
}

PersistedMetricSnapshot buildRollupBackedMetricSnapshot(
    StorageService& storage,
    std::time_t now,
    PersistedMetricSnapshotMode mode = PersistedMetricSnapshotMode::Full) {
    PersistedMetricSnapshot snapshot;
    if (!storage.isOpen()) {
        return snapshot;
    }

    const std::time_t thirtyMinuteStart = std::max<std::time_t>(0, now - (30 * 60));
    const QuantifiedUsageAggregate lastThirtyMinutes =
        queryHybridUsageAggregate(storage, thirtyMinuteStart, now);
    if (lastThirtyMinutes.requestCount > 0) {
        std::ostringstream suffix;
        suffix << " - " << lastThirtyMinutes.requestCount << " req / "
               << formatCompactTokenCount(lastThirtyMinutes.totalTokens) << " tok in 30m";
        snapshot.trayTooltipSuffix = suffix.str();
    }

    if (mode == PersistedMetricSnapshotMode::TrayOnly) {
        return snapshot;
    }

    const DashboardActivityWindowSpec windows[] = {
        {"Last 5 Minutes", 5 * 60, false},
        {"Last 30 Minutes", 30 * 60, false},
        {"Last 5 Hours", 5 * 60 * 60, false},
        {"Today", 0, true}
    };

    struct WindowUsageSummary {
        const char* label = "";
        QuantifiedUsageAggregate aggregate;
    };

    std::vector<WindowUsageSummary> windowUsage;
    windowUsage.reserve(sizeof(windows) / sizeof(windows[0]));

    long long maxRequestCount = 0;
    for (const auto& window : windows) {
        const std::time_t windowStart = window.useUtcDayStart
            ? utcDayStart(now)
            : std::max<std::time_t>(0, now - window.windowSeconds);
        WindowUsageSummary summary;
        summary.label = window.label;
        summary.aggregate = queryHybridUsageAggregate(storage, windowStart, now);
        maxRequestCount = std::max(maxRequestCount, summary.aggregate.requestCount);
        windowUsage.push_back(std::move(summary));
    }

    if (maxRequestCount > 0) {
        snapshot.dashboardSummaries.reserve(windowUsage.size());
        for (const auto& summary : windowUsage) {
            DashboardSummaryItem item;
            item.label = summary.label;
            item.detail = buildQuantifiedUsageDetail(
                summary.aggregate.requestCount,
                summary.aggregate.totalTokens,
                summary.aggregate.providerCount);
            if (summary.aggregate.requestCount > 0) {
                const double relativePercentage =
                    (static_cast<double>(summary.aggregate.requestCount) / static_cast<double>(maxRequestCount)) * 100.0;
                item.percentage = std::max(
                    1,
                    static_cast<int>(std::round(std::clamp(relativePercentage, 0.0, 100.0))));
            }
            item.badge = "Usage";
            snapshot.dashboardSummaries.push_back(std::move(item));
        }
    }

    const std::time_t fiveMinuteStart = std::max<std::time_t>(0, now - (5 * 60));
    std::vector<QuantifiedProviderUsageAggregate> providerUsage =
        queryHybridProviderUsage(storage, thirtyMinuteStart, now);
    if (!providerUsage.empty()) {
        const auto hotUsage = queryHybridProviderUsage(storage, fiveMinuteStart, now);
        std::map<std::string, long long> hotWindowRequests;
        for (const auto& hot : hotUsage) {
            hotWindowRequests[hot.providerKey] += hot.requestCount;
        }

        for (auto& aggregate : providerUsage) {
            aggregate.hotWindowRequests = hotWindowRequests[aggregate.providerKey];
        }

        std::sort(
            providerUsage.begin(),
            providerUsage.end(),
            [](const QuantifiedProviderUsageAggregate& left,
               const QuantifiedProviderUsageAggregate& right) {
                if (left.requestCount != right.requestCount) {
                    return left.requestCount > right.requestCount;
                }
                if (left.totalTokens != right.totalTokens) {
                    return left.totalTokens > right.totalTokens;
                }
                return left.providerDisplayName < right.providerDisplayName;
            });

        constexpr size_t kMaxTaskbarProviders = 2;
        const size_t providerCount = std::min(providerUsage.size(), kMaxTaskbarProviders);
        snapshot.taskbarMetrics.reserve(providerCount);
        for (size_t index = 0; index < providerCount; ++index) {
            const auto& aggregate = providerUsage[index];
            TaskbarMetrics metric;
            metric.providerKey = aggregate.providerKey;
            metric.providerDisplayName = aggregate.providerDisplayName;
            metric.kind = TaskbarMetricKind::PersistedActivity;
            metric.used = aggregate.requestCount;
            metric.lastUpdated = now;
            metric.summaryLabel = metric.providerDisplayName + " 30m";
            metric.primaryMetric = formatCompactRequestCount(aggregate.requestCount);
            metric.secondaryMetric = formatCompactTokenCount(aggregate.totalTokens) + " tok";
            metric.statusIcon = aggregate.hotWindowRequests > 0 ? "+" : "~";
            snapshot.taskbarMetrics.push_back(std::move(metric));
        }
    }

    return snapshot;
}

std::vector<DashboardSummaryItem> buildPersistedActivityDashboardSummaries(
    StorageService& storage,
    std::time_t now,
    const DashboardPreferences& preferences) {
    std::vector<DashboardSummaryItem> summaries;
    if (!storage.isOpen()) {
        return summaries;
    }

    const DashboardActivityWindowSpec windows[] = {
        {"Last 5 Minutes", 5 * 60, false},
        {"Last 30 Minutes", 30 * 60, false},
        {"Last 5 Hours", 5 * 60 * 60, false},
        {"Today", 0, true}
    };

    summaries.reserve(sizeof(windows) / sizeof(windows[0]));
    for (const auto& window : windows) {
        const std::time_t windowStart = window.useUtcDayStart
            ? utcDayStart(now)
            : std::max<std::time_t>(0, now - window.windowSeconds);
        const long long windowSpanSeconds = window.useUtcDayStart
            ? std::max<long long>(1, static_cast<long long>(now - windowStart))
            : window.windowSeconds;

        std::string query =
            "SELECT COUNT(*), COALESCE(SUM(ae.duration_seconds), 0), COUNT(DISTINCT ae.provider_id) "
            "FROM activity_events ae "
            "JOIN providers p ON p.id = ae.provider_id "
            "WHERE ae.source_type = ? AND ae.started_at_utc >= ? AND ae.started_at_utc <= ?";
        std::vector<std::string> params = {
            "local",
            formatTimeUtcIso(windowStart),
            formatTimeUtcIso(now)
        };
        if (!preferences.openaiSubscriptionTrackingEnabled) {
            query += " AND p.provider_key != ?";
            params.push_back("openai");
        }
        if (!preferences.miniMaxSubscriptionTrackingEnabled) {
            query += " AND p.provider_key != ?";
            params.push_back("minimax");
        }
        if (!preferences.claudeCodeEnabled) {
            query += " AND p.provider_key != ?";
            params.push_back("claude_code");
        }
        if (!preferences.kimiCodeEnabled) {
            query += " AND p.provider_key != ?";
            params.push_back("kimi");
        }
        query += ";";

        QueryResult result = storage.execute(query, params);
        if (!result.success() || result.rowCount() == 0) {
            return {};
        }

        const long long sessionCount = result.getInt64(0, 0);
        const long long activeSeconds = result.getInt64(0, 1);
        const long long providerCount = result.getInt64(0, 2);

        DashboardSummaryItem summary;
        summary.label = window.label;
        summary.detail = buildDashboardActivityDetail(sessionCount, activeSeconds, providerCount);
        summary.percentage = computeWindowOccupancyPercentage(activeSeconds, windowSpanSeconds);
        summary.badge = "Local";
        summaries.push_back(summary);
    }

    return summaries;
}

std::vector<TaskbarMetrics> buildPersistedActivityTaskbarMetrics(
    StorageService& storage,
    std::time_t now,
    const DashboardPreferences& preferences) {
    std::vector<TaskbarMetrics> metrics;
    if (!storage.isOpen()) {
        return metrics;
    }

    std::string query =
        "SELECT p.provider_key, p.display_name, COUNT(*), COALESCE(SUM(ae.duration_seconds), 0), "
        "SUM(CASE WHEN ae.started_at_utc >= ? THEN 1 ELSE 0 END), "
        "MAX(ae.started_at_utc) "
        "FROM activity_events ae "
        "JOIN providers p ON p.id = ae.provider_id "
        "WHERE ae.source_type = ? AND ae.started_at_utc >= ? AND ae.started_at_utc <= ? ";
    std::vector<std::string> params = {
        formatTimeUtcIso(std::max<std::time_t>(0, now - (5 * 60))),
        "local",
        formatTimeUtcIso(std::max<std::time_t>(0, now - (30 * 60))),
        formatTimeUtcIso(now)
    };
    if (!preferences.openaiSubscriptionTrackingEnabled) {
        query += "AND p.provider_key != ? ";
        params.push_back("openai");
    }
    if (!preferences.miniMaxSubscriptionTrackingEnabled) {
        query += "AND p.provider_key != ? ";
        params.push_back("minimax");
    }
    if (!preferences.claudeCodeEnabled) {
        query += "AND p.provider_key != ? ";
        params.push_back("claude_code");
    }
    if (!preferences.kimiCodeEnabled) {
        query += "AND p.provider_key != ? ";
        params.push_back("kimi");
    }
    query +=
        "GROUP BY p.provider_key, p.display_name "
        "ORDER BY MAX(ae.started_at_utc) DESC, p.display_name ASC;";
    QueryResult result = storage.execute(query, params);
    if (!result.success()) {
        return metrics;
    }

    constexpr size_t kMaxTaskbarProviders = 2;
    const size_t rowCount = std::min(result.rowCount(), kMaxTaskbarProviders);
    metrics.reserve(rowCount);
    for (size_t row = 0; row < rowCount; ++row) {
        const long long sessionCount = result.getInt64(row, 2);
        const long long activeSeconds = result.getInt64(row, 3);
        const long long hotWindowSessions = result.getInt64(row, 4);
        if (sessionCount <= 0) {
            continue;
        }

        TaskbarMetrics metric;
        metric.providerKey = result.getString(row, 0);
        metric.providerDisplayName = result.getString(row, 1);
        metric.kind = TaskbarMetricKind::PersistedActivity;
        metric.used = sessionCount;
        metric.lastUpdated = now;
        metric.summaryLabel = metric.providerDisplayName + " 30m";
        metric.primaryMetric = formatCompactSessionCount(sessionCount);
        metric.secondaryMetric = buildTaskbarActivityDetail(sessionCount, activeSeconds);
        metric.statusIcon = hotWindowSessions > 0 ? "!" : "~";
        metrics.push_back(metric);
    }

    return metrics;
}

QuotaInfo makeQuota(
    const std::string& planName,
    const std::string& windowType,
    long long used,
    long long limit,
    const std::string& confidenceTier,
    long long remaining = -1) {
    QuotaInfo quota;
    quota.planName = planName;
    quota.windowType = windowType;
    quota.confidenceTier = confidenceTier;
    quota.limit = std::max<long long>(0, limit);
    quota.remaining = quota.limit > 0
        ? std::clamp(remaining >= 0 ? remaining : (quota.limit - std::max<long long>(0, used)),
                     0LL,
                     quota.limit)
        : std::max<long long>(0, remaining);
    quota.used = quota.limit > 0 ? std::clamp(quota.limit - quota.remaining, 0LL, quota.limit)
                                 : std::max<long long>(0, used);
    if (quota.limit > 0) {
        quota.usedPercentage =
            (static_cast<double>(quota.used) / static_cast<double>(quota.limit)) * 100.0;
        quota.remainingPercentage =
            (static_cast<double>(quota.remaining) / static_cast<double>(quota.limit)) * 100.0;
    }
    quota.percentage = quota.usedPercentage;
    return quota;
}

ProviderStatus makeProvider(
    const std::string& providerKey,
    const std::string& displayName,
    const std::string& lastSyncTime,
    const std::vector<QuotaInfo>& quotas,
    ProviderState state = ProviderState::Sample) {
    ProviderStatus provider;
    provider.providerKey = providerKey;
    provider.displayName = displayName;
    provider.isEnabled = true;
    provider.state = state;
    provider.lastSyncTime = lastSyncTime;
    provider.quotas = quotas;
    return provider;
}

TaskbarMetrics makeTaskbarMetric(
    const std::string& providerKey,
    const std::string& displayName,
    long long used,
    long long limit,
    const std::string& confidenceTier,
    std::time_t lastUpdated) {
    TaskbarMetrics metrics;
    metrics.providerKey = providerKey;
    metrics.providerDisplayName = displayName;
    metrics.used = used;
    metrics.limit = limit;
    metrics.confidenceTier = confidenceTier;
    metrics.lastUpdated = lastUpdated;
    return metrics;
}

double computeRemainingPercentage(long long remaining, long long limit) {
    if (limit <= 0) {
        return -1.0;
    }
    return std::clamp(
        (static_cast<double>(remaining) / static_cast<double>(limit)) * 100.0,
        0.0,
        100.0);
}

NormalizedQuotaValues normalizeQuotaValues(const QuotaResponse& quota) {
    NormalizedQuotaValues values;
    values.limit = std::max<long long>(0, quota.limit);
    if (values.limit <= 0) {
        values.used = std::max<long long>(0, quota.used);
        values.remaining = std::max<long long>(0, quota.remaining);
        return values;
    }

    const long long derivedRemainingFromUsed =
        std::clamp(values.limit - std::max<long long>(0, quota.used), 0LL, values.limit);
    const bool hasExplicitRemaining =
        quota.remaining > 0 || quota.used >= quota.limit || quota.remaining == quota.limit;
    values.remaining = hasExplicitRemaining
        ? std::clamp(quota.remaining, 0LL, values.limit)
        : derivedRemainingFromUsed;
    values.used = std::clamp(values.limit - values.remaining, 0LL, values.limit);
    return values;
}

std::string normalizeOpenAIPlanLabel(const std::string& planKeyOrLabel) {
    if (planKeyOrLabel == "openai_api_import" ||
        planKeyOrLabel == "Imported Org Usage" ||
        planKeyOrLabel == "API Usage" ||
        planKeyOrLabel == "API Platform Import") {
        return "API Platform Import";
    }
    return "Subscription";
}

std::string normalizeMiniMaxPlanLabel(const std::string& planKeyOrLabel) {
    if (planKeyOrLabel == "minimax_subscription_token_plan" ||
        planKeyOrLabel == "token_plan" ||
        planKeyOrLabel == "MiniMax Token Plan" ||
        planKeyOrLabel == "Token Plan Remaining" ||
        planKeyOrLabel == "Token Plan") {
        return "Token Plan";
    }
    return "Coding Plan";
}

std::string normalizeClaudePlanLabel(const std::string& planKeyOrLabel) {
    if (planKeyOrLabel.empty() ||
        planKeyOrLabel == "claude_code_local_stats" ||
        planKeyOrLabel == "Claude Code Local" ||
        planKeyOrLabel == "Local Activity") {
        return "Local Activity";
    }

    return planKeyOrLabel;
}

std::string normalizeKimiPlanLabel(const std::string& planKeyOrLabel) {
    if (planKeyOrLabel == "kimi_subscription_api" ||
        planKeyOrLabel == "Kimi Subscription" ||
        planKeyOrLabel == "Subscription") {
        return "Subscription";
    }

    if (planKeyOrLabel.empty() ||
        planKeyOrLabel == "kimi_code_local" ||
        planKeyOrLabel == "kimi_local_runtime" ||
        planKeyOrLabel == "kimi_local_activity" ||
        planKeyOrLabel == "Kimi Code Local" ||
        planKeyOrLabel == "Local") {
        return "Local";
    }

    return planKeyOrLabel;
}

std::string quotaPlanLabelForProvider(
    const std::string& providerKey,
    const std::string& providerSurface,
    const std::string& persistedPlanKey = "",
    const std::string& persistedPlanLabel = "") {
    if (providerKey == "openai") {
        return normalizeOpenAIPlanLabel(
            !providerSurface.empty() ? providerSurface : persistedPlanLabel);
    }
    if (providerKey == "minimax") {
        if (!providerSurface.empty()) {
            return normalizeMiniMaxPlanLabel(providerSurface);
        }
        if (!persistedPlanKey.empty()) {
            return normalizeMiniMaxPlanLabel(persistedPlanKey);
        }
        if (!persistedPlanLabel.empty()) {
            return normalizeMiniMaxPlanLabel(persistedPlanLabel);
        }
    }
    if (providerKey == "kimi") {
        if (!providerSurface.empty()) {
            return normalizeKimiPlanLabel(providerSurface);
        }
        if (!persistedPlanKey.empty()) {
            return normalizeKimiPlanLabel(persistedPlanKey);
        }
        if (!persistedPlanLabel.empty()) {
            return normalizeKimiPlanLabel(persistedPlanLabel);
        }
        return "Local";
    }
    if (providerKey == "claude_code") {
        if (!providerSurface.empty()) {
            return normalizeClaudePlanLabel(providerSurface);
        }
        if (!persistedPlanKey.empty()) {
            return normalizeClaudePlanLabel(persistedPlanKey);
        }
        if (!persistedPlanLabel.empty()) {
            return normalizeClaudePlanLabel(persistedPlanLabel);
        }
        return "Local Activity";
    }

    if (providerKey == "grok" || providerKey == "cursor") {
        return "Subscription";
    }

    if (!persistedPlanLabel.empty()) {
        return persistedPlanLabel;
    }
    if (!providerSurface.empty()) {
        return providerSurface;
    }

    return "API Usage";
}

std::optional<DashboardViewModel> buildDashboardViewModelFromPersistedQuotaSnapshots(
    std::time_t now,
    StorageService& storage,
    const DashboardPreferences& preferences) {
    if (!storage.isOpen()) {
        return std::nullopt;
    }

    QueryResult result = storage.execute(
        "WITH latest_provider_snapshots AS ("
        "    SELECT provider_id, MAX(snapshot_time_utc) AS latest_snapshot_time "
        "    FROM quota_snapshots "
        "    GROUP BY provider_id"
        ") "
        "SELECT p.provider_key, "
        "       p.display_name, "
        "       qs.snapshot_time_utc, "
        "       COALESCE(pl.plan_key, ''), "
        "       COALESCE(pl.display_name, ''), "
        "       COALESCE(pl.quota_window, ''), "
        "       qs.used_value, "
        "       COALESCE(qs.limit_value, 0), "
        "       COALESCE(qs.remaining_value, "
        "           CASE WHEN qs.limit_value IS NOT NULL THEN qs.limit_value - qs.used_value ELSE 0 END), "
        "       qs.confidence_tier, qs.window_end_utc "
        "FROM latest_provider_snapshots latest "
        "JOIN quota_snapshots qs "
        "  ON qs.provider_id = latest.provider_id "
        " AND qs.snapshot_time_utc = latest.latest_snapshot_time "
        "JOIN providers p ON p.id = qs.provider_id "
        "LEFT JOIN plans pl ON pl.id = qs.plan_id "
        "WHERE p.enabled = 1 "
        "ORDER BY p.display_name ASC, COALESCE(pl.display_name, '') ASC;");
    if (!result.success() || result.rowCount() == 0) {
        return std::nullopt;
    }

    DashboardViewModel viewModel;
    viewModel.appVersion = kJiStatsAppVersion;
    viewModel.preferences = preferences;

    std::time_t freshestSnapshotTime = 0;
    for (size_t row = 0; row < result.rowCount(); ++row) {
        const std::string providerKey = result.getString(row, 0);
        if (!isProviderTrackingEnabled(preferences, providerKey)) {
            continue;
        }

        const std::string providerDisplayName = result.getString(row, 1);
        const std::string snapshotTimeLabel = result.getString(row, 2);
        const std::time_t snapshotTime = parseRuntimeTimestamp(snapshotTimeLabel);
        freshestSnapshotTime = std::max(freshestSnapshotTime, snapshotTime);

        auto providerIt = std::find_if(
            viewModel.providers.begin(),
            viewModel.providers.end(),
            [&providerKey](const ProviderStatus& provider) {
                return provider.providerKey == providerKey;
            });
        if (providerIt == viewModel.providers.end()) {
            ProviderStatus provider;
            provider.providerKey = providerKey;
            provider.displayName = providerDisplayName;
            provider.accountId = providerKey + ".runtime_status";
            provider.isEnabled = true;
            provider.lastSyncTime = snapshotTimeLabel;

            QuotaResponse snapshotQuota;
            snapshotQuota.success = true;
            snapshotQuota.snapshotTime = snapshotTime;
            provider.state = determineProviderState(snapshotQuota, now);
            viewModel.providers.push_back(std::move(provider));
            providerIt = std::prev(viewModel.providers.end());
        }

        const auto planKey = result.getString(row, 3);
        const long long used = result.getInt64(row, 6);
        const long long limit = result.getInt64(row, 7);
        const long long remaining = result.getInt64(row, 8);
        QuotaInfo quota = makeQuota(quotaPlanLabelForProvider(providerKey, "", planKey, result.getString(row, 4)),
                                    result.getString(row, 5), used, limit, result.getString(row, 9), remaining);
        if (providerKey == "grok" || providerKey == "cursor") {
            quota.providerSurface = providerKey + "_subscription_api";
            const auto resetTime = parseRuntimeTimestamp(result.getString(row, 10));
            quota.windowResetAt = resetTime > snapshotTime ? resetTime : 0;
        } else if (providerKey == "minimax") {
            // Runtime plan keys store the source ahead of the optional window suffix.
            const auto planSource = planKey.substr(0, planKey.find(':'));
            if (planSource == "minimax_subscription_coding_plan" || planSource == "coding_plan") {
                quota.providerSurface = "minimax_subscription_coding_plan";
            } else if (planSource == "minimax_subscription_token_plan" || planSource == "token_plan") {
                quota.providerSurface = "minimax_subscription_token_plan";
            }
            const auto resetTime = parseRuntimeTimestamp(result.getString(row, 10));
            quota.windowResetAt = resetTime > snapshotTime ? resetTime : 0;
        }
        providerIt->quotas.push_back(std::move(quota));
    }

    if (viewModel.providers.empty()) {
        return std::nullopt;
    }

    viewModel.lastUpdateTime = freshestSnapshotTime > 0
        ? formatTimeUtc(freshestSnapshotTime)
        : formatTimeUtc(now);
    applyRuntimeStateToViewModel(viewModel);
    return viewModel;
}

std::string quotaPlanLabelForResponse(const std::string& providerKey, const QuotaResponse& quota) {
    if (quota.providerSurface == "openai_subscription_codex_local") {
        return quotaPlanLabelForProvider(providerKey, quota.providerSurface);
    }
    if (quota.providerSurface == "minimax_subscription_coding_plan") {
        return quotaPlanLabelForProvider(providerKey, quota.providerSurface);
    }
    if (quota.providerSurface == "minimax_subscription_token_plan") {
        return quotaPlanLabelForProvider(providerKey, quota.providerSurface);
    }

    return quotaPlanLabelForProvider(providerKey, quota.providerSurface);
}

bool shouldExposeQuotaSnapshot(const QuotaResponse& quota) {
    if (quota.providerSurface == "kimi_code_local" ||
        quota.providerSurface == "kimi_local_runtime" ||
        quota.providerSurface == "kimi_local_activity") {
        return false;
    }

    return quota.success &&
        (quota.limit > 0 ||
         quota.used > 0 ||
         quota.remaining > 0 ||
         !quota.providerSurface.empty());
}

bool shouldTreatAsSuppressedLocalKimiQuota(const std::string& providerKey, const QuotaResponse& quota) {
    return providerKey == "kimi" &&
        (quota.providerSurface == "kimi_code_local" ||
         quota.providerSurface == "kimi_local_runtime" ||
         quota.providerSurface == "kimi_local_activity");
}

bool isKnownLocalAvailabilityError(
    const std::string& providerKey,
    const std::string& error) {
    if (error.empty()) {
        return false;
    }

    if (providerKey == "kimi") {
        return error == "Unable to determine the Kimi local root path" ||
            error == "Kimi local root path does not exist" ||
            error == "Kimi sessions directory does not exist" ||
            error == "No approved Kimi runtime snapshots were found" ||
            error == "Kimi quota API credentials are not available";
    }

    if (providerKey == "claude_code") {
        return error == "Could not resolve Claude stats-cache path" ||
            error == "Claude stats-cache path is empty" ||
            error.find("Claude stats-cache file does not exist:") == 0 ||
            error.find("Claude stats-cache file is empty:") == 0;
    }

    return false;
}

std::string quotaConfidenceTierForResponse(const std::string& providerKey, const QuotaResponse& quota) {
    if (providerKey == "claude_code" &&
        (quota.limit <= 0 ||
         quota.providerSurface == "claude_code_local_stats")) {
        return "Unknown";
    }

    return "high";
}

void applyRuntimeStateToViewModel(DashboardViewModel& viewModel) {
    std::vector<ProviderState> states;
    states.reserve(viewModel.providers.size());
    for (const auto& provider : viewModel.providers) {
        if (provider.isEnabled && provider.contributesToRuntimeState) {
            states.push_back(provider.state);
        }
    }

    viewModel.runtimeState = deriveRuntimeState(states);
    viewModel.isOnline = runtimeStateIsOnline(viewModel.runtimeState);
}

ProviderState determineProviderState(const QuotaResponse& quota, std::time_t now) {
    if (quota.unavailable) {
        return ProviderState::Sample;
    }
    if (!quota.success) {
        return ProviderState::Offline;
    }

    if (quota.snapshotTime > 0 &&
        isRuntimeTimestampStale(formatTimeUtc(quota.snapshotTime), now)) {
        return ProviderState::Stale;
    }

    return ProviderState::Live;
}

TaskbarMetrics makeProviderStateTaskbarMetric(const ProviderStatus& provider, std::time_t now) {
    TaskbarMetrics metric;
    metric.providerKey = provider.providerKey;
    metric.providerDisplayName = provider.displayName;
    metric.kind = TaskbarMetricKind::PersistedActivity;
    metric.lastUpdated = now;
    metric.summaryLabel = provider.displayName;
    metric.primaryMetric = providerStateCompactLabel(provider.state);
    metric.secondaryMetric = providerStateLabel(provider.state);
    metric.statusIcon = providerStateIndicator(provider.state);
    return metric;
}

bool isLimitlessLocalActivityQuota(const ProviderStatus& provider, const QuotaInfo& quota) {
    return quota.limit <= 0 &&
        provider.providerKey == "claude_code" &&
        quota.providerSurface == "claude_code_local_stats";
}

bool isSubscriptionQuotaReading(const ProviderStatus& provider, const QuotaInfo& quota) {
    if (quota.limit <= 0) {
        return false;
    }

    const std::string& surface = quota.providerSurface;
    if (surface.empty()) {
        return provider.providerKey == "openai" ||
            provider.providerKey == "minimax" ||
            provider.providerKey == "kimi";
    }

    if (surface == "openai_subscription_codex_local" || surface == "minimax_subscription_coding_plan" ||
        surface == "minimax_subscription_token_plan" || surface == "kimi_subscription_api" ||
        surface == "grok_subscription_api" || surface == "cursor_subscription_api") {
        return true;
    }

    return false;
}

void retainLastKnownDashboardQuota(DashboardViewModel& next, const DashboardViewModel& previous) {
    for (auto& provider : next.providers) {
        if (!provider.isEnabled || provider.state != ProviderState::Offline || !provider.quotas.empty()) {
            continue;
        }

        const auto prior = std::find_if(
            previous.providers.begin(), previous.providers.end(), [&provider](const ProviderStatus& candidate) {
                return candidate.isEnabled && candidate.providerKey == provider.providerKey &&
                       candidate.accountId == provider.accountId;
            });
        if (prior == previous.providers.end()) {
            continue;
        }
        if (prior->state == ProviderState::Sample) {
            continue;
        }

        const auto& previousQuota = prior->lastKnownQuota ? prior->lastKnownQuota->quotas : prior->quotas;
        LastKnownQuotaReading reading;
        reading.lastSyncTime = prior->lastKnownQuota ? prior->lastKnownQuota->lastSyncTime : prior->lastSyncTime;
        if (reading.lastSyncTime.empty()) {
            continue;
        }
        for (const auto& quota : previousQuota) {
            const bool matchingPlan = provider.providerKey != "minimax" ||
                                      (!provider.quotaSource.empty() && quota.providerSurface == provider.quotaSource);
            if (matchingPlan && isSubscriptionQuotaReading(*prior, quota)) {
                reading.quotas.push_back(quota);
            }
        }
        if (!reading.quotas.empty()) {
            provider.lastKnownQuota = std::move(reading);
        }
    }
}

const QuotaInfo* findWeeklyQuotaRow(
    const ProviderStatus& provider,
    const QuotaInfo* primaryQuota) {
    for (const auto& quotaCandidate : provider.quotas) {
        if (&quotaCandidate == primaryQuota) {
            continue;
        }
        if (quotaCandidate.windowType == "weekly") {
            return &quotaCandidate;
        }
    }
    return nullptr;
}

TaskbarMetrics makeRuntimeStateTaskbarMetric(RuntimeState runtimeState, std::time_t now) {
    TaskbarMetrics metric;
    metric.providerKey = "runtime";
    metric.providerDisplayName = "Runtime";
    metric.kind = TaskbarMetricKind::PersistedActivity;
    metric.lastUpdated = now;
    metric.summaryLabel = "Runtime";
    metric.primaryMetric = runtimeStateCompactLabel(runtimeState);
    metric.secondaryMetric = runtimeStateLabel(runtimeState);
    metric.statusIcon = runtimeStateIndicator(runtimeState);
    return metric;
}

std::vector<TaskbarMetrics> buildTaskbarMetricsFromViewModel(
    std::time_t now,
    const DashboardViewModel& viewModel) {
    std::vector<TaskbarMetrics> metrics;
    metrics.reserve(viewModel.providers.size());

    for (const auto& provider : viewModel.providers) {
        if (!provider.isEnabled) {
            continue;
        }

        const std::time_t lastUpdated = parseRuntimeTimestamp(provider.lastSyncTime);
        if (!provider.quotas.empty()) {
            const auto* primaryQuota = &provider.quotas.front();
            for (const auto& quotaCandidate : provider.quotas) {
                if (quotaCandidate.windowType == "interval") {
                    primaryQuota = &quotaCandidate;
                    break;
                }
            }

            const auto* secondaryQuota = static_cast<const QuotaInfo*>(nullptr);
            if (provider.providerKey == "openai") {
                secondaryQuota = findWeeklyQuotaRow(provider, primaryQuota);
            }

            const auto& quota = *primaryQuota;
            TaskbarMetrics metric;
            metric.providerKey = provider.providerKey;
            metric.providerDisplayName = provider.displayName;
            metric.used = quota.used;
            metric.limit = quota.limit;
            metric.usagePercentage = quota.usedPercentage;
            metric.remainingPercentage = quota.remainingPercentage;
            metric.windowResetAt = quota.windowResetAt;
            metric.secondaryWindowType = quota.secondaryWindowType;
            metric.secondaryRemainingPercentage = quota.secondaryRemainingPercentage;
            metric.secondaryWindowResetAt = quota.secondaryWindowResetAt;
            metric.confidenceTier = quota.confidenceTier;
            metric.providerSurface = quota.providerSurface;
            metric.lastUpdated = (lastUpdated > 0) ? lastUpdated : now;
            if (isLimitlessLocalActivityQuota(provider, quota)) {
                metric.kind = TaskbarMetricKind::PersistedActivity;
                metric.summaryLabel = provider.displayName + " local";
                metric.primaryMetric = formatCompactSessionCount(std::max<long long>(0, quota.used));
                metric.secondaryMetric = "Local activity";
                metric.pressureBand = TaskbarPressureBand::Blue;
            }
            if (secondaryQuota != nullptr) {
                metric.secondaryWindowType = secondaryQuota->windowType;
                metric.secondaryRemainingPercentage = secondaryQuota->remainingPercentage;
                metric.secondaryWindowResetAt = secondaryQuota->windowResetAt;
            }
            if (provider.state != ProviderState::Live) {
                metric.statusIcon = providerStateIndicator(provider.state);
            }
            metrics.push_back(metric);
            continue;
        }

        if (provider.state != ProviderState::Live) {
            metrics.push_back(makeProviderStateTaskbarMetric(provider, (lastUpdated > 0) ? lastUpdated : now));
        }
    }

    if (metrics.empty() && viewModel.runtimeState != RuntimeState::Current) {
        metrics.push_back(makeRuntimeStateTaskbarMetric(viewModel.runtimeState, now));
    }

    return metrics;
}

std::string formatTaskbarThroughputRate(double value) {
    if (value < 0.0) {
        value = 0.0;
    }

    std::ostringstream stream;
    if (value >= 10.0) {
        const std::string digits = std::to_string(static_cast<long long>(value + 0.5));
        const size_t firstGroup = digits.size() % 3;
        for (size_t index = 0; index < digits.size(); ++index) {
            if (index > 0 && (index == firstGroup || (index > firstGroup && ((index - firstGroup) % 3 == 0)))) {
                stream << ',';
            }
            stream << digits[index];
        }
    } else {
        stream << std::fixed << std::setprecision(1) << value;
    }
    return stream.str();
}

std::string buildOpenAICodexThroughputTooltipDetail(const AnalysisCodexThroughput& throughput) {
    std::ostringstream stream;
    stream << "Codex throughput 5m: "
           << formatTaskbarThroughputRate(throughput.tokensPerMinute5m)
           << " tok/min, "
           << formatTaskbarThroughputRate(throughput.promptsPerMinute5m)
           << " prm/min";
    stream << "\nCodex throughput 30m: "
           << formatTaskbarThroughputRate(throughput.tokensPerMinute30m)
           << " tok/min, "
           << formatTaskbarThroughputRate(throughput.promptsPerMinute30m)
           << " prm/min";
    return stream.str();
}

std::vector<TaskbarMetrics> buildSubscriptionQuotaTaskbarMetricsFromViewModel(
    std::time_t now,
    const DashboardViewModel& viewModel) {
    std::vector<TaskbarMetrics> metrics;
    metrics.reserve(viewModel.providers.size());

    for (const auto& provider : viewModel.providers) {
        if (!provider.isEnabled || provider.quotas.empty()) {
            continue;
        }

        const auto* primaryQuota = static_cast<const QuotaInfo*>(nullptr);
        for (const auto& quotaCandidate : provider.quotas) {
            if (!isSubscriptionQuotaReading(provider, quotaCandidate)) {
                continue;
            }
            if (primaryQuota == nullptr || quotaCandidate.windowType == "interval") {
                primaryQuota = &quotaCandidate;
                if (quotaCandidate.windowType == "interval") {
                    break;
                }
            }
        }
        if (primaryQuota == nullptr) {
            continue;
        }

        const std::time_t lastUpdated = parseRuntimeTimestamp(provider.lastSyncTime);
        const auto* secondaryQuota = static_cast<const QuotaInfo*>(nullptr);
        if (provider.providerKey == "openai") {
            secondaryQuota = findWeeklyQuotaRow(provider, primaryQuota);
            if (secondaryQuota != nullptr && !isSubscriptionQuotaReading(provider, *secondaryQuota)) {
                secondaryQuota = nullptr;
            }
        }

        const auto& quota = *primaryQuota;
        TaskbarMetrics metric;
        metric.providerKey = provider.providerKey;
        metric.providerDisplayName = provider.displayName;
        metric.used = quota.used;
        metric.limit = quota.limit;
        metric.usagePercentage = quota.usedPercentage;
        metric.remainingPercentage = quota.remainingPercentage;
        metric.windowResetAt = quota.windowResetAt;
        metric.secondaryWindowType = quota.secondaryWindowType;
        metric.secondaryRemainingPercentage = quota.secondaryRemainingPercentage;
        metric.secondaryWindowResetAt = quota.secondaryWindowResetAt;
        metric.confidenceTier = quota.confidenceTier;
        metric.providerSurface = quota.providerSurface;
        metric.lastUpdated = (lastUpdated > 0) ? lastUpdated : now;
        if (provider.providerKey == "openai" && viewModel.analysis.codexThroughput.hasData) {
            metric.tooltipDetail = buildOpenAICodexThroughputTooltipDetail(viewModel.analysis.codexThroughput);
        }
        if (secondaryQuota != nullptr) {
            metric.secondaryWindowType = secondaryQuota->windowType;
            metric.secondaryRemainingPercentage = secondaryQuota->remainingPercentage;
            metric.secondaryWindowResetAt = secondaryQuota->windowResetAt;
        }
        if (provider.state != ProviderState::Live) {
            metric.statusIcon = providerStateIndicator(provider.state);
        }
        metrics.push_back(std::move(metric));
    }

    return metrics;
}

void overlayProviderStatesOnTaskbarMetrics(
    const DashboardViewModel& viewModel,
    std::vector<TaskbarMetrics>& metrics,
    std::time_t now) {
    for (auto& metric : metrics) {
        auto providerIt = std::find_if(
            viewModel.providers.begin(),
            viewModel.providers.end(),
            [&metric](const ProviderStatus& provider) {
                return provider.providerKey == metric.providerKey;
            });
        if (providerIt != viewModel.providers.end() &&
            providerIt->state != ProviderState::Live &&
            providerIt->state != ProviderState::Sample) {
            metric.statusIcon = providerStateIndicator(providerIt->state);
        }
    }

    for (const auto& provider : viewModel.providers) {
        if (!provider.isEnabled ||
            provider.state == ProviderState::Live ||
            provider.state == ProviderState::Sample) {
            continue;
        }

        const bool alreadyRepresented = std::any_of(
            metrics.begin(),
            metrics.end(),
            [&provider](const TaskbarMetrics& metric) {
                return metric.providerKey == provider.providerKey;
            });
        if (!alreadyRepresented) {
            metrics.push_back(makeProviderStateTaskbarMetric(provider, now));
        }
    }
}

TrayStatus trayStatusForRuntimeState(RuntimeState runtimeState) {
    switch (runtimeState) {
        case RuntimeState::Current:
            return TrayStatus::Normal;
        case RuntimeState::Stale:
            return TrayStatus::Warning;
        case RuntimeState::CatchingUp:
            return TrayStatus::Syncing;
        case RuntimeState::Offline:
            return TrayStatus::Offline;
        default:
            return TrayStatus::Error;
    }
}

std::string buildTrayTooltip(const DashboardViewModel& viewModel) {
    std::ostringstream tooltip;
    tooltip << "JiStats - " << runtimeStateLabel(viewModel.runtimeState);
    if (!viewModel.providers.empty()) {
        tooltip << " (" << viewModel.providers.size() << " providers)";
    }
    return tooltip.str();
}

std::string summarizeRecoveryOutcomeLabel(
    const std::string& outcome,
    const std::string& comparisonResult,
    long long localEventCount,
    long long importedTotalTokens) {
    if (outcome == "imported") {
        if (comparisonResult == "matched_local_evidence") {
            return "Imported backfill matched local evidence";
        }
        if (comparisonResult == "mismatch") {
            return "Imported backfill differs from local evidence";
        }
        return "Imported backfill with no local evidence";
    }
    if (outcome == "no_data") {
        if (comparisonResult == "mismatch" || localEventCount > 0) {
            return "Provider returned no history but local evidence exists";
        }
        return "Provider returned no recovery history";
    }
    if (outcome == "fetch_failed") {
        return "Provider backfill fetch failed";
    }
    if (outcome == "persist_failed") {
        return importedTotalTokens > 0
            ? "Backfill persistence failed after provider import"
            : "Backfill persistence failed";
    }
    return "No recent recovery activity";
}

Notice buildOfflineNotice(const DashboardViewModel& viewModel, std::time_t now) {
    Notice notice;
    notice.type = NoticeType::Offline;
    notice.priority = NotificationService::getDefaultPriority(NoticeType::Offline);
    notice.title = NotificationService::getDefaultTitle(NoticeType::Offline);
    notice.message =
        "Provider connectivity is unavailable. JiStats will keep the last known state and retry when connectivity returns.";
    if (viewModel.providers.size() == 1) {
        notice.providerName = viewModel.providers.front().displayName;
    }
    notice.hasRetryAction = true;
    notice.createdAt = std::chrono::system_clock::from_time_t(now);
    return notice;
}

std::optional<int> findProviderIdByKey(StorageService& storage, const std::string& providerKey) {
    QueryResult result = storage.execute(
        "SELECT id FROM providers WHERE provider_key = ? LIMIT 1;",
        {providerKey});
    if (!result.success() || result.rowCount() == 0) {
        return std::nullopt;
    }

    return result.getInt(0, 0);
}

std::string runtimeTrackingAccountKey(const std::string& providerKey) {
    return providerKey + ".runtime_status";
}

std::string buildRecoveryMarker(std::time_t from) {
    return "catchup:" + std::to_string(std::max<std::time_t>(from, 0));
}

std::time_t parseRecoveryMarkerStart(const std::string& marker) {
    if (marker.empty()) {
        return 0;
    }

    const std::size_t separator = marker.find_last_of(':');
    if (separator == std::string::npos || separator + 1 >= marker.size()) {
        return 0;
    }

    try {
        return static_cast<std::time_t>(std::stoll(marker.substr(separator + 1)));
    } catch (...) {
        return 0;
    }
}

std::optional<int> ensureRuntimeTrackingAccountId(
    StorageService& storage,
    const std::string& providerKey,
    const std::string& displayName) {
    const auto providerId = findProviderIdByKey(storage, providerKey);
    if (!providerId.has_value()) {
        return std::nullopt;
    }

    const std::string accountKey = runtimeTrackingAccountKey(providerKey);
    QueryResult existing = storage.execute(
        "SELECT id FROM accounts WHERE provider_id = ? AND account_key = ? LIMIT 1;",
        {std::to_string(*providerId), accountKey});
    if (existing.success() && existing.rowCount() == 1) {
        return existing.getInt(0, 0);
    }

    QueryResult insert = storage.execute(
        "INSERT INTO accounts (provider_id, account_key, display_name, scope_type, scope_identifier) "
        "VALUES (?, ?, ?, ?, ?);",
        {
            std::to_string(*providerId),
            accountKey,
            displayName + " Runtime Status",
            "runtime",
            accountKey
        });
    if (!insert.success()) {
        return std::nullopt;
    }

    return static_cast<int>(insert.lastInsertRowId());
}

std::string quotaSnapshotPlanKey(
    const std::string& providerKey,
    const std::string& providerSurface,
    const std::string& windowType) {
    const std::string base = !providerSurface.empty() ? providerSurface : providerKey;
    if (windowType.empty()) {
        return base;
    }
    return base + ":" + windowType;
}

std::optional<int> ensureQuotaSnapshotPlanId(
    StorageService& storage,
    int accountId,
    const std::string& providerKey,
    const std::string& providerSurface,
    const std::string& windowType,
    const std::string& displayName,
    long long limitValue) {
    const std::string planKey = quotaSnapshotPlanKey(providerKey, providerSurface, windowType);
    QueryResult existing = storage.execute(
        "SELECT id FROM plans WHERE account_id = ? AND plan_key = ? LIMIT 1;",
        {std::to_string(accountId), planKey});
    if (existing.success() && existing.rowCount() == 1) {
        return existing.getInt(0, 0);
    }

    QueryResult insert = storage.execute(
        "INSERT INTO plans (account_id, plan_key, plan_type, display_name, quota_window, limit_value) "
        "VALUES (?, ?, ?, ?, ?, ?);",
        {
            std::to_string(accountId),
            planKey,
            "quota_snapshot",
            displayName.empty() ? (!providerSurface.empty() ? providerSurface : providerKey) : displayName,
            windowType.empty() ? "rolling" : windowType,
            std::to_string(std::max<long long>(0, limitValue))
        });
    if (!insert.success()) {
        return std::nullopt;
    }

    return static_cast<int>(insert.lastInsertRowId());
}

bool persistQuotaSnapshotRow(StorageService& storage, int providerId, int accountId, std::optional<int> planId,
                             std::time_t snapshotTime, std::time_t windowStart, std::time_t windowEnd, long long used,
                             long long limit, long long remaining, const std::string& confidenceTier,
                             const std::string& basisSummary, bool percentageReading) {
    if (percentageReading && !planId.has_value()) {
        return false;
    }
    if (snapshotTime <= 0) {
        snapshotTime = std::time(nullptr);
    }
    if (!percentageReading) {
        if (windowEnd <= 0) {
            windowEnd = snapshotTime;
        }
        if (windowStart <= 0) {
            windowStart = windowEnd;
        }
    }

    std::string sql =
        "INSERT INTO quota_snapshots ("
        "provider_id, account_id, plan_id, snapshot_time_utc, window_start_utc, window_end_utc, "
        "used_value, limit_value, confidence_tier, basis_summary, quota_dimension, remaining_value, is_reconciled"
        ") ";
    sql += percentageReading ? "SELECT ?, ?, " : "VALUES (?, ?, ";
    sql += planId.has_value() ? "?" : "NULL";
    sql += ", ?, ?, ?, ?, ";
    sql += limit > 0 ? "?" : "NULL";
    sql += ", ?, ?, ?, ?, ?";
    if (percentageReading) {
        // Refreshes may reuse the same provider response until its polling budget expires.
        sql += " WHERE NOT EXISTS (SELECT 1 FROM quota_snapshots WHERE provider_id = ? AND account_id = ? "
               "AND plan_id = ? AND snapshot_time_utc = ?);";
    } else {
        sql += ");";
    }

    std::vector<std::string> params = {
        std::to_string(providerId),
        std::to_string(accountId)
    };
    if (planId.has_value()) {
        params.push_back(std::to_string(*planId));
    }
    params.push_back(formatTimeUtcIso(snapshotTime));
    params.push_back(windowStart > 0 ? formatTimeUtcIso(windowStart) : std::string{});
    params.push_back(windowEnd > 0 ? formatTimeUtcIso(windowEnd) : std::string{});
    params.push_back(std::to_string(std::max<long long>(0, used)));
    if (limit > 0) {
        params.push_back(std::to_string(limit));
    }
    params.push_back(confidenceTier.empty() ? "Unknown" : confidenceTier);
    params.push_back(basisSummary);
    params.push_back(percentageReading ? "percent" : "requests");
    params.push_back(std::to_string(std::max<long long>(0, remaining)));
    params.push_back("0");
    if (percentageReading) {
        params.push_back(std::to_string(providerId));
        params.push_back(std::to_string(accountId));
        params.push_back(std::to_string(*planId));
        params.push_back(formatTimeUtcIso(snapshotTime));
    }

    QueryResult insert = storage.execute(sql, params);
    return insert.success();
}

std::time_t queryLatestLocalObservationAt(StorageService& storage) {
    QueryResult result = storage.execute(
        "SELECT MAX(started_at_utc) FROM activity_events;");
    if (!result.success() || result.rowCount() == 0) {
        return 0;
    }

    return parseRuntimeTimestamp(result.getString(0, 0));
}

bool providerHasBackfilledUsage(
    StorageService& storage,
    int providerId,
    std::time_t from,
    std::time_t to) {
    QueryResult result = storage.execute(
        "SELECT COUNT(*) FROM usage_events "
        "WHERE provider_id = ? AND event_time_utc >= ? AND event_time_utc <= ? AND is_backfilled = 1 "
        "AND source_type != 'codex_local_rollout';",
        {
            std::to_string(providerId),
            formatTimeUtcIso(from),
            formatTimeUtcIso(to)
        });
    if (!result.success() || result.rowCount() == 0) {
        return false;
    }

    return result.getInt64(0, 0) > 0;
}

bool rebuildUsageRollupsForRange(StorageService& storage, std::time_t from, std::time_t to) {
    QueryResult scopes = storage.execute(
        "SELECT DISTINCT provider_id, account_id FROM usage_events "
        "WHERE event_time_utc >= ? AND event_time_utc <= ? "
        "AND source_type != 'codex_local_rollout';",
        {formatTimeUtcIso(from), formatTimeUtcIso(to)});
    if (!scopes.success()) {
        return false;
    }

    bool success = true;
    for (size_t row = 0; row < scopes.rowCount(); ++row) {
        const int providerId = scopes.getInt(row, 0);
        const int accountId = scopes.getInt(row, 1);
        success = storage.rebuildUsageRollups(
            UsageRollupResolution::Hourly,
            providerId,
            accountId,
            from,
            to) && success;
        success = storage.rebuildUsageRollups(
            UsageRollupResolution::Daily,
            providerId,
            accountId,
            from,
            to) && success;
    }

    return success;
}

std::string minimaxPlanDisplayName(const std::string& planKey) {
    return planKey == "token_plan" ? "MiniMax Token Plan" : "MiniMax Coding Plan";
}

std::string minimaxAccountKey(const std::string& planKey) {
    return planKey + ".default";
}

std::string minimaxDefaultAccountDisplayName(const std::string& planKey) {
    return minimaxPlanDisplayName(planKey) + " Subscription";
}

std::vector<std::string> minimaxSupportedPlanKeys() {
    return {"coding_plan", "token_plan"};
}

std::string normalizeMiniMaxValidationError(const std::string& error) {
    if (error.rfind("Credential not found:", 0) == 0) {
        return "Stored MiniMax credential is missing from Windows Credential Manager";
    }
    return error;
}

std::string lastValidatedLabel(std::time_t timestamp) {
    return timestamp > 0 ? formatTimeUtc(timestamp) : "";
}

struct MiniMaxStoredAccountRecord {
    int providerId = 0;
    int accountId = 0;
    std::string accountKey;
    std::string accountDisplayName;
    std::string scopeIdentifier;
    bool exists = false;
};

MiniMaxStoredAccountRecord loadMiniMaxStoredAccount(
    StorageService& storage,
    int providerId,
    const std::string& planKey) {
    MiniMaxStoredAccountRecord record;
    record.providerId = providerId;
    record.accountKey = minimaxAccountKey(planKey);
    record.accountDisplayName = minimaxDefaultAccountDisplayName(planKey);
    record.scopeIdentifier = record.accountKey;

    QueryResult result = storage.execute(
        "SELECT id, display_name, scope_identifier "
        "FROM accounts WHERE provider_id = ? AND account_key = ? LIMIT 1;",
        {std::to_string(providerId), record.accountKey});
    if (!result.success() || result.rowCount() == 0) {
        return record;
    }

    record.exists = true;
    record.accountId = result.getInt(0, 0);
    record.accountDisplayName = result.getString(0, 1);
    record.scopeIdentifier = result.getString(0, 2);
    if (record.scopeIdentifier.empty()) {
        record.scopeIdentifier = record.accountKey;
    }
    return record;
}

std::string buildMirrorPolicyVersionLabel(
    StorageService& storage,
    const DashboardViewModel& viewModel) {
    if (!storage.isOpen()) {
        return "";
    }

    std::set<int> ruleVersions;
    for (const auto& provider : viewModel.providers) {
        const auto providerId = findProviderIdByKey(storage, provider.providerKey);
        if (!providerId.has_value()) {
            continue;
        }

        for (const auto& rule : storage.getMirrorRules(*providerId)) {
            ruleVersions.insert(rule.ruleVersion);
        }
    }

    if (ruleVersions.empty()) {
        return "";
    }

    if (ruleVersions.size() == 1) {
        return "v" + std::to_string(*ruleVersions.begin());
    }

    std::ostringstream stream;
    stream << "mixed (";
    bool first = true;
    for (const int version : ruleVersions) {
        if (!first) {
            stream << ", ";
        }
        first = false;
        stream << "v" << version;
    }
    stream << ")";
    return stream.str();
}

// ============================================================================
// SAMPLE DATA FUNCTIONS - Deprecated, only available under JISTATS_ENABLE_TEST_HOOKS
// These functions exist for backward compatibility with existing tests and fixtures.
// New code should use buildDashboardViewModelFromProviders() instead.
// ============================================================================

#ifdef JISTATS_ENABLE_TEST_HOOKS

// [[deprecated("Use buildDashboardViewModelFromProviders() with ProviderManager")]]
DashboardViewModel buildSampleDashboardViewModel(std::time_t now) {
    DashboardViewModel viewModel;
    viewModel.appVersion = kJiStatsAppVersion;
    viewModel.lastUpdateTime = formatTimeUtc(now);
    viewModel.preferences.taskbarOverlayEnabled = false;
    viewModel.preferences.startupEnabled = false;
    viewModel.preferences.startupStartInBackground = true;
    viewModel.providers = {
        makeProvider(
            "openai",
            "OpenAI",
            formatTimeUtc(now - 90),
            {
                makeQuota("Codex Subscription", "monthly", 1305, 1500, "high"),
                makeQuota("Imported Org Usage", "daily", 248000, 500000, "medium")
            },
            ProviderState::Sample),
        makeProvider(
            "minimax",
            "MiniMax",
            formatTimeUtc(now - 45),
            {
                makeQuota("Coding Plan", "daily", 42000, 100000, "high"),
                makeQuota("Token Plan", "monthly", 310000, 500000, "high")
            },
            ProviderState::Sample)
    };
    applyRuntimeStateToViewModel(viewModel);
    return viewModel;
}

// [[deprecated("Use buildTaskbarMetricsFromProviders() with ProviderManager")]]
std::vector<TaskbarMetrics> buildSampleTaskbarMetrics(std::time_t now) {
    return {
        makeTaskbarMetric("openai", "OpenAI", 1305, 1500, "high", now),
        makeTaskbarMetric("minimax", "MiniMax", 42000, 100000, "high", now)
    };
}

// TEMPORARY SCAFFOLDING: The functions below (makeSampleEvent and
// buildDashboardViewModelFromMirror) exist to validate the MirrorEngine-to-
// DashboardPresenter data flow using hardcoded sample data. They will be
// replaced when real provider adapters land.

// [[deprecated("Sample data function - use real provider data instead")]]
UsageEvent makeSampleEvent(
    const std::string&,
    const std::string& modelKey,
    std::time_t eventTime,
    long long totalTokens,
    long long costMinorUnits) {
    UsageEvent event;
    event.eventTime = eventTime;
    event.modelKey = modelKey;
    event.requestType = "chat completions";
    event.inputTokens = totalTokens * 3 / 4;
    event.outputTokens = totalTokens / 4;
    event.totalTokens = totalTokens;
    event.costMinorUnits = costMinorUnits;
    event.sourceType = "api";
    return event;
}

// [[deprecated("Use buildDashboardViewModelFromProviders() with ProviderManager")]]
DashboardViewModel buildDashboardViewModelFromMirror(std::time_t now, MirrorEngine& mirrorEngine) {
    DashboardViewModel viewModel;
    viewModel.appVersion = kJiStatsAppVersion;
    viewModel.lastUpdateTime = formatTimeUtc(now);
    viewModel.preferences.taskbarOverlayEnabled = false;
    viewModel.preferences.startupEnabled = false;
    viewModel.preferences.startupStartInBackground = true;

    // Create sample events for OpenAI - these would come from real API in production
    std::vector<UsageEvent> openaiEvents;
    openaiEvents.push_back(makeSampleEvent("openai", "gpt-4", now - 3600, 50000, 150));
    openaiEvents.push_back(makeSampleEvent("openai", "gpt-4", now - 7200, 75000, 225));
    openaiEvents.push_back(makeSampleEvent("openai", "gpt-4", now - 10800, 30000, 90));

    // Compute OpenAI daily mirror
    MirrorInput openaiInput;
    openaiInput.providerKey = "openai";
    openaiInput.localEvents = openaiEvents;
    openaiInput.windowStart = now - 86400; // last 24 hours
    openaiInput.windowEnd = now;
    MirrorOutput openaiOutput = mirrorEngine.computeMirror(openaiInput);

    // Build OpenAI provider with computed mirror data
    ProviderStatus openaiProvider;
    openaiProvider.providerKey = "openai";
    openaiProvider.displayName = "OpenAI";
    openaiProvider.isEnabled = true;
    openaiProvider.state = ProviderState::Sample;
    openaiProvider.lastSyncTime = formatTimeUtc(now - 90);

    QuotaInfo openaiQuota = makeQuota(
        "Imported Org Usage",
        "daily",
        openaiOutput.success ? openaiOutput.snapshot.used : 155000,
        500000,
        MirrorEngine::tierToString(openaiOutput.tier));
    openaiProvider.quotas.push_back(openaiQuota);

    // Add Codex subscription quota
    QuotaInfo codexQuota = makeQuota(
        "Codex Subscription",
        "monthly",
        1305,
        1500,
        "high");
    openaiProvider.quotas.push_back(codexQuota);

    viewModel.providers.push_back(openaiProvider);

    // Create sample events for MiniMax
    std::vector<UsageEvent> minimaxEvents;
    minimaxEvents.push_back(makeSampleEvent("minimax", "abab6.5s-chat", now - 1800, 20000, 60));
    minimaxEvents.push_back(makeSampleEvent("minimax", "abab6.5s-chat", now - 5400, 15000, 45));

    // Compute MiniMax daily mirror
    MirrorInput minimaxInput;
    minimaxInput.providerKey = "minimax";
    minimaxInput.localEvents = minimaxEvents;
    minimaxInput.windowStart = now - 86400;
    minimaxInput.windowEnd = now;
    MirrorOutput minimaxOutput = mirrorEngine.computeMirror(minimaxInput);

    // Build MiniMax provider
    ProviderStatus minimaxProvider;
    minimaxProvider.providerKey = "minimax";
    minimaxProvider.displayName = "MiniMax";
    minimaxProvider.isEnabled = true;
    minimaxProvider.state = ProviderState::Sample;
    minimaxProvider.lastSyncTime = formatTimeUtc(now - 45);

    QuotaInfo minimaxQuota = makeQuota(
        "Coding Plan",
        "daily",
        minimaxOutput.success ? minimaxOutput.snapshot.used : 35000,
        100000,
        MirrorEngine::tierToString(minimaxOutput.tier));
    minimaxProvider.quotas.push_back(minimaxQuota);

    QuotaInfo tokenQuota = makeQuota(
        "Token Plan",
        "monthly",
        310000,
        500000,
        "high");
    minimaxProvider.quotas.push_back(tokenQuota);

    viewModel.providers.push_back(minimaxProvider);
    applyRuntimeStateToViewModel(viewModel);

    return viewModel;
}

#endif // JISTATS_ENABLE_TEST_HOOKS

// Provider-fed data path: builds DashboardViewModel from ProviderManager data
DashboardViewModel buildDashboardViewModelFromResolvedProviderData(
    std::time_t now,
    const std::vector<std::shared_ptr<IProviderAdapter>>& providers,
    const std::vector<QuotaResponse>& quotaResponses) {
    DashboardViewModel viewModel;
    viewModel.appVersion = kJiStatsAppVersion;
    viewModel.lastUpdateTime = formatTimeUtc(now);
    viewModel.preferences.taskbarOverlayEnabled = false;
    viewModel.preferences.startupEnabled = false;
    viewModel.preferences.startupStartInBackground = true;

    // Match responses to providers by index (fetchAllQuota returns in same order as getEnabledProviders)
    size_t quotaIndex = 0;
    for (const auto& provider : providers) {
        ProviderStatus status;
        status.providerKey = provider->getProviderKey();
        status.displayName = provider->getDisplayName();
        status.accountId = runtimeTrackingAccountKey(status.providerKey);
        status.isEnabled = true;
        status.state = ProviderState::Offline;
        
        // Add quota info if we have a matching response
        if (quotaIndex < quotaResponses.size()) {
            const auto& quota = quotaResponses[quotaIndex];
            status.quotaSource = quota.providerSurface;
            const bool missingLocalPrerequisite = isKnownLocalAvailabilityError(status.providerKey, quota.error);
            if (missingLocalPrerequisite || shouldTreatAsSuppressedLocalKimiQuota(status.providerKey, quota)) {
                if (status.providerKey == "kimi") {
                    status.state = ProviderState::Sample;
                    // The Dashboard row is useful even when no quota result can inform overall runtime status.
                    status.contributesToRuntimeState = !missingLocalPrerequisite;
                    viewModel.providers.push_back(std::move(status));
                }
                ++quotaIndex;
                continue;
            }

            status.state = determineProviderState(quota, now);
            if (quota.unavailable) {
                status.detectionMessage = quota.error;
            }
            if (quota.success && quota.snapshotTime > 0) {
                status.lastSyncTime = formatTimeUtc(quota.snapshotTime);
            }
            if (shouldExposeQuotaSnapshot(quota)) {
                const NormalizedQuotaValues normalizedValues = normalizeQuotaValues(quota);
                QuotaInfo quotaInfo = makeQuota(
                    quotaPlanLabelForResponse(status.providerKey, quota),
                    quota.windowType,
                    normalizedValues.used,
                    normalizedValues.limit,
                    quotaConfidenceTierForResponse(status.providerKey, quota),
                    normalizedValues.remaining);
                quotaInfo.windowResetAt = quota.windowEnd;
                if (quota.secondaryLimit > 0) {
                    quotaInfo.secondaryWindowType = quota.secondaryWindowType;
                    quotaInfo.secondaryRemainingPercentage =
                        computeRemainingPercentage(quota.secondaryRemaining, quota.secondaryLimit);
                    quotaInfo.secondaryWindowResetAt = quota.secondaryWindowEnd;
                }
                quotaInfo.providerSurface = quota.providerSurface;
                status.quotas.push_back(quotaInfo);

                if ((status.providerKey == "kimi" && quota.secondaryWindowType == "weekly") ||
                    (status.providerKey == "cursor" && quota.secondaryLimit > 0)) {
                    QuotaInfo secondaryQuota = makeQuota(
                        quotaPlanLabelForResponse(status.providerKey, quota), quota.secondaryWindowType,
                        std::max<long long>(0, quota.secondaryUsed), std::max<long long>(0, quota.secondaryLimit),
                        quotaConfidenceTierForResponse(status.providerKey, quota),
                        std::max<long long>(0, quota.secondaryRemaining));
                    secondaryQuota.windowResetAt = quota.secondaryWindowEnd;
                    secondaryQuota.providerSurface = quota.providerSurface;
                    status.quotas.push_back(std::move(secondaryQuota));
                }
            }
            ++quotaIndex;
        }
        
        viewModel.providers.push_back(status);
    }

    applyRuntimeStateToViewModel(viewModel);

    return viewModel;
}

DashboardViewModel buildDashboardViewModelFromProviders(
    std::time_t now,
    ProviderManager& providerManager) {
    const auto providers = providerManager.getEnabledProviders();
    const auto quotaResponses = providerManager.fetchAllQuota();
    return buildDashboardViewModelFromResolvedProviderData(now, providers, quotaResponses);
}

namespace preference_keys {
const char* const TASKBAR_OVERLAY_ENABLED = "ui.taskbar_overlay_enabled";
const char* const OPENAI_SUBSCRIPTION_TRACKING_ENABLED = "runtime.openai_subscription_tracking_enabled";
const char* const CODEX_MODEL_ATTRIBUTION_GENERATION = "runtime.codex_model_attribution_generation";
const char* const MINIMAX_SUBSCRIPTION_TRACKING_ENABLED = "runtime.minimax_subscription_tracking_enabled";
const char* const CLAUDE_CODE_ENABLED = "runtime.claude_code_enabled";
const char* const KIMI_CODE_ENABLED = "runtime.kimi_code_enabled";
const char* const GROK_TRACKING_ENABLED = "runtime.grok_subscription_tracking_enabled";
const char* const CURSOR_TRACKING_ENABLED = "runtime.cursor_subscription_tracking_enabled";
const char* const STARTUP_ENABLED = "runtime.startup_enabled";
const char* const STARTUP_BACKGROUND = "runtime.startup_background";
const char* const LAST_SELECTED_PROVIDER_ID = "ui.last_selected_provider_id";
const char* const LAST_SELECTED_ACCOUNT_ID = "ui.last_selected_account_id";
const char* const ANALYSIS_SELECTED_PROVIDER_ID = "ui.analysis_selected_provider_id";
const char* const ANALYSIS_SELECTED_ACCOUNT_ID = "ui.analysis_selected_account_id";
const char* const ANALYSIS_DATE_RANGE_PRESET = "ui.analysis_date_range_preset";
const char* const ANALYSIS_CUSTOM_RANGE_START_UTC = "ui.analysis_custom_range_start_utc";
const char* const ANALYSIS_CUSTOM_RANGE_END_UTC = "ui.analysis_custom_range_end_utc";
const char* const SCHEDULED_EXPORT_ENABLED = "reporting.scheduled_export_enabled";
const char* const SCHEDULED_EXPORT_INTERVAL_MINUTES = "reporting.scheduled_export_interval_minutes";
const char* const SCHEDULED_EXPORT_PATH = "reporting.scheduled_export_path";
const char* const SCHEDULED_EXPORT_LAST_RUN_UTC = "reporting.scheduled_export_last_run_utc";
const char* const RETENTION_LAST_RUN_UTC = "retention.last_run_utc";
} // namespace preference_keys

} // namespace

#ifdef JISTATS_ENABLE_TEST_HOOKS
std::chrono::milliseconds dormantRefreshWatcherIntervalForTesting() {
    return dormantRefreshWatcherInterval();
}

std::chrono::milliseconds adaptiveWatcherIntervalOrDormantForTesting(
    bool watcherActive,
    int baseIntervalMs,
    int maxIntervalMs,
    size_t unchangedPasses) {
    return adaptiveWatcherIntervalOrDormant(
        watcherActive,
        std::chrono::milliseconds(std::max(1, baseIntervalMs)),
        std::chrono::milliseconds(std::max(1, maxIntervalMs)),
        unchangedPasses);
}
#endif

class AppController::Impl {
public:
    struct QuotaRefresh {
        std::uint64_t generation;
        std::set<std::string> fetchedProviderKeys;
        // Subset of fetchedProviderKeys whose result came from a direct provider API.
        std::set<std::string> providerApiKeys;
        bool deferNewSubscriptions = false;
    };

    struct StagedConnectivity {
        PersistedConnectivityState connectivity;
        // The provider returned a non-offline result fetched in this refresh.
        bool fetchedRecovery = false;
    };

    struct ProviderConnectivityUpdates {
        std::vector<StagedConnectivity> rows;
        // Includes providers whose connectivity cannot be stored.
        bool anyOfflineProvider = false;
        bool providerApiConfirmedRecovery = false;
    };

    bool initialized = false;
    std::atomic<const char*> state{"constructed"};
    DashboardPresenter dashboardPresenter;
    AnalysisPresenter analysisPresenter;
    TaskbarPresenter taskbarPresenter;
    TrayController trayController;
    NotificationService notificationService;
    StorageService storageService;
    AnalysisQueryEngine analysisQueryEngine;
    SettingsService settingsService;
    LocalObservationService localObservationService;
    ObservationPersistenceBridge observationPersistenceBridge;
    MirrorEngine mirrorEngine;
    ProviderManager providerManager;
    StartupManager startupManager;
    ReportingService reportingService;
    RuntimeState lastRuntimeState = RuntimeState::Unknown;
    PersistedMetricSnapshot latestPersistedMetricSnapshot;
    std::time_t latestPersistedMetricSnapshotAt = 0;
    PersistedMetricSnapshotMode latestPersistedMetricSnapshotMode = PersistedMetricSnapshotMode::Full;
    bool persistedMetricSnapshotDirty = true;
    struct CodexAttributionChange {
        bool enabled;
        std::uint64_t revocations;
        std::uint64_t request = 0;
    };
    std::mutex codexModelAttributionMutex;
    // Keep ordinary preferences and attribution off the connection shared by runtime and retention work.
    StorageService preferencesStorage;
    SettingsService preferenceWriter{preferencesStorage};
    StorageService codexAttributionStorage;
    SettingsService codexAttributionSettings{codexAttributionStorage};
    std::atomic<bool> codexModelAttributionEnabled{false};
    // Off requests block scans immediately and survive coalescing with a later On.
    std::atomic<std::uint64_t> requestedCodexRevocations{0};
    std::atomic<std::uint64_t> savedCodexRevocations{0};
    std::atomic<std::uint64_t> completedCodexAttributionRequest{0};
    std::atomic<CodexAttributionSaveError> codexAttributionSaveError{CodexAttributionSaveError::None};
    int lastCodexLocalSqliteGeneration = 0;
    std::filesystem::path lastCodexLocalSqlitePath;
    std::string lastCodexLocalSqliteFingerprint;
    std::string databasePath = "jistats.db";
    std::shared_ptr<MiniMaxAdapter> miniMaxCredentialAdapter;
    std::shared_ptr<WindowsCredentialStore> miniMaxCredentialStore;
    std::map<std::string, MiniMaxSubscriptionCredentialProfile> miniMaxCredentialProfiles;
    mutable std::mutex miniMaxValidationMutex;
    std::mutex runtimeRefreshMutex;
    // Serializes connectivity writes and notice publication independently of provider I/O.
    std::mutex offlineNoticeMutex;
    std::thread startupRefreshThread;
    std::thread interactionWorkThread;
    std::thread taskbarRefreshThread;
    std::thread openAICodexRefreshThread;
    std::thread localProviderRefreshThread;
    std::thread miniMaxRefreshThread;
    std::thread scheduledExportThread;
    std::thread retentionThread;
    std::mutex refreshWatcherMutex;
    mutable std::mutex interactionWorkMutex;
    mutable std::mutex persistedMetricMutex;
    struct ExportSchedule {
        bool enabled = false;
        int intervalMinutes = 1440;
        std::string path;
    };
    mutable std::mutex scheduledExportMutex;
    ExportSchedule appliedExportSchedule;
    std::condition_variable refreshWatcherCv;
    std::atomic<unsigned> subscriptionWatchMask{0};
    std::condition_variable interactionWorkCv;
    std::optional<DashboardPreferences> pendingInteractionPreferences;
    std::optional<DashboardPreferences> activeInteractionPreferences;
    std::optional<DashboardPreferences>
        failedInteractionPreferences; // Effective runtime preferences while a save failed.
    std::atomic<std::uint64_t> completedPreferencesRequest{0};
    std::atomic<std::uint64_t> failedPreferencesRequest{0};
    std::uint64_t completedProviderPreferencesRequest = 0; // Protected by runtimeRefreshMutex.
    std::optional<AnalysisScopeState> pendingAnalysisScope;
    std::optional<CodexAttributionChange> pendingCodexModelAttribution;
    bool pendingProviderDiscoveryRefresh = false;
    bool pendingPersistedActivityRefresh = false;
    std::vector<std::thread> miniMaxValidationThreads;
    std::chrono::milliseconds openAICodexRefreshPollInterval{
        std::chrono::duration_cast<std::chrono::milliseconds>(kOpenAICodexRefreshPollInterval)};
    std::chrono::milliseconds localProviderRefreshPollInterval{std::chrono::seconds(15)};
    std::chrono::milliseconds taskbarRefreshPollInterval{
        std::chrono::duration_cast<std::chrono::milliseconds>(kTaskbarRefreshPollInterval)};
    std::chrono::milliseconds taskbarTrayOnlyRefreshPollInterval{
        std::chrono::duration_cast<std::chrono::milliseconds>(kTaskbarTrayOnlyRefreshPollInterval)};
    std::chrono::milliseconds miniMaxRefreshPollInterval{
        std::chrono::duration_cast<std::chrono::milliseconds>(kMiniMaxRefreshPollInterval)};
    std::chrono::milliseconds scheduledExportPollInterval{std::chrono::seconds(60)};
    std::chrono::milliseconds scheduledExportDormantPollInterval{std::chrono::hours(6)};
    std::chrono::milliseconds retentionPollInterval{
        std::chrono::duration_cast<std::chrono::milliseconds>(kRetentionCheckInterval)};
    std::optional<RefreshWatcherFileSample> lastObservedCodexRolloutSample;
    std::optional<RefreshWatcherFileSample> lastObservedLocalProviderArtifactSample;
    size_t openAICodexWatcherUnchangedPasses = 0;
    size_t localProviderWatcherUnchangedPasses = 0;
    std::map<std::string, DirectApiQuotaPollState> directApiQuotaPollStates;
    std::uint64_t quotaRefreshGeneration = 0;
    bool claudeCodeDetected = false;
    bool kimiCodeDetected = false;
    std::string claudeCodeDetectionMessage;
    std::string kimiCodeDetectionMessage;
    std::time_t lastScheduledExportAt = 0;
    std::time_t lastRetentionRunAt = 0;
    std::atomic<bool> shuttingDown{false};
    std::atomic<bool> exitRequested{false};
#ifdef JISTATS_ENABLE_TEST_HOOKS
    bool forceDashboardInitFailureForTesting = false;
    std::shared_ptr<ProviderManager> testProviderManager;
    bool headlessForTesting = false;
    bool disableTaskbarForTesting = false;
    bool taskbarTestModeForTesting = false;
    bool disableCodexTokenImportForTesting = false;
    std::shared_future<void> miniMaxValidationReleaseForTesting;
    std::function<void()> afterSettingsDequeueForTesting;
    std::function<void()> beforeRuntimePublicationForTesting;
    std::function<void()> beforeCodexSourceReadForTesting;
    std::optional<std::time_t> currentTimeForTesting;
#endif

    Impl()
        : analysisQueryEngine(storageService),
          settingsService(storageService),
          observationPersistenceBridge(storageService) {}

    bool storageAvailable = false;

    PersistedMetricSnapshot rebuildPersistedMetricSnapshot(
        std::time_t currentTime,
        PersistedMetricSnapshotMode mode = PersistedMetricSnapshotMode::Full) {
        const PersistedMetricSnapshot snapshot =
            buildRollupBackedMetricSnapshot(storageService, currentTime, mode);
        {
            std::lock_guard<std::mutex> lock(persistedMetricMutex);
            latestPersistedMetricSnapshot = snapshot;
            latestPersistedMetricSnapshotAt = currentTime;
            latestPersistedMetricSnapshotMode = mode;
            persistedMetricSnapshotDirty = false;
        }
        return snapshot;
    }

    PersistedMetricSnapshot copyPersistedMetricSnapshot() {
        std::lock_guard<std::mutex> lock(persistedMetricMutex);
        return latestPersistedMetricSnapshot;
    }

    void markPersistedMetricSnapshotDirty(bool notifyWatcher = true) {
        {
            std::lock_guard<std::mutex> lock(persistedMetricMutex);
            persistedMetricSnapshotDirty = true;
        }
        if (notifyWatcher) {
            refreshWatcherCv.notify_all();
        }
    }

    bool latestPersistedMetricSnapshotSatisfies(PersistedMetricSnapshotMode requestedMode) const {
        if (requestedMode == PersistedMetricSnapshotMode::TrayOnly) {
            return true;
        }
        return latestPersistedMetricSnapshotMode == PersistedMetricSnapshotMode::Full;
    }

    std::chrono::milliseconds persistedMetricSnapshotFallbackInterval(
        PersistedMetricSnapshotMode mode) const {
        return mode == PersistedMetricSnapshotMode::Full
            ? taskbarRefreshPollInterval
            : taskbarTrayOnlyRefreshPollInterval;
    }

    PersistedMetricSnapshot ensureFreshPersistedMetricSnapshot(
        std::time_t now,
        PersistedMetricSnapshotMode mode = PersistedMetricSnapshotMode::Full) {
        {
            std::lock_guard<std::mutex> lock(persistedMetricMutex);
            const auto fallbackInterval = persistedMetricSnapshotFallbackInterval(mode);
            const auto elapsed = latestPersistedMetricSnapshotAt > 0
                ? std::chrono::seconds(std::max<std::time_t>(0, now - latestPersistedMetricSnapshotAt))
                : fallbackInterval;
            if (!persistedMetricSnapshotDirty &&
                latestPersistedMetricSnapshotAt > 0 &&
                latestPersistedMetricSnapshotSatisfies(mode) &&
                elapsed < fallbackInterval) {
                return latestPersistedMetricSnapshot;
            }
        }

        return rebuildPersistedMetricSnapshot(now, mode);
    }

    bool hasLiveResources() const {
        return initialized ||
               storageAvailable ||
               localObservationService.isInitialized() ||
               storageService.isOpen() ||
               Logger::isInitialized();
    }

    void startInteractionWorkThread() {
        if (interactionWorkThread.joinable()) {
            return;
        }
        interactionWorkThread = std::thread([this]() {
            runInteractionWorkLoop();
        });
    }

    void requestPreferencesChange(const DashboardPreferences& preferences) {
        {
            std::lock_guard<std::mutex> lock(interactionWorkMutex);
            pendingInteractionPreferences = preferences;
        }
        interactionWorkCv.notify_one();
    }

    void requestAnalysisScopeChange(const AnalysisScopeState& scope) {
        {
            std::lock_guard<std::mutex> lock(interactionWorkMutex);
            pendingAnalysisScope = scope;
        }
        interactionWorkCv.notify_one();
    }

    CodexAttributionChange makeCodexAttributionChange(bool enabled, std::uint64_t request = 0) {
        return {enabled, enabled ? requestedCodexRevocations.load() : ++requestedCodexRevocations, request};
    }

    void requestCodexModelAttributionChange(bool enabled, std::uint64_t request) {
        {
            std::lock_guard<std::mutex> lock(interactionWorkMutex);
            pendingCodexModelAttribution = makeCodexAttributionChange(enabled, request);
        }
        interactionWorkCv.notify_all();
    }

    void requestProviderDiscoveryRefresh() {
        {
            std::lock_guard<std::mutex> lock(interactionWorkMutex);
            pendingProviderDiscoveryRefresh = true;
        }
        interactionWorkCv.notify_all();
    }

    void requestPersistedActivityRefresh() {
        {
            std::lock_guard<std::mutex> lock(interactionWorkMutex);
            if (shuttingDown.load()) {
                return;
            }
            pendingPersistedActivityRefresh = true;
        }
        interactionWorkCv.notify_one();
    }

    void runInteractionWorkLoop() {
        while (true) {
            std::optional<DashboardPreferences> preferences;
            std::optional<AnalysisScopeState> analysisScope;
            std::optional<CodexAttributionChange> codexModelAttribution;
            bool requiresProviderDiscoveryRefresh = false;
            bool requiresPersistedActivityRefresh = false;
            {
                std::unique_lock<std::mutex> lock(interactionWorkMutex);
                const auto workReady = [this]() {
                    return shuttingDown.load() || pendingInteractionPreferences.has_value() ||
                           pendingAnalysisScope.has_value() || pendingCodexModelAttribution.has_value() ||
                           pendingProviderDiscoveryRefresh || pendingPersistedActivityRefresh;
                };
                if (codexAttributionSaveError.load() == CodexAttributionSaveError::Revocation) {
                    if (!interactionWorkCv.wait_for(lock, std::chrono::seconds(15), workReady)) {
                        pendingCodexModelAttribution = CodexAttributionChange{false, requestedCodexRevocations.load()};
                    }
                } else {
                    interactionWorkCv.wait(lock, workReady);
                }
                if (shuttingDown.load() && !pendingInteractionPreferences.has_value() &&
                    !pendingAnalysisScope.has_value() && !pendingCodexModelAttribution.has_value() &&
                    !pendingProviderDiscoveryRefresh) {
                    pendingPersistedActivityRefresh = false;
                    break;
                }
                preferences = pendingInteractionPreferences;
                activeInteractionPreferences = preferences;
                analysisScope = pendingAnalysisScope;
                codexModelAttribution = pendingCodexModelAttribution;
                requiresProviderDiscoveryRefresh = pendingProviderDiscoveryRefresh;
                requiresPersistedActivityRefresh = pendingPersistedActivityRefresh;
                pendingInteractionPreferences.reset();
                pendingAnalysisScope.reset();
                pendingCodexModelAttribution.reset();
                pendingProviderDiscoveryRefresh = false;
                pendingPersistedActivityRefresh = false;
            }

            if (codexModelAttribution.has_value()) {
                applyCodexModelAttributionPreference(*codexModelAttribution);
                if (codexModelAttribution->request != 0) {
                    completedCodexAttributionRequest.store(codexModelAttribution->request);
                }
                refreshPersistedActivityPresenters(std::time(nullptr));
            }

            if (preferences.has_value()) {
#ifdef JISTATS_ENABLE_TEST_HOOKS
                if (afterSettingsDequeueForTesting) {
                    afterSettingsDequeueForTesting();
                }
#endif
                const auto effectivePreferences =
                    refreshProviderRuntimeState(std::time(nullptr), requiresProviderDiscoveryRefresh, true,
                                                &*preferences, analysisScope, requiresPersistedActivityRefresh);
                {
                    std::lock_guard<std::mutex> lock(interactionWorkMutex);
                    activeInteractionPreferences.reset();
                }
                refreshWatcherCv.notify_all();
#ifdef JISTATS_ENABLE_TEST_HOOKS
                if (!disableTaskbarForTesting) {
                    applyTaskbarPreference(effectivePreferences);
                }
#else
                applyTaskbarPreference(effectivePreferences);
#endif
                runScheduledExportIfDue(std::time(nullptr));
                continue;
            }

            if (analysisScope.has_value()) {
                refreshPersistedActivityPresenters(std::time(nullptr), &*analysisScope);
            } else if (requiresPersistedActivityRefresh && !shuttingDown.load() && shouldRefreshAnalysisSurface()) {
                refreshPersistedActivityPresenters(std::time(nullptr));
            }

            if (requiresProviderDiscoveryRefresh) {
                runProviderDiscovery();
                std::lock_guard<std::mutex> noticeLock(offlineNoticeMutex);
                DashboardViewModel refreshedViewModel = dashboardPresenter.getViewModel();
                if (!refreshedViewModel.appVersion.empty()) {
                    applyProviderDiscoveryStatusToViewModel(refreshedViewModel);
                    dashboardPresenter.updateViewModel(refreshedViewModel);
                }
            }
        }
    }

    void joinInteractionWorkThread() {
        interactionWorkCv.notify_all();
        if (interactionWorkThread.joinable() &&
            interactionWorkThread.get_id() != std::this_thread::get_id()) {
            interactionWorkThread.join();
        }
    }

    std::time_t currentTime() const {
#ifdef JISTATS_ENABLE_TEST_HOOKS
        if (currentTimeForTesting.has_value()) {
            return *currentTimeForTesting;
        }
#endif
        return std::time(nullptr);
    }

    std::time_t currentDirectApiQuotaPollIntervalSeconds(
        const DirectApiQuotaPollState& pollState,
        std::time_t now) const {
        if (pollState.hasResponse &&
            pollState.response.success &&
            pollState.stableSince > 0 &&
            (now - pollState.stableSince) >= kDirectApiQuotaStableWindowSeconds) {
            return kDirectApiQuotaStablePollIntervalSeconds;
        }
        return kDirectApiQuotaActivePollIntervalSeconds;
    }

    QuotaResponse fetchQuotaWithDirectApiBudget(
        const std::shared_ptr<IProviderAdapter>& provider,
        std::time_t now,
        QuotaRefresh& refresh) {
        if (!provider || !provider->isEnabled()) {
            return QuotaResponse{};
        }

        if (!provider->hasDirectQuotaApiConfigured()) {
            directApiQuotaPollStates.erase(provider->getProviderKey());
            refresh.fetchedProviderKeys.insert(provider->getProviderKey());
            return provider->fetchQuota();
        }

        const std::string throttleKey = provider->directQuotaApiThrottleKey().empty()
            ? provider->getProviderKey()
            : provider->directQuotaApiThrottleKey();
        DirectApiQuotaPollState& pollState = directApiQuotaPollStates[throttleKey];
        const std::time_t intervalSeconds = currentDirectApiQuotaPollIntervalSeconds(pollState, now);
        if (pollState.hasResponse &&
            pollState.lastAttemptAt > 0 &&
            (now - pollState.lastAttemptAt) < intervalSeconds) {
            // The post-catch-up rebuild may reuse a result fetched in this cycle.
            if (pollState.fetchedInRefresh == refresh.generation) {
                refresh.fetchedProviderKeys.insert(provider->getProviderKey());
                refresh.providerApiKeys.insert(provider->getProviderKey());
            }
            return pollState.response;
        }

        const QuotaResponse response = provider->fetchQuota();
        const bool unchanged = pollState.hasResponse &&
            pollState.response.success &&
            response.success &&
            quotaUsageFingerprintMatches(pollState.response, response);

        pollState.lastAttemptAt = now;
        pollState.hasResponse = true;
        pollState.response = response;
        pollState.fetchedInRefresh = refresh.generation;
        refresh.fetchedProviderKeys.insert(provider->getProviderKey());
        refresh.providerApiKeys.insert(provider->getProviderKey());

        if (response.success) {
            if (!unchanged || pollState.stableSince <= 0) {
                pollState.stableSince = now;
            }
        } else {
            pollState.stableSince = 0;
        }

        return pollState.response;
    }

    bool hasProviderOptOutInProgress(const std::string& providerKey) {
        // A Settings request can be waiting for the refresh lock with the old opt-in still on disk.
        std::lock_guard<std::mutex> lock(interactionWorkMutex);
        return (activeInteractionPreferences &&
                !isProviderTrackingEnabled(*activeInteractionPreferences, providerKey)) ||
               (pendingInteractionPreferences &&
                !isProviderTrackingEnabled(*pendingInteractionPreferences, providerKey)) ||
               (failedInteractionPreferences && !isProviderTrackingEnabled(*failedInteractionPreferences, providerKey));
    }

    std::vector<QuotaResponse> fetchQuotaResponses(std::vector<std::shared_ptr<IProviderAdapter>>& providers,
                                                   std::time_t now, QuotaRefresh& refresh) {
        std::vector<QuotaResponse> responses;
        responses.reserve(providers.size());
        for (auto nextProvider = providers.begin(); nextProvider != providers.end();) {
            const auto& provider = *nextProvider;
            const auto key = provider->getProviderKey();
            if (exitRequested.load() || hasProviderOptOutInProgress(key)) {
                nextProvider = providers.erase(nextProvider);
                continue;
            }
            if (refresh.deferNewSubscriptions && (key == "grok" || key == "cursor")) {
                QuotaResponse pending;
                pending.unavailable = true;
                pending.error = "Checking subscription quota";
                responses.push_back(std::move(pending));
            } else {
                responses.push_back(fetchQuotaWithDirectApiBudget(provider, now, refresh));
            }
            ++nextProvider;
        }
        return responses;
    }

    ProviderManager& activeProviderManager() {
#ifdef JISTATS_ENABLE_TEST_HOOKS
        if (testProviderManager) {
            return *testProviderManager;
        }
#endif
        return providerManager;
    }

    DashboardPreferences effectiveRuntimePreferences(DashboardPreferences preferences) const {
        std::lock_guard<std::mutex> lock(interactionWorkMutex);
        if (!failedInteractionPreferences) {
            return preferences;
        }
        DashboardPreferences effective = *failedInteractionPreferences;
        effective.codexModelAttributionEnabled = preferences.codexModelAttributionEnabled;
        setAnalysisScope(effective, analysisScopeFromPreferences(preferences));
        return effective;
    }

    DashboardPreferences currentDashboardPreferences() const {
        const DashboardViewModel viewModel = dashboardPresenter.getViewModel();
        if (!viewModel.appVersion.empty()) {
            return effectiveRuntimePreferences(viewModel.preferences);
        }
        return effectiveRuntimePreferences(loadPreferences());
    }

    bool shouldPersistLocalActivity(const ActivityEvent& event) {
        return isProviderTrackingEnabled(currentDashboardPreferences(), event.providerKey) &&
               !hasProviderOptOutInProgress(event.providerKey);
    }

    void persistQuotaSnapshots(
        const std::vector<std::shared_ptr<IProviderAdapter>>& providers,
        const std::vector<QuotaResponse>& quotaResponses,
        std::time_t now) {
        if (!storageAvailable || !storageService.isOpen()) {
            return;
        }

        const size_t pairCount = std::min(providers.size(), quotaResponses.size());
        std::set<std::pair<int, int>> touchedScopes;
        for (size_t index = 0; index < pairCount; ++index) {
            const auto& provider = providers[index];
            const auto& quota = quotaResponses[index];
            if (!provider || !shouldExposeQuotaSnapshot(quota)) {
                continue;
            }

            const std::string providerKey = provider->getProviderKey();
            const auto providerId = findProviderIdByKey(storageService, providerKey);
            const auto accountId =
                ensureRuntimeTrackingAccountId(storageService, providerKey, provider->getDisplayName());
            if (!providerId.has_value() || !accountId.has_value()) {
                continue;
            }

            touchedScopes.emplace(*providerId, *accountId);

            const std::string confidenceTier = quotaConfidenceTierForResponse(providerKey, quota);
            const std::string displayName = quotaPlanLabelForResponse(providerKey, quota);
            const bool percentageReading = providerKey == "grok" || providerKey == "cursor";
            const std::time_t snapshotTime = quota.snapshotTime > 0 ? quota.snapshotTime : now;

            const NormalizedQuotaValues primaryValues = normalizeQuotaValues(quota);
            const auto primaryPlanId = ensureQuotaSnapshotPlanId(
                storageService,
                *accountId,
                providerKey,
                quota.providerSurface,
                quota.windowType,
                displayName,
                primaryValues.limit);
            const std::string primaryBasisSummary =
                "Runtime quota snapshot captured from " +
                reportSourceLabelForSurface(quota.providerSurface.empty() ? providerKey : quota.providerSurface);
            persistQuotaSnapshotRow(storageService, *providerId, *accountId, primaryPlanId, snapshotTime,
                                    quota.windowStart, quota.windowEnd, primaryValues.used, primaryValues.limit,
                                    primaryValues.remaining, confidenceTier, primaryBasisSummary, percentageReading);

            if (quota.secondaryLimit > 0 || quota.secondaryUsed > 0 || quota.secondaryRemaining > 0) {
                QuotaResponse secondaryQuota = quota;
                secondaryQuota.windowStart = quota.secondaryWindowStart;
                secondaryQuota.windowEnd = quota.secondaryWindowEnd;
                secondaryQuota.windowType = quota.secondaryWindowType;
                secondaryQuota.used = quota.secondaryUsed;
                secondaryQuota.limit = quota.secondaryLimit;
                secondaryQuota.remaining = quota.secondaryRemaining;
                secondaryQuota.secondaryWindowStart = 0;
                secondaryQuota.secondaryWindowEnd = 0;
                secondaryQuota.secondaryUsed = 0;
                secondaryQuota.secondaryLimit = 0;
                secondaryQuota.secondaryRemaining = 0;
                secondaryQuota.secondaryWindowType.clear();

                const NormalizedQuotaValues secondaryValues = normalizeQuotaValues(secondaryQuota);
                const auto secondaryPlanId = ensureQuotaSnapshotPlanId(
                    storageService,
                    *accountId,
                    providerKey,
                    quota.providerSurface,
                    secondaryQuota.windowType,
                    displayName,
                    secondaryValues.limit);
                const std::string secondaryBasisSummary =
                    "Runtime quota snapshot captured from " +
                    reportSourceLabelForSurface(quota.providerSurface.empty() ? providerKey : quota.providerSurface) +
                    " (" + (secondaryQuota.windowType.empty() ? std::string("secondary") : secondaryQuota.windowType) +
                    " window)";
                persistQuotaSnapshotRow(storageService, *providerId, *accountId, secondaryPlanId, snapshotTime,
                                        secondaryQuota.windowStart, secondaryQuota.windowEnd, secondaryValues.used,
                                        secondaryValues.limit, secondaryValues.remaining, confidenceTier,
                                        secondaryBasisSummary, percentageReading);
            }
        }

        const std::time_t rebuildAnchor = now > 0 ? now : std::time(nullptr);
        const std::time_t dayStart = utcDayStart(rebuildAnchor);
        const std::time_t dayEnd = dayStart + 86399;
        for (const auto& scope : touchedScopes) {
            storageService.rebuildQuotaSnapshotRollups(scope.first, scope.second, dayStart, dayEnd);
        }
    }

    std::shared_ptr<MiniMaxAdapter> resolveMiniMaxRuntimeAdapter() const {
#ifdef JISTATS_ENABLE_TEST_HOOKS
        if (testProviderManager) {
            const auto injected = std::dynamic_pointer_cast<MiniMaxAdapter>(
                testProviderManager->getProvider("minimax"));
            if (injected) {
                return injected;
            }
        }
#endif

        const auto registered = std::dynamic_pointer_cast<MiniMaxAdapter>(
            providerManager.getProvider("minimax"));
        if (registered) {
            return registered;
        }

        return miniMaxCredentialAdapter;
    }

    std::shared_ptr<IProviderAdapter> resolveOpenAIRuntimeAdapter() const {
#ifdef JISTATS_ENABLE_TEST_HOOKS
        if (testProviderManager) {
            const auto injected = testProviderManager->getProvider("openai");
            if (injected) {
                return injected;
            }
        }
#endif
        return providerManager.getProvider("openai");
    }

    void ensureMiniMaxCredentialAdapter() {
        if (!miniMaxCredentialAdapter) {
            miniMaxCredentialAdapter = std::make_shared<MiniMaxAdapter>();
        }

        if (miniMaxCredentialStore) {
            miniMaxCredentialAdapter->setCredentialStore(miniMaxCredentialStore);
        }
    }

    void syncActiveMiniMaxRuntimeProfile(const std::string& preferredPlanKey = "") {
        const auto adapter = resolveMiniMaxRuntimeAdapter();
        if (!adapter) {
            return;
        }

        MiniMaxSubscriptionCredentialProfile selectedProfile;
        bool hasSelectedProfile = false;
        {
            std::lock_guard<std::mutex> lock(miniMaxValidationMutex);
            if (!preferredPlanKey.empty()) {
                const auto preferred = miniMaxCredentialProfiles.find(preferredPlanKey);
                if (preferred != miniMaxCredentialProfiles.end() &&
                    !preferred->second.secretRef.empty()) {
                    selectedProfile = preferred->second;
                    hasSelectedProfile = true;
                }
            }

            if (!hasSelectedProfile) {
                for (const std::string& planKey : minimaxSupportedPlanKeys()) {
                    const auto it = miniMaxCredentialProfiles.find(planKey);
                    if (it != miniMaxCredentialProfiles.end() && !it->second.secretRef.empty()) {
                        selectedProfile = it->second;
                        hasSelectedProfile = true;
                        break;
                    }
                }
            }
        }

        if (hasSelectedProfile) {
            adapter->setSubscriptionCredentialProfile(selectedProfile);
        }
    }

    void applyOpenAIPreferenceToProviderManager(bool enabled) {
#ifdef JISTATS_ENABLE_TEST_HOOKS
        if (testProviderManager) {
            if (const auto provider = testProviderManager->getProvider("openai")) {
                provider->setEnabled(enabled);
            }
        }
#endif

        if (const auto provider = providerManager.getProvider("openai")) {
            provider->setEnabled(enabled);
        }
    }

    void applyMiniMaxPreferenceToProviderManager(bool enabled) {
#ifdef JISTATS_ENABLE_TEST_HOOKS
        if (testProviderManager) {
            if (const auto provider = testProviderManager->getProvider("minimax")) {
                provider->setEnabled(enabled);
            }
        }
#endif

        if (const auto provider = providerManager.getProvider("minimax")) {
            provider->setEnabled(enabled);
        }
    }

    void applyClaudeCodePreferenceToProviderManager(bool enabled) {
#ifdef JISTATS_ENABLE_TEST_HOOKS
        if (testProviderManager) {
            if (const auto provider = testProviderManager->getProvider("claude_code")) {
                provider->setEnabled(enabled);
            }
        }
#endif

        if (const auto provider = providerManager.getProvider("claude_code")) {
            provider->setEnabled(enabled);
        }
    }

    void applyKimiCodePreferenceToProviderManager(bool enabled) {
#ifdef JISTATS_ENABLE_TEST_HOOKS
        if (testProviderManager) {
            if (const auto provider = testProviderManager->getProvider("kimi")) {
                provider->setEnabled(enabled);
            }
        }
#endif

        if (const auto provider = providerManager.getProvider("kimi")) {
            provider->setEnabled(enabled);
        }
    }

    void applyProviderPreferences(const DashboardPreferences& preferences) {
        applyOpenAIPreferenceToProviderManager(preferences.openaiSubscriptionTrackingEnabled);
        applyMiniMaxPreferenceToProviderManager(preferences.miniMaxSubscriptionTrackingEnabled);
        applyClaudeCodePreferenceToProviderManager(preferences.claudeCodeEnabled);
        applyKimiCodePreferenceToProviderManager(preferences.kimiCodeEnabled);
        for (const auto* key : {"grok", "cursor"}) {
            if (const auto provider = activeProviderManager().getProvider(key)) {
                provider->setEnabled(isProviderTrackingEnabled(preferences, key));
            }
        }
        const unsigned subscriptionMask = (preferences.grokSubscriptionTrackingEnabled ? 1u : 0u) |
                                          (preferences.cursorSubscriptionTrackingEnabled ? 2u : 0u);
        bool changed;
        {
            std::lock_guard<std::mutex> lock(refreshWatcherMutex);
            changed = subscriptionWatchMask.exchange(subscriptionMask) != subscriptionMask;
        }
        if (changed) {
            refreshWatcherCv.notify_all();
        }
    }

    void importEnabledLocalProviderActivity(const DashboardPreferences& preferences) {
        if (!storageService.isOpen()) {
            return;
        }

        bool importedActivity = false;
        if (preferences.claudeCodeEnabled) {
            const std::string claudeStatsPath = ClaudeCodeLocalParser::resolveDefaultStatsCachePath();
            if (!claudeStatsPath.empty()) {
                std::error_code ec;
                if (std::filesystem::exists(claudeStatsPath, ec)) {
                    ClaudeCodeLocalParser::importLocalActivity(
                        storageService,
                        claudeStatsPath,
                        ClaudeCodeLocalParser::resolveDefaultSettingsPath());
                    importedActivity = true;
                }
            }
        }

#if JISTATS_HAS_KIMI_ADAPTER
        if (preferences.kimiCodeEnabled) {
            const std::string kimiRootPath = KimiAdapter::resolveDefaultRootPath();
            if (!kimiRootPath.empty()) {
                std::error_code ec;
                if (std::filesystem::exists(kimiRootPath, ec)) {
                    KimiAdapter::importLocalActivity(storageService, kimiRootPath);
                    importedActivity = true;
                }
            }
        }
#endif

        const bool skipCodexTokenImport =
#ifdef JISTATS_ENABLE_TEST_HOOKS
            headlessForTesting || disableCodexTokenImportForTesting;
#else
            false;
#endif
        if (preferences.openaiSubscriptionTrackingEnabled && !skipCodexTokenImport) {
            const std::string codexRolloutDir = OpenAIAdapter::resolveCodexRolloutDirectory();
            if (!codexRolloutDir.empty()) {
                std::error_code ec;
                if (std::filesystem::exists(codexRolloutDir, ec)) {
                    const auto batch = OpenAIAdapter::collectCodexTokenUsageSnapshots(codexRolloutDir);
                    if (!batch.observations.empty() || !batch.savedCounters.empty()) {
                        const auto providerId = findProviderIdByKey(storageService, "openai");
                        const auto accountId =
                            ensureRuntimeTrackingAccountId(storageService, "openai", "OpenAI");
                        if (providerId && accountId &&
                            OpenAIAdapter::persistCodexTokenUsageSnapshots(storageService, *providerId, *accountId,
                                                                           batch)) {
                            importedActivity = true;
                        }
                    }
                }
            }
        }

        if (importedActivity) {
            markPersistedMetricSnapshotDirty(false);
        }
    }

    void runProviderDiscovery() {
        std::shared_ptr<IProviderAdapter> claudeProvider;
#ifdef JISTATS_ENABLE_TEST_HOOKS
        if (testProviderManager) {
            claudeProvider = testProviderManager->getProvider("claude_code");
        }
#endif
        if (!claudeProvider) {
            claudeProvider = providerManager.getProvider("claude_code");
        }

        if (claudeProvider) {
            claudeCodeDetected = claudeProvider->validateCredentials();
            claudeCodeDetectionMessage = claudeCodeDetected
                                             ? "Detected Claude Code stats cache"
                                             : "Not detected. Install Claude Code to enable local tracking.";
        }

        kimiCodeDetected = KimiAdapter::hasLocalInstallation();
        kimiCodeDetectionMessage = kimiCodeDetected
            ? "Detected at ~/.kimi"
            : "Not detected. Install Kimi Code to enable local tracking.";
    }

    bool shouldImportLocalActivityDuringStartupRefresh(
        const DashboardPreferences& preferences) const {
        return preferences.openaiSubscriptionTrackingEnabled || preferences.claudeCodeEnabled ||
               preferences.kimiCodeEnabled;
    }

    bool shouldQueueDeferredStartupRefresh(
        const DashboardPreferences& preferences,
        bool usedPersistedStartupHydration,
        bool executedStartupCatchUp) {
        if (executedStartupCatchUp || hasPendingRecovery() || preferences.grokSubscriptionTrackingEnabled ||
            preferences.cursorSubscriptionTrackingEnabled) {
            return true;
        }

        if (!usedPersistedStartupHydration) {
            return false;
        }

        return preferences.openaiSubscriptionTrackingEnabled ||
            preferences.claudeCodeEnabled ||
            preferences.kimiCodeEnabled;
    }

    void joinMiniMaxValidationThreads() {
        std::vector<std::thread> threads;
        {
            std::lock_guard<std::mutex> lock(miniMaxValidationMutex);
            threads.swap(miniMaxValidationThreads);
        }

        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    void joinStartupRefreshThread() {
        if (startupRefreshThread.joinable()) {
            startupRefreshThread.join();
        }
    }

    void joinTaskbarRefreshThread() {
        if (taskbarRefreshThread.joinable()) {
            taskbarRefreshThread.join();
        }
    }

    void joinOpenAICodexRefreshThread() {
        if (openAICodexRefreshThread.joinable()) {
            openAICodexRefreshThread.join();
        }
    }

    void joinLocalProviderRefreshThread() {
        if (localProviderRefreshThread.joinable()) {
            localProviderRefreshThread.join();
        }
    }

    void joinMiniMaxRefreshThread() {
        if (miniMaxRefreshThread.joinable()) {
            miniMaxRefreshThread.join();
        }
    }

    void joinScheduledExportThread() {
        if (scheduledExportThread.joinable()) {
            scheduledExportThread.join();
        }
    }

    void joinRetentionThread() {
        if (retentionThread.joinable()) {
            retentionThread.join();
        }
    }

    bool waitForRefreshWatcherInterval(std::chrono::milliseconds interval) {
        std::unique_lock<std::mutex> lock(refreshWatcherMutex);
        return refreshWatcherCv.wait_for(lock, interval, [this]() { return shuttingDown.load(); });
    }

    void queueDeferredStartupRefresh(bool refreshProviderDiscovery, bool importLocalActivity) {
        joinStartupRefreshThread();
        startupRefreshThread = std::thread([this, refreshProviderDiscovery, importLocalActivity]() {
            if (shouldStopMiniMaxValidation()) {
                return;
            }
            refreshProviderRuntimeState(currentTime(), refreshProviderDiscovery, importLocalActivity);
        });
    }

    bool shouldRunTaskbarRefreshWatcher() const {
#ifdef JISTATS_ENABLE_TEST_HOOKS
        if (headlessForTesting) {
            return false;
        }
#endif
        const DashboardPreferences preferences = currentDashboardPreferences();
        return preferences.taskbarOverlayEnabled || trayController.isAvailable();
    }

    std::chrono::milliseconds taskbarRefreshWaitInterval(std::time_t now) const {
        if (!shouldRunTaskbarRefreshWatcher()) {
            return std::chrono::hours(6);
        }

        const DashboardPreferences preferences = currentDashboardPreferences();
        const PersistedMetricSnapshotMode requestedMode = preferences.taskbarOverlayEnabled
            ? PersistedMetricSnapshotMode::Full
            : PersistedMetricSnapshotMode::TrayOnly;

        std::lock_guard<std::mutex> lock(persistedMetricMutex);
        if (persistedMetricSnapshotDirty ||
            latestPersistedMetricSnapshotAt <= 0 ||
            !latestPersistedMetricSnapshotSatisfies(requestedMode)) {
            return std::chrono::milliseconds(0);
        }

        const auto fallbackInterval = persistedMetricSnapshotFallbackInterval(requestedMode);
        const auto elapsed = std::chrono::seconds(
            std::max<std::time_t>(0, now - latestPersistedMetricSnapshotAt));
        if (elapsed >= fallbackInterval) {
            return std::chrono::milliseconds(0);
        }

        return std::chrono::duration_cast<std::chrono::milliseconds>(fallbackInterval - elapsed);
    }

    void refreshTaskbarPresentationFromPersistedState(std::time_t now) {
        if (!shouldRunTaskbarRefreshWatcher()) {
            return;
        }

        DashboardViewModel viewModel = dashboardPresenter.getViewModel();
        viewModel.preferences = effectiveRuntimePreferences(std::move(viewModel.preferences));
        if (viewModel.appVersion.empty()) {
            return;
        }

        const PersistedMetricSnapshotMode snapshotMode = viewModel.preferences.taskbarOverlayEnabled
            ? PersistedMetricSnapshotMode::Full
            : PersistedMetricSnapshotMode::TrayOnly;
        ensureFreshPersistedMetricSnapshot(now, snapshotMode);
        syncTrayRuntimeIndicator(viewModel);

        if (!viewModel.preferences.taskbarOverlayEnabled) {
            return;
        }

#ifdef JISTATS_ENABLE_TEST_HOOKS
        if (headlessForTesting || taskbarTestModeForTesting) {
            taskbarPresenter.setRuntimeStateForTesting(true, true);
            taskbarPresenter.replaceMetrics(buildRuntimeTaskbarMetrics(now, viewModel));
            return;
        }
#endif

        if (!taskbarPresenter.isAvailable()) {
            if (!taskbarPresenter.init()) {
                return;
            }
            taskbarPresenter.setOverlayEnabled(true);
        }

        taskbarPresenter.replaceMetrics(buildRuntimeTaskbarMetrics(now, viewModel));
    }

    void startTaskbarRefreshWatcher() {
        joinTaskbarRefreshThread();
        taskbarRefreshThread = std::thread([this]() {
            while (!shouldStopMiniMaxValidation()) {
                const std::time_t now = currentTime();
                if (taskbarRefreshWaitInterval(now) == std::chrono::milliseconds(0)) {
                    refreshTaskbarPresentationFromPersistedState(now);
                }
                if (waitForRefreshWatcherInterval(taskbarRefreshWaitInterval(currentTime()))) {
                    break;
                }
            }
        });
    }

    bool shouldRunOpenAICodexRefreshWatcher() const {
        const auto provider = resolveOpenAIRuntimeAdapter();
        return provider && provider->isEnabled();
    }

    bool shouldRunMiniMaxRefreshWatcher() const {
        const auto provider = resolveMiniMaxRuntimeAdapter();
        if (!provider || !provider->isEnabled()) {
            return false;
        }

        std::lock_guard<std::mutex> lock(miniMaxValidationMutex);
        for (const auto& [planKey, profile] : miniMaxCredentialProfiles) {
            (void)planKey;
            if (!profile.secretRef.empty()) {
                return true;
            }
        }
        return false;
    }

    std::chrono::milliseconds nextMiniMaxRefreshWaitInterval(std::time_t now) const {
        if (!shouldRunMiniMaxRefreshWatcher()) {
            return dormantRefreshWatcherInterval();
        }

        const auto provider = resolveMiniMaxRuntimeAdapter();
        if (!provider || !provider->isEnabled() || !provider->hasDirectQuotaApiConfigured()) {
            return dormantRefreshWatcherInterval();
        }

        const std::string throttleKey = provider->directQuotaApiThrottleKey().empty()
            ? provider->getProviderKey()
            : provider->directQuotaApiThrottleKey();
        const auto it = directApiQuotaPollStates.find(throttleKey);
        if (it == directApiQuotaPollStates.end() || !it->second.hasResponse || it->second.lastAttemptAt <= 0) {
            return miniMaxRefreshPollInterval;
        }

        const std::time_t intervalSeconds = currentDirectApiQuotaPollIntervalSeconds(it->second, now);
        const std::time_t dueAt = it->second.lastAttemptAt + intervalSeconds;
        if (dueAt <= now) {
            return std::chrono::milliseconds(0);
        }

        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::seconds(std::max<std::time_t>(1, dueAt - now)));
    }

    bool shouldRunLocalProviderRefreshWatcher() const {
        const DashboardPreferences preferences = currentDashboardPreferences();
        return preferences.claudeCodeEnabled || preferences.kimiCodeEnabled;
    }

    bool detectOpenAICodexRolloutUpdate() {
        const auto latestSample = sampleLatestCodexRolloutFileForRefreshWatcher();
        if (!latestSample.has_value()) {
            return false;
        }

        if (!lastObservedCodexRolloutSample.has_value()) {
            lastObservedCodexRolloutSample = latestSample;
            return true;
        }

        if (latestSample->path == lastObservedCodexRolloutSample->path &&
            latestSample->lastWriteTime == lastObservedCodexRolloutSample->lastWriteTime) {
            return false;
        }

        lastObservedCodexRolloutSample = latestSample;
        return true;
    }

    bool detectLocalProviderArtifactUpdate() {
        const DashboardPreferences preferences = currentDashboardPreferences();
        const auto latestSample = sampleLatestLocalProviderArtifactForRefreshWatcher(
            preferences.claudeCodeEnabled,
            preferences.kimiCodeEnabled);
        if (!latestSample.has_value()) {
            return false;
        }

        if (!lastObservedLocalProviderArtifactSample.has_value()) {
            lastObservedLocalProviderArtifactSample = latestSample;
            return true;
        }

        if (latestSample->path == lastObservedLocalProviderArtifactSample->path &&
            latestSample->lastWriteTime == lastObservedLocalProviderArtifactSample->lastWriteTime) {
            return false;
        }

        lastObservedLocalProviderArtifactSample = latestSample;
        return true;
    }

    void startOpenAICodexRefreshWatcher() {
        joinOpenAICodexRefreshThread();
        lastObservedCodexRolloutSample = sampleLatestCodexRolloutFileForRefreshWatcher();
        openAICodexWatcherUnchangedPasses = 0;
        openAICodexRefreshThread = std::thread([this]() {
            while (!shouldStopMiniMaxValidation()) {
                const bool watcherActive = shouldRunOpenAICodexRefreshWatcher();
                if (watcherActive) {
                    if (detectOpenAICodexRolloutUpdate()) {
                        openAICodexWatcherUnchangedPasses = 0;
                        refreshProviderRuntimeState(std::time(nullptr), false, false);
                    } else {
                        ++openAICodexWatcherUnchangedPasses;
                    }
                } else {
                    openAICodexWatcherUnchangedPasses = 0;
                }

                if (waitForRefreshWatcherInterval(
                        adaptiveWatcherIntervalOrDormant(
                            watcherActive,
                            openAICodexRefreshPollInterval,
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                kOpenAICodexRefreshMaxBackoffInterval),
                            openAICodexWatcherUnchangedPasses))) {
                    break;
                }
            }
        });
    }

    void startLocalProviderRefreshWatcher() {
        joinLocalProviderRefreshThread();
        const DashboardPreferences preferences = currentDashboardPreferences();
        lastObservedLocalProviderArtifactSample = sampleLatestLocalProviderArtifactForRefreshWatcher(
            preferences.claudeCodeEnabled,
            preferences.kimiCodeEnabled);
        localProviderWatcherUnchangedPasses = 0;
        localProviderRefreshThread = std::thread([this]() {
            while (!shouldStopMiniMaxValidation()) {
                const bool watcherActive = shouldRunLocalProviderRefreshWatcher();
                if (watcherActive) {
                    if (detectLocalProviderArtifactUpdate()) {
                        localProviderWatcherUnchangedPasses = 0;
                        refreshProviderRuntimeState(std::time(nullptr), false, true);
                    } else {
                        ++localProviderWatcherUnchangedPasses;
                    }
                } else {
                    localProviderWatcherUnchangedPasses = 0;
                }

                if (waitForRefreshWatcherInterval(
                        adaptiveWatcherIntervalOrDormant(
                            watcherActive,
                            localProviderRefreshPollInterval,
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                kLocalProviderRefreshMaxBackoffInterval),
                            localProviderWatcherUnchangedPasses))) {
                    break;
                }
            }
        });
    }

    void startMiniMaxRefreshWatcher() {
        joinMiniMaxRefreshThread();
        miniMaxRefreshThread = std::thread([this]() {
            while (!shouldStopMiniMaxValidation()) {
                const std::time_t now = currentTime();
                const unsigned subscriptionMask = subscriptionWatchMask.load();
                const bool additionalSubscriptions = subscriptionMask != 0;
                if (additionalSubscriptions || shouldRunMiniMaxRefreshWatcher()) {
                    refreshProviderRuntimeState(now, false, false);
                }

                auto interval = nextMiniMaxRefreshWaitInterval(now);
                if (additionalSubscriptions) {
                    interval = std::min(interval, miniMaxRefreshPollInterval);
                }
                std::unique_lock<std::mutex> lock(refreshWatcherMutex);
                refreshWatcherCv.wait_for(lock, interval, [this, subscriptionMask]() {
                    return shuttingDown.load() || subscriptionWatchMask.load() != subscriptionMask;
                });
            }
        });
    }

    void loadScheduledExportStateFromStorage() {
        std::lock_guard<std::mutex> exportLock(scheduledExportMutex);
        lastScheduledExportAt = 0;
        if (!storageAvailable) {
            return;
        }

        if (const auto value = settingsService.getString(preference_keys::SCHEDULED_EXPORT_LAST_RUN_UTC)) {
            if (const auto parsed = parseTimeUtcIso(*value)) {
                lastScheduledExportAt = *parsed;
            }
        }
    }

    void persistScheduledExportLastRun(std::time_t time) {
        std::lock_guard<std::mutex> exportLock(scheduledExportMutex);
        lastScheduledExportAt = time;
        if (!storageAvailable) {
            return;
        }

        settingsService.setString(
            preference_keys::SCHEDULED_EXPORT_LAST_RUN_UTC,
            formatTimeUtcIso(time));
    }

    std::time_t currentScheduledExportLastRun() {
        std::lock_guard<std::mutex> exportLock(scheduledExportMutex);
        return lastScheduledExportAt;
    }

    void applyScheduledExportStatus(DashboardViewModel& viewModel) {
        const std::time_t lastRunAt = currentScheduledExportLastRun();
        std::optional<PersistedExportRun> latestScheduledRun;
        if (storageAvailable && storageService.isOpen()) {
            latestScheduledRun = storageService.getLatestExportRun("scheduled_html");
        }
        viewModel.scheduledExportLastRunTime =
            lastRunAt > 0 ? formatTimeUtc(lastRunAt) : "";

        if (!viewModel.preferences.scheduledExportEnabled) {
            viewModel.scheduledExportStatusSummary = "Disabled";
            viewModel.scheduledExportStatusDetail =
                "Scheduled HTML export is disabled.";
            if (lastRunAt > 0) {
                viewModel.scheduledExportStatusDetail +=
                    " Last successful run: " + formatTimeUtc(lastRunAt) + ".";
            }
            return;
        }

        const int intervalMinutes = std::max(1, viewModel.preferences.scheduledExportIntervalMinutes);
        viewModel.scheduledExportStatusSummary =
            "Every " + pluralize(intervalMinutes, "min", "mins");

        std::ostringstream detail;
        detail << "Scheduled HTML export is enabled every "
               << pluralize(intervalMinutes, "minute", "minutes") << ". ";
        if (viewModel.preferences.scheduledExportPath.empty()) {
            detail << "Output folder: ReportingService default output directory. ";
        } else {
            detail << "Output folder: " << viewModel.preferences.scheduledExportPath << ". ";
        }
        if (lastRunAt > 0) {
            detail << "Last successful run: " << formatTimeUtc(lastRunAt) << ".";
        } else {
            detail << "Last successful run: Never.";
        }
        if (latestScheduledRun.has_value()) {
            detail << " Latest attempt: "
                   << (latestScheduledRun->requestedAt > 0
                           ? formatTimeUtc(latestScheduledRun->requestedAt)
                           : "Unknown")
                   << " ("
                   << (latestScheduledRun->success ? "success" : "failed")
                   << ").";
            if (!latestScheduledRun->success && !latestScheduledRun->errorSummary.empty()) {
                detail << " Error: " << latestScheduledRun->errorSummary << ".";
            }
        }
        viewModel.scheduledExportStatusDetail = detail.str();
    }

    void applyRecoveryStatus(DashboardViewModel& viewModel) {
        std::vector<std::string> unknownProviders;
        std::vector<std::string> backfilledProviders;
        std::string latestOutcome;
        std::string latestComparisonResult;
        std::string latestProviderDisplayName;
        std::time_t latestOutcomeAt = 0;
        long long latestFetchedEventCount = 0;
        long long latestLocalEventCount = 0;
        long long latestLocalTotalTokens = 0;
        long long latestImportedTotalTokens = 0;

        for (const auto& provider : viewModel.providers) {
            bool providerHasUnknown = false;
            bool providerHasBackfilled = false;
            for (const auto& quota : provider.quotas) {
                providerHasUnknown = providerHasUnknown || quota.hasUnknownGaps;
                providerHasBackfilled = providerHasBackfilled || quota.isBackfilled;
            }

            if (providerHasUnknown) {
                unknownProviders.push_back(provider.displayName);
            }
            if (providerHasBackfilled) {
                backfilledProviders.push_back(provider.displayName);
            }

            if (!storageAvailable || !storageService.isOpen()) {
                continue;
            }

            const auto providerId = findProviderIdByKey(storageService, provider.providerKey);
            const auto accountId = ensureRuntimeTrackingAccountId(
                storageService,
                provider.providerKey,
                provider.displayName);
            if (!providerId.has_value() || !accountId.has_value()) {
                continue;
            }

            const auto checkpoint =
                storageService.getLatestRecoveryCheckpoint(*providerId, *accountId, kRuntimeBackfillSyncKind);
            if (!checkpoint.has_value()) {
                continue;
            }

            const std::time_t candidateAt = checkpoint->attemptAt;
            if (candidateAt <= latestOutcomeAt) {
                continue;
            }

            latestOutcomeAt = candidateAt;
            latestOutcome = checkpoint->outcome;
            latestComparisonResult = checkpoint->comparisonResult;
            latestFetchedEventCount = checkpoint->fetchedEventCount;
            latestLocalEventCount = checkpoint->localEventCount;
            latestLocalTotalTokens = checkpoint->localTotalTokens;
            latestImportedTotalTokens = checkpoint->importedTotalTokens;
            latestProviderDisplayName = provider.displayName;

            if (latestOutcome.empty()) {
                latestOutcome = checkpoint->failureCategory.empty() ? "no_data" : "fetch_failed";
            }
        }

        if (!unknownProviders.empty()) {
            viewModel.recoveryStatusSummary =
                "Unknown gaps in " + pluralize(
                    static_cast<long long>(unknownProviders.size()),
                    "provider",
                    "providers");
        } else if (!backfilledProviders.empty()) {
            viewModel.recoveryStatusSummary =
                "Backfilled windows in " + pluralize(
                    static_cast<long long>(backfilledProviders.size()),
                    "provider",
                    "providers");
        } else if (viewModel.runtimeState == RuntimeState::CatchingUp) {
            viewModel.recoveryStatusSummary = "Checking for missed history";
        } else if (latestOutcome == "imported" || latestOutcome == "no_data" || latestOutcome == "fetch_failed") {
            viewModel.recoveryStatusSummary = summarizeRecoveryOutcomeLabel(
                latestOutcome,
                latestComparisonResult,
                latestLocalEventCount,
                latestImportedTotalTokens);
        } else {
            viewModel.recoveryStatusSummary = "No recent recovery activity";
        }

        std::ostringstream detail;
        detail << "Current shared sync state: " << runtimeStateLabel(viewModel.runtimeState) << ".";
        if (!unknownProviders.empty()) {
            detail << " Unknown-gap windows are still active for ";
            for (size_t index = 0; index < unknownProviders.size(); ++index) {
                if (index > 0) {
                    detail << (index + 1 == unknownProviders.size() ? " and " : ", ");
                }
                detail << unknownProviders[index];
            }
            detail << ".";
        }
        if (!backfilledProviders.empty()) {
            detail << " Backfilled windows are currently visible for ";
            for (size_t index = 0; index < backfilledProviders.size(); ++index) {
                if (index > 0) {
                    detail << (index + 1 == backfilledProviders.size() ? " and " : ", ");
                }
                detail << backfilledProviders[index];
            }
            detail << ".";
        }
        if (!latestOutcome.empty()) {
            detail << " Last recovery outcome: "
                   << summarizeRecoveryOutcomeLabel(
                          latestOutcome,
                          latestComparisonResult,
                          latestLocalEventCount,
                          latestImportedTotalTokens);
            if (!latestProviderDisplayName.empty()) {
                detail << " for " << latestProviderDisplayName;
            }
            if (latestFetchedEventCount > 0) {
                detail << " (" << latestFetchedEventCount << " imported event";
                if (latestFetchedEventCount != 1) {
                    detail << "s";
                }
                detail << ")";
            }
            if (latestComparisonResult == "matched_local_evidence") {
                detail << ", matched " << latestLocalEventCount << " local event";
                if (latestLocalEventCount != 1) {
                    detail << "s";
                }
                if (latestImportedTotalTokens > 0) {
                    detail << " / " << latestImportedTotalTokens << " tokens";
                }
            } else if (latestComparisonResult == "mismatch") {
                detail << ", local evidence differs";
                if (latestLocalTotalTokens > 0 || latestImportedTotalTokens > 0) {
                    detail << " (" << latestLocalTotalTokens
                           << " local tokens vs " << latestImportedTotalTokens
                           << " imported tokens)";
                }
            } else if (latestComparisonResult == "no_local_evidence") {
                detail << ", no local quantified evidence was available for comparison";
            }
            if (latestOutcomeAt > 0) {
                detail << " at " << formatTimeUtc(latestOutcomeAt);
            }
            detail << ".";
        } else if (viewModel.runtimeState != RuntimeState::CatchingUp) {
            detail << " No runtime backfill attempt has been recorded yet.";
        }
        viewModel.recoveryStatusDetail = detail.str();
    }

    void applyDashboardOperationalStatus(DashboardViewModel& viewModel) {
        applyRecoveryStatus(viewModel);
        applyScheduledExportStatus(viewModel);
    }

    void applyExportSchedule(const DashboardPreferences& preferences) {
        std::lock_guard<std::mutex> exportLock(scheduledExportMutex);
        appliedExportSchedule = {preferences.scheduledExportEnabled, preferences.scheduledExportIntervalMinutes,
                                 preferences.scheduledExportPath};
    }

    DashboardPreferences appliedScheduledExportPreferences() const {
        auto preferences = currentDashboardPreferences();
        std::lock_guard<std::mutex> exportLock(scheduledExportMutex);
        preferences.scheduledExportEnabled = appliedExportSchedule.enabled;
        preferences.scheduledExportIntervalMinutes = appliedExportSchedule.intervalMinutes;
        preferences.scheduledExportPath = appliedExportSchedule.path;
        return preferences;
    }

    bool shouldRunScheduledExport(const DashboardPreferences& preferences) const {
        return preferences.scheduledExportEnabled && preferences.scheduledExportIntervalMinutes > 0;
    }

    bool isScheduledExportDue(const DashboardPreferences& preferences, std::time_t now) const {
        if (!shouldRunScheduledExport(preferences)) {
            return false;
        }

        const std::time_t intervalSeconds =
            static_cast<std::time_t>(preferences.scheduledExportIntervalMinutes) * 60;
        if (lastScheduledExportAt <= 0) {
            return true;
        }

        return now >= lastScheduledExportAt + intervalSeconds;
    }

    std::chrono::milliseconds scheduledExportWaitInterval(std::time_t now) const {
        const DashboardPreferences preferences = appliedScheduledExportPreferences();
        if (!shouldRunScheduledExport(preferences)) {
            return scheduledExportDormantPollInterval;
        }

        if (lastScheduledExportAt <= 0) {
            return std::chrono::milliseconds(0);
        }

        const std::time_t intervalSeconds =
            static_cast<std::time_t>(preferences.scheduledExportIntervalMinutes) * 60;
        const std::time_t dueAt = lastScheduledExportAt + intervalSeconds;
        if (dueAt <= now) {
            return std::chrono::milliseconds(0);
        }

        const auto waitSeconds = std::max<std::time_t>(1, dueAt - now);
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::seconds(waitSeconds));
    }

    std::chrono::milliseconds retentionWaitInterval(std::time_t now) const {
        if (!storageAvailable) {
            return retentionPollInterval;
        }

        if (lastRetentionRunAt <= 0) {
            return retentionPollInterval;
        }

        const std::time_t dueAt = lastRetentionRunAt + kRetentionIntervalSeconds;
        if (dueAt <= now) {
            return std::chrono::milliseconds(0);
        }

        const auto dueWait = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::seconds(std::max<std::time_t>(1, dueAt - now)));
#ifdef JISTATS_ENABLE_TEST_HOOKS
        const auto defaultRetentionWait =
            std::chrono::duration_cast<std::chrono::milliseconds>(kRetentionCheckInterval);
        if (headlessForTesting && retentionPollInterval < defaultRetentionWait) {
            return std::min(retentionPollInterval, dueWait);
        }
#endif
        return dueWait;
    }

    ReportRequest buildAnalysisScopedHtmlExportRequest(
        const DashboardPreferences& preferences,
        const std::string& outputPath,
        std::time_t now) const {
        const ResolvedAnalysisWindow analysisWindow = analysisQueryEngine.resolveWindow(preferences, now);

        ReportRequest request;
        request.format = ReportFormat::Html;
        request.outputPath = outputPath;
        request.dateRange = analysisWindow.reportDateRange;
        if (!preferences.analysisSelectedProviderId.empty()) {
            request.providerFilter.push_back(preferences.analysisSelectedProviderId);
        } else if (!preferences.lastSelectedProviderId.empty()) {
            request.providerFilter.push_back(preferences.lastSelectedProviderId);
        }
        if (!preferences.analysisSelectedAccountId.empty()) {
            request.accountFilter.push_back(preferences.analysisSelectedAccountId);
        } else if (!preferences.lastSelectedAccountId.empty()) {
            request.accountFilter.push_back(preferences.lastSelectedAccountId);
        }
        return request;
    }

    void persistExportRun(
        const std::string& runKind,
        const ReportRequest& request,
        const ReportResult& reportResult,
        std::time_t requestedAt,
        std::time_t completedAt) {
        if (!storageAvailable || !storageService.isOpen()) {
            return;
        }

        PersistedExportRun exportRun;
        exportRun.runKind = runKind;
        exportRun.format = request.format == ReportFormat::Html
            ? "html"
            : (request.format == ReportFormat::Csv ? "csv" : "json");
        exportRun.requestedAt = requestedAt;
        exportRun.completedAt = completedAt;
        exportRun.success = reportResult.success;
        exportRun.outputPath = reportResult.outputPath;
        exportRun.errorSummary = reportResult.errorMessage;
        exportRun.rangeStart = std::chrono::system_clock::to_time_t(request.dateRange.start);
        exportRun.rangeEnd = std::chrono::system_clock::to_time_t(request.dateRange.end);
        exportRun.providerFilterCsv = joinCsvValues(request.providerFilter);
        exportRun.accountFilterCsv = joinCsvValues(request.accountFilter);
        (void)storageService.insertExportRun(exportRun);
    }

    void runScheduledExportIfDue(std::time_t now) {
        const DashboardPreferences preferences = appliedScheduledExportPreferences();
        if (!shouldRunScheduledExport(preferences)) {
            return;
        }
        {
            std::lock_guard<std::mutex> exportLock(scheduledExportMutex);
            if (!isScheduledExportDue(preferences, now)) {
                return;
            }
        }

        const std::time_t requestedAt = now;
        ReportRequest request =
            buildAnalysisScopedHtmlExportRequest(preferences, "", requestedAt);

        const std::string originalDefaultOutputDir = reportingService.getDefaultOutputDirectory();
        if (!preferences.scheduledExportPath.empty()) {
            reportingService.setDefaultOutputDirectory(preferences.scheduledExportPath);
        }

        const ReportResult result = reportingService.generateReport(request);
        persistExportRun("scheduled_html", request, result, requestedAt, std::time(nullptr));

        if (!preferences.scheduledExportPath.empty()) {
            reportingService.setDefaultOutputDirectory(originalDefaultOutputDir);
        }

        if (!result.success) {
            JISTATS_LOG_WARN(
                LogCategory::General,
                "Scheduled HTML export failed: " + result.errorMessage);
            return;
        }

        {
            std::lock_guard<std::mutex> exportLock(scheduledExportMutex);
            lastScheduledExportAt = now;
            if (storageAvailable) {
                settingsService.setString(
                    preference_keys::SCHEDULED_EXPORT_LAST_RUN_UTC,
                    formatTimeUtcIso(now));
            }
        }

        if (!dashboardPresenter.getViewModel().appVersion.empty()) {
            refreshPersistedActivityPresenters(now);
        }
        JISTATS_LOG_INFO(
            LogCategory::General,
            "Scheduled HTML export wrote: " + result.outputPath);
    }

    DashboardHtmlExportResult handleManualHtmlExport(const std::string& outputPath) {
        DashboardHtmlExportResult result;

        const DashboardPreferences preferences = currentDashboardPreferences();
        const std::time_t requestedAt = std::time(nullptr);
        ReportRequest request =
            buildAnalysisScopedHtmlExportRequest(preferences, outputPath, requestedAt);

        const ReportResult reportResult = reportingService.generateReport(request);
        persistExportRun("manual_html", request, reportResult, requestedAt, std::time(nullptr));

        if (!reportResult.success) {
            result.message = reportResult.errorMessage.empty()
                ? "HTML export failed."
                : reportResult.errorMessage;
            JISTATS_LOG_WARN(
                LogCategory::General,
                "Manual HTML export failed: " + result.message);
            return result;
        }

        result.success = true;
        result.outputPath = reportResult.outputPath;
        result.message = "HTML report exported to " + reportResult.outputPath;
        JISTATS_LOG_INFO(
            LogCategory::General,
            "Manual HTML export wrote: " + reportResult.outputPath);
        return result;
    }

    void startScheduledExportWatcher() {
        joinScheduledExportThread();
        scheduledExportThread = std::thread([this]() {
            while (!shouldStopMiniMaxValidation()) {
                const std::time_t now = std::time(nullptr);
                runScheduledExportIfDue(now);

                if (waitForRefreshWatcherInterval(scheduledExportWaitInterval(now))) {
                    break;
                }
            }
        });
    }

    void loadRetentionStateFromStorage() {
        lastRetentionRunAt = 0;
        if (!storageAvailable) {
            return;
        }

        if (const auto value = settingsService.getString(preference_keys::RETENTION_LAST_RUN_UTC)) {
            if (const auto parsed = parseTimeUtcIso(*value)) {
                lastRetentionRunAt = *parsed;
            }
        }
    }

    bool isRetentionDue(std::time_t now) const {
        if (lastRetentionRunAt <= 0) {
            return true;
        }
        return now >= lastRetentionRunAt + kRetentionIntervalSeconds;
    }

    void runRetentionPassIfDue(std::time_t now) {
        if (!storageAvailable) {
            return;
        }
        if (!isRetentionDue(now)) {
            return;
        }

        constexpr std::time_t kSecondsPerDay = 24 * 3600;

        const std::time_t rawEventCutoff = now - (kRetentionRawEventDays * kSecondsPerDay);
        const std::time_t hourlyRollupCutoff = now - (kRetentionHourlyRollupDays * kSecondsPerDay);
        const std::time_t dailyRollupCutoff = now - (kRetentionDailyRollupDays * kSecondsPerDay);
        const std::time_t gapMarkerCutoff = now - (kRetentionGapMarkerDays * kSecondsPerDay);

        JISTATS_LOG_INFO(LogCategory::Db, "Retention pass starting");

        bool allSucceeded = true;
        if (!storageService.deleteUsageEventsOlderThan(rawEventCutoff)) {
            JISTATS_LOG_WARN(LogCategory::Db, "Retention: failed to delete old usage events");
            allSucceeded = false;
        }
        if (!storageService.deleteUsageRollupsOlderThan(
                UsageRollupResolution::Hourly, hourlyRollupCutoff)) {
            JISTATS_LOG_WARN(LogCategory::Db, "Retention: failed to delete old hourly rollups");
            allSucceeded = false;
        }
        if (!storageService.deleteUsageRollupsOlderThan(
                UsageRollupResolution::Daily, dailyRollupCutoff)) {
            JISTATS_LOG_WARN(LogCategory::Db, "Retention: failed to delete old daily rollups");
            allSucceeded = false;
        }
        if (!storageService.deleteGapMarkersOlderThan(gapMarkerCutoff)) {
            JISTATS_LOG_WARN(LogCategory::Db, "Retention: failed to delete old gap markers");
            allSucceeded = false;
        }

        if (allSucceeded) {
            lastRetentionRunAt = now;
            settingsService.setString(
                preference_keys::RETENTION_LAST_RUN_UTC,
                formatTimeUtcIso(now));
            JISTATS_LOG_INFO(LogCategory::Db, "Retention pass completed");
        }
    }

    void startRetentionWatcher() {
        joinRetentionThread();
        retentionThread = std::thread([this]() {
            while (!shouldStopMiniMaxValidation()) {
                const std::time_t now = std::time(nullptr);
                runRetentionPassIfDue(now);

                if (waitForRefreshWatcherInterval(retentionWaitInterval(now))) {
                    break;
                }
            }
        });
    }

    void updateMiniMaxCredentialProfile(
        const std::string& planKey,
        const MiniMaxSubscriptionCredentialProfile& profile) {
        std::lock_guard<std::mutex> lock(miniMaxValidationMutex);
        miniMaxCredentialProfiles[planKey] = profile;
    }

    std::optional<MiniMaxSubscriptionCredentialProfile> getMiniMaxCredentialProfile(
        const std::string& planKey) const {
        std::lock_guard<std::mutex> lock(miniMaxValidationMutex);
        const auto it = miniMaxCredentialProfiles.find(planKey);
        if (it == miniMaxCredentialProfiles.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    bool shouldStopMiniMaxValidation() const {
        std::lock_guard<std::mutex> lock(miniMaxValidationMutex);
        return shuttingDown;
    }

    void queueMiniMaxCredentialValidation(
        const std::string& planKey,
        const MiniMaxSubscriptionCredentialProfile& profile) {
        std::lock_guard<std::mutex> lock(miniMaxValidationMutex);
        miniMaxValidationThreads.emplace_back([this, planKey, profile]() {
#ifdef JISTATS_ENABLE_TEST_HOOKS
            if (miniMaxValidationReleaseForTesting.valid()) {
                miniMaxValidationReleaseForTesting.wait();
            }
#endif

            if (shouldStopMiniMaxValidation()) {
                return;
            }

            ensureMiniMaxCredentialAdapter();
            const auto adapter = resolveMiniMaxRuntimeAdapter();
            if (!adapter) {
                return;
            }

            if (miniMaxCredentialStore) {
                adapter->setCredentialStore(miniMaxCredentialStore);
            }

            adapter->setSubscriptionCredentialProfile(profile);
            adapter->validateCredentials();

            MiniMaxSubscriptionCredentialProfile validatedProfile =
                adapter->getSubscriptionCredentialProfile();
            validatedProfile.lastValidationError =
                normalizeMiniMaxValidationError(validatedProfile.lastValidationError);

            std::string persistError;
            if (!persistMiniMaxCredentialProfile(validatedProfile, persistError)) {
                validatedProfile.validationState = "invalid";
                validatedProfile.lastValidationError = persistError;
            }

            updateMiniMaxCredentialProfile(planKey, validatedProfile);
            syncActiveMiniMaxRuntimeProfile(planKey);

            if (!shouldStopMiniMaxValidation()) {
                refreshProviderRuntimeState(std::time(nullptr), true, true);
            }
        });
    }

    MiniMaxSubscriptionCredentialProfile buildMiniMaxProfileForPlan(
        const std::string& planKey,
        const std::string& accountDisplayName) const {
        MiniMaxSubscriptionCredentialProfile profile;
        profile.accountKey = minimaxAccountKey(planKey);
        profile.accountDisplayName = accountDisplayName.empty()
            ? minimaxDefaultAccountDisplayName(planKey)
            : accountDisplayName;
        profile.planKey = planKey;
        return profile;
    }

    void hydrateMiniMaxCredentialProfilesFromStorage() {
        {
            std::lock_guard<std::mutex> lock(miniMaxValidationMutex);
            miniMaxCredentialProfiles.clear();
        }
        ensureMiniMaxCredentialAdapter();

        if (!storageAvailable) {
            syncActiveMiniMaxRuntimeProfile();
            return;
        }

        const auto providerId = findProviderIdByKey(storageService, "minimax");
        if (!providerId.has_value()) {
            syncActiveMiniMaxRuntimeProfile();
            return;
        }

        for (const std::string& planKey : minimaxSupportedPlanKeys()) {
            const MiniMaxStoredAccountRecord account =
                loadMiniMaxStoredAccount(storageService, *providerId, planKey);
            if (!account.exists) {
                continue;
            }

            const auto persisted =
                storageService.getCredentialProfile(account.accountId, minimaxPlanDisplayName(planKey));
            if (!persisted.has_value()) {
                continue;
            }

            MiniMaxSubscriptionCredentialProfile profile = buildMiniMaxProfileForPlan(
                planKey,
                account.accountDisplayName);
            profile.profileName = persisted->profileName;
            profile.secretRef = persisted->secretRef;
            profile.validationState = "unknown";
            profile.lastValidatedAt = persisted->lastValidatedAt;
            profile.lastValidationError.clear();
            updateMiniMaxCredentialProfile(planKey, profile);
        }

        syncActiveMiniMaxRuntimeProfile();
    }

    std::vector<MiniMaxCredentialProfileSummary> buildMiniMaxCredentialSummaries() const {
        std::vector<MiniMaxCredentialProfileSummary> summaries;
        summaries.reserve(2);
        const std::map<std::string, MiniMaxSubscriptionCredentialProfile> profileSnapshot = [&]() {
            std::lock_guard<std::mutex> lock(miniMaxValidationMutex);
            return miniMaxCredentialProfiles;
        }();

        for (const std::string& planKey : minimaxSupportedPlanKeys()) {
            MiniMaxCredentialProfileSummary summary;
            summary.planKey = planKey;
            summary.planDisplayName = minimaxPlanDisplayName(planKey);
            summary.profileName = minimaxPlanDisplayName(planKey);
            summary.accountKey = minimaxAccountKey(planKey);
            summary.accountDisplayName = minimaxDefaultAccountDisplayName(planKey);

            const auto it = profileSnapshot.find(planKey);
            if (it != profileSnapshot.end()) {
                summary.profileName = it->second.profileName.empty()
                    ? summary.profileName
                    : it->second.profileName;
                summary.accountKey = it->second.accountKey.empty()
                    ? summary.accountKey
                    : it->second.accountKey;
                summary.accountDisplayName = it->second.accountDisplayName.empty()
                    ? summary.accountDisplayName
                    : it->second.accountDisplayName;
                summary.secretRef = it->second.secretRef;
                summary.validationState = it->second.validationState;
                summary.lastValidationError = it->second.lastValidationError;
                summary.lastValidatedAtLabel = lastValidatedLabel(it->second.lastValidatedAt);
                summary.hasPersistedProfile = !it->second.secretRef.empty();
            }

            summaries.push_back(summary);
        }

        return summaries;
    }

    void applyMiniMaxCredentialProfiles(DashboardViewModel& viewModel) const {
        viewModel.minimaxCredentialProfiles = buildMiniMaxCredentialSummaries();
    }

    bool persistMiniMaxCredentialProfile(const MiniMaxSubscriptionCredentialProfile& profile, std::string& error) {
        if (!storageAvailable) {
            error.clear();
            return true;
        }

        const auto providerId = findProviderIdByKey(storageService, "minimax");
        if (!providerId.has_value()) {
            error = "MiniMax provider row is missing from storage";
            return false;
        }

        MiniMaxStoredAccountRecord account =
            loadMiniMaxStoredAccount(storageService, *providerId, profile.planKey);
        const std::string desiredDisplayName = profile.accountDisplayName.empty()
            ? minimaxDefaultAccountDisplayName(profile.planKey)
            : profile.accountDisplayName;

        if (!account.exists) {
            QueryResult insert = storageService.execute(
                "INSERT INTO accounts (provider_id, account_key, display_name, scope_type, scope_identifier) "
                "VALUES (?, ?, ?, ?, ?);",
                {
                    std::to_string(*providerId),
                    minimaxAccountKey(profile.planKey),
                    desiredDisplayName,
                    "subscription",
                    minimaxAccountKey(profile.planKey)
                });
            if (!insert.success()) {
                error = storageService.getLastError();
                return false;
            }
            account.accountId = static_cast<int>(insert.lastInsertRowId());
        } else if (account.accountDisplayName != desiredDisplayName) {
            QueryResult update = storageService.execute(
                "UPDATE accounts SET display_name = ?, updated_at_utc = strftime('%Y-%m-%dT%H:%M:%SZ', 'now') "
                "WHERE id = ?;",
                {desiredDisplayName, std::to_string(account.accountId)});
            if (!update.success()) {
                error = storageService.getLastError();
                return false;
            }
        }

        PersistedCredentialProfile persisted;
        persisted.providerId = *providerId;
        persisted.accountId = account.accountId;
        persisted.profileName = profile.profileName.empty()
            ? minimaxPlanDisplayName(profile.planKey)
            : profile.profileName;
        persisted.secretRef = profile.secretRef;
        persisted.credentialType = profile.credentialType.empty()
            ? "subscription_key"
            : profile.credentialType;
        persisted.lastValidatedAt = profile.lastValidatedAt;

        if (!storageService.upsertCredentialProfile(persisted)) {
            error = storageService.getLastError();
            return false;
        }

        error.clear();
        return true;
    }

    DashboardMiniMaxCredentialResult handleMiniMaxCredentialAction(
        const DashboardMiniMaxCredentialRequest& request) {
        DashboardMiniMaxCredentialResult result;
        result.planKey = request.planKey;

        ensureMiniMaxCredentialAdapter();
        auto adapter = resolveMiniMaxRuntimeAdapter();
        if (!adapter) {
            result.message = "MiniMax runtime adapter is not available.";
            return result;
        }

        if (miniMaxCredentialStore) {
            adapter->setCredentialStore(miniMaxCredentialStore);
        }

        const std::string planKey = request.planKey == "token_plan" ? "token_plan" : "coding_plan";
        const auto storedProfile = getMiniMaxCredentialProfile(planKey);
        MiniMaxSubscriptionCredentialProfile profile = storedProfile.has_value()
            ? *storedProfile
            : buildMiniMaxProfileForPlan(planKey, request.accountDisplayName);

        if (!request.accountDisplayName.empty()) {
            profile.accountDisplayName = request.accountDisplayName;
        } else if (profile.accountDisplayName.empty()) {
            profile.accountDisplayName = minimaxDefaultAccountDisplayName(planKey);
        }

        if (request.action == DashboardMiniMaxCredentialAction::Save) {
            if (!adapter->storeSubscriptionCredential(profile, request.secret)) {
                MiniMaxSubscriptionCredentialProfile failed = adapter->getSubscriptionCredentialProfile();
                failed.lastValidationError = normalizeMiniMaxValidationError(failed.lastValidationError);
                updateMiniMaxCredentialProfile(planKey, failed);
                syncActiveMiniMaxRuntimeProfile(planKey);
                result.message = failed.lastValidationError.empty()
                    ? "MiniMax credential save failed."
                    : failed.lastValidationError;
                refreshProviderRuntimeState(std::time(nullptr), true, true);
                return result;
            }

            profile = adapter->getSubscriptionCredentialProfile();
            std::string persistError;
            if (!persistMiniMaxCredentialProfile(profile, persistError)) {
                profile.validationState = "invalid";
                profile.lastValidationError = persistError;
                updateMiniMaxCredentialProfile(planKey, profile);
                syncActiveMiniMaxRuntimeProfile(planKey);
                result.message = persistError;
                refreshProviderRuntimeState(std::time(nullptr), true, true);
                return result;
            }

            updateMiniMaxCredentialProfile(planKey, profile);
            syncActiveMiniMaxRuntimeProfile(planKey);
            result.success = true;
            result.accountDisplayName = profile.accountDisplayName;
            result.message = "Stored " + minimaxPlanDisplayName(planKey) + " credential.";
            refreshProviderRuntimeState(std::time(nullptr), true, true);
            return result;
        }

        if (!request.secret.empty()) {
            if (!adapter->storeSubscriptionCredential(profile, request.secret)) {
                MiniMaxSubscriptionCredentialProfile failed = adapter->getSubscriptionCredentialProfile();
                failed.lastValidationError = normalizeMiniMaxValidationError(failed.lastValidationError);
                updateMiniMaxCredentialProfile(planKey, failed);
                syncActiveMiniMaxRuntimeProfile(planKey);
                result.message = failed.lastValidationError.empty()
                    ? "MiniMax credential save failed."
                    : failed.lastValidationError;
                refreshProviderRuntimeState(std::time(nullptr), true, true);
                return result;
            }

            profile = adapter->getSubscriptionCredentialProfile();
            std::string persistError;
            if (!persistMiniMaxCredentialProfile(profile, persistError)) {
                profile.validationState = "invalid";
                profile.lastValidationError = persistError;
                updateMiniMaxCredentialProfile(planKey, profile);
                syncActiveMiniMaxRuntimeProfile(planKey);
                result.message = persistError;
                refreshProviderRuntimeState(std::time(nullptr), true, true);
                return result;
            }
        } else {
            if (!storedProfile.has_value()) {
                MiniMaxSubscriptionCredentialProfile missing = buildMiniMaxProfileForPlan(
                    planKey,
                    request.accountDisplayName);
                missing.validationState = "missing";
                missing.lastValidationError = "No MiniMax credential has been saved for this plan yet";
                updateMiniMaxCredentialProfile(planKey, missing);
                result.message = missing.lastValidationError;
                refreshProviderRuntimeState(std::time(nullptr), true, true);
                return result;
            }
            adapter->setSubscriptionCredentialProfile(profile);
        }

        profile.validationState = "pending";
        profile.lastValidationError.clear();
        updateMiniMaxCredentialProfile(planKey, profile);
        syncActiveMiniMaxRuntimeProfile(planKey);
        queueMiniMaxCredentialValidation(planKey, profile);
        result.success = true;
        result.accountDisplayName = profile.accountDisplayName;
        result.message = "Validating " + minimaxPlanDisplayName(planKey) + " credential...";
        refreshProviderRuntimeState(std::time(nullptr), true, true);
        return result;
    }

    DashboardOpenAIImportResult handleOpenAIImportAction(const std::string& inputPath) {
        DashboardOpenAIImportResult result;
        result.inputPath = inputPath;

        if (databasePath.empty()) {
            result.message = "OpenAI export import requires an available SQLite database.";
            return result;
        }

        StorageService importStorage;
        if (!importStorage.init(databasePath)) {
            result.message = importStorage.getLastError().empty()
                ? "OpenAI export import requires an available SQLite database."
                : importStorage.getLastError();
            return result;
        }
        importStorage.execute("PRAGMA busy_timeout = 5000;");

        const OpenAIExportImportRunResult run =
            OpenAIExportImport::importHistoricalActivity(importStorage, inputPath);
        importStorage.close();
        if (!run.success) {
            result.message = run.error.empty()
                ? "OpenAI export import failed."
                : run.error;
            return result;
        }

        result.success = true;
        result.insertedCount = run.insertedCount;

        std::ostringstream message;
        message << "OpenAI export import completed";
        if (run.insertedCount > 0) {
            message << " (" << run.insertedCount << " historical messages added)";
        } else {
            message << " (no new historical messages)";
        }
        result.message = message.str();
        return result;
    }

    void applyPersistedActivitySummaries(DashboardViewModel& viewModel, std::time_t currentTime) {
        const PersistedMetricSnapshot snapshot = ensureFreshPersistedMetricSnapshot(currentTime);
        if (!snapshot.dashboardSummaries.empty()) {
            viewModel.runtimeSummaries = snapshot.dashboardSummaries;
            return;
        }

        viewModel.runtimeSummaries = buildPersistedActivityDashboardSummaries(
            storageService,
            currentTime,
            viewModel.preferences);
    }

    std::vector<TaskbarMetrics> buildRuntimeTaskbarMetrics(
        std::time_t now,
        const DashboardViewModel& viewModel) {
        std::vector<TaskbarMetrics> metrics =
            buildSubscriptionQuotaTaskbarMetricsFromViewModel(now, viewModel);
        if (!metrics.empty()) {
            return metrics;
        }

#ifdef JISTATS_ENABLE_TEST_HOOKS
        metrics = buildSampleTaskbarMetrics(now);
#endif
        return metrics;
    }

    void applyOfflineNoticePresentation(DashboardViewModel& viewModel) {
        viewModel.offlineRetryAvailable = viewModel.runtimeState == RuntimeState::Offline;
        viewModel.offlineNoticeVisible = false;
        viewModel.offlineNoticeTitle.clear();
        viewModel.offlineNoticeMessage.clear();

        const auto activeNotice = notificationService.getActiveNotice(NoticeType::Offline);
        if (!activeNotice.has_value()) {
            return;
        }

        viewModel.offlineNoticeVisible = true;
        viewModel.offlineNoticeTitle = activeNotice->title;
        viewModel.offlineNoticeMessage = activeNotice->message;
    }

    std::time_t latestOfflineNoticeDismissal() {
        if (!storageAvailable || !storageService.isOpen()) {
            return 0;
        }

        const auto result = storageService.execute(
            "SELECT MAX(notice_dismissed_at_utc) FROM connectivity_state "
            "WHERE account_id IN (SELECT id FROM accounts WHERE scope_type = 'runtime');");
        return result.success() && result.rowCount() == 1
            ? parseRuntimeTimestamp(result.getString(0, 0))
            : 0;
    }

    void persistOfflineNoticeDismissal(const DashboardViewModel& viewModel, std::time_t now) {
        if (!storageAvailable || !storageService.isOpen()) {
            return;
        }

        for (const auto& provider : viewModel.providers) {
            if (provider.state != ProviderState::Offline) {
                continue;
            }

            const auto providerId = findProviderIdByKey(storageService, provider.providerKey);
            const auto runtimeAccountId =
                ensureRuntimeTrackingAccountId(storageService, provider.providerKey, provider.displayName);
            if (!providerId.has_value() || !runtimeAccountId.has_value()) {
                continue;
            }

            auto connectivity = storageService.getConnectivityState(*providerId, *runtimeAccountId);
            if (!connectivity.has_value() || connectivity->status != "offline") {
                continue;
            }

            connectivity->noticeDismissedAt = now;
            storageService.upsertConnectivityState(*connectivity);
        }
    }

    void publishDashboardWithOfflineNotice(DashboardViewModel& viewModel) {
        applyOfflineNoticePresentation(viewModel);
        dashboardPresenter.updateViewModel(viewModel);
        taskbarPresenter.setOfflineMenuState(
            viewModel.offlineRetryAvailable,
            viewModel.offlineNoticeVisible);
    }

    DashboardNoticeActionResult handleOfflineRetryAction() {
        refreshProviderRuntimeState(std::time(nullptr), true, true);

        DashboardNoticeActionResult result;
        const DashboardViewModel refreshed = dashboardPresenter.getViewModel();
        result.success = true;
        if (refreshed.runtimeState == RuntimeState::Offline) {
            result.message = "Retry attempted. Provider connectivity is still unavailable.";
        } else if (refreshed.runtimeState == RuntimeState::CatchingUp) {
            result.message = "Retry succeeded. JiStats is catching up on missed history.";
        } else {
            result.message = "Retry succeeded. Provider connectivity has recovered.";
        }
        return result;
    }

    DashboardNoticeActionResult handleOfflineDismissAction() {
        std::lock_guard<std::mutex> noticeLock(offlineNoticeMutex);
        DashboardViewModel viewModel = dashboardPresenter.getViewModel();
        DashboardNoticeActionResult result;
        if (viewModel.runtimeState != RuntimeState::Offline) {
            result.success = false;
            result.message = "Offline notice is unavailable.";
            return result;
        }

        const bool hadActiveNotice = notificationService.hasActiveNotice(NoticeType::Offline);
        if (hadActiveNotice) {
            notificationService.dismissNotice(NoticeType::Offline);
        } else {
            notificationService.suppressUntilCleared(NoticeType::Offline);
        }

        persistOfflineNoticeDismissal(viewModel, std::time(nullptr));
        publishDashboardWithOfflineNotice(viewModel);
        result.success = true;
        result.message = "Offline notice dismissed. JiStats will keep the offline indicator visible and retry automatically.";
        return result;
    }

    void syncTrayRuntimeIndicator(const DashboardViewModel& viewModel) {
        const PersistedMetricSnapshot snapshot = copyPersistedMetricSnapshot();
        if (trayController.isAvailable()) {
            std::string tooltip = buildTrayTooltip(viewModel);
            if (!snapshot.trayTooltipSuffix.empty()) {
                tooltip += snapshot.trayTooltipSuffix;
            }
            trayController.setState(trayStatusForRuntimeState(viewModel.runtimeState), tooltip);
        }
    }

    void updateOfflineNotice(const DashboardViewModel& viewModel, std::time_t now) {
        const RuntimeStatusTransition transition =
            describeRuntimeStateTransition(lastRuntimeState, viewModel.runtimeState);
        if (transition.enteredOffline) {
            const std::time_t dismissedAt = latestOfflineNoticeDismissal();
            if (dismissedAt > 0 && (now - dismissedAt) <= kOfflineNoticeCooldownSeconds) {
                notificationService.suppressUntilCleared(NoticeType::Offline);
            }
            notificationService.showNotice(buildOfflineNotice(viewModel, now));
        } else if (transition.exitedOffline) {
            notificationService.clearNotice(NoticeType::Offline);
        } else if (viewModel.runtimeState == RuntimeState::Offline &&
                   lastRuntimeState == RuntimeState::Offline) {
            notificationService.suppressUntilCleared(NoticeType::Offline);
        }

        lastRuntimeState = viewModel.runtimeState;
    }

    std::optional<PersistedRuntimeState> loadPersistedRuntimeState() {
        if (!storageAvailable || !storageService.isOpen()) {
            return std::nullopt;
        }

        return storageService.getRuntimeState();
    }

    void markLaunchRecoveryState(std::time_t now) {
        if (!storageAvailable || !storageService.isOpen()) {
            return;
        }

        PersistedRuntimeState runtimeState;
        if (const auto existing = loadPersistedRuntimeState()) {
            runtimeState = *existing;
        }

        const std::time_t baseline = std::max(runtimeState.lastLaunchAt, runtimeState.lastSuccessfulSyncAt);
        if (baseline > 0 &&
            runtimeState.recoveryMarker.empty() &&
            (now - baseline) > kRecoveryGapThresholdSeconds) {
            runtimeState.recoveryMarker = buildRecoveryMarker(baseline);
        }

        runtimeState.lastLaunchAt = now;
        const std::time_t latestObservationAt = queryLatestLocalObservationAt(storageService);
        if (latestObservationAt > 0) {
            runtimeState.lastLocalObservationAt = latestObservationAt;
        }
        storageService.upsertRuntimeState(runtimeState);
    }

    bool hasPendingRecovery() {
        const auto runtimeState = loadPersistedRuntimeState();
        return runtimeState.has_value() && !runtimeState->recoveryMarker.empty();
    }

    void promotePendingRecoveryState(DashboardViewModel& viewModel) {
        bool promotedAnyProvider = false;
        for (auto& provider : viewModel.providers) {
            if (!provider.isEnabled) {
                continue;
            }

            if (provider.state == ProviderState::Live || provider.state == ProviderState::Stale) {
                provider.state = ProviderState::Syncing;
                promotedAnyProvider = true;
            }
        }

        if (promotedAnyProvider || viewModel.runtimeState != RuntimeState::Offline) {
            applyRuntimeStateToViewModel(viewModel);
        }
    }

    void overlayPendingRecoveryState(DashboardViewModel& viewModel) {
        if (!hasPendingRecovery()) {
            return;
        }

        promotePendingRecoveryState(viewModel);
    }

    void applyGapAnnotations(DashboardViewModel& viewModel, std::time_t now) {
        if (!storageAvailable || !storageService.isOpen()) {
            return;
        }

        const std::time_t windowStart = std::max<std::time_t>(0, now - kGapAnnotationLookbackSeconds);
        for (auto& provider : viewModel.providers) {
            const auto providerId = findProviderIdByKey(storageService, provider.providerKey);
            const auto runtimeAccountId =
                ensureRuntimeTrackingAccountId(storageService, provider.providerKey, provider.displayName);
            if (!providerId.has_value() || !runtimeAccountId.has_value()) {
                continue;
            }

            const auto markers = storageService.getGapMarkers(
                *providerId,
                *runtimeAccountId,
                windowStart,
                now);

            bool hasUnknown = false;
            bool hasBackfilled = false;
            for (const auto& marker : markers) {
                hasUnknown = hasUnknown || marker.classification == GapClassification::Unknown;
                hasBackfilled =
                    hasBackfilled || marker.classification == GapClassification::Reconstructed;
            }

            for (auto& quota : provider.quotas) {
                quota.hasUnknownGaps = hasUnknown;
                quota.isBackfilled = hasBackfilled;
            }
        }
    }

    ProviderConnectivityUpdates persistProviderRefreshState(
        const DashboardViewModel& viewModel,
        std::time_t now,
        const QuotaRefresh& refresh) {
        ProviderConnectivityUpdates connectivityUpdates;
        if (!storageAvailable || !storageService.isOpen()) {
            return connectivityUpdates;
        }

        connectivityUpdates.anyOfflineProvider =
            std::any_of(viewModel.providers.begin(), viewModel.providers.end(), [](const auto& provider) {
                return provider.state == ProviderState::Offline;
            });
        // Only quota fetched from a provider API in this refresh confirms that
        // connectivity returned. Local reads and cached responses do not.
        connectivityUpdates.providerApiConfirmedRecovery =
            !connectivityUpdates.anyOfflineProvider &&
            std::any_of(viewModel.providers.begin(), viewModel.providers.end(), [&refresh](const auto& provider) {
                return (provider.state == ProviderState::Live || provider.state == ProviderState::Stale) &&
                    refresh.providerApiKeys.count(provider.providerKey) != 0 &&
                    !provider.quotas.empty();
            });

        PersistedRuntimeState runtimeState;
        if (const auto existing = loadPersistedRuntimeState()) {
            runtimeState = *existing;
        }

        const std::time_t latestObservationAt = queryLatestLocalObservationAt(storageService);
        if (latestObservationAt > 0) {
            runtimeState.lastLocalObservationAt = latestObservationAt;
        }

        bool anyOnlineProvider = false;
        bool anyOfflineProvider = false;
        for (const auto& provider : viewModel.providers) {
            if (!provider.contributesToRuntimeState) {
                continue;
            }
            const auto providerId = findProviderIdByKey(storageService, provider.providerKey);
            const auto runtimeAccountId =
                ensureRuntimeTrackingAccountId(storageService, provider.providerKey, provider.displayName);
            if (!providerId.has_value() || !runtimeAccountId.has_value()) {
                continue;
            }

            StagedConnectivity staged;
            PersistedConnectivityState& connectivity = staged.connectivity;
            connectivity.providerId = *providerId;
            connectivity.accountId = *runtimeAccountId;
            connectivity.lastCheckedAt = now;

            PersistedSyncState syncState;
            syncState.providerId = *providerId;
            syncState.accountId = *runtimeAccountId;
            syncState.syncKind = kRuntimeRefreshSyncKind;
            syncState.lastAttemptAt = now;

            if (provider.state == ProviderState::Offline) {
                anyOfflineProvider = true;
                connectivity.status = "offline";
                connectivity.failureCategory = "provider_unreachable";
                syncState.lastFailureAt = now;
                syncState.lastFailureCategory = connectivity.failureCategory;

                if (runtimeState.recoveryMarker.empty()) {
                    runtimeState.recoveryMarker = buildRecoveryMarker(now);
                }
            } else {
                anyOnlineProvider = true;
                connectivity.status =
                    provider.state == ProviderState::Syncing ? "catching_up" :
                    (provider.state == ProviderState::Stale ? "stale" : "online");
                connectivity.failureCategory.clear();
                connectivity.nextRetryAt = 0;
                staged.fetchedRecovery =
                    (provider.state == ProviderState::Live || provider.state == ProviderState::Stale) &&
                    refresh.fetchedProviderKeys.count(provider.providerKey) != 0;

                syncState.lastSuccessAt = now;
                syncState.lastFailureCategory.clear();
            }

            connectivityUpdates.rows.push_back(std::move(staged));
            storageService.upsertSyncState(syncState);
        }

        if (anyOnlineProvider && runtimeState.recoveryMarker.empty()) {
            runtimeState.lastSuccessfulSyncAt = now;
        }
        if (!anyOfflineProvider &&
            viewModel.runtimeState != RuntimeState::Offline &&
            runtimeState.recoveryMarker.empty()) {
            runtimeState.lastSuccessfulSyncAt = now;
        }

        storageService.upsertRuntimeState(runtimeState);
        return connectivityUpdates;
    }

    void persistConnectivityForPublication(
        ProviderConnectivityUpdates& updates,
        std::time_t now) {
        if (!storageAvailable || !storageService.isOpen()) {
            return;
        }

        // The newest saved dismissal belongs to the app-wide episode, even
        // when a refresh omitted the provider that originally stored it.
        const bool hasOfflineRows = std::any_of(updates.rows.begin(), updates.rows.end(), [](const auto& row) {
            return row.connectivity.status == "offline";
        });
        const std::time_t carriedDismissal = hasOfflineRows ? latestOfflineNoticeDismissal() : 0;
        // Save offline rows first so a carried dismissal is stored before the
        // row that held it is overwritten.
        bool offlineRowsSaved = true;
        for (auto& row : updates.rows) {
            PersistedConnectivityState& connectivity = row.connectivity;
            if (connectivity.status != "offline") {
                continue;
            }
            const auto existing = storageService.getConnectivityState(
                connectivity.providerId, connectivity.accountId);
            const bool wasOffline = existing.has_value() && existing->status == "offline";
            connectivity.nextRetryAt =
                now + (wasOffline ? kRetryDelayEscalatedSeconds : kRetryDelaySeconds);
            connectivity.noticeDismissedAt = carriedDismissal;
            if (!storageService.upsertConnectivityState(connectivity)) {
                offlineRowsSaved = false;
            }
        }
        if (!offlineRowsSaved) {
            JISTATS_LOG_WARN(LogCategory::Db, "Failed to save offline connectivity; keeping the saved notice dismissal");
        }

        const bool preserveExistingDismissals =
            (!updates.providerApiConfirmedRecovery && !hasOfflineRows) || !offlineRowsSaved;
        for (auto& row : updates.rows) {
            PersistedConnectivityState& connectivity = row.connectivity;
            if (connectivity.status == "offline") {
                continue;
            }
            if (preserveExistingDismissals) {
                const auto existing = storageService.getConnectivityState(
                    connectivity.providerId, connectivity.accountId);
                if (existing.has_value()) {
                    // A local result can end only its own provider's outage.
                    // Omitted providers keep the dismissal until they recover.
                    const bool recoveredOwnOutage = !updates.anyOfflineProvider &&
                        row.fetchedRecovery && existing->status == "offline";
                    connectivity.noticeDismissedAt = recoveredOwnOutage ? 0 : existing->noticeDismissedAt;
                }
            }
            storageService.upsertConnectivityState(connectivity);
        }

        // Provider API evidence ends the saved episode, including for omitted providers.
        if (updates.providerApiConfirmedRecovery) {
            const auto result = storageService.execute(
                "UPDATE connectivity_state SET notice_dismissed_at_utc = NULL, "
                "updated_at_utc = strftime('%Y-%m-%dT%H:%M:%SZ', 'now') "
                "WHERE notice_dismissed_at_utc IS NOT NULL AND account_id IN ("
                "SELECT id FROM accounts WHERE scope_type = 'runtime');");
            if (!result.success()) {
                JISTATS_LOG_WARN(LogCategory::Db, "Failed to clear the saved offline notice dismissal");
            }
        }
    }

    bool executePendingCatchUp(
        std::time_t now,
        const DashboardViewModel& viewModel,
        ProviderManager& activeProviderManager) {
        if (!storageAvailable || !storageService.isOpen()) {
            return false;
        }

        auto runtimeState = loadPersistedRuntimeState();
        if (!runtimeState.has_value() || runtimeState->recoveryMarker.empty()) {
            return false;
        }

        const std::time_t recoveryStart = parseRecoveryMarkerStart(runtimeState->recoveryMarker);
        if (recoveryStart <= 0 || recoveryStart >= now) {
            runtimeState->recoveryMarker.clear();
            runtimeState->lastSuccessfulSyncAt = now;
            storageService.upsertRuntimeState(*runtimeState);
            return false;
        }

        std::map<int, std::string> recoveryGapBasisByProviderId;

        // Execute provider-side backfill first to populate usage_events before rollup rebuild
        for (const auto& provider : viewModel.providers) {
            const auto adapter = activeProviderManager.getProvider(provider.providerKey);
            if (!adapter || !adapter->isEnabled() || !adapter->supportsUsageHistory()) {
                continue;
            }

            const auto providerId = findProviderIdByKey(storageService, provider.providerKey);
            const auto runtimeAccountId =
                ensureRuntimeTrackingAccountId(storageService, provider.providerKey, provider.displayName);
            if (!providerId.has_value() || !runtimeAccountId.has_value()) {
                continue;
            }

            if (exitRequested.load()) {
                return false;
            }
            if (hasProviderOptOutInProgress(provider.providerKey)) {
                continue;
            }

            // Attempt provider-side backfill for this provider's gap window
            UsageResponse usageResp = adapter->fetchUsage(recoveryStart, now);
            if (exitRequested.load()) {
                return false;
            }
            std::string backfillOutcome = usageResp.success ? "no_data" : "fetch_failed";
            std::string backfillFailureCategory;
            std::size_t fetchedEventCount = usageResp.events.size();
            RecoveryBackfillComparisonSummary comparisonSummary;
            if (usageResp.success && !usageResp.events.empty()) {
                std::vector<PersistedUsageEvent> backfillEvents;
                for (const auto& evt : usageResp.events) {
                    PersistedUsageEvent persisted;
                    persisted.providerId = *providerId;
                    persisted.accountId = *runtimeAccountId;
                    persisted.eventTime = evt.eventTime;
                    persisted.sourceType = "provider_backfill";
                    persisted.sourceRunId = evt.sourceRunId;
                    persisted.modelKey = evt.modelKey;
                    persisted.requestType = evt.requestType;
                    persisted.inputTokens = evt.inputTokens;
                    persisted.outputTokens = evt.outputTokens;
                    persisted.totalTokens = evt.totalTokens;
                    persisted.costMinorUnits = evt.costMinorUnits;
                    persisted.isBackfilled = true;
                    persisted.rawReference = evt.rawReference;
                    backfillEvents.push_back(persisted);
                }

                if (!backfillEvents.empty()) {
                    const auto comparison =
                        summarizeRecoveryBackfillAgainstLocalEvidence(
                            storageService,
                            *providerId,
                            *runtimeAccountId,
                            recoveryStart,
                            now,
                            backfillEvents);
                    if (!comparison.has_value()) {
                        JISTATS_LOG_WARN(
                            LogCategory::Reconciliation,
                            "Failed to compare provider backfill against local evidence for " +
                                provider.providerKey + ": " + storageService.getLastError());
                        return false;
                    }
                    comparisonSummary = *comparison;

                    if (!storageService.insertUsageEvents(backfillEvents)) {
                        persistRecoveryBackfillSyncState(
                            storageService,
                            *providerId,
                            *runtimeAccountId,
                            recoveryStart,
                            now,
                            now,
                            "persist_failed",
                            fetchedEventCount,
                            "provider_backfill_persist_failed",
                            comparisonSummary);
                        JISTATS_LOG_WARN(
                            LogCategory::Reconciliation,
                            "Failed to persist provider backfill for " + provider.providerKey +
                                ": " + storageService.getLastError());
                        return false;
                    }
                    backfillOutcome = "imported";
                    recoveryGapBasisByProviderId[*providerId] =
                        buildRecoveryGapBasisSummary(backfillOutcome, comparisonSummary);
                    {
                        std::ostringstream oss;
                        oss << "Backfilled " << backfillEvents.size() << " usage events for provider " << provider.providerKey << " during recovery window";
                        JISTATS_LOG_INFO(LogCategory::Reconciliation, oss.str());
                    }
                }
            } else if (usageResp.success) {
                const auto comparison =
                    summarizeRecoveryBackfillAgainstLocalEvidence(
                        storageService,
                        *providerId,
                        *runtimeAccountId,
                        recoveryStart,
                        now,
                        {});
                if (!comparison.has_value()) {
                    JISTATS_LOG_WARN(
                        LogCategory::Reconciliation,
                        "Failed to compare no-data recovery window against local evidence for " +
                            provider.providerKey + ": " + storageService.getLastError());
                    return false;
                }
                comparisonSummary = *comparison;
                recoveryGapBasisByProviderId[*providerId] =
                    buildRecoveryGapBasisSummary(backfillOutcome, comparisonSummary);
            } else if (!usageResp.success) {
                backfillFailureCategory = "provider_backfill_fetch_failed";
                recoveryGapBasisByProviderId[*providerId] =
                    buildRecoveryGapBasisSummary(backfillOutcome, comparisonSummary);
                JISTATS_LOG_WARN(
                    LogCategory::Reconciliation,
                    "Provider backfill failed for " + provider.providerKey +
                        ": " + (usageResp.error.empty() ? std::string("unknown error") : usageResp.error));
            }

            if (!persistRecoveryBackfillSyncState(
                    storageService,
                    *providerId,
                    *runtimeAccountId,
                    recoveryStart,
                    now,
                    now,
                    backfillOutcome,
                    fetchedEventCount,
                    backfillFailureCategory,
                    comparisonSummary)) {
                JISTATS_LOG_WARN(
                    LogCategory::Reconciliation,
                    "Failed to persist recovery backfill sync state for " + provider.providerKey +
                        ": " + storageService.getLastError());
                return false;
            }
            {
                std::ostringstream oss;
                oss << "Recorded recovery backfill outcome for provider " << provider.providerKey
                    << ": outcome=" << backfillOutcome
                    << ", fetched_event_count=" << fetchedEventCount
                    << ", comparison_result=" << comparisonSummary.comparisonResult;
                if (!backfillFailureCategory.empty()) {
                    oss << ", failure_category=" << backfillFailureCategory;
                }
                JISTATS_LOG_INFO(LogCategory::Reconciliation, oss.str());
            }
        }

        // Rebuild usage rollups for the recovery range
        if (!rebuildUsageRollupsForRange(storageService, recoveryStart, now) || exitRequested.load()) {
            return false;
        }

        // Once gap writes start, finish the recovery marker update even if Exit arrives.
        for (const auto& provider : viewModel.providers) {
            const auto providerId = findProviderIdByKey(storageService, provider.providerKey);
            const auto runtimeAccountId =
                ensureRuntimeTrackingAccountId(storageService, provider.providerKey, provider.displayName);
            if (!providerId.has_value() || !runtimeAccountId.has_value()) {
                continue;
            }

            GapClassificationInputs inputs;
            inputs.providerBackfillAvailable =
                providerHasBackfilledUsage(storageService, *providerId, recoveryStart, now);

            PersistedGapMarker marker;
            marker.providerId = *providerId;
            marker.accountId = *runtimeAccountId;
            marker.periodStart = recoveryStart;
            marker.periodEnd = now;
            marker.classification = classifyGap(inputs);
            const auto basisIt = recoveryGapBasisByProviderId.find(*providerId);
            marker.basisSummary = basisIt != recoveryGapBasisByProviderId.end()
                ? basisIt->second
                : (marker.classification == GapClassification::Reconstructed
                    ? "Recovered usage from provider-imported backfill"
                    : "JiStats was not fully observing provider state during the recovery window");
            marker.sourceKind =
                marker.classification == GapClassification::Reconstructed
                    ? "provider_backfill"
                    : "runtime_recovery";
            storageService.upsertGapMarker(marker);
        }

        runtimeState->recoveryMarker.clear();
        runtimeState->lastSuccessfulSyncAt = now;
        storageService.upsertRuntimeState(*runtimeState);
        markPersistedMetricSnapshotDirty(false);
        return true;
    }

    bool applyRecoveryPersistenceCycle(
        DashboardViewModel& viewModel,
        const DashboardPreferences& preferences,
        std::time_t now,
        QuotaRefresh& refresh,
        ProviderConnectivityUpdates& connectivityUpdates) {
        bool executedCatchUp = false;
        if (viewModel.runtimeState != RuntimeState::Offline && hasPendingRecovery()) {
            executedCatchUp = executePendingCatchUp(now, viewModel, activeProviderManager());
            if (executedCatchUp) {
                viewModel = buildDashboardViewModel(now, refresh);
                viewModel.preferences = preferences;
            }
        }

        // Catch-up can replace the quota result used for the notice.
        connectivityUpdates = persistProviderRefreshState(viewModel, now, refresh);
        applyGapAnnotations(viewModel, now);
        if (viewModel.runtimeState != RuntimeState::Offline && hasPendingRecovery()) {
            promotePendingRecoveryState(viewModel);
        }

        return executedCatchUp;
    }

    void applyRuntimeViewModel(
        const DashboardViewModel& viewModel,
        std::time_t now,
        std::optional<ProviderConnectivityUpdates> connectivityUpdates = std::nullopt) {
        syncTrayRuntimeIndicator(viewModel);

        DashboardViewModel presentedViewModel = viewModel;
        {
            std::lock_guard<std::mutex> noticeLock(offlineNoticeMutex);
            // Display-only publications have no live connectivity result.
            if (connectivityUpdates.has_value()) {
                persistConnectivityForPublication(*connectivityUpdates, now);
            }
            presentedViewModel.completedPreferencesRequest = completedPreferencesRequest.load();
            presentedViewModel.failedPreferencesRequest = failedPreferencesRequest.load();
            presentedViewModel.preferencesSessionOnly = !storageAvailable;
            presentedViewModel.completedCodexAttributionRequest = completedCodexAttributionRequest.load();
            presentedViewModel.preferences.codexModelAttributionEnabled = codexModelAttributionEnabled.load();
            presentedViewModel.codexAttributionSaveError = codexAttributionSaveError.load();
            updateOfflineNotice(presentedViewModel, now);
            publishDashboardWithOfflineNotice(presentedViewModel);
        }

        if (viewModel.preferences.taskbarOverlayEnabled) {
#ifdef JISTATS_ENABLE_TEST_HOOKS
            if (!disableTaskbarForTesting)
#endif
            {
                if (taskbarPresenter.isInitialized()) {
                    taskbarPresenter.replaceMetrics(buildRuntimeTaskbarMetrics(now, viewModel));
                }
            }
        }
    }

    bool openCodexAttributionStorage() {
        if (codexAttributionStorage.isOpen()) {
            return true;
        }
        if (storageAvailable && codexAttributionStorage.init(storageService.getDbPath())) {
            return true;
        }
        codexAttributionStorage.close();
        return false;
    }

    int readCodexAttributionGeneration() {
        const int generation =
            openCodexAttributionStorage()
                ? codexAttributionSettings.getInt(preference_keys::CODEX_MODEL_ATTRIBUTION_GENERATION).value_or(0)
                : 0;
        codexModelAttributionEnabled.store(generation > 0);
        return generation;
    }

    bool saveCodexAttributionGeneration(bool enabled) {
        if (!openCodexAttributionStorage()) {
            return false;
        }
        const CodexProfileLock profileLock(codexAttributionStorage.getDbPath(), 5000);
        if (!profileLock.ownsLock()) {
            return false;
        }
        // Update from the current profile value so another instance cannot make us reuse a grant.
        // Positive numbers grant consent. Negating a number revokes it while preserving its identity.
        const auto saved = codexAttributionStorage.execute(
            "INSERT INTO settings (key, value, updated_at_utc) VALUES (?, ?, strftime('%Y-%m-%dT%H:%M:%SZ', 'now')) "
            "ON CONFLICT(key) DO UPDATE SET value = CASE "
            "WHEN excluded.value = '1' AND CAST(settings.value AS INTEGER) <= 0 "
            "THEN 1 - CAST(settings.value AS INTEGER) "
            "WHEN excluded.value = '0' AND CAST(settings.value AS INTEGER) > 0 "
            "THEN -CAST(settings.value AS INTEGER) ELSE settings.value END, "
            "updated_at_utc = excluded.updated_at_utc "
            "WHERE excluded.value = '0' OR CAST(settings.value AS INTEGER) >= ?;",
            {preference_keys::CODEX_MODEL_ATTRIBUTION_GENERATION, enabled ? "1" : "0",
             std::to_string(1 - std::numeric_limits<int>::max())});
        if (!saved.success() || saved.rowsAffected() != 1) {
            return false;
        }
        codexModelAttributionEnabled.store(enabled);
        return true;
    }

    void applyCodexModelAttributionPreference(CodexAttributionChange change) {
        std::lock_guard<std::mutex> lock(codexModelAttributionMutex);
        if (change.revocations != requestedCodexRevocations.load()) {
            return;
        }

        if (change.revocations != savedCodexRevocations.load()) {
            if (!saveCodexAttributionGeneration(false)) {
                codexAttributionSaveError.store(CodexAttributionSaveError::Revocation);
                JISTATS_LOG_WARN(LogCategory::Db, "Failed to save Codex model attribution revocation");
                return;
            }
            savedCodexRevocations.store(change.revocations);
        }

        if (change.enabled && change.revocations == requestedCodexRevocations.load() &&
            !saveCodexAttributionGeneration(true)) {
            codexAttributionSaveError.store(CodexAttributionSaveError::Enable);
            JISTATS_LOG_WARN(LogCategory::Db, "Failed to save Codex model attribution consent");
            return;
        }
        codexAttributionSaveError.store(CodexAttributionSaveError::None);
    }

    bool hasPendingCodexRevocation() const {
        return requestedCodexRevocations.load() != savedCodexRevocations.load();
    }

    void refreshCodexLocalSqliteUsage(std::time_t now) {
        std::lock_guard<std::mutex> lock(codexModelAttributionMutex);
        if (hasPendingCodexRevocation()) {
            return;
        }
        // A launch in the same profile may have changed consent since the previous refresh.
        const int generation = readCodexAttributionGeneration();
        if (generation <= 0 || hasPendingCodexRevocation()) {
            return;
        }

        const CodexProfileLock profileLock(codexAttributionStorage.getDbPath(), 0);
        if (!profileLock.ownsLock() || readCodexAttributionGeneration() != generation || hasPendingCodexRevocation()) {
            return;
        }

        const std::string sourcePath = CodexLocalSqliteReader::resolveDefaultStateDbPath();
        if (sourcePath.empty()) {
            return;
        }

        std::filesystem::path path(sourcePath);
        const std::string fingerprint = codexLocalSqliteFingerprint(path);
        if (fingerprint.empty()) {
            return;
        }

        if (generation == lastCodexLocalSqliteGeneration && path == lastCodexLocalSqlitePath &&
            fingerprint == lastCodexLocalSqliteFingerprint) {
            return;
        }

        if (hasPendingCodexRevocation()) {
            return;
        }
#ifdef JISTATS_ENABLE_TEST_HOOKS
        if (beforeCodexSourceReadForTesting) {
            beforeCodexSourceReadForTesting();
        }
#endif
        CodexLocalUsageImporter importer(codexAttributionStorage);
        auto result = importer.importFromSource(sourcePath, now, generation);
        if (result.success) {
            lastCodexLocalSqlitePath = path;
            lastCodexLocalSqliteFingerprint = fingerprint;
            lastCodexLocalSqliteGeneration = generation;
        }
    }

    std::string codexLocalSqliteFingerprint(const std::filesystem::path& path) const {
        std::error_code ec;
        const uintmax_t size = std::filesystem::file_size(path, ec);
        if (ec) {
            return {};
        }
        const auto writeTime = std::filesystem::last_write_time(path, ec);
        if (ec) {
            return {};
        }

        std::ostringstream fingerprint;
        fingerprint << path.string()
            << '|' << size
            << '|' << writeTime.time_since_epoch().count();

        for (const char* suffix : {"-wal", "-shm"}) {
            const std::filesystem::path sidecar(path.string() + suffix);
            ec.clear();
            if (!std::filesystem::exists(sidecar, ec) || ec) {
                fingerprint << '|' << suffix << ":missing";
                continue;
            }

            ec.clear();
            const uintmax_t sidecarSize = std::filesystem::file_size(sidecar, ec);
            if (ec) {
                fingerprint << '|' << suffix << ":unreadable";
                continue;
            }

            ec.clear();
            const auto sidecarWriteTime = std::filesystem::last_write_time(sidecar, ec);
            if (ec) {
                fingerprint << '|' << suffix << ":unreadable";
                continue;
            }

            fingerprint << '|' << suffix
                << ':' << sidecarSize
                << ':' << sidecarWriteTime.time_since_epoch().count();
        }

        return fingerprint.str();
    }

    void applyAnalysisSurface(DashboardViewModel& viewModel, std::time_t now) {
        refreshCodexLocalSqliteUsage(now);
        viewModel.analysis = analysisQueryEngine.buildViewModel(viewModel, viewModel.preferences, now);
    }

    bool shouldRefreshAnalysisSurface() const {
        return dashboardPresenter.isVisible() || dashboardPresenter.isAnalysisWindowOpen();
    }

    void applyProviderDiscoveryStatusToViewModel(DashboardViewModel& viewModel) {
        viewModel.claudeCodeLocallyDetected = claudeCodeDetected;
        viewModel.claudeCodeDetectionMessage = claudeCodeDetectionMessage;
        viewModel.kimiCodeLocallyDetected = kimiCodeDetected;
        viewModel.kimiCodeDetectionMessage = kimiCodeDetectionMessage;

        for (auto& provider : viewModel.providers) {
            if (provider.providerKey == "claude_code") {
                provider.locallyDetected = claudeCodeDetected;
                provider.detectionMessage = claudeCodeDetectionMessage;
            }
            if (provider.providerKey == "kimi") {
                provider.locallyDetected = kimiCodeDetected;
                provider.detectionMessage = kimiCodeDetectionMessage;
            }
        }
    }

    DashboardViewModel buildDashboardViewModel(std::time_t now, QuotaRefresh& refresh) {
        refresh.fetchedProviderKeys.clear();
        refresh.providerApiKeys.clear();
#ifdef JISTATS_ENABLE_TEST_HOOKS
        if (testProviderManager) {
            auto providers = testProviderManager->getEnabledProviders();
            const auto quotaResponses = fetchQuotaResponses(providers, now, refresh);
            persistQuotaSnapshots(providers, quotaResponses, now);
            DashboardViewModel viewModel =
                buildDashboardViewModelFromResolvedProviderData(now, providers, quotaResponses);
            applyMiniMaxCredentialProfiles(viewModel);
            applyProviderDiscoveryStatusToViewModel(viewModel);
            return viewModel;
        }
        DashboardViewModel viewModel = buildDashboardViewModelFromMirror(now, mirrorEngine);
        applyMiniMaxCredentialProfiles(viewModel);
        applyProviderDiscoveryStatusToViewModel(viewModel);
        return viewModel;
#else
        auto providers = providerManager.getEnabledProviders();
        const auto quotaResponses = fetchQuotaResponses(providers, now, refresh);
        persistQuotaSnapshots(providers, quotaResponses, now);
        DashboardViewModel viewModel =
            buildDashboardViewModelFromResolvedProviderData(now, providers, quotaResponses);
        applyMiniMaxCredentialProfiles(viewModel);
        applyProviderDiscoveryStatusToViewModel(viewModel);
        return viewModel;
#endif
    }

    std::optional<DashboardViewModel> buildPersistedStartupHydrationViewModel(
        std::time_t now,
        const DashboardPreferences& preferences) {
        auto viewModel =
            buildDashboardViewModelFromPersistedQuotaSnapshots(now, storageService, preferences);
        if (!viewModel.has_value()) {
            return std::nullopt;
        }

        applyMiniMaxCredentialProfiles(*viewModel);
        applyProviderDiscoveryStatusToViewModel(*viewModel);
        return viewModel;
    }

    void refreshPersistedActivityPresenters(std::time_t now, const AnalysisScopeState* requestedScope = nullptr) {
        std::lock_guard<std::mutex> refreshLock(runtimeRefreshMutex);
        DashboardViewModel viewModel = dashboardPresenter.getViewModel();
        viewModel.preferences = effectiveRuntimePreferences(std::move(viewModel.preferences));
        if (requestedScope) {
            setAnalysisScope(viewModel.preferences, *requestedScope);
            saveAnalysisScope(*requestedScope);
        }
        if (!viewModel.appVersion.empty()) {
            viewModel.lastUpdateTime = formatTimeUtc(now);
            applyPersistedActivitySummaries(viewModel, now);
            applyDashboardOperationalStatus(viewModel);
            if (shouldRefreshAnalysisSurface()) {
                applyAnalysisSurface(viewModel, now);
            }
            applyRuntimeViewModel(viewModel, now);
        }
    }

    void flushObservedActivities(const std::string& context, bool refreshPresenters = true) {
        if (!storageService.isOpen()) {
            return;
        }

        const DashboardPreferences preferences = currentDashboardPreferences();
        const size_t persistedCount = observationPersistenceBridge.persistPending(
            localObservationService, [this, &preferences](const ActivityEvent& event) {
                return isProviderTrackingEnabled(preferences, event.providerKey) &&
                       !hasProviderOptOutInProgress(event.providerKey);
            });
        const std::string persistenceError = observationPersistenceBridge.getLastError();
        if (!persistenceError.empty() && Logger::isInitialized()) {
            JISTATS_LOG_WARN(
                LogCategory::Db,
                "Failed to persist observed activity " + context + ": " + persistenceError);
        }
        if (refreshPresenters && persistedCount > 0) {
            markPersistedMetricSnapshotDirty(false);
            refreshPersistedActivityPresenters(std::time(nullptr));
        } else if (persistedCount > 0) {
            markPersistedMetricSnapshotDirty();
        }
    }

    void releaseResources(bool finalShutdownState) {
        {
            // A worker must either see shutdown or be waiting when we notify it.
            std::scoped_lock lock(refreshWatcherMutex, interactionWorkMutex);
            shuttingDown.store(true);
        }
        refreshWatcherCv.notify_all();
        interactionWorkCv.notify_all();
        joinInteractionWorkThread();
        joinStartupRefreshThread();
        joinTaskbarRefreshThread();
        joinOpenAICodexRefreshThread();
        joinLocalProviderRefreshThread();
        joinMiniMaxRefreshThread();
        joinScheduledExportThread();
        joinRetentionThread();
        joinMiniMaxValidationThreads();
        dashboardPresenter.shutdown();
        taskbarPresenter.shutdown();
#ifdef JISTATS_ENABLE_TEST_HOOKS
        if (!headlessForTesting) {
            trayController.shutdown();
        }
#else
        trayController.shutdown();
#endif
        flushObservedActivities("during shutdown", false);
        localObservationService.shutdown();
        if (hasPendingCodexRevocation()) {
            applyCodexModelAttributionPreference({false, requestedCodexRevocations.load()});
        }
        codexAttributionStorage.close();
        preferencesStorage.close();
        if (storageService.isOpen()) {
            storageService.close();
        }
        storageAvailable = false;
        initialized = false;
        if (finalShutdownState) {
            state = "shutdown";
        }
        if (Logger::isInitialized()) {
            if (finalShutdownState) {
                JISTATS_LOG_INFO(LogCategory::Startup, "JiStats shutdown complete");
            }
            Logger::shutdown();
        }
    }

    DashboardPreferences
    refreshProviderRuntimeState(std::time_t now, bool refreshProviderDiscovery, bool importLocalActivity,
                                const DashboardPreferences* requestedSettings = nullptr,
                                const std::optional<AnalysisScopeState>& requestedScope = std::nullopt,
                                bool refreshLocalPresenters = false) {
        std::lock_guard<std::mutex> refreshLock(runtimeRefreshMutex);
        DashboardViewModel currentViewModel = dashboardPresenter.getViewModel();
        DashboardPreferences preferences = effectiveRuntimePreferences(
            storageAvailable || currentViewModel.appVersion.empty() ? loadPreferences() : currentViewModel.preferences);
        bool settingsSaved = false;
        if (requestedSettings) {
            const auto scope = requestedScope ? *requestedScope : analysisScopeFromPreferences(preferences);
            // A queued off/on edit can suppress a provider between refreshes even if the final value matches storage.
            bool providerTrackingChanged =
                requestedSettings->providerChangeRequest > completedProviderPreferencesRequest;
            for (const auto* key : {"openai", "minimax", "claude_code", "kimi", "grok", "cursor"}) {
                providerTrackingChanged |=
                    isProviderTrackingEnabled(preferences, key) != isProviderTrackingEnabled(*requestedSettings, key);
            }

            const auto previousStartup = resolveStartupPreferences(preferences);
            DashboardPreferences requested = applyStartupPreferences(*requestedSettings);
            settingsSaved = !storageAvailable || savePreferences(requested);
            if (settingsSaved) {
                preferences = std::move(requested);
                applyExportSchedule(preferences);
                {
                    std::lock_guard<std::mutex> lock(interactionWorkMutex);
                    failedInteractionPreferences.reset();
                }
                failedPreferencesRequest.store(0);
                completedPreferencesRequest.store(requestedSettings->changeRequest);
            } else {
                JISTATS_LOG_WARN(LogCategory::General, "Could not save Settings changes to SQLite.");
                if (previousStartup.startupEnabled != requested.startupEnabled ||
                    previousStartup.startupStartInBackground != requested.startupStartInBackground) {
                    applyStartupPreferences(previousStartup);
                    preferences = resolveStartupPreferences(preferences);
                }
                // Keep an unsaved opt-out in force for this process. A failed opt-in cannot start tracking.
                preferences.openaiSubscriptionTrackingEnabled &= requested.openaiSubscriptionTrackingEnabled;
                preferences.miniMaxSubscriptionTrackingEnabled &= requested.miniMaxSubscriptionTrackingEnabled;
                preferences.claudeCodeEnabled &= requested.claudeCodeEnabled;
                preferences.kimiCodeEnabled &= requested.kimiCodeEnabled;
                preferences.grokSubscriptionTrackingEnabled &= requested.grokSubscriptionTrackingEnabled;
                preferences.cursorSubscriptionTrackingEnabled &= requested.cursorSubscriptionTrackingEnabled;
                {
                    std::lock_guard<std::mutex> lock(interactionWorkMutex);
                    failedInteractionPreferences = preferences;
                }
                failedPreferencesRequest.store(requestedSettings->changeRequest);
            }
            setAnalysisScope(preferences, scope);
            if (requestedScope) {
                saveAnalysisScope(scope);
            }

            if (storageAvailable && !providerTrackingChanged) {
                currentViewModel.preferences = preferences;
                if (refreshProviderDiscovery) {
                    runProviderDiscovery();
                    applyProviderDiscoveryStatusToViewModel(currentViewModel);
                }
                if (refreshLocalPresenters) {
                    applyPersistedActivitySummaries(currentViewModel, now);
                }
                if ((requestedScope || refreshLocalPresenters) && shouldRefreshAnalysisSurface()) {
                    applyAnalysisSurface(currentViewModel, now);
                }
                applyDashboardOperationalStatus(currentViewModel);
                applyRuntimeViewModel(currentViewModel, now);
                return preferences;
            }
        }
        if (refreshProviderDiscovery || requestedSettings) {
            runProviderDiscovery();
        }
        applyProviderPreferences(preferences);
        if (importLocalActivity) {
            importEnabledLocalProviderActivity(preferences);
        }
        QuotaRefresh refresh{++quotaRefreshGeneration, {}, {}};
        DashboardViewModel viewModel = buildDashboardViewModel(now, refresh);
        if (exitRequested.load()) {
            return preferences;
        }
        viewModel.preferences = preferences;
        ProviderConnectivityUpdates connectivityUpdates;
        applyRecoveryPersistenceCycle(viewModel, preferences, now, refresh, connectivityUpdates);
        retainLastKnownDashboardQuota(viewModel, currentViewModel);
        applyPersistedActivitySummaries(viewModel, now);
        applyDashboardOperationalStatus(viewModel);
        if (shouldRefreshAnalysisSurface()) {
            applyAnalysisSurface(viewModel, now);
        }
#ifdef JISTATS_ENABLE_TEST_HOOKS
        if (beforeRuntimePublicationForTesting) {
            beforeRuntimePublicationForTesting();
        }
#endif
        applyRuntimeViewModel(viewModel, now, std::move(connectivityUpdates));
        if (settingsSaved) {
            completedProviderPreferencesRequest =
                std::max(completedProviderPreferencesRequest, requestedSettings->providerChangeRequest);
        }
        return preferences;
    }

    DashboardPreferences loadPreferences() const {
        DashboardPreferences preferences;
        preferences.taskbarOverlayEnabled = true; // enabled for current validation work
        preferences.openaiSubscriptionTrackingEnabled = false;
        preferences.codexModelAttributionEnabled = codexModelAttributionEnabled.load();
        preferences.miniMaxSubscriptionTrackingEnabled = false;
        preferences.claudeCodeEnabled = false;
        preferences.kimiCodeEnabled = false;
        preferences.startupEnabled = startupManager.isEnabled();
        preferences.startupStartInBackground = startupManager.isStartMinimized();
        preferences.scheduledExportEnabled = false;
        preferences.scheduledExportIntervalMinutes = 1440;
        preferences.analysisDateRangePreset = AnalysisDateRangePreset::Last7Days;

        if (storageAvailable) {
            if (auto value = settingsService.getBool(preference_keys::TASKBAR_OVERLAY_ENABLED)) {
                preferences.taskbarOverlayEnabled = *value;
            }
            if (auto value =
                    settingsService.getBool(preference_keys::OPENAI_SUBSCRIPTION_TRACKING_ENABLED)) {
                preferences.openaiSubscriptionTrackingEnabled = *value;
            }
            if (auto value =
                    settingsService.getBool(preference_keys::MINIMAX_SUBSCRIPTION_TRACKING_ENABLED)) {
                preferences.miniMaxSubscriptionTrackingEnabled = *value;
            }
            if (auto value = settingsService.getBool(preference_keys::CLAUDE_CODE_ENABLED)) {
                preferences.claudeCodeEnabled = *value;
            }
            if (auto value = settingsService.getBool(preference_keys::KIMI_CODE_ENABLED)) {
                preferences.kimiCodeEnabled = *value;
            }
            if (auto value = settingsService.getBool(preference_keys::GROK_TRACKING_ENABLED)) {
                preferences.grokSubscriptionTrackingEnabled = *value;
            }
            if (auto value = settingsService.getBool(preference_keys::CURSOR_TRACKING_ENABLED)) {
                preferences.cursorSubscriptionTrackingEnabled = *value;
            }
            if (auto value = settingsService.getBool(preference_keys::STARTUP_ENABLED)) {
                preferences.startupEnabled = *value;
            }
            if (auto value = settingsService.getBool(preference_keys::STARTUP_BACKGROUND)) {
                preferences.startupStartInBackground = *value;
            }
            if (auto value = settingsService.getString(preference_keys::LAST_SELECTED_PROVIDER_ID)) {
                preferences.lastSelectedProviderId = *value;
            }
            if (auto value = settingsService.getString(preference_keys::LAST_SELECTED_ACCOUNT_ID)) {
                preferences.lastSelectedAccountId = *value;
            }
            if (auto value = settingsService.getString(preference_keys::ANALYSIS_SELECTED_PROVIDER_ID)) {
                preferences.analysisSelectedProviderId = *value;
            }
            if (auto value = settingsService.getString(preference_keys::ANALYSIS_SELECTED_ACCOUNT_ID)) {
                preferences.analysisSelectedAccountId = *value;
            }
            if (auto value = settingsService.getString(preference_keys::ANALYSIS_DATE_RANGE_PRESET)) {
                preferences.analysisDateRangePreset = AnalysisQueryEngine::parseDateRangePreset(*value);
            }
            if (auto value = settingsService.getString(preference_keys::ANALYSIS_CUSTOM_RANGE_START_UTC)) {
                if (const auto parsed = parseTimeUtcIso(*value)) {
                    preferences.analysisCustomRangeStartUtc = *parsed;
                }
            }
            if (auto value = settingsService.getString(preference_keys::ANALYSIS_CUSTOM_RANGE_END_UTC)) {
                if (const auto parsed = parseTimeUtcIso(*value)) {
                    preferences.analysisCustomRangeEndUtc = *parsed;
                }
            }
            if (auto value = settingsService.getBool(preference_keys::SCHEDULED_EXPORT_ENABLED)) {
                preferences.scheduledExportEnabled = *value;
            }
            if (auto value = settingsService.getInt(preference_keys::SCHEDULED_EXPORT_INTERVAL_MINUTES)) {
                preferences.scheduledExportIntervalMinutes = std::max(1, *value);
            }
            if (auto value = settingsService.getString(preference_keys::SCHEDULED_EXPORT_PATH)) {
                preferences.scheduledExportPath = *value;
            }
        }

        return preferences;
    }

    DashboardPreferences resolveStartupPreferences(const DashboardPreferences& preferences) {
        DashboardPreferences resolved = preferences;
        resolved.startupEnabled = startupManager.isEnabled();
        resolved.startupStartInBackground = startupManager.isStartMinimized();
        return resolved;
    }

    static AnalysisScopeState analysisScopeFromPreferences(const DashboardPreferences& preferences) {
        return {preferences.analysisSelectedProviderId, preferences.analysisSelectedAccountId,
                preferences.analysisDateRangePreset, preferences.analysisCustomRangeStartUtc,
                preferences.analysisCustomRangeEndUtc};
    }

    static void setAnalysisScope(DashboardPreferences& preferences, const AnalysisScopeState& scope) {
        preferences.analysisSelectedProviderId = scope.selectedProviderId;
        preferences.analysisSelectedAccountId = scope.selectedAccountId;
        preferences.analysisDateRangePreset = scope.dateRangePreset;
        preferences.analysisCustomRangeStartUtc = scope.customRangeStartUtc;
        preferences.analysisCustomRangeEndUtc = scope.customRangeEndUtc;
    }

    void saveAnalysisScope(const AnalysisScopeState& scope) {
        if (!storageAvailable) {
            return;
        }

        settingsService.setString(preference_keys::ANALYSIS_SELECTED_PROVIDER_ID, scope.selectedProviderId);
        settingsService.setString(preference_keys::ANALYSIS_SELECTED_ACCOUNT_ID, scope.selectedAccountId);
        settingsService.setString(preference_keys::ANALYSIS_DATE_RANGE_PRESET,
                                  AnalysisQueryEngine::dateRangePresetSettingValue(scope.dateRangePreset));
        settingsService.setString(preference_keys::ANALYSIS_CUSTOM_RANGE_START_UTC,
                                  scope.customRangeStartUtc > 0 ? formatTimeUtcIso(scope.customRangeStartUtc) : "");
        settingsService.setString(preference_keys::ANALYSIS_CUSTOM_RANGE_END_UTC,
                                  scope.customRangeEndUtc > 0 ? formatTimeUtcIso(scope.customRangeEndUtc) : "");
    }

    bool savePreferences(const DashboardPreferences& preferences) {
        if (!storageAvailable ||
            (!preferencesStorage.isOpen() && !preferencesStorage.init(storageService.getDbPath())) ||
            !preferencesStorage.beginTransaction(StorageService::TransactionMode::Immediate)) {
            return false;
        }

        if (preferenceWriter.setBool(preference_keys::TASKBAR_OVERLAY_ENABLED, preferences.taskbarOverlayEnabled) &&
            preferenceWriter.setBool(preference_keys::OPENAI_SUBSCRIPTION_TRACKING_ENABLED,
                                     preferences.openaiSubscriptionTrackingEnabled) &&
            preferenceWriter.setBool(preference_keys::MINIMAX_SUBSCRIPTION_TRACKING_ENABLED,
                                     preferences.miniMaxSubscriptionTrackingEnabled) &&
            preferenceWriter.setBool(preference_keys::CLAUDE_CODE_ENABLED, preferences.claudeCodeEnabled) &&
            preferenceWriter.setBool(preference_keys::KIMI_CODE_ENABLED, preferences.kimiCodeEnabled) &&
            preferenceWriter.setBool(preference_keys::GROK_TRACKING_ENABLED,
                                     preferences.grokSubscriptionTrackingEnabled) &&
            preferenceWriter.setBool(preference_keys::CURSOR_TRACKING_ENABLED,
                                     preferences.cursorSubscriptionTrackingEnabled) &&
            preferenceWriter.setBool(preference_keys::STARTUP_ENABLED, preferences.startupEnabled) &&
            preferenceWriter.setBool(preference_keys::STARTUP_BACKGROUND, preferences.startupStartInBackground) &&
            preferenceWriter.setString(preference_keys::LAST_SELECTED_PROVIDER_ID,
                                       preferences.lastSelectedProviderId) &&
            preferenceWriter.setString(preference_keys::LAST_SELECTED_ACCOUNT_ID, preferences.lastSelectedAccountId) &&
            preferenceWriter.setBool(preference_keys::SCHEDULED_EXPORT_ENABLED, preferences.scheduledExportEnabled) &&
            preferenceWriter.setInt(preference_keys::SCHEDULED_EXPORT_INTERVAL_MINUTES,
                                    std::max(1, preferences.scheduledExportIntervalMinutes)) &&
            preferenceWriter.setString(preference_keys::SCHEDULED_EXPORT_PATH, preferences.scheduledExportPath) &&
            preferencesStorage.commitTransaction()) {
            return true;
        }
        if (!preferencesStorage.rollbackTransaction()) {
            preferencesStorage.close();
        }
        return false;
    }

    void syncPresentedPreferences(const DashboardPreferences& preferences) {
        std::lock_guard<std::mutex> noticeLock(offlineNoticeMutex);
        DashboardViewModel viewModel = dashboardPresenter.getViewModel();
        if (viewModel.appVersion.empty()) {
            return;
        }

        if (viewModel.preferences.taskbarOverlayEnabled == preferences.taskbarOverlayEnabled &&
            viewModel.preferences.openaiSubscriptionTrackingEnabled == preferences.openaiSubscriptionTrackingEnabled &&
            viewModel.preferences.miniMaxSubscriptionTrackingEnabled ==
                preferences.miniMaxSubscriptionTrackingEnabled &&
            viewModel.preferences.claudeCodeEnabled == preferences.claudeCodeEnabled &&
            viewModel.preferences.kimiCodeEnabled == preferences.kimiCodeEnabled &&
            viewModel.preferences.grokSubscriptionTrackingEnabled == preferences.grokSubscriptionTrackingEnabled &&
            viewModel.preferences.cursorSubscriptionTrackingEnabled == preferences.cursorSubscriptionTrackingEnabled &&
            viewModel.preferences.startupEnabled == preferences.startupEnabled &&
            viewModel.preferences.startupStartInBackground == preferences.startupStartInBackground &&
            viewModel.preferences.scheduledExportEnabled == preferences.scheduledExportEnabled &&
            viewModel.preferences.scheduledExportIntervalMinutes == preferences.scheduledExportIntervalMinutes &&
            viewModel.preferences.scheduledExportPath == preferences.scheduledExportPath &&
            viewModel.preferences.lastSelectedProviderId == preferences.lastSelectedProviderId &&
            viewModel.preferences.lastSelectedAccountId == preferences.lastSelectedAccountId &&
            viewModel.preferences.analysisSelectedProviderId == preferences.analysisSelectedProviderId &&
            viewModel.preferences.analysisSelectedAccountId == preferences.analysisSelectedAccountId &&
            viewModel.preferences.analysisDateRangePreset == preferences.analysisDateRangePreset &&
            viewModel.preferences.analysisCustomRangeStartUtc == preferences.analysisCustomRangeStartUtc &&
            viewModel.preferences.analysisCustomRangeEndUtc == preferences.analysisCustomRangeEndUtc) {
            return;
        }

        viewModel.preferences = preferences;
        viewModel.preferences.codexModelAttributionEnabled = codexModelAttributionEnabled.load();
        applyDashboardOperationalStatus(viewModel);
        dashboardPresenter.updateViewModel(viewModel);
    }

    DashboardPreferences applyStartupPreferences(const DashboardPreferences& preferences) {
        bool backgroundRewriteFailed = false;
        bool startupTransitionFailed = false;
        const bool startupWasEnabled = startupManager.isEnabled();

        if (preferences.startupEnabled) {
            if (!startupManager.setStartMinimized(preferences.startupStartInBackground)) {
                backgroundRewriteFailed = startupManager.isEnabled();
                JISTATS_LOG_WARN(LogCategory::Startup,
                    "Failed to update startup background mode: " + startupManager.getLastError());
            }

            if (!startupManager.isEnabled() && !startupManager.enable()) {
                startupTransitionFailed = true;
                JISTATS_LOG_WARN(LogCategory::Startup,
                    "Failed to enable run-on-startup: " + startupManager.getLastError());
            }
        } else {
            if (startupWasEnabled && !startupManager.disable()) {
                startupTransitionFailed = true;
                JISTATS_LOG_WARN(LogCategory::Startup,
                    "Failed to disable run-on-startup: " + startupManager.getLastError());
            }

            if (!startupTransitionFailed &&
                !startupManager.setStartMinimized(preferences.startupStartInBackground)) {
                JISTATS_LOG_WARN(LogCategory::Startup,
                    "Failed to cache startup background mode after disabling startup: " +
                    startupManager.getLastError());
            }
        }

        if (startupTransitionFailed) {
            return preferences;
        }

        DashboardPreferences resolved = resolveStartupPreferences(preferences);
        if (backgroundRewriteFailed &&
            resolved.startupEnabled == preferences.startupEnabled &&
            resolved.startupStartInBackground != preferences.startupStartInBackground) {
            JISTATS_LOG_WARN(
                LogCategory::Startup,
                "Startup background preference mismatch detected; keeping the registry Run entry as the source of truth");
        }

        return resolved;
    }

    void applyTaskbarPreference(const DashboardPreferences& preferences) {
        if (preferences.taskbarOverlayEnabled) {
            const std::time_t now = std::time(nullptr);
            const DashboardViewModel viewModel = dashboardPresenter.getViewModel();
#ifdef JISTATS_ENABLE_TEST_HOOKS
            if (headlessForTesting || taskbarTestModeForTesting) {
                taskbarPresenter.setRuntimeStateForTesting(true, true);
                taskbarPresenter.replaceMetrics(buildRuntimeTaskbarMetrics(now, viewModel));
                return;
            }
#endif
            if (taskbarPresenter.init()) {
                taskbarPresenter.setOverlayEnabled(true);
                taskbarPresenter.replaceMetrics(buildRuntimeTaskbarMetrics(now, viewModel));
            } else {
                JISTATS_LOG_WARN(LogCategory::Startup,
                    "TaskbarPresenter unavailable: " + taskbarPresenter.getLastError());
            }
        } else {
#ifdef JISTATS_ENABLE_TEST_HOOKS
            if (headlessForTesting || taskbarTestModeForTesting) {
                taskbarPresenter.clearMetrics();
                taskbarPresenter.setRuntimeStateForTesting(false, false);
                return;
            }
#endif
            taskbarPresenter.setOverlayEnabled(false);
            taskbarPresenter.clearMetrics();
        }
    }
};

AppController::AppController() : pImpl(std::make_unique<Impl>()) {}

AppController::~AppController() {
    shutdown();
}

std::string AppController::getStartupWarning() const {
    if (pImpl->exitRequested.load()) {
        return {};
    }
    const auto version = pImpl->storageService.getRejectedSchemaVersion();
    if (!version) {
        return {};
    }

    return "JiStats could not use this database because it uses development schema version " +
           std::to_string(*version) + ".\n\n" + pImpl->storageService.getDbPath() +
           "\n\nDatabase persistence is disabled. JiStats will not save settings or activity to the database."
           "\n\nTo start fresh, exit JiStats and move this database and any companion files "
           "(-wal, -shm, or -journal) to a backup folder. If there is also a legacy jistats.db "
           "beside JiStats.exe or in its working directory, move that file and its companion files as well. "
           "Restart JiStats to create a fresh database.";
}

bool AppController::init(bool launchInBackground) {
    if (pImpl->initialized) {
        return true;
    }

    try {
        pImpl->exitRequested.store(false);
        pImpl->shuttingDown.store(false);
        // Initialize logging first so all subsequent init steps can log
        Logger::init();
        JISTATS_LOG_INFO(LogCategory::Startup, "JiStats initializing");

        DashboardViewModel startupView;
        startupView.startupPending = true;
        startupView.runtimeState = RuntimeState::CatchingUp;
        pImpl->dashboardPresenter.updateViewModel(startupView);
        pImpl->dashboardPresenter.setVisible(!launchInBackground);
        pImpl->dashboardPresenter.setExitRequestedCallback([this]() { requestShutdown(); });

#ifdef JISTATS_ENABLE_TEST_HOOKS
        if (pImpl->forceDashboardInitFailureForTesting) {
            JISTATS_LOG_ERROR(LogCategory::Startup, "DashboardPresenter unavailable: forced init failure for test");
            pImpl->state = "error";
            pImpl->releaseResources(false);
            return false;
        }
#endif

#ifdef JISTATS_ENABLE_TEST_HOOKS
        if (!pImpl->headlessForTesting) {
            if (!pImpl->dashboardPresenter.init(true)) {
                JISTATS_LOG_ERROR(LogCategory::Startup,
                                  "DashboardPresenter unavailable: " + pImpl->dashboardPresenter.getLastError());
                pImpl->state = "error";
                pImpl->releaseResources(false);
                return false;
            }
        }
#else
        if (!pImpl->dashboardPresenter.init(true)) {
            JISTATS_LOG_ERROR(LogCategory::Startup,
                              "DashboardPresenter unavailable: " + pImpl->dashboardPresenter.getLastError());
            pImpl->state = "error";
            pImpl->releaseResources(false);
            return false;
        }
#endif

        {
#ifdef JISTATS_ENABLE_TEST_HOOKS
            if (!pImpl->headlessForTesting)
#endif
            {
                const auto showDashboard = [this]() {
                    pImpl->dashboardPresenter.setVisible(true);
                    if (!pImpl->dashboardPresenter.getViewModel().startupPending) {
                        pImpl->requestPersistedActivityRefresh();
                    }
                };
                pImpl->trayController.setOnShowDashboardCallback(showDashboard);
                pImpl->trayController.setOnPreferencesCallback(showDashboard);
                pImpl->trayController.setOnRetryNowCallback(
                    [this]() { pImpl->dashboardPresenter.requestOfflineRetry(); });
                pImpl->trayController.setOnDismissOfflineNoticeCallback(
                    [this]() { pImpl->dashboardPresenter.requestOfflineDismiss(); });
                pImpl->trayController.setOnExitCallback([this]() { requestShutdown(); });
                pImpl->taskbarPresenter.setOnShowDashboardCallback(showDashboard);
                pImpl->taskbarPresenter.setOnPreferencesCallback(showDashboard);
                pImpl->taskbarPresenter.setOnRetryNowCallback(
                    [this]() { pImpl->dashboardPresenter.requestOfflineRetry(); });
                pImpl->taskbarPresenter.setOnDismissOfflineNoticeCallback(
                    [this]() { pImpl->dashboardPresenter.requestOfflineDismiss(); });
                pImpl->taskbarPresenter.setOnExitCallback([this]() { requestShutdown(); });

                pImpl->trayController.setState(TrayStatus::Syncing, "JiStats - Starting...");
                if (!pImpl->trayController.init()) {
                    JISTATS_LOG_WARN(LogCategory::Startup,
                                     "TrayController unavailable: " + pImpl->trayController.getLastError());
                    if (launchInBackground) {
                        pImpl->dashboardPresenter.setVisible(true);
                        JISTATS_LOG_WARN(
                            LogCategory::Startup,
                            "TrayController init failed during background launch; showing dashboard instead");
                    }
                }
            }
        }
        pImpl->initialized = true;
        if (pImpl->exitRequested.load()) {
            return true;
        }

        // Initialize storage service (optional - app works without it)
        if (pImpl->storageService.init(pImpl->databasePath)) {
            pImpl->storageAvailable = true;
            JISTATS_LOG_INFO(
                LogCategory::Startup,
                "StorageService ready: " + pImpl->storageService.getDbPath());
            pImpl->loadScheduledExportStateFromStorage();
            pImpl->loadRetentionStateFromStorage();
        } else {
            JISTATS_LOG_WARN(LogCategory::Startup,
                "StorageService unavailable: " + pImpl->storageService.getLastError());
            JISTATS_LOG_WARN(LogCategory::Startup, "Preferences will use safe defaults");
        }

        if (pImpl->exitRequested.load()) {
            return true;
        }
#ifdef JISTATS_ENABLE_TEST_HOOKS
        if (!pImpl->headlessForTesting)
#endif
        {
            pImpl->dashboardPresenter.restoreStartupPlacement();
        }

        pImpl->codexModelAttributionEnabled.store(false);
        pImpl->lastCodexLocalSqliteGeneration = 0;

        if (pImpl->localObservationService.init()) {
            if (pImpl->storageAvailable) {
                pImpl->localObservationService.setActivityCallback([this](const ActivityEvent&) {
                    pImpl->flushObservedActivities("from LocalObservationService callback");
                });
            }
            JISTATS_LOG_INFO(LogCategory::Startup, "LocalObservationService ready");
        } else {
            JISTATS_LOG_WARN(
                LogCategory::Startup,
                "LocalObservationService unavailable: " + pImpl->localObservationService.getLastError());
        }

        pImpl->ensureMiniMaxCredentialAdapter();
        pImpl->hydrateMiniMaxCredentialProfilesFromStorage();

        // Load persisted preferences
        DashboardPreferences loadedPreferences = pImpl->loadPreferences();
        loadedPreferences = pImpl->applyStartupPreferences(loadedPreferences);
        pImpl->savePreferences(loadedPreferences);
        pImpl->applyExportSchedule(loadedPreferences);
        pImpl->saveAnalysisScope(Impl::analysisScopeFromPreferences(loadedPreferences));

        pImpl->dashboardPresenter.setMiniMaxCredentialActionCallback(
            [this](const DashboardMiniMaxCredentialRequest& request) {
                return pImpl->handleMiniMaxCredentialAction(request);
            });
        pImpl->dashboardPresenter.setOpenAIImportActionCallback(
            [this](const std::string& inputPath) {
                return pImpl->handleOpenAIImportAction(inputPath);
            });
        pImpl->dashboardPresenter.setHtmlExportActionCallback(
            [this](const std::string& outputPath) {
                return pImpl->handleManualHtmlExport(outputPath);
            });
        pImpl->dashboardPresenter.setOfflineRetryActionCallback([this]() { return pImpl->handleOfflineRetryAction(); });
        pImpl->dashboardPresenter.setOfflineDismissActionCallback(
            [this]() { return pImpl->handleOfflineDismissAction(); });
        pImpl->dashboardPresenter.setAnalysisPresenter(&pImpl->analysisPresenter);

        const std::time_t now = pImpl->currentTime();
        pImpl->markLaunchRecoveryState(now);

#ifndef JISTATS_ENABLE_TEST_HOOKS
        // Production path: register providers and use provider-fed data
        // Register OpenAI adapter
        auto openaiAdapter = std::make_shared<OpenAIAdapter>();
        pImpl->providerManager.addProvider(openaiAdapter);

        // Register MiniMax adapter
        pImpl->providerManager.addProvider(pImpl->miniMaxCredentialAdapter);

        // Register Claude Code adapter
        auto claudeAdapter = std::make_shared<ClaudeCodeAdapter>();
        pImpl->providerManager.addProvider(claudeAdapter);

        pImpl->providerManager.addProvider(std::make_shared<SubscriptionQuotaAdapter>(SubscriptionProvider::Grok));
        pImpl->providerManager.addProvider(std::make_shared<SubscriptionQuotaAdapter>(SubscriptionProvider::Cursor));

#if JISTATS_HAS_KIMI_ADAPTER
        auto kimiAdapter = std::make_shared<KimiAdapter>();
        pImpl->providerManager.addProvider(kimiAdapter);
#endif
#endif
        pImpl->applyProviderPreferences(loadedPreferences);

        bool usedPersistedStartupHydration = false;
        bool executedStartupCatchUp = false;
        DashboardViewModel viewModel;
        std::optional<Impl::ProviderConnectivityUpdates> connectivityUpdates;
        if (const auto hydratedViewModel =
                pImpl->buildPersistedStartupHydrationViewModel(now, loadedPreferences)) {
            viewModel = *hydratedViewModel;
            usedPersistedStartupHydration = true;
        } else {
            pImpl->runProviderDiscovery();
            pImpl->importEnabledLocalProviderActivity(loadedPreferences);
            Impl::QuotaRefresh refresh{++pImpl->quotaRefreshGeneration, {}, {}, true};
            viewModel = pImpl->buildDashboardViewModel(now, refresh);
            if (pImpl->exitRequested.load()) {
                return true;
            }
            viewModel.preferences = loadedPreferences; // Use persisted preferences
            executedStartupCatchUp =
                pImpl->applyRecoveryPersistenceCycle(
                    viewModel, loadedPreferences, now, refresh, connectivityUpdates.emplace());
        }
        viewModel.preferences = loadedPreferences; // Use persisted preferences
        if (usedPersistedStartupHydration) {
            pImpl->applyGapAnnotations(viewModel, now);
            if (pImpl->hasPendingRecovery() && viewModel.runtimeState != RuntimeState::Offline) {
                pImpl->overlayPendingRecoveryState(viewModel);
            }
        }
        pImpl->applyPersistedActivitySummaries(viewModel, now);
        pImpl->applyDashboardOperationalStatus(viewModel);
        pImpl->applyAnalysisSurface(viewModel, now);
        pImpl->reportingService.setDashboardDataProvider([this](DashboardViewModel& reportViewModel) {
            reportViewModel = pImpl->dashboardPresenter.getViewModel();
            return true;
        });
        pImpl->reportingService.setMirrorPolicyVersionProvider([this](const DashboardViewModel& reportViewModel) {
            return buildMirrorPolicyVersionLabel(pImpl->storageService, reportViewModel);
        });
        pImpl->reportingService.setAggregateDataProvider([this](const ReportRequest& request,
                                                                const DashboardViewModel& reportViewModel,
                                                                const ReportMetadata& metadata) {
            return buildReportAggregateData(pImpl->storageService, request, reportViewModel, metadata,
                                            std::time(nullptr));
        });
        pImpl->dashboardPresenter.setPreferencesChangedCallback(
            [this](const DashboardPreferences& preferences) { pImpl->requestPreferencesChange(preferences); });
        pImpl->dashboardPresenter.setCodexModelAttributionChangedCallback([this](bool enabled, std::uint64_t request) {
            pImpl->requestCodexModelAttributionChange(enabled, request);
        });
        pImpl->dashboardPresenter.setPreferencesOpenedCallback([this]() {
            pImpl->requestProviderDiscoveryRefresh();
        });
        pImpl->dashboardPresenter.setPeriodicRefreshCallback([this]() { pImpl->requestPersistedActivityRefresh(); });
        pImpl->analysisPresenter.setScopeChangedCallback([this](const AnalysisScopeState& state) {
            pImpl->analysisPresenter.setInteractionPending(true);
            pImpl->requestAnalysisScopeChange(state);
        });

        if (pImpl->exitRequested.load()) {
            return true;
        }
        pImpl->applyRuntimeViewModel(viewModel, now, std::move(connectivityUpdates));
#ifdef JISTATS_ENABLE_TEST_HOOKS
        if (!pImpl->disableTaskbarForTesting) {
            pImpl->applyTaskbarPreference(viewModel.preferences);
        }
#else
        pImpl->applyTaskbarPreference(viewModel.preferences);
#endif

        pImpl->startInteractionWorkThread();
        if (pImpl->shouldQueueDeferredStartupRefresh(
                loadedPreferences,
                usedPersistedStartupHydration,
                executedStartupCatchUp)) {
            pImpl->queueDeferredStartupRefresh(
                false,
                pImpl->shouldImportLocalActivityDuringStartupRefresh(loadedPreferences));
        }
        pImpl->startTaskbarRefreshWatcher();
        pImpl->startOpenAICodexRefreshWatcher();
        pImpl->startLocalProviderRefreshWatcher();
        pImpl->startMiniMaxRefreshWatcher();
        pImpl->startScheduledExportWatcher();
        pImpl->startRetentionWatcher();

        pImpl->state = "running";
        JISTATS_LOG_INFO(LogCategory::Startup, "JiStats initialized");
        return true;
    } catch (const std::exception& e) {
        JISTATS_LOG_ERROR(LogCategory::Startup,
            std::string("Failed to initialize AppController: ") + e.what());
        pImpl->state = "error";
        pImpl->releaseResources(false);
        return false;
    }
}

int AppController::run() {
    if (!pImpl->initialized) {
        return 1;
    }

    pImpl->state = "ui-active";
    const int exitCode = pImpl->dashboardPresenter.waitForExit();
    pImpl->state = "ui-closed";
    return exitCode;
}

void AppController::requestShutdown() {
    pImpl->exitRequested.store(true);
    pImpl->state = "stopping";
    pImpl->dashboardPresenter.requestClose();
}

void AppController::shutdown() {
    if (!pImpl->hasLiveResources()) {
        return;
    }
    pImpl->releaseResources(true);
}

std::string AppController::getState() const {
    return pImpl->state.load();
}

#ifdef JISTATS_ENABLE_TEST_HOOKS
void AppController::setHeadlessForTesting(bool value) {
    pImpl->headlessForTesting = value;
}

void AppController::setDisableTaskbarForTesting(bool value) {
    pImpl->disableTaskbarForTesting = value;
}

void AppController::setTaskbarTestModeForTesting(bool value) {
    pImpl->taskbarTestModeForTesting = value;
}

void AppController::setForceDashboardInitFailureForTesting(bool value) {
    pImpl->forceDashboardInitFailureForTesting = value;
}

void AppController::setDatabasePathForTesting(const std::string& dbPath) {
    pImpl->databasePath = dbPath;
    pImpl->disableCodexTokenImportForTesting = true;
}

void AppController::setProviderManagerForTesting(std::shared_ptr<ProviderManager> providerManager) {
    pImpl->testProviderManager = std::move(providerManager);
}

bool AppController::recordLocalActivityForTesting(const ActivityEvent& event) {
    if (!pImpl->shouldPersistLocalActivity(event)) {
        return false;
    }
    return pImpl->localObservationService.recordActivity(event);
}

std::vector<PersistedActivityEvent> AppController::getPersistedActivitiesForTesting(
    const std::string& providerKey,
    const std::string& providerSurface,
    std::time_t startedAtFrom,
    std::time_t startedAtTo) {
    return pImpl->observationPersistenceBridge.getPersistedActivities(
        providerKey,
        providerSurface,
        startedAtFrom,
        startedAtTo);
}

std::optional<PersistedConnectivityState> AppController::getConnectivityStateForTesting(
    const std::string& providerKey) const {
    const auto providerId = findProviderIdByKey(pImpl->storageService, providerKey);
    const auto accountId =
        ensureRuntimeTrackingAccountId(pImpl->storageService, providerKey, providerKey);
    if (!providerId.has_value() || !accountId.has_value()) {
        return std::nullopt;
    }

    return pImpl->storageService.getConnectivityState(*providerId, *accountId);
}

std::optional<PersistedSyncState> AppController::getSyncStateForTesting(
    const std::string& providerKey,
    const std::string& syncKind) const {
    const auto providerId = findProviderIdByKey(pImpl->storageService, providerKey);
    const auto accountId =
        ensureRuntimeTrackingAccountId(pImpl->storageService, providerKey, providerKey);
    if (!providerId.has_value() || !accountId.has_value()) {
        return std::nullopt;
    }

    return pImpl->storageService.getSyncState(*providerId, *accountId, syncKind);
}

std::optional<PersistedRuntimeState> AppController::getPersistedRuntimeStateForTesting() const {
    return pImpl->storageService.getRuntimeState();
}

std::vector<PersistedGapMarker> AppController::getGapMarkersForTesting(
    const std::string& providerKey,
    std::time_t periodStartFrom,
    std::time_t periodEndTo) const {
    const auto providerId = findProviderIdByKey(pImpl->storageService, providerKey);
    const auto accountId =
        ensureRuntimeTrackingAccountId(pImpl->storageService, providerKey, providerKey);
    if (!providerId.has_value() || !accountId.has_value()) {
        return {};
    }

    return pImpl->storageService.getGapMarkers(
        *providerId,
        *accountId,
        periodStartFrom,
        periodEndTo);
}

DashboardViewModel AppController::getDashboardViewModelForTesting() const {
    return pImpl->dashboardPresenter.getViewModel();
}

std::vector<TaskbarMetrics> AppController::getTaskbarMetricsForTesting() const {
    return pImpl->taskbarPresenter.getCurrentMetrics();
}

bool AppController::isDashboardVisibleForTesting() const {
    return pImpl->dashboardPresenter.isVisible();
}

bool AppController::dispatchTrayEventForTesting(TrayController::TestEvent event) {
    return pImpl->trayController.dispatchTestEvent(event);
}

bool AppController::dispatchDashboardShellCommandForTesting(DashboardShellCommand command) {
    return pImpl->dashboardPresenter.dispatchShellCommandForTesting(command);
}

bool AppController::selectDashboardPreferencesTabForTesting(int tabIndex) {
    return pImpl->dashboardPresenter.selectPreferencesTabForTesting(tabIndex);
}

bool AppController::clickDashboardOfflineRetryForTesting() {
    return pImpl->dashboardPresenter.clickOfflineRetryForTesting();
}

bool AppController::clickDashboardOfflineDismissForTesting() {
    return pImpl->dashboardPresenter.clickOfflineDismissForTesting();
}

bool AppController::isDashboardShellDialogOpenForTesting(DashboardShellDialog dialog) const {
    return pImpl->dashboardPresenter.isShellDialogOpenForTesting(dialog);
}

std::string AppController::getDashboardShellDialogBodyTextForTesting(DashboardShellDialog dialog) const {
    return pImpl->dashboardPresenter.getShellDialogBodyTextForTesting(dialog);
}

bool AppController::submitMiniMaxCredentialActionForTesting(
    const DashboardMiniMaxCredentialRequest& request) {
    if (pImpl->dashboardPresenter.submitMiniMaxCredentialActionForTesting(request)) {
        return true;
    }

    return pImpl->handleMiniMaxCredentialAction(request).success;
}

std::string AppController::getMiniMaxSecretFieldTextForTesting() const {
    return pImpl->dashboardPresenter.getMiniMaxSecretFieldTextForTesting();
}

std::optional<std::string> AppController::getMiniMaxValidationStateForTesting(
    const std::string& planKey) const {
    const auto profile = pImpl->getMiniMaxCredentialProfile(planKey);
    if (!profile.has_value()) {
        return std::nullopt;
    }
    return profile->validationState;
}

std::vector<DashboardSummaryItem> AppController::getDashboardUsageProgressSummariesForTesting() const {
    return pImpl->dashboardPresenter.getUsageProgressSummariesForTesting();
}

TrayController::TestState AppController::getTrayStateForTesting() const {
    return pImpl->trayController.getTestState();
}

std::optional<Notice> AppController::getActiveNoticeForTesting(NoticeType type) const {
    return pImpl->notificationService.getActiveNotice(type);
}

bool AppController::isDashboardOfflineNoticeVisibleForTesting() const {
    return pImpl->dashboardPresenter.isOfflineNoticeVisibleForTesting();
}

std::string AppController::getDashboardOfflineNoticeTextForTesting() const {
    return pImpl->dashboardPresenter.getOfflineNoticeTextForTesting();
}

std::string AppController::getDashboardStatusTextForTesting() const {
    return pImpl->dashboardPresenter.getStatusTextForTesting();
}

DashboardHtmlExportResult AppController::exportHtmlReportForTesting(const std::string& outputPath) {
    return pImpl->handleManualHtmlExport(outputPath);
}

void AppController::refreshRuntimeForTesting() {
    pImpl->refreshProviderRuntimeState(pImpl->currentTime(), true, true);
}

void AppController::setAfterSettingsDequeueForTesting(std::function<void()> callback) {
    pImpl->afterSettingsDequeueForTesting = std::move(callback);
}

void AppController::setBeforeRuntimePublicationForTesting(std::function<void()> callback) {
    pImpl->beforeRuntimePublicationForTesting = std::move(callback);
}

void AppController::setBeforeCodexSourceReadForTesting(std::function<void()> callback) {
    pImpl->beforeCodexSourceReadForTesting = std::move(callback);
}

void AppController::applyDashboardPreferencesForTesting(const DashboardPreferences& preferences) {
    DashboardPreferences effectivePreferences = pImpl->applyStartupPreferences(preferences);
    if (effectivePreferences.codexModelAttributionEnabled != pImpl->codexModelAttributionEnabled.load() ||
        pImpl->hasPendingCodexRevocation()) {
        pImpl->applyCodexModelAttributionPreference(
            pImpl->makeCodexAttributionChange(effectivePreferences.codexModelAttributionEnabled));
    }
    pImpl->savePreferences(effectivePreferences);
    pImpl->saveAnalysisScope(Impl::analysisScopeFromPreferences(effectivePreferences));
    pImpl->applyProviderPreferences(effectivePreferences);
    pImpl->syncPresentedPreferences(effectivePreferences);
    pImpl->refreshProviderRuntimeState(std::time(nullptr), true, true);
    pImpl->savePreferences(effectivePreferences);
    pImpl->saveAnalysisScope(Impl::analysisScopeFromPreferences(effectivePreferences));
    pImpl->syncPresentedPreferences(effectivePreferences);
    pImpl->applyExportSchedule(effectivePreferences);
    pImpl->runScheduledExportIfDue(std::time(nullptr));
    pImpl->refreshWatcherCv.notify_all();
#ifdef JISTATS_ENABLE_TEST_HOOKS
    if (!pImpl->disableTaskbarForTesting) {
        pImpl->applyTaskbarPreference(effectivePreferences);
    }
#else
    pImpl->applyTaskbarPreference(effectivePreferences);
#endif
}

void AppController::setMiniMaxCredentialStoreForTesting(std::shared_ptr<WindowsCredentialStore> store) {
    pImpl->miniMaxCredentialStore = std::move(store);
    pImpl->ensureMiniMaxCredentialAdapter();
    if (pImpl->miniMaxCredentialStore) {
        pImpl->miniMaxCredentialAdapter->setCredentialStore(pImpl->miniMaxCredentialStore);
    }
    if (const auto runtimeAdapter = pImpl->resolveMiniMaxRuntimeAdapter()) {
        if (pImpl->miniMaxCredentialStore) {
            runtimeAdapter->setCredentialStore(pImpl->miniMaxCredentialStore);
        }
    }
}

void AppController::setMiniMaxValidationResponseForTesting(
    bool success,
    int statusCode,
    const std::string& body,
    const std::string& error) {
    pImpl->ensureMiniMaxCredentialAdapter();
    pImpl->miniMaxCredentialAdapter->setMockValidationResponse(success, statusCode, body, error);
    if (const auto runtimeAdapter = pImpl->resolveMiniMaxRuntimeAdapter()) {
        runtimeAdapter->setMockValidationResponse(success, statusCode, body, error);
    }
}

void AppController::setMiniMaxValidationReleaseForTesting(std::shared_future<void> release) {
    pImpl->miniMaxValidationReleaseForTesting = std::move(release);
}

void AppController::setOpenAICodexRefreshPollIntervalForTesting(int intervalMs) {
    pImpl->openAICodexRefreshPollInterval =
        std::chrono::milliseconds(std::max(1, intervalMs));
}

void AppController::setLocalProviderRefreshPollIntervalForTesting(int intervalMs) {
    pImpl->localProviderRefreshPollInterval =
        std::chrono::milliseconds(std::max(1, intervalMs));
}

void AppController::setCurrentTimeForTesting(std::time_t now) {
    pImpl->currentTimeForTesting = now;
}

void AppController::clearCurrentTimeForTesting() {
    pImpl->currentTimeForTesting.reset();
}

void AppController::setScheduledExportPollIntervalForTesting(int intervalMs) {
    pImpl->scheduledExportPollInterval =
        std::chrono::milliseconds(std::max(1, intervalMs));
}

void AppController::setRetentionPollIntervalForTesting(int intervalMs) {
    pImpl->retentionPollInterval =
        std::chrono::milliseconds(std::max(1, intervalMs));
}

void AppController::setOpenAIImportPathForTesting(const std::string& inputPath) {
    pImpl->dashboardPresenter.setOpenAIImportPathForTesting(inputPath);
}

void AppController::setHtmlExportPathForTesting(const std::string& outputPath) {
    pImpl->dashboardPresenter.setHtmlExportPathForTesting(outputPath);
}

void AppController::setStartupManagerTestHooks(const StartupManager::TestHooks& hooks) {
    pImpl->startupManager.setTestHooks(hooks);
}

void AppController::clearStartupManagerTestHooks() {
    pImpl->startupManager.clearTestHooks();
}
#endif

} // namespace JiStats
