#include <catch_amalgamated.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "db/StorageService.hpp"
#include "gui/DashboardPresenter.hpp"
#include "providers/openai/OpenAIAdapter.hpp"
#include "reporting/AnalysisQueryEngine.hpp"
#include "sqlite_test_support.hpp"
#include "temp_test_directory.hpp"

using namespace JiStats;
using JiStats::TestSupport::TempTestDatabase;
using JiStats::TestSupport::requireSqlite;

namespace {

std::time_t makeUtcTime(
    int year,
    int month,
    int day,
    int hour = 0,
    int minute = 0,
    int second = 0) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    return _mkgmtime(&tm);
}

ProviderStatus makeProvider(
    const std::string& providerKey,
    const std::string& displayName,
    const std::string& accountId) {
    ProviderStatus provider;
    provider.providerKey = providerKey;
    provider.displayName = displayName;
    provider.accountId = accountId;
    provider.isEnabled = true;
    provider.state = ProviderState::Live;
    return provider;
}

std::time_t toTimeT(const std::chrono::system_clock::time_point& value) {
    return std::chrono::system_clock::to_time_t(value);
}

std::vector<std::string> optionPairs(const std::vector<AnalysisScopeOption>& options) {
    std::vector<std::string> result;
    result.reserve(options.size());
    for (const auto& option : options) {
        result.push_back(option.key + "|" + option.label);
    }
    return result;
}

long long requireSingleId(const QueryResult& result) {
    REQUIRE(result.success());
    REQUIRE(result.rowCount() == 1);
    return result.getInt64(0, 0);
}

int requireProviderId(StorageService& storage, const std::string& providerKey) {
    return static_cast<int>(requireSingleId(storage.execute(
        "SELECT id FROM providers WHERE provider_key = ? LIMIT 1;",
        {providerKey})));
}

std::optional<int> loadAccountId(StorageService& storage, int providerId, const std::string& accountKey) {
    const QueryResult result = storage.execute(
        "SELECT id FROM accounts WHERE provider_id = ? AND account_key = ? LIMIT 1;",
        {std::to_string(providerId), accountKey});
    if (!result.success() || result.rowCount() == 0) {
        return std::nullopt;
    }
    return result.getInt(0, 0);
}

int ensureRuntimeAccountId(
    StorageService& storage,
    const std::string& providerKey,
    const std::string& displayName) {
    const int providerId = requireProviderId(storage, providerKey);
    const std::string accountKey = providerKey + ".runtime_status";
    if (const auto existing = loadAccountId(storage, providerId, accountKey)) {
        return *existing;
    }

    const QueryResult insert = storage.execute(
        "INSERT INTO accounts (provider_id, account_key, display_name, scope_type, scope_identifier) "
        "VALUES (?, ?, ?, ?, ?);",
        {
            std::to_string(providerId),
            accountKey,
            displayName + " Runtime",
            "runtime",
            accountKey
        });
    REQUIRE(insert.success());
    return static_cast<int>(insert.lastInsertRowId());
}

void seedUsageEvent(
    StorageService& storage,
    const std::string& providerKey,
    const std::string& displayName,
    std::time_t eventTime,
    long long totalTokens,
    long long costMinorUnits,
    const std::string& modelKey) {
    PersistedUsageEvent event;
    event.providerId = requireProviderId(storage, providerKey);
    event.accountId = ensureRuntimeAccountId(storage, providerKey, displayName);
    event.eventTime = eventTime;
    event.sourceType = "api";
    event.sourceRunId = providerKey + "-usage-" + std::to_string(eventTime);
    event.modelKey = modelKey;
    event.requestType = "chat";
    event.inputTokens = totalTokens / 2;
    event.outputTokens = totalTokens - event.inputTokens;
    event.totalTokens = totalTokens;
    event.costMinorUnits = costMinorUnits;
    event.rawReference = providerKey + "://" + modelKey + "/" + std::to_string(eventTime);
    REQUIRE(storage.insertUsageEvents({event}));
}

void seedCodexUsageEvent(StorageService& storage, std::time_t eventTime, long long totalTokens,
                         const std::string& rawReference = "codex://rollout/current", const std::string& requestId = {},
                         std::optional<int> subsecondNs = std::nullopt) {
    PersistedUsageEvent event;
    event.providerId = requireProviderId(storage, "openai");
    event.accountId = ensureRuntimeAccountId(storage, "openai", "OpenAI");
    event.eventTime = eventTime;
    event.eventSubsecondNs = subsecondNs;
    event.sourceType = "codex_local_rollout";
    event.sourceRunId = requestId.empty() ? "codex_token_usage" : requestId;
    event.modelKey = "codex";
    event.requestType = requestId.empty() ? "codex_turn" : "codex_request";
    event.inputTokens = totalTokens / 2;
    event.outputTokens = totalTokens - event.inputTokens;
    event.totalTokens = totalTokens;
    event.costMinorUnits = 0;
    event.rawReference = rawReference;
    REQUIRE(storage.insertUsageEvents({event}));
}

void seedCodexSqliteUsageEvent(
    StorageService& storage,
    const std::string& providerKey,
    const std::string& modelKey,
    std::time_t eventTime,
    long long totalTokens) {
    PersistedUsageEvent event;
    event.providerId = requireProviderId(storage, providerKey);
    event.accountId = ensureRuntimeAccountId(storage, providerKey, providerKey);
    event.eventTime = eventTime;
    event.sourceType = "codex_local_sqlite";
    event.sourceRunId = providerKey + "-codex-sqlite-" + std::to_string(eventTime);
    event.modelKey = modelKey;
    event.requestType = "codex_thread_delta";
    event.totalTokens = totalTokens;
    event.costMinorUnits = 0;
    event.rawReference = providerKey + "://codex-sqlite/" + modelKey + "/" + std::to_string(eventTime);
    REQUIRE(storage.insertUsageEvents({event}));
}

void seedLocalActivity(
    StorageService& storage,
    const std::string& providerKey,
    const std::string& displayName,
    const std::string& providerSurface,
    std::time_t startedAt,
    std::time_t durationSeconds) {
    PersistedActivityEvent persisted;
    persisted.providerId = requireProviderId(storage, providerKey);
    persisted.accountId = ensureRuntimeAccountId(storage, providerKey, displayName);
    persisted.activity.providerKey = providerKey;
    persisted.activity.providerSurface = providerSurface;
    persisted.activity.activityKind = ActivityKind::SessionActive;
    persisted.activity.startedAt = startedAt;
    persisted.activity.endedAt = startedAt + durationSeconds;
    persisted.activity.durationSeconds = durationSeconds;
    persisted.activity.sourceType = "local";
    persisted.activity.sourceRunId = providerKey + "-local-" + std::to_string(startedAt);
    persisted.activity.surfaceKey = "workspace";
    persisted.activity.windowTitleHint = displayName;
    persisted.activity.confidence = ActivityConfidence::LocalExact;
    persisted.activity.rawReference = "local://" + providerKey + "/" + std::to_string(startedAt);
    REQUIRE(storage.insertActivityEvent(persisted));
}

void seedGapMarker(
    StorageService& storage,
    const std::string& providerKey,
    const std::string& displayName,
    AnalysisGapClassification classification,
    std::time_t dayStart,
    const std::string& basisSummary,
    const std::string& sourceKind) {
    PersistedGapMarker marker;
    marker.providerId = requireProviderId(storage, providerKey);
    marker.accountId = ensureRuntimeAccountId(storage, providerKey, displayName);
    marker.periodStart = dayStart;
    marker.periodEnd = dayStart + 3600;
    switch (classification) {
    case AnalysisGapClassification::KnownZero:
        marker.classification = GapClassification::KnownZero;
        break;
    case AnalysisGapClassification::Reconstructed:
        marker.classification = GapClassification::Reconstructed;
        break;
    case AnalysisGapClassification::Unknown:
        marker.classification = GapClassification::Unknown;
        break;
    default:
        marker.classification = GapClassification::Unknown;
        break;
    }
    marker.basisSummary = basisSummary;
    marker.sourceKind = sourceKind;
    REQUIRE(storage.upsertGapMarker(marker));
}

