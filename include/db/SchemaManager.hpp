#pragma once

#include <string>
#include <memory>

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
    
    // Apply specific migration
    bool migrateToVersion(int version);
    
    // Get migration status
    std::string getStatus() const;
    
    // Get last error
    std::string getLastError() const;
    
private:
    // Load migration from file
    std::string loadMigration(int version) const;
    std::string buildRecoveryMigration(int version) const;
    bool tableExists(const std::string& tableName) const;
    bool columnExists(const std::string& tableName, const std::string& columnName) const;
    
    StorageService& storage;
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace JiStats
