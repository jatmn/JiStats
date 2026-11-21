#include <catch_amalgamated.hpp>

#include <string>
#include <vector>

#include "core/JsonParser.hpp"

using namespace JiStats;

TEST_CASE("JsonParser parses empty object", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("{}", result));
    REQUIRE(result.isObject());
    REQUIRE(result.getObject().empty());
}

TEST_CASE("JsonParser parses empty array", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("[]", result));
    REQUIRE(result.isArray());
    REQUIRE(result.getArray().empty());
}

TEST_CASE("JsonParser parses simple string", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("\"hello\"", result));
    REQUIRE(result.isString());
    REQUIRE(result.getString() == "hello");
}

TEST_CASE("JsonParser parses simple number", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("42", result));
    REQUIRE(result.isNumber());
    REQUIRE(result.getNumber() == 42.0);
}

TEST_CASE("JsonParser parses negative number", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("-17", result));
    REQUIRE(result.isNumber());
    REQUIRE(result.getNumber() == -17.0);
}

TEST_CASE("JsonParser parses decimal number", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("3.14159", result));
    REQUIRE(result.isNumber());
    REQUIRE(result.getNumber() == 3.14159);
}

TEST_CASE("JsonParser parses boolean true", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("true", result));
    REQUIRE(result.isBoolean());
    REQUIRE(result.getBoolean() == true);
}

TEST_CASE("JsonParser parses boolean false", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("false", result));
    REQUIRE(result.isBoolean());
    REQUIRE(result.getBoolean() == false);
}

TEST_CASE("JsonParser parses null", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("null", result));
    REQUIRE(result.isNull());
}

TEST_CASE("JsonParser parses object with single member", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse(R"({"name": "test"})", result));
    REQUIRE(result.isObject());
    REQUIRE(result.getObject().size() == 1);
    REQUIRE(JsonParser::getString(*result.getMember("name")) == "test");
}

TEST_CASE("JsonParser parses object with multiple members", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse(R"({"name": "test", "value": 42, "active": true})", result));
    REQUIRE(result.isObject());
    REQUIRE(result.getObject().size() == 3);
    REQUIRE(JsonParser::getString(*result.getMember("name")) == "test");
    REQUIRE(JsonParser::getInt(*result.getMember("value")) == 42);
}

TEST_CASE("JsonParser parses nested object", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse(R"({"outer": {"inner": "value"}})", result));
    REQUIRE(result.isObject());
    auto* outer = result.getMember("outer");
    REQUIRE(outer != nullptr);
    REQUIRE(outer->isObject());
    REQUIRE(JsonParser::getString(*outer->getMember("inner")) == "value");
}

TEST_CASE("JsonParser parses array with numbers", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("[1, 2, 3]", result));
    REQUIRE(result.isArray());
    REQUIRE(result.getArray().size() == 3);
    REQUIRE(result.getArray()[0].getNumber() == 1.0);
    REQUIRE(result.getArray()[1].getNumber() == 2.0);
    REQUIRE(result.getArray()[2].getNumber() == 3.0);
}

TEST_CASE("JsonParser parses array with mixed types", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse(R"([1, "two", true, null])", result));
    REQUIRE(result.isArray());
    REQUIRE(result.getArray().size() == 4);
    REQUIRE(result.getArray()[0].isNumber());
    REQUIRE(result.getArray()[1].isString());
    REQUIRE(result.getArray()[2].isBoolean());
    REQUIRE(result.getArray()[3].isNull());
}

TEST_CASE("JsonParser parses object with array value", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse(R"({"items": [1, 2, 3]})", result));
    REQUIRE(result.isObject());
    auto* items = result.getMember("items");
    REQUIRE(items != nullptr);
    REQUIRE(items->isArray());
    REQUIRE(items->getArray().size() == 3);
}

TEST_CASE("JsonParser parses array with objects", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse(R"([{"id": 1}, {"id": 2}])", result));
    REQUIRE(result.isArray());
    REQUIRE(result.getArray().size() == 2);
    REQUIRE(JsonParser::getInt(*result.getArray()[0].getMember("id")) == 1);
    REQUIRE(JsonParser::getInt(*result.getArray()[1].getMember("id")) == 2);
}

