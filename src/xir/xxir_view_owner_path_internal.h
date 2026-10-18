/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_owner_path_internal.h - Conditional physical owner observations
 *
 * KEY CONCEPT:
 *   Required normal edges are visited, not merely left open in a filtered CFG.
 */
#ifndef XXIR_VIEW_OWNER_PATH_INTERNAL_H
#define XXIR_VIEW_OWNER_PATH_INTERNAL_H
#include "xxir_view_loan_origins_internal.h"

typedef struct XirViewOwnerPathRequest {
    uint32_t function,instruction,owner;
    const XrXirViewLoanChoice *left,*right;
    uint32_t left_count,right_count;
} XirViewOwnerPathRequest;

/* Requires ordinary and independent affine admission of the immutable module.
 * Observe immediately before the instruction, after every required normal
 * predecessor edge to a non-entry block. Choices in cyclic blocks remain
 * UNRESOLVED. False proves absence in this CFG overapproximation; true is not
 * logical availability, actual path feasibility or an access grant. Panic
 * states retain all protected prefixes. The output is unchanged on failure.
 * All borrowed requests expire on return; scratch uses the caller's ledger. */
XR_FUNC XrXirStatus xir_compile_view_owner_path_possible(const XrXirCompileContext *context,
    const XrXirModule *module,const XirViewOwnerPathRequest *request,bool *possible,XrXirDiagnostic *diagnostic);
#endif // XXIR_VIEW_OWNER_PATH_INTERNAL_H
