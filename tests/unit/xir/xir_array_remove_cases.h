/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_remove_cases.h - Independent values and transaction boundaries
 *
 * KEY CONCEPT:
 *   Cancellation prefixes locate publication independently of fault injection.
 */
#ifndef XIR_ARRAY_REMOVE_CASES_H
#define XIR_ARRAY_REMOVE_CASES_H
static XrXirInstance *remove_open(RemoveCompile *run) {
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.value_limit = 1048576;
    XrXirInstance *instance = NULL; CHECK(xr_xir_instance_new(run->program, &config, &instance) == XR_XIR_CALL_READY);
    XrXirValue entry = remove_run(instance, run->program->declarations->entry_function);
    CHECK(entry.type == XR_XIR_I64 && entry.payload == 0); xr_xir_value_drop(&entry); return instance;
}
static XrXirValueAdmission remove_admission(const XrXirValue *value, XrXirDomain **reader) {
    CHECK(xr_xir_domain_new(1048576, reader) == XR_XIR_VALUE_OK);
    return (XrXirValueAdmission){xr_xir_value_arena(value), *reader, NULL, NULL, 100000, 1048576};
}
static void remove_array(const XrXirValue *array, const int64_t *expected, int64_t count, bool nullable, int64_t none) {
    XrXirDomain *reader = NULL; XrXirValueAdmission admission = remove_admission(array, &reader);
    int64_t length = -1; CHECK(xr_xir_array_len(array, &admission, &length) == XR_XIR_VALUE_OK && length == count);
    for (int64_t i = 0; i < count; ++i) {
        XrXirValue element = {0}; XrXirFaultDetail fault = {0};
        CHECK(xr_xir_array_get(array, i, &admission, &element, &fault) == XR_XIR_VALUE_OK);
        const XrXirValue *payload = &element; bool some = true;
        if (nullable) { CHECK(xr_xir_nullable_view(&element, &some, &payload)); CHECK(some == (i != none)); }
        if (some) CHECK(payload && payload->type == XR_XIR_I64 && payload->payload == expected[i]);
        else CHECK(!payload);
        xr_xir_value_drop(&element);
    }
    xr_xir_domain_drop(reader);
}
static void remove_state(XrXirInstance *instance, RemoveCompile *run, bool shift, bool committed) {
    static const int64_t old[] = {1, 0, 3}, pop[] = {1, 0}, front[] = {0, 3};
    XrXirValue count = remove_run(instance, remove_find(run->module, shift ? "headCount" : "tailCount"));
    CHECK(count.type == XR_XIR_I64 && count.payload == (committed ? 2 : 3)); xr_xir_value_drop(&count);
    XrXirValue root = remove_run(instance, remove_find(run->module, shift ? "savedHead" : "savedTail"));
    remove_array(&root, committed ? (shift ? front : pop) : old, committed ? 2 : 3, true, committed && shift ? 0 : 1);
    xr_xir_value_drop(&root);
}
static void remove_goldens(RemoveCompile *run) {
    XrXirInstance *instance = remove_open(run);
    static const char *const numbers[] = {"numbersPop", "numbersShift"};
    static const int64_t pop[] = {3, 2, 1, 2, 3, 3}, front[] = {1, 2, 2, 3, 3, 3};
    for (unsigned n = 0; n < 2; ++n) {
        XrXirValue value = remove_run(instance, remove_find(run->module, numbers[n]));
        remove_array(&value, n ? front : pop, 6, false, -1); xr_xir_value_drop(&value);
    }
    for (unsigned shift = 0; shift < 2; ++shift) {
        const char *method = shift ? "removeHead" : "removeTail";
        for (unsigned ordinal = 0; ordinal < 4; ++ordinal) {
            XrXirValue value = remove_run(instance, remove_find(run->module, method));
            const XrXirValue *inner = NULL, *payload = NULL; bool outer = false, some = false;
            CHECK(xr_xir_nullable_view(&value, &outer, &inner) && outer == (ordinal < 3));
            if (outer) {
                CHECK(xr_xir_nullable_view(inner, &some, &payload) && some == (ordinal != 1));
                if (some) CHECK(payload->type == XR_XIR_I64 && payload->payload == (shift ? (ordinal == 0 ? 1 : 3) : (ordinal == 0 ? 3 : 1)));
                else CHECK(!payload);
            } else CHECK(!inner);
            xr_xir_value_drop(&value);
        }
    }
    static const char *const text[] = {"textPop", "textShift"};
    for (unsigned n = 0; n < 2; ++n) {
        XrXirValue value = remove_run(instance, remove_find(run->module, text[n]));
        const XrXirValue *payload = NULL; bool some = false; const char *bytes = NULL; size_t length = 0;
        CHECK(xr_xir_nullable_view(&value, &some, &payload) && some);
        CHECK(xr_xir_string_view(payload, &bytes, &length) && length == 5 && !memcmp(bytes, "a\0中", 5));
        xr_xir_value_drop(&value);
    }
    for (unsigned shift = 0; shift < 2; ++shift) {
        XrXirValue value = remove_run(instance, remove_find(run->module, shift ? "nestedShift" : "nestedPop"));
        const XrXirValue *payload = NULL; bool some = false;
        CHECK(xr_xir_nullable_view(&value, &some, &payload) && some && payload->payload == (shift ? 11 : 33)); xr_xir_value_drop(&value);
        value = remove_run(instance, remove_find(run->module, "currentTrace"));
        CHECK(value.type == XR_XIR_I64 && value.payload == 1); xr_xir_value_drop(&value);
        value = remove_run(instance, remove_find(run->module, "nestedCount"));
        CHECK(value.payload == 2); xr_xir_value_drop(&value);
    }
    for (unsigned shift = 0; shift < 2; ++shift) {
        XrXirValue value = remove_run(instance, remove_find(run->module, shift ? "reboundShift" : "reboundPop"));
        bool some = false; const XrXirValue *payload = NULL;
        CHECK(xr_xir_nullable_view(&value, &some, &payload) && some && payload->payload == (shift ? 71 : 73)); xr_xir_value_drop(&value);
        value = remove_run(instance, remove_find(run->module, "currentTrace")); CHECK(value.payload == 1); xr_xir_value_drop(&value);
        value = remove_run(instance, remove_find(run->module, "nestedCount")); CHECK(value.payload == 1); xr_xir_value_drop(&value);
    }
    XrXirValue empty = remove_run(instance, remove_find(run->module, "savedEmpty"));
    for (unsigned shift = 0; shift < 2; ++shift) {
        XrXirValue value = remove_run(instance, remove_find(run->module, shift ? "removeEmptyShift" : "removeEmptyPop"));
        bool some = true; const XrXirValue *payload = NULL; CHECK(xr_xir_nullable_view(&value, &some, &payload) && !some && !payload); xr_xir_value_drop(&value);
        value = remove_run(instance, remove_find(run->module, "savedEmpty"));
        CHECK(value.type == empty.type && value.payload == empty.payload); xr_xir_value_drop(&value);
    }
    xr_xir_value_drop(&empty);
    XrXirValue handle = remove_run(instance, remove_find(run->module, "readHandle"));
    XrXirDomain *reader = NULL; XrXirValueAdmission admission = remove_admission(&handle, &reader);
    for (int64_t i = 0; i < 2; ++i) {
        XrXirValue value = {0}; XrXirFaultDetail fault = {0}; bool some = false; const XrXirValue *payload = NULL;
        CHECK(xr_xir_array_get(&handle, i, &admission, &value, &fault) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_nullable_view(&value, &some, &payload) && some && payload->payload == (i ? 1 : 2)); xr_xir_value_drop(&value);
    }
    xr_xir_domain_drop(reader); xr_xir_value_drop(&handle);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY && !runtime_live && !runtime_bytes);
    puts("remove exact nested states, owned NUL/UTF8, aliases and selectors once: physical0");
}
typedef struct RemoveTimeline {size_t ticks, sites, commit, allocation_ticks[4096];} RemoveTimeline;
static RemoveTimeline remove_timeline(RemoveCompile *run, bool shift) {
    RemoveTimeline line = {0}; line.commit = SIZE_MAX;
    const size_t live = runtime_live, bytes = runtime_bytes;
    const uint32_t function = remove_find(run->module, shift ? "removeHead" : "removeTail");
    XrXirInstance *instance = remove_open(run);
    runtime_attempts = 0; runtime_fail_at = SIZE_MAX;
    CHECK(xr_xir_instance_start(instance, function, NULL, 0) == XR_XIR_CALL_READY);
    for (size_t i = 0; i < runtime_attempts; ++i) line.allocation_ticks[i] = 0;
    XrXirCallStatus status = XR_XIR_CALL_READY;
    while (status == XR_XIR_CALL_READY) {
        CHECK(line.ticks < 1024); ++line.ticks; size_t first = runtime_attempts;
        status = xr_xir_instance_poll_bounded(instance, 1).outcome.status;
        CHECK(runtime_attempts < 4096);
        for (size_t i = first; i < runtime_attempts; ++i) line.allocation_ticks[i] = line.ticks;
    }
    CHECK(status == XR_XIR_CALL_RETURNED); line.sites = runtime_attempts;
    XrXirValue result = {0}; CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
    xr_xir_value_drop(&result); remove_state(instance, run, shift, true);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY && runtime_live == live && runtime_bytes == bytes);
    for (size_t prefix = 0; prefix < line.ticks; ++prefix) {
        instance = remove_open(run);
        XrXirValue alias = remove_run(instance, remove_find(run->module, shift ? "savedHead" : "savedTail"));
        CHECK(xr_xir_instance_start(instance, function, NULL, 0) == XR_XIR_CALL_READY);
        for (size_t tick = 0; tick < prefix; ++tick) CHECK(xr_xir_instance_poll_bounded(instance, 1).outcome.status == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_cancel_current(instance) == XR_XIR_CALL_CANCEL_REQUESTED);
        CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_CANCELLED);
        XrXirValue count = remove_run(instance, remove_find(run->module, shift ? "headCount" : "tailCount"));
        CHECK(count.payload == 2 || count.payload == 3);
        if (count.payload == 2 && line.commit == SIZE_MAX) line.commit = prefix;
        xr_xir_value_drop(&count); remove_state(instance, run, shift, line.commit != SIZE_MAX);
        static const int64_t old[] = {1, 0, 3}; remove_array(&alias, old, 3, true, 1); xr_xir_value_drop(&alias);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY && runtime_live == live && runtime_bytes == bytes);
    }
    CHECK(line.commit > 0 && line.commit < line.ticks && line.sites);
    return line;
}
static void remove_runtime_faults(RemoveCompile *run, bool shift) {
    RemoveTimeline line = remove_timeline(run, shift);
    const uint32_t function = remove_find(run->module, shift ? "removeHead" : "removeTail");
    const size_t live = runtime_live, bytes = runtime_bytes; size_t before = 0, after = 0;
    for (size_t failure = 0; failure < line.sites; ++failure) {
        XrXirInstance *instance = remove_open(run);
        XrXirValue alias = remove_run(instance, remove_find(run->module, shift ? "savedHead" : "savedTail"));
        runtime_attempts = 0; runtime_fail_at = failure;
        XrXirCallStatus status = xr_xir_instance_start(instance, function, NULL, 0);
        size_t tick = 0, failed = runtime_attempts > failure ? 0 : SIZE_MAX;
        while (status == XR_XIR_CALL_READY) {
            CHECK(++tick < 2048); size_t first = runtime_attempts;
            status = xr_xir_instance_poll_bounded(instance, 1).outcome.status;
            if (first <= failure && failure < runtime_attempts) { CHECK(failed == SIZE_MAX); failed = tick; }
        }
        if (status != XR_XIR_CALL_OOM) fprintf(stderr,"fault%zu status%u tick%zu expected%zu\n", failure, status, failed, line.allocation_ticks[failure]);
        CHECK(runtime_attempts > failure && status == XR_XIR_CALL_OOM && failed == line.allocation_ticks[failure]);
        runtime_fail_at = SIZE_MAX;
        XrXirValue untouched = {0}; CHECK(xr_xir_instance_take_result(instance, &untouched) == XR_XIR_CALL_BAD_STATE && !untouched.type && !untouched.payload);
        bool committed = failed > line.commit; remove_state(instance, run, shift, committed);
        if (committed) ++after; else ++before;
        static const int64_t old[] = {1, 0, 3}; remove_array(&alias, old, 3, true, 1); xr_xir_value_drop(&alias);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY && runtime_live == live && runtime_bytes == bytes);
    }
    CHECK(before && after && before + after == line.sites);
    printf("remove %s runtimeOOM%zu pre%zu post%zu cancel-prefixes%zu commit%zu physical0\n", shift ? "shift" : "pop", line.sites, before, after, line.ticks, line.commit);
}
#endif // XIR_ARRAY_REMOVE_CASES_H