TEST_CASE("JsonParser handles whitespace before content", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("   {}", result));
    REQUIRE(result.isObject());
}

TEST_CASE("JsonParser handles whitespace after content", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("{}   ", result));
    REQUIRE(result.isObject());
}

TEST_CASE("JsonParser handles whitespace between members", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse(R"({"a" : 1 , "b" : 2})", result));
    REQUIRE(result.isObject());
    REQUIRE(result.getObject().size() == 2);
}

TEST_CASE("JsonParser handles escaped characters", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse(R"({"text": "line1\nline2"})", result));
    REQUIRE(result.isObject());
    REQUIRE(result.getMember("text")->getString() == "line1\nline2");
}

TEST_CASE("JsonParser handles escaped quote", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse(R"({"text": "say \"hello\""})", result));
    REQUIRE(result.isObject());
    REQUIRE(result.getMember("text")->getString() == R"(say "hello")");
}

TEST_CASE("JsonParser handles escaped backslash", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse(R"({"path": "C:\\Users\\test"})", result));
    REQUIRE(result.isObject());
    REQUIRE(result.getMember("path")->getString() == "C:\\Users\\test");
}

TEST_CASE("JsonParser handles empty string", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("\"\"", result));
    REQUIRE(result.isString());
    REQUIRE(result.getString().empty());
}

TEST_CASE("JsonParser handles unicode escape", "[json]") {
    JsonValue result;
    // \u0048\u0065\u006c\u006c\u006f = "Hello"
    REQUIRE(JsonParser::parse(R"({"greeting": "\u0048\u0065\u006c\u006c\u006f"})", result));
    REQUIRE(result.isObject());
    REQUIRE(JsonParser::getString(*result.getMember("greeting")) == "Hello");
}

TEST_CASE("JsonParser handles unicode escape for special chars", "[json]") {
    JsonValue result;
    // \u00A9 = copyright symbol, \u00E9 = é, \u4E2D = 中 (Chinese)
    REQUIRE(JsonParser::parse(R"({"symbol": "\u00A9", "accent": "\u00E9", "chinese": "\u4E2D"})", result));
    REQUIRE(result.isObject());
    REQUIRE(JsonParser::getString(*result.getMember("symbol")) == "\xC2\xA9");
    REQUIRE(JsonParser::getString(*result.getMember("accent")) == "\xC3\xA9");
    REQUIRE(JsonParser::getString(*result.getMember("chinese")) == "\xE4\xB8\xAD");
}

TEST_CASE("JsonParser handles escaped backslash followed by u", "[json]") {
    JsonValue result;
    // \u in a string preceded by \\ should be preserved as \\u
    REQUIRE(JsonParser::parse(R"({"path": "\\u0048"})", result));
    REQUIRE(result.isObject());
    REQUIRE(JsonParser::getString(*result.getMember("path")) == "\\u0048");
}

TEST_CASE("JsonParser handles unicode surrogate pair escape", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse(R"({"emoji": "\uD83D\uDE03"})", result));
    REQUIRE(result.isObject());
    REQUIRE(JsonParser::getString(*result.getMember("emoji")) == "\xF0\x9F\x98\x83");
}

TEST_CASE("JsonParser handles mixed BMP and non-BMP unicode escapes", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse(R"({"text": "A\u00A9\uD83D\uDE80Z"})", result));
    REQUIRE(result.isObject());
    REQUIRE(JsonParser::getString(*result.getMember("text")) == std::string("A\xC2\xA9\xF0\x9F\x9A\x80Z"));
}

TEST_CASE("JsonParser rejects truncated unicode escape", "[json]") {
    JsonValue result;
    // \u without 4 hex digits should fail
    REQUIRE(JsonParser::parse(R"({"incomplete": "\u004"})", result) == false);
}

TEST_CASE("JsonParser rejects invalid hex in unicode escape", "[json]") {
    JsonValue result;
    // \uXXXX with non-hex characters should fail
    REQUIRE(JsonParser::parse(R"({"invalid": "\uGGGG"})", result) == false);
}