std::optional<AnalysisDailySummary> findDailySummary(
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

std::optional<AnalysisBreakdownRow> findBreakdownRow(
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

std::optional<AnalysisTrendPoint> findTrendPoint(
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

} // namespace

TEST_CASE("AnalysisQueryEngine builds rollup-backed daily summaries breakdowns and trend points from persisted usage scope", "[analysis][query_engine][analysis_query_engine]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    const std::time_t now = makeUtcTime(2026, 4, 20, 18, 0, 0);
    const std::time_t openaiDayOne = makeUtcTime(2026, 4, 18, 8, 0, 0);
    const std::time_t openaiDayTwo = makeUtcTime(2026, 4, 19, 9, 0, 0);
    const std::time_t minimaxDayTwo = makeUtcTime(2026, 4, 19, 11, 0, 0);

    seedUsageEvent(storage, "openai", "OpenAI", openaiDayOne, 240, 18, "gpt-4.1");
    seedUsageEvent(storage, "openai", "OpenAI", openaiDayTwo, 180, 14, "gpt-4.1");
    seedUsageEvent(storage, "minimax", "MiniMax", minimaxDayTwo, 500, 40, "minimax-m1");

    const int openaiProviderId = requireProviderId(storage, "openai");
    const int openaiAccountId = ensureRuntimeAccountId(storage, "openai", "OpenAI");
    REQUIRE(storage.rebuildUsageRollups(
        UsageRollupResolution::Daily,
        openaiProviderId,
        openaiAccountId,
        makeUtcTime(2026, 4, 17, 0, 0, 0),
        now));
    seedGapMarker(
        storage,
        "openai",
        "OpenAI",
        AnalysisGapClassification::Unknown,
        makeUtcTime(2026, 4, 18, 0, 0, 0),
        "analysis query gap",
        "test");

    AnalysisQueryEngine engine(storage);

    DashboardViewModel dashboard;
    dashboard.providers = {
        makeProvider("openai", "OpenAI", "acct-openai-primary"),
        makeProvider("openai", "OpenAI", "acct-openai-secondary"),
        makeProvider("openai", "OpenAI", "acct-openai-primary"),
        makeProvider("minimax", "MiniMax", "acct-minimax")
    };

    DashboardPreferences preferences;
    preferences.analysisSelectedProviderId = "openai";
    preferences.analysisSelectedAccountId = "openai.runtime_status";
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Last7Days;

    const AnalysisViewModel analysis = engine.buildViewModel(
        dashboard,
        preferences,
        now);
    const std::vector<std::string> accountOptions = optionPairs(analysis.availableAccounts);
    const std::string openaiDayOneLabel = "2026-04-18";
    const std::string openaiDayTwoLabel = "2026-04-19";

    CHECK(analysis.scope.selectedProviderId == "openai");
    CHECK(analysis.scope.selectedAccountId == "openai.runtime_status");
    CHECK(analysis.scope.dateRangePreset == AnalysisDateRangePreset::Last7Days);
    CHECK(analysis.trendBucketGranularity == AnalysisTrendBucketGranularity::Daily);
    CHECK(analysis.trendBucketLabel == "Daily");
    CHECK(optionPairs(analysis.availableProviders).size() >= 3);
    CHECK(std::find_if(
        analysis.availableProviders.begin(),
        analysis.availableProviders.end(),
        [](const AnalysisScopeOption& option) {
            return option.key == "openai" && option.label == "OpenAI";
        }) != analysis.availableProviders.end());
    CHECK(std::find_if(
        analysis.availableProviders.begin(),
        analysis.availableProviders.end(),
        [](const AnalysisScopeOption& option) {
            return option.key == "minimax" && option.label == "MiniMax";
        }) != analysis.availableProviders.end());
    CHECK(accountOptions.size() >= 2);
    CHECK(std::find(
        accountOptions.begin(),
        accountOptions.end(),
        "openai.runtime_status|OpenAI / OpenAI Runtime") != accountOptions.end());
    CHECK(analysis.scopeLabel == "OpenAI / OpenAI / OpenAI Runtime / Last 7 Days");

    const auto dayOneSummary = findDailySummary(analysis.dailySummaries, openaiDayOneLabel);
    const auto dayTwoSummary = findDailySummary(analysis.dailySummaries, openaiDayTwoLabel);
    REQUIRE(dayOneSummary.has_value());
    REQUIRE(dayTwoSummary.has_value());
    CHECK(dayOneSummary->activityCount >= 1);
    CHECK(dayTwoSummary->activityCount >= 1);
    CHECK(dayOneSummary->sourceLabel != "Unknown");
    CHECK(dayTwoSummary->sourceLabel != "Unknown");
    CHECK(dayOneSummary->gapDetail.classification != AnalysisGapClassification::None);
    CHECK_FALSE(dayOneSummary->gapDetail.classificationLabel.empty());
    CHECK(dayOneSummary->gapDetail.basisSummary.find("analysis query gap") != std::string::npos);

    const auto providerRow = findBreakdownRow(analysis.providerBreakdown, "OpenAI");
    REQUIRE(providerRow.has_value());
    CHECK(providerRow->activityCount >= 2);
    CHECK(providerRow->sourceLabel != "Unknown");

    const auto accountRow = findBreakdownRow(analysis.accountBreakdown, "OpenAI Runtime");
    REQUIRE(accountRow.has_value());
    CHECK(accountRow->activityCount >= 2);
    CHECK(accountRow->secondaryLabel == "OpenAI");
    CHECK(accountRow->sourceLabel != "Unknown");

    const auto dayOneTrend = findTrendPoint(analysis.trendPoints, openaiDayOneLabel);
    const auto dayTwoTrend = findTrendPoint(analysis.trendPoints, openaiDayTwoLabel);
    REQUIRE(dayOneTrend.has_value());
    REQUIRE(dayTwoTrend.has_value());
    CHECK(dayOneTrend->activityCount == dayOneSummary->activityCount);
    CHECK(dayTwoTrend->activityCount == dayTwoSummary->activityCount);

    CHECK(analysis.gapSummaryCounts.knownZeroDays == 0);
    CHECK(analysis.gapSummaryCounts.reconstructedDays == 0);
    CHECK(analysis.gapSummaryCounts.unknownDays >= 1);
    CHECK(analysis.gapSummaryCounts.unknownSourceDays >= 0);
    CHECK(analysis.dataQualityNote.find("Rollup") != std::string::npos);
    CHECK(analysis.dataQualityNote.find("gap") != std::string::npos);
    CHECK(analysis.dataQualityNote.find("Trend buckets: Daily") != std::string::npos);
}

TEST_CASE("AnalysisQueryEngine keeps Codex throughput separate from scoped usage summaries", "[analysis][query_engine][analysis_query_engine][codex]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    const std::time_t now = makeUtcTime(2026, 4, 20, 18, 0, 0);
    seedUsageEvent(storage, "openai", "OpenAI", now - 240, 120, 12, "gpt-4.1");
    seedCodexUsageEvent(storage, now - 120, 150);
    seedCodexUsageEvent(storage, now - 60, 300);
    seedCodexUsageEvent(storage, now, 450);

    DashboardViewModel dashboard;
    dashboard.providers = {makeProvider("openai", "OpenAI", "openai.runtime_status")};

    DashboardPreferences preferences;
    preferences.analysisSelectedProviderId = "openai";
    preferences.analysisSelectedAccountId = "openai.runtime_status";
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Last24Hours;

    AnalysisQueryEngine engine(storage);
    const AnalysisViewModel analysis = engine.buildViewModel(dashboard, preferences, now);

    REQUIRE(analysis.codexThroughput.hasData);
    CHECK(analysis.codexThroughput.tokensPerMinute5m == Catch::Approx(60.0));
    CHECK(analysis.codexThroughput.promptsPerMinute5m == Catch::Approx(0.4));
    CHECK(analysis.codexThroughput.tokensPerMinute30m == Catch::Approx(10.0));
    CHECK(analysis.codexThroughput.promptsPerMinute30m == Catch::Approx(1.0 / 15.0));
    CHECK(analysis.codexThroughput.tokensLast1h == 300);
    CHECK(analysis.codexThroughput.promptsLast1h == 2);
    CHECK(analysis.codexThroughput.averageDailyTokens == 10);
    CHECK(analysis.codexThroughput.averageDailyPrompts == 0);
    CHECK(analysis.codexThroughput.averageMonthlyTokens == 15);
    CHECK(analysis.codexThroughput.averageMonthlyPrompts == 0);
    CHECK(analysis.codexThroughput.calendarMonthTokens == 300);
    CHECK(analysis.codexThroughput.calendarMonthPrompts == 2);
    CHECK(analysis.codexThroughput.rolling30DayTokens == 300);
    CHECK(analysis.codexThroughput.rolling30DayPrompts == 2);
    CHECK(analysis.codexThroughput.confidenceLabel == "LocalExact");
    REQUIRE(analysis.codexHourlyHistory.hasData);
    CHECK(analysis.codexHourlyHistory.points.size() == 3);

    const auto currentDaySummary = findDailySummary(analysis.dailySummaries, "2026-04-20");
    REQUIRE(currentDaySummary.has_value());
    CHECK(currentDaySummary->activityCount == 1);
}

TEST_CASE("Analysis uses the same Codex request events for totals and chart buckets", "[analysis][codex]") {
    requireSqlite();
    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    const auto now = makeUtcTime(2026, 4, 20, 18);
    const std::string rollout = "codex://rollout/current";
    seedCodexUsageEvent(storage, now - 240, 100);
    seedCodexUsageEvent(storage, now - 180, 200);
    seedCodexUsageEvent(storage, now - 120, 250);
    seedCodexUsageEvent(storage, now - 120, 30, rollout, "response:one");
    seedCodexUsageEvent(storage, now - 120, 20, rollout, "response:two");
    seedCodexUsageEvent(storage, now - 120, 20, rollout, "response:two");
    seedCodexUsageEvent(storage, now - 120, 20, "codex://rollout/copied", "response:two");

    seedCodexUsageEvent(storage, now - 60, 370);
    seedCodexUsageEvent(storage, now, 490);

    DashboardViewModel dashboard;
    dashboard.providers = {makeProvider("openai", "OpenAI", "openai.runtime_status")};
    AnalysisQueryEngine engine(storage);
    const auto analysis = engine.buildViewModel(dashboard, DashboardPreferences{}, now);

    REQUIRE(analysis.codexThroughput.hasData);
    CHECK(analysis.codexThroughput.tokensLast1h == 390);
    CHECK(analysis.codexThroughput.promptsLast1h == 5);
    REQUIRE(analysis.codexHourlyHistory.hasData);
    REQUIRE(analysis.codexHourlyHistory.points.size() == 5);
    const auto& minute = analysis.codexHourlyHistory.points[2];
    CHECK(minute.snapshotTime == now - 120);
    CHECK(analysis.codexHourlyHistory.points.back().tokensPerMinute == Catch::Approx(120));
    CHECK(minute.tokensPerMinute == Catch::Approx(50));
    CHECK(minute.promptsPerMinute == Catch::Approx(2));
}

TEST_CASE("Codex accounting preserves request ordering within a second", "[analysis][codex]") {
    requireSqlite();
    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    const std::string rollout = "codex://rollout/current";
    const auto now = makeUtcTime(2026, 4, 20, 18);
    const auto sameSecond = now - 120;
    seedCodexUsageEvent(storage, sameSecond, 100);
    seedCodexUsageEvent(storage, sameSecond, 50, rollout, "response:request");

    long long expectedTokens = 150;
    int baselineNs = 100000000;
    SECTION("The counter precedes the request") {
        seedCodexUsageEvent(storage, sameSecond, 100, rollout, {}, baselineNs);
        seedCodexUsageEvent(storage, sameSecond, 50, rollout, "response:request", 500000000);
    }
    SECTION("The request precedes the counter") {
        baselineNs = 800000000;
        seedCodexUsageEvent(storage, sameSecond, 100, rollout, {}, baselineNs);
        seedCodexUsageEvent(storage, sameSecond, 50, rollout, "response:request", 500000000);
        expectedTokens = 200;
    }
    SECTION("Seconds-only legacy rows use conservative overlap") {
        baselineNs = -1;
    }
    seedCodexUsageEvent(storage, now - 60, 250);

    const auto stored = storage.getUsageEvents(requireProviderId(storage, "openai"),
                                               ensureRuntimeAccountId(storage, "openai", "OpenAI"), sameSecond, now);
    REQUIRE(stored.size() == 3);
    const auto baseline =
        std::find_if(stored.begin(), stored.end(), [](const auto& row) { return row.totalTokens == 100; });
    REQUIRE(baseline != stored.end());
    if (baselineNs >= 0) {
        CHECK(baseline->eventSubsecondNs == baselineNs);
    } else {
        CHECK_FALSE(baseline->eventSubsecondNs.has_value());
    }

    const auto analysis =
        AnalysisQueryEngine(storage).buildViewModel(DashboardViewModel{}, DashboardPreferences{}, now);
    CHECK(analysis.codexThroughput.tokensLast1h == expectedTokens);
    CHECK(analysis.codexThroughput.promptsLast1h == 2);
    REQUIRE(analysis.codexHourlyHistory.points.size() == 2);
    CHECK(analysis.codexHourlyHistory.points.front().tokensPerMinute == Catch::Approx(50));
    CHECK(analysis.codexHourlyHistory.points.back().tokensPerMinute == Catch::Approx(expectedTokens - 50));
}

TEST_CASE("Codex rollout replay links requests to their previously saved cumulative baseline", "[analysis][codex]") {
    requireSqlite();
    const bool hasLegacyBaseline = GENERATE(true, false);
    CAPTURE(hasLegacyBaseline);
    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    JiStats::TestSupport::TempTestDirectory fixture("jistats_codex_upgrade");
    const auto rollout = fixture.path() / "rollout-upgrade.jsonl";
    {
        std::ofstream out(rollout);
        out << R"json({"timestamp":"2026-04-20T17:58:00.500Z","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":25,"output_tokens":25,"total_tokens":50},"total_token_usage":{"input_tokens":50,"output_tokens":50,"total_tokens":100}}}}
{"timestamp":"2026-04-20T17:59:00.123Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":100,"output_tokens":100,"total_tokens":200}}}}
)json";
        REQUIRE(out.good());
    }

    const int providerId = requireProviderId(storage, "openai");
    const int accountId = ensureRuntimeAccountId(storage, "openai", "OpenAI");
    const auto first = makeUtcTime(2026, 4, 20, 17, 58);
    const auto now = makeUtcTime(2026, 4, 20, 18);
    if (hasLegacyBaseline) {
        seedCodexUsageEvent(storage, first, 100, rollout.string());
    }

    const auto batch = OpenAIAdapter::collectCodexTokenUsageSnapshots(fixture.path().string());
    REQUIRE(batch.observations.size() == 2);
    REQUIRE(OpenAIAdapter::persistCodexTokenUsageSnapshots(storage, providerId, accountId, batch));
    REQUIRE(OpenAIAdapter::persistCodexTokenUsageSnapshots(storage, providerId, accountId, batch));

    const auto rows = storage.getUsageEvents(providerId, accountId, first, now);
    REQUIRE(rows.size() == (hasLegacyBaseline ? 3 : 2));
    const auto baseline =
        std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.totalTokens == 100; });
    if (hasLegacyBaseline) {
        REQUIRE(baseline != rows.end());
        CHECK(baseline->sourceRunId == "line:1");
        CHECK(baseline->requestType == "codex_turn");
        CHECK(baseline->eventSubsecondNs == 500000000);
    } else {
        CHECK(baseline == rows.end());
    }

    const auto analysis =
        AnalysisQueryEngine(storage).buildViewModel(DashboardViewModel{}, DashboardPreferences{}, now);
    CHECK(analysis.codexThroughput.tokensLast1h == (hasLegacyBaseline ? 150 : 50));
    REQUIRE(analysis.codexHourlyHistory.points.size() == 2);
    CHECK(analysis.codexHourlyHistory.points.front().tokensPerMinute == Catch::Approx(50));
    CHECK(analysis.codexHourlyHistory.points.back().tokensPerMinute == Catch::Approx(hasLegacyBaseline ? 100 : 0));
}

TEST_CASE("Codex rollout replay preserves the sequence of saved same-second counters", "[analysis][codex]") {
    requireSqlite();
    const bool laterHasPrecision = GENERATE(true, false);
    const bool legacyRowsOutOfOrder = GENERATE(false, true);
    CAPTURE(laterHasPrecision, legacyRowsOutOfOrder);
    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    JiStats::TestSupport::TempTestDirectory fixture("jistats_codex_order");
    const auto rollout = fixture.path() / "rollout-upgrade.jsonl";
    {
        std::ofstream out(rollout);
        const auto baselineTimestamp = legacyRowsOutOfOrder ? "17:58:00Z" : "17:57:00.200Z";
        out << R"json({"timestamp":"2026-04-20T)json" << baselineTimestamp
            << R"json(","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":50,"output_tokens":50,"total_tokens":100}}}}
{"timestamp":"2026-04-20T17:58:00.100Z","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":25,"output_tokens":25,"total_tokens":50},"total_token_usage":{"input_tokens":75,"output_tokens":75,"total_tokens":150}}}}
{"timestamp":"2026-04-20T17:58:00.700Z","type":"token_usage_record","payload":{"response_id":"response-2","usage":{"input_tokens":25,"output_tokens":25,"total_tokens":50}}}
{"timestamp":"2026-04-20T17:58:00)json";
        out << (laterHasPrecision ? ".800" : "");
        out << R"json(Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":100,"output_tokens":100,"total_tokens":200}}}}
{"type":"turn_context","payload":{}}
{"timestamp":"2026-04-20T17:59:00.300Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":125,"output_tokens":125,"total_tokens":250}}}}
)json";
        REQUIRE(out.good());
    }

    const auto now = makeUtcTime(2026, 4, 20, 18);
    const auto sameSecond = now - 120;
    seedCodexUsageEvent(storage, legacyRowsOutOfOrder ? sameSecond : now - 180, 100, rollout.string());
    seedCodexUsageEvent(storage, sameSecond, legacyRowsOutOfOrder ? 200 : 150, rollout.string());
    seedCodexUsageEvent(storage, sameSecond, legacyRowsOutOfOrder ? 150 : 200, rollout.string());
    seedCodexUsageEvent(storage, now - 60, 250, rollout.string());

    const int providerId = requireProviderId(storage, "openai");
    const int accountId = ensureRuntimeAccountId(storage, "openai", "OpenAI");
    const auto batch = OpenAIAdapter::collectCodexTokenUsageSnapshots(fixture.path().string());
    REQUIRE(OpenAIAdapter::persistCodexTokenUsageSnapshots(storage, providerId, accountId, batch));
    REQUIRE(OpenAIAdapter::persistCodexTokenUsageSnapshots(storage, providerId, accountId, batch));

    const auto analysis =
        AnalysisQueryEngine(storage).buildViewModel(DashboardViewModel{}, DashboardPreferences{}, now);
    CHECK(analysis.codexThroughput.tokensLast1h == 150);
    REQUIRE(analysis.codexHourlyHistory.points.size() == (legacyRowsOutOfOrder ? 2 : 3));
    const auto& firstRequestMinute = analysis.codexHourlyHistory.points[analysis.codexHourlyHistory.points.size() - 2];
    CHECK(firstRequestMinute.snapshotTime == sameSecond);
    CHECK(firstRequestMinute.tokensPerMinute == Catch::Approx(100));
    CHECK(analysis.codexHourlyHistory.points.back().tokensPerMinute == Catch::Approx(50));

    const auto rows = storage.getUsageEvents(providerId, accountId, now - 180, now);
    REQUIRE(rows.size() == 6);
    const auto laterCounter =
        std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.totalTokens == 200; });
    REQUIRE(laterCounter != rows.end());
    CHECK(laterCounter->eventSubsecondNs == (laterHasPrecision ? std::optional<int>(800000000) : std::nullopt));
}

TEST_CASE("Codex collection and replay preserve resets at whole-second rollout records", "[analysis][codex]") {
    requireSqlite();
    const bool legacy = GENERATE(false, true);
    const int nextCounter = GENERATE(50, 175);
    CAPTURE(legacy, nextCounter);
    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));
    JiStats::TestSupport::TempTestDirectory fixture("jistats_codex_reset");
    const auto rollout = fixture.path() / "rollout-reset.jsonl";
    {
        std::ofstream out(rollout);
        out << R"json({"timestamp":"2026-04-20T17:57:00.200Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":50,"output_tokens":50,"total_tokens":100}}}}
{"timestamp":"2026-04-20T17:58:00.100Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":75,"output_tokens":75,"total_tokens":150}}}}
{"timestamp":"2026-04-20T17:58:00Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":12,"output_tokens":13,"total_tokens":25}}}}
{"timestamp":"2026-04-20T17:59:00.300Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":)json";
        out << nextCounter / 2 << R"json(,"output_tokens":)json" << nextCounter - nextCounter / 2
            << R"json(,"total_tokens":)json" << nextCounter << "}}}}\n";
        REQUIRE(out.good());
    }

    const auto now = makeUtcTime(2026, 4, 20, 18);
    if (legacy) {
        seedCodexUsageEvent(storage, now - 180, 100, rollout.string());
        seedCodexUsageEvent(storage, now - 120, 25, rollout.string());
        seedCodexUsageEvent(storage, now - 120, 150, rollout.string());
        seedCodexUsageEvent(storage, now - 60, nextCounter, rollout.string());
    }

    const int providerId = requireProviderId(storage, "openai");
    const int accountId = ensureRuntimeAccountId(storage, "openai", "OpenAI");
    const auto batch = OpenAIAdapter::collectCodexTokenUsageSnapshots(fixture.path().string());
    REQUIRE(OpenAIAdapter::persistCodexTokenUsageSnapshots(storage, providerId, accountId, batch));
    REQUIRE(OpenAIAdapter::persistCodexTokenUsageSnapshots(storage, providerId, accountId, batch));

    const auto analysis =
        AnalysisQueryEngine(storage).buildViewModel(DashboardViewModel{}, DashboardPreferences{}, now);
    CHECK(analysis.codexThroughput.tokensLast1h == 50 + nextCounter - 25);
    REQUIRE(analysis.codexHourlyHistory.points.size() == 3);
    CHECK(analysis.codexHourlyHistory.points[1].tokensPerMinute == Catch::Approx(50));
    CHECK(analysis.codexHourlyHistory.points.back().tokensPerMinute == Catch::Approx(nextCounter - 25));

    const auto rows = storage.getUsageEvents(providerId, accountId, now - 180, now);
    REQUIRE(rows.size() == 4);
    const auto reset = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.totalTokens == 25; });
    REQUIRE(reset != rows.end());
    CHECK(reset->sourceRunId == "line:3");
    CHECK_FALSE(reset->eventSubsecondNs.has_value());
}

