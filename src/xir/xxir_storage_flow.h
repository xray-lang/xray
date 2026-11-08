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

typedef struct XirStorageFlowFunction {
    uint32_t physical_values, value_count;
    const XrXirValueFlow *flows;
    uint32_t flow_count;
} XirStorageFlowFunction;

typedef struct XirStorageCrossFlow {
    uint32_t source_function, source, target_function, target;
    /* UINT32_MAX denotes an ordinary relation. Other values index applications. */
    uint32_t application;
} XirStorageCrossFlow;

typedef struct XirStorageBinding {
    uint32_t formal_function, formal_value, actual_function, actual_value;
    /* Every non-sentinel value, including zero, indexes applications. */
    uint32_t actual_environment;
} XirStorageBinding;

typedef struct XirStorageApplication {
    XirExecutionSiteKind kind;
    /* Function is the caller template. Declaration is the callee template. */
    uint32_t site, function, instruction, declaration;
    uint32_t target, caller_instance, environment, order;
    XirExecutionPort port;
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
 * The declaration identity determines whether it is a hidden capture. */
typedef struct XirStorageInput { uint32_t function, value, handle; } XirStorageInput;

typedef struct XirStorageUnknown {
    uint32_t function, value;
    XrXirType type;
} XirStorageUnknown;

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
} XirStorageFlowView;

typedef struct XirStorageFlow XirStorageFlow;

/* Input bodies and the unique execution owner are synchronous construction
 * borrows. No instruction or caller-owned array survives sealing. */
XR_FUNC XrXirStatus xir_storage_flow_new(const XrXirCompileContext *, const XrXirModule *,
    const XirExecutionOwner *, XrCompileRequestControl *, XirStorageFlow **);
/* New real execution edges extend applications monotonically. Read-origin
 * consumers must use this graph before closing indirect call candidates. */
XR_FUNC XrXirStatus xir_storage_flow_link(XirStorageFlow *, const XirExecutionOwner *, bool *changed);
/* Returns the contents immediately before instruction. Physical handles retain
 * identity independently of contents. Queries may append numeric aux values. */
XR_FUNC XrXirStatus xir_storage_flow_contents(XirStorageFlow *, uint32_t function,
    uint32_t instruction, uint32_t handle, uint32_t *value);
/* Every borrow must be rebound after a query or link can grow storage. */
XR_FUNC XrXirStatus xir_storage_flow_view(const XirStorageFlow *, XirStorageFlowView *);
/* Sealing retires body borrows. The resulting numeric view stays valid until
 * free. A sealed owner rejects further construction queries and links. */
XR_FUNC XrXirStatus xir_storage_flow_seal(XirStorageFlow *, XirStorageFlowView *);
XR_FUNC void xir_storage_flow_free(XirStorageFlow *);
#endif // XXIR_STORAGE_FLOW_H
