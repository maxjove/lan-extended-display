#pragma once

#include <mutex>
#include <string>

namespace led {

enum class LogLevel {
    debug,
    info,
    warn,
    error
};

class Logger {
public:
    static Logger& instance();

    void setLevel(LogLevel level);
    void write(LogLevel level, const std::string& message);

private:
    Logger() = default;

    std::mutex mutex_;
    LogLevel level_{LogLevel::info};
};

void logDebug(const std::string& message);
void logInfo(const std::string& message);
void logWarn(const std::string& message);
void logError(const std::string& message);

}  // namespace led
