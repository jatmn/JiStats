#pragma once

#include <string>
#include <memory>
#include <optional>

namespace JiStats {

class StorageService;

class SchemaManager {
public:
    SchemaManager(StorageService& storage);
    ~SchemaManager();
    
    // Check if migrations are needed
    bool needsMigration() const;
    
    // Get current schema version
    int getCurrentVersion() const;
    
    // Get latest available migration version
    int getLatestVersion() const;
    
    // Apply all pending migrations
    bool migrate();

    // Get migration status
    std::string getStatus() const;
    
    // Get last error
    std::string getLastError() const;
    std::optional<int> getRejectedVersion() const;

  private:
    bool prepareBaseline(int& current);
    bool reconcileBaseline();
    bool migrateToVersion(int version);
    std::string loadMigration(int version) const;

    StorageService& storage;
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace JiStats
