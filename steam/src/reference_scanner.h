// Finds which functions take the address of something: a C string or another
// function. References are ADRP+ADD pairs and ADR in __text; the function
// around each one comes from LC_FUNCTION_STARTS.
#pragma once

#include "macho_image.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REFERENCE_MAX_ADDRESSES 4
#define REFERENCE_MAX_FUNCTIONS 8

typedef struct {
    // In: the addresses that count as this target (a string can have copies).
    uint64_t addresses[REFERENCE_MAX_ADDRESSES];
    int address_count;

    // Out.
    int reference_count;
    uint64_t functions[REFERENCE_MAX_FUNCTIONS]; // distinct function starts
    int function_count;
    bool overflowed; // more functions than fit, or a reference outside any function
} reference_target;

// Addresses of every copy of text (whole, NUL-terminated) in __cstring.
int find_cstring(const macho_image *image, const char *text,
                 uint64_t addresses[REFERENCE_MAX_ADDRESSES]);

// Fills in the references to each target in one pass over __text.
void find_references(const macho_image *image, reference_target *targets, size_t count);
