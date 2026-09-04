/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_native_types.c - Load sealed embedded builtin type declarations
 *
 * KEY CONCEPT:
 *   Builtin type interfaces (Array, Map, String, ...) are declared in
 *   stdlib/types/ as signature-only schema. At build time a script
 *   embeds their source into C string literals (xnative_type_defs.inc.c).
 *   This module parses those strings once at startup and fills the
 *   builtin member tables used by the analyzer and LSP.
 */

#include "xanalyzer_native_types.h"
#include "xanalyzer_builtins.h"
#include "xanalyzer_xrd.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include "../../runtime/value/xtype_names.h"
#include "../../runtime/object/xnative_type.h"
#include "../../runtime/class/xclass.h"
#include "../../runtime/class/xclass_system.h"
#include "../../runtime/symbol/xsymbol_table.h"
#include "../../runtime/xisolate_api.h"
#include "../../os/os_thread.h"
#include "../../base/xlog.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdatomic.h>

/* ========== Embedded .xr sources ========== */

#include "xnative_type_defs.inc.c"

/* ========== Lightweight text parser ========================================
 *
 * We do NOT use the full Xray parser here — the .xr files have a strict,
 * controlled format.  A line-by-line text parser is simpler, faster, and
 * avoids bootstrapping issues (the full parser needs an XrVMRuntime).
 * ===================================================================== */

#define MAX_MEMBERS_PER_TYPE 64

typedef struct NativeMemberContractEntry {
    const char *type_name;
    const char *member_name;
    bool is_static;
    XaAllocationContractKind allocation;
    XaEffectContractKind effect;
    const char *errors_csv;
    XaBuiltinReturnOwnership return_ownership;
} NativeMemberContractEntry;

static const NativeMemberContractEntry native_member_contracts[] = {
#define XA_NATIVE_MEMBER_CONTRACT(type, member, is_static, allocation, effect, errors_csv,         \
                                  return_ownership)                                                \
    {type, member, is_static, allocation, effect, errors_csv, return_ownership},
#include "xa_native_member_contract.def"
#undef XA_NATIVE_MEMBER_CONTRACT
};

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

static const NativeMemberContractEntry *
native_member_contract(const char *type_name, const char *member_name, bool is_static) {
    for (size_t i = 0; i < sizeof(native_member_contracts) / sizeof(native_member_contracts[0]);
         i++) {
        const NativeMemberContractEntry *entry = &native_member_contracts[i];
        if (entry->is_static == is_static && strcmp(entry->type_name, type_name) == 0 &&
            strcmp(entry->member_name, member_name) == 0)
            return entry;
    }
    return NULL;
}

static void native_member_apply_contract(const char *type_name, XaBuiltinMember *member) {
    const NativeMemberContractEntry *entry =
        native_member_contract(type_name, member->name, member->is_static);
    member->allocation_contract = entry ? entry->allocation : XA_ALLOCATION_CONTRACT_MISSING;
    member->effect_contract.kind = entry ? entry->effect : XA_EFFECT_CONTRACT_MISSING;
    member->return_ownership = entry ? entry->return_ownership : XA_BUILTIN_RETURN_UNKNOWN;
    if (!entry || entry->effect != XA_EFFECT_CONTRACT_ERRORS || !entry->errors_csv[0])
        return;
    uint32_t count = 1;
    for (const char *p = entry->errors_csv; *p; p++) {
        if (*p == ',')
            count++;
    }
    member->effect_contract.errors = xr_calloc(count, sizeof(const char *));
    XR_CHECK(member->effect_contract.errors != NULL,
             "builtin member contract error-set allocation failed");
    const char *cursor = entry->errors_csv;
    while (*cursor) {
        while (*cursor == ' ')
            cursor++;
        const char *end = strchr(cursor, ',');
        if (!end)
            end = cursor + strlen(cursor);
        const char *trim = end;
        while (trim > cursor && trim[-1] == ' ')
            trim--;
        member->effect_contract.errors[member->effect_contract.error_count++] =
            dup_range(cursor, (int) (trim - cursor));
        cursor = *end ? end + 1 : end;
    }
}

/* ========== Parse one sealed builtin class ========== */

