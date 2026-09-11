/*
 * TestHub - 日志
 * 线程安全、分级、可选文件输出
 */

#pragma once

#include "time_util.h"

#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace testhub {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 4 };

inline LogLevel logLevelFromString(const std::string& s) {
    if (s == "debug") return LogLevel::Debug;
    if (s == "info") return LogLevel::Info;
    if (s == "warn" || s == "warning") return LogLevel::Warn;
    if (s == "error") return LogLevel::Error;
    if (s == "off" || s == "none") return LogLevel::Off;
    return LogLevel::Info;
}

inline const char* logLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Off: return "OFF";
    }
    return "?";
}

class Logger {
public:
    static Logger& getInstance() {
        static Logger instance;
        return instance;
    }

    void setLevel(LogLevel level) {
        std::lock_guard<std::mutex> lock(mutex_);
        level_ = level;
    }
    LogLevel getLevel() const { return level_; }

    bool setFile(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        file_.close();
        if (path.empty()) return true;
        file_.open(path, std::ios::app);
        return file_.is_open();
    }

    void setConsole(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        console_ = enabled;
    }

    void log(LogLevel level, const std::string& component, const std::string& message) {
        if (level < level_) return;
        std::ostringstream line;
        line << TimeUtil::toIso8601(TimeUtil::now()) << " [" << logLevelName(level) << "] "
             << "[" << component << "] " << message;
        std::lock_guard<std::mutex> lock(mutex_);
        if (console_) {
            std::ostream& os = (level >= LogLevel::Warn) ? std::cerr : std::cout;
            os << line.str() << std::endl;
        }
        if (file_.is_open()) {
            file_ << line.str() << '\n';
            file_.flush();
        }
    }

    void debug(const std::string& component, const std::string& msg) { log(LogLevel::Debug, component, msg); }
    void info(const std::string& component, const std::string& msg) { log(LogLevel::Info, component, msg); }
    void warn(const std::string& component, const std::string& msg) { log(LogLevel::Warn, component, msg); }
    void error(const std::string& component, const std::string& msg) { log(LogLevel::Error, component, msg); }

private:
    Logger() = default;
    LogLevel level_ = LogLevel::Info;
    bool console_ = true;
    std::ofstream file_;
    std::mutex mutex_;
};

#define TH_LOG_DEBUG(component, msg) ::testhub::Logger::getInstance().debug(component, msg)
#define TH_LOG_INFO(component, msg) ::testhub::Logger::getInstance().info(component, msg)
#define TH_LOG_WARN(component, msg) ::testhub::Logger::getInstance().warn(component, msg)
#define TH_LOG_ERROR(component, msg) ::testhub::Logger::getInstance().error(component, msg)

} // namespace testhub
