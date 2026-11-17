// Runs the resolver over each fake_steamclient variant and checks that only the
// good one would be patched, and that each miss is reported for the right reason.
// Usage: test_fixtures <folder holding fixture_<variant>.dylib>

#include "macho_image.h"
#include "steam_patch_plan.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *variant;
    const char *expected_problem; // NULL: the plan must be ready
} expectation;

static const expectation expectations[] = {
    { "good", NULL },
    { "string_missing", "compat_manager_disable: string not found" },
    { "not_referenced", "compat_manager_disable: not referenced by any function" },
    { "string_twice", "compat_manager_post_logon_handler: referenced from more than one function" },
    { "constructor_twice", "compat_manager_constructor: referenced from more than one function" },
    { "flag_mismatch", "enabled flag: the callbacks test different bytes" },
    { "no_flag_test", "enabled flag: a callback no longer starts by testing the flag" },
    { "disable_wrong", "compat_manager_disable: does not clear the enabled flag" },
    { "entry_pc_relative", "compat_manager_constructor: its first instructions cannot be moved" },
};

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <fixture folder>\n", argv[0]);
        return 2;
    }
    int failures = 0;
    for (size_t i = 0; i < sizeof(expectations) / sizeof(expectations[0]); i++) {
        const expectation *e = &expectations[i];
        char path[1024];
        snprintf(path, sizeof(path), "%s/fixture_%s.dylib", argv[1], e->variant);

        macho_image image;
        if (!macho_image_open_file(&image, path)) {
            fprintf(stderr, "FAIL %s: cannot open %s\n", e->variant, path);
            failures++;
            continue;
        }
        steam_patch_plan plan;
        steam_patch_plan_build(&image, &plan);
        macho_image_close(&image);

        bool ok = e->expected_problem ? !plan.ready && strstr(plan.problem, e->expected_problem)
                                      : plan.ready;
        if (!ok) {
            fprintf(stderr, "FAIL %s: ready=%d problem=\"%s\"\n", e->variant, plan.ready,
                    plan.problem);
            failures++;
        }
    }
    return failures ? 1 : 0;
}
