/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_vtable.h - Atomic owned virtual-table publication
 */
#ifndef XANALYZER_VTABLE_H
#define XANALYZER_VTABLE_H
#include "../../base/xcompile_state.h"
#include "../../runtime/class/xclass_info.h"

typedef struct XaVTableClassInput {
    XrClassInfo *info;
    /* One real runtime symbol ID per methods[] entry. Zero is only valid for
     * a NULL entry or an entry without a name. All IDs share one authority. */
    const int32_t *method_ids;
} XaVTableClassInput;

/* Inputs are one base-to-derived unbuilt chain. An already-built ancestor may
 * precede the first input. The caller excludes concurrent metadata mutation.
 * Failure preserves every table and ancestor flag; first resource failure wins.
 * Display names are copied. Method symbols and class owners remain borrowed
 * for the same analyzer lifetime. No runtime identity is reconstructed here. */
XR_FUNC XrCompileResourceStatus xa_class_vtable_build_owned(XrCompileState *state,
    const XaVTableClassInput *inputs, size_t count);
/* Every table produced above uses this destructor, including after the
 * producing Session/state dies. No cleanup work is charged. */
XR_FUNC void xa_class_vtable_free_owned(XrClassInfo *info);
#endif // XANALYZER_VTABLE_H
