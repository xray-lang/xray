/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_storage_flow.h - Numeric storage versions and contextual applications
 *
 * KEY CONCEPT:
 *   Reads depend on the storage version reaching that instruction. A call
 *   substitutes symbolic callee inputs without changing the shared template.
 */
#ifndef XXIR_STORAGE_FLOW_H
#define XXIR_STORAGE_FLOW_H
#include "xxir_execution_internal.h"

typedef enum XirStorageFlowBank {
    XIR_STORAGE_CONTENTS, XIR_STORAGE_EXPOSURE, XIR_STORAGE_FOOTPRINT
} XirStorageFlowBank;
typedef enum XirStoragePointKind {
    XIR_STORAGE_BEFORE, XIR_STORAGE_NORMAL, XIR_STORAGE_ERROR, XIR_STORAGE_PANIC
} XirStoragePointKind;

typedef struct XirStorageFlowFunction {
    uint32_t physical_values, value_count, parameter_count;
    const XrXirValueFlow *flows;
    uint32_t flow_count;
    const XrXirValueFlow *exposure_flows;
    uint32_t exposure_flow_count;
    /* Environment dependencies transport restriction and loan facts. They do
     * not identify the callable value itself and cannot select a callee. */
    const XrXirValueFlow *footprint_flows;
    uint32_t footprint_flow_count;
} XirStorageFlowFunction;

typedef struct XirStorageCrossFlow {
    uint32_t source_function, source, target_function, target;
    /* UINT32_MAX denotes an ordinary relation. Other values index applications. */
    uint32_t application;
    XirStorageFlowBank bank;
} XirStorageCrossFlow;

typedef struct XirStorageBinding {
    uint32_t formal_function, formal_value, actual_function, actual_value;
    /* Every non-sentinel value, including zero, indexes applications. */
    uint32_t actual_environment;
} XirStorageBinding;

typedef struct XirStorageApplication {
    XirExecutionSiteKind kind;
    /* Function is the caller template. Declaration is the callee template. */
    uint32_t site, function, instruction, block, declaration;
    uint32_t target, caller_instance, environment, order;
    /* Reference is the execution owner's capture-call ID, or UINT32_MAX.
     * Block is a caller-template ordinal. Both environments use application
     * indices or the execution environment sentinels. */
    uint32_t reference, capture_environment;
    XirExecutionPort port, outcome;
    uint8_t protocol;
    uint32_t first, count;
} XirStorageApplication;

typedef struct XirStorageIdentity {
    uint32_t source_function, source, target_function, target;
    uint32_t application;
} XirStorageIdentity;

/* An allocation belongs to this template frame. Applications preserve the
 * corresponding invocation context. An absent allocation is external. */
typedef struct XirStorageAllocation { uint32_t function, value; } XirStorageAllocation;

/* Every formal Cell has an explicit contents input, including unused bodies.
 * A FOOTPRINT input names a separate callable-environment leaf. Its PointValue
 * records the use point, while handle is the physical parameter ordinal.
 * The leaf denotes a caller-held loan for this invocation. Ordinary formals
 * retain that loan independently of their escape permission. Hidden formals
 * require the storage producer's complete environment transport proof. Neither denotes
 * an empty environment. Applications must substitute their actual footprint. */
typedef struct XirStorageInput {
    uint32_t function, value, handle;
    XirStorageFlowBank bank;
} XirStorageInput;

typedef struct XirStorageUnknown {
    uint32_t function, value;
    XrXirType type;
    uint32_t application;
} XirStorageUnknown;

typedef struct XirStorageExposureSeed { uint32_t function, value, application; } XirStorageExposureSeed;

/* A value may be an owned storage identity proxy. Execution's physical actual
 * namespace remains unchanged. Environment IDs use the execution sentinels. */
typedef XirExecutionValue XirStorageReference;
typedef struct XirStorageObservation {
    uint32_t function, instruction;
    XirStoragePointKind point;
    XirStorageReference reference;
} XirStorageObservation;
typedef struct XirStorageValuePoint {
    uint32_t function, instruction;
    XirStorageReference reference;
    XirStoragePointKind point;
} XirStorageValuePoint;
/* A proxy records the actual observation point even when contents and their
 * ordinary restriction formula are unchanged across several instructions. */
typedef struct XirStoragePointValue {
    uint32_t function, value, instruction;
    XirStoragePointKind point;
    XirStorageReference reference;
    /* A separate environment input leaf, or UINT32_MAX. Intrinsic callable
     * origins and the current footprint are independent dependencies. */
    uint32_t footprint_input;
} XirStoragePointValue;
typedef enum XirStorageCaptureKind {
    XIR_STORAGE_CAPTURE_CELL, XIR_STORAGE_CAPTURE_FORMAL_FN, XIR_STORAGE_CAPTURE_STORABLE_FN,
    XIR_STORAGE_CAPTURE_HIDDEN_FN
} XirStorageCaptureKind;
typedef struct XirStorageCapture {
    XirStorageReference identity;
    uint32_t function, contents, exposure;
    /* A formal carrier transports its independent restriction and loan facts.
     * A storable terminal has no contents or exposure value. Known environment
     * Cells remain separate captures. */
    XirStorageCaptureKind kind;
} XirStorageCapture;
typedef XrXirStatus (*XirStorageCaptureVisitor)(void *, const XirStorageCapture *);
enum {
    XIR_STORAGE_QUERY_CONTENTS = 1u,
    XIR_STORAGE_QUERY_EXPOSURE = 2u,
    XIR_STORAGE_QUERY_ALL = 3u
};

