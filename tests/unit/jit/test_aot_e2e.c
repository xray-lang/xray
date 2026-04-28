/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_aot_e2e.c - End-to-end AOT tests: XIR â†?ARM64 (AAPCS) â†?Execute
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
#include "../../../src/jit/xir.h"
#include "../../../src/jit/xir_pass.h"
#include "../../../src/jit/xir_codegen.h"
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
    if (sig == 11) write(2, "SIGSEGV\n", 8);
    else if (sig == 10) write(2, "SIGBUS\n", 7);
    else write(2, "OTHER\n", 6);
    _exit(128 + sig);
}

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, " FAIL: %s\n", msg); \
        tests_failed++; \
        return; \
    } \
} while(0)

/*
 * Test 1: fn() -> int { return 42; }
 */
static void test_return_constant(void) {
    fprintf(stderr, "  test_aot_return_constant...");

    XirFunc *func = xir_func_new("ret42");
    func->num_params = 0;

    XirBlock *entry = xir_func_add_block(func, "entry");
    XirRef c42 = xir_const_i64(func, 42);
    XirRef v0 = xir_emit_unary(func, entry, XIR_CONST_I64, XR_REP_I64, c42);
    xir_block_set_ret(entry, v0);

    xir_run_pipeline(func, XIR_OPT_BASIC);

    XirCodeAlloc alloc;
    xir_code_alloc_init(&alloc);

    XirCodegenResult result = xir_codegen_arm64(func, &alloc);
    CHECK(result.success, "codegen failed");

    AotFn0 fn = (AotFn0)result.code;
    int64_t ret = fn();
    CHECK(ret == 42, "expected 42");

    fprintf(stderr, " OK (ret=%lld)\n", (long long)ret);
    tests_passed++;
    xir_func_destroy(func);
    xir_code_alloc_destroy(&alloc);
}

/*
 * Test 2: fn(a: int, b: int) -> int { return a + b; }
 */
static void test_add(void) {
    fprintf(stderr, "  test_aot_add...");

    XirFunc *func = xir_func_new("add");
    func->num_params = 2;

    // Declare param vregs
    XirRef p0 = xir_new_vreg(func, XR_REP_I64);  // a
    XirRef p1 = xir_new_vreg(func, XR_REP_I64);  // b

    XirBlock *entry = xir_func_add_block(func, "entry");
    XirRef sum = xir_emit(func, entry, XIR_ADD, XR_REP_I64, p0, p1);
    xir_block_set_ret(entry, sum);

    xir_run_pipeline(func, XIR_OPT_BASIC);

    XirCodeAlloc alloc;
    xir_code_alloc_init(&alloc);

    XirCodegenResult result = xir_codegen_arm64(func, &alloc);
    CHECK(result.success, "codegen failed");

    AotFn2 fn = (AotFn2)result.code;
    int64_t ret = fn(10, 32);
    CHECK(ret == 42, "expected 42");

    ret = fn(100, -58);
    CHECK(ret == 42, "expected 42");

    fprintf(stderr, " OK\n");
    tests_passed++;
    xir_func_destroy(func);
    xir_code_alloc_destroy(&alloc);
}

/*
 * Test 3: fn(a: int, b: int) -> int { return a * b - a; }
 */
static void test_mul_sub(void) {
    fprintf(stderr, "  test_aot_mul_sub...");

    XirFunc *func = xir_func_new("mul_sub");
    func->num_params = 2;

    XirRef p0 = xir_new_vreg(func, XR_REP_I64);
    XirRef p1 = xir_new_vreg(func, XR_REP_I64);

    XirBlock *entry = xir_func_add_block(func, "entry");
    XirRef prod = xir_emit(func, entry, XIR_MUL, XR_REP_I64, p0, p1);
    XirRef diff = xir_emit(func, entry, XIR_SUB, XR_REP_I64, prod, p0);
    xir_block_set_ret(entry, diff);

    xir_run_pipeline(func, XIR_OPT_BASIC);

    XirCodeAlloc alloc;
    xir_code_alloc_init(&alloc);

    XirCodegenResult result = xir_codegen_arm64(func, &alloc);
    CHECK(result.success, "codegen failed");

    AotFn2 fn = (AotFn2)result.code;
    // 7 * 6 - 7 = 35
    int64_t ret = fn(7, 6);
    CHECK(ret == 35, "expected 35");

    fprintf(stderr, " OK (ret=%lld)\n", (long long)ret);
    tests_passed++;
    xir_func_destroy(func);
    xir_code_alloc_destroy(&alloc);
}

/*
 * Test 4: fn(n: int) -> int { if (n <= 1) return n; return n * 2; }
 * Tests conditional branch.
 */
static void test_branch(void) {
    fprintf(stderr, "  test_aot_branch...");

    XirFunc *func = xir_func_new("branch_test");
    func->num_params = 1;

    XirRef p0 = xir_new_vreg(func, XR_REP_I64);

    XirBlock *entry = xir_func_add_block(func, "entry");
    XirBlock *then_blk = xir_func_add_block(func, "then");
    XirBlock *else_blk = xir_func_add_block(func, "else");

    // entry: cmp n <= 1, branch
    XirRef c1 = xir_const_i64(func, 1);
    XirRef one = xir_emit_unary(func, entry, XIR_CONST_I64, XR_REP_I64, c1);
    XirRef cond = xir_emit(func, entry, XIR_LE, XR_REP_I64, p0, one);
    xir_block_set_br(entry, cond, then_blk, else_blk);

    // then: return n
    xir_block_set_ret(then_blk, p0);

    // else: return n * 2
    XirRef c2 = xir_const_i64(func, 2);
    XirRef two = xir_emit_unary(func, else_blk, XIR_CONST_I64, XR_REP_I64, c2);
    XirRef doubled = xir_emit(func, else_blk, XIR_MUL, XR_REP_I64, p0, two);
    xir_block_set_ret(else_blk, doubled);

    xir_run_pipeline(func, XIR_OPT_BASIC);

    XirCodeAlloc alloc;
    xir_code_alloc_init(&alloc);

    XirCodegenResult result = xir_codegen_arm64(func, &alloc);
    CHECK(result.success, "codegen failed");

    AotFn1 fn = (AotFn1)result.code;

    int64_t ret0 = fn(0);
    CHECK(ret0 == 0, "expected 0 for n=0");

    int64_t ret1 = fn(1);
    CHECK(ret1 == 1, "expected 1 for n=1");

    int64_t ret5 = fn(5);
    CHECK(ret5 == 10, "expected 10 for n=5");

    fprintf(stderr, " OK (0â†?lld, 1â†?lld, 5â†?lld)\n",
            (long long)ret0, (long long)ret1, (long long)ret5);
    tests_passed++;
    xir_func_destroy(func);
    xir_code_alloc_destroy(&alloc);
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

    fprintf(stderr, "\n--- AOT Results: %d passed, %d failed ---\n\n",
            tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
