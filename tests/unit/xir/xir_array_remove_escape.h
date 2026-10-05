/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_remove_escape.h - Transport bits and final owner lifetime
 *
 * KEY CONCEPT:
 *   Data owners remain valid after producer, Instance and Program destruction.
 */
#ifndef XIR_ARRAY_REMOVE_ESCAPE_H
#define XIR_ARRAY_REMOVE_ESCAPE_H
static void remove_float_bits(RemoveCompile *run) {
    static const uint64_t bits[] = {0, UINT64_C(0x8000000000000000), 1, UINT64_C(0x8000000000000001),
        UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000), UINT64_C(0x7ff8000000001234),
        UINT64_C(0x7ff0000000000001), UINT64_C(0xfff8123456789abc)};
    XrXirInstance *instance = remove_open(run);
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
    for (unsigned shift = 0; shift < 2; ++shift) {
        uint32_t function = remove_find(run->module, shift ? "floatShift" : "floatPop");
        XrXirValueAdmission admission = {run->program->arena, domain, NULL, NULL, 100000, 1048576};
        for (unsigned n = 0; n < sizeof(bits)/sizeof(bits[0]); ++n) {
            XrXirValue input = {XR_XIR_F64, 0, 0}, array = {0}; memcpy(&input.payload, &bits[n], sizeof(input.payload));
            CHECK(xr_xir_array_new(run->program->entries[function].parameters[0], &input, 1, &admission, &array) == XR_XIR_VALUE_OK);
            CHECK(xr_xir_instance_start(instance, function, &array, 1) == XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
            XrXirValue result = {0}; CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
            bool some = false; const XrXirValue *payload = NULL;
            CHECK(xr_xir_nullable_view(&result, &some, &payload) && some && payload->type == XR_XIR_F64);
            uint64_t actual = 0; memcpy(&actual, &payload->payload, sizeof(actual)); CHECK(actual == bits[n]);
            int64_t length = 0; CHECK(xr_xir_array_len(&array, &admission, &length) == XR_XIR_VALUE_OK && length == 1);
            xr_xir_value_drop(&result); xr_xir_value_drop(&array);
        }
    }
    xr_xir_domain_drop(domain); CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    CHECK(!runtime_live && !runtime_bytes); puts("remove Float64 exact18 arbitrary-bit transports and original input ownership physical0");
}
static void remove_escaped(RemoveCompile *run) {
    XrXirInstance *instance = remove_open(run);
    XrXirValue callable = remove_run(instance, remove_find(run->module, "retainedCallable"));
    XrXirValue text = remove_run(instance, remove_find(run->module, "textPop"));
    XrXirValue alias = remove_run(instance, remove_find(run->module, "savedTail"));
    XrXirValue nested = remove_run(instance, remove_find(run->module, "removeTail"));
    xr_xir_compile_artifact_free(run->lowered); run->lowered = NULL;
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(run->program); run->program = NULL; run->module = NULL;
    CHECK(remove_stats(&run->context).live_bytes > run->baseline.live_bytes);
    const XrXirValue *payload = NULL; bool some = false;
    CHECK(xr_xir_nullable_view(&callable, &some, &payload) && some);
    const XrXirFunctionBinding *binding = xr_xir_function_binding(payload);
    CHECK(binding && binding->owner && binding->capture_count == 1 && binding->captures[0].type == XR_XIR_I64 && binding->captures[0].payload == 7);
    CHECK(xr_xir_nullable_view(&text, &some, &payload) && some);
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(payload, &bytes, &length) && length == 5 && !memcmp(bytes, "a\0中", 5));
    const XrXirValue *inner = NULL;
    CHECK(xr_xir_nullable_view(&nested, &some, &inner) && some);
    CHECK(xr_xir_nullable_view(inner, &some, &payload) && some && payload->payload == 3);
    static const int64_t old[] = {1, 0, 3}; remove_array(&alias, old, 3, true, 1);
    xr_xir_value_drop(&nested); xr_xir_value_drop(&alias); xr_xir_value_drop(&text); xr_xir_value_drop(&callable);
    CHECK(!runtime_live && !runtime_bytes);
    CHECK(remove_stats(&run->context).live_bytes == run->baseline.live_bytes);
    puts("remove escaped Callable/String/SomeSome and old alias pin arena/code until final physical0");
}
#endif // XIR_ARRAY_REMOVE_ESCAPE_H
