/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_process_lifecycle.c - Hold a real product during runner termination
 *
 * KEY CONCEPT:
 *   A flushed readiness record proves public compilation completed before a
 *   deliberate host wait. Forced process death is not compiler owner cleanup.
 */
#define _POSIX_C_SOURCE 200809L
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "base/xplatform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if XR_OS_WINDOWS
#include <windows.h>
#else
#include <time.h>
#endif
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked identity");

static void wait_for_termination(void) {
#if XR_OS_WINDOWS
    Sleep(30000);
#else
    struct timespec remaining = {30, 0};
    while (nanosleep(&remaining, &remaining)) {}
#endif
}

int main(int argc, char **argv) {
    if (argc != 4 || (strcmp(argv[3], "normal") && strcmp(argv[3], "nonzero") && strcmp(argv[3], "timeout"))) return 2;
    char path[2048];
    int length = snprintf(path, sizeof(path), "%s/root.xr", argv[1]);
    CHECK(length > 0 && (size_t)length < sizeof(path));
    XrCompileResourceLimits caps = {UINT64_C(67108864), UINT64_C(33554432), UINT64_C(128000000)};
    XrXirCompileContext context = {.limits = xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&caps, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, argv[1]};
    XrXirSourceProductRequest request = {{session, path, &authority, &context, argv[2],
        NULL, XR_XIR_PROGRAM, NULL}, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product = NULL;
    CHECK(xr_xir_compile_source_product_build(&request, &product, NULL) == XR_XIR_OK && product);
    xr_compile_session_free(session); session = NULL;
    memset(path, 0xa5, sizeof(path));
    CHECK(xr_xir_compile_source_product_verify(product, 1048576, NULL) == XR_XIR_OK);
    CHECK(instance_compile_live > 0 && instance_compile_bytes > 0);
    printf("process-lifecycle ready=1 mode=%s product-verified=1 session-dead=1 physical=%zu/%zu\n",
        argv[3], instance_compile_live, instance_compile_bytes);
    CHECK(fflush(stdout) == 0);
    fprintf(stderr, "process-lifecycle stderr-ready mode=%s\n", argv[3]);
    CHECK(fflush(stderr) == 0);
    if (!strcmp(argv[3], "timeout")) wait_for_termination();
    xr_xir_compile_source_product_free(product);
    xr_compile_resources_release(context.resources);
    instance_compile_report();
    printf("process-lifecycle released=1 mode=%s physical=0/0\n", argv[3]);
    if (!strcmp(argv[3], "nonzero")) return 23;
    return !strcmp(argv[3], "timeout") ? 29 : 0;
}
