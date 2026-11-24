#include "providers/minimax/MiniMaxLiveCapture.hpp"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace JiStats {

namespace {

std::string formatTimeUtcIso(std::time_t time) {
    if (time <= 0) {
        return "";
    }

    std::tm tm{};
    gmtime_s(&tm, &time);

    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream stream;
    for (const char ch : value) {
        switch (ch) {
        case '\\':
            stream << "\\\\";
            break;
        case '"':
            stream << "\\\"";
            break;
        case '\r':
            stream << "\\r";
            break;
        case '\n':
            stream << "\\n";
            break;
        case '\t':
            stream << "\\t";
            break;
        default:
            stream << ch;
            break;
        }
    }
    return stream.str();
}

std::optional<std::filesystem::path> prepareCaptureOutputPath(const std::string& outputPath, std::string& error) {
    std::error_code ec;
    const auto absolutePath = std::filesystem::absolute(std::filesystem::path(outputPath), ec);
    if (ec) {
        error = "Failed to resolve output path: " + outputPath;
        return std::nullopt;
    }
    const auto path = std::filesystem::weakly_canonical(absolutePath, ec);
    if (ec) {
        error = "Failed to resolve output path: " + outputPath;
        return std::nullopt;
    }

    const auto parent = path.parent_path();
    for (auto directory = parent; !directory.empty(); directory = directory.parent_path()) {
        // Recognize a source tree even for an out-of-tree build or an extracted source archive.
        if (std::filesystem::is_regular_file(directory / "CMakeLists.txt", ec) &&
            std::filesystem::is_regular_file(directory / "AGENTS.md", ec) &&
            std::filesystem::is_regular_file(directory / "src/providers/minimax/MiniMaxLiveCapture.cpp", ec)) {
            error = "Capture output must be outside the JiStats source checkout.";
            return std::nullopt;
        }
        if (directory == directory.root_path()) {
            break;
        }
    }

    std::filesystem::create_directories(parent, ec);
    if (ec) {
        error = "Failed to create output directory: " + parent.string();
        return std::nullopt;
    }
    error.clear();
    return path;
}

std::string planDisplayName(const std::string& planKey) {
    return planKey == "token_plan" ? "MiniMax Token Plan" : "MiniMax Coding Plan";
}

std::string endpointFamilyForPlan(const std::string& planKey) {
    return planKey == "token_plan" ? "token_plan_remains" : "coding_plan_remains";
}

std::string accountKeyForPlan(const std::string& planKey) {
    return planKey + ".default";
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

std::optional<int> findAccountId(StorageService& storage,
                                 int providerId,
                                 const std::string& accountKey,
                                 std::string& displayName) {
    QueryResult result = storage.execute(
        "SELECT id, display_name FROM accounts WHERE provider_id = ? AND account_key = ? LIMIT 1;",
        {std::to_string(providerId), accountKey});
    if (!result.success() || result.rowCount() == 0) {
        return std::nullopt;
    }

    displayName = result.getString(0, 1);
    return result.getInt(0, 0);
}

MiniMaxSubscriptionCredentialProfile buildProfileFromStatus(const MiniMaxCapturePlanStatus& status) {
    MiniMaxSubscriptionCredentialProfile profile;
    profile.accountKey = status.accountKey;
    profile.accountDisplayName = status.accountDisplayName;
    profile.profileName = status.profileName.empty() ? status.planDisplayName : status.profileName;
    profile.planKey = status.planKey;
    profile.planDisplayName = status.planDisplayName;
    profile.secretRef = status.secretRef;
    profile.endpointFamily = status.endpointFamily;
    profile.validationState = status.validationState;
    profile.lastValidatedAt = status.lastValidatedAt;
    profile.lastValidationError = status.lastValidationError;
    return profile;
}

} // namespace

MiniMaxLiveCapture::MiniMaxLiveCapture(
    std::shared_ptr<WindowsCredentialStore> credentialStore,
    AdapterFactory adapterFactory)
    : credentialStore_(credentialStore ? std::move(credentialStore)
                                       : std::make_shared<WindowsCredentialStore>()),
      adapterFactory_(adapterFactory ? std::move(adapterFactory)
                                     : []() { return std::make_shared<MiniMaxAdapter>(); }) {}

bool MiniMaxLiveCapture::isSupportedPlanKey(const std::string& planKey) {
    return planKey == "coding_plan" || planKey == "token_plan";
}

std::vector<MiniMaxCapturePlanStatus> MiniMaxLiveCapture::inspectStoredPlans(
    const std::string& dbPath,
    bool validateLive) {
    lastError_.clear();
    std::vector<MiniMaxCapturePlanStatus> statuses;
    StorageService storage;
    if (!storage.init(dbPath)) {
        lastError_ = storage.getLastError().empty()
            ? "Failed to open JiStats database: " + dbPath
            : storage.getLastError();
        return statuses;
    }

    const auto providerId = findProviderIdByKey(storage, "minimax");
    if (!providerId.has_value()) {
        lastError_ = "MiniMax provider metadata was not found in the configured JiStats database";
        storage.close();
        return statuses;
    }

    for (const std::string& planKey : {"coding_plan", "token_plan"}) {
        MiniMaxCapturePlanStatus status;
        status.planKey = planKey;
        status.planDisplayName = planDisplayName(planKey);
        status.endpointFamily = endpointFamilyForPlan(planKey);
        status.accountKey = accountKeyForPlan(planKey);

        std::string accountDisplayName;
        const auto accountId = findAccountId(storage, *providerId, status.accountKey, accountDisplayName);
        if (!accountId.has_value()) {
            statuses.push_back(std::move(status));
            continue;
        }

        status.metadataPresent = true;
        status.accountDisplayName = accountDisplayName;
        const auto storedProfile = storage.getCredentialProfile(*accountId, status.planDisplayName);
        if (!storedProfile.has_value()) {
            status.lastValidationError = "Credential profile metadata is missing";
            statuses.push_back(std::move(status));
            continue;
        }

        status.profileName = storedProfile->profileName;
        status.secretRef = storedProfile->secretRef;
        status.lastValidatedAt = storedProfile->lastValidatedAt;
        if (!status.secretRef.empty()) {
            status.secretPresent = credentialStore_->readGenericCredential(
                status.secretRef).has_value();
            if (!status.secretPresent) {
                status.lastValidationError = credentialStore_->getLastError();
            }
        } else {
            status.lastValidationError = "Credential profile secret_ref is missing";
        }

        if (validateLive && status.secretPresent) {
            const auto adapter = adapterFactory_();
            adapter->setCredentialStore(credentialStore_);
            adapter->setSubscriptionCredentialProfile(buildProfileFromStatus(status));
            adapter->validateCredentials();
            const MiniMaxSubscriptionCredentialProfile validated = adapter->getSubscriptionCredentialProfile();
            status.validationState = validated.validationState;
            status.lastValidationError = validated.lastValidationError;
            status.lastValidatedAt = validated.lastValidatedAt;
            status.ready = validated.validationState == "valid";
        } else {
            status.ready = status.metadataPresent && status.secretPresent;
            status.validationState = status.ready ? "present" : "missing";
        }

        statuses.push_back(std::move(status));
    }

    storage.close();
    return statuses;
}

MiniMaxCaptureResult MiniMaxLiveCapture::capturePlan(
    const std::string& dbPath,
    const std::string& planKey,
    bool validateLive) {
    MiniMaxCaptureResult result;
    result.captureUtc = formatTimeUtcIso(std::time(nullptr));
    result.planStatus.planKey = planKey;
    result.planStatus.planDisplayName = planDisplayName(planKey);
    result.planStatus.endpointFamily = endpointFamilyForPlan(planKey);

    if (!isSupportedPlanKey(planKey)) {
        result.error = "Unsupported MiniMax plan key: " + planKey;
        return result;
    }

    const std::vector<MiniMaxCapturePlanStatus> statuses = inspectStoredPlans(dbPath, validateLive);
    if (statuses.empty() && !lastError_.empty()) {
        result.error = lastError_;
        return result;
    }
    const auto it = std::find_if(
        statuses.begin(),
        statuses.end(),
        [&planKey](const MiniMaxCapturePlanStatus& status) {
            return status.planKey == planKey;
        });
    if (it == statuses.end()) {
        result.error = "MiniMax credential metadata was not found for plan " + planKey;
        return result;
    }

    result.planStatus = *it;
    if (!it->ready) {
        result.error = it->lastValidationError.empty()
            ? "MiniMax plan is not ready for live capture"
            : it->lastValidationError;
        return result;
    }

    const auto adapter = adapterFactory_();
    adapter->setCredentialStore(credentialStore_);
    adapter->setSubscriptionCredentialProfile(buildProfileFromStatus(*it));
    result.quota = adapter->fetchQuota();
    if (!result.quota.success) {
        result.error = result.quota.error;
        return result;
    }

    result.success = true;
    return result;
}

bool MiniMaxLiveCapture::storePlanSecret(
    const std::string& dbPath,
    const std::string& planKey,
    const std::string& secret,
    std::string& error) {
    if (!isSupportedPlanKey(planKey)) {
        error = "Unsupported MiniMax plan key: " + planKey;
        return false;
    }
    if (secret.empty()) {
        error = "MiniMax secret is required";
        return false;
    }

    const std::vector<MiniMaxCapturePlanStatus> statuses = inspectStoredPlans(dbPath, false);
    if (statuses.empty() && !lastError_.empty()) {
        error = lastError_;
        return false;
    }

    const auto it = std::find_if(
        statuses.begin(),
        statuses.end(),
        [&planKey](const MiniMaxCapturePlanStatus& status) {
            return status.planKey == planKey;
        });
    if (it == statuses.end()) {
        error = "MiniMax credential metadata was not found for plan " + planKey;
        return false;
    }
    if (!it->metadataPresent) {
        error = "MiniMax account metadata is missing for plan " + planKey;
        return false;
    }
    if (it->secretRef.empty()) {
        error = "Credential profile secret_ref is missing for plan " + planKey;
        return false;
    }

    WindowsCredentialWriteRequest request;
    request.targetName = it->secretRef;
    request.secret = secret;
    request.userName = it->profileName.empty() ? it->planDisplayName : it->profileName;
    request.comment = "JiStats MiniMax subscription credential";

    if (!credentialStore_->writeGenericCredential(request)) {
        error = credentialStore_->getLastError();
        return false;
    }

    error.clear();
    return true;
}

bool MiniMaxLiveCapture::writeCaptureJson(
    const MiniMaxCaptureResult& result,
    const std::string& outputPath,
    std::string& error) const {
    const auto path = prepareCaptureOutputPath(outputPath, error);
    if (!path) {
        return false;
    }

    std::ofstream output(*path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        error = "Failed to open output path: " + outputPath;
        return false;
    }

    output << "{\n";
    output << "  \"success\": " << (result.success ? "true" : "false") << ",\n";
    output << "  \"capture_utc\": \"" << jsonEscape(result.captureUtc) << "\",\n";
    output << "  \"plan_key\": \"" << jsonEscape(result.planStatus.planKey) << "\",\n";
    output << "  \"plan_display_name\": \"" << jsonEscape(result.planStatus.planDisplayName) << "\",\n";
    output << "  \"endpoint_family\": \"" << jsonEscape(result.planStatus.endpointFamily) << "\",\n";
    output << "  \"account_key\": \"" << jsonEscape(result.planStatus.accountKey) << "\",\n";
    output << "  \"account_display_name\": \"" << jsonEscape(result.planStatus.accountDisplayName) << "\",\n";
    output << "  \"profile_name\": \"" << jsonEscape(result.planStatus.profileName) << "\",\n";
    output << "  \"secret_ref\": \"" << jsonEscape(result.planStatus.secretRef) << "\",\n";
    output << "  \"validation_state\": \"" << jsonEscape(result.planStatus.validationState) << "\",\n";
    output << "  \"last_validation_error\": \"" << jsonEscape(result.planStatus.lastValidationError) << "\",\n";
    output << "  \"last_validated_at_utc\": \"" << jsonEscape(formatTimeUtcIso(result.planStatus.lastValidatedAt)) << "\",\n";
    output << "  \"snapshot_time_utc\": \"" << jsonEscape(formatTimeUtcIso(result.quota.snapshotTime)) << "\",\n";
    output << "  \"window_start_utc\": \"" << jsonEscape(formatTimeUtcIso(result.quota.windowStart)) << "\",\n";
    output << "  \"window_end_utc\": \"" << jsonEscape(formatTimeUtcIso(result.quota.windowEnd)) << "\",\n";
    output << "  \"window_type\": \"" << jsonEscape(result.quota.windowType) << "\",\n";
    output << "  \"provider_surface\": \"" << jsonEscape(result.quota.providerSurface) << "\",\n";
    output << "  \"used\": " << result.quota.used << ",\n";
    output << "  \"limit\": " << result.quota.limit << ",\n";
    output << "  \"remaining\": " << result.quota.remaining << ",\n";
    output << "  \"error\": \"" << jsonEscape(result.error) << "\",\n";
    output << "  \"raw_response\": \"" << jsonEscape(result.quota.rawResponse) << "\"\n";
    output << "}\n";

    if (!output.good()) {
        error = "Failed while writing output path: " + outputPath;
        return false;
    }

    error.clear();
    return true;
}

MiniMaxDiagnosticReport MiniMaxLiveCapture::collectDiagnostics(
    const std::string& dbPath,
    bool validateLive) {
    MiniMaxDiagnosticReport report;
    report.generatedAtUtc = formatTimeUtcIso(std::time(nullptr));
    report.dbPath = dbPath;
    report.validateLive = validateLive;
    report.planStatuses = inspectStoredPlans(dbPath, validateLive);
    report.error = getLastError();
    report.credentialTargets = credentialStore_->listGenericCredentials("JiStats/minimax/");
    if (report.error.empty() && !credentialStore_->getLastError().empty()) {
        report.error = credentialStore_->getLastError();
    }
    return report;
}

bool MiniMaxLiveCapture::writeDiagnosticJson(
    const MiniMaxDiagnosticReport& report,
    const std::string& outputPath,
    std::string& error) const {
    const auto path = prepareCaptureOutputPath(outputPath, error);
    if (!path) {
        return false;
    }

    std::ofstream output(*path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        error = "Failed to open output path: " + outputPath;
        return false;
    }

    output << "{\n";
    output << "  \"generated_at_utc\": \"" << jsonEscape(report.generatedAtUtc) << "\",\n";
    output << "  \"db_path\": \"" << jsonEscape(report.dbPath) << "\",\n";
    output << "  \"validate_live\": " << (report.validateLive ? "true" : "false") << ",\n";
    output << "  \"error\": \"" << jsonEscape(report.error) << "\",\n";
    output << "  \"plans\": [\n";
    for (size_t index = 0; index < report.planStatuses.size(); ++index) {
        const MiniMaxCapturePlanStatus& status = report.planStatuses[index];
        output << "    {\n";
        output << "      \"plan_key\": \"" << jsonEscape(status.planKey) << "\",\n";
        output << "      \"plan_display_name\": \"" << jsonEscape(status.planDisplayName) << "\",\n";
        output << "      \"endpoint_family\": \"" << jsonEscape(status.endpointFamily) << "\",\n";
        output << "      \"account_key\": \"" << jsonEscape(status.accountKey) << "\",\n";
        output << "      \"account_display_name\": \"" << jsonEscape(status.accountDisplayName) << "\",\n";
        output << "      \"profile_name\": \"" << jsonEscape(status.profileName) << "\",\n";
        output << "      \"secret_ref\": \"" << jsonEscape(status.secretRef) << "\",\n";
        output << "      \"metadata_present\": " << (status.metadataPresent ? "true" : "false") << ",\n";
        output << "      \"secret_present\": " << (status.secretPresent ? "true" : "false") << ",\n";
        output << "      \"ready\": " << (status.ready ? "true" : "false") << ",\n";
        output << "      \"validation_state\": \"" << jsonEscape(status.validationState) << "\",\n";
        output << "      \"last_validation_error\": \"" << jsonEscape(status.lastValidationError) << "\",\n";
        output << "      \"last_validated_at_utc\": \"" << jsonEscape(formatTimeUtcIso(status.lastValidatedAt)) << "\"\n";
        output << "    }";
        output << (index + 1 < report.planStatuses.size() ? ",\n" : "\n");
    }
    output << "  ],\n";
    output << "  \"credential_targets\": [\n";
    for (size_t index = 0; index < report.credentialTargets.size(); ++index) {
        const WindowsCredentialSummary& summary = report.credentialTargets[index];
        output << "    {\n";
        output << "      \"target_name\": \"" << jsonEscape(summary.targetName) << "\",\n";
        output << "      \"user_name\": \"" << jsonEscape(summary.userName) << "\",\n";
        output << "      \"comment\": \"" << jsonEscape(summary.comment) << "\"\n";
        output << "    }";
        output << (index + 1 < report.credentialTargets.size() ? ",\n" : "\n");
    }
    output << "  ]\n";
    output << "}\n";

    if (!output.good()) {
        error = "Failed while writing output path: " + outputPath;
        return false;
    }

    error.clear();
    return true;
}

std::string MiniMaxLiveCapture::getLastError() const {
    return lastError_;
}

} // namespace JiStats
