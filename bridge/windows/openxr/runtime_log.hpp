#pragma once

#include <windows.h>

// Appends to wheelio_openxr.log next to the runtime DLL. Unlike the dinput8
// proxy this always writes: the runtime is still being discovered against the
// game and every call it makes is evidence. Also mirrors to OutputDebugString.
void runtime_log_set_module(HMODULE module);
void runtime_log(const char* message);
void runtime_logf(const char* format, ...);

// Absolute path of the directory holding the DLL, with a trailing backslash.
// Used for the log and the eye-texture dumps. Returns false before DllMain ran.
bool runtime_module_directory(char* buffer, unsigned long buffer_size);