/* Parse all members from a sealed builtin class source string.
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
    memset(members, 0, sizeof(members));
    int count = 0;

    const char *p = source;
    bool next_member_lowered_only = false;

    /* Find "class <Name>" line */
    while (*p) {
        const char *line = skip_ws(p);

        /* Skip compiler-owned directive lines, if a future schema adds any. */
        if (*line == '@') {
            p = next_line(p);
            continue;
        }

        /* Skip comment lines */
        if (line[0] == '/' && line[1] == '/') {
            if (strstr(line, "@lowered"))
                next_member_lowered_only = true;
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
            if (strstr(line, "@lowered"))
                next_member_lowered_only = true;
            p = next_line(p);
            continue;
        }
        if (*line == ';') {
            p = next_line(p);
            continue;
        }

        XR_CHECK_BOUNDS(count, MAX_MEMBERS_PER_TYPE, "too many members in builtin class");

        bool is_static = false;
        if (strncmp(line, "static ", 7) == 0) {
            is_static = true;
            line += 7;
            line = skip_ws(line);
        }
        XrParamMode receiver_mode = XR_PARAM_READ;
        if (strncmp(line, "ref ", 4) == 0) {
            receiver_mode = XR_PARAM_REF;
            line = skip_ws(line + 4);
        } else if (strncmp(line, "move ", 5) == 0) {
            receiver_mode = XR_PARAM_MOVE;
            line = skip_ws(line + 5);
        }
        XR_CHECK(!is_static || receiver_mode == XR_PARAM_READ,
                 "native static method cannot declare a receiver mode");

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
        members[count].is_internal = false;
        members[count].is_lowered_only = next_member_lowered_only;
        members[count].receiver_mode = receiver_mode;
        members[count].is_yieldable = false;
        native_member_apply_contract(*out_class_name, &members[count]);
        next_member_lowered_only = false;
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
    {"i64", XR_TID_I64, TYPE_NAME_I64},
    {"f64", XR_TID_F64, TYPE_NAME_F64},
    {"bool", XR_TID_BOOL, TYPE_NAME_BOOL},
    {"string", XR_TID_STRING, TYPE_NAME_STRING},
    {"rune", XR_TID_RUNE, TYPE_NAME_RUNE},
    {"Array", XR_TID_ARRAY, TYPE_NAME_ARRAY},
    {"Map", XR_TID_MAP, TYPE_NAME_MAP},
    {"Set", XR_TID_SET, TYPE_NAME_SET},
    {"BigInt", XR_TID_BIGINT, TYPE_NAME_BIGINT},
    {"StringBuilder", XR_TID_STRINGBUILDER, TYPE_NAME_STRINGBUILDER},
    {"Iterator", XR_TID_ITERATOR, TYPE_NAME_ITERATOR},
    {"Channel", XR_TID_CHANNEL, TYPE_NAME_CHANNEL},
    {"Regex", XR_TID_REGEX, TYPE_NAME_REGEX},
    {"PanicInfo", XR_TID_PANIC_INFO, TYPE_NAME_PANIC_INFO},
    {"Task", XR_TID_COROUTINE, TYPE_NAME_TASK},
    {"Atomic", XR_TID_ATOMIC, TYPE_NAME_ATOMIC},
    {"Thread", XR_TID_THREAD, TYPE_NAME_THREAD},
    {"Buffer", XR_TID_BUFFER, TYPE_NAME_BUFFER},
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

static bool class_name_is_generated_plain_class(const char *name) {
    XR_DCHECK(name != NULL, "class_name_is_generated_plain_class: NULL name");
    return strcmp(name, "PanicInfo") == 0;
}

/* ========== Initialization ========== */

/* Runtime-populated builtin type table (indexed by XrTypeId). */
static XaBuiltinType native_builtin_types[XR_TID_COUNT];
static XaBuiltinType compiler_builtin_json_namespace;
static XaBuiltinType compiler_builtin_corolocal;
static xr_once_t native_types_once = XR_ONCE_INITIALIZER;
static atomic_bool native_types_initialized = false;

/* Parse one .xr source; it may contain multiple sealed builtin classes. */
static const char *find_next_builtin_class(const char *source) {
    for (const char *line = source; line && *line; line = next_line(line)) {
        const char *trimmed = skip_ws(line);
        if (strncmp(trimmed, "class ", 6) == 0)
            return trimmed;
    }
    return NULL;
}

static void load_one_source(const char *source) {
    XR_DCHECK(source != NULL, "load_one_source: NULL source");

    const char *p = source;
    while (*p) {
        const char *class_decl = find_next_builtin_class(p);
        if (!class_decl)
            break;

        char *class_name = NULL;
        int member_count = 0;
        XaBuiltinMember *members = parse_native_class(class_decl, &class_name, &member_count);

        if (class_name) {
            const char *display_name = NULL;
            XrTypeId tid = class_name_to_tid(class_name, &display_name);

            if (strcmp(class_name, "JSON") == 0) {
                compiler_builtin_json_namespace.name = TYPE_NAME_JSON;
                compiler_builtin_json_namespace.members = members;
                compiler_builtin_json_namespace.member_count = member_count;
            } else if (tid != XR_TID_NULL) {
                native_builtin_types[tid].name = display_name;
                native_builtin_types[tid].members = members;
                native_builtin_types[tid].member_count = member_count;
            } else if (class_name_is_generated_plain_class(class_name)) {
                /* Owned by .def-generated class metadata rather than the legacy
                 * XrTypeId-indexed builtin table. */
                if (members) {
                    for (int i = 0; i < member_count; i++) {
                        xr_free((void *) members[i].name);
                        xr_free((void *) members[i].signature);
                        xa_effect_contract_clear(&members[i].effect_contract);
                    }
                    xr_free(members);
                }
            } else {
                fprintf(stderr, "xray: warning: builtin class '%s' has no XrTypeId mapping\n",
                        class_name);
                /* Free unused members */
                if (members) {
                    for (int i = 0; i < member_count; i++) {
                        xr_free((void *) members[i].name);
                        xr_free((void *) members[i].signature);
                        xa_effect_contract_clear(&members[i].effect_contract);
                    }
                    xr_free(members);
                }
            }
            xr_free(class_name);
        }

        /* Advance past this class to find the next one */
        const char *brace = strchr(class_decl, '{');
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

/* Generated member tables for stdlib definition files rather than .xr declarations.
 * An omitted trailing contract is XA_EFFECT_CONTRACT_MISSING, never nothrow. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "xanalyzer_builtins_generated.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static void xa_native_types_init_once(void) {
    memset(native_builtin_types, 0, sizeof(native_builtin_types));

    /* Load each embedded .xr source */
#define LOAD_NATIVE(file_name, source_var) load_one_source(source_var);
    XR_NATIVE_TYPE_DEFS(LOAD_NATIVE)
#undef LOAD_NATIVE

    /* Inject type members generated from C source (single source of truth). */
#ifdef GEN_BUFFER_MEMBER_COUNT
    native_builtin_types[XR_TID_BUFFER].name = TYPE_NAME_BUFFER;
    native_builtin_types[XR_TID_BUFFER].members = g_gen_buffer_members;
    native_builtin_types[XR_TID_BUFFER].member_count = GEN_BUFFER_MEMBER_COUNT;
#endif
#ifdef GEN_COROLOCAL_MEMBER_COUNT
    compiler_builtin_corolocal.name = "CoroLocal";
    compiler_builtin_corolocal.members = g_gen_corolocal_members;
    compiler_builtin_corolocal.member_count = GEN_COROLOCAL_MEMBER_COUNT;
#endif

    atomic_store_explicit(&native_types_initialized, true, memory_order_release);
}

XR_FUNC void xa_native_types_init(void) {
    xr_once_call(&native_types_once, xa_native_types_init_once);
}

XR_FUNC bool xa_native_types_ready(void) {
    return atomic_load_explicit(&native_types_initialized, memory_order_acquire);
}

/* ========== Query API (called by xanalyzer_builtins.c) ========== */

/* Get the populated builtin type table.  Guaranteed non-NULL after init. */
XR_FUNC const XaBuiltinType *xa_native_get_builtin_types(void) {
    XR_DCHECK(atomic_load_explicit(&native_types_initialized, memory_order_acquire),
              "xa_native_get_builtin_types: not initialized");
    return native_builtin_types;
}

XR_FUNC const XaBuiltinType *xa_native_get_compiler_builtin_type(const char *name) {
    if (!atomic_load_explicit(&native_types_initialized, memory_order_acquire))
        xa_native_types_init();
    if (name && strcmp(name, "CoroLocal") == 0 && compiler_builtin_corolocal.members)
        return &compiler_builtin_corolocal;
    if (name && strcmp(name, "JSON") == 0 && compiler_builtin_json_namespace.members)
        return &compiler_builtin_json_namespace;
    return NULL;
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
    {XR_TID_BOOL, XR_TBOOL},
    {XR_TID_I64, XR_TINT},
    {XR_TID_F64, XR_TFLOAT},
    {XR_TID_STRING, XR_TSTRING},
    {XR_TID_ARRAY, XR_TARRAY},
    {XR_TID_MAP, XR_TMAP},
    {XR_TID_SET, XR_TSET},
    {XR_TID_BIGINT, XR_TINSTANCE},
    {XR_TID_STRINGBUILDER, XR_TINSTANCE},
    {XR_TID_CHANNEL, XR_TCHANNEL},
    {XR_TID_THREAD, XR_TTHREAD},
    {XR_TID_REGEX, XR_TINSTANCE},
    {XR_TID_PANIC_INFO, XR_TINSTANCE},
    {XR_TID_COROUTINE, XR_TTASK},
    {XR_TID_ATOMIC, XR_TATOMIC},
};

#define NUM_TID_OBJ_MAPPINGS (int) (sizeof(tid_obj_map) / sizeof(tid_obj_map[0]))

static XrClass *xa_native_protocol_core_class(XrVMRuntime *X, XrTypeId tid,
                                              const XaBuiltinMember *mem) {
    XrayCoreClasses *core = xr_isolate_get_core_classes(X);
    if (!core)
        return NULL;
    switch (tid) {
        case XR_TID_BIGINT:
            return core->bigintClass;
        case XR_TID_STRINGBUILDER:
            return core->stringBuilderClass;
        case XR_TID_ITERATOR:
            return core->iteratorClass;
        case XR_TID_REGEX:
            return core->regexClass;
        case XR_TID_PANIC_INFO:
            return core->panicInfoClass;
        case XR_TID_BUFFER:
            return core->memBufferClass;
        default:
            return NULL;
    }
}

static bool xa_native_protocol_intrinsic_method(XrTypeId tid, const char *name, bool is_static) {
    if (!name || is_static)
        return false;
    if (tid == XR_TID_RUNE)
        return true;
    if (tid == XR_TID_CHANNEL) {
        static const char *channel_methods[] = {
            "send",        "recv",        "recvOr", "trySend", "tryRecv",
            "sendTimeout", "recvTimeout", "close",  NULL,
        };
        for (int i = 0; channel_methods[i]; i++) {
            if (strcmp(name, channel_methods[i]) == 0)
                return true;
        }
    }
    if (tid == XR_TID_COROUTINE) {
        static const char *task_methods[] = {
            "cancel", "poll", "awaitResult", "awaitTimeout", NULL,
        };
        for (int i = 0; task_methods[i]; i++) {
            if (strcmp(name, task_methods[i]) == 0)
                return true;
        }
    }
    if (tid == XR_TID_BUFFER) {
        static const char *buffer_methods[] = {
            "length", "asBytes", "asMutBytes", "borrowPtr", "resize", NULL,
        };
        for (int i = 0; buffer_methods[i]; i++) {
            if (strcmp(name, buffer_methods[i]) == 0)
                return true;
        }
    }
    return false;
}

static XrClass *xa_native_protocol_runtime_class(XrVMRuntime *X, XrTypeId tid,
                                                 const XaBuiltinMember *mem) {
    XrClass *core_cls = xa_native_protocol_core_class(X, tid, mem);
    if (core_cls)
        return core_cls;
    for (int m = 0; m < NUM_TID_OBJ_MAPPINGS; m++) {
        if (tid_obj_map[m].tid == tid)
            return xr_isolate_get_native_type_class(X, tid_obj_map[m].obj_type);
    }
    return NULL;
}

XR_FUNC int xa_native_verify_protocol(XrVMRuntime *X) {
    if (!X)
        return -1;
    xa_native_types_init();

    int mismatches = 0;

    for (int type_index = -1; type_index < XR_TID_COUNT; type_index++) {
        const bool is_json_surface = type_index < 0;
        const XrTypeId tid = is_json_surface ? XR_TID_OBJECT : (XrTypeId) type_index;
        const XaBuiltinType *bt =
            is_json_surface ? &compiler_builtin_json_namespace : &native_builtin_types[type_index];
        if (!bt->members || bt->member_count == 0)
            continue;

        /* Check each declared method exists in the class */
        for (int i = 0; i < bt->member_count; i++) {
            const XaBuiltinMember *mem = &bt->members[i];
            if (!mem->is_method)
                continue; /* Skip properties — they may be computed */
            if (mem->is_lowered_only)
                continue; /* Explicit compiler/VM lowering surface. */
            if (xa_native_protocol_intrinsic_method(tid, mem->name, mem->is_static))
                continue; /* Runtime-dispatched by xvm_invoke/bytecode ops, not XrClass. */

            XrayCoreClasses *core = is_json_surface ? xr_isolate_get_core_classes(X) : NULL;
            XrClass *cls = is_json_surface
                               ? (core && mem && mem->is_static ? core->jsonClass : NULL)
                               : xa_native_protocol_runtime_class(X, tid, mem);
            if (!cls) {
                xr_log_warning("protocol", "native class '%s' has no runtime method table for %s",
                               bt->name ? bt->name : "?", mem->name ? mem->name : "?");
                mismatches++;
                continue;
            }

            SymbolId sym = xr_builtin_symbol_from_name(mem->name);
            if (sym == SYMBOL_INVALID) {
                /* Method name not in builtin symbol table — look up via
                 * isolate symbol table instead */
                sym = xr_symbol_lookup_in_table(xr_isolate_get_symbol_table(X), mem->name);
            }
            if (sym == SYMBOL_INVALID) {
                xr_log_warning("protocol", "'%s.%s' symbol not interned", bt->name, mem->name);
                mismatches++;
                continue;
            }

            XrMethod *method = xr_class_lookup_method(cls, sym);
            if (!method) {
                xr_log_warning("protocol", "'%s.%s' declared in .xr but missing in C", bt->name,
                               mem->name);
                mismatches++;
            }
        }
    }

    return mismatches;
}
