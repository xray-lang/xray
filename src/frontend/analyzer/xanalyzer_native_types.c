/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_native_types.c - Parse embedded .xr @native class declarations
 *
 * KEY CONCEPT:
 *   Builtin type interfaces (Array, Map, String, ...) are declared in
 *   stdlib/types/ using @native class syntax.  At build time a script
 *   embeds their source into C string literals (xnative_type_defs.inc).
 *   This module parses those strings once at startup and fills the
 *   builtin member tables used by the analyzer and LSP.
 */

#include "xanalyzer_native_types.h"
#include "xanalyzer_builtins.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include "../../runtime/value/xtype_names.h"
#include "../../runtime/object/xnative_type.h"
#include "../../runtime/class/xclass.h"
#include "../../runtime/symbol/xsymbol_table.h"
#include "../../runtime/xisolate_api.h"
#include "../../base/xlog.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* ========== Embedded .xr sources ========== */

#include "xnative_type_defs.inc"

/* ========== Lightweight text parser ========================================
 *
 * We do NOT use the full Xray parser here — the .xr files have a strict,
 * controlled format.  A line-by-line text parser is simpler, faster, and
 * avoids bootstrapping issues (the full parser needs an XrayIsolate).
 * ===================================================================== */

#define MAX_MEMBERS_PER_TYPE 64

/* Skip ASCII whitespace, return pointer to first non-space. */
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

/* Advance past the current line, return pointer to start of next line. */
static const char *next_line(const char *p) {
    while (*p && *p != '\n')
        p++;
    if (*p == '\n')
        p++;
    return p;
}

/* Read an identifier, return its length.  p must point to first char. */
static int read_ident(const char *p) {
    int len = 0;
    while (isalnum((unsigned char) p[len]) || p[len] == '_')
        len++;
    return len;
}

/* Duplicate a substring into xr_malloc'd memory. */
static char *dup_range(const char *start, int len) {
    XR_DCHECK(start != NULL, "dup_range: NULL start");
    XR_DCHECK(len >= 0, "dup_range: negative len");
    char *s = xr_malloc((size_t) len + 1);
    XR_CHECK(s != NULL, "dup_range: allocation failed");
    memcpy(s, start, (size_t) len);
    s[len] = '\0';
    return s;
}