TEST_CASE("Codex estimates unsequenced counters using neighboring seconds", "[analysis][codex]") {
    requireSqlite();
    const long long nextCounter = GENERATE(50, 175);
    CAPTURE(nextCounter);
    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    const std::string rollout = "codex://rollout/current";
    const auto now = makeUtcTime(2026, 4, 20, 18);
    seedCodexUsageEvent(storage, now - 180, 100, rollout);
    seedCodexUsageEvent(storage, now - 120, 25, rollout);
    seedCodexUsageEvent(storage, now - 120, 150, rollout, {}, 100000000);
    seedCodexUsageEvent(storage, now - 60, nextCounter, rollout);

    const auto analysis =
        AnalysisQueryEngine(storage).buildViewModel(DashboardViewModel{}, DashboardPreferences{}, now);
    const long long firstMinuteTokens = nextCounter == 50 ? 50 : 125;
    CHECK(analysis.codexThroughput.tokensLast1h == firstMinuteTokens + 25);
    CHECK(analysis.codexThroughput.confidenceLabel == "Estimated");
    REQUIRE(analysis.codexHourlyHistory.points.size() == 3);
    CHECK(analysis.codexHourlyHistory.points[1].tokensPerMinute == Catch::Approx(firstMinuteTokens));
    CHECK(analysis.codexHourlyHistory.points.back().tokensPerMinute == Catch::Approx(25));
}

TEST_CASE("Codex legacy readings in different seconds establish resets without rollout order", "[analysis][codex]") {
    requireSqlite();
    const bool reverseRowOrder = GENERATE(false, true);
    const bool nextSecondIsUnsequenced = GENERATE(false, true);
    CAPTURE(reverseRowOrder, nextSecondIsUnsequenced);
    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    const std::string rollout = "codex://rollout/no-longer-available";
    const auto now = makeUtcTime(2026, 4, 20, 18);
    seedCodexUsageEvent(storage, now - 180, 100, rollout);
    seedCodexUsageEvent(storage, now - 120, reverseRowOrder ? 50 : 25, rollout);
    seedCodexUsageEvent(storage, now - 120, reverseRowOrder ? 25 : 50, rollout);
    seedCodexUsageEvent(storage, now - 60, 150, rollout);
    if (nextSecondIsUnsequenced) {
        seedCodexUsageEvent(storage, now - 60, 125, rollout);
        seedCodexUsageEvent(storage, now, 175, rollout);
    }

    const auto analysis =
        AnalysisQueryEngine(storage).buildViewModel(DashboardViewModel{}, DashboardPreferences{}, now);
    CHECK(analysis.codexThroughput.tokensLast1h == (nextSecondIsUnsequenced ? 150 : 125));
    CHECK(analysis.codexThroughput.confidenceLabel == "Estimated");
    REQUIRE(analysis.codexHourlyHistory.points.size() == (nextSecondIsUnsequenced ? 4 : 3));
    CHECK(analysis.codexHourlyHistory.points[1].tokensPerMinute == Catch::Approx(25));
    CHECK(analysis.codexHourlyHistory.points[2].tokensPerMinute == Catch::Approx(100));
    if (nextSecondIsUnsequenced) {
        CHECK(analysis.codexHourlyHistory.points.back().tokensPerMinute == Catch::Approx(25));
    }
}

TEST_CASE("Codex estimates adjacent unordered seconds together", "[analysis][codex]") {
    requireSqlite();
    const bool hasFractionalOrder = GENERATE(false, true);
    const bool reverseLegacyRows = GENERATE(false, true);
    const bool extendsUnorderedRun = GENERATE(false, true);
    CAPTURE(hasFractionalOrder, reverseLegacyRows, extendsUnorderedRun);
    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    const std::string rollout = "codex://rollout/no-longer-available";
    const auto now = makeUtcTime(2026, 4, 20, 18);
    const auto seedPair = [&](std::time_t seconds, long long first, long long last) {
        if (hasFractionalOrder) {
            seedCodexUsageEvent(storage, seconds, first, rollout, {}, 100000000);
            seedCodexUsageEvent(storage, seconds, last, rollout, {}, 800000000);
        } else {
            seedCodexUsageEvent(storage, seconds, reverseLegacyRows ? last : first, rollout);
            seedCodexUsageEvent(storage, seconds, reverseLegacyRows ? first : last, rollout);
        }
    };
    seedCodexUsageEvent(storage, now - 240, 100, rollout);
    seedPair(now - 180, 150, 50);
    seedPair(now - 120, 60, 175);
    if (extendsUnorderedRun) {
        seedPair(now - 60, 180, 195);
    }
    seedCodexUsageEvent(storage, extendsUnorderedRun ? now : now - 60, 200, rollout);

    const auto analysis =
        AnalysisQueryEngine(storage).buildViewModel(DashboardViewModel{}, DashboardPreferences{}, now);
    CHECK(analysis.codexThroughput.tokensLast1h == 200);
    REQUIRE(analysis.codexHourlyHistory.points.size() == (extendsUnorderedRun ? 5 : 4));
    CHECK(analysis.codexHourlyHistory.points[0].tokensPerMinute == Catch::Approx(0));
    CHECK(analysis.codexHourlyHistory.points[1].tokensPerMinute == Catch::Approx(50));
    CHECK(analysis.codexHourlyHistory.points[2].tokensPerMinute == Catch::Approx(125));
    CHECK(analysis.codexHourlyHistory.points[3].tokensPerMinute == Catch::Approx(extendsUnorderedRun ? 20 : 25));
    if (extendsUnorderedRun) {
        CHECK(analysis.codexHourlyHistory.points.back().tokensPerMinute == Catch::Approx(5));
    }
}

TEST_CASE("Codex tallies distinct interior readings from a legacy second", "[analysis][codex]") {
    requireSqlite();
    const bool reset = GENERATE(false, true);
    const int timedReadings = GENERATE(0, 1, 3);
    CAPTURE(reset, timedReadings);
    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    const std::string rollout = "codex://rollout/no-longer-available";
    const auto now = makeUtcTime(2026, 4, 20, 18);
    seedCodexUsageEvent(storage, now - 180, 100, rollout);
    const std::vector<long long> readings =
        reset ? std::vector<long long>{150, 25, 50} : std::vector<long long>{125, 150, 175};
    // Insert out of order for the legacy, mixed-precision and fully timed controls.
    for (int index : {2, 0, 1}) {
        const std::optional<int> nanos =
            index < timedReadings ? std::optional<int>((index + 1) * 100000000) : std::nullopt;
        seedCodexUsageEvent(storage, now - 120, readings[index], rollout, {}, nanos);
    }
    if (reset && timedReadings == 1) {
        // The known 150-to-145 decline cannot account for a later 25-to-50 gain.
        seedCodexUsageEvent(storage, now - 120, 145, rollout, {}, 150000000);
    }
    seedCodexUsageEvent(storage, now - 60, reset ? 75 : 200, rollout);

    const auto analysis =
        AnalysisQueryEngine(storage).buildViewModel(DashboardViewModel{}, DashboardPreferences{}, now);
    CHECK(analysis.codexThroughput.tokensLast1h == 100);
    CHECK(analysis.codexThroughput.promptsLast1h == (reset ? 3 : 4));
    REQUIRE(analysis.codexHourlyHistory.points.size() == 3);
    CHECK(analysis.codexHourlyHistory.points[0].tokensPerMinute == Catch::Approx(0));
    CHECK(analysis.codexHourlyHistory.points[1].tokensPerMinute == Catch::Approx(75));
    CHECK(analysis.codexHourlyHistory.points[2].tokensPerMinute == Catch::Approx(25));
}

TEST_CASE("Codex request overlap stays within the enclosing timed counters", "[analysis][codex]") {
    requireSqlite();
    const int requestNs = GENERATE(100000000, 450000000, 900000000);
    const bool timedInterior = GENERATE(false, true);
    const bool twoInteriorReadings = GENERATE(false, true);
    CAPTURE(requestNs, timedInterior, twoInteriorReadings);
    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    const std::string rollout = "codex://rollout/mixed-precision";
    const auto now = makeUtcTime(2026, 4, 20, 18);
    const auto second = now - 60;
    seedCodexUsageEvent(storage, second, 100, rollout, {}, 300000000);
    seedCodexUsageEvent(storage, second, 150, rollout, {},
                        timedInterior ? std::optional<int>(400000000) : std::nullopt);
    if (twoInteriorReadings) {
        seedCodexUsageEvent(storage, second, 200, rollout, {},
                            timedInterior ? std::optional<int>(500000000) : std::nullopt);
    }
    seedCodexUsageEvent(storage, second, twoInteriorReadings ? 250 : 200, rollout, {}, 600000000);
    seedCodexUsageEvent(storage, second, 50, rollout, "response:recorded", requestNs);

    const auto analysis =
        AnalysisQueryEngine(storage).buildViewModel(DashboardViewModel{}, DashboardPreferences{}, now);
    const bool separateRequest = requestNs < 300000000 || requestNs > 600000000;
    const auto expectedPrompts = 2 + twoInteriorReadings + separateRequest;
    CHECK(analysis.codexThroughput.promptsLast1h == expectedPrompts);
    CHECK(analysis.codexThroughput.tokensLast1h == expectedPrompts * 50);
    REQUIRE(analysis.codexHourlyHistory.points.size() == 1);
    CHECK(analysis.codexHourlyHistory.points.front().tokensPerMinute == Catch::Approx(expectedPrompts * 50));
}

TEST_CASE("An unsequenced Codex counter does not replace a matching timed boundary", "[analysis][codex]") {
    requireSqlite();
    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    const std::string rollout = "codex://rollout/current";
    const auto now = makeUtcTime(2026, 4, 20, 18);
    seedCodexUsageEvent(storage, now - 180, 100, rollout);
    seedCodexUsageEvent(storage, now - 120, 150, rollout);
    REQUIRE(
        storage
            .execute(
                "INSERT INTO usage_events (provider_id, account_id, event_time_utc, event_subsecond_ns, source_type, "
                "source_run_id, model_key, request_type, input_tokens, output_tokens, total_tokens, raw_reference) "
                "SELECT provider_id, account_id, event_time_utc, 100000000, source_type, 'line:2', model_key, "
                "request_type, input_tokens, output_tokens, total_tokens, raw_reference FROM usage_events "
                "WHERE source_type = 'codex_local_rollout' AND total_tokens = 150;")
            .success());
    seedCodexUsageEvent(storage, now - 120, 30, rollout, "response:after-counter", 700000000);
    seedCodexUsageEvent(storage, now - 60, 200, rollout);

    const auto analysis =
        AnalysisQueryEngine(storage).buildViewModel(DashboardViewModel{}, DashboardPreferences{}, now);
    CHECK(analysis.codexThroughput.tokensLast1h == 100);
    REQUIRE(analysis.codexHourlyHistory.points.size() == 3);
    CHECK(analysis.codexHourlyHistory.points[1].tokensPerMinute == Catch::Approx(80));
    CHECK(analysis.codexHourlyHistory.points.back().tokensPerMinute == Catch::Approx(20));
}

TEST_CASE("Codex accounting preserves timed counter resets in a mixed-precision second", "[analysis][codex]") {
    requireSqlite();
    const long long unsequenced = GENERATE(75, 200);
    CAPTURE(unsequenced);
    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    const std::string rollout = "codex://rollout/current";
    const auto now = makeUtcTime(2026, 4, 20, 18);
    const auto sameSecond = now - 120;
    seedCodexUsageEvent(storage, now - 180, 100, rollout);
    seedCodexUsageEvent(storage, sameSecond, 50, rollout, {}, 800000000);
    seedCodexUsageEvent(storage, sameSecond, unsequenced, rollout);
    seedCodexUsageEvent(storage, sameSecond, 25, rollout, {}, 500000000);
    seedCodexUsageEvent(storage, sameSecond, 150, rollout, {}, 100000000);
    seedCodexUsageEvent(storage, now - 60, 100, rollout);

    const auto analysis =
        AnalysisQueryEngine(storage).buildViewModel(DashboardViewModel{}, DashboardPreferences{}, now);
    const long long firstMinuteTokens = unsequenced == 75 ? 75 : 125;
    CHECK(analysis.codexThroughput.tokensLast1h == firstMinuteTokens + 50);
    CHECK(analysis.codexThroughput.confidenceLabel == "Estimated");
    REQUIRE(analysis.codexHourlyHistory.points.size() == 3);
    CHECK(analysis.codexHourlyHistory.points[1].tokensPerMinute == Catch::Approx(firstMinuteTokens));
    CHECK(analysis.codexHourlyHistory.points.back().tokensPerMinute == Catch::Approx(50));
}

TEST_CASE("AnalysisQueryEngine limits Codex throughput chart history to the active thirty-minute window", "[analysis][query_engine][analysis_query_engine][codex]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    const std::time_t now = makeUtcTime(2026, 4, 20, 18, 0, 0);
    seedCodexUsageEvent(storage, now - 3600, 100);
    seedCodexUsageEvent(storage, now - 120, 200);
    seedCodexUsageEvent(storage, now - 60, 300);

    DashboardViewModel dashboard;
    dashboard.providers = {makeProvider("openai", "OpenAI", "openai.runtime_status")};
    DashboardPreferences preferences;
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Last24Hours;

    AnalysisQueryEngine engine(storage);
    const AnalysisViewModel analysis = engine.buildViewModel(dashboard, preferences, now);

    REQUIRE(analysis.codexHourlyHistory.hasData);
    REQUIRE(analysis.codexHourlyHistory.points.size() == 2);
    CHECK(analysis.codexHourlyHistory.points.front().snapshotTime == now - 120);
    CHECK(analysis.codexHourlyHistory.points.back().snapshotTime == now - 60);
}

