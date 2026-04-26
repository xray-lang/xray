/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_names.c - Type name utility functions
 *
 * KEY CONCEPT:
 *   Maps type name strings to XrObjType enum values.
 */

#include "xtype_names.h"
#include "../../base/xchecks.h"
#include <string.h>

// Defined in xvalue.c — single source of truth for type name strings
XR_DATA const char *typeid_names[XR_TID_COUNT];

// Reverse lookup: type name string → XrTypeId
// Iterates typeid_names[] so adding new types only requires updating the table.
int xr_type_from_name(const char *type_name) {
    if (!type_name)
        return -1;

    for (int i = 0; i < XR_TID_COUNT; i++) {
        if (typeid_names[i] && strcmp(type_name, typeid_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

// Check if type name is valid
int xr_is_valid_type_name(const char *type_name) {
    return xr_type_from_name(type_name) != -1;
}
