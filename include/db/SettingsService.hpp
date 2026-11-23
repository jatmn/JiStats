#pragma once

#include <string>
#include <optional>
#include <utility>
#include <vector>

namespace JiStats {

class StorageService;

class SettingsService {
public:
    explicit SettingsService(StorageService& storage);
    ~SettingsService();

    // Boolean settings
    std::optional<bool> getBool(const std::string& key) const;
    bool setBool(const std::string& key, bool value);

    // Integer settings
    std::optional<int> getInt(const std::string& key) const;
    bool setInt(const std::string& key, int value);

    // String settings
    std::optional<std::string> getString(const std::string& key) const;
    bool setString(const std::string& key, const std::string& value);

    using StringValues = std::vector<std::pair<std::string, std::string>>;
    bool setStrings(const StringValues& values);

    // Check if a setting exists
    bool exists(const std::string& key) const;

    // Delete a setting
    bool remove(const std::string& key);

private:
    StorageService& storage_;
};

} // namespace JiStats
