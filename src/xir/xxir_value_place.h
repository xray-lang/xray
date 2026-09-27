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
typedef struct XrXirValuePlace {
    XrXirType type;
    void *payload;
} XrXirValuePlace;
XR_FUNC XrXirValueStatus xr_xir_cell_value_place(const XrXirValue *cell,
    XrXirValueAdmission *admission, XrXirValuePlace *output);
#endif // XXIR_VALUE_PLACE_H
