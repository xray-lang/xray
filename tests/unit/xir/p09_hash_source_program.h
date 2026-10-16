/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_hash_source_program.h - Late container results from real Program and Instance calls
 */
#ifndef P09_HASH_SOURCE_PROGRAM_H
#define P09_HASH_SOURCE_PROGRAM_H
#include "xir/xxir_map.h"
#include "xir/xxir_array.h"
#include "xir/xxir_program_internal.h"
#include "xir/xxir_value_internal.h"
static void hash_source_program_call(XrXirInstance *instance, uint32_t entry,
    const XrXirValue *input, XrXirValue *output) {
    CHECK(xr_xir_instance_start(instance, entry, input, 1) == XR_XIR_CALL_READY);
    XrXirInstanceResult result = {0}; unsigned polls = 0;
    do { result = xr_xir_instance_poll_bounded(instance, 1); ++polls; }
    while (result.outcome.status == XR_XIR_CALL_READY && polls < 1024);
    CHECK(result.outcome.status == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance, output) == XR_XIR_CALL_RETURNED);
    CHECK(output->payload == input->payload);
}
static void hash_source_program_run(XrXirProgram *program, uint32_t map_entry, uint32_t set_entry) {
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {program->arena, domain, NULL, NULL, 1000000, 65536};
    const XrXirTypes *types = program->types;
    XrXirType array_type = XR_XIR_UNIT, map_type = XR_XIR_UNIT, set_type = XR_XIR_UNIT;
    for (uint32_t i = 0; i < types->count; ++i) {
        if (types->nodes[i].kind == XR_XIR_TYPE_MAP) map_type = (XrXirType)(256 + i);
        if (types->nodes[i].kind == XR_XIR_TYPE_ARRAY) array_type = (XrXirType)(256 + i);
        if (types->nodes[i].kind == XR_XIR_TYPE_SET) set_type = (XrXirType)(256 + i);
    }
    CHECK(map_type != XR_XIR_UNIT && array_type != XR_XIR_UNIT && set_type != XR_XIR_UNIT);
    XrXirValue key = {0}, array = {0}, map = {0}, outcomes[2] = {{0}};
    XrXirValue elements[] = {{XR_XIR_I64, 0, 19}, {XR_XIR_I64, 0, 23}};
    CHECK(xr_xir_string_new(domain, "k\0\xce\xa9", 4, &key) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new(array_type, elements, 2, &admission, &array) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_map_new(map_type, &key, &array, 1, &admission, &map) == XR_XIR_VALUE_OK);
    XrXirValue members[] = {{XR_XIR_F64, 0, INT64_C(0x7ff8000000000000)}, {XR_XIR_F64, 0, INT64_MIN}};
    XrXirValue set = {0}, set_outcomes[2] = {{0}};
    CHECK(xr_xir_set_new(set_type, members, 2, &admission, &set) == XR_XIR_VALUE_OK);
    XrXirInstance *instances[2] = {NULL, NULL};
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceConfig config = {0};
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
        hash_source_program_call(instances[i], map_entry, &map, &outcomes[i]);
        hash_source_program_call(instances[i], set_entry, &set, &set_outcomes[i]);
        xr_xir_instance_free(instances[i]);
    }
    xr_xir_compile_program_drop(program);
    xr_xir_value_drop(&map); xr_xir_value_drop(&array); xr_xir_value_drop(&set);
    xr_xir_domain_close(domain); xr_xir_domain_drop(domain);
    XrXirValue read = {0}; bool found = false;
    CHECK(xr_xir_map_get(&outcomes[0], &key, &admission, &read, &found) == XR_XIR_VALUE_OK && found);
    XrXirValue number = {0}; XrXirFaultDetail fault = {0};
    CHECK(xr_xir_array_get(&read, 1, &admission, &number, &fault) == XR_XIR_VALUE_OK && number.payload == 23);
    xr_xir_value_drop(&number); xr_xir_value_drop(&read);
    XrXirValuePlace place = {map_type, &outcomes[0].payload};
    CHECK(xr_xir_hash_delete(&place, &key, &admission, &found) == XR_XIR_VALUE_OK && found);
    CHECK(xr_xir_map_get(&outcomes[1], &key, &admission, &read, &found) == XR_XIR_VALUE_OK && found);
    xr_xir_value_drop(&read); xr_xir_value_drop(&key);
    CHECK(xr_xir_set_contains(&set_outcomes[0], &members[0], &admission, &found) == XR_XIR_VALUE_OK && found);
    place = (XrXirValuePlace){set_type, &set_outcomes[0].payload};
    CHECK(xr_xir_hash_delete(&place, &members[1], &admission, &found) == XR_XIR_VALUE_OK && found);
    CHECK(xr_xir_set_contains(&set_outcomes[1], &members[1], &admission, &found) == XR_XIR_VALUE_OK && found);
    xr_xir_value_drop(&set_outcomes[0]); xr_xir_value_drop(&set_outcomes[1]);
    xr_xir_value_drop(&outcomes[0]); xr_xir_value_drop(&outcomes[1]);
}
#endif // P09_HASH_SOURCE_PROGRAM_H
