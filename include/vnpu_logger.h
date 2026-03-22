#ifndef VNPU_LOGGER_H
#define VNPU_LOGGER_H

#include <string>
#include <mutex>

enum class LogLevel { INFO, WARN, ERROR, FATAL };

class Logger {
public:
    // 增加 log_file_name 參數，讓不同進程寫入各自的檔案
    // 增加 tag 參數，方便在日誌中標註來源（如 [Firmware] 或 [Driver]）
    static void log(LogLevel level, const std::string& msg, const std::string& log_file_name, const std::string& tag);
};

// 透過巨集簡化呼叫過程
#define LOG_INFO(msg, file, tag)  Logger::log(LogLevel::INFO, msg, file, tag)
#define LOG_WARN(msg, file, tag)  Logger::log(LogLevel::WARN, msg, file, tag)
#define LOG_ERROR(msg, file, tag) Logger::log(LogLevel::ERROR, msg, file, tag)
#define LOG_FATAL(msg, file, tag) Logger::log(LogLevel::FATAL, msg, file, tag)

#endif