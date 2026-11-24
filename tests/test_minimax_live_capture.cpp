#define JISTATS_ENABLE_TEST_HOOKS 1

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "db/StorageService.hpp"
#include "platform/WindowsCredentialStore.hpp"
#include "providers/minimax/MiniMaxAdapter.hpp"
#include "providers/minimax/MiniMaxLiveCapture.hpp"
#include "sqlite_test_support.hpp"
#include "temp_test_directory.hpp"

using namespace JiStats;
using JiStats::TestSupport::TempTestDatabase;
using JiStats::TestSupport::TempTestDirectory;
using JiStats::TestSupport::requireSqlite;

namespace {

class TempOutputFile {
public:
    std::string path() const {
        return path_.string();
    }

    std::string readAll() const {
        std::ifstream input(path_, std::ios::binary);
        if (!input.is_open()) {
            throw std::runtime_error("Failed to open capture output file");
        }
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    }

private:
    TempTestDirectory directory_{"jistats_minimax_output"};
    std::filesystem::path path_ = directory_.path() / "capture.json";
};

class TempOutputDirectory {
public:
    std::string nestedFilePath(const std::string& fileName) const {
        return (directory_.path() / "captures" / fileName).string();
    }

private:
    TempTestDirectory directory_{"jistats_minimax_capture"};
};

class FakeWindowsCredentialBackend final : public IWindowsCredentialBackend {
public:
    bool writeGenericCredential(
        const WindowsCredentialWriteRequest& request,
        std::string& error) override {
        if (request.targetName.empty()) {
            error = "Credential target name is required";
            return false;
        }
        if (request.secret.empty()) {
            error = "Credential secret is required";
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

private:
    std::map<std::string, std::string> secrets_;
};

std::shared_ptr<WindowsCredentialStore> makeFakeCredentialStore() {
    return std::make_shared<WindowsCredentialStore>(
        std::make_shared<FakeWindowsCredentialBackend>());
}

MiniMaxSubscriptionCredentialProfile makeSubscriptionProfile(const std::string& planKey) {
    MiniMaxSubscriptionCredentialProfile profile;
    profile.accountKey = planKey + ".default";
    profile.accountDisplayName = "MiniMax Subscription";
    profile.profileName = planKey == "token_plan" ? "MiniMax Token Plan" : "MiniMax Coding Plan";
    profile.planKey = planKey;
    profile.planDisplayName = profile.profileName;
    profile.scopeIdentifier = profile.accountKey;
    profile.endpointFamily = planKey == "token_plan"
        ? "token_plan_remains"
        : "coding_plan_remains";
    return profile;
}

void seedCredentialMetadata(const std::string& dbPath, const MiniMaxSubscriptionCredentialProfile& profile) {
    StorageService storage;
    REQUIRE(storage.init(dbPath));

    const QueryResult providerResult = storage.execute(
        "SELECT id FROM providers WHERE provider_key = 'minimax' LIMIT 1;");
    REQUIRE(providerResult.success());
    REQUIRE(providerResult.rowCount() == 1);
    const int providerId = providerResult.getInt(0, 0);

    const QueryResult accountInsert = storage.execute(
        "INSERT INTO accounts (provider_id, account_key, display_name, scope_type, scope_identifier) "
        "VALUES (?, ?, ?, ?, ?);",
        {
            std::to_string(providerId),
            profile.accountKey,
            profile.accountDisplayName,
            profile.scopeType,
            profile.scopeIdentifier
        });
    REQUIRE(accountInsert.success());
    const int accountId = static_cast<int>(accountInsert.lastInsertRowId());

    PersistedCredentialProfile persisted;
    persisted.providerId = providerId;
    persisted.accountId = accountId;
    persisted.profileName = profile.profileName;
    persisted.secretRef = MiniMaxAdapter::buildSecretRef(profile);
    persisted.credentialType = profile.credentialType;
    persisted.lastValidatedAt = 1713900000;
    REQUIRE(storage.upsertCredentialProfile(persisted));

    storage.close();
}

void seedCredentialSecret(
    const std::shared_ptr<WindowsCredentialStore>& store,
    const MiniMaxSubscriptionCredentialProfile& profile,
    const std::string& secret) {
    WindowsCredentialWriteRequest request;
    request.targetName = MiniMaxAdapter::buildSecretRef(profile);
    request.secret = secret;
    request.userName = profile.profileName;
    request.comment = "JiStats MiniMax live capture test secret";
    REQUIRE(store->writeGenericCredential(request));
}

const MiniMaxCapturePlanStatus* findPlanStatus(
    const std::vector<MiniMaxCapturePlanStatus>& statuses,
    const std::string& planKey) {
    const auto it = std::find_if(
        statuses.begin(),
        statuses.end(),
        [&planKey](const MiniMaxCapturePlanStatus& status) {
            return status.planKey == planKey;
        });
    return it == statuses.end() ? nullptr : &(*it);
}

} // namespace

TEST_CASE("MiniMax live capture reports stored metadata with missing secrets as not ready",
          "[minimax][live_capture]") {
    requireSqlite();

    TempTestDatabase tempDb;
    const MiniMaxSubscriptionCredentialProfile profile = makeSubscriptionProfile("coding_plan");
    seedCredentialMetadata(tempDb.path(), profile);

    MiniMaxLiveCapture liveCapture(makeFakeCredentialStore());
    const std::vector<MiniMaxCapturePlanStatus> statuses =
        liveCapture.inspectStoredPlans(tempDb.path(), false);

    const MiniMaxCapturePlanStatus* codingPlan = findPlanStatus(statuses, "coding_plan");
    REQUIRE(codingPlan != nullptr);
    CHECK(codingPlan->metadataPresent);
    CHECK_FALSE(codingPlan->secretPresent);
    CHECK_FALSE(codingPlan->ready);
    CHECK(codingPlan->validationState == "missing");
    CHECK(codingPlan->secretRef == MiniMaxAdapter::buildSecretRef(profile));
    CHECK(codingPlan->lastValidationError.find("Credential not found:") != std::string::npos);
}

TEST_CASE("MiniMax live capture validates stored credentials through MiniMax adapter hooks",
          "[minimax][live_capture]") {
    requireSqlite();

    TempTestDatabase tempDb;
    const auto credentialStore = makeFakeCredentialStore();
    const MiniMaxSubscriptionCredentialProfile profile = makeSubscriptionProfile("coding_plan");
    seedCredentialMetadata(tempDb.path(), profile);
    seedCredentialSecret(credentialStore, profile, "sk-coding-secret");

    MiniMaxLiveCapture liveCapture(
        credentialStore,
        []() {
            auto adapter = std::make_shared<MiniMaxAdapter>();
            adapter->setMockValidationResponse(true, 200, "{\"model_remains\":[]}");
            return adapter;
        });

    const std::vector<MiniMaxCapturePlanStatus> statuses =
        liveCapture.inspectStoredPlans(tempDb.path(), true);

    const MiniMaxCapturePlanStatus* codingPlan = findPlanStatus(statuses, "coding_plan");
    REQUIRE(codingPlan != nullptr);
    CHECK(codingPlan->metadataPresent);
    CHECK(codingPlan->secretPresent);
    CHECK(codingPlan->ready);
    CHECK(codingPlan->validationState == "valid");
    CHECK(codingPlan->lastValidationError.empty());
    CHECK(codingPlan->lastValidatedAt > 0);
}

TEST_CASE("MiniMax live capture can store a plan secret using persisted secret_ref metadata",
          "[minimax][live_capture]") {
    requireSqlite();

    TempTestDatabase tempDb;
    const auto credentialStore = makeFakeCredentialStore();
    const MiniMaxSubscriptionCredentialProfile profile = makeSubscriptionProfile("token_plan");
    seedCredentialMetadata(tempDb.path(), profile);

    MiniMaxLiveCapture liveCapture(credentialStore);
    std::string error;
    REQUIRE(liveCapture.storePlanSecret(tempDb.path(), "token_plan", "sk-token-secret", error));
    CHECK(error.empty());

    const auto storedSecret =
        credentialStore->readGenericCredential(MiniMaxAdapter::buildSecretRef(profile));
    REQUIRE(storedSecret.has_value());
    CHECK(*storedSecret == "sk-token-secret");
}

TEST_CASE("MiniMax live capture writes successful capture output to JSON",
          "[minimax][live_capture]") {
    requireSqlite();

    TempTestDatabase tempDb;
    TempOutputFile outputFile;
    const auto credentialStore = makeFakeCredentialStore();
    const MiniMaxSubscriptionCredentialProfile profile = makeSubscriptionProfile("coding_plan");
    seedCredentialMetadata(tempDb.path(), profile);
    seedCredentialSecret(credentialStore, profile, "sk-coding-secret");

    const std::string mockQuotaResponse = R"({
        "model_remains": [{
            "start_time": 1735689600000,
            "end_time": 1735707600000,
            "current_interval_total_count": 240,
            "current_interval_usage_count": 223,
            "model_name": "MiniMax-M*"
        }]
    })";

