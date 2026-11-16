// ~/Library/Application Support/Wheelio/Steam/status.json, which the app reads.
#pragma once

#include <stdbool.h>

// Replaces the status file with text, creating its folder if needed.
bool status_file_write(const char *text);
