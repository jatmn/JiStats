// test_minimax_adapter.cpp
// Tests for MiniMaxAdapter live client behavior

#define JISTATS_ENABLE_TEST_HOOKS 1

#include <catch_amalgamated.hpp>
#include <algorithm>
#include <array>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <ctime>

#include "providers/minimax/MiniMaxAdapter.hpp"
#include "platform/WindowsCredentialStore.hpp"

using namespace JiStats;

namespace {

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

void seedSubscriptionSecret(
    const std::shared_ptr<WindowsCredentialStore>& store,
    const MiniMaxSubscriptionCredentialProfile& profile,
    const std::string& secret) {
    WindowsCredentialWriteRequest request;
    request.targetName = MiniMaxAdapter::buildSecretRef(profile);
    request.secret = secret;
    request.userName = profile.profileName.empty() ? "MiniMax Test Profile" : profile.profileName;
    request.comment = "JiStats MiniMax adapter test secret";
    REQUIRE(store->writeGenericCredential(request));
}

MiniMaxSubscriptionCredentialProfile makeSubscriptionProfile(const std::string& planKey) {
    MiniMaxSubscriptionCredentialProfile profile;
    profile.accountKey = planKey + ".default";
    profile.accountDisplayName = "MiniMax Subscription";
    profile.planKey = planKey;
    return profile;
}

} // namespace

TEST_CASE("MiniMax adapter returns provider key and display name", "[minimax]") {
    MiniMaxAdapter adapter;
    REQUIRE(adapter.getProviderKey() == "minimax");
    REQUIRE(adapter.getDisplayName() == "MiniMax");
}

TEST_CASE("MiniMax adapter enabled/disabled state", "[minimax]") {
    MiniMaxAdapter adapter;
    REQUIRE(adapter.isEnabled());
    
    adapter.setEnabled(false);
    REQUIRE_FALSE(adapter.isEnabled());
    
    adapter.setEnabled(true);
    REQUIRE(adapter.isEnabled());
}

TEST_CASE("MiniMax adapter API key setter/getter", "[minimax]") {
    MiniMaxAdapter adapter;
    adapter.setApiKey("test-api-key");
    adapter.setGroupId("test-group-id");
    
    REQUIRE(adapter.getApiKey() == "test-api-key");
    REQUIRE(adapter.getGroupId() == "test-group-id");
}

TEST_CASE("MiniMax adapter validateCredentials with empty key", "[minimax]") {
    MiniMaxAdapter adapter;
    // Empty API key should not validate
    REQUIRE_FALSE(adapter.validateCredentials());
}

TEST_CASE("MiniMax adapter validateCredentials succeeds on explicit success response", "[minimax]") {
    MiniMaxAdapter adapter("test-key");
    adapter.setMockValidationResponse(true, 200, "{\"data\":[]}");

    REQUIRE(adapter.validateCredentials());
}

TEST_CASE("MiniMax adapter validateCredentials rejects transport failure", "[minimax]") {
    MiniMaxAdapter adapter("test-key");
    adapter.setMockValidationResponse(false, 0, "", "connection failed");

    REQUIRE_FALSE(adapter.validateCredentials());
}

TEST_CASE("MiniMax adapter validateCredentials rejects forbidden response", "[minimax]") {
    MiniMaxAdapter adapter("test-key");
    adapter.setMockValidationResponse(false, 403, "", "forbidden");

    REQUIRE_FALSE(adapter.validateCredentials());
}

TEST_CASE("MiniMax adapter builds canonical MiniMax subscription secret refs", "[minimax][credentials]") {
    const MiniMaxSubscriptionCredentialProfile profile = makeSubscriptionProfile("coding_plan");
    REQUIRE(
        MiniMaxAdapter::buildSecretRef(profile) ==
        "JiStats/minimax/subscription/coding_plan/coding_plan.default");
}

TEST_CASE("MiniMax adapter validates subscription credential profiles", "[minimax][credentials]") {
    std::string error;
    REQUIRE(MiniMaxAdapter::validateSubscriptionCredentialProfile(
        makeSubscriptionProfile("token_plan"), error));
    REQUIRE(error.empty());

    MiniMaxSubscriptionCredentialProfile invalid = makeSubscriptionProfile("unsupported");
    REQUIRE_FALSE(MiniMaxAdapter::validateSubscriptionCredentialProfile(invalid, error));
    REQUIRE(error == "MiniMax subscription profile planKey must be coding_plan or token_plan");
}

