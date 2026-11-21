#include <catch_amalgamated.hpp>
#include "core/Logger.hpp"

using namespace JiStats;

TEST_CASE("Logger::isInitialized before init", "[logger]") {
    // Logger may already be shut down from a previous test; ensure clean state
    if (Logger::isInitialized()) {
        Logger::shutdown();
    }
    REQUIRE_FALSE(Logger::isInitialized());
}

TEST_CASE("Logger::init and shutdown lifecycle", "[logger]") {
    if (Logger::isInitialized()) {
        Logger::shutdown();
    }

    Logger::init("test_logs");
    REQUIRE(Logger::isInitialized());

    Logger::shutdown();
    REQUIRE_FALSE(Logger::isInitialized());
}

TEST_CASE("Logger methods are safe to call before init", "[logger]") {
    if (Logger::isInitialized()) {
        Logger::shutdown();
    }

    // These must not crash
    Logger::trace(LogCategory::General, "trace before init");
    Logger::debug(LogCategory::General, "debug before init");
    Logger::info(LogCategory::General, "info before init");
    Logger::warn(LogCategory::General, "warn before init");
    Logger::error(LogCategory::General, "error before init");
    Logger::critical(LogCategory::General, "critical before init");

    REQUIRE_FALSE(Logger::isInitialized());
}

TEST_CASE("Logger::categoryTag returns correct tags", "[logger]") {
    REQUIRE(std::string(Logger::categoryTag(LogCategory::Startup)) == "[startup]");
    REQUIRE(std::string(Logger::categoryTag(LogCategory::Sync)) == "[sync]");
    REQUIRE(std::string(Logger::categoryTag(LogCategory::Retry)) == "[retry]");
    REQUIRE(std::string(Logger::categoryTag(LogCategory::Offline)) == "[offline]");
    REQUIRE(std::string(Logger::categoryTag(LogCategory::Reconciliation)) == "[reconciliation]");
    REQUIRE(std::string(Logger::categoryTag(LogCategory::Mirror)) == "[mirror]");
    REQUIRE(std::string(Logger::categoryTag(LogCategory::Taskbar)) == "[taskbar]");
    REQUIRE(std::string(Logger::categoryTag(LogCategory::Db)) == "[db]");
    REQUIRE(std::string(Logger::categoryTag(LogCategory::General)) == "[general]");
}

TEST_CASE("sanitize::mask masks all but last N chars", "[sanitize]") {
    REQUIRE(sanitize::mask("sk-abc123xyz") == "********3xyz");
    REQUIRE(sanitize::mask("sk-abc123xyz", 4) == "********3xyz");
}

TEST_CASE("sanitize::mask with short strings", "[sanitize]") {
    REQUIRE(sanitize::mask("abc") == "***");
    REQUIRE(sanitize::mask("abcd") == "****");
    REQUIRE(sanitize::mask("ab", 4) == "**");
}

TEST_CASE("sanitize::mask with empty string", "[sanitize]") {
    REQUIRE(sanitize::mask("") == "");
}

TEST_CASE("sanitize::mask with custom suffix length", "[sanitize]") {
    REQUIRE(sanitize::mask("1234567890", 6) == "****567890");
    REQUIRE(sanitize::mask("1234567890", 0) == "**********");
}