TEST_CASE("AnalysisQueryEngine separates Codex calendar month totals from rolling thirty day totals", "[analysis][query_engine][analysis_query_engine][codex]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    const std::time_t now = makeUtcTime(2026, 4, 20, 18, 0, 0);
    seedCodexUsageEvent(storage, now - (25 * 86400), 100, "codex://rollout/march");
    seedCodexUsageEvent(storage, now - (24 * 86400), 400, "codex://rollout/march");
    seedCodexUsageEvent(storage, makeUtcTime(2026, 4, 5, 12, 0, 0), 50, "codex://rollout/april");
    seedCodexUsageEvent(storage, makeUtcTime(2026, 4, 19, 12, 0, 0), 250, "codex://rollout/april");

    DashboardViewModel dashboard;
    dashboard.providers = {makeProvider("openai", "OpenAI", "openai.runtime_status")};
    DashboardPreferences preferences;
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Last24Hours;

    AnalysisQueryEngine engine(storage);
    const AnalysisViewModel analysis = engine.buildViewModel(dashboard, preferences, now);

    REQUIRE(analysis.codexThroughput.hasData);
    CHECK(analysis.codexThroughput.calendarMonthTokens == 200);
    CHECK(analysis.codexThroughput.calendarMonthPrompts == 1);
    CHECK(analysis.codexThroughput.rolling30DayTokens == 500);
    CHECK(analysis.codexThroughput.rolling30DayPrompts == 2);
    CHECK(analysis.codexThroughput.averageDailyTokens == 16);
    CHECK(analysis.codexThroughput.averageMonthlyTokens == 10);
}

TEST_CASE("AnalysisQueryEngine computes Codex throughput within rollout session boundaries", "[analysis][query_engine][analysis_query_engine][codex]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    const std::time_t now = makeUtcTime(2026, 4, 20, 18, 0, 0);
    seedCodexUsageEvent(storage, now - 120, 100, "codex://rollout/a");
    seedCodexUsageEvent(storage, now - 90, 10000, "codex://rollout/b");
    seedCodexUsageEvent(storage, now - 60, 200, "codex://rollout/a");

    DashboardViewModel dashboard;
    dashboard.providers = {makeProvider("openai", "OpenAI", "openai.runtime_status")};

    DashboardPreferences preferences;
    preferences.analysisSelectedProviderId = "openai";
    preferences.analysisSelectedAccountId = "openai.runtime_status";
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Last24Hours;

    AnalysisQueryEngine engine(storage);
    const AnalysisViewModel analysis = engine.buildViewModel(dashboard, preferences, now);

    REQUIRE(analysis.codexThroughput.hasData);
    CHECK(analysis.codexThroughput.tokensPerMinute5m == Catch::Approx(20.0));
    CHECK(analysis.codexThroughput.promptsPerMinute5m == Catch::Approx(0.2));
    CHECK(analysis.codexThroughput.confidenceLabel == "Estimated");
}

TEST_CASE("Percentage quota history marks resets only when reported bounds change", "[analysis][quota]") {
    requireSqlite();
    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    const int providerId = requireProviderId(storage, "grok");
    const int accountId = ensureRuntimeAccountId(storage, "grok", "Grok");
    auto insert = [&](const char* observedAt, const char* windowStart, const char* windowEnd) {
        REQUIRE(
            storage
                .execute("INSERT INTO quota_snapshots (provider_id, account_id, snapshot_time_utc, window_start_utc, "
                         "window_end_utc, used_value, limit_value, confidence_tier, quota_dimension) "
                         "VALUES (?, ?, ?, ?, ?, 40, 100, 'high', 'percent');",
                         {std::to_string(providerId), std::to_string(accountId), observedAt, windowStart, windowEnd})
                .success());
    };

    // Include a previously stored poll-time placeholder and two observations without reported dates.
    insert("2026-04-20T13:00:00Z", "2026-04-20T13:00:00Z", "2026-04-20T13:00:00Z");
    insert("2026-04-20T14:00:00Z", "", "");
    insert("2026-04-20T15:00:00Z", "", "");
    insert("2026-04-20T16:00:00Z", "", "2026-04-20T17:00:00Z");
    insert("2026-04-20T16:30:00Z", "2026-04-13T17:00:00Z", "2026-04-20T17:00:00Z");
    insert("2026-04-20T17:15:00Z", "2026-04-13T17:00:00Z", "");
    insert("2026-04-20T17:30:00Z", "", "2026-04-27T17:00:00Z");
    insert("2026-04-20T17:45:00Z", "2026-04-20T17:00:00Z", "2026-04-27T17:00:00Z");

    const auto from = makeUtcTime(2026, 4, 20);
    const auto now = makeUtcTime(2026, 4, 20, 18);
    DashboardViewModel dashboard;
    DashboardPreferences preferences;
    preferences.analysisSelectedProviderId = "grok";
    preferences.analysisSelectedAccountId = "grok.runtime_status";
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Last24Hours;
    auto analysis = AnalysisQueryEngine(storage).buildViewModel(dashboard, preferences, now);
    REQUIRE(analysis.quotaTrendSeries.size() == 1);
    const auto& series = analysis.quotaTrendSeries.front();
    REQUIRE(series.points.size() == 8);
    CHECK(series.resetCount == 1);
    for (std::size_t index = 0; index < series.points.size(); ++index) {
        CHECK(series.points[index].resetPoint == (index == 6));
    }
    CHECK(series.points.back().remainingPercentage == Catch::Approx(60));

    REQUIRE(storage.rebuildQuotaSnapshotRollups(providerId, accountId, from, now));
    const auto rollups = storage.getQuotaSnapshotRollups(providerId, accountId, from, from);
    REQUIRE(rollups.size() == 1);
    CHECK(rollups.front().sampleCount == 8);
    CHECK(rollups.front().resetCount == 1);
}

TEST_CASE("AnalysisQueryEngine keeps Codex model usage buckets granular for recent deltas", "[analysis][query_engine][analysis_query_engine][codex]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    const std::time_t now = makeUtcTime(2026, 4, 20, 18, 0, 0);
    seedCodexSqliteUsageEvent(storage, "openai", "gpt-5.5", now - 45 * 60, 100);
    seedCodexSqliteUsageEvent(storage, "openai", "gpt-5.5", now - 38 * 60, 200);
    seedCodexSqliteUsageEvent(storage, "openai", "gpt-5.5", now - 31 * 60, 300);
    seedCodexSqliteUsageEvent(storage, "openai", "gpt-5.5", now - 24 * 60, 400);
    seedUsageEvent(storage, "minimax", "MiniMax", now - 10 * 60, 50, 2, "billable-test-model");

    DashboardViewModel dashboard;
    dashboard.providers = {makeProvider("openai", "OpenAI", "openai.runtime_status")};
    DashboardPreferences preferences;
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Last24Hours;

    AnalysisQueryEngine engine(storage);
    const AnalysisViewModel analysis = engine.buildViewModel(dashboard, preferences, now);

    REQUIRE(analysis.openAiCodexModelUsage.hasData);
    REQUIRE(analysis.openAiCodexModelUsage.series.size() == 1);
    CHECK(analysis.openAiCodexModelUsage.series.front().points.size() == 4);
    CHECK(analysis.openAiCodexModelUsage.series.front().totalTokens == 1000);

    long long activityCount = 0;
    for (const auto& point : analysis.trendPoints) {
        activityCount += point.activityCount;
    }
    CHECK(activityCount == 1);
    REQUIRE(analysis.providerBreakdown.size() == 1);
    CHECK(analysis.providerBreakdown.front().label == "MiniMax");
    CHECK(analysis.providerBreakdown.front().activityCount == 1);
}

TEST_CASE("Analysis keeps attribution-only accounts out of general report choices",
          "[analysis][query_engine][analysis_query_engine][codex]") {
    requireSqlite();
    TempTestDatabase dbPath;
    StorageService storage;
    REQUIRE(storage.init(dbPath.path()));
    const std::time_t now = makeUtcTime(2026, 4, 20, 18);
    seedCodexSqliteUsageEvent(storage, "openai", "gpt-5.5", now - 600, 321);
    const int providerId = requireProviderId(storage, "openai");
    REQUIRE(storage
                .execute("UPDATE accounts SET account_key = 'openai.codex_local_sqlite' "
                         "WHERE provider_id = ?;",
                         {std::to_string(providerId)})
                .success());
    seedUsageEvent(storage, "openai", "OpenAI", now - 300, 777, 1, "billable-model");

    DashboardPreferences preferences;
    preferences.analysisSelectedProviderId = "openai";
    preferences.analysisSelectedAccountId = "openai.codex_local_sqlite";
    AnalysisQueryEngine engine(storage);
    const auto analysis = engine.buildViewModel({}, preferences, now);
    CHECK(analysis.scope.selectedAccountId == "openai.codex_local_sqlite");
    CHECK(analysis.scopeLabel.find("model attribution") != std::string::npos);
    CHECK(optionPairs(analysis.availableAccounts) ==
          std::vector<std::string>{"|All Accounts", "openai.runtime_status|OpenAI / OpenAI Runtime"});
    REQUIRE(analysis.openAiCodexModelUsage.series.size() == 1);
    CHECK(analysis.openAiCodexModelUsage.series.front().totalTokens == 321);
    CHECK(analysis.accountBreakdown.empty());
}

TEST_CASE("AnalysisQueryEngine keeps an account selectable after only daily rollups remain",
          "[analysis][query_engine][analysis_query_engine]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));
    const std::time_t day = makeUtcTime(2026, 4, 18);
    for (int hour = 8; hour < 13; ++hour) {
        seedUsageEvent(storage, "openai", "OpenAI", day + hour * 3600, 100, 0, "gpt-4.1");
    }
    const int providerId = requireProviderId(storage, "openai");
    const int accountId = ensureRuntimeAccountId(storage, "openai", "OpenAI");
    REQUIRE(storage.rebuildUsageRollups(UsageRollupResolution::Daily, providerId, accountId, day, day));
    REQUIRE(storage.execute("DELETE FROM usage_events WHERE account_id = ?;", {std::to_string(accountId)}).success());
    seedUsageEvent(storage, "minimax", "MiniMax", day + 3600, 777, 1, "minimax-m1");

    DashboardViewModel dashboard;
    dashboard.providers = {makeProvider("minimax", "MiniMax", "minimax.runtime_status")};
    DashboardPreferences preferences;
    preferences.analysisSelectedProviderId = "openai";
    preferences.analysisSelectedAccountId = "openai.runtime_status";
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Custom;
    preferences.analysisCustomRangeStartUtc = day;
    preferences.analysisCustomRangeEndUtc = day;

    AnalysisQueryEngine engine(storage);
    const auto analysis = engine.buildViewModel(dashboard, preferences, makeUtcTime(2026, 4, 20));
    CHECK(analysis.scope.selectedProviderId == "openai");
    CHECK(analysis.scope.selectedAccountId == "openai.runtime_status");
    CHECK(optionPairs(analysis.availableAccounts) ==
          std::vector<std::string>{"|All Accounts", "openai.runtime_status|OpenAI / OpenAI Runtime"});
    const auto daily = findDailySummary(analysis.dailySummaries, "2026-04-18");
    REQUIRE(daily.has_value());
    CHECK(daily->activityCount == 5);
    REQUIRE(analysis.accountBreakdown.size() == 1);
    CHECK(analysis.accountBreakdown.front().label == "OpenAI Runtime");
    CHECK(analysis.accountBreakdown.front().activityCount == 5);
}

TEST_CASE("AnalysisQueryEngine clears stale scope selections without inventing scoped account rows", "[analysis][query_engine][analysis_query_engine]") {
    StorageService storage;
    AnalysisQueryEngine engine(storage);

    DashboardViewModel dashboard;
    dashboard.providers = {
        makeProvider("openai", "OpenAI", "acct-openai-primary"),
        makeProvider("minimax", "MiniMax", "acct-minimax")
    };

    DashboardPreferences preferences;
    preferences.analysisSelectedProviderId = "missing-provider";
    preferences.analysisSelectedAccountId = "missing-account";
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Last7Days;

    const AnalysisViewModel analysis = engine.buildViewModel(
        dashboard,
        preferences,
        makeUtcTime(2026, 4, 20, 18, 0, 0));

    CHECK(analysis.scope.selectedProviderId.empty());
    CHECK(analysis.scope.selectedAccountId.empty());
    CHECK(analysis.scopeLabel == "All Providers / All Accounts / Last 7 Days");
    REQUIRE_FALSE(analysis.availableAccounts.empty());
    CHECK(analysis.availableAccounts.front().key.empty());
    CHECK(analysis.availableAccounts.front().label == "All Accounts");
    CHECK(analysis.dailySummaries.empty());
    CHECK(analysis.providerBreakdown.empty());
    CHECK(analysis.accountBreakdown.empty());
    CHECK(analysis.trendPoints.empty());
}

TEST_CASE("AnalysisQueryEngine resolves custom windows to UTC day bounds and clamps the end at now", "[analysis][query_engine][analysis_query_engine]") {
    StorageService storage;
    AnalysisQueryEngine engine(storage);

    DashboardPreferences preferences;
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Custom;
    preferences.analysisCustomRangeStartUtc = makeUtcTime(2026, 4, 18, 15, 30, 0);
    preferences.analysisCustomRangeEndUtc = makeUtcTime(2026, 4, 20, 1, 0, 0);

    const std::time_t now = makeUtcTime(2026, 4, 19, 12, 34, 56);
    const ResolvedAnalysisWindow window = engine.resolveWindow(preferences, now);

    CHECK(window.start == makeUtcTime(2026, 4, 18, 0, 0, 0));
    CHECK(window.end == now);
    CHECK(window.label == "2026-04-18 to 2026-04-19 UTC");
    CHECK(toTimeT(window.reportDateRange.start) == window.start);
    CHECK(toTimeT(window.reportDateRange.end) == window.end);
}

TEST_CASE("AnalysisQueryEngine keeps wholly future custom windows empty",
          "[analysis][query_engine][analysis_query_engine]") {
    requireSqlite();
    TempTestDatabase dbPath;
    StorageService storage;
    REQUIRE(storage.init(dbPath.path()));
    const auto now = makeUtcTime(2026, 4, 20, 12, 0, 0);
    seedUsageEvent(storage, "openai", "OpenAI", now, 777, 1, "current-model");

    DashboardPreferences preferences;
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Custom;
    preferences.analysisCustomRangeStartUtc = makeUtcTime(2026, 4, 22);
    preferences.analysisCustomRangeEndUtc = makeUtcTime(2026, 4, 24);
    AnalysisQueryEngine engine(storage);
    const auto window = engine.resolveWindow(preferences, now);
    CHECK(window.start == preferences.analysisCustomRangeStartUtc);
    CHECK(window.end == makeUtcTime(2026, 4, 24, 23, 59, 59));
    CHECK(toTimeT(window.reportDateRange.start) == window.start);
    CHECK(toTimeT(window.reportDateRange.end) == window.end);

    const auto analysis = engine.buildViewModel({}, preferences, now);
    CHECK(analysis.scope.dateRangePreset == AnalysisDateRangePreset::Custom);
    CHECK(analysis.scopeLabel.find("2026-04-22 to 2026-04-24 UTC") != std::string::npos);
    CHECK(analysis.dataQualityNote.find("future") != std::string::npos);
    CHECK(analysis.dailySummaries.empty());
    CHECK(analysis.trendPoints.empty());
    CHECK(analysis.providerBreakdown.empty());
    CHECK(analysis.quotaTrendSeries.empty());
}

