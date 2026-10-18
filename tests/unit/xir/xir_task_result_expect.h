/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_task_result_expect.h - Independent values for closed Task result families
 */
#ifndef XIR_TASK_RESULT_EXPECT_H
#define XIR_TASK_RESULT_EXPECT_H
#include "xir/xxir_array.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_tuple.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_atomic.h"
static bool constructed_i64_array(const XrXirValue *value, XrXirValueAdmission *admission) {
    int64_t length = 0; XrXirValueStatus status = xr_xir_array_len(value, admission, &length);
    if (status != XR_XIR_VALUE_OK) { CHECK(status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_OOM); return false; }
    CHECK(length == 2);
    for (int64_t i = 0; i < 2; ++i) {
        XrXirValue field = {0}; XrXirFaultDetail fault = {0};
        status = xr_xir_array_get(value, i, admission, &field, &fault);
        if (status != XR_XIR_VALUE_OK) { CHECK(status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_OOM); return false; }
        CHECK(field.type == XR_XIR_I64 && field.payload == 10 * (i + 1)); xr_xir_value_drop(&field);
    }
    return true;
}
static bool constructed_value(const XrXirValue *value, unsigned family) {
    CHECK(xr_xir_value_valid(value));
    const XirObject *object = (const XirObject *)(uintptr_t)value->payload;
    XrXirValueAdmission admission = {object->arena, object->domain, NULL, NULL, 1000000, 16777216};
    if (!family) return constructed_i64_array(value, &admission);
    if (family == 1) {
        int64_t length = 0; XrXirValueStatus status = xr_xir_array_len(value, &admission, &length);
        if (status != XR_XIR_VALUE_OK) { CHECK(status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_OOM); return false; }
        CHECK(length == 2); const char *words[] = {"owned", "array", "typed"}; unsigned word = 0;
        for (unsigned i = 0; i < 2; ++i) {
            XrXirValue inner = {0}; XrXirFaultDetail fault = {0};
            status = xr_xir_array_get(value, i, &admission, &inner, &fault);
            if (status != XR_XIR_VALUE_OK) { CHECK(status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_OOM); return false; }
            status = xr_xir_array_len(&inner, &admission, &length);
            if (status != XR_XIR_VALUE_OK) { xr_xir_value_drop(&inner); CHECK(status == XR_XIR_VALUE_LIMIT); return false; }
            CHECK(length == (i ? 1 : 2));
            for (int64_t j = 0; j < length; ++j) {
                XrXirValue text = {0}; status = xr_xir_array_get(&inner, j, &admission, &text, &fault);
                if (status != XR_XIR_VALUE_OK) { xr_xir_value_drop(&inner); CHECK(status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_OOM); return false; }
                const char *bytes = NULL; size_t size = 0;
                CHECK(xr_xir_string_view(&text, &bytes, &size) && size == 5 && !memcmp(bytes, words[word++], size));
                xr_xir_value_drop(&text);
            }
            xr_xir_value_drop(&inner);
        }
        CHECK(word == 3); return true;
    }
    if (family == 2 || family == 3) {
        bool some = false; const XrXirValue *payload = NULL;
        CHECK(xr_xir_nullable_view(value, &some, &payload) && some && payload);
        if (family == 3) return constructed_i64_array(payload, &admission);
        CHECK(payload->type == XR_XIR_I64 && payload->payload == 42); return true;
    }
    if (family == 4) {
        XrXirValue number = {0}, text = {0};
        XrXirValueStatus status = xr_xir_tuple_get(value, 0, &number);
        if (status != XR_XIR_VALUE_OK) { CHECK(status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_OOM); return false; }
        status = xr_xir_tuple_get(value, 1, &text);
        if (status != XR_XIR_VALUE_OK) { xr_xir_value_drop(&number); CHECK(status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_OOM); return false; }
        const char *bytes = NULL; size_t length = 0;
        CHECK(number.type == XR_XIR_I64 && number.payload == 42 && xr_xir_string_view(&text, &bytes, &length) && length == 5 && !memcmp(bytes, "owned", length));
        xr_xir_value_drop(&number); xr_xir_value_drop(&text); return true;
    }
    if (family == 5) {
        XrXirEnumBorrow record = {0}; CHECK(xr_xir_enum_borrow(value, &record) == XR_XIR_VALUE_OK);
        CHECK(record.name.length == 6 && !memcmp(record.name.bytes, "Choice", 6) && record.member.length == 8 && !memcmp(record.member.bytes, "Selected", 8) && record.field_count == 1);
        return constructed_i64_array(&record.fields[0], &admission);
    }
    if (family == 6) {
        XrXirAtomicRequest request = {value, NULL, NULL, 0, XR_XIR_ATOMIC_OPERATION_LOAD, XR_XIR_I64};
        XrXirAtomicProgress progress = {0}; XrXirValue number = {0};
        XrXirAtomicOutcome outcome = xr_xir_atomic_start(&request, &admission, NULL, &progress, &number);
        xr_xir_atomic_progress_clear(&progress);
        if (outcome.status != XR_XIR_RUN_OK || outcome.permission != XR_XIR_CALL_READY) {
            CHECK(outcome.status == XR_XIR_RUN_STEP_LIMIT || outcome.status == XR_XIR_RUN_FRAME_LIMIT || outcome.permission == XR_XIR_CALL_LIMIT || outcome.status == XR_XIR_RUN_OUT_OF_MEMORY); return false;
        }
        CHECK(!outcome.continuing && number.type == XR_XIR_I64 && number.payload == 42); xr_xir_value_drop(&number); return true;
    }
    CHECK(family == 7); XrXirCallResult inner = {0}; XrXirCallStatus status = xr_xir_task_copy_outcome(value, &inner);
    if (status != XR_XIR_CALL_RETURNED) { CHECK(status == XR_XIR_CALL_LIMIT); return false; }
    CHECK(inner.value.type == XR_XIR_I64 && inner.value.payload == 42); xr_xir_call_result_drop(&inner); return true;
}
#endif // XIR_TASK_RESULT_EXPECT_H
