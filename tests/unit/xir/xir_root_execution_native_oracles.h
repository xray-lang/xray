/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_root_execution_native_oracles.h - Independent native execution outcomes
 *
 * KEY CONCEPT:
 *   Independent typed outcomes observe actual generated native activations.
 */
#ifndef XIR_ROOT_EXECUTION_NATIVE_ORACLES_H
#define XIR_ROOT_EXECUTION_NATIVE_ORACLES_H

typedef struct RnRawCall {
    XrXirCall *call;
    XrXirDomain *domain;
    XrXirTypeArena *arena;
    XrXirCallAccounting accounting;
    XrXirDomainStats domain_baseline;
    size_t domain_before_live, domain_before_bytes;
    size_t compile_before_live, compile_before_bytes;
} RnRawCall;

typedef struct RnObserver {
    XrXirInstance *instance;
    RnRawCall *raw;
    uint32_t entry;
    int64_t values[8];
    unsigned count, busy_callbacks, raw_callbacks;
    XrXirLifecycleEvent events[8];
    uint32_t indices[8];
    unsigned trace_count;
    bool reenter, observe_failure;
} RnObserver;

static void rn_empty_value(const XrXirValue *value) {
    CHECK(!value->type && !value->reserved && !value->payload);
}

static XrXirCallResult rn_raw_poll(XrXirCall *call) {
    for (unsigned i = 0; i < 65536; ++i) {
        XrXirCallResult result = xr_xir_call_poll_bounded(call, 1);
        if (result.status != XR_XIR_CALL_READY) return result;
    }
    CHECK(false);
    return (XrXirCallResult){0};
}

static void rn_unbound_fault(RnRawCall *raw) {
    XrXirCallResult result = rn_raw_poll(raw->call);
    if (result.status != XR_XIR_CALL_BAD_STATE) fprintf(stderr,
        "ROOT_NATIVE_UNBOUND status=%u code=%u\n", (unsigned)result.status, result.panic.detail.code);
    CHECK(result.status == XR_XIR_CALL_BAD_STATE && !result.wake && xr_xir_panic_empty(&result.panic));
    rn_empty_value(&result.value);
}

static void rn_busy_output(RnObserver *observer) {
    XrXirInstance *instance = observer->instance;
    CHECK(xr_xir_instance_start(instance, observer->entry, NULL, 0) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_poll_bounded(instance, 1).outcome.status == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_resume(instance, 0, 0) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_cancel_current(instance) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_BUSY);
    ++observer->busy_callbacks;
}

static XrXirOutputStatus rn_output(void *context, const XrXirOutputGroup *group) {
    RnObserver *observer = context;
    CHECK(group && group->stream == XR_XIR_STDOUT && group->line && group->count == 1 && group->values);
    CHECK(observer->count < 8 && group->values[0].type == XR_XIR_I64 && !group->values[0].reserved);
    observer->values[observer->count++] = group->values[0].payload;
    if (observer->reenter) rn_busy_output(observer);
    if (observer->raw) {
        /* The real instance is driving; only this second real Call lacks binding. */
        CHECK(observer->instance->driving && !observer->instance->observing);
        rn_unbound_fault(observer->raw);
        ++observer->raw_callbacks;
    }
    return XR_XIR_OUTPUT_OK;
}

static void rn_trace(void *context, XrXirLifecycleEvent event, uint32_t index) {
    RnObserver *observer = context;
    CHECK(observer->trace_count < 8);
    observer->events[observer->trace_count] = event;
    observer->indices[observer->trace_count++] = index;
    if (!observer->observe_failure) return;
    CHECK(observer->instance && observer->instance->observing);
    CHECK(xr_xir_instance_start(observer->instance, observer->entry, NULL, 0) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_cancel_current(observer->instance) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_stop(observer->instance) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_free(observer->instance) == XR_XIR_CALL_BUSY);
}

static void rn_instances(const XrXirCompileContext *context, const XrXirProgramSpec *spec,
    uint32_t entry, RnObserver observers[2], XrXirInstance *instances[2]) {
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(context, spec, &program) == XR_XIR_OK);
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, rn_output, &observers[i]};
        config.trace = rn_trace;
        config.trace_context = &observers[i];
        observers[i].entry = entry;
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
        observers[i].instance = instances[i];
    }
    CHECK(instances[0]->domain != instances[1]->domain);
    xr_xir_compile_program_drop(program);
}

