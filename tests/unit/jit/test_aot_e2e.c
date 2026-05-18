/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_aot_e2e.c - End-to-end AOT tests: Xm â†?ARM64 (AAPCS) â†?Execute
 *
 * KEY CONCEPT:
 *   Verifies AOT codegen produces correct native code with AAPCS
 *   calling convention. Functions are called directly as native
 *   function pointers (no VM, no coro).
 */

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include "../../../src/jit/xm.h"
#include "../../../src/jit/xm_pass.h"
#include "../../../src/jit/xm_codegen.h"
#include "../test_win_compat.h"

// AOT calling convention: AAPCS (params in x0-x7, return in x0)
typedef int64_t (*AotFn0)(void);
typedef int64_t (*AotFn1)(int64_t);
typedef int64_t (*AotFn2)(int64_t, int64_t);

static int tests_passed = 0;
static int tests_failed = 0;

static void crash_handler(int sig) {
    const char *msg = "\n!!! SIGNAL received: ";
    write(2, msg, strlen(msg));
    if (sig == 11)
        write(2, "SIGSEGV\n", 8);
    else if (sig == 10)
        write(2, "SIGBUS\n", 7);
    else
        write(2, "OTHER\n", 6);
    _exit(128 + sig);
}

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, " FAIL: %s\n", msg);                                                   \
            tests_failed++;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

/*
 * Test 1:() -> int { return 42; }
 */
static void test_return_constant(void) {
    fprintf(stderr, "  test_aot_return_constant...");

    XmFunc *func = xm_func_new("ret42");
    func->num_params = 0;

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmRef c42 = xm_const_i64(func, 42);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c42);
    xm_block_set_ret(entry, v0);

    xm_run_pipeline(func, XM_OPT_BASIC);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);

    XmCodegenResult result = xm_codegen_arm64(func, &alloc);
    CHECK(result.success, "codegen failed");

    AotFn0 fn = (AotFn0) result.code;
    int64_t ret = fn();
    CHECK(ret == 42, "expected 42");

    fprintf(stderr, " OK (ret=%lld)\n", (long long) ret);
    tests_passed++;
    xm_func_destroy(func);
    xm_code_alloc_destroy(&alloc);
}

/*
 * Test 2:(a: int, b: int) -> int { return a + b; }
 */
static void test_add(void) {
    fprintf(stderr, "  test_aot_add...");

    XmFunc *func = xm_func_new("add");
    func->num_params = 2;

    // Declare param vregs
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);  // a
    XmRef p1 = xm_new_vreg(func, XR_REP_I64);  // b

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmRef sum = xm_emit(func, entry, XM_ADD, XR_REP_I64, p0, p1);
    xm_block_set_ret(entry, sum);

    xm_run_pipeline(func, XM_OPT_BASIC);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);

    XmCodegenResult result = xm_codegen_arm64(func, &alloc);
    CHECK(result.success, "codegen failed");

    AotFn2 fn = (AotFn2) result.code;
    int64_t ret = fn(10, 32);
    CHECK(ret == 42, "expected 42");

    ret = fn(100, -58);
    CHECK(ret == 42, "expected 42");

    fprintf(stderr, " OK\n");
    tests_passed++;
    xm_func_destroy(func);
    xm_code_alloc_destroy(&alloc);
}

/*
 * Test 3:(a: int, b: int) -> int { return a * b - a; }
 */
static void test_mul_sub(void) {
    fprintf(stderr, "  test_aot_mul_sub...");

    XmFunc *func = xm_func_new("mul_sub");
    func->num_params = 2;

    XmRef p0 = xm_new_vreg(func, XR_REP_I64);
    XmRef p1 = xm_new_vreg(func, XR_REP_I64);

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmRef prod = xm_emit(func, entry, XM_MUL, XR_REP_I64, p0, p1);
    XmRef diff = xm_emit(func, entry, XM_SUB, XR_REP_I64, prod, p0);
    xm_block_set_ret(entry, diff);

    xm_run_pipeline(func, XM_OPT_BASIC);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);

    XmCodegenResult result = xm_codegen_arm64(func, &alloc);
    CHECK(result.success, "codegen failed");

    AotFn2 fn = (AotFn2) result.code;
    // 7 * 6 - 7 = 35
    int64_t ret = fn(7, 6);
    CHECK(ret == 35, "expected 35");

    fprintf(stderr, " OK (ret=%lld)\n", (long long) ret);
    tests_passed++;
    xm_func_destroy(func);
    xm_code_alloc_destroy(&alloc);
}

/*
 * Test 4:(n: int) -> int { if (n <= 1) return n; return n * 2; }
 * Tests conditional branch.
 */
static void test_branch(void) {
    fprintf(stderr, "  test_aot_branch...");

    XmFunc *func = xm_func_new("branch_test");
    func->num_params = 1;

    XmRef p0 = xm_new_vreg(func, XR_REP_I64);

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmBlock *then_blk = xm_func_add_block(func, "then");
    XmBlock *else_blk = xm_func_add_block(func, "else");

    // entry: cmp n <= 1, branch
    XmRef c1 = xm_const_i64(func, 1);
    XmRef one = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c1);
    XmRef cond = xm_emit(func, entry, XM_LE, XR_REP_I64, p0, one);
    xm_block_set_br(entry, cond, then_blk, else_blk);

    // then: return n
    xm_block_set_ret(then_blk, p0);

    // else: return n * 2
    XmRef c2 = xm_const_i64(func, 2);
    XmRef two = xm_emit_unary(func, else_blk, XM_CONST_I64, XR_REP_I64, c2);
    XmRef doubled = xm_emit(func, else_blk, XM_MUL, XR_REP_I64, p0, two);
    xm_block_set_ret(else_blk, doubled);

    xm_run_pipeline(func, XM_OPT_BASIC);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);

    XmCodegenResult result = xm_codegen_arm64(func, &alloc);
    CHECK(result.success, "codegen failed");

    AotFn1 fn = (AotFn1) result.code;

    int64_t ret0 = fn(0);
    CHECK(ret0 == 0, "expected 0 for n=0");

    int64_t ret1 = fn(1);
    CHECK(ret1 == 1, "expected 1 for n=1");

    int64_t ret5 = fn(5);
    CHECK(ret5 == 10, "expected 10 for n=5");

    fprintf(stderr, " OK (0â†?lld, 1â†?lld, 5â†?lld)\n", (long long) ret0, (long long) ret1,
            (long long) ret5);
    tests_passed++;
    xm_func_destroy(func);
    xm_code_alloc_destroy(&alloc);
}

int main(void) {
    xr_test_suppress_dialogs();
    signal(SIGSEGV, crash_handler);
    signal(SIGBUS, crash_handler);

    fprintf(stderr, "\n=== AOT End-to-End Tests ===\n");

    test_return_constant();
    test_add();
    test_mul_sub();
    test_branch();

    fprintf(stderr, "\n--- AOT Results: %d passed, %d failed ---\n\n", tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