TEST_CASE("MiniMax adapter stores a subscription credential in the Windows credential store",
    "[minimax][credentials]") {
    MiniMaxAdapter adapter;
    adapter.setCredentialStore(makeFakeCredentialStore());

    MiniMaxSubscriptionCredentialProfile profile = makeSubscriptionProfile("coding_plan");
    REQUIRE(adapter.storeSubscriptionCredential(profile, "sk-cp-secret"));

    const MiniMaxSubscriptionCredentialProfile stored = adapter.getSubscriptionCredentialProfile();
    REQUIRE(stored.planKey == "coding_plan");
    REQUIRE(stored.scopeType == "subscription");
    REQUIRE(stored.scopeIdentifier == "coding_plan.default");
    REQUIRE(stored.planDisplayName == "MiniMax Coding Plan");
    REQUIRE(stored.accountDisplayName == "MiniMax Subscription");
    REQUIRE(stored.endpointFamily == "coding_plan_remains");
    REQUIRE(stored.secretRef == "JiStats/minimax/subscription/coding_plan/coding_plan.default");
    REQUIRE(stored.validationState == "pending");
}

TEST_CASE("MiniMax adapter fetchQuota resolves stored subscription secrets",
    "[minimax][credentials]") {
    MiniMaxAdapter adapter;
    adapter.setCredentialStore(makeFakeCredentialStore());

    MiniMaxSubscriptionCredentialProfile profile = makeSubscriptionProfile("coding_plan");
    REQUIRE(adapter.storeSubscriptionCredential(profile, "sk-cp-secret"));
    adapter.setMockQuotaResponse(R"({
        "model_remains": [{
            "start_time": 1735689600000,
            "end_time": 1735707600000,
            "current_interval_total_count": 240,
            "current_interval_usage_count": 223
        }]
    })");

    const QuotaResponse quota = adapter.fetchQuota();
    REQUIRE(quota.success);
    REQUIRE(quota.used == 17);
    REQUIRE(quota.remaining == 223);
    REQUIRE(adapter.getLastResolvedEndpointForTesting() ==
        "/v1/api/openplatform/coding_plan/remains");
}

TEST_CASE("MiniMax adapter resolves subscription polling config from the credential store",
    "[minimax][credentials]") {
    MiniMaxAdapter adapter;
    adapter.setCredentialStore(makeFakeCredentialStore());

    MiniMaxSubscriptionCredentialProfile profile = makeSubscriptionProfile("token_plan");
    REQUIRE(adapter.storeSubscriptionCredential(profile, "sk-tp-secret"));

    std::string error;
    const auto pollingConfig = adapter.resolveSubscriptionPollingConfig(error);
    REQUIRE(pollingConfig.has_value());
    REQUIRE(error.empty());
    REQUIRE(pollingConfig->profile.planKey == "token_plan");
    REQUIRE(pollingConfig->endpointPath == "/v1/token_plan/remains");
    REQUIRE(pollingConfig->bearerToken == "sk-tp-secret");
}

TEST_CASE("MiniMax adapter validates subscription credentials against the configured remains endpoint",
    "[minimax][credentials]") {
    MiniMaxAdapter adapter;
    adapter.setCredentialStore(makeFakeCredentialStore());

    MiniMaxSubscriptionCredentialProfile profile = makeSubscriptionProfile("token_plan");
    REQUIRE(adapter.storeSubscriptionCredential(profile, "sk-tp-secret"));
    adapter.setMockValidationResponse(true, 200, R"({"base_resp":{"status_code":0},"model_remains":[]})");

    REQUIRE(adapter.validateCredentials());

    const MiniMaxSubscriptionCredentialProfile stored = adapter.getSubscriptionCredentialProfile();
    REQUIRE(stored.validationState == "valid");
    REQUIRE(stored.lastValidatedAt > 0);
    CHECK(stored.lastValidationError.empty());
    REQUIRE(adapter.getLastResolvedEndpointForTesting() == "/v1/token_plan/remains");

    SECTION("provider rejection over HTTP 200 replaces the valid state") {
        adapter.setMockValidationResponse(true, 200,
                                          R"({"base_resp":{"status_code":1004,"status_msg":"authentication failed"}})");
        REQUIRE_FALSE(adapter.validateCredentials());
        const auto rejected = adapter.getSubscriptionCredentialProfile();
        CHECK(rejected.validationState == "invalid");
        CHECK(rejected.lastValidatedAt > 0);
        CHECK(rejected.lastValidationError == "MiniMax subscription validation failed with a provider error");
    }

    SECTION("a missing or malformed provider status cannot validate the key") {
        const auto body = GENERATE(R"({"model_remains":[]})", R"({"base_resp":{"status_code":"0"}})");
        adapter.setMockValidationResponse(true, 200, body);
        REQUIRE_FALSE(adapter.validateCredentials());
        const auto unconfirmed = adapter.getSubscriptionCredentialProfile();
        CHECK(unconfirmed.validationState == "unreachable");
        CHECK(unconfirmed.lastValidationError == "MiniMax subscription validation response has no numeric status code");
    }
}

