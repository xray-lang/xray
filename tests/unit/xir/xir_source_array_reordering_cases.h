/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_array_reordering_cases.h - Independent Array reordering program expectations
 *
 * KEY CONCEPT:
 *   The complete program has a separate owner, module state and physical lifecycle.
 */
#ifndef XIR_SOURCE_ARRAY_REORDERING_CASES_H
#define XIR_SOURCE_ARRAY_REORDERING_CASES_H

enum {
    REORDER_ENTRY, REORDER_CURRENT, REORDER_TRACE, REORDER_NUMBERS, REORDER_TEXT,
    REORDER_NESTED, REORDER_SUSPENDED, REORDER_ROOT_REVERSE, REORDER_ROOT_UNSHIFT,
    REORDER_ROOT_STATE, REORDER_FUNCTION_COUNT
};
static void source_array_reorder_numbers(const XrXirValue *array) {
    static const int64_t expected[] = {0, -7, 9, 4, 1, 4, 1, 7, 5};
    XrXirValueAdmission admission = {xr_xir_value_arena(array), NULL, NULL, NULL, 10000, 65536};
    int64_t length = 0;
    CHECK(xr_xir_array_len(array, &admission, &length) == XR_XIR_VALUE_OK && length == 9);
    for (int64_t i = 0; i < length; ++i) {
        XrXirValue element = {0}; XrXirFaultDetail fault = {0};
        CHECK(xr_xir_array_get(array, i, &admission, &element, &fault) == XR_XIR_VALUE_OK);
        CHECK(element.type == XR_XIR_I64 && element.payload == expected[i]); xr_xir_value_drop(&element);
    }
}
static void source_array_reorder_text(const XrXirValue *array) {
    static const char middle[] = "a\0中";
    const char *const expected[] = {"first", middle, "尾"};
    const size_t lengths[] = {5, sizeof(middle) - 1, sizeof("尾") - 1};
    XrXirValueAdmission admission = {xr_xir_value_arena(array), NULL, NULL, NULL, 10000, 65536};
    int64_t length = 0;
    CHECK(xr_xir_array_len(array, &admission, &length) == XR_XIR_VALUE_OK && length == 3);
    for (int64_t i = 0; i < length; ++i) {
        XrXirValue element = {0}; XrXirFaultDetail fault = {0}; const char *bytes = NULL; size_t count = 0;
        CHECK(xr_xir_array_get(array, i, &admission, &element, &fault) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_string_view(&element, &bytes, &count));
        CHECK(count == lengths[i] && !memcmp(bytes, expected[i], count)); xr_xir_value_drop(&element);
    }
}


