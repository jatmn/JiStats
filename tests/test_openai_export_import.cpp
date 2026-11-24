#include <catch_amalgamated.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "db/StorageService.hpp"
#include "providers/openai/OpenAIExportImport.hpp"
#include "sqlite_test_support.hpp"
#include "temp_test_directory.hpp"

using namespace JiStats;
using JiStats::TestSupport::TempTestDatabase;
using JiStats::TestSupport::TempTestDirectory;
using JiStats::TestSupport::requireSqlite;

namespace {

const std::filesystem::path kRedactedExportFixtureDir =
    std::filesystem::path("docs/fixtures/openai/chatgpt-export-redacted");
const std::filesystem::path kRedactedConversationsFixturePath =
    kRedactedExportFixtureDir / "conversations.json";

class TempExportDirectory {
public:
    std::filesystem::path makeFile(
        const std::filesystem::path& relativePath,
        const std::string& contents) const {
        const std::filesystem::path filePath = directory_.path() / relativePath;
        std::filesystem::create_directories(filePath.parent_path());
        std::ofstream out(filePath, std::ios::binary);
        if (!out.is_open()) {
            throw std::runtime_error("Failed to create temporary export fixture");
        }

        out << contents;
        return filePath;
    }

    std::filesystem::path path() const {
        return directory_.path();
    }

private:
    TempTestDirectory directory_{"jistats_openai_export"};
};

long long requireSingleId(const QueryResult& result) {
    REQUIRE(result.success());
    REQUIRE(result.rowCount() == 1);
    return result.getInt64(0, 0);
}

int requireImportedAccountId(StorageService& storage, int providerId) {
    const QueryResult result = storage.execute(
        "SELECT id FROM accounts WHERE provider_id = ? AND account_key = ?;",
        {std::to_string(providerId), "imported-history:openai_subscription_export_import"});
    REQUIRE(result.success());
    REQUIRE(result.rowCount() == 1);
    return result.getInt(0, 0);
}

} // namespace

TEST_CASE("OpenAI export import parser accepts the committed redacted export directory",
          "[openai][export_import][parser]") {
    const auto parsed = OpenAIExportImport::parseInputPath(kRedactedExportFixtureDir.string());

    REQUIRE(parsed.success);
    CHECK(parsed.inputType == "directory");
    CHECK(parsed.artifactName == "chatgpt-export-redacted");
    CHECK(parsed.parserVersion == OpenAIExportImport::kParserVersion);
    CHECK(parsed.conversationsPath == kRedactedConversationsFixturePath.string());
    CHECK(parsed.coverageStartAt == 1712365215);
    CHECK(parsed.coverageEndAt == 1712566810);
    REQUIRE(parsed.userMessages.size() == 3);
    CHECK(parsed.userMessages[0].conversationId == "conv_redacted_001");
    CHECK(parsed.userMessages[0].messageId == "msg_user_001");
    CHECK(parsed.userMessages[0].conversationTitle == "Redacted export sample one");
    CHECK(parsed.userMessages[2].conversationId == "conv_redacted_002");
    CHECK(parsed.userMessages[2].messageId == "msg_user_101");
}

TEST_CASE("OpenAI export import parser accepts provenance-checked conversations.json selection",
          "[openai][export_import][parser]") {
    const auto parsed = OpenAIExportImport::parseInputPath(kRedactedConversationsFixturePath.string());

    REQUIRE(parsed.success);
    CHECK(parsed.inputType == "conversations_json");
    CHECK(parsed.artifactName == "chatgpt-export-redacted");
    REQUIRE(parsed.userMessages.size() == 3);
}

TEST_CASE("OpenAI export import parser rejects unsupported or weak-provenance inputs",
          "[openai][export_import][parser]") {
    SECTION("zip archives are deferred in the first slice") {
        TempExportDirectory tempDir;
        const std::filesystem::path zipPath = tempDir.makeFile("chatgpt-export.zip", "not-a-real-zip");

        const auto parsed = OpenAIExportImport::parseInputPath(zipPath.string());
        REQUIRE_FALSE(parsed.success);
        CHECK(parsed.error.find("zip archives are not supported") != std::string::npos);
    }

    SECTION("conversations.json without marker siblings is rejected") {
        TempExportDirectory tempDir;
        const std::filesystem::path conversationsPath = tempDir.makeFile(
            "conversations.json",
            R"json([{"id":"conv-1","mapping":{}}])json");

        const auto parsed = OpenAIExportImport::parseInputPath(conversationsPath.string());
        REQUIRE_FALSE(parsed.success);
        CHECK(parsed.error.find("missing official export marker siblings") != std::string::npos);
    }

    SECTION("malformed export layout fails safely") {
        TempExportDirectory tempDir;
        tempDir.makeFile("chat.html", "<html></html>");
        tempDir.makeFile("message_feedback.json", "[]");
        tempDir.makeFile("conversations.json", R"json({"not":"an-array"})json");

        const auto parsed = OpenAIExportImport::parseInputPath(tempDir.path().string());
        REQUIRE_FALSE(parsed.success);
        CHECK(parsed.error.find("expected top-level array") != std::string::npos);
    }
}