TEST_CASE("AnalysisQueryEngine falls back invalid custom preferences to the effective last-7-days preset", "[analysis][query_engine][analysis_query_engine]") {
    StorageService storage;
    AnalysisQueryEngine engine(storage);

    DashboardViewModel dashboard;
    DashboardPreferences preferences;
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Custom;

    const std::time_t now = makeUtcTime(2026, 4, 19, 12, 34, 56);
    const AnalysisViewModel analysis = engine.buildViewModel(dashboard, preferences, now);
    const ResolvedAnalysisWindow window = engine.resolveWindow(preferences, now);

    CHECK(window.effectivePreset == AnalysisDateRangePreset::Last7Days);
    CHECK(window.label == "Last 7 Days");
    CHECK(analysis.scope.dateRangePreset == AnalysisDateRangePreset::Last7Days);
    CHECK(analysis.scopeLabel == "All Providers / All Accounts / Last 7 Days");
    REQUIRE(analysis.requestedScope.has_value());
    CHECK(analysis.requestedScope->dateRangePreset == AnalysisDateRangePreset::Custom);
    CHECK(analysis.effectiveStartUtc == makeUtcTime(2026, 4, 13));
    CHECK(analysis.effectiveEndUtc == now);
}

TEST_CASE("AnalysisQueryEngine falls back missing current-quota windows to the effective this-month preset", "[analysis][query_engine][analysis_query_engine]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    AnalysisQueryEngine engine(storage);
    DashboardViewModel dashboard;
    DashboardPreferences preferences;
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::CurrentQuotaWindow;

    const std::time_t now = makeUtcTime(2026, 4, 19, 12, 34, 56);
    const AnalysisViewModel analysis = engine.buildViewModel(dashboard, preferences, now);
    const ResolvedAnalysisWindow window = engine.resolveWindow(preferences, now);

    CHECK(window.effectivePreset == AnalysisDateRangePreset::ThisMonth);
    CHECK(window.label == "This Month");
    CHECK(analysis.scope.dateRangePreset == AnalysisDateRangePreset::ThisMonth);
    CHECK(analysis.scopeLabel == "All Providers / All Accounts / This Month");
}

TEST_CASE("AnalysisQueryEngine combines persisted usage and local activity into shared provider and account breakdowns", "[analysis][query_engine][analysis_query_engine]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    const std::time_t now = makeUtcTime(2026, 4, 20, 18, 0, 0);
    const std::time_t openaiDay = makeUtcTime(2026, 4, 18, 8, 0, 0);
    const std::time_t localDay = makeUtcTime(2026, 4, 19, 9, 0, 0);
    seedUsageEvent(storage, "openai", "OpenAI", openaiDay, 240, 18, "gpt-4.1");
    seedLocalActivity(storage, "openai", "OpenAI", "openai_subscription_local", localDay, 300);
    seedLocalActivity(storage, "claude_code", "Claude Code", "claude_code_local_stats", localDay, 420);
    seedGapMarker(
        storage,
        "openai",
        "OpenAI",
        AnalysisGapClassification::Reconstructed,
        makeUtcTime(2026, 4, 19, 0, 0, 0),
        "recovered activity window",
        "recovery");

    AnalysisQueryEngine engine(storage);

    DashboardViewModel dashboard;
    dashboard.providers = {
        makeProvider("openai", "OpenAI", "openai.runtime_status"),
        makeProvider("claude_code", "Claude Code", "claude_code.runtime_status")
    };

    DashboardPreferences preferences;
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Last7Days;

    const AnalysisViewModel analysis = engine.buildViewModel(
        dashboard,
        preferences,
        now);

    CHECK(analysis.scope.selectedProviderId.empty());
    CHECK(analysis.scope.selectedAccountId.empty());
    CHECK(analysis.scopeLabel == "All Providers / All Accounts / Last 7 Days");
    CHECK(std::any_of(
        analysis.availableProviders.begin(),
        analysis.availableProviders.end(),
        [](const AnalysisScopeOption& option) { return option.key == "openai"; }));
    CHECK(std::any_of(
        analysis.availableProviders.begin(),
        analysis.availableProviders.end(),
        [](const AnalysisScopeOption& option) { return option.key == "claude_code"; }));

    const auto openaiDaySummary = findDailySummary(analysis.dailySummaries, "2026-04-18");
    const auto localDaySummary = findDailySummary(analysis.dailySummaries, "2026-04-19");
    REQUIRE(openaiDaySummary.has_value());
    REQUIRE(localDaySummary.has_value());
    CHECK(openaiDaySummary->activityCount >= 1);
    CHECK(localDaySummary->activityCount >= 2);
    CHECK(openaiDaySummary->sourceLabel != "Unknown");
    CHECK(localDaySummary->sourceLabel != "Unknown");
    CHECK(localDaySummary->gapDetail.classification != AnalysisGapClassification::None);

    const auto openaiProviderRow = findBreakdownRow(analysis.providerBreakdown, "OpenAI");
    const auto claudeProviderRow = findBreakdownRow(analysis.providerBreakdown, "Claude Code");
    REQUIRE(openaiProviderRow.has_value());
    REQUIRE(claudeProviderRow.has_value());
    CHECK(openaiProviderRow->activityCount >= 2);
    CHECK(claudeProviderRow->activityCount >= 1);
    CHECK(openaiProviderRow->sourceLabel != "Unknown");
    CHECK(claudeProviderRow->sourceLabel != "Unknown");

    const auto openaiAccountRow = findBreakdownRow(analysis.accountBreakdown, "OpenAI Runtime");
    const auto claudeAccountRow = std::find_if(
        analysis.accountBreakdown.begin(),
        analysis.accountBreakdown.end(),
        [](const AnalysisBreakdownRow& row) {
            return row.secondaryLabel == "Claude Code" ||
                row.label.find("Claude") != std::string::npos ||
                row.key.find("claude_code.runtime_status") != std::string::npos;
        });
    REQUIRE(openaiAccountRow.has_value());
    REQUIRE(claudeAccountRow != analysis.accountBreakdown.end());
    CHECK(openaiAccountRow->secondaryLabel == "OpenAI");
    CHECK(claudeAccountRow->activityCount >= 1);

    const auto openaiTrend = findTrendPoint(analysis.trendPoints, "2026-04-18");
    const auto localTrend = findTrendPoint(analysis.trendPoints, "2026-04-19");
    REQUIRE(openaiTrend.has_value());
    REQUIRE(localTrend.has_value());
    CHECK(openaiTrend->activityCount == openaiDaySummary->activityCount);
    CHECK(localTrend->activityCount == localDaySummary->activityCount);

    CHECK(analysis.gapSummaryCounts.reconstructedDays >= 1);
    CHECK((analysis.dataQualityNote.find("local") != std::string::npos ||
           analysis.dataQualityNote.find("reconstructed") != std::string::npos ||
           analysis.dataQualityNote.find("provenance") != std::string::npos));
}

TEST_CASE("AnalysisQueryEngine builds hourly trend buckets from persisted rollups and raw edge usage for short windows", "[analysis][query_engine][analysis_query_engine]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    const std::time_t now = makeUtcTime(2026, 4, 20, 12, 34, 56);
    const std::time_t rolledHourOne = makeUtcTime(2026, 4, 20, 9, 5, 0);
    const std::time_t rolledHourTwo = makeUtcTime(2026, 4, 20, 10, 15, 0);
    const std::time_t rawEdgeHour = makeUtcTime(2026, 4, 20, 12, 10, 0);

    seedUsageEvent(storage, "openai", "OpenAI", rolledHourOne, 240, 18, "gpt-4.1");
    seedUsageEvent(storage, "openai", "OpenAI", rolledHourTwo, 180, 14, "gpt-4.1");

    const int providerId = requireProviderId(storage, "openai");
    const int accountId = ensureRuntimeAccountId(storage, "openai", "OpenAI");
    REQUIRE(storage.rebuildUsageRollups(
        UsageRollupResolution::Hourly,
        providerId,
        accountId,
        makeUtcTime(2026, 4, 20, 9, 0, 0),
        makeUtcTime(2026, 4, 20, 10, 59, 59)));

    seedUsageEvent(storage, "openai", "OpenAI", rawEdgeHour, 300, 21, "gpt-4.1");
    seedLocalActivity(storage, "openai", "OpenAI", "codex_cli", makeUtcTime(2026, 4, 20, 11, 5, 0), 60);

    AnalysisQueryEngine engine(storage);
    DashboardViewModel dashboard;
    DashboardPreferences preferences;
    preferences.analysisSelectedProviderId = "openai";
    preferences.analysisSelectedAccountId = "openai.runtime_status";
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Today;

    const AnalysisViewModel analysis = engine.buildViewModel(dashboard, preferences, now);

    CHECK(analysis.trendBucketGranularity == AnalysisTrendBucketGranularity::Hourly);
    CHECK(analysis.trendBucketLabel == "Hourly");
    REQUIRE(analysis.trendPoints.size() == 13);

    const auto rolledPointOne = findTrendPoint(analysis.trendPoints, "04-20 09:00");
    const auto rolledPointTwo = findTrendPoint(analysis.trendPoints, "04-20 10:00");
    const auto localPoint = findTrendPoint(analysis.trendPoints, "04-20 11:00");
    const auto rawPoint = findTrendPoint(analysis.trendPoints, "04-20 12:00");
    REQUIRE(rolledPointOne.has_value());
    REQUIRE(rolledPointTwo.has_value());
    REQUIRE(localPoint.has_value());
    REQUIRE(rawPoint.has_value());
    CHECK(rolledPointOne->activityCount == 1);
    CHECK(rolledPointTwo->activityCount == 1);
    CHECK(localPoint->activityCount == 1);
    CHECK(rawPoint->activityCount == 1);
    CHECK(rolledPointOne->bucketStartUtc == makeUtcTime(2026, 4, 20, 9, 0, 0));
    CHECK(rolledPointOne->bucketEndUtc == makeUtcTime(2026, 4, 20, 9, 59, 59));
    CHECK(rawPoint->bucketStartUtc == makeUtcTime(2026, 4, 20, 12, 0, 0));
    CHECK(rawPoint->bucketEndUtc == now);
    CHECK(analysis.dataQualityNote.find("Trend buckets: Hourly across 13 points.") != std::string::npos);

    REQUIRE(analysis.dailySummaries.size() == 1);
    CHECK(analysis.dailySummaries.front().dateLabel == "2026-04-20");
    CHECK(analysis.dailySummaries.front().activityCount == 3);
}

TEST_CASE("AnalysisQueryEngine surfaces persisted quota recovery and export status in scoped data quality notes", "[analysis][query_engine][analysis_query_engine]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    const std::time_t now = makeUtcTime(2026, 4, 20, 18, 0, 0);
    const int providerId = requireProviderId(storage, "minimax");
    const int accountId = ensureRuntimeAccountId(storage, "minimax", "MiniMax");
    seedUsageEvent(storage, "minimax", "MiniMax", makeUtcTime(2026, 4, 20, 10, 0, 0), 500, 40, "minimax-m1");
    REQUIRE(storage.rebuildUsageRollups(
        UsageRollupResolution::Daily,
        providerId,
        accountId,
        makeUtcTime(2026, 4, 14, 0, 0, 0),
        now));

    const QueryResult planInsert = storage.execute(
        "INSERT INTO plans (account_id, plan_key, plan_type, display_name, quota_window, limit_value) "
        "VALUES (?, ?, ?, ?, ?, ?);",
        {
            std::to_string(accountId),
            "minimax_subscription_coding_plan",
            "subscription",
            "MiniMax Coding Plan",
            "daily",
            "1000"
        });
    REQUIRE(planInsert.success());
    const long long planId = planInsert.lastInsertRowId();

    REQUIRE(storage.execute(
        "INSERT INTO quota_snapshots ("
        "provider_id, account_id, plan_id, snapshot_time_utc, window_start_utc, window_end_utc, "
        "used_value, limit_value, confidence_tier, basis_summary, quota_dimension, remaining_value, is_reconciled"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
        {
            std::to_string(providerId),
            std::to_string(accountId),
            std::to_string(planId),
            "2026-04-20T17:30:00Z",
            "2026-04-20T00:00:00Z",
            "2026-04-20T23:59:59Z",
            "300",
            "1000",
            "high",
            "Persisted analysis test snapshot",
            "requests",
            "700",
            "1"
        }).success());

    PersistedRecoveryCheckpoint checkpoint;
    checkpoint.providerId = providerId;
    checkpoint.accountId = accountId;
    checkpoint.syncKind = "runtime_backfill";
    checkpoint.recoveryStart = makeUtcTime(2026, 4, 20, 15, 0, 0);
    checkpoint.recoveryEnd = makeUtcTime(2026, 4, 20, 16, 0, 0);
    checkpoint.attemptAt = makeUtcTime(2026, 4, 20, 16, 30, 0);
    checkpoint.outcome = "imported";
    checkpoint.fetchedEventCount = 2;
    checkpoint.comparisonResult = "mismatch";
    checkpoint.localEventCount = 1;
    checkpoint.localTotalTokens = 120;
    checkpoint.importedTotalTokens = 150;
    REQUIRE(storage.insertRecoveryCheckpoint(checkpoint));

    PersistedRecoveryCheckpoint failedCheckpoint = checkpoint;
    failedCheckpoint.attemptAt = makeUtcTime(2026, 4, 20, 16, 45, 0);
    failedCheckpoint.outcome = "persist_failed";
    failedCheckpoint.fetchedEventCount = 0;
    failedCheckpoint.comparisonResult = "not_available";
    failedCheckpoint.localEventCount = 0;
    failedCheckpoint.localTotalTokens = 0;
    failedCheckpoint.importedTotalTokens = 0;
    failedCheckpoint.failureCategory = "provider_backfill_persist_failed";
    REQUIRE(storage.insertRecoveryCheckpoint(failedCheckpoint));

    PersistedExportRun manualExport;
    manualExport.runKind = "manual_html";
    manualExport.format = "html";
    manualExport.requestedAt = makeUtcTime(2026, 4, 20, 17, 0, 0);
    manualExport.completedAt = makeUtcTime(2026, 4, 20, 17, 1, 0);
    manualExport.success = true;
    manualExport.outputPath = "C:/temp/manual-report.html";
    manualExport.rangeStart = makeUtcTime(2026, 4, 14, 0, 0, 0);
    manualExport.rangeEnd = now;
    manualExport.providerFilterCsv = "minimax";
    manualExport.accountFilterCsv = "minimax.runtime_status";
    REQUIRE(storage.insertExportRun(manualExport));

    PersistedExportRun scheduledExport;
    scheduledExport.runKind = "scheduled_html";
    scheduledExport.format = "html";
    scheduledExport.requestedAt = makeUtcTime(2026, 4, 20, 17, 30, 0);
    scheduledExport.completedAt = makeUtcTime(2026, 4, 20, 17, 31, 0);
    scheduledExport.success = false;
    scheduledExport.errorSummary = "disk full";
    scheduledExport.rangeStart = makeUtcTime(2026, 4, 14, 0, 0, 0);
    scheduledExport.rangeEnd = now;
    scheduledExport.providerFilterCsv = "minimax";
    scheduledExport.accountFilterCsv = "minimax.runtime_status";
    REQUIRE(storage.insertExportRun(scheduledExport));

    AnalysisQueryEngine engine(storage);

    DashboardViewModel dashboard;
    DashboardPreferences preferences;
    preferences.analysisSelectedProviderId = "minimax";
    preferences.analysisSelectedAccountId = "minimax.runtime_status";
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Last7Days;

    const AnalysisViewModel analysis = engine.buildViewModel(dashboard, preferences, now);

    REQUIRE_FALSE(analysis.dailySummaries.empty());
    REQUIRE(analysis.quotaTrendSeries.size() == 1);
    const auto& quotaSeries = analysis.quotaTrendSeries.front();
    CHECK(quotaSeries.label == "MiniMax Coding Plan 1d");
    CHECK(quotaSeries.windowLabel == "1d");
    CHECK(quotaSeries.points.size() == 1);
    CHECK(quotaSeries.latestRemainingValue == 700);
    CHECK(quotaSeries.latestRemainingPercentage == Catch::Approx(70.0));
    CHECK(quotaSeries.lowWaterRemainingPercentage == Catch::Approx(70.0));
    CHECK(quotaSeries.resetCount == 0);
    CHECK(std::any_of(
        analysis.availableProviders.begin(),
        analysis.availableProviders.end(),
        [](const AnalysisScopeOption& option) { return option.key == "minimax"; }));
    CHECK(std::any_of(
        analysis.availableAccounts.begin(),
        analysis.availableAccounts.end(),
        [](const AnalysisScopeOption& option) { return option.key == "minimax.runtime_status"; }));
    CHECK(analysis.scope.selectedProviderId == "minimax");
    CHECK(analysis.scope.selectedAccountId == "minimax.runtime_status");
    CHECK(analysis.dataQualityNote.find("Recovery checkpoints: 1 imported, 1 mismatch, 1 failed.") != std::string::npos);
    CHECK(analysis.dataQualityNote.find("Persisted quota history points in scope: 1 across 1 series") != std::string::npos);
    CHECK(analysis.dataQualityNote.find("Latest manual export:") != std::string::npos);
    CHECK(analysis.dataQualityNote.find("Latest scheduled export:") != std::string::npos);
    CHECK(analysis.dataQualityNote.find("(success)") != std::string::npos);
    CHECK(analysis.dataQualityNote.find("(failed)") != std::string::npos);
}

