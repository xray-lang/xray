/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_integer_width_owned.c - Bitwise source owners and diagnostics
 *
 * KEY CONCEPT:
 *   Positive admission requirements remain failures when unavailable, while
 *   their real source owners and diagnostics are still released completely.
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Actual Checked identity");

int main(int argc, char **argv) {
    if (argc != 4) return 2;
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
    XrXirArtifact *checked = NULL, *lowered = NULL;
    if (status == XR_XIR_OK) {
        XrXirSourceProductPacketView packet = {0}; CHECK(product);
        CHECK(xr_xir_compile_source_product_verify(product, 16777216, NULL) == XR_XIR_OK);
        CHECK(xr_xir_compile_source_product_packet(product, XR_XIR_SOURCE_PRODUCT_CLOSED, &packet) == XR_XIR_OK);
        CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
    } else CHECK(!product && diagnostic.status == status && diagnostic.source.message[0]);
    xr_compile_session_free(session);
    xr_xir_compile_source_product_free(product);
    memset(root, 0xa5, sizeof(root)); memset(file, 0xa5, sizeof(file));
    memset(&request, 0xa5, sizeof(request)); memset(&authority, 0xa5, sizeof(authority));
    if (checked) {
        CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_OK);
        XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
        CHECK(xr_xir_compile_lower(checked, &target, &lowered, NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(checked);
        CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(lowered);
        puts("integer-width-owned detached-Checked=PASS detached-Lowered=PASS session-dead=1 product-dead=1 input-path-dead=1");
    } else printf("integer-width-owned required=0 actual=%u stage=%u line=%d column=%d session-dead=1 product-dead=1 input-path-dead=1 message=%s\n",
        status, diagnostic.stage, diagnostic.source.line, diagnostic.source.column, diagnostic.source.message);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    source_program_owners_free();
    CHECK(!source_program_compile_allocations && !source_program_compile_capacity);
    printf("integer-width-owned required=0 actual=%u compiler-physical=0/0 table=0 result=%s\n", status,
        status == XR_XIR_OK ? "PASS" : "FAIL");
    return status == XR_XIR_OK ? 0 : 1;
}
