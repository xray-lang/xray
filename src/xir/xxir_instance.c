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
#include "xxir_instance_value.h"
#include "xxir_value_internal.h"
#include "xxir_call_internal.h"
#include "xxir_task_internal.h"
#include "xxir_struct.h"
#include "xxir_output.h"
#include "xxir_panic.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"

typedef struct FunctionGate {
    _Atomic(uint32_t) references;
    XrXirProgram *program;
    XrXirInstance *instance;
    XrXirDomain *domain;
} FunctionGate;
static void function_gate_drop(void *owner) {
    FunctionGate *gate = owner;
    uint32_t before = atomic_fetch_sub_explicit(&gate->references, 1, memory_order_acq_rel);
    XR_CHECK(before, "function gate reference underflow");
    if (before == 1) {
        XrXirDomain *domain = gate->domain;
        xr_xir_compile_program_drop(gate->program);
        xr_xir_domain_metadata_deallocate(domain, gate, sizeof(*gate));
        xr_xir_domain_drop(domain);
    }
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
    XrXirTaskExecutor *executor;
    XrXirCallAccounting accounting[2];
    XrXirCallBudget budget;
    XrXirCallResult failure;
    uint64_t epoch, metadata_bytes;
    bool driving, stopping, observing;
};
static XrXirCallResult instance_result(XrXirCallStatus status) {
    return (XrXirCallResult) {status, {0, 0, 0}, 0, {0}};
}
static XrXirCallStatus value_call_status(XrXirValueStatus status) {
    if (status == XR_XIR_VALUE_OK) return XR_XIR_CALL_READY;
    if (status == XR_XIR_VALUE_BOUNDS) return XR_XIR_CALL_BOUNDS;
    if (status == XR_XIR_VALUE_OOM) return XR_XIR_CALL_OOM;
    if (status == XR_XIR_VALUE_UNSUPPORTED) return XR_XIR_CALL_UNSUPPORTED;
    if (status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_REFCOUNT_LIMIT) return XR_XIR_CALL_LIMIT;
    return XR_XIR_CALL_BAD_STATE;
}
static bool instance_cleanup_grant(const XrXirInstance *instance) {
    return instance->driving && !instance->observing && xr_xir_task_executor_cleanup_active(instance->executor);
}
static XrXirValueStatus admit_function_binding(void *context, const XrXirFunctionBinding *binding,
    XrXirType type, uint64_t *work) {
    XrXirInstance *instance = context;
    if (!instance || (instance->stopping && !instance_cleanup_grant(instance)) || !binding || binding->release != function_gate_drop ||
        !binding->owner || binding->owner != instance->function_gate || !work) return XR_XIR_VALUE_BAD_ARGUMENT;
    const FunctionGate *gate = binding->owner;
    if ((gate->instance != instance && !(instance->stopping && !gate->instance && instance_cleanup_grant(instance))) ||
        gate->program != instance->program ||
        binding->entry >= instance->program->entry_count) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!instance->program->active_modules[instance->program->declarations->functions[binding->entry].module])
        return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypeNode *signature = xr_xir_callable_signature(instance->program->types, type);
    const XrXirCallEntry *entry = &instance->program->entries[binding->entry];
    if (signature && (signature->flags & XR_XIR_CALLABLE_NO_SUSPEND) &&
        !(instance->program->declarations->functions[binding->entry].promises & XR_XIR_FUNCTION_NO_SUSPEND))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!signature || entry->cleanup_owner || binding->capture_count > entry->parameter_count ||
        signature->parameter_count != entry->parameter_count - binding->capture_count ||
        signature->result != entry->result || (binding->capture_count && !binding->captures))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    uint64_t cost = (uint64_t) entry->parameter_count + 1;
    if (cost > *work) return XR_XIR_VALUE_LIMIT;
    *work -= cost;
    for (uint32_t i = 0; i < binding->capture_count; ++i)
        if (!xr_xir_value_argument(&binding->captures[i], instance->program->arena, entry->parameters[i]))
            return XR_XIR_VALUE_BAD_ARGUMENT;
    for (uint32_t i = 0; i < signature->parameter_count; ++i)
        if (entry->parameters[i + binding->capture_count] != signature->parameters[i].type)
            return XR_XIR_VALUE_BAD_ARGUMENT;
    return XR_XIR_VALUE_OK;
}
static XrXirValueAdmission instance_candidate_admission(XrXirInstance *instance) {
    return (XrXirValueAdmission) {instance->program->arena, instance->domain,
        admit_function_binding, instance, instance->config.poll_limit, instance->config.metadata_limit};
}
static XrXirCallStatus admit_instance_value(XrXirValueAdmission *admission,
                                           const XrXirValue *value, XrXirType type) {
    XrXirValueStatus status = xr_xir_value_admit(value, type, admission);
    return status == XR_XIR_VALUE_BAD_ARGUMENT ? XR_XIR_CALL_BAD_ARGUMENT : value_call_status(status);
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
    XrXirCallResult copy = instance_result(XR_XIR_CALL_READY);
    XrXirValueStatus status = xr_xir_call_result_copy(&failure, &copy);
    if (status != XR_XIR_VALUE_OK) copy = instance_result(value_call_status(status));
    xr_xir_call_result_drop(&instance->failure);
    xr_xir_call_result_move(&copy, &instance->failure);
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
            return (XrXirAction) {XR_XIR_ACTION_THROW, 0, NULL, 0, view->inbox.value, {0}, 0};
        if (view->inbox.status != XR_XIR_CALL_RETURNED)
            return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}, {0}, 0};
        if (*phase == 2)
            return (XrXirAction) {XR_XIR_ACTION_RETURN, 0, NULL, 0, view->inbox.value, {0}, 0};
        uint32_t module = instance->current_module;
        if (instance->published_counts[module] != instance->program->module_slots[module])
            return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}, {0}, 0};
        instance->ready[module] = 1;
        instance_trace(instance, XR_XIR_MODULE_READY, module);
        ++instance->cursor;
        instance->current_module = UINT32_MAX;
    }
    if (instance->cursor < instance->program->initialization_count) {
        instance->current_module = instance->program->order[instance->cursor];
        *phase = 1;
        instance_trace(instance, XR_XIR_MODULE_BEGIN, instance->current_module);
        return (XrXirAction) {XR_XIR_ACTION_CALL, d->modules[instance->current_module].initializer,
            NULL, 0, {0, 0, 0}, {0}, 0};
    }
    instance->state = XR_XIR_INSTANCE_READY;
    *phase = 2;
    return (XrXirAction) {XR_XIR_ACTION_CALL, instance->requested, view->arguments,
        view->argument_count, {0, 0, 0}, {0}, 0};
}
XrXirCallStatus xr_xir_instance_config_init(XrXirInstanceConfig *config, size_t size) {
    if (!config) return XR_XIR_CALL_BAD_ARGUMENT;
    if (size != sizeof(*config)) return XR_XIR_CALL_BAD_ABI;
    *config = (XrXirInstanceConfig) {XR_XIR_CALL_ABI_VERSION, sizeof(*config),
        UINT64_C(16) << 20, UINT64_C(16) << 20, UINT64_C(16) << 20,
        UINT64_C(1000000), 4096, {0}, NULL, NULL, {0},
        UINT64_C(67108864), UINT64_C(67108864), UINT64_C(128000000)};
    return XR_XIR_CALL_READY;
}
static void instance_dispose(XrXirInstance *instance) {
    XrXirDomain *domain = instance->domain;
    XrXirProgram *program = instance->program;
    const XrXirDeclarations *d = program->declarations;
    XR_CHECK(xr_xir_task_executor_free(instance->executor, NULL) == XR_XIR_CALL_READY,
        "unpublished or already drained instance executor has no incomplete cleanup");
    if (instance->entries) xr_xir_domain_metadata_deallocate(domain, instance->entries,
        ((uint64_t)program->entry_count + 1) * sizeof(*instance->entries));
    if (instance->slots) xr_xir_domain_metadata_deallocate(domain, instance->slots,
        (uint64_t)d->slot_count * sizeof(*instance->slots));
    if (instance->published) xr_xir_domain_metadata_deallocate(domain, instance->published, d->slot_count);
    if (instance->ready) xr_xir_domain_metadata_deallocate(domain, instance->ready, d->module_count);
    if (instance->publication_order) xr_xir_domain_metadata_deallocate(domain, instance->publication_order,
        (uint64_t)d->slot_count * sizeof(*instance->publication_order));
    if (instance->published_counts) xr_xir_domain_metadata_deallocate(domain, instance->published_counts,
        (uint64_t)d->module_count * sizeof(*instance->published_counts));
    if (instance->function_gate) {
        instance->function_gate->instance = NULL;
        function_gate_drop(instance->function_gate);
    }
    xr_xir_domain_metadata_deallocate(domain, instance, sizeof(*instance));
    xr_xir_compile_program_drop(program);
    xr_xir_domain_drop(domain);
}
static void *instance_metadata_allocate(XrXirDomain *domain, uint64_t bytes, XrXirCallStatus *status) {
    XrXirValueStatus value_status = XR_XIR_VALUE_OK;
    void *memory = xr_xir_domain_metadata_allocate(domain, bytes, &value_status);
    if (!memory) *status = value_call_status(value_status);
    return memory;
}
XrXirCallStatus xr_xir_instance_new(XrXirProgram *program, const XrXirInstanceConfig *config,
                                    XrXirInstance **output) {
    if (!output) return XR_XIR_CALL_BAD_ARGUMENT;
    *output = NULL;
    if (!program || !config) return XR_XIR_CALL_BAD_ARGUMENT;
    if (config->abi_version != XR_XIR_CALL_ABI_VERSION || config->struct_size != sizeof(*config))
        return XR_XIR_CALL_BAD_ABI;
    if (!xr_xir_output_provider_valid(&config->output) || !xr_xir_time_provider_valid(&config->time))
        return XR_XIR_CALL_BAD_ABI;
    if (!config->requested_value_limit || !config->requested_call_limit || !config->work_limit)
        return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirDeclarations *d = program->declarations;
    uint64_t bytes = sizeof(XrXirInstance) + (uint64_t) d->slot_count * (sizeof(XrXirValue) + 5) +
        (uint64_t) d->module_count * 5 + ((uint64_t) program->entry_count + 1) * sizeof(XrXirCallEntry);
    if (bytes > config->metadata_limit || bytes > SIZE_MAX) return XR_XIR_CALL_LIMIT;
    if (!xr_xir_compile_program_retain(program)) return XR_XIR_CALL_LIMIT;
    XrXirDomain *domain = NULL;
    XrXirDomainBudgetControls controls = {config->requested_value_limit, config->requested_call_limit,
        config->work_limit, config->metadata_limit, config->call_limit};
    XrXirCallStatus status = value_call_status(xr_xir_domain_new_budgeted(config->value_limit, &controls, &domain));
    if (status != XR_XIR_CALL_READY) { xr_xir_compile_program_drop(program); return status; }
    if (!xr_xir_domain_work(domain, (uint64_t)program->entry_count + d->module_count + d->slot_count)) {
        xr_xir_domain_drop(domain); xr_xir_compile_program_drop(program); return XR_XIR_CALL_LIMIT;
    }
    XrXirInstance *instance = instance_metadata_allocate(domain, sizeof(*instance), &status);
    if (!instance) { xr_xir_domain_drop(domain); xr_xir_compile_program_drop(program); return status; }
    instance->program = program;
    instance->domain = domain;
    instance->config = *config;
    instance->metadata_bytes = bytes;
    instance->current_module = UINT32_MAX;
    instance->budget = (XrXirCallBudget){.byte_limit = config->call_limit,
        .requested_limit = config->requested_call_limit, .resume_limit = config->poll_limit, .work_domain = domain};
    instance->entries = instance_metadata_allocate(domain,
        ((uint64_t)program->entry_count + 1) * sizeof(*instance->entries), &status);
    if (status == XR_XIR_CALL_READY) instance->ready = instance_metadata_allocate(domain, d->module_count, &status);
    if (status == XR_XIR_CALL_READY) instance->published_counts = instance_metadata_allocate(domain,
        (uint64_t)d->module_count * sizeof(*instance->published_counts), &status);
    if (d->slot_count && status == XR_XIR_CALL_READY) {
        instance->slots = instance_metadata_allocate(domain, (uint64_t)d->slot_count * sizeof(*instance->slots), &status);
        if (status == XR_XIR_CALL_READY) instance->published = instance_metadata_allocate(domain, d->slot_count, &status);
        if (status == XR_XIR_CALL_READY) instance->publication_order = instance_metadata_allocate(domain,
            (uint64_t)d->slot_count * sizeof(*instance->publication_order), &status);
    }
    if (!instance->entries || !instance->ready || !instance->published_counts ||
        (d->slot_count && (!instance->slots || !instance->published || !instance->publication_order))) {
        instance_dispose(instance);
        return status;
    }
    memcpy(instance->entries, program->entries, (size_t) program->entry_count * sizeof(*program->entries));
    XrXirTaskExecutorConfig execution = {program, domain, program->arena,
        config->call_limit, config->poll_limit, config->requested_value_limit,
        config->requested_call_limit, config->work_limit, config->depth_limit,
        &instance->budget, instance, instance->entries, program->entry_count + 1,
        instance_candidate_admission(instance), config->output};
    status = xr_xir_task_executor_new(&execution, &instance->executor);
    if (status != XR_XIR_CALL_READY) { instance_dispose(instance); return status; }
    instance->metadata_bytes = xr_xir_domain_budget_stats(domain).metadata_live;
    *output = instance;
    return XR_XIR_CALL_READY;
}
XrXirInstanceState xr_xir_instance_state(const XrXirInstance *instance) {
    return !instance || instance->stopping ? XR_XIR_INSTANCE_DRAINING : instance->state;
}
static void drop_arguments(XrXirDomain *domain, XrXirValue *arguments, uint32_t count) {
    if (arguments) for (uint32_t p = 0; p < count; ++p) xr_xir_value_drop(&arguments[p]);
    if (arguments) xr_xir_domain_metadata_deallocate(domain, arguments, (uint64_t)count * sizeof(*arguments));
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
    if (!xr_xir_domain_work(instance->domain, count)) return XR_XIR_CALL_LIMIT;
    XrXirCallStatus allocation_status = XR_XIR_CALL_READY;
    XrXirValue *owned = instance_metadata_allocate(instance->domain, bytes, &allocation_status);
    if (!owned) return allocation_status;
    for (uint32_t p = 0; p < count; ++p) {
        XrXirCallStatus status = value_call_status(xr_xir_value_copy(p < captures ? &binding->captures[p] : &arguments[p - captures], &owned[p]));
        if (status != XR_XIR_CALL_READY) { drop_arguments(instance->domain, owned, count); return status; }
    }
    *output = owned;
    return XR_XIR_CALL_READY;
}
static XrXirCallStatus instance_start(XrXirInstance *instance, uint32_t entry,
    const XrXirValue *arguments, uint32_t count, const XrXirFunctionBinding *binding,
    XrXirValueAdmission *admission, bool test_entry) {
    if (!instance) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance->driving || instance->observing) return XR_XIR_CALL_BUSY;
    if (instance->state == XR_XIR_INSTANCE_FAILED) return instance->failure.status;
    if (instance->stopping) return XR_XIR_CALL_BAD_STATE;
    if (!xr_xir_task_executor_root_idle(instance->executor)) return XR_XIR_CALL_BUSY;
    if (instance->call) {
        XrXirCallStatus state = xr_xir_call_state(instance->call);
        if (state == XR_XIR_CALL_READY || state == XR_XIR_CALL_SUSPENDED) return XR_XIR_CALL_BUSY;
    }
    const XrXirDeclarations *d = instance->program->declarations;
    if (entry >= instance->program->entry_count ||
        !instance->program->active_modules[d->functions[entry].module] ||
        (!test_entry && !binding && entry != d->entry_function && !d->functions[entry].exported)) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirCallEntry *requested = &instance->program->entries[entry];
    if (test_entry) {
        const XrXirFunctionIdentity *identity = &d->functions[entry];
        if (identity->module != d->root_module || identity->test_role == XR_XIR_TEST_ROLE_NONE ||
            identity->test_role == XR_XIR_TEST_ROLE_SKIP || identity->test_role > XR_XIR_TEST_ROLE_AFTER_EACH ||
            identity->nominal_owner || entry == d->entry_function ||
            entry == d->modules[d->root_module].initializer || requested->parameter_count ||
            requested->result != XR_XIR_UNIT) return XR_XIR_CALL_BAD_ARGUMENT;
    }
    if (requested->cleanup_owner) return XR_XIR_CALL_BAD_ARGUMENT;
    uint32_t captures = binding ? binding->capture_count : 0;
    if (captures > requested->parameter_count || count != requested->parameter_count - captures ||
        (count && !arguments)) return XR_XIR_CALL_BAD_ARGUMENT;
    for (uint32_t p = 0; p < requested->parameter_count; ++p) {
        const XrXirValue *value = p < captures ? &binding->captures[p] : &arguments[p - captures];
        XrXirCallStatus admitted = admit_instance_value(admission, value, requested->parameters[p]);
        if (admitted != XR_XIR_CALL_READY) return admitted;
    }
    if (instance->epoch == UINT64_MAX) return XR_XIR_CALL_LIMIT;
    XrXirValue *owned = NULL;
    XrXirCallStatus status = capture_arguments(instance, arguments, count, binding, &owned);
    if (status != XR_XIR_CALL_READY) return status;
    count += captures;
    uint32_t root = instance->program->entry_count;
    instance->entries[root] = (XrXirCallEntry) {XR_XIR_CALL_ABI_VERSION, requested->parameters,
        count, requested->result, sizeof(uint32_t), initialization_resume, NULL, NULL, 0, 0};
    XrXirCallConfig config;
    status = xr_xir_call_config_init(&config, sizeof(config));
    if (status != XR_XIR_CALL_READY) { drop_arguments(instance->domain, owned, count); return status; }
    config.entries = instance->entries; config.entry_count = root + 1; config.instance = instance;
    config.byte_limit = instance->config.call_limit; config.poll_limit = instance->config.poll_limit;
    config.depth_limit = instance->config.depth_limit;
    config.accounting = &instance->accounting[(instance->epoch + 1) % 2];
    config.output = instance->config.output; config.admission = *admission;
    XrXirCall *replacement = NULL;
    XrXirCallRequest request = {root, owned, count};
    status = xr_xir_call_new_budgeted(&config, &request, &instance->budget, &replacement);
    drop_arguments(instance->domain, owned, count);
    if (status != XR_XIR_CALL_READY) return status;
    status = xr_xir_task_executor_attach_root(instance->executor, replacement, instance->epoch + 1);
    if (status != XR_XIR_CALL_READY) { xr_xir_call_free(replacement); return status; }
    xr_xir_call_free(instance->call);
    instance->call = replacement;
    ++instance->epoch;
    instance->requested = entry;
    if (instance->state == XR_XIR_INSTANCE_NEW) instance->state = XR_XIR_INSTANCE_INITIALIZING;
    return status;
}
XrXirInstanceResult xr_xir_instance_poll_bounded(XrXirInstance *instance, uint64_t quantum) {
    if (!instance || !quantum) return (XrXirInstanceResult) {instance_result(XR_XIR_CALL_BAD_ARGUMENT), 0};
    if (instance->driving || instance->observing)
        return (XrXirInstanceResult) {instance_result(XR_XIR_CALL_BUSY), instance->epoch};
    if (instance->state == XR_XIR_INSTANCE_FAILED)
        return (XrXirInstanceResult) {instance->failure, instance->epoch};
    if (!instance->call) return (XrXirInstanceResult) {instance_result(XR_XIR_CALL_BAD_STATE), instance->epoch};
    instance->driving = true;
    XrXirCallResult result = xr_xir_task_executor_poll_root(instance->executor, quantum);
    if (instance->state == XR_XIR_INSTANCE_INITIALIZING &&
        result.status != XR_XIR_CALL_READY && result.status != XR_XIR_CALL_SUSPENDED) {
        fail_initialization(instance, result);
        result = instance->failure;
    }
    instance->driving = false;
    return (XrXirInstanceResult) {result, instance->epoch};
}
XrXirCallStatus xr_xir_instance_resume(XrXirInstance *instance, uint64_t epoch, uint64_t wake) {
    if (!instance) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance->driving || instance->observing) return XR_XIR_CALL_BUSY;
    if (instance->stopping || !instance->call || epoch != instance->epoch) return XR_XIR_CALL_BAD_STATE;
    return xr_xir_task_executor_resume_root(instance->executor, epoch, wake);
}
XrXirCallStatus xr_xir_instance_wait_request(const XrXirInstance *instance, uint64_t epoch,
    uint64_t wake, XrXirWaitRequest *output) {
    if (!instance || !output) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance->driving || instance->observing) return XR_XIR_CALL_BUSY;
    if (instance->stopping || !instance->call || epoch != instance->epoch) return XR_XIR_CALL_BAD_STATE;
    return xr_xir_task_executor_root_wait(instance->executor, epoch, wake, output);
}
XrXirCallStatus xr_xir_instance_take_result(XrXirInstance *instance, XrXirValue *output) {
    if (!instance || !output) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance->driving || instance->observing) return XR_XIR_CALL_BUSY;
    if (instance->state == XR_XIR_INSTANCE_FAILED) return XR_XIR_CALL_BAD_STATE;
    if (!instance->call) return XR_XIR_CALL_BAD_STATE;
    if (!xr_xir_task_executor_root_idle(instance->executor)) return XR_XIR_CALL_BAD_STATE;
    XrXirCallStatus completed = xr_xir_task_executor_completion_status(instance->executor);
    if (completed != XR_XIR_CALL_READY) return completed;
    return xr_xir_call_take_result(instance->call, output);
}
XrXirCallStatus xr_xir_instance_copy_failure(XrXirInstance *instance, XrXirCallResult *output) {
    if (!instance || !xr_xir_call_result_empty(output)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance->driving || instance->observing) return XR_XIR_CALL_BUSY;
    if (instance->state != XR_XIR_INSTANCE_FAILED) return XR_XIR_CALL_BAD_STATE;
    XrXirCallResult copy = instance_result(XR_XIR_CALL_READY);
    XrXirCallStatus status = value_call_status(xr_xir_call_result_copy(&instance->failure, &copy));
    if (status != XR_XIR_CALL_READY) return status;
    XrXirCallStatus failure_status = copy.status;
    xr_xir_call_result_move(&copy, output);
    return failure_status;
}
XrXirCallStatus xr_xir_instance_cancel_current(XrXirInstance *instance) {
    if (!instance) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance->driving || instance->observing) return XR_XIR_CALL_BUSY;
    if (instance->stopping || !instance->call) return XR_XIR_CALL_BAD_STATE;
    return xr_xir_task_executor_cancel_root(instance->executor);
}
XrXirCallStatus xr_xir_instance_stop(XrXirInstance *instance) {
    if (!instance) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance->observing) return XR_XIR_CALL_BUSY;
    instance->stopping = true;
    if (instance->function_gate) instance->function_gate->instance = NULL;
    bool driving = instance->driving;
    instance->driving = true;
    XrXirCallStatus status = xr_xir_task_executor_stop(instance->executor);
    if (!driving && xr_xir_task_executor_root_idle(instance->executor))
        status = xr_xir_task_executor_completion_status(instance->executor);
    else if (instance->call && !driving) {
        do { status = xr_xir_task_executor_poll_root(instance->executor, 256).status; }
        while (status == XR_XIR_CALL_READY);
    }
    instance->driving = driving;
    return status == XR_XIR_CALL_CANCELLED || status == XR_XIR_CALL_CANCEL_REQUESTED ? XR_XIR_CALL_READY : status;
}
XrXirCallStatus xr_xir_instance_free(XrXirInstance *instance) {
    if (!instance) return XR_XIR_CALL_READY;
    if (instance->driving || instance->observing) return XR_XIR_CALL_BUSY;
    instance->driving = true;
    instance->stopping = true;
    if (instance->function_gate) instance->function_gate->instance = NULL;
    XrXirCallStatus tasks = xr_xir_task_executor_free(instance->executor, NULL);
    instance->executor = NULL;
    XrXirCallStatus status = xr_xir_call_free(instance->call);
    instance->call = NULL;
    if (status == XR_XIR_CALL_READY) status = tasks;
    clear_slots(instance);
    xr_xir_call_result_drop(&instance->failure);
    instance_dispose(instance);
    return status;
}
static XrXirInstance *view_instance(XrXirCallView *view) {
    if (!xr_xir_call_admission(view) || !view->instance) return NULL;
    XrXirInstance *instance = view->instance;
    bool child = false;
    if (!instance->driving || instance->observing || (instance->stopping && !instance_cleanup_grant(instance)) ||
        !xr_xir_task_executor_view_member(instance->executor, view, &child) ||
        (!child && view->activation != instance->call) ||
        xr_xir_call_current_entry(view->activation) >= instance->program->entry_count) return NULL;
    return instance;
}

