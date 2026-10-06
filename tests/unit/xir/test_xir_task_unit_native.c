/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */
#include "xir/xxir_program.h"
#include "xir/xxir_task.h"
#include "xir/xxir_error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_go_source_native_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#include "xir/xxir_task_budget.c"
#include "xir/xxir_format.c"
#include "xir/xxir_effects.c"
#include "xir/xxir_specialize.c"
#include "xir_task_unit_native_oracles.h"
#include "task_unit_generated.h"
static XrXirOutputStatus unit_native_output(void *context, const XrXirOutputGroup *group) {
    unsigned *count = context;
    CHECK(group && group->stream == XR_XIR_STDOUT && group->line && group->count == 1 && group->values);
    CHECK(group->values[0].type == XR_XIR_I64 && !group->values[0].reserved && group->values[0].payload == 41);
    CHECK(!*count); ++*count; return XR_XIR_OUTPUT_OK;
}
static XrXirInstanceResult unit_native_poll(XrXirInstance *instance) {
    XrXirInstanceResult result = {0};
    for (unsigned n = 0; n < 4096; ++n) {
        result = xr_xir_instance_poll_bounded(instance, 1);
        if (result.outcome.status != XR_XIR_CALL_READY) return result;
    }
    CHECK(false); return result;
}
static void unit_native_escape(const XrXirValue *value, bool error) {
    XrXirValue alias = {0}; CHECK(xr_xir_value_copy(value, &alias) == XR_XIR_VALUE_OK);
    CHECK(alias.type == value->type && alias.payload == value->payload);
    XrXirCallResult occupied = {.status = XR_XIR_CALL_RETURNED, .value = {XR_XIR_I64, 0, 99}}, before = occupied;
    CHECK(xr_xir_task_copy_outcome(&alias, &occupied) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!memcmp(&before, &occupied, sizeof(before))); xr_xir_call_result_drop(&occupied);
    for (unsigned n = 0; n < 2; ++n) {
        XrXirCallResult result = {0};
        CHECK(xr_xir_task_copy_outcome(n ? &alias : value, &result) == (error ? XR_XIR_CALL_THROWN : XR_XIR_CALL_RETURNED));
        unit_native_outcome(&result, error); xr_xir_call_result_drop(&result);
    }
    xr_xir_value_drop(&alias);
}
static void unit_native_execute(unsigned index) {
    const UnitNativeOracle *oracle = &unit_native_oracles[index];
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(context, task_unit_native_specs[index], &program) == XR_XIR_OK);
    XrXirInstance *instances[2] = {0}; XrXirValue values[2] = {{0}}; unsigned outputs[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, unit_native_output, &outputs[i]};
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
    }
    CHECK(instances[0]->domain != instances[1]->domain);
    xr_xir_compile_program_drop(program); program = NULL;
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start(instances[i], task_unit_native_entries[index], NULL, 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult result = unit_native_poll(instances[i]);
        CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && outputs[i] == oracle->outputs);
        XrXirValue occupied = {XR_XIR_I64, 0, 99}, before = occupied;
        CHECK(xr_xir_instance_take_result(instances[i], &occupied) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(!memcmp(&before, &occupied, sizeof(before)));
        CHECK(xr_xir_instance_take_result(instances[i], &values[i]) == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY); instances[i] = NULL;
    }
    if (oracle->escaped) CHECK(values[0].payload != values[1].payload);
    for (unsigned i = 0; i < 2; ++i) {
        if (oracle->escaped) unit_native_escape(&values[i], oracle->error);
        else if (oracle->unit_root) unit_native_empty(&values[i]);
        else CHECK(values[i].type == XR_XIR_I64 && !values[i].reserved && values[i].payload == 7);
        xr_xir_value_drop(&values[i]);
    }
    CHECK(!runtime_live && !runtime_bytes);
    printf("UNIT_NATIVE %s instances=2 fixed7_Unit_Error=1 output41=%u escaped=%u physical=0/0\n", oracle->name, oracle->outputs, oracle->escaped);
}
static void unit_native_invalid(void) {
    XrXirValue invalid[] = {{XR_XIR_UNIT, 0, 1}, {XR_XIR_UNIT, 1, 0}, {XR_XIR_BOOL, 0, 0}};
    for (unsigned i = 0; i < 3; ++i) {
        if (i < 2) CHECK(!xr_xir_value_valid(&invalid[i]));
        XrXirCallResult result = {0};
        CHECK(xr_xir_task_copy_outcome(&invalid[i], &result) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(xr_xir_call_result_empty(&result));
    }
    puts("UNIT_NATIVE invalidUnitPayload_reserved_nonTask=3 exactBAD_ARGUMENT empty=1");
}
int main(void) {
    CHECK(!runtime_live && !runtime_bytes && !effects_compile_live && !effects_compile_bytes);
    for (unsigned i = 0; i < 8; ++i) unit_native_execute(i);
    unit_native_invalid(); effects_source_owners_free();
    CHECK(!runtime_live && !runtime_bytes && !effects_compile_live && !effects_compile_bytes);
    puts("UNIT_NATIVE_COMPLETE graphs=8 instances=16 identity=25/68/22/28/29 physical=0/0"); return 0;
}
