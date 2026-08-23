/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnumber_parse_error.h - Stable NumberParseError builtin registry row
 */

#ifndef XNUMBER_PARSE_ERROR_H
#define XNUMBER_PARSE_ERROR_H

#include "xglobal_indices.h"
#include <stdbool.h>
#include <stdint.h>

#define XR_NUMBER_PARSE_ERROR_LAYOUT_ID UINT32_C(3802613823)
#define XR_NUMBER_PARSE_ERROR_NAME "NumberParseError"
#define XR_NUMBER_PARSE_ERROR_NOMINAL_OWNER "prelude"
#define XR_NUMBER_PARSE_ERROR_INVALID_SYNTAX_NAME "InvalidSyntax"
#define XR_NUMBER_PARSE_ERROR_OUT_OF_RANGE_NAME "OutOfRange"

enum {
    XR_NUMBER_PARSE_ERROR_INVALID_SYNTAX = 0,
    XR_NUMBER_PARSE_ERROR_OUT_OF_RANGE = 1,
    XR_NUMBER_PARSE_ERROR_MEMBER_COUNT = 2,
};

typedef struct XrNumberParseErrorRegistryRow {
    uint32_t global_index;
    uint32_t enum_layout_id;
    const char *enum_name;
    const char *nominal_owner;
    const char *members[XR_NUMBER_PARSE_ERROR_MEMBER_COUNT];
} XrNumberParseErrorRegistryRow;

/* This is intentionally one frozen row, not a fallback registry for arbitrary
 * enum globals. Only the global id selects it; names bind source and verify
 * typed metadata but do not select runtime semantics. */
static inline const XrNumberParseErrorRegistryRow *
xr_number_parse_error_registry_row(uint32_t global_index) {
    static const XrNumberParseErrorRegistryRow row = {
        XR_GLOBAL_VAR_NUMBER_PARSE_ERROR,
        XR_NUMBER_PARSE_ERROR_LAYOUT_ID,
        XR_NUMBER_PARSE_ERROR_NAME,
        XR_NUMBER_PARSE_ERROR_NOMINAL_OWNER,
        {XR_NUMBER_PARSE_ERROR_INVALID_SYNTAX_NAME,
         XR_NUMBER_PARSE_ERROR_OUT_OF_RANGE_NAME},
    };
    return global_index == XR_GLOBAL_VAR_NUMBER_PARSE_ERROR ? &row : NULL;
}

static inline int xr_number_parse_error_member_index(const char *name) {
    const XrNumberParseErrorRegistryRow *row =
        xr_number_parse_error_registry_row(XR_GLOBAL_VAR_NUMBER_PARSE_ERROR);
    if (!name || !row)
        return -1;
    for (uint32_t i = 0; i < XR_NUMBER_PARSE_ERROR_MEMBER_COUNT; i++) {
        const char *candidate = row->members[i];
        uint32_t offset = 0;
        while (name[offset] && candidate[offset] && name[offset] == candidate[offset])
            offset++;
        if (name[offset] == candidate[offset])
            return (int) i;
    }
    return -1;
}

_Static_assert(XR_GLOBAL_VAR_NUMBER_PARSE_ERROR == 30,
               "NumberParseError has a frozen runtime global index");

#endif /* XNUMBER_PARSE_ERROR_H */
