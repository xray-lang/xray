/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_original_string_builder_snapshot_source.c - Owned original builder snapshot admission
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
static void probe_source(const char *path) {
    unsigned char bytes[612]; FILE *file = fopen(path, "rb"); CHECK(file);
    size_t size = fread(bytes, 1, sizeof(bytes), file);
    bool valid = !ferror(file) && size == 611; int closed = fclose(file);
    CHECK(valid && !closed);
    probe_digest(bytes, 557, "b00bf926577b5282c46087b37ffbeac4ff43ad5e02fc102883420e5abc6a2973");
    probe_digest(bytes, 611, "50601008fc99e83bf2c106ed03be941798a094cbd6b60819fbbbddf5f7e7b632");
}
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    probe_source(argv[2]);
    char root[2048], file[2048];
    CHECK(strlen(argv[1]) < sizeof(root) && strlen(argv[2]) < sizeof(file));
    strcpy(root, argv[1]); strcpy(file, argv[2]);
    const XrXirCompileContext *context = source_program_owner(67108864, 128000000);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, root};
    XrXirSourceProductRequest request = {{session, file, &authority, context, argv[3], NULL, XR_XIR_PROGRAM, NULL},
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
        printf("string-builder-snapshot-original-public OwnedCheckedLoweredmetadata=PASS Lowered-stage=%u functions=%u runtime=NOT_RUN\n",
            (unsigned)module->stage, module->function_count);
        xr_xir_compile_artifact_free(lowered); lowered = NULL;
    }
    printf("string-builder-snapshot-original-public required=0 actual=%u stage=%u line=%d column=%d session-dead=1 product-dead=1 path=%s message=%s\n",
        status, diagnostic.stage, diagnostic.source.line, diagnostic.source.column,
        diagnostic.source_path ? diagnostic.source_path : "(none)", diagnostic.source.message);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic); source_program_owners_free();
    CHECK(!source_program_compile_allocations && !source_program_compile_capacity);
    printf("string-builder-snapshot-original-public prefix=557 adapter=54 full=611 instances=0 runtime=NOT_RUN compiler-physical=0/0 compiler-observer-table=0 result=%s\n",
        status == XR_XIR_OK ? "PASS_OWNED_CHECKED_LOWERED_METADATA_ONLY" : "FAIL");
    return status == XR_XIR_OK ? 0 : 1;
}
