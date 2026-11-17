#include "steam_build_version.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long manifest_version(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file)
        return 0;
    char line[256];
    unsigned long long version = 0;
    // The version is one of the first lines: "version"		"1792618285"
    for (int i = 0; i < 8 && fgets(line, sizeof(line), file); i++) {
        if (sscanf(line, " \"version\" \"%llu\"", &version) == 1)
            break;
    }
    fclose(file);
    return version;
}

bool steam_build_version(const char *steamclient_path, char *out, size_t size) {
    char directory[1024];
    const char *slash = strrchr(steamclient_path, '/');
    if (!slash || (size_t)(slash - steamclient_path) + sizeof("/package") >= sizeof(directory))
        return false;
    snprintf(directory, sizeof(directory), "%.*s/package", (int)(slash - steamclient_path),
             steamclient_path);

    DIR *dir = opendir(directory);
    if (!dir)
        return false;
    unsigned long long best = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        size_t length = strlen(entry->d_name);
        if (strncmp(entry->d_name, "steam_client", 12) != 0 || length < 9 ||
            strcmp(entry->d_name + length - 9, ".manifest") != 0)
            continue;
        char path[1400];
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        unsigned long long version = manifest_version(path);
        if (version > best)
            best = version;
    }
    closedir(dir);
    if (!best)
        return false;
    snprintf(out, size, "%llu", best);
    return true;
}
