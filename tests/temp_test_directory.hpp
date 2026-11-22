#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

namespace JiStats::TestSupport {

class TempTestDirectory {
public:
    explicit TempTestDirectory(const std::string& prefix) {
        const auto root = std::filesystem::temp_directory_path();
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        static std::atomic<unsigned long long> sequence{0};
        do {
            path_ = root / (prefix + "_" + std::to_string(stamp) + "_" +
                            std::to_string(++sequence));
        } while (!std::filesystem::create_directory(path_));
    }

    TempTestDirectory(const TempTestDirectory&) = delete;
    TempTestDirectory& operator=(const TempTestDirectory&) = delete;

    ~TempTestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class TempTestDatabase {
public:
    std::string path() const {
        return (directory_.path() / "test.db").string();
    }

private:
    TempTestDirectory directory_{"jistats_db"};
};

} // namespace JiStats::TestSupport
