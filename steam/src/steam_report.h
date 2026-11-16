// The JSON report shared by status.json and the app's pre-check.
#pragma once

#include "steam_patch_plan.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *steam_build;          // NULL when unknown
    const char *steamclient_uuid;     // NULL when the image could not be read
    const steam_patch_plan *plan;     // NULL when the image could not be read
    const char *reason;               // why not ready or not applied, or NULL
    bool has_patch_applied;           // status.json only
    bool patch_applied;
} steam_report;

// Returns false if out is too small (out then holds a truncated report).
bool steam_report_json(const steam_report *report, char *out, size_t size);
