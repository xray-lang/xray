/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_storage_internal.h - Stable runtime storage for authenticated views
 *
 * KEY CONCEPT:
 *   Range access rechecks the exact root loan; a physical pin grants no access.
 */
#ifndef XXIR_VIEW_STORAGE_INTERNAL_H
#define XXIR_VIEW_STORAGE_INTERNAL_H
#include "xxir_cell_read_loan_internal.h"
#include "xxir.h"

typedef struct XrXirViewStorage {
    XrXirValue root;
    uint32_t kind;
    XrXirType element;
    int64_t offset, length;
    const XrXirCellLoan *parent;
    union { XrXirCellReadLoan shared; XrXirCellLoan exclusive; } loan;
} XrXirViewStorage;

typedef struct XrXirViewStorageSpec {
    XrXirCellAuthority authority;
    int64_t offset, length;
    uint32_t kind;
} XrXirViewStorageSpec;

/* The authenticated caller owns complete root admission and keeps the actual
 * origin scope and any parent loan alive. Output is zero and its address must
 * remain stable until end; moving a handle must never memcpy this storage.
 * Only whole Cell<Array<T>> and shared Cell<string> byte ranges are handled.
 * All fallible COW work precedes publication; failure preserves root and output.
 * This core grants neither Source escape permission nor a public Value handle. */
XR_FUNC XrXirValueStatus xr_xir_view_storage_begin(const XrXirValue *root,
    const XrXirCellAuthority *parent, const XrXirViewStorageSpec *spec,
    XrXirValueAdmission *admission, XrXirViewStorage *output);
/* Child ranges are relative to the parent's range and cannot enlarge it.
 * A shared parent cannot grant an exclusive child. The caller authenticates
 * the live parent binding before supplying its exact origin authority. */
XR_FUNC XrXirValueStatus xr_xir_view_storage_reborrow(const XrXirViewStorage *parent,
    const XrXirCellAuthority *authority, const XrXirViewStorageSpec *spec,
    XrXirValueAdmission *admission, XrXirViewStorage *output);
/* Children end before an exclusive parent. Retirement is allocation-free. */
XR_FUNC void xr_xir_view_storage_end(XrXirViewStorage *storage);
XR_FUNC XrXirValueStatus xr_xir_view_storage_len(const XrXirViewStorage *storage,
    const XrXirCellAuthority *authority, XrXirValueAdmission *admission, int64_t *output);
XR_FUNC XrXirValueStatus xr_xir_view_storage_get(const XrXirViewStorage *storage,
    const XrXirCellAuthority *authority, int64_t index, XrXirValueAdmission *admission,
    XrXirValue *output, XrXirFaultDetail *fault);
XR_FUNC XrXirValueStatus xr_xir_view_storage_set(const XrXirViewStorage *storage,
    const XrXirCellAuthority *authority, int64_t index, const XrXirValue *value,
    XrXirValueAdmission *admission, XrXirFaultDetail *fault);
/* Materialization returns an owned Array while preserving element identities. */
XR_FUNC XrXirValueStatus xr_xir_view_storage_copy(XrXirType array,
    const XrXirViewStorage *storage, const XrXirCellAuthority *authority,
    XrXirValueAdmission *admission, XrXirValue *output);
#endif // XXIR_VIEW_STORAGE_INTERNAL_H