TEST_CASE("MiniMax adapter prefers stored subscription credentials over legacy API keys for hydrated profiles",
    "[minimax][credentials]") {
    const auto store = makeFakeCredentialStore();
    MiniMaxSubscriptionCredentialProfile profile = makeSubscriptionProfile("coding_plan");
    seedSubscriptionSecret(store, profile, "sk-cp-secret");

    MiniMaxAdapter adapter("legacy-api-key");
    adapter.setCredentialStore(store);
    adapter.setSubscriptionCredentialProfile(profile);

    std::string error;
    const auto pollingConfig = adapter.resolveSubscriptionPollingConfig(error);
    REQUIRE(pollingConfig.has_value());
    REQUIRE(error.empty());
    CHECK(pollingConfig->bearerToken == "sk-cp-secret");
    CHECK(pollingConfig->endpointPath == "/v1/api/openplatform/coding_plan/remains");
}

TEST_CASE("MiniMax adapter does not fall back to legacy API keys when an active subscription profile has no stored secret",
    "[minimax][credentials]") {
    MiniMaxAdapter adapter("legacy-api-key");
    adapter.setCredentialStore(makeFakeCredentialStore());
    adapter.setSubscriptionCredentialProfile(makeSubscriptionProfile("token_plan"));
    adapter.setMockQuotaResponse(R"({
        "model_remains": [{
            "start_time": 1735689600000,
            "end_time": 1735707600000,
            "current_interval_total_count": 240,
            "current_interval_usage_count": 223
        }]
    })");

    const QuotaResponse quota = adapter.fetchQuota();
    REQUIRE_FALSE(quota.success);
    CHECK(quota.providerSurface == "minimax_subscription_token_plan");
    CHECK(quota.error.find("Credential not found: JiStats/minimax/subscription/token_plan/token_plan.default")
        != std::string::npos);
}

TEST_CASE("MiniMax adapter maps wrong-surface validation failures to invalid state",
    "[minimax][credentials]") {
    MiniMaxAdapter adapter;
    adapter.setCredentialStore(makeFakeCredentialStore());

    MiniMaxSubscriptionCredentialProfile profile = makeSubscriptionProfile("token_plan");
    REQUIRE(adapter.storeSubscriptionCredential(profile, "sk-tp-secret"));
    adapter.setMockValidationResponse(false, 404, "", "not found");

    REQUIRE_FALSE(adapter.validateCredentials());

    const MiniMaxSubscriptionCredentialProfile stored = adapter.getSubscriptionCredentialProfile();
    REQUIRE(stored.validationState == "invalid");
    REQUIRE(stored.lastValidationError ==
        "This credential does not match the selected MiniMax subscription surface");
}

TEST_CASE("MiniMax adapter maps provider endpoint outages to unreachable state",
    "[minimax][credentials]") {
    MiniMaxAdapter adapter;
    adapter.setCredentialStore(makeFakeCredentialStore());

    MiniMaxSubscriptionCredentialProfile profile = makeSubscriptionProfile("coding_plan");
    REQUIRE(adapter.storeSubscriptionCredential(profile, "sk-cp-secret"));
    adapter.setMockValidationResponse(false, 503, "", "service unavailable");

    REQUIRE_FALSE(adapter.validateCredentials());

    const MiniMaxSubscriptionCredentialProfile stored = adapter.getSubscriptionCredentialProfile();
    REQUIRE(stored.validationState == "unreachable");
    REQUIRE(stored.lastValidationError == "MiniMax validation endpoint is temporarily unavailable");
}