/* A complete old or new sequence is the only observable root state. */
static bool source_array_reorder_state_kind(const XrXirValue *array, bool unshift) {
    static const int64_t before[] = {1, 2, 3};
    static const int64_t after_reverse[] = {3, 2, 1};
    static const int64_t after_unshift[] = {-7, 1, 2, 3};
    XrXirValueAdmission admission = {xr_xir_value_arena(array), NULL, NULL, NULL, 10000, 65536};
    int64_t length = 0;
    CHECK(xr_xir_array_len(array, &admission, &length) == XR_XIR_VALUE_OK);
    CHECK(length == 3 || (unshift && length == 4));
    XrXirValue first = {0}; XrXirFaultDetail fault = {0};
    CHECK(xr_xir_array_get(array, 0, &admission, &first, &fault) == XR_XIR_VALUE_OK);
    CHECK(first.type == XR_XIR_I64);
    bool committed = unshift ? length == 4 : first.payload == 3;
    xr_xir_value_drop(&first);
    const int64_t *expected = committed ? (unshift ? after_unshift : after_reverse) : before;
    CHECK(length == (committed && unshift ? 4 : 3));
    for (int64_t i = 0; i < length; ++i) {
        XrXirValue element = {0};
        CHECK(xr_xir_array_get(array, i, &admission, &element, &fault) == XR_XIR_VALUE_OK);
        CHECK(element.type == XR_XIR_I64 && element.payload == expected[i]); xr_xir_value_drop(&element);
    }
    return committed;
}
static XrXirInstance *source_array_reorder_open(XrXirProgram *program, const uint32_t *functions) {
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    XrXirValue entry = source_array_run(instance, functions[REORDER_ENTRY]);
    CHECK(entry.type == XR_XIR_I64 && entry.payload == 0); xr_xir_value_drop(&entry);
    return instance;
}
static void source_array_reorder_transactions(XrXirProgram *program, const uint32_t *functions) {
    const size_t baseline_live = runtime_live, baseline_bytes = runtime_bytes;
    const unsigned methods[] = {REORDER_ROOT_REVERSE, REORDER_ROOT_UNSHIFT};
    for (unsigned method = 0; method < 2; ++method) {
        bool unshift = method != 0;
        size_t sites = 0;
        /* Initialize first: these faults belong to the method activation,
         * candidate preparation and publication, not module startup. */
        for (size_t site = 0; site <= sites; ++site) {
            XrXirInstance *instance = source_array_reorder_open(program, functions);
            XrXirValue saved = source_array_run(instance, functions[REORDER_ROOT_STATE]);
            CHECK(!source_array_reorder_state_kind(&saved, unshift));
            runtime_attempts = 0; runtime_fail_at = site ? site - 1 : SIZE_MAX;
            XrXirCallStatus status = xr_xir_instance_start(instance, functions[methods[method]], NULL, 0);
            if (status == XR_XIR_CALL_READY) status = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
            if (!site) { CHECK(status == XR_XIR_CALL_RETURNED); sites = runtime_attempts; CHECK(sites && sites <= 512); }
            else {
                if (status != XR_XIR_CALL_OOM) fprintf(stderr, "Array reordering method=%u site=%zu/%zu status=%u\n",
                    method, site, sites, (unsigned) status);
                CHECK(runtime_attempts > runtime_fail_at && status == XR_XIR_CALL_OOM);
            }
            runtime_fail_at = SIZE_MAX;
            if (!site) {
                XrXirValue result = {0}; CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
                if (unshift) CHECK(result.type == XR_XIR_UNIT && !result.payload);
                else CHECK(source_array_reorder_state_kind(&result, false));
                xr_xir_value_drop(&result);
            }
            XrXirValue state = source_array_run(instance, functions[REORDER_ROOT_STATE]);
            CHECK(source_array_reorder_state_kind(&state, unshift) == !site); xr_xir_value_drop(&state);
            CHECK(!source_array_reorder_state_kind(&saved, unshift)); xr_xir_value_drop(&saved);
            CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
            CHECK(runtime_live == baseline_live && runtime_bytes == baseline_bytes);
        }
        printf("Source Array %s: %zu actual method OOM sites preserve the precommit root and alias\n",
            unshift ? "unshift" : "reverse", sites);
        /* Discover the finite sequence of real resume quanta without assuming
         * a VM or generated-native instruction count. */
        size_t steps = 0;
        {
            XrXirInstance *instance = source_array_reorder_open(program, functions);
            CHECK(xr_xir_instance_start(instance, functions[methods[method]], NULL, 0) == XR_XIR_CALL_READY);
            XrXirCallStatus status = XR_XIR_CALL_READY;
            while (status == XR_XIR_CALL_READY) {
                CHECK(steps < 512); ++steps;
                status = xr_xir_instance_poll_bounded(instance, 1).outcome.status;
            }
            CHECK(status == XR_XIR_CALL_RETURNED);
            XrXirValue result = {0}; CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
            if (unshift) CHECK(result.type == XR_XIR_UNIT && !result.payload);
            else CHECK(source_array_reorder_state_kind(&result, false));
            xr_xir_value_drop(&result);
            XrXirValue state = source_array_run(instance, functions[REORDER_ROOT_STATE]);
            CHECK(source_array_reorder_state_kind(&state, unshift)); xr_xir_value_drop(&state);
            CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
            CHECK(runtime_live == baseline_live && runtime_bytes == baseline_bytes);
        }
        size_t before = 0, after = 0;
        bool observed_commit = false;
        /* Every cancellable prefix includes first/middle/last preparation and
         * the postpublication window. Cancellation never permanently stops
         * the Instance, so its real module root remains queryable. */
        for (size_t prefix = 0; prefix < steps; ++prefix) {
            XrXirInstance *instance = source_array_reorder_open(program, functions);
            XrXirValue saved = source_array_run(instance, functions[REORDER_ROOT_STATE]);
            CHECK(!source_array_reorder_state_kind(&saved, unshift));
            CHECK(xr_xir_instance_start(instance, functions[methods[method]], NULL, 0) == XR_XIR_CALL_READY);
            for (size_t i = 0; i < prefix; ++i)
                CHECK(xr_xir_instance_poll_bounded(instance, 1).outcome.status == XR_XIR_CALL_READY);
            XrXirValue absent = {0};
            CHECK(xr_xir_instance_take_result(instance, &absent) == XR_XIR_CALL_BAD_STATE);
            CHECK(absent.type == XR_XIR_UNIT && !absent.payload);
            CHECK(xr_xir_instance_cancel_current(instance) == XR_XIR_CALL_CANCEL_REQUESTED);
            CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_CANCELLED);
            CHECK(xr_xir_instance_take_result(instance, &absent) == XR_XIR_CALL_BAD_STATE);
            CHECK(absent.type == XR_XIR_UNIT && !absent.payload);
            XrXirValue state = source_array_run(instance, functions[REORDER_ROOT_STATE]);
            bool committed = source_array_reorder_state_kind(&state, unshift); xr_xir_value_drop(&state);
            CHECK(!observed_commit || committed);
            observed_commit = observed_commit || committed;
            if (committed) ++after; else ++before;
            CHECK(!source_array_reorder_state_kind(&saved, unshift)); xr_xir_value_drop(&saved);
            CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
            CHECK(runtime_live == baseline_live && runtime_bytes == baseline_bytes);
        }
        CHECK(before >= 3 && after && before + after == steps);
        printf("Source Array %s: %zu real cancellation prefixes (%zu precommit, %zu postcommit), physical baseline restored\n",
            unshift ? "unshift" : "reverse", steps, before, after);
    }
}


