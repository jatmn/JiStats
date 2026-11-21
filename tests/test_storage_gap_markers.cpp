#include <catch_amalgamated.hpp>

#include <filesystem>
#include <string>

#include "analytics/GapClassifier.hpp"
#include "db/StorageService.hpp"

using namespace JiStats;

namespace {

class TempDbFile {
public:
    TempDbFile() {
        path_ = std::filesystem::temp_directory_path() / "jistats_gap_markers_test.db";
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    ~TempDbFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    std::string path() const {
        return path_.string();
    }

private:
    std::filesystem::path path_;
};

long long requireSingleId(const QueryResult& result) {
    REQUIRE(result.success());
    REQUIRE(result.rowCount() == 1);
    return result.getInt64(0, 0);
}

struct StorageIds {
    int providerId = 0;
    int accountId = 0;
};

StorageIds seedProviderAccount(StorageService& storage) {
    StorageIds ids;
    ids.providerId = static_cast<int>(requireSingleId(
        storage.execute("SELECT id FROM providers WHERE provider_key = 'openai';")));

    auto accountInsert = storage.execute(
        "INSERT INTO accounts (provider_id, account_key, display_name, scope_type, scope_identifier) "
        "VALUES (?, ?, ?, ?, ?);",
        {
            std::to_string(ids.providerId),
            "acct-gap-test",
            "Gap Marker Test Account",
            "user",
            "user-gap-test"
        });
    REQUIRE(accountInsert.success());
    ids.accountId = static_cast<int>(accountInsert.lastInsertRowId());
    return ids;
}

} // namespace

TEST_CASE("GapClassifier classifies known-zero reconstructed and unknown periods deterministically", "[gap][classifier]") {
    SECTION("known-zero requires app-running provider confirmation and no local activity") {
        const auto result = GapClassifier::classify(
            {true, false, true, false});
        REQUIRE(result.classification == GapClassification::KnownZero);
        CHECK(result.basisSummary == "App was running and provider confirmed no activity");
    }

    SECTION("provider backfill wins over other evidence") {
        const auto result = GapClassifier::classify(
            {false, true, false, true});
        REQUIRE(result.classification == GapClassification::Reconstructed);
        CHECK(result.basisSummary == "Provider backfill is available for the missing period");
    }

    SECTION("local-only missing period stays unknown") {
        const auto result = GapClassifier::classify(
            {true, false, false, true});
        REQUIRE(result.classification == GapClassification::Unknown);
        CHECK(result.basisSummary == "Local activity exists without provider backfill");
    }

    SECTION("app-not-running missing period stays unknown") {
        const auto result = GapClassifier::classify(
            {false, false, false, false});
        REQUIRE(result.classification == GapClassification::Unknown);
        CHECK(result.basisSummary == "App was not running and no provider backfill is available");
    }

    CHECK(toString(GapClassification::KnownZero) == "known_zero");
    CHECK(toString(GapClassification::Reconstructed) == "reconstructed");
    CHECK(toString(GapClassification::Unknown) == "unknown");
    REQUIRE(gapClassificationFromString("known_zero").value() == GapClassification::KnownZero);
    REQUIRE(gapClassificationFromString("reconstructed").value() == GapClassification::Reconstructed);
    REQUIRE(gapClassificationFromString("unknown").value() == GapClassification::Unknown);
    CHECK_FALSE(gapClassificationFromString("bogus").has_value());
}

TEST_CASE("StorageService upserts and queries gap markers when SQLite is available", "[storage][gap]") {
    TempDbFile tempDb;
    StorageService storage;

    if (!storage.init(tempDb.path())) {
        REQUIRE(storage.getLastError().find("SQLite not available") != std::string::npos);
        return;
    }

    const auto ids = seedProviderAccount(storage);

    PersistedGapMarker marker;
    marker.providerId = ids.providerId;
    marker.accountId = ids.accountId;
    marker.periodStart = 1714000000;
    marker.periodEnd = 1714003600;
    marker.classification = GapClassification::Unknown;
    marker.basisSummary = "Provider did not confirm a known-zero period";
    marker.sourceKind = "runtime_recovery";

    long long rowId = 0;
    REQUIRE(storage.upsertGapMarker(marker, &rowId));
    CHECK(rowId > 0);

    auto markers = storage.getGapMarkers(
        ids.providerId,
        ids.accountId,
        1713999900,
        1714003601);
    REQUIRE(markers.size() == 1);
    CHECK(markers.front().id == rowId);
    CHECK(markers.front().periodStart == 1714000000);
    CHECK(markers.front().periodEnd == 1714003600);
    CHECK(markers.front().classification == GapClassification::Unknown);
    CHECK(markers.front().basisSummary == "Provider did not confirm a known-zero period");
    CHECK(markers.front().sourceKind == "runtime_recovery");
    CHECK_FALSE(markers.front().createdAtUtc.empty());
    CHECK_FALSE(markers.front().updatedAtUtc.empty());

    marker.classification = GapClassification::Reconstructed;
    marker.basisSummary = "Provider backfill is available for the missing period";
    marker.sourceKind = "provider_backfill";
    REQUIRE(storage.upsertGapMarker(marker, &rowId));

    markers = storage.getGapMarkers(
        ids.providerId,
        ids.accountId,
        1714000000,
        1714003601);
    REQUIRE(markers.size() == 1);
    CHECK(markers.front().id == rowId);
    CHECK(markers.front().classification == GapClassification::Reconstructed);
    CHECK(markers.front().basisSummary == "Provider backfill is available for the missing period");
    CHECK(markers.front().sourceKind == "provider_backfill");
}

TEST_CASE("StorageService queries overlapping gap markers and prunes expired rows when SQLite is available", "[storage][gap]") {
    TempDbFile tempDb;
    StorageService storage;

    if (!storage.init(tempDb.path())) {
        REQUIRE(storage.getLastError().find("SQLite not available") != std::string::npos);
        return;
    }

    const auto ids = seedProviderAccount(storage);

    PersistedGapMarker first;
    first.providerId = ids.providerId;
    first.accountId = ids.accountId;
    first.periodStart = 1714100000;
    first.periodEnd = 1714103600;
    first.classification = GapClassification::KnownZero;
    first.basisSummary = "App was running and provider confirmed no activity";
    first.sourceKind = "local_runtime";
    REQUIRE(storage.upsertGapMarker(first));

    PersistedGapMarker second = first;
    second.periodStart = 1714200000;
    second.periodEnd = 1714207200;
    second.classification = GapClassification::Unknown;
    second.basisSummary = "App was not running and no provider backfill is available";
    second.sourceKind = "startup_recovery";
    REQUIRE(storage.upsertGapMarker(second));

    auto markers = storage.getGapMarkers(
        ids.providerId,
        ids.accountId,
        1714101800,
        1714203600);
    REQUIRE(markers.size() == 2);
    CHECK(markers[0].classification == GapClassification::KnownZero);
    CHECK(markers[1].classification == GapClassification::Unknown);

    REQUIRE(storage.deleteGapMarkersOlderThan(1714200000));
    markers = storage.getGapMarkers(
        ids.providerId,
        ids.accountId,
        1714100000,
        1714207201);
    REQUIRE(markers.size() == 1);
    CHECK(markers.front().periodStart == 1714200000);
}

TEST_CASE("StorageService rejects invalid gap marker inputs before insert", "[storage][gap]") {
    TempDbFile tempDb;
    StorageService storage;

    if (!storage.init(tempDb.path())) {
        REQUIRE(storage.getLastError().find("SQLite not available") != std::string::npos);
        return;
    }

    PersistedGapMarker invalid;
    invalid.providerId = 1;
    invalid.accountId = 1;
    invalid.periodStart = 1714300000;
    invalid.periodEnd = 1714300000;
    REQUIRE_FALSE(storage.upsertGapMarker(invalid));
    CHECK(storage.getLastError() == "Gap marker requires periodEnd after periodStart");

    invalid.periodEnd = 1714303600;
    invalid.providerId = 0;
    REQUIRE_FALSE(storage.upsertGapMarker(invalid));
    CHECK(storage.getLastError() == "Gap marker requires providerId and accountId");

    REQUIRE_FALSE(storage.deleteGapMarkersOlderThan(0));
    CHECK(storage.getLastError() == "Gap-marker retention cutoff must be positive");
}
