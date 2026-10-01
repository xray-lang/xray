/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_struct_ops_cases.h - Independent nominal program output and lifetime expectations
 */
#ifndef XIR_STRUCT_OPS_CASES_H
#define XIR_STRUCT_OPS_CASES_H
#include "xir/xxir_struct.h"
#include "xir/xxir_type_arena.h"
static XrXirOutputStatus struct_ops_output(void *context, const XrXirOutputGroup *group) {
    unsigned *calls = context; ++*calls;
    CHECK(group->stream == XR_XIR_STDOUT && !group->line && group->count == 1);
    const char *bytes; size_t count;
    CHECK(xr_xir_string_view(&group->values[0],&bytes,&count) && count == 11 && !memcmp(bytes,"constructed",11));
    return XR_XIR_OUTPUT_OK;
}
static void struct_ops_cases(XrXirProgram *program) {
    XrXirInstance *instances[3] = {0}; unsigned outputs = 0;
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, struct_ops_output, &outputs};
    for (unsigned i = 0; i < 3; ++i)
        CHECK(xr_xir_instance_new(program,&config,&instances[i]) == XR_XIR_CALL_READY);
    xr_xir_program_drop(program);
    XrXirDomain *domain = NULL; XrXirValue arguments[2] = {{XR_XIR_I64,0,47},{0}}, escaped = {0};
    CHECK(xr_xir_domain_new(65536,&domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,"constructed",11,&arguments[1]) == XR_XIR_VALUE_OK);
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(xr_xir_instance_start(instances[i],i ? 2 : 1,i ? arguments : NULL,i ? 2 : 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult wait = xr_xir_instance_poll(instances[i]);
        CHECK(wait.outcome.status == XR_XIR_CALL_SUSPENDED);
        if (i != 2) {
            CHECK(xr_xir_instance_resume(instances[i],wait.epoch,wait.outcome.wake) == XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_poll(instances[i]).outcome.status == XR_XIR_CALL_RETURNED);
            CHECK(xr_xir_instance_take_result(instances[i],&escaped) == XR_XIR_CALL_RETURNED);
            if (!i) { CHECK(escaped.type == XR_XIR_I64 && escaped.payload == 23); xr_xir_value_drop(&escaped); }
        }
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    }
    CHECK(outputs == 3); xr_xir_value_drop(&arguments[1]);
    XrXirValueAdmission admission = {xr_xir_value_arena(&escaped),domain,NULL,NULL,10000,65536};
    XrXirValue field = {0};
    CHECK(xr_xir_struct_get(&escaped,0,&admission,&field) == XR_XIR_VALUE_OK);
    CHECK(field.type == XR_XIR_I64 && field.payload == 47); xr_xir_value_drop(&field);
    CHECK(xr_xir_struct_get(&escaped,1,&admission,&field) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&escaped); xr_xir_domain_drop(domain);
    const char *bytes; size_t count;
    CHECK(xr_xir_string_view(&field,&bytes,&count) && count == 11 && !memcmp(bytes,"constructed",11));
    xr_xir_value_drop(&field);
}
#endif // XIR_STRUCT_OPS_CASES_H
