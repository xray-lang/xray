/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generic_method_allocations.h - Generic method ownership at every allocation
 *
 * KEY CONCEPT:
 *   Completed and partially initialized method constraints have one cleanup owner.
 */
#ifndef XIR_GENERIC_METHOD_ALLOCATIONS_H
#define XIR_GENERIC_METHOD_ALLOCATIONS_H
#include "xir_generic_method_owned_fixture.h"
static void generic_method_clone_allocations(void) {
    CHECK(!live); size_t sites = 0;
    for (size_t attempt = 0; attempt <= sites; ++attempt) {
        GenericMethodOwnedFixture fixture; generic_method_owned_fixture(&fixture);
        XrXirBudget budget = xr_xir_default_budget(); XrXirInterfaceTable *copy = NULL;
        calls = 0; fail_at = attempt ? attempt-1 : SIZE_MAX;
        XrXirStatus status = xr_xir_interfaces_clone(&fixture.table,&fixture.types,&budget,&copy);
        if (!attempt) {
            CHECK(status == XR_XIR_OK && copy); sites = calls;
            CHECK(copy->declarations[1].methods != fixture.methods);
            for (uint32_t m = 0; m < 2; ++m) {
                const XrXirInterfaceMethod *method = &copy->declarations[1].methods[m];
                CHECK(method->name.bytes != fixture.methods[m].name.bytes);
                CHECK(method->constraints != &fixture.own[m]);
                CHECK(method->constraints[0].interfaces != &fixture.applications[m]);
                CHECK(method->constraints[0].interfaces[0].arguments != &fixture.parent);
            }
            memset(&fixture,0xCC,sizeof(fixture)); generic_method_owned_assert(copy);
        } else CHECK(status == XR_XIR_OUT_OF_MEMORY && !copy);
        xr_xir_interfaces_free(copy); CHECK(!live);
    }
    fail_at = SIZE_MAX;
    for (unsigned bad = 0; bad < 2; ++bad) {
        GenericMethodOwnedFixture fixture; generic_method_owned_fixture(&fixture);
        if (!bad) fixture.methods[0].own_parameter_count = 0;
        else fixture.methods[0].constraints = NULL;
        XrXirBudget budget = xr_xir_default_budget(); XrXirInterfaceTable *copy = NULL;
        CHECK(xr_xir_interfaces_clone(&fixture.table,&fixture.types,&budget,&copy) == XR_XIR_BAD_STRUCTURE);
        CHECK(!copy && !live);
    }
    printf("Generic method clone: %zu allocation failure sites\n",sites);
}
static void generic_method_packet_allocations(void) {
    CHECK(!live); fail_at = SIZE_MAX;
    GenericMethodOwnedFixture fixture; generic_method_owned_fixture(&fixture);
    XrXirArtifact *source = NULL, *decoded = NULL;
    CHECK(xr_xir_check(&fixture.module,NULL,&source,NULL) == XR_XIR_OK);
    memset(&fixture,0xCC,sizeof(fixture));
    XrXirCheckedPacket packet = {0}; size_t baseline = live; calls = 0;
    CHECK(xr_xir_checked_write(source,NULL,&packet,NULL) == XR_XIR_OK);
    size_t writes = calls;
    xr_xir_checked_packet_free(&packet); CHECK(live == baseline);
    for (size_t site = 0; site < writes; ++site) {
        calls = 0; fail_at = site;
        CHECK(xr_xir_checked_write(source,NULL,&packet,NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!packet.bytes && !packet.length && live == baseline);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_xir_checked_write(source,NULL,&packet,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(source); CHECK(live == 1); baseline = live; calls = 0;
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OK);
    size_t reads = calls;
    xr_xir_artifact_free(decoded); CHECK(live == baseline);
    for (size_t site = 0; site < reads; ++site) {
        calls = 0; fail_at = site; decoded = (XrXirArtifact *)(uintptr_t)1;
        CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!decoded && live == baseline);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OK);
    memset(packet.bytes,0xCC,packet.length); xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_artifact_verify(decoded,NULL,NULL) == XR_XIR_OK);
    generic_method_owned_assert(xr_xir_artifact_module(decoded)->types->interfaces);
    xr_xir_artifact_free(decoded); CHECK(!live);
    printf("Generic method packet: %zu writer and %zu reader allocation failure sites\n",writes,reads);
}
#endif // XIR_GENERIC_METHOD_ALLOCATIONS_H
