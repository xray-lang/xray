/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call.c - Nonrecursive call driving and exactly-once frame cleanup
 *
 * KEY CONCEPT:
 *   Only the driver owns frame transitions; callbacks publish actions and
 *   cancellation cannot reclaim a frame while its callback is running.
 *   A panic unwinds children first until the nearest frame whose pending
 *   call is protected; every other failure unwinds the whole activation.
 */
#include "xxir_call.h"
#include "xxir_call_internal.h"
#include "xxir_call_observer_internal.h"
#include "xxir_call_native_observer_internal.h"
#include "xxir_call_native_source_internal.h"
#include "xxir_value_internal.h"
#include "xxir_task_internal.h"
#include "xxir_type_arena.h"
#include "xxir_type_arena_internal.h"
#include "xxir_types.h"
#include "xxir_panic.h"
#include "../shared/xr_error_core.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define XR_XIR_FRAME_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(XR_XIR_FRAME_ASAN)
#define XR_XIR_FRAME_ASAN 1
#endif
#if defined(XR_XIR_FRAME_ASAN)
#include <sanitizer/asan_interface.h>
#endif

typedef struct CallSegment {
    struct CallSegment *parent;
    uint64_t allocation_bytes, used;
} CallSegment;

typedef struct CallCellLoan {
    XrXirCellLoan loan;
    uint32_t parameter;
} CallCellLoan;
typedef struct CallFrame {
    struct CallFrame *parent;
    const XrXirCallEntry *entry;
    const XrXirCallMetadata *metadata;
    XrXirCallResult inbox, pending;
    uint64_t allocation_bytes;
    void *state;
    XrXirValue *arguments;
    CallCellLoan *loans;
    uint64_t epoch;
    XrXirCallCodeKind code_kind;
    uint32_t depth, loan_count, cell_capacity;
    XirEffectInvocationSelection invocation,pending_invocation;
    uint32_t effect_instruction;
    bool effect_pending;
    uint32_t cleanup_frontier;
    bool protected_call, entered, exiting, scope_exit, cleanup_call, in_cleanup, exit_done;
    bool observer_announced;
    bool native_pending;
} CallFrame;

struct XrXirCall {
    XrXirCallConfig config;
    CallFrame *top;
    CallSegment *segment;
    XrXirCallView *active_view;
    XrXirCallResult result;
    uint64_t allocation_bytes, polls_left, next_wake;
    uint64_t next_epoch;
    XrXirCallMetadata metadata;
    XrXirCallBudget *budget;
    XrXirWaitRequest wait;
    XrXirValue waiting_task;
    const void *executor_owner;
    void *executor_activation;
    uint64_t executor_generation, executor_ticket;
    struct XrXirCall *wait_previous, *wait_next;
    struct XrXirCall *observed_previous, *observed_next;
    bool wait_linked;
    bool driving, cleaning, admitting, cancel_requested;
    bool aborting, cleanup_incomplete;
    /* The first accepted execution failure also becomes the physical-abort reason. */
    XrXirCallStatus abort_reason;
};

/* The public config ABI is unchanged; frame metadata remains private.
 * A NULL admission domain remains legal for unbudgeted scalar activations. */
static bool call_observing(const XrXirCall *call) {
    return call->budget && call->budget->notifying;
}
static bool call_owner_current(const XrXirCall *call) {
    if (!call) return true;
    if (call->config.abi_version != XR_XIR_CALL_ABI_VERSION ||
        call->config.struct_size != sizeof(call->config)) return false;
    if (call->config.admission.domain && !xr_xir_domain_current(call->config.admission.domain)) return false;
    return !call->budget || xr_xir_domain_current(call->budget->work_domain);
}

XrXirAction xr_xir_call_fault(XrXirRunStatus status) {
    XrXirCallStatus reason;
    switch (status) {
        case XR_XIR_RUN_DIVIDE_BY_ZERO: reason = XR_XIR_CALL_DIVIDE_BY_ZERO; break;
        case XR_XIR_RUN_NUMERIC_RANGE: reason = XR_XIR_CALL_NUMERIC_RANGE; break;
        case XR_XIR_RUN_OUT_OF_MEMORY: reason = XR_XIR_CALL_OOM; break;
        case XR_XIR_RUN_HOST_ERROR: reason = XR_XIR_CALL_HOST_ERROR; break;
        case XR_XIR_RUN_NULL_UNWRAP: reason = XR_XIR_CALL_RUNTIME_PANIC; break;
        case XR_XIR_RUN_ATOMIC_ARGUMENT: reason = XR_XIR_CALL_BAD_ARGUMENT; break;
        case XR_XIR_RUN_UNSUPPORTED: reason = XR_XIR_CALL_UNSUPPORTED; break;
        case XR_XIR_RUN_STEP_LIMIT:
        case XR_XIR_RUN_FRAME_LIMIT: reason = XR_XIR_CALL_LIMIT; break;
        default: reason = XR_XIR_CALL_BAD_STATE; break;
    }
    XrXirAction action = {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, reason}, {0}, 0};
    if (reason == XR_XIR_CALL_DIVIDE_BY_ZERO) action.panic.detail.code = XR_XIR_PANIC_DIVIDE;
    else if (reason == XR_XIR_CALL_NUMERIC_RANGE) action.panic.detail.code = XR_XIR_PANIC_RANGE;
    else if (reason == XR_XIR_CALL_RUNTIME_PANIC) action.panic.detail.code = XR_XIR_PANIC_NULL_UNWRAP;
    return action;
}
XrXirAction xr_xir_call_numeric_fault(XrXirRunStatus status, bool remainder) {
    XrXirAction action = xr_xir_call_fault(status);
    if (remainder && status == XR_XIR_RUN_DIVIDE_BY_ZERO) action.panic.detail.code = XR_XIR_PANIC_REMAINDER;
    return action;
}
bool xr_xir_call_panic_status(XrXirCallStatus status) {
    return status == XR_XIR_CALL_DIVIDE_BY_ZERO || status == XR_XIR_CALL_NUMERIC_RANGE ||
        status == XR_XIR_CALL_BOUNDS || status == XR_XIR_CALL_MATCH_FAILURE || status == XR_XIR_CALL_DEFER_ASYNC || status == XR_XIR_CALL_ASSERTION ||
        status == XR_XIR_CALL_RUNTIME_PANIC;
}
XR_FUNCDEF bool xr_xir_call_panic_payload(XrXirCallStatus status, const XrXirPanicPayload *panic) {
    if (!xr_xir_panic_valid(panic) || xr_xir_panic_empty(panic)) return false;
    XrXirFaultDetail detail = panic->detail;
    switch (status) {
    case XR_XIR_CALL_DIVIDE_BY_ZERO: return xr_xir_fault_divide_valid(detail);
    case XR_XIR_CALL_NUMERIC_RANGE: return xr_xir_fault_range_valid(detail);
    case XR_XIR_CALL_BOUNDS: return xr_xir_fault_bounds_valid(detail);
    case XR_XIR_CALL_MATCH_FAILURE: return xr_xir_fault_match_valid(detail);
    case XR_XIR_CALL_DEFER_ASYNC: return xr_xir_fault_defer_async_valid(detail);
    case XR_XIR_CALL_ASSERTION: return detail.code == XR_XIR_PANIC_ASSERTION;
    case XR_XIR_CALL_RUNTIME_PANIC: return xr_xir_fault_runtime_valid(detail);
    default: return false;
    }
}

uint32_t xr_xir_call_current_entry(const XrXirCall *call) {
    if (!call_owner_current(call)) return UINT32_MAX;
    return call && call->driving && !call->cleaning && !call_observing(call) && call->top ?
        (uint32_t) (call->top->entry - call->top->metadata->entries) : UINT32_MAX;
}
XrXirCallStatus xr_xir_call_state(const XrXirCall *call) {
    if (!call_owner_current(call)) return XR_XIR_CALL_BAD_ARGUMENT;
    return !call ? XR_XIR_CALL_BAD_ARGUMENT : (call->driving || call_observing(call)) ? XR_XIR_CALL_BUSY : call->result.status;
}
static bool cleanup_active(const XrXirCall *call) {
    return call->top && (call->top->exiting || call->top->in_cleanup);
}
bool xr_xir_call_cleanup_active(const XrXirCall *call) {
    if (!call_owner_current(call)) return false;
    return call && call->driving && !call->cleaning && (call->active_view || call->admitting) && cleanup_active(call);
}
XrXirValueAdmission *xr_xir_call_admission(const XrXirCallView *view) {
    if (!(!view || call_owner_current(view->activation))) return NULL;
    if (!view || !view->activation) return NULL;
    XrXirCall *call = view->activation;
    if (!call->driving || call->cleaning || call_observing(call) || !call->top || call->active_view != view)
        return NULL;
    const CallFrame *frame = call->top;
    if (view->instance != call->config.instance || view->environment != frame->entry->environment ||
        view->state != frame->state || view->arguments != frame->arguments ||
        view->argument_count != frame->entry->parameter_count || view->arena != call->config.admission.arena ||
        view->phase != (frame->exiting ? XR_XIR_CALL_EXIT : XR_XIR_CALL_NORMAL) || view->scope_exit != frame->scope_exit)
        return NULL;
    return &call->config.admission;
}

#include "xxir_call_invocation.inc.c"

XR_FUNC XrXirCallStatus xr_xir_call_cleanup_frontier(XrXirCallView *view, uint32_t frontier) {
    if (!(!view || call_owner_current(view->activation))) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!xr_xir_call_admission(view) || !(view->activation->top->entry->flags & XR_XIR_ENTRY_EXIT))
        return XR_XIR_CALL_BAD_STATE;
    view->activation->top->cleanup_frontier = frontier;
    return XR_XIR_CALL_READY;
}
XR_FUNC bool xr_xir_call_cleanup_incomplete(const XrXirCall *call) {
    if (!call_owner_current(call)) return false;
    return call && call->cleanup_incomplete;
}

