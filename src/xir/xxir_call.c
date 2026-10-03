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
#include "xxir_type_arena.h"
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

typedef struct CallFrame {
    struct CallFrame *parent;
    const XrXirCallEntry *entry;
    XrXirCallResult inbox, pending;
    uint64_t allocation_bytes;
    void *state;
    XrXirValue *arguments;
    bool protected_call, entered, exiting, scope_exit, cleanup_call, in_cleanup, exit_done;
} CallFrame;

struct XrXirCall {
    XrXirCallConfig config;
    CallFrame *top;
    CallSegment *segment;
    const XrXirCallView *active_view;
    XrXirCallResult result;
    uint64_t allocation_bytes, polls_left, next_wake;
    XrXirWaitRequest wait;
    bool driving, cleaning, admitting, cancel_requested;
    bool aborting;
    XrXirCallStatus abort_reason;
};

XrXirAction xr_xir_call_fault(XrXirRunStatus status) {
    XrXirCallStatus reason;
    switch (status) {
        case XR_XIR_RUN_DIVIDE_BY_ZERO: reason = XR_XIR_CALL_DIVIDE_BY_ZERO; break;
        case XR_XIR_RUN_NUMERIC_RANGE: reason = XR_XIR_CALL_NUMERIC_RANGE; break;
        case XR_XIR_RUN_OUT_OF_MEMORY: reason = XR_XIR_CALL_OOM; break;
        case XR_XIR_RUN_HOST_ERROR: reason = XR_XIR_CALL_HOST_ERROR; break;
        case XR_XIR_RUN_STEP_LIMIT:
        case XR_XIR_RUN_FRAME_LIMIT: reason = XR_XIR_CALL_LIMIT; break;
        default: reason = XR_XIR_CALL_BAD_STATE; break;
    }
    XrXirAction action = {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, reason}, {0}, 0};
    if (reason == XR_XIR_CALL_DIVIDE_BY_ZERO) action.panic.detail.code = XR_XIR_PANIC_DIVIDE;
    else if (reason == XR_XIR_CALL_NUMERIC_RANGE) action.panic.detail.code = XR_XIR_PANIC_RANGE;
    return action;
}
XrXirAction xr_xir_call_numeric_fault(XrXirRunStatus status, bool remainder) {
    XrXirAction action = xr_xir_call_fault(status);
    if (remainder && status == XR_XIR_RUN_DIVIDE_BY_ZERO) action.panic.detail.code = XR_XIR_PANIC_REMAINDER;
    return action;
}
bool xr_xir_call_panic_status(XrXirCallStatus status) {
    return status == XR_XIR_CALL_DIVIDE_BY_ZERO || status == XR_XIR_CALL_NUMERIC_RANGE ||
        status == XR_XIR_CALL_BOUNDS || status == XR_XIR_CALL_MATCH_FAILURE || status == XR_XIR_CALL_DEFER_ASYNC || status == XR_XIR_CALL_ASSERTION;
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
    default: return false;
    }
}

uint32_t xr_xir_call_current_entry(const XrXirCall *call) {
    return call && call->driving && !call->cleaning && call->top ?
        (uint32_t) (call->top->entry - call->config.entries) : UINT32_MAX;
}
XrXirCallStatus xr_xir_call_state(const XrXirCall *call) {
    return !call ? XR_XIR_CALL_BAD_ARGUMENT : call->driving ? XR_XIR_CALL_BUSY : call->result.status;
}
static bool cleanup_active(const XrXirCall *call) {
    return call->top && (call->top->exiting || call->top->in_cleanup);
}
bool xr_xir_call_cleanup_active(const XrXirCall *call) {
    return call && call->driving && !call->cleaning && (call->active_view || call->admitting) && cleanup_active(call);
}
XrXirValueAdmission *xr_xir_call_admission(const XrXirCallView *view) {
    if (!view || !view->activation) return NULL;
    XrXirCall *call = view->activation;
    if (!call->driving || call->cleaning || !call->top || call->active_view != view)
        return NULL;
    const CallFrame *frame = call->top;
    if (view->instance != call->config.instance || view->environment != frame->entry->environment ||
        view->state != frame->state || view->arguments != frame->arguments ||
        view->argument_count != frame->entry->parameter_count || view->arena != call->config.admission.arena ||
        view->phase != (frame->exiting ? XR_XIR_CALL_EXIT : XR_XIR_CALL_NORMAL) || view->scope_exit != frame->scope_exit)
        return NULL;
    return &call->config.admission;
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
        action->value.payload >= XR_XIR_CALL_READY && action->value.payload <= XR_XIR_CALL_ASSERTION &&
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
    if (type == XR_XIR_UNIT)
        return value && boundary_value(*value, type) ? XR_XIR_CALL_READY : XR_XIR_CALL_BAD_ARGUMENT;
    if (!xr_xir_value_argument(value, admission->arena, type)) return XR_XIR_CALL_BAD_ARGUMENT;
    if ((uint32_t) type < XR_XIR_CONSTRUCTED_TYPE_BASE) return XR_XIR_CALL_READY;
    XrXirValueStatus status = xr_xir_value_admit(value, type, admission);
    if (status == XR_XIR_VALUE_OK) return XR_XIR_CALL_READY;
    if (status == XR_XIR_VALUE_OOM) return XR_XIR_CALL_OOM;
    if (status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_REFCOUNT_LIMIT) return XR_XIR_CALL_LIMIT;
    return XR_XIR_CALL_BAD_ARGUMENT;
}

