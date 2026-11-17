#include "steam_patch_plan.h"

#include "arm64_instructions.h"

#include <stdio.h>
#include <string.h>

// The flag test sits right after the frame setup.
#define FLAG_TEST_WINDOW 16

static const uint32_t *function_code(const macho_image *image, uint64_t start, uint64_t end,
                                     size_t *words) {
    *words = (size_t)((end - start) / 4);
    return macho_image_pointer(image, start, (size_t)(end - start));
}

// The byte a callback tests against 1 before anything else: LDRB Wt, [X0, #off]
// then CMP Wt, #1, with only frame setup before it.
static bool flag_test_offset(const macho_image *image, const anchor_result *handler,
                             uint32_t *offset) {
    size_t words;
    const uint32_t *code = function_code(image, handler->function, handler->function_end, &words);
    if (!code)
        return false;
    for (size_t i = 0; i < words && i < FLAG_TEST_WINDOW; i++) {
        unsigned rt, rn, cmp_rn;
        uint32_t off, imm;
        if (arm64_ldrb(code[i], &rt, &rn, &off)) {
            if (rn != 0 || i + 1 >= words)
                return false;
            if (!arm64_cmp_w_immediate(code[i + 1], &cmp_rn, &imm) || cmp_rn != rt || imm != 1)
                return false;
            *offset = off;
            return true;
        }
        if (!arm64_is_frame_setup(code[i]))
            return false;
    }
    return false;
}

static bool stores_byte_at(const macho_image *image, const anchor_result *function,
                           uint32_t offset, bool zero_only) {
    size_t words;
    const uint32_t *code = function_code(image, function->function, function->function_end, &words);
    for (size_t i = 0; code && i < words; i++) {
        unsigned rt, rn;
        uint32_t off;
        if (arm64_strb(code[i], &rt, &rn, &off) && off == offset && (!zero_only || rt == 31))
            return true;
    }
    return false;
}

// The first instructions can move into a trampoline: none is PC-relative and
// nothing in the function branches back between them.
static bool entry_relocatable(const macho_image *image, const anchor_result *function) {
    size_t words;
    const uint32_t *code = function_code(image, function->function, function->function_end, &words);
    if (!code || words < STEAM_PATCH_ENTRY_INSTRUCTIONS)
        return false;
    for (size_t i = 0; i < STEAM_PATCH_ENTRY_INSTRUCTIONS; i++)
        if (arm64_is_pc_relative(code[i]))
            return false;
    uint64_t low = function->function + 4;
    uint64_t high = function->function + 4 * STEAM_PATCH_ENTRY_INSTRUCTIONS;
    for (size_t i = 0; i < words; i++) {
        uint64_t target;
        if (arm64_branch_target(function->function + 4 * i, code[i], &target) &&
            target >= low && target < high)
            return false;
    }
    return true;
}

static void derive_flag_offset(const macho_image *image, steam_patch_plan *plan) {
    const anchor_result *config = &plan->anchors[STEAM_ANCHOR_APP_CONFIG_HANDLER];
    const anchor_result *logon = &plan->anchors[STEAM_ANCHOR_POST_LOGON_HANDLER];
    uint32_t from_config, from_logon;
    if (config->outcome != ANCHOR_RESOLVED || logon->outcome != ANCHOR_RESOLVED) {
        snprintf(plan->flag_offset_problem, sizeof(plan->flag_offset_problem),
                 "the callbacks it is read from did not resolve");
        return;
    }
    if (!flag_test_offset(image, config, &from_config) ||
        !flag_test_offset(image, logon, &from_logon)) {
        snprintf(plan->flag_offset_problem, sizeof(plan->flag_offset_problem),
                 "a callback no longer starts by testing the flag");
        return;
    }
    if (from_config != from_logon) {
        snprintf(plan->flag_offset_problem, sizeof(plan->flag_offset_problem),
                 "the callbacks test different bytes (0x%x and 0x%x)", from_config, from_logon);
        return;
    }
    plan->enabled_flag_offset = from_config;
    plan->flag_offset_found = true;
}

void steam_patch_plan_build(const macho_image *image, steam_patch_plan *plan) {
    memset(plan, 0, sizeof(*plan));
    resolve_anchors(image, steam_anchors, STEAM_ANCHOR_COUNT, plan->anchors);
    derive_flag_offset(image, plan);

    for (int i = 0; i < STEAM_ANCHOR_COUNT; i++) {
        const anchor_result *r = &plan->anchors[i];
        if (r->outcome == ANCHOR_RESOLVED)
            continue;
        snprintf(plan->problem, sizeof(plan->problem), "%s: %s%s%s%s", steam_anchors[i].name,
                 anchor_outcome_description(r->outcome), r->failed_item ? " (\"" : "",
                 r->failed_item ? r->failed_item : "", r->failed_item ? "\")" : "");
        return;
    }
    if (!plan->flag_offset_found) {
        snprintf(plan->problem, sizeof(plan->problem), "enabled flag: %s",
                 plan->flag_offset_problem);
        return;
    }

    const anchor_result *constructor = &plan->anchors[STEAM_ANCHOR_COMPAT_MANAGER_CONSTRUCTOR];
    const anchor_result *disable = &plan->anchors[STEAM_ANCHOR_COMPAT_DISABLE];
    const uint32_t offset = plan->enabled_flag_offset;
    if (!stores_byte_at(image, constructor, offset, false)) {
        snprintf(plan->problem, sizeof(plan->problem),
                 "compat_manager_constructor: does not set the enabled flag at 0x%x", offset);
        return;
    }
    if (!stores_byte_at(image, disable, offset, true)) {
        snprintf(plan->problem, sizeof(plan->problem),
                 "compat_manager_disable: does not clear the enabled flag at 0x%x", offset);
        return;
    }
    if (!entry_relocatable(image, constructor)) {
        snprintf(plan->problem, sizeof(plan->problem),
                 "compat_manager_constructor: its first instructions cannot be moved");
        return;
    }

    plan->constructor = constructor->function;
    plan->disable_function = disable->function;
    plan->ready = true;
}
