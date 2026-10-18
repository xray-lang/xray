/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_set_algebra_program.h - Owned Set results after their program dies
 */
#ifndef P09_SET_ALGEBRA_PROGRAM_H
#define P09_SET_ALGEBRA_PROGRAM_H
#include "xir/xxir_program.h"
#include "xir/xxir_array.h"
#include "xir/xxir_map.h"
#include "xir/xxir_types.h"
#include "xir/xxir_type_arena.h"
static XrXirValue algebra_call(XrXirInstance *instance, uint32_t entry) {
    CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult result = {0}; uint32_t polls = 0;
    do { result = xr_xir_instance_poll_bounded(instance, 1); ++polls; }
    while (result.outcome.status == XR_XIR_CALL_READY && polls < 10000);
    if (result.outcome.status != XR_XIR_CALL_RETURNED)
        fprintf(stderr, "Set algebra entry=%u status=%u polls=%u\n", entry, result.outcome.status, polls);
    CHECK(result.outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue output = {0};
    CHECK(xr_xir_instance_take_result(instance, &output) == XR_XIR_CALL_RETURNED);
    return output;
}
static XrXirType algebra_array_type(const XrXirTypes *types, XrXirType element) {
    XrXirType result = XR_XIR_UNIT;
    for (uint32_t i = 0; i < types->count; ++i)
        if (types->nodes[i].kind == XR_XIR_TYPE_ARRAY && types->nodes[i].element == element) {
            CHECK(result == XR_XIR_UNIT); result = (XrXirType)(256 + i);
        }
    CHECK(result != XR_XIR_UNIT); return result;
}
static void algebra_program_run(XrXirProgram *program, const uint32_t *entries) {
    XrXirValue held[4] = {{0}};
    for (uint32_t run = 0; run < 2; ++run) {
        XrXirInstanceConfig config = {0}; XrXirInstance *instance = NULL;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
        XrXirValue result = algebra_call(instance, entries[0]);
        CHECK(result.type == XR_XIR_I64 && result.payload == 123);
        for (uint32_t i = 0; i < 4; ++i) {
            XrXirValue value = algebra_call(instance, entries[i + 1]);
            if (!run) held[i] = value; else xr_xir_value_drop(&value);
        }
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    const int64_t lengths[] = {3, 1, 3, 4};
    const int64_t numbers[2][4] = {{3, 1, 2}, {9, 4, 1, 2}};
    const char *texts[2][3] = {{"a", "b", "c"}, {"b"}};
    for (uint32_t i = 0; i < 4; ++i) {
        XrXirValueAdmission admission = {xr_xir_value_arena(held + i), domain, NULL, NULL, 100000, 65536};
        const XrXirTypes *types = xr_xir_compile_type_arena_types(admission.arena);
        XrXirType element = xr_xir_set_element(types, (XrXirType)held[i].type);
        CHECK(element == (i < 2 ? XR_XIR_STRING : XR_XIR_I64));
        XrXirValue array = {0};
        CHECK(xr_xir_hash_values(held + i, algebra_array_type(types, element), &admission, &array) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(held + i);
        int64_t length = 0;
        CHECK(xr_xir_array_len(&array, &admission, &length) == XR_XIR_VALUE_OK && length == lengths[i]);
        for (uint32_t j = 0; j < (uint32_t)length; ++j) {
            XrXirValue item = {0}; XrXirFaultDetail fault = {0};
            CHECK(xr_xir_array_get(&array, j, &admission, &item, &fault) == XR_XIR_VALUE_OK);
            if (i < 2) {
                const char *bytes = NULL; size_t count = 0;
                CHECK(xr_xir_string_view(&item, &bytes, &count) && count == strlen(texts[i][j]) && !memcmp(bytes, texts[i][j], count));
            } else CHECK(item.type == XR_XIR_I64 && item.payload == numbers[i - 2][j]);
            xr_xir_value_drop(&item);
        }
        xr_xir_value_drop(&array);
    }
    xr_xir_domain_drop(domain);
}
#endif // P09_SET_ALGEBRA_PROGRAM_H