static void *call_allocate(const XrXirCallConfig *config, uint64_t bytes,
                           XrXirCallStatus *status) {
    XrXirCallAccounting *accounting = config->accounting;
    if (bytes > SIZE_MAX || accounting->live_bytes > config->byte_limit ||
        bytes > config->byte_limit - accounting->live_bytes ||
        accounting->allocations == UINT64_MAX) {
        *status = XR_XIR_CALL_LIMIT;
        return NULL;
    }
    void *memory = xr_calloc(1, (size_t) bytes);
    if (!memory) {
        *status = XR_XIR_CALL_OOM;
        return NULL;
    }
    accounting->live_bytes += bytes;
    if (accounting->live_bytes > accounting->peak_bytes)
        accounting->peak_bytes = accounting->live_bytes;
    ++accounting->allocations;
    return memory;
}

static void call_deallocate(XrXirCallAccounting *accounting, void *memory, uint64_t bytes) {
    XR_CHECK(memory && accounting->live_bytes >= bytes &&
             accounting->allocations > accounting->frees, "invalid call storage release");
    accounting->live_bytes -= bytes;
    ++accounting->frees;
    xr_free(memory);
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
        segment = call_allocate(&call->config, capacity, status);
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
        call_deallocate(call->config.accounting, segment, segment->allocation_bytes);
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

static XrXirCallStatus push_frame(XrXirCall *call, uint32_t id,
    const XrXirValue *arguments, uint32_t count, const XrXirFunctionBinding *binding, bool admitted) {
    if (id >= call->config.entry_count)
        return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirCallEntry *entry = &call->config.entries[id];
    if (!admitted) {
        XrXirCallStatus status = entry_arguments(entry, arguments, count, binding, &call->config.admission);
        if (status != XR_XIR_CALL_READY) return status;
    }
    XrXirCallAccounting *accounting = call->config.accounting;
    if (accounting->depth >= call->config.depth_limit)
        return XR_XIR_CALL_LIMIT;
    uint64_t arguments_offset = state_offset() + ((uint64_t) entry->state_bytes + 7) / 8 * 8;
    uint64_t bytes = arguments_offset + (uint64_t) entry->parameter_count * sizeof(XrXirValue);
    XrXirCallStatus status = XR_XIR_CALL_READY;
    CallFrame *frame = frame_reserve(call, bytes, &status);
    if (!frame)
        return status;
    frame->in_cleanup = cleanup_active(call);
    frame->parent = call->top;
    frame->entry = entry;
    frame->inbox = call_result(XR_XIR_CALL_READY);
    frame->state = (unsigned char *) frame + (size_t) state_offset();
    frame->arguments = (XrXirValue *) ((unsigned char *) frame + (size_t) arguments_offset);
    uint32_t captures = binding ? binding->capture_count : 0;
    for (uint32_t i = 0; i < entry->parameter_count; ++i) {
        const XrXirValue *value = i < captures ? &binding->captures[i] : &arguments[i - captures];
        if (xr_xir_value_copy(value, &frame->arguments[i]) != XR_XIR_VALUE_OK) {
            for (uint32_t p = 0; p < i; ++p) xr_xir_value_drop(&frame->arguments[p]);
            frame_release(call, frame);
            return XR_XIR_CALL_LIMIT;
        }
    }
    call->top = frame;
    ++accounting->depth;
    if (accounting->depth > accounting->peak_depth)
        accounting->peak_depth = accounting->depth;
    return XR_XIR_CALL_READY;
}

static XrXirCallView frame_view(XrXirCall *call) {
    CallFrame *frame = call->top;
    return (XrXirCallView) {call, call->config.instance, frame->entry->environment,
        frame->state, frame->arguments, frame->entry->parameter_count, frame->inbox,
        call->config.admission.arena, frame->exiting ? XR_XIR_CALL_EXIT : XR_XIR_CALL_NORMAL, frame->pending, frame->scope_exit};
}

static void pop_frame(XrXirCall *call, XrXirCallStatus reason) {
    CallFrame *frame = call->top;
    if (frame->entry->release) {
        XrXirCallView view = frame_view(call);
        call->cleaning = true;
        frame->entry->release(&view, reason);
        call->cleaning = false;
    }
    call->top = frame->parent;
    for (uint32_t i = 0; i < frame->entry->parameter_count; ++i)
        xr_xir_value_drop(&frame->arguments[i]);
    xr_xir_call_result_drop(&frame->inbox);
    xr_xir_call_result_drop(&frame->pending);
    --call->config.accounting->depth;
    frame_release(call, frame);
}

static void abort_frames(XrXirCall *call, XrXirCallStatus reason) {
    if (call->aborting) return;
    call->aborting = true;
    call->abort_reason = reason;
}
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
            (entry->result != XR_XIR_UNIT && entry->result != XR_XIR_BOOL &&
             !xr_xir_type_is_number((XrXirType) entry->result) && !xr_xir_type_is_owned(types, entry->result)))
            return XR_XIR_CALL_BAD_ARGUMENT;
        *bytes += (uint64_t) entry->parameter_count * sizeof(XrXirType);
        if (*bytes > config->byte_limit || *bytes > SIZE_MAX)
            return XR_XIR_CALL_LIMIT;
        for (uint32_t p = 0; p < entry->parameter_count; ++p)
            if (entry->parameters[p] != XR_XIR_BOOL && !xr_xir_type_is_number((XrXirType) entry->parameters[p]) &&
                !xr_xir_type_is_owned(types, entry->parameters[p]))
                return XR_XIR_CALL_BAD_ARGUMENT;
    }
    return XR_XIR_CALL_READY;
}

