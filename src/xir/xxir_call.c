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
 */
#include "xxir_call.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"

typedef struct CallFrame {
    struct CallFrame *parent;
    const XrXirCallEntry *entry;
    XrXirCallResult inbox;
    uint64_t allocation_bytes;
    void *state;
    XrXirValue *arguments;
} CallFrame;

struct XrXirCall {
    XrXirCallConfig config;
    CallFrame *top;
    XrXirCallResult result;
    uint64_t allocation_bytes, polls_left, next_wake;
    bool driving, cleaning, cancel_requested;
};

static XrXirCallResult call_result(XrXirCallStatus status) {
    return (XrXirCallResult) {status, {XR_XIR_UNIT, 0, 0}, 0};
}

static bool boundary_value(XrXirValue value, XrXirType type) {
    if (type == XR_XIR_UNIT)
        return value.type == XR_XIR_UNIT && !value.reserved && !value.payload;
    return xr_xir_value_argument(&value, type);
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

static bool entry_arguments(const XrXirCallEntry *entry, const XrXirValue *arguments,
                             uint32_t count) {
    if (count != entry->parameter_count || (count && !arguments))
        return false;
    for (uint32_t i = 0; i < count; ++i)
        if (!xr_xir_value_argument(&arguments[i], entry->parameters[i]))
            return false;
    return true;
}

static uint64_t state_offset(void) {
    uint64_t alignment = XR_XIR_CALL_STATE_ALIGNMENT;
    return (sizeof(CallFrame) + alignment - 1) / alignment * alignment;
}

static XrXirCallStatus push_frame(XrXirCall *call, uint32_t id,
                                  const XrXirValue *arguments, uint32_t count) {
    if (id >= call->config.entry_count)
        return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirCallEntry *entry = &call->config.entries[id];
    if (!entry_arguments(entry, arguments, count))
        return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallAccounting *accounting = call->config.accounting;
    if (accounting->depth >= call->config.depth_limit)
        return XR_XIR_CALL_LIMIT;
    uint64_t arguments_offset = state_offset() + ((uint64_t) entry->state_bytes + 7) / 8 * 8;
    uint64_t bytes = arguments_offset + (uint64_t) count * sizeof(XrXirValue);
    XrXirCallStatus status = XR_XIR_CALL_READY;
    CallFrame *frame = call_allocate(&call->config, bytes, &status);
    if (!frame)
        return status;
    frame->parent = call->top;
    frame->entry = entry;
    frame->allocation_bytes = bytes;
    frame->inbox = call_result(XR_XIR_CALL_READY);
    frame->state = (unsigned char *) frame + (size_t) state_offset();
    frame->arguments = (XrXirValue *) ((unsigned char *) frame + (size_t) arguments_offset);
    for (uint32_t i = 0; i < count; ++i) {
        if (xr_xir_value_copy(&arguments[i], &frame->arguments[i]) != XR_XIR_VALUE_OK) {
            for (uint32_t p = 0; p < i; ++p) xr_xir_value_drop(&frame->arguments[p]);
            call_deallocate(accounting, frame, bytes);
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
        frame->state, frame->arguments, frame->entry->parameter_count, frame->inbox};
}

static void pop_frame(XrXirCall *call, XrXirCallStatus reason) {
    CallFrame *frame = call->top;
    if (frame->entry->cleanup) {
        XrXirCallView view = frame_view(call);
        call->cleaning = true;
        frame->entry->cleanup(&view, reason);
        call->cleaning = false;
    }
    call->top = frame->parent;
    for (uint32_t i = 0; i < frame->entry->parameter_count; ++i)
        xr_xir_value_drop(&frame->arguments[i]);
    xr_xir_value_drop(&frame->inbox.value);
    --call->config.accounting->depth;
    call_deallocate(call->config.accounting, frame, frame->allocation_bytes);
}

static void unwind(XrXirCall *call, XrXirCallStatus reason) {
    while (call->top)
        pop_frame(call, reason);
    call->result = call_result(reason);
}

static XrXirCallStatus table_size(const XrXirCallConfig *config, uint64_t *bytes) {
    if (!config || !config->entries || !config->entry_count || config->entry_count > 65536 ||
        !config->accounting || config->accounting->live_bytes || config->accounting->depth ||
        config->accounting->allocations != config->accounting->frees)
        return XR_XIR_CALL_BAD_ARGUMENT;
    *bytes = sizeof(XrXirCall) + (uint64_t) config->entry_count * sizeof(XrXirCallEntry);
    if (*bytes > config->byte_limit || *bytes > SIZE_MAX)
        return XR_XIR_CALL_LIMIT;
    for (uint32_t i = 0; i < config->entry_count; ++i) {
        const XrXirCallEntry *entry = &config->entries[i];
        if (entry->abi_version != XR_XIR_CALL_ABI_VERSION)
            return XR_XIR_CALL_BAD_ABI;
        if (!entry->resume || entry->parameter_count > 65536 ||
            (entry->parameter_count && !entry->parameters) ||
            (entry->result != XR_XIR_UNIT && entry->result != XR_XIR_BOOL &&
             entry->result != XR_XIR_I64 && entry->result != XR_XIR_STRING))
            return XR_XIR_CALL_BAD_ARGUMENT;
        *bytes += (uint64_t) entry->parameter_count * sizeof(XrXirType);
        if (*bytes > config->byte_limit || *bytes > SIZE_MAX)
            return XR_XIR_CALL_LIMIT;
        for (uint32_t p = 0; p < entry->parameter_count; ++p)
            if (entry->parameters[p] != XR_XIR_BOOL && entry->parameters[p] != XR_XIR_I64 &&
                entry->parameters[p] != XR_XIR_STRING)
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
    if (entry >= config->entry_count || !entry_arguments(&config->entries[entry], arguments, count))
        return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCall *call = call_allocate(config, bytes, &status);
    if (!call)
        return status;
    call->config = *config;
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
    status = push_frame(call, entry, arguments, count);
    if (status != XR_XIR_CALL_READY) {
        call_deallocate(config->accounting, call, bytes);
        return status;
    }
    *output = call;
    return XR_XIR_CALL_READY;
}

static void accept_action(XrXirCall *call, XrXirAction action) {
    if (action.kind == XR_XIR_ACTION_OUTPUT) {
        if ((action.callee != XR_XIR_STDOUT && action.callee != XR_XIR_STDERR) ||
            action.arguments || action.argument_count ||
            !xr_xir_value_argument(&action.value, (XrXirType) action.value.type)) {
            unwind(call, XR_XIR_CALL_BAD_STATE);
            return;
        }
        bool accepted = call->config.output.write && call->config.output.write(
            call->config.output.context, (XrXirOutputStream) action.callee, &action.value);
        if (call->cancel_requested) unwind(call, XR_XIR_CALL_CANCELLED);
        else if (!accepted) unwind(call, XR_XIR_CALL_OUTPUT_ERROR);
        return;
    }
    if (action.kind == XR_XIR_ACTION_CALL) {
        if (!boundary_value(action.value, XR_XIR_UNIT)) {
            unwind(call, XR_XIR_CALL_BAD_STATE);
            return;
        }
        CallFrame *parent = call->top;
        XrXirCallStatus status = push_frame(call, action.callee, action.arguments, action.argument_count);
        if (status != XR_XIR_CALL_READY)
            unwind(call, status);
        else {
            xr_xir_value_drop(&parent->inbox.value);
            parent->inbox = call_result(XR_XIR_CALL_READY);
        }
        return;
    }
    if (action.callee || action.arguments || action.argument_count) {
        unwind(call, XR_XIR_CALL_BAD_STATE);
        return;
    }
    if (action.kind == XR_XIR_ACTION_CONTINUE) {
        if (!boundary_value(action.value, XR_XIR_UNIT))
            unwind(call, XR_XIR_CALL_BAD_STATE);
        return;
    }
    if (action.kind == XR_XIR_ACTION_FAULT) {
        XrXirCallStatus reason = XR_XIR_CALL_BAD_STATE;
        if (boundary_value(action.value, XR_XIR_I64) &&
            (action.value.payload == XR_XIR_CALL_OVERFLOW || action.value.payload == XR_XIR_CALL_OOM ||
             action.value.payload == XR_XIR_CALL_LIMIT))
            reason = (XrXirCallStatus) action.value.payload;
        unwind(call, reason);
        return;
    }
    if (action.kind == XR_XIR_ACTION_SUSPEND) {
        if (!boundary_value(action.value, XR_XIR_UNIT) || call->next_wake == UINT64_MAX) {
            unwind(call, XR_XIR_CALL_BAD_STATE);
            return;
        }
        call->result = call_result(XR_XIR_CALL_SUSPENDED);
        call->result.wake = ++call->next_wake;
        return;
    }
    bool returning = action.kind == XR_XIR_ACTION_RETURN;
    if ((!returning && action.kind != XR_XIR_ACTION_THROW) ||
        !boundary_value(action.value, returning ? call->top->entry->result : XR_XIR_I64)) {
        unwind(call, XR_XIR_CALL_BAD_STATE);
        return;
    }
    XrXirCallResult result = call_result(returning ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_THROWN);
    if (xr_xir_value_copy(&action.value, &result.value) != XR_XIR_VALUE_OK) {
        unwind(call, XR_XIR_CALL_LIMIT);
        return;
    }
    pop_frame(call, result.status);
    if (call->top) {
        xr_xir_value_drop(&call->top->inbox.value);
        call->top->inbox = result;
    } else call->result = result;
}

XrXirCallResult xr_xir_call_poll(XrXirCall *call) {
    if (!call)
        return call_result(XR_XIR_CALL_BAD_ARGUMENT);
    if (call->driving)
        return call_result(XR_XIR_CALL_BUSY);
    if (call->result.status != XR_XIR_CALL_READY)
        return call->result;
    call->driving = true;
    while (call->top && call->result.status == XR_XIR_CALL_READY) {
        if (call->cancel_requested) {
            unwind(call, XR_XIR_CALL_CANCELLED);
            break;
        }
        if (!call->polls_left || call->config.accounting->polls == UINT64_MAX) {
            unwind(call, XR_XIR_CALL_LIMIT);
            break;
        }
        --call->polls_left;
        ++call->config.accounting->polls;
        XrXirCallView view = frame_view(call);
        XrXirAction action = call->top->entry->resume(&view);
        if (call->cancel_requested)
            unwind(call, XR_XIR_CALL_CANCELLED);
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

XrXirCallStatus xr_xir_call_cancel(XrXirCall *call) {
    if (!call)
        return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->cleaning)
        return XR_XIR_CALL_BUSY;
    if (call->result.status != XR_XIR_CALL_READY && call->result.status != XR_XIR_CALL_SUSPENDED)
        return XR_XIR_CALL_BAD_STATE;
    call->cancel_requested = true;
    if (!call->driving) {
        call->driving = true;
        unwind(call, XR_XIR_CALL_CANCELLED);
        call->driving = false;
    }
    return XR_XIR_CALL_CANCELLED;
}

XrXirCallStatus xr_xir_call_free(XrXirCall *call) {
    if (!call)
        return XR_XIR_CALL_READY;
    if (call->driving)
        return XR_XIR_CALL_BUSY;
    call->driving = true;
    if (call->top)
        unwind(call, XR_XIR_CALL_CANCELLED);
    xr_xir_value_drop(&call->result.value);
    call_deallocate(call->config.accounting, call, call->allocation_bytes);
    return XR_XIR_CALL_READY;
}
