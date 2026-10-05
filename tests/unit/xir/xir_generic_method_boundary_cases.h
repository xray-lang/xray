/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generic_method_boundary_cases.h - Method scope bounds before owned traversal
 *
 * KEY CONCEPT:
 *   Descriptor consistency and declaration scope are independent admission checks.
 */
#ifndef XIR_GENERIC_METHOD_BOUNDARY_CASES_H
#define XIR_GENERIC_METHOD_BOUNDARY_CASES_H
#include "xir_generic_method_owned_fixture.h"
static void generic_method_total_boundary(void) {
    GenericMethodOwnedFixture f; generic_method_owned_fixture(&f);
    f.methods[0].own_parameter_count = 65536;
    /* The short owned array must never be traversed; even the signature is
     * deliberately invalid so its BAD_TYPE cannot hide the earlier count gate. */
    f.methods[0].signature = XR_XIR_I64;
    XrXirCompileContext limit = consumer_context_default();
    limit.limits.parameters = UINT32_MAX;
    XrXirCompileContext copy_budget = limit; XrXirInterfaceTable *copy = NULL;
    CHECK(xr_xir_compile_interfaces_clone(&copy_budget, &f.table, &f.types, &copy) == XR_XIR_BUDGET);
    CHECK(!copy);
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_compile_check(&limit, &f.module, &checked, NULL) == XR_XIR_BUDGET);
    CHECK(!checked);
}
static void generic_method_nested_prepare(GenericMethodOwnedFixture *f, XrXirTypeNode nodes[2]) {
    generic_method_owned_fixture(f);
    nodes[0] = f->signature; memset(&nodes[1],0,sizeof(nodes[1]));
    nodes[1].kind = XR_XIR_TYPE_ARRAY;
    nodes[1].element = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1); nodes[1].parameter_span = 2;
    f->types.nodes = nodes; f->types.count = 2;
    f->parent = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1);
}
static size_t generic_method_array_wire(const XrXirCheckedPacket *packet) {
    uint8_t expected[12];
    put32(expected,XR_XIR_TYPE_ARRAY); put32(expected+4,2);
    put32(expected+8,XR_XIR_TYPE_PARAMETER_BASE+1);
    size_t found = 0; unsigned count = 0;
    for (size_t at = 64; at+sizeof(expected) <= packet->length; ++at)
        if (!memcmp(packet->bytes+at,expected,sizeof(expected))) { found = at; ++count; }
    CHECK(count == 1); return found;
}
static void generic_method_nested_boundaries(void) {
    GenericMethodOwnedFixture f; XrXirTypeNode nodes[2];
    generic_method_nested_prepare(&f,nodes);
    XrXirCompileContext budget = consumer_context_default(); XrXirInterfaceTable *copy = NULL;
    CHECK(xr_xir_compile_interfaces_clone(&budget, &f.table, &f.types, &copy) == XR_XIR_OK && copy);
    xr_xir_compile_interfaces_free(copy);
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_compile_check(suite_context, &f.module, &checked, NULL) == XR_XIR_OK && checked);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    size_t array = generic_method_array_wire(&packet);
    for (unsigned attack = 0; attack < 2; ++attack) {
        nodes[1].element = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+2);
        nodes[1].parameter_span = attack ? 0 : 3;
        /* A truthful span is valid as a descriptor but outside this method.
         * A forged zero span must fail descriptor admission independently. */
        budget = consumer_context_default();
        CHECK(xr_xir_compile_type_descriptors_verify(&budget, &f.types) ==
            (attack ? XR_XIR_BAD_TYPE : XR_XIR_OK));
        budget = consumer_context_default(); copy = NULL;
        CHECK(xr_xir_compile_interfaces_clone(&budget, &f.table, &f.types, &copy) == XR_XIR_BAD_TYPE && !copy);
        checked = NULL;
        CHECK(xr_xir_compile_check(suite_context, &f.module, &checked, NULL) == XR_XIR_BAD_TYPE && !checked);
        put32(packet.bytes+array+4,nodes[1].parameter_span);
        put32(packet.bytes+array+8,(uint32_t)nodes[1].element); digest_packet(&packet);
        CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_BAD_TYPE && !checked);
    }
    put32(packet.bytes+array+4,2); put32(packet.bytes+array+8,XR_XIR_TYPE_PARAMETER_BASE+1);
    digest_packet(&packet);
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK && checked);
    xr_xir_compile_artifact_free(checked);checked=NULL; xr_xir_compile_checked_packet_free(&packet);
}
static void generic_method_boundary_cases(void) {
    generic_method_total_boundary(); generic_method_nested_boundaries();
}
#endif // XIR_GENERIC_METHOD_BOUNDARY_CASES_H
