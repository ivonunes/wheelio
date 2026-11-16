// A read-only view of one arm64 Mach-O image, either loaded in this process or
// read from a file on disk. Addresses are always the image's own (unslid)
// virtual addresses; macho_image_pointer turns one into readable memory.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MACHO_IMAGE_MAX_SEGMENTS 16

typedef struct {
    char name[17];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
} macho_segment;

typedef struct {
    uint64_t addr;
    uint64_t size;
} macho_range;

typedef struct {
    const uint8_t *header;     // the mach_header_64
    size_t file_size;          // bytes readable from header (file mode only)
    bool in_memory;
    intptr_t slide;            // in-memory only

    macho_segment segments[MACHO_IMAGE_MAX_SEGMENTS];
    int segment_count;

    macho_range text;          // __TEXT,__text
    macho_range cstrings;      // __TEXT,__cstring
    uint8_t uuid[16];

    uint64_t *function_starts; // sorted, from LC_FUNCTION_STARTS
    size_t function_count;

    void *mapping;             // file mode: the whole file, unmapped on close
    size_t mapping_size;
} macho_image;

// Opens an image already loaded in this process. Returns false if it is not a
// usable arm64 image (no __text, no LC_FUNCTION_STARTS, ...).
bool macho_image_open_loaded(macho_image *image, const void *header, intptr_t slide);

// Opens a file on disk, taking the arm64 slice of a universal binary.
bool macho_image_open_file(macho_image *image, const char *path);

void macho_image_close(macho_image *image);

// Readable memory for [addr, addr + size), or NULL if outside the image.
const void *macho_image_pointer(const macho_image *image, uint64_t addr, size_t size);

// The function containing addr: its start and end (the next function's start,
// or the end of __text). Returns false if addr is not inside a known function.
bool macho_image_function_at(const macho_image *image, uint64_t addr,
                             uint64_t *start, uint64_t *end);

void macho_image_uuid_string(const macho_image *image, char out[37]);
