/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_task_array_admission_cases.h - Exact copy/store values and final owner lifetime
 *
 * KEY CONCEPT:
 *   The fixed position map distinguishes unchanged aliases from filled result and receiver copies.
 */

#ifndef XIR_TASK_ARRAY_ADMISSION_CASES_H
#define XIR_TASK_ARRAY_ADMISSION_CASES_H
#include "xir/xxir_struct.h"
#include "xir/xxir_class.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_tuple.h"
#include "xir/xxir_atomic.h"
#include "xir/xxir_error.h"
#include "xir/xxir_panic.h"
enum { MATRIX_ROWS = 4, MATRIX_WIDTH = 14 };
static XrXirInstance *matrix_open_program(XrXirProgram *program) {
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.value_limit = 1048576;
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    XrXirValue entry = matrix_run(instance, program->declarations->entry_function);
    CHECK(entry.type == XR_XIR_I64 && entry.payload == 0); xr_xir_value_drop(&entry);
    return instance;
}
static void matrix_text(const XrXirValue *value, const char *expected, size_t size) {
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(value, &bytes, &length) && length == size && !memcmp(bytes, expected, size));
}
static int64_t matrix_payload(unsigned row, bool filled, const XrXirValue *value) {
    int64_t identity = 0;
    XrXirCallResult first = {0}, second = {0};
    CHECK(xr_xir_task_copy_outcome(value, &first) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_task_copy_outcome(value, &second) == XR_XIR_CALL_RETURNED);
    CHECK(first.status == XR_XIR_CALL_RETURNED && second.status == XR_XIR_CALL_RETURNED);
    CHECK(first.value.type == second.value.type && first.value.payload == second.value.payload);
    if (row == 0) CHECK(first.value.type == XR_XIR_I64 && first.value.payload == (filled ? 7 : 3));
    else if (row == 1) CHECK(first.value.type == XR_XIR_BOOL && first.value.payload == (filled ? 1 : 0));
    else if (row == 2) matrix_text(&first.value, filled ? "a\0中" : "old", filled ? 5 : 3);
    else CHECK(first.value.type == XR_XIR_UNIT && !first.value.reserved && !first.value.payload);
    XrXirCallResult occupied = {.status = XR_XIR_CALL_RETURNED, .value = {XR_XIR_I64, 0, 99}};
    const XrXirCallResult before = occupied;
    CHECK(xr_xir_task_copy_outcome(value, &occupied) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!memcmp(&before, &occupied, sizeof(before)));
    xr_xir_call_result_drop(&occupied); xr_xir_call_result_drop(&first); xr_xir_call_result_drop(&second);
    identity = value->payload;
    return identity;
}
static int64_t matrix_value(unsigned row, const XrXirValue *array) {
    static const bool filled[MATRIX_WIDTH] = {false, false, false, false, true, false, false, true, false, true, true, false, false, false};
    CHECK(xr_xir_value_valid(array));
    XrXirValueAdmission admission = {xr_xir_value_arena(array), object_pointer(array)->domain, NULL, NULL, 100000, 1048576};
    int64_t length = 0;
    CHECK(xr_xir_array_len(array, &admission, &length) == XR_XIR_VALUE_OK && length == MATRIX_WIDTH);
    int64_t identities[2] = {0};
    for (int64_t index = 0; index < MATRIX_WIDTH; ++index) {
        XrXirValue element = {0}; XrXirFaultDetail fault = {0};
        CHECK(xr_xir_array_get(array, index, &admission, &element, &fault) == XR_XIR_VALUE_OK);
        const int64_t identity = matrix_payload(row, filled[index], &element);
        const unsigned position = filled[index] ? 1 : 0;
        if (!identities[position]) identities[position] = identity;
        else CHECK(identities[position] == identity);
        if (identities[0] && identities[1]) CHECK(identities[0] != identities[1]);
        xr_xir_value_drop(&element);
    }
    return identities[1];
}
static XrXirValueAdmission task_admission(const XrXirValue *array) {
    return (XrXirValueAdmission){xr_xir_value_arena(array), object_pointer(array)->domain, NULL, NULL, 100000, 1048576};
}
static void task_array_guards(TaskArrayCompile *run) {
    XrXirInstance *instance = matrix_open_program(run->program);
    XrXirValue direct = matrix_run(instance, matrix_find(run->module, "directTask"));
    matrix_payload(0, false, &direct); xr_xir_value_drop(&direct);
    XrXirValue plain = matrix_run(instance, matrix_find(run->module, "plainArray")), child = {0};
    XrXirValueAdmission admission = task_admission(&plain); XrXirFaultDetail fault = {0};
    int64_t length = 0;
    CHECK(xr_xir_array_len(&plain, &admission, &length) == XR_XIR_VALUE_OK && length == 1);
    CHECK(xr_xir_array_get(&plain, 0, &admission, &child, &fault) == XR_XIR_VALUE_OK);
    matrix_payload(0, false, &child); xr_xir_value_drop(&child); xr_xir_value_drop(&plain);
    XrXirValue array = matrix_run(instance, matrix_find(run->module, "case0"));
    XrXirValue booleans = matrix_run(instance, matrix_find(run->module, "case1"));
    XrXirValue wrong = {0}, alias = {0}; admission = task_admission(&booleans);
    CHECK(xr_xir_array_get(&booleans, 0, &admission, &wrong, &fault) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&array, &alias) == XR_XIR_VALUE_OK);
    const XrXirValue before = array, alias_before = alias;
    const size_t blocks = runtime_live, bytes = runtime_bytes;
    XrXirValuePlace place = {(XrXirType)array.type, &array.payload};
    const XrXirValue scalar = {XR_XIR_I64, 0, 3};
    admission = task_admission(&array);
    CHECK(xr_xir_array_set(&place, 0, &scalar, &admission, &fault) == XR_XIR_VALUE_BAD_ARGUMENT);
    admission = task_admission(&array);
    CHECK(xr_xir_array_set(&place, 0, &wrong, &admission, &fault) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!memcmp(&before, &array, sizeof(array)) && !memcmp(&alias_before, &alias, sizeof(alias)));
    CHECK(runtime_live == blocks && runtime_bytes == bytes);
    matrix_value(0, &array); matrix_value(0, &alias);
    /* Deliberately corrupt only test-owned storage, then restore it before release. */
    XirArray *backing = (XirArray *)object_pointer(&array);
    CHECK(backing->stride == sizeof(wrong.payload));
    unsigned char *last = backing->data + (backing->length - 1) * backing->stride;
    int64_t original = 0; memcpy(&original, last, sizeof(original));
    memcpy(last, &wrong.payload, sizeof(wrong.payload));
    admission = task_admission(&array);
    CHECK(xr_xir_value_admit(&array, (XrXirType)array.type, &admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(admission.scratch_bytes == 1048576 && runtime_live == blocks && runtime_bytes == bytes);
    memcpy(last, &original, sizeof(original));
    matrix_value(0, &array); matrix_value(0, &alias);
    admission = task_admission(&array); admission.work = 3;
    CHECK(xr_xir_value_admit(&array, (XrXirType)array.type, &admission) == XR_XIR_VALUE_LIMIT);
    CHECK(admission.scratch_bytes == 1048576 && runtime_live == blocks && runtime_bytes == bytes);
    admission = task_admission(&array); admission.scratch_bytes = sizeof(ValueAdmissionFrame) - 1;
    CHECK(xr_xir_value_admit(&array, (XrXirType)array.type, &admission) == XR_XIR_VALUE_LIMIT);
    CHECK(admission.scratch_bytes == sizeof(ValueAdmissionFrame) - 1 && runtime_live == blocks && runtime_bytes == bytes);
    admission = task_admission(&array); runtime_fail_at = runtime_attempts;
    CHECK(xr_xir_value_admit(&array, (XrXirType)array.type, &admission) == XR_XIR_VALUE_OOM);
    runtime_fail_at = SIZE_MAX;
    CHECK(admission.scratch_bytes == 1048576 && runtime_live == blocks && runtime_bytes == bytes);
    admission = task_admission(&array);
    CHECK(xr_xir_value_admit(&array, (XrXirType)array.type, &admission) == XR_XIR_VALUE_OK);
    CHECK(admission.scratch_bytes == 1048576 && runtime_live == blocks && runtime_bytes == bytes);
    matrix_value(0, &array); matrix_value(0, &alias);
    xr_xir_value_drop(&alias); xr_xir_value_drop(&wrong); xr_xir_value_drop(&booleans); xr_xir_value_drop(&array);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    CHECK(!runtime_live && !runtime_bytes);
    printf("Task array direct/plain/concat; wrong scalar/wrong Task/corrupt child rejected; work/scratch/OOM owners preserved\n");
}
static void matrix_goldens(TaskArrayCompile *run) {
    uint32_t functions[MATRIX_ROWS] = {0};
    for (unsigned row = 0; row < MATRIX_ROWS; ++row) {
        char name[16]; CHECK(snprintf(name, sizeof(name), "case%u", row) > 0);
        functions[row] = matrix_find(run->module, name);
    }
    XrXirInstance *first = matrix_open_program(run->program), *second = matrix_open_program(run->program);
    XrXirValue escaped[MATRIX_ROWS] = {{0}};
    for (unsigned row = 0; row < MATRIX_ROWS; ++row) {
        escaped[row] = matrix_run(first, functions[row]);
        const int64_t first_identity = matrix_value(row, &escaped[row]);
        XrXirValue other = matrix_run(second, functions[row]);
        const int64_t second_identity = matrix_value(row, &other);
        CHECK(second_identity && first_identity != second_identity); xr_xir_value_drop(&other);
    }
    xr_xir_compile_artifact_free(run->lowered); run->lowered = NULL;
    CHECK(xr_xir_instance_free(first) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_free(second) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(run->program); run->program = NULL; run->module = NULL;
    CHECK(runtime_live && runtime_bytes && task_array_stats(&run->context).live_bytes > run->baseline.live_bytes);
    for (unsigned row = 0; row < MATRIX_ROWS; ++row) matrix_value(row, &escaped[row]);
    for (unsigned row = 0; row < MATRIX_ROWS; ++row) {
        xr_xir_value_drop(&escaped[row]);
        if (row + 1 < MATRIX_ROWS) CHECK(runtime_live && runtime_bytes);
    }
    CHECK(!runtime_live && !runtime_bytes && task_array_stats(&run->context).live_bytes == run->baseline.live_bytes);
    printf("Task array four result types; fixed14; two instances; escaped owners final dualphysical0\n");
}
#endif // XIR_TASK_ARRAY_ADMISSION_CASES_H
