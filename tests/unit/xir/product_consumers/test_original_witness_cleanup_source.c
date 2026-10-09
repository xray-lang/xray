/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_original_witness_cleanup_source.c - Owned original witness-source admission
 *
 * KEY CONCEPT:
 *   A required positive retains its original source and remains a failure when
 *   admission is unavailable. Diagnostics and artifacts outlive their producer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_types.h"
#include "xir/xxir_declarations.h"
#include "base/xsha256.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Actual Checked identity");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 22u && XR_XIR_CALL_ABI_VERSION == 28u &&
    XR_XIR_PROGRAM_ABI_VERSION == 29u, "Current public execution contracts");
static void probe_digest(const void *bytes, size_t size, const char *expected) {
    uint8_t digest[32]; char hex[65];
    xr_sha256(bytes, size, digest);
    for (unsigned i = 0; i < 32; ++i) {
        hex[i * 2] = "0123456789abcdef"[digest[i] >> 4];
        hex[i * 2 + 1] = "0123456789abcdef"[digest[i] & 15];
    }
    hex[64] = 0; CHECK(!strcmp(hex, expected));
}
typedef struct WitnessSourceDescriptor {
    const char *family;
    size_t prefix_bytes, full_bytes;
    const char *prefix_sha256, *full_sha256;
} WitnessSourceDescriptor;
static const WitnessSourceDescriptor witness_sources[] = {
    {"witness_panic_only_defer", 711, 842,
        "53b4b073c6af3571bf83b8ed1dafa43a69a3b903818d3c31774411c404805897",
        "fa4f621858dc1113b71ae898a30a87bcd56ae8cd658dfd888d255d43613977eb"},
    {"witness_invoke_defer", 1007, 1138,
        "67639e0f465ca7b9a5da0baa7bc94fe4ac84d1a73e6285295a13d79b4165dd99",
        "2113723f45435d5567c4fcce33b540ff64072e073e7a57f023318257b3b54c42"},
};
static const WitnessSourceDescriptor *witness_source_descriptor(const char *family) {
    for (size_t i = 0; i < sizeof(witness_sources) / sizeof(witness_sources[0]); ++i)
        if (!strcmp(family, witness_sources[i].family)) return &witness_sources[i];
    return NULL;
}
static void probe_source(const char *path, const WitnessSourceDescriptor *descriptor) {
    unsigned char bytes[1139]; FILE *input = fopen(path, "rb"); CHECK(input);
    size_t size = fread(bytes, 1, descriptor->full_bytes + 1, input);
    bool valid = !ferror(input) && size == descriptor->full_bytes; int closed = fclose(input);
    CHECK(valid && !closed);
    probe_digest(bytes, descriptor->prefix_bytes, descriptor->prefix_sha256);
    probe_digest(bytes, descriptor->full_bytes, descriptor->full_sha256);
}
int main(int argc, char **argv) {
    if (argc != 5) return 2;
    const WitnessSourceDescriptor *descriptor = witness_source_descriptor(argv[1]);
    if (!descriptor) return 2;
    probe_source(argv[3], descriptor);
    char root[2048], file[2048];
    CHECK(strlen(argv[2]) < sizeof(root) && strlen(argv[3]) < sizeof(file));
    strcpy(root, argv[2]); strcpy(file, argv[3]);
    const XrXirCompileContext *context = source_program_owner(67108864, 128000000);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, root};
    XrXirSourceProductRequest request = {{session, file, &authority, context, argv[4], NULL, XR_XIR_PROGRAM, NULL},
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product = NULL; XrXirSourceProductDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_product_build(&request, &product, &diagnostic);
    XrXirArtifact *checked = NULL, *lowered = NULL; XrXirCheckedPacket golden = {0};
    if (status == XR_XIR_OK) {
        XrXirSourceProductPacketView packet = {0}; CHECK(product);
        CHECK(xr_xir_compile_source_product_verify(product, 16777216, NULL) == XR_XIR_OK);
        CHECK(xr_xir_compile_source_product_packet(product, XR_XIR_SOURCE_PRODUCT_CLOSED, &packet) == XR_XIR_OK);
        CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
        CHECK(xr_xir_compile_checked_write(checked, &golden, NULL) == XR_XIR_OK);
        CHECK(golden.bytes && golden.bytes != packet.bytes && golden.length == packet.length);
        CHECK(!memcmp(golden.bytes, packet.bytes, packet.length));
    } else {
        CHECK(!product && diagnostic.status == status && diagnostic.source.message[0]);
        if (diagnostic.source.line > 0 || diagnostic.source.column > 0)
            CHECK(diagnostic.source_path && diagnostic.source_path[0]);
    }
    XrXirSourceProductDiagnostic saved = diagnostic; char path_copy[2048] = {0};
    if (diagnostic.source_path) {
        CHECK(strlen(diagnostic.source_path) < sizeof(path_copy)); strcpy(path_copy, diagnostic.source_path);
    }
    xr_compile_session_free(session); xr_xir_compile_source_product_free(product);
    memset(root, 0xa5, sizeof(root)); memset(file, 0xa5, sizeof(file));
    memset(&request, 0xa5, sizeof(request)); memset(&authority, 0xa5, sizeof(authority));
    CHECK(!memcmp(&saved, &diagnostic, sizeof(saved)));
    if (path_copy[0]) CHECK(!strcmp(path_copy, diagnostic.source_path));
    if (checked) {
        CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_OK);
        XrXirCheckedPacket rewritten = {0};
        CHECK(xr_xir_compile_checked_write(checked, &rewritten, NULL) == XR_XIR_OK);
        CHECK(rewritten.bytes && rewritten.length == golden.length);
        CHECK(!memcmp(rewritten.bytes, golden.bytes, golden.length));
        xr_xir_compile_checked_packet_free(&rewritten);
        XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
        CHECK(xr_xir_compile_lower(checked, &target, &lowered, NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(checked); checked = NULL;
        xr_xir_compile_checked_packet_free(&golden);
        CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
        const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
        CHECK(module && module->stage == XR_XIR_LOWERED && module->declarations);
        printf("witness-original-public family=%s OwnedCheckedLoweredmetadata=PASS Lowered-stage=%u functions=%u runtime=NOT_RUN\n",
            descriptor->family, (unsigned)module->stage, module->function_count);
        xr_xir_compile_artifact_free(lowered); lowered = NULL;
    }
    printf("witness-original-public family=%s required=0 actual=%u stage=%u line=%d column=%d session-dead=1 product-dead=1 path=%s message=%s\n",
        descriptor->family, status, diagnostic.stage, diagnostic.source.line, diagnostic.source.column,
        diagnostic.source_path ? diagnostic.source_path : "(none)", diagnostic.source.message);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic); source_program_owners_free();
    CHECK(!source_program_compile_allocations && !source_program_compile_capacity);
    printf("witness-original-public family=%s prefix=%zu adapter=131 full=%zu instances=0 runtime=NOT_RUN compiler-physical=0/0 compiler-observer-table=0 result=%s\n",
        descriptor->family, descriptor->prefix_bytes, descriptor->full_bytes,
        status == XR_XIR_OK ? "PASS_OWNED_CHECKED_LOWERED_METADATA_ONLY" : "FAIL");
    return status == XR_XIR_OK ? 0 : 1;
}