XrXirCallStatus xr_xir_call_new(const XrXirCallConfig *config, uint32_t entry,
    const XrXirValue *arguments, uint32_t count, XrXirCall **output) {
    if (!output)
        return XR_XIR_CALL_BAD_ARGUMENT;
    *output = NULL;
#if !defined(XR_ARCH_X86_64)
    return XR_XIR_CALL_BAD_ABI;
#endif
    uint64_t bytes = 0;
    XrXirCallStatus status = table_size(config, &bytes);
    if (status != XR_XIR_CALL_READY)
        return status;
    if (entry >= config->entry_count || config->entries[entry].cleanup_owner)
        return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirValueAdmission admission = config->admission;
    status = entry_arguments(&config->entries[entry], arguments, count, NULL, &admission);
    if (status != XR_XIR_CALL_READY) return status;
    XrXirTypeArena *arena = (XrXirTypeArena *) admission.arena;
    if (arena && !xr_xir_compile_type_arena_retain(arena)) return XR_XIR_CALL_LIMIT;
    XrXirCall *call = call_allocate(config, bytes, &status);
    if (!call) {
        xr_xir_compile_type_arena_drop(arena);
        return status;
    }
    call->config = *config;
    call->config.admission = admission;
    call->allocation_bytes = bytes;
    call->polls_left = config->poll_limit;
    call->result = call_result(XR_XIR_CALL_READY);
    XrXirCallEntry *entries = (XrXirCallEntry *) (call + 1);
    XrXirType *parameters = (XrXirType *) (entries + config->entry_count);
    memcpy(entries, config->entries, (size_t) config->entry_count * sizeof(*entries));
    call->config.entries = entries;
    for (uint32_t i = 0; i < config->entry_count; ++i) {
        if (entries[i].parameter_count) {
            memcpy(parameters, entries[i].parameters, (size_t) entries[i].parameter_count * sizeof(*parameters));
            entries[i].parameters = parameters;
            parameters += entries[i].parameter_count;
        } else entries[i].parameters = NULL;
    }
    status = push_frame(call, entry, arguments, count, NULL, true);
    if (status != XR_XIR_CALL_READY) {
        xr_xir_compile_type_arena_drop(arena);
        call_deallocate(config->accounting, call, bytes);
        return status;
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
static void accept_action(XrXirCall *call, XrXirAction action) {
    bool panic = fault_panics(&action);
    uint32_t allowed = action.kind == XR_XIR_ACTION_CALL ? XR_XIR_ACTION_PROTECTED | XR_XIR_ACTION_CLEANUP :
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
                (value->type != XR_XIR_BOOL && !xr_xir_type_is_number((XrXirType) value->type) && value->type != XR_XIR_STRING) ||
                !xr_xir_value_argument(value, NULL, (XrXirType) value->type)) {
                abort_frames(call, XR_XIR_CALL_BAD_STATE); return;
            }
        }
        XrXirOutputGroup group = {action.callee == XR_XIR_OUTPUT_LINE ? XR_XIR_STDOUT :
            (XrXirOutputStream) action.callee, action.arguments, action.argument_count, action.callee == XR_XIR_OUTPUT_LINE};
        XrXirOutputStatus output_status = call->config.output.write ?
            call->config.output.write(call->config.output.context, &group) : XR_XIR_OUTPUT_ERROR;
        if (call->cancel_requested && !cleanup_active(call)) request_exit_status(call, XR_XIR_CALL_CANCELLED);
        else if (writing && call->config.output.write &&
            (output_status == XR_XIR_OUTPUT_OK || output_status == XR_XIR_OUTPUT_ERROR)) {
            xr_xir_call_result_drop(&call->top->inbox);
            call->top->inbox = call_result(XR_XIR_CALL_RETURNED);
            call->top->inbox.value = (XrXirValue) {XR_XIR_BOOL, 0, output_status == XR_XIR_OUTPUT_OK};
        }
        else if (output_status != XR_XIR_OUTPUT_OK) abort_frames(call, output_call_status(output_status));
        return;
    }
    if (action.kind == XR_XIR_ACTION_CALL) {
        bool cleanup = (action.flags & XR_XIR_ACTION_CLEANUP) != 0;
        if (action.callee >= call->config.entry_count ||
            (cleanup ? (!call->top->exiting || action.flags != XR_XIR_ACTION_CLEANUP ||
                !boundary_value(action.value, XR_XIR_UNIT) ||
                call->config.entries[action.callee].cleanup_owner !=
                    (uint32_t)(call->top->entry - call->config.entries) + 1) :
                (call->top->exiting || call->config.entries[action.callee].cleanup_owner != 0))) {
            abort_frames(call, XR_XIR_CALL_BAD_STATE); return;
        }
        const XrXirFunctionBinding *binding = xr_xir_function_binding(&action.value);
        if ((!binding && !boundary_value(action.value, XR_XIR_UNIT)) || (binding && binding->entry != action.callee)) {
            abort_frames(call, XR_XIR_CALL_BAD_STATE);
            return;
        }
        CallFrame *parent = call->top;
        call->admitting = true;
        XrXirCallStatus status = binding ? admit_value(&action.value, (XrXirType) action.value.type,
            &call->config.admission) : XR_XIR_CALL_READY;
        if (status == XR_XIR_CALL_READY)
            status = entry_arguments(&call->config.entries[action.callee], action.arguments,
                action.argument_count, binding, &call->config.admission);
        call->admitting = false;
        if (status == XR_XIR_CALL_READY)
            status = push_frame(call, action.callee, action.arguments, action.argument_count, binding, true);
        if (status != XR_XIR_CALL_READY)
            abort_frames(call, status);
        else {
            call->top->cleanup_call = cleanup;
            xr_xir_call_result_drop(&parent->inbox);
            parent->inbox = call_result(XR_XIR_CALL_READY);
            parent->protected_call = (action.flags & XR_XIR_ACTION_PROTECTED) != 0;
        }
        return;
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
                abort_frames(call, XR_XIR_CALL_LIMIT); return;
            }
            request_exit(call, &result, false);
            return;
        }
        XrXirCallStatus reason = XR_XIR_CALL_BAD_STATE;
        if (boundary_value(action.value, XR_XIR_I64) &&
            (action.value.payload == XR_XIR_CALL_OOM || action.value.payload == XR_XIR_CALL_LIMIT ||
             action.value.payload == XR_XIR_CALL_HOST_ERROR))
            reason = (XrXirCallStatus) action.value.payload;
        abort_frames(call, reason);
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
            timer ? (uint64_t) action.value.payload : 0};
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
        abort_frames(call, admitted == XR_XIR_CALL_BAD_ARGUMENT ? XR_XIR_CALL_BAD_STATE : admitted);
        return;
    }
    XrXirCallResult result = call_result(returning ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_THROWN);
    if (xr_xir_value_copy(&action.value, &result.value) != XR_XIR_VALUE_OK) {
        abort_frames(call, XR_XIR_CALL_LIMIT);
        return;
    }
    request_exit(call, &result, false);
}

