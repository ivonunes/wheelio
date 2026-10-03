#include "runtime_log.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

HMODULE g_module = nullptr;
CRITICAL_SECTION g_log_lock;
bool g_log_lock_ready = false;

}  // namespace

void runtime_log_set_module(HMODULE module) {
    g_module = module;
    if (module && !g_log_lock_ready) {
        InitializeCriticalSection(&g_log_lock);
        g_log_lock_ready = true;
        // One log per game launch rather than an ever-growing file.
        char path[MAX_PATH] = {0};
        if (runtime_module_directory(path, MAX_PATH)) {
            std::strncat(path, "wheelio_openxr.log", MAX_PATH - std::strlen(path) - 1);
            HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                CloseHandle(file);
            }
        }
    }
}

bool runtime_module_directory(char* buffer, unsigned long buffer_size) {
    if (!g_module || GetModuleFileNameA(g_module, buffer, buffer_size) == 0) {
        return false;
    }
    char* separator = std::strrchr(buffer, '\\');
    if (!separator) {
        separator = std::strrchr(buffer, '/');
    }
    if (!separator) {
        return false;
    }
    separator[1] = '\0';
    return true;
}

void runtime_log(const char* message) {
    char path[MAX_PATH] = {0};
    if (runtime_module_directory(path, MAX_PATH)) {
        std::strncat(path, "wheelio_openxr.log", MAX_PATH - std::strlen(path) - 1);

        if (g_log_lock_ready) {
            EnterCriticalSection(&g_log_lock);
        }
        HANDLE file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            char line[1100] = {0};
            std::snprintf(line, sizeof(line), "[%lu] %s\r\n", GetCurrentThreadId(), message);
            DWORD written = 0;
            WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
            CloseHandle(file);
        }
        if (g_log_lock_ready) {
            LeaveCriticalSection(&g_log_lock);
        }
    }

    OutputDebugStringA(message);
    OutputDebugStringA("\n");
}

void runtime_logf(const char* format, ...) {
    char buffer[1024] = {0};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    runtime_log(buffer);
}
