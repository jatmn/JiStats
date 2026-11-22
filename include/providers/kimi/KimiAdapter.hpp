#pragma once

#include "observation/ActivityEvent.hpp"
#include "providers/IProviderAdapter.hpp"

#include <memory>
#include <string>
#include <vector>

namespace JiStats {

class StorageService;

struct KimiApprovalMode {
    bool yolo = false;
    bool autoApproveActions = false;
};

struct KimiTaskRuntimeSummary {
    std::string taskId;
    std::string status;
    std::time_t startedAt = 0;
    std::time_t updatedAt = 0;
    std::time_t finishedAt = 0;
    int exitCode = 0;
    bool hasExitCode = false;
    bool interrupted = false;
    bool timedOut = false;
};

struct KimiNotificationSummary {
    std::string notificationId;
    std::string category;
    std::string type;
    std::string sourceKind;
    std::string severity;
    std::time_t createdAt = 0;
    std::string payloadStatus;
    int payloadExitCode = 0;
    bool hasPayloadExitCode = false;
    bool payloadInterrupted = false;
    bool payloadTimedOut = false;
    std::string deliveryStatus;
};

struct KimiLocalActivitySnapshot {
    std::string providerKey = "kimi";
    std::string providerSurface = "kimi_local_runtime";
    std::string sourceKind = "kimi_local_runtime";
    std::string sessionRootId;
    std::string sessionId;
    std::string taskId;
    std::time_t observedAt = 0;
    std::string workingDirScope;
    std::string modelKey;
    std::string providerType;
    std::string providerBaseUrl;
    bool planMode = false;
    KimiApprovalMode approvalMode;
    int dynamicSubagentCount = 0;
    int additionalDirCount = 0;
    KimiTaskRuntimeSummary taskRuntime;
    KimiNotificationSummary notificationSummary;
    std::vector<std::string> confidenceNotes;

    ActivityEvent toActivityEvent() const;
};

struct KimiLocalParseResult {
    bool success = false;
    std::string error;
    std::string rootPath;
    std::string providerKey = "kimi";
    std::string providerSurface = "kimi_local_runtime";
    std::string defaultModelKey;
    std::string providerType;
    std::string providerBaseUrl;
    long long sessionCount = 0;
    long long taskCount = 0;
    long long completedTaskCount = 0;
    long long failedTaskCount = 0;
    long long timedOutTaskCount = 0;
    std::time_t earliestObservedAt = 0;
    std::time_t latestObservedAt = 0;
    std::vector<KimiLocalActivitySnapshot> activitySnapshots;
    std::vector<ActivityEvent> normalizedActivityEvents;
    std::string rawSummary;
};

class KimiAdapter : public IProviderAdapter {
public:
    explicit KimiAdapter(const std::string& rootPath = "");
    ~KimiAdapter() override;

    UsageResponse fetchUsage(std::time_t from, std::time_t to) override;
    QuotaResponse fetchQuota() override;
    bool validateCredentials() override;
    std::vector<Capability> getCapabilities() override;
    std::string getProviderKey() const override;
    std::string getDisplayName() const override;
    bool isEnabled() const override;
    void setEnabled(bool enabled) override;
    bool hasDirectQuotaApiConfigured() const override;
    std::string directQuotaApiThrottleKey() const override;

    KimiLocalParseResult fetchLocalActivity() const;

    static KimiLocalParseResult parseLocalState(const std::string& rootPath);
    static bool importLocalActivity(StorageService& storage, const std::string& rootPath = "");
    static std::string hashLocalPathScope(const std::string& rawPath);
    static bool hasLocalInstallation(const std::string& rootPath = "");
    static std::string resolveDefaultRootPath();

#ifdef JISTATS_ENABLE_TEST_HOOKS
    static QuotaResponse parseUsagesResponseForTesting(const std::string& jsonBody);
    static std::string resolveEffectiveRootPathForTesting(const std::string& configuredRootPath = "");
#endif

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace JiStats
