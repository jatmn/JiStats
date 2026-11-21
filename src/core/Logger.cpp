#include "core/Logger.hpp"

#ifdef JISTATS_HAS_SPDLOG
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#endif

#include <filesystem>
#include <cstdlib>

namespace JiStats {

static constexpr size_t MAX_FILE_SIZE = 5 * 1024 * 1024; // 5 MB
static constexpr size_t MAX_FILES = 3;
static const char* LOG_FILENAME = "jistats.log";

class Logger::Impl {
public:
#ifdef JISTATS_HAS_SPDLOG
    std::shared_ptr<spdlog::logger> logger;
#endif
    bool initialized = false;

    static Impl& instance() {
        static Impl inst;
        return inst;
    }
};

static std::string resolveLogDir(const std::string& logDir) {
    if (!logDir.empty()) return logDir;

#ifdef _WIN32
    char* localAppData = nullptr;
    size_t len = 0;
    if (_dupenv_s(&localAppData, &len, "LOCALAPPDATA") == 0 && localAppData) {
        std::string dir = std::string(localAppData) + "\\JiStats\\logs";
        free(localAppData);
        return dir;
    }
#endif
    return "logs";
}

void Logger::init(const std::string& logDir) {
    auto& impl = Impl::instance();
    if (impl.initialized) return;

#ifdef JISTATS_HAS_SPDLOG
    try {
        std::string dir = resolveLogDir(logDir);
        std::filesystem::create_directories(dir);

        std::string logPath = (std::filesystem::path(dir) / LOG_FILENAME).string();

        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            logPath, MAX_FILE_SIZE, MAX_FILES));

#ifndef NDEBUG
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
#endif

        impl.logger = std::make_shared<spdlog::logger>("jistats", sinks.begin(), sinks.end());
        impl.logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        impl.logger->set_level(spdlog::level::trace);
        impl.logger->flush_on(spdlog::level::warn);

        spdlog::set_default_logger(impl.logger);
        impl.initialized = true;
    } catch (...) {
        // Logging init failure must not crash the app
    }
#endif
}

void Logger::shutdown() {
    auto& impl = Impl::instance();
    if (!impl.initialized) return;

#ifdef JISTATS_HAS_SPDLOG
    impl.logger->flush();
    spdlog::shutdown();
    impl.logger.reset();
#endif
    impl.initialized = false;
}

bool Logger::isInitialized() {
    return Impl::instance().initialized;
}

const char* Logger::categoryTag(LogCategory cat) {
    switch (cat) {
        case LogCategory::Startup:        return "[startup]";
        case LogCategory::Sync:           return "[sync]";
        case LogCategory::Retry:          return "[retry]";
        case LogCategory::Offline:        return "[offline]";
        case LogCategory::Reconciliation: return "[reconciliation]";
        case LogCategory::Mirror:         return "[mirror]";
        case LogCategory::Taskbar:        return "[taskbar]";
        case LogCategory::Db:             return "[db]";
        case LogCategory::General:        return "[general]";
    }
    return "[unknown]";
}

void Logger::trace(LogCategory cat, const std::string& message) {
#ifdef JISTATS_HAS_SPDLOG
    auto& impl = Impl::instance();
    if (!impl.initialized) return;
    impl.logger->trace("{} {}", categoryTag(cat), message);
#endif
}

void Logger::debug(LogCategory cat, const std::string& message) {
#ifdef JISTATS_HAS_SPDLOG
    auto& impl = Impl::instance();
    if (!impl.initialized) return;
    impl.logger->debug("{} {}", categoryTag(cat), message);
#endif
}

void Logger::info(LogCategory cat, const std::string& message) {
#ifdef JISTATS_HAS_SPDLOG
    auto& impl = Impl::instance();
    if (!impl.initialized) return;
    impl.logger->info("{} {}", categoryTag(cat), message);
#endif
}

void Logger::warn(LogCategory cat, const std::string& message) {
#ifdef JISTATS_HAS_SPDLOG
    auto& impl = Impl::instance();
    if (!impl.initialized) return;
    impl.logger->warn("{} {}", categoryTag(cat), message);
#endif
}

void Logger::error(LogCategory cat, const std::string& message) {
#ifdef JISTATS_HAS_SPDLOG
    auto& impl = Impl::instance();
    if (!impl.initialized) return;
    impl.logger->error("{} {}", categoryTag(cat), message);
#endif
}

void Logger::critical(LogCategory cat, const std::string& message) {
#ifdef JISTATS_HAS_SPDLOG
    auto& impl = Impl::instance();
    if (!impl.initialized) return;
    impl.logger->critical("{} {}", categoryTag(cat), message);
#endif
}

} // namespace JiStats
