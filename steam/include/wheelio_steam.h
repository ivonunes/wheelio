// Checks whether Wheelio's Steam Play patch would apply to a Steam build,
// without loading or changing it. Plain C, for the Swift app.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Size of report buffer that always fits.
#define WHEELIO_STEAM_REPORT_SIZE 4096

// Runs the patch's resolver over the steamclient.dylib at path (universal or
// arm64). Returns true when every gate resolves, so the patch would apply.
// When report is not NULL it receives a JSON report in the same format as
// status.json, without "patch_applied".
bool wheelio_steam_check(const char *steamclient_path, char *report, size_t report_size);

#ifdef __cplusplus
}
#endif
