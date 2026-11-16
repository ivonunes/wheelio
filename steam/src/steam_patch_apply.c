#include "steam_patch_apply.h"

#include "code_patcher.h"

#include <stdio.h>

// Passing x0-x7 through covers the constructor's (this, owner) and anything
// Steam adds later; on arm64 Apple's constructors return this.
typedef uint64_t (*constructor_function)(uint64_t, uint64_t, uint64_t, uint64_t,
                                         uint64_t, uint64_t, uint64_t, uint64_t);

static constructor_function original_constructor;
static uint32_t enabled_flag_offset;

static uint64_t construct_compat_manager(uint64_t self, uint64_t a1, uint64_t a2, uint64_t a3,
                                         uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    uint64_t result = original_constructor(self, a1, a2, a3, a4, a5, a6, a7);
    ((volatile uint8_t *)(uintptr_t)self)[enabled_flag_offset] = 1;
    return result;
}

bool steam_patch_apply(const steam_patch_plan *plan, intptr_t slide, char *problem,
                       size_t problem_size) {
    if (!plan->ready) {
        snprintf(problem, problem_size, "%s", plan->problem);
        return false;
    }
    uintptr_t constructor = (uintptr_t)(plan->constructor + (uint64_t)slide);
    uintptr_t disable = (uintptr_t)(plan->disable_function + (uint64_t)slide);

    void *trampoline = trampoline_create(constructor, STEAM_PATCH_ENTRY_INSTRUCTIONS);
    if (!trampoline) {
        snprintf(problem, problem_size, "could not allocate a trampoline");
        return false;
    }
    original_constructor = (constructor_function)trampoline;
    enabled_flag_offset = plan->enabled_flag_offset;

    const code_patch patches[] = {
        code_patch_jump(constructor, (uintptr_t)construct_compat_manager),
        code_patch_return(disable),
    };
    if (!code_patches_apply(patches, sizeof(patches) / sizeof(patches[0]))) {
        original_constructor = NULL;
        trampoline_destroy(trampoline);
        snprintf(problem, problem_size, "could not make Steam's code writable");
        return false;
    }
    return true;
}
