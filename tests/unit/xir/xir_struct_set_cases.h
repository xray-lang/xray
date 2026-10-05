/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_struct_set_cases.h - Independent COW, root writeback and isolation oracle
 */
#ifndef XIR_STRUCT_SET_CASES_H
#define XIR_STRUCT_SET_CASES_H
static XrXirOutputStatus struct_set_output(void *context, const XrXirOutputGroup *group) {
    unsigned *calls = context, event = (*calls)++;
    CHECK(group->stream == XR_XIR_STDOUT && event < 6);
    if (event % 2) {
        static const int64_t expected[3][5] = {{23,31,41,7,7},{23,31,41,41,7},{23,31,41,7,7}};
        CHECK(group->line && group->count == 5);
        for (unsigned i = 0; i < 5; ++i)
            CHECK(group->values[i].type == XR_XIR_I64 && group->values[i].payload == expected[event / 2][i]);
    } else {
        CHECK(!group->line && group->count == 1);
        const char *bytes; size_t count;
        CHECK(xr_xir_string_view(&group->values[0],&bytes,&count) && count == 11 && !memcmp(bytes,"replacement",11));
    }
    return XR_XIR_OUTPUT_OK;
}
static void struct_set_cases(XrXirProgram *program) {
    XrXirInstance *instances[2] = {0}; unsigned outputs = 0;
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, struct_set_output, &outputs};
    for (unsigned i = 0; i < 2; ++i)
        CHECK(xr_xir_instance_new(program,&config,&instances[i]) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    for (unsigned i = 0; i < 2; ++i) {
        for (unsigned run = 0; run < (i ? 1u : 2u); ++run) {
            CHECK(xr_xir_instance_start(instances[i],1,NULL,0) == XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_poll_bounded(instances[i], UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
            XrXirValue result = {0};
            CHECK(xr_xir_instance_take_result(instances[i],&result) == XR_XIR_CALL_RETURNED);
            CHECK(result.type == XR_XIR_I64 && result.payload == (run ? 143 : 109));
            xr_xir_value_drop(&result);
        }
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    }
    CHECK(outputs == 6);
}
#endif // XIR_STRUCT_SET_CASES_H
