/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_task.c - Owned task results and a bounded canonical-call FIFO driver
 *
 * KEY CONCEPT:
 *   A public handle never drives or cancels the executor that owns pending work.
 */
#include "xxir_task_internal.h"
#include "xxir_error.h"
#include <string.h>

struct XrXirTaskExecutor {
    XrXirTaskExecutorConfig config;
    XrXirCallBudget *budget;
    XirTask *active;
    XirTaskActivation *ready_head, *ready_tail;
    XirTaskActivation root;
    XirTaskActivation *current, *external;
    uint64_t external_call_wake, host_wake, root_epoch;
    uint64_t generation, next_identity;
    XrXirCallStatus shutdown_status;
    XrXirCallStatus first_failure;
    bool driving, stopping, cleanup_incomplete;
};
static _Atomic(uint64_t) task_executor_generation;
#include "xxir_task_outcome.inc.c"
static XrXirCallStatus task_core_value_status(XrXirValueStatus status) {
    return status == XR_XIR_VALUE_OK ? XR_XIR_CALL_READY : status == XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM :
        status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_BAD_ARGUMENT;
}
static bool task_core_empty_value(const XrXirValue *value) {
    return value && !value->type && !value->reserved && !value->payload;
}
static XrXirValue task_core_task_value(XirTask *task) {
    return (XrXirValue){(uint32_t)task->object.type, 0, (int64_t)(uintptr_t)task};
}
XR_FUNC bool xr_xir_task_storage_valid(const XirTask *task) {
    const XrXirTypes *types = xr_xir_compile_type_arena_types(task->object.arena);
    XrXirType result = xr_xir_task_element(types, task->object.type);
    if (result != XR_XIR_I64 && result != XR_XIR_STRING) return false;
    XirTaskState state = atomic_load_explicit(&task->state, memory_order_acquire);
    if (state == XIR_TASK_PREPARING)
        return !task->generation && !task->identity && !task->executor && !task->call && xr_xir_call_result_empty(&task->outcome);
    /* Pending handles may cross threads. Only the driver touches execution
     * pointers and the uncommitted outcome until release publication. */
    if (state == XIR_TASK_PENDING || state == XIR_TASK_RUNNING) return task->generation && task->identity;
    return state == XIR_TASK_TERMINAL && !task->executor && !task->call && task->generation && task->identity &&
        xr_xir_call_result_valid(&task->outcome) &&
        task->outcome.status != XR_XIR_CALL_READY && task->outcome.status != XR_XIR_CALL_SUSPENDED &&
        task->outcome.status != XR_XIR_CALL_CONSUMED &&
        (task->outcome.status != XR_XIR_CALL_RETURNED || task->outcome.value.type == (uint32_t)result);
}
XR_FUNC XrXirCallStatus xr_xir_task_copy_outcome(const XrXirValue *value, XrXirCallResult *output) {
    if (!value || !xr_xir_value_valid(value) || !xr_xir_call_result_empty(output)) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirTypeArena *arena = xr_xir_value_arena(value);
    if (!xr_xir_task_element(xr_xir_compile_type_arena_types(arena), (XrXirType)value->type))
        return XR_XIR_CALL_BAD_ARGUMENT;
    const XirTask *task = (const XirTask *)(uintptr_t)value->payload;
    if (atomic_load_explicit(&task->state, memory_order_acquire) != XIR_TASK_TERMINAL) return XR_XIR_CALL_BAD_STATE;
    if (!xr_xir_domain_work(task->object.domain, 1)) return XR_XIR_CALL_LIMIT;
    XrXirValueStatus copied = xr_xir_call_result_copy(&task->outcome, output);
    return copied == XR_XIR_VALUE_OK ? task->outcome.status : task_core_value_status(copied);
}
static void task_core_enqueue(XrXirTaskExecutor *executor, XirTaskActivation *activation) {
    XR_CHECK(!activation->queued && !activation->ready_next && activation->call,
        "a pending task owns exactly one ready-queue position");
    if (executor->ready_tail) executor->ready_tail->ready_next = activation;
    else executor->ready_head = activation;
    executor->ready_tail = activation;
    activation->queued = true;
}
static XirTaskActivation *task_core_dequeue(XrXirTaskExecutor *executor) {
    XirTaskActivation *activation = executor->ready_head;
    if (!activation) return NULL;
    executor->ready_head = activation->ready_next;
    if (!executor->ready_head) executor->ready_tail = NULL;
    activation->ready_next = NULL;
    activation->queued = false;
    return activation;
}
XR_FUNC bool xr_xir_task_executor_root_idle(const XrXirTaskExecutor *executor) {
    if (!executor || executor->driving || executor->active || executor->ready_head) return false;
    if (!executor->root.call) return true;
    XrXirCallStatus state = xr_xir_call_state(executor->root.call);
    return state != XR_XIR_CALL_READY && state != XR_XIR_CALL_SUSPENDED;
}
/* Capture acceptance order at the shared FIFO transition boundary, before
 * another activation can fail or terminal publication can require resources. */
