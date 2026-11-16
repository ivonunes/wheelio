#include "reference_scanner.h"

#include "arm64_instructions.h"

#include <string.h>

// How far past an ADRP to look for the ADD that completes the address. Looking
// further can only find more references, which turns a hit into a miss, never
// the other way round.
#define MAX_ADD_DISTANCE 1024

int find_cstring(const macho_image *image, const char *text,
                 uint64_t addresses[REFERENCE_MAX_ADDRESSES]) {
    const char *section = macho_image_pointer(image, image->cstrings.addr, image->cstrings.size);
    size_t size = image->cstrings.size;
    size_t length = strlen(text);
    int count = 0;
    if (!section || length == 0)
        return 0;

    for (size_t at = 0; at + length < size;) {
        const char *hit = memmem(section + at, size - at, text, length + 1);
        if (!hit)
            break;
        size_t offset = (size_t)(hit - section);
        if ((offset == 0 || section[offset - 1] == '\0') && count < REFERENCE_MAX_ADDRESSES)
            addresses[count++] = image->cstrings.addr + offset;
        at = offset + length + 1;
    }
    return count;
}

static void add_reference(const macho_image *image, reference_target *target, uint64_t pc) {
    uint64_t start, end;
    target->reference_count++;
    if (!macho_image_function_at(image, pc, &start, &end)) {
        target->overflowed = true;
        return;
    }
    for (int i = 0; i < target->function_count; i++)
        if (target->functions[i] == start)
            return;
    if (target->function_count == REFERENCE_MAX_FUNCTIONS) {
        target->overflowed = true;
        return;
    }
    target->functions[target->function_count++] = start;
}

static void note_address(const macho_image *image, reference_target *targets, size_t count,
                         uint64_t pc, uint64_t address) {
    for (size_t i = 0; i < count; i++)
        for (int a = 0; a < targets[i].address_count; a++)
            if (targets[i].addresses[a] == address)
                add_reference(image, &targets[i], pc);
}

static bool page_wanted(const reference_target *targets, size_t count, uint64_t page) {
    for (size_t i = 0; i < count; i++)
        for (int a = 0; a < targets[i].address_count; a++)
            if ((targets[i].addresses[a] & ~0xFFFull) == page)
                return true;
    return false;
}

void find_references(const macho_image *image, reference_target *targets, size_t count) {
    for (size_t i = 0; i < count; i++) {
        targets[i].reference_count = 0;
        targets[i].function_count = 0;
        targets[i].overflowed = false;
    }

    const uint32_t *code = macho_image_pointer(image, image->text.addr, image->text.size);
    size_t words = image->text.size / 4;
    if (!code)
        return;

    for (size_t i = 0; i < words; i++) {
        uint64_t pc = image->text.addr + i * 4;
        unsigned rd;
        uint64_t address;
        if (arm64_adr(pc, code[i], &rd, &address)) {
            note_address(image, targets, count, pc, address);
            continue;
        }
        if (!arm64_adrp(pc, code[i], &rd, &address) || !page_wanted(targets, count, address))
            continue;

        // The ADD can be scheduled well after the ADRP, but stays in the function.
        uint64_t start, end = pc + 4;
        if (!macho_image_function_at(image, pc, &start, &end))
            end = pc + 4;
        size_t limit = (size_t)((end - image->text.addr) / 4);
        if (limit > i + MAX_ADD_DISTANCE)
            limit = i + MAX_ADD_DISTANCE;
        for (size_t j = i + 1; j < limit && j < words; j++) {
            unsigned add_rd, add_rn;
            uint64_t immediate;
            if (arm64_add_immediate(code[j], &add_rd, &add_rn, &immediate) && add_rn == rd)
                note_address(image, targets, count, pc, address + immediate);
        }
    }
}
