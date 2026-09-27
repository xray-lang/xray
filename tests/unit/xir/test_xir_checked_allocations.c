/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_checked_allocations.c - Packet allocation rollback and physical release
 *
 * KEY CONCEPT:
 *   Fail every allocation in the actual reader, writer and semantic verifier.
 */
#include "base/xmalloc.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static size_t calls, live, fail_at = SIZE_MAX;
static void *packet_malloc(size_t size) {
    if (calls++ == fail_at) return NULL;
    void *p = xr_malloc(size); if (p) ++live; return p;
}
static void *packet_calloc(size_t count, size_t size) {
    if (calls++ == fail_at) return NULL;
    void *p = xr_calloc(count, size); if (p) ++live; return p;
}
static void packet_free(void *p) {
    if (p) { CHECK(live); --live; } xr_free(p);
}
#undef xr_malloc
#undef xr_calloc
#undef xr_free
#define xr_malloc(size) packet_malloc(size)
#define xr_calloc(count, size) packet_calloc(count, size)
#define xr_free(p) packet_free(p)
#include "xir/xxir_types.c"
#include "xir/xxir_type_layout.c"
#include "xir/xxir_generic.c"
#include "xir/xxir.c"
#include "xir/xxir_declarations.c"
#include "xir/xxir_verify.c"
#include "xir/xxir_layout.c"
#include "xir/xxir_checked.c"
#include "xir/xxir_specialize.c"
#include "xir_checked_fixture.h"
#include "xir_generic_fixture.h"
#include "xir_local_fixture.h"
#include "xir_types_fixture.h"
#include "xir_array_metadata_fixture.h"
#include "xir_array_generic_fixture.h"
#include "xir_nominal_checked_fixture.h"
#include "xir_nominal_generic_fixture.h"
#include "xir_nominal_chain_fixture.h"
#include "xir_struct_ops_fixture.h"
#include "xir_struct_set_fixture.h"
static XrXirArtifact *array_packet_fixture(void) {
    XirArrayMetadataFixture f; xir_array_metadata_init(&f);
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_check(&f.module, NULL, &checked, NULL) == XR_XIR_OK);
    return checked;
}
static void packet_failures(unsigned kind) {
    XrXirArtifact *checked = kind == 16 ? struct_set_checked(0) : kind == 15 ? struct_ops_checked(0) : kind == 14 ? nominal_chain_fixture(3, 2) : kind >= 9 ? nominal_checked_fixture(kind >= 12 ? 3 : kind == 11 ? 2 : kind == 10 ? 1 : 0) : kind == 8 ? array_generic_fixture() : kind == 7 ? array_packet_fixture() :
        kind == 6 ? constructed_fixture() : kind == 5 ? generic_callable_fixture() : kind == 4 ? function_ir_fixture() : kind == 3 ? callable_fixture() : kind == 2 ? local_fixture() : kind == 1 ? generic_fixture() : checked_fixture();
    if (kind == 13) {
        XrXirArtifact *closed = NULL;
        CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
        xr_xir_artifact_free(checked); checked = closed;
    }
    size_t baseline = live;
    XrXirCheckedPacket packet = {0};
    calls = 0;
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    size_t write_sites = calls;
    xr_xir_checked_packet_free(&packet); CHECK(live == baseline);
    for (size_t i = 0; i < write_sites; ++i) {
        calls = 0; fail_at = i;
        CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!packet.bytes && !packet.length && live == baseline);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked); CHECK(live == 1);
    XrXirArtifact *decoded = NULL;
    calls = 0;
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    size_t read_sites = calls;
    xr_xir_artifact_free(decoded); CHECK(live == 1);
    for (size_t i = 0; i < read_sites; ++i) {
        calls = 0; fail_at = i;
        CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!decoded && live == 1);
    }
    fail_at = SIZE_MAX;
    /* Exercise partial metadata teardown after valid integrity checks too. */
    for (size_t i = 64; i < packet.length; ++i) {
        packet.bytes[i] ^= 0xFF;
        checked_digest(packet.bytes, packet.length, packet.bytes + 32);
        XrXirStatus status = xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL);
        CHECK((status == XR_XIR_OK) == (decoded != NULL));
        xr_xir_artifact_free(decoded); CHECK(live == 1);
        packet.bytes[i] ^= 0xFF;
    }
    xr_xir_checked_packet_free(&packet); CHECK(!live);
    printf("%s packet physical release: %zu writer and %zu reader allocation sites\n",
        kind == 14 ? "Nominal field graph" : kind == 13 ? "Closed nominal fields" : kind == 12 ? "Nominal field expression" : kind == 11 ? "Nominal instance" : kind == 10 ? "Nominal Array field" : kind == 9 ? "Nominal declaration" : kind == 8 ? "Generic Array" : kind == 7 ? "Array operations" : kind == 6 ? "Constructed" : kind == 5 ? "Generic callable" : kind == 4 ? "Function" : kind == 3 ? "Callable" : kind == 2 ? "Local" : kind == 1 ? "Generic" : "Closed", write_sites, read_sites);
}