XR_FUNC XrXirCallStatus xr_xir_call_execution_status(const XrXirCallView *view) {
    if (!(!view || call_owner_current(view->activation))) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!xr_xir_call_admission(view)) return XR_XIR_CALL_BAD_STATE;
    XrXirCall *call = view->activation;
    return call->cancel_requested && !cleanup_active(call) && call->abort_reason == XR_XIR_CALL_READY ?
        XR_XIR_CALL_CANCELLED : XR_XIR_CALL_READY;
}

static XrXirCallResult call_result(XrXirCallStatus status) {
    return (XrXirCallResult) {status, {XR_XIR_UNIT, 0, 0}, 0, {0}};
}

static bool boundary_value(XrXirValue value, XrXirType type) {
    if (type == XR_XIR_UNIT)
        return value.type == XR_XIR_UNIT && !value.reserved && !value.payload;
    return xr_xir_value_argument(&value, NULL, type);
}
#include "xxir_call_result.inc.c"

static bool fault_panics(const XrXirAction *action) {
    return (action->kind == XR_XIR_ACTION_FAULT ||
        (action->kind == XR_XIR_ACTION_LEAVE && action->flags == XR_XIR_ACTION_LEAVE_PANIC)) && boundary_value(action->value, XR_XIR_I64) &&
        action->value.payload >= XR_XIR_CALL_READY && action->value.payload <= XR_XIR_CALL_RUNTIME_PANIC &&
        xr_xir_call_panic_status((XrXirCallStatus) action->value.payload);
}
bool xr_xir_call_panic_action(const XrXirAction *action) {
    return action && action->kind == XR_XIR_ACTION_FAULT && fault_panics(action) && !action->callee && !action->arguments &&
        !action->argument_count && !action->flags &&
        xr_xir_call_panic_payload((XrXirCallStatus) action->value.payload, &action->panic);
}

XrXirAction xr_xir_call_bounds(int64_t index, int64_t length) {
    return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0,
        {XR_XIR_I64, 0, XR_XIR_CALL_BOUNDS}, {{430, 0, index, length}, {0}}, 0};
}

static XrXirCallStatus admit_value(const XrXirValue *value, XrXirType type,
                                   XrXirValueAdmission *admission) {
    if (admission && admission->domain && !xr_xir_domain_current(admission->domain))
        return XR_XIR_CALL_BAD_ARGUMENT;
    if (type == XR_XIR_UNIT)
        return value && boundary_value(*value, type) ? XR_XIR_CALL_READY : XR_XIR_CALL_BAD_ARGUMENT;
    if (!xr_xir_value_argument(value, admission->arena, type)) return XR_XIR_CALL_BAD_ARGUMENT;
    if ((uint32_t) type < XR_XIR_CONSTRUCTED_TYPE_BASE) return XR_XIR_CALL_READY;
    XrXirValueStatus status = xr_xir_value_admit(value, type, admission);
    if (status == XR_XIR_VALUE_OK) return XR_XIR_CALL_READY;
    if (status == XR_XIR_VALUE_OOM) return XR_XIR_CALL_OOM;
    if (status == XR_XIR_VALUE_UNSUPPORTED) return XR_XIR_CALL_UNSUPPORTED;
    if (status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_REFCOUNT_LIMIT) return XR_XIR_CALL_LIMIT;
    return XR_XIR_CALL_BAD_ARGUMENT;
}


static void *call_allocate(const XrXirCallConfig *config, XrXirCallBudget *budget, uint64_t bytes,
                           XrXirCallStatus *status) {
    XrXirCallAccounting *accounting = config->accounting;
    if (bytes > SIZE_MAX || accounting->live_bytes > config->byte_limit ||
        bytes > config->byte_limit - accounting->live_bytes ||
        accounting->allocations == UINT64_MAX) {
        *status = XR_XIR_CALL_LIMIT;
        return NULL;
    }
    void *memory = budget ? xr_xir_call_budget_allocate(budget, bytes, status) : xr_calloc(1, (size_t) bytes);
    if (!memory) {
        if (!budget) *status = XR_XIR_CALL_OOM;
        return NULL;
    }
    accounting->live_bytes += bytes;
    if (accounting->live_bytes > accounting->peak_bytes)
        accounting->peak_bytes = accounting->live_bytes;
    ++accounting->allocations;
    return memory;
}

static void call_deallocate(XrXirCallAccounting *accounting, XrXirCallBudget *budget, void *memory, uint64_t bytes) {
    XR_CHECK(memory && accounting->live_bytes >= bytes &&
             accounting->allocations > accounting->frees, "invalid call storage release");
    accounting->live_bytes -= bytes;
    ++accounting->frees;
    if (budget) xr_xir_call_budget_deallocate(budget, memory, bytes); else xr_free(memory);
}

static uint64_t frame_align(uint64_t bytes) {
    return (bytes + XR_XIR_CALL_STATE_ALIGNMENT - 1) / XR_XIR_CALL_STATE_ALIGNMENT * XR_XIR_CALL_STATE_ALIGNMENT;
}
static void frame_poison(void *pointer, uint64_t bytes, bool poison) {
#if defined(XR_XIR_FRAME_ASAN)
    if (poison) __asan_poison_memory_region(pointer, (size_t) bytes);
    else __asan_unpoison_memory_region(pointer, (size_t) bytes);
#else
    (void) pointer; (void) bytes; (void) poison;
#endif
}
static CallFrame *frame_reserve(XrXirCall *call, uint64_t bytes, XrXirCallStatus *status) {
    uint64_t reserved = frame_align(bytes) + XR_XIR_CALL_STATE_ALIGNMENT;
    CallSegment *segment = call->segment;
    if (!segment || reserved > segment->allocation_bytes - segment->used) {
        uint64_t header = frame_align(sizeof(CallSegment)), required = header + reserved;
        if (call->config.accounting->live_bytes > call->config.byte_limit) {
            *status = XR_XIR_CALL_LIMIT; return NULL;
        }
        uint64_t available = call->config.byte_limit - call->config.accounting->live_bytes;
        uint64_t capacity = required < 4096 ? 4096 : required;
        if (capacity > available && required <= available)
            capacity = available / XR_XIR_CALL_STATE_ALIGNMENT * XR_XIR_CALL_STATE_ALIGNMENT;
        segment = call_allocate(&call->config, call->budget, capacity, status);
        if (!segment) return NULL;
        segment->parent = call->segment; segment->allocation_bytes = capacity; segment->used = header;
        call->segment = segment;
        frame_poison((unsigned char *) segment + (size_t) header, capacity - header, true);
    }
    CallFrame *frame = (CallFrame *) ((unsigned char *) segment + (size_t) segment->used);
    segment->used += reserved;
    frame_poison(frame, bytes, false);
    memset(frame, 0, (size_t) bytes);
    frame->allocation_bytes = reserved;
    return frame;
}
static void call_observer_event(XrXirCall *call, CallFrame *frame,
    XrXirCallEventKind kind, XrXirCallStatus status) {
    XrXirCallBudget *budget = call->budget;
    if (!budget || !budget->observer || !call->executor_owner || !call->executor_activation ||
        !call->executor_generation || !call->executor_ticket) return;
    XR_CHECK(!budget->notifying && budget->observer_instance == call->config.instance,
        "observation belongs to the exact executor instance and cannot reenter");
    XrXirCallEvent event = {{call->executor_owner, call->executor_activation,
        call->executor_generation, call->executor_ticket}, call->config.instance,
        frame ? frame->epoch : 0, frame && frame->parent ? frame->parent->epoch : 0,
        frame ? (uint32_t)(frame->entry - call->config.entries) : UINT32_MAX,
        frame ? frame->depth : 0, kind, frame ? frame->code_kind : XR_XIR_CALL_CODE_UNKNOWN,
        frame && frame->exiting ? XR_XIR_CALL_EXIT : XR_XIR_CALL_NORMAL, status};
    if (frame && (kind == XR_XIR_CALL_FRAME_ENTER || kind == XR_XIR_CALL_FRAME_PRESENT))
        frame->observer_announced = true;
    XrXirCallView *active = call->active_view;
    call->active_view = NULL; budget->notifying = true;
    budget->observer->event(budget->observer->owner, &event);
    budget->notifying = false; call->active_view = active;
}
XR_FUNC XrXirCallStatus xr_xir_call_observer_bind(const XrXirCallView *view,
    const XrXirCallObserver *observer) {
    if (!view || !observer || !observer->owner || !observer->event) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!xr_xir_call_admission(view)) return XR_XIR_CALL_BAD_STATE;
    XrXirCall *call = view->activation; XrXirCallBudget *budget = call->budget;
    if (!budget || !call->executor_owner || !call->executor_activation || !call->executor_generation ||
        !call->executor_ticket || !call->config.instance) return XR_XIR_CALL_BAD_STATE;
    bool child = false;
    if (!xr_xir_task_executor_view_member(call->executor_owner, view, &child)) return XR_XIR_CALL_BAD_STATE;
    if (budget->notifying) return XR_XIR_CALL_BUSY;
    if (budget->observer) return budget->observer == observer &&
        budget->observer_instance == call->config.instance ? XR_XIR_CALL_READY : XR_XIR_CALL_BAD_STATE;
    /* Prepay both validation and replay before publishing the observer. A
     * failed installation preserves the receiver and never refunds work. */
    for (XrXirCall *member = budget->observed_calls; member; member = member->observed_next) {
        if (!xr_xir_domain_work(budget->work_domain, 2)) return XR_XIR_CALL_LIMIT;
        if (member->config.instance != call->config.instance) return XR_XIR_CALL_BAD_STATE;
        for (CallFrame *frame = member->top; frame; frame = frame->parent)
            if (!xr_xir_domain_work(budget->work_domain, 2)) return XR_XIR_CALL_LIMIT;
    }
    budget->observer_instance = call->config.instance; budget->observer = observer;
    for (XrXirCall *member = budget->observed_calls; member; member = member->observed_next)
        for (CallFrame *frame = member->top; frame; frame = frame->parent)
            call_observer_event(member, frame, XR_XIR_CALL_FRAME_PRESENT, member->result.status);
    return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xr_xir_call_observer_vm(const XrXirCallView *view) {
    if (!xr_xir_call_admission(view)) return XR_XIR_CALL_BAD_STATE;
    XrXirCall *call = view->activation; CallFrame *frame = call->top;
    if (frame->code_kind == XR_XIR_CALL_CODE_NATIVE) return XR_XIR_CALL_BAD_STATE;
    if (frame->code_kind == XR_XIR_CALL_CODE_UNKNOWN) {
        frame->code_kind = XR_XIR_CALL_CODE_VM;
        call_observer_event(call, frame, XR_XIR_CALL_FRAME_CODE, XR_XIR_CALL_READY);
    }
    return XR_XIR_CALL_READY;
}
XR_FUNC bool xir_call_vm_view_current(const XrXirCallView *view) {
    return xr_xir_call_admission(view) && view->activation->top->code_kind==XR_XIR_CALL_CODE_VM;
}