XrXirCallResult xr_xir_call_poll_bounded(XrXirCall *call, uint64_t quantum) {
    if (!call || !quantum)
        return call_result(XR_XIR_CALL_BAD_ARGUMENT);
    if (call->driving)
        return call_result(XR_XIR_CALL_BUSY);
    if (call->result.status != XR_XIR_CALL_READY)
        return call->result;
    call->driving = true;
    while (call->top && call->result.status == XR_XIR_CALL_READY && quantum) {
        if (call->aborting) {
            --quantum;
            pop_frame(call, call->abort_reason);
            if (!call->top) {
                xr_xir_call_result_drop(&call->result);
                call->result = call_result(call->abort_reason);
            }
            continue;
        }
        if (call->cancel_requested && !cleanup_active(call))
            request_exit_status(call, XR_XIR_CALL_CANCELLED);
        if (call->top->exiting && (call->top->exit_done || !call->top->entered ||
            !(call->top->entry->flags & XR_XIR_ENTRY_EXIT))) {
            --quantum;
            finish_exit(call); continue;
        }
        if (!call->polls_left || call->config.accounting->polls == UINT64_MAX) {
            abort_frames(call, XR_XIR_CALL_LIMIT);
            continue;
        }
        --quantum;
        --call->polls_left;
        ++call->config.accounting->polls;
        XrXirCallView view = frame_view(call);
        call->top->entered = true;
        call->active_view = &view;
        XrXirAction action = call->top->entry->resume(&view);
        call->active_view = NULL;
        if (call->cancel_requested && !cleanup_active(call))
            request_exit_status(call, XR_XIR_CALL_CANCELLED);
        else accept_action(call, action);
    }
    call->driving = false;
    return call->result;
}

