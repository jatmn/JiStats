#include "db/QueryResult.hpp"
#include <algorithm>

namespace JiStats {

// Impl is now defined as public struct in QueryResult.hpp
QueryResult::QueryResult() : pImpl(std::make_unique<Impl>()) {
}

QueryResult::~QueryResult() = default;

bool QueryResult::success() const {
    return pImpl->success_;
}

std::string QueryResult::error() const {
    return pImpl->error_;
}

size_t QueryResult::rowCount() const {
    return pImpl->data_.size();
}

size_t QueryResult::columnCount() const {
    return pImpl->columnNames_.size();
}

std::vector<std::string> QueryResult::columnNames() const {
    return pImpl->columnNames_;
}

std::string QueryResult::getString(size_t row, size_t col) const {
    if (row >= pImpl->data_.size() || col >= pImpl->data_[row].size()) {
        return "";
    }
    return pImpl->data_[row][col];
}

int QueryResult::getInt(size_t row, size_t col) const {
    return static_cast<int>(getInt64(row, col));
}

long long QueryResult::getInt64(size_t row, size_t col) const {
    if (row >= pImpl->data_.size() || col >= pImpl->data_[row].size()) {
        return 0;
    }
    try {
        return std::stoll(pImpl->data_[row][col]);
    } catch (...) {
        return 0;
    }
}

double QueryResult::getDouble(size_t row, size_t col) const {
    if (row >= pImpl->data_.size() || col >= pImpl->data_[row].size()) {
        return 0.0;
    }
    try {
        return std::stod(pImpl->data_[row][col]);
    } catch (...) {
        return 0.0;
    }
}

bool QueryResult::getBool(size_t row, size_t col) const {
    return getInt(row, col) != 0;
}

bool QueryResult::isNull(size_t row, size_t col) const {
    if (row >= pImpl->nullFlags_.size() || col >= pImpl->nullFlags_[row].size()) {
        return true;
    }
    return pImpl->nullFlags_[row][col];
}

std::map<std::string, std::string> QueryResult::getRowAsMap(size_t row) const {
    std::map<std::string, std::string> result;
    if (row >= pImpl->data_.size()) {
        return result;
    }
    
    const auto& rowData = pImpl->data_[row];
    for (size_t i = 0; i < pImpl->columnNames_.size() && i < rowData.size(); ++i) {
        result[pImpl->columnNames_[i]] = rowData[i];
    }
    return result;
}

long long QueryResult::lastInsertRowId() const {
    return pImpl->lastInsertRowId_;
}

int QueryResult::rowsAffected() const {
    return pImpl->rowsAffected_;
}

} // namespace JiStats