#include "status_file.h"

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *home_directory(void) {
    const char *home = getenv("HOME");
    if (home && home[0])
        return home;
    struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_dir : NULL;
}

// mkdir -p
static bool make_directories(char *path) {
    for (char *slash = strchr(path + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        bool made = mkdir(path, 0755) == 0 || errno == EEXIST;
        *slash = '/';
        if (!made)
            return false;
    }
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

bool status_file_write(const char *text) {
    const char *home = home_directory();
    if (!home)
        return false;
    char directory[1024], path[1100], temporary[1100];
    snprintf(directory, sizeof(directory), "%s/Library/Application Support/Wheelio/Steam", home);
    if (!make_directories(directory))
        return false;
    snprintf(path, sizeof(path), "%s/status.json", directory);
    snprintf(temporary, sizeof(temporary), "%s/.status.json.%d", directory, (int)getpid());

    FILE *file = fopen(temporary, "w");
    if (!file)
        return false;
    bool written = fputs(text, file) >= 0;
    written &= fclose(file) == 0;
    if (!written || rename(temporary, path) != 0) {
        unlink(temporary);
        return false;
    }
    return true;
}
