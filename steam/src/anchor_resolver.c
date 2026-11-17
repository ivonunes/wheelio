#include "anchor_resolver.h"

#include "reference_scanner.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef enum { PENDING, DONE } anchor_state;

static int anchor_index(const anchor *anchors, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++)
        if (strcmp(anchors[i].name, name) == 0)
            return (int)i;
    return -1;
}

// 1 when every function the anchor names has resolved, 0 while one is still
// pending, -1 when one can never resolve (unknown, missed, or itself).
static int dependencies_ready(const anchor *anchors, size_t count, size_t index,
                              const anchor_state *state, const anchor_result *results,
                              const char **failed) {
    int ready = 1;
    for (int f = 0; f < ANCHOR_MAX_ITEMS && anchors[index].functions[f]; f++) {
        int d = anchor_index(anchors, count, anchors[index].functions[f]);
        if (d < 0 || (size_t)d == index ||
            (state[d] == DONE && results[d].outcome != ANCHOR_RESOLVED)) {
            *failed = anchors[index].functions[f];
            return -1;
        }
        if (state[d] == PENDING)
            ready = 0;
    }
    return ready;
}

static void add_candidate(anchor_result *r, uint64_t function, bool *overflow) {
    for (int k = 0; k < r->candidate_count; k++)
        if (r->candidates[k] == function)
            return;
    if (r->candidate_count == ANCHOR_MAX_FUNCTIONS)
        *overflow = true;
    else
        r->candidates[r->candidate_count++] = function;
}

// Resolves the anchors marked in `batch` with one scan of __text.
static void resolve_batch(const macho_image *image, const anchor *anchors, size_t count,
                          const bool *batch, anchor_result *results) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++)
        if (batch[i])
            total += 2 * ANCHOR_MAX_ITEMS;
    reference_target *targets = calloc(total ? total : 1, sizeof(*targets));
    const char **items = calloc(total ? total : 1, sizeof(*items));
    if (!targets || !items) {
        for (size_t i = 0; i < count; i++)
            if (batch[i])
                results[i].outcome = ANCHOR_NOT_REFERENCED;
        free(targets);
        free(items);
        return;
    }

    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        if (!batch[i])
            continue;
        for (int s = 0; s < ANCHOR_MAX_ITEMS && anchors[i].strings[s]; s++, n++) {
            items[n] = anchors[i].strings[s];
            targets[n].address_count = find_cstring(image, items[n], targets[n].addresses);
        }
        for (int f = 0; f < ANCHOR_MAX_ITEMS && anchors[i].functions[f]; f++, n++) {
            items[n] = anchors[i].functions[f];
            int d = anchor_index(anchors, count, items[n]);
            targets[n].addresses[0] = results[d].function;
            targets[n].address_count = 1;
        }
    }

    find_references(image, targets, n);

    n = 0;
    for (size_t i = 0; i < count; i++) {
        if (!batch[i])
            continue;
        anchor_result *r = &results[i];
        bool overflow = false;
        size_t first = n;
        size_t items_here = 0;
        for (int s = 0; s < ANCHOR_MAX_ITEMS && anchors[i].strings[s]; s++)
            items_here++;
        for (int f = 0; f < ANCHOR_MAX_ITEMS && anchors[i].functions[f]; f++)
            items_here++;
        n += items_here;

        for (size_t t = first; t < n; t++) {
            if (!r->failed_item && targets[t].address_count == 0) {
                r->outcome = ANCHOR_STRING_MISSING;
                r->failed_item = items[t];
            } else if (!r->failed_item && targets[t].reference_count == 0) {
                r->outcome = ANCHOR_NOT_REFERENCED;
                r->failed_item = items[t];
            }
            overflow |= targets[t].overflowed;
            for (int f = 0; f < targets[t].function_count; f++)
                add_candidate(r, targets[t].functions[f], &overflow);
        }
        if (r->failed_item)
            continue;
        if (items_here == 0 || overflow || r->candidate_count != 1) {
            r->outcome = ANCHOR_SEVERAL_FUNCTIONS;
            continue;
        }
        uint64_t start, end;
        if (!macho_image_function_at(image, r->candidates[0], &start, &end)) {
            r->outcome = ANCHOR_NOT_REFERENCED;
            continue;
        }
        r->outcome = ANCHOR_RESOLVED;
        r->function = start;
        r->function_end = end;
    }
    free(targets);
    free(items);
}

void resolve_anchors(const macho_image *image, const anchor *anchors, size_t count,
                     anchor_result *results) {
    anchor_state *state = calloc(count ? count : 1, sizeof(*state));
    bool *batch = calloc(count ? count : 1, sizeof(*batch));
    for (size_t i = 0; i < count; i++)
        memset(&results[i], 0, sizeof(results[i]));
    if (!state || !batch) {
        for (size_t i = 0; i < count; i++)
            results[i].outcome = ANCHOR_DEPENDENCY_MISSING;
        free(state);
        free(batch);
        return;
    }

    for (;;) {
        bool any = false;
        for (size_t i = 0; i < count; i++) {
            batch[i] = false;
            if (state[i] != PENDING)
                continue;
            const char *failed = NULL;
            int ready = dependencies_ready(anchors, count, i, state, results, &failed);
            if (ready < 0) {
                results[i].outcome = ANCHOR_DEPENDENCY_MISSING;
                results[i].failed_item = failed;
                state[i] = DONE;
                any = true;
            } else if (ready > 0) {
                batch[i] = true;
                any = true;
            }
        }
        if (!any)
            break;
        bool scan = false;
        for (size_t i = 0; i < count; i++)
            scan |= batch[i];
        if (scan)
            resolve_batch(image, anchors, count, batch, results);
        for (size_t i = 0; i < count; i++)
            if (batch[i])
                state[i] = DONE;
    }

    // Anything still pending depends on a cycle.
    for (size_t i = 0; i < count; i++)
        if (state[i] == PENDING)
            results[i].outcome = ANCHOR_DEPENDENCY_MISSING;
    free(state);
    free(batch);
}

const char *anchor_outcome_description(anchor_outcome outcome) {
    switch (outcome) {
    case ANCHOR_RESOLVED: return "resolved";
    case ANCHOR_STRING_MISSING: return "string not found";
    case ANCHOR_NOT_REFERENCED: return "not referenced by any function";
    case ANCHOR_SEVERAL_FUNCTIONS: return "referenced from more than one function";
    case ANCHOR_DEPENDENCY_MISSING: return "depends on an anchor that did not resolve";
    }
    return "unknown";
}