static void frame_release(XrXirCall *call, CallFrame *frame) {
    CallSegment *segment = call->segment;
    uint64_t bytes = frame->allocation_bytes;
    XR_CHECK(segment && segment->used >= bytes &&
        (unsigned char *) segment + (size_t) (segment->used - bytes) == (unsigned char *) frame,
        "call frame release must rewind the current segment");
    segment->used -= bytes;
    frame_poison(frame, bytes, true);
    if (segment->used == frame_align(sizeof(CallSegment))) {
        call->segment = segment->parent;
        frame_poison(segment, segment->allocation_bytes, false);
        call_deallocate(call->config.accounting, call->budget, segment, segment->allocation_bytes);
    }
}

static XrXirCallStatus entry_arguments(const XrXirCallEntry *entry, const XrXirValue *arguments,
    uint32_t count, const XrXirFunctionBinding *binding, XrXirValueAdmission *admission) {
    uint32_t captures = binding ? binding->capture_count : 0;
    if (captures > entry->parameter_count || count != entry->parameter_count - captures || (count && !arguments))
        return XR_XIR_CALL_BAD_ARGUMENT;
    for (uint32_t i = 0; i < entry->parameter_count; ++i) {
        const XrXirValue *value = i < captures ? &binding->captures[i] : &arguments[i - captures];
        XrXirCallStatus status = admit_value(value, entry->parameters[i], admission);
        if (status != XR_XIR_CALL_READY) return status;
    }
    return XR_XIR_CALL_READY;
}

