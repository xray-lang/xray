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
#include "xxir_vm_debug_calls_internal.h"
#include "xxir_error.h"
#include "xxir_type_arena.h"
#include "xxir_cell_owner_internal.h"
#include "xxir_instance_debug_internal.h"
#include "xxir_value_path_internal.h"
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
    XrXirVmDebugHitKind hit_kind;
    uint64_t hit_value;
    uint32_t counter,group;
} DebugBreakpoint;
typedef struct DebugSourcePoint { uint32_t module,statement,line,flags; } DebugSourcePoint;
typedef struct DebugWrite {
    XrXirValue value;
    XrXirValuePathStep *steps;
    XrXirVmDebugSnapshot *snapshot;
    uint64_t epoch,frame;
    uint32_t operand,phase,count;
    XrXirValueStatus result;
} DebugWrite;
typedef struct SnapshotFrame {
    XrXirVmDebugFrameInfo info;
    XrXirValue name;
    XrXirValue *values, *cell_values;
    unsigned char *known;
    XrXirTypeArena *read_arena;
    unsigned char *nominal_visible;
    uint32_t nominal_count,nominal_owner;
} SnapshotFrame;
static const uint64_t debug_identity = UINT64_C(0x5852564d44424731);
static const uint64_t snapshot_identity = UINT64_C(0x5852564d534e5031);
struct XrXirVmDebugSnapshot {
    uint64_t identity;
    _Atomic uint32_t references;
    uint32_t count;
    uint64_t stopped_thread;
    XrXirDomain *domain;
    SnapshotFrame *frames;
    XrXirValue exception;
    XirVmDebugStopCause cause;
};
struct XrXirVmDebug {
    uint64_t identity;
    _Atomic uint32_t references;
    uint32_t capacity, count;
    XrXirDomain *domain;
    const XrXirArtifact *artifact;
    XrXirProgram *program;
    XrXirInstance *target;
    XirDebugCalls calls;
    DebugFrame *frames;
    DebugBreakpoint breakpoints[32];
    uint64_t hits[32];
    uint64_t epoch, skip_id, step_thread,resume_id;
    uint32_t skip_instruction, step_depth,resume_instruction;
    void *source_memory;
    uint32_t source_functions;
    uint32_t *source_offsets;
    DebugSourcePoint *source_points,step_source;
    bool skip_once, disabled, faulted, break_throw;
    _Atomic unsigned mode;
    _Atomic bool paused;
    XrXirVmDebugSnapshot *snapshot;
    DebugWrite write;
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
#include "xxir_vm_debug_source.inc.c"
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
    if (!output || *output || !capacity || capacity==UINT32_MAX || !xr_xir_domain_current(domain))
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
    status=xir_debug_calls_new(&debug->calls,domain,capacity+1);
    if (status!=XR_XIR_VALUE_OK) {
        debug_deallocate(domain,debug->frames,(uint64_t)capacity*sizeof(*debug->frames));
        debug_deallocate(domain,debug,sizeof(*debug));xr_xir_domain_drop(domain);return status;
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
    xr_xir_value_drop(&snapshot->exception);
    for (uint32_t i = 0; i < snapshot->count; ++i) {
        SnapshotFrame *frame = &snapshot->frames[i];
        if (frame->values) for (uint32_t j = 0; j < frame->info.value_count; ++j)
            xr_xir_value_drop(&frame->values[j]);
        if (frame->cell_values) for (uint32_t j = 0; j < frame->info.value_count; ++j)
            xr_xir_value_drop(&frame->cell_values[j]);
        xr_xir_value_drop(&frame->name);
        xr_xir_compile_type_arena_drop(frame->read_arena);
        debug_deallocate(domain,frame->nominal_visible,frame->nominal_count);
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
    xr_xir_value_drop(&debug->write.value);xr_xir_vm_debug_snapshot_drop(debug->write.snapshot);
    debug_deallocate(debug->domain,debug->write.steps,(uint64_t)debug->write.count*sizeof(*debug->write.steps));
    XR_CHECK(!debug->count && !debug->artifact, "VM debug owner still bound");
    clear_snapshot(debug);
    XrXirDomain *domain = debug->domain;
    xir_debug_calls_free(&debug->calls);
    debug_source_clear(debug);
    debug_deallocate(domain, debug->frames,
        (uint64_t)debug->capacity * sizeof(*debug->frames));
    debug_deallocate(domain, debug, sizeof(*debug));
    xr_xir_domain_drop(domain);
}
XR_FUNC bool xir_vm_debug_bind(XrXirVmDebug *debug, const XrXirArtifact *artifact) {
    if (!debug_valid(debug) || debug->disabled || debug->artifact || debug->count || !artifact) return false;
    debug->artifact = artifact;
    debug->calls.functions=xr_xir_compile_artifact_module(artifact)->function_count;
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
    if (status == XR_XIR_CALL_READY) { debug->target = *output;debug->calls.target=*output; }
    return status;
}
XR_FUNC void xir_vm_debug_unbind(XrXirVmDebug *debug) {
    XR_CHECK(debug_valid(debug) && !debug->count, "live debug program retired");
    clear_snapshot(debug);
    debug->artifact = NULL;
    debug->program = NULL;
    debug->target = NULL;
    debug_source_clear(debug);
}
XR_FUNC XrXirValueStatus xr_xir_vm_debug_breakpoint(XrXirVmDebug *debug,
    uint32_t function, uint32_t instruction, bool enabled) {
    if (!debug_valid(debug) || !debug->artifact ||
        (debug->count && !xr_xir_vm_debug_paused(debug))) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirModule *module = xr_xir_compile_artifact_module(debug->artifact);
    if (!module || function >= module->function_count ||
        instruction >= module->functions[function].instruction_count) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(debug->domain, 96)) return XR_XIR_VALUE_LIMIT;
    DebugBreakpoint *empty = NULL,*selected=NULL;
    for (unsigned i = 0; i < 32; ++i) {
        DebugBreakpoint *bp = &debug->breakpoints[i];
        if (bp->enabled && bp->function == function && bp->instruction == instruction) {
            selected=bp;break;
        }
        if (!bp->enabled && !empty) empty = bp;
    }
    if (!enabled) { if (selected) selected->enabled=false;return XR_XIR_VALUE_OK; }
    if (!selected) selected=empty;
    if (!selected) return XR_XIR_VALUE_LIMIT;
    bool used[32]={0};
    for (unsigned i=0;i<32;++i) if (debug->breakpoints[i].enabled && &debug->breakpoints[i]!=selected)
        used[debug->breakpoints[i].counter]=true;
    uint32_t counter=0;while (counter<32 && used[counter]) ++counter;
    if (counter==32) return XR_XIR_VALUE_LIMIT;
    *selected=(DebugBreakpoint){.function=function,.instruction=instruction,.enabled=true,.counter=counter};
    debug->hits[counter]=0;
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_vm_debug_breakpoints_replace(XrXirVmDebug *debug,
    const XrXirVmDebugLocation *locations, uint32_t count) {
    if (!debug_valid(debug) || !debug->artifact || count>32 || (!locations && count) ||
        (debug->count && !xr_xir_vm_debug_paused(debug))) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirModule *module=xr_xir_compile_artifact_module(debug->artifact);
    DebugBreakpoint fresh[32]={{0}};
    if (!xr_xir_domain_work(debug->domain,(uint64_t)count+2*sizeof(fresh)+sizeof(debug->hits))) return XR_XIR_VALUE_LIMIT;
    for (uint32_t i=0;i<count;++i) {
        if (locations[i].function>=module->function_count || locations[i].instruction>=
            module->functions[locations[i].function].instruction_count) return XR_XIR_VALUE_BAD_ARGUMENT;
        fresh[i]=(DebugBreakpoint){.function=locations[i].function,.instruction=locations[i].instruction,.enabled=true,.counter=i};
    }
    memcpy(debug->breakpoints,fresh,sizeof(fresh));memset(debug->hits,0,sizeof(debug->hits));return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_vm_debug_rules_replace(XrXirVmDebug *debug,const XrXirVmDebugRule *rules,uint32_t count) {
    if (!debug_valid(debug) || !debug->artifact || count>32 || (!rules && count) || debug->disabled ||
        (debug->count && !xr_xir_vm_debug_paused(debug))) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirModule *module=xr_xir_compile_artifact_module(debug->artifact);DebugBreakpoint fresh[32]={{0}};
    if (!xr_xir_domain_work(debug->domain,(uint64_t)count*count+2*sizeof(fresh)+sizeof(debug->hits))) return XR_XIR_VALUE_LIMIT;
    for (uint32_t i=0;i<count;++i) {
        const XrXirVmDebugRule *rule=&rules[i];
        if (rule->location.function>=module->function_count || rule->location.instruction>=module->functions[rule->location.function].instruction_count ||
            (unsigned)rule->hit_kind>XR_XIR_VM_DEBUG_HIT_MULTIPLE ||
            (rule->hit_kind==XR_XIR_VM_DEBUG_HIT_ALWAYS && rule->hit_value) ||
            (rule->hit_kind==XR_XIR_VM_DEBUG_HIT_MULTIPLE && !rule->hit_value) || rule->group>32) return XR_XIR_VALUE_BAD_ARGUMENT;
        fresh[i]=(DebugBreakpoint){.function=rule->location.function,.instruction=rule->location.instruction,.enabled=true,
            .hit_kind=rule->hit_kind,.hit_value=rule->hit_value,.counter=i,.group=rule->group};
        for (uint32_t j=0;rule->group && j<i;++j) if (rules[j].group==rule->group) {
            if (rules[j].hit_kind!=rule->hit_kind || rules[j].hit_value!=rule->hit_value) return XR_XIR_VALUE_BAD_ARGUMENT;
            fresh[i].counter=fresh[j].counter;break;
        }
    }
    memcpy(debug->breakpoints,fresh,sizeof(fresh));memset(debug->hits,0,sizeof(debug->hits));return XR_XIR_VALUE_OK;
}
static bool hit_matches(const DebugBreakpoint *rule,uint64_t hits) {
    switch (rule->hit_kind) {
    case XR_XIR_VM_DEBUG_HIT_ALWAYS:return true;
    case XR_XIR_VM_DEBUG_HIT_EQUAL:return hits==rule->hit_value;
    case XR_XIR_VM_DEBUG_HIT_GREATER:return hits>rule->hit_value;
    case XR_XIR_VM_DEBUG_HIT_GREATER_EQUAL:return hits>=rule->hit_value;
    case XR_XIR_VM_DEBUG_HIT_LESS:return hits<rule->hit_value;
    case XR_XIR_VM_DEBUG_HIT_LESS_EQUAL:return hits<=rule->hit_value;
    case XR_XIR_VM_DEBUG_HIT_MULTIPLE:return hits%rule->hit_value==0;
    default:XR_CHECK(false,"invalid admitted VM breakpoint rule");return false;
    }
}
XR_FUNC XrXirValueStatus xr_xir_vm_debug_break_on_throw(XrXirVmDebug *debug, bool enabled) {
    if (!debug_valid(debug) || !debug->artifact || debug->disabled ||
        (debug->count && !xr_xir_vm_debug_paused(debug))) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(debug->domain, 1)) return XR_XIR_VALUE_LIMIT;
    debug->break_throw=enabled;return XR_XIR_VALUE_OK;
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
    debug->calls.disabled=true;
    clear_snapshot(debug);
}
XR_FUNC XrXirValueStatus xr_xir_vm_debug_observation_status(XrXirVmDebug *debug,bool terminal) {
    if (!debug_valid(debug)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (debug->disabled) return XR_XIR_VALUE_OK;
    XrXirValueStatus status=xir_debug_calls_pay(&debug->calls);
    return debug->faulted || !terminal ? XR_XIR_VALUE_OK : status;
}
XR_FUNC void xir_vm_debug_faulted(XrXirVmDebug *debug,const XrXirCallView *view) {
    if (!view || view->instance!=debug->target || !xr_xir_call_admission(view)) return;
    debug->faulted=true;debug->calls.disabled=true;clear_snapshot(debug);
}
XR_FUNC XrXirValueStatus xr_xir_vm_debug_continue(XrXirVmDebug *debug,
    uint64_t epoch, XrXirVmDebugMode mode) {
    if (!debug_valid(debug) || !xr_xir_vm_debug_paused(debug) || !debug->snapshot ||
        (unsigned)mode > XR_XIR_VM_DEBUG_LINE_OUT || mode == XR_XIR_VM_DEBUG_PAUSE ||
        !epoch || epoch != debug->epoch || debug->write.phase) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (mode>=XR_XIR_VM_DEBUG_STATEMENT_IN && !debug->source_memory) return XR_XIR_VALUE_UNSUPPORTED;
    if (!xr_xir_domain_work(debug->domain, 1)) return XR_XIR_VALUE_LIMIT;
    const XrXirVmDebugFrameInfo *current=&debug->snapshot->frames[0].info;
    const XirDebugCallFrame *call=xir_debug_calls_id(&debug->calls,current->id);
    if (!call) return XR_XIR_VALUE_BAD_ARGUMENT;
    debug->skip_id=current->id;debug->skip_instruction=current->instruction;
    debug->resume_id=current->id;debug->resume_instruction=current->instruction;
    debug->skip_once = true;
    debug->step_depth=call->event.depth;debug->step_thread=call->thread;
    debug->step_source=debug_source_point(debug,current->function,current->instruction);
    atomic_store_explicit(&debug->mode, (unsigned)mode, memory_order_release);
    clear_snapshot(debug);
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xir_vm_debug_snapshot_cause(const XrXirVmDebugSnapshot *snapshot,XirVmDebugStopCause *output) {
    if (!snapshot_valid(snapshot) || !output) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(snapshot->domain,sizeof(*output))) return XR_XIR_VALUE_LIMIT;
    *output=snapshot->cause;return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xir_vm_debug_continue_filtered(XrXirVmDebug *debug,uint64_t epoch) {
    if (!debug_valid(debug) || !xr_xir_vm_debug_paused(debug) || !debug->snapshot || !epoch || epoch!=debug->epoch ||
        !debug->snapshot->cause.groups || debug->snapshot->cause.unfiltered || debug->write.phase) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(debug->domain,1)) return XR_XIR_VALUE_LIMIT;
    const XrXirVmDebugFrameInfo *frame=&debug->snapshot->frames[0].info;
    debug->resume_id=frame->id;debug->resume_instruction=frame->instruction;debug->skip_once=true;
    clear_snapshot(debug);return XR_XIR_VALUE_OK;
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
    const XirDebugCallFrame *call=xir_debug_calls_current(&debug->calls);
    if (!call || call->event.code!=XR_XIR_CALL_CODE_VM || call->event.entry!=point->binding->function)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    DebugFrame *frame = find_frame(debug, point->state);
    if (frame) {
        if (frame->id!=call->id) return XR_XIR_VALUE_BAD_ARGUMENT;
        frame->point = stored; *output = frame; return XR_XIR_VALUE_OK;
    }
    if (debug->count >= debug->capacity) return XR_XIR_VALUE_LIMIT;
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
    *frame = (DebugFrame){stored, call->id, known};
    *output = frame;
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus copy_frame_values(XrXirVmDebug *debug, const DebugFrame *live,
    XrXirCallView *view, SnapshotFrame *frame) {
    const XrXirModule *module = xr_xir_compile_artifact_module(live->point.binding->artifact);
    const XrXirFunction *function = &module->functions[live->point.binding->function];
    XrXirValueStatus status;
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
        /* Verified layouts assign no value storage to root/place operands. */
        if (live->point.layout->offsets[i] == UINT32_MAX) continue;
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
            /* A bounds result has already passed the genuine loan check. */
            if (read == XR_XIR_CALL_BOUNDS) { frame->known[i]|=4;continue; }
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
static const DebugFrame *find_id(const XrXirVmDebug *debug,uint64_t id) {
    for (uint32_t i=0;i<debug->count;++i) if (debug->frames[i].id==id) return &debug->frames[i];
    return NULL;
}
static bool snapshot_contains(const XrXirVmDebugSnapshot *snapshot,uint32_t count,uint64_t id) {
    for (uint32_t i=0;i<count;++i) if (snapshot->frames[i].info.id==id) return true;
    return false;
}
static XrXirValueStatus capture_read_scope(XrXirVmDebug *debug,uint32_t function,XrXirCallView *view,SnapshotFrame *frame) {
    const XrXirModule *module=xr_xir_compile_artifact_module(debug->artifact);
    uint32_t count=module->types && module->types->nominals ? module->types->nominals->count : 0;
    if (!count) return XR_XIR_VALUE_OK;
    if (!module->declarations || !module->declarations->functions || function>=module->function_count || !view->arena)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(debug->domain,count)) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status;frame->nominal_visible=debug_allocate(debug->domain,count,&status);
    if (!frame->nominal_visible) return status;
    frame->nominal_count=count;frame->nominal_owner=module->declarations->functions[function].nominal_owner;
    if (!xr_xir_compile_type_arena_retain((XrXirTypeArena *)view->arena)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    frame->read_arena=(XrXirTypeArena *)view->arena;
    const XrXirTypes *types=xr_xir_compile_type_arena_types(view->arena);
    if (!types || !types->nominals || types->nominals->count!=count) return XR_XIR_VALUE_BAD_ARGUMENT;
    for (uint32_t declaration=0;declaration<count;++declaration) {
        XrXirStatus checked=xr_xir_compile_nominal_access(&debug->artifact->context,module,function,declaration,0,XR_XIR_NOMINAL_TYPE);
        if (checked==XR_XIR_OK) frame->nominal_visible[declaration]=1;
        else if (checked!=XR_XIR_BAD_TYPE) return checked==XR_XIR_BUDGET ? XR_XIR_VALUE_LIMIT : XR_XIR_VALUE_BAD_ARGUMENT;
    }
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus capture_frame(XrXirVmDebug *debug,const XirDebugCallFrame *call,
    XrXirCallView *view,XrXirVmDebugSnapshot *snapshot,uint32_t index) {
    SnapshotFrame *frame=&snapshot->frames[index];
    const XrXirModule *module=xr_xir_compile_artifact_module(debug->artifact);
    const XrXirFunction *function=&module->functions[call->event.entry];
    const DebugFrame *live=call->event.code==XR_XIR_CALL_CODE_VM ? find_id(debug,call->id) : NULL;
    bool values=live && call->thread==snapshot->stopped_thread;
    frame->info=(XrXirVmDebugFrameInfo){.id=call->id,.epoch=debug->epoch+1,.function=call->event.entry,
        .instruction=live ? live->point.instruction : UINT32_MAX,
        .value_count=values ? live->point.layout->slot_count : 0,.thread=call->thread,
        .code=call->event.code==XR_XIR_CALL_CODE_VM ? XR_XIR_VM_DEBUG_VM :
            call->event.code==XR_XIR_CALL_CODE_NATIVE ? XR_XIR_VM_DEBUG_NATIVE : XR_XIR_VM_DEBUG_PENDING};
    XrXirValueStatus status=xr_xir_string_new(debug->domain,function->name ? function->name : "",function->name_length,&frame->name);
    if (status!=XR_XIR_VALUE_OK || !values) return status;
    status=capture_read_scope(debug,call->event.entry,view,frame);if (status!=XR_XIR_VALUE_OK) return status;
    if (!xr_xir_domain_work(debug->domain,frame->info.value_count)) return XR_XIR_VALUE_LIMIT;
    return copy_frame_values(debug,live,view,frame);
}
static XrXirValueStatus capture_prepare(XrXirVmDebug *debug,XrXirCallView *view,const XrXirInstruction *throw_op,
    XirVmDebugStopCause cause,XrXirVmDebugSnapshot **output) {
    if (debug->epoch == UINT64_MAX) return XR_XIR_VALUE_LIMIT;
    const XirDebugCallFrame *current=xir_debug_calls_current(&debug->calls);
    if (!current) return XR_XIR_VALUE_BAD_ARGUMENT;
    uint32_t count=0;
    for (uint32_t i=0;i<debug->calls.capacity;++i)
        if (debug->calls.frames[i].active && debug->calls.frames[i].event.entry<debug->calls.functions) ++count;
    if (!count || count>debug->capacity) return XR_XIR_VALUE_LIMIT;
    if (!xr_xir_domain_work(debug->domain,(uint64_t)count*debug->calls.capacity*4+debug->calls.capacity+1)) return XR_XIR_VALUE_LIMIT;
    if (!xr_xir_domain_retain(debug->domain)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    XrXirValueStatus status;
    XrXirVmDebugSnapshot *snapshot = debug_allocate(debug->domain, sizeof(*snapshot), &status);
    if (!snapshot) { xr_xir_domain_drop(debug->domain); return status; }
    snapshot->identity = snapshot_identity;
    atomic_init(&snapshot->references, 1);
    snapshot->domain = debug->domain;
    snapshot->count=count;snapshot->stopped_thread=current->thread;
    snapshot->cause=cause;
    snapshot->frames = debug_allocate(debug->domain,
        (uint64_t)snapshot->count * sizeof(*snapshot->frames), &status);
    if (!snapshot->frames) goto fail;
    uint32_t at=0;
    for (const XirDebugCallFrame *call=current;call;call=xir_debug_calls_parent(&debug->calls,call)) {
        if (call->event.entry==debug->calls.functions) continue;
        if (at==snapshot->count || snapshot_contains(snapshot,at,call->id)) { status=XR_XIR_VALUE_BAD_ARGUMENT;goto fail; }
        status=capture_frame(debug,call,view,snapshot,at++);if (status!=XR_XIR_VALUE_OK) goto fail;
    }
    while (at<snapshot->count) {
        const XirDebugCallFrame *selected=NULL;
        for (uint32_t i=0;i<debug->calls.capacity;++i) {
            const XirDebugCallFrame *call=&debug->calls.frames[i];
            if (!call->active || call->event.entry==debug->calls.functions || snapshot_contains(snapshot,at,call->id)) continue;
            if (!selected || call->thread<selected->thread ||
                (call->thread==selected->thread && call->event.depth>selected->event.depth)) selected=call;
        }
        if (!selected) { status=XR_XIR_VALUE_BAD_ARGUMENT;goto fail; }
        status=capture_frame(debug,selected,view,snapshot,at++);if (status!=XR_XIR_VALUE_OK) goto fail;
    }
    if (throw_op) {
        uint32_t operand=throw_op->args[0];SnapshotFrame *frame=&snapshot->frames[0];
        if (operand>=frame->info.value_count || !frame->known[operand]) { status=XR_XIR_VALUE_BAD_ARGUMENT;goto fail; }
        XrXirValue concrete={0};
        if (frame->values[operand].type==XR_XIR_ERROR && !xr_xir_error_borrow(&frame->values[operand],&concrete)) {
            status=XR_XIR_VALUE_BAD_ARGUMENT;goto fail;
        }
        if (frame->values[operand].type!=XR_XIR_ERROR) concrete=frame->values[operand];
        if (!xr_xir_domain_work(debug->domain,1)) { status=XR_XIR_VALUE_LIMIT;goto fail; }
        status=xr_xir_value_copy(&concrete,&snapshot->exception);if (status!=XR_XIR_VALUE_OK) goto fail;
    }
    *output=snapshot;
    return XR_XIR_VALUE_OK;
fail:
    if (!snapshot->frames) snapshot->count = 0;
    xr_xir_vm_debug_snapshot_drop(snapshot);
    return status;
}
static void capture_publish(XrXirVmDebug *debug,XrXirVmDebugSnapshot *snapshot) {
    XrXirVmDebugSnapshot *previous=debug->snapshot;debug->snapshot=snapshot;++debug->epoch;
    atomic_store_explicit(&debug->paused,true,memory_order_release);xr_xir_vm_debug_snapshot_drop(previous);
}
static XrXirValueStatus capture(XrXirVmDebug *debug,XrXirCallView *view,const XrXirInstruction *throw_op,XirVmDebugStopCause cause) {
    XrXirVmDebugSnapshot *snapshot=NULL;XrXirValueStatus status=capture_prepare(debug,view,throw_op,cause,&snapshot);
    if (status==XR_XIR_VALUE_OK) capture_publish(debug,snapshot);return status;
}
#include "xxir_vm_debug_write.inc.c"
XR_FUNC XrXirValueStatus xir_vm_debug_before(XrXirVmDebug *debug,
    const XirVmDebugPoint *point, bool *stopped) {
    *stopped = false;
    if (!debug_valid(debug) || debug->artifact != point->binding->artifact)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (debug->disabled || debug->faulted) return XR_XIR_VALUE_OK;
    if (point->view->instance != debug->target) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (xr_xir_call_execution_status(point->view) != XR_XIR_CALL_READY ||
        xr_xir_call_cleanup_active(point->view->activation)) return XR_XIR_VALUE_OK;
    XrXirValueStatus bound=xir_debug_calls_bind(&debug->calls,point->view);
    if (bound!=XR_XIR_VALUE_OK) return bound;
    if (xr_xir_vm_debug_paused(debug)) {
        if (debug->write.phase==1) { debug->write.result=debug_write_apply(debug,point);debug->write.phase=2; }
        *stopped=true;return XR_XIR_VALUE_OK;
    }
    if (!xr_xir_domain_work(debug->domain, (uint64_t)debug->count * 2 + 33))
        return XR_XIR_VALUE_LIMIT;
    if (!xr_xir_domain_work(debug->domain,32)) return XR_XIR_VALUE_LIMIT;
    DebugFrame *frame;
    XrXirValueStatus status = register_frame(debug, point, &frame);
    if (status != XR_XIR_VALUE_OK) return status;
    if (debug->skip_once && frame->id == debug->resume_id &&
        point->instruction == debug->resume_instruction) {
        debug->skip_once = false;
        return XR_XIR_VALUE_OK;
    }
    unsigned mode = atomic_load_explicit(&debug->mode, memory_order_acquire);
    const XrXirModule *module=xr_xir_compile_artifact_module(debug->artifact);
    const XrXirInstruction *op=&module->functions[point->binding->function].instructions[point->instruction];
    bool throwing=debug->break_throw && op->op==XR_XIR_THROW;
    const XirDebugCallFrame *call=xir_debug_calls_current(&debug->calls);
    bool same=call->thread==debug->step_thread;
    bool stop = throwing || mode == XR_XIR_VM_DEBUG_PAUSE || (same && (mode == XR_XIR_VM_DEBUG_STEP_IN ||
        (mode == XR_XIR_VM_DEBUG_STEP_OVER && call->event.depth <= debug->step_depth) ||
        (mode == XR_XIR_VM_DEBUG_STEP_OUT && call->event.depth < debug->step_depth)));
    if (same && mode>=XR_XIR_VM_DEBUG_STATEMENT_IN) {
        if (!xr_xir_domain_work(debug->domain,sizeof(DebugSourcePoint)+1)) return XR_XIR_VALUE_LIMIT;
        if (debug_source_boundary(debug,frame,call,mode)) stop=true;
    }
    XirVmDebugStopCause cause={.unfiltered=stop};bool advanced[32]={0};
    for (unsigned i = 0; i < 32; ++i) {
        DebugBreakpoint *rule=&debug->breakpoints[i];
        if (!rule->enabled || rule->function!=point->binding->function || rule->instruction!=point->instruction) continue;
        uint64_t *hits=&debug->hits[rule->counter];
        if (!advanced[rule->counter]) {
            if (*hits==UINT64_MAX) return XR_XIR_VALUE_LIMIT;
            ++*hits;advanced[rule->counter]=true;
        }
        if (hit_matches(rule,*hits)) {
            stop=true;
            if (rule->group) cause.groups|=UINT32_C(1)<<(rule->group-1);else cause.unfiltered=true;
        }
    }
    if (!stop) return XR_XIR_VALUE_OK;
    status = capture(debug,point->view,throwing ? op : NULL,cause);
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
    if (point->layout->offsets[id] == UINT32_MAX || op->op == XR_XIR_LOCAL_UNINIT) frame->known[id] = 0;
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
    debug_deallocate(debug->domain, frame->known, frame->point.layout->slot_count);
    memmove(frame,frame+1,(size_t)(debug->count-at-1)*sizeof(*frame));
    --debug->count;
    memset(&debug->frames[debug->count],0,sizeof(*frame));
    if (!debug->count) clear_snapshot(debug);
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
XR_FUNC XrXirValueStatus xir_vm_debug_threads(XrXirVmDebug *debug,uint64_t *output,uint32_t capacity,uint32_t *count) {
    if (!debug_valid(debug) || !count || (capacity && !output) || capacity>1025 ||
        debug->calls.current!=UINT32_MAX) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (debug->disabled || !debug->calls.installed) return XR_XIR_VALUE_UNSUPPORTED;
    XrXirValueStatus status=xir_debug_calls_pay(&debug->calls);if (status!=XR_XIR_VALUE_OK) return status;
    uint64_t work=sizeof(uint64_t)*1025+(uint64_t)debug->calls.capacity*(debug->calls.capacity+1)*(sizeof(uint64_t)+1)/2+
        (uint64_t)capacity*sizeof(*output)+sizeof(*count);
    if (!xr_xir_domain_work(debug->domain,work)) return XR_XIR_VALUE_LIMIT;
    uint64_t fresh[1025]={0};uint32_t size=0;
    for (uint32_t i=0;i<debug->calls.capacity;++i) {
        const XirDebugCallFrame *frame=&debug->calls.frames[i];if (!frame->active) continue;
        uint32_t at=0;while (at<size && fresh[at]<frame->thread) ++at;
        if (at<size && fresh[at]==frame->thread) continue;
        if (size==capacity) return XR_XIR_VALUE_LIMIT;
        memmove(fresh+at+1,fresh+at,(size_t)(size-at)*sizeof(*fresh));fresh[at]=frame->thread;++size;
    }
    if (size) memcpy(output,fresh,(size_t)size*sizeof(*output));*count=size;return XR_XIR_VALUE_OK;
}
XR_FUNC uint64_t xr_xir_vm_debug_snapshot_stopped_thread(const XrXirVmDebugSnapshot *snapshot) {
    return snapshot_valid(snapshot) ? snapshot->stopped_thread : 0;
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
XR_FUNC XrXirValueStatus xr_xir_vm_debug_snapshot_exception(const XrXirVmDebugSnapshot *snapshot,XrXirValue *output) {
    if (!snapshot_valid(snapshot) || !output || output->type || output->reserved || output->payload)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!snapshot->exception.type) return XR_XIR_VALUE_UNSUPPORTED;
    if (!xr_xir_domain_work(snapshot->domain,1)) return XR_XIR_VALUE_LIMIT;
    return xr_xir_value_copy(&snapshot->exception,output);
}
XR_FUNC XrXirValueStatus xr_xir_vm_debug_snapshot_member(const XrXirVmDebugSnapshot *snapshot,
    uint32_t frame,const XrXirValue *value,uint32_t field,bool *allowed) {
    if (!snapshot_valid(snapshot) || frame>=snapshot->count || !allowed || !xr_xir_value_valid(value)) return XR_XIR_VALUE_BAD_ARGUMENT;
    const SnapshotFrame *scope=&snapshot->frames[frame];
    if (!scope->read_arena || xr_xir_value_arena(value)!=scope->read_arena) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(snapshot->domain,1)) return XR_XIR_VALUE_LIMIT;
    const XrXirTypes *types=xr_xir_compile_type_arena_types(scope->read_arena);
    const XrXirTypeNode *node=xr_xir_type_node(types,(XrXirType)value->type);
    if (!node || node->kind!=XR_XIR_TYPE_NOMINAL || node->nominal.declaration>=scope->nominal_count) return XR_XIR_VALUE_UNSUPPORTED;
    uint32_t declaration=node->nominal.declaration;const XrXirNominalIdentity *identity=&types->nominals->identities[declaration];
    if (field>=identity->field_count) return XR_XIR_VALUE_BOUNDS;
    *allowed=scope->nominal_visible[declaration] &&
        (!(identity->fields[field].flags & (XR_XIR_FIELD_PRIVATE|XR_XIR_FIELD_PROTECTED)) || scope->nominal_owner==declaration+1);
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_vm_debug_snapshot_type_visible(const XrXirVmDebugSnapshot *snapshot,
    uint32_t frame,const XrXirValue *value,XrXirType type,bool *allowed) {
    if (!snapshot_valid(snapshot) || frame>=snapshot->count || !allowed || !xr_xir_value_valid(value)) return XR_XIR_VALUE_BAD_ARGUMENT;
    const SnapshotFrame *scope=&snapshot->frames[frame];
    if (!scope->read_arena || xr_xir_value_arena(value)!=scope->read_arena) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(snapshot->domain,1)) return XR_XIR_VALUE_LIMIT;
    const XrXirTypeNode *node=xr_xir_type_node(xr_xir_compile_type_arena_types(scope->read_arena),type);
    if (!node || node->kind!=XR_XIR_TYPE_NOMINAL || node->nominal.declaration>=scope->nominal_count) return XR_XIR_VALUE_UNSUPPORTED;
    *allowed=scope->nominal_visible[node->nominal.declaration]!=0;return XR_XIR_VALUE_OK;
}
