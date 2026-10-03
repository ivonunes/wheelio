#include "utilities.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>

bool Logger::enabled_ = true;
    
void Logger::debug(const std::string& message) {
    if (enabled_) {
        log(LogLevel::Debug, message);
    }
}

void Logger::info(const std::string& message) {
    if (enabled_) {
        log(LogLevel::Info, message);
    }
}

void Logger::warning(const std::string& message) {
    log(LogLevel::Warning, message);
}

void Logger::error(const std::string& message) {
    log(LogLevel::Error, message);
}

void Logger::log(LogLevel level, const std::string& message) {
    std::string prefix;
    int color_code = 0;
    
    switch (level) {
        case LogLevel::Debug:
            prefix = "wheelio::debug";
            color_code = 37; // White
            break;
        case LogLevel::Info:
            prefix = "wheelio::info";
            color_code = 32; // Green
            break;
        case LogLevel::Warning:
            prefix = "wheelio::warning";
            color_code = 33; // Yellow
            break;
        case LogLevel::Error:
            prefix = "wheelio::error";
            color_code = 31; // Red
            break;
    }
    
    print_with_color(prefix, message, color_code);
}

void Logger::print_with_color(const std::string& prefix, const std::string& message, int color_code) {
    printf("\033[1;%dm=== %s \033[0m: %s\n", color_code, prefix.c_str(), message.c_str());
}

bool ErrorHandler::check_io_result(const std::string& operation, IOReturn result) {
    if (result != kIOReturnSuccess) {
        std::string description = get_error_description(result);
        log_error(operation, result, description);
        return false;
    }
    return true;
}

std::string ErrorHandler::get_error_description(IOReturn result) {
    return std::string(mach_error_string(result));
}

void ErrorHandler::log_error(const std::string& operation, IOReturn result, const std::string& description) {
    std::ostringstream oss;
    oss << operation << " failed with error code 0x" << std::hex << result 
        << " (" << description << ")";
    Logger::error(oss.str());
}

namespace utils {
    std::string format_device_id(device_id_t device_id) {
        std::ostringstream oss;
        oss << "0x" << std::hex << std::uppercase << device_id;
        return oss.str();
    }
    
    bool is_valid_device_id(device_id_t device_id) {
        return device_id != 0;
    }
    
    float clamp(float value, float min, float max) {
        return std::max(min, std::min(max, value));
    }
    
    float lerp(float a, float b, float t) {
        return a + t * (b - a);
    }
}