static bool view_is_child(const XrXirInstance *instance, const XrXirCallView *view) {
    bool child = false;
    XR_CHECK(xr_xir_task_executor_view_member(instance->executor, view, &child),
        "an admitted instance view keeps its exact activation membership");
    return child;
}
static bool worker_slot_read(const XrXirInstance *instance, uint32_t slot) {
    const XrXirProgram *program = instance->program;
    const XrXirSlot *entry = &program->declarations->slots[slot];
    return !entry->mutable && instance->ready[entry->module] && program->go_authority &&
        program->go_authority[(uint64_t)program->entry_count + slot] == XR_XIR_OK;
}
static bool value_unit(XrXirValue value) {
    return value.type == XR_XIR_UNIT && !value.reserved && !value.payload;
}
static XrXirCallStatus value_place(XrXirCallView *view, XrXirInstance *instance,
    const XrXirValueReceiver *receiver, bool writable, XrXirValueAdmission *admission,
    XrXirValuePlace *place) {
    if (!receiver) return XR_XIR_CALL_BAD_STATE;
    place->type = receiver->type;
    if (receiver->kind == XR_XIR_ROOT_CELL) {
        if (receiver->local_payload || receiver->slot) return XR_XIR_CALL_BAD_STATE;
        XrXirValueStatus status = xr_xir_cell_value_place(&receiver->value, admission, place);
        if (status != XR_XIR_VALUE_OK) return value_call_status(status);
        return place->type == receiver->type ? XR_XIR_CALL_READY : XR_XIR_CALL_BAD_STATE;
    }
    if (receiver->kind == XR_XIR_ROOT_OBJECT) {
        /* The object handle is only navigated: class steps never publish through this temporary. */
        if (receiver->local_payload || receiver->slot || receiver->value.type != (uint32_t) receiver->type)
            return XR_XIR_CALL_BAD_STATE;
        place->payload = (void *) &receiver->value.payload;
        return XR_XIR_CALL_READY;
    }
    if (!value_unit(receiver->value)) return XR_XIR_CALL_BAD_STATE;
    if (receiver->kind == XR_XIR_ROOT_LOCAL) {
        uint32_t entry = xr_xir_call_current_entry(view->activation);
        uintptr_t address = (uintptr_t) receiver->local_payload, base = (uintptr_t) view->state;
        uint32_t bytes = instance->entries[entry].state_bytes;
        if (receiver->slot || !receiver->local_payload || address < base ||
            bytes < sizeof(int64_t) || address - base > bytes - sizeof(int64_t))
            return XR_XIR_CALL_BAD_STATE;
        place->payload = receiver->local_payload;
        return XR_XIR_CALL_READY;
    }
    const XrXirDeclarations *d = instance->program->declarations;
    if (receiver->kind != XR_XIR_ROOT_SLOT || receiver->local_payload || receiver->slot >= d->slot_count)
        return XR_XIR_CALL_BAD_STATE;
    uint32_t slot = receiver->slot, module = d->slots[slot].module;
    if (view_is_child(instance, view) && (writable || !worker_slot_read(instance, slot)))
        return XR_XIR_CALL_BAD_STATE;
    uint32_t function = xr_xir_call_current_entry(view->activation);
    if (d->slots[slot].type != receiver->type || !instance->published[slot] ||
        d->functions[function].module != module ||
        (!instance->ready[module] && instance->current_module != module) ||
        (writable && (!d->slots[slot].mutable || module != d->root_module)))
        return XR_XIR_CALL_BAD_STATE;
    place->payload = &instance->slots[slot].payload;
    return XR_XIR_CALL_READY;
}
#include "xxir_instance_path.inc.c"
XrXirCallStatus xr_xir_instance_array_new(XrXirCallView *view, XrXirType type,
    const XrXirValue *values, uint32_t count, XrXirValue *output) {
    XrXirInstance *instance = view_instance(view);
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    return value_call_status(xr_xir_array_new(type, values, count, admission, output));
}
XrXirCallStatus xr_xir_instance_array_repeat(XrXirCallView *view, XrXirType type,
    int64_t length, const XrXirValue *fill, XrXirValue *output) {
    XrXirInstance *instance = view_instance(view);
    if (!instance || !fill || !output) return XR_XIR_CALL_BAD_STATE;
    if (length < 0) return XR_XIR_CALL_NUMERIC_RANGE;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    return value_call_status(xr_xir_array_repeat(type, length, fill, admission, output));
}
XrXirCallStatus xr_xir_instance_array_read(XrXirCallView *view,
    const XrXirValueReceiver *receiver, int64_t index, bool length,
    XrXirValue *output, XrXirFaultDetail *fault) {
    if (!fault) return XR_XIR_CALL_BAD_ARGUMENT;
    *fault = (XrXirFaultDetail) {0};
    XrXirInstance *instance = view_instance(view);
    if (!instance || !receiver || !output || !value_unit(*output)) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    XrXirValue borrowed = {0}, held = {0};
    if (receiver->kind == XR_XIR_ROOT_VALUE) {
        if (receiver->local_payload || receiver->slot || receiver->value.type != (uint32_t) receiver->type)
            return XR_XIR_CALL_BAD_STATE;
        borrowed = receiver->value;
    } else {
        XrXirValuePlace place = {0};
        XrXirCallStatus status = value_place(view, instance, receiver, false, admission, &place);
        if (status != XR_XIR_CALL_READY) return status;
        borrowed.type = (uint32_t) place.type;
        memcpy(&borrowed.payload, place.payload, sizeof(borrowed.payload));
        if (!length) {
            XrXirValueStatus copied = xr_xir_value_copy(&borrowed, &held);
            if (copied != XR_XIR_VALUE_OK) return value_call_status(copied);
            borrowed = held;
        }
    }
    XrXirValueStatus status;
    if (length) {
        int64_t size = 0;
        status = xr_xir_array_len(&borrowed, admission, &size);
        if (status == XR_XIR_VALUE_OK) *output = (XrXirValue) {XR_XIR_I64, 0, size};
    } else status = xr_xir_array_get(&borrowed, index, admission, output, fault);
    xr_xir_value_drop(&held);
    return value_call_status(status);
}
XrXirCallStatus xr_xir_instance_array_write(XrXirCallView *view,
    const XrXirValueReceiver *receiver, int64_t index, const XrXirValue *element,
    bool append, XrXirFaultDetail *fault) {
    if (!fault) return XR_XIR_CALL_BAD_ARGUMENT;
    *fault = (XrXirFaultDetail) {0};
    XrXirInstance *instance = view_instance(view);
    if (!instance || !receiver) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    XrXirValuePlace place = {0};
    XrXirCallStatus status = value_place(view, instance, receiver, true, admission, &place);
    if (status != XR_XIR_CALL_READY) return status;
    return value_call_status(append ? xr_xir_array_push(&place, element, admission) :
        xr_xir_array_set(&place, index, element, admission, fault));
}
XrXirCallStatus xr_xir_instance_struct_write(XrXirCallView *view,
    const XrXirValueReceiver *receiver, uint32_t field, const XrXirValue *value) {
    XrXirInstance *instance = view_instance(view);
    if (!instance || !receiver) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    XrXirValuePlace place = {0};
    XrXirCallStatus status = value_place(view, instance, receiver, true, admission, &place);
    if (status != XR_XIR_CALL_READY) return status;
    return value_call_status(xr_xir_struct_set(&place, field, value, admission));
}
XrXirCallStatus xr_xir_instance_literal(XrXirCallView *view, uint32_t literal, XrXirValue *output) {
    XrXirInstance *instance = view_instance(view);
    if (!instance || literal >= instance->program->declarations->literal_count) return XR_XIR_CALL_BAD_STATE;
    const XrXirLiteral *bytes = &instance->program->declarations->literals[literal];
    return value_call_status(xr_xir_string_new(instance->domain, bytes->bytes, bytes->length, output));
}
XrXirCallStatus xr_xir_instance_scalar_text(XrXirCallView *view, const XrXirValue *scalar, XrXirValue *output) {
    XrXirInstance *instance = view_instance(view);
    char buffer[XR_XIR_SCALAR_TEXT_BYTES]; const char *bytes = NULL; size_t length = 0;
    if (!instance || !scalar || !output || !xr_xir_scalar_text(scalar, buffer, &bytes, &length))
        return XR_XIR_CALL_BAD_STATE;
    return value_call_status(xr_xir_string_new(instance->domain, bytes, length, output));
}
static XrXirCallStatus time_call_status(XrXirTimeStatus status) {
    return status == XR_XIR_TIME_OK ? XR_XIR_CALL_READY :
        status == XR_XIR_TIME_RANGE ? XR_XIR_CALL_NUMERIC_RANGE : XR_XIR_CALL_HOST_ERROR;
}
XrXirCallStatus xr_xir_instance_clock_ns(XrXirCallView *view, XrXirClockKind clock, int64_t *nanoseconds) {
    XrXirInstance *instance = view_instance(view);
    if (!instance || !nanoseconds || clock < XR_XIR_CLOCK_REALTIME || clock > XR_XIR_CLOCK_MONOTONIC)
        return XR_XIR_CALL_BAD_STATE;
    const XrXirTimeProvider *provider = &instance->config.time;
    if (!provider->abi_version) return XR_XIR_CALL_HOST_ERROR;
    int64_t reading = 0;
    XrXirTimeStatus status = provider->clock(provider->context, clock, &reading);
    /* Clock readings are nonnegative nanosecond counts; a rejection is not a range fault. */
    if (status == XR_XIR_TIME_OK && reading < 0) status = XR_XIR_TIME_FAILED;
    if (status == XR_XIR_TIME_RANGE) status = XR_XIR_TIME_FAILED;
    if (status == XR_XIR_TIME_OK) *nanoseconds = reading;
    return time_call_status(status);
}
XrXirCallStatus xr_xir_instance_utc_offset(XrXirCallView *view, int64_t seconds, int64_t *minutes) {
    XrXirInstance *instance = view_instance(view);
    if (!instance || !minutes) return XR_XIR_CALL_BAD_STATE;
    const XrXirTimeProvider *provider = &instance->config.time;
    if (!provider->abi_version) return XR_XIR_CALL_HOST_ERROR;
    int64_t offset = 0;
    XrXirTimeStatus status = provider->utc_offset(provider->context, seconds, &offset);
    if (status == XR_XIR_TIME_OK) *minutes = offset;
    return time_call_status(status);
}
XrXirCallStatus xr_xir_instance_slot_read(XrXirCallView *view, uint32_t slot, XrXirValue *output) {
    XrXirInstance *instance = view_instance(view);
    if (!instance || slot >= instance->program->declarations->slot_count) return XR_XIR_CALL_BAD_STATE;
    const XrXirDeclarations *d = instance->program->declarations;
    uint32_t function = xr_xir_call_current_entry(view->activation), module = d->slots[slot].module;
    if (view_is_child(instance, view) && !worker_slot_read(instance, slot)) return XR_XIR_CALL_BAD_STATE;
    if (!instance->published[slot] || d->functions[function].module != module ||
        (!instance->ready[module] && instance->current_module != module)) return XR_XIR_CALL_BAD_STATE;
    return value_call_status(xr_xir_value_copy(&instance->slots[slot], output));
}
XrXirCallStatus xr_xir_instance_slot_write(XrXirCallView *view, uint32_t slot,
                                          const XrXirValue *value, bool publish) {
    XrXirInstance *instance = view_instance(view);
    if (!instance || slot >= instance->program->declarations->slot_count || view_is_child(instance, view))
        return XR_XIR_CALL_BAD_STATE;
    const XrXirDeclarations *d = instance->program->declarations;
    uint32_t function = xr_xir_call_current_entry(view->activation), module = d->slots[slot].module;
    if (d->functions[function].module != module || !(d->slots[slot].type == XR_XIR_UNIT ?
            (value && value_unit(*value)) :
            xr_xir_value_argument(value, instance->program->arena, d->slots[slot].type)))
        return XR_XIR_CALL_BAD_STATE;
    if (publish ? (instance->published[slot] || instance->current_module != module ||
                   d->modules[module].initializer != function) :
                  (!instance->published[slot] || !d->slots[slot].mutable || module != d->root_module))
        return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    XrXirCallStatus admitted = d->slots[slot].type == XR_XIR_UNIT ? XR_XIR_CALL_READY :
        admit_instance_value(admission, value, d->slots[slot].type);
    if (admitted != XR_XIR_CALL_READY) return admitted;
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

#include "xxir_instance_slot_group.inc.c"

XrXirCallStatus xr_xir_instance_start(XrXirInstance *instance, uint32_t entry,
    const XrXirValue *arguments, uint32_t count) {
    if (!instance) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirValueAdmission admission = instance_candidate_admission(instance);
    return instance_start(instance, entry, arguments, count, NULL, &admission, false);
}
XrXirCallStatus xr_xir_instance_start_test(XrXirInstance *instance, uint32_t entry) {
    if (!instance) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirValueAdmission admission = instance_candidate_admission(instance);
    return instance_start(instance, entry, NULL, 0, NULL, &admission, true);
}
static XrXirCallStatus resolve_function(XrXirInstance *instance, const XrXirValue *value,
    uint32_t *entry, XrXirValueAdmission *admission) {
    if (!instance || !entry) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!value || !xr_xir_value_argument(value, instance->program->arena, (XrXirType) value->type))
        return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirFunctionBinding *binding = xr_xir_function_binding(value);
    if (!binding || binding->release != function_gate_drop || !binding->owner ||
        binding->owner != instance->function_gate) return XR_XIR_CALL_BAD_ARGUMENT;
    FunctionGate *gate = binding->owner;
    if ((!gate->instance || instance->stopping) && !instance_cleanup_grant(instance)) return XR_XIR_CALL_BAD_STATE;
    if ((gate->instance != instance && !(instance->stopping && !gate->instance && instance_cleanup_grant(instance))) ||
        gate->program != instance->program ||
        binding->entry >= instance->program->entry_count) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!instance->program->active_modules[instance->program->declarations->functions[binding->entry].module])
        return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallStatus admitted = admit_instance_value(admission, value, (XrXirType) value->type);
    if (admitted != XR_XIR_CALL_READY) return admitted;
    *entry = binding->entry; return XR_XIR_CALL_READY;
}
XrXirCallStatus xr_xir_instance_start_function(XrXirInstance *instance, const XrXirValue *function,
    const XrXirValue *arguments, uint32_t count) {
    if (!instance) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance->stopping) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission admission = instance_candidate_admission(instance);
    uint32_t entry = 0;
    XrXirCallStatus status = resolve_function(instance, function, &entry, &admission);
    return status == XR_XIR_CALL_READY ? instance_start(instance, entry, arguments, count,
        xr_xir_function_binding(function), &admission, false) : status;
}
XrXirCallStatus xr_xir_instance_resolve_function(XrXirCallView *view, const XrXirValue *function, uint32_t *entry) {
    XrXirInstance *instance = view_instance(view);
    return instance && !view_is_child(instance, view) ?
        resolve_function(instance, function, entry, xr_xir_call_admission(view)) : XR_XIR_CALL_BAD_STATE;
}
static bool instance_admission_work(void *owner, uint64_t units) {
    uint64_t *remaining = owner;
    if (units > *remaining) return false;
    *remaining -= units;
    return true;
}
XR_FUNC XrXirCallStatus xr_xir_instance_weaken_function(XrXirCallView *view,
    XrXirType type, const XrXirValue *input, XrXirValue *output) {
    XrXirInstance *instance = view_instance(view);
    if (!instance || !input) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    XrXirCallStatus status = admit_instance_value(admission, input, (XrXirType)input->type);
    if (status != XR_XIR_CALL_READY) return status;
    XrXirStatus match = xr_xir_callable_weakening_admit(instance->program->types,
        (XrXirType)input->type, type, &admission->work, instance_admission_work);
    if (match != XR_XIR_OK) return match == XR_XIR_BUDGET ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirFunctionBinding *binding = xr_xir_function_binding(input);
    if (!binding || !function_gate_retain(instance->function_gate)) return XR_XIR_CALL_LIMIT;
    status = value_call_status(xr_xir_function_new(instance->domain, instance->program->arena,
        type, binding, admission, output));
    if (status != XR_XIR_CALL_READY) function_gate_drop(instance->function_gate);
    return status;
}
static XrXirCallStatus prepare_function_gate(XrXirInstance *instance) {
    if (instance->function_gate) return XR_XIR_CALL_READY;
    if (sizeof(FunctionGate) > instance->config.metadata_limit - instance->metadata_bytes)
        return XR_XIR_CALL_LIMIT;
    if (!xr_xir_domain_retain(instance->domain)) return XR_XIR_CALL_LIMIT;
    XrXirCallStatus status = XR_XIR_CALL_READY;
    FunctionGate *gate = instance_metadata_allocate(instance->domain, sizeof(*gate), &status);
    if (!gate) { xr_xir_domain_drop(instance->domain); return status; }
    if (!xr_xir_compile_program_retain(instance->program)) {
        xr_xir_domain_metadata_deallocate(instance->domain, gate, sizeof(*gate));
        xr_xir_domain_drop(instance->domain); return XR_XIR_CALL_LIMIT;
    }
    atomic_init(&gate->references, 1);
    gate->program = instance->program; gate->instance = instance->stopping ? NULL : instance;
    gate->domain = instance->domain;
    instance->function_gate = gate; instance->metadata_bytes += sizeof(*gate);
    return XR_XIR_CALL_READY;
}
XrXirCallStatus xr_xir_instance_function(XrXirCallView *view, XrXirType type, uint32_t entry,
    const XrXirValue *captures, uint32_t count, XrXirValue *output) {
    XrXirInstance *instance = view_instance(view);
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    const XrXirTypeNode *signature = xr_xir_callable_signature(instance->program->types, type);
    if (!signature || entry >= instance->program->entry_count || (count && !captures)) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirCallEntry *target = &instance->program->entries[entry];
    const XrXirDeclarations *d = instance->program->declarations;
    uint32_t caller = xr_xir_call_current_entry(view->activation);
    uint32_t from = d->functions[caller].module, to = d->functions[entry].module;
    if (!instance->program->active_modules[from] || !instance->program->active_modules[to])
        return XR_XIR_CALL_BAD_ARGUMENT;
    if ((signature->flags & XR_XIR_CALLABLE_NO_SUSPEND) &&
        !(d->functions[entry].promises & XR_XIR_FUNCTION_NO_SUSPEND)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (target->cleanup_owner || entry == d->modules[to].initializer || !xr_xir_module_imports(d, from, to) ||
        (from != to && !d->functions[entry].exported) || count > target->parameter_count || target->parameter_count - count != signature->parameter_count ||
        target->result != signature->result) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    for (uint32_t p = 0; p < count; ++p) {
        XrXirCallStatus admitted = admit_instance_value(admission, &captures[p], target->parameters[p]);
        if (admitted != XR_XIR_CALL_READY) return admitted;
    }
    for (uint32_t p = 0; p < signature->parameter_count; ++p)
        if (target->parameters[p + count] != signature->parameters[p].type) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallStatus status = prepare_function_gate(instance);
    if (status != XR_XIR_CALL_READY) return status;
    if (!function_gate_retain(instance->function_gate)) return XR_XIR_CALL_LIMIT;
    XrXirFunctionBinding binding = {instance->function_gate, function_gate_drop, entry, captures, count};
    status = value_call_status(xr_xir_function_new(instance->domain, instance->program->arena,
        type, &binding, admission, output));
    if (status != XR_XIR_CALL_READY) function_gate_drop(instance->function_gate);
    return status;
}
XR_FUNC XrXirCallStatus xr_xir_task_go(XrXirCallView *view, XrXirType task_type, uint32_t direct_entry,
    const XrXirValue *arguments, uint32_t count, XrXirValue *output) {
    if (!output || !value_unit(*output) || (count && !arguments)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance = view_instance(view);
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    if (xr_xir_call_cleanup_active(view->activation)) return XR_XIR_CALL_DEFER_ASYNC;
    XrXirCallStatus status = xr_xir_call_execution_status(view);
    if (status != XR_XIR_CALL_READY) return status;
    const XrXirProgram *program = instance->program;
    const XrXirDeclarations *declarations = program->declarations;
    if (direct_entry >= program->entry_count || !program->go_authority || view->arena != program->arena)
        return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirCallEntry *target = &instance->entries[direct_entry];
    uint32_t caller = xr_xir_call_current_entry(view->activation);
    uint32_t from = declarations->functions[caller].module, to = declarations->functions[direct_entry].module;
    if (!program->active_modules[from] || !program->active_modules[to] ||
        !xr_xir_module_imports(declarations, from, to) ||
        (from != to && !declarations->functions[direct_entry].exported) ||
        target->cleanup_owner || direct_entry == declarations->modules[to].initializer ||
        count != target->parameter_count || program->go_authority[direct_entry] != XR_XIR_OK)
        return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirType result = xr_xir_task_element(program->types, task_type);
    if (!result || result != target->result) return XR_XIR_CALL_BAD_ARGUMENT;
    if (result != XR_XIR_I64 && result != XR_XIR_STRING) return XR_XIR_CALL_UNSUPPORTED;
    for (uint32_t p = 0; p < count; ++p) {
        if (!xr_xir_task_parameter_supported(program->types, target->parameters[p]))
            return XR_XIR_CALL_UNSUPPORTED;
        if (!xr_xir_value_argument(&arguments[p], program->arena, target->parameters[p]))
            return XR_XIR_CALL_BAD_ARGUMENT;
    }
    const XrXirCallRequest request = {direct_entry, arguments, count};
    return xr_xir_task_executor_spawn_from_view(instance->executor, view, task_type, &request, output);
}
XrXirCallStatus xr_xir_instance_cell(XrXirCallView *view, XrXirType type,
    const XrXirValue *initial, XrXirValue *output) {
    XrXirInstance *instance = view_instance(view);
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    return value_call_status(xr_xir_cell_new(instance->domain, instance->program->arena, type,
        initial, admission, output));
}
XrXirCallStatus xr_xir_instance_cell_read(XrXirCallView *view, const XrXirValue *cell, XrXirValue *output) {
    XrXirInstance *instance = view_instance(view);
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    if (!xr_xir_cell_in_domain(cell, instance->domain)) return XR_XIR_CALL_BAD_ARGUMENT;
    return value_call_status(xr_xir_cell_read(cell, output));
}
XrXirCallStatus xr_xir_instance_cell_write(XrXirCallView *view, const XrXirValue *cell, const XrXirValue *value) {
    XrXirInstance *instance = view_instance(view);
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    if (!xr_xir_cell_in_domain(cell, instance->domain)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    return value_call_status(xr_xir_cell_write(cell, value, admission));
}
XrXirAction xr_xir_instance_panic_land(XrXirCallView *view, void *frame, XrXirAction action,
    uint32_t destination, uint32_t handler_pc, uint32_t *pc) {
    if (!xr_xir_call_panic_action(&action)) return action;
    XrXirCallStatus status = XR_XIR_CALL_READY;
    if (!frame || !pc || !handler_pc) status = XR_XIR_CALL_BAD_STATE;
    else if (destination != UINT32_MAX) {
        XrXirInstance *instance = view_instance(view);
        XrXirValue info = {0};
        status = !instance ? XR_XIR_CALL_BAD_STATE :
            value_call_status(xr_xir_panic_info_new(instance->domain, &action.panic, &info));
        if (status == XR_XIR_CALL_READY) xr_xir_owned_slot_move(frame, destination, &info);
    }
    if (status != XR_XIR_CALL_READY)
        return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, status}, {0}, 0};
    *pc = handler_pc;
    return (XrXirAction) {XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0, 0, 0}, {0}, 0};
}
