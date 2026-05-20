#include "led/common/logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace led {

namespace {

const char* levelName(LogLevel level) {
    switch (level) {
    case LogLevel::debug:
        return "DEBUG";
    case LogLevel::info:
        return "INFO";
    case LogLevel::warn:
        return "WARN";
    case LogLevel::error:
        return "ERROR";
    }
    return "UNKNOWN";
}

std::string currentTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);

    std::tm localTime{};
#if defined(_WIN32)
    localtime_s(&localTime, &time);
#else
    localtime_r(&time, &localTime);
#endif

    std::ostringstream stream;
    stream << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return stream.str();
}

}  // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::setLevel(LogLevel level) {
    std::lock_guard lock(mutex_);
    level_ = level;
}

void Logger::write(LogLevel level, const std::string& message) {
    if (static_cast<int>(level) < static_cast<int>(level_)) {
        return;
    }

    std::lock_guard lock(mutex_);
    std::clog << currentTimestamp() << " [" << levelName(level) << "] " << message << '\n';
}

void logDebug(const std::string& message) {
    Logger::instance().write(LogLevel::debug, message);
}

void logInfo(const std::string& message) {
    Logger::instance().write(LogLevel::info, message);
}

void logWarn(const std::string& message) {
    Logger::instance().write(LogLevel::warn, message);
}

void logError(const std::string& message) {
    Logger::instance().write(LogLevel::error, message);
}

}  // namespace led
