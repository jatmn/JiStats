#pragma once

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "app/TrayController.hpp"
#include "db/StorageService.hpp"
#include "gui/DashboardPresenter.hpp"
#include "providers/IProviderAdapter.hpp"
#include "runtime_test_support.hpp"

namespace JiStats::TestSupport {

class ScopedEnvironmentVariable {
  public:
    ScopedEnvironmentVariable(const char* name, const std::string& value) : name_(name ? name : "") {
        char* current = nullptr;
        std::size_t currentLength = 0;
        if (!name_.empty() && _dupenv_s(&current, &currentLength, name_.c_str()) == 0 && current != nullptr) {
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

inline std::string toUtcIso(std::time_t value) {
    std::tm utcTime{};
    gmtime_s(&utcTime, &value);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utcTime);
    return buffer;
}

void seedStartupPreferences(
    const std::string& path, bool enabled, bool startBackground, bool openaiSubscriptionTrackingEnabled = false,
    bool miniMaxSubscriptionTrackingEnabled = false, bool claudeCodeEnabled = false, bool kimiCodeEnabled = false,
    bool scheduledExportEnabled = false, int scheduledExportIntervalMinutes = 1440,
    const std::string& scheduledExportPath = "", const std::string& lastSelectedProviderId = "",
    const std::string& lastSelectedAccountId = "", const std::string& analysisSelectedProviderId = "",
    const std::string& analysisSelectedAccountId = "", const std::string& analysisDateRangePreset = "last_7_days",
    const std::string& analysisCustomRangeStartUtc = "", const std::string& analysisCustomRangeEndUtc = "");

inline void seedAllProviderTrackingEnabled(const std::string& path) {
    seedStartupPreferences(path, false, false, true, true, true, true);
}

inline std::optional<int> loadProviderId(StorageService& storage, const std::string& providerKey) {
    const QueryResult result =
        storage.execute("SELECT id FROM providers WHERE provider_key = ? LIMIT 1;", {providerKey});
    if (!result.success() || result.rowCount() == 0) {
        return std::nullopt;
    }

    return result.getInt(0, 0);
}

inline std::string makeCodexRolloutPayload(double usedPercent, std::time_t eventTime, std::time_t resetsAt) {
    const std::string timestamp = eventTime > 0 ? "\"timestamp\":\"" + toUtcIso(eventTime) + "\"," : "";
    return "{" + timestamp +
           "\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"rate_limits\":"
           "{\"limit_id\":\"codex\",\"primary\":{\"used_percent\":" +
           std::to_string(usedPercent) + ",\"window_minutes\":300,\"resets_at\":" + std::to_string(resetsAt) +
           "},\"secondary\":null,\"plan_type\":\"prolite\"}}}\n";
}

inline void writeCodexRolloutFile(const std::filesystem::path& path, double usedPercent, std::time_t eventTime,
                                  std::time_t resetsAt) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    REQUIRE_FALSE(ec);

    std::ofstream out(path, std::ios::trunc);
    REQUIRE(out.is_open());
    out << makeCodexRolloutPayload(usedPercent, eventTime, resetsAt);
    out.close();
}

inline void seedRuntimeState(const std::string& path, std::time_t lastLaunchAt, std::time_t lastSuccessfulSyncAt,
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

inline long long requireSingleId(const QueryResult& result) {
    REQUIRE(result.success());
    REQUIRE(result.rowCount() == 1);
    return result.getInt64(0, 0);
}

inline int requireProviderId(StorageService& storage, const std::string& providerKey) {
    return static_cast<int>(
        requireSingleId(storage.execute("SELECT id FROM providers WHERE provider_key = ? LIMIT 1;", {providerKey})));
}

inline int requireAccountId(StorageService& storage, int providerId, const std::string& accountKey) {
    return static_cast<int>(
        requireSingleId(storage.execute("SELECT id FROM accounts WHERE provider_id = ? AND account_key = ? LIMIT 1;",
                                        {std::to_string(providerId), accountKey})));
}

inline int ensureProviderId(StorageService& storage, const std::string& providerKey, const std::string& displayName) {
    if (const auto existing = loadProviderId(storage, providerKey)) {
        return *existing;
    }

    auto insert = storage.execute("INSERT INTO providers (provider_key, display_name) VALUES (?, ?);",
                                  {providerKey, displayName});
    REQUIRE(insert.success());
    return static_cast<int>(insert.lastInsertRowId());
}

inline const ProviderStatus* findProviderStatus(const DashboardViewModel& viewModel, const std::string& providerKey) {
    const auto it =
        std::find_if(viewModel.providers.begin(), viewModel.providers.end(),
                     [&providerKey](const ProviderStatus& provider) { return provider.providerKey == providerKey; });
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

inline std::string formatTimeUtcIsoForTest(std::time_t time) {
    std::tm tm{};
    gmtime_s(&tm, &time);

    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

inline void seedPersistedQuotaSnapshot(const std::string& path, const std::string& providerKey,
                                       const std::string& accountKey, const std::string& accountDisplayName,
                                       const std::string& planKey, const std::string& planDisplayName,
                                       const std::string& quotaWindow, std::time_t snapshotTime, long long usedValue,
                                       long long limitValue, long long remainingValue,
                                       const std::string& confidenceTier = "high") {
    StorageService storage;
    REQUIRE(storage.init(path));

    const QueryResult providerResult =
        storage.execute("SELECT id FROM providers WHERE provider_key = ? LIMIT 1;", {providerKey});
    REQUIRE(providerResult.success());
    REQUIRE(providerResult.rowCount() == 1);
    const int providerId = providerResult.getInt(0, 0);

    const auto accountInsert =
        storage.execute("INSERT INTO accounts (provider_id, account_key, display_name, scope_type, scope_identifier) "
                        "VALUES (?, ?, ?, ?, ?);",
                        {std::to_string(providerId), accountKey, accountDisplayName, "subscription", accountKey});
    REQUIRE(accountInsert.success());
    const int accountId = static_cast<int>(accountInsert.lastInsertRowId());

    const auto planInsert = storage.execute(
        "INSERT INTO plans (account_id, plan_key, plan_type, display_name, quota_window, limit_value) "
        "VALUES (?, ?, ?, ?, ?, ?);",
        {std::to_string(accountId), planKey, "subscription", planDisplayName, quotaWindow, std::to_string(limitValue)});
    REQUIRE(planInsert.success());
    const int planId = static_cast<int>(planInsert.lastInsertRowId());

    REQUIRE(storage
                .execute("INSERT INTO quota_snapshots ("
                         "provider_id, account_id, plan_id, snapshot_time_utc, window_start_utc, window_end_utc, "
                         "used_value, limit_value, confidence_tier, basis_summary, quota_dimension, remaining_value, "
                         "is_reconciled) "
                         "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
                         {std::to_string(providerId), std::to_string(accountId), std::to_string(planId),
                          formatTimeUtcIsoForTest(snapshotTime), formatTimeUtcIsoForTest(snapshotTime - 3600),
                          formatTimeUtcIsoForTest(snapshotTime), std::to_string(usedValue), std::to_string(limitValue),
                          confidenceTier, "Persisted test snapshot", "requests", std::to_string(remainingValue), "0"})
                .success());

    storage.close();
}

class ControlledQuotaProviderAdapter final : public IProviderAdapter {
  public:
    ControlledQuotaProviderAdapter(std::string providerKey, std::string displayName, QuotaResponse quotaResponse,
                                   std::shared_future<void> releaseQuota = {}, int firstBlockedFetch = 1,
                                   std::optional<QuotaResponse> firstQuotaResponse = std::nullopt)
        : providerKey_(std::move(providerKey)), displayName_(std::move(displayName)),
          quotaResponse_(std::move(quotaResponse)), releaseQuota_(std::move(releaseQuota)),
          firstBlockedFetch_(firstBlockedFetch), firstQuotaResponse_(std::move(firstQuotaResponse)) {}

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

inline bool waitForCondition(const std::function<bool()>& predicate,
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

} // namespace JiStats::TestSupport
