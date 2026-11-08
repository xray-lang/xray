/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_execution_internal.h - Borrowed numeric execution and capture relations
 *
 * KEY CONCEPT:
 *   The shared semantic owner produces actual candidates once. Storage and
 *   lifetime consumers read numeric relations without retaining body storage.
 */
#ifndef XXIR_EXECUTION_INTERNAL_H
#define XXIR_EXECUTION_INTERNAL_H
#include "xxir.h"

typedef struct EffectCheckedRoots XirExecutionOwner;
typedef enum XirExecutionSiteKind {
    XIR_EXECUTION_CALL, XIR_EXECUTION_PENDING, XIR_EXECUTION_CLEANUP_USE, XIR_EXECUTION_FORMAL
} XirExecutionSiteKind;

typedef enum XirExecutionPort {
    XIR_EXECUTION_NORMAL, XIR_EXECUTION_ERROR, XIR_EXECUTION_PANIC
} XirExecutionPort;

#define XIR_EXECUTION_CURRENT_ENVIRONMENT UINT32_MAX
#define XIR_EXECUTION_UNKNOWN_ENVIRONMENT (UINT32_MAX - 1u)

enum {
    XIR_EXECUTION_REMAINDER_STORAGE = 1u,
    XIR_EXECUTION_REMAINDER_PARAMETER = 2u,
    XIR_EXECUTION_REMAINDER_OPAQUE = 4u
};

typedef struct XirRestrictionFormula {
    bool constant;
    const uint32_t *inputs;
    uint32_t input_count;
} XirRestrictionFormula;

typedef struct XirRestrictionFormulaRow {
    uint32_t first, count;
    bool constant;
} XirRestrictionFormulaRow;

typedef struct XirRestrictionFunction {
    uint32_t physical_values, value_count;
    const uint32_t *intrinsic;
    const XrXirValueFlow *flows;
    uint32_t flow_count;
} XirRestrictionFunction;

typedef struct XirRestrictionPoint {
    uint32_t function, instruction, value, formula, carrier;
} XirRestrictionPoint;

/* Formula inputs are sorted physical/aux leaf IDs in the observing function's
 * namespace. Intrinsic has value_count formula IDs. Point rows are ordered by
 * function/instruction/value and name the current transport carrier, including
 * mutable capture contents. Formula IDs belong to exactly one function. A true
 * formula retains its independent loan flows. */
typedef struct XirRestrictionView {
    const XirRestrictionFunction *functions;
    uint32_t function_count;
    const XirRestrictionFormulaRow *formulas;
    uint32_t formula_count;
    const uint32_t *inputs;
    uint32_t input_count;
    const XirRestrictionPoint *points;
    uint32_t point_count;
} XirRestrictionView;

/* Accept copies the complete numeric view synchronously. The producer releases
 * its temporary owner after the call, including when receiving fails. */
typedef struct XirRestrictionReceiver {
    void *context;
    XrXirStatus (*accept)(void *, const XirRestrictionView *);
} XirRestrictionReceiver;

typedef struct XirExecutionSite {
    XirExecutionSiteKind kind;
    uint32_t index;
    uint32_t function, instruction, block, order, caller_instance;
    uint32_t target, declaration;
    uint32_t argument_count, capture_count;
    uint32_t output_function, output_value;
    uint32_t receiver_function, receiver_value;
    /* The semantic call-record ID of the original registration, or UINT32_MAX. */
    uint32_t registration;
    uint32_t native_callable, environment, reference, capture_environment, remainder;
    XirExecutionPort port;
    XrXirType signature;
    uint8_t protocol;
    bool unresolved;
} XirExecutionSite;

typedef struct XirExecutionValue { uint32_t function, value, environment; } XirExecutionValue;

typedef struct XirExecutionArgument {
    uint32_t function, value, caller_handle, environment;
    uint32_t mode;
    XrXirType type;
    bool cell, escaping;
} XirExecutionArgument;

typedef struct XirExecutionParameter {
    XrXirType type;
    uint32_t mode;
    bool escaping, hidden, cell;
} XirExecutionParameter;