static void specialization_failures(unsigned callable) {
    XrXirArtifact *checked = callable == 7 ? nominal_chain_fixture(3, 2) : callable == 6 ? nominal_generic_fixture(true) : callable == 5 ? nominal_checked_fixture(3) : callable == 4 ? array_generic_fixture() : callable == 3 ? constructed_fixture() : callable == 2 ? generic_callable_fixture() : callable ? callable_fixture() : generic_fixture(), *output = NULL;
    XrXirModule built = *xr_xir_artifact_module(checked); built.stage = XR_XIR_BUILT;
    size_t baseline = live, sites[2] = {0};
    for (unsigned mode = 0; mode < 2; ++mode) {
        for (size_t attempt = 0; attempt <= sites[mode]; ++attempt) {
            calls = 0; fail_at = attempt ? attempt - 1 : SIZE_MAX;
            XrXirStatus status = mode ? xr_xir_specialize(checked, NULL, &output, NULL) :
                xr_xir_check(&built, NULL, &output, NULL);
            if (!attempt) { CHECK(status == XR_XIR_OK && output); sites[mode] = calls; }
            else CHECK(status == XR_XIR_OUT_OF_MEMORY && !output);
            xr_xir_artifact_free(output); CHECK(live == baseline);
        }
    }
    fail_at = SIZE_MAX; xr_xir_artifact_free(checked); CHECK(!live);
    printf("%s physical release: %zu checking and %zu specialization allocation sites\n", callable == 7 ? "Nominal field graph" : callable == 6 ? "Combined nominal and function" : callable == 5 ? "Nominal field substitution" : callable == 4 ? "Generic Array" : callable == 3 ? "Constructed" : callable == 2 ? "Generic callable" : callable ? "Callable" : "Generic", sites[0], sites[1]);
}
static void nominal_lowering_failures(void) {
    XrXirArtifact *checked = nominal_checked_fixture(3), *closed = NULL, *lowered = NULL;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    size_t baseline = live; calls = 0; fail_at = SIZE_MAX;
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    size_t sites = calls; xr_xir_artifact_free(lowered); CHECK(live == baseline);
    for (size_t i = 0; i < sites; ++i) {
        calls = 0; fail_at = i;
        CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!lowered && live == baseline);
    }
    fail_at = SIZE_MAX; xr_xir_artifact_free(closed); CHECK(!live);
    printf("Nominal lowering: %zu allocation sites physically released\n", sites);
}

static void nominal_layout_failures(void) {
    XrXirArtifact *checked = nominal_chain_fixture(3, 2), *closed = NULL;
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    const XrXirTypes *types = xr_xir_artifact_module(closed)->types;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirBudget original = xr_xir_default_budget(), budget = original;
    XrXirLayout layout = {0}; uint32_t offsets[] = {99, 99};
    size_t baseline = live; calls = 0;
    CHECK(xr_xir_nominal_layout(types, (XrXirType)256, &target, &budget, &layout, offsets, 2) == XR_XIR_OK);
    CHECK(layout.size == 64 && layout.alignment == 8 && offsets[0] == 0 && offsets[1] == 32 && live == baseline);
    size_t sites = calls;
    uint64_t work = original.work - budget.work;
    for (size_t i = 0; i < sites; ++i) {
        calls = 0; fail_at = i; budget = original; offsets[0] = offsets[1] = 99;
        CHECK(xr_xir_nominal_layout(types, (XrXirType)256, &target, &budget, &layout, offsets, 2) == XR_XIR_OUT_OF_MEMORY);
        CHECK(live == baseline && !layout.size && !layout.alignment && offsets[0] == 99 && offsets[1] == 99 &&
            !memcmp(&budget, &original, sizeof(budget)));
    }
    fail_at = SIZE_MAX; budget = original; budget.work = work - 1;
    XrXirBudget short_budget = budget;
    CHECK(xr_xir_nominal_layout(types, (XrXirType)256, &target, &budget, &layout, offsets, 2) == XR_XIR_BUDGET);
    CHECK(live == baseline && !layout.size && !layout.alignment && offsets[0] == 99 && offsets[1] == 99 &&
        !memcmp(&budget, &short_budget, sizeof(budget)));
    xr_xir_artifact_free(closed); CHECK(!live);
}
int main(void) {
    nominal_layout_failures();
    packet_failures(16);
    packet_failures(15);
    packet_failures(14);
    specialization_failures(7);
    specialization_failures(6);
    nominal_lowering_failures();
    packet_failures(9); packet_failures(10); packet_failures(11); packet_failures(12); packet_failures(13); specialization_failures(5);
    packet_failures(7); packet_failures(8); specialization_failures(4);
    packet_failures(6); specialization_failures(3);
    packet_failures(false); packet_failures(true); packet_failures(2); packet_failures(3); packet_failures(4); specialization_failures(false); specialization_failures(true); packet_failures(5); specialization_failures(2);
    return 0;
}
