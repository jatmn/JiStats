#pragma once

#include "db/StorageService.hpp"
#include "platform/WindowsCredentialStore.hpp"
#include "providers/IProviderAdapter.hpp"
#include "providers/minimax/MiniMaxAdapter.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace JiStats {

struct MiniMaxCapturePlanStatus {
    std::string planKey;
    std::string planDisplayName;
    std::string endpointFamily;
    std::string accountKey;
    std::string accountDisplayName;
    std::string profileName;
    std::string secretRef;
    bool metadataPresent = false;
    bool secretPresent = false;
    bool ready = false;
    std::string validationState = "missing";
    std::string lastValidationError;
    std::time_t lastValidatedAt = 0;
};

struct MiniMaxCaptureResult {
    bool success = false;
    MiniMaxCapturePlanStatus planStatus;
    std::string captureUtc;
    QuotaResponse quota;
    std::string error;
};

struct MiniMaxDiagnosticReport {
    std::string generatedAtUtc;
    std::string dbPath;
    bool validateLive = false;
    std::string error;
    std::vector<MiniMaxCapturePlanStatus> planStatuses;
    std::vector<WindowsCredentialSummary> credentialTargets;
};

class MiniMaxLiveCapture {
public:
    using AdapterFactory = std::function<std::shared_ptr<MiniMaxAdapter>()>;

    explicit MiniMaxLiveCapture(
        std::shared_ptr<WindowsCredentialStore> credentialStore = nullptr,
        AdapterFactory adapterFactory = {});

    std::vector<MiniMaxCapturePlanStatus> inspectStoredPlans(
        const std::string& dbPath,
        bool validateLive = false);

    MiniMaxCaptureResult capturePlan(
        const std::string& dbPath,
        const std::string& planKey,
        bool validateLive = true);

    bool storePlanSecret(
        const std::string& dbPath,
        const std::string& planKey,
        const std::string& secret,
        std::string& error);

    bool writeCaptureJson(
        const MiniMaxCaptureResult& result,
        const std::string& outputPath,
        std::string& error) const;

    MiniMaxDiagnosticReport collectDiagnostics(
        const std::string& dbPath,
        bool validateLive = false);

    bool writeDiagnosticJson(
        const MiniMaxDiagnosticReport& report,
        const std::string& outputPath,
        std::string& error) const;

    std::string getLastError() const;

    static bool isSupportedPlanKey(const std::string& planKey);

private:
    std::string lastError_;
    std::shared_ptr<WindowsCredentialStore> credentialStore_;
    AdapterFactory adapterFactory_;
};

} // namespace JiStats
