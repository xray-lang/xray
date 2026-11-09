/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_loan_internal.h - Borrowed transport from the semantic owner
 */
#ifndef XXIR_VIEW_LOAN_INTERNAL_H
#define XXIR_VIEW_LOAN_INTERNAL_H
#include "xxir_storage_flow.h"

struct XrXirEffects;
typedef struct XirViewLoanFacts {
    XirRestrictionView restrictions;
    XirStorageFlowView storage;
    const XirExecutionOwner *execution;
} XirViewLoanFacts;

/* These arrays belong to the semantic owner and remain valid only until its
 * next mechanical relocation or destruction. Formula constants never remove
 * loan transport. Contextual bindings substitute an actual invocation without
 * adding any actual dependency to a shared callee template. */
XR_FUNC XrXirStatus xir_effects_view_loan_facts(const XrXirCompileContext *,
    const struct XrXirEffects *, XirViewLoanFacts *);
#endif // XXIR_VIEW_LOAN_INTERNAL_H