/* Runtime implementation structs are visible through the existing allocation observer. */
static void source_array_reorder_limits(XrXirProgram *program, const uint32_t *functions) {
    const size_t baseline_live = runtime_live, baseline_bytes = runtime_bytes;
    const unsigned methods[] = {REORDER_ROOT_REVERSE, REORDER_ROOT_UNSHIFT};
    for (unsigned method = 0; method < 2; ++method) {
        for (unsigned mode = 0; mode < 4; ++mode) {
            XrXirInstance *instance = source_array_reorder_open(program, functions);
            XrXirValue saved = source_array_run(instance, functions[REORDER_ROOT_STATE]);
            CHECK(!source_array_reorder_state_kind(&saved, method != 0));
            XirObject *backing = object_pointer(&saved);
            uint64_t original_limit = instance->domain->limit;
            _Atomic uint32_t *references = mode == 1 ? &backing->references :
                mode == 2 ? &instance->domain->references : &instance->program->arena->references;
            uint32_t original_references = atomic_load(references);
            if (!mode) instance->domain->limit = instance->domain->stats.live_bytes;
            else atomic_store(references, UINT32_MAX);
            XrXirCallStatus status = xr_xir_instance_start(instance, functions[methods[method]], NULL, 0);
            XrXirCallStatus started = status;
            if (mode != 3) {
                CHECK(status == XR_XIR_CALL_READY);
                status = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
            } else CHECK(status == XR_XIR_CALL_LIMIT);
            instance->domain->limit = original_limit;
            if (mode) atomic_store(references, original_references);
            if (status != XR_XIR_CALL_LIMIT)
                fprintf(stderr, "Array method %u limit mode %u status %u\n", method, mode, (unsigned) status);
            CHECK(status == XR_XIR_CALL_LIMIT);
            printf("Array method %u limit mode %u start=%u final=%u\n",
                method, mode, (unsigned) started, (unsigned) status);
            XrXirValue absent = {0};
            CHECK(xr_xir_instance_take_result(instance, &absent) == XR_XIR_CALL_BAD_STATE && !absent.type && !absent.payload);
            XrXirValue state = source_array_run(instance, functions[REORDER_ROOT_STATE]);
            CHECK(!source_array_reorder_state_kind(&state, method != 0)); xr_xir_value_drop(&state);
            CHECK(!source_array_reorder_state_kind(&saved, method != 0)); xr_xir_value_drop(&saved);
            CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
            CHECK(runtime_live == baseline_live && runtime_bytes == baseline_bytes);
        }
    }
    puts("Source Array reordering: domain ceiling and backing/domain/arena retain limits preserve root and alias");
}

