/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_cycle_report.c - see xlsp_cycle_report.h
 */

#include "xlsp_cycle_report.h"
#include "xlsp_utils.h"
#include "../../base/xjson.h"
#include "../../base/xmalloc.h"
#include "../../frontend/parser/xast.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* One candidate edge, already reduced to the identities that exist in source:
 * the class that holds the reference and the field it holds it in. Object
 * addresses stay in the detector's own report — they mean nothing here. */
typedef struct {
    char *from_class;
    char *field;
    char *to_class;
    bool weak_annotatable;
    uint32_t objects; /* size of the cycle this edge is part of */
    uint64_t bytes;
} XlspCycleEdge;

struct XlspCycleReport {
    XlspCycleEdge *edges;
    int count;
    int cap;
    time_t mtime;
    char *path;
};

static const char *SIDECAR_NAME = ".xray-cycles.json";

static void report_free(struct XlspCycleReport *r) {
    if (!r)
        return;
    for (int i = 0; i < r->count; i++) {
        xr_free(r->edges[i].from_class);
        xr_free(r->edges[i].field);
        xr_free(r->edges[i].to_class);
    }
    xr_free(r->edges);
    xr_free(r->path);
    xr_free(r);
}

void xlsp_cycle_report_clear(XrLspServer *server) {
    if (!server)
        return;
    report_free(server->cycle_report);
    server->cycle_report = NULL;
}

static void report_push(struct XlspCycleReport *r, const char *from, const char *field,
                        const char *to, bool annotatable, uint32_t objects, uint64_t bytes) {
    if (r->count == r->cap) {
        int cap = r->cap ? r->cap * 2 : 16;
        XlspCycleEdge *grown =
            (XlspCycleEdge *) xr_realloc(r->edges, (size_t) cap * sizeof(*grown));
        if (!grown)
            return;
        r->edges = grown;
        r->cap = cap;
    }
    XlspCycleEdge *e = &r->edges[r->count];
    e->from_class = xr_strdup(from);
    e->field = xr_strdup(field);
    e->to_class = xr_strdup(to ? to : "?");
    e->weak_annotatable = annotatable;
    e->objects = objects;
    e->bytes = bytes;
    r->count++;
}

static char *read_whole_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    /* A report this large is not a report, it is a runaway loop. Refuse rather
     * than stall the editor parsing it. */
    if (size < 0 || size > 16 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *) xr_malloc((size_t) size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t) size, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len)
        *out_len = got;
    return buf;
}

void xlsp_cycle_report_refresh(XrLspServer *server) {
    if (!server || server->workspace_folder_count == 0)
        return;
    const char *root = server->workspace_folders[0].path;
    if (!root || !*root)
        return;

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", root, SIDECAR_NAME);

    struct stat st;
    if (stat(path, &st) != 0) {
        /* Report deleted (a clean run, or the user removed it): drop what we
         * had, so stale diagnostics do not outlive the leak they describe. */
        xlsp_cycle_report_clear(server);
        return;
    }
    if (server->cycle_report && server->cycle_report->mtime == st.st_mtime)
        return;

    size_t len = 0;
    char *text = read_whole_file(path, &len);
    if (!text)
        return;
    XrJsonValue *root_json = xjson_parse(text, len);
    xr_free(text);
    if (!root_json)
        return;

    struct XlspCycleReport *r = (struct XlspCycleReport *) xr_calloc(1, sizeof(*r));
    if (!r) {
        xjson_free(root_json);
        return;
    }
    r->mtime = st.st_mtime;
    r->path = xr_strdup(path);

    XrJsonValue *cycles = xjson_get(root_json, "cycles");
    if (cycles && cycles->type == XR_JSON_ARRAY) {
        for (int i = 0; i < xjson_array_len(cycles); i++) {
            XrJsonValue *cycle = xjson_array_get(cycles, i);
            uint32_t objects = (uint32_t) xjson_get_int(cycle, "objects");
            uint64_t bytes = (uint64_t) xjson_get_int(cycle, "bytes");
            XrJsonValue *edges = xjson_get(cycle, "edges");
            if (!edges || edges->type != XR_JSON_ARRAY)
                continue;
            for (int j = 0; j < xjson_array_len(edges); j++) {
                XrJsonValue *edge = xjson_array_get(edges, j);
                const char *from = xjson_get_string(edge, "from");
                const char *field = xjson_get_string(edge, "field");
                /* No field name means no declaration to annotate — a container
                 * element or a closure capture. Those are reported by the
                 * detector but there is nothing for a code action to edit. */
                if (!from || !field)
                    continue;
                report_push(r, from, field, xjson_get_string(edge, "to"),
                            xjson_get_bool(edge, "weak_annotatable"), objects, bytes);
            }
        }
    }
    xjson_free(root_json);

    xlsp_cycle_report_clear(server);
    server->cycle_report = r;
}