typedef struct XirStorageFlowView {
    const XirStorageFlowFunction *functions;
    uint32_t function_count;
    const XirStorageCrossFlow *crossflows;
    uint32_t crossflow_count;
    const XirStorageApplication *applications;
    uint32_t application_count;
    const XirStorageBinding *bindings;
    uint32_t binding_count;
    const XirStorageIdentity *identities;
    uint32_t identity_count;
    const XirStorageAllocation *allocations;
    uint32_t allocation_count;
    const XirStorageInput *inputs;
    uint32_t input_count;
    const XirStorageUnknown *unknowns;
    uint32_t unknown_count;
    const XirStorageExposureSeed *exposure_seeds;
    uint32_t exposure_seed_count;
    const XirStoragePointValue *point_values;
    uint32_t point_value_count;
} XirStorageFlowView;

typedef struct XirStorageFlow XirStorageFlow;

/* Input bodies and the unique execution owner are synchronous construction
 * borrows. No instruction or caller-owned array survives sealing. */
XR_FUNC XrXirStatus xir_storage_flow_new(const XrXirCompileContext *, const XrXirModule *,
    const XirExecutionOwner *, XrCompileRequestControl *, XirStorageFlow **);
/* The unique instruction inventory calls observe once per physical operation.
 * New reads only headers and block descriptors. Finish closes local versions. */
XR_FUNC XrXirStatus xir_storage_flow_observe(XirStorageFlow *, uint32_t function,
    uint32_t instruction, const XrXirInstruction *);
XR_FUNC XrXirStatus xir_storage_flow_finish_local(XirStorageFlow *);
/* New real execution edges extend applications monotonically. Read-origin
 * consumers must use this graph before closing indirect call candidates. */
XR_FUNC XrXirStatus xir_storage_flow_link(XirStorageFlow *, const XirExecutionOwner *, bool *changed);
/* Returns the contents immediately before instruction. Physical handles retain
 * identity independently of contents. Queries may append numeric aux values. */
XR_FUNC XrXirStatus xir_storage_flow_contents(XirStorageFlow *, uint32_t function,
    uint32_t instruction, uint32_t handle, uint32_t *value);
/* NORMAL after RETURN includes its actual LIFO cleanup effects. */
XR_FUNC XrXirStatus xir_storage_flow_contents_at(XirStorageFlow *, uint32_t function,
    uint32_t instruction, uint32_t handle, XirStoragePointKind, uint32_t *value);
/* HIR supplies publication events. These transport exposure through the same
 * graph and never decide whether a value may escape. Exposure has its own bank.
 * Register every event before the first exposure query freezes the inventory.
 * Repeating an event during construction is idempotent. */
XR_FUNC XrXirStatus xir_storage_flow_expose(XirStorageFlow *, uint32_t function,
    uint32_t instruction, uint32_t handle, XirStoragePointKind);
XR_FUNC XrXirStatus xir_storage_flow_exposure_at(XirStorageFlow *, uint32_t function,
    uint32_t instruction, uint32_t handle, XirStoragePointKind, uint32_t *value);
/* Qualified observations project R/X values into the observation's function.
 * Either output may be NULL. A contents-only query leaves exposure events open.
 * A foreign identity never becomes a fabricated caller-local physical value. */
XR_FUNC XrXirStatus xir_storage_flow_reference(XirStorageFlow *, const XirStorageObservation *,
    uint32_t *contents, uint32_t *exposure);
XR_FUNC XrXirStatus xir_storage_flow_expose_reference(XirStorageFlow *, const XirStorageObservation *);
/* The returned reference names a storage-owned aux value in the observing
 * function. New proxies add an ordinary dependency and require a Root round. */
XR_FUNC XrXirStatus xir_storage_flow_point_value(XirStorageFlow *, const XirStorageValuePoint *,
    XirStorageReference *);
/* Recursive carrier/capture relations come from the unique execution owner.
 * Each callback observes a Cell reference at this use, including older heap
 * instances retained across a backedge. It never freezes creation contents. */
XR_FUNC XrXirStatus xir_storage_flow_visit_value_captures(XirStorageFlow *, const XirStorageValuePoint *,
    uint32_t banks, XirStorageCaptureVisitor, void *);
XR_FUNC XrXirStatus xir_storage_flow_expose_value(XirStorageFlow *, const XirStorageValuePoint *);
/* Storing a carrier in exposed storage exposes its reachable environment only
 * when the store completes. Condition names the destination's prior exposure. */
XR_FUNC XrXirStatus xir_storage_flow_expose_value_when(XirStorageFlow *, const XirStorageValuePoint *,
    const XirStorageObservation *condition);
/* Read immediately after an unresolved qualified query. STORAGE can await the
 * next shared receiving round. PARAMETER and OPAQUE require a semantic bound. */
XR_FUNC uint32_t xir_storage_flow_remainder(const XirStorageFlow *);
/* Every borrow must be rebound after a query or link can grow storage. */
XR_FUNC XrXirStatus xir_storage_flow_view(const XirStorageFlow *, XirStorageFlowView *);
/* Sealing retires body borrows. The resulting numeric view stays valid until
 * free. A sealed owner rejects further construction queries and links. */
XR_FUNC XrXirStatus xir_storage_flow_seal(XirStorageFlow *, XirStorageFlowView *);
typedef struct XirStorageRelocation {
    uint32_t function, physical_values, instruction_count;
    const uint32_t *values, *instructions;
} XirStorageRelocation;
/* A batch remaps physical values and instruction coordinates once. Aux values
 * keep their offset after the new physical prefix. Failure preserves the owner. */
XR_FUNC XrXirStatus xir_storage_flow_relocate(XirStorageFlow *, const XirStorageRelocation *,
    uint32_t count, XrCompileRequestControl *);
XR_FUNC void xir_storage_flow_free(XirStorageFlow *);
#endif // XXIR_STORAGE_FLOW_H
