/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance.c - One initialization publisher and isolated runtime slots
 *
 * KEY CONCEPT:
 *   Instances retain immutable descriptors until execution and cleanup finish.
 */


#include "xxir_program_internal.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"

typedef struct FunctionGate {
    _Atomic(uint32_t) references;
    XrXirProgram *program;
    XrXirInstance *instance;
} FunctionGate;
static void function_gate_drop(void *owner) {
    FunctionGate *gate = owner;
    uint32_t before = atomic_fetch_sub_explicit(&gate->references, 1, memory_order_acq_rel);
    XR_CHECK(before, "function gate reference underflow");
    if (before == 1) { xr_xir_program_drop(gate->program); xr_free(gate); }
}
static bool function_gate_retain(FunctionGate *gate) {
    uint32_t count = atomic_load_explicit(&gate->references, memory_order_relaxed);
    for (;;) {
        if (!count || count == UINT32_MAX) return false;
        if (atomic_compare_exchange_weak_explicit(&gate->references, &count, count + 1,
                memory_order_relaxed, memory_order_relaxed)) return true;
    }
}

struct XrXirInstance {
    XrXirProgram *program;
    XrXirInstanceConfig config;
    XrXirInstanceState state;
    XrXirDomain *domain;
    FunctionGate *function_gate;
    XrXirValue *slots;
    uint8_t *published, *ready;
    uint32_t *publication_order, publication_count, cursor, current_module, requested;
    uint32_t *published_counts;
    XrXirCallEntry *entries;
    XrXirCall *call;
    XrXirCallAccounting accounting;
    XrXirCallResult failure;
    uint64_t epoch, metadata_bytes;
    bool driving, stopping, observing;
};
static XrXirCallResult instance_result(XrXirCallStatus status) {
    return (XrXirCallResult) {status, {0, 0, 0}, 0};
}
static XrXirCallStatus value_call_status(XrXirValueStatus status) {
    if (status == XR_XIR_VALUE_OK) return XR_XIR_CALL_READY;
    if (status == XR_XIR_VALUE_OOM) return XR_XIR_CALL_OOM;
    if (status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_REFCOUNT_LIMIT) return XR_XIR_CALL_LIMIT;
    return XR_XIR_CALL_BAD_STATE;
}
static void instance_trace(XrXirInstance *instance, XrXirLifecycleEvent event, uint32_t index) {
    if (!instance->config.trace) return;
    instance->observing = true;
    instance->config.trace(instance->config.trace_context, event, index);
    instance->observing = false;
}
static void clear_slots(XrXirInstance *instance) {
    while (instance->publication_count) {
        uint32_t slot = instance->publication_order[--instance->publication_count];
        instance->published[slot] = 0;
        --instance->published_counts[instance->program->declarations->slots[slot].module];
        xr_xir_value_drop(&instance->slots[slot]);
        instance_trace(instance, XR_XIR_SLOT_RELEASED, slot);
    }
}
static void fail_initialization(XrXirInstance *instance, XrXirCallResult failure) {
    instance->failure = instance_result(failure.status);
    if (xr_xir_value_copy(&failure.value, &instance->failure.value) != XR_XIR_VALUE_OK)
        instance->failure = instance_result(XR_XIR_CALL_LIMIT);
    instance->state = XR_XIR_INSTANCE_FAILED;
    instance->current_module = UINT32_MAX;
    clear_slots(instance);
}
static XrXirAction initialization_resume(XrXirCallView *view) {
    XrXirInstance *instance = view->instance;
    const XrXirDeclarations *d = instance->program->declarations;
    uint32_t *phase = view->state;
    if (*phase) {
        if (view->inbox.status == XR_XIR_CALL_THROWN)
            return (XrXirAction) {XR_XIR_ACTION_THROW, 0, NULL, 0, view->inbox.value};
        if (view->inbox.status != XR_XIR_CALL_RETURNED)
            return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}};
        if (*phase == 2)
            return (XrXirAction) {XR_XIR_ACTION_RETURN, 0, NULL, 0, view->inbox.value};
        uint32_t module = instance->current_module;
        if (instance->published_counts[module] != instance->program->module_slots[module])
            return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}};
        instance->ready[module] = 1;
        instance_trace(instance, XR_XIR_MODULE_READY, module);
        ++instance->cursor;
        instance->current_module = UINT32_MAX;
    }
    if (instance->cursor < d->module_count) {
        instance->current_module = instance->program->order[instance->cursor];
        *phase = 1;
        instance_trace(instance, XR_XIR_MODULE_BEGIN, instance->current_module);
        return (XrXirAction) {XR_XIR_ACTION_CALL, d->modules[instance->current_module].initializer,
            NULL, 0, {0, 0, 0}};
    }
    instance->state = XR_XIR_INSTANCE_READY;
    *phase = 2;
    return (XrXirAction) {XR_XIR_ACTION_CALL, instance->requested, view->arguments,
        view->argument_count, {0, 0, 0}};
}
XrXirInstanceConfig xr_xir_instance_defaults(void) {
    return (XrXirInstanceConfig) {UINT64_C(16) << 20, UINT64_C(16) << 20,
        UINT64_C(16) << 20, UINT64_C(1000000), 4096, {NULL, NULL}, NULL, NULL};
}
static void instance_dispose(XrXirInstance *instance) {
    xr_free(instance->entries); xr_free(instance->slots); xr_free(instance->published);
    xr_free(instance->ready); xr_free(instance->publication_order);
    xr_free(instance->published_counts);
    if (instance->function_gate) {
        instance->function_gate->instance = NULL;
        function_gate_drop(instance->function_gate);
    }
    xr_xir_domain_drop(instance->domain);
    xr_xir_program_drop(instance->program);
    xr_free(instance);
}
XrXirCallStatus xr_xir_instance_new(XrXirProgram *program, const XrXirInstanceConfig *config,
                                    XrXirInstance **output) {
    if (!output) return XR_XIR_CALL_BAD_ARGUMENT;
    *output = NULL;
    if (!program || !config) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirDeclarations *d = program->declarations;
    uint64_t bytes = sizeof(XrXirInstance) + (uint64_t) d->slot_count * (sizeof(XrXirValue) + 5) +
        (uint64_t) d->module_count * 5 + ((uint64_t) program->entry_count + 1) * sizeof(XrXirCallEntry);
    if (bytes > config->metadata_limit || bytes > SIZE_MAX) return XR_XIR_CALL_LIMIT;
    if (!xr_xir_program_retain(program)) return XR_XIR_CALL_LIMIT;
    XrXirInstance *instance = xr_calloc(1, sizeof(*instance));
    if (!instance) { xr_xir_program_drop(program); return XR_XIR_CALL_OOM; }
    instance->program = program;
    instance->config = *config;
    instance->metadata_bytes = bytes;
    instance->current_module = UINT32_MAX;
    instance->entries = xr_calloc((size_t) program->entry_count + 1, sizeof(*instance->entries));
    instance->ready = xr_calloc(d->module_count, 1);
    instance->published_counts = xr_calloc(d->module_count, sizeof(*instance->published_counts));
    if (d->slot_count) {
        instance->slots = xr_calloc(d->slot_count, sizeof(*instance->slots));
        instance->published = xr_calloc(d->slot_count, 1);
        instance->publication_order = xr_calloc(d->slot_count, sizeof(*instance->publication_order));
    }
    if (!instance->entries || !instance->ready || !instance->published_counts ||
        (d->slot_count && (!instance->slots || !instance->published || !instance->publication_order))) {
        instance_dispose(instance);
        return XR_XIR_CALL_OOM;
    }
    XrXirCallStatus status = value_call_status(xr_xir_domain_new(config->value_limit, &instance->domain));
    if (status != XR_XIR_CALL_READY) { instance_dispose(instance); return status; }
    memcpy(instance->entries, program->entries, (size_t) program->entry_count * sizeof(*program->entries));
    *output = instance;
    return XR_XIR_CALL_READY;
}
XrXirInstanceState xr_xir_instance_state(const XrXirInstance *instance) {
    return !instance || instance->stopping ? XR_XIR_INSTANCE_DRAINING : instance->state;
}
static void drop_arguments(XrXirValue *arguments, uint32_t count) {
    if (arguments) for (uint32_t p = 0; p < count; ++p) xr_xir_value_drop(&arguments[p]);
    xr_free(arguments);
}
static XrXirCallStatus capture_arguments(XrXirInstance *instance, const XrXirValue *arguments,
                                        uint32_t count, const XrXirFunctionBinding *binding, XrXirValue **output) {
    *output = NULL;
    uint32_t captures = binding ? binding->capture_count : 0;
    count += captures;
    if (!count) return XR_XIR_CALL_READY;
    uint64_t bytes = (uint64_t) count * sizeof(XrXirValue);
    if (bytes > SIZE_MAX || bytes > instance->config.metadata_limit - instance->metadata_bytes)
        return XR_XIR_CALL_LIMIT;
    XrXirValue *owned = xr_calloc(count, sizeof(*owned));
    if (!owned) return XR_XIR_CALL_OOM;
    for (uint32_t p = 0; p < count; ++p) {
        XrXirCallStatus status = value_call_status(xr_xir_value_copy(p < captures ? &binding->captures[p] : &arguments[p - captures], &owned[p]));
        if (status != XR_XIR_CALL_READY) { drop_arguments(owned, count); return status; }
    }
    *output = owned;
    return XR_XIR_CALL_READY;
}
static XrXirCallStatus resolve_function(XrXirInstance *instance, const XrXirValue *value, uint32_t *entry);
static XrXirCallStatus instance_start(XrXirInstance *instance, uint32_t entry,
                                      const XrXirValue *arguments, uint32_t count, bool public_entry, const XrXirFunctionBinding *binding) {
    if (!instance) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance->driving || instance->observing) return XR_XIR_CALL_BUSY;
    if (instance->state == XR_XIR_INSTANCE_FAILED) return instance->failure.status;
    if (instance->stopping) return XR_XIR_CALL_BAD_STATE;
    if (instance->call) {
        XrXirCallStatus state = xr_xir_call_state(instance->call);
        if (state == XR_XIR_CALL_READY || state == XR_XIR_CALL_SUSPENDED) return XR_XIR_CALL_BUSY;
    }
    const XrXirDeclarations *d = instance->program->declarations;
    if (entry >= instance->program->entry_count ||
        (public_entry && entry != d->entry_function && !d->functions[entry].exported)) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirCallEntry *requested = &instance->program->entries[entry];
    uint32_t captures = binding ? binding->capture_count : 0;
    if (captures > requested->parameter_count || count != requested->parameter_count - captures ||
        (count && !arguments)) return XR_XIR_CALL_BAD_ARGUMENT;
    for (uint32_t p = 0; p < requested->parameter_count; ++p) {
        const XrXirValue *value = p < captures ? &binding->captures[p] : &arguments[p - captures];
        if (!xr_xir_value_argument(value, requested->parameters[p])) return XR_XIR_CALL_BAD_ARGUMENT;
        uint32_t target;
        if (xr_xir_type_is_callable(requested->parameters[p]) &&
            resolve_function(instance, value, &target) != XR_XIR_CALL_READY) return XR_XIR_CALL_BAD_ARGUMENT;
    }
    if (instance->epoch == UINT64_MAX) return XR_XIR_CALL_LIMIT;
    XrXirValue *owned = NULL;
    XrXirCallStatus status = capture_arguments(instance, arguments, count, binding, &owned);
    if (status != XR_XIR_CALL_READY) return status;
    count += captures;
    xr_xir_call_free(instance->call);
    instance->call = NULL;
    ++instance->epoch;
    instance->requested = entry;
    if (instance->state == XR_XIR_INSTANCE_NEW) instance->state = XR_XIR_INSTANCE_INITIALIZING;
    uint32_t root = instance->program->entry_count;
    instance->entries[root] = (XrXirCallEntry) {XR_XIR_CALL_ABI_VERSION, requested->parameters,
        count, requested->result, sizeof(uint32_t), initialization_resume, NULL, NULL};
    XrXirCallConfig config = {instance->entries, root + 1, instance, instance->config.call_limit,
        instance->config.poll_limit, instance->config.depth_limit, &instance->accounting, instance->config.output};
    status = xr_xir_call_new(&config, root, owned, count, &instance->call);
    drop_arguments(owned, count);
    if (status != XR_XIR_CALL_READY && instance->state == XR_XIR_INSTANCE_INITIALIZING)
        fail_initialization(instance, instance_result(status));
    return status;
}
XrXirInstanceResult xr_xir_instance_poll(XrXirInstance *instance) {
    if (!instance) return (XrXirInstanceResult) {instance_result(XR_XIR_CALL_BAD_ARGUMENT), 0};
    if (instance->driving || instance->observing)
        return (XrXirInstanceResult) {instance_result(XR_XIR_CALL_BUSY), instance->epoch};
    if (instance->state == XR_XIR_INSTANCE_FAILED)
        return (XrXirInstanceResult) {instance->failure, instance->epoch};
    if (!instance->call) return (XrXirInstanceResult) {instance_result(XR_XIR_CALL_BAD_STATE), instance->epoch};
    instance->driving = true;
    XrXirCallResult result = xr_xir_call_poll(instance->call);
    if (instance->state == XR_XIR_INSTANCE_INITIALIZING &&
        result.status != XR_XIR_CALL_READY && result.status != XR_XIR_CALL_SUSPENDED)
        fail_initialization(instance, result);
    instance->driving = false;
    return (XrXirInstanceResult) {result, instance->epoch};
}
XrXirCallStatus xr_xir_instance_resume(XrXirInstance *instance, uint64_t epoch, uint64_t wake) {
    if (!instance) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance->driving || instance->observing) return XR_XIR_CALL_BUSY;
    if (instance->stopping || !instance->call || epoch != instance->epoch) return XR_XIR_CALL_BAD_STATE;
    return xr_xir_call_resume(instance->call, wake);
}
XrXirCallStatus xr_xir_instance_take_result(XrXirInstance *instance, XrXirValue *output) {
    if (!instance || !output) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance->driving || instance->observing) return XR_XIR_CALL_BUSY;
    if (!instance->call) return XR_XIR_CALL_BAD_STATE;
    return xr_xir_call_take_result(instance->call, output);
}
XrXirCallStatus xr_xir_instance_copy_failure(XrXirInstance *instance, XrXirValue *output) {
    if (!instance || !output) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance->driving || instance->observing) return XR_XIR_CALL_BUSY;
    if (instance->state != XR_XIR_INSTANCE_FAILED) return XR_XIR_CALL_BAD_STATE;
    XrXirCallStatus status = value_call_status(xr_xir_value_copy(&instance->failure.value, output));
    return status == XR_XIR_CALL_READY ? instance->failure.status : status;
}
XrXirCallStatus xr_xir_instance_stop(XrXirInstance *instance) {
    if (!instance) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance->observing) return XR_XIR_CALL_BUSY;
    instance->stopping = true;
    if (instance->function_gate) instance->function_gate->instance = NULL;
    if (instance->call) xr_xir_call_cancel(instance->call);
    return XR_XIR_CALL_READY;
}
XrXirCallStatus xr_xir_instance_free(XrXirInstance *instance) {
    if (!instance) return XR_XIR_CALL_READY;
    if (instance->driving || instance->observing) return XR_XIR_CALL_BUSY;
    instance->driving = true;
    instance->stopping = true;
    if (instance->function_gate) instance->function_gate->instance = NULL;
    xr_xir_call_free(instance->call);
    clear_slots(instance);
    xr_xir_value_drop(&instance->failure.value);
    instance_dispose(instance);
    return XR_XIR_CALL_READY;
}
static XrXirInstance *view_instance(XrXirCallView *view) {
    if (!view || !view->instance) return NULL;
    XrXirInstance *instance = view->instance;
    if (!instance->driving || instance->observing || view->activation != instance->call ||
        xr_xir_call_current_entry(view->activation) >= instance->program->entry_count) return NULL;
    return instance;
}
XrXirCallStatus xr_xir_instance_literal(XrXirCallView *view, uint32_t literal, XrXirValue *output) {
    XrXirInstance *instance = view_instance(view);
    if (!instance || literal >= instance->program->declarations->literal_count) return XR_XIR_CALL_BAD_STATE;
    const XrXirLiteral *bytes = &instance->program->declarations->literals[literal];
    return value_call_status(xr_xir_string_new(instance->domain, bytes->bytes, bytes->length, output));
}
XrXirCallStatus xr_xir_instance_atomic(XrXirCallView *view, int64_t initial, XrXirValue *output) {
    XrXirInstance *instance = view_instance(view);
    return instance ? value_call_status(xr_xir_atomic_i64_new(instance->domain, initial, output)) : XR_XIR_CALL_BAD_STATE;
}
XrXirCallStatus xr_xir_instance_slot_read(XrXirCallView *view, uint32_t slot, XrXirValue *output) {
    XrXirInstance *instance = view_instance(view);
    if (!instance || slot >= instance->program->declarations->slot_count) return XR_XIR_CALL_BAD_STATE;
    const XrXirDeclarations *d = instance->program->declarations;
    uint32_t function = xr_xir_call_current_entry(view->activation), module = d->slots[slot].module;
    if (!instance->published[slot] || d->functions[function].module != module ||
        (!instance->ready[module] && instance->current_module != module)) return XR_XIR_CALL_BAD_STATE;
    return value_call_status(xr_xir_value_copy(&instance->slots[slot], output));
}
XrXirCallStatus xr_xir_instance_slot_write(XrXirCallView *view, uint32_t slot,
                                          const XrXirValue *value, bool publish) {
    XrXirInstance *instance = view_instance(view);
    if (!instance || slot >= instance->program->declarations->slot_count) return XR_XIR_CALL_BAD_STATE;
    const XrXirDeclarations *d = instance->program->declarations;
    uint32_t function = xr_xir_call_current_entry(view->activation), module = d->slots[slot].module;
    if (d->functions[function].module != module || !xr_xir_value_argument(value, d->slots[slot].type))
        return XR_XIR_CALL_BAD_STATE;
    if (publish ? (instance->published[slot] || instance->current_module != module ||
                   d->modules[module].initializer != function) :
                  (!instance->published[slot] || !d->slots[slot].mutable || module != d->root_module))
        return XR_XIR_CALL_BAD_STATE;
    XrXirValue owned = {0};
    XrXirCallStatus status = value_call_status(xr_xir_value_copy(value, &owned));
    if (status != XR_XIR_CALL_READY) return status;
    xr_xir_value_drop(&instance->slots[slot]);
    instance->slots[slot] = owned;
    if (publish) {
        instance->published[slot] = 1;
        ++instance->published_counts[module];
        instance->publication_order[instance->publication_count++] = slot;
        instance_trace(instance, XR_XIR_SLOT_PUBLISHED, slot);
    }
    return XR_XIR_CALL_READY;
}

