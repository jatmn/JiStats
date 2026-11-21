#pragma once

#include <string>

namespace JiStats {

enum class LogCategory {
    Startup,
    Sync,
    Retry,
    Offline,
    Reconciliation,
    Mirror,
    Taskbar,
    Db,
    General
};

class Logger {
public:
    static void init(const std::string& logDir = "");
    static void shutdown();

    static void trace(LogCategory cat, const std::string& message);
    static void debug(LogCategory cat, const std::string& message);
    static void info(LogCategory cat, const std::string& message);
    static void warn(LogCategory cat, const std::string& message);
    static void error(LogCategory cat, const std::string& message);
    static void critical(LogCategory cat, const std::string& message);

    static const char* categoryTag(LogCategory cat);
    static bool isInitialized();

private:
    Logger() = delete;
    class Impl;
};

namespace sanitize {
    inline std::string mask(const std::string& value, size_t visibleSuffix = 4) {
        if (value.size() <= visibleSuffix) return std::string(value.size(), '*');
        return std::string(value.size() - visibleSuffix, '*') +
               value.substr(value.size() - visibleSuffix);
    }
}

} // namespace JiStats

#define JISTATS_LOG_TRACE(cat, msg)    ::JiStats::Logger::trace((cat), (msg))
#define JISTATS_LOG_DEBUG(cat, msg)    ::JiStats::Logger::debug((cat), (msg))
#define JISTATS_LOG_INFO(cat, msg)     ::JiStats::Logger::info((cat), (msg))
#define JISTATS_LOG_WARN(cat, msg)     ::JiStats::Logger::warn((cat), (msg))
#define JISTATS_LOG_ERROR(cat, msg)    ::JiStats::Logger::error((cat), (msg))
#define JISTATS_LOG_CRITICAL(cat, msg) ::JiStats::Logger::critical((cat), (msg))
