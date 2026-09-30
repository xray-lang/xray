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
    XrXirBudget limit = xr_xir_default_budget();
    limit.parameters = UINT32_MAX; limit.metadata_bytes = UINT64_MAX; limit.work = UINT64_MAX;
    XrXirBudget copy_budget = limit; XrXirInterfaceTable *copy = NULL;
    CHECK(xr_xir_interfaces_clone(&f.table,&f.types,&copy_budget,&copy) == XR_XIR_BUDGET);
    CHECK(!copy);
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_check(&f.module,&limit,&checked,NULL) == XR_XIR_BUDGET);
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
    XrXirBudget budget = xr_xir_default_budget(); XrXirInterfaceTable *copy = NULL;
    CHECK(xr_xir_interfaces_clone(&f.table,&f.types,&budget,&copy) == XR_XIR_OK && copy);
    xr_xir_interfaces_free(copy);
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_check(&f.module,NULL,&checked,NULL) == XR_XIR_OK && checked);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked,NULL,&packet,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    size_t array = generic_method_array_wire(&packet);
    for (unsigned attack = 0; attack < 2; ++attack) {
        nodes[1].element = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+2);
        nodes[1].parameter_span = attack ? 0 : 3;
        /* A truthful span is valid as a descriptor but outside this method.
         * A forged zero span must fail descriptor admission independently. */
        budget = xr_xir_default_budget();
        CHECK(xr_xir_type_descriptors_verify(&f.types,&budget) ==
            (attack ? XR_XIR_BAD_TYPE : XR_XIR_OK));
        budget = xr_xir_default_budget(); copy = NULL;
        CHECK(xr_xir_interfaces_clone(&f.table,&f.types,&budget,&copy) == XR_XIR_BAD_TYPE && !copy);
        checked = NULL;
        CHECK(xr_xir_check(&f.module,NULL,&checked,NULL) == XR_XIR_BAD_TYPE && !checked);
        put32(packet.bytes+array+4,nodes[1].parameter_span);
        put32(packet.bytes+array+8,(uint32_t)nodes[1].element); digest_packet(&packet);
        CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&checked,NULL) == XR_XIR_BAD_TYPE && !checked);
    }
    put32(packet.bytes+array+4,2); put32(packet.bytes+array+8,XR_XIR_TYPE_PARAMETER_BASE+1);
    digest_packet(&packet);
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&checked,NULL) == XR_XIR_OK && checked);
    xr_xir_artifact_free(checked); xr_xir_checked_packet_free(&packet);
}
static void generic_method_boundary_cases(void) {
    generic_method_total_boundary(); generic_method_nested_boundaries();
}
#endif // XIR_GENERIC_METHOD_BOUNDARY_CASES_H
