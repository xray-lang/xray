/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_struct_name.c - shared AOT native struct C type naming
 */

#include "xaot_struct_name.h"
#include "xaot_layout_gen.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static bool struct_c_identifier_is_valid(const char *s) {
    if (!s || !((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || *s == '_'))
        return false;
    for (const char *p = s + 1; *p; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '_'))
            return false;
    }
    return true;
}

static bool struct_c_identifier_is_reserved(const char *s) {
    static const char *const reserved[] = {
        "alignas",       "alignof",  "auto",           "bool",          "break",    "case",
        "char",          "const",    "continue",       "default",       "do",       "double",
        "else",          "enum",     "extern",         "false",         "float",    "for",
        "goto",          "if",       "inline",         "int",           "long",     "register",
        "restrict",      "return",   "short",          "signed",        "sizeof",   "static",
        "static_assert", "struct",   "switch",         "thread_local",  "true",     "typedef",
        "union",         "unsigned", "void",           "volatile",      "while",    "_Alignas",
        "_Alignof",      "_Atomic",  "_Bool",          "_Complex",      "_Generic", "_Imaginary",
        "_Noreturn",     "_Pragma",  "_Static_assert", "_Thread_local",
    };
    if (!s)
        return true;
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        if (strcmp(s, reserved[i]) == 0)
            return true;
    }
    return false;
}

static bool struct_source_field_name_is_usable(const XrStructLayout *sl, int64_t idx) {
    if (!sl || idx < 0 || idx >= sl->field_count || !sl->field_names)
        return false;
    const char *name = sl->field_names[idx];
    if (!struct_c_identifier_is_valid(name) || struct_c_identifier_is_reserved(name))
        return false;
    for (uint16_t i = 0; i < sl->field_count; i++) {
        if ((int64_t) i == idx)
            continue;
        const char *other = sl->field_names[i];
        if (other && struct_c_identifier_is_valid(other) &&
            !struct_c_identifier_is_reserved(other) && strcmp(name, other) == 0)
            return false;
    }
    return true;
}

static void struct_field_c_name(const XrStructLayout *sl, int64_t idx, char *buf, size_t buflen) {
    if (!buf || buflen == 0)
        return;
    if (struct_source_field_name_is_usable(sl, idx)) {
        const char *name = sl->field_names[idx];
        if (strlen(name) < buflen) {
            snprintf(buf, buflen, "%s", name);
            return;
        }
    }
    snprintf(buf, buflen, "f%d", (int) idx);
}

static uint64_t struct_hash_string(uint64_t h, const char *s) {
    if (!s)
        return h;
    for (const unsigned char *p = (const unsigned char *) s; *p; p++) {
        h ^= *p;
        h *= UINT64_C(1099511628211);
    }
    h ^= UINT64_C(0xff);
    h *= UINT64_C(1099511628211);
    return h;
}

static uint64_t struct_layout_hash_depth(const XrStructLayout *sl, int depth) {
    uint64_t h = UINT64_C(1469598103934665603);
    if (!sl)
        return h;
    if (depth > 8)
        return h ^ UINT64_C(0x9e3779b97f4a7c15);
    h ^= sl->field_count;
    h *= UINT64_C(1099511628211);
    h ^= sl->repr;
    h *= UINT64_C(1099511628211);
    h ^= sl->explicit_align;
    h *= UINT64_C(1099511628211);
    for (uint16_t i = 0; i < sl->field_count; i++) {
        char fname[128];
        struct_field_c_name(sl, i, fname, sizeof(fname));
        h = struct_hash_string(h, fname);
        h ^= sl->fields[i].native_type;
        h *= UINT64_C(1099511628211);
        h ^= sl->fields[i].elem_native_type;
        h *= UINT64_C(1099511628211);
        h ^= sl->fields[i].elem_count;
        h *= UINT64_C(1099511628211);
        h ^= sl->fields[i].size;
        h *= UINT64_C(1099511628211);
        h ^= sl->fields[i].sub_layout_id;
        h *= UINT64_C(1099511628211);
        if (sl->fields[i].native_type == XR_NATIVE_STRUCT) {
            h ^= struct_layout_hash_depth(sl->fields[i].sub_layout, depth + 1);
            h *= UINT64_C(1099511628211);
        }
    }
    return h;
}

XR_FUNC uint64_t xaot_struct_layout_hash(const XrStructLayout *sl) {
    return struct_layout_hash_depth(sl, 0);
}

XR_FUNC void xaot_struct_c_type_name(char *buf, size_t buflen, const char *prefix,
                                     const XrStructLayout *sl) {
    if (!buf || buflen == 0)
        return;
    snprintf(buf, buflen, "xrt_struct_%s_%016" PRIx64, prefix ? prefix : "mod",
             xaot_struct_layout_hash(sl));
}
