// The anchors that find Steam Play's gates in steamclient.dylib.
#pragma once

#include "anchor_resolver.h"

// Indices into steam_anchors, by the role each function plays in the patch.
enum {
    STEAM_ANCHOR_APP_CONFIG_HANDLER,
    STEAM_ANCHOR_POST_LOGON_HANDLER,
    STEAM_ANCHOR_COMPAT_MANAGER_CONSTRUCTOR,
    STEAM_ANCHOR_COMPAT_DISABLE,
    STEAM_ANCHOR_COUNT
};

extern const anchor steam_anchors[STEAM_ANCHOR_COUNT];