static uint64_t state_offset(void) {
    uint64_t alignment = XR_XIR_CALL_STATE_ALIGNMENT;
    return (sizeof(CallFrame) + alignment - 1) / alignment * alignment;
}
#include "xxir_call_cell_loans.inc.c"
XR_FUNC XrXirCallStatus xir_call_vm_frame_entry(const XrXirCallView *view,uint64_t identity,uint32_t *output) {
    if (!identity || !output || !xir_call_vm_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCall *call=view->activation;
    if (call->cleaning || call->cancel_requested || call->aborting) return XR_XIR_CALL_BAD_STATE;
    for (const CallFrame *frame=call->top;frame;frame=frame->parent) {
        if (!call_cell_work(call,1)) return XR_XIR_CALL_LIMIT;
        if (frame->epoch!=identity) continue;
        if (!frame->entered || frame->exiting || frame->in_cleanup || frame->code_kind!=XR_XIR_CALL_CODE_VM)
            return XR_XIR_CALL_BAD_STATE;
        *output=(uint32_t)(frame->entry-call->config.entries);return XR_XIR_CALL_READY;
    }
    return XR_XIR_CALL_BAD_STATE;
}
XR_FUNC XrXirCallStatus xir_call_debug_frame_entry(const XrXirCallView *view,uint64_t identity,
    uint32_t *output,XrXirCallCodeKind *code) {
    if (!identity || !output || !code || !xr_xir_call_admission(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCall *call=view->activation;
    if (view->phase!=XR_XIR_CALL_NORMAL || call->cleaning || call->cancel_requested || call->aborting)
        return XR_XIR_CALL_BAD_STATE;
    for (const CallFrame *frame=call->top;frame;frame=frame->parent) {
        if (!call_cell_work(call,1)) return XR_XIR_CALL_LIMIT;
        if (frame->epoch!=identity) continue;
        if (!frame->entered || frame->exiting || frame->in_cleanup ||
            (frame->code_kind!=XR_XIR_CALL_CODE_VM && frame->code_kind!=XR_XIR_CALL_CODE_NATIVE)) return XR_XIR_CALL_BAD_STATE;
        *output=(uint32_t)(frame->entry-call->config.entries);*code=frame->code_kind;return XR_XIR_CALL_READY;
    }
    return XR_XIR_CALL_BAD_STATE;
}
XR_FUNC XrXirCallStatus xir_call_vm_visit_ready(XrXirCall *call,const XrXirExecutorBinding *binding,uint64_t frame) {
    if (!call_owner_current(call) || !call || !frame || !xr_xir_call_executor_member(call,binding)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->driving || call_observing(call) || call->cleaning || call->admitting || call->active_view) return XR_XIR_CALL_BUSY;
    if (!call->top || call->top->epoch!=frame || call->top->code_kind!=XR_XIR_CALL_CODE_VM ||
        !call->top->entered || call->top->exiting || call->top->in_cleanup || call->cancel_requested || call->aborting ||
        call->result.status!=XR_XIR_CALL_READY) return XR_XIR_CALL_BAD_STATE;
    if (!call_cell_work(call,1)) return XR_XIR_CALL_LIMIT;
    return XR_XIR_CALL_READY;
}
#include "xxir_call_class_access.inc.c"

typedef struct CallFramePreparation {
    bool admitted;
    XrXirFaultDetail *fault;
    const XirEffectInvocationSelection *invocation;
    const XrXirValue *function;
    const XrXirCallMetadata *metadata;
} CallFramePreparation;
/* The real parent callback stays active until child preparation completes.
 * Class reservations, not the graph operation lease, exclude publication. */
static XrXirCallStatus frame_class_recheck(XrXirCall *call, const CallFrame *frame,
    const CallFramePreparation *preparation) {
    if (!frame->parent || !frame->loan_count) return XR_XIR_CALL_READY;
    bool identity = false;
    for (uint32_t i = 0; i < frame->loan_count; ++i) {
        if (!call_cell_work(call, 1)) return XR_XIR_CALL_LIMIT;
        uint32_t parameter = frame->loans[i].parameter;
        if (parameter >= frame->entry->parameter_count) return XR_XIR_CALL_BAD_STATE;
        const XrXirValue *root = xr_xir_cell_canonical_root(&frame->arguments[parameter]);
        if (!root) return XR_XIR_CALL_BAD_STATE;
        identity |= xr_xir_type_is_class(xr_xir_compile_type_arena_types(xr_xir_value_arena(root)),
            (XrXirType)root->type);
    }
    if (!identity) return XR_XIR_CALL_READY;
    if (!call->active_view || !preparation->function || call->top != frame->parent)
        return XR_XIR_CALL_BAD_STATE;
    const XrXirFunctionBinding *binding = xr_xir_function_binding(preparation->function);
    uint32_t captures = binding ? binding->capture_count : 0;
    if (captures > frame->entry->parameter_count) return XR_XIR_CALL_BAD_STATE;
    return xr_xir_instance_call_entry_recheck(call->active_view,
        (uint32_t)(frame->entry - frame->metadata->entries), preparation->function,
        frame->arguments + captures, frame->entry->parameter_count - captures, &frame->invocation);
}
/* Equal coordinates borrow the immutable activation or ancestor storage.
 * Only an actual generation transition needs a private copy in its frame;
 * nested frames retire before the ancestor that owns their coordinates. */
static const XrXirCallMetadata *frame_metadata_borrow(const XrXirCall *call,
    const XrXirCallMetadata *metadata) {
    const XrXirCallMetadata *current=call->top?call->top->metadata:&call->metadata;
    return current->entries==metadata->entries && current->entry_count==metadata->entry_count &&
        current->arena==metadata->arena && current->admission_context==metadata->admission_context &&
        current->cell_role==metadata->cell_role && current->cell_context==metadata->cell_context?
        current:NULL;
}
static XrXirCallStatus push_frame_owned(XrXirCall *call, uint32_t id,
    const XrXirValue *arguments, uint32_t count, const XrXirFunctionBinding *binding,
    const CallFramePreparation *preparation) {
    XR_CHECK(preparation && preparation->fault, "frame preparation preserves its exact path fault detail");
    XrXirFaultDetail *fault = preparation->fault;
    bool admitted = preparation->admitted;
    *fault = (XrXirFaultDetail){0};
    const XrXirCallMetadata *metadata=preparation->metadata;
    if (!metadata || id >= metadata->entry_count) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirCallEntry *entry = &metadata->entries[id];
    if (!admitted) {
        XrXirCallStatus status = entry_arguments(entry, arguments, count, binding, &call->config.admission);
        if (status != XR_XIR_CALL_READY) return status;
    }
    XrXirCallAccounting *accounting = call->config.accounting;
    if (accounting->depth >= call->config.depth_limit)
        return XR_XIR_CALL_LIMIT;
    if (call->budget && call->budget->release_tickets == UINT64_MAX) return XR_XIR_CALL_LIMIT;
    if (call->next_epoch == UINT64_MAX) return XR_XIR_CALL_LIMIT;
    uint64_t epoch = ++call->next_epoch;
    uint64_t arguments_offset = state_offset() + ((uint64_t) entry->state_bytes + 7) / 8 * 8;
    uint64_t loans_offset = arguments_offset + (uint64_t)entry->parameter_count * sizeof(XrXirValue);
    uint32_t cells = frame_cell_count(call, entry);
    uint64_t bytes = loans_offset + (uint64_t)cells * sizeof(CallCellLoan);
    const XrXirCallMetadata *borrowed_metadata=frame_metadata_borrow(call,metadata);
    uint64_t metadata_offset=bytes;
    if (!borrowed_metadata) bytes+=sizeof(*metadata);
    if (cells && !call_cell_work(call, (uint64_t)entry->parameter_count + (uint64_t)cells *
            (sizeof(CallCellLoan) + 16))) return XR_XIR_CALL_LIMIT;
    XrXirCallStatus status = XR_XIR_CALL_READY;
    CallFrame *frame = frame_reserve(call, bytes, &status);
    if (!frame)
        return status;
    frame->in_cleanup = cleanup_active(call);
    frame->parent = call->top;
    frame->entry = entry;
    frame->metadata = borrowed_metadata;
    if (!borrowed_metadata) {
        XrXirCallMetadata *owned=(XrXirCallMetadata *)((unsigned char *)frame+(size_t)metadata_offset);
        *owned=*metadata;frame->metadata=owned;
    }
    frame->epoch = epoch;
    frame->depth = frame->parent ? frame->parent->depth + 1 : 1;
    frame->cell_capacity = cells;
    frame->inbox = call_result(XR_XIR_CALL_READY);
    frame->state = (unsigned char *) frame + (size_t) state_offset();
    frame->arguments = (XrXirValue *) ((unsigned char *) frame + (size_t) arguments_offset);
    frame->loans = (CallCellLoan *)((unsigned char *)frame + (size_t)loans_offset);
    uint32_t captures = binding ? binding->capture_count : 0;
    for (uint32_t i = 0; i < entry->parameter_count; ++i) {
        const XrXirValue *value = i < captures ? &binding->captures[i] : &arguments[i - captures];
        if (xr_xir_value_copy(value, &frame->arguments[i]) != XR_XIR_VALUE_OK) {
            for (uint32_t p = 0; p < i; ++p) xr_xir_value_drop(&frame->arguments[p]);
            frame_release(call, frame);
            return XR_XIR_CALL_LIMIT;
        }
    }
    if (preparation->invocation) {
        if (!call_cell_work(call,sizeof(frame->invocation)) ||
            preparation->invocation->target!=id || !preparation->invocation->owner) {
            for (uint32_t p=entry->parameter_count;p;--p) xr_xir_value_drop(&frame->arguments[p-1]);
            frame_release(call,frame);
            return preparation->invocation->target!=id || !preparation->invocation->owner?
                XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_LIMIT;
        }
        frame->invocation=*preparation->invocation;
    }
    status = frame_cell_prepare(call, frame, captures, fault);
    /* The authentic parent view stays current during its final actual-loan check. */
    call->config.admission.arena=frame->parent?frame->parent->metadata->arena:call->metadata.arena;
    call->config.admission.context=frame->parent?frame->parent->metadata->admission_context:call->metadata.admission_context;
    if (status == XR_XIR_CALL_READY) status = frame_class_recheck(call, frame, preparation);
    if (status != XR_XIR_CALL_READY) {
        frame_cell_abort(frame);
        for (uint32_t p = entry->parameter_count; p; --p) xr_xir_value_drop(&frame->arguments[p - 1]);
        frame_release(call, frame);
        return status;
    }
    frame_cell_commit(call, frame);
    if (call->budget) ++call->budget->release_tickets;
    call->top = frame;
    ++accounting->depth;
    call_observer_event(call, frame, XR_XIR_CALL_FRAME_ENTER, XR_XIR_CALL_READY);
    if (accounting->depth > accounting->peak_depth)
        accounting->peak_depth = accounting->depth;
    return XR_XIR_CALL_READY;
}

/* Only metadata coordinates change. Work, scratch and Domain stay on the one
 * activation ledger even when preparation fails before frame publication. */
static void frame_metadata_apply(XrXirCall *call,const XrXirCallMetadata *metadata) {
    call->config.admission.arena=metadata->arena;
    call->config.admission.context=metadata->admission_context;
}
static XrXirCallStatus push_frame(XrXirCall *call,uint32_t id,
    const XrXirValue *arguments,uint32_t count,const XrXirFunctionBinding *binding,
    const CallFramePreparation *preparation) {
    if (!preparation || !preparation->metadata) return XR_XIR_CALL_BAD_STATE;
    frame_metadata_apply(call,preparation->metadata);
    XrXirCallStatus status=push_frame_owned(call,id,arguments,count,binding,preparation);
    frame_metadata_apply(call,call->top?call->top->metadata:&call->metadata);
    return status;
}
static XrXirCallView frame_view(XrXirCall *call) {
    CallFrame *frame = call->top;
    return (XrXirCallView) {call, call->config.instance, frame->entry->environment,
        frame->state, frame->arguments, frame->entry->parameter_count, frame->inbox,
        call->config.admission.arena, frame->exiting ? XR_XIR_CALL_EXIT : XR_XIR_CALL_NORMAL, frame->pending, frame->scope_exit};
}

static void pop_frame(XrXirCall *call, XrXirCallStatus reason) {
    CallFrame *frame = call->top;
    if (call->budget && call->budget->native_hold==call) call->budget->native_hold=NULL;
    if (frame->cleanup_frontier) call->cleanup_incomplete = true;
    if (call->budget) {
        XR_CHECK(call->budget->released_frames < call->budget->release_tickets,
            "a published frame prepays exactly one physical release");
        ++call->budget->released_frames;
    }
    if (frame->entry->release) {
        XrXirCallView view = frame_view(call);
        call->cleaning = true;
        frame->entry->release(&view, reason);
        call->cleaning = false;
    }
    frame_cell_release(frame);
    if (!frame->observer_announced) call_observer_event(call, frame, XR_XIR_CALL_FRAME_ENTER, XR_XIR_CALL_READY);
    call_observer_event(call, frame, XR_XIR_CALL_FRAME_LEAVE, reason);
    call->top = frame->parent;
    for (uint32_t i = 0; i < frame->entry->parameter_count; ++i)
        xr_xir_value_drop(&frame->arguments[i]);
    xr_xir_call_result_drop(&frame->inbox);
    xr_xir_call_result_drop(&frame->pending);
    --call->config.accounting->depth;
    frame_release(call, frame);
    frame_metadata_apply(call,call->top?call->top->metadata:&call->metadata);
}

static void abort_frames(XrXirCall *call, XrXirCallStatus reason) {
    if (call->aborting) return;
    call->aborting = true;
    if (call->abort_reason == XR_XIR_CALL_READY) call->abort_reason = reason;
}
#include "xxir_call_native_observer.inc.c"
#include "xxir_call_native_source.inc.c"
#include "xxir_call_exit.inc.c"

XrXirCallStatus xr_xir_call_config_init(XrXirCallConfig *config, size_t size) {
    if (!config) return XR_XIR_CALL_BAD_ARGUMENT;
    if (size != sizeof(*config)) return XR_XIR_CALL_BAD_ABI;
    *config = (XrXirCallConfig) {XR_XIR_CALL_ABI_VERSION, sizeof(*config), NULL, 0, NULL,
        UINT64_C(16) << 20, UINT64_C(1000000), 4096, NULL, {0}, {0}};
    return XR_XIR_CALL_READY;
}
static XrXirCallStatus table_size(const XrXirCallConfig *config, uint64_t *bytes) {
    if (!config) return XR_XIR_CALL_BAD_ARGUMENT;
    if (config->abi_version != XR_XIR_CALL_ABI_VERSION || config->struct_size != sizeof(*config))
        return XR_XIR_CALL_BAD_ABI;
    if (config->admission.domain && !xr_xir_domain_current(config->admission.domain))
        return XR_XIR_CALL_BAD_ARGUMENT;
    if (!xr_xir_output_provider_valid(&config->output)) return XR_XIR_CALL_BAD_ABI;
    if (!config->entries || !config->entry_count || config->entry_count > 65536 ||
        !config->accounting || config->accounting->live_bytes || config->accounting->depth ||
        config->accounting->allocations != config->accounting->frees)
        return XR_XIR_CALL_BAD_ARGUMENT;
    *bytes = sizeof(XrXirCall) + (uint64_t) config->entry_count * sizeof(XrXirCallEntry);
    if (*bytes > config->byte_limit || *bytes > SIZE_MAX)
        return XR_XIR_CALL_LIMIT;
    const XrXirTypes *types = xr_xir_compile_type_arena_types(config->admission.arena);
    for (uint32_t i = 0; i < config->entry_count; ++i) {
        const XrXirCallEntry *entry = &config->entries[i];
        if (entry->abi_version != XR_XIR_CALL_ABI_VERSION)
            return XR_XIR_CALL_BAD_ABI;
        if ((entry->flags & ~XR_XIR_ENTRY_EXIT) || entry->cleanup_owner > i ||
            (entry->cleanup_owner && (entry->result != XR_XIR_UNIT ||
             !(config->entries[entry->cleanup_owner - 1].flags & XR_XIR_ENTRY_EXIT)))) return XR_XIR_CALL_BAD_ARGUMENT;
        if (!entry->resume || entry->parameter_count > 65536 ||
            (entry->parameter_count && !entry->parameters) ||
            xr_xir_type_is_cell(types, entry->result) ||
            (entry->result != XR_XIR_UNIT && entry->result != XR_XIR_BOOL && entry->result != XR_XIR_RUNE &&
             !xr_xir_type_is_number((XrXirType) entry->result) && !xr_xir_type_is_owned(types, entry->result)))
            return XR_XIR_CALL_BAD_ARGUMENT;
        *bytes += (uint64_t) entry->parameter_count * sizeof(XrXirType);
        if (*bytes > config->byte_limit || *bytes > SIZE_MAX)
            return XR_XIR_CALL_LIMIT;
        for (uint32_t p = 0; p < entry->parameter_count; ++p)
            if (entry->parameters[p] != XR_XIR_BOOL && entry->parameters[p] != XR_XIR_RUNE && !xr_xir_type_is_number((XrXirType) entry->parameters[p]) &&
                !xr_xir_type_is_owned(types, entry->parameters[p]))
                return XR_XIR_CALL_BAD_ARGUMENT;
    }
    return XR_XIR_CALL_READY;
}

static XrXirCallStatus call_new(const XrXirCallConfig *config, uint32_t entry,
    const XrXirCallRequest *request, XrXirCallBudget *budget, XrXirCall **output) {
    const XrXirValue *arguments = request->arguments;
    uint32_t count = request->count;
    if (!output)
        return XR_XIR_CALL_BAD_ARGUMENT;
    if (budget && budget->notifying) return XR_XIR_CALL_BUSY;
    *output = NULL;
#if !defined(XR_ARCH_X86_64)
    return XR_XIR_CALL_BAD_ABI;
#endif
    uint64_t bytes = 0;
    XrXirCallStatus status = table_size(config, &bytes);
    if (status != XR_XIR_CALL_READY)
        return status;
    if (budget && budget->observer && budget->observer_instance != config->instance) return XR_XIR_CALL_BAD_STATE;
    if (budget && budget->native_observer && budget->native_instance!=config->instance) return XR_XIR_CALL_BAD_STATE;
    if (budget && budget->native_source && budget->native_source_instance!=config->instance) return XR_XIR_CALL_BAD_STATE;
    if (entry >= config->entry_count || config->entries[entry].cleanup_owner)
        return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirValueAdmission admission = config->admission;
    status = entry_arguments(&config->entries[entry], arguments, count, NULL, &admission);
    if (status != XR_XIR_CALL_READY) return status;
    XrXirTypeArena *arena = (XrXirTypeArena *) admission.arena;
    if (arena && !xr_xir_compile_type_arena_retain(arena)) return XR_XIR_CALL_LIMIT;
    XrXirCall *call = call_allocate(config, budget, bytes, &status);
    if (!call) {
        xr_xir_compile_type_arena_drop(arena);
        return status;
    }
    call->config = *config;
    call->budget = budget;
    call->metadata=(XrXirCallMetadata){.entry_count=config->entry_count,.arena=admission.arena,
        .admission_context=admission.context,.cell_role=request->cell_role,.cell_context=request->cell_context};
    call->config.admission = admission;
    call->allocation_bytes = bytes;
    call->polls_left = config->poll_limit;
    call->result = call_result(XR_XIR_CALL_READY);
    XrXirCallEntry *entries = (XrXirCallEntry *) (call + 1);
    XrXirType *parameters = (XrXirType *) (entries + config->entry_count);
    memcpy(entries, config->entries, (size_t) config->entry_count * sizeof(*entries));
    call->config.entries = entries;
    call->metadata.entries = entries;
    for (uint32_t i = 0; i < config->entry_count; ++i) {
        if (entries[i].parameter_count) {
            memcpy(parameters, entries[i].parameters, (size_t) entries[i].parameter_count * sizeof(*parameters));
            entries[i].parameters = parameters;
            parameters += entries[i].parameter_count;
        } else entries[i].parameters = NULL;
    }
    XrXirFaultDetail preparation_fault = {0};
    CallFramePreparation preparation = {.admitted=true, .fault=&preparation_fault,.metadata=&call->metadata};
    status = push_frame(call, entry, arguments, count, NULL, &preparation);
    if (status != XR_XIR_CALL_READY) {
        xr_xir_compile_type_arena_drop(arena);
        call_deallocate(config->accounting, budget, call, bytes);
        return status;
    }
    if (call->budget) {
        call->observed_next = call->budget->observed_calls;
        if (call->observed_next) call->observed_next->observed_previous = call;
        call->budget->observed_calls = call;
    }
    *output = call;
    return XR_XIR_CALL_READY;
}

XrXirAction xr_xir_call_match_failure(void) {
    return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0,
        {XR_XIR_I64, 0, XR_XIR_CALL_MATCH_FAILURE}, {{442, 0, 0, 0}, {0}}, 0};
}

XR_FUNCDEF XrXirAction xr_xir_call_assertion(const XrXirValue *message) {
    if (!xr_xir_value_argument(message, NULL, XR_XIR_STRING))
        return xr_xir_call_fault(XR_XIR_RUN_BAD_ARGUMENT);
    return (XrXirAction){XR_XIR_ACTION_FAULT, 0, NULL, 0,
        {XR_XIR_I64, 0, XR_XIR_CALL_ASSERTION}, {{XR_XIR_PANIC_ASSERTION, 0, 0, 0}, *message}, 0};
}

static XrXirCallStatus output_call_status(XrXirOutputStatus status) {
    switch (status) {
    case XR_XIR_OUTPUT_OK: return XR_XIR_CALL_READY;
    case XR_XIR_OUTPUT_OOM: return XR_XIR_CALL_OOM;
    case XR_XIR_OUTPUT_LIMIT: return XR_XIR_CALL_LIMIT;
    case XR_XIR_OUTPUT_ERROR: return XR_XIR_CALL_OUTPUT_ERROR;
    case XR_XIR_OUTPUT_BAD_ABI: return XR_XIR_CALL_BAD_ABI;
    default: return XR_XIR_CALL_BAD_ARGUMENT;
    }
}
static void accept_output_action(XrXirCall *call, XrXirAction action) {
    bool writing = action.kind == XR_XIR_ACTION_WRITE_STREAM;
    if (action.callee < XR_XIR_STDOUT || action.callee > XR_XIR_OUTPUT_LINE ||
        (writing && action.callee == XR_XIR_OUTPUT_LINE) ||
        !boundary_value(action.value, XR_XIR_UNIT) || action.argument_count > 65536 ||
        (action.argument_count && !action.arguments) ||
        (action.callee != XR_XIR_OUTPUT_LINE && action.argument_count != 1)) {
        abort_frames(call, XR_XIR_CALL_BAD_STATE);
        return;
    }
    for (uint32_t i = 0; i < action.argument_count; ++i) {
        const XrXirValue *value = &action.arguments[i];
        if ((writing && value->type != XR_XIR_STRING) ||
            (value->type != XR_XIR_BOOL && value->type != XR_XIR_RUNE && !xr_xir_type_is_number((XrXirType) value->type) && value->type != XR_XIR_STRING) ||
            !xr_xir_value_argument(value, NULL, (XrXirType) value->type)) {
            abort_frames(call, XR_XIR_CALL_BAD_STATE); return;
        }
    }
    XrXirOutputGroup group = {action.callee == XR_XIR_OUTPUT_LINE ? XR_XIR_STDOUT :
        (XrXirOutputStream) action.callee, action.arguments, action.argument_count, action.callee == XR_XIR_OUTPUT_LINE};
    XrXirOutputStatus output_status = call->config.output.write ?
        call->config.output.write(call->config.output.context, &group) : XR_XIR_OUTPUT_ERROR;
    if (call->cancel_requested && !cleanup_active(call) && call->abort_reason == XR_XIR_CALL_READY)
        request_exit_status(call, XR_XIR_CALL_CANCELLED);
    else if (writing && call->config.output.write &&
        (output_status == XR_XIR_OUTPUT_OK || output_status == XR_XIR_OUTPUT_ERROR)) {
        xr_xir_call_result_drop(&call->top->inbox);
        call->top->inbox = call_result(XR_XIR_CALL_RETURNED);
        call->top->inbox.value = (XrXirValue) {XR_XIR_BOOL, 0, output_status == XR_XIR_OUTPUT_OK};
    }
    else if (output_status == XR_XIR_OUTPUT_ERROR || output_status == XR_XIR_OUTPUT_OOM ||
        output_status == XR_XIR_OUTPUT_LIMIT) request_execution_failure(call, output_call_status(output_status));
    else if (output_status != XR_XIR_OUTPUT_OK) abort_frames(call, output_call_status(output_status));
    return;
}

static void accept_child_action(XrXirCall *call, XrXirAction action) {
    bool cleanup = (action.flags & XR_XIR_ACTION_CLEANUP) != 0;
    CallFrame *parent = call->top;
    XrXirCallMetadata metadata=*parent->metadata;
    XrXirCallStatus status=xr_xir_instance_call_target(call->active_view,&action.value,action.callee,&metadata);
    if (status!=XR_XIR_CALL_READY) {
        if (status==XR_XIR_CALL_OOM || status==XR_XIR_CALL_LIMIT) request_execution_failure(call,status);
        else abort_frames(call,status);
        return;
    }
    if (action.callee >= metadata.entry_count ||
        (cleanup ? (!parent->exiting || action.flags != XR_XIR_ACTION_CLEANUP ||
            !boundary_value(action.value, XR_XIR_UNIT) || metadata.entries!=parent->metadata->entries ||
            metadata.entries[action.callee].cleanup_owner !=
                (uint32_t)(parent->entry - parent->metadata->entries) + 1) :
            (parent->exiting || metadata.entries[action.callee].cleanup_owner != 0))) {
        abort_frames(call, XR_XIR_CALL_BAD_STATE); return;
    }
    const XrXirFunctionBinding *binding = xr_xir_function_binding(&action.value);
    if ((!binding && !boundary_value(action.value, XR_XIR_UNIT)) || (binding && binding->entry != action.callee)) {
        abort_frames(call, XR_XIR_CALL_BAD_STATE);return;
    }
    call->admitting = true;
    if (binding) {
        XrXirType type=(XrXirType)action.value.type;
        const XrXirTypeArena *actual=xr_xir_value_arena(&action.value);
        if (actual!=call->config.admission.arena) {
            uint64_t work=xir_type_arena_lookup_work(call->config.admission.arena);
            if (!call_cell_work(call,work)) status=XR_XIR_CALL_LIMIT;
            else if (!xir_type_arena_local_type(call->config.admission.arena,actual,type,&type))
                status=XR_XIR_CALL_BAD_ARGUMENT;
        }
        if (status==XR_XIR_CALL_READY) status=admit_value(&action.value,type,&call->config.admission);
    }
    XrXirFaultDetail preparation_fault = {0};
    CallFramePreparation preparation = {.admitted=false, .fault=&preparation_fault,
        .invocation=parent->pending_invocation.owner?&parent->pending_invocation:NULL,
        .function=&action.value,.metadata=&metadata};
    if (status == XR_XIR_CALL_READY)
        status = push_frame(call, action.callee, action.arguments, action.argument_count, binding,&preparation);
    call->admitting = false;
    parent->effect_pending=false;
    parent->pending_invocation=(XirEffectInvocationSelection){0};
    if (status != XR_XIR_CALL_READY) {
        if (!cleanup && status == XR_XIR_CALL_BOUNDS && xr_xir_fault_bounds_valid(preparation_fault)) {
            XrXirCallResult failed = call_result(status);
            failed.panic.detail = preparation_fault;
            if (action.flags & XR_XIR_ACTION_PROTECTED) {
                xr_xir_call_result_drop(&parent->inbox);
                xr_xir_call_result_move(&failed, &parent->inbox);
                parent->protected_call = false;
            } else request_exit(call, &failed, false);
        }
        else if (!cleanup && (status == XR_XIR_CALL_OOM || status == XR_XIR_CALL_LIMIT))
            request_execution_failure(call, status);
        else abort_frames(call, status);
    }
    else {
        call->top->cleanup_call = cleanup;
        xr_xir_call_result_drop(&parent->inbox);
        parent->inbox = call_result(XR_XIR_CALL_READY);
        parent->protected_call = (action.flags & XR_XIR_ACTION_PROTECTED) != 0;
    }
    return;
}

static void accept_action(XrXirCall *call, XrXirAction action) {
    bool panic = fault_panics(&action);
    uint32_t allowed = action.kind == XR_XIR_ACTION_CALL ? XR_XIR_ACTION_PROTECTED | XR_XIR_ACTION_CLEANUP :
        action.kind == XR_XIR_ACTION_AWAIT_TASK ? XR_XIR_ACTION_PROTECTED :
        action.kind == XR_XIR_ACTION_LEAVE ? XR_XIR_ACTION_LEAVE_ERROR | XR_XIR_ACTION_LEAVE_PANIC : 0;
    if ((panic ? !xr_xir_call_panic_payload((XrXirCallStatus) action.value.payload, &action.panic) :
         !xr_xir_panic_empty(&action.panic)) ||
        (action.flags & ~allowed)) {
        abort_frames(call, XR_XIR_CALL_BAD_STATE);
        return;
    }
    if (call->top->exiting && panic) { abort_frames(call, XR_XIR_CALL_BAD_STATE); return; }
    if (call->top->exiting && action.kind != XR_XIR_ACTION_CALL && action.kind != XR_XIR_ACTION_CONTINUE &&
        action.kind != XR_XIR_ACTION_EXIT_DONE && action.kind != XR_XIR_ACTION_FAULT) {
        abort_frames(call, XR_XIR_CALL_BAD_STATE); return;
    }
    if (action.kind == XR_XIR_ACTION_OUTPUT || action.kind == XR_XIR_ACTION_WRITE_STREAM) {
        accept_output_action(call, action); return;
    }
    if (action.kind == XR_XIR_ACTION_CALL) {
        accept_child_action(call, action); return;
    }
    if (action.callee || action.arguments || action.argument_count) {
        abort_frames(call, XR_XIR_CALL_BAD_STATE);
        return;
    }
    if (action.kind == XR_XIR_ACTION_LEAVE) { accept_scope_exit(call, action, panic); return; }
    if (action.kind == XR_XIR_ACTION_EXIT_DONE) {
        if (!call->top->exiting || !boundary_value(action.value, XR_XIR_UNIT)) abort_frames(call, XR_XIR_CALL_BAD_STATE);
        else call->top->exit_done = true;
        return;
    }
    if (action.kind == XR_XIR_ACTION_CONTINUE) {
        if (!boundary_value(action.value, XR_XIR_UNIT))
            abort_frames(call, XR_XIR_CALL_BAD_STATE);
        return;
    }
    if (action.kind == XR_XIR_ACTION_FAULT) {
        if (panic) {
            XrXirCallResult result = call_result((XrXirCallStatus) action.value.payload);
            if (xr_xir_panic_copy(&action.panic, &result.panic) != XR_XIR_VALUE_OK) {
                request_execution_failure(call, XR_XIR_CALL_LIMIT); return;
            }
            request_exit(call, &result, false);
            return;
        }
        XrXirCallStatus reason = XR_XIR_CALL_BAD_STATE;
        if (boundary_value(action.value, XR_XIR_I64) &&
            (action.value.payload == XR_XIR_CALL_OOM || action.value.payload == XR_XIR_CALL_LIMIT ||
             action.value.payload == XR_XIR_CALL_HOST_ERROR))
            reason = (XrXirCallStatus) action.value.payload;
        if (boundary_value(action.value, XR_XIR_I64) &&
            (action.value.payload == XR_XIR_CALL_BAD_ARGUMENT || action.value.payload == XR_XIR_CALL_UNSUPPORTED) &&
            !action.callee && !action.arguments && !action.argument_count && !action.flags &&
            xr_xir_panic_empty(&action.panic))
            reason = (XrXirCallStatus) action.value.payload;
        if (reason == XR_XIR_CALL_BAD_STATE) abort_frames(call, reason);
        else request_execution_failure(call, reason);
        return;
    }
    if (action.kind == XR_XIR_ACTION_AWAIT_TASK) {
        const XrXirTypes *types = xr_xir_compile_type_arena_types(call->config.admission.arena);
        const XrXirTypeNode *type = xr_xir_type_node(types, (XrXirType)action.value.type);
        if (action.callee || action.arguments || action.argument_count || !call->executor_owner ||
            !call->executor_generation || !call->executor_ticket ||
            !type || type->kind != XR_XIR_TYPE_TASK ||
            !xr_xir_value_argument(&action.value, call->config.admission.arena, (XrXirType)action.value.type) ||
            call->next_wake == UINT64_MAX) { abort_frames(call, XR_XIR_CALL_BAD_STATE); return; }
        if (cleanup_active(call)) {
            XrXirCallResult result = call_result(XR_XIR_CALL_DEFER_ASYNC);
            result.panic.detail.code = XR_XIR_PANIC_DEFER_ASYNC;
            request_exit(call, &result, false); return;
        }
        const XirTask *task = (const XirTask *)(uintptr_t)action.value.payload;
        XirTaskState state = atomic_load_explicit(&task->state, memory_order_acquire);
        if (task->generation != call->executor_generation || !task->identity ||
            task->identity == call->executor_ticket ||
            (state != XIR_TASK_TERMINAL && task->executor != call->executor_owner)) {
            abort_frames(call, XR_XIR_CALL_BAD_STATE); return;
        }
        XrXirValue prepared = {0};
        XrXirValueStatus copied = xr_xir_value_copy(&action.value, &prepared);
        if (copied != XR_XIR_VALUE_OK) {
            if (copied == XR_XIR_VALUE_OOM || copied == XR_XIR_VALUE_LIMIT || copied == XR_XIR_VALUE_REFCOUNT_LIMIT)
                request_execution_failure(call, copied == XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM : XR_XIR_CALL_LIMIT);
            else abort_frames(call, XR_XIR_CALL_BAD_STATE);
            return;
        }
        if (call->cancel_requested) { xr_xir_value_drop(&prepared); request_exit_status(call, XR_XIR_CALL_CANCELLED); return; }
        XR_CHECK(!call->waiting_task.type, "one activation owns at most one pending task lease");
        call->waiting_task = prepared;
        call->top->protected_call = (action.flags & XR_XIR_ACTION_PROTECTED) != 0;
        call->result = call_result(XR_XIR_CALL_SUSPENDED); call->result.wake = ++call->next_wake;
        call->wait = (XrXirWaitRequest){XR_XIR_WAIT_TASK, 0, 0, task, task->generation, task->identity};
        return;
    }
    if (action.kind == XR_XIR_ACTION_SUSPEND || action.kind == XR_XIR_ACTION_TIMER) {
        bool timer = action.kind == XR_XIR_ACTION_TIMER;
        bool shaped = timer ? boundary_value(action.value, XR_XIR_I64) && action.value.payload >= 1 &&
            (uint64_t) action.value.payload <= XR_XIR_TIMER_MAX_MS : boundary_value(action.value, XR_XIR_UNIT);
        if (cleanup_active(call) && shaped) {
            XrXirCallResult result = call_result(XR_XIR_CALL_DEFER_ASYNC);
            result.panic.detail.code = XR_XIR_PANIC_DEFER_ASYNC;
            request_exit(call, &result, false); return;
        }
        if (!shaped || call->next_wake == UINT64_MAX) {
            abort_frames(call, XR_XIR_CALL_BAD_STATE);
            return;
        }
        call->result = call_result(XR_XIR_CALL_SUSPENDED);
        call->result.wake = ++call->next_wake;
        call->wait = (XrXirWaitRequest) {timer ? XR_XIR_WAIT_TIMER_MS : XR_XIR_WAIT_YIELD, 0,
            timer ? (uint64_t) action.value.payload : 0, NULL, 0, 0};
        return;
    }
    bool returning = action.kind == XR_XIR_ACTION_RETURN;
    if (!returning && action.kind != XR_XIR_ACTION_THROW) {
        abort_frames(call, XR_XIR_CALL_BAD_STATE);
        return;
    }
    XrXirType result_type = returning ? call->top->entry->result : (XrXirType) action.value.type;
    if (!returning && !xr_xir_type_is_enum(xr_xir_compile_type_arena_types(call->config.admission.arena), result_type)) {
        abort_frames(call, XR_XIR_CALL_BAD_STATE);
        return;
    }
    call->admitting = true;
    XrXirCallStatus admitted = admit_value(&action.value, result_type, &call->config.admission);
    call->admitting = false;
    if (admitted != XR_XIR_CALL_READY) {
        if (admitted == XR_XIR_CALL_OOM || admitted == XR_XIR_CALL_LIMIT)
            request_execution_failure(call, admitted);
        else abort_frames(call, admitted == XR_XIR_CALL_BAD_ARGUMENT ? XR_XIR_CALL_BAD_STATE : admitted);
        return;
    }
    XrXirCallResult result = call_result(returning ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_THROWN);
    if (xr_xir_value_copy(&action.value, &result.value) != XR_XIR_VALUE_OK) {
        xr_xir_call_result_drop(&result);
        request_execution_failure(call, XR_XIR_CALL_LIMIT);
        return;
    }
    if (!returning && call->top->code_kind==XR_XIR_CALL_CODE_NATIVE && !cleanup_active(call)) {
        bool held=false;XrXirCallStatus observed=call_native_throw_offer(call,&result,false,&held);
        if (observed!=XR_XIR_CALL_READY) {
            xr_xir_call_result_drop(&result);
            if (observed==XR_XIR_CALL_OOM || observed==XR_XIR_CALL_LIMIT) request_execution_failure(call,observed);
            else abort_frames(call,XR_XIR_CALL_BAD_STATE);
            return;
        }
        if (held) return;
    }
    request_exit(call, &result, false);
}

XR_FUNC XrXirCallStatus xr_xir_call_new(const XrXirCallConfig *config, uint32_t entry,
    const XrXirValue *arguments, uint32_t count, XrXirCall **output) {
    if (config && config->abi_version == XR_XIR_CALL_ABI_VERSION &&
        config->struct_size == sizeof(*config) && config->admission.domain &&
        !xr_xir_domain_current(config->admission.domain)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallRequest request = {entry, arguments, count, NULL, NULL};
    return call_new(config, entry, &request, NULL, output);
}
XR_FUNC XrXirCallStatus xr_xir_call_new_budgeted(const XrXirCallConfig *config,
    const XrXirCallRequest *request, XrXirCallBudget *budget, XrXirCall **output) {
    if (config && config->abi_version == XR_XIR_CALL_ABI_VERSION &&
        config->struct_size == sizeof(*config) && config->admission.domain &&
        !xr_xir_domain_current(config->admission.domain)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!request || !budget || !xr_xir_domain_current(budget->work_domain)) return XR_XIR_CALL_BAD_ARGUMENT;
    return call_new(config, request->entry, request, budget, output);
}
XR_FUNC XrXirCallStatus xr_xir_call_take_outcome(XrXirCall *call, XrXirCallResult *output) {
    if (!call_owner_current(call)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!call || !xr_xir_call_result_empty(output)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->driving || call_observing(call)) return XR_XIR_CALL_BUSY;
    XrXirCallStatus status = call->result.status;
    if (status == XR_XIR_CALL_READY || status == XR_XIR_CALL_SUSPENDED || status == XR_XIR_CALL_CONSUMED)
        return XR_XIR_CALL_BAD_STATE;
    xr_xir_call_result_move(&call->result, output);
    call->result = call_result(XR_XIR_CALL_CONSUMED);
    return status;
}
XrXirCallResult xr_xir_call_poll_bounded(XrXirCall *call, uint64_t quantum) {
    if (!call_owner_current(call)) return (XrXirCallResult){.status = XR_XIR_CALL_BAD_ARGUMENT};
    if (!call || !quantum)
        return call_result(XR_XIR_CALL_BAD_ARGUMENT);
    if (call->driving || call_observing(call))
        return call_result(XR_XIR_CALL_BUSY);
    if (call->result.status != XR_XIR_CALL_READY)
        return call->result;
    if (call->budget && call->budget->native_hold==call) return call->result;
    call->driving = true;
    while (call->top && call->result.status == XR_XIR_CALL_READY && quantum &&
        (!call->budget || call->budget->native_hold!=call)) {
        if (!call->top->observer_announced)
            call_observer_event(call, call->top, XR_XIR_CALL_FRAME_ENTER, XR_XIR_CALL_READY);
        if (call->aborting) {
            --quantum;
            pop_frame(call, call->abort_reason);
            if (!call->top) {
                xr_xir_call_result_drop(&call->result);
                call->result = call_result(call->abort_reason);
            }
            continue;
        }
        if (call->budget && (call->budget->exhausted || !xr_xir_domain_work(call->budget->work_domain, 1))) {
            call->budget->exhausted = true;
            abort_frames(call, XR_XIR_CALL_LIMIT);
            continue;
        }
        if (call->budget) ++call->budget->transitions;
        if (call->cancel_requested && !cleanup_active(call) && call->abort_reason == XR_XIR_CALL_READY)
            request_exit_status(call, XR_XIR_CALL_CANCELLED);
        if (call->top->native_pending) {
            --quantum;XrXirCallResult original=call_result(XR_XIR_CALL_READY);
            bool scope=call->top->scope_exit;
            xr_xir_call_result_move(&call->top->pending,&original);
            call->top->native_pending=false;request_exit(call,&original,scope);continue;
        }
        if (call->top->exiting && (call->top->exit_done || !call->top->entered ||
            !(call->top->entry->flags & XR_XIR_ENTRY_EXIT))) {
            --quantum;
            finish_exit(call); continue;
        }
        if (!call->polls_left || call->config.accounting->polls == UINT64_MAX ||
            (call->budget && call->budget->resumes == call->budget->resume_limit)) {
            if (call->budget) call->budget->exhausted = true;
            abort_frames(call, XR_XIR_CALL_LIMIT);
            continue;
        }
        --quantum;
        --call->polls_left;
        if (call->budget) ++call->budget->resumes;
        ++call->config.accounting->polls;
        XrXirCallView view = frame_view(call);
        call->top->entered = true;
        call->active_view = &view;
        call->top->effect_pending=false;
        call->top->pending_invocation=(XirEffectInvocationSelection){0};
        call_observer_event(call, call->top, XR_XIR_CALL_DISPATCH_BEGIN, XR_XIR_CALL_READY);
        XrXirAction action = call->top->entry->resume(&view);
        if (call->top->code_kind == XR_XIR_CALL_CODE_UNKNOWN) {
            call->top->code_kind = XR_XIR_CALL_CODE_NATIVE;
            call_observer_event(call, call->top, XR_XIR_CALL_FRAME_CODE, XR_XIR_CALL_READY);
        }
        call_observer_event(call, call->top, XR_XIR_CALL_DISPATCH_END, XR_XIR_CALL_READY);
        /* Missing module publication is an owned initializer rejection, not
         * a malformed host action or incomplete cleanup. Classify it while
         * the exact callback view and executor binding are still active. */
        bool publication_missing = action.kind == XR_XIR_ACTION_FAULT &&
            xr_xir_instance_initialization_publication_missing(&view, &action);
        /* Authenticate the caller before invalidating its callback view. */
        XrXirCallStatus permission = action.kind == XR_XIR_ACTION_CALL ?
            xr_xir_instance_call_entry_admit(&view, action.callee, &action.value,
                action.arguments, action.argument_count) : XR_XIR_CALL_READY;
        /* A child prepare reuses this authentic parent view before commit.
         * Non-call actions retain the original callback boundary. */
        if (action.kind != XR_XIR_ACTION_CALL) call->active_view = NULL;
        if (call->cancel_requested && !cleanup_active(call) && call->abort_reason == XR_XIR_CALL_READY)
            request_exit_status(call, XR_XIR_CALL_CANCELLED);
        else if (permission==XR_XIR_CALL_OOM || permission==XR_XIR_CALL_LIMIT)
            request_execution_failure(call,permission);
        else if (permission != XR_XIR_CALL_READY) abort_frames(call, permission);
        else if (publication_missing) request_exit_status(call, XR_XIR_CALL_BAD_STATE);
        else accept_action(call, action);
        call->active_view = NULL;
        if (call->top) {
            /* A selection is consumed only by a successful frame publication. */
            call->top->effect_pending=false;
            call->top->pending_invocation=(XirEffectInvocationSelection){0};
        }
    }
    call->driving = false;
    return call->result;
}

XrXirCallStatus xr_xir_call_resume(XrXirCall *call, uint64_t wake) {
    if (!call_owner_current(call)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!call)
        return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->driving || call_observing(call))
        return XR_XIR_CALL_BUSY;
    if (call->result.status != XR_XIR_CALL_SUSPENDED || !wake || wake != call->result.wake ||
        call->wait.kind == XR_XIR_WAIT_TASK)
        return XR_XIR_CALL_BAD_STATE;
    call->result = call_result(XR_XIR_CALL_READY);
    call->wait = (XrXirWaitRequest) {0};
    return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xr_xir_call_bind_executor(XrXirCall *call, const XrXirExecutorBinding *binding) {
    if (!call_owner_current(call)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!call || !binding || !binding->owner || !binding->activation || !binding->generation || !binding->ticket)
        return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->driving || call_observing(call)) return XR_XIR_CALL_BUSY;
    if (call->executor_owner || call->result.status != XR_XIR_CALL_READY || call->next_wake ||
        !call->top || call->top->entered) return XR_XIR_CALL_BAD_STATE;
    call->executor_owner = binding->owner; call->executor_activation = binding->activation;
    call->executor_generation = binding->generation; call->executor_ticket = binding->ticket;
    return XR_XIR_CALL_READY;
}
XR_FUNC bool xr_xir_call_executor_member(const XrXirCall *call, const XrXirExecutorBinding *binding) {
    if (!call_owner_current(call)) return false;
    return call && binding && binding->owner && binding->activation && binding->generation && binding->ticket &&
        call->executor_owner == binding->owner && call->executor_activation == binding->activation &&
        call->executor_generation == binding->generation && call->executor_ticket == binding->ticket;
}
XR_FUNC bool xr_xir_call_driver_failure(const XrXirCall *call, const XrXirExecutorBinding *binding,
    XrXirCallStatus *output) {
    if (!call_owner_current(call)) return false;
    if (!output || !call || call->driving || call_observing(call) || !xr_xir_call_executor_member(call, binding)) return false;
    *output = call->abort_reason;
    return true;
}
XR_FUNC bool xr_xir_call_task_wait_token(const XrXirCall *call, XrXirTaskWaitToken *output) {
    if (!call_owner_current(call)) return false;
    if (!call || !output || call->driving || call_observing(call) || call->result.status != XR_XIR_CALL_SUSPENDED ||
        call->wait.kind != XR_XIR_WAIT_TASK || !call->executor_owner || !call->executor_activation) return false;
    *output = (XrXirTaskWaitToken){{call->executor_owner, call->executor_activation,
        call->executor_generation, call->executor_ticket}, call->result.wake, call->wait};
    return true;
}
static bool task_wait_token_matches(const XrXirCall *call, const XrXirTaskWaitToken *token) {
    if (!call || !token) return false;
    const XrXirWaitRequest *wait = &token->request;
    return xr_xir_call_executor_member(call, &token->binding) && call->top && !call->cancel_requested &&
        call->result.status == XR_XIR_CALL_SUSPENDED && token->wake && call->result.wake == token->wake &&
        call->wait.kind == XR_XIR_WAIT_TASK && wait->kind == XR_XIR_WAIT_TASK && !wait->reserved && !wait->after_ms &&
        call->wait.subject == wait->subject && call->wait.generation == wait->generation &&
        call->wait.ticket == wait->ticket && call->waiting_task.type;
}
static void unlink_task_wait(XrXirCall *call) {
    if (!call->wait_linked) return;
    XR_CHECK(call->waiting_task.type && call->wait.kind == XR_XIR_WAIT_TASK,
        "a linked waiter owns its target until unlink completes");
    XirTask *task = (XirTask *)(uintptr_t)call->waiting_task.payload;
    if (call->wait_previous) call->wait_previous->wait_next = call->wait_next;
    else {
        XR_CHECK(task->waiter_head == call, "waiter head belongs to its exact target");
        task->waiter_head = call->wait_next;
    }
    if (call->wait_next) call->wait_next->wait_previous = call->wait_previous;
    else {
        XR_CHECK(task->waiter_tail == call, "waiter tail belongs to its exact target");
        task->waiter_tail = call->wait_previous;
    }
    call->wait_previous = call->wait_next = NULL; call->wait_linked = false;
}
XR_FUNC XrXirCallStatus xr_xir_call_link_task_wait(XrXirCall *call, const XrXirTaskWaitToken *token) {
    if (!call_owner_current(call)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!call || !token) return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->driving || call_observing(call)) return XR_XIR_CALL_BUSY;
    if (!task_wait_token_matches(call, token) || call->wait_linked) return XR_XIR_CALL_BAD_STATE;
    if (!xr_xir_domain_work(call->config.admission.domain, 1)) return XR_XIR_CALL_LIMIT;
    XirTask *task = (XirTask *)(uintptr_t)call->waiting_task.payload;
    if (task->generation != token->binding.generation || task->identity != token->request.ticket ||
        task->identity == token->binding.ticket) return XR_XIR_CALL_BAD_STATE;
    if (atomic_load_explicit(&task->state, memory_order_acquire) == XIR_TASK_TERMINAL) return XR_XIR_CALL_RETURNED;
    if (task->executor != token->binding.owner) return XR_XIR_CALL_BAD_STATE;
    call->wait_previous = task->waiter_tail;
    if (task->waiter_tail) task->waiter_tail->wait_next = call;
    else task->waiter_head = call;
    task->waiter_tail = call; call->wait_linked = true;
    return XR_XIR_CALL_SUSPENDED;
}
XR_FUNC XrXirCallStatus xr_xir_call_complete_task_wait(XrXirCall *call, const XrXirTaskWaitToken *token,
    XrXirCallResult *outcome) {
    if (!call_owner_current(call)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!call || !token || !outcome) return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->driving || call_observing(call)) return XR_XIR_CALL_BUSY;
    if (!task_wait_token_matches(call, token) || !xr_xir_call_result_valid(outcome) ||
        outcome->status == XR_XIR_CALL_READY || outcome->status == XR_XIR_CALL_SUSPENDED ||
        outcome->status == XR_XIR_CALL_CONSUMED) return XR_XIR_CALL_BAD_STATE;
    const XrXirWaitRequest *wait = &token->request;
    const XirTask *task = (const XirTask *)(uintptr_t)call->waiting_task.payload;
    if (atomic_load_explicit(&task->state, memory_order_acquire) != XIR_TASK_TERMINAL ||
        task->generation != wait->generation || task->identity != wait->ticket ||
        task->outcome.status != outcome->status || task->outcome.value.type != outcome->value.type ||
        task->outcome.value.reserved != outcome->value.reserved || task->outcome.value.payload != outcome->value.payload ||
        task->outcome.panic.detail.code != outcome->panic.detail.code ||
        task->outcome.panic.detail.reserved != outcome->panic.detail.reserved ||
        task->outcome.panic.detail.index != outcome->panic.detail.index ||
        task->outcome.panic.detail.length != outcome->panic.detail.length ||
        task->outcome.panic.message.type != outcome->panic.message.type ||
        task->outcome.panic.message.reserved != outcome->panic.message.reserved ||
        task->outcome.panic.message.payload != outcome->panic.message.payload) return XR_XIR_CALL_BAD_STATE;
    unlink_task_wait(call);
    bool propagate = outcome->status == XR_XIR_CALL_CANCELLED ||
        (xr_xir_call_panic_status(outcome->status) && !call->top->protected_call);
    call->top->protected_call = false;
    xr_xir_value_drop(&call->waiting_task);
    call->wait = (XrXirWaitRequest){0}; call->result = call_result(XR_XIR_CALL_READY);
    if (propagate) request_exit(call, outcome, false);
    else if (outcome->status != XR_XIR_CALL_RETURNED && outcome->status != XR_XIR_CALL_THROWN &&
        !xr_xir_call_panic_status(outcome->status)) {
        XrXirCallStatus reason = outcome->status;
        xr_xir_call_result_drop(outcome);
        request_execution_failure(call, reason);
    } else {
        xr_xir_call_result_drop(&call->top->inbox);
        xr_xir_call_result_move(outcome, &call->top->inbox);
    }
    return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xr_xir_call_fail_task_wait(XrXirCall *call, const XrXirTaskWaitToken *token,
    XrXirCallStatus reason) {
    if (!call_owner_current(call)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!call || !token) return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->driving || call_observing(call)) return XR_XIR_CALL_BUSY;
    if (!task_wait_token_matches(call, token) || (reason != XR_XIR_CALL_OOM && reason != XR_XIR_CALL_LIMIT &&
        reason != XR_XIR_CALL_BAD_STATE && reason != XR_XIR_CALL_BAD_ARGUMENT && reason != XR_XIR_CALL_UNSUPPORTED))
        return XR_XIR_CALL_BAD_STATE;
    unlink_task_wait(call); xr_xir_value_drop(&call->waiting_task);
    call->wait = (XrXirWaitRequest){0}; call->result = call_result(XR_XIR_CALL_READY);
    request_execution_failure(call, reason);
    return XR_XIR_CALL_READY;
}

