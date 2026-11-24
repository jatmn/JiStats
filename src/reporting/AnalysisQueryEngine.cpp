#include "reporting/AnalysisQueryEngine.hpp"

#include "db/StorageService.hpp"
#include "gui/DashboardPresenter.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>

namespace JiStats {

namespace {

constexpr const char* kRuntimeBackfillSyncKind = "runtime_backfill";

struct SourceFlags {
    bool rollupBacked = false;
    bool local = false;
    bool imported = false;
    bool api = false;
    bool unknown = false;

    void merge(const SourceFlags& other) {
        rollupBacked = rollupBacked || other.rollupBacked;
        local = local || other.local;
        imported = imported || other.imported;
        api = api || other.api;
        unknown = unknown || other.unknown;
    }

    int categoryCount() const {
        return static_cast<int>(rollupBacked) +
               static_cast<int>(local) +
               static_cast<int>(imported) +
               static_cast<int>(api) +
               static_cast<int>(unknown);
    }
};

struct ScopedHistoryBucket {
    std::string dayLabel;
    std::string providerKey;
    std::string providerLabel;
    std::string accountKey;
    std::string accountLabel;
    long long activityCount = 0;
    bool hasQuantifiedUsage = false;
    bool hasActivityFallback = false;
    SourceFlags sources;
};

struct DayGapRollup {
    int knownZeroMarkers = 0;
    int reconstructedMarkers = 0;
    int unknownMarkers = 0;
    std::vector<std::string> basisSummaries;
    std::set<std::string> sourceKinds;
};

struct ScopedProviderAccountIds {
    int providerId = 0;
    int accountId = 0;
};

struct QuotaWindowResolution {
    std::time_t startUtc = 0;
    std::time_t endUtc = 0;
};

void appendScopeFilter(
    std::string& sql,
    std::vector<std::string>& params,
    const std::string& value,
    const char* columnName);

struct TrendBucketConfig {
    AnalysisTrendBucketGranularity granularity = AnalysisTrendBucketGranularity::Daily;
    const char* label = "Daily";
};

struct TrendBucketWindow {
    std::time_t startUtc = 0;
    std::time_t endUtc = 0;
    std::string label;
};

struct ScopedTrendBucket {
    std::time_t bucketStartUtc = 0;
    std::time_t bucketEndUtc = 0;
    std::string bucketLabel;
    std::string providerKey;
    std::string providerLabel;
    std::string accountKey;
    std::string accountLabel;
    long long activityCount = 0;
    bool hasQuantifiedUsage = false;
    bool hasActivityFallback = false;
    SourceFlags sources;
};

std::time_t utcDayStart(std::time_t time) {
    std::tm tm{};
    gmtime_s(&tm, &time);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    return _mkgmtime(&tm);
}

std::time_t utcHourStart(std::time_t value) {
    std::tm tm{};
    gmtime_s(&tm, &value);
    tm.tm_min = 0;
    tm.tm_sec = 0;
    return _mkgmtime(&tm);
}

std::time_t utcWeekStart(std::time_t value) {
    std::tm tm{};
    gmtime_s(&tm, &value);
    const int daysFromMonday = (tm.tm_wday + 6) % 7;
    tm.tm_mday -= daysFromMonday;
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    return _mkgmtime(&tm);
}

std::time_t utcMonthStart(std::time_t value) {
    std::tm tm{};
    gmtime_s(&tm, &value);
    tm.tm_mday = 1;
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    return _mkgmtime(&tm);
}

long long elapsedUtcDaysInclusive(std::time_t start, std::time_t end) {
    if (end < start) {
        return 1;
    }

    return static_cast<long long>((utcDayStart(end) - utcDayStart(start)) / 86400) + 1;
}

std::string formatReportDayLabel(std::time_t value) {
    std::tm tm{};
    gmtime_s(&tm, &value);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &tm);
    return buffer;
}

std::string formatTimeUtcIso(std::time_t value) {
    std::tm tm{};
    gmtime_s(&tm, &value);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

std::optional<std::time_t> parseUtcIso(const std::string& value) {
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

std::string formatQuotaSnapshotLabel(std::time_t value, std::time_t windowSpanSeconds) {
    std::tm tm{};
    gmtime_s(&tm, &value);
    char buffer[32]{};
    if (windowSpanSeconds <= (36 * 60 * 60)) {
        std::strftime(buffer, sizeof(buffer), "%m-%d %H:%M", &tm);
    } else {
        std::strftime(buffer, sizeof(buffer), "%m-%d", &tm);
    }
    return buffer;
}

std::string quotaWindowLabelFromSpan(std::time_t start, std::time_t end) {
    const std::time_t spanSeconds = std::max<std::time_t>(1, end - start);
    const long long spanHours = static_cast<long long>((spanSeconds + 3599) / 3600);
    const long long spanDays = static_cast<long long>((spanSeconds + 86399) / 86400);
    if (spanHours <= 8) {
        return std::to_string(spanHours) + "h";
    }
    if (spanDays <= 14) {
        return std::to_string(spanDays) + "d";
    }
    return formatReportDayLabel(start) + " to " + formatReportDayLabel(end);
}

std::string normalizeQuotaWindowLabel(std::string value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (unsigned char ch : value) {
        normalized.push_back(static_cast<char>(std::tolower(ch)));
    }

    if (normalized.empty() || normalized == "rolling" || normalized == "calendar") {
        return {};
    }
    if (normalized == "daily" || normalized == "day" || normalized == "24h") {
        return "1d";
    }
    if (normalized == "weekly" || normalized == "week") {
        return "7d";
    }
    if (normalized == "monthly" || normalized == "month") {
        return "30d";
    }
    return value;
}

std::optional<ScopedProviderAccountIds> resolveScopedProviderAccountIds(
    StorageService& storage,
    const std::string& providerKey,
    const std::string& accountKey) {
    if (providerKey.empty() || accountKey.empty()) {
        return std::nullopt;
    }

    QueryResult scopedIds = storage.execute(
        "SELECT p.id, a.id "
        "FROM providers p "
        "JOIN accounts a ON a.provider_id = p.id "
        "WHERE p.provider_key = ? AND a.account_key = ? LIMIT 1;",
        {providerKey, accountKey});
    if (!scopedIds.success() || scopedIds.rowCount() == 0) {
        return std::nullopt;
    }

    return ScopedProviderAccountIds{
        scopedIds.getInt(0, 0),
        scopedIds.getInt(0, 1)
    };
}

std::optional<QuotaWindowResolution> resolveLatestQuotaWindow(
    StorageService& storage,
    const std::string& providerKey,
    const std::string& accountKey,
    std::time_t now) {
    std::string sql =
        "SELECT qs.window_start_utc, qs.window_end_utc "
        "FROM quota_snapshots qs "
        "JOIN providers p ON p.id = qs.provider_id "
        "JOIN accounts a ON a.id = qs.account_id "
        "WHERE qs.window_start_utc > 0 AND qs.window_end_utc > 0";
    std::vector<std::string> params;
    appendScopeFilter(sql, params, providerKey, "p.provider_key");
    appendScopeFilter(sql, params, accountKey, "a.account_key");
    sql += " ORDER BY qs.snapshot_time_utc DESC LIMIT 1;";

    QueryResult latestWindow = storage.execute(sql, params);
    if (!latestWindow.success() || latestWindow.rowCount() == 0) {
        return std::nullopt;
    }

    const auto startUtc = parseUtcIso(latestWindow.getString(0, 0));
    const auto endUtc = parseUtcIso(latestWindow.getString(0, 1));
    if (!startUtc.has_value() || !endUtc.has_value()) {
        return std::nullopt;
    }

    QuotaWindowResolution resolved;
    resolved.startUtc = *startUtc;
    resolved.endUtc = std::min<std::time_t>(now, *endUtc);
    if (resolved.startUtc <= 0 || resolved.endUtc <= 0 || resolved.endUtc < resolved.startUtc) {
        return std::nullopt;
    }

    return resolved;
}

std::string formatReportWindowLabel(std::time_t start, std::time_t end) {
    return formatReportDayLabel(start) + " to " + formatReportDayLabel(end) + " UTC";
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

bool shouldUseQuotaSnapshotRollups(std::time_t start, std::time_t end) {
    if (start <= 0 || end <= 0 || end < start) {
        return false;
    }
    constexpr std::time_t kQuotaRollupThresholdSeconds = 14 * 86400;
    return (end - start) >= kQuotaRollupThresholdSeconds;
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

TrendBucketConfig resolveTrendBucketConfig(const ResolvedAnalysisWindow& window) {
    const std::time_t spanSeconds = std::max<std::time_t>(1, (window.end - window.start) + 1);
    const long long spanHours = static_cast<long long>((spanSeconds + 3599) / 3600);
    const long long spanDays = static_cast<long long>((spanSeconds + 86399) / 86400);

    if (spanHours <= 48) {
        return {AnalysisTrendBucketGranularity::Hourly, "Hourly"};
    }
    if (spanDays <= 45) {
        return {AnalysisTrendBucketGranularity::Daily, "Daily"};
    }
    if (spanDays <= 180) {
        return {AnalysisTrendBucketGranularity::Weekly, "Weekly"};
    }
    return {AnalysisTrendBucketGranularity::Monthly, "Monthly"};
}

std::time_t trendBucketStart(std::time_t value, AnalysisTrendBucketGranularity granularity) {
    switch (granularity) {
    case AnalysisTrendBucketGranularity::Hourly:
        return utcHourStart(value);
    case AnalysisTrendBucketGranularity::Weekly:
        return utcWeekStart(value);
    case AnalysisTrendBucketGranularity::Monthly:
        return utcMonthStart(value);
    case AnalysisTrendBucketGranularity::Daily:
    default:
        return utcDayStart(value);
    }
}

std::time_t nextTrendBucketStart(std::time_t value, AnalysisTrendBucketGranularity granularity) {
    switch (granularity) {
    case AnalysisTrendBucketGranularity::Hourly:
        return value + 3600;
    case AnalysisTrendBucketGranularity::Weekly:
        return value + (7 * 86400);
    case AnalysisTrendBucketGranularity::Monthly: {
        std::tm tm{};
        gmtime_s(&tm, &value);
        tm.tm_mday = 1;
        tm.tm_mon += 1;
        tm.tm_hour = 0;
        tm.tm_min = 0;
        tm.tm_sec = 0;
        return _mkgmtime(&tm);
    }
    case AnalysisTrendBucketGranularity::Daily:
    default:
        return value + 86400;
    }
}

std::string formatTrendBucketLabel(
    std::time_t startUtc,
    AnalysisTrendBucketGranularity granularity) {
    std::tm tm{};
    gmtime_s(&tm, &startUtc);
    char buffer[32]{};
    switch (granularity) {
    case AnalysisTrendBucketGranularity::Hourly:
        std::strftime(buffer, sizeof(buffer), "%m-%d %H:%M", &tm);
        return buffer;
    case AnalysisTrendBucketGranularity::Monthly:
        std::strftime(buffer, sizeof(buffer), "%Y-%m", &tm);
        return buffer;
    case AnalysisTrendBucketGranularity::Weekly:
    case AnalysisTrendBucketGranularity::Daily:
    default:
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &tm);
        return buffer;
    }
}

std::vector<TrendBucketWindow> buildTrendBucketWindows(
    const ResolvedAnalysisWindow& window,
    const TrendBucketConfig& config) {
    std::vector<TrendBucketWindow> buckets;
    if (window.end < window.start) {
        return buckets;
    }

    const std::time_t firstBucketStart = trendBucketStart(window.start, config.granularity);
    const std::time_t lastBucketStart = trendBucketStart(window.end, config.granularity);
    for (std::time_t bucketStart = firstBucketStart;
         bucketStart <= lastBucketStart;
         bucketStart = nextTrendBucketStart(bucketStart, config.granularity)) {
        TrendBucketWindow bucket;
        bucket.startUtc = bucketStart;
        bucket.endUtc = std::min(
            window.end,
            nextTrendBucketStart(bucketStart, config.granularity) - 1);
        bucket.label = formatTrendBucketLabel(bucketStart, config.granularity);
        buckets.push_back(std::move(bucket));
    }

    return buckets;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> splitCsv(const std::string& value) {
    std::vector<std::string> tokens;
    if (value.empty()) {
        return tokens;
    }

    size_t start = 0;
    while (start <= value.size()) {
        const size_t end = value.find(',', start);
        const std::string token = trim(value.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start));
        if (!token.empty()) {
            tokens.push_back(token);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    return tokens;
}

void appendScopeFilter(
    std::string& sql,
    std::vector<std::string>& params,
    const std::string& selectedValue,
    const char* columnName) {
    if (selectedValue.empty() || columnName == nullptr || *columnName == '\0') {
        return;
    }

    sql += " AND ";
    sql += columnName;
    sql += " = ?";
    params.push_back(selectedValue);
}

SourceFlags sourceFlagsFromSourceType(const std::string& rawValue) {
    SourceFlags flags;
    const std::string value = trim(rawValue);
    if (value.empty()) {
        flags.unknown = true;
        return flags;
    }

    if (value == "api") {
        flags.api = true;
        return flags;
    }

    if (value == "imported" ||
        value == "provider_backfill" ||
        value == "chatgpt_export") {
        flags.imported = true;
        return flags;
    }

    if (value == "local" ||
        value == "local_runtime" ||
        value == "codex_local_rollout" ||
        value == "codex_local_fixture") {
        flags.local = true;
        return flags;
    }

    if (value.find("import") != std::string::npos ||
        value.find("backfill") != std::string::npos) {
        flags.imported = true;
        return flags;
    }

    if (value.find("api") != std::string::npos) {
        flags.api = true;
        return flags;
    }

    if (value.find("local") != std::string::npos) {
        flags.local = true;
        return flags;
    }

    flags.unknown = true;
    return flags;
}

void applyProviderSurface(SourceFlags& flags, const std::string& providerSurface) {
    const std::string value = trim(providerSurface);
    if (value.empty()) {
        return;
    }

    if (value == "openai_subscription_local" ||
        value == "openai_subscription_codex_local" ||
        value == "claude_code_local_stats" ||
        value == "kimi_code_local" ||
        value == "kimi_local_runtime" ||
        value == "kimi_local_activity") {
        flags.local = true;
        return;
    }

    if (value == "openai_subscription_export_import" ||
        value == "openai_api_import") {
        flags.imported = true;
        return;
    }

    if (value == "minimax_subscription_coding_plan" || value == "minimax_subscription_token_plan" ||
        value == "kimi_subscription_api" || value == "grok_subscription_api" || value == "cursor_subscription_api") {
        flags.api = true;
    }
}

std::string sourceLabelForFlags(const SourceFlags& flags) {
    if (flags.categoryCount() == 0) {
        return "Unknown";
    }

    if (flags.rollupBacked &&
        !flags.local &&
        !flags.imported &&
        !flags.api &&
        !flags.unknown) {
        return "Rollup-backed";
    }

    if (flags.local &&
        !flags.rollupBacked &&
        !flags.imported &&
        !flags.api &&
        !flags.unknown) {
        return "Local";
    }

    if (flags.imported &&
        !flags.rollupBacked &&
        !flags.local &&
        !flags.api &&
        !flags.unknown) {
        return "Imported";
    }

    if (flags.api &&
        !flags.rollupBacked &&
        !flags.local &&
        !flags.imported &&
        !flags.unknown) {
        return "API";
    }

    if (flags.unknown &&
        !flags.rollupBacked &&
        !flags.local &&
        !flags.imported &&
        !flags.api) {
        return "Unknown";
    }

    if (flags.rollupBacked && (flags.local || flags.imported || flags.api) && !flags.unknown) {
        return "Mixed";
    }

    return "Mixed";
}

SourceFlags sourceFlagsFromRollupCounters(
    long long localRequestCount,
    long long importedRequestCount,
    long long apiRequestCount,
    long long unknownRequestCount,
    long long backfilledRequestCount) {
    SourceFlags flags;
    if (localRequestCount > 0) {
        flags.local = true;
    }
    if (importedRequestCount > 0 || backfilledRequestCount > 0) {
        flags.imported = true;
    }
    if (apiRequestCount > 0) {
        flags.api = true;
    }
    if (unknownRequestCount > 0) {
        flags.unknown = true;
    }
    if (flags.categoryCount() == 0) {
        flags.rollupBacked = true;
    }
    return flags;
}

std::string makeScopedDayKey(
    const std::string& dayLabel,
    const std::string& providerKey,
    const std::string& accountKey) {
    return dayLabel + "|" + providerKey + "|" + accountKey;
}

std::string makeScopedTrendKey(
    std::time_t bucketStartUtc,
    const std::string& providerKey,
    const std::string& accountKey) {
    return std::to_string(static_cast<long long>(bucketStartUtc)) + "|" + providerKey + "|" + accountKey;
}

AnalysisGapClassification toAnalysisGapClassification(const std::string& value) {
    if (value == "known_zero") {
        return AnalysisGapClassification::KnownZero;
    }
    if (value == "reconstructed") {
        return AnalysisGapClassification::Reconstructed;
    }
    if (value == "unknown") {
        return AnalysisGapClassification::Unknown;
    }
    return AnalysisGapClassification::None;
}

std::string gapClassificationLabel(AnalysisGapClassification classification) {
    switch (classification) {
    case AnalysisGapClassification::KnownZero:
        return "Known Zero";
    case AnalysisGapClassification::Reconstructed:
        return "Reconstructed";
    case AnalysisGapClassification::Unknown:
        return "Unknown";
    case AnalysisGapClassification::None:
    default:
        return "";
    }
}

std::string displayGapSourceKind(const std::string& rawValue) {
    const std::string value = trim(rawValue);
    if (value.empty()) {
        return "";
    }
    if (value == "provider_backfill") {
        return "Provider backfill";
    }
    if (value == "local_runtime") {
        return "Local runtime";
    }
    if (value == "local") {
        return "Local";
    }

    std::string display = value;
    std::replace(display.begin(), display.end(), '_', ' ');
    if (!display.empty()) {
        display[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(display[0])));
    }
    return display;
}

std::string summarizeGapSourceKinds(const std::set<std::string>& sourceKinds) {
    if (sourceKinds.empty()) {
        return "";
    }
    if (sourceKinds.size() == 1) {
        return *sourceKinds.begin();
    }

    std::ostringstream stream;
    size_t index = 0;
    for (const auto& sourceKind : sourceKinds) {
        if (index > 0) {
            stream << (index + 1 == sourceKinds.size() ? " and " : ", ");
        }
        stream << sourceKind;
        ++index;
    }
    return stream.str();
}

std::string summarizeGapBasis(
    const DayGapRollup& rollup,
    AnalysisGapClassification classification) {
    if (rollup.basisSummaries.size() == 1) {
        return rollup.basisSummaries.front();
    }

    if (classification == AnalysisGapClassification::Unknown) {
        std::ostringstream stream;
        stream << rollup.unknownMarkers << " scoped unknown gap marker";
        if (rollup.unknownMarkers != 1) {
            stream << "s";
        }
        return stream.str();
    }

    if (classification == AnalysisGapClassification::Reconstructed) {
        std::ostringstream stream;
        stream << rollup.reconstructedMarkers << " reconstructed gap marker";
        if (rollup.reconstructedMarkers != 1) {
            stream << "s";
        }
        return stream.str();
    }

    if (classification == AnalysisGapClassification::KnownZero) {
        std::ostringstream stream;
        stream << rollup.knownZeroMarkers << " known-zero gap marker";
        if (rollup.knownZeroMarkers != 1) {
            stream << "s";
        }
        return stream.str();
    }

    return "";
}

AnalysisGapDetail buildGapDetail(const DayGapRollup* rollup) {
    AnalysisGapDetail detail;
    if (rollup == nullptr) {
        return detail;
    }

    AnalysisGapClassification classification = AnalysisGapClassification::None;
    if (rollup->unknownMarkers > 0) {
        classification = AnalysisGapClassification::Unknown;
    } else if (rollup->reconstructedMarkers > 0) {
        classification = AnalysisGapClassification::Reconstructed;
    } else if (rollup->knownZeroMarkers > 0) {
        classification = AnalysisGapClassification::KnownZero;
    }

    detail.classification = classification;
    detail.classificationLabel = gapClassificationLabel(classification);
    detail.basisSummary = summarizeGapBasis(*rollup, classification);
    detail.sourceKind = summarizeGapSourceKinds(rollup->sourceKinds);
    return detail;
}

void addPersistedProviderOptions(
    StorageService& storage,
    std::vector<AnalysisScopeOption>& options,
    std::set<std::string>& seenProviders) {
    QueryResult result =
        storage.execute("SELECT p.provider_key, p.display_name "
                        "FROM providers p "
                        "WHERE EXISTS (SELECT 1 FROM usage_events ue WHERE ue.provider_id = p.id AND ue.source_type != "
                        "'codex_local_rollout') "
                        "   OR EXISTS (SELECT 1 FROM usage_rollups_daily r WHERE r.provider_id = p.id) "
                        "   OR EXISTS (SELECT 1 FROM activity_events ae WHERE ae.provider_id = p.id) "
                        "   OR EXISTS (SELECT 1 FROM quota_snapshots qs WHERE qs.provider_id = p.id) "
                        "   OR EXISTS (SELECT 1 FROM gap_markers gm WHERE gm.provider_id = p.id) "
                        "   OR EXISTS (SELECT 1 FROM recovery_checkpoints rc WHERE rc.provider_id = p.id) "
                        "ORDER BY p.display_name, p.provider_key;");
    if (!result.success()) {
        return;
    }

    for (size_t row = 0; row < result.rowCount(); ++row) {
        const std::string key = result.getString(row, 0);
        if (key.empty() || !seenProviders.insert(key).second) {
            continue;
        }

        const std::string label = result.getString(row, 1);
        options.push_back({key, label.empty() ? key : label});
    }
}

void addPersistedAccountOptions(
    StorageService& storage,
    const std::string& selectedProviderId,
    std::vector<AnalysisScopeOption>& options,
    std::set<std::string>& seenAccounts) {
    std::string sql = "SELECT a.account_key, a.display_name, p.display_name "
                      "FROM accounts a "
                      "JOIN providers p ON p.id = a.provider_id "
                      "WHERE ("
                      "    EXISTS (SELECT 1 FROM usage_events ue WHERE ue.account_id = a.id AND ue.source_type != "
                      "'codex_local_rollout') "
                      "    OR EXISTS (SELECT 1 FROM usage_rollups_daily r WHERE r.provider_id = a.provider_id AND "
                      "r.account_id = a.id) "
                      "    OR EXISTS (SELECT 1 FROM activity_events ae WHERE ae.account_id = a.id) "
                      "    OR EXISTS (SELECT 1 FROM quota_snapshots qs WHERE qs.account_id = a.id) "
                      "    OR EXISTS (SELECT 1 FROM gap_markers gm WHERE gm.account_id = a.id) "
                      "    OR EXISTS (SELECT 1 FROM recovery_checkpoints rc WHERE rc.account_id = a.id)"
                      ")";
    std::vector<std::string> params;
    if (!selectedProviderId.empty()) {
        sql += " AND p.provider_key = ?";
        params.push_back(selectedProviderId);
    }
    sql += " ORDER BY p.display_name, a.display_name, a.account_key;";

    QueryResult result = storage.execute(sql, params);
    if (!result.success()) {
        return;
    }

    for (size_t row = 0; row < result.rowCount(); ++row) {
        const std::string key = result.getString(row, 0);
        if (key.empty() || !seenAccounts.insert(key).second) {
            continue;
        }

        const std::string accountLabel = result.getString(row, 1);
        const std::string providerLabel = result.getString(row, 2);
        const std::string labelBase = accountLabel.empty() ? key : accountLabel;
        const std::string label = providerLabel.empty()
            ? labelBase
            : providerLabel + " / " + labelBase;
        options.push_back({key, label});
    }
}

void accumulateScopedBucket(
    std::map<std::string, ScopedHistoryBucket>& buckets,
    const std::string& dayLabel,
    const std::string& providerKey,
    const std::string& providerLabel,
    const std::string& accountKey,
    const std::string& accountLabel,
    long long activityCount,
    const SourceFlags& sources,
    bool hasQuantifiedUsage,
    bool hasActivityFallback) {
    const std::string bucketKey = makeScopedDayKey(dayLabel, providerKey, accountKey);
    auto& bucket = buckets[bucketKey];
    bucket.dayLabel = dayLabel;
    bucket.providerKey = providerKey;
    bucket.providerLabel = providerLabel.empty() ? providerKey : providerLabel;
    bucket.accountKey = accountKey;
    bucket.accountLabel = accountLabel.empty() ? accountKey : accountLabel;
    bucket.activityCount += activityCount;
    bucket.hasQuantifiedUsage = bucket.hasQuantifiedUsage || hasQuantifiedUsage;
    bucket.hasActivityFallback = bucket.hasActivityFallback || hasActivityFallback;
    bucket.sources.merge(sources);
}

void accumulateScopedTrendBucket(
    std::map<std::string, ScopedTrendBucket>& buckets,
    std::time_t bucketStartUtc,
    std::time_t bucketEndUtc,
    const std::string& bucketLabel,
    const std::string& providerKey,
    const std::string& providerLabel,
    const std::string& accountKey,
    const std::string& accountLabel,
    long long activityCount,
    const SourceFlags& sources,
    bool hasQuantifiedUsage,
    bool hasActivityFallback) {
    const std::string bucketKey = makeScopedTrendKey(bucketStartUtc, providerKey, accountKey);
    auto& bucket = buckets[bucketKey];
    bucket.bucketStartUtc = bucketStartUtc;
    bucket.bucketEndUtc = bucketEndUtc;
    bucket.bucketLabel = bucketLabel;
    bucket.providerKey = providerKey;
    bucket.providerLabel = providerLabel.empty() ? providerKey : providerLabel;
    bucket.accountKey = accountKey;
    bucket.accountLabel = accountLabel.empty() ? accountKey : accountLabel;
    bucket.activityCount += activityCount;
    bucket.hasQuantifiedUsage = bucket.hasQuantifiedUsage || hasQuantifiedUsage;
    bucket.hasActivityFallback = bucket.hasActivityFallback || hasActivityFallback;
    bucket.sources.merge(sources);
}

} // namespace

AnalysisQueryEngine::AnalysisQueryEngine(StorageService& storage)
    : storage_(&storage) {}

ResolvedAnalysisWindow AnalysisQueryEngine::resolveWindow(
    const DashboardPreferences& preferences,
    std::time_t now) const {
    ResolvedAnalysisWindow window;
    const AnalysisDateRangePreset preset = preferences.analysisDateRangePreset;
    switch (preset) {
    case AnalysisDateRangePreset::Today:
        window.start = utcDayStart(now);
        window.end = now;
        window.label = "Today";
        window.effectivePreset = AnalysisDateRangePreset::Today;
        break;
    case AnalysisDateRangePreset::Last24Hours:
        window.start = std::max<std::time_t>(0, now - (24 * 3600));
        window.end = now;
        window.label = "Last 24 Hours";
        window.effectivePreset = AnalysisDateRangePreset::Last24Hours;
        break;
    case AnalysisDateRangePreset::CurrentQuotaWindow:
        if (const auto quotaWindow = resolveLatestQuotaWindow(
                *storage_,
                preferences.analysisSelectedProviderId,
                preferences.analysisSelectedAccountId,
                now)) {
            window.start = quotaWindow->startUtc;
            window.end = quotaWindow->endUtc;
            window.label = "Current Billing/Quota Window";
            window.effectivePreset = AnalysisDateRangePreset::CurrentQuotaWindow;
            break;
        }
        [[fallthrough]];
    case AnalysisDateRangePreset::ThisMonth:
        window.start = utcMonthStart(now);
        window.end = now;
        window.label = "This Month";
        window.effectivePreset = AnalysisDateRangePreset::ThisMonth;
        break;
    case AnalysisDateRangePreset::Custom:
        if (preferences.analysisCustomRangeStartUtc > 0 &&
            preferences.analysisCustomRangeEndUtc > 0) {
            window.start = utcDayStart(preferences.analysisCustomRangeStartUtc);
            window.end = utcDayStart(preferences.analysisCustomRangeEndUtc) + 86399;
            if (window.end > now) {
                window.end = now;
            }
            if (window.start > window.end) {
                std::swap(window.start, window.end);
            }
            window.label = formatReportWindowLabel(window.start, window.end);
            window.effectivePreset = AnalysisDateRangePreset::Custom;
            break;
        }
        [[fallthrough]];
    case AnalysisDateRangePreset::Last7Days:
    default:
        window.start = std::max<std::time_t>(0, utcDayStart(now) - (6 * 86400));
        window.end = now;
        window.label = "Last 7 Days";
        window.effectivePreset = AnalysisDateRangePreset::Last7Days;
        break;
    }

    window.reportDateRange.start = std::chrono::system_clock::from_time_t(window.start);
    window.reportDateRange.end = std::chrono::system_clock::from_time_t(window.end);
    return window;
}

AnalysisViewModel AnalysisQueryEngine::buildViewModel(
    const DashboardViewModel& sourceViewModel,
    const DashboardPreferences& preferences,
    std::time_t now) const {
    AnalysisViewModel analysis;
    analysis.scope.selectedProviderId = preferences.analysisSelectedProviderId;
    analysis.scope.selectedAccountId = preferences.analysisSelectedAccountId;
    analysis.scope.customRangeStartUtc = preferences.analysisCustomRangeStartUtc;
    analysis.scope.customRangeEndUtc = preferences.analysisCustomRangeEndUtc;

    analysis.scope.dateRangePreset = preferences.analysisDateRangePreset;
    analysis.requestedScope = analysis.scope;

    const ResolvedAnalysisWindow window = resolveWindow(preferences, now);
    analysis.scope.dateRangePreset = window.effectivePreset;
    analysis.effectiveStartUtc = window.start;
    analysis.effectiveEndUtc = window.end;
    const TrendBucketConfig trendBucketConfig = resolveTrendBucketConfig(window);
    analysis.trendBucketGranularity = trendBucketConfig.granularity;
    analysis.trendBucketLabel = trendBucketConfig.label;

    std::set<std::string> seenProviders;
    std::set<std::string> seenAccounts;
    analysis.availableProviders.push_back({"", "All Providers"});
    analysis.availableAccounts.push_back({"", "All Accounts"});

    auto addProviderOption = [&](const std::string& key, const std::string& label) {
        if (key.empty() || !seenProviders.insert(key).second) {
            return;
        }
        analysis.availableProviders.push_back({key, label.empty() ? key : label});
    };

    auto addAccountOption = [&](const std::string& key, const std::string& label) {
        if (key.empty() || !seenAccounts.insert(key).second) {
            return;
        }
        analysis.availableAccounts.push_back({key, label.empty() ? key : label});
    };

    if (storage_ != nullptr && storage_->isOpen()) {
        addPersistedProviderOptions(*storage_, analysis.availableProviders, seenProviders);
        addPersistedAccountOptions(
            *storage_,
            analysis.scope.selectedProviderId,
            analysis.availableAccounts,
            seenAccounts);
    }

    for (const auto& provider : sourceViewModel.providers) {
        addProviderOption(provider.providerKey, provider.displayName);
        if (!provider.accountId.empty() &&
            (analysis.scope.selectedProviderId.empty() ||
             provider.providerKey == analysis.scope.selectedProviderId)) {
            addAccountOption(provider.accountId, provider.displayName + " / " + provider.accountId);
        }
    }

    if (!analysis.scope.selectedProviderId.empty() &&
        seenProviders.find(analysis.scope.selectedProviderId) == seenProviders.end()) {
        analysis.scope.selectedProviderId.clear();
    }
    if (!analysis.scope.selectedAccountId.empty() &&
        seenAccounts.find(analysis.scope.selectedAccountId) == seenAccounts.end()) {
        analysis.scope.selectedAccountId.clear();
    }

    const auto providerOption = std::find_if(
        analysis.availableProviders.begin(),
        analysis.availableProviders.end(),
        [&analysis](const AnalysisScopeOption& option) {
            return option.key == analysis.scope.selectedProviderId;
        });
    const auto accountOption = std::find_if(
        analysis.availableAccounts.begin(),
        analysis.availableAccounts.end(),
        [&analysis](const AnalysisScopeOption& option) {
            return option.key == analysis.scope.selectedAccountId;
        });

    analysis.scopeLabel =
        (providerOption != analysis.availableProviders.end() ? providerOption->label : "All Providers") +
        " / " +
        (accountOption != analysis.availableAccounts.end() ? accountOption->label : "All Accounts") +
        " / " +
        window.label;

    if (storage_ == nullptr || !storage_->isOpen()) {
        analysis.dataQualityNote = "History analysis is unavailable until JiStats storage is ready.";
        return analysis;
    }

    std::map<std::string, ScopedHistoryBucket> scopedBuckets;
    std::set<std::string> usageBackedScopedDays;
    std::map<std::string, ScopedTrendBucket> scopedTrendBuckets;
    std::set<std::string> usageBackedScopedTrendBuckets;

    auto accumulateUsageQuery = [&](const QueryResult& result, bool fromRollup) {
        if (!result.success()) {
            return;
        }

        for (size_t row = 0; row < result.rowCount(); ++row) {
            SourceFlags flags;
            if (fromRollup) {
                flags = sourceFlagsFromRollupCounters(
                    result.getInt64(row, 8),
                    result.getInt64(row, 9),
                    result.getInt64(row, 10),
                    result.getInt64(row, 11),
                    result.getInt64(row, 12));
            } else {
                const auto sourceTypes = splitCsv(result.getString(row, 6));
                if (sourceTypes.empty()) {
                    flags.unknown = true;
                } else {
                    for (const auto& sourceType : sourceTypes) {
                        flags.merge(sourceFlagsFromSourceType(sourceType));
                    }
                }
                if (result.getInt64(row, 7) > 0) {
                    flags.imported = true;
                }
            }

            const std::string dayLabel = result.getString(row, 0);
            const std::string providerKey = result.getString(row, 1);
            const std::string providerLabel = result.getString(row, 2);
            const std::string accountKey = result.getString(row, 3);
            const std::string accountLabel = result.getString(row, 4);
            const long long activityCount = result.getInt64(row, 5);

            accumulateScopedBucket(
                scopedBuckets,
                dayLabel,
                providerKey,
                providerLabel,
                accountKey,
                accountLabel,
                activityCount,
                flags,
                true,
                false);
            usageBackedScopedDays.insert(makeScopedDayKey(dayLabel, providerKey, accountKey));
        }
    };

    auto queryUsageRaw = [&](std::time_t from, std::time_t to, bool missingDailyRollupsOnly) {
        if (from > to) {
            return;
        }

        std::string sql =
            "SELECT substr(ue.event_time_utc, 1, 10), "
            "p.provider_key, p.display_name, a.account_key, a.display_name, "
            "COUNT(*), "
            "COALESCE(GROUP_CONCAT(DISTINCT ue.source_type), ''), "
            "MAX(CASE WHEN ue.is_backfilled = 1 THEN 1 ELSE 0 END) "
            "FROM usage_events ue "
            "JOIN providers p ON p.id = ue.provider_id "
            "JOIN accounts a ON a.id = ue.account_id "
            "WHERE ue.event_time_utc >= ? AND ue.event_time_utc <= ?";
        std::vector<std::string> params{formatTimeUtcIso(from), formatTimeUtcIso(to)};
        appendScopeFilter(sql, params, analysis.scope.selectedProviderId, "p.provider_key");
        appendScopeFilter(sql, params, analysis.scope.selectedAccountId, "a.account_key");
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
        sql +=
            " GROUP BY substr(ue.event_time_utc, 1, 10), "
            "p.provider_key, p.display_name, a.account_key, a.display_name "
            "ORDER BY substr(ue.event_time_utc, 1, 10) ASC, p.display_name, a.display_name;";
        accumulateUsageQuery(storage_->execute(sql, params), false);
    };

    auto queryUsageRollups = [&](std::time_t fromDayStart, std::time_t toDayStart) {
        if (fromDayStart > toDayStart) {
            return;
        }

        std::string sql =
            "SELECT substr(r.rollup_period_start_utc, 1, 10), "
            "p.provider_key, p.display_name, a.account_key, a.display_name, "
            "COALESCE(SUM(r.request_count), 0), "
            "'' AS source_types, 0 AS has_backfilled, "
            "COALESCE(SUM(r.local_request_count), 0), "
            "COALESCE(SUM(r.imported_request_count), 0), "
            "COALESCE(SUM(r.api_request_count), 0), "
            "COALESCE(SUM(r.unknown_request_count), 0), "
            "COALESCE(SUM(r.backfilled_request_count), 0) "
            "FROM usage_rollups_daily r "
            "JOIN providers p ON p.id = r.provider_id "
            "JOIN accounts a ON a.id = r.account_id "
            "WHERE r.rollup_period_start_utc >= ? AND r.rollup_period_start_utc <= ?";
        std::vector<std::string> params{
            formatTimeUtcIso(fromDayStart),
            formatTimeUtcIso(toDayStart)
        };
        appendScopeFilter(sql, params, analysis.scope.selectedProviderId, "p.provider_key");
        appendScopeFilter(sql, params, analysis.scope.selectedAccountId, "a.account_key");
        sql +=
            " GROUP BY substr(r.rollup_period_start_utc, 1, 10), "
            "p.provider_key, p.display_name, a.account_key, a.display_name "
            "ORDER BY substr(r.rollup_period_start_utc, 1, 10) ASC, p.display_name, a.display_name;";
        accumulateUsageQuery(storage_->execute(sql, params), true);
    };

    const auto fullDays = fullyCoveredDailyRange(window.start, window.end);
    if (!fullDays.has_value()) {
        queryUsageRaw(window.start, window.end, false);
    } else {
        if (window.start < fullDays->first) {
            queryUsageRaw(window.start, fullDays->first - 1, false);
        }
        queryUsageRollups(fullDays->first, fullDays->second);
        queryUsageRaw(fullDays->first, fullDays->second + 86399, true);
        const std::time_t trailingStart = fullDays->second + 86400;
        if (trailingStart <= window.end) {
            queryUsageRaw(trailingStart, window.end, false);
        }
    }

    {
        std::string sql =
            "SELECT substr(ae.started_at_utc, 1, 10), "
            "p.provider_key, p.display_name, a.account_key, a.display_name, "
            "COUNT(*), "
            "COALESCE(GROUP_CONCAT(DISTINCT ae.source_type), ''), "
            "COALESCE(GROUP_CONCAT(DISTINCT ae.provider_surface), '') "
            "FROM activity_events ae "
            "JOIN providers p ON p.id = ae.provider_id "
            "JOIN accounts a ON a.id = ae.account_id "
            "WHERE ae.started_at_utc >= ? AND ae.started_at_utc <= ?";
        std::vector<std::string> params{
            formatTimeUtcIso(window.start),
            formatTimeUtcIso(window.end)
        };
        appendScopeFilter(sql, params, analysis.scope.selectedProviderId, "p.provider_key");
        appendScopeFilter(sql, params, analysis.scope.selectedAccountId, "a.account_key");
        sql +=
            " GROUP BY substr(ae.started_at_utc, 1, 10), "
            "p.provider_key, p.display_name, a.account_key, a.display_name "
            "ORDER BY substr(ae.started_at_utc, 1, 10) ASC, p.display_name, a.display_name;";

        QueryResult result = storage_->execute(sql, params);
        if (result.success()) {
            for (size_t row = 0; row < result.rowCount(); ++row) {
                const std::string dayLabel = result.getString(row, 0);
                const std::string providerKey = result.getString(row, 1);
                const std::string accountKey = result.getString(row, 3);
                if (usageBackedScopedDays.find(makeScopedDayKey(dayLabel, providerKey, accountKey)) !=
                    usageBackedScopedDays.end()) {
                    continue;
                }

                SourceFlags flags;
                const auto sourceTypes = splitCsv(result.getString(row, 6));
                const auto providerSurfaces = splitCsv(result.getString(row, 7));
                if (sourceTypes.empty() && providerSurfaces.empty()) {
                    flags.unknown = true;
                } else {
                    for (const auto& sourceType : sourceTypes) {
                        flags.merge(sourceFlagsFromSourceType(sourceType));
                    }
                    for (const auto& providerSurface : providerSurfaces) {
                        applyProviderSurface(flags, providerSurface);
                    }
                }

                accumulateScopedBucket(
                    scopedBuckets,
                    dayLabel,
                    providerKey,
                    result.getString(row, 2),
                    accountKey,
                    result.getString(row, 4),
                    result.getInt64(row, 5),
                    flags,
                    false,
                    true);
            }
        }
    }

    if (trendBucketConfig.granularity == AnalysisTrendBucketGranularity::Hourly) {
        auto accumulateHourlyUsageQuery = [&](const QueryResult& result, bool fromRollup) {
            if (!result.success()) {
                return;
            }

            for (size_t row = 0; row < result.rowCount(); ++row) {
                const auto bucketStartValue = parseUtcIso(result.getString(row, 0));
                if (!bucketStartValue.has_value()) {
                    continue;
                }

                SourceFlags flags;
                if (fromRollup) {
                    flags = sourceFlagsFromRollupCounters(
                        result.getInt64(row, 8),
                        result.getInt64(row, 9),
                        result.getInt64(row, 10),
                        result.getInt64(row, 11),
                        result.getInt64(row, 12));
                } else {
                    const auto sourceTypes = splitCsv(result.getString(row, 6));
                    if (sourceTypes.empty()) {
                        flags.unknown = true;
                    } else {
                        for (const auto& sourceType : sourceTypes) {
                            flags.merge(sourceFlagsFromSourceType(sourceType));
                        }
                    }
                    if (result.getInt64(row, 7) > 0) {
                        flags.imported = true;
                    }
                }

                const std::time_t bucketStartUtc = *bucketStartValue;
                accumulateScopedTrendBucket(
                    scopedTrendBuckets,
                    bucketStartUtc,
                    std::min(window.end, bucketStartUtc + 3599),
                    formatTrendBucketLabel(bucketStartUtc, trendBucketConfig.granularity),
                    result.getString(row, 1),
                    result.getString(row, 2),
                    result.getString(row, 3),
                    result.getString(row, 4),
                    result.getInt64(row, 5),
                    flags,
                    true,
                    false);
                usageBackedScopedTrendBuckets.insert(makeScopedTrendKey(
                    bucketStartUtc,
                    result.getString(row, 1),
                    result.getString(row, 3)));
            }
        };

        auto queryHourlyUsageRaw = [&](std::time_t from, std::time_t to, bool missingHourlyRollupsOnly) {
            if (from > to) {
                return;
            }

            std::string sql =
                "SELECT substr(ue.event_time_utc, 1, 13) || ':00:00Z', "
                "p.provider_key, p.display_name, a.account_key, a.display_name, "
                "COUNT(*), "
                "COALESCE(GROUP_CONCAT(DISTINCT ue.source_type), ''), "
                "MAX(CASE WHEN ue.is_backfilled = 1 THEN 1 ELSE 0 END) "
                "FROM usage_events ue "
                "JOIN providers p ON p.id = ue.provider_id "
                "JOIN accounts a ON a.id = ue.account_id "
                "WHERE ue.event_time_utc >= ? AND ue.event_time_utc <= ?";
            std::vector<std::string> params{formatTimeUtcIso(from), formatTimeUtcIso(to)};
            appendScopeFilter(sql, params, analysis.scope.selectedProviderId, "p.provider_key");
            appendScopeFilter(sql, params, analysis.scope.selectedAccountId, "a.account_key");
            sql += " AND ue.source_type != 'codex_local_rollout'";
            if (missingHourlyRollupsOnly) {
                sql +=
                    " AND NOT EXISTS ("
                    "    SELECT 1 FROM usage_rollups_hourly r "
                    "    WHERE r.provider_id = ue.provider_id "
                    "      AND r.account_id = ue.account_id "
                    "      AND r.model_key = COALESCE(ue.model_key, '') "
                    "      AND r.rollup_period_start_utc = substr(ue.event_time_utc, 1, 13) || ':00:00Z'"
                    ")";
            }
            sql +=
                " GROUP BY substr(ue.event_time_utc, 1, 13), "
                "p.provider_key, p.display_name, a.account_key, a.display_name "
                "ORDER BY substr(ue.event_time_utc, 1, 13) ASC, p.display_name, a.display_name;";
            accumulateHourlyUsageQuery(storage_->execute(sql, params), false);
        };

        auto queryHourlyUsageRollups = [&](std::time_t fromHourStart, std::time_t toHourStart) {
            if (fromHourStart > toHourStart) {
                return;
            }

            std::string sql =
                "SELECT r.rollup_period_start_utc, "
                "p.provider_key, p.display_name, a.account_key, a.display_name, "
                "COALESCE(SUM(r.request_count), 0), "
                "'' AS source_types, 0 AS has_backfilled, "
                "COALESCE(SUM(r.local_request_count), 0), "
                "COALESCE(SUM(r.imported_request_count), 0), "
                "COALESCE(SUM(r.api_request_count), 0), "
                "COALESCE(SUM(r.unknown_request_count), 0), "
                "COALESCE(SUM(r.backfilled_request_count), 0) "
                "FROM usage_rollups_hourly r "
                "JOIN providers p ON p.id = r.provider_id "
                "JOIN accounts a ON a.id = r.account_id "
                "WHERE r.rollup_period_start_utc >= ? AND r.rollup_period_start_utc <= ?";
            std::vector<std::string> params{
                formatTimeUtcIso(fromHourStart),
                formatTimeUtcIso(toHourStart)
            };
            appendScopeFilter(sql, params, analysis.scope.selectedProviderId, "p.provider_key");
            appendScopeFilter(sql, params, analysis.scope.selectedAccountId, "a.account_key");
            sql +=
                " GROUP BY r.rollup_period_start_utc, "
                "p.provider_key, p.display_name, a.account_key, a.display_name "
                "ORDER BY r.rollup_period_start_utc ASC, p.display_name, a.display_name;";
            accumulateHourlyUsageQuery(storage_->execute(sql, params), true);
        };

        const auto fullHours = fullyCoveredHourlyRange(window.start, window.end);
        if (!fullHours.has_value()) {
            queryHourlyUsageRaw(window.start, window.end, false);
        } else {
            if (window.start < fullHours->first) {
                queryHourlyUsageRaw(window.start, fullHours->first - 1, false);
            }
            queryHourlyUsageRollups(fullHours->first, fullHours->second);
            queryHourlyUsageRaw(fullHours->first, fullHours->second + 3599, true);
            const std::time_t trailingStart = fullHours->second + 3600;
            if (trailingStart <= window.end) {
                queryHourlyUsageRaw(trailingStart, window.end, false);
            }
        }

        std::string sql =
            "SELECT substr(ae.started_at_utc, 1, 13) || ':00:00Z', "
            "p.provider_key, p.display_name, a.account_key, a.display_name, "
            "COUNT(*), "
            "COALESCE(GROUP_CONCAT(DISTINCT ae.source_type), ''), "
            "COALESCE(GROUP_CONCAT(DISTINCT ae.provider_surface), '') "
            "FROM activity_events ae "
            "JOIN providers p ON p.id = ae.provider_id "
            "JOIN accounts a ON a.id = ae.account_id "
            "WHERE ae.started_at_utc >= ? AND ae.started_at_utc <= ?";
        std::vector<std::string> params{
            formatTimeUtcIso(window.start),
            formatTimeUtcIso(window.end)
        };
        appendScopeFilter(sql, params, analysis.scope.selectedProviderId, "p.provider_key");
        appendScopeFilter(sql, params, analysis.scope.selectedAccountId, "a.account_key");
        sql +=
            " GROUP BY substr(ae.started_at_utc, 1, 13), "
            "p.provider_key, p.display_name, a.account_key, a.display_name "
            "ORDER BY substr(ae.started_at_utc, 1, 13) ASC, p.display_name, a.display_name;";

        QueryResult result = storage_->execute(sql, params);
        if (result.success()) {
            for (size_t row = 0; row < result.rowCount(); ++row) {
                const auto bucketStartValue = parseUtcIso(result.getString(row, 0));
                if (!bucketStartValue.has_value()) {
                    continue;
                }

                const std::string providerKey = result.getString(row, 1);
                const std::string accountKey = result.getString(row, 3);
                if (usageBackedScopedTrendBuckets.find(makeScopedTrendKey(
                        *bucketStartValue,
                        providerKey,
                        accountKey)) != usageBackedScopedTrendBuckets.end()) {
                    continue;
                }

                SourceFlags flags;
                const auto sourceTypes = splitCsv(result.getString(row, 6));
                const auto providerSurfaces = splitCsv(result.getString(row, 7));
                if (sourceTypes.empty() && providerSurfaces.empty()) {
                    flags.unknown = true;
                } else {
                    for (const auto& sourceType : sourceTypes) {
                        flags.merge(sourceFlagsFromSourceType(sourceType));
                    }
                    for (const auto& providerSurface : providerSurfaces) {
                        applyProviderSurface(flags, providerSurface);
                    }
                }

                accumulateScopedTrendBucket(
                    scopedTrendBuckets,
                    *bucketStartValue,
                    std::min(window.end, *bucketStartValue + 3599),
                    formatTrendBucketLabel(*bucketStartValue, trendBucketConfig.granularity),
                    providerKey,
                    result.getString(row, 2),
                    accountKey,
                    result.getString(row, 4),
                    result.getInt64(row, 5),
                    flags,
                    false,
                    true);
            }
        }
    } else {
        for (const auto& entry : scopedBuckets) {
            const auto bucketStartValue = parseUtcIso(entry.second.dayLabel + "T00:00:00Z");
            if (!bucketStartValue.has_value()) {
                continue;
            }

            const std::time_t bucketStartUtc =
                trendBucketStart(*bucketStartValue, trendBucketConfig.granularity);
            accumulateScopedTrendBucket(
                scopedTrendBuckets,
                bucketStartUtc,
                std::min(window.end, nextTrendBucketStart(bucketStartUtc, trendBucketConfig.granularity) - 1),
                formatTrendBucketLabel(bucketStartUtc, trendBucketConfig.granularity),
                entry.second.providerKey,
                entry.second.providerLabel,
                entry.second.accountKey,
                entry.second.accountLabel,
                entry.second.activityCount,
                entry.second.sources,
                entry.second.hasQuantifiedUsage,
                entry.second.hasActivityFallback);
        }
    }

    std::map<std::string, long long> dailyCounts;
    std::map<std::string, SourceFlags> dailySources;
    std::map<std::string, AnalysisBreakdownRow> providerRows;
    std::map<std::string, AnalysisBreakdownRow> accountRows;
    std::map<std::string, SourceFlags> providerSources;
    std::map<std::string, SourceFlags> accountSources;
    std::set<std::string> usageBackedDays;
    std::set<std::string> activityFallbackDays;

    for (const auto& entry : scopedBuckets) {
        const auto& bucket = entry.second;
        dailyCounts[bucket.dayLabel] += bucket.activityCount;
        dailySources[bucket.dayLabel].merge(bucket.sources);

        if (bucket.hasQuantifiedUsage) {
            usageBackedDays.insert(bucket.dayLabel);
        }
        if (bucket.hasActivityFallback && !bucket.hasQuantifiedUsage) {
            activityFallbackDays.insert(bucket.dayLabel);
        }

        auto& providerRow = providerRows[bucket.providerKey];
        providerRow.key = bucket.providerKey;
        providerRow.label = bucket.providerLabel;
        providerRow.activityCount += bucket.activityCount;
        providerSources[bucket.providerKey].merge(bucket.sources);

        const std::string accountRowKey = bucket.providerKey + "|" + bucket.accountKey;
        auto& accountRow = accountRows[accountRowKey];
        accountRow.key = accountRowKey;
        accountRow.label = bucket.accountLabel;
        accountRow.secondaryLabel = bucket.providerLabel;
        accountRow.activityCount += bucket.activityCount;
        accountSources[accountRowKey].merge(bucket.sources);
    }

    std::map<std::string, DayGapRollup> dayGapRollups;
    {
        std::string sql = "SELECT gm.period_start_utc, gm.period_end_utc, gm.classification, "
                          "CASE WHEN gm.classification = 'unknown' AND gm.source_kind = 'runtime_recovery' AND ("
                          "  (p.provider_key = 'openai' AND gm.basis_summary IN ("
                          "    'Provider backfill returned no history, but JiStats has local quantified evidence for "
                          "the same recovery window',"
                          "    'No provider-imported history or local quantified evidence was available for the "
                          "recovery window')) OR "
                          "  (p.provider_key = 'minimax' AND gm.basis_summary = "
                          "    'JiStats could not retrieve provider backfill during the recovery window')) "
                          "THEN 'JiStats was not fully observing provider state during the recovery window' "
                          "ELSE COALESCE(gm.basis_summary, '') END, COALESCE(gm.source_kind, '') "
                          "FROM gap_markers gm "
                          "JOIN providers p ON p.id = gm.provider_id "
                          "JOIN accounts a ON a.id = gm.account_id "
                          "WHERE gm.period_end_utc >= ? AND gm.period_start_utc <= ?";
        std::vector<std::string> params{
            formatTimeUtcIso(window.start),
            formatTimeUtcIso(window.end)
        };
        appendScopeFilter(sql, params, analysis.scope.selectedProviderId, "p.provider_key");
        appendScopeFilter(sql, params, analysis.scope.selectedAccountId, "a.account_key");
        sql += " ORDER BY gm.period_start_utc ASC, gm.period_end_utc ASC;";

        QueryResult result = storage_->execute(sql, params);
        if (result.success()) {
            const std::time_t firstDay = utcDayStart(window.start);
            const std::time_t lastDay = utcDayStart(window.end);
            for (size_t row = 0; row < result.rowCount(); ++row) {
                std::time_t markerStart = 0;
                std::time_t markerEnd = 0;
                const std::string startValue = result.getString(row, 0);
                const std::string endValue = result.getString(row, 1);
                if (startValue.empty() || endValue.empty()) {
                    continue;
                }

                std::tm tmStart{};
                std::tm tmEnd{};
                std::istringstream startStream(startValue);
                std::istringstream endStream(endValue);
                startStream >> std::get_time(&tmStart, "%Y-%m-%dT%H:%M:%SZ");
                endStream >> std::get_time(&tmEnd, "%Y-%m-%dT%H:%M:%SZ");
                if (startStream.fail() || endStream.fail()) {
                    continue;
                }
                markerStart = _mkgmtime(&tmStart);
                markerEnd = _mkgmtime(&tmEnd);

                const std::time_t clampedStart = std::max(firstDay, utcDayStart(markerStart));
                const std::time_t clampedEnd = std::min(lastDay, utcDayStart(markerEnd));
                if (clampedStart > clampedEnd) {
                    continue;
                }

                const AnalysisGapClassification classification =
                    toAnalysisGapClassification(result.getString(row, 2));
                const std::string basisSummary = result.getString(row, 3);
                const std::string sourceKind = displayGapSourceKind(result.getString(row, 4));
                for (std::time_t day = clampedStart; day <= clampedEnd; day += 86400) {
                    auto& rollup = dayGapRollups[formatReportDayLabel(day)];
                    switch (classification) {
                    case AnalysisGapClassification::KnownZero:
                        ++rollup.knownZeroMarkers;
                        break;
                    case AnalysisGapClassification::Reconstructed:
                        ++rollup.reconstructedMarkers;
                        break;
                    case AnalysisGapClassification::Unknown:
                        ++rollup.unknownMarkers;
                        break;
                    case AnalysisGapClassification::None:
                    default:
                        break;
                    }
                    if (!basisSummary.empty()) {
                        rollup.basisSummaries.push_back(basisSummary);
                    }
                    if (!sourceKind.empty()) {
                        rollup.sourceKinds.insert(sourceKind);
                    }
                }
            }
        }
    }

    const std::vector<TrendBucketWindow> trendBucketWindows =
        buildTrendBucketWindows(window, trendBucketConfig);
    std::map<std::time_t, long long> trendCounts;
    std::map<std::time_t, SourceFlags> trendSources;
    for (const auto& entry : scopedTrendBuckets) {
        const auto& bucket = entry.second;
        trendCounts[bucket.bucketStartUtc] += bucket.activityCount;
        trendSources[bucket.bucketStartUtc].merge(bucket.sources);
    }

    const std::time_t firstDay = utcDayStart(window.start);
    const std::time_t lastDay = utcDayStart(window.end);
    for (std::time_t day = firstDay; day <= lastDay; day += 86400) {
        AnalysisDailySummary summary;
        summary.dateLabel = formatReportDayLabel(day);
        if (const auto countIt = dailyCounts.find(summary.dateLabel); countIt != dailyCounts.end()) {
            summary.activityCount = countIt->second;
        }

        const auto sourceIt = dailySources.find(summary.dateLabel);
        if (sourceIt != dailySources.end()) {
            summary.sourceLabel = sourceLabelForFlags(sourceIt->second);
        } else {
            summary.sourceLabel = "Unknown";
        }

        const auto gapIt = dayGapRollups.find(summary.dateLabel);
        if (gapIt != dayGapRollups.end()) {
            summary.gapDetail = buildGapDetail(&gapIt->second);
        }

        if (summary.activityCount == 0 && summary.gapDetail.classification != AnalysisGapClassification::None) {
            if (summary.gapDetail.classification == AnalysisGapClassification::KnownZero) {
                summary.sourceLabel = "Known zero";
            } else if (summary.gapDetail.classification == AnalysisGapClassification::Reconstructed) {
                summary.sourceLabel = "Reconstructed";
            } else if (summary.gapDetail.classification == AnalysisGapClassification::Unknown) {
                summary.sourceLabel = "Unknown";
            }
        }

        analysis.dailySummaries.push_back(summary);

        switch (summary.gapDetail.classification) {
        case AnalysisGapClassification::KnownZero:
            ++analysis.gapSummaryCounts.knownZeroDays;
            break;
        case AnalysisGapClassification::Reconstructed:
            ++analysis.gapSummaryCounts.reconstructedDays;
            break;
        case AnalysisGapClassification::Unknown:
            ++analysis.gapSummaryCounts.unknownDays;
            break;
        case AnalysisGapClassification::None:
        default:
            break;
        }

        if (summary.sourceLabel == "Unknown") {
            ++analysis.gapSummaryCounts.unknownSourceDays;
        }
    }

    for (const auto& bucketWindow : trendBucketWindows) {
        AnalysisTrendPoint point;
        point.dateLabel = bucketWindow.label;
        point.activityCount = trendCounts[bucketWindow.startUtc];
        point.bucketStartUtc = bucketWindow.startUtc;
        point.bucketEndUtc = bucketWindow.endUtc;
        const auto sourceIt = trendSources.find(bucketWindow.startUtc);
        if (sourceIt != trendSources.end()) {
            point.sourceLabel = sourceLabelForFlags(sourceIt->second);
        }
        analysis.trendPoints.push_back(std::move(point));
    }

    for (auto& entry : providerRows) {
        entry.second.sourceLabel = sourceLabelForFlags(providerSources[entry.first]);
        analysis.providerBreakdown.push_back(std::move(entry.second));
    }
    for (auto& entry : accountRows) {
        entry.second.sourceLabel = sourceLabelForFlags(accountSources[entry.first]);
        analysis.accountBreakdown.push_back(std::move(entry.second));
    }

    auto sortBreakdownRows = [](std::vector<AnalysisBreakdownRow>& rows) {
        std::sort(
            rows.begin(),
            rows.end(),
            [](const AnalysisBreakdownRow& lhs, const AnalysisBreakdownRow& rhs) {
                if (lhs.activityCount != rhs.activityCount) {
                    return lhs.activityCount > rhs.activityCount;
                }
                if (lhs.label != rhs.label) {
                    return lhs.label < rhs.label;
                }
                return lhs.secondaryLabel < rhs.secondaryLabel;
            });
    };
    sortBreakdownRows(analysis.providerBreakdown);
    sortBreakdownRows(analysis.accountBreakdown);

    bool quotaHistoryUsesRollups = false;
    const auto scopedProviderAccountIds = resolveScopedProviderAccountIds(
        *storage_,
        analysis.scope.selectedProviderId,
        analysis.scope.selectedAccountId);
    if (scopedProviderAccountIds.has_value()) {
        std::map<int, std::pair<std::string, std::string>> planLabels;
        QueryResult planResult = storage_->execute(
            "SELECT id, COALESCE(display_name, ''), COALESCE(quota_window, '') "
            "FROM plans WHERE account_id = ?;",
            {std::to_string(scopedProviderAccountIds->accountId)});
        if (planResult.success()) {
            for (size_t row = 0; row < planResult.rowCount(); ++row) {
                planLabels[planResult.getInt(row, 0)] = {
                    planResult.getString(row, 1),
                    planResult.getString(row, 2)
                };
            }
        }

        std::map<std::string, AnalysisQuotaTrendSeries> quotaSeriesByKey;
        std::map<std::string, std::optional<std::pair<std::time_t, std::time_t>>> previousWindowsByKey;
        std::map<std::string, std::optional<double>> rollupLowWaterBySeriesKey;
        std::map<std::string, int> rollupResetCountBySeriesKey;
        const auto configureQuotaSeries =
            [&](AnalysisQuotaTrendSeries& series,
                const std::optional<int>& planId,
                const std::string& quotaDimension,
                std::time_t windowStartUtc,
                std::time_t windowEndUtc,
                const std::string& seriesKey) {
                if (!series.key.empty()) {
                    return;
                }

                const auto planIt =
                    planId.has_value() ? planLabels.find(*planId) : planLabels.end();
                const std::string derivedWindowLabel =
                    quotaDimension == "percent" && (windowStartUtc <= 0 || windowEndUtc <= windowStartUtc)
                        ? std::string{}
                        : quotaWindowLabelFromSpan(windowStartUtc, windowEndUtc);
                const std::string planDisplayName =
                    planIt != planLabels.end() && !planIt->second.first.empty()
                    ? planIt->second.first
                    : "Scoped quota";
                const std::string configuredWindowLabel =
                    planIt != planLabels.end()
                    ? normalizeQuotaWindowLabel(planIt->second.second)
                    : std::string();
                const std::string windowLabel =
                    !configuredWindowLabel.empty() ? configuredWindowLabel : derivedWindowLabel;

                series.key = seriesKey;
                series.label = planDisplayName.find(windowLabel) != std::string::npos
                    ? planDisplayName
                    : planDisplayName + " " + windowLabel;
                series.quotaDimension = quotaDimension;
                series.windowLabel = windowLabel;
            };

        const bool preferQuotaRollups = shouldUseQuotaSnapshotRollups(window.start, window.end);
        if (preferQuotaRollups) {
            const auto quotaRollups = storage_->getQuotaSnapshotRollups(
                scopedProviderAccountIds->providerId,
                scopedProviderAccountIds->accountId,
                utcDayStart(window.start),
                utcDayStart(window.end));
            for (const auto& rollup : quotaRollups) {
                if (!rollup.latestLimitValue.has_value() || *rollup.latestLimitValue <= 0) {
                    continue;
                }

                const long long limitValue = *rollup.latestLimitValue;
                const long long remainingValue = rollup.latestRemainingValue.has_value()
                    ? std::clamp(*rollup.latestRemainingValue, 0LL, limitValue)
                    : std::clamp(limitValue - rollup.latestUsedValue, 0LL, limitValue);
                const double remainingPercentage =
                    (static_cast<double>(remainingValue) / static_cast<double>(limitValue)) * 100.0;
                const std::string seriesKey =
                    std::to_string(rollup.planId.value_or(0)) + "|" + rollup.quotaDimension;

                auto& series = quotaSeriesByKey[seriesKey];
                configureQuotaSeries(
                    series,
                    rollup.planId,
                    rollup.quotaDimension,
                    rollup.latestWindowStart,
                    rollup.latestWindowEnd,
                    seriesKey);

                AnalysisQuotaTrendPoint point;
                point.label = formatQuotaSnapshotLabel(
                    rollup.latestSnapshotTime > 0 ? rollup.latestSnapshotTime : rollup.periodStart,
                    rollup.latestWindowStart > 0
                        ? std::max<std::time_t>(1, rollup.latestWindowEnd - rollup.latestWindowStart + 1)
                        : 0);
                point.snapshotTime =
                    rollup.latestSnapshotTime > 0 ? rollup.latestSnapshotTime : rollup.periodStart;
                point.remainingPercentage = remainingPercentage;
                point.remainingValue = remainingValue;
                point.limitValue = limitValue;
                point.resetPoint = rollup.resetCount > 0;
                point.reconciled = rollup.reconciledSampleCount > 0;
                point.dailyRollup = true;
                series.points.push_back(std::move(point));

                if (rollup.lowWaterRemainingValue.has_value() &&
                    rollup.lowWaterLimitValue.has_value() &&
                    *rollup.lowWaterLimitValue > 0) {
                    const double lowWaterPercentage =
                        (static_cast<double>(*rollup.lowWaterRemainingValue) /
                         static_cast<double>(*rollup.lowWaterLimitValue)) * 100.0;
                    auto& existing = rollupLowWaterBySeriesKey[seriesKey];
                    if (!existing.has_value() || lowWaterPercentage < *existing) {
                        existing = lowWaterPercentage;
                    }
                }
                rollupResetCountBySeriesKey[seriesKey] += rollup.resetCount;
            }
            quotaHistoryUsesRollups = !quotaSeriesByKey.empty();
        }

        if (quotaSeriesByKey.empty()) {
            const auto quotaSnapshots = storage_->getQuotaSnapshots(
                scopedProviderAccountIds->providerId,
                scopedProviderAccountIds->accountId,
                window.start,
                window.end);
            for (const auto& snapshot : quotaSnapshots) {
                if (!snapshot.limitValue.has_value() || *snapshot.limitValue <= 0) {
                    continue;
                }

                const long long limitValue = *snapshot.limitValue;
                const long long remainingValue = snapshot.remainingValue.has_value()
                    ? std::clamp(*snapshot.remainingValue, 0LL, limitValue)
                    : std::clamp(limitValue - snapshot.usedValue, 0LL, limitValue);
                const double remainingPercentage =
                    (static_cast<double>(remainingValue) / static_cast<double>(limitValue)) * 100.0;
                const std::string seriesKey =
                    std::to_string(snapshot.planId.value_or(0)) + "|" + snapshot.quotaDimension;

                auto& series = quotaSeriesByKey[seriesKey];
                configureQuotaSeries(
                    series,
                    snapshot.planId,
                    snapshot.quotaDimension,
                    snapshot.windowStart,
                    snapshot.windowEnd,
                    seriesKey);

                AnalysisQuotaTrendPoint point;
                point.label = formatQuotaSnapshotLabel(
                    snapshot.snapshotTime, snapshot.windowStart > 0
                                               ? std::max<std::time_t>(1, snapshot.windowEnd - snapshot.windowStart + 1)
                                               : 0);
                point.snapshotTime = snapshot.snapshotTime;
                point.remainingPercentage = remainingPercentage;
                point.remainingValue = remainingValue;
                point.limitValue = limitValue;
                point.resetPoint = snapshot.observeBillingWindow(previousWindowsByKey[seriesKey]);
                point.reconciled = snapshot.isReconciled;
                series.points.push_back(std::move(point));
            }
        }

        for (auto& entry : quotaSeriesByKey) {
            auto& series = entry.second;
            std::sort(
                series.points.begin(),
                series.points.end(),
                [](const AnalysisQuotaTrendPoint& left, const AnalysisQuotaTrendPoint& right) {
                    return left.snapshotTime < right.snapshotTime;
                });
            if (series.points.empty()) {
                continue;
            }

            const auto latestIt = std::max_element(
                series.points.begin(),
                series.points.end(),
                [](const AnalysisQuotaTrendPoint& left, const AnalysisQuotaTrendPoint& right) {
                    return left.snapshotTime < right.snapshotTime;
                });
            const auto lowWaterIt = std::min_element(
                series.points.begin(),
                series.points.end(),
                [](const AnalysisQuotaTrendPoint& left, const AnalysisQuotaTrendPoint& right) {
                    return left.remainingPercentage < right.remainingPercentage;
                });
            series.latestRemainingPercentage = latestIt->remainingPercentage;
            series.latestRemainingValue = latestIt->remainingValue;
            series.limitValue = latestIt->limitValue;

            const auto lowWaterOverrideIt = rollupLowWaterBySeriesKey.find(entry.first);
            if (lowWaterOverrideIt != rollupLowWaterBySeriesKey.end() &&
                lowWaterOverrideIt->second.has_value()) {
                series.lowWaterRemainingPercentage = *lowWaterOverrideIt->second;
            } else {
                series.lowWaterRemainingPercentage = lowWaterIt->remainingPercentage;
            }

            const auto resetOverrideIt = rollupResetCountBySeriesKey.find(entry.first);
            if (resetOverrideIt != rollupResetCountBySeriesKey.end()) {
                series.resetCount = resetOverrideIt->second;
            } else {
                series.resetCount = static_cast<int>(std::count_if(
                    series.points.begin(),
                    series.points.end(),
                    [](const AnalysisQuotaTrendPoint& point) {
                        return point.resetPoint;
                    }));
            }

            analysis.quotaTrendSeries.push_back(std::move(series));
        }

        std::sort(
            analysis.quotaTrendSeries.begin(),
            analysis.quotaTrendSeries.end(),
            [](const AnalysisQuotaTrendSeries& left, const AnalysisQuotaTrendSeries& right) {
                const std::time_t leftTime =
                    left.points.empty() ? 0 : left.points.back().snapshotTime;
                const std::time_t rightTime =
                    right.points.empty() ? 0 : right.points.back().snapshotTime;
                if (leftTime != rightTime) {
                    return leftTime > rightTime;
                }
                return left.label < right.label;
            });
    }

    // Compute Codex throughput metrics from persisted local rollout token usage.
    {
        const std::time_t throughputNow = now;
        const std::time_t window5mStart = std::max<std::time_t>(0, throughputNow - 300);
        const std::time_t window30mStart = std::max<std::time_t>(0, throughputNow - 1800);
        const std::time_t window1hStart = std::max<std::time_t>(0, throughputNow - 3600);
        const std::time_t window30dStart = std::max<std::time_t>(0, throughputNow - (30 * 86400));
        const std::time_t calendarMonthStart = utcMonthStart(throughputNow);
        const std::time_t historyWindowStart = std::min(window30dStart, calendarMonthStart);
        const std::time_t historyQueryStart =
            historyWindowStart > 86400 ? historyWindowStart - 86400 : 0;

        std::string sql =
            "SELECT ue.event_time_utc, ue.total_tokens, ue.raw_reference, ue.request_type, ue.source_run_id, "
            "ue.event_subsecond_ns "
            "FROM usage_events ue "
            "JOIN providers p ON p.id = ue.provider_id "
            "JOIN accounts a ON a.id = ue.account_id "
            "WHERE ue.source_type = 'codex_local_rollout' "
            "  AND ue.event_time_utc >= ? AND ue.event_time_utc <= ?";
        std::vector<std::string> params{
            formatTimeUtcIso(historyQueryStart),
            formatTimeUtcIso(throughputNow)};
        appendScopeFilter(sql, params, analysis.scope.selectedProviderId, "p.provider_key");
        appendScopeFilter(sql, params, analysis.scope.selectedAccountId, "a.account_key");
        sql += " ORDER BY ue.event_time_utc, ue.event_subsecond_ns, ue.id;";

        QueryResult result = storage_->execute(sql, params);
        if (result.success() && result.rowCount() > 0) {
            struct EventTime {
                std::time_t seconds;
                std::optional<int> subsecondNs;
            };
            struct UsageSample {
                EventTime time;
                long long tokens;
                std::string sourceId;
                std::size_t sourceLine = 0;
            };
            const auto definitelyBefore = [](const EventTime& lhs, const EventTime& rhs) {
                return lhs.seconds < rhs.seconds || (lhs.seconds == rhs.seconds && lhs.subsecondNs && rhs.subsecondNs &&
                                                     *lhs.subsecondNs < *rhs.subsecondNs);
            };
            const auto getEventTime = [&](std::size_t row, std::time_t seconds) -> EventTime {
                return {seconds,
                        result.getString(row, 5).empty() ? std::nullopt : std::optional<int>(result.getInt(row, 5))};
            };

            struct TokenWindowTotals {
                long long totalTokenDelta = 0;
                long long promptDeltaCount = 0;
            };
            std::map<std::time_t, TokenWindowTotals> usageAt;
            std::map<std::string, std::vector<UsageSample>> requestsByRollout;
            std::map<std::string, std::vector<UsageSample>> countersByRollout;
            std::set<std::pair<std::string, std::string>> linkedCounters;
            std::set<std::pair<std::string, std::string>> indexedResponses;
            std::set<std::string> countedResponseIds;
            std::size_t legacySampleCount = 0;
            bool legacyEstimated = false;
            for (std::size_t row = 0; row < result.rowCount(); ++row) {
                const auto seconds = parseUtcIso(result.getString(row, 0));
                if (!seconds) {
                    continue;
                }
                const auto rollout = result.getString(row, 2);
                const auto type = result.getString(row, 3);
                UsageSample sample{getEventTime(row, *seconds), result.getInt64(row, 1), result.getString(row, 4)};
                if (type == "codex_request") {
                    const bool isResponse = sample.sourceId.rfind("response:", 0) == 0;
                    if (!isResponse || countedResponseIds.insert(sample.sourceId).second) {
                        auto& usage = usageAt[*seconds];
                        usage.totalTokenDelta += sample.tokens;
                        ++usage.promptDeltaCount;
                    }
                    if (!isResponse || indexedResponses.emplace(rollout, sample.sourceId).second) {
                        requestsByRollout[rollout].push_back(std::move(sample));
                    }
                    continue;
                }

                if (type == "codex_turn" && sample.sourceId.rfind("line:", 0) == 0) {
                    const char* end = sample.sourceId.data() + sample.sourceId.size();
                    const auto parsed = std::from_chars(sample.sourceId.data() + 5, end, sample.sourceLine);
                    if (parsed.ec == std::errc{} && parsed.ptr == end && sample.sourceLine > 0) {
                        linkedCounters.emplace(rollout, sample.sourceId);
                    } else {
                        sample.sourceLine = 0;
                    }
                }
                usageAt.try_emplace(*seconds);
                countersByRollout[rollout].push_back(std::move(sample));
                ++legacySampleCount;
            }

            const auto accountForInterval = [&](const std::string& rollout, const UsageSample& baseline,
                                                const UsageSample& counter, const EventTime& overlapStart,
                                                const EventTime& overlapEnd) {
                const bool estimatedOrder = baseline.time.seconds == counter.time.seconds &&
                                            (!baseline.time.subsecondNs || !counter.time.subsecondNs);
                legacyEstimated =
                    legacyEstimated || estimatedOrder || counter.time.seconds - baseline.time.seconds > 300;
                if ((!estimatedOrder && !definitelyBefore(baseline.time, counter.time)) ||
                    counter.tokens <= baseline.tokens) {
                    return;
                }

                long long unrecordedTokens = counter.tokens - baseline.tokens;
                if (const auto requests = requestsByRollout.find(rollout); requests != requestsByRollout.end()) {
                    const auto first = std::lower_bound(
                        requests->second.begin(), requests->second.end(), baseline.time.seconds,
                        [](const UsageSample& request, std::time_t seconds) { return request.time.seconds < seconds; });
                    for (auto request = first; request != requests->second.end() &&
                                               request->time.seconds <= counter.time.seconds && unrecordedTokens > 0;
                         ++request) {
                        // When a boundary's order is ambiguous, assume overlap and prefer the request record.
                        const bool linked = request->sourceId.rfind("line:", 0) == 0 &&
                                            linkedCounters.count({rollout, request->sourceId}) != 0;
                        const bool coversRequest = linked ? request->sourceId == counter.sourceId
                                                          : !definitelyBefore(request->time, overlapStart) &&
                                                                !definitelyBefore(overlapEnd, request->time);
                        if (coversRequest) {
                            // A boundary shared by two intervals may overlap both. Consume its evidence once.
                            const long long overlap = std::min(unrecordedTokens, request->tokens);
                            unrecordedTokens -= overlap;
                            request->tokens -= overlap;
                            legacyEstimated = legacyEstimated || overlap > 0;
                        }
                    }
                }
                if (unrecordedTokens > 0) {
                    auto& usage = usageAt[counter.time.seconds];
                    usage.totalTokenDelta += unrecordedTokens;
                    ++usage.promptDeltaCount;
                }
            };

            using CounterIterator = std::vector<UsageSample>::iterator;
            struct CounterSecond {
                CounterIterator begin;
                CounterIterator ordered;
                CounterIterator end;
            };
            const auto bySourceLine = [](const UsageSample& left, const UsageSample& right) {
                return left.sourceLine < right.sourceLine;
            };
            const auto orderCounters = [&](CounterIterator begin, CounterIterator end) {
                if (end - begin == 1 || begin->time.subsecondNs) {
                    return begin;
                }

                const auto firstTimed = std::find_if(
                    begin, end, [](const UsageSample& sample) { return sample.time.subsecondNs.has_value(); });
                const bool sourcesAgreeWithTimes =
                    std::all_of(firstTimed, end, [](const UsageSample& sample) { return sample.sourceLine != 0; }) &&
                    std::is_sorted(firstTimed, end, bySourceLine);
                if (!sourcesAgreeWithTimes) {
                    return firstTimed;
                }
                const auto firstOrdered =
                    std::stable_partition(begin, end, [](const UsageSample& sample) { return sample.sourceLine == 0; });
                std::stable_sort(firstOrdered, end, bySourceLine);
                return firstOrdered;
            };

            using CounterCost = std::pair<std::size_t, unsigned long long>;
            struct CounterPlacement {
                std::size_t gap = 0;
                const UsageSample* sample = nullptr;
            };
            struct CounterOrder {
                const UsageSample* first;
                const UsageSample* last;
                CounterPlacement firstBound;
                CounterPlacement secondBound;
                CounterCost internalCost;
                CounterCost pathCost{};
                std::size_t previousOrder = 0;
            };
            const auto transitionCost = [](const UsageSample* left, const UsageSample* right) -> CounterCost {
                if (!left || !right) {
                    return {};
                }
                return {left->tokens > right->tokens,
                        static_cast<unsigned long long>(std::max(0LL, right->tokens - left->tokens))};
            };
            const auto addCost = [](CounterCost left, CounterCost right) -> CounterCost {
                return {left.first + right.first,
                        left.second +
                            std::min(right.second, std::numeric_limits<unsigned long long>::max() - left.second)};
            };
            const auto counterOrders = [&](const CounterSecond& second) {
                const auto count = static_cast<std::size_t>(second.end - second.ordered);
                const auto sampleAt = [&](std::size_t index) { return &*(second.ordered + index); };
                const auto firstKnown = count > 0 ? sampleAt(0) : nullptr;
                const auto lastKnown = count > 0 ? sampleAt(count - 1) : nullptr;
                const auto [knownMin, knownMax] =
                    std::minmax_element(second.ordered, second.end,
                                        [](const auto& left, const auto& right) { return left.tokens < right.tokens; });

                // Without source order only the observed bounds extend the established counter range.
                const UsageSample* low =
                    count == 0 || second.begin->tokens < knownMin->tokens ? &*second.begin : nullptr;
                const UsageSample* high =
                    count == 0 || (second.ordered - 1)->tokens > knownMax->tokens ? &*(second.ordered - 1) : nullptr;
                if (low && high && low->tokens == high->tokens) {
                    high = nullptr;
                } else if (!low) {
                    std::swap(low, high);
                }

                CounterCost knownCost{};
                for (std::size_t i = 1; i < count; ++i) {
                    knownCost = addCost(knownCost, transitionCost(sampleAt(i - 1), sampleAt(i)));
                }
                if (!low) {
                    return std::vector<CounterOrder>{{firstKnown, lastKnown, {}, {}, knownCost}};
                }

                std::vector<CounterOrder> orders;
                const auto extraCost = [&](std::size_t gap, const UsageSample* first, const UsageSample* last) {
                    const auto left = gap > 0 ? sampleAt(gap - 1) : nullptr;
                    const auto right = gap < count ? sampleAt(gap) : nullptr;
                    const auto original = transitionCost(left, right);
                    const auto expanded = addCost(addCost(transitionCost(left, first), transitionCost(first, last)),
                                                  transitionCost(last, right));
                    return CounterCost{expanded.first - original.first, expanded.second - original.second};
                };
                const auto consider = [&](CounterPlacement first, CounterPlacement last) {
                    auto extra = extraCost(first.gap, first.sample,
                                           last.sample && first.gap == last.gap ? last.sample : first.sample);
                    if (last.sample && first.gap != last.gap) {
                        extra = addCost(extra, extraCost(last.gap, last.sample, last.sample));
                    }
                    const auto from = first.gap == 0 ? first.sample : firstKnown;
                    const auto to = last.sample && last.gap == count ? last.sample
                                    : first.gap == count             ? first.sample
                                                                     : lastKnown;
                    const auto cost = addCost(knownCost, extra);
                    const auto sameEndpoints =
                        std::find_if(orders.begin(), orders.end(), [&](const CounterOrder& order) {
                            return order.first == from && order.last == to;
                        });
                    if (sameEndpoints == orders.end()) {
                        orders.push_back({from, to, first, last, cost});
                    } else if (cost < sameEndpoints->internalCost) {
                        *sameEndpoints = {from, to, first, last, cost};
                    }
                };

                if (!high) {
                    for (std::size_t gap = 0; gap <= count; ++gap) {
                        consider({gap, low}, {});
                    }
                    return orders;
                }

                const auto placeBounds = [&](const UsageSample* first, const UsageSample* last) {
                    std::optional<std::size_t> bestInnerGap;
                    for (std::size_t gap = 0; gap <= count; ++gap) {
                        consider({gap, first}, {gap, last});
                        if (gap > 0) {
                            consider({0, first}, {gap, last});
                        }
                        // Separate gaps have independent costs. Only the cheapest earlier interior gap can win.
                        if (bestInnerGap) {
                            consider({*bestInnerGap, first}, {gap, last});
                        }
                        if (gap > 0 &&
                            (!bestInnerGap || extraCost(gap, first, first) < extraCost(*bestInnerGap, first, first))) {
                            bestInnerGap = gap;
                        }
                    }
                };
                placeBounds(low, high);
                placeBounds(high, low);
                return orders;
            };

            for (auto& [rollout, counters] : countersByRollout) {
                std::vector<CounterSecond> seconds;
                for (auto begin = counters.begin(); begin != counters.end();) {
                    const auto end = std::find_if(begin + 1, counters.end(), [&](const UsageSample& sample) {
                        return sample.time.seconds != begin->time.seconds;
                    });
                    const auto ordered = orderCounters(begin, end);
                    std::stable_sort(begin, ordered, [](const UsageSample& left, const UsageSample& right) {
                        return left.tokens < right.tokens;
                    });
                    seconds.push_back({begin, ordered, end});
                    begin = end;
                }

                std::vector<const UsageSample*> orderedCounters;
                orderedCounters.reserve(counters.size());
                const auto useEstimatedSecond = [&](const CounterSecond& second, const CounterOrder& order,
                                                    const UsageSample* next) {
                    std::vector<const UsageSample*> anchors;
                    const auto count = static_cast<std::size_t>(second.end - second.ordered);
                    for (std::size_t gap = 0; gap <= count; ++gap) {
                        if (order.firstBound.sample && gap == order.firstBound.gap) {
                            anchors.push_back(order.firstBound.sample);
                        }
                        if (order.secondBound.sample && gap == order.secondBound.gap) {
                            anchors.push_back(order.secondBound.sample);
                        }
                        if (gap < count) {
                            anchors.push_back(&*(second.ordered + gap));
                        }
                    }

                    if (anchors.size() == static_cast<std::size_t>(second.end - second.begin)) {
                        orderedCounters.insert(orderedCounters.end(), anchors.begin(), anchors.end());
                        return;
                    }

                    // Keep the timestamp/source of an established boundary when its total repeats.
                    std::map<long long, const UsageSample*> remaining;
                    for (const auto* anchor : anchors) {
                        remaining.emplace(anchor->tokens, anchor);
                    }
                    for (auto sample = second.begin; sample != second.ordered; ++sample) {
                        const auto [existing, inserted] = remaining.emplace(sample->tokens, &*sample);
                        if (!inserted && existing->second != &*sample) {
                            linkedCounters.erase({rollout, sample->sourceId});
                        }
                    }
                    for (const auto* anchor : anchors) {
                        remaining.erase(anchor->tokens);
                    }

                    // Split the selected intervals at actual observations before counting prompts or overlap.
                    std::vector<std::vector<const UsageSample*>> gaps(anchors.size() + 1);
                    const auto fillGap = [&](std::size_t gap, long long from, long long to) {
                        const auto first = remaining.lower_bound(std::min(from, to));
                        const auto last = remaining.upper_bound(std::max(from, to));
                        auto& samples = gaps[gap];
                        for (auto it = first; it != last; ++it) {
                            samples.push_back(it->second);
                        }
                        if (from > to) {
                            std::reverse(samples.begin(), samples.end());
                        }
                        remaining.erase(first, last);
                    };
                    const auto fillInternalGaps = [&](bool rising) {
                        for (std::size_t gap = 1; gap < anchors.size() && !remaining.empty(); ++gap) {
                            const auto from = anchors[gap - 1]->tokens;
                            const auto to = anchors[gap]->tokens;
                            if ((from < to) == rising) {
                                fillGap(gap, from, to);
                            }
                        }
                    };
                    fillInternalGaps(true);
                    // Only a reading within an established drop can belong to that drop instead of
                    // establishing growth across the boundary of this second.
                    std::map<long long, const UsageSample*> withinKnownDrops;
                    for (auto known = second.ordered; known != second.end && !remaining.empty(); ++known) {
                        if (known == second.ordered || (known - 1)->tokens <= known->tokens) {
                            continue;
                        }
                        auto it = remaining.lower_bound(known->tokens);
                        while (it != remaining.end() && it->first < (known - 1)->tokens) {
                            withinKnownDrops.insert(remaining.extract(it++));
                        }
                    }
                    const auto* baseline = orderedCounters.empty() ? nullptr : orderedCounters.back();
                    if (!remaining.empty() && (!baseline || baseline->tokens <= order.first->tokens)) {
                        fillGap(0, baseline ? baseline->tokens : 0, order.first->tokens);
                    }
                    if (!remaining.empty() && (!next || order.last->tokens <= next->tokens)) {
                        fillGap(anchors.size(), order.last->tokens,
                                next ? next->tokens : std::numeric_limits<long long>::max());
                    }
                    remaining.merge(withinKnownDrops);
                    fillInternalGaps(false);
                    for (std::size_t gap = 0; gap < gaps.size(); ++gap) {
                        orderedCounters.insert(orderedCounters.end(), gaps[gap].begin(), gaps[gap].end());
                        if (gap < anchors.size()) {
                            orderedCounters.push_back(anchors[gap]);
                        }
                    }
                };
                using SecondIterator = std::vector<CounterSecond>::const_iterator;
                const auto useEstimatedRun = [&](SecondIterator begin, SecondIterator end) {
                    legacyEstimated = true;
                    std::vector<std::vector<CounterOrder>> choices;
                    choices.reserve(end - begin);
                    const auto* baseline = orderedCounters.empty() ? nullptr : orderedCounters.back();
                    for (auto second = begin; second != end; ++second) {
                        auto orders = counterOrders(*second);
                        for (auto& order : orders) {
                            order.pathCost = addCost(order.internalCost, transitionCost(baseline, order.first));
                            if (choices.empty()) {
                                continue;
                            }
                            const auto& previous = choices.back();
                            for (std::size_t i = 0; i < previous.size(); ++i) {
                                const auto boundary = transitionCost(previous[i].last, order.first);
                                const auto cost = addCost(order.internalCost, addCost(previous[i].pathCost, boundary));
                                if (i == 0 || cost < order.pathCost) {
                                    order.pathCost = cost;
                                    order.previousOrder = i;
                                }
                            }
                        }
                        choices.push_back(std::move(orders));
                    }
                    const auto after = end != seconds.cend() ? &*end->ordered : nullptr;
                    const auto& last = choices.back();
                    const auto best =
                        std::min_element(last.begin(), last.end(), [&](const auto& left, const auto& right) {
                            return addCost(left.pathCost, transitionCost(left.last, after)) <
                                   addCost(right.pathCost, transitionCost(right.last, after));
                        });

                    // Recover the joint choice before reporting any deltas or consuming request overlap.
                    std::vector<std::size_t> selected(choices.size());
                    auto chosen = static_cast<std::size_t>(best - last.begin());
                    for (std::size_t i = choices.size(); i-- > 0;) {
                        selected[i] = chosen;
                        chosen = choices[i][chosen].previousOrder;
                    }
                    for (std::size_t i = 0; i < choices.size(); ++i) {
                        const auto& second = *(begin + i);
                        const auto& order = choices[i][selected[i]];
                        const auto* next = i + 1 < choices.size() ? choices[i + 1][selected[i + 1]].first : after;
                        useEstimatedSecond(second, order, next);
                    }
                };
                for (auto second = seconds.cbegin(); second != seconds.cend();) {
                    if (second->begin == second->ordered) {
                        for (auto sample = second->begin; sample != second->end; ++sample) {
                            orderedCounters.push_back(&*sample);
                        }
                        ++second;
                    } else {
                        const auto end = std::find_if(second + 1, seconds.cend(), [](const CounterSecond& next) {
                            return next.begin == next.ordered;
                        });
                        useEstimatedRun(second, end);
                        second = end;
                    }
                }

                // An untimed counter splits the token delta, but every split still lies between the
                // nearest known timestamps. Keep those bounds when reconciling unlinked requests.
                for (std::size_t start = 0; start + 1 < orderedCounters.size();) {
                    std::size_t end = start + 1;
                    while (end + 1 < orderedCounters.size() && !orderedCounters[end]->time.subsecondNs) {
                        ++end;
                    }
                    const auto& firstTime = orderedCounters[start]->time;
                    const auto& lastTime = orderedCounters[end]->time;
                    for (std::size_t i = start; i < end; ++i) {
                        const auto& before = *orderedCounters[i];
                        const auto& after = *orderedCounters[i + 1];
                        const auto& overlapStart = before.time.seconds == firstTime.seconds ? firstTime : before.time;
                        const auto& overlapEnd = after.time.seconds == lastTime.seconds ? lastTime : after.time;
                        accountForInterval(rollout, before, after, overlapStart, overlapEnd);
                    }
                    start = end;
                }
            }

            auto computeWindowTotals = [&](std::time_t windowStart, std::time_t windowEnd) {
                TokenWindowTotals totals;
                for (auto position = usageAt.lower_bound(windowStart);
                     position != usageAt.end() && position->first <= windowEnd; ++position) {
                    totals.totalTokenDelta += position->second.totalTokenDelta;
                    totals.promptDeltaCount += position->second.promptDeltaCount;
                }
                return totals;
            };

            auto computeWindowRates = [&](std::time_t windowStart, std::time_t windowEnd)
                -> std::pair<double, double> {
                const TokenWindowTotals totals = computeWindowTotals(windowStart, windowEnd);
                const double windowSeconds = static_cast<double>(windowEnd - windowStart);
                if (totals.promptDeltaCount == 0 || windowSeconds <= 0.0) {
                    return {0.0, 0.0};
                }
                return {
                    (static_cast<double>(totals.totalTokenDelta) / windowSeconds) * 60.0,
                    (static_cast<double>(totals.promptDeltaCount) / windowSeconds) * 60.0
                };
            };

            const auto rates5m = computeWindowRates(window5mStart, throughputNow);
            const auto rates30m = computeWindowRates(window30mStart, throughputNow);
            const TokenWindowTotals totals1h = computeWindowTotals(window1hStart, throughputNow);
            const TokenWindowTotals totals30d = computeWindowTotals(window30dStart, throughputNow);
            const TokenWindowTotals totalsMonth = computeWindowTotals(calendarMonthStart, throughputNow);
            const long long monthElapsedDays = std::max<long long>(1, elapsedUtcDaysInclusive(calendarMonthStart, throughputNow));

            analysis.codexThroughput.tokensPerMinute5m = rates5m.first;
            analysis.codexThroughput.promptsPerMinute5m = rates5m.second;
            analysis.codexThroughput.tokensPerMinute30m = rates30m.first;
            analysis.codexThroughput.promptsPerMinute30m = rates30m.second;
            analysis.codexThroughput.tokensLast1h = totals1h.totalTokenDelta;
            analysis.codexThroughput.promptsLast1h = totals1h.promptDeltaCount;
            analysis.codexThroughput.rolling30DayTokens = totals30d.totalTokenDelta;
            analysis.codexThroughput.rolling30DayPrompts = totals30d.promptDeltaCount;
            analysis.codexThroughput.calendarMonthTokens = totalsMonth.totalTokenDelta;
            analysis.codexThroughput.calendarMonthPrompts = totalsMonth.promptDeltaCount;
            analysis.codexThroughput.averageDailyTokens = totals30d.totalTokenDelta / 30;
            analysis.codexThroughput.averageDailyPrompts = totals30d.promptDeltaCount / 30;
            analysis.codexThroughput.averageMonthlyTokens = totalsMonth.totalTokenDelta / monthElapsedDays;
            analysis.codexThroughput.averageMonthlyPrompts = totalsMonth.promptDeltaCount / monthElapsedDays;
            analysis.codexThroughput.hasData = totals30d.promptDeltaCount > 0 || legacySampleCount > 1;
            analysis.codexThroughput.confidenceLabel =
                (legacyEstimated || countersByRollout.size() > 1) ? "Estimated" : "LocalExact";

            std::map<std::time_t, TokenWindowTotals> minutes;
            for (auto position = usageAt.lower_bound(window30mStart); position != usageAt.end(); ++position) {
                const auto minute = position->first - position->first % 60;
                auto& totals = minutes[minute];
                totals.totalTokenDelta += position->second.totalTokenDelta;
                totals.promptDeltaCount += position->second.promptDeltaCount;
            }
            for (const auto& [minute, totals] : minutes) {
                AnalysisCodexMinutePoint point;
                point.snapshotTime = minute;
                point.tokensPerMinute = static_cast<double>(totals.totalTokenDelta);
                point.promptsPerMinute = static_cast<double>(totals.promptDeltaCount);
                analysis.codexHourlyHistory.points.push_back(point);
            }
            analysis.codexHourlyHistory.hasData = !minutes.empty();
        }
    }

    auto populateModelUsageChart = [&](const std::string& providerKey, const std::string& title, AnalysisProviderModelUsageChart& chart) {
        const std::time_t windowStart = std::max<std::time_t>(0, now - 86400);
        const int bucketSeconds = 300;
        auto points = storage_->getModelUsagePoints({providerKey}, "codex_local_sqlite", windowStart, now, bucketSeconds);
        chart.providerKey = providerKey;
        chart.title = title;
        chart.subtitle = "Observed local Codex usage since JiStats baseline";

        std::map<std::string, AnalysisModelUsageSeries> byModel;
        for (const auto& point : points) {
            auto& series = byModel[point.modelKey];
            series.modelKey = point.modelKey.empty() ? "unknown" : point.modelKey;
            series.totalTokens += point.totalTokens;
            AnalysisModelUsagePoint modelPoint;
            modelPoint.bucketStartUtc = point.bucketStart;
            modelPoint.totalTokens = point.totalTokens;
            series.points.push_back(std::move(modelPoint));
        }
        for (auto& entry : byModel) {
            std::sort(entry.second.points.begin(), entry.second.points.end(), [](const auto& left, const auto& right) {
                return left.bucketStartUtc < right.bucketStartUtc;
            });
            chart.series.push_back(std::move(entry.second));
        }
        std::sort(chart.series.begin(), chart.series.end(), [](const auto& left, const auto& right) {
            if (left.totalTokens != right.totalTokens) return left.totalTokens > right.totalTokens;
            return left.modelKey < right.modelKey;
        });
        if (chart.series.size() > 6) {
            chart.series.resize(6);
        }
        chart.hasData = !chart.series.empty();
    };

    populateModelUsageChart("openai", "OpenAI Codex Usage by Model", analysis.openAiCodexModelUsage);
    populateModelUsageChart("hicap-ai", "HICAP Codex Usage by Model", analysis.hicapCodexModelUsage);

    long long importedBackfillCheckpoints = 0;
    long long mismatchCheckpoints = 0;
    long long failedBackfillCheckpoints = 0;
    {
        std::string sql =
            "SELECT "
            "COALESCE(SUM(CASE WHEN rc.outcome = 'imported' THEN 1 ELSE 0 END), 0), "
            "COALESCE(SUM(CASE WHEN rc.comparison_result = 'mismatch' THEN 1 ELSE 0 END), 0), "
            "COALESCE(SUM(CASE WHEN rc.outcome IN ('fetch_failed', 'persist_failed') THEN 1 ELSE 0 END), 0) "
            "FROM recovery_checkpoints rc "
            "JOIN providers p ON p.id = rc.provider_id "
            "JOIN accounts a ON a.id = rc.account_id "
            // The legacy OpenAI and MiniMax adapters wrote these outcomes without querying history.
            "WHERE rc.sync_kind = ? AND NOT (rc.fetched_event_count = 0 AND ("
            "(p.provider_key = 'openai' AND rc.outcome = 'no_data') OR "
            "(p.provider_key = 'minimax' AND rc.outcome = 'fetch_failed')))";
        std::vector<std::string> params{kRuntimeBackfillSyncKind};
        appendScopeFilter(sql, params, analysis.scope.selectedProviderId, "p.provider_key");
        appendScopeFilter(sql, params, analysis.scope.selectedAccountId, "a.account_key");
        QueryResult result = storage_->execute(sql, params);
        if (result.success() && result.rowCount() > 0) {
            importedBackfillCheckpoints = result.getInt64(0, 0);
            mismatchCheckpoints = result.getInt64(0, 1);
            failedBackfillCheckpoints = result.getInt64(0, 2);
        }
    }

    const long long totalObservedCount = std::accumulate(
        analysis.dailySummaries.begin(),
        analysis.dailySummaries.end(),
        0LL,
        [](long long total, const AnalysisDailySummary& row) {
            return total + row.activityCount;
        });

    std::ostringstream quality;
    auto appendQualitySentence = [&quality](const std::string& sentence) {
        if (sentence.empty()) {
            return;
        }
        if (quality.tellp() > 0) {
            quality << " ";
        }
        quality << sentence;
    };
    if (analysis.dailySummaries.empty()) {
        appendQualitySentence("No analysis days resolved for the current scope.");
    } else if (totalObservedCount == 0 &&
               analysis.gapSummaryCounts.knownZeroDays == 0 &&
               analysis.gapSummaryCounts.reconstructedDays == 0 &&
               analysis.gapSummaryCounts.unknownDays == 0) {
        appendQualitySentence("No persisted usage, activity, or gap markers were found for this analysis scope.");
    } else {
        appendQualitySentence(
            "Rollup-backed daily usage is preferred when available, alongside raw usage events, "
            "with local activity fallback when quantified usage is not persisted for a scoped provider/account/day.");

        if (!usageBackedDays.empty() || !activityFallbackDays.empty()) {
            std::ostringstream coverage;
            coverage << "Coverage: " << usageBackedDays.size() << " usage-backed day";
            if (usageBackedDays.size() != 1) {
                coverage << "s";
            }
            coverage << " and " << activityFallbackDays.size() << " activity-fallback day";
            if (activityFallbackDays.size() != 1) {
                coverage << "s";
            }
            coverage << ".";
            appendQualitySentence(coverage.str());
        }

        if (!analysis.trendPoints.empty()) {
            std::ostringstream trendSummary;
            trendSummary << "Trend buckets: " << analysis.trendBucketLabel
                         << " across " << analysis.trendPoints.size() << " point";
            if (analysis.trendPoints.size() != 1) {
                trendSummary << "s";
            }
            trendSummary << ".";
            appendQualitySentence(trendSummary.str());
        }

        if (analysis.gapSummaryCounts.knownZeroDays > 0 ||
            analysis.gapSummaryCounts.reconstructedDays > 0 ||
            analysis.gapSummaryCounts.unknownDays > 0 ||
            analysis.gapSummaryCounts.unknownSourceDays > 0) {
            std::ostringstream gapVisibility;
            gapVisibility
                << "Gap visibility: "
                << analysis.gapSummaryCounts.knownZeroDays << " known-zero, "
                << analysis.gapSummaryCounts.reconstructedDays << " reconstructed, "
                << analysis.gapSummaryCounts.unknownDays << " unknown-gap, and "
                << analysis.gapSummaryCounts.unknownSourceDays << " unknown-source day";
            if (analysis.gapSummaryCounts.unknownSourceDays != 1) {
                gapVisibility << "s";
            }
            gapVisibility << ".";
            appendQualitySentence(gapVisibility.str());
        }
    }

    if (importedBackfillCheckpoints > 0 ||
        mismatchCheckpoints > 0 ||
        failedBackfillCheckpoints > 0) {
        std::ostringstream recoverySummary;
        recoverySummary
            << "Recovery checkpoints: "
            << importedBackfillCheckpoints << " imported, "
            << mismatchCheckpoints << " mismatch, "
            << failedBackfillCheckpoints << " failed.";
        appendQualitySentence(recoverySummary.str());
    }

    if (!analysis.quotaTrendSeries.empty()) {
        const long long totalSnapshots = std::accumulate(
            analysis.quotaTrendSeries.begin(),
            analysis.quotaTrendSeries.end(),
            0LL,
            [](long long total, const AnalysisQuotaTrendSeries& series) {
                return total + static_cast<long long>(series.points.size());
            });
        std::time_t latestSnapshotTime = 0;
        int resetMarkers = 0;
        for (const auto& series : analysis.quotaTrendSeries) {
            if (!series.points.empty()) {
                latestSnapshotTime = std::max(latestSnapshotTime, series.points.back().snapshotTime);
            }
            resetMarkers += series.resetCount;
        }

        std::ostringstream quotaSummary;
        quotaSummary << "Persisted quota history points in scope: " << totalSnapshots
                     << " across " << analysis.quotaTrendSeries.size() << " series";
        if (latestSnapshotTime > 0) {
            quotaSummary << ", latest " << formatTimeUtcIso(latestSnapshotTime);
        }
        if (resetMarkers > 0) {
            quotaSummary << ", " << resetMarkers << " reset marker";
            if (resetMarkers != 1) {
                quotaSummary << "s";
            }
        }
        if (quotaHistoryUsesRollups) {
            quotaSummary << ", daily rollup-backed";
        }
        quotaSummary << ".";
        appendQualitySentence(quotaSummary.str());
    }

    if (const auto latestManualExport = storage_->getLatestExportRun("manual_html");
        latestManualExport.has_value() && latestManualExport->requestedAt > 0) {
        std::ostringstream exportSummary;
        exportSummary << "Latest manual export: "
                      << formatTimeUtcIso(latestManualExport->requestedAt)
                      << " (" << (latestManualExport->success ? "success" : "failed") << ").";
        appendQualitySentence(exportSummary.str());
    }
    if (const auto latestScheduledExport = storage_->getLatestExportRun("scheduled_html");
        latestScheduledExport.has_value() && latestScheduledExport->requestedAt > 0) {
        std::ostringstream exportSummary;
        exportSummary << "Latest scheduled export: "
                      << formatTimeUtcIso(latestScheduledExport->requestedAt)
                      << " (" << (latestScheduledExport->success ? "success" : "failed") << ").";
        appendQualitySentence(exportSummary.str());
    }
    analysis.dataQualityNote = quality.str();

    return analysis;
}

const char* AnalysisQueryEngine::dateRangePresetSettingValue(AnalysisDateRangePreset preset) {
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

AnalysisDateRangePreset AnalysisQueryEngine::parseDateRangePreset(const std::string& value) {
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

std::string AnalysisQueryEngine::dateRangePresetLabel(AnalysisDateRangePreset preset) {
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

} // namespace JiStats
