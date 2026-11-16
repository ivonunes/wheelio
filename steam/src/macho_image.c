#include "macho_image.h"

#include <fcntl.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libkern/OSByteOrder.h>

static const macho_segment *segment_named(const macho_image *image, const char *name) {
    for (int i = 0; i < image->segment_count; i++)
        if (strcmp(image->segments[i].name, name) == 0)
            return &image->segments[i];
    return NULL;
}

// Memory holding the file bytes [fileoff, fileoff + size).
static const uint8_t *file_pointer(const macho_image *image, uint64_t fileoff, uint64_t size) {
    if (!image->in_memory) {
        if (fileoff > image->file_size || size > image->file_size - fileoff)
            return NULL;
        return image->header + fileoff;
    }
    for (int i = 0; i < image->segment_count; i++) {
        const macho_segment *s = &image->segments[i];
        if (fileoff >= s->fileoff && fileoff - s->fileoff + size <= s->filesize)
            return (const uint8_t *)(uintptr_t)(s->vmaddr + (uint64_t)image->slide + (fileoff - s->fileoff));
    }
    return NULL;
}

const void *macho_image_pointer(const macho_image *image, uint64_t addr, size_t size) {
    for (int i = 0; i < image->segment_count; i++) {
        const macho_segment *s = &image->segments[i];
        if (addr < s->vmaddr || addr - s->vmaddr + size > s->filesize)
            continue;
        if (image->in_memory)
            return (const void *)(uintptr_t)(addr + (uint64_t)image->slide);
        return file_pointer(image, s->fileoff + (addr - s->vmaddr), size);
    }
    return NULL;
}

static bool read_function_starts(macho_image *image, uint32_t dataoff, uint32_t datasize) {
    const uint8_t *p = file_pointer(image, dataoff, datasize);
    const macho_segment *text_segment = segment_named(image, "__TEXT");
    if (!p || !text_segment)
        return false;

    size_t capacity = 1024;
    uint64_t *starts = malloc(capacity * sizeof(*starts));
    if (!starts)
        return false;

    size_t count = 0;
    uint64_t addr = text_segment->vmaddr;
    const uint8_t *end = p + datasize;
    while (p < end) {
        uint64_t delta = 0;
        int shift = 0;
        uint8_t byte;
        do {
            if (p >= end || shift > 63) {
                free(starts);
                return false;
            }
            byte = *p++;
            delta |= (uint64_t)(byte & 0x7f) << shift;
            shift += 7;
        } while (byte & 0x80);
        if (delta == 0)
            break;
        addr += delta;
        if (count == capacity) {
            capacity *= 2;
            uint64_t *grown = realloc(starts, capacity * sizeof(*starts));
            if (!grown) {
                free(starts);
                return false;
            }
            starts = grown;
        }
        starts[count++] = addr;
    }
    image->function_starts = starts;
    image->function_count = count;
    return count > 0;
}

static bool parse(macho_image *image) {
    const struct mach_header_64 *mh = (const struct mach_header_64 *)image->header;
    if (mh->magic != MH_MAGIC_64 || mh->cputype != CPU_TYPE_ARM64)
        return false;
    if (!image->in_memory && sizeof(*mh) + mh->sizeofcmds > image->file_size)
        return false;

    uint32_t starts_off = 0, starts_size = 0;
    const uint8_t *cursor = image->header + sizeof(*mh);
    const uint8_t *commands_end = cursor + mh->sizeofcmds;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)cursor;
        if (cursor + sizeof(*lc) > commands_end || lc->cmdsize < sizeof(*lc) ||
            cursor + lc->cmdsize > commands_end)
            return false;

        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sc = (const struct segment_command_64 *)lc;
            if (image->segment_count < MACHO_IMAGE_MAX_SEGMENTS) {
                macho_segment *s = &image->segments[image->segment_count++];
                memcpy(s->name, sc->segname, 16);
                s->name[16] = '\0';
                s->vmaddr = sc->vmaddr;
                s->vmsize = sc->vmsize;
                s->fileoff = sc->fileoff;
                s->filesize = sc->filesize;
            }
            const struct section_64 *sect = (const struct section_64 *)(sc + 1);
            for (uint32_t j = 0; j < sc->nsects; j++) {
                if ((const uint8_t *)(sect + j + 1) > cursor + lc->cmdsize)
                    return false;
                if (strncmp(sect[j].segname, "__TEXT", 16) != 0)
                    continue;
                if (strncmp(sect[j].sectname, "__text", 16) == 0)
                    image->text = (macho_range){ sect[j].addr, sect[j].size };
                else if (strncmp(sect[j].sectname, "__cstring", 16) == 0)
                    image->cstrings = (macho_range){ sect[j].addr, sect[j].size };
            }
        } else if (lc->cmd == LC_FUNCTION_STARTS) {
            const struct linkedit_data_command *ld = (const struct linkedit_data_command *)lc;
            starts_off = ld->dataoff;
            starts_size = ld->datasize;
        } else if (lc->cmd == LC_UUID) {
            memcpy(image->uuid, ((const struct uuid_command *)lc)->uuid, 16);
        }
        cursor += lc->cmdsize;
    }

    if (!image->text.size || !image->cstrings.size || !starts_size)
        return false;
    if (!macho_image_pointer(image, image->text.addr, image->text.size) ||
        !macho_image_pointer(image, image->cstrings.addr, image->cstrings.size))
        return false;
    return read_function_starts(image, starts_off, starts_size);
}

