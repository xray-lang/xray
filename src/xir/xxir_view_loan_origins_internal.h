/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_loan_origins_internal.h - Scope producers and actual PHI choices
 *
 * KEY CONCEPT:
 *   Producer sites and branch choices are observations, never runtime identity.
 */
#ifndef XXIR_VIEW_LOAN_ORIGINS_INTERNAL_H
#define XXIR_VIEW_LOAN_ORIGINS_INTERNAL_H
#include "xxir_view_roots_internal.h"

typedef enum XrXirViewLoanOriginKind {
    XR_XIR_VIEW_LOAN_INPUT,XR_XIR_VIEW_LOAN_NEW,XR_XIR_VIEW_LOAN_REBORROW,
    XR_XIR_VIEW_LOAN_CALL,XR_XIR_VIEW_LOAN_ABSENT,XR_XIR_VIEW_LOAN_UNRESOLVED
} XrXirViewLoanOriginKind;
typedef struct XrXirViewLoanChoice { uint32_t block,predecessor; } XrXirViewLoanChoice;
typedef struct XrXirViewLoanOrigin {
    uint32_t function,instruction,value,source;
    XrXirViewLoanOriginKind kind;
    XrXirType type;
    const uint32_t *selection;
    uint32_t selection_count;
    const XrXirViewLoanChoice *choices;
    uint32_t choice_count;
    bool repeated_phi;
} XrXirViewLoanOrigin;
typedef XrXirStatus (*XrXirViewLoanOriginVisitor)(void *user,const XrXirCompileContext *context,
    const XrXirViewLoanOrigin *event);

/* Uses the resolver's admitted immutable module generation and resource owner.
 * Stops at actual input, constructor, reborrow or call-result producers instead
 * of collapsing their ancestry to storage roots. Input/call selections may
 * denote a borrowed subtree; they are not enumerated exponential type leaves.
 * NEW/REBORROW sources are their actual Cell/view operands. CALL sources come
 * only from the independently derived signature candidate, not body inference;
 * the result selection is not a source-selection or authority grant.
 * PHI choices name the actual target block and normal predecessor. A repeated
 * target during value replay is explicitly unresolved, not a contradictory
 * branch discarded as impossible. A site may execute in multiple loop epochs.
 * Borrowed arrays expire on callback return. Discard all partial observations
 * on failure; resolver reentry fails. No ancestor, epoch, alias or access proof
 * is issued, and absent choices do not certify an acyclic CFG or scope. */
XR_FUNC XrXirStatus xir_compile_view_loan_origins(XirViewRootResolver *resolver,
    const XirViewRootRequest *request,XrXirViewLoanOriginVisitor visitor,void *user,XrXirDiagnostic *diagnostic);
#endif // XXIR_VIEW_LOAN_ORIGINS_INTERNAL_H
