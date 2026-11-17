// libwheelio_steam.dylib: loaded into steam_osx through DYLD_INSERT_LIBRARIES.
// When steamclient.dylib loads (before any of its code runs) it resolves every
// gate and, only if all of them resolve, patches them all. Either way it
// records the outcome in status.json.

#include "macho_image.h"
#include "status_file.h"
#include "steam_build_version.h"
#include "steam_patch_apply.h"
#include "steam_patch_plan.h"
#include "steam_report.h"

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STEAMCLIENT_NAME "steamclient.dylib"
#define TOOLS_FOLDER "/Library/Application Support/Steam/compatibilitytools.d"

static void patch_steamclient(const struct mach_header *header, intptr_t slide, const char *path) {
    static steam_patch_plan plan;
    char build[32], uuid[37], problem[192] = "";
    steam_report report = {
        .steam_build = steam_build_version(path, build, sizeof(build)) ? build : NULL,
        .has_patch_applied = true,
    };

    macho_image image;
    if (macho_image_open_loaded(&image, header, slide)) {
        steam_patch_plan_build(&image, &plan);
        macho_image_uuid_string(&image, uuid);
        macho_image_close(&image);
        report.steamclient_uuid = uuid;
        report.plan = &plan;
        report.patch_applied = steam_patch_apply(&plan, slide, problem, sizeof(problem));
    } else {
        snprintf(problem, sizeof(problem), "steamclient.dylib could not be read in memory");
    }
    report.reason = report.patch_applied ? NULL : problem;

    static char json[8192];
    steam_report_json(&report, json, sizeof(json));
    if (!status_file_write(json))
        fprintf(stderr, "wheelio: could not write the Steam status file\n");
    if (!report.patch_applied)
        fprintf(stderr, "wheelio: Steam Play patch not applied: %s\n", problem);
}

static void image_added(const struct mach_header *header, intptr_t slide) {
    static atomic_flag handled = ATOMIC_FLAG_INIT;
    Dl_info info;
    if (!dladdr(header, &info) || !info.dli_fname)
        return;
    const char *slash = strrchr(info.dli_fname, '/');
    const char *name = slash ? slash + 1 : info.dli_fname;
    if (strcmp(name, STEAMCLIENT_NAME) != 0 || atomic_flag_test_and_set(&handled))
        return;
    patch_steamclient(header, slide, info.dli_fname);
}

// Mac Steam looks for local tools only next to steamclient.dylib and in this
// variable, so point it at the usual compatibilitytools.d too.
static void add_tools_folder(void) {
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return;
    char folder[1024];
    snprintf(folder, sizeof(folder), "%s%s", home, TOOLS_FOLDER);

    const char *current = getenv("STEAM_EXTRA_COMPAT_TOOLS_PATHS");
    if (!current || !current[0]) {
        setenv("STEAM_EXTRA_COMPAT_TOOLS_PATHS", folder, 1);
        return;
    }
    size_t length = strlen(folder);
    for (const char *at = strstr(current, folder); at; at = strstr(at + 1, folder))
        if ((at == current || at[-1] == ':') && (at[length] == '\0' || at[length] == ':'))
            return;
    char *joined = NULL;
    if (asprintf(&joined, "%s:%s", current, folder) > 0)
        setenv("STEAM_EXTRA_COMPAT_TOOLS_PATHS", joined, 1);
    free(joined);
}

// Inherited by everything Steam starts, so it stays inert outside steam_osx.
__attribute__((constructor)) static void wheelio_steam_load(void) {
    const char *program = getprogname();
    if (!program || strcmp(program, "steam_osx") != 0)
        return;
    add_tools_folder();
    _dyld_register_func_for_add_image(image_added);
}
