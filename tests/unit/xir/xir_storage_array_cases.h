/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_storage_array_cases.h - Compact aggregate backing and mutation rollback
 */
#ifndef XIR_STORAGE_ARRAY_CASES_H
#define XIR_STORAGE_ARRAY_CASES_H
static void storage_array_cases(XrXirValueAdmission *admission, const XrXirValue *box) {
    uint64_t baseline = admission->domain->stats.live_bytes; size_t blocks = live;
    XrXirValue array = {0}, copy = {0}, result = {0}; XrXirFaultDetail fault = {0};
    admission->work = 100000;
    CHECK(xr_xir_array_new((XrXirType)260, box, 1, admission, &array) == XR_XIR_VALUE_OK);
    XirArray *backing = (XirArray *)object_pointer(&array);
    CHECK(backing->stride == 48 && backing->length == 1 && backing->capacity == 4);
    CHECK(admission->domain->stats.live_bytes == baseline + array_header_bytes(admission->arena, (XrXirType)257) + 192);
    CHECK(backing->data[0] == 1 && backing->data[24] == 0);
    CHECK(xr_xir_array_get(&array, 0, admission, &result, &fault) == XR_XIR_VALUE_OK && xr_xir_value_valid(&result));
    CHECK(object_pointer(&result) != object_pointer(box)); xr_xir_value_drop(&result);
    CHECK(xr_xir_value_copy(&array, &copy) == XR_XIR_VALUE_OK);
    XrXirValuePlace place = {(XrXirType)260, &copy.payload};
    size_t begin = calls;
    CHECK(xr_xir_array_push(&place, box, admission) == XR_XIR_VALUE_OK);
    size_t sites = calls - begin;
    CHECK(((XirArray *)object_pointer(&copy))->length == 2 && backing->length == 1);
    CHECK(object_pointer(&copy) != object_pointer(&array));
    xr_xir_value_drop(&copy);
    uint64_t bytes = admission->domain->stats.live_bytes; size_t array_blocks = live;
    for (size_t i = 0; i < sites; ++i) {
        CHECK(xr_xir_value_copy(&array, &copy) == XR_XIR_VALUE_OK);
        admission->work = 100000; fail_at = calls + i;
        CHECK(xr_xir_array_push(&place, box, admission) == XR_XIR_VALUE_OOM);
        fail_at = SIZE_MAX;
        CHECK(copy.payload == array.payload && backing->length == 1 && admission->scratch_bytes == 65536);
        CHECK(admission->domain->stats.live_bytes == bytes && live == array_blocks);
        xr_xir_value_drop(&copy);
    }
    admission->work = 100000;
    CHECK(xr_xir_value_copy(&array, &copy) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_set(&place, 0, box, admission, &fault) == XR_XIR_VALUE_OK);
    CHECK(object_pointer(&copy) != object_pointer(&array));
    CHECK(xr_xir_array_set(&place, 0, box, admission, &fault) == XR_XIR_VALUE_OK);
    for (unsigned i = 0; i < 8; ++i) CHECK(xr_xir_array_push(&place, box, admission) == XR_XIR_VALUE_OK);
    CHECK(((XirArray *)object_pointer(&copy))->length == 9 && backing->length == 1);
    CHECK(xr_xir_array_get(&copy, 8, admission, &result, &fault) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&array); xr_xir_value_drop(&copy);
    CHECK(xr_xir_value_valid(&result)); xr_xir_value_drop(&result);
    CHECK(admission->domain->stats.live_bytes == baseline && live == blocks && admission->scratch_bytes == 65536);
    XrXirValue empty = {0};
    CHECK(xr_xir_struct_new((XrXirType)258, NULL, 0, admission, &empty) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)261, &empty, 1, admission, &array) == XR_XIR_VALUE_OK);
    backing = (XirArray *)object_pointer(&array);
    CHECK(!backing->stride && !backing->data && backing->length == 1);
    place = (XrXirValuePlace){(XrXirType)261, &array.payload};
    for (unsigned i = 0; i < 8; ++i) CHECK(xr_xir_array_push(&place, &empty, admission) == XR_XIR_VALUE_OK);
    CHECK(!backing->data && backing->length == 9 && backing->capacity >= 9);
    CHECK(xr_xir_array_get(&array, 8, admission, &result, &fault) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&result); xr_xir_value_drop(&empty);
    begin = calls; fail_at = calls; xr_xir_value_drop(&array); fail_at = SIZE_MAX;
    CHECK(calls == begin && admission->domain->stats.live_bytes == baseline && live == blocks);
}
#endif // XIR_STORAGE_ARRAY_CASES_H
