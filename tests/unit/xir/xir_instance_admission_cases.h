/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_instance_admission_cases.h - Untrusted gate and rejected-call isolation
 *
 * KEY CONCEPT:
 *   Rejected admission preserves the previous result and every physical owner.
 */
#ifndef XIR_INSTANCE_ADMISSION_CASES_H
#define XIR_INSTANCE_ADMISSION_CASES_H

static void admission_case_setup(unsigned *releases, XrXirProgram **program,
                                 XrXirInstance **instance, XrXirValue *function) {
    fail_at = SIZE_MAX;
    CHECK(!live);
    CHECK(function_case_seal(releases, program) == XR_XIR_OK);
    XrXirInstanceConfig config = xr_xir_instance_defaults();
    CHECK(xr_xir_instance_new(*program, &config, instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(*instance, 2, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(*instance).outcome.status == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(*instance, function) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_start(*instance, 1, NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult previous = xr_xir_instance_poll(*instance);
    CHECK(previous.outcome.status == XR_XIR_CALL_RETURNED && previous.outcome.value.payload == 17);
}

static void admission_case_dispose(unsigned *releases, XrXirProgram *program,
                                   XrXirInstance *instance, XrXirValue *function) {
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    xr_xir_program_drop(program);
    CHECK(!*releases && xr_xir_value_valid(function));
    xr_xir_value_drop(function);
    CHECK(*releases == 1 && !live);
}

static void admission_gate_owner_case(void) {
    unsigned releases = 0;
    XrXirProgram *program = NULL;
    XrXirInstance *instance = NULL;
    XrXirValue function = {0};
    admission_case_setup(&releases, &program, &instance, &function);
    XirFunction *object = (XirFunction *) object_pointer(&function);
    XrXirFunctionBinding original = object->binding;
    FunctionGate forged;
    atomic_init(&forged.references, 1);
    forged.program = program; forged.instance = instance;
    void *owners[] = {&forged, (void *) (uintptr_t) 1};
    uint64_t epoch = instance->epoch;
    XrXirCall *previous_call = instance->call;
    size_t baseline = live;
    uint32_t gate_references = atomic_load(&instance->function_gate->references);
    for (size_t i = 0; i < sizeof(owners) / sizeof(*owners); ++i) {
        /* The release and entry are copied from a real binding. Neither grants
         * authority to a copied gate or makes an arbitrary owner dereferenceable. */
        object->binding.owner = owners[i];
        CHECK(xr_xir_value_valid(&function));
        CHECK(xr_xir_instance_start_function(instance, &function, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(xr_xir_instance_start(instance, 4, &function, 1) == XR_XIR_CALL_BAD_ARGUMENT);
        XrXirValueAdmission admission = instance_admission(instance);
        XrXirValue unpublished = {0};
        CHECK(xr_xir_function_new(instance->domain, program->arena, (XrXirType) function.type,
            &object->binding, &admission, &unpublished) == XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(!unpublished.type && !unpublished.reserved && !unpublished.payload);
        CHECK(instance->call == previous_call && instance->epoch == epoch &&
            xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
        CHECK(atomic_load(&forged.references) == 1 &&
            atomic_load(&instance->function_gate->references) == gate_references && live == baseline);
    }
    object->binding = original;
    XrXirValue result = {0};
    CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
    CHECK(result.type == XR_XIR_I64 && result.payload == 17);
    xr_xir_value_drop(&result);
    admission_case_dispose(&releases, program, instance, &function);
}

static void admission_replacement_budget_case(void) {
    unsigned releases = 0;
    XrXirProgram *program = NULL;
    XrXirInstance *instance = NULL;
    XrXirValue function = {0};
    admission_case_setup(&releases, &program, &instance, &function);
    uint64_t epoch = instance->epoch, original_limit = instance->config.poll_limit;
    XrXirCall *previous_call = instance->call;
    uint32_t requested = instance->requested;
    size_t baseline = live;
    uint32_t references = atomic_load(&object_pointer(&function)->references);
    XrXirCallAccounting before[2] = {instance->accounting[0], instance->accounting[1]};
    /* One function admission costs one value plus its capture-prefix/signature
     * scan. The replacement has no work left for its own boundary validation. */
    instance->config.poll_limit = 3;
    XrXirValueAdmission admission = instance_admission(instance);
    CHECK(xr_xir_value_admit(&function, (XrXirType) function.type, &admission) == XR_XIR_VALUE_OK);
    CHECK(!admission.work);
    CHECK(xr_xir_instance_start(instance, 4, &function, 1) == XR_XIR_CALL_LIMIT);
    CHECK(instance->call == previous_call && instance->epoch == epoch && instance->requested == requested);
    CHECK(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY && live == baseline);
    CHECK(atomic_load(&object_pointer(&function)->references) == references);
    CHECK(!memcmp(before, instance->accounting, sizeof(before)));
    instance->config.poll_limit = original_limit;
    /* Argument capture and the candidate's copied table succeed; its first
     * physical frame segment allocation then fails. The completed call stays. */
    size_t failed_segment = calls + 2;
    fail_at = failed_segment;
    CHECK(xr_xir_instance_start(instance, 4, &function, 1) == XR_XIR_CALL_OOM);
    fail_at = SIZE_MAX;
    CHECK(calls == failed_segment + 1 && live == baseline);
    uint32_t current = (uint32_t) (epoch % 2), pending = (uint32_t) ((epoch + 1) % 2);
    CHECK(!memcmp(&before[current], &instance->accounting[current], sizeof(before[current])));
    CHECK(!instance->accounting[pending].live_bytes && !instance->accounting[pending].depth);
    CHECK(instance->accounting[pending].allocations == before[pending].allocations + 1 &&
        instance->accounting[pending].frees == before[pending].frees + 1);
    CHECK(instance->call == previous_call && instance->epoch == epoch && instance->requested == requested &&
        xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    CHECK(atomic_load(&object_pointer(&function)->references) == references);
    XrXirInstanceResult previous = xr_xir_instance_poll(instance);
    CHECK(previous.epoch == epoch && previous.outcome.status == XR_XIR_CALL_RETURNED &&
        previous.outcome.value.type == XR_XIR_I64 && previous.outcome.value.payload == 17);
    XrXirValue result = {0};
    CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED && result.payload == 17);
    xr_xir_value_drop(&result);
    CHECK(xr_xir_instance_start(instance, 4, &function, 1) == XR_XIR_CALL_READY);
    uint64_t descriptor_bytes = sizeof(XrXirCall) +
        ((uint64_t) program->entry_count + 1) * sizeof(XrXirCallEntry) + sizeof(XrXirType);
    for (uint32_t i = 0; i < program->entry_count; ++i)
        descriptor_bytes += (uint64_t) program->entries[i].parameter_count * sizeof(XrXirType);
    CHECK(instance->call->allocation_bytes == descriptor_bytes && instance->call->segment &&
        !instance->call->segment->parent);
    CHECK(instance->accounting[pending].live_bytes == descriptor_bytes + instance->call->segment->allocation_bytes &&
        instance->accounting[pending].live_bytes <= instance->config.call_limit);
    CHECK(!instance->accounting[current].live_bytes && !instance->accounting[current].depth &&
        instance->accounting[current].allocations == instance->accounting[current].frees);
    XrXirInstanceResult wait = xr_xir_instance_poll(instance);
    CHECK(wait.epoch == epoch + 1 && wait.outcome.status == XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_instance_resume(instance, wait.epoch, wait.outcome.wake) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
    CHECK(instance->accounting[pending].live_bytes == descriptor_bytes && !instance->accounting[pending].depth);
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&result, &bytes, &length) && length == 21 &&
        !memcmp(bytes, "owned callback result", 21));
    xr_xir_value_drop(&result);
    admission_case_dispose(&releases, program, instance, &function);
}

static void instance_admission_cases(void) {
    admission_gate_owner_case();
    admission_replacement_budget_case();
    puts("Instance admission: forged gate rejected; failed replacement preserves result and epoch; zero live blocks");
}
#endif // XIR_INSTANCE_ADMISSION_CASES_H
