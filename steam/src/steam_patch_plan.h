// Works out, from one steamclient.dylib, exactly what the patch would change,
// and whether it can. The same plan serves the patch inside Steam and the
// app's check of a Steam build on disk.
#pragma once

#include "anchor_resolver.h"
#include "macho_image.h"
#include "steam_anchors.h"

#include <stdbool.h>
#include <stdint.h>

// The patch wraps the constructor's first four instructions in a trampoline.
#define STEAM_PATCH_ENTRY_INSTRUCTIONS 4

typedef struct {
    anchor_result anchors[STEAM_ANCHOR_COUNT];
    bool flag_offset_found;
    char flag_offset_problem[128];

    bool ready;
    char problem[192]; // why not ready, for people

    // Unslid addresses and the derived field offset; valid when ready.
    uint64_t constructor;
    uint64_t disable_function;
    uint32_t enabled_flag_offset;
} steam_patch_plan;

void steam_patch_plan_build(const macho_image *image, steam_patch_plan *plan);
