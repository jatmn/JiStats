#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>

namespace JiStats {

class QueryResult {
public:
    QueryResult();
    QueryResult(const QueryResult&) = delete;
    QueryResult& operator=(const QueryResult&) = delete;
    QueryResult(QueryResult&&) noexcept = default;
    QueryResult& operator=(QueryResult&&) noexcept = default;
    ~QueryResult();
    
    // Check if query was successful
    bool success() const;
    
    // Get error message if any
    std::string error() const;
    
    // Get number of rows
    size_t rowCount() const;
    
    // Get number of columns
    size_t columnCount() const;
    
    // Get column names
    std::vector<std::string> columnNames() const;
    
    // Get value at row, column
    std::string getString(size_t row, size_t col) const;
    int getInt(size_t row, size_t col) const;
    long long getInt64(size_t row, size_t col) const;
    double getDouble(size_t row, size_t col) const;
    bool getBool(size_t row, size_t col) const;
    
    // Check if value is NULL
    bool isNull(size_t row, size_t col) const;
    
    // Get row as map
    std::map<std::string, std::string> getRowAsMap(size_t row) const;
    
    // Get last insert rowid
    long long lastInsertRowId() const;
    
    // Get rows affected
    int rowsAffected() const;
    
public:
    // For internal use by StorageService
    struct Impl {
        bool success_ = false;
        std::string error_;
        std::vector<std::string> columnNames_;
        std::vector<std::vector<std::string>> data_;
        std::vector<std::vector<bool>> nullFlags_;
        long long lastInsertRowId_ = 0;
        int rowsAffected_ = 0;
    };
    
private:
    friend class StorageService;
    std::unique_ptr<Impl> pImpl;
};

} // namespace JiStats