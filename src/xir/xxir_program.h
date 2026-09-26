/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_program.h - Immutable code ownership and isolated module instances
 *
 * KEY CONCEPT:
 *   Programs own code leases; instances own initialization, slots and execution.
 */
#ifndef XXIR_PROGRAM_H
#define XXIR_PROGRAM_H
#include "xxir.h"
#include "xxir_call.h"

#define XR_XIR_PROGRAM_ABI_VERSION 4u
typedef struct XrXirProgram XrXirProgram;
typedef struct XrXirInstance XrXirInstance;
typedef struct XrXirCodeLease {
    void *owner;
    void (*release)(void *owner);
} XrXirCodeLease;
typedef struct XrXirProgramSpec {
    uint32_t abi_version;
    XrXirTarget target;
    const XrXirCallEntry *entries;
    uint32_t entry_count;
    const XrXirDeclarations *declarations;
    XrXirCodeLease code;
    const XrXirCallableTypes *callables;
} XrXirProgramSpec;
typedef enum XrXirInstanceState {
    XR_XIR_INSTANCE_NEW, XR_XIR_INSTANCE_INITIALIZING, XR_XIR_INSTANCE_READY,
    XR_XIR_INSTANCE_FAILED, XR_XIR_INSTANCE_DRAINING
} XrXirInstanceState;
typedef enum XrXirLifecycleEvent {
    XR_XIR_MODULE_BEGIN, XR_XIR_MODULE_READY, XR_XIR_SLOT_PUBLISHED, XR_XIR_SLOT_RELEASED
} XrXirLifecycleEvent;
typedef void (*XrXirLifecycleEntry)(void *context, XrXirLifecycleEvent event, uint32_t index);
typedef struct XrXirInstanceConfig {
    uint64_t metadata_limit, value_limit, call_limit, poll_limit;
    uint32_t depth_limit;
    XrXirOutputProvider output;
    XrXirLifecycleEntry trace;
    void *trace_context;
} XrXirInstanceConfig;
typedef struct XrXirInstanceResult {
    XrXirCallResult outcome;
    uint64_t epoch;
} XrXirInstanceResult;

/* Successful sealing takes the code lease. A null lease declares static code. */
XR_FUNC XrXirStatus xr_xir_program_seal(const XrXirProgramSpec *spec, uint64_t byte_limit,
                                      XrXirProgram **output);
XR_FUNC void xr_xir_program_drop(XrXirProgram *program);
XR_FUNC XrXirInstanceConfig xr_xir_instance_defaults(void);
XR_FUNC XrXirCallStatus xr_xir_instance_new(XrXirProgram *program, const XrXirInstanceConfig *config,
                                          XrXirInstance **output);
XR_FUNC XrXirInstanceState xr_xir_instance_state(const XrXirInstance *instance);
XR_FUNC XrXirCallStatus xr_xir_instance_start(XrXirInstance *instance, uint32_t entry,
    const XrXirValue *arguments, uint32_t count);
XR_FUNC XrXirInstanceResult xr_xir_instance_poll(XrXirInstance *instance);
XR_FUNC XrXirCallStatus xr_xir_instance_resume(XrXirInstance *instance, uint64_t epoch, uint64_t wake);
XR_FUNC XrXirCallStatus xr_xir_instance_take_result(XrXirInstance *instance, XrXirValue *output);
XR_FUNC XrXirCallStatus xr_xir_instance_copy_failure(XrXirInstance *instance, XrXirValue *output);
XR_FUNC XrXirCallStatus xr_xir_instance_stop(XrXirInstance *instance);
XR_FUNC XrXirCallStatus xr_xir_instance_free(XrXirInstance *instance);
XR_FUNC XrXirCallStatus xr_xir_instance_start_function(XrXirInstance *instance, const XrXirValue *function,
    const XrXirValue *arguments, uint32_t count);
XR_FUNC XrXirCallStatus xr_xir_instance_function(XrXirCallView *view, XrXirType type, uint32_t entry,
    const XrXirValue *captures, uint32_t count, XrXirValue *output);
XR_FUNC XrXirCallStatus xr_xir_instance_resolve_function(XrXirCallView *view, const XrXirValue *function, uint32_t *entry);
/* Execution helpers require a view from the instance's active callback. */
XR_FUNC XrXirCallStatus xr_xir_instance_literal(XrXirCallView *view, uint32_t literal, XrXirValue *output);
XR_FUNC XrXirCallStatus xr_xir_instance_atomic(XrXirCallView *view, int64_t initial, XrXirValue *output);
XR_FUNC XrXirCallStatus xr_xir_instance_slot_read(XrXirCallView *view, uint32_t slot, XrXirValue *output);
XR_FUNC XrXirCallStatus xr_xir_instance_slot_write(XrXirCallView *view, uint32_t slot,
                                                 const XrXirValue *value, bool publish);
#endif // XXIR_PROGRAM_H
