#pragma once

#include <ctime>
#include <string>

namespace JiStats {

class StorageService;
class CodexLocalSqliteReader;

struct CodexLocalUsageImportResult {
    bool success = false;
    std::string sourcePath;
    std::string error;
    int rowsRead = 0;
    int baselinedThreads = 0;
    int deltaEventsInserted = 0;
    int resetThreads = 0;
    int skippedRows = 0;
};

class CodexLocalUsageImporter {
public:
    explicit CodexLocalUsageImporter(StorageService& storage);
    CodexLocalUsageImportResult importFromDefaultSource(std::time_t now = 0);
    CodexLocalUsageImportResult importFromSource(const std::string& sourcePath, std::time_t now = 0);

private:
    StorageService& storage_;
};

} // namespace JiStats