/* Trim trailing whitespace/newline from a string in-place. */
static void trim_trailing(char *s) {
    int len = (int) strlen(s);
    while (len > 0 &&
           (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

/* ========== Parse one @native class ========== */

/* Parse all members from an @native class source string.
 * Returns heap-allocated array of XaBuiltinMember (caller owns).
 * Sets *out_count. Sets *out_class_name to the class name (heap). */
static XaBuiltinMember *parse_native_class(const char *source, char **out_class_name,
                                           int *out_count) {
    XR_DCHECK(source != NULL, "parse_native_class: NULL source");
    XR_DCHECK(out_class_name != NULL, "parse_native_class: NULL out_class_name");
    XR_DCHECK(out_count != NULL, "parse_native_class: NULL out_count");

    *out_class_name = NULL;
    *out_count = 0;

    /* Scratch buffer for members */
    XaBuiltinMember members[MAX_MEMBERS_PER_TYPE];
    int count = 0;

    const char *p = source;

    /* Find "class <Name>" line */
    while (*p) {
        const char *line = skip_ws(p);

        /* Skip @native annotation line */
        if (*line == '@') {
            p = next_line(p);
            continue;
        }

        /* Skip comment lines */
        if (line[0] == '/' && line[1] == '/') {
            p = next_line(p);
            continue;
        }

        /* Look for "class" keyword */
        if (strncmp(line, "class ", 6) == 0) {
            const char *name_start = line + 6;
            while (*name_start == ' ')
                name_start++;
            int name_len = read_ident(name_start);
            if (name_len > 0) {
                *out_class_name = dup_range(name_start, name_len);
            }
            /* Skip past the opening brace */
            while (*p && *p != '{')
                p++;
            if (*p == '{')
                p++;
            break;
        }

        p = next_line(p);
    }

    if (!*out_class_name)
        return NULL;

    /* Parse member lines until closing brace */
    while (*p && *p != '}') {
        const char *line = skip_ws(p);

        /* Skip empty lines, comments, semicolons */
        if (*line == '\n' || *line == '\0' || *line == '}') {
            p = next_line(p);
            continue;
        }
        if (line[0] == '/' && line[1] == '/') {
            p = next_line(p);
            continue;
        }
        if (*line == ';') {
            p = next_line(p);
            continue;
        }

        XR_CHECK_BOUNDS(count, MAX_MEMBERS_PER_TYPE, "too many members in @native class");

        bool is_static = false;
        if (strncmp(line, "static ", 7) == 0) {
            is_static = true;
            line += 7;
            line = skip_ws(line);
        }

        /* Read member name */
        int name_len = read_ident(line);
        if (name_len == 0) {
            p = next_line(p);
            continue;
        }

        char *member_name = dup_range(line, name_len);
        const char *after_name = line + name_len;

        /* Determine field vs method by looking for ( or < before : */
        bool is_method = false;
        const char *sig_start = after_name;

        if (*after_name == '(' || *after_name == '<') {
            is_method = true;
            /* Signature starts at ( or < — includes generic params */
            if (*after_name == '<') {
                /* Skip generic params to find ( */
                sig_start = after_name;
            }
        } else if (*after_name == ':') {
            is_method = false;
            sig_start = after_name;
        } else {
            /* Unexpected — skip line */
            xr_free(member_name);
            p = next_line(p);
            continue;
        }

        /* Extract signature: from sig_start to end of line (trimmed) */
        const char *eol = p;
        while (*eol && *eol != '\n')
            eol++;
        /* sig_start is within the trimmed line; compute offset from p */
        int sig_offset = (int) (sig_start - p);
        int line_len = (int) (eol - p);
        int sig_len = line_len - sig_offset;
        if (sig_len <= 0) {
            xr_free(member_name);
            p = next_line(p);
            continue;
        }

        char *signature = dup_range(sig_start, sig_len);
        trim_trailing(signature);

        members[count].name = member_name;
        members[count].signature = signature;
        members[count].doc = "";
        members[count].is_method = is_method;
        members[count].is_static = is_static;
        count++;

        p = next_line(p);
    }

    if (count == 0) {
        *out_count = 0;
        return NULL;
    }

    /* Copy to heap-allocated array */
    XaBuiltinMember *result = xr_malloc(sizeof(XaBuiltinMember) * (size_t) count);
    XR_CHECK(result != NULL, "parse_native_class: member array allocation failed");
    memcpy(result, members, sizeof(XaBuiltinMember) * (size_t) count);
    *out_count = count;
    return result;
}

/* ========== Class name → XrTypeId mapping ========== */

typedef struct {
    const char *class_name;
    XrTypeId tid;
    const char *display_name;
} NativeTypeMapping;

/* Maps .xr class names to XrTypeId and display name. */
static const NativeTypeMapping type_mappings[] = {
    {"int", XR_TID_INT, TYPE_NAME_INT},
    {"float", XR_TID_FLOAT, TYPE_NAME_FLOAT},
    {"bool", XR_TID_BOOL, TYPE_NAME_BOOL},
    {"string", XR_TID_STRING, TYPE_NAME_STRING},
    {"Array", XR_TID_ARRAY, TYPE_NAME_ARRAY},
    {"Map", XR_TID_MAP, TYPE_NAME_MAP},
    {"Set", XR_TID_SET, TYPE_NAME_SET},
    {"Json", XR_TID_JSON, TYPE_NAME_JSON},
    {"BigInt", XR_TID_BIGINT, TYPE_NAME_BIGINT},
    {"StringBuilder", XR_TID_STRINGBUILDER, TYPE_NAME_STRINGBUILDER},
    {"Channel", XR_TID_CHANNEL, TYPE_NAME_CHANNEL},
    {"EnumValue", XR_TID_ENUM_VALUE, TYPE_NAME_ENUM_VALUE},
    {"EnumType", XR_TID_ENUM_TYPE, TYPE_NAME_ENUM_TYPE},
    {"Regex", XR_TID_REGEX, TYPE_NAME_REGEX},
    {"Exception", XR_TID_EXCEPTION, TYPE_NAME_EXCEPTION},
    {"Task", XR_TID_COROUTINE, TYPE_NAME_COROUTINE},
    {"WeakMap", XR_TID_WEAKMAP, TYPE_NAME_WEAKMAP},
    {"WeakSet", XR_TID_WEAKSET, TYPE_NAME_WEAKSET},
};

#define NUM_TYPE_MAPPINGS (int) (sizeof(type_mappings) / sizeof(type_mappings[0]))

static XrTypeId class_name_to_tid(const char *name, const char **out_display) {
    XR_DCHECK(name != NULL, "class_name_to_tid: NULL name");
    for (int i = 0; i < NUM_TYPE_MAPPINGS; i++) {
        if (strcmp(name, type_mappings[i].class_name) == 0) {
            if (out_display)
                *out_display = type_mappings[i].display_name;
            return type_mappings[i].tid;
        }
    }
    return XR_TID_NULL;
}

/* ========== Initialization ========== */

/* Runtime-populated builtin type table (indexed by XrTypeId). */
static XaBuiltinType native_builtin_types[XR_TID_COUNT];
static bool native_types_initialized = false;

/* Parse one .xr source; may contain multiple @native classes (e.g. enum.xr). */
static void load_one_source(const char *source) {
    XR_DCHECK(source != NULL, "load_one_source: NULL source");

    const char *p = source;
    while (*p) {
        /* Find next @native annotation */
        const char *at = strstr(p, "@native");
        if (!at)
            break;

        char *class_name = NULL;
        int member_count = 0;
        XaBuiltinMember *members = parse_native_class(at, &class_name, &member_count);

        if (class_name) {
            const char *display_name = NULL;
            XrTypeId tid = class_name_to_tid(class_name, &display_name);

            if (tid != XR_TID_NULL) {
                native_builtin_types[tid].name = display_name;
                native_builtin_types[tid].members = members;
                native_builtin_types[tid].member_count = member_count;
            } else {
                fprintf(stderr, "xray: warning: @native class '%s' has no XrTypeId mapping\n",
                        class_name);
                /* Free unused members */
                if (members) {
                    for (int i = 0; i < member_count; i++) {
                        xr_free((void *) members[i].name);
                        xr_free((void *) members[i].signature);
                    }
                    xr_free(members);
                }
            }
            xr_free(class_name);
        }

        /* Advance past this class to find the next one */
        const char *brace = strchr(at, '{');
        if (brace) {
            /* Skip to closing brace */
            p = brace + 1;
            int depth = 1;
            while (*p && depth > 0) {
                if (*p == '{')
                    depth++;
                else if (*p == '}')
                    depth--;
                p++;
            }
        } else {
            break;
        }
    }
}

XR_FUNC void xa_native_types_init(void) {
    if (native_types_initialized)
        return;

    memset(native_builtin_types, 0, sizeof(native_builtin_types));

    /* Load each embedded .xr source */
#define LOAD_NATIVE(file_name, source_var) load_one_source(source_var);
    XR_NATIVE_TYPE_DEFS(LOAD_NATIVE)
#undef LOAD_NATIVE

    native_types_initialized = true;
}

XR_FUNC bool xa_native_types_ready(void) {
    return native_types_initialized;
}

/* ========== Query API (called by xanalyzer_builtins.c) ========== */

/* Get the populated builtin type table.  Guaranteed non-NULL after init. */
XR_FUNC const XaBuiltinType *xa_native_get_builtin_types(void) {
    XR_DCHECK(native_types_initialized, "xa_native_get_builtin_types: not initialized");
    return native_builtin_types;
}

/* ========== Protocol Verification ==========
 *
 * Compares the .xr-declared methods against the C-registered XrClass
 * methods for each native type.  Logs mismatches as warnings.
 * Only meaningful after all native types have been registered.
 * ===================================================================== */

/* XrTypeId → XrObjType mapping for types registered via xr_register_native_type.
 * Value types (int, float, bool) use a separate dispatch path and
 * are excluded from verification. */
typedef struct {
    XrTypeId tid;
    uint8_t obj_type;
} TidObjMapping;

static const TidObjMapping tid_obj_map[] = {
    {XR_TID_STRING, XR_TSTRING},
    {XR_TID_ARRAY, XR_TARRAY},
    {XR_TID_MAP, XR_TMAP},
    {XR_TID_SET, XR_TSET},
    {XR_TID_JSON, XR_TINSTANCE},
    {XR_TID_BIGINT, XR_TINSTANCE},
    {XR_TID_STRINGBUILDER, XR_TINSTANCE},
    {XR_TID_CHANNEL, XR_TCHANNEL},
    {XR_TID_REGEX, XR_TINSTANCE},
    {XR_TID_EXCEPTION, XR_TINSTANCE},
    {XR_TID_COROUTINE, XR_TTASK},
    {XR_TID_WEAKMAP, XR_TMAP},
    {XR_TID_WEAKSET, XR_TSET},
};

#define NUM_TID_OBJ_MAPPINGS (int) (sizeof(tid_obj_map) / sizeof(tid_obj_map[0]))

XR_FUNC int xa_native_verify_protocol(XrayIsolate *X) {
    if (!X)
        return -1;
    if (!native_types_initialized)
        xa_native_types_init();

    int mismatches = 0;

    for (int m = 0; m < NUM_TID_OBJ_MAPPINGS; m++) {
        XrTypeId tid = tid_obj_map[m].tid;
        uint8_t obj_type = tid_obj_map[m].obj_type;

        const XaBuiltinType *bt = &native_builtin_types[tid];
        if (!bt->members || bt->member_count == 0)
            continue;

        XrClass *cls = xr_isolate_get_native_type_class(X, obj_type);
        if (!cls) {
            /* Type declared in .xr but not registered at runtime */
            xr_log_debug("protocol", "@native class '%s' declared but not registered",
                         bt->name ? bt->name : "?");
            mismatches++;
            continue;
        }

        /* Check each declared method exists in the class */
        for (int i = 0; i < bt->member_count; i++) {
            const XaBuiltinMember *mem = &bt->members[i];
            if (!mem->is_method)
                continue; /* Skip properties — they may be computed */

            SymbolId sym = xr_builtin_symbol_from_name(mem->name);
            if (sym == SYMBOL_INVALID) {
                /* Method name not in builtin symbol table — look up via
                 * isolate symbol table instead */
                sym = xr_symbol_lookup_in_table(xr_isolate_get_symbol_table(X), mem->name);
            }
            if (sym == SYMBOL_INVALID) {
                xr_log_debug("protocol", "'%s.%s' — symbol not interned", bt->name, mem->name);
                mismatches++;
                continue;
            }

            XrMethod *method = xr_class_lookup_method(cls, sym);
            if (!method) {
                xr_log_debug("protocol", "'%s.%s' declared in .xr but missing in C", bt->name,
                             mem->name);
                mismatches++;
            }
        }
    }

    return mismatches;
}