    MiniMaxLiveCapture liveCapture(
        credentialStore,
        [mockQuotaResponse]() {
            auto adapter = std::make_shared<MiniMaxAdapter>();
            adapter->setMockValidationResponse(true, 200, "{\"model_remains\":[]}");
            adapter->setMockQuotaResponse(mockQuotaResponse);
            return adapter;
        });

    const MiniMaxCaptureResult capture = liveCapture.capturePlan(tempDb.path(), "coding_plan", true);
    REQUIRE(capture.success);
    CHECK(capture.error.empty());
    CHECK(capture.planStatus.validationState == "valid");
    CHECK(capture.quota.success);
    CHECK(capture.quota.providerSurface == "minimax_subscription_coding_plan");
    CHECK(capture.quota.used == 17);
    CHECK(capture.quota.limit == 240);
    CHECK(capture.quota.remaining == 223);
    CHECK_FALSE(capture.captureUtc.empty());

    std::string writeError;
    REQUIRE(liveCapture.writeCaptureJson(capture, outputFile.path(), writeError));
    CHECK(writeError.empty());

    const std::string json = outputFile.readAll();
    CHECK(json.find("\"success\": true") != std::string::npos);
    CHECK(json.find("\"plan_key\": \"coding_plan\"") != std::string::npos);
    CHECK(json.find("\"validation_state\": \"valid\"") != std::string::npos);
    CHECK(json.find("\"provider_surface\": \"minimax_subscription_coding_plan\"") != std::string::npos);
    CHECK(json.find("\"used\": 17") != std::string::npos);
    CHECK(json.find("\"remaining\": 223") != std::string::npos);
    CHECK(json.find("MiniMax-M*") != std::string::npos);
}