TEST_CASE("JsonParser rejects unicode escape at end of string", "[json]") {
    JsonValue result;
    // \u at end of string without 4 digits should fail
    REQUIRE(JsonParser::parse(R"({"truncated": "hello\u"})", result) == false);
}

TEST_CASE("JsonParser rejects missing low surrogate after high surrogate", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse(R"({"broken": "\uD83D"})", result) == false);
}

TEST_CASE("JsonParser rejects high surrogate followed by non-surrogate escape", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse(R"({"broken": "\uD83D\u0041"})", result) == false);
}

TEST_CASE("JsonParser rejects standalone low surrogate", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse(R"({"broken": "\uDE03"})", result) == false);
}

TEST_CASE("JsonParser rejects truncated low surrogate after high surrogate", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse(R"({"broken": "\uD83D\uDE0"})", result) == false);
}

TEST_CASE("JsonParser rejects unclosed object", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("{", result) == false);
}

TEST_CASE("JsonParser rejects unclosed array", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("[", result) == false);
}

TEST_CASE("JsonParser rejects unclosed string", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("\"hello", result) == false);
}

TEST_CASE("JsonParser rejects invalid number", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("-", result) == false);
}

TEST_CASE("JsonParser rejects invalid comma placement", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("{,}", result) == false);
}

TEST_CASE("JsonParser rejects missing colon", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse(R"({"key" value})", result) == false);
}

TEST_CASE("JsonParser rejects trailing comma in object", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse(R"({"key": "value",})", result) == false);
}

TEST_CASE("JsonParser rejects trailing comma in array", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("[1, 2,]", result) == false);
}

TEST_CASE("JsonParser handles scientific notation", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("1.5e10", result));
    REQUIRE(result.isNumber());
    REQUIRE(result.getNumber() == 1.5e10);
}

TEST_CASE("JsonParser handles negative scientific notation", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("-2.5E-3", result));
    REQUIRE(result.isNumber());
    REQUIRE(result.getNumber() == -2.5E-3);
}

TEST_CASE("JsonParser rejects invalid literal", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("tru", result) == false);
}

TEST_CASE("JsonParser rejects invalid literal 2", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("falsee", result) == false);
}

TEST_CASE("JsonParser getLongLong converts number correctly", "[json]") {
    JsonValue numVal(123.0);
    REQUIRE(JsonParser::getLongLong(numVal) == 123LL);
}

TEST_CASE("JsonParser getLongLong returns default for non-number", "[json]") {
    JsonValue strVal(std::string("not a number"));
    REQUIRE(JsonParser::getLongLong(strVal, 999LL) == 999LL);
}

TEST_CASE("JsonParser getInt converts number correctly", "[json]") {
    JsonValue numVal(456.0);
    REQUIRE(JsonParser::getInt(numVal) == 456);
}

TEST_CASE("JsonParser getInt returns default for non-number", "[json]") {
    JsonValue boolVal(true);
    REQUIRE(JsonParser::getInt(boolVal, 111) == 111);
}

TEST_CASE("JsonParser getString returns string correctly", "[json]") {
    JsonValue strVal(std::string("hello"));
    REQUIRE(JsonParser::getString(strVal) == "hello");
}

TEST_CASE("JsonParser getString returns default for non-string", "[json]") {
    JsonValue numVal(42.0);
    REQUIRE(JsonParser::getString(numVal, "default") == "default");
}

TEST_CASE("JsonParser getMember returns null for missing key", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse(R"({"a": 1})", result));
    REQUIRE(result.getMember("missing") == nullptr);
}

TEST_CASE("JsonParser parses zero", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("0", result));
    REQUIRE(result.isNumber());
    REQUIRE(result.getNumber() == 0.0);
}

TEST_CASE("JsonParser parses zero with decimal", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("0.0", result));
    REQUIRE(result.isNumber());
    REQUIRE(result.getNumber() == 0.0);
}

TEST_CASE("JsonParser parses large number", "[json]") {
    JsonValue result;
    REQUIRE(JsonParser::parse("123456789012345", result));
    REQUIRE(result.isNumber());
    REQUIRE(result.getNumber() == 123456789012345.0);
}
