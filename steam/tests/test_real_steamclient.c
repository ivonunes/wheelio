// Checks that the patch would apply to the Steam client installed on this Mac
// (read-only). Pass other steamclient.dylib paths to check those as well.
// Exits 77 (skipped) when no Steam client is installed.

#include "wheelio_steam.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    char installed[1024];
    snprintf(installed, sizeof(installed),
             "%s/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamclient.dylib",
             getenv("HOME") ? getenv("HOME") : "");

    int failures = 0, checked = 0;
    for (int i = 0; i < (argc > 1 ? argc - 1 : 1); i++) {
        const char *path = argc > 1 ? argv[i + 1] : installed;
        if (argc == 1 && access(path, R_OK) != 0) {
            fprintf(stderr, "SKIP no Steam client at %s\n", path);
            return 77;
        }
        char report[WHEELIO_STEAM_REPORT_SIZE];
        checked++;
        if (!wheelio_steam_check(path, report, sizeof(report))) {
            fprintf(stderr, "FAIL %s\n%s", path, report);
            failures++;
        }
    }
    return failures || !checked ? 1 : 0;
}
