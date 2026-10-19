/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbuiltin_enum.h - Immutable builtin enum declarations from the prelude registry
 */

#ifndef XBUILTIN_ENUM_H
#define XBUILTIN_ENUM_H

#include "xglobal_indices.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_BUILTIN_ENUM_MAX_MEMBERS 8

typedef struct XrBuiltinEnumMember {
    const char *name;
    bool has_payload;
} XrBuiltinEnumMember;

typedef struct XrBuiltinEnumRow {
    int builtin_index;
    const char *enum_name;
    uint32_t member_count;
    XrBuiltinEnumMember members[XR_BUILTIN_ENUM_MAX_MEMBERS];
} XrBuiltinEnumRow;

static inline const XrBuiltinEnumRow *xr_builtin_enum_registry(size_t *count) {
    static const XrBuiltinEnumRow rows[] = {
#define XR_BUILTIN_ENUM(ename, earity, slot, variants)                                             \
    {XR_GLOBAL_VAR_##slot,                                                                         \
     ename,                                                                                        \
     sizeof((const XrBuiltinEnumMember[]) {variants}) / sizeof(XrBuiltinEnumMember),               \
     {variants}},
#define XR_BUILTIN_ENUM_VARIANT(name, payload) {name, XR_ENUM_PAYLOAD_##payload},
#define XR_ENUM_PAYLOAD_NONE false
#define XR_ENUM_PAYLOAD_TYPE_PARAM_0 true
#define XR_ENUM_PAYLOAD_ERROR true
#include "../../stdlib/prelude/builtin_symbols.def"
#undef XR_ENUM_PAYLOAD_NONE
#undef XR_ENUM_PAYLOAD_TYPE_PARAM_0
#undef XR_ENUM_PAYLOAD_ERROR
    };
    if (count)
        *count = sizeof(rows) / sizeof(rows[0]);
    return rows;
}

static inline const XrBuiltinEnumRow *xr_builtin_enum_registry_row(int builtin_index) {
    size_t count = 0;
    const XrBuiltinEnumRow *rows = xr_builtin_enum_registry(&count);
    for (size_t i = 0; i < count; i++)
        if (rows[i].builtin_index == builtin_index)
            return &rows[i];
    return NULL;
}

static inline bool xr_builtin_enum_row_is_unit(const XrBuiltinEnumRow *row) {
    if (!row || !row->member_count || row->member_count > XR_BUILTIN_ENUM_MAX_MEMBERS)
        return false;
    for (uint32_t i = 0; i < row->member_count; i++) {
        if (!row->members[i].name || row->members[i].has_payload)
            return false;
    }
    return true;
}

#endif  // XBUILTIN_ENUM_H