TEST_CASE("AnalysisQueryEngine builds reset-aware quota trend series from scoped quota snapshots", "[analysis][query_engine][analysis_query_engine]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    const std::time_t now = makeUtcTime(2026, 4, 20, 20, 0, 0);
    const int providerId = requireProviderId(storage, "openai");
    const int accountId = ensureRuntimeAccountId(storage, "openai", "OpenAI");

    const QueryResult planInsert = storage.execute(
        "INSERT INTO plans (account_id, plan_key, plan_type, display_name, quota_window, limit_value) "
        "VALUES (?, ?, ?, ?, ?, ?);",
        {
            std::to_string(accountId),
            "openai_subscription_5h",
            "subscription",
            "OpenAI 5h",
            "rolling",
            "1000"
        });
    REQUIRE(planInsert.success());
    const long long planId = planInsert.lastInsertRowId();

    auto insertSnapshot = [&](const char* snapshotTime,
                              const char* windowStart,
                              const char* windowEnd,
                              const char* usedValue,
                              const char* remainingValue) {
        REQUIRE(storage.execute(
            "INSERT INTO quota_snapshots ("
            "provider_id, account_id, plan_id, snapshot_time_utc, window_start_utc, window_end_utc, "
            "used_value, limit_value, confidence_tier, basis_summary, quota_dimension, remaining_value, is_reconciled"
            ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
            {
                std::to_string(providerId),
                std::to_string(accountId),
                std::to_string(planId),
                snapshotTime,
                windowStart,
                windowEnd,
                usedValue,
                "1000",
                "high",
                "Reset-aware quota trend test snapshot",
                "requests",
                remainingValue,
                "1"
            }).success());
    };

    insertSnapshot("2026-04-20T09:00:00Z", "2026-04-20T05:00:00Z", "2026-04-20T10:00:00Z", "120", "880");
    insertSnapshot("2026-04-20T11:00:00Z", "2026-04-20T05:00:00Z", "2026-04-20T10:00:00Z", "590", "410");
    insertSnapshot("2026-04-20T15:30:00Z", "2026-04-20T10:00:01Z", "2026-04-20T15:00:00Z", "50", "950");

    AnalysisQueryEngine engine(storage);
    DashboardViewModel dashboard;
    DashboardPreferences preferences;
    preferences.analysisSelectedProviderId = "openai";
    preferences.analysisSelectedAccountId = "openai.runtime_status";
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Today;

    const AnalysisViewModel analysis = engine.buildViewModel(dashboard, preferences, now);

    REQUIRE(analysis.quotaTrendSeries.size() == 1);
    const auto& quotaSeries = analysis.quotaTrendSeries.front();
    CHECK(quotaSeries.label == "OpenAI 5h");
    CHECK(quotaSeries.points.size() == 3);
    CHECK(quotaSeries.latestRemainingValue == 950);
    CHECK(quotaSeries.latestRemainingPercentage == Catch::Approx(95.0));
    CHECK(quotaSeries.lowWaterRemainingPercentage == Catch::Approx(41.0));
    CHECK(quotaSeries.resetCount == 1);
    CHECK(quotaSeries.points[0].resetPoint == false);
    CHECK(quotaSeries.points[1].resetPoint == false);
    CHECK(quotaSeries.points[2].resetPoint == true);
}

TEST_CASE("Quota rollups preserve the lowest percentage across changing limits", "[analysis][quota][low_water]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    const int providerId = requireProviderId(storage, "openai");
    const int accountId = ensureRuntimeAccountId(storage, "openai", "OpenAI");
    const auto plan =
        storage.execute("INSERT INTO plans (account_id, plan_key, plan_type, display_name, quota_window, limit_value) "
                        "VALUES (?, 'variable-limit', 'subscription', 'Variable Limit', 'daily', 100);",
                        {std::to_string(accountId)});
    REQUIRE(plan.success());

    auto insert = [&](const char* capturedAt, const char* used, const char* limit, const char* remaining) {
        REQUIRE(storage
                    .execute("INSERT INTO quota_snapshots (provider_id, account_id, plan_id, snapshot_time_utc, "
                             "window_start_utc, window_end_utc, used_value, limit_value, confidence_tier, "
                             "quota_dimension, remaining_value) "
                             "VALUES (?, ?, ?, ?, '2026-04-01T00:00:00Z', '2026-04-02T00:00:00Z', ?, "
                             "NULLIF(?, ''), 'high', 'requests', NULLIF(?, ''));",
                             {std::to_string(providerId), std::to_string(accountId),
                              std::to_string(plan.lastInsertRowId()), capturedAt, used, limit, remaining})
                    .success());
    };

    const bool includeUnquantifiedReading = GENERATE(false, true);
    CAPTURE(includeUnquantifiedReading);
    insert("2026-04-01T08:00:00Z", "80", "100", "20");
    insert("2026-04-01T09:00:00Z", "900", "1000", "");
    if (includeUnquantifiedReading) {
        insert("2026-04-01T10:00:00Z", "0", "", "1");
    }
    insert("2026-04-02T11:00:00Z", "50", "100", "50");

    const auto day = makeUtcTime(2026, 4, 1);
    const auto nextDay = makeUtcTime(2026, 4, 2);
    const auto now = makeUtcTime(2026, 4, 20, 12);
    DashboardPreferences preferences;
    preferences.analysisSelectedProviderId = "openai";
    preferences.analysisSelectedAccountId = "openai.runtime_status";
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Custom;
    preferences.analysisCustomRangeStartUtc = day;
    preferences.analysisCustomRangeEndUtc = now;
    AnalysisQueryEngine engine(storage);

    const auto raw = engine.buildViewModel({}, preferences, now);
    REQUIRE(raw.quotaTrendSeries.size() == 1);
    CHECK(raw.quotaTrendSeries.front().lowWaterRemainingPercentage == Catch::Approx(10));
    REQUIRE(storage.rebuildQuotaSnapshotRollups(providerId, accountId, day, nextDay));

    const auto saved = storage.getQuotaSnapshotRollups(providerId, accountId, day, nextDay);
    REQUIRE(saved.size() == 2);
    CHECK(saved.front().lowWaterRemainingValue == 100);
    CHECK(saved.front().lowWaterLimitValue == 1000);

    const auto rebuilt = engine.buildViewModel({}, preferences, now);
    REQUIRE(rebuilt.quotaTrendSeries.size() == 1);
    const auto& series = rebuilt.quotaTrendSeries.front();
    REQUIRE(series.points.size() == (includeUnquantifiedReading ? 1 : 2));
    CHECK(series.points.back().dailyRollup);
    CHECK(series.latestRemainingPercentage == Catch::Approx(50));
    CHECK(series.lowWaterRemainingPercentage == Catch::Approx(10));

    // Emulate a daily minimum saved by the old count-based aggregation rule.
    REQUIRE(storage
                .execute("UPDATE quota_snapshot_rollups_daily SET low_water_remaining_value = 20, "
                         "low_water_limit_value = 100 WHERE rollup_period_start_utc = '2026-04-01T00:00:00Z';")
                .success());
    const auto legacyWithRaw = engine.buildViewModel({}, preferences, now);
    REQUIRE(legacyWithRaw.quotaTrendSeries.size() == 1);
    CHECK(legacyWithRaw.quotaTrendSeries.front().lowWaterRemainingPercentage == Catch::Approx(10));

    REQUIRE(storage.execute("DELETE FROM quota_snapshots WHERE snapshot_time_utc < '2026-04-02T00:00:00Z';").success());
    const auto legacyWithoutRaw = engine.buildViewModel({}, preferences, now);
    REQUIRE(legacyWithoutRaw.quotaTrendSeries.size() == 1);
    const double retainedMinimum = includeUnquantifiedReading ? 20.0 : 10.0;
    CHECK(legacyWithoutRaw.quotaTrendSeries.front().lowWaterRemainingPercentage == Catch::Approx(retainedMinimum));
}

TEST_CASE("AnalysisQueryEngine prefers daily quota rollups for long-range quota history", "[analysis][query_engine][analysis_query_engine]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    const std::time_t now = makeUtcTime(2026, 4, 20, 20, 0, 0);
    const int providerId = requireProviderId(storage, "openai");
    const int accountId = ensureRuntimeAccountId(storage, "openai", "OpenAI");

    const QueryResult planInsert = storage.execute(
        "INSERT INTO plans (account_id, plan_key, plan_type, display_name, quota_window, limit_value) "
        "VALUES (?, ?, ?, ?, ?, ?);",
        {
            std::to_string(accountId),
            "openai_subscription_5h",
            "subscription",
            "OpenAI 5h",
            "rolling",
            "1000"
        });
    REQUIRE(planInsert.success());
    const long long planId = planInsert.lastInsertRowId();

    auto insertSnapshot = [&](const char* snapshotTime, const char* windowStart, const char* windowEnd,
                              const char* usedValue, const char* remainingValue, const char* reconciled = "1") {
        REQUIRE(storage
                    .execute("INSERT INTO quota_snapshots ("
                             "provider_id, account_id, plan_id, snapshot_time_utc, window_start_utc, window_end_utc, "
                             "used_value, limit_value, confidence_tier, basis_summary, quota_dimension, "
                             "remaining_value, is_reconciled"
                             ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
                             {std::to_string(providerId), std::to_string(accountId), std::to_string(planId),
                              snapshotTime, windowStart, windowEnd, usedValue, "1000", "high",
                              "Long-range rollup quota test snapshot", "requests", remainingValue, reconciled})
                    .success());
    };

    insertSnapshot("2026-04-01T08:00:00Z", "2026-04-01T05:00:00Z", "2026-04-01T10:00:00Z", "100", "900");
    insertSnapshot("2026-04-01T09:00:00Z", "2026-04-01T05:00:00Z", "2026-04-01T10:00:00Z", "550", "450");
    insertSnapshot("2026-04-01T11:00:00Z", "2026-04-01T10:00:00Z", "2026-04-01T15:00:00Z", "250", "750");
    insertSnapshot("2026-04-10T09:00:00Z", "2026-04-10T05:00:00Z", "2026-04-10T10:00:00Z", "350", "650");
    insertSnapshot("2026-04-10T14:00:00Z", "2026-04-10T10:00:00Z", "2026-04-10T15:00:00Z", "600", "400");

    insertSnapshot("2026-04-10T14:45:00Z", "2026-04-10T10:00:00Z", "2026-04-10T15:00:00Z", "600", "400", "0");

    REQUIRE(storage.rebuildQuotaSnapshotRollups(
        providerId,
        accountId,
        makeUtcTime(2026, 4, 1, 0, 0, 0),
        now));

    AnalysisQueryEngine engine(storage);
    DashboardViewModel dashboard;
    DashboardPreferences preferences;
    preferences.analysisSelectedProviderId = "openai";
    preferences.analysisSelectedAccountId = "openai.runtime_status";
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Custom;
    preferences.analysisCustomRangeStartUtc = makeUtcTime(2026, 4, 1, 0, 0, 0);
    preferences.analysisCustomRangeEndUtc = now;

    const AnalysisViewModel analysis = engine.buildViewModel(dashboard, preferences, now);

    REQUIRE(analysis.quotaTrendSeries.size() == 1);
    const auto& quotaSeries = analysis.quotaTrendSeries.front();
    CHECK(quotaSeries.label == "OpenAI 5h");
    REQUIRE(quotaSeries.points.size() == 2);
    CHECK(quotaSeries.points[0].remainingValue == 750);
    CHECK(quotaSeries.points[1].remainingValue == 400);
    CHECK(quotaSeries.latestRemainingValue == 400);
    CHECK(quotaSeries.latestRemainingPercentage == Catch::Approx(40.0));
    CHECK(quotaSeries.lowWaterRemainingPercentage == Catch::Approx(40.0));
    CHECK(quotaSeries.resetCount == 3);
    CHECK(quotaSeries.points[0].resetPoint == true);
    CHECK(quotaSeries.points[1].resetPoint == true);
    CHECK(quotaSeries.points[1].dailyRollup);
    CHECK(quotaSeries.points[1].reconciled);
    CHECK(quotaSeries.points[1].snapshotTime == makeUtcTime(2026, 4, 10, 14, 45, 0));
    CHECK(analysis.dataQualityNote.find("Persisted quota history points in scope: 2 across 1 series") != std::string::npos);
    CHECK(analysis.dataQualityNote.find("daily rollup-backed") != std::string::npos);

    REQUIRE(storage
                .execute("UPDATE quota_snapshot_rollups_daily SET observed_billing_windows = NULL "
                         "WHERE rollup_period_start_utc = '2026-04-10T00:00:00Z';")
                .success());
    REQUIRE(storage.execute("DELETE FROM quota_snapshots WHERE snapshot_time_utc LIKE '2026-04-10%';").success());
    const auto legacy = engine.buildViewModel(dashboard, preferences, now);
    REQUIRE(legacy.quotaTrendSeries.size() == 1);
    CHECK(legacy.quotaTrendSeries.front().resetCount == 2);
}

