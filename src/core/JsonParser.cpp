#include "core/JsonParser.hpp"
#include <cstring>
#include <cstdlib>

namespace JiStats {

namespace {

bool parseHexDigit(char hexChar, unsigned int& value) {
    if (hexChar >= '0' && hexChar <= '9') {
        value = static_cast<unsigned int>(hexChar - '0');
        return true;
    }
    if (hexChar >= 'a' && hexChar <= 'f') {
        value = static_cast<unsigned int>(hexChar - 'a' + 10);
        return true;
    }
    if (hexChar >= 'A' && hexChar <= 'F') {
        value = static_cast<unsigned int>(hexChar - 'A' + 10);
        return true;
    }
    return false;
}

bool parseCodeUnit(const std::string& json, size_t hexStart, unsigned int& codeUnit) {
    if (hexStart + 4 > json.size()) {
        return false;
    }

    codeUnit = 0;
    for (size_t i = 0; i < 4; ++i) {
        unsigned int digit = 0;
        if (!parseHexDigit(json[hexStart + i], digit)) {
            return false;
        }
        codeUnit = (codeUnit << 4) | digit;
    }
    return true;
}

void appendUtf8(std::string& result, unsigned int codePoint) {
    if (codePoint < 0x80) {
        result += static_cast<char>(codePoint);
    } else if (codePoint < 0x800) {
        result += static_cast<char>(0xC0 | (codePoint >> 6));
        result += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else if (codePoint < 0x10000) {
        result += static_cast<char>(0xE0 | (codePoint >> 12));
        result += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else {
        result += static_cast<char>(0xF0 | (codePoint >> 18));
        result += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        result += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
}

bool parseUnicodeEscape(const std::string& json, size_t& pos, std::string& result) {
    if (pos >= json.size() || json[pos] != 'u') {
        return false;
    }

    unsigned int codeUnit = 0;
    if (!parseCodeUnit(json, pos + 1, codeUnit)) {
        return false;
    }

    pos += 5;

    if (codeUnit >= 0xD800 && codeUnit <= 0xDBFF) {
        if (pos + 1 >= json.size() || json[pos] != '\\' || json[pos + 1] != 'u') {
            return false;
        }

        unsigned int lowSurrogate = 0;
        if (!parseCodeUnit(json, pos + 2, lowSurrogate)) {
            return false;
        }
        if (lowSurrogate < 0xDC00 || lowSurrogate > 0xDFFF) {
            return false;
        }

        unsigned int codePoint =
            0x10000 + (((codeUnit - 0xD800) << 10) | (lowSurrogate - 0xDC00));
        appendUtf8(result, codePoint);
        pos += 6;
        return true;
    }

    if (codeUnit >= 0xDC00 && codeUnit <= 0xDFFF) {
        return false;
    }

    appendUtf8(result, codeUnit);
    return true;
}

} // namespace

/* static */
bool JsonParser::skipWhitespace(const std::string& json, size_t& pos) {
    while (pos < json.size()) {
        char c = json[pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            pos++;
        } else {
            return true;
        }
    }
    return false;
}

/* static */
bool JsonParser::parse(const std::string& json, JsonValue& result) {
    size_t pos = 0;
    if (!skipWhitespace(json, pos)) {
        return false;
    }
    if (!parseValue(json, pos, result)) {
        return false;
    }
    skipWhitespace(json, pos);
    return pos >= json.size();
}

/* static */
bool JsonParser::parseValue(const std::string& json, size_t& pos, JsonValue& result) {
    skipWhitespace(json, pos);
    if (pos >= json.size()) {
        return false;
    }

    char c = json[pos];
    if (c == '{') {
        return parseObject(json, pos, result);
    } else if (c == '[') {
        return parseArray(json, pos, result);
    } else if (c == '"') {
        std::string str;
        if (!parseString(json, pos, str)) {
            return false;
        }
        result = JsonValue(str);
        return true;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        return parseNumber(json, pos, result);
    } else if (json.compare(pos, 4, "true") == 0) {
        result = JsonValue(true);
        pos += 4;
        return true;
    } else if (json.compare(pos, 5, "false") == 0) {
        result = JsonValue(false);
        pos += 5;
        return true;
    } else if (json.compare(pos, 4, "null") == 0) {
        result = JsonValue();
        pos += 4;
        return true;
    }
    return false;
}

/* static */
bool JsonParser::parseString(const std::string& json, size_t& pos, std::string& result) {
    if (pos >= json.size() || json[pos] != '"') {
        return false;
    }
    pos++; // skip opening quote

    result.clear();
    while (pos < json.size()) {
        char c = json[pos];
        if (c == '"') {
            pos++; // skip closing quote
            return true;
        } else if (c == '\\') {
            pos++;
            if (pos >= json.size()) {
                return false;
            }
            char escaped = json[pos];
            switch (escaped) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case '/': result += '/'; break;
                case 'b': result += '\b'; break;
                case 'f': result += '\f'; break;
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                case 'u': {
                    if (!parseUnicodeEscape(json, pos, result)) {
                        return false;
                    }
                    continue;
                }
                default:
                    result += escaped;
                    break;
            }
            pos++; // advance past the escape character
        } else {
            result += c;
            pos++;
        }
    }
    return false; // unterminated string
}

/* static */
bool JsonParser::parseNumber(const std::string& json, size_t& pos, JsonValue& result) {
    size_t start = pos;
    if (pos < json.size() && json[pos] == '-') {
        pos++;
    }
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        pos++;
    }
    if (pos < json.size() && json[pos] == '.') {
        pos++;
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
            pos++;
        }
    }
    if (pos < json.size() && (json[pos] == 'e' || json[pos] == 'E')) {
        pos++;
        if (pos < json.size() && (json[pos] == '+' || json[pos] == '-')) {
            pos++;
        }
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
            pos++;
        }
    }
    
