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

#define XR_XIR_PROGRAM_ABI_VERSION 28u
typedef struct XrXirProgram XrXirProgram;
typedef struct XrXirInstance XrXirInstance;
typedef struct XrXirCodeLease {
    void *owner;
    void (*release)(void *owner);
} XrXirCodeLease;
typedef struct XrXirProgramProof {
    const uint8_t *bytes;
    size_t length;
    const uint8_t *identity;
    const XrXirFunctionLayout *layouts;
} XrXirProgramProof;
/* Borrowed until the source artifact is destroyed. */
XR_FUNC XrXirProgramProof xr_xir_compile_program_proof(const XrXirArtifact *artifact);
typedef struct XrXirProgramSpec {
    uint32_t abi_version;
    XrXirTarget target;
    const XrXirCallEntry *entries;
    uint32_t entry_count;
    const XrXirDeclarations *declarations;
    XrXirCodeLease code;
    const XrXirTypes *types;
    XrXirProgramProof proof;
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
    uint32_t abi_version, struct_size;
    uint64_t metadata_limit, value_limit, call_limit, poll_limit;
    uint32_t depth_limit;
    XrXirOutputProvider output;
    XrXirLifecycleEntry trace;
    void *trace_context;
    XrXirTimeProvider time;
} XrXirInstanceConfig;
typedef struct XrXirInstanceResult {
    XrXirCallResult outcome;
    uint64_t epoch;
} XrXirInstanceResult;

/* Successful sealing takes the code lease. A null lease declares static code. */
XR_FUNC XrXirStatus xr_xir_compile_program_seal(const XrXirCompileContext *context,
    const XrXirProgramSpec *spec, XrXirProgram **output);
XR_FUNC void xr_xir_compile_program_drop(XrXirProgram *program);
XR_FUNC XrXirCallStatus xr_xir_instance_config_init(XrXirInstanceConfig *config, size_t size);
XR_FUNC XrXirCallStatus xr_xir_instance_weaken_function(XrXirCallView *view,
    XrXirType type, const XrXirValue *input, XrXirValue *output);
XR_FUNC XrXirCallStatus xr_xir_instance_new(XrXirProgram *program, const XrXirInstanceConfig *config,
                                          XrXirInstance **output);
XR_FUNC XrXirInstanceState xr_xir_instance_state(const XrXirInstance *instance);
XR_FUNC XrXirCallStatus xr_xir_instance_start(XrXirInstance *instance, uint32_t entry,
    const XrXirValue *arguments, uint32_t count);
/* Starts an admitted root-module test or hook, including private declarations.
 * Skipped tests and ordinary functions receive no additional authority. The
 * same initialization and call lifecycle apply as for ordinary entry starts. */
XR_FUNC XrXirCallStatus xr_xir_instance_start_test(XrXirInstance *instance, uint32_t entry);
XR_FUNC XrXirInstanceResult xr_xir_instance_poll_bounded(XrXirInstance *instance, uint64_t quantum);
/* Cancels only the current activation; initialized module state remains usable. */
XR_FUNC XrXirCallStatus xr_xir_instance_cancel_current(XrXirInstance *instance);
XR_FUNC XrXirCallStatus xr_xir_instance_resume(XrXirInstance *instance, uint64_t epoch, uint64_t wake);
/* Publishes the host request of the current suspension for its exact epoch and
 * wake token. Failure leaves output unchanged; nothing is read or resumed. */
XR_FUNC XrXirCallStatus xr_xir_instance_wait_request(const XrXirInstance *instance, uint64_t epoch,
    uint64_t wake, XrXirWaitRequest *output);
/* Transfers ordinary entry outcomes. Initialization failure stays instance-owned:
 * take_result returns BAD_STATE without changing output; use copy_failure. */
XR_FUNC XrXirCallStatus xr_xir_instance_take_result(XrXirInstance *instance, XrXirValue *output);
XR_FUNC XrXirCallStatus xr_xir_instance_copy_failure(XrXirInstance *instance, XrXirCallResult *output);
XR_FUNC XrXirCallStatus xr_xir_instance_stop(XrXirInstance *instance);
/* Consumes the instance except on BUSY; reports a failed language cleanup. */
XR_FUNC XrXirCallStatus xr_xir_instance_free(XrXirInstance *instance);
XR_FUNC XrXirCallStatus xr_xir_instance_start_function(XrXirInstance *instance, const XrXirValue *function,
    const XrXirValue *arguments, uint32_t count);
XR_FUNC XrXirCallStatus xr_xir_instance_function(XrXirCallView *view, XrXirType type, uint32_t entry,
    const XrXirValue *captures, uint32_t count, XrXirValue *output);
XR_FUNC XrXirCallStatus xr_xir_instance_resolve_function(XrXirCallView *view, const XrXirValue *function, uint32_t *entry);
/* Execution helpers require a view from the instance's active callback. Clock
 * reads and UTC offsets are synchronous provider calls: they never suspend, a
 * provider range rejection is the numeric-range status and every other provider
 * failure, or an absent provider, is a host error. Failure leaves output unchanged. */
XR_FUNC XrXirCallStatus xr_xir_instance_clock_ns(XrXirCallView *view, XrXirClockKind clock,
    int64_t *nanoseconds);
XR_FUNC XrXirCallStatus xr_xir_instance_utc_offset(XrXirCallView *view, int64_t seconds,
    int64_t *minutes);
XR_FUNC XrXirCallStatus xr_xir_instance_literal(XrXirCallView *view, uint32_t literal, XrXirValue *output);
/* Owned string spelling of one bool or number value, as `print` would write it. */
XR_FUNC XrXirCallStatus xr_xir_instance_scalar_text(XrXirCallView *view, const XrXirValue *scalar,
    XrXirValue *output);
XR_FUNC XrXirCallStatus xr_xir_instance_atomic(XrXirCallView *view, int64_t initial, XrXirValue *output);
XR_FUNC XrXirCallStatus xr_xir_instance_cell(XrXirCallView *view, XrXirType type,
    const XrXirValue *initial, XrXirValue *output);
XR_FUNC XrXirCallStatus xr_xir_instance_cell_read(XrXirCallView *view, const XrXirValue *cell, XrXirValue *output);
XR_FUNC XrXirCallStatus xr_xir_instance_cell_write(XrXirCallView *view, const XrXirValue *cell, const XrXirValue *value);
XR_FUNC XrXirCallStatus xr_xir_instance_slot_read(XrXirCallView *view, uint32_t slot, XrXirValue *output);
XR_FUNC XrXirCallStatus xr_xir_instance_slot_write(XrXirCallView *view, uint32_t slot,
                                                 const XrXirValue *value, bool publish);
/* Enter a protected region's handler with a panic action: bind a new
 * PanicInfo at destination (UINT32_MAX for an unbound handler), store
 * handler_pc and continue. Every other action is returned unchanged; a
 * failed binding becomes that resource failure, which no handler observes. */
XR_FUNC XrXirAction xr_xir_instance_panic_land(XrXirCallView *view, void *frame, XrXirAction action,
    uint32_t destination, uint32_t handler_pc, uint32_t *pc);
#endif // XXIR_PROGRAM_H