bool macho_image_open_loaded(macho_image *image, const void *header, intptr_t slide) {
    memset(image, 0, sizeof(*image));
    image->header = header;
    image->in_memory = true;
    image->slide = slide;
    if (parse(image))
        return true;
    macho_image_close(image);
    return false;
}

// The arm64 slice of a universal binary, or the whole file if it is thin.
static bool arm64_slice(const uint8_t *file, size_t size, uint64_t *offset, uint64_t *length) {
    if (size < sizeof(struct fat_header))
        return false;
    const struct fat_header *fh = (const struct fat_header *)file;
    uint32_t magic = OSSwapBigToHostInt32(fh->magic);
    if (magic != FAT_MAGIC && magic != FAT_MAGIC_64) {
        *offset = 0;
        *length = size;
        return true;
    }
    uint32_t n = OSSwapBigToHostInt32(fh->nfat_arch);
    const uint8_t *p = file + sizeof(*fh);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t off, len;
        cpu_type_t cpu;
        if (magic == FAT_MAGIC) {
            if (p + sizeof(struct fat_arch) > file + size)
                return false;
            const struct fat_arch *a = (const struct fat_arch *)p;
            cpu = (cpu_type_t)OSSwapBigToHostInt32((uint32_t)a->cputype);
            off = OSSwapBigToHostInt32(a->offset);
            len = OSSwapBigToHostInt32(a->size);
            p += sizeof(*a);
        } else {
            if (p + sizeof(struct fat_arch_64) > file + size)
                return false;
            const struct fat_arch_64 *a = (const struct fat_arch_64 *)p;
            cpu = (cpu_type_t)OSSwapBigToHostInt32((uint32_t)a->cputype);
            off = OSSwapBigToHostInt64(a->offset);
            len = OSSwapBigToHostInt64(a->size);
            p += sizeof(*a);
        }
        if (cpu == CPU_TYPE_ARM64 && off <= size && len <= size - off) {
            *offset = off;
            *length = len;
            return true;
        }
    }
    return false;
}

bool macho_image_open_file(macho_image *image, const char *path) {
    memset(image, 0, sizeof(*image));
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return false;
    }
    void *mapping = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED)
        return false;
    image->mapping = mapping;
    image->mapping_size = (size_t)st.st_size;

    uint64_t offset, length;
    if (!arm64_slice(mapping, (size_t)st.st_size, &offset, &length) ||
        length < sizeof(struct mach_header_64)) {
        macho_image_close(image);
        return false;
    }
    image->header = (const uint8_t *)mapping + offset;
    image->file_size = (size_t)length;
    if (parse(image))
        return true;
    macho_image_close(image);
    return false;
}

void macho_image_close(macho_image *image) {
    free(image->function_starts);
    if (image->mapping)
        munmap(image->mapping, image->mapping_size);
    memset(image, 0, sizeof(*image));
}

bool macho_image_function_at(const macho_image *image, uint64_t addr,
                             uint64_t *start, uint64_t *end) {
    uint64_t text_end = image->text.addr + image->text.size;
    if (addr < image->text.addr || addr >= text_end || image->function_count == 0)
        return false;

    size_t lo = 0, hi = image->function_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (image->function_starts[mid] <= addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return false;
    *start = image->function_starts[lo - 1];
    *end = lo < image->function_count ? image->function_starts[lo] : text_end;
    if (*end > text_end)
        *end = text_end;
    return *start >= image->text.addr;
}

void macho_image_uuid_string(const macho_image *image, char out[37]) {
    const uint8_t *u = image->uuid;
    snprintf(out, 37, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
             u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}