    std::string numStr = json.substr(start, pos - start);
    if (numStr.empty() || numStr == "-" || numStr == ".") {
        return false;
    }
    
    char* endPtr = nullptr;
    double num = std::strtod(numStr.c_str(), &endPtr);
    if (endPtr == numStr.c_str() + numStr.size()) {
        result = JsonValue(num);
        return true;
    }
    return false;
}

/* static */
bool JsonParser::parseObject(const std::string& json, size_t& pos, JsonValue& result) {
    if (pos >= json.size() || json[pos] != '{') {
        return false;
    }
    pos++; // skip opening brace

    result = JsonValue(JsonValue::Type::Object);

    skipWhitespace(json, pos);
    if (pos < json.size() && json[pos] == '}') {
        pos++; // empty object
        return true;
    }

    while (true) {
        skipWhitespace(json, pos);
        if (pos >= json.size() || json[pos] != '"') {
            return false;
        }

        // Parse key string
        std::string key;
        if (!parseString(json, pos, key)) {
            return false;
        }

        skipWhitespace(json, pos);
        if (pos >= json.size() || json[pos] != ':') {
            return false;
        }
        pos++; // skip colon

        JsonValue value;
        if (!parseValue(json, pos, value)) {
            return false;
        }

        result.getObject()[key] = std::move(value);

        skipWhitespace(json, pos);
        if (pos < json.size() && json[pos] == '}') {
            pos++;
            return true;
        }
        if (pos >= json.size() || json[pos] != ',') {
            return false;
        }
        pos++; // skip comma
    }
}

/* static */
bool JsonParser::parseArray(const std::string& json, size_t& pos, JsonValue& result) {
    if (pos >= json.size() || json[pos] != '[') {
        return false;
    }
    pos++; // skip opening bracket

    result = JsonValue(JsonValue::Type::Array);

    skipWhitespace(json, pos);
    if (pos < json.size() && json[pos] == ']') {
        pos++; // empty array
        return true;
    }

    while (true) {
        JsonValue value;
        if (!parseValue(json, pos, value)) {
            return false;
        }
        result.getArray().push_back(std::move(value));

        skipWhitespace(json, pos);
        if (pos < json.size() && json[pos] == ']') {
            pos++;
            return true;
        }
        if (pos >= json.size() || json[pos] != ',') {
            return false;
        }
        pos++; // skip comma
    }
}

/* static */
bool JsonParser::parseLiteral(const std::string& json, size_t& pos, const std::string& literal) {
    if (json.compare(pos, literal.size(), literal) == 0) {
        pos += literal.size();
        return true;
    }
    return false;
}

/* static */
long long JsonParser::getLongLong(const JsonValue& value, long long defaultValue) {
    if (value.isNumber()) {
        return static_cast<long long>(value.getNumber());
    }
    return defaultValue;
}

/* static */
int JsonParser::getInt(const JsonValue& value, int defaultValue) {
    if (value.isNumber()) {
        return static_cast<int>(value.getNumber());
    }
    return defaultValue;
}

/* static */
std::string JsonParser::getString(const JsonValue& value, const std::string& defaultValue) {
    if (value.isString()) {
        return value.getString();
    }
    return defaultValue;
}

} // namespace JiStats
