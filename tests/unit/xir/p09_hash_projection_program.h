/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_hash_projection_program.h - Snapshot ownership after containers and code die
 */
#ifndef P09_HASH_PROJECTION_PROGRAM_H
#define P09_HASH_PROJECTION_PROGRAM_H
#include "xir/xxir_program.h"
#include "xir/xxir_array.h"
#include "xir/xxir_tuple.h"
static XrXirValue projection_call(XrXirInstance *instance, uint32_t entry) {
    CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult result = {0}; uint32_t polls = 0;
    do { result = xr_xir_instance_poll_bounded(instance, 1); ++polls; }
    while (result.outcome.status == XR_XIR_CALL_READY && polls < 10000);
    if (result.outcome.status != XR_XIR_CALL_RETURNED)
        fprintf(stderr, "projection entry=%u status=%u polls=%u\n", entry, result.outcome.status, polls);
    CHECK(result.outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue output = {0};
    CHECK(xr_xir_instance_take_result(instance, &output) == XR_XIR_CALL_RETURNED);
    return output;
}
static void projection_text(const XrXirValue *value, const char *expected) {
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(value, &bytes, &length) && length == strlen(expected) && !memcmp(bytes, expected, length));
}
static void projection_program_run(XrXirProgram *program, const uint32_t *entries) {
    XrXirValue held[4] = {{0}};
    for (uint32_t run = 0; run < 2; ++run) {
        XrXirInstanceConfig config = {0}; XrXirInstance *instance = NULL;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
        XrXirValue result = projection_call(instance, entries[0]);
        CHECK(result.type == XR_XIR_I64 && result.payload == 123);
        for (uint32_t i = 0; i < 4; ++i) {
            XrXirValue value = projection_call(instance, entries[i + 1]);
            if (!run) held[i] = value; else xr_xir_value_drop(&value);
        }
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    for (uint32_t i = 0; i < 4; ++i) {
        XrXirValueAdmission admission = {xr_xir_value_arena(held + i), domain, NULL, NULL, 10000, 65536};
        int64_t length = 0;
        CHECK(xr_xir_array_len(held + i, &admission, &length) == XR_XIR_VALUE_OK && length == 2);
        XrXirValue elements[2] = {{0}}; XrXirFaultDetail fault = {0};
        for (uint32_t j = 0; j < 2; ++j)
            CHECK(xr_xir_array_get(held + i, j, &admission, elements + j, &fault) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(held + i);
        for (uint32_t j = 0; j < 2; ++j) {
            if (i < 2) projection_text(elements + j, i ? (j ? "tail" : "alive") : (j ? "b" : "a"));
            else if (i == 2) {
                XrXirValue key = {0}, value = {0};
                CHECK(xr_xir_tuple_get(elements + j, 0, &key) == XR_XIR_VALUE_OK);
                CHECK(xr_xir_tuple_get(elements + j, 1, &value) == XR_XIR_VALUE_OK);
                xr_xir_value_drop(elements + j);
                CHECK(key.type == XR_XIR_I64 && key.payload == (j ? 1 : 2));
                projection_text(&value, j ? "one" : "two");
                xr_xir_value_drop(&key); xr_xir_value_drop(&value);
            } else CHECK(elements[j].type == XR_XIR_I64 && elements[j].payload == (j ? 3 : 5));
            xr_xir_value_drop(elements + j);
        }
    }
    xr_xir_domain_drop(domain);
}
#endif // P09_HASH_PROJECTION_PROGRAM_H
