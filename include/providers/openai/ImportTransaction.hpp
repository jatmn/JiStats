#pragma once

#include "db/StorageService.hpp"

namespace JiStats::OpenAIImportDetail {

class ImportTransaction {
  public:
    explicit ImportTransaction(StorageService& storage) : storage_(storage) {}

    ImportTransaction(const ImportTransaction&) = delete;
    ImportTransaction& operator=(const ImportTransaction&) = delete;

    ~ImportTransaction() {
        if (active_) {
            storage_.rollbackTransaction();
        }
    }

    bool begin() {
        active_ = storage_.beginTransaction(StorageService::TransactionMode::Immediate);
        return active_;
    }

    bool commit() {
        if (active_ && !storage_.commitTransaction()) {
            return false;
        }
        active_ = false;
        return true;
    }

  private:
    StorageService& storage_;
    bool active_ = false;
};

} // namespace JiStats::OpenAIImportDetail
