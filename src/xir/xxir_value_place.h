/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_value_place.h - Synchronous access to an existing owned payload root
 *
 * KEY CONCEPT:
 *   A place borrows owner storage only for the duration of a checked operation.
 */
#ifndef XXIR_VALUE_PLACE_H
#define XXIR_VALUE_PLACE_H
#include "xxir_value.h"
#include "xxir_fault.h"
typedef struct XrXirValuePlace {
    XrXirType type;
    void *payload;
} XrXirValuePlace;
typedef enum XrXirValuePathKind {
    XR_XIR_PATH_FIELD,
    XR_XIR_PATH_INDEX
} XrXirValuePathKind;
typedef struct XrXirValuePathStep {
    XrXirValuePathKind kind;
    XrXirType container;
    int64_t selector;
} XrXirValuePathStep;
typedef struct XrXirValuePath {
    const XrXirValuePathStep *steps;
    uint32_t count;
} XrXirValuePath;
/* Callers own checked declaration authority. Steps contain values, never
 * persistent interior addresses; access resolves the current root. */
XR_FUNC XrXirValueStatus xr_xir_value_path_read(const XrXirValuePlace *root,
    const XrXirValuePath *path, XrXirValueAdmission *admission,
    XrXirValue *output, XrXirFaultDetail *fault);
XR_FUNC XrXirValueStatus xr_xir_value_path_write(const XrXirValuePlace *root,
    const XrXirValuePath *path, const XrXirValue *value,
    XrXirValueAdmission *admission, XrXirFaultDetail *fault);
XR_FUNC XrXirValueStatus xr_xir_value_path_push(const XrXirValuePlace *root,
    const XrXirValuePath *path, const XrXirValue *value,
    XrXirValueAdmission *admission, XrXirFaultDetail *fault);
XR_FUNC XrXirValueStatus xr_xir_cell_value_place(const XrXirValue *cell,
    XrXirValueAdmission *admission, XrXirValuePlace *output);
#endif // XXIR_VALUE_PLACE_H