XrXirCallStatus xr_xir_call_wait_request(const XrXirCall *call, uint64_t wake, XrXirWaitRequest *output) {
    if (!call_owner_current(call)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!call || !output) return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->driving || call_observing(call)) return XR_XIR_CALL_BUSY;
    if (call->result.status != XR_XIR_CALL_SUSPENDED || !wake || wake != call->result.wake ||
        call->wait.kind == XR_XIR_WAIT_NONE) return XR_XIR_CALL_BAD_STATE;
    *output = call->wait;
    return XR_XIR_CALL_READY;
}

XrXirCallStatus xr_xir_call_take_result(XrXirCall *call, XrXirValue *output) {
    if (!call_owner_current(call)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!call || !output || !boundary_value(*output, XR_XIR_UNIT)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->driving || call_observing(call)) return XR_XIR_CALL_BUSY;
    XrXirCallStatus status = call->result.status;
    if (status != XR_XIR_CALL_RETURNED && status != XR_XIR_CALL_THROWN) return XR_XIR_CALL_BAD_STATE;
    *output = call->result.value;
    call->result = call_result(XR_XIR_CALL_CONSUMED);
    return status;
}

XrXirCallStatus xr_xir_call_request_cancel(XrXirCall *call) {
    if (!call_owner_current(call)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!call)
        return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->cleaning || call_observing(call))
        return XR_XIR_CALL_BUSY;
    if (call->aborting || (call->result.status != XR_XIR_CALL_READY && call->result.status != XR_XIR_CALL_SUSPENDED))
        return XR_XIR_CALL_BAD_STATE;
    call->cancel_requested = true;
    if (call->budget && call->budget->native_hold==call) call->budget->native_hold=NULL;
    if (call->result.status == XR_XIR_CALL_SUSPENDED) {
        unlink_task_wait(call);
        xr_xir_value_drop(&call->waiting_task);
        call->result = call_result(XR_XIR_CALL_READY);
        call->wait = (XrXirWaitRequest) {0};
    }
    return XR_XIR_CALL_CANCEL_REQUESTED;
}

XrXirCallStatus xr_xir_call_free(XrXirCall *call) {
    if (!call_owner_current(call)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!call)
        return XR_XIR_CALL_READY;
    if (call->driving || call_observing(call))
        return XR_XIR_CALL_BUSY;
    XrXirCallStatus status = XR_XIR_CALL_READY;
    if (call->top) {
        if (!call->aborting) (void)xr_xir_call_request_cancel(call);
        do { status = xr_xir_call_poll_bounded(call, 256).status; }
        while (status == XR_XIR_CALL_READY);
        if (status == XR_XIR_CALL_CANCELLED) status = XR_XIR_CALL_READY;
    }
    call->driving = true;
    unlink_task_wait(call);
    xr_xir_value_drop(&call->waiting_task);
    xr_xir_call_result_drop(&call->result);
    xr_xir_compile_type_arena_drop((XrXirTypeArena *) call->metadata.arena);
    if (call->budget) {
        if (call->observed_previous) call->observed_previous->observed_next = call->observed_next;
        else call->budget->observed_calls = call->observed_next;
        if (call->observed_next) call->observed_next->observed_previous = call->observed_previous;
    }
    call_deallocate(call->config.accounting, call->budget, call, call->allocation_bytes);
    return status;
}
