#include "db/SettingsService.hpp"
#include "db/StorageService.hpp"

#include <sstream>

namespace JiStats {

namespace {

std::string boolToString(bool value) {
    return value ? "true" : "false";
}

bool stringToBool(const std::string& value) {
    return value == "true" || value == "1";
}

} // namespace

SettingsService::SettingsService(StorageService& storage) : storage_(storage) {
}

SettingsService::~SettingsService() = default;

std::optional<bool> SettingsService::getBool(const std::string& key) const {
    auto result = storage_.execute(
        "SELECT value FROM settings WHERE key = ?",
        {key}
    );

    if (result.success() && result.rowCount() > 0) {
        return stringToBool(result.getString(0, 0));
    }
    return std::nullopt;
}

bool SettingsService::setBool(const std::string& key, bool value) {
    auto result = storage_.execute(
        "INSERT OR REPLACE INTO settings (key, value, updated_at_utc) VALUES (?, ?, strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))",
        {key, boolToString(value)}
    );
    return result.success();
}

std::optional<int> SettingsService::getInt(const std::string& key) const {
    auto result = storage_.execute(
        "SELECT value FROM settings WHERE key = ?",
        {key}
    );

    if (result.success() && result.rowCount() > 0) {
        try {
            return result.getInt(0, 0);
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

bool SettingsService::setInt(const std::string& key, int value) {
    auto result = storage_.execute(
        "INSERT OR REPLACE INTO settings (key, value, updated_at_utc) VALUES (?, ?, strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))",
        {key, std::to_string(value)}
    );
    return result.success();
}

std::optional<std::string> SettingsService::getString(const std::string& key) const {
    auto result = storage_.execute(
        "SELECT value FROM settings WHERE key = ?",
        {key}
    );

    if (result.success() && result.rowCount() > 0) {
        return result.getString(0, 0);
    }
    return std::nullopt;
}

bool SettingsService::setString(const std::string& key, const std::string& value) {
    auto result = storage_.execute(
        "INSERT OR REPLACE INTO settings (key, value, updated_at_utc) VALUES (?, ?, strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))",
        {key, value}
    );
    return result.success();
}

bool SettingsService::setStrings(const StringValues& values) {
    if (values.empty()) {
        return true;
    }

    std::string sql = "INSERT OR REPLACE INTO settings (key, value, updated_at_utc) VALUES ";
    std::vector<std::string> parameters;
    parameters.reserve(values.size() * 2);
    for (const auto& [key, value] : values) {
        if (!parameters.empty()) {
            sql += ", ";
        }
        sql += "(?, ?, strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))";
        parameters.push_back(key);
        parameters.push_back(value);
    }

    return storage_.execute(sql, parameters).success();
}

bool SettingsService::exists(const std::string& key) const {
    auto result = storage_.execute(
        "SELECT 1 FROM settings WHERE key = ? LIMIT 1",
        {key}
    );
    return result.success() && result.rowCount() > 0;
}

bool SettingsService::remove(const std::string& key) {
    auto result = storage_.execute(
        "DELETE FROM settings WHERE key = ?",
        {key}
    );
    return result.success();
}

} // namespace JiStats
