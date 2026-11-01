/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_value_take_internal.h - Prepared transfer from a whole local Cell
 *
 * KEY CONCEPT:
 *   Preparation borrows the caller's live Cell without adding a payload owner.
 *   The final execution check precedes an infallible ownership commit.
 */
#ifndef XXIR_VALUE_TAKE_INTERNAL_H
#define XXIR_VALUE_TAKE_INTERNAL_H
#include "xxir_value.h"

typedef struct XirCellTake {
    void *cell;
    XrXirValue value;
} XirCellTake;
typedef struct XirCellNewTake {
    void *cell;
    XrXirValue value;
} XirCellNewTake;
/* The caller keeps its actual Cell alive and excludes mutation until commit.
 * This grants no Source availability, projection or reference authority. */
XR_FUNC XrXirValueStatus xir_cell_take_prepare(const XrXirValue *source,
    XrXirType expected,XrXirValueAdmission *admission,XirCellTake *output);
XR_FUNC void xir_cell_take_commit(XirCellTake *prepared,XrXirValue *output);
/* The new Cell is unpublished and owns no input until its real source word is
 * cleared at commit. Abandonment discards only that prepared empty Cell. */
XR_FUNC XrXirValueStatus xir_cell_new_take_prepare(XrXirType type,
    const XrXirValue *initial,XrXirValueAdmission *admission,XirCellNewTake *output);
XR_FUNC void xir_cell_new_take_abort(XirCellNewTake *prepared);
XR_FUNC void xir_cell_new_take_commit(XirCellNewTake *prepared,
    void *source_payload,XrXirValue *output);
#endif // XXIR_VALUE_TAKE_INTERNAL_H
