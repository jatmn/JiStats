#include "db/SchemaManager.hpp"
#include "db/StorageService.hpp"
#include "BaselineSchema.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace JiStats {

namespace {
constexpr int kBaselineVersion = 12;
}

class SchemaManager::Impl {
public:
    std::string lastError;
    std::string migrationsPath = "db_migrations";
};

SchemaManager::SchemaManager(StorageService& storage) 
    : storage(storage), pImpl(std::make_unique<Impl>()) {
}

SchemaManager::~SchemaManager() = default;

bool SchemaManager::needsMigration() const {
    return getCurrentVersion() < getLatestVersion();
}

int SchemaManager::getCurrentVersion() const {
    // Check if schema_version table exists
    auto result = storage.execute(
        "SELECT MAX(version) FROM schema_version;"
    );
    
    if (result.success() && result.rowCount() > 0) {
        return result.getInt(0, 0);
    }
    
    return 0;
}

int SchemaManager::getLatestVersion() const {
    int latest = kBaselineVersion;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(pImpl->migrationsPath)) {
            const std::string name = entry.path().filename().string();
            if (name.size() >= 4 && name.substr(name.size() - 4) == ".sql") {
                try {
                    int version = std::stoi(name.substr(0, 3));
                    if (version > latest) {
                        latest = version;
                    }
                } catch (...) {}
            }
        }
    } catch (...) {}
    return latest;
}

bool SchemaManager::migrate() {
    const int current = getCurrentVersion();
    if (current != 0 && current < kBaselineVersion) {
        pImpl->lastError = "Database schema version " + std::to_string(current) +
            " predates the supported baseline (version " + std::to_string(kBaselineVersion) + ")";
        return false;
    }

    const int latest = getLatestVersion();
    if (current >= latest) {
        return true;
    }

    const int firstPending = current == 0 ? kBaselineVersion : current + 1;
    for (int version = firstPending; version <= latest; version++) {
        if (!migrateToVersion(version)) {
            return false;
        }
    }
    
    return true;
}

bool SchemaManager::migrateToVersion(int version) {
    const std::string migration = version == kBaselineVersion ? kBaselineSchemaSql : loadMigration(version);
    if (migration.empty()) {
        pImpl->lastError = "Migration file not found for version " + std::to_string(version);
        return false;
    }

    if (!storage.beginTransaction()) {
        pImpl->lastError = "Failed to begin migration transaction";
        return false;
    }

    auto result = storage.execute(migration);
    if (!result.success()) {
        storage.rollbackTransaction();
        pImpl->lastError = "Migration failed: " + result.error();
        return false;
    }

    if (!storage.commitTransaction()) {
        storage.rollbackTransaction();
        pImpl->lastError = "Failed to commit migration transaction";
        return false;
    }

    return true;
}

std::string SchemaManager::getStatus() const {
    std::ostringstream oss;
    oss << "Schema version: " << getCurrentVersion() << "/" << getLatestVersion();
    if (needsMigration()) {
        oss << " (migration needed)";
    } else {
        oss << " (up to date)";
    }
    return oss.str();
}

std::string SchemaManager::getLastError() const {
    return pImpl->lastError;
}

std::string SchemaManager::loadMigration(int version) const {
    // Build the version prefix (e.g. "001")
    std::ostringstream prefixStream;
    prefixStream << std::setfill('0') << std::setw(3) << version;
    const std::string prefix = prefixStream.str();

    // Scan db_migrations/ for a file starting with the version prefix
    try {
        for (const auto& entry : std::filesystem::directory_iterator(pImpl->migrationsPath)) {
            const std::string name = entry.path().filename().string();
            if (name.size() > 4
                && name.substr(0, 3) == prefix
                && name.substr(name.size() - 4) == ".sql") {
                std::ifstream file(entry.path());
                if (!file.is_open()) {
                    pImpl->lastError = "Cannot open migration file: " + entry.path().string();
                    return "";
                }
                std::ostringstream content;
                content << file.rdbuf();
                return content.str();
            }
        }
    } catch (const std::exception& e) {
        pImpl->lastError = std::string("Error scanning migrations: ") + e.what();
        return "";
    }

    pImpl->lastError = "Migration file not found for version " + std::to_string(version);
    return "";
}

} // namespace JiStats