TEST_CASE("MiniMax adapter preserves network validation failures as unreachable rather than invalid",
    "[minimax][credentials]") {
    MiniMaxAdapter adapter;
    adapter.setCredentialStore(makeFakeCredentialStore());

    MiniMaxSubscriptionCredentialProfile profile = makeSubscriptionProfile("coding_plan");
    REQUIRE(adapter.storeSubscriptionCredential(profile, "sk-cp-secret"));
    adapter.setMockValidationResponse(false, 0, "", "connection failed");

    REQUIRE_FALSE(adapter.validateCredentials());

    const MiniMaxSubscriptionCredentialProfile stored = adapter.getSubscriptionCredentialProfile();
    REQUIRE(stored.validationState == "unreachable");
    REQUIRE(stored.lastValidationError ==
        "Could not reach MiniMax to validate this credential: connection failed");
}

TEST_CASE("MiniMax adapter maps rejected subscription credentials to invalid state",
    "[minimax][credentials]") {
    MiniMaxAdapter adapter;
    adapter.setCredentialStore(makeFakeCredentialStore());

    MiniMaxSubscriptionCredentialProfile profile = makeSubscriptionProfile("coding_plan");
    REQUIRE(adapter.storeSubscriptionCredential(profile, "sk-cp-secret"));
    adapter.setMockValidationResponse(false, 403, "", "forbidden");

    REQUIRE_FALSE(adapter.validateCredentials());

    const MiniMaxSubscriptionCredentialProfile stored = adapter.getSubscriptionCredentialProfile();
    REQUIRE(stored.validationState == "invalid");
    REQUIRE(stored.lastValidationError == "MiniMax subscription key rejected");
}

TEST_CASE("MiniMax adapter fetchQuota with mock response", "[minimax]") {
    MiniMaxAdapter adapter;
    adapter.setApiKey("test-key");
    
    // Set mock response in the MiniMax response format
    const char* mockJson = R"({
        "model_remains": [{
            "start_time": 1735689600000,
            "end_time": 1735707600000,
            "remains_time": 4500000,
            "current_interval_total_count": 240,
            "current_interval_usage_count": 223,
            "model_name": "MiniMax-M*"
        }]
    })";
    
    adapter.setMockQuotaResponse(mockJson);
    
    QuotaResponse quota = adapter.fetchQuota();
    REQUIRE(quota.success);
    REQUIRE(quota.used == 17);
    REQUIRE(quota.limit == 240);
    REQUIRE(quota.remaining == 223);
    REQUIRE(quota.windowType == "interval");
    REQUIRE(quota.windowStart == 1735689600);
    REQUIRE(quota.windowEnd == 1735707600);
}

TEST_CASE("MiniMax quota selects the shared five-hour allowance independent of row order", "[minimax][quota_order]") {
    MiniMaxAdapter adapter;
    adapter.setCredentialStore(makeFakeCredentialStore());
    const auto plan = GENERATE("coding_plan", "token_plan");
    REQUIRE(adapter.storeSubscriptionCredential(makeSubscriptionProfile(plan), "test-key"));

    std::array<std::string_view, 4> rows{{
        R"({"model_name":"speech-hd","start_time":1735689600000,"end_time":1735776000000,
            "current_interval_total_count":0,"current_interval_usage_count":0})",
        R"({"model_name":"MiniMax-M*","start_time":1735689600000,"end_time":1735707600000,
            "current_interval_total_count":100,"current_interval_usage_count":97})",
        R"({"model_name":"MiniMax-M*","start_time":1735689600000,"end_time":1735776000000,
            "current_interval_total_count":500,"current_interval_usage_count":400})",
        R"({"model_name":"coding-plan-vlm","start_time":1735689600000,"end_time":1735707600000,
            "current_interval_total_count":190,"current_interval_usage_count":178})",
    }};
    std::sort(rows.begin(), rows.end());
    do {
        std::string payload = "{\"model_remains\":[";
        payload += rows.front();
        for (auto row = std::next(rows.begin()); row != rows.end(); ++row) {
            payload += ',';
            payload += *row;
        }
        payload += "]}";
        adapter.setMockQuotaResponse(payload);

        const auto quota = adapter.fetchQuota();
        REQUIRE(quota.success);
        CHECK(quota.providerSurface == std::string("minimax_subscription_") + plan);
        CHECK(quota.limit == 100);
        CHECK(quota.remaining == 97);
        CHECK(quota.used == 3);
        CHECK(quota.windowType == "interval");
        CHECK(quota.windowStart == 1735689600);
        CHECK(quota.windowEnd == 1735707600);
    } while (std::next_permutation(rows.begin(), rows.end()));
}

