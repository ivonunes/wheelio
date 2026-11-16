#include "steam_report.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *out;
    size_t size;
    size_t length;
    bool overflowed;
} json_buffer;

static void append(json_buffer *b, const char *format, ...) {
    va_list args;
    va_start(args, format);
    size_t room = b->length < b->size ? b->size - b->length : 0;
    int written = vsnprintf(b->out + b->length, room, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= room) {
        b->overflowed = true;
        b->length = b->size ? b->size - 1 : 0;
    } else {
        b->length += (size_t)written;
    }
}

static void append_string(json_buffer *b, const char *text) {
    if (!text) {
        append(b, "null");
        return;
    }
    append(b, "\"");
    for (const unsigned char *c = (const unsigned char *)text; *c; c++) {
        if (*c == '"' || *c == '\\')
            append(b, "\\%c", *c);
        else if (*c < 0x20)
            append(b, "\\u%04x", *c);
        else
            append(b, "%c", *c);
    }
    append(b, "\"");
}

static void append_gates(json_buffer *b, const steam_patch_plan *plan) {
    append(b, "  \"gates\": [");
    if (!plan) {
        append(b, "]\n");
        return;
    }
    for (int i = 0; i < STEAM_ANCHOR_COUNT; i++) {
        const anchor_result *r = &plan->anchors[i];
        append(b, "\n    {\"name\": ");
        append_string(b, steam_anchors[i].name);
        if (r->outcome == ANCHOR_RESOLVED) {
            append(b, ", \"resolved\": true, \"address\": \"0x%llx\"},",
                   (unsigned long long)r->function);
            continue;
        }
        append(b, ", \"resolved\": false, \"problem\": ");
        append_string(b, anchor_outcome_description(r->outcome));
        if (r->failed_item) {
            append(b, ", \"item\": ");
            append_string(b, r->failed_item);
        }
        append(b, "},");
    }
    append(b, "\n    {\"name\": \"enabled_flag\"");
    if (plan->flag_offset_found) {
        append(b, ", \"resolved\": true, \"offset\": \"0x%x\"}", plan->enabled_flag_offset);
    } else {
        append(b, ", \"resolved\": false, \"problem\": ");
        append_string(b, plan->flag_offset_problem);
        append(b, "}");
    }
    append(b, "\n  ]\n");
}

bool steam_report_json(const steam_report *report, char *out, size_t size) {
    json_buffer b = { .out = out, .size = size };
    if (size)
        out[0] = '\0';
    append(&b, "{\n  \"steam_build\": ");
    append_string(&b, report->steam_build);
    append(&b, ",\n  \"steamclient_uuid\": ");
    append_string(&b, report->steamclient_uuid);
    append(&b, ",\n  \"ready\": %s", report->plan && report->plan->ready ? "true" : "false");
    if (report->has_patch_applied)
        append(&b, ",\n  \"patch_applied\": %s", report->patch_applied ? "true" : "false");
    append(&b, ",\n  \"reason\": ");
    append_string(&b, report->reason);
    append(&b, ",\n");
    append_gates(&b, report->plan);
    append(&b, "}\n");
    return !b.overflowed;
}
