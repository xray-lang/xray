/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvalue_print.c - Value printing and dump
 *
 * KEY CONCEPT:
 *   xr_value_fprint/print/println delegate to xr_value_to_string()
 *   for unified output. xr_value_dump provides indented pretty-print
 *   for container types (Array, Map, Set, Json, Instance), also
 *   delegating leaf types to xr_value_to_string().
 */

#include "xvalue_print.h"
#include "../../base/xchecks.h"
#include "../object/xstring.h"
#include "../class/xinstance.h"
#include "../class/xclass.h"
#include "../object/xjson.h"
#include "../object/xarray.h"
#include "../object/xmap.h"
#include "../object/xset.h"
#include "../object/xtuple.h"
#include "../xisolate_api.h"
#include "../../base/xconstants.h"
#include "../symbol/xsymbol_table.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../xstdlib_bridge.h"
#include "xvalue_format.h"

// Max recursion depth for dump functions
#define XR_PRINT_MAX_DEPTH 5

// ========== Core Implementation ==========

// Delegate to xr_value_to_string for unified formatting

// Print value to file stream
// Delegates to xr_value_to_string for consistent output across print/toString/string+
void xr_value_fprint(FILE *stream, XrValue value) {
    XrayIsolate *X = xray_isolate_current();
    if (X) {
        XrString *str = xr_value_to_string(X, value);
        fprintf(stream, "%s", str->data);
    } else {
        // Fallback when no isolate (early init or shutdown)
        if (XR_IS_INT(value))
            fprintf(stream, "%lld", (long long) XR_TO_INT(value));
        else if (XR_IS_FLOAT(value))
            fprintf(stream, "%g", XR_TO_FLOAT(value));
        else if (XR_IS_BOOL(value))
            fprintf(stream, "%s", XR_TO_BOOL(value) ? "true" : "false");
        else if (XR_IS_NULL(value))
            fprintf(stream, "null");
        else if (XR_IS_STRING(value)) {
            XrString *_s = XR_TO_STRING(value);
            fprintf(stream, "%.*s", (int) _s->length, _s->data);
        } else
            fprintf(stream, "<value>");
    }
}

// Print value to stdout
void xr_value_print(XrValue value) {
    xr_value_fprint(stdout, value);
}

// Print value with newline
void xr_value_println(XrValue value) {
    xr_value_print(value);
    printf("\n");
}

// ========== Formatted Print (dump) ==========

// Dump context
typedef struct {
    int indent;  // Indent spaces
    int depth;   // Current depth
} DumpContext;

// Forward declaration
static void dump_value_internal(XrValue value, DumpContext *ctx);

// Print indent
static void dump_indent(DumpContext *ctx) {
    if (ctx->indent > 0) {
        int spaces = ctx->depth * ctx->indent;
        for (int i = 0; i < spaces; i++) {
            putchar(' ');
        }
    }
}

// Print newline and indent
static void dump_newline(DumpContext *ctx) {
    if (ctx->indent > 0) {
        putchar('\n');
        dump_indent(ctx);
    }
}

// Dump Array
static void dump_array(XrArray *arr, DumpContext *ctx) {
    XR_DCHECK(arr != NULL, "dump_array: NULL array");
    XR_DCHECK(XR_GC_GET_TYPE(&arr->gc) == XR_TARRAY, "dump_array: object is not an array");
    printf("[");
    int32_t count = arr->length;
    if (count == 0) {
        printf("]");
        return;
    }

    ctx->depth++;
    for (int32_t i = 0; i < count; i++) {
        if (i > 0)
            printf(",");
        dump_newline(ctx);
        dump_value_internal(xr_array_get_element(arr, i), ctx);
    }
    ctx->depth--;
    dump_newline(ctx);
    printf("]");
}

// Dump tuple: render with parentheses (a, b, c), matching the literal
// syntax. Unary tuple uses the canonical trailing comma form so the
// print round-trip preserves the arity distinction from a scalar.
static void dump_tuple(XrTuple *tup, DumpContext *ctx) {
    XR_DCHECK(tup != NULL, "dump_tuple: NULL tuple");
    printf("(");
    uint16_t n = xr_tuple_arity(tup);
    if (n == 0) {
        printf(")");
        return;
    }
    ctx->depth++;
    for (uint16_t i = 0; i < n; i++) {
        if (i > 0)
            printf(", ");
        dump_value_internal(tup->elements[i], ctx);
    }
    if (n == 1)
        printf(",");
    ctx->depth--;
    printf(")");
}

// Dump Map
static void dump_map(XrMap *map, DumpContext *ctx) {
    XR_DCHECK(map != NULL, "dump_map: NULL map");
    XR_DCHECK(XR_GC_GET_TYPE(&map->gc) == XR_TMAP, "dump_map: object is not a map");
    if (map->count == 0) {
        printf("#{}");
        return;
    }
    printf("#{");
    ctx->depth++;
    size_t output = 0;
    size_t size = xr_map_sizenode(map);
    for (size_t i = 0; i < size; i++) {
        XrMapNode *node = xr_map_node(map, i);
        if (!XR_MAP_NODE_EMPTY(node)) {
            if (output > 0)
                printf(",");
            dump_newline(ctx);

            // Print key
            dump_value_internal(node->key, ctx);
            printf(" => ");
            // Print value
            dump_value_internal(node->value, ctx);
            output++;
        }
    }
    ctx->depth--;
    dump_newline(ctx);
    printf("}");
}