static void source_array_reordering_program_cases(XrXirProgram *program, const uint32_t *functions) {
    source_array_runtime_failures(program, functions[REORDER_ENTRY], 0);
    const unsigned failures[] = {REORDER_NUMBERS, REORDER_TEXT, REORDER_NESTED};
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i)
        source_array_runtime_failures(program, functions[failures[i]], 0);
    source_array_reorder_transactions(program, functions);
    source_array_reorder_limits(program, functions);
    const size_t baseline_live = runtime_live, baseline_bytes = runtime_bytes;
    {
        XrXirInstance *instance = source_array_reorder_open(program, functions);
        CHECK(xr_xir_instance_start(instance, functions[REORDER_SUSPENDED], NULL, 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult paused = xr_xir_instance_poll_bounded(instance, UINT64_MAX);
        CHECK(paused.outcome.status == XR_XIR_CALL_SUSPENDED && paused.epoch && paused.outcome.wake);
        XrXirValue absent = {0};
        CHECK(xr_xir_instance_take_result(instance, &absent) == XR_XIR_CALL_BAD_STATE);
        CHECK(absent.type == XR_XIR_UNIT && !absent.payload);
        CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_CANCELLED);
        CHECK(xr_xir_instance_resume(instance, paused.epoch, paused.outcome.wake) == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
        CHECK(runtime_live == baseline_live && runtime_bytes == baseline_bytes);
    }
    XrXirInstance *instances[2] = {0};
    XrXirValue escaped[2] = {{0}, {0}};
    for (unsigned i = 0; i < 2; ++i) {
        instances[i] = source_array_reorder_open(program, functions);
        escaped[i] = source_array_run(instances[i], functions[REORDER_TEXT]);
    }
    for (unsigned i = 0; i < 2; ++i) {
        XrXirValue value = source_array_run(instances[i], functions[REORDER_NUMBERS]);
        source_array_reorder_numbers(&value); xr_xir_value_drop(&value);
        value = source_array_run(instances[i], functions[REORDER_NESTED]);
        CHECK(value.type == XR_XIR_I64 && value.payload == 122791); xr_xir_value_drop(&value);
        value = source_array_resume_one(instances[i], functions[REORDER_SUSPENDED]);
        source_array_text(&value, "newUnshift"); xr_xir_value_drop(&value);
        value = source_array_run(instances[i], functions[REORDER_CURRENT]);
        source_array_text(&value, "head"); xr_xir_value_drop(&value);
        value = source_array_run(instances[i], functions[REORDER_TRACE]);
        CHECK(value.type == XR_XIR_I64 && value.payload == 12); xr_xir_value_drop(&value);
        if (!i) {
            value = source_array_run(instances[1], functions[REORDER_CURRENT]);
            source_array_text(&value, "init"); xr_xir_value_drop(&value);
            value = source_array_run(instances[1], functions[REORDER_TRACE]);
            CHECK(value.type == XR_XIR_I64 && !value.payload); xr_xir_value_drop(&value);
        }
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    for (unsigned i = 0; i < 2; ++i) {
        source_array_reorder_text(&escaped[i]); xr_xir_value_drop(&escaped[i]);
    }
    CHECK(!runtime_live && !runtime_bytes);
}
#endif // XIR_SOURCE_ARRAY_REORDERING_CASES_H
