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
/* The caller pins a real writable root and authenticates its complete loan.
 * Failure preserves the root. A current admission Domain is required; missing
 * admission or owner is rejected before charging work or changing storage. */
XR_FUNC XrXirValueStatus xr_xir_string_place_append(const XrXirValuePlace *place,
    const XrXirValue *suffix, XrXirValueAdmission *admission);
/* A caller-owned UTF-8 span is consumed synchronously, with no suffix object.
 * Validation and copying debit the same finite work and preserve failed roots. */
XR_FUNC XrXirValueStatus xr_xir_string_place_append_utf8(const XrXirValuePlace *place,
    const char *bytes, size_t length, XrXirValueAdmission *admission);
typedef enum XrXirValuePathKind {
    XR_XIR_PATH_FIELD,
    XR_XIR_PATH_INDEX,
    /* Immutable wrapper selectors are observation facts. Value mutation and
     * Cell projection do not accept them as writable storage paths. */
    XR_XIR_PATH_TUPLE_FIELD,
    XR_XIR_PATH_NULLABLE_VALUE,
    XR_XIR_PATH_ENUM_FIELD,
    XR_XIR_PATH_KEY
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
 * persistent interior addresses; access resolves the current root. A KEY
 * selector stores the exact Map key payload. Synchronous callers pin those
 * keys; retained Cell projections acquire and release their own key owners. */
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
