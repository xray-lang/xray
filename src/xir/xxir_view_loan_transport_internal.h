/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_loan_transport_internal.h - Contextual demand over owned equations
 */
#ifndef XXIR_VIEW_LOAN_TRANSPORT_INTERNAL_H
#define XXIR_VIEW_LOAN_TRANSPORT_INTERNAL_H
#include "xxir_view_loan_internal.h"

typedef struct XirViewLoanTransport XirViewLoanTransport;
typedef struct XirViewLoanDemand {
    uint32_t function, carrier;
    uint64_t frame_bytes;
    XrXirStatus (*physical)(void *, uint32_t value);
    void *context;
} XirViewLoanDemand;
typedef struct XirViewLoanUse {
    uint32_t function, instruction, value;
    uint64_t frame_bytes;
    XrXirStatus (*physical)(void *, uint32_t value);
    void *context;
} XirViewLoanUse;

/* Requires ordinary admission and complete semantic facts for this generation.
 * The module, flags and semantic facts are immutable synchronous borrows.
 * One index serves every function in this generation. Applications substitute
 * their actual point values without adding edges to a shared callee. The
 * reverse index is allocated only by the first actual demand, within the
 * frame budget remaining after the observing liveness frame. */
XR_FUNC XrXirStatus xir_view_loan_transport_new(const XrXirCompileContext *,
    const XrXirModule *, const XirViewLoanFacts *, const uint8_t *flags,
    XrCompileRequestControl *, XirViewLoanTransport **);
/* Physical callbacks name only the observing function's current frame.
 * Unapplied definition-local FOOTPRINT inputs retain the caller-held loan
 * contract. Applied inputs require their actual binding. Unknown storage
 * inputs, foreign borrowed terminals and ungrounded cycles fail closed.
 * A later failure invalidates every callback from this demand operation. */
XR_FUNC XrXirStatus xir_view_loan_transport_demand(XirViewLoanTransport *, const XirViewLoanDemand *);
XR_FUNC XrXirStatus xir_view_loan_transport_use(XirViewLoanTransport *, const XirViewLoanUse *);
XR_FUNC uint64_t xir_view_loan_transport_frame_bytes(const XirViewLoanTransport *);
XR_FUNC void xir_view_loan_transport_free(XirViewLoanTransport *);
#endif // XXIR_VIEW_LOAN_TRANSPORT_INTERNAL_H