static void task_core_record_status(XrXirTaskExecutor *executor, XrXirCallStatus failure) {
    if (executor->first_failure == XR_XIR_CALL_READY && failure != XR_XIR_CALL_READY)
        executor->first_failure = failure;
}
static void task_core_record_failure(XrXirTaskExecutor *executor, XirTaskActivation *activation) {
    const XrXirExecutorBinding binding = {executor, activation, executor->generation, activation->ticket};
    XrXirCallStatus failure = XR_XIR_CALL_READY;
    XR_CHECK(xr_xir_call_driver_failure(activation->call, &binding, &failure),
        "the driven activation still owns its exact executor binding");
    task_core_record_status(executor, failure);
}
static XrXirCallStatus task_core_completion_status(const XrXirTaskExecutor *executor) {
    if (executor->shutdown_status != XR_XIR_CALL_READY)
        return executor->first_failure != XR_XIR_CALL_READY ? executor->first_failure : executor->shutdown_status;
    if (executor->budget->exhausted)
        return executor->first_failure != XR_XIR_CALL_READY ? executor->first_failure : XR_XIR_CALL_LIMIT;
    return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xr_xir_task_executor_completion_status(const XrXirTaskExecutor *executor) {
    if (!executor) return XR_XIR_CALL_BAD_ARGUMENT;
    if (executor->driving) return XR_XIR_CALL_BUSY;
    if (!xr_xir_task_executor_root_idle(executor)) return XR_XIR_CALL_BAD_STATE;
    return task_core_completion_status(executor);
}
XR_FUNC XrXirCallStatus xr_xir_task_executor_attach_root(XrXirTaskExecutor *executor, XrXirCall *call, uint64_t epoch) {
    if (!executor || !call || !epoch) return XR_XIR_CALL_BAD_ARGUMENT;
    if (executor->driving) return XR_XIR_CALL_BUSY;
    if (!xr_xir_task_executor_root_idle(executor)) return XR_XIR_CALL_BUSY;
    if (executor->stopping || executor->budget->exhausted || executor->cleanup_incomplete ||
        xr_xir_call_cleanup_incomplete(executor->root.call) || executor->next_identity == UINT64_MAX)
        return XR_XIR_CALL_BAD_STATE;
    if (epoch <= executor->root_epoch) return XR_XIR_CALL_BAD_STATE;
    uint64_t ticket = executor->next_identity + 1;
    const XrXirExecutorBinding binding = {executor, &executor->root, executor->generation, ticket};
    XrXirCallStatus bound = xr_xir_call_bind_executor(call, &binding);
    if (bound != XR_XIR_CALL_READY) return bound;
    /* A successfully authenticated ordinary root begins a new fault summary,
     * while the Instance lifetime budget and sticky cleanup facts remain. */
    executor->shutdown_status = XR_XIR_CALL_READY;
    executor->first_failure = XR_XIR_CALL_READY;
    executor->next_identity = ticket;
    executor->root = (XirTaskActivation){.call = call, .ticket = ticket};
    executor->root_epoch = epoch; executor->host_wake = 0;
    executor->external = NULL; executor->external_call_wake = 0;
    task_core_enqueue(executor, &executor->root);
    return XR_XIR_CALL_READY;
}
XR_FUNC bool xr_xir_task_executor_view_member(const XrXirTaskExecutor *executor,
    const XrXirCallView *view, bool *child) {
    if (!executor || !view || !child || !executor->driving || !executor->current ||
        executor->current->call != view->activation || !xr_xir_call_admission(view)) return false;
    const XirTaskActivation *activation = executor->current;
    const XrXirExecutorBinding binding = {executor, (void *)activation, executor->generation, activation->ticket};
    if (!xr_xir_call_executor_member(view->activation, &binding)) return false;
    if (activation->task && (activation->task->executor != executor || activation->task->call != view->activation ||
        activation->task->generation != executor->generation || activation->task->identity != activation->ticket)) return false;
    if (!activation->task && activation != &executor->root) return false;
    *child = activation->task != NULL;
    return true;
}
XR_FUNC bool xr_xir_task_executor_cleanup_active(const XrXirTaskExecutor *executor) {
    if (!executor || !executor->driving || !executor->current || !executor->current->call) return false;
    const XirTaskActivation *activation = executor->current;
    const XrXirExecutorBinding binding = {executor, (void *)activation, executor->generation, activation->ticket};
    return xr_xir_call_executor_member(activation->call, &binding) && xr_xir_call_cleanup_active(activation->call);
}
XR_FUNC XrXirCallStatus xr_xir_task_executor_new(const XrXirTaskExecutorConfig *config,
    XrXirTaskExecutor **output) {
    if (!config || !output || *output || !config->program || !config->domain ||
        !config->budget || config->budget->work_domain != config->domain || !config->work_limit ||
        !config->requested_call_limit) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirDomainBudgetStats authority = xr_xir_domain_budget_stats(config->domain);
    if (!authority.bound || authority.requested_limit != config->requested_value_limit ||
        authority.requested_call_limit != config->requested_call_limit || authority.work_limit != config->work_limit ||
        authority.call_limit != config->call_limit || config->budget->byte_limit != config->call_limit ||
        config->budget->resume_limit != config->poll_limit ||
        config->budget->requested_limit != config->requested_call_limit) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!xr_xir_compile_program_retain(config->program)) return XR_XIR_CALL_LIMIT;
    if (!xr_xir_domain_retain(config->domain)) {
        xr_xir_compile_program_drop(config->program); return XR_XIR_CALL_LIMIT;
    }
    if (config->task_arena && !xr_xir_compile_type_arena_retain(config->task_arena)) {
        xr_xir_domain_drop(config->domain); xr_xir_compile_program_drop(config->program); return XR_XIR_CALL_LIMIT;
    }
    XrXirValueStatus allocation_status = XR_XIR_VALUE_OK;
    XrXirTaskExecutor *executor = xr_xir_domain_metadata_allocate(config->domain, sizeof(*executor), &allocation_status);
    if (!executor) {
        xr_xir_compile_type_arena_drop(config->task_arena);
        xr_xir_domain_drop(config->domain); xr_xir_compile_program_drop(config->program);
        return task_core_value_status(allocation_status);
    }
    uint64_t generation = atomic_load_explicit(&task_executor_generation, memory_order_relaxed);
    for (;;) {
        if (generation == UINT64_MAX) {
            xr_xir_domain_metadata_deallocate(config->domain, executor, sizeof(*executor));
            xr_xir_compile_type_arena_drop(config->task_arena); xr_xir_domain_drop(config->domain);
            xr_xir_compile_program_drop(config->program); return XR_XIR_CALL_LIMIT;
        }
        if (atomic_compare_exchange_weak_explicit(&task_executor_generation, &generation, generation + 1,
                memory_order_relaxed, memory_order_relaxed)) break;
    }
    executor->config = *config; executor->budget = config->budget; executor->generation = generation + 1;
    *output = executor;
    return XR_XIR_CALL_READY;
}
static XrXirValueStatus task_core_task_prepare(XrXirTaskExecutor *executor, XrXirType type, XirTask **output) {
    XrXirDomain *domain = executor->config.domain;
    XrXirTypeArena *arena = executor->config.task_arena;
    if (!xr_xir_domain_retain(domain)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    if (!xr_xir_compile_type_arena_retain(arena)) { xr_xir_domain_drop(domain); return XR_XIR_VALUE_REFCOUNT_LIMIT; }
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    XirTask *task = xr_xir_domain_allocate(domain, sizeof(*task), &status);
    if (!task) { xr_xir_compile_type_arena_drop(arena); xr_xir_domain_drop(domain); return status; }
    memset(task, 0, sizeof(*task));
    atomic_init(&task->object.references, 1);
    atomic_init(&task->state, XIR_TASK_PREPARING);
    task->object.domain = domain; task->object.arena = arena; task->object.type = type;
    task->object.kind = XR_XIR_TYPE_TASK;
    *output = task;
    return XR_XIR_VALUE_OK;
}
static XrXirCallStatus task_core_spawn(XrXirTaskExecutor *executor, XrXirCallView *view, XrXirType type,
    const XrXirCallRequest *request, XrXirValue *output) {
    if (!executor || !request || !task_core_empty_value(output)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (executor->driving) {
        bool child = false;
        if (!view || !xr_xir_task_executor_view_member(executor, view, &child)) return XR_XIR_CALL_BUSY;
    } else if (view) return XR_XIR_CALL_BAD_STATE;
    if (executor->stopping || executor->budget->exhausted || executor->next_identity == UINT64_MAX)
        return XR_XIR_CALL_BAD_STATE;
    const XrXirProgram *program = executor->config.program;
    XrXirType result = xr_xir_task_element(xr_xir_compile_type_arena_types(executor->config.task_arena), type);
    if ((result != XR_XIR_I64 && result != XR_XIR_STRING) || request->entry >= program->entry_count ||
        program->entries[request->entry].result != result || program->entries[request->entry].cleanup_owner)
        return XR_XIR_CALL_BAD_ARGUMENT;
    for (uint32_t p = 0; p < program->entries[request->entry].parameter_count; ++p) {
        XrXirType parameter = program->entries[request->entry].parameters[p];
        if (!xr_xir_task_parameter_supported(program->types, parameter)) return XR_XIR_CALL_UNSUPPORTED;
    }
    XirTask *task = NULL;
    XrXirValueStatus prepared = task_core_task_prepare(executor, type, &task);
    if (prepared != XR_XIR_VALUE_OK) return task_core_value_status(prepared);
    XrXirCallConfig config;
    XR_CHECK(xr_xir_call_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY, "canonical call configuration fits");
    config.entries = executor->config.entries ? executor->config.entries : program->entries;
    config.entry_count = executor->config.entries ? executor->config.entry_count : program->entry_count;
    config.instance = executor->config.instance; config.output = executor->config.output;
    config.byte_limit = executor->config.call_limit; config.poll_limit = executor->config.poll_limit;
    config.depth_limit = executor->config.depth_limit; config.accounting = &task->accounting;
    config.admission = (XrXirValueAdmission){program->arena, executor->config.domain, NULL, NULL,
        executor->config.poll_limit, executor->config.call_limit};
    if (executor->config.admission.arena) config.admission = executor->config.admission;
    XrXirCallStatus status = xr_xir_call_new_budgeted(&config, request, executor->budget, &task->call);
    if (status != XR_XIR_CALL_READY) {
        XrXirValue unpublished = task_core_task_value(task); xr_xir_value_drop(&unpublished); return status;
    }
    if (view) status = xr_xir_call_execution_status(view);
    if (status == XR_XIR_CALL_READY && (executor->stopping || executor->budget->exhausted))
        status = executor->budget->exhausted ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_CANCELLED;
    if (status != XR_XIR_CALL_READY) {
        XrXirCallStatus freed = xr_xir_call_free(task->call);
        task->call = NULL;
        XrXirValue unpublished = task_core_task_value(task); xr_xir_value_drop(&unpublished);
        return freed == XR_XIR_CALL_READY ? status : freed;
    }
    task->generation = executor->generation; task->identity = ++executor->next_identity;
    task->activation = (XirTaskActivation){.call = task->call, .task = task, .ticket = task->identity};
    const XrXirExecutorBinding binding = {executor, &task->activation, executor->generation, task->identity};
    XR_CHECK(xr_xir_call_bind_executor(task->call, &binding) == XR_XIR_CALL_READY,
        "new child activation owns one non-reusable executor ticket");
    task->executor = executor; task->state = XIR_TASK_PENDING;
    /* One reference is the returned handle; the other is the executor running lease. */
    atomic_store_explicit(&task->object.references, 2, memory_order_relaxed);
    task->active_next = executor->active;
    if (executor->active) executor->active->active_previous = task;
    executor->active = task;
    task_core_enqueue(executor, &task->activation);
    *output = task_core_task_value(task);
    return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xr_xir_task_executor_spawn(XrXirTaskExecutor *executor, XrXirType type,
    const XrXirCallRequest *request, XrXirValue *output) {
    return task_core_spawn(executor, NULL, type, request, output);
}
XR_FUNC XrXirCallStatus xr_xir_task_executor_spawn_from_view(XrXirTaskExecutor *executor,
    XrXirCallView *view, XrXirType type, const XrXirCallRequest *request, XrXirValue *output) {
    if (!view) return XR_XIR_CALL_BAD_ARGUMENT;
    return task_core_spawn(executor, view, type, request, output);
}
static void task_core_complete_wait(XrXirTaskExecutor *executor, XrXirCall *call) {
    XrXirTaskWaitToken token = {0};
    XR_CHECK(xr_xir_call_task_wait_token(call, &token) && token.binding.owner == executor &&
        token.binding.generation == executor->generation, "completion keeps the exact executor generation");
    XirTaskActivation *activation = token.binding.activation;
    XR_CHECK(activation && activation->call == call && activation->ticket == token.binding.ticket && !activation->queued,
        "only the owner's suspended activation can receive a task outcome");
    XirTask *subject = (XirTask *)(uintptr_t)token.request.subject;
    XrXirValue value = task_core_task_value(subject);
    XrXirCallResult prepared = {0};
    XrXirCallStatus copied = xr_xir_task_copy_outcome(&value, &prepared);
    XrXirCallStatus completed;
    if (xr_xir_call_result_empty(&prepared))
        completed = xr_xir_call_fail_task_wait(call, &token, copied);
    else completed = xr_xir_call_complete_task_wait(call, &token, &prepared);
    xr_xir_call_result_drop(&prepared);
    XR_CHECK(completed == XR_XIR_CALL_READY, "a validated waiter completes or faults exactly once");
    task_core_record_failure(executor, activation);
    task_core_enqueue(executor, activation);
}
static void task_core_register_wait(XrXirTaskExecutor *executor, XrXirCall *call) {
    XrXirTaskWaitToken token = {0};
    XR_CHECK(xr_xir_call_task_wait_token(call, &token), "a task wait retains its exact request");
    XrXirCallStatus linked = xr_xir_call_link_task_wait(call, &token);
    if (linked == XR_XIR_CALL_RETURNED) task_core_complete_wait(executor, call);
    else if (linked != XR_XIR_CALL_SUSPENDED) {
        XR_CHECK(xr_xir_call_fail_task_wait(call, &token, linked) == XR_XIR_CALL_READY,
            "failed registration does not publish a waiter or modify its target");
        task_core_record_failure(executor, token.binding.activation);
        task_core_enqueue(executor, token.binding.activation);
    }
}
static void task_core_finish_task(XrXirTaskExecutor *executor, XirTask *task) {
    XrXirCallResult prepared = {0};
    XrXirCallStatus status = xr_xir_call_take_outcome(task->call, &prepared);
    XR_CHECK(status != XR_XIR_CALL_READY && status != XR_XIR_CALL_SUSPENDED,
        "terminal task publication takes its complete call outcome");
    XrXirCallStatus admitted = task_core_outcome_admit(executor, &prepared);
    if (admitted != XR_XIR_CALL_READY) {
        task_core_record_status(executor, admitted);
        xr_xir_call_result_drop(&prepared); prepared.status = status = admitted;
    }
    /* Ordinary language outcomes belong to the child, but host faults and incomplete cleanup belong to shutdown. */
    if (executor->shutdown_status == XR_XIR_CALL_READY && status != XR_XIR_CALL_RETURNED &&
        status != XR_XIR_CALL_CANCELLED && (executor->stopping ||
        (status != XR_XIR_CALL_THROWN && !xr_xir_call_panic_status(status))))
        executor->shutdown_status = status;
    if (xr_xir_call_cleanup_incomplete(task->call)) executor->cleanup_incomplete = true;
    XrXirCallStatus freed = xr_xir_call_free(task->call);
    if (freed != XR_XIR_CALL_READY) executor->cleanup_incomplete = true;
    if (executor->shutdown_status == XR_XIR_CALL_READY && freed != XR_XIR_CALL_READY) {
        task_core_record_status(executor, freed);
        executor->shutdown_status = freed;
    }
    task->call = NULL;
    if (executor->external == &task->activation) {
        executor->external = NULL; executor->external_call_wake = 0;
    }
    task->activation.call = NULL;
    if (task->active_previous) task->active_previous->active_next = task->active_next;
    else {
        XR_CHECK(executor->active == task, "terminal task keeps its original executor membership");
        executor->active = task->active_next;
    }
    if (task->active_next) task->active_next->active_previous = task->active_previous;
    task->active_next = task->active_previous = NULL;
    task->executor = NULL;
    xr_xir_call_result_move(&prepared, &task->outcome);
    atomic_store_explicit(&task->state, XIR_TASK_TERMINAL, memory_order_release);
    while (task->waiter_head) task_core_complete_wait(executor, task->waiter_head);
    XrXirValue running_lease = task_core_task_value(task); xr_xir_value_drop(&running_lease);
}
static void task_core_reject_closed_wait(XrXirTaskExecutor *executor) {
    if (executor->ready_head) return;
    bool root_waiting = executor->root.call && xr_xir_call_state(executor->root.call) == XR_XIR_CALL_SUSPENDED;
    if (!executor->active && !root_waiting) return;
    bool permitted = true;
    if (root_waiting) {
        XrXirTaskWaitToken token = {0};
        if (!xr_xir_domain_work(executor->config.domain, 1)) permitted = false;
        if (!xr_xir_call_task_wait_token(executor->root.call, &token)) return;
    }
    for (XirTask *task = executor->active; task; task = task->active_next) {
        if (!xr_xir_domain_work(executor->config.domain, 1)) permitted = false;
        XrXirTaskWaitToken token = {0};
        /* A real external request keeps its host wake source. Only a set made
         * entirely of internal dependencies is closed without runnable work. */
        if (!xr_xir_call_task_wait_token(task->call, &token)) return;
    }
    XrXirCallStatus reason = permitted ? XR_XIR_CALL_BAD_STATE : XR_XIR_CALL_LIMIT;
    if (root_waiting) {
        XrXirTaskWaitToken token = {0};
        XR_CHECK(xr_xir_call_task_wait_token(executor->root.call, &token) &&
            xr_xir_call_fail_task_wait(executor->root.call, &token, reason) == XR_XIR_CALL_READY,
            "closed root wait leaves no unowned target or borrowed frame");
        task_core_record_failure(executor, &executor->root);
        task_core_enqueue(executor, &executor->root);
    }
    for (XirTask *task = executor->active; task; task = task->active_next) {
        XrXirTaskWaitToken token = {0};
        XR_CHECK(xr_xir_call_task_wait_token(task->call, &token) &&
            xr_xir_call_fail_task_wait(task->call, &token, reason) == XR_XIR_CALL_READY,
            "closed dependency rejection unlinks every charged waiter before release");
        task_core_record_failure(executor, &task->activation);
        task_core_enqueue(executor, &task->activation);
    }
}
XR_FUNC XrXirCallStatus xr_xir_task_executor_poll(XrXirTaskExecutor *executor, uint64_t quantum) {
    if (!executor || !quantum) return XR_XIR_CALL_BAD_ARGUMENT;
    if (executor->driving) return XR_XIR_CALL_BUSY;
    executor->driving = true;
    while (quantum-- && executor->ready_head) {
        XirTaskActivation *activation = task_core_dequeue(executor);
        XirTask *task = activation->task;
        if (task) task->state = XIR_TASK_RUNNING;
        executor->current = activation;
        if (executor->stopping) (void)xr_xir_call_request_cancel(activation->call);
        XrXirCallResult result = xr_xir_call_poll_bounded(activation->call, 1);
        task_core_record_failure(executor, activation);
        executor->current = NULL;
        if (result.status == XR_XIR_CALL_READY) task_core_enqueue(executor, activation);
        else if (result.status == XR_XIR_CALL_SUSPENDED) {
            XrXirWaitRequest wait = {0};
            XR_CHECK(xr_xir_call_wait_request(activation->call, result.wake, &wait) == XR_XIR_CALL_READY,
                "suspended task owns one canonical wait request");
            if (task && wait.kind == XR_XIR_WAIT_YIELD) {
                XR_CHECK(xr_xir_call_resume(activation->call, result.wake) == XR_XIR_CALL_READY,
                    "the executor resumes its own exact yield token");
                task_core_enqueue(executor, activation);
            }
            else if (wait.kind == XR_XIR_WAIT_TASK) task_core_register_wait(executor, activation->call);
        } else if (task) task_core_finish_task(executor, task);
    }
    task_core_reject_closed_wait(executor);
    executor->driving = false;
    bool root_waiting = executor->root.call && xr_xir_call_state(executor->root.call) == XR_XIR_CALL_SUSPENDED;
    return executor->ready_head ? XR_XIR_CALL_READY : executor->active || root_waiting ? XR_XIR_CALL_SUSPENDED : XR_XIR_CALL_RETURNED;
}
static bool task_core_external_pending(const XrXirTaskExecutor *executor) {
    if (!executor->external || !executor->external->call) return false;
    XrXirWaitRequest request = {0};
    return xr_xir_call_wait_request(executor->external->call, executor->external_call_wake, &request) == XR_XIR_CALL_READY &&
        (request.kind == XR_XIR_WAIT_YIELD || request.kind == XR_XIR_WAIT_TIMER_MS) &&
        !request.subject && !request.generation && !request.ticket;
}
static bool task_core_select_external(XrXirTaskExecutor *executor, XirTaskActivation *activation) {
    if (!activation->call || xr_xir_call_state(activation->call) != XR_XIR_CALL_SUSPENDED) return false;
    XrXirCallResult state = xr_xir_call_poll_bounded(activation->call, 1);
    XrXirWaitRequest request = {0};
    if (xr_xir_call_wait_request(activation->call, state.wake, &request) != XR_XIR_CALL_READY ||
        (request.kind != XR_XIR_WAIT_YIELD && request.kind != XR_XIR_WAIT_TIMER_MS)) return false;
    if (executor->host_wake == UINT64_MAX) return false;
    executor->external = activation; executor->external_call_wake = state.wake; ++executor->host_wake;
    return true;
}
XR_FUNC XrXirCallResult xr_xir_task_executor_poll_root(XrXirTaskExecutor *executor, uint64_t quantum) {
    if (!executor || !executor->root.call || !quantum) return (XrXirCallResult){.status = XR_XIR_CALL_BAD_ARGUMENT};
    XrXirCallStatus driven = xr_xir_task_executor_poll(executor, quantum);
    if (driven == XR_XIR_CALL_BUSY) return (XrXirCallResult){.status = driven};
    if (executor->ready_head) return (XrXirCallResult){.status = XR_XIR_CALL_READY};
    if (driven == XR_XIR_CALL_RETURNED) {
        XrXirCallStatus completed = xr_xir_task_executor_completion_status(executor);
        if (completed != XR_XIR_CALL_READY) return (XrXirCallResult){.status = completed};
        return xr_xir_call_poll_bounded(executor->root.call, 1);
    }
    if (!task_core_external_pending(executor)) {
        executor->external = NULL; executor->external_call_wake = 0;
        if (!task_core_select_external(executor, &executor->root)) {
            for (XirTask *task = executor->active; task; task = task->active_next)
                if (task_core_select_external(executor, &task->activation)) break;
        }
    }
    if (!executor->external) return (XrXirCallResult){.status = XR_XIR_CALL_BAD_STATE};
    return (XrXirCallResult){.status = XR_XIR_CALL_SUSPENDED, .wake = executor->host_wake};
}
XR_FUNC XrXirCallStatus xr_xir_task_executor_root_wait(const XrXirTaskExecutor *executor, uint64_t epoch, uint64_t wake,
    XrXirWaitRequest *output) {
    if (!executor || !output) return XR_XIR_CALL_BAD_ARGUMENT;
    if (executor->driving) return XR_XIR_CALL_BUSY;
    if (!epoch || epoch != executor->root_epoch || !wake || wake != executor->host_wake ||
        !task_core_external_pending(executor)) return XR_XIR_CALL_BAD_STATE;
    return xr_xir_call_wait_request(executor->external->call, executor->external_call_wake, output);
}
XR_FUNC XrXirCallStatus xr_xir_task_executor_resume_root(XrXirTaskExecutor *executor, uint64_t epoch, uint64_t wake) {
    if (!executor) return XR_XIR_CALL_BAD_ARGUMENT;
    if (executor->driving) return XR_XIR_CALL_BUSY;
    if (executor->stopping || !epoch || epoch != executor->root_epoch || !wake || wake != executor->host_wake ||
        !task_core_external_pending(executor))
        return XR_XIR_CALL_BAD_STATE;
    XirTaskActivation *activation = executor->external;
    XrXirCallStatus status = xr_xir_call_resume(activation->call, executor->external_call_wake);
    if (status != XR_XIR_CALL_READY) return status;
    executor->external = NULL; executor->external_call_wake = 0;
    task_core_enqueue(executor, activation);
    return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xr_xir_task_executor_cancel_root(XrXirTaskExecutor *executor) {
    if (!executor || !executor->root.call) return XR_XIR_CALL_BAD_ARGUMENT;
    if (executor->driving) return XR_XIR_CALL_BUSY;
    XrXirCallStatus status = xr_xir_call_request_cancel(executor->root.call);
    if (status != XR_XIR_CALL_CANCEL_REQUESTED) return status;
    if (executor->external == &executor->root) {
        executor->external = NULL; executor->external_call_wake = 0;
    }
    if (!executor->root.queued) task_core_enqueue(executor, &executor->root);
    return status;
}
XR_FUNC XrXirCallStatus xr_xir_task_executor_stop(XrXirTaskExecutor *executor) {
    if (!executor) return XR_XIR_CALL_BAD_ARGUMENT;
    executor->stopping = true;
    executor->external = NULL; executor->external_call_wake = 0;
    XrXirCallStatus final = XR_XIR_CALL_CANCEL_REQUESTED;
    if (executor->root.call) {
        XrXirCallStatus status = xr_xir_call_request_cancel(executor->root.call);
        if (status == XR_XIR_CALL_BUSY) final = status;
        if (status == XR_XIR_CALL_CANCEL_REQUESTED && !executor->root.queued && executor->current != &executor->root)
            task_core_enqueue(executor, &executor->root);
    }
    for (XirTask *task = executor->active; task; task = task->active_next) {
        XrXirCallStatus status = xr_xir_call_request_cancel(task->call);
        XR_CHECK(status == XR_XIR_CALL_CANCEL_REQUESTED || status == XR_XIR_CALL_BAD_STATE || status == XR_XIR_CALL_BUSY,
            "a stopped executor only cancels its own canonical calls");
        if (status == XR_XIR_CALL_BUSY) final = status;
        if (!task->activation.queued && executor->current != &task->activation)
            task_core_enqueue(executor, &task->activation);
    }
    return final;
}
XR_FUNC XrXirCallStatus xr_xir_task_executor_free(XrXirTaskExecutor *executor, XrXirCallBudget *accounting) {
    if (!executor) return XR_XIR_CALL_READY;
    if (executor->driving) return XR_XIR_CALL_BUSY;
    (void)xr_xir_task_executor_stop(executor);
    XrXirDomainBudgetStats work = xr_xir_domain_budget_stats(executor->config.domain);
    uint64_t tasks = 0;
    for (XirTask *task = executor->active; task; task = task->active_next) ++tasks;
    if (executor->root.call) ++tasks;
    uint64_t releases = executor->budget->release_tickets - executor->budget->released_frames;
    uint64_t remaining = work.work_limit - work.work;
    XR_CHECK(releases < UINT64_MAX - remaining && tasks < UINT64_MAX - remaining - releases,
        "finite drain has a representable prepaid bound");
    remaining += releases + tasks;
    while (executor->active || executor->ready_head) {
        XR_CHECK(remaining, "task shutdown stays inside its prepaid release and work bound");
        --remaining;
        (void)xr_xir_task_executor_poll(executor, 1);
    }
    XrXirTaskExecutorConfig config = executor->config;
    XrXirCallStatus status = task_core_completion_status(executor);
    if (accounting) *accounting = *executor->budget;
    xr_xir_domain_metadata_deallocate(config.domain, executor, sizeof(*executor));
    xr_xir_compile_type_arena_drop(config.task_arena);
    xr_xir_compile_program_drop(config.program);
    xr_xir_domain_drop(config.domain);
    return status;
}
