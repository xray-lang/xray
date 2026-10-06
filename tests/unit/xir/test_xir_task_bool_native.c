/* Public native program ownership and independent typed bool expectations. */
#include "xir/xxir_program.h"
#include "xir/xxir_task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
#include "xir_go_source_native_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#include "xir/xxir_task_budget.c"
#include "xir/xxir_format.c"
#include "xir/xxir_effects.c"
#include "xir/xxir_specialize.c"
#include "go_bool_generated.h"
typedef struct GoBoolOutput { unsigned count; bool malformed; } GoBoolOutput;
static XrXirOutputStatus go_bool_output(void *context, const XrXirOutputGroup *group) {
    GoBoolOutput *log = context;
    static const int64_t expected[3] = {1, 1, 0};
    if (!group || group->stream != XR_XIR_STDOUT || !group->line || group->count != 1 ||
        !group->values || log->count >= 3 || group->values[0].type != XR_XIR_BOOL ||
        group->values[0].reserved || group->values[0].payload != expected[log->count]) {
        log->malformed = true; return XR_XIR_OUTPUT_ERROR;
    }
    ++log->count; return XR_XIR_OUTPUT_OK;
}
static XrXirInstanceResult go_bool_poll(XrXirInstance *instance) {
    XrXirInstanceResult result = {0};
    for (unsigned n = 0; n < 32768; ++n) {
        result = xr_xir_instance_poll_bounded(instance, 1);
        if (result.outcome.status != XR_XIR_CALL_READY) return result;
    }
    CHECK(false); return result;
}
static void go_bool_closed_outcome(const XrXirValue *task) {
    XrXirCallResult first = {0}, second = {0};
    CHECK(xr_xir_task_copy_outcome(task, &first) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_task_copy_outcome(task, &second) == XR_XIR_CALL_RETURNED);
    CHECK(first.status == XR_XIR_CALL_RETURNED && second.status == XR_XIR_CALL_RETURNED);
    CHECK(first.value.type == XR_XIR_BOOL && !first.value.reserved && !first.value.payload);
    CHECK(!memcmp(&first, &second, sizeof(first)));
    XrXirCallResult occupied = {.status = XR_XIR_CALL_RETURNED, .value = {XR_XIR_BOOL, 0, 1}};
    XrXirCallResult before = occupied;
    CHECK(xr_xir_task_copy_outcome(task, &occupied) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!memcmp(&before, &occupied, sizeof(before)));
    xr_xir_call_result_drop(&occupied); xr_xir_call_result_drop(&first); xr_xir_call_result_drop(&second);
}
int main(void) {
    CHECK(!runtime_live && !runtime_bytes && !effects_compile_live && !effects_compile_bytes);
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(context, &go_bool_native_program, &program) == XR_XIR_OK);
    XrXirInstance *instances[2] = {0}; GoBoolOutput logs[2] = {0};
    XrXirValue escaped[2] = {0}, aliases[2] = {0};
    for (unsigned pass = 0; pass < 2; ++pass) {
        XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, go_bool_output, &logs[pass]};
        CHECK(xr_xir_instance_new(program, &config, &instances[pass]) == XR_XIR_CALL_READY);
    }
    CHECK(instances[0]->domain != instances[1]->domain);
    xr_xir_compile_program_drop(program); program = NULL;
    for (unsigned pass = 0; pass < 2; ++pass) {
        XrXirInstance *instance = instances[pass]; XrXirValue value = {0};
        CHECK(xr_xir_instance_start(instance, go_bool_entries[0], NULL, 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult result = go_bool_poll(instance);
        CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && result.outcome.value.type == XR_XIR_I64 &&
            !result.outcome.value.reserved && result.outcome.value.payload == 84);
        XrXirValue occupied = {XR_XIR_I64, 0, 99}, before = occupied;
        CHECK(xr_xir_instance_take_result(instance, &occupied) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(!memcmp(&occupied, &before, sizeof(before)));
        CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_I64 && !value.reserved && value.payload == 84); xr_xir_value_drop(&value);
        CHECK(logs[pass].count == 3 && !logs[pass].malformed);
        XrXirValue malformed[2] = {{XR_XIR_BOOL, 0, 2}, {XR_XIR_BOOL, 1, 1}};
        for (unsigned n = 0; n < 2; ++n)
            CHECK(xr_xir_instance_start(instance, go_bool_entries[2], &malformed[n], 1) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(xr_xir_instance_start(instance, go_bool_entries[1], NULL, 0) == XR_XIR_CALL_READY);
        result = go_bool_poll(instance); CHECK(result.outcome.status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance, &escaped[pass]) == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_value_copy(&escaped[pass], &aliases[pass]) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY); instances[pass] = NULL;
    }
    for (unsigned pass = 0; pass < 2; ++pass) {
        go_bool_closed_outcome(&escaped[pass]);
        xr_xir_value_drop(&escaped[pass]); go_bool_closed_outcome(&aliases[pass]);
        xr_xir_value_drop(&aliases[pass]);
    }
    CHECK(!runtime_live && !runtime_bytes);
    effects_source_owners_free(); CHECK(!effects_compile_live && !effects_compile_bytes);
    puts("GO_BOOL_NATIVE instances=2 outputs=true,true,false repeatedawait=2 sum=84 escaped=false copies=4 physical=0/0");
    return 0;
}
