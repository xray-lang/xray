/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_builtin_schema.h - Runtime-neutral builtin object field schemas
 */

#ifndef XR_BUILTIN_SCHEMA_H
#define XR_BUILTIN_SCHEMA_H

#include <string.h>

typedef enum XrExceptionFieldIndex {
    EXCEPTION_FIELD_MESSAGE = 0,
    EXCEPTION_FIELD_STACK = 1,
    EXCEPTION_FIELD_CAUSE = 2,
    EXCEPTION_FIELD_CODE = 3,
    EXCEPTION_FIELD_DATA = 4,
    EXCEPTION_FIELD_COUNT = 5,
} XrExceptionFieldIndex;

static inline const char *xr_exception_field_name(int index) {
    static const char *const names[EXCEPTION_FIELD_COUNT] = {
        "message", "stack", "cause", "code", "data",
    };
    if (index < 0 || index >= EXCEPTION_FIELD_COUNT)
        return NULL;
    return names[index];
}

static inline const char *const *xr_exception_field_names(void) {
    static const char *const names[EXCEPTION_FIELD_COUNT] = {
        "message", "stack", "cause", "code", "data",
    };
    return names;
}

static inline int xr_exception_field_index(const char *name) {
    if (!name)
        return -1;
    for (int i = 0; i < EXCEPTION_FIELD_COUNT; i++) {
        const char *field = xr_exception_field_name(i);
        if (field && strcmp(field, name) == 0)
            return i;
    }
    return -1;
}

typedef enum XrProcessFieldIndex {
    PROCESS_FIELD_FILE = 0,
    PROCESS_FIELD_ARGS = 1,
    PROCESS_FIELD_DIR = 2,
    PROCESS_FIELD_COUNT = 3,
} XrProcessFieldIndex;

static inline const char *xr_process_field_name(int index) {
    static const char *const names[PROCESS_FIELD_COUNT] = {
        "file",
        "args",
        "dir",
    };
    if (index < 0 || index >= PROCESS_FIELD_COUNT)
        return NULL;
    return names[index];
}

static inline int xr_process_field_index(const char *name) {
    if (!name)
        return -1;
    for (int i = 0; i < PROCESS_FIELD_COUNT; i++) {
        const char *field = xr_process_field_name(i);
        if (field && strcmp(field, name) == 0)
            return i;
    }
    return -1;
}

#endif  // XR_BUILTIN_SCHEMA_H