TEST_CASE("AnalysisQueryEngine preserves observed quota resets across rebuilt UTC days",
          "[analysis][query_engine][quota_rollup_boundary]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));
    const int providerId = requireProviderId(storage, "grok");
    const int accountId = ensureRuntimeAccountId(storage, "grok", "Grok");

    const auto insert = [&](const char* time, const char* start, const char* end, const char* limit = "100") {
        REQUIRE(
            storage
                .execute("INSERT INTO quota_snapshots (provider_id, account_id, snapshot_time_utc, "
                         "window_start_utc, window_end_utc, used_value, limit_value, confidence_tier, quota_dimension) "
                         "VALUES (?, ?, ?, ?, ?, 40, ?, 'high', 'percent');",
                         {std::to_string(providerId), std::to_string(accountId), time, start, end, limit})
                .success());
    };

    const char* firstStart = "2026-04-01T12:00:00Z";
    const char* firstEnd = "2026-04-02T12:00:00Z";
    const char* secondStart = firstEnd;
    const char* secondEnd = "2026-04-03T12:00:00Z";
    insert("2026-04-01T23:50:00Z", firstStart, firstEnd);
    int expected = 1;
    int secondDayResets = 0;
    SECTION("Matching windows across midnight contribute zero") {
        insert("2026-04-02T00:05:00Z", secondStart, secondEnd, "0");
        insert("2026-04-02T00:10:00Z", firstStart, firstEnd);
        expected = 0;
    }
    SECTION("Changed windows across midnight contribute one") {
        insert("2026-04-02T13:00:00Z", secondStart, secondEnd);
        insert("2026-04-02T13:01:00Z", secondStart, secondEnd);
    }
    SECTION("Partial bounds can establish a change against the previous day") {
        insert("2026-04-02T00:10:00Z", firstStart, "");
        insert("2026-04-02T13:00:00Z", "", secondEnd);
    }
    SECTION("A partial reset can discard an older bound before a later sample") {
        insert("2026-04-02T00:10:00Z", firstStart, "");
        insert("2026-04-02T13:00:00Z", "", secondEnd);
        insert("2026-04-02T13:10:00Z", secondStart, "");
        secondDayResets = 1;
    }

    const auto firstDay = makeUtcTime(2026, 4, 1);
    const auto secondDay = makeUtcTime(2026, 4, 2);
    const auto now = makeUtcTime(2026, 4, 20);
    DashboardPreferences preferences;
    preferences.analysisSelectedProviderId = "grok";
    preferences.analysisSelectedAccountId = "grok.runtime_status";
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Custom;
    preferences.analysisCustomRangeStartUtc = firstDay;
    preferences.analysisCustomRangeEndUtc = now;
    DashboardViewModel dashboard;
    dashboard.providers = {makeProvider("grok", "Grok", "grok.runtime_status")};
    AnalysisQueryEngine engine(storage);
    const auto raw = engine.buildViewModel(dashboard, preferences, now);
    REQUIRE(raw.quotaTrendSeries.size() == 1);
    CHECK(raw.quotaTrendSeries.front().resetCount == expected);

    REQUIRE(storage.rebuildQuotaSnapshotRollups(providerId, accountId, firstDay, firstDay));
    REQUIRE(storage.rebuildQuotaSnapshotRollups(providerId, accountId, secondDay, secondDay));
    REQUIRE(storage.execute("DELETE FROM quota_snapshots;").success());
    storage.close();
    REQUIRE(storage.init(tempDb.path()));

    const auto compacted = engine.buildViewModel(dashboard, preferences, now);
    REQUIRE(compacted.quotaTrendSeries.size() == 1);
    const auto& series = compacted.quotaTrendSeries.front();
    REQUIRE(series.points.size() == 2);
    CHECK(series.points[0].dailyRollup);
    CHECK(series.points[1].dailyRollup);
    CHECK_FALSE(series.points[0].resetPoint);
    CHECK(series.points[1].resetPoint == (expected != 0));
    CHECK(series.resetCount == expected);

    preferences.analysisCustomRangeStartUtc = secondDay;
    const auto startAfterMidnight = engine.buildViewModel(dashboard, preferences, now);
    REQUIRE(startAfterMidnight.quotaTrendSeries.size() == 1);
    REQUIRE(startAfterMidnight.quotaTrendSeries.front().points.size() == 1);
    CHECK(startAfterMidnight.quotaTrendSeries.front().resetCount == secondDayResets);
}

TEST_CASE("Analysis counts rollup resets only inside the selected partial UTC days",
          "[analysis][query_engine][quota_rollup_boundary]") {
    requireSqlite();
    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));
    const int providerId = requireProviderId(storage, "grok");
    const int accountId = ensureRuntimeAccountId(storage, "grok", "Grok");
    const auto insert = [&](const char* time, const char* start, const char* end) {
        REQUIRE(
            storage
                .execute("INSERT INTO quota_snapshots (provider_id, account_id, snapshot_time_utc, window_start_utc, "
                         "window_end_utc, used_value, limit_value, confidence_tier, quota_dimension) "
                         "VALUES (?, ?, ?, ?, ?, 40, 100, 'high', 'percent');",
                         {std::to_string(providerId), std::to_string(accountId), time, start, end})
                .success());
    };
    DashboardViewModel dashboard;
    dashboard.providers = {makeProvider("grok", "Grok", "grok.runtime_status")};
    DashboardPreferences preferences;
    preferences.analysisSelectedProviderId = "grok";
    preferences.analysisSelectedAccountId = "grok.runtime_status";
    const auto firstDay = makeUtcTime(2026, 4, 1);
    const auto now = makeUtcTime(2026, 4, 20, 12);
    const char* currentStart = "2026-04-01T12:00:00Z";
    const char* currentEnd = "2026-04-30T12:00:00Z";
    const char* partialDay = "2026-04-01";
    SECTION("Current quota window starts after a recorded morning reset boundary") {
        preferences.analysisDateRangePreset = AnalysisDateRangePreset::CurrentQuotaWindow;
        insert("2026-04-01T08:00:00Z", "2026-03-02T12:00:00Z", currentStart);
        insert("2026-04-01T13:00:00Z", currentStart, currentEnd);
        insert("2026-04-02T13:00:00Z", currentStart, currentEnd);
    }
    SECTION("The current day has a later observation outside the selected end") {
        preferences.analysisDateRangePreset = AnalysisDateRangePreset::ThisMonth;
        partialDay = "2026-04-20";
        insert("2026-04-01T13:00:00Z", currentStart, currentEnd);
        insert("2026-04-20T08:00:00Z", currentStart, currentEnd);
        insert("2026-04-20T13:00:00Z", "2026-04-20T12:30:00Z", "2026-05-19T12:00:00Z");
    }

    AnalysisQueryEngine engine(storage);
    const auto raw = engine.buildViewModel(dashboard, preferences, now);
    REQUIRE(raw.scope.dateRangePreset == preferences.analysisDateRangePreset);
    REQUIRE(raw.quotaTrendSeries.size() == 1);
    CHECK(raw.quotaTrendSeries.front().resetCount == 0);

    REQUIRE(storage.rebuildQuotaSnapshotRollups(providerId, accountId, firstDay, now));
    const bool hasPartialFinalDay = preferences.analysisDateRangePreset == AnalysisDateRangePreset::ThisMonth;
    const auto checkRollups = [&](bool rawRetained) {
        const auto analysis = engine.buildViewModel(dashboard, preferences, now);
        REQUIRE(analysis.quotaTrendSeries.size() == 1);
        const auto& series = analysis.quotaTrendSeries.front();
        REQUIRE(series.points.size() == (hasPartialFinalDay && !rawRetained ? 1 : 2));
        CHECK(series.points[0].dailyRollup);
        CHECK_FALSE(series.points[0].resetPoint);
        if (series.points.size() == 2) {
            CHECK(series.points[1].dailyRollup == !hasPartialFinalDay);
            CHECK_FALSE(series.points[1].resetPoint);
            if (hasPartialFinalDay) {
                CHECK(series.points[1].snapshotTime == makeUtcTime(2026, 4, 20, 8));
            }
        }
        CHECK(series.resetCount == 0);
    };
    checkRollups(true);
    REQUIRE(storage.execute("DELETE FROM quota_snapshots WHERE substr(snapshot_time_utc, 1, 10) = ?;", {partialDay})
                .success());
    checkRollups(false);
}

TEST_CASE("Analysis uses retained raw reset evidence for legacy and unrolled quota days",
          "[analysis][query_engine][quota_rollup_boundary]") {
    requireSqlite();
    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));
    const int providerId = requireProviderId(storage, "grok");
    const int accountId = ensureRuntimeAccountId(storage, "grok", "Grok");
    const auto insert = [&](const char* time, const char* start, const char* end, const char* dimension = "percent",
                            const char* limit = "100") {
        REQUIRE(
            storage
                .execute("INSERT INTO quota_snapshots (provider_id, account_id, snapshot_time_utc, window_start_utc, "
                         "window_end_utc, used_value, limit_value, confidence_tier, quota_dimension) "
                         "VALUES (?, ?, ?, ?, ?, 40, ?, 'high', ?);",
                         {std::to_string(providerId), std::to_string(accountId), time, start, end, limit, dimension})
                .success());
    };
    insert("2026-04-01T14:00:00Z", "2026-04-01T12:00:00Z", "2026-04-02T12:00:00Z");
    insert("2026-04-02T14:00:00Z", "2026-04-02T12:00:00Z", "2026-04-03T12:00:00Z");
    insert("2026-04-03T14:00:00Z", "2026-04-03T12:00:00Z", "2026-04-04T12:00:00Z");
    const auto firstDay = makeUtcTime(2026, 4, 1);
    const auto secondDay = makeUtcTime(2026, 4, 2);
    const auto thirdDay = makeUtcTime(2026, 4, 3);
    const auto now = makeUtcTime(2026, 4, 20);
    DashboardViewModel dashboard;
    dashboard.providers = {makeProvider("grok", "Grok", "grok.runtime_status")};
    DashboardPreferences preferences;
    preferences.analysisSelectedProviderId = "grok";
    preferences.analysisSelectedAccountId = "grok.runtime_status";
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Custom;
    preferences.analysisCustomRangeStartUtc = firstDay;
    preferences.analysisCustomRangeEndUtc = now;
    AnalysisQueryEngine engine(storage);
    const auto raw = engine.buildViewModel(dashboard, preferences, now);
    REQUIRE(raw.quotaTrendSeries.size() == 1);
    CHECK(raw.quotaTrendSeries.front().resetCount == 2);

    REQUIRE(storage.rebuildQuotaSnapshotRollups(providerId, accountId, firstDay, firstDay));
    std::size_t expectedSeries = 1;
    std::size_t expectedDailyPoints = 2;
    bool expectRawDay = false;
    const auto reopenWithLegacyRollups = [&]() {
        REQUIRE(storage
                    .execute("ALTER TABLE quota_snapshot_rollups_daily DROP COLUMN observed_billing_windows;"
                             "DELETE FROM schema_version WHERE version = 18;")
                    .success());
        storage.close();
        REQUIRE(storage.init(tempDb.path()));
    };
    SECTION("The v17 upgrade retains raw readings for the previously rolled-up day") {
        reopenWithLegacyRollups();
        REQUIRE(storage.rebuildQuotaSnapshotRollups(providerId, accountId, secondDay, secondDay));
        expectedDailyPoints = 3;
    }
    SECTION("A legacy day keeps its saved count when some raw readings have expired") {
        insert("2026-04-01T15:00:00Z", "2026-04-02T12:00:00Z", "2026-04-03T12:00:00Z");
        REQUIRE(storage.rebuildQuotaSnapshotRollups(providerId, accountId, firstDay, firstDay));
        reopenWithLegacyRollups();
        REQUIRE(
            storage.execute("DELETE FROM quota_snapshots WHERE snapshot_time_utc = '2026-04-01T14:00:00Z';").success());
        REQUIRE(storage.rebuildQuotaSnapshotRollups(providerId, accountId, secondDay, secondDay));
        expectedDailyPoints = 3;
    }
    SECTION("The middle day is retained only as raw history") {
        expectRawDay = true;
    }
    SECTION("A non-displayable rollup retains reset evidence after its raw observations are removed") {
        insert("2026-04-02T15:00:00Z", "2026-04-02T12:00:00Z", "2026-04-03T12:00:00Z", "percent", "0");
        REQUIRE(storage.rebuildQuotaSnapshotRollups(providerId, accountId, secondDay, secondDay));
        REQUIRE(storage.execute("DELETE FROM quota_snapshots WHERE snapshot_time_utc LIKE '2026-04-02%';").success());
    }
    SECTION("A rollup for another dimension does not hide the middle day's raw reset evidence") {
        insert("2026-04-02T15:00:00Z", "2026-04-02T10:00:00Z", "2026-04-02T16:00:00Z", "requests");
        REQUIRE(storage.rebuildQuotaSnapshotRollups(providerId, accountId, secondDay, secondDay));
        REQUIRE(storage
                    .execute("DELETE FROM quota_snapshot_rollups_daily WHERE quota_dimension = 'percent' "
                             "AND rollup_period_start_utc = '2026-04-02T00:00:00Z';")
                    .success());
        expectedSeries = 2;
        expectRawDay = true;
    }
    REQUIRE(storage.rebuildQuotaSnapshotRollups(providerId, accountId, thirdDay, thirdDay));

    const auto mixed = engine.buildViewModel(dashboard, preferences, now);
    REQUIRE(mixed.quotaTrendSeries.size() == expectedSeries);
    const auto series = std::find_if(mixed.quotaTrendSeries.begin(), mixed.quotaTrendSeries.end(),
                                     [](const auto& entry) { return entry.quotaDimension == "percent"; });
    REQUIRE(series != mixed.quotaTrendSeries.end());
    REQUIRE(series->points.size() == expectedDailyPoints + (expectRawDay ? 1 : 0));
    CHECK(std::count_if(series->points.begin(), series->points.end(),
                        [](const auto& point) { return point.dailyRollup; }) == expectedDailyPoints);
    if (expectRawDay) {
        CHECK_FALSE(series->points[1].dailyRollup);
        CHECK(series->points[1].resetPoint);
    }
    CHECK(series->points.back().dailyRollup);
    CHECK(series->points.back().resetPoint);
    CHECK(series->resetCount == 2);
}

