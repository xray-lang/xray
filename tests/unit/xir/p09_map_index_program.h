/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_map_index_program.h - Independent results and late panic/string ownership
 */
#ifndef P09_MAP_INDEX_PROGRAM_H
#define P09_MAP_INDEX_PROGRAM_H
#include "xir/xxir_program.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_panic.h"
static XrXirInstanceResult index_poll(XrXirInstance *instance, uint32_t entry) {
    CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult result = {0}; uint32_t polls = 0;
    do { result = xr_xir_instance_poll_bounded(instance, 1); }
    while (result.outcome.status == XR_XIR_CALL_READY && ++polls < 10000);
    return result;
}
static void index_program_run(XrXirProgram *program, const uint32_t *entries) {
    XrXirValue held[4] = {{0}};
    const int64_t expected[] = {1130, 9, 203, 1020, 730, 1230730};
    for (uint32_t run = 0; run < 2; ++run) {
        XrXirInstanceConfig config = {0}; XrXirInstance *instance = NULL;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
        for (uint32_t i = 0; i < INDEX_COUNT - 1; ++i) {
            XrXirInstanceResult result = index_poll(instance, entries[i]);
            if (result.outcome.status != XR_XIR_CALL_RETURNED)
                fprintf(stderr, "index entry=%u status=%u code=%u\n", i,
                    result.outcome.status, result.outcome.panic.detail.code);
            CHECK(result.outcome.status == XR_XIR_CALL_RETURNED);
            XrXirValue value = {0};
            CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
            if (i <= INDEX_ORDER) CHECK(value.type == XR_XIR_I64 && value.payload == expected[i]);
            else if (i == INDEX_CAUGHT) CHECK(value.type == XR_XIR_I64 && value.payload == 431);
            else if (!run) {
                uint32_t slot = i == INDEX_LATE ? 0 : i == INDEX_MESSAGE ? 1 : i == INDEX_NULL_VALUE ? 2 : 3;
                held[slot] = value; value = (XrXirValue){0};
            }
            xr_xir_value_drop(&value);
        }
        XrXirInstanceResult missing = index_poll(instance, entries[INDEX_MISSING]);
        CHECK(missing.outcome.status == XR_XIR_CALL_RUNTIME_PANIC);
        CHECK(missing.outcome.panic.detail.code == 431 && xr_xir_panic_valid(&missing.outcome.panic));
        CHECK(!missing.outcome.panic.detail.index && !missing.outcome.panic.detail.length);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    for (uint32_t i = 0; i < 2; ++i) {
        const char *bytes = NULL; size_t length = 0;
        const char *expected_text = i ? "map key not found" : "alive";
        CHECK(xr_xir_string_view(&held[i], &bytes, &length));
        CHECK(length == strlen(expected_text) && !memcmp(bytes, expected_text, length));
    }
    bool some = true; const XrXirValue *element = (const XrXirValue *)(uintptr_t)1;
    CHECK(xr_xir_nullable_view(&held[2], &some, &element) && !some && !element);
    XrXirFaultDetail detail = {0}; XrXirValue message = {0};
    CHECK(xr_xir_panic_info_detail(&held[3], &detail) && detail.code == 431 && xr_xir_fault_runtime_valid(detail));
    CHECK(xr_xir_panic_info_message(&held[3], &message) == XR_XIR_VALUE_OK);
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&message, &bytes, &length) && length == 17 && !memcmp(bytes, "map key not found", 17));
    xr_xir_value_drop(&message);
    for (uint32_t i = 0; i < 4; ++i) xr_xir_value_drop(held + i);
}
#endif // P09_MAP_INDEX_PROGRAM_H