TEST_CASE("MiniMax quota distinguishes an exhausted five-hour allowance from unsupported rows",
          "[minimax][quota_order]") {
    MiniMaxAdapter adapter("test-key");
    SECTION("A positive allowance with zero remaining stays valid") {
        adapter.setMockQuotaResponse(R"({
            "model_remains":[
                {"model_name":"MiniMax-M*","start_time":1735689600000,"end_time":1735707599000,
                 "current_interval_total_count":100,"current_interval_usage_count":0}
            ]
        })");
        const auto quota = adapter.fetchQuota();
        REQUIRE(quota.success);
        CHECK(quota.limit == 100);
        CHECK(quota.remaining == 0);
        CHECK(quota.used == 100);
        CHECK(quota.windowEnd == 1735707599);
    }
    SECTION("An unrelated named model does not establish subscription text quota") {
        adapter.setMockQuotaResponse(R"({
            "model_remains":[
                {"model_name":"speech-hd","start_time":1735689600000,"end_time":1735776000000,
                 "current_interval_total_count":0,"current_interval_usage_count":0}
            ]
        })");
        CHECK_FALSE(adapter.fetchQuota().success);
    }
}

TEST_CASE("MiniMax quota disambiguates shared model windows by their bounds", "[minimax][quota_order]") {
    MiniMaxAdapter adapter("test-key");
    const std::string first = R"({"model_name":"MiniMax-M*","start_time":1735689600000,"end_time":1735707600000,
                                 "current_interval_total_count":100,"current_interval_usage_count":32})";
    std::string second;
    bool expectQuota = false;
    SECTION("The same model's newer window supplies the balance") {
        expectQuota = true;
        second = R"({"model_name":"MiniMax-M*","start_time":1735707600000,"end_time":1735725600000,
                     "current_interval_total_count":100,"current_interval_usage_count":99})";
    }
    SECTION("Conflicting balances for one window leave it unavailable") {
        second = R"({"model_name":"MiniMax-M*","start_time":1735689600000,"end_time":1735707600000,
                     "current_interval_total_count":100,"current_interval_usage_count":99})";
    }
    for (const auto& rows : {first + ',' + second, second + ',' + first}) {
        adapter.setMockQuotaResponse("{\"model_remains\":[" + rows + "]}");
        const auto quota = adapter.fetchQuota();
        if (expectQuota) {
            REQUIRE(quota.success);
            CHECK(quota.remaining == 99);
            CHECK(quota.windowEnd == 1735725600);
        } else {
            CHECK_FALSE(quota.success);
        }
    }
}

TEST_CASE("MiniMax adapter fetchQuota rejects mock payload without model_remains", "[minimax]") {
    MiniMaxAdapter adapter;
    adapter.setApiKey("test-key");
    adapter.setMockQuotaResponse(R"({"base_resp":{"status_code":0}})");

    QuotaResponse quota = adapter.fetchQuota();
    REQUIRE_FALSE(quota.success);
    REQUIRE(quota.error == "No model_remains in response");
}

TEST_CASE("MiniMax adapter fetchQuota rejects null required fields", "[minimax]") {
    MiniMaxAdapter adapter;
    adapter.setApiKey("test-key");
    adapter.setMockQuotaResponse(R"({
        "model_remains": [{
            "start_time": 1735689600000,
            "end_time": 1735707600000,
            "current_interval_total_count": null,
            "current_interval_usage_count": 223
        }]
    })");

    QuotaResponse quota = adapter.fetchQuota();
    REQUIRE_FALSE(quota.success);
    REQUIRE(quota.error == "Invalid numeric field: current_interval_total_count");
}

TEST_CASE("MiniMax adapter fetchUsage returns not implemented error", "[minimax]") {
    MiniMaxAdapter adapter;
    UsageResponse usage = adapter.fetchUsage(0, std::time(nullptr));
    
    REQUIRE_FALSE(usage.success);
    REQUIRE(usage.error ==
        "MiniMax provider-side usage history remains deferred for the current subscription-first baseline");
}

TEST_CASE("MiniMax adapter getCapabilities returns expected list", "[minimax]") {
    MiniMaxAdapter adapter;
    auto caps = adapter.getCapabilities();
    
    REQUIRE(caps.size() == 4);
    
    // Find quota_monitoring capability
    bool foundQuota = false;
    bool foundUsage = false;
    for (const auto& cap : caps) {
        if (cap.name == "quota_monitoring") {
            foundQuota = true;
            REQUIRE(cap.supported);
        }
        if (cap.name == "usage_tracking") {
            foundUsage = true;
            REQUIRE_FALSE(cap.supported); // Not implemented yet
        }
    }
    REQUIRE(foundQuota);
    REQUIRE(foundUsage);
}