static XrXirInstanceResult rn_poll(XrXirInstance *instance) {
    for (unsigned i = 0; i < 65536; ++i) {
        XrXirInstanceResult result = xr_xir_instance_poll_bounded(instance, 1);
        if (result.outcome.status != XR_XIR_CALL_READY) return result;
    }
    CHECK(false);
    return (XrXirInstanceResult){0};
}

static void rn_start(XrXirInstance *instance, uint32_t entry) {
    XrXirCallStatus status = xr_xir_instance_start(instance, entry, NULL, 0);
    if (status != XR_XIR_CALL_READY) fprintf(stderr, "ROOT_NATIVE_START entry=%u status=%u state=%u\n",
        entry, (unsigned)status, (unsigned)xr_xir_instance_state(instance));
    CHECK(status == XR_XIR_CALL_READY);
}

static XrXirValue rn_take(XrXirInstance *instance) {
    XrXirInstanceResult result = rn_poll(instance);
    if (result.outcome.status != XR_XIR_CALL_RETURNED) fprintf(stderr,
        "ROOT_NATIVE_RETURN status=%u code=%u epoch=%llu\n", (unsigned)result.outcome.status,
        result.outcome.panic.detail.code, (unsigned long long)result.epoch);
    CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && !result.outcome.wake &&
        xr_xir_panic_empty(&result.outcome.panic));
    XrXirValue occupied = {XR_XIR_I64, 0, 777}, preserved = occupied;
    CHECK(xr_xir_instance_take_result(instance, &occupied) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!memcmp(&occupied, &preserved, sizeof(occupied)));
    XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED && xr_xir_value_valid(&value));
    return value;
}

static void rn_scalar_value(XrXirValue *value, int64_t expected) {
    CHECK(value->type == XR_XIR_I64 && !value->reserved && value->payload == expected);
    xr_xir_value_drop(value);
}

static void rn_scalar(XrXirInstance *instance, uint32_t entry, int64_t expected) {
    rn_start(instance, entry);
    XrXirValue value = rn_take(instance);
    rn_scalar_value(&value, expected);
}

static void rn_function_scalar(XrXirInstance *instance, const XrXirValue *function, int64_t expected) {
    CHECK(xr_xir_instance_start_function(instance, function, NULL, 0) == XR_XIR_CALL_READY);
    XrXirValue value = rn_take(instance);
    rn_scalar_value(&value, expected);
}

static void rn_string(const XrXirValue *value, const char *expected, size_t count) {
    const char *bytes = NULL;
    size_t length = 0;
    CHECK(value->type == XR_XIR_STRING && !value->reserved && xr_xir_string_view(value, &bytes, &length));
    CHECK(length == count && !memcmp(bytes, expected, count));
}