TEST_CASE("AnalysisQueryEngine resolves current quota window from the latest scoped quota snapshot", "[analysis][query_engine][analysis_query_engine]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    const std::time_t now = makeUtcTime(2026, 4, 20, 20, 0, 0);
    const int providerId = requireProviderId(storage, "openai");
    const int accountId = ensureRuntimeAccountId(storage, "openai", "OpenAI");

    const QueryResult planInsert = storage.execute(
        "INSERT INTO plans (account_id, plan_key, plan_type, display_name, quota_window, limit_value) "
        "VALUES (?, ?, ?, ?, ?, ?);",
        {
            std::to_string(accountId),
            "openai_subscription_5h",
            "subscription",
            "OpenAI 5h",
            "rolling",
            "1000"
        });
    REQUIRE(planInsert.success());

    REQUIRE(storage.execute(
        "INSERT INTO quota_snapshots ("
        "provider_id, account_id, plan_id, snapshot_time_utc, window_start_utc, window_end_utc, "
        "used_value, limit_value, confidence_tier, basis_summary, quota_dimension, remaining_value, is_reconciled"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
        {
            std::to_string(providerId),
            std::to_string(accountId),
            std::to_string(planInsert.lastInsertRowId()),
            "2026-04-20T15:30:00Z",
            "2026-04-20T10:00:01Z",
            "2026-04-20T15:00:00Z",
            "50",
            "1000",
            "high",
            "Current quota window test snapshot",
            "requests",
            "950",
            "1"
        }).success());
    REQUIRE(storage.execute(
        "INSERT INTO quota_snapshots ("
        "provider_id, account_id, plan_id, snapshot_time_utc, window_start_utc, window_end_utc, "
        "used_value, limit_value, confidence_tier, basis_summary, quota_dimension, remaining_value, is_reconciled"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
        {
            std::to_string(providerId),
            std::to_string(accountId),
            std::to_string(planInsert.lastInsertRowId()),
            "2026-04-20T14:45:00Z",
            "2026-04-20T10:00:01Z",
            "2026-04-20T15:00:00Z",
            "300",
            "1000",
            "high",
            "Current quota window in-range snapshot",
            "requests",
            "700",
            "1"
        }).success());

    AnalysisQueryEngine engine(storage);
    DashboardViewModel dashboard;
    DashboardPreferences preferences;
    preferences.analysisSelectedProviderId = "openai";
    preferences.analysisSelectedAccountId = "openai.runtime_status";
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::CurrentQuotaWindow;

    const ResolvedAnalysisWindow window = engine.resolveWindow(preferences, now);
    CHECK(window.start == makeUtcTime(2026, 4, 20, 10, 0, 1));
    CHECK(window.end == makeUtcTime(2026, 4, 20, 15, 0, 0));
    CHECK(window.label == "Current Billing/Quota Window");

    const AnalysisViewModel analysis = engine.buildViewModel(dashboard, preferences, now);
    CHECK(analysis.scope.dateRangePreset == AnalysisDateRangePreset::CurrentQuotaWindow);
    CHECK(analysis.scopeLabel == "OpenAI / OpenAI / OpenAI Runtime / Current Billing/Quota Window");
    REQUIRE(analysis.quotaTrendSeries.size() == 1);
    CHECK(analysis.quotaTrendSeries.front().points.size() == 1);
    CHECK(analysis.quotaTrendSeries.front().points.front().snapshotTime == makeUtcTime(2026, 4, 20, 14, 45, 0));
}

TEST_CASE("AnalysisQueryEngine offers persisted scope options from quota snapshots and recovery checkpoints without dashboard provider rows", "[analysis][query_engine][analysis_query_engine]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    const std::time_t now = makeUtcTime(2026, 4, 21, 12, 0, 0);
    const QueryResult minimaxInsert = storage.execute(
        "INSERT OR IGNORE INTO providers (provider_key, display_name) VALUES (?, ?);",
        {"minimax", "MiniMax"});
    REQUIRE(minimaxInsert.success());
    const int minimaxProviderId = requireProviderId(storage, "minimax");
    const int minimaxAccountId = ensureRuntimeAccountId(storage, "minimax", "MiniMax");
    const QueryResult kimiInsert = storage.execute(
        "INSERT OR IGNORE INTO providers (provider_key, display_name) VALUES (?, ?);",
        {"kimi", "Kimi Code"});
    REQUIRE(kimiInsert.success());
    const int kimiProviderId = requireProviderId(storage, "kimi");
    const int kimiAccountId = ensureRuntimeAccountId(storage, "kimi", "Kimi Code");

    const QueryResult planInsert = storage.execute(
        "INSERT INTO plans (account_id, plan_key, plan_type, display_name, quota_window, limit_value) "
        "VALUES (?, ?, ?, ?, ?, ?);",
        {
            std::to_string(minimaxAccountId),
            "minimax_subscription_coding_plan",
            "subscription",
            "MiniMax Coding Plan",
            "daily",
            "1000"
        });
    REQUIRE(planInsert.success());

    REQUIRE(storage.execute(
        "INSERT INTO quota_snapshots ("
        "provider_id, account_id, plan_id, snapshot_time_utc, window_start_utc, window_end_utc, "
        "used_value, limit_value, confidence_tier, basis_summary, quota_dimension, remaining_value, is_reconciled"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
        {
            std::to_string(minimaxProviderId),
            std::to_string(minimaxAccountId),
            std::to_string(planInsert.lastInsertRowId()),
            "2026-04-21T11:45:00Z",
            "2026-04-21T00:00:00Z",
            "2026-04-21T23:59:59Z",
            "320",
            "1000",
            "high",
            "Persisted quota-only scope seed",
            "requests",
            "680",
            "1"
        }).success());

    PersistedRecoveryCheckpoint kimiCheckpoint;
    kimiCheckpoint.providerId = kimiProviderId;
    kimiCheckpoint.accountId = kimiAccountId;
    kimiCheckpoint.syncKind = "runtime_backfill";
    kimiCheckpoint.recoveryStart = makeUtcTime(2026, 4, 21, 9, 0, 0);
    kimiCheckpoint.recoveryEnd = makeUtcTime(2026, 4, 21, 10, 0, 0);
    kimiCheckpoint.attemptAt = makeUtcTime(2026, 4, 21, 10, 5, 0);
    kimiCheckpoint.outcome = "fetch_failed";
    kimiCheckpoint.comparisonResult = "not_available";
    kimiCheckpoint.failureCategory = "provider_backfill_fetch_failed";
    REQUIRE(storage.insertRecoveryCheckpoint(kimiCheckpoint));

    AnalysisQueryEngine engine(storage);
    DashboardViewModel dashboard;
    DashboardPreferences preferences;
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Last7Days;

    const AnalysisViewModel analysis = engine.buildViewModel(dashboard, preferences, now);
    const std::vector<std::string> providerOptions = optionPairs(analysis.availableProviders);
    const std::vector<std::string> accountOptions = optionPairs(analysis.availableAccounts);

    CHECK(std::find(providerOptions.begin(), providerOptions.end(), "minimax|MiniMax") != providerOptions.end());
    CHECK(std::find(providerOptions.begin(), providerOptions.end(), "kimi|Kimi Code") != providerOptions.end());
    CHECK(std::find(
        accountOptions.begin(),
        accountOptions.end(),
        "minimax.runtime_status|MiniMax / MiniMax Runtime") != accountOptions.end());
    CHECK(std::find(
        accountOptions.begin(),
        accountOptions.end(),
        "kimi.runtime_status|Kimi Code / Kimi Code Runtime") != accountOptions.end());
}

TEST_CASE("Analysis fills uncovered quota days and series without repeating daily rollups",
          "[analysis][query_engine][quota_rollup_coverage]") {
    requireSqlite();
    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));
    const int providerId = requireProviderId(storage, "openai");
    const int accountId = ensureRuntimeAccountId(storage, "openai", "OpenAI");
    const auto plan = storage.execute("INSERT INTO plans (account_id, plan_key, plan_type, display_name, quota_window) "
                                      "VALUES (?, 'openai_subscription_5h', 'subscription', 'OpenAI 5h', 'rolling');",
                                      {std::to_string(accountId)});
    REQUIRE(plan.success());

    const auto insert = [&](const char* time, const char* remaining, const char* dimension, const char* limit = "100") {
        REQUIRE(storage
                    .execute("INSERT INTO quota_snapshots (provider_id, account_id, plan_id, snapshot_time_utc, "
                             "window_start_utc, window_end_utc, used_value, limit_value, confidence_tier, "
                             "quota_dimension, remaining_value) "
                             "VALUES (?, ?, ?, ?, '2026-04-01T00:00:00Z', '2026-04-02T00:00:00Z', 0, ?, 'high', ?, ?);",
                             {std::to_string(providerId), std::to_string(accountId),
                              std::to_string(plan.lastInsertRowId()), time, limit, dimension, remaining})
                    .success());
    };
    const auto firstDay = makeUtcTime(2026, 4, 1);
    const auto now = makeUtcTime(2026, 4, 20, 12);
    insert("2026-04-01T08:00:00Z", "50", "requests");
    insert("2026-04-01T10:00:00Z", "10", "requests");
    insert("2026-04-01T08:00:00Z", "70", "tokens");
    insert("2026-04-01T10:00:00Z", "0", "tokens", "0");
    REQUIRE(storage.rebuildQuotaSnapshotRollups(providerId, accountId, firstDay, firstDay));
    insert("2026-04-01T11:00:00Z", "80", "percent");
    insert("2026-04-02T08:00:00Z", "5", "requests");
    insert("2026-04-02T10:00:00Z", "90", "requests");

    DashboardPreferences preferences;
    preferences.analysisDateRangePreset = AnalysisDateRangePreset::Custom;
    preferences.analysisCustomRangeStartUtc = firstDay;
    preferences.analysisCustomRangeEndUtc = now;
    preferences.analysisSelectedProviderId = "openai";
    preferences.analysisSelectedAccountId = "openai.runtime_status";
    AnalysisQueryEngine engine(storage);
    const auto checkCoverage = [&](const AnalysisViewModel& analysis) {
        CHECK(analysis.quotaTrendSeries.size() == 3);
        const auto series = [&](std::string_view dimension) {
            return std::find_if(analysis.quotaTrendSeries.begin(), analysis.quotaTrendSeries.end(),
                                [dimension](const auto& item) { return item.quotaDimension == dimension; });
        };
        const auto requests = series("requests");
        REQUIRE(requests != analysis.quotaTrendSeries.end());
        CHECK(requests->latestRemainingPercentage == Catch::Approx(90.0));
        CHECK(requests->lowWaterRemainingPercentage == Catch::Approx(5.0));
        REQUIRE(requests->points.size() == 3);
        CHECK(requests->points[0].dailyRollup);
        CHECK(requests->points[0].remainingValue == 10);
        CHECK_FALSE(requests->points[1].dailyRollup);
        CHECK_FALSE(requests->points[2].dailyRollup);
        CHECK(requests->points[2].snapshotTime == makeUtcTime(2026, 4, 2, 10));

        const auto percent = series("percent");
        REQUIRE(percent != analysis.quotaTrendSeries.end());
        REQUIRE(percent->points.size() == 1);
        CHECK_FALSE(percent->points.front().dailyRollup);
        CHECK(percent->latestRemainingPercentage == Catch::Approx(80.0));

        const auto tokens = series("tokens");
        REQUIRE(tokens != analysis.quotaTrendSeries.end());
        REQUIRE(tokens->points.size() == 1);
        CHECK_FALSE(tokens->points.front().dailyRollup);
        CHECK(tokens->latestRemainingPercentage == Catch::Approx(70.0));
    };

    checkCoverage(engine.buildViewModel({}, preferences, now));
    REQUIRE(storage
                .execute("DELETE FROM quota_snapshots WHERE quota_dimension = 'requests' "
                         "AND snapshot_time_utc LIKE '2026-04-01%';")
                .success());
    checkCoverage(engine.buildViewModel({}, preferences, now));
}

TEST_CASE("AnalysisQueryEngine keeps date-range preset helper mappings stable for settings persistence", "[analysis][query_engine][analysis_query_engine]") {
    CHECK(std::string(AnalysisQueryEngine::dateRangePresetSettingValue(AnalysisDateRangePreset::Today)) == "today");
    CHECK(std::string(AnalysisQueryEngine::dateRangePresetSettingValue(AnalysisDateRangePreset::Last24Hours)) == "last_24_hours");
    CHECK(std::string(AnalysisQueryEngine::dateRangePresetSettingValue(AnalysisDateRangePreset::Last7Days)) == "last_7_days");
    CHECK(std::string(AnalysisQueryEngine::dateRangePresetSettingValue(AnalysisDateRangePreset::CurrentQuotaWindow)) == "current_quota_window");
    CHECK(std::string(AnalysisQueryEngine::dateRangePresetSettingValue(AnalysisDateRangePreset::ThisMonth)) == "this_month");
    CHECK(std::string(AnalysisQueryEngine::dateRangePresetSettingValue(AnalysisDateRangePreset::Custom)) == "custom");

    CHECK(AnalysisQueryEngine::parseDateRangePreset("today") == AnalysisDateRangePreset::Today);
    CHECK(AnalysisQueryEngine::parseDateRangePreset("last_24_hours") == AnalysisDateRangePreset::Last24Hours);
    CHECK(AnalysisQueryEngine::parseDateRangePreset("last_7_days") == AnalysisDateRangePreset::Last7Days);
    CHECK(AnalysisQueryEngine::parseDateRangePreset("current_quota_window") == AnalysisDateRangePreset::CurrentQuotaWindow);
    CHECK(AnalysisQueryEngine::parseDateRangePreset("this_month") == AnalysisDateRangePreset::ThisMonth);
    CHECK(AnalysisQueryEngine::parseDateRangePreset("custom") == AnalysisDateRangePreset::Custom);
    CHECK(AnalysisQueryEngine::parseDateRangePreset("unexpected") == AnalysisDateRangePreset::Last7Days);

    CHECK(AnalysisQueryEngine::dateRangePresetLabel(AnalysisDateRangePreset::Today) == "Today");
    CHECK(AnalysisQueryEngine::dateRangePresetLabel(AnalysisDateRangePreset::Last24Hours) == "Last 24 Hours");
    CHECK(AnalysisQueryEngine::dateRangePresetLabel(AnalysisDateRangePreset::Last7Days) == "Last 7 Days");
    CHECK(AnalysisQueryEngine::dateRangePresetLabel(AnalysisDateRangePreset::CurrentQuotaWindow) == "Current Billing/Quota Window");
    CHECK(AnalysisQueryEngine::dateRangePresetLabel(AnalysisDateRangePreset::ThisMonth) == "This Month");
    CHECK(AnalysisQueryEngine::dateRangePresetLabel(AnalysisDateRangePreset::Custom) == "Custom");
}
