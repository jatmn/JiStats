#pragma once

#include "providers/IProviderAdapter.hpp"
#include <string>
#include <vector>
#include <memory>

namespace JiStats {

struct CodexRateLimitBucket {
    std::string bucketKey;
    double usedPercent = 0.0;
    int windowDurationMinutes = 0;
    std::time_t resetsAt = 0;
    bool present = false;
};

struct CodexRateLimitSnapshot {
    bool success = false;
    std::string error;
    std::time_t snapshotTime = 0;
    std::time_t eventTime = 0;
    CodexRateLimitBucket primary;
    CodexRateLimitBucket secondary;
    std::string limitId;
    std::string planType;
    std::string reachedType;
    std::string rawResponse;
    std::string sourcePath;
    std::string sourceType;
};

struct CodexTokenUsageSnapshot {
    std::time_t eventTime = 0;
    long long inputTokens = 0;
    long long cachedInputTokens = 0;
    long long outputTokens = 0;
    long long reasoningOutputTokens = 0;
    long long totalTokens = 0;
    std::string sourcePath;
    std::string requestId; // Empty for cumulative-only rollouts.
};

class OpenAIAdapter : public IProviderAdapter {
public:
    OpenAIAdapter(const std::string& apiKey = "", const std::string& groupId = "");
    ~OpenAIAdapter() override;

    // IProviderAdapter implementation
    UsageResponse fetchUsage(std::time_t from, std::time_t to) override;
    QuotaResponse fetchQuota() override;
    bool validateCredentials() override;
    std::vector<Capability> getCapabilities() override;
    std::string getProviderKey() const override;
    std::string getDisplayName() const override;
    bool isEnabled() const override;
    void setEnabled(bool enabled) override;

    // Fixture parsing helpers
    static std::vector<UsageEvent> parseFixtureFile(const std::string& filePath);
    static CodexRateLimitSnapshot parseCodexRateLimitFixtureFile(const std::string& filePath);
    static CodexRateLimitSnapshot parseLatestCodexRateLimitSnapshotFromRolloutDirectory(
        const std::string& directoryPath);

    // Codex token-usage extraction
    static std::vector<CodexTokenUsageSnapshot> parseCodexTokenUsageFromRolloutFile(
        const std::string& filePath);
    static std::vector<CodexTokenUsageSnapshot> collectCodexTokenUsageSnapshots(
        const std::string& directoryPath);
    static std::string resolveCodexRolloutDirectory();

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace JiStats
