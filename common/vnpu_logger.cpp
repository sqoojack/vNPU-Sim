#include "vnpu_logger.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>

void Logger::log(LogLevel level, const std::string& msg, const std::string& log_file_name, const std::string& tag) {
    static std::mutex log_mutex;
    std::lock_guard<std::mutex> lock(log_mutex);

    std::ofstream log_file(log_file_name, std::ios::app);
    
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::string label;
    switch (level) {
        case LogLevel::INFO:  label = "[INFO]"; break;
        case LogLevel::WARN:  label = "[WARN]"; break;
        case LogLevel::ERROR: label = "[ERROR]"; break;
        case LogLevel::FATAL: label = "[FATAL]"; break;
    }

    std::stringstream ss;
    ss << std::put_time(std::localtime(&now), "%Y-%m-%d %H:%M:%S") 
       << " " << label << " [" << tag << "] " << msg << std::endl;
    
    if (log_file.is_open()) log_file << ss.str();
    std::cout << ss.str(); 
}