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
#include "xir/xxir_generic.c"
#include "xir/xxir.c"
#include "xir/xxir_declarations.c"
#include "xir/xxir_verify.c"
#include "xir/xxir_layout.c"
#include "xir/xxir_checked.c"
#include "xir/xxir_specialize.c"
#include "xir_checked_fixture.h"
#include "xir_generic_fixture.h"
static void packet_failures(bool generic) {
    XrXirArtifact *checked = generic ? generic_fixture() : checked_fixture();
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
        generic ? "Generic" : "Closed", write_sites, read_sites);
}

static void specialization_failures(void) {
    XrXirArtifact *checked = generic_fixture(), *output = NULL;
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
    printf("Generic physical release: %zu checking and %zu specialization allocation sites\n", sites[0], sites[1]);
}
int main(void) {
    packet_failures(false); packet_failures(true); specialization_failures();
    return 0;
}
