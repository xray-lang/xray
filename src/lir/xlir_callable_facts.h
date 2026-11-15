/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_callable_facts.h - Owned final function-value and scope descriptions
 *
 * KEY CONCEPT:
 *   Construction publishes the existing callable verifier's facts once.
 *   Receiving copies own indices and spans, never code addresses or values.
 */
#ifndef XLIR_CALLABLE_FACTS_H
#define XLIR_CALLABLE_FACTS_H
#include "xlir_callable.h"

typedef struct XrLirCallableFacts XrLirCallableFacts;

typedef enum XrLirCallableTransport {
    XR_LIR_CALLABLE_TRANSPORT_NONE, XR_LIR_CALLABLE_TRANSPORT_FRAME,
    XR_LIR_CALLABLE_TRANSPORT_BORROW, XR_LIR_CALLABLE_TRANSPORT_OWNED,
    XR_LIR_CALLABLE_TRANSPORT_MIXED
} XrLirCallableTransport;

typedef struct XrLirCallableValueFact {
    XrLirType type;
    XrLirOp operation;
    /* Positions are one-based. Formals have block=0 and parameter!=0; block
     * arguments have their actual block, instruction=0 and parameter=0. */
    uint32_t block, instruction, parameter;
    uint32_t origin, alias_source, known_target;
    uint32_t first_capture, capture_count;
    uint32_t owner_scope, receiving_scope, frame_floor;
    uint32_t constructor_storage;
    uint64_t binding_identity;
    bool persistent, state_capture;
    /* Transport describes a verified value flow, not escaping permission.
     * A code-only FRAME constructor materializes an Env only when a checked
     * escaping use requires an independent owner. Its original storage fact
     * remains unchanged. Mixed owning/borrowed joins need explicit handling. */
    XrLirCallableTransport transport;
    bool materialize;
} XrLirCallableValueFact;

typedef struct XrLirCallableFunctionFacts {
    /* values has value_count+1 rows, including empty SSA zero. Scope handles
     * also have rows; unrelated values have type zero. Capture spans contain
     * source SSA ids and occur only on FUNCTION_REF; aliases name their origin. */
    const XrLirCallableValueFact *values;
    uint32_t value_count;
    const uint32_t *captures;
    uint32_t capture_count;
} XrLirCallableFunctionFacts;

/* Copy the facts produced by this exact finished module's constructor. No
 * callable, escape or scope analysis is repeated. Scalar-only modules succeed
 * without allocating and leave *empty NULL. A copy survives module retirement.
 * All returned slices borrow this immutable owner, not a compiler type pool.
 * Failure leaves output and the source unchanged.
 *
 * persistent is the existing escape predicate, not an owned-reference fact.
 * Parameters remain borrows even when escaping. constructor_storage applies
 * only to FUNCTION_REF, as do the capture span and state_capture. known_target
 * is zero for opaque parameters, block
 * arguments, State reads and returned values; a factory is not its result.
 * Scope ids are SSA capabilities. frame_floor is recorded for BIND/LEAVE only
 * and is UINT32_MAX elsewhere. Entries and captures grant neither invocation
 * nor mutation permission. These facts do not admit any execution carrier. */
XR_FUNC XrLirStatus xr_lir_callable_facts_create(const XrLirModule *, XrLirCallableFacts **empty);
XR_FUNC void xr_lir_callable_facts_destroy(XrLirCallableFacts *);
XR_FUNC uint32_t xr_lir_callable_facts_function_count(const XrLirCallableFacts *);
/* function is one-based. Functions without callable values return an empty
 * view. Rejection preserves the caller's output. */
XR_FUNC XrLirStatus xr_lir_callable_facts_function(const XrLirCallableFacts *, uint32_t function,
    XrLirCallableFunctionFacts *);
#endif // XLIR_CALLABLE_FACTS_H
