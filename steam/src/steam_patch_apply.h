// Applies a ready plan to the steamclient.dylib loaded in this process: the
// compat manager's constructor turns Steam Play on after it runs, and the one
// function that turns it off does nothing.
#pragma once

#include "steam_patch_plan.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Changes nothing and says why when it cannot apply everything.
bool steam_patch_apply(const steam_patch_plan *plan, intptr_t slide, char *problem,
                       size_t problem_size);
