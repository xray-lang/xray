/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_channel_owner.c - Preserve Channel failures through complete owner destruction
 *
 * KEY CONCEPT:
 *   Failed Source admission remains a failed positive after its diagnostics and
 *   allocation owners have been inspected and destroyed. Input guards are separate.
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "channel_owner_cases.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked identity");

static void path_join(char *output, size_t capacity, const char *root, const char *tail) {
    int n = snprintf(output, capacity, "%s/%s", root, tail);
    CHECK(n > 0 && (size_t)n < capacity);
}

static bool run_case(const ChannelOwnerCase *test, unsigned with_diagnostic,
    const char *root, const char *stdlib, const char *control_root) {
    XrCompileResourceLimits limits = {UINT64_C(67108864), UINT64_C(33554432), UINT64_C(128000000)};
    XrXirCompileContext context = {.limits = xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILER_SESSION_OK);
    char control_file[2048], directory[2048], filename[2048];
    path_join(control_file, sizeof(control_file), control_root, "root.xr");
    XrModuleIdentityAuthority control_authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, control_root};
    XrXirSourceProductRequest request = {{session, control_file, &control_authority, &context,
        stdlib, NULL, XR_XIR_PROGRAM, NULL}, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *control = NULL;
    CHECK(xr_xir_compile_source_product_build(&request, &control, NULL) == XR_XIR_OK && control);
    XrXirSourceProductPacketView packet = {0};
    CHECK(xr_xir_compile_source_product_packet(control, XR_XIR_SOURCE_PRODUCT_CLOSED, &packet) == XR_XIR_OK);
    CHECK(packet.length > 64);
    uint8_t digest[32]; memcpy(digest, packet.bytes + 32, sizeof(digest));
    size_t packet_length = packet.length;
    path_join(directory, sizeof(directory), root, test->name);
    path_join(filename, sizeof(filename), directory, "root.xr");
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, directory};
    request.source.entry_path = filename; request.source.authority = &authority;
    XrCompileResourceStats before = {0}, after = {0};
    CHECK(xr_compile_resources_stats(context.resources, &before) == XR_COMPILE_RESOURCE_OK);
    size_t guard_live = instance_compile_live, guard_bytes = instance_compile_bytes;
    XrXirSourceProduct *occupied = control;
    XrXirSourceProductDiagnostic guard = {0};
    CHECK(xr_xir_compile_source_product_build(&request, &occupied, &guard) == XR_XIR_BAD_STRUCTURE);
    CHECK(occupied == control && guard.status == XR_XIR_BAD_STRUCTURE && guard.stage == XR_XIR_SOURCE_PRODUCT_INPUT);
    CHECK(!guard.snapshot && !guard.source_path);
    CHECK(xr_compile_resources_stats(context.resources, &after) == XR_COMPILE_RESOURCE_OK);
    CHECK(!memcmp(&before, &after, sizeof(before)) && guard_live == instance_compile_live && guard_bytes == instance_compile_bytes);
    xr_xir_compile_source_product_diagnostic_free(&guard);
    printf("channel-guard case=%s diagnostic=%u output-preserved=1 no-allocation=1 no-work=1\n", test->name, with_diagnostic);
    XrXirSourceProduct *product = NULL;
    XrXirSourceProductDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_product_build(&request, &product, with_diagnostic ? &diagnostic : NULL);
    char saved_path[2048] = {0}, saved_message[sizeof(diagnostic.source.message)] = {0};
    if (status != XR_XIR_OK) {
        CHECK(!product);
        if (with_diagnostic) {
            CHECK(diagnostic.status == status && diagnostic.source.status == status && diagnostic.stage == XR_XIR_SOURCE_PRODUCT_CHECK);
            CHECK(diagnostic.source_path && diagnostic.source_path != filename && !diagnostic.snapshot && diagnostic.source.line > 0);
            CHECK(strlen(diagnostic.source_path) < sizeof(saved_path));
            memcpy(saved_path, diagnostic.source_path, strlen(diagnostic.source_path) + 1);
            memcpy(saved_message, diagnostic.source.message, sizeof(saved_message));
            CHECK(saved_message[0]);
        }
    } else CHECK(product);
    xr_compile_session_free(session); session = NULL;
    memset(filename, 0xa5, sizeof(filename)); memset(directory, 0xa5, sizeof(directory));
    memset(control_file, 0xa5, sizeof(control_file));
    CHECK(xr_xir_compile_source_product_verify(control, 1048576, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_source_product_packet(control, XR_XIR_SOURCE_PRODUCT_CLOSED, &packet) == XR_XIR_OK);
    CHECK(packet.length == packet_length && !memcmp(digest, packet.bytes + 32, sizeof(digest)));
    xr_xir_compile_source_product_free(control); xr_xir_compile_source_product_free(product);
    xr_compile_resources_release(context.resources); context.resources = NULL;
    if (status != XR_XIR_OK && with_diagnostic) {
        CHECK(instance_compile_live > 0 && instance_compile_bytes > 0);
        CHECK(!strcmp(saved_path, diagnostic.source_path) && !memcmp(saved_message, diagnostic.source.message, sizeof(saved_message)));
        printf("channel-detached case=%s status=%u stage=%u line=%d session-dead=1 external-ledger-released=1 products-dead=1 path-owned=1 message=%s\n",
            test->name, status, diagnostic.stage, diagnostic.source.line, diagnostic.source.message);
    } else if (!with_diagnostic) instance_compile_zero();
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    const XrXirSourceProductDiagnostic zero = {0}; CHECK(!memcmp(&diagnostic, &zero, sizeof(zero)));
    xr_xir_compile_source_product_diagnostic_free(&diagnostic); instance_compile_zero();
    printf("channel-owner case=%s diagnostic=%u expected=0 actual=%u cleanup=0/0 control-preserved=1 result=%s\n",
        test->name, with_diagnostic, status, status == XR_XIR_OK ? "PASS" : "FAIL");
    return status == XR_XIR_OK;
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    unsigned failures = 0;
    for (size_t i = 0; i < sizeof(channel_owner_cases)/sizeof(channel_owner_cases[0]); ++i)
        for (unsigned diagnostic = 0; diagnostic < 2; ++diagnostic)
            if (!run_case(&channel_owner_cases[i], diagnostic, argv[1], argv[2], argv[3])) ++failures;
    instance_compile_report();
    printf("channel-owner cases=8 mismatches=%u physical=0/0 result=%s\n", failures, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
