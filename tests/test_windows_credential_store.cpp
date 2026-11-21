#define JISTATS_ENABLE_TEST_HOOKS 1

#include <catch_amalgamated.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
        lastTargetName_ = request.targetName;
        lastUserName_ = request.userName;
        lastComment_ = request.comment;
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
            if (targetName == lastTargetName_) {
                summary.userName = lastUserName_;
                summary.comment = lastComment_;
            }
            summaries.push_back(std::move(summary));
        }
        error.clear();
        return summaries;
    }

    std::string getLastUserName() const {
        return lastUserName_;
    }

    std::string getLastComment() const {
        return lastComment_;
    }

private:
    std::map<std::string, std::string> secrets_;
    std::string lastTargetName_;
    std::string lastUserName_;
    std::string lastComment_;
};

} // namespace

TEST_CASE("WindowsCredentialStore writes reads and deletes secrets through the backend",
    "[credential_store]") {
    auto backend = std::make_shared<FakeWindowsCredentialBackend>();
    WindowsCredentialStore store(backend);

    WindowsCredentialWriteRequest request;
    request.targetName = "JiStats/minimax/subscription/coding_plan/default";
    request.secret = "sk-cp-secret";
    request.userName = "MiniMax Coding Plan";
    request.comment = "JiStats test credential";

    REQUIRE(store.writeGenericCredential(request));
    REQUIRE(backend->getLastUserName() == "MiniMax Coding Plan");
    REQUIRE(backend->getLastComment() == "JiStats test credential");

    const auto secret = store.readGenericCredential(request.targetName);
    REQUIRE(secret.has_value());
    REQUIRE(*secret == "sk-cp-secret");

    REQUIRE(store.deleteGenericCredential(request.targetName));
    REQUIRE_FALSE(store.readGenericCredential(request.targetName).has_value());
    REQUIRE(store.getLastError() == "Credential not found: " + request.targetName);
}

TEST_CASE("WindowsCredentialStore rejects empty target names and secrets", "[credential_store]") {
    auto backend = std::make_shared<FakeWindowsCredentialBackend>();
    WindowsCredentialStore store(backend);

    WindowsCredentialWriteRequest missingTarget;
    missingTarget.secret = "sk-cp-secret";
    REQUIRE_FALSE(store.writeGenericCredential(missingTarget));
    REQUIRE(store.getLastError() == "Credential target name is required");

    WindowsCredentialWriteRequest missingSecret;
    missingSecret.targetName = "JiStats/minimax/subscription/token_plan/default";
    REQUIRE_FALSE(store.writeGenericCredential(missingSecret));
    REQUIRE(store.getLastError() == "Credential secret is required");
}

TEST_CASE("WindowsCredentialStore lists generic credentials by prefix", "[credential_store]") {
    auto backend = std::make_shared<FakeWindowsCredentialBackend>();
    WindowsCredentialStore store(backend);

    WindowsCredentialWriteRequest codingPlan;
    codingPlan.targetName = "JiStats/minimax/subscription/coding_plan/coding_plan.default";
    codingPlan.secret = "sk-coding";
    codingPlan.userName = "MiniMax Coding Plan";
    REQUIRE(store.writeGenericCredential(codingPlan));

    WindowsCredentialWriteRequest unrelated;
    unrelated.targetName = "OtherApp/secret";
    unrelated.secret = "ignore-me";
    unrelated.userName = "Other";
    REQUIRE(store.writeGenericCredential(unrelated));

    const std::vector<WindowsCredentialSummary> summaries =
        store.listGenericCredentials("JiStats/minimax/");
    REQUIRE(summaries.size() == 1);
    CHECK(summaries.front().targetName == codingPlan.targetName);
}
