// The Steam client build (e.g. 1792618285) installed next to a steamclient.dylib.
#pragma once

#include <stdbool.h>
#include <stddef.h>

// Reads the highest "version" among package/steam_client*.manifest in the
// dylib's directory. Returns false if there is none.
bool steam_build_version(const char *steamclient_path, char *out, size_t size);