TEST_CASE("MiniMax live capture creates nested output directories when writing JSON",
          "[minimax][live_capture]") {
    requireSqlite();

    TempTestDatabase tempDb;
    TempOutputDirectory outputDir;
    const auto credentialStore = makeFakeCredentialStore();
    const MiniMaxSubscriptionCredentialProfile profile = makeSubscriptionProfile("coding_plan");
    seedCredentialMetadata(tempDb.path(), profile);
    seedCredentialSecret(credentialStore, profile, "sk-coding-secret");

    const std::string mockQuotaResponse = R"({
        "model_remains": [{
            "start_time": 1735689600000,
            "end_time": 1735707600000,
            "current_interval_total_count": 240,
            "current_interval_usage_count": 223,
            "model_name": "MiniMax-M*"
        }]
    })";

    MiniMaxLiveCapture liveCapture(
        credentialStore,
        [mockQuotaResponse]() {
            auto adapter = std::make_shared<MiniMaxAdapter>();
            adapter->setMockValidationResponse(true, 200, "{\"model_remains\":[]}");
            adapter->setMockQuotaResponse(mockQuotaResponse);
            return adapter;
        });

    const MiniMaxCaptureResult capture = liveCapture.capturePlan(tempDb.path(), "coding_plan", true);
    REQUIRE(capture.success);

    const std::string nestedOutputPath = outputDir.nestedFilePath("coding-plan.json");
    std::string writeError;
    REQUIRE(liveCapture.writeCaptureJson(capture, nestedOutputPath, writeError));
    CHECK(writeError.empty());
    CHECK(std::filesystem::exists(nestedOutputPath));
}

