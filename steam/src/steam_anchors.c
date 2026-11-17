#include "steam_anchors.h"

// CCompatManager keeps Steam Play's on/off switch in a byte its constructor sets
// to (oslist == "linux"). These find the constructor, two callbacks that test
// the byte first thing (they tell us its offset), and the one function that
// clears it again.
const anchor steam_anchors[STEAM_ANCHOR_COUNT] = {
    [STEAM_ANCHOR_APP_CONFIG_HANDLER] = {
        .name = "compat_manager_app_config_handler",
        .strings = { "CCompatManager::OnAppConfigInitialized" },
    },
    [STEAM_ANCHOR_POST_LOGON_HANDLER] = {
        .name = "compat_manager_post_logon_handler",
        .strings = { "CCompatManager::OnPostLogonState" },
    },
    // The constructor has no string of its own, but it is the only code that
    // takes the address of these callbacks, to register them.
    [STEAM_ANCHOR_COMPAT_MANAGER_CONSTRUCTOR] = {
        .name = "compat_manager_constructor",
        .functions = { "compat_manager_app_config_handler", "compat_manager_post_logon_handler" },
    },
    [STEAM_ANCHOR_COMPAT_DISABLE] = {
        .name = "compat_manager_disable",
        .strings = { "Disabling compatibility layer." },
    },
};
