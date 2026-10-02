/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_host_call_linked.c - Host API consumer linked against production archives
 */
#include "execution/xr_xir_host_execution.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
XR_DATA const XrXirProgramSpec host_call_source_program;
XR_DATA const uint32_t host_call_entries[3];
typedef struct LinkedTrace { int64_t values[3]; unsigned count; } LinkedTrace;
static XrXirOutputStatus linked_output(void *context, const XrXirOutputGroup *group) {
    LinkedTrace *trace = context;
    CHECK(group->stream == XR_XIR_STDOUT && group->line && group->count == 1 && trace->count < 3);
    CHECK(group->values[0].type == XR_XIR_I64);
    trace->values[trace->count++] = (int64_t)group->values[0].payload;
    return XR_XIR_OUTPUT_OK;
}
int main(void) {
    XrXirProgram *program = NULL;
    CHECK(xr_xir_program_seal(&host_call_source_program,
        (XrXirProgramBudget){16777216, 64000000}, &program) == XR_XIR_OK);
    XrXirHostCall *calls[2] = {0}; LinkedTrace traces[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, linked_output, &traces[i]};
        XrXirHostExecutionRequest request = {program, &config, host_call_entries[0], NULL, 0};
        CHECK(xr_xir_host_call_begin(&request, &calls[i]) == XR_XIR_CALL_READY);
    }
    xr_xir_program_drop(program);
    XrXirInstanceResult first = xr_xir_host_call_step(calls[0]), second = xr_xir_host_call_step(calls[1]);
    CHECK(first.outcome.status == XR_XIR_CALL_SUSPENDED && second.outcome.status == XR_XIR_CALL_SUSPENDED);
    CHECK(traces[0].count == 1 && traces[1].count == 1 && traces[0].values[0] == 1 && traces[1].values[0] == 1);
    CHECK(xr_xir_host_call_cancel(calls[0]) == XR_XIR_CALL_CANCELLED);
    CHECK(traces[0].count == 2 && traces[0].values[1] == 1);
    XrXirCallResult results[2] = {0};
    CHECK(xr_xir_host_call_take(calls[0], &results[0]) == XR_XIR_CALL_CANCELLED);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_host_call_resume(calls[1], second.epoch, second.outcome.wake) == XR_XIR_CALL_READY);
        second = xr_xir_host_call_step(calls[1]);
        CHECK(second.outcome.status == (i ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_SUSPENDED));
    }
    CHECK(traces[1].count == 3 && traces[1].values[1] == 11 && traces[1].values[2] == 11);
    CHECK(xr_xir_host_call_take(calls[1], &results[1]) == XR_XIR_CALL_RETURNED);
    for (unsigned i = 0; i < 2; ++i) CHECK(xr_xir_host_call_drop(calls[i]) == XR_XIR_CALL_READY);
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&results[1].value, &bytes, &length) && length == 11 && !memcmp(bytes, "held-result", 11));
    for (unsigned i = 0; i < 2; ++i) xr_xir_call_result_drop(&results[i]);
    puts("Production host archive: first suspension cancellation, independent resume and owned result PASS");
    return 0;
}
