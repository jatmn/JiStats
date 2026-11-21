#pragma once

#include <string>
#include <vector>
#include <map>

namespace JiStats {

/**
 * Lightweight JSON parsing utilities for MiniMax API responses.
 * 
 * Per AGENTS.md, we prefer non-model API endpoints and avoid adding
 * heavy dependencies. This is a simple parser for the specific JSON
 * structures we need to handle.
 */

class JsonValue {
public:
    enum class Type {
        Null,
        String,
        Number,
        Boolean,
        Object,
        Array
    };

    JsonValue() : type(Type::Null) {}
    JsonValue(Type t) : type(t) {}
    JsonValue(const std::string& s) : type(Type::String), strValue(s) {}
    JsonValue(double n) : type(Type::Number), numValue(n) {}
    JsonValue(bool b) : type(Type::Boolean), boolValue(b) {}

    Type getType() const { return type; }

    // Get string value
    const std::string& getString() const { return strValue; }
    std::string& getString() { return strValue; }

    // Get number value
    double getNumber() const { return numValue; }

    // Get boolean value
    bool getBoolean() const { return boolValue; }

    // Get object map
    const std::map<std::string, JsonValue>& getObject() const { return objValue; }
    std::map<std::string, JsonValue>& getObject() { return objValue; }

    // Get array
    const std::vector<JsonValue>& getArray() const { return arrValue; }
    std::vector<JsonValue>& getArray() { return arrValue; }

    // Check if null
    bool isNull() const { return type == Type::Null; }
    bool isString() const { return type == Type::String; }
    bool isNumber() const { return type == Type::Number; }
    bool isBoolean() const { return type == Type::Boolean; }
    bool isObject() const { return type == Type::Object; }
    bool isArray() const { return type == Type::Array; }

    // Get value by key (for objects)
    const JsonValue* getMember(const std::string& key) const {
        auto it = objValue.find(key);
        if (it != objValue.end()) {
            return &it->second;
        }
        return nullptr;
    }

private:
    Type type = Type::Null;
    std::string strValue;
    double numValue = 0.0;
    bool boolValue = false;
    std::map<std::string, JsonValue> objValue;
    std::vector<JsonValue> arrValue;
};

/**
 * Simple JSON parser for parsing MiniMax API responses.
 * Supports only the subset of JSON we need: objects, arrays, strings, numbers, booleans.
 */
class JsonParser {
public:
    /**
     * Parse a JSON string into a JsonValue.
     * Returns true on success, false on parse error.
     */
    static bool parse(const std::string& json, JsonValue& result);

    /**
     * Extract a long long from a JsonValue (for millisecond timestamps).
     */
    static long long getLongLong(const JsonValue& value, long long defaultValue = 0);

    /**
     * Extract an integer from a JsonValue.
     */
    static int getInt(const JsonValue& value, int defaultValue = 0);

    /**
     * Extract a string from a JsonValue.
     */
    static std::string getString(const JsonValue& value, const std::string& defaultValue = "");

private:
    static bool skipWhitespace(const std::string& json, size_t& pos);
    static bool parseValue(const std::string& json, size_t& pos, JsonValue& result);
    static bool parseString(const std::string& json, size_t& pos, std::string& result);
    static bool parseNumber(const std::string& json, size_t& pos, JsonValue& result);
    static bool parseObject(const std::string& json, size_t& pos, JsonValue& result);
    static bool parseArray(const std::string& json, size_t& pos, JsonValue& result);
    static bool parseLiteral(const std::string& json, size_t& pos, const std::string& literal);
};

} // namespace JiStats