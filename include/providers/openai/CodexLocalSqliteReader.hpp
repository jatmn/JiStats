#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace JiStats {

struct CodexLocalThreadRow {
    std::string threadId;
    std::string modelProvider;
    std::string model;
    std::string reasoningEffort;
    long long tokensUsed = 0;
    long long createdAtMs = 0;
    long long updatedAtMs = 0;
};

struct CodexLocalSqliteReadResult {
    bool success = false;
    std::string sourcePath;
    std::string sourceIdentity;
    std::string error;
    std::vector<CodexLocalThreadRow> threads;
};

class CodexLocalSqliteReader {
public:
    static std::string resolveDefaultStateDbPath();
    CodexLocalSqliteReadResult readThreads(const std::string& path = {}) const;
};

} // namespace JiStats