TEST_CASE("OpenAI export import persists historical activity events into the durable storage path",
          "[openai][export_import][storage]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    const auto run = OpenAIExportImport::importHistoricalActivity(
        storage,
        kRedactedExportFixtureDir.string());

    REQUIRE(run.success);
    CHECK(run.error.empty());
    CHECK(run.importRunId == "openai_export_import_v1:chatgpt_export_redacted:1712365215:1712566810:3");
    CHECK(run.insertedCount == 3);
    REQUIRE(run.parsed.success);

    const int providerId = static_cast<int>(requireSingleId(
        storage.execute("SELECT id FROM providers WHERE provider_key = 'openai';")));
    const int accountId = requireImportedAccountId(storage, providerId);

    const auto persisted = storage.getActivityEvents(
        providerId,
        accountId,
        run.parsed.coverageStartAt,
        run.parsed.coverageEndAt);
    REQUIRE(persisted.size() == 3);
    CHECK(persisted[0].activity.providerSurface == OpenAIExportImport::kProviderSurface);
    CHECK(persisted[0].activity.providerKey == "openai");
    CHECK(persisted[0].activity.activityKind == ActivityKind::MessageSend);
    CHECK(persisted[0].activity.sourceType == "imported");
    CHECK(persisted[0].activity.confidence == ActivityConfidence::Imported);
    CHECK(persisted[0].activity.sourceRunId == run.importRunId);
    CHECK(persisted[0].activity.surfaceKey == "chatgpt_export_conversations");
    CHECK(persisted[0].activity.windowTitleHint == "Redacted export sample one");
    CHECK(persisted[0].activity.rawReference == "conversation:conv_redacted_001/message:msg_user_001");

    const auto syncState = storage.getSyncState(
        providerId,
        accountId,
        OpenAIExportImport::kSyncKind);
    REQUIRE(syncState.has_value());
    CHECK(syncState->cursorValue.find("\"source_type\":\"chatgpt_export\"") != std::string::npos);
    CHECK(syncState->cursorValue.find(OpenAIExportImport::kProviderSurface) != std::string::npos);
    CHECK(syncState->cursorValue.find("\"artifact_name\":\"chatgpt-export-redacted\"") != std::string::npos);
    CHECK(syncState->cursorValue.find("\"user_message_count\":3") != std::string::npos);
}

TEST_CASE("OpenAI export import is idempotent for the same artifact run",
          "[openai][export_import][storage]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;

    REQUIRE(storage.init(tempDb.path()));

    const auto firstRun = OpenAIExportImport::importHistoricalActivity(
        storage,
        kRedactedConversationsFixturePath.string());
    REQUIRE(firstRun.success);
    CHECK(firstRun.insertedCount == 3);

    const auto secondRun = OpenAIExportImport::importHistoricalActivity(
        storage,
        kRedactedConversationsFixturePath.string());
    REQUIRE(secondRun.success);
    CHECK(secondRun.importRunId == firstRun.importRunId);
    CHECK(secondRun.insertedCount == 0);
}

TEST_CASE("OpenAI export import adds only new messages from overlapping artifacts",
          "[openai][export_import][storage]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));

    const auto first = OpenAIExportImport::importHistoricalActivity(storage, kRedactedExportFixtureDir.string());
    REQUIRE(first.success);
    REQUIRE(first.insertedCount == 3);

    TempExportDirectory exports;
    const auto renamedFolder = exports.path() / "renamed-export";
    std::filesystem::copy(kRedactedExportFixtureDir, renamedFolder, std::filesystem::copy_options::recursive);
    const auto renamed = OpenAIExportImport::importHistoricalActivity(storage, renamedFolder.string());
    REQUIRE(renamed.success);
    CHECK(renamed.importRunId != first.importRunId);
    CHECK(renamed.insertedCount == 0);

    const auto overlap = exports.makeFile("later-export/conversations.json", R"json([
        {"id":"conv_redacted_001","title":"Renamed conversation","mapping":{
            "existing":{"message":{"id":"msg_user_001","author":{"role":"user"},"create_time":1712624400}},
            "new":{"message":{"id":"msg_user_new","author":{"role":"user"},"create_time":1712624401}}
        }}
    ])json");
    exports.makeFile("later-export/user.json", "{}");
    const auto later = OpenAIExportImport::importHistoricalActivity(storage, overlap.string());
    REQUIRE(later.success);
    REQUIRE(later.parsed.coverageStartAt > first.parsed.coverageEndAt);
    CHECK(later.insertedCount == 1);

    const int providerId =
        static_cast<int>(requireSingleId(storage.execute("SELECT id FROM providers WHERE provider_key = 'openai';")));
    const auto events = storage.getActivityEvents(providerId, requireImportedAccountId(storage, providerId),
                                                  first.parsed.coverageStartAt, later.parsed.coverageEndAt);
    REQUIRE(events.size() == 4);
    CHECK(events.front().activity.sourceRunId == first.importRunId);
    CHECK(events.back().activity.rawReference == "conversation:conv_redacted_001/message:msg_user_new");
    CHECK(events.back().activity.sourceRunId == later.importRunId);
}

TEST_CASE("OpenAI export import retries without partial activity when the sync state write fails",
          "[openai][export_import][storage]") {
    requireSqlite();

    TempTestDatabase tempDb;
    StorageService storage;
    REQUIRE(storage.init(tempDb.path()));
    REQUIRE(storage
                .execute("CREATE TEMP TRIGGER fail_export_cursor BEFORE INSERT ON sync_state "
                         "WHEN NEW.sync_kind = 'openai_export_import_history' "
                         "BEGIN SELECT RAISE(ABORT, 'test sync state failure'); END;")
                .success());

    const auto failed = OpenAIExportImport::importHistoricalActivity(storage, kRedactedExportFixtureDir.string());
    CHECK_FALSE(failed.success);
    CHECK(failed.insertedCount == 0);
    CHECK(requireSingleId(storage.execute("SELECT COUNT(*) FROM activity_events;")) == 0);

    REQUIRE(storage.execute("DROP TRIGGER fail_export_cursor;").success());
    const auto retried = OpenAIExportImport::importHistoricalActivity(storage, kRedactedExportFixtureDir.string());
    REQUIRE(retried.success);
    CHECK(retried.insertedCount == 3);
}