typedef XrXirStatus (*XirExecutionVisitor)(void *, const XirExecutionSite *);

/* Function and value ordinals use the physical declaration namespace. Target
 * retains the numeric generic instance identity, while declaration names the
 * admitted body. A site's kind/index stays stable while new edges are linked.
 * Known edges and an unresolved remainder may coexist at one real callsite.
 * Cleanup uses have increasing order in actual LIFO execution order. Panic
 * sequences may name a whole block with instruction UINT32_MAX; their input
 * is the join of the block's before-instruction states. */
XR_FUNC XrXirStatus xir_execution_site(const XirExecutionOwner *,
    XirExecutionSiteKind, uint32_t, XirExecutionSite *);
XR_FUNC XrXirStatus xir_execution_visit_calls(const XirExecutionOwner *,
    XirExecutionVisitor, void *);
XR_FUNC XrXirStatus xir_execution_visit_captures(const XirExecutionOwner *,
    XirExecutionVisitor, void *);

/* Actuals include the authenticated physical capture prefix. A numeric aux
 * or an unavailable receiver returns UNRESOLVED, never a forged physical SSA
 * ordinal. Caller_handle is UINT32_MAX unless the captured identity has a
 * caller-local physical representative. Foreign values keep their explicit
 * environment application instead. Invalid requests preserve the output. */
XR_FUNC XrXirStatus xir_execution_argument(const XirExecutionOwner *,
    const XirExecutionSite *, uint32_t, XirExecutionArgument *);
/* Reads the owning instance's closed formal contract without requiring an
 * actual physical value. Type IDs use xir_execution_types(owner). */
XR_FUNC XrXirStatus xir_execution_parameter(const XirExecutionOwner *,
    const XirExecutionSite *, uint32_t, XirExecutionParameter *);

/* This reads the same candidate relation as execution linking. Complete is
 * false when any callable origin remains opaque or symbolic. The visitor may
 * receive known bindings even when another origin is unresolved. */
XR_FUNC XrXirStatus xir_execution_visit_value_captures(const XirExecutionOwner *,
    uint32_t, uint32_t, XirExecutionVisitor, void *, bool *complete);
XR_FUNC XrXirStatus xir_execution_visit_value_captures_in(const XirExecutionOwner *,
    const XirExecutionValue *, XirExecutionVisitor, void *, bool *complete, uint32_t *remainder);
/* Environment observation additionally represents an ordinary callable formal
 * as a typed symbolic terminal. It never adds a call or resolves its callee. */
XR_FUNC XrXirStatus xir_execution_visit_value_environment_in(const XirExecutionOwner *,
    const XirExecutionValue *, XirExecutionVisitor, void *, bool *complete, uint32_t *remainder);
/* Construction rebinds this receiving index once after each storage round. */
XR_FUNC XrXirStatus xir_execution_refresh_storage(XirExecutionOwner *);
/* Qualified substitution of this owner's symbolic receiver. Returned pending
 * projections retain their canonical index and name the originating capture
 * record in reference. Consumers key an application by both IDs and context. */
XR_FUNC XrXirStatus xir_execution_resolve_site(const XirExecutionOwner *,
    const XirExecutionSite *, uint32_t environment, XirExecutionVisitor, void *,
    bool *complete, uint32_t *remainder);
/* The same relation supplies late effect precision. An ordinary weakening
 * keeps its declared upper bound while physical capture queries stay exact. */
XR_FUNC XrXirStatus xir_execution_resolve_effect_site(const XirExecutionOwner *,
    const XirExecutionSite *, XirExecutionVisitor, void *, bool *, uint32_t *);


/* Type IDs belong to this owner's stable numeric type namespace. The returned
 * pool is borrowed only during the synchronous query and must be rebound after
 * linking can grow its storage. Native or unresolved calls conservatively
 * clobber reachable writable Cell actuals and callable environments. */
XR_FUNC const XrXirTypes *xir_execution_types(const XirExecutionOwner *);
#endif // XXIR_EXECUTION_INTERNAL_H
