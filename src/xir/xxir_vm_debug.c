/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_debug.c - Bounded VM frames and owned stopped snapshots
 */
#include "xxir_vm_debug_internal.h"
#include "xxir_value_internal.h"
#include "xxir_internal.h"
#include "xxir_types.h"
#include "xxir_scalar_core.h"
#include "xxir_instance_value.h"
#include <stdatomic.h>
#include <string.h>

typedef struct DebugFrame {
    XirVmDebugPoint point;
    uint64_t id;
    unsigned char *known;
} DebugFrame;
typedef struct DebugBreakpoint {
    uint32_t function, instruction;
    bool enabled;
} DebugBreakpoint;
typedef struct SnapshotFrame {
    XrXirVmDebugFrameInfo info;
    XrXirValue name;
    XrXirValue *values, *cell_values;
    unsigned char *known;
} SnapshotFrame;
static const uint64_t debug_identity = UINT64_C(0x5852564d44424731);
static const uint64_t snapshot_identity = UINT64_C(0x5852564d534e5031);
struct XrXirVmDebugSnapshot {
    uint64_t identity;
    _Atomic uint32_t references;
    uint32_t count;
    XrXirDomain *domain;
    SnapshotFrame *frames;
};
struct XrXirVmDebug {
    uint64_t identity;
    _Atomic uint32_t references;
    uint32_t capacity, count;
    XrXirDomain *domain;
    const XrXirArtifact *artifact;
    XrXirProgram *program;
    XrXirInstance *target;
    const XrXirCall *activation;
    DebugFrame *frames;
    DebugBreakpoint breakpoints[32];
    uint64_t next_id, epoch, skip_id;
    uint32_t skip_instruction, step_depth;
    bool skip_once, disabled;
    _Atomic unsigned mode;
    _Atomic bool paused;
    XrXirVmDebugSnapshot *snapshot;
};
static bool debug_valid(const XrXirVmDebug *debug) {
    return debug && debug->identity == debug_identity && atomic_load_explicit(&debug->references, memory_order_relaxed) &&
        xr_xir_domain_current(debug->domain);
}
static bool snapshot_valid(const XrXirVmDebugSnapshot *snapshot) {
    return snapshot && snapshot->identity == snapshot_identity &&
        atomic_load_explicit(&snapshot->references, memory_order_relaxed) &&
        xr_xir_domain_current(snapshot->domain);
}
static void debug_deallocate(XrXirDomain *domain, void *memory, uint64_t bytes) {
    if (memory) xr_xir_domain_metadata_deallocate(domain, memory, bytes);
}
static void *debug_allocate(XrXirDomain *domain, uint64_t bytes, XrXirValueStatus *status) {
    void *memory = xr_xir_domain_metadata_allocate(domain, bytes, status);
    if (memory) memset(memory, 0, (size_t)bytes);
    return memory;
}
XR_FUNC XrXirValueStatus xr_xir_vm_debug_new(XrXirDomain *domain, uint32_t capacity,
    XrXirVmDebug **output) {
    if (!output || *output || !capacity || !xr_xir_domain_current(domain))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_retain(domain)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    XrXirValueStatus status;
    XrXirVmDebug *debug = debug_allocate(domain, sizeof(*debug), &status);
    if (!debug) { xr_xir_domain_drop(domain); return status; }
    debug->identity = debug_identity;
    debug->domain = domain;
    atomic_init(&debug->references, 1);
    debug->capacity = capacity;
    debug->frames = debug_allocate(domain, (uint64_t)capacity * sizeof(*debug->frames), &status);
    if (!debug->frames) {
        debug_deallocate(domain, debug, sizeof(*debug));
        xr_xir_domain_drop(domain);
        return status;
    }
    atomic_init(&debug->mode, XR_XIR_VM_DEBUG_CONTINUE);
    atomic_init(&debug->paused, false);
    *output = debug;
    return XR_XIR_VALUE_OK;
}
XR_FUNC void xr_xir_vm_debug_snapshot_drop(XrXirVmDebugSnapshot *snapshot) {
    if (!snapshot) return;
    XR_CHECK(snapshot_valid(snapshot), "invalid VM snapshot owner");
    if (!xr_xir_reference_release(&snapshot->references)) return;
    XrXirDomain *domain = snapshot->domain;
    for (uint32_t i = 0; i < snapshot->count; ++i) {
        SnapshotFrame *frame = &snapshot->frames[i];
        if (frame->values) for (uint32_t j = 0; j < frame->info.value_count; ++j)
            xr_xir_value_drop(&frame->values[j]);
        if (frame->cell_values) for (uint32_t j = 0; j < frame->info.value_count; ++j)
            xr_xir_value_drop(&frame->cell_values[j]);
        xr_xir_value_drop(&frame->name);
        debug_deallocate(domain, frame->values,
            (uint64_t)frame->info.value_count * sizeof(*frame->values));
        debug_deallocate(domain, frame->cell_values,
            (uint64_t)frame->info.value_count * sizeof(*frame->cell_values));
        debug_deallocate(domain, frame->known, frame->info.value_count);
    }
    debug_deallocate(domain, snapshot->frames,
        (uint64_t)snapshot->count * sizeof(*snapshot->frames));
    debug_deallocate(domain, snapshot, sizeof(*snapshot));
    xr_xir_domain_drop(domain);
}
static void clear_snapshot(XrXirVmDebug *debug) {
    xr_xir_vm_debug_snapshot_drop(debug->snapshot);
    debug->snapshot = NULL;
    atomic_store_explicit(&debug->paused, false, memory_order_release);
}
XR_FUNC bool xir_vm_debug_retain(XrXirVmDebug *debug) {
    return debug_valid(debug) && xr_xir_reference_retain(&debug->references);
}
XR_FUNC void xr_xir_vm_debug_drop(XrXirVmDebug *debug) {
    if (!debug) return;
    XR_CHECK(debug_valid(debug), "invalid VM debug owner");
    if (!xr_xir_reference_release(&debug->references)) return;
    XR_CHECK(!debug->count && !debug->artifact, "VM debug owner still bound");
    clear_snapshot(debug);
    XrXirDomain *domain = debug->domain;
    debug_deallocate(domain, debug->frames,
        (uint64_t)debug->capacity * sizeof(*debug->frames));
    debug_deallocate(domain, debug, sizeof(*debug));
    xr_xir_domain_drop(domain);
}
XR_FUNC bool xir_vm_debug_bind(XrXirVmDebug *debug, const XrXirArtifact *artifact) {
    if (!debug_valid(debug) || debug->disabled || debug->artifact || debug->count || !artifact) return false;
    debug->artifact = artifact;
    return true;
}
XR_FUNC void xir_vm_debug_program(XrXirVmDebug *debug, XrXirProgram *program) {
    XR_CHECK(debug_valid(debug) && debug->artifact && !debug->program, "debug Program binding invalid");
    debug->program = program;
}
XR_FUNC XrXirCallStatus xr_xir_vm_debug_instance_new(XrXirVmDebug *debug,
    XrXirProgram *program, const XrXirInstanceConfig *config, XrXirInstance **output) {
    if (!debug_valid(debug) || !program || debug->program != program || debug->target ||
        debug->disabled || !output || *output) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallStatus status = xr_xir_instance_new(program, config, output);
    if (status == XR_XIR_CALL_READY) debug->target = *output;
    return status;
}
XR_FUNC void xir_vm_debug_unbind(XrXirVmDebug *debug) {
    XR_CHECK(debug_valid(debug) && !debug->count, "live debug program retired");
    clear_snapshot(debug);
    debug->artifact = NULL;
    debug->program = NULL;
    debug->target = NULL;
}
XR_FUNC XrXirValueStatus xr_xir_vm_debug_breakpoint(XrXirVmDebug *debug,
    uint32_t function, uint32_t instruction, bool enabled) {
    if (!debug_valid(debug) || !debug->artifact ||
        (debug->count && !xr_xir_vm_debug_paused(debug))) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirModule *module = xr_xir_compile_artifact_module(debug->artifact);
    if (!module || function >= module->function_count ||
        instruction >= module->functions[function].instruction_count) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(debug->domain, 32)) return XR_XIR_VALUE_LIMIT;
    DebugBreakpoint *empty = NULL;
    for (unsigned i = 0; i < 32; ++i) {
        DebugBreakpoint *bp = &debug->breakpoints[i];
        if (bp->enabled && bp->function == function && bp->instruction == instruction) {
            bp->enabled = enabled;
            return XR_XIR_VALUE_OK;
        }
        if (!bp->enabled && !empty) empty = bp;
    }
    if (!enabled) return XR_XIR_VALUE_OK;
    if (!empty) return XR_XIR_VALUE_LIMIT;
    *empty = (DebugBreakpoint){function, instruction, true};
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_vm_debug_breakpoints_replace(XrXirVmDebug *debug,
    const XrXirVmDebugLocation *locations, uint32_t count) {
    if (!debug_valid(debug) || !debug->artifact || count>32 || (!locations && count) ||
        (debug->count && !xr_xir_vm_debug_paused(debug))) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirModule *module=xr_xir_compile_artifact_module(debug->artifact);
    DebugBreakpoint fresh[32]={{0}};
    if (!xr_xir_domain_work(debug->domain,(uint64_t)count+32)) return XR_XIR_VALUE_LIMIT;
    for (uint32_t i=0;i<count;++i) {
        if (locations[i].function>=module->function_count || locations[i].instruction>=
            module->functions[locations[i].function].instruction_count) return XR_XIR_VALUE_BAD_ARGUMENT;
        fresh[i]=(DebugBreakpoint){locations[i].function,locations[i].instruction,true};
    }
    memcpy(debug->breakpoints,fresh,sizeof(fresh));return XR_XIR_VALUE_OK;
}
XR_FUNC void xr_xir_vm_debug_request_pause(XrXirVmDebug *debug) {
    if (debug_valid(debug)) atomic_store_explicit(&debug->mode, XR_XIR_VM_DEBUG_PAUSE, memory_order_release);
}
XR_FUNC bool xr_xir_vm_debug_paused(const XrXirVmDebug *debug) {
    return debug && debug->identity == debug_identity && atomic_load_explicit(&debug->paused, memory_order_acquire);
}
XR_FUNC uint64_t xr_xir_vm_debug_epoch(const XrXirVmDebug *debug) {
    return xr_xir_vm_debug_paused(debug) ? debug->epoch : 0;
}
XR_FUNC void xr_xir_vm_debug_disable(XrXirVmDebug *debug) {
    if (!debug_valid(debug)) return;
    debug->disabled = true;
    clear_snapshot(debug);
}
XR_FUNC XrXirValueStatus xr_xir_vm_debug_continue(XrXirVmDebug *debug,
    uint64_t epoch, XrXirVmDebugMode mode) {
    if (!debug_valid(debug) || !xr_xir_vm_debug_paused(debug) || !debug->snapshot ||
        (unsigned)mode > XR_XIR_VM_DEBUG_STEP_OUT || mode == XR_XIR_VM_DEBUG_PAUSE ||
        !epoch || epoch != debug->epoch) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(debug->domain, 1)) return XR_XIR_VALUE_LIMIT;
    debug->skip_id = debug->frames[debug->count - 1].id;
    debug->skip_instruction = debug->frames[debug->count - 1].point.instruction;
    debug->skip_once = true;
    debug->step_depth = debug->count;
    atomic_store_explicit(&debug->mode, (unsigned)mode, memory_order_release);
    clear_snapshot(debug);
    return XR_XIR_VALUE_OK;
}
static DebugFrame *find_frame(XrXirVmDebug *debug, void *state) {
    for (uint32_t i = 0; i < debug->count; ++i)
        if (debug->frames[i].point.state == state) return &debug->frames[i];
    return NULL;
}
static XrXirValueStatus register_frame(XrXirVmDebug *debug, const XirVmDebugPoint *point,
    DebugFrame **output) {
    XirVmDebugPoint stored = *point;
    stored.view = NULL;
    DebugFrame *frame = find_frame(debug, point->state);
    if (frame) { frame->point = stored; *output = frame; return XR_XIR_VALUE_OK; }
    if (debug->count >= debug->capacity || debug->next_id == UINT64_MAX ||
        (debug->activation && debug->activation != point->view->activation)) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status;
    unsigned char *known = debug_allocate(debug->domain, point->layout->slot_count, &status);
    if (!known) return status;
    const XrXirModule *module = xr_xir_compile_artifact_module(point->binding->artifact);
    const XrXirFunction *function = &module->functions[point->binding->function];
    if (!xr_xir_domain_work(debug->domain, function->parameter_count)) {
        debug_deallocate(debug->domain, known, point->layout->slot_count);
        return XR_XIR_VALUE_LIMIT;
    }
    memset(known, 1, function->parameter_count);
    frame = &debug->frames[debug->count++];
    *frame = (DebugFrame){stored, ++debug->next_id, known};
    debug->activation = point->view->activation;
    *output = frame;
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus copy_frame_values(XrXirVmDebug *debug, const DebugFrame *live,
    XrXirCallView *view, SnapshotFrame *frame) {
    const XrXirModule *module = xr_xir_compile_artifact_module(live->point.binding->artifact);
    const XrXirFunction *function = &module->functions[live->point.binding->function];
    frame->info = (XrXirVmDebugFrameInfo){live->id, debug->epoch + 1,
        live->point.binding->function, live->point.instruction, live->point.layout->slot_count};
    XrXirValueStatus status = xr_xir_string_new(debug->domain,
        function->name ? function->name : "", function->name_length, &frame->name);
    if (status != XR_XIR_VALUE_OK) return status;
    uint32_t count = frame->info.value_count;
    frame->values = debug_allocate(debug->domain, (uint64_t)count * sizeof(*frame->values), &status);
    if (!frame->values) return status;
    frame->cell_values = debug_allocate(debug->domain, (uint64_t)count * sizeof(*frame->cell_values), &status);
    if (!frame->cell_values) return status;
    frame->known = debug_allocate(debug->domain, count, &status);
    if (!frame->known) return status;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    if (!admission) return XR_XIR_VALUE_BAD_ARGUMENT;
    for (uint32_t i = 0; i < count; ++i) if (live->known[i]) {
        XrXirType type = xr_xir_operand_type(function, i);
        if (type == XR_XIR_UNIT) continue;
        XrXirValue value = {(uint32_t)type, 0,
            xr_xir_scalar_load(live->point.frame, live->point.layout->offsets[i])};
        /* A moved owned slot is canonical empty storage, not a readable value. */
        if (!value.payload && xr_xir_type_is_owned(module->types, type) &&
            !xr_xir_value_argument(&value, view->arena, type)) continue;
        status = xr_xir_value_admit(&value, type, admission);
        if (status != XR_XIR_VALUE_OK) return status;
        status = xr_xir_value_copy(&value, &frame->values[i]);
        if (status != XR_XIR_VALUE_OK) return status;
        frame->known[i] = 1;
        if (xr_xir_type_is_cell(module->types, type)) {
            XrXirValue observed = {0};
            XrXirCallStatus read = xr_xir_instance_cell_read(view, &value, &observed);
            if (read == XR_XIR_CALL_BAD_ARGUMENT || read == XR_XIR_CALL_BAD_STATE) continue;
            if (read != XR_XIR_CALL_READY) return read == XR_XIR_CALL_OOM ? XR_XIR_VALUE_OOM :
                read == XR_XIR_CALL_LIMIT ? XR_XIR_VALUE_LIMIT : XR_XIR_VALUE_BAD_ARGUMENT;
            status = xr_xir_value_admit(&observed, xr_xir_cell_element(module->types, type), admission);
            if (status == XR_XIR_VALUE_OK) status = xr_xir_value_copy(&observed, &frame->cell_values[i]);
            xr_xir_value_drop(&observed);
            if (status != XR_XIR_VALUE_OK) return status;
            frame->known[i] |= 2;
        }
    }
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus capture(XrXirVmDebug *debug, XrXirCallView *view) {
    if (debug->epoch == UINT64_MAX) return XR_XIR_VALUE_LIMIT;
    if (!xr_xir_domain_work(debug->domain, (uint64_t)debug->count + 1)) return XR_XIR_VALUE_LIMIT;
    if (!xr_xir_domain_retain(debug->domain)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    XrXirValueStatus status;
    XrXirVmDebugSnapshot *snapshot = debug_allocate(debug->domain, sizeof(*snapshot), &status);
    if (!snapshot) { xr_xir_domain_drop(debug->domain); return status; }
    snapshot->identity = snapshot_identity;
    atomic_init(&snapshot->references, 1);
    snapshot->domain = debug->domain;
    snapshot->count = debug->count;
    snapshot->frames = debug_allocate(debug->domain,
        (uint64_t)snapshot->count * sizeof(*snapshot->frames), &status);
    if (!snapshot->frames) goto fail;
    for (uint32_t i = 0; i < snapshot->count; ++i) {
        const DebugFrame *live = &debug->frames[debug->count - 1 - i];
        if (!xr_xir_domain_work(debug->domain, live->point.layout->slot_count)) {
            status = XR_XIR_VALUE_LIMIT; goto fail;
        }
        status = copy_frame_values(debug, live, view, &snapshot->frames[i]);
        if (status != XR_XIR_VALUE_OK) goto fail;
    }
    debug->snapshot = snapshot;
    ++debug->epoch;
    atomic_store_explicit(&debug->paused, true, memory_order_release);
    return XR_XIR_VALUE_OK;
fail:
    if (!snapshot->frames) snapshot->count = 0;
    xr_xir_vm_debug_snapshot_drop(snapshot);
    return status;
}
XR_FUNC XrXirValueStatus xir_vm_debug_before(XrXirVmDebug *debug,
    const XirVmDebugPoint *point, bool *stopped) {
    *stopped = false;
    if (!debug_valid(debug) || debug->artifact != point->binding->artifact)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (debug->disabled) return XR_XIR_VALUE_OK;
    if (point->view->instance != debug->target) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (xr_xir_call_execution_status(point->view) != XR_XIR_CALL_READY ||
        xr_xir_call_cleanup_active(point->view->activation)) return XR_XIR_VALUE_OK;
    if (xr_xir_vm_debug_paused(debug)) { *stopped = true; return XR_XIR_VALUE_OK; }
    if (!xr_xir_domain_work(debug->domain, (uint64_t)debug->count * 2 + 33))
        return XR_XIR_VALUE_LIMIT;
    DebugFrame *frame;
    XrXirValueStatus status = register_frame(debug, point, &frame);
    if (status != XR_XIR_VALUE_OK) return status;
    if (debug->skip_once && frame->id == debug->skip_id &&
        point->instruction == debug->skip_instruction) {
        debug->skip_once = false;
        return XR_XIR_VALUE_OK;
    }
    unsigned mode = atomic_load_explicit(&debug->mode, memory_order_acquire);
    bool stop = mode == XR_XIR_VM_DEBUG_PAUSE || mode == XR_XIR_VM_DEBUG_STEP_IN ||
        (mode == XR_XIR_VM_DEBUG_STEP_OVER && debug->count <= debug->step_depth) ||
        (mode == XR_XIR_VM_DEBUG_STEP_OUT && debug->count < debug->step_depth);
    for (unsigned i = 0; i < 32 && !stop; ++i)
        stop = debug->breakpoints[i].enabled &&
            debug->breakpoints[i].function == point->binding->function &&
            debug->breakpoints[i].instruction == point->instruction;
    if (!stop) return XR_XIR_VALUE_OK;
    status = capture(debug, point->view);
    if (status == XR_XIR_VALUE_OK) *stopped = true;
    return status;
}
XR_FUNC void xir_vm_debug_after(XrXirVmDebug *debug, const XirVmDebugPoint *point,
    XrXirActionKind action) {
    if (debug->disabled) return;
    DebugFrame *frame = find_frame(debug, point->state);
    if (!frame) return;
    const XrXirModule *module = xr_xir_compile_artifact_module(point->binding->artifact);
    const XrXirFunction *function = &module->functions[point->binding->function];
    if (point->instruction >= function->instruction_count) return;
    const XrXirInstruction *op = &function->instructions[point->instruction];
    uint32_t id = function->parameter_count + point->instruction;
    if (op->op == XR_XIR_LOCAL_UNINIT) frame->known[id] = 0;
    else if (action == XR_XIR_ACTION_CALL || action == XR_XIR_ACTION_AWAIT_TASK)
        frame->known[id] = 0;
    else if (action != XR_XIR_ACTION_FAULT && op->type != XR_XIR_UNIT && op->op != XR_XIR_LOCAL_UNINIT)
        frame->known[id] = 1;
    if (action != XR_XIR_ACTION_FAULT && (op->op == XR_XIR_SCALAR_LOCAL_WRITE ||
        op->op == XR_XIR_OWNED_LOCAL_WRITE || op->op == XR_XIR_CELL_LOCAL_WRITE))
        frame->known[op->args[0]] = 1;
}
XR_FUNC XrXirValueStatus xir_vm_debug_inbox(XrXirVmDebug *debug, void *state, uint32_t offset) {
    if (debug->disabled || offset == UINT32_MAX) return XR_XIR_VALUE_OK;
    if (!xr_xir_domain_work(debug->domain, debug->count)) return XR_XIR_VALUE_LIMIT;
    DebugFrame *frame = find_frame(debug, state);
    if (!frame) return XR_XIR_VALUE_OK;
    if (!xr_xir_domain_work(debug->domain, frame->point.layout->slot_count)) return XR_XIR_VALUE_LIMIT;
    for (uint32_t i = 0; i < frame->point.layout->slot_count; ++i)
        if (frame->point.layout->offsets[i] == offset) frame->known[i] = 1;
    return XR_XIR_VALUE_OK;
}
XR_FUNC void xir_vm_debug_release(XrXirVmDebug *debug, void *state) {
    DebugFrame *frame = find_frame(debug, state);
    if (!frame) return;
    uint32_t at = (uint32_t)(frame - debug->frames);
    XR_CHECK(at + 1 == debug->count, "debug VM frames released out of order");
    debug_deallocate(debug->domain, frame->known, frame->point.layout->slot_count);
    memset(frame, 0, sizeof(*frame));
    --debug->count;
    if (!debug->count) { clear_snapshot(debug); debug->activation = NULL; }
}
XR_FUNC XrXirValueStatus xr_xir_vm_debug_snapshot(XrXirVmDebug *debug,
    XrXirVmDebugSnapshot **output) {
    if (!output || *output || !debug_valid(debug) || !xr_xir_vm_debug_paused(debug) ||
        !debug->snapshot) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(debug->domain, 1)) return XR_XIR_VALUE_LIMIT;
    if (!xr_xir_reference_retain(&debug->snapshot->references)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    *output = debug->snapshot;
    return XR_XIR_VALUE_OK;
}
XR_FUNC uint32_t xr_xir_vm_debug_snapshot_depth(const XrXirVmDebugSnapshot *snapshot) {
    return snapshot_valid(snapshot) ? snapshot->count : 0;
}
XR_FUNC XrXirValueStatus xr_xir_vm_debug_snapshot_frame(const XrXirVmDebugSnapshot *snapshot,
    uint32_t index, XrXirVmDebugFrameInfo *output, XrXirValue *name) {
    if (!snapshot_valid(snapshot) || index >= snapshot->count || !output || !name ||
        name->type || name->reserved || name->payload) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(snapshot->domain, 1)) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status = xr_xir_value_copy(&snapshot->frames[index].name, name);
    if (status == XR_XIR_VALUE_OK) *output = snapshot->frames[index].info;
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_vm_debug_snapshot_value(const XrXirVmDebugSnapshot *snapshot,
    uint32_t frame, uint32_t value, XrXirValue *output) {
    if (!snapshot_valid(snapshot) || frame >= snapshot->count || !output ||
        value >= snapshot->frames[frame].info.value_count ||
        !snapshot->frames[frame].known[value]) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(snapshot->domain, 1)) return XR_XIR_VALUE_LIMIT;
    return xr_xir_value_copy(&snapshot->frames[frame].values[value], output);
}

XR_FUNC XrXirValueStatus xr_xir_vm_debug_snapshot_cell_value(const XrXirVmDebugSnapshot *snapshot,
    uint32_t frame, uint32_t value, XrXirValue *output) {
    if (!snapshot_valid(snapshot) || frame >= snapshot->count || !output ||
        value >= snapshot->frames[frame].info.value_count || output->type || output->reserved || output->payload)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!(snapshot->frames[frame].known[value] & 2)) return XR_XIR_VALUE_UNSUPPORTED;
    if (!xr_xir_domain_work(snapshot->domain, 1)) return XR_XIR_VALUE_LIMIT;
    return xr_xir_value_copy(&snapshot->frames[frame].cell_values[value], output);
}