/* ---------- Diagnostics ---------- */

static const XlspCycleEdge *report_lookup(const struct XlspCycleReport *r, const char *cls,
                                          const char *field) {
    for (int i = 0; i < r->count; i++) {
        if (strcmp(r->edges[i].from_class, cls) == 0 && strcmp(r->edges[i].field, field) == 0)
            return &r->edges[i];
    }
    return NULL;
}

static void emit_field_diagnostic(XrJsonValue *diagnostics, AstNode *field_node,
                                  const char *class_name, const XlspCycleEdge *edge) {
    int line = field_node->line > 0 ? field_node->line - 1 : 0;
    int col = field_node->column > 0 ? field_node->column - 1 : 0;
    int name_len =
        field_node->as.field_decl.name ? (int) strlen(field_node->as.field_decl.name) : 1;

    char message[512];
    /* Say what breaks and what it costs, then what the choice MEANS. Which edge
     * to break is an ownership decision (247 section 2.5) and the message has
     * to give the reader enough to make it before clicking anything. */
    snprintf(message, sizeof(message),
             XLSP_CYCLE_DIAG_PREFIX
             "`%s.%s` is on a cycle that kept %u object(s) / %llu bytes alive past teardown.\n"
             "Marking this field `weak` breaks the cycle, and asserts that %s does NOT own the %s "
             "it points at — the field reads as null once the target is gone.",
             class_name, edge->field, edge->objects, (unsigned long long) edge->bytes, class_name,
             edge->to_class);

    XrJsonValue *diag = xjson_new_object();
    xjson_object_set(diag, "range", xjson_make_range(line, col, line, col + name_len));
    /* A warning, not an error: the detector is a development-build observation
     * of one run, and a cycle inside a coroutine is bounded, not fatal. */
    xjson_object_set(diag, "severity", xjson_new_number(2));
    xjson_object_set(diag, "source", xjson_new_string("xray"));
    xjson_object_set(diag, "code", xjson_new_string("reference-cycle"));
    xjson_object_set(diag, "message", xjson_new_string(message));
    xjson_array_push(diagnostics, diag);
}

static void walk_for_classes(AstNode *node, const struct XlspCycleReport *report,
                             XrJsonValue *diagnostics) {
    if (!node)
        return;
    switch (node->type) {
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL:
        case AST_UNION_DECL: {
            const char *cls = node->as.class_decl.name;
            if (!cls)
                break;
            for (int i = 0; i < node->as.class_decl.field_count; i++) {
                AstNode *f = node->as.class_decl.fields[i];
                if (!f || f->type != AST_FIELD_DECL || !f->as.field_decl.name)
                    continue;
                /* Already annotated: the cycle in the report predates the fix. */
                if (f->as.field_decl.is_weak)
                    continue;
                const XlspCycleEdge *edge = report_lookup(report, cls, f->as.field_decl.name);
                if (edge && edge->weak_annotatable)
                    emit_field_diagnostic(diagnostics, f, cls, edge);
            }
            break;
        }
        case AST_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++)
                walk_for_classes(node->as.program.statements[i], report, diagnostics);
            break;
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++)
                walk_for_classes(node->as.block.statements[i], report, diagnostics);
            break;
        default:
            break;
    }
}

void xlsp_cycle_report_diagnostics(XrLspDocument *doc, XrJsonValue *diagnostics) {
    if (!doc || !doc->server || !diagnostics || diagnostics->type != XR_JSON_ARRAY)
        return;
    const struct XlspCycleReport *report = doc->server->cycle_report;
    if (!report || report->count == 0 || !doc->ast)
        return;
    walk_for_classes(doc->ast, report, diagnostics);
}
