// Rewrites code in this process: whole-function replacements and entry jumps
// with a trampoline back to the original. arm64 only.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CODE_PATCH_MAX_INSTRUCTIONS 4

typedef struct {
    uintptr_t address;
    uint32_t instructions[CODE_PATCH_MAX_INSTRUCTIONS];
    size_t count;
} code_patch;

// A patch that makes the function at address jump to target.
code_patch code_patch_jump(uintptr_t address, uintptr_t target);

// A patch that makes the function at address return at once.
code_patch code_patch_return(uintptr_t address);

// Executable copy of the first `count` instructions at function, followed by a
// jump to the instruction after them: calling it runs the original function.
// The caller has checked those instructions are not PC-relative.
void *trampoline_create(uintptr_t function, size_t count);
void trampoline_destroy(void *trampoline);

// Writes every patch, or none if any page cannot be made writable.
bool code_patches_apply(const code_patch *patches, size_t count);