static void rn_raw_new(const XrXirCompileContext *context, const XrXirProgramSpec *spec,
    XrXirInstance *instance, uint32_t entry, RnRawCall *raw) {
    raw->compile_before_live = instance_compile_live;
    raw->compile_before_bytes = instance_compile_bytes;
    CHECK(xr_xir_compile_type_arena_new(context, spec->types, &raw->arena) == XR_XIR_VALUE_OK);
    raw->domain_before_live = runtime_live;
    raw->domain_before_bytes = runtime_bytes;
    CHECK(xr_xir_domain_new(UINT64_C(1048576), &raw->domain) == XR_XIR_VALUE_OK);
    raw->domain_baseline = xr_xir_domain_stats(raw->domain);
    CHECK(raw->domain_baseline.live_bytes == sizeof(XrXirDomain) &&
        raw->domain_baseline.peak_bytes == raw->domain_baseline.live_bytes &&
        raw->domain_baseline.allocations == 1 && !raw->domain_baseline.frees && !raw->domain_baseline.reallocations);
    CHECK(runtime_live == raw->domain_before_live + 1 &&
        runtime_bytes == raw->domain_before_bytes + raw->domain_baseline.live_bytes);
    XrXirCallConfig config;
    CHECK(xr_xir_call_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.entries = spec->entries;
    config.entry_count = spec->entry_count;
    config.instance = instance;
    config.accounting = &raw->accounting;
    config.admission = (XrXirValueAdmission){raw->arena, raw->domain, NULL, NULL, 1000000, 1048576};
    CHECK(xr_xir_call_new(&config, entry, NULL, 0, &raw->call) == XR_XIR_CALL_READY);
}

static void rn_raw_domain_baseline(const RnRawCall *raw) {
    XrXirDomainStats stats = xr_xir_domain_stats(raw->domain);
    CHECK(stats.live_bytes == raw->domain_baseline.live_bytes && stats.peak_bytes == raw->domain_baseline.peak_bytes &&
        stats.allocations == raw->domain_baseline.allocations && stats.frees == raw->domain_baseline.frees &&
        stats.reallocations == raw->domain_baseline.reallocations);
    XrXirDomainBudgetStats budget = xr_xir_domain_budget_stats(raw->domain);
    CHECK(!budget.metadata_live && !budget.call_live && budget.metadata_allocations == budget.metadata_frees &&
        budget.call_allocations == budget.call_frees);
    CHECK(runtime_live == raw->domain_before_live + 1 &&
        runtime_bytes == raw->domain_before_bytes + raw->domain_baseline.live_bytes);
}

static void rn_raw_free(RnRawCall *raw) {
    CHECK(xr_xir_call_free(raw->call) == XR_XIR_CALL_READY);
    CHECK(!raw->accounting.live_bytes && !raw->accounting.depth &&
        raw->accounting.allocations == raw->accounting.frees);
    rn_raw_domain_baseline(raw);
    size_t attempts = runtime_attempts;
    CHECK(xr_xir_domain_retain(raw->domain));
    /* One lease is dropped while the retained lease still owns the domain. */
    xr_xir_domain_drop(raw->domain);
    rn_raw_domain_baseline(raw);
    CHECK(runtime_attempts == attempts);
    xr_xir_domain_drop(raw->domain);
    CHECK(runtime_live == raw->domain_before_live && runtime_bytes == raw->domain_before_bytes);
    xr_xir_compile_type_arena_drop(raw->arena);
    CHECK(instance_compile_live == raw->compile_before_live && instance_compile_bytes == raw->compile_before_bytes);
    *raw = (RnRawCall){0};
}

static void rn_identity(const XrXirCompileContext *context) {
    RnObserver observers[2] = {0};
    XrXirInstance *instances[2] = {0};
    rn_instances(context, &rn_identity_program, rn_identity_ids[0], observers, instances);
    XrXirValue functions[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        rn_scalar(instances[i], rn_identity_ids[0], 42);
        rn_start(instances[i], rn_identity_ids[2]);
        functions[i] = rn_take(instances[i]);
        CHECK(xr_xir_callable_signature(xr_xir_compile_type_arena_types(xr_xir_value_arena(&functions[i])),
            (XrXirType)functions[i].type));
    }
    CHECK(functions[0].payload != functions[1].payload);
    size_t attempts = runtime_attempts;
    CHECK(xr_xir_instance_start_function(instances[1], &functions[0], NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(runtime_attempts == attempts && !observers[0].count && !observers[1].count);
    rn_scalar(instances[1], rn_identity_ids[1], 42);
    rn_function_scalar(instances[0], &functions[0], 44);
    rn_function_scalar(instances[0], &functions[0], 46);
    rn_function_scalar(instances[1], &functions[1], 44);
    RnRawCall raw = {0};
    rn_raw_new(context, &rn_identity_program, instances[0], rn_identity_ids[1], &raw);
    CHECK(!instances[0]->driving);
    rn_unbound_fault(&raw);
    rn_raw_free(&raw);
    rn_raw_new(context, &rn_identity_program, instances[0], rn_identity_ids[1], &raw);
    observers[0].raw = &raw;
    rn_scalar(instances[0], rn_identity_ids[3], 46);
    CHECK(observers[0].count == 1 && observers[0].values[0] == 46 && observers[0].raw_callbacks == 1 &&
        !observers[1].count);
    observers[0].raw = NULL;
    rn_raw_free(&raw);
    CHECK(xr_xir_instance_stop(instances[0]) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start_function(instances[0], &functions[0], NULL, 0) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_start_function(instances[1], &functions[0], NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
    rn_scalar(instances[1], rn_identity_ids[1], 44);
    for (unsigned i = 0; i < 2; ++i) CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    CHECK(runtime_live && runtime_bytes);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_value_valid(&functions[i]));
        xr_xir_value_drop(&functions[i]);
    }
    CHECK(!runtime_live && !runtime_bytes);
}

static void rn_pending(XrXirInstance *instance, RnObserver *observer, XrXirInstanceResult result) {
    CHECK(result.outcome.status == XR_XIR_CALL_SUSPENDED && result.epoch && result.outcome.wake &&
        xr_xir_panic_empty(&result.outcome.panic));
    rn_empty_value(&result.outcome.value);
    CHECK(result.epoch < UINT64_MAX && result.outcome.wake < UINT64_MAX);
    XrXirWaitRequest sentinel, preserved;
    memset(&sentinel, 0xA5, sizeof(sentinel));
    preserved = sentinel;
    CHECK(xr_xir_instance_wait_request(instance, result.epoch + 1, result.outcome.wake, &sentinel) == XR_XIR_CALL_BAD_STATE);
    CHECK(!memcmp(&sentinel, &preserved, sizeof(sentinel)));
    CHECK(xr_xir_instance_wait_request(instance, result.epoch, result.outcome.wake + 1, &sentinel) == XR_XIR_CALL_BAD_STATE);
    CHECK(!memcmp(&sentinel, &preserved, sizeof(sentinel)));
    CHECK(xr_xir_instance_wait_request(instance, 0, 0, &sentinel) == XR_XIR_CALL_BAD_STATE);
    CHECK(!memcmp(&sentinel, &preserved, sizeof(sentinel)));
    XrXirWaitRequest request = {0};
    CHECK(xr_xir_instance_wait_request(instance, result.epoch, result.outcome.wake, &request) == XR_XIR_CALL_READY);
    CHECK(request.kind == XR_XIR_WAIT_YIELD && !request.reserved && !request.after_ms && !request.subject &&
        !request.generation && !request.ticket);
    CHECK(xr_xir_instance_resume(instance, result.epoch + 1, result.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(instance, result.epoch, result.outcome.wake + 1) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(instance, 0, 0) == XR_XIR_CALL_BAD_STATE);
    unsigned count = observer->count;
    XrXirInstanceResult repeat = xr_xir_instance_poll_bounded(instance, 1);
    CHECK(repeat.outcome.status == XR_XIR_CALL_SUSPENDED && repeat.epoch == result.epoch &&
        repeat.outcome.wake == result.outcome.wake && observer->count == count);
}

static void rn_resume_once(XrXirInstance *instance, RnObserver *observer, XrXirInstanceResult result) {
    rn_pending(instance, observer, result);
    CHECK(xr_xir_instance_resume(instance, result.epoch, result.outcome.wake) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_resume(instance, result.epoch, result.outcome.wake) == XR_XIR_CALL_BAD_STATE);
}

static void rn_cancelled(XrXirInstance *instance, const RnObserver *observer) {
    XrXirInstanceResult result = rn_poll(instance);
    CHECK(result.outcome.status == XR_XIR_CALL_CANCELLED && !result.outcome.wake &&
        xr_xir_panic_empty(&result.outcome.panic));
    rn_empty_value(&result.outcome.value);
    size_t attempts = runtime_attempts;
    unsigned outputs = observer->count, traces = observer->trace_count;
    XrXirValue sentinel = {XR_XIR_I64, 0, 555}, preserved = sentinel;
    CHECK(xr_xir_instance_take_result(instance, &sentinel) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!memcmp(&sentinel, &preserved, sizeof(sentinel)));
    XrXirValue empty = {0};
    CHECK(xr_xir_instance_take_result(instance, &empty) == XR_XIR_CALL_BAD_STATE);
    rn_empty_value(&empty);
    XrXirInstanceResult repeat = xr_xir_instance_poll_bounded(instance, 1);
    CHECK(repeat.outcome.status == XR_XIR_CALL_CANCELLED && repeat.epoch == result.epoch &&
        !repeat.outcome.wake && xr_xir_panic_empty(&repeat.outcome.panic));
    rn_empty_value(&repeat.outcome.value);
    CHECK(runtime_attempts == attempts && observer->count == outputs && observer->trace_count == traces);
}

static void rn_resume_case(const XrXirCompileContext *context) {
    RnObserver observers[2] = {0};
    XrXirInstance *instances[2] = {0};
    rn_instances(context, &rn_resume_program, rn_resume_ids[0], observers, instances);
    XrXirInstanceResult first[2];
    for (unsigned i = 0; i < 2; ++i) {
        rn_start(instances[i], rn_resume_ids[0]);
        first[i] = rn_poll(instances[i]);
        CHECK(observers[i].count == 1 && observers[i].values[0] == 1);
    }
    rn_resume_once(instances[0], &observers[0], first[0]);
    XrXirInstanceResult second = rn_poll(instances[0]);
    CHECK(second.epoch == first[0].epoch && second.outcome.wake != first[0].outcome.wake &&
        observers[0].count == 2 && observers[0].values[1] == 11 && observers[1].count == 1);
    rn_pending(instances[1], &observers[1], first[1]);
    rn_resume_once(instances[0], &observers[0], second);
    XrXirValue owned[2] = {rn_take(instances[0]), {0}};
    rn_string(&owned[0], "root-result", 11);
    rn_start(instances[0], rn_resume_ids[0]);
    XrXirInstanceResult newer = rn_poll(instances[0]);
    CHECK(newer.epoch > first[0].epoch && newer.outcome.status == XR_XIR_CALL_SUSPENDED &&
        observers[0].count == 3 && observers[0].values[2] == 12);
    CHECK(xr_xir_instance_resume(instances[0], second.epoch, second.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_cancel_current(instances[0]) == XR_XIR_CALL_CANCEL_REQUESTED);
    rn_cancelled(instances[0], &observers[0]);
    CHECK(observers[0].count == 3 && observers[1].count == 1);
    rn_resume_once(instances[1], &observers[1], first[1]);
    XrXirInstanceResult other_second = rn_poll(instances[1]);
    CHECK(observers[1].count == 2 && observers[1].values[1] == 11);
    rn_resume_once(instances[1], &observers[1], other_second);
    owned[1] = rn_take(instances[1]);
    CHECK(owned[0].payload != owned[1].payload);
    rn_scalar(instances[0], rn_resume_ids[1], 12);
    rn_scalar(instances[1], rn_resume_ids[1], 11);
    for (unsigned i = 0; i < 2; ++i) CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    CHECK(runtime_live && runtime_bytes);
    for (unsigned i = 0; i < 2; ++i) {
        rn_string(&owned[i], "root-result", 11);
        xr_xir_value_drop(&owned[i]);
    }
    CHECK(!runtime_live && !runtime_bytes);
}

static void rn_closing_pair(const XrXirCompileContext *context, unsigned mode) {
    RnObserver observers[2] = {0};
    XrXirInstance *instances[2] = {0};
    observers[0].reenter = true;
    observers[1].reenter = true;
    rn_instances(context, &rn_closing_program, rn_closing_ids[0], observers, instances);
    XrXirInstanceResult suspended[2];
    for (unsigned i = 0; i < 2; ++i) {
        rn_start(instances[i], rn_closing_ids[0]);
        suspended[i] = rn_poll(instances[i]);
        CHECK(suspended[i].outcome.status == XR_XIR_CALL_SUSPENDED && observers[i].count == 1 &&
            observers[i].values[0] == 1 && observers[i].busy_callbacks == 1);
    }
    if (!mode) {
        CHECK(xr_xir_instance_cancel_current(instances[0]) == XR_XIR_CALL_CANCEL_REQUESTED);
        rn_cancelled(instances[0], &observers[0]);
        CHECK(xr_xir_instance_poll_bounded(instances[0], 1).outcome.status == XR_XIR_CALL_CANCELLED);
        rn_scalar(instances[0], rn_closing_ids[1], 101);
    } else if (mode == 1) {
        CHECK(xr_xir_instance_stop(instances[0]) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_stop(instances[0]) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instances[0], rn_closing_ids[1], NULL, 0) == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_resume(instances[0], suspended[0].epoch, suspended[0].outcome.wake) == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_poll_bounded(instances[0], 1).outcome.status == XR_XIR_CALL_CANCELLED);
    } else {
        CHECK(xr_xir_instance_free(instances[0]) == XR_XIR_CALL_READY);
        instances[0] = NULL;
        observers[0].instance = NULL;
    }
    CHECK(observers[0].count == 2 && observers[0].values[1] == 101 && observers[0].busy_callbacks == 2 &&
        observers[1].count == 1);
    rn_resume_once(instances[1], &observers[1], suspended[1]);
    XrXirValue owned = rn_take(instances[1]);
    CHECK(observers[1].count == 2 && observers[1].values[1] == 111 && observers[1].busy_callbacks == 2);
    rn_scalar(instances[1], rn_closing_ids[1], 111);
    if (instances[0]) CHECK(xr_xir_instance_free(instances[0]) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_free(instances[1]) == XR_XIR_CALL_READY);
    CHECK(observers[0].count == 2 && observers[1].count == 2 && runtime_live && runtime_bytes);
    rn_string(&owned, "root-result", 11);
    xr_xir_value_drop(&owned);
    CHECK(!runtime_live && !runtime_bytes);
}

static void rn_closing_case(const XrXirCompileContext *context) {
    for (unsigned mode = 0; mode < 3; ++mode) rn_closing_pair(context, mode);
}

static void rn_failure_payload(const XrXirInstanceResult *result) {
    CHECK(result->outcome.status == XR_XIR_CALL_ASSERTION && result->epoch && !result->outcome.wake);
    rn_empty_value(&result->outcome.value);
    const XrXirFaultDetail *detail = &result->outcome.panic.detail;
    CHECK(detail->code == XR_XIR_PANIC_ASSERTION && !detail->reserved && !detail->index && !detail->length);
    rn_string(&result->outcome.panic.message, "root-init-failure", 17);
}

static void rn_failure_case(const XrXirCompileContext *context) {
    RnObserver observers[2] = {0};
    XrXirInstance *instances[2] = {0};
    observers[0].observe_failure = true;
    observers[1].observe_failure = true;
    rn_instances(context, &rn_init_failure_program, rn_init_failure_ids[0], observers, instances);
    const XrXirDeclarations *declarations = rn_init_failure_program.declarations;
    CHECK(declarations->module_count == 2 && declarations->modules[declarations->root_module].dependency_count == 1);
    uint32_t core = declarations->modules[declarations->root_module].dependencies[0];
    CHECK(core < declarations->module_count && core != declarations->root_module);
    XrXirCallResult owned[2] = {0};
    XrXirInstanceResult first[2];
    for (unsigned i = 0; i < 2; ++i) {
        rn_start(instances[i], rn_init_failure_ids[0]);
        first[i] = rn_poll(instances[i]);
        rn_failure_payload(&first[i]);
        CHECK(xr_xir_instance_state(instances[i]) == XR_XIR_INSTANCE_FAILED && !observers[i].count);
        CHECK(observers[i].trace_count == 5 && observers[i].events[0] == XR_XIR_MODULE_BEGIN &&
            observers[i].indices[0] == core && observers[i].events[1] == XR_XIR_MODULE_READY &&
            observers[i].indices[1] == core && observers[i].events[2] == XR_XIR_MODULE_BEGIN &&
            observers[i].indices[2] == declarations->root_module &&
            observers[i].events[3] == XR_XIR_SLOT_PUBLISHED && observers[i].indices[3] == 0 &&
            observers[i].events[4] == XR_XIR_SLOT_RELEASED && observers[i].indices[4] == 0);
        size_t attempts = runtime_attempts;
        for (unsigned repeat = 0; repeat < 24; ++repeat) {
            CHECK(xr_xir_instance_start(instances[i], rn_init_failure_ids[0], NULL, 0) == XR_XIR_CALL_ASSERTION);
            XrXirInstanceResult sticky = xr_xir_instance_poll_bounded(instances[i], 1);
            rn_failure_payload(&sticky);
            CHECK(sticky.epoch == first[i].epoch && sticky.outcome.panic.message.payload == first[i].outcome.panic.message.payload &&
                runtime_attempts == attempts && observers[i].trace_count == 5 && !observers[i].count);
            XrXirValue sentinel = {XR_XIR_I64, 0, 888}, preserved = sentinel;
            CHECK(xr_xir_instance_take_result(instances[i], &sentinel) == XR_XIR_CALL_BAD_STATE);
            CHECK(!memcmp(&sentinel, &preserved, sizeof(sentinel)) && runtime_attempts == attempts);
        }
        CHECK(xr_xir_instance_copy_failure(instances[i], &owned[i]) == XR_XIR_CALL_ASSERTION);
        CHECK(owned[i].panic.message.payload == first[i].outcome.panic.message.payload);
    }
    CHECK(owned[0].panic.message.payload != owned[1].panic.message.payload);
    for (unsigned i = 0; i < 2; ++i) CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    CHECK(runtime_live && runtime_bytes);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(observers[i].trace_count == 5 && !observers[i].count && xr_xir_call_result_valid(&owned[i]));
        rn_string(&owned[i].panic.message, "root-init-failure", 17);
        xr_xir_call_result_drop(&owned[i]);
    }
    CHECK(!runtime_live && !runtime_bytes);
}
#endif // XIR_ROOT_EXECUTION_NATIVE_ORACLES_H