TEST_CASE("MiniMax live capture writes a diagnostic report with plan state and credential targets",
          "[minimax][live_capture]") {
    requireSqlite();

    TempTestDatabase tempDb;
    TempOutputFile outputFile;
    const auto credentialStore = makeFakeCredentialStore();
    const MiniMaxSubscriptionCredentialProfile codingProfile = makeSubscriptionProfile("coding_plan");
    const MiniMaxSubscriptionCredentialProfile tokenProfile = makeSubscriptionProfile("token_plan");
    seedCredentialMetadata(tempDb.path(), codingProfile);
    seedCredentialMetadata(tempDb.path(), tokenProfile);
    seedCredentialSecret(credentialStore, codingProfile, "sk-coding-secret");

    MiniMaxLiveCapture liveCapture(credentialStore);
    const MiniMaxDiagnosticReport report =
        liveCapture.collectDiagnostics(tempDb.path(), false);

    REQUIRE(report.error.empty());
    REQUIRE(report.planStatuses.size() == 2);
    REQUIRE(report.credentialTargets.size() == 1);
    CHECK(report.credentialTargets.front().targetName ==
        MiniMaxAdapter::buildSecretRef(codingProfile));

    std::string writeError;
    REQUIRE(liveCapture.writeDiagnosticJson(report, outputFile.path(), writeError));
    CHECK(writeError.empty());

    const std::string json = outputFile.readAll();
    CHECK(json.find("\"plans\": [") != std::string::npos);
    CHECK(json.find("\"credential_targets\": [") != std::string::npos);
    CHECK(json.find("\"plan_key\": \"coding_plan\"") != std::string::npos);
    CHECK(json.find("\"plan_key\": \"token_plan\"") != std::string::npos);
    CHECK(json.find(MiniMaxAdapter::buildSecretRef(codingProfile)) != std::string::npos);
}

TEST_CASE("MiniMax capture writers keep output outside source trees", "[minimax][live_capture]") {
    const TempTestDirectory temp("jistats_minimax_capture_boundary");
    const auto source = temp.path() / "checkout";
    const auto markers = source / "src/providers/minimax";
    std::filesystem::create_directories(markers);
    REQUIRE(std::ofstream(source / "CMakeLists.txt").is_open());
    REQUIRE(std::ofstream(source / "AGENTS.md").is_open());
    REQUIRE(std::ofstream(markers / "MiniMaxLiveCapture.cpp").is_open());

    MiniMaxLiveCapture capture(makeFakeCredentialStore());
    std::string error;
    const auto privateOutput = source / "captures";
    CHECK_FALSE(capture.writeCaptureJson({}, (privateOutput / "quota.json").string(), error));
    CHECK(error == "Capture output must be outside the JiStats source checkout.");
    CHECK_FALSE(capture.writeDiagnosticJson({}, (privateOutput / "diagnostics.json").string(), error));
    CHECK(error == "Capture output must be outside the JiStats source checkout.");
    CHECK_FALSE(std::filesystem::exists(privateOutput));

    const auto sibling = temp.path() / "checkout-captures";
    REQUIRE(capture.writeCaptureJson({}, (sibling / "quota.json").string(), error));
    REQUIRE(capture.writeDiagnosticJson({}, (sibling / "diagnostics.json").string(), error));
    CHECK(std::filesystem::is_regular_file(sibling / "quota.json"));
    CHECK(std::filesystem::is_regular_file(sibling / "diagnostics.json"));
}

TEST_CASE("MiniMax live capture surfaces database open failures distinctly during readiness inspection",
          "[minimax][live_capture]") {
    requireSqlite();

    const TempTestDirectory directory("jistats_minimax_open_failure");
    const std::string directoryPath = directory.path().string();

    MiniMaxLiveCapture liveCapture(makeFakeCredentialStore());
    const std::vector<MiniMaxCapturePlanStatus> statuses =
        liveCapture.inspectStoredPlans(directoryPath, false);

    REQUIRE(statuses.empty());
    CHECK(liveCapture.getLastError() == "Failed to open database");

    const MiniMaxCaptureResult capture = liveCapture.capturePlan(directoryPath, "coding_plan", false);
    REQUIRE_FALSE(capture.success);
    CHECK(capture.error == "Failed to open database");
}

TEST_CASE("MiniMax live capture surfaces missing provider metadata distinctly",
          "[minimax][live_capture]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));
    const QueryResult deleteResult = storage.execute(
        "DELETE FROM providers WHERE provider_key = 'minimax';");
    REQUIRE(deleteResult.success());
    storage.close();

    MiniMaxLiveCapture liveCapture(makeFakeCredentialStore());
    const std::vector<MiniMaxCapturePlanStatus> statuses =
        liveCapture.inspectStoredPlans(tempDb.path(), false);

    REQUIRE(statuses.empty());
    CHECK(liveCapture.getLastError() ==
        "MiniMax provider metadata was not found in the configured JiStats database");

    const MiniMaxCaptureResult capture = liveCapture.capturePlan(tempDb.path(), "coding_plan", false);
    REQUIRE_FALSE(capture.success);
    CHECK(capture.error ==
        "MiniMax provider metadata was not found in the configured JiStats database");
}
