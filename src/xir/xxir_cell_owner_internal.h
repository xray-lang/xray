/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_cell_owner_internal.h - Stable cell publication and frame-owned loans
 *
 * KEY CONCEPT:
 *   A retained cell keeps storage alive but never grants access to a frame loan.
 */
#ifndef XXIR_CELL_OWNER_INTERNAL_H
#define XXIR_CELL_OWNER_INTERNAL_H
#include "xxir_value_place.h"

typedef struct XrXirCellAuthority {
    const void *activation, *frame;
    uint64_t epoch;
} XrXirCellAuthority;
typedef struct XrXirCellLoan {
    void *cell;
    struct XrXirCellLoan *previous;
    XrXirCellAuthority authority;
} XrXirCellLoan;
typedef struct XrXirCellPublication {
    const void *owner;
    XrXirDomain *domain;
    uint32_t slot;
} XrXirCellPublication;

/* Preparation changes no publication or loan state. Commit cannot allocate. */
XR_FUNC XrXirValueStatus xr_xir_cell_publication_prepare(const XrXirValue *cell,
    const XrXirCellPublication *publication);
XR_FUNC void xr_xir_cell_publication_commit(const XrXirValue *cell,
    const XrXirCellPublication *publication);
XR_FUNC bool xr_xir_cell_publication_matches(const XrXirValue *cell,
    const XrXirCellPublication *publication);
XR_FUNC bool xr_xir_cell_module_storage(const XrXirValue *cell);
XR_FUNC bool xr_xir_cell_same_owner(const XrXirValue *left, const XrXirValue *right);
XR_FUNC bool xr_xir_cell_unborrowed(const XrXirValue *cell);
/* Only the immediate live parent authority may authorize a nested loan. */
XR_FUNC XrXirValueStatus xr_xir_cell_loan_prepare(const XrXirValue *cell,
    const XrXirCellAuthority *parent);
XR_FUNC void xr_xir_cell_loan_commit(const XrXirValue *cell,
    XrXirCellLoan *loan, const XrXirCellAuthority *authority);
XR_FUNC void xr_xir_cell_loan_release(XrXirCellLoan *loan);
/* The call driver authenticates authority before entering these primitives. */
XR_FUNC XrXirValueStatus xr_xir_cell_authorized_read(const XrXirValue *cell,
    const XrXirCellAuthority *authority, XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_cell_authorized_write(const XrXirValue *cell,
    const XrXirCellAuthority *authority, const XrXirValue *value,
    XrXirValueAdmission *admission);
XR_FUNC XrXirValueStatus xr_xir_cell_authorized_place(const XrXirValue *cell,
    const XrXirCellAuthority *authority, XrXirValueAdmission *admission,
    XrXirValuePlace *output);
#endif // XXIR_CELL_OWNER_INTERNAL_H
