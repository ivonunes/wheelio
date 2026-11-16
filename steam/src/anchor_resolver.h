// Resolves anchors to functions. An anchor names the C strings a function
// references, or the other anchors whose function address it takes (for a
// function with no string of its own). It resolves only when every reference
// to every one of those lies in the same single function; anything else is a
// miss. Anchors are plain data.
#pragma once

#include "macho_image.h"

#include <stddef.h>
#include <stdint.h>

#define ANCHOR_MAX_ITEMS 4
#define ANCHOR_MAX_FUNCTIONS 8

typedef struct {
    const char *name;
    const char *strings[ANCHOR_MAX_ITEMS];   // unused slots are NULL
    const char *functions[ANCHOR_MAX_ITEMS]; // names of earlier anchors
} anchor;

typedef enum {
    ANCHOR_RESOLVED,
    ANCHOR_STRING_MISSING,     // a string is not in __cstring
    ANCHOR_NOT_REFERENCED,     // a string or function is never referenced
    ANCHOR_SEVERAL_FUNCTIONS,  // references span more than one function
    ANCHOR_DEPENDENCY_MISSING, // a named anchor is unknown or did not resolve
} anchor_outcome;

typedef struct {
    anchor_outcome outcome;
    const char *failed_item; // the string or anchor name behind a miss
    uint64_t function;       // unslid start of the resolved function
    uint64_t function_end;
    // Every distinct function referencing the anchor's items, for diagnostics.
    uint64_t candidates[ANCHOR_MAX_FUNCTIONS];
    int candidate_count;
} anchor_result;

void resolve_anchors(const macho_image *image, const anchor *anchors, size_t count,
                     anchor_result *results);

const char *anchor_outcome_description(anchor_outcome outcome);