XrXirCallStatus xr_xir_instance_start(XrXirInstance *instance, uint32_t entry,
    const XrXirValue *arguments, uint32_t count) {
    return instance_start(instance, entry, arguments, count, true, NULL);
}
static XrXirCallStatus resolve_function(XrXirInstance *instance, const XrXirValue *value, uint32_t *entry) {
    if (!instance || !entry) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirFunctionBinding *binding = xr_xir_function_binding(value);
    if (!binding || binding->release != function_gate_drop) return XR_XIR_CALL_BAD_ARGUMENT;
    FunctionGate *gate = binding->owner;
    if (!gate->instance || instance->stopping) return XR_XIR_CALL_BAD_STATE;
    if (gate->instance != instance || gate->program != instance->program ||
        binding->entry >= instance->program->entry_count) return XR_XIR_CALL_BAD_ARGUMENT;
    *entry = binding->entry; return XR_XIR_CALL_READY;
}
XrXirCallStatus xr_xir_instance_start_function(XrXirInstance *instance, const XrXirValue *function,
    const XrXirValue *arguments, uint32_t count) {
    uint32_t entry = 0;
    XrXirCallStatus status = resolve_function(instance, function, &entry);
    return status == XR_XIR_CALL_READY ? instance_start(instance, entry, arguments, count, false, xr_xir_function_binding(function)) : status;
}
XrXirCallStatus xr_xir_instance_resolve_function(XrXirCallView *view, const XrXirValue *function, uint32_t *entry) {
    XrXirInstance *instance = view_instance(view);
    return instance ? resolve_function(instance, function, entry) : XR_XIR_CALL_BAD_STATE;
}
static XrXirCallStatus prepare_function_gate(XrXirInstance *instance) {
    if (instance->function_gate) return XR_XIR_CALL_READY;
    if (sizeof(FunctionGate) > instance->config.metadata_limit - instance->metadata_bytes)
        return XR_XIR_CALL_LIMIT;
    FunctionGate *gate = xr_malloc(sizeof(*gate));
    if (!gate) return XR_XIR_CALL_OOM;
    if (!xr_xir_program_retain(instance->program)) { xr_free(gate); return XR_XIR_CALL_LIMIT; }
    atomic_init(&gate->references, 1);
    gate->program = instance->program; gate->instance = instance;
    instance->function_gate = gate; instance->metadata_bytes += sizeof(*gate);
    return XR_XIR_CALL_READY;
}
XrXirCallStatus xr_xir_instance_function(XrXirCallView *view, XrXirType type, uint32_t entry,
    const XrXirValue *captures, uint32_t count, XrXirValue *output) {
    XrXirInstance *instance = view_instance(view);
    if (!instance || instance->stopping) return XR_XIR_CALL_BAD_STATE;
    const XrXirCallableSignature *signature = xr_xir_callable_signature(instance->program->callables, type);
    if (!signature || entry >= instance->program->entry_count || (count && !captures)) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirCallEntry *target = &instance->program->entries[entry];
    const XrXirDeclarations *d = instance->program->declarations;
    uint32_t caller = xr_xir_call_current_entry(view->activation);
    uint32_t from = d->functions[caller].module, to = d->functions[entry].module;
    if (entry == d->modules[to].initializer || !xr_xir_module_imports(d, from, to) ||
        (from != to && !d->functions[entry].exported) || count > target->parameter_count || target->parameter_count - count != signature->parameter_count ||
        target->result != signature->result) return XR_XIR_CALL_BAD_ARGUMENT;
    for (uint32_t p = 0; p < count; ++p) {
        if (!xr_xir_value_argument(&captures[p], target->parameters[p])) return XR_XIR_CALL_BAD_ARGUMENT;
        uint32_t nested;
        if (xr_xir_type_is_callable(target->parameters[p]) &&
            resolve_function(instance, &captures[p], &nested) != XR_XIR_CALL_READY) return XR_XIR_CALL_BAD_ARGUMENT;
    }
    for (uint32_t p = 0; p < signature->parameter_count; ++p)
        if (target->parameters[p + count] != signature->parameters[p].type) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallStatus status = prepare_function_gate(instance);
    if (status != XR_XIR_CALL_READY) return status;
    if (!function_gate_retain(instance->function_gate)) return XR_XIR_CALL_LIMIT;
    XrXirFunctionBinding binding = {instance->function_gate, function_gate_drop, entry, captures, count};
    status = value_call_status(xr_xir_function_new(instance->domain, type, &binding, output));
    if (status != XR_XIR_CALL_READY) function_gate_drop(instance->function_gate);
    return status;
}