XrXirCallStatus xr_xir_call_resume(XrXirCall *call, uint64_t wake) {
    if (!call)
        return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->driving)
        return XR_XIR_CALL_BUSY;
    if (call->result.status != XR_XIR_CALL_SUSPENDED || !wake || wake != call->result.wake)
        return XR_XIR_CALL_BAD_STATE;
    call->result = call_result(XR_XIR_CALL_READY);
    call->wait = (XrXirWaitRequest) {0};
    return XR_XIR_CALL_READY;
}

XrXirCallStatus xr_xir_call_wait_request(const XrXirCall *call, uint64_t wake, XrXirWaitRequest *output) {
    if (!call || !output) return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->driving) return XR_XIR_CALL_BUSY;
    if (call->result.status != XR_XIR_CALL_SUSPENDED || !wake || wake != call->result.wake ||
        call->wait.kind == XR_XIR_WAIT_NONE) return XR_XIR_CALL_BAD_STATE;
    *output = call->wait;
    return XR_XIR_CALL_READY;
}

XrXirCallStatus xr_xir_call_take_result(XrXirCall *call, XrXirValue *output) {
    if (!call || !output || !boundary_value(*output, XR_XIR_UNIT)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->driving) return XR_XIR_CALL_BUSY;
    XrXirCallStatus status = call->result.status;
    if (status != XR_XIR_CALL_RETURNED && status != XR_XIR_CALL_THROWN) return XR_XIR_CALL_BAD_STATE;
    *output = call->result.value;
    call->result = call_result(XR_XIR_CALL_CONSUMED);
    return status;
}

XrXirCallStatus xr_xir_call_request_cancel(XrXirCall *call) {
    if (!call)
        return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->cleaning)
        return XR_XIR_CALL_BUSY;
    if (call->aborting || (call->result.status != XR_XIR_CALL_READY && call->result.status != XR_XIR_CALL_SUSPENDED))
        return XR_XIR_CALL_BAD_STATE;
    call->cancel_requested = true;
    if (call->result.status == XR_XIR_CALL_SUSPENDED) {
        call->result = call_result(XR_XIR_CALL_READY);
        call->wait = (XrXirWaitRequest) {0};
    }
    return XR_XIR_CALL_CANCEL_REQUESTED;
}

XrXirCallStatus xr_xir_call_free(XrXirCall *call) {
    if (!call)
        return XR_XIR_CALL_READY;
    if (call->driving)
        return XR_XIR_CALL_BUSY;
    XrXirCallStatus status = XR_XIR_CALL_READY;
    if (call->top) {
        if (!call->aborting) (void)xr_xir_call_request_cancel(call);
        do { status = xr_xir_call_poll_bounded(call, 256).status; }
        while (status == XR_XIR_CALL_READY);
        if (status == XR_XIR_CALL_CANCELLED) status = XR_XIR_CALL_READY;
    }
    call->driving = true;
    xr_xir_call_result_drop(&call->result);
    xr_xir_compile_type_arena_drop((XrXirTypeArena *) call->config.admission.arena);
    call_deallocate(call->config.accounting, call, call->allocation_bytes);
    return status;
}
