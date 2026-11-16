// wheelio-steam-check [steamclient.dylib]
// Prints the JSON report for a Steam build and exits 0 when the patch would
// apply, 1 when it would not. Defaults to the installed Steam client.

#include "wheelio_steam.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    char path[1024];
    if (argc > 1) {
        snprintf(path, sizeof(path), "%s", argv[1]);
    } else {
        const char *home = getenv("HOME");
        snprintf(path, sizeof(path),
                 "%s/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamclient.dylib",
                 home ? home : "");
    }
    char report[WHEELIO_STEAM_REPORT_SIZE];
    bool ready = wheelio_steam_check(path, report, sizeof(report));
    fputs(report, stdout);
    return ready ? 0 : 1;
}
