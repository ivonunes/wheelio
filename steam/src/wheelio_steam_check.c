#include "wheelio_steam.h"

#include "macho_image.h"
#include "steam_build_version.h"
#include "steam_patch_plan.h"
#include "steam_report.h"

bool wheelio_steam_check(const char *steamclient_path, char *report, size_t report_size) {
    char build[32];
    steam_report r = {
        .steam_build = steam_build_version(steamclient_path, build, sizeof(build)) ? build : NULL,
    };

    macho_image image;
    if (!macho_image_open_file(&image, steamclient_path)) {
        r.reason = "not an arm64 steamclient.dylib that can be read";
        if (report)
            steam_report_json(&r, report, report_size);
        return false;
    }

    steam_patch_plan plan;
    steam_patch_plan_build(&image, &plan);
    char uuid[37];
    macho_image_uuid_string(&image, uuid);
    macho_image_close(&image);

    r.steamclient_uuid = uuid;
    r.plan = &plan;
    r.reason = plan.ready ? NULL : plan.problem;
    if (report)
        steam_report_json(&r, report, report_size);
    return plan.ready;
}
