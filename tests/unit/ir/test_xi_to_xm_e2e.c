/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xi_to_xm_e2e.c - End-to-end test: Xi IR → Xm → codegen → execute
 *
 * Validates that xi_to_xm_lower produces Xm that survives the full
 * compilation pipeline and produces correct machine code results.
 */

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/mman.h>
#endif
#include "../../../src/ir/xi.h"
#include "../../../src/jit/xi_to_xm.h"
#include "../../../src/jit/xm.h"
#include "../../../src/jit/xm_pass.h"
#include "../../../src/jit/xm_codegen.h"
#include "../../../src/jit/xm_jit.h"
#include "../../../src/jit/xm_jit_runtime.h"
#include "../../../src/jit/xm_offsets.h"
#include "../../../src/runtime/value/xvalue.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/coro/xcoroutine.h"
#include "../../../src/base/xmalloc.h"

/* Pick native codegen backend */
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
#  define xm_codegen_native(func, alloc) xm_codegen_arm64((func), (alloc))
#elif defined(__x86_64__) || defined(__amd64__) || defined(_M_X64)
#  define xm_codegen_native(func, alloc) xm_codegen_x64((func), (alloc))
#else
#  error "unsupported architecture"
#endif

/* JIT calling convention: (coro_ptr, args_ptr) -> raw int64 result */
typedef int64_t (*JitFn)(intptr_t, int64_t *);

/* Minimal fake JIT runtime env */
static XrCoroutine g_jit_coro;
static XrJitScratch g_jit_ctx;
static void *g_safepoint_page = NULL;

static void env_init(void) {
    if (!g_safepoint_page) {
        g_safepoint_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                MAP_ANON | MAP_PRIVATE, -1, 0);
        assert(g_safepoint_page != MAP_FAILED);
    }
    memset(&g_jit_coro, 0, sizeof(g_jit_coro));
    memset(&g_jit_ctx, 0, sizeof(g_jit_ctx));
    g_jit_coro.jit_ctx = &g_jit_ctx;
    g_jit_ctx.safepoint_page = g_safepoint_page;
}

static int64_t jit_call2(void *code, int64_t a, int64_t b) {
    memset(&g_jit_ctx, 0, sizeof(g_jit_ctx));
    g_jit_ctx.safepoint_page = g_safepoint_page;
    int64_t args[] = {a, b};
    return ((JitFn)code)((intptr_t)&g_jit_coro, args);
}

/* Stub XrType for tests */
static XrType stub_int = { .kind = XR_KIND_INT, .id = 1, .frozen = true };

/* Test counter */
static int tests_passed = 0;
static int tests_run = 0;

#define TEST(name) static void run_##name(void)
#define RUN(name) do { \
    tests_run++; \
    fprintf(stderr, "  " #name "..."); \
    run_##name(); \
    tests_passed++; \
    fprintf(stderr, " OK\n"); \
} while(0)

/* Helper: set up params array on XiFunc */
static void setup_params(XiFunc *f, uint16_t n, XiValue **pvs) {
    f->nparams = n;
    f->params = (XiValue **)xr_calloc(n, sizeof(XiValue *));
    for (uint16_t i = 0; i < n; i++)
        f->params[i] = pvs[i];
}

/* ========== Test: fn(a, b) -> a + b ========== */
TEST(add_i64) {
    XiFunc *f = xi_func_new("add", &stub_int);
    XiBlock *entry = xi_block_new(f);
    f->entry = entry;
    entry->sealed = true;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *b = xi_param(f, entry, 1, &stub_int);
    XiValue *pvs[] = {a, b};
    setup_params(f, 2, pvs);
    XiValue *sum = xi_binary(f, entry, XI_ADD, &stub_int, a, b);
    xi_block_set_return(entry, sum);

    /* Lower to Xm */
    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL && "lowering must succeed");

    /* Run optimization pipeline */
    xm_rebuild_preds(xm);
    xm_run_pipeline(xm, XM_OPT_BASIC);

    /* Generate machine code */
    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = xm_codegen_native(xm, &alloc);
    assert(res.success && "codegen must succeed");

    /* Execute */
    int64_t result = jit_call2(res.code, 17, 25);
    assert(result == 42 && "17 + 25 should be 42");

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(xm);
    xi_func_free(f);
}

/* ========== Test: fn(a, b) -> a - b ========== */
TEST(sub_i64) {
    XiFunc *f = xi_func_new("sub", &stub_int);
    XiBlock *entry = xi_block_new(f);
    f->entry = entry;
    entry->sealed = true;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *b = xi_param(f, entry, 1, &stub_int);
    XiValue *pvs[] = {a, b};
    setup_params(f, 2, pvs);
    XiValue *diff = xi_binary(f, entry, XI_SUB, &stub_int, a, b);
    xi_block_set_return(entry, diff);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL);

    xm_rebuild_preds(xm);
    xm_run_pipeline(xm, XM_OPT_BASIC);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = xm_codegen_native(xm, &alloc);
    assert(res.success);

    int64_t result = jit_call2(res.code, 100, 58);
    assert(result == 42 && "100 - 58 should be 42");

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(xm);
    xi_func_free(f);
}

/* ========== Test: fn(a, b) -> a * b ========== */
TEST(mul_i64) {
    XiFunc *f = xi_func_new("mul", &stub_int);
    XiBlock *entry = xi_block_new(f);
    f->entry = entry;
    entry->sealed = true;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *b = xi_param(f, entry, 1, &stub_int);
    XiValue *pvs[] = {a, b};
    setup_params(f, 2, pvs);
    XiValue *prod = xi_binary(f, entry, XI_MUL, &stub_int, a, b);
    xi_block_set_return(entry, prod);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL);

    xm_rebuild_preds(xm);
    xm_run_pipeline(xm, XM_OPT_BASIC);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = xm_codegen_native(xm, &alloc);
    assert(res.success);

    int64_t result = jit_call2(res.code, 6, 7);
    assert(result == 42 && "6 * 7 should be 42");

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(xm);
    xi_func_free(f);
}

/* ========== Test: fn(a, b) -> return const 99 ========== */
TEST(const_return) {
    XiFunc *f = xi_func_new("const99", &stub_int);
    XiBlock *entry = xi_block_new(f);
    f->entry = entry;
    entry->sealed = true;

    XiValue *c = xi_const_int(f, entry, 99, &stub_int);
    xi_block_set_return(entry, c);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL);

    xm_rebuild_preds(xm);
    xm_run_pipeline(xm, XM_OPT_BASIC);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = xm_codegen_native(xm, &alloc);
    assert(res.success);

    memset(&g_jit_ctx, 0, sizeof(g_jit_ctx));
    g_jit_ctx.safepoint_page = g_safepoint_page;
    int64_t result = ((JitFn)res.code)((intptr_t)&g_jit_coro, NULL);
    assert(result == 99 && "should return 99");

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(xm);
    xi_func_free(f);
}

/* ========== Main ========== */

int main(void) {
    env_init();
    fprintf(stderr, "=== Xi-to-Xm E2E Tests ===\n");

    RUN(add_i64);
    RUN(sub_i64);
    RUN(mul_i64);
    RUN(const_return);

    fprintf(stderr, "\n=== %d/%d E2E tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