// Dump Set
static void dump_set(XrSet *set, DumpContext *ctx) {
    XR_DCHECK(set != NULL, "dump_set: NULL set");
    XR_DCHECK(XR_GC_GET_TYPE(&set->gc) == XR_TSET, "dump_set: object is not a set");
    printf("#[");
    if (set->count == 0) {
        printf("]");
        return;
    }

    ctx->depth++;
    size_t output = 0;
    for (size_t i = 0; i < set->capacity; i++) {
        XrSetEntry *entry = &set->entries[i];
        if (entry->state & XR_SET_VALID) {
            if (output > 0)
                printf(",");
            dump_newline(ctx);
            dump_value_internal(entry->value, ctx);
            output++;
        }
    }
    ctx->depth--;
    dump_newline(ctx);
    printf("]");
}

// Dump Json
static void dump_json(XrJson *json, DumpContext *ctx) {
    printf("{");
    if (!json) {
        printf("}");
        return;
    }

    XrClass *cls = json->klass;
    if (!cls || cls->field_count == 0) {
        printf("}");
        return;
    }

    ctx->depth++;
    for (uint16_t i = 0; i < cls->field_count; i++) {
        if (i > 0)
            printf(",");
        dump_newline(ctx);

        const char *name = cls->fields[i].name;
        if (name) {
            printf("%s: ", name);
        } else {
            printf("field%d: ", i);
        }

        XrValue fval = xr_instance_get_dynamic_field(json, i);
        dump_value_internal(fval, ctx);
    }
    ctx->depth--;
    dump_newline(ctx);
    printf("}");
}

// Dump Instance
static void dump_instance(XrInstance *inst, DumpContext *ctx) {
    XrClass *cls = xr_instance_get_class(inst);
    if (!cls) {
        printf("<instance>");
        return;
    }

    printf("%s {", cls->name ? cls->name : "<anonymous>");

    if (cls->field_count == 0) {
        printf("}");
        return;
    }

    ctx->depth++;
    for (uint16_t i = 0; i < cls->field_count; i++) {
        if (cls->fields[i].flags & XR_FIELD_STATIC)
            continue;

        if (i > 0)
            printf(",");
        dump_newline(ctx);

        const char *name = cls->fields[i].name;
        printf("%s: ", name ? name : "?");

        XrValue field_val = xr_instance_get_field_fast(inst, i);
        dump_value_internal(field_val, ctx);
    }
    ctx->depth--;
    dump_newline(ctx);
    printf("}");
}

// Recursively print value
// Container types (Array, Map, Set, Json, Instance) use indented formatting.
// All other types delegate to xr_value_to_string for consistent output.
static void dump_value_internal(XrValue value, DumpContext *ctx) {
    if (ctx->depth > XR_PRINT_MAX_DEPTH) {
        printf("...");
        return;
    }

    // Strings in dump always have quotes
    if (XR_IS_STRING(value)) {
        printf("\"%s\"", XR_TO_STRING(value)->data);
        return;
    }

    // Container types: indented formatting
    if (XR_IS_PTR(value)) {
        XrGCHeader *gc = (XrGCHeader *) XR_TO_PTR(value);
        XrObjType type = XR_GC_GET_TYPE(gc);
        switch (type) {
            case XR_TARRAY:
                dump_array((XrArray *) gc, ctx);
                return;
            case XR_TMAP:
                dump_map((XrMap *) gc, ctx);
                return;
            case XR_TSET:
                dump_set((XrSet *) gc, ctx);
                return;
            case XR_TINSTANCE: {
                XrInstance *inst = (XrInstance *) gc;
                if (inst->klass && inst->klass->builtin_kind == XR_BK_JSON) {
                    dump_json((XrJson *) gc, ctx);
                    return;
                }
                if (inst->klass && inst->klass->builtin_kind == XR_BK_TUPLE) {
                    dump_tuple((XrTuple *) gc, ctx);
                    return;
                }
                XrayIsolate *X = xray_isolate_current();
                if (X && xr_value_is_datetime(X, value)) {
                    void *dt = xr_instance_native_body(inst);
                    char buf[64];
                    int n = xr_datetime_format(dt, XR_DATETIME_DEFAULT_FORMAT, buf, sizeof(buf));
                    if (n > 0)
                        printf("%s", buf);
                    else
                        printf("<DateTime>");
                } else {
                    dump_instance(inst, ctx);
                }
                return;
            }
            default:
                break;
        }
    }

    // All other types: delegate to xr_value_to_string, with inline fallback
    XrayIsolate *X = xray_isolate_current();
    if (X) {
        XrString *str = xr_value_to_string(X, value);
        printf("%s", str->data);
    } else if (XR_IS_INT(value)) {
        printf("%lld", (long long) XR_TO_INT(value));
    } else if (XR_IS_FLOAT(value)) {
        printf("%g", XR_TO_FLOAT(value));
    } else if (XR_IS_BOOL(value)) {
        printf("%s", XR_TO_BOOL(value) ? "true" : "false");
    } else if (XR_IS_NULL(value)) {
        printf("null");
    } else {
        printf("<value>");
    }
}

// Formatted print value (with indentation)
// Maintains xray native syntax format
void xr_value_dump(XrValue value, int indent) {
    if (indent < 0)
        indent = 0;
    if (indent > 8)
        indent = 8;

    DumpContext ctx = {.indent = indent, .depth = 0};
    dump_value_internal(value, &ctx);
    printf("\n");
}
