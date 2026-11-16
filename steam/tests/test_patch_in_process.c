// Loads the good fixture, patches it in memory exactly as the dylib patches
// Steam, and checks the constructor now leaves the flag on and the disable
// function no longer clears it.
// Usage: test_patch_in_process <fixture_good.dylib>

#include "macho_image.h"
#include "steam_patch_apply.h"
#include "steam_patch_plan.h"

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FLAG 0x810

typedef void *(*construct_function)(void *self);
typedef void (*disable_function)(void *self);

static bool loaded_image(const void *symbol, const void **header, intptr_t *slide) {
    Dl_info info;
    if (!dladdr(symbol, &info))
        return false;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        if (_dyld_get_image_header(i) == info.dli_fbase) {
            *header = info.dli_fbase;
            *slide = _dyld_get_image_vmaddr_slide(i);
            return true;
        }
    }
    return false;
}

static int fail(const char *message) {
    fprintf(stderr, "FAIL %s\n", message);
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 2)
        return fail("usage: test_patch_in_process <fixture_good.dylib>");
    void *library = dlopen(argv[1], RTLD_NOW);
    if (!library)
        return fail(dlerror());
    construct_function construct = (construct_function)dlsym(library, "fake_compat_manager_construct");
    disable_function disable = (disable_function)dlsym(library, "fake_compat_manager_disable");
    if (!construct || !disable)
        return fail("fixture symbols missing");

    static uint8_t manager[0x1000];
    if (construct(manager) != manager || manager[FLAG] != 0)
        return fail("unpatched constructor does not behave like Steam's");

    const void *header;
    intptr_t slide;
    if (!loaded_image((const void *)construct, &header, &slide))
        return fail("fixture image not found");
    macho_image image;
    if (!macho_image_open_loaded(&image, header, slide))
        return fail("fixture image not readable in memory");
    steam_patch_plan plan;
    steam_patch_plan_build(&image, &plan);
    macho_image_close(&image);
    if (!plan.ready)
        return fail(plan.problem);

    char problem[192];
    if (!steam_patch_apply(&plan, slide, problem, sizeof(problem)))
        return fail(problem);

    memset(manager, 0, sizeof(manager));
    if (construct(manager) != manager)
        return fail("patched constructor does not return this");
    if (manager[FLAG] != 1)
        return fail("patched constructor leaves Steam Play off");
    uintptr_t callback;
    memcpy(&callback, manager + 8, sizeof(callback));
    if (!callback)
        return fail("patched constructor skipped the original body");
    disable(manager);
    if (manager[FLAG] != 1)
        return fail("disable still turns Steam Play off");
    return 0;
}
