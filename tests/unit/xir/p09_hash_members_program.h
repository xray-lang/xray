/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_hash_members_program.h - Real calls, logical copies and late nullable results
 */
#ifndef P09_HASH_MEMBERS_PROGRAM_H
#define P09_HASH_MEMBERS_PROGRAM_H
#include "xir/xxir_program.h"
#include "xir/xxir_nullable.h"
static XrXirValue members_call(XrXirInstance *instance, uint32_t entry) {
    CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult result = {0}; uint32_t polls = 0;
    do { result = xr_xir_instance_poll_bounded(instance, 1); ++polls; }
    while (result.outcome.status == XR_XIR_CALL_READY && polls < 10000);
    if (result.outcome.status != XR_XIR_CALL_RETURNED)
        fprintf(stderr, "member call entry=%u status=%u polls=%u\n", entry,
            result.outcome.status, polls);
    CHECK(result.outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue output = {0};
    CHECK(xr_xir_instance_take_result(instance, &output) == XR_XIR_CALL_RETURNED);
    return output;
}
static void members_program_run(XrXirProgram *program, const uint32_t *entries) {
    XrXirValue held[4] = {{0}};
    for (uint32_t run = 0; run < 2; ++run) {
        XrXirInstanceConfig config = {0}; XrXirInstance *instance = NULL;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
        XrXirValue result = members_call(instance, entries[0]);
        CHECK(result.type == XR_XIR_I64 && result.payload == 202);
        for (uint32_t i = 0; i < 4; ++i) {
            XrXirValue value = members_call(instance, entries[i + 1]);
            if (!run) held[i] = value; else xr_xir_value_drop(&value);
        }
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    const int64_t expected[] = {7, 0, 9, 3};
    for (uint32_t i = 0; i < 4; ++i) {
        bool some = false; const XrXirValue *value = NULL;
        CHECK(xr_xir_nullable_view(&held[i], &some, &value));
        CHECK(some == (i != 1));
        if (some) CHECK(value && value->type == XR_XIR_I64 && value->payload == expected[i]);
        else CHECK(!value);
        xr_xir_value_drop(&held[i]);
    }
}
#endif // P09_HASH_MEMBERS_PROGRAM_H
