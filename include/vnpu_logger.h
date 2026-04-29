#ifndef VNPU_LOGGER_H
#define VNPU_LOGGER_H

#include <string>
#include <mutex>

enum class LogLevel { INFO, WARN, ERROR, FATAL };

class Logger {
public:
    // Add log_file_name parameter to allow different processes to write to their respective files
    // Add tag parameter to easily label the source in logs (e.g., [Firmware] or [Driver])
    static void log(LogLevel level, const std::string& msg, const std::string& log_file_name, const std::string& tag);
};

// Simplify the calling process using macros
#define LOG_INFO(msg, file, tag)  Logger::log(LogLevel::INFO, msg, file, tag)
#define LOG_WARN(msg, file, tag)  Logger::log(LogLevel::WARN, msg, file, tag)
#define LOG_ERROR(msg, file, tag) Logger::log(LogLevel::ERROR, msg, file, tag)
#define LOG_FATAL(msg, file, tag) Logger::log(LogLevel::FATAL, msg, file, tag)

#endif