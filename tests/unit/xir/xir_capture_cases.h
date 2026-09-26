/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_capture_cases.h - Independent capture, suspension and escaped owner oracles
 *
 * KEY CONCEPT:
 *   Both consumers must satisfy expected values after producer frames die.
 */
#ifndef XIR_CAPTURE_CASES_H
#define XIR_CAPTURE_CASES_H
static void capture_result(const XrXirValue *value) {
    const char *bytes = NULL; size_t count = 0;
    CHECK(xr_xir_string_view(value,&bytes,&count) && count == 9 && !memcmp(bytes,"captured!",9));
}
static void capture_cases(XrXirProgram *program) {
    XrXirInstanceConfig config = xr_xir_instance_defaults();
    XrXirDomain *domain = NULL; XrXirValue suffix = {0};
    CHECK(xr_xir_domain_new(65536,&domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,"!",1,&suffix) == XR_XIR_VALUE_OK);
    xr_xir_domain_drop(domain);
    for (unsigned mode = 0; mode < 3; ++mode) {
        XrXirInstance *instance = NULL, *other = NULL;
        CHECK(xr_xir_instance_new(program,&config,&instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(program,&config,&other) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,2,NULL,0) == XR_XIR_CALL_READY);
        XrXirInstanceResult made = xr_xir_instance_poll(instance);
        CHECK(made.outcome.status == XR_XIR_CALL_RETURNED);
        const XrXirFunctionBinding *outer = xr_xir_function_binding(&made.outcome.value);
        CHECK(outer && outer->capture_count == 1);
        const XrXirFunctionBinding *inner = xr_xir_function_binding(&outer->captures[0]);
        CHECK(inner && inner->capture_count == 1 && inner->captures[0].type == XR_XIR_STRING);
        XrXirValue escaped = {0}, result = {0};
        CHECK(xr_xir_value_copy(&made.outcome.value,&escaped) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_instance_start_function(other,&escaped,&suffix,1) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(xr_xir_instance_start_function(instance,&escaped,NULL,0) == XR_XIR_CALL_BAD_ARGUMENT);
        if (mode == 0) {
            /* Leave the previous result as the sole environment owner before admission. */
            xr_xir_value_drop(&escaped);
            CHECK(xr_xir_instance_start_function(instance,&made.outcome.value,&suffix,1) == XR_XIR_CALL_READY);
        } else {
            XrXirValue args[] = {escaped,suffix};
            CHECK(xr_xir_instance_start(instance,4,args,2) == XR_XIR_CALL_READY);
        }
        for (unsigned suspension = 0; suspension < 2; ++suspension) {
            XrXirInstanceResult wait = xr_xir_instance_poll(instance);
            CHECK(wait.outcome.status == XR_XIR_CALL_SUSPENDED);
            if (mode == 2 && suspension == 1) break;
            CHECK(xr_xir_instance_resume(instance,wait.epoch,wait.outcome.wake) == XR_XIR_CALL_READY);
        }
        if (mode != 2) {
            CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_RETURNED);
            CHECK(xr_xir_instance_take_result(instance,&result) == XR_XIR_CALL_RETURNED);
        }
        CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY);
        if (mode != 0) CHECK(xr_xir_instance_start_function(instance,&escaped,&suffix,1) == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_free(other) == XR_XIR_CALL_READY);
        if (mode != 0) {
            outer = xr_xir_function_binding(&escaped); inner = xr_xir_function_binding(&outer->captures[0]);
            const char *bytes; size_t count;
            CHECK(xr_xir_string_view(&inner->captures[0],&bytes,&count) && count == 8 && !memcmp(bytes,"captured",8));
        }
        if (mode != 2) capture_result(&result);
        xr_xir_value_drop(&result); xr_xir_value_drop(&escaped);
    }
    xr_xir_value_drop(&suffix); xr_xir_program_drop(program);
}
#endif // XIR_CAPTURE_CASES_H
