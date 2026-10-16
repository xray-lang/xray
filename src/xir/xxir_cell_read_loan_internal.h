/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_cell_read_loan_internal.h - Shared read reservations over an actual Cell root
 *
 * KEY CONCEPT:
 *   A read loan restricts mutation without granting access or owning elements.
 */
#ifndef XXIR_CELL_READ_LOAN_INTERNAL_H
#define XXIR_CELL_READ_LOAN_INTERNAL_H
#include "xxir_cell_owner_internal.h"

#define XR_XIR_CELL_READ_LOAN_WORK 8u
typedef struct XrXirCellReadLoan {
    void *cell;
    struct XrXirCellReadLoan *next;
    struct XrXirCellReadLoan **previous;
    XrXirCellAuthority authority;
} XrXirCellReadLoan;

/* The authenticated caller pins the already admitted whole Cell and this stable
 * record until abort or release. This restriction grants no payload admission.
 * Prepare prepays every terminal link operation.
 * Output is canonical zero; a successful prepare already excludes mutation. */
XR_FUNC XrXirValueStatus xr_xir_cell_read_loan_prepare(const XrXirValue *cell,
    const XrXirCellAuthority *authority,XrXirValueAdmission *admission,XrXirCellReadLoan *output);
XR_FUNC void xr_xir_cell_read_loan_abort(XrXirCellReadLoan *loan);
/* Commit requires the same live authority that prepared the record. */
XR_FUNC void xr_xir_cell_read_loan_commit(const XrXirValue *cell,
    XrXirCellReadLoan *loan,const XrXirCellAuthority *authority);
/* Committed shared loans may end in any order; release cannot allocate. */
XR_FUNC void xr_xir_cell_read_loan_release(XrXirCellReadLoan *loan);
#endif // XXIR_CELL_READ_LOAN_INTERNAL_H
