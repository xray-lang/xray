/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_jit_e2e.c - End-to-end JIT tests: Xm → ARM64 → Execute
 */

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <signal.h>
#include <string.h>
#include "../test_win_compat.h"
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#include <sys/mman.h>
#endif
#include "../../../src/jit/xm.h"
#include "../../../src/jit/xm_printer.h"
#include "../../../src/jit/xm_codegen.h"
#include "../../../src/jit/xm_pass.h"
#include "../../../src/jit/xm_pass_sccp.h"
#include "../../../src/jit/xm_jit.h"
#include "../../../src/jit/xm_jit_runtime.h"
#include "../../../src/jit/xm_offsets.h"
#include "../../../src/runtime/value/xvalue.h"
#include "../../../src/coro/xcoroutine.h"

/*
 * Pick the native JIT backend for this host. xm_codegen_arm64 and
 * xm_codegen_x64 live in separate translation units that the build
 * conditionally compiles; only the host arch's symbol is linked, so
 * the test must dispatch through this alias instead of hardcoding one
 * backend (otherwise the Linux x86_64 link fails on undefined
 * xm_codegen_arm64).
 */
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
#define xm_codegen_native(func, alloc) xm_codegen_arm64((func), (alloc))
#elif defined(__x86_64__) || defined(__amd64__) || defined(_M_X64)
#define xm_codegen_native(func, alloc) xm_codegen_x64((func), (alloc))
#else
#error "test_jit_e2e: unsupported architecture"
#endif

// Unified JIT calling convention: (coro, args_ptr) -> raw result
typedef int64_t (*JitFn)(intptr_t, int64_t *);

/* ====================================================================
 * Fake JIT runtime env shared by all tests.
 *
 * The normal JIT prologue starts with
 *     LDR JIT_CTX_REG, [CORO_REG, #jit_ctx_offset]
 *     LDR ..., [JIT_CTX_REG, #active_stack_map_offset]
 *     STR FP, [JIT_CTX_REG, #jit_frame_sp_offset]
 *     LDR SAFEPT_PAGE_REG, [JIT_CTX_REG, #safepoint_page_offset]
 * so passing coro=0 makes every test segfault before its first
 * instruction. Stubs (CALL_C / CALL_SELF / deopt / OSR) also write into
 * jit_ctx aggressively. These globals provide the minimum coro +
 * jit_scratch + safepoint guard page needed for those paths to be
 * non-faulting in unit tests.
 *
 * We zero g_jit_ctx between tests so per-test transient state
 * (deopt_id, call_args, jit_frame_depth, ...) does not leak.
 * ==================================================================== */
static XrCoroutine g_jit_coro;
static XrJitScratch g_jit_ctx;
static void *g_safepoint_page = NULL;

static void jit_env_init(void) {
    if (g_safepoint_page == NULL) {
#ifdef _WIN32
        g_safepoint_page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        assert(g_safepoint_page != NULL && "VirtualAlloc safepoint_page");
#else
        g_safepoint_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        assert(g_safepoint_page != MAP_FAILED && "mmap safepoint_page");
#endif
    }
    memset(&g_jit_coro, 0, sizeof(g_jit_coro));
    memset(&g_jit_ctx, 0, sizeof(g_jit_ctx));
    g_jit_coro.jit_ctx = &g_jit_ctx;
    g_jit_ctx.safepoint_page = g_safepoint_page;
}

static void jit_env_reset(void) {
    // Preserve safepoint_page mapping; clear all transient fields.
    void *sp = g_safepoint_page;
    memset(&g_jit_ctx, 0, sizeof(g_jit_ctx));
    g_jit_ctx.safepoint_page = sp;
    // Note: we intentionally leave g_jit_coro alone — only jit_ctx pointer
    // matters and it is already wired up.
}

static inline intptr_t jit_test_coro(void) {
    return (intptr_t) &g_jit_coro;
}

// Helper: call JIT with 0 args
static inline int64_t jit_call0(void *code) {
    jit_env_reset();
    return ((JitFn) code)(jit_test_coro(), NULL);
}
// Helper: call JIT with 1 arg
static inline int64_t jit_call1(void *code, int64_t a0) {
    jit_env_reset();
    int64_t args[] = {a0};
    return ((JitFn) code)(jit_test_coro(), args);
}
// Helper: call JIT with 2 args
static inline int64_t jit_call2(void *code, int64_t a0, int64_t a1) {
    jit_env_reset();
    int64_t args[] = {a0, a1};
    return ((JitFn) code)(jit_test_coro(), args);
}
// Helper: call JIT with 3 args
static inline int64_t jit_call3(void *code, int64_t a0, int64_t a1, int64_t a2) {
    jit_env_reset();
    int64_t args[] = {a0, a1, a2};
    return ((JitFn) code)(jit_test_coro(), args);
}
// Helper: call JIT with 5 args (used by OSR-pressure regression)
static inline int64_t jit_call5(void *code, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                                int64_t a4) {
    jit_env_reset();
    int64_t args[] = {a0, a1, a2, a3, a4};
    return ((JitFn) code)(jit_test_coro(), args);
}
// Helper: call JIT with N args from a caller-owned array (used by 12-param
// pressure regressions). Caller must keep `args` alive for the call.
static inline int64_t jit_calln(void *code, const int64_t *args) {
    jit_env_reset();
    return ((JitFn) code)(jit_test_coro(), (int64_t *) args);
}
// Helper: call JIT with 1 f64 arg (bits as int64)
static inline int64_t jit_call1_f64(void *code, double a0) {
    jit_env_reset();
    int64_t args[1];
    memcpy(&args[0], &a0, 8);
    return ((JitFn) code)(jit_test_coro(), args);
}
// Helper: call JIT with 2 f64 args
static inline int64_t jit_call2_f64(void *code, double a0, double a1) {
    jit_env_reset();
    int64_t args[2];
    memcpy(&args[0], &a0, 8);
    memcpy(&args[1], &a1, 8);
    return ((JitFn) code)(jit_test_coro(), args);
}

/*
 * Helper: ensure pipeline runs before codegen.
 * Tests that already call xm_run_pipeline should set skip_auto_pipeline=true.
 *
 * Many tests build CFG with xm_block_set_br / xm_block_set_jmp but rely on
 * the codegen to derive preds from s1/s2. The optimisation pipeline now runs
 * xm_verify_cfg between pass groups, which DCHECKs that every successor lists
 * the predecessor block. We therefore rebuild preds from s1/s2 here before
 * running the pipeline; tests that already wired preds explicitly (loop / OSR
 * tests with phis) are unaffected because xm_rebuild_preds remaps phi args
 * to match the new pred ordering.
 */
static bool skip_auto_pipeline = false;
static XmCodegenResult safe_codegen(XmFunc *f, XmCodeAlloc *a) {
    xm_rebuild_preds(f);
    if (!skip_auto_pipeline)
        xm_run_pipeline(f, XM_OPT_BASIC);
    skip_auto_pipeline = false;
    return xm_codegen_native(f, a);
}

static void crash_handler(int sig) {
    const char *msg = "\n!!! SIGNAL received: ";
    write(2, msg, strlen(msg));
    if (sig == SIGSEGV)
        write(2, "SIGSEGV\n", 8);
#ifdef SIGBUS
    else if (sig == SIGBUS)
        write(2, "SIGBUS\n", 7);
#endif
    else
        write(2, "OTHER\n", 6);
    _exit(128 + sig);
}

/*
 * Test 1:() -> int { return 42; }
 * Xm:
 *   @entry:
 *     v0 =i64 const.i64 #42
 *     ret v0
 */
static void test_return_constant(void) {
    fprintf(stderr, "  test_return_constant...");

    XmFunc *func = xm_func_new("ret42");
    func->num_params = 0;

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmRef c42 = xm_const_i64(func, 42);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c42);
    xm_block_set_ret(entry, v0);

    fprintf(stderr, " Xm:");
    xm_print_func(stderr, func);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);

    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    assert(res.code != NULL);
    fprintf(stderr, "    code_size=%u bytes\n", res.code_size);

    int64_t result = jit_call0(res.code);
    fprintf(stderr, "    result=%lld\n", (long long) result);
    assert(result == 42);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "  PASS\n");
}

/*
 * Test 2:(a: int, b: int) -> int { return a + b; }
 * Xm:
 *   @entry:
 *     v2 =i64 add v0, v1
 *     ret v2
 */
static void test_add_two_params(void) {
    fprintf(stderr, "  test_add_two_params...");

    XmFunc *func = xm_func_new("add");
    func->num_params = 2;

    // Create param vregs
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);  // v0 = param a
    XmRef p1 = xm_new_vreg(func, XR_REP_I64);  // v1 = param b

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmRef v2 = xm_emit(func, entry, XM_ADD, XR_REP_I64, p0, p1);
    xm_block_set_ret(entry, v2);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);

    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);

    assert(jit_call2(res.code, 10, 20) == 30);
    assert(jit_call2(res.code, 0, 0) == 0);
    assert(jit_call2(res.code, -5, 5) == 0);
    assert(jit_call2(res.code, 100, -42) == 58);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test 3:(a: int, b: int) -> int { return a - b; }
 */
static void test_sub(void) {
    fprintf(stderr, "  test_sub...");

    XmFunc *func = xm_func_new("sub");
    func->num_params = 2;
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);
    XmRef p1 = xm_new_vreg(func, XR_REP_I64);

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmRef v2 = xm_emit(func, entry, XM_SUB, XR_REP_I64, p0, p1);
    xm_block_set_ret(entry, v2);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);

    assert(jit_call2(res.code, 30, 10) == 20);
    assert(jit_call2(res.code, 10, 30) == -20);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test 4:(a: int, b: int) -> int { return a * b; }
 */
static void test_mul(void) {
    fprintf(stderr, "  test_mul...");

    XmFunc *func = xm_func_new("mul");
    func->num_params = 2;
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);
    XmRef p1 = xm_new_vreg(func, XR_REP_I64);

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmRef v2 = xm_emit(func, entry, XM_MUL, XR_REP_I64, p0, p1);
    xm_block_set_ret(entry, v2);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);

    assert(jit_call2(res.code, 6, 7) == 42);
    assert(jit_call2(res.code, 0, 999) == 0);
    assert(jit_call2(res.code, -3, 4) == -12);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test 5:(a: int, b: int) -> int { return a + b + 10; }
 * Tests multi-instruction sequence with constant
 */
static void test_add_with_const(void) {
    fprintf(stderr, "  test_add_with_const...");

    XmFunc *func = xm_func_new("add_const");
    func->num_params = 2;
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);
    XmRef p1 = xm_new_vreg(func, XR_REP_I64);

    XmBlock *entry = xm_func_add_block(func, "entry");

    // v2 = a + b
    XmRef v2 = xm_emit(func, entry, XM_ADD, XR_REP_I64, p0, p1);
    // v3 = const 10
    XmRef c10 = xm_const_i64(func, 10);
    XmRef v3 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c10);
    // v4 = v2 + v3
    XmRef v4 = xm_emit(func, entry, XM_ADD, XR_REP_I64, v2, v3);
    xm_block_set_ret(entry, v4);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);

    assert(jit_call2(res.code, 5, 3) == 18);
    assert(jit_call2(res.code, 0, 0) == 10);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test 6:(a: int, b: int) -> int { return (a < b) ? 1 : 0; }
 * Tests comparison
 */
static void test_compare(void) {
    fprintf(stderr, "  test_compare...");

    XmFunc *func = xm_func_new("lt");
    func->num_params = 2;
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);
    XmRef p1 = xm_new_vreg(func, XR_REP_I64);

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmRef v2 = xm_emit(func, entry, XM_LT, XR_REP_I64, p0, p1);
    xm_block_set_ret(entry, v2);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);

    assert(jit_call2(res.code, 1, 2) == 1);
    assert(jit_call2(res.code, 2, 1) == 0);
    assert(jit_call2(res.code, 5, 5) == 0);
    assert(jit_call2(res.code, -1, 0) == 1);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test 7:(a: int) -> int { return -a; }
 */
static void test_negate(void) {
    fprintf(stderr, "  test_negate...");

    XmFunc *func = xm_func_new("neg");
    func->num_params = 1;
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmRef v1 = xm_emit_unary(func, entry, XM_NEG, XR_REP_I64, p0);
    xm_block_set_ret(entry, v1);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);

    assert(jit_call1(res.code, 42) == -42);
    assert(jit_call1(res.code, -10) == 10);
    assert(jit_call1(res.code, 0) == 0);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test 8:(a: int, b: int) -> int { return a & b; }
 * Bitwise operations
 */
static void test_bitwise(void) {
    fprintf(stderr, "  test_bitwise...");

    XmFunc *func = xm_func_new("bitand");
    func->num_params = 2;
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);
    XmRef p1 = xm_new_vreg(func, XR_REP_I64);

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmRef v2 = xm_emit(func, entry, XM_AND, XR_REP_I64, p0, p1);
    xm_block_set_ret(entry, v2);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);

    assert(jit_call2(res.code, 0xFF, 0x0F) == 0x0F);
    assert(jit_call2(res.code, 0xFF, 0x00) == 0x00);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test 9:(a: int, b: int) -> int { if (a > b) return a; else return b; }
 * Tests multi-block if/else with branch patching
 *
 * Xm:
 *   @entry: v2 = gt v0, v1; br v2 @then @else
 *   @then:  ret v0
 *   @else:  ret v1
 */
static void test_if_else_max(void) {
    fprintf(stderr, "  test_if_else_max...");

    XmFunc *func = xm_func_new("max");
    func->num_params = 2;
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);  // v0 = a
    XmRef p1 = xm_new_vreg(func, XR_REP_I64);  // v1 = b

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmBlock *then_blk = xm_func_add_block(func, "then");
    XmBlock *else_blk = xm_func_add_block(func, "else");

    XmRef v2 = xm_emit(func, entry, XM_GT, XR_REP_I64, p0, p1);
    xm_block_set_br(entry, v2, then_blk, else_blk);

    xm_block_set_ret(then_blk, p0);
    xm_block_set_ret(else_blk, p1);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);

    assert(jit_call2(res.code, 10, 5) == 10);
    assert(jit_call2(res.code, 5, 10) == 10);
    assert(jit_call2(res.code, 7, 7) == 7);
    assert(jit_call2(res.code, -3, -5) == -3);
    assert(jit_call2(res.code, -1, 1) == 1);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test 10:(a: int) -> int { if (a < 0) return -a; else return a; }
 * Tests if/else where then branch does computation
 */
static void test_if_else_abs(void) {
    fprintf(stderr, "  test_if_else_abs...");

    XmFunc *func = xm_func_new("abs");
    func->num_params = 1;
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmBlock *neg_blk = xm_func_add_block(func, "neg");
    XmBlock *pos_blk = xm_func_add_block(func, "pos");

    XmRef czero = xm_const_i64(func, 0);
    XmRef v_zero = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, czero);
    XmRef v_cmp = xm_emit(func, entry, XM_LT, XR_REP_I64, p0, v_zero);
    xm_block_set_br(entry, v_cmp, neg_blk, pos_blk);

    // neg: return -a
    XmRef v_neg = xm_emit_unary(func, neg_blk, XM_NEG, XR_REP_I64, p0);
    xm_block_set_ret(neg_blk, v_neg);

    // pos: return a
    xm_block_set_ret(pos_blk, p0);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);

    assert(jit_call1(res.code, 42) == 42);
    assert(jit_call1(res.code, -42) == 42);
    assert(jit_call1(res.code, 0) == 0);
    assert(jit_call1(res.code, -1) == 1);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test 11:(a: int, b: int) -> int { return a % b; }
 * Tests MOD (SDIV + MSUB)
 */
static void test_mod(void) {
    fprintf(stderr, "  test_mod...");

    XmFunc *func = xm_func_new("mod");
    func->num_params = 2;
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);
    XmRef p1 = xm_new_vreg(func, XR_REP_I64);

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmRef v2 = xm_emit(func, entry, XM_MOD, XR_REP_I64, p0, p1);
    xm_block_set_ret(entry, v2);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);

    assert(jit_call2(res.code, 10, 3) == 1);
    assert(jit_call2(res.code, 9, 3) == 0);
    assert(jit_call2(res.code, 7, 2) == 1);
    assert(jit_call2(res.code, 100, 7) == 2);
    assert(jit_call2(res.code, -7, 3) == -1);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test 12:(n: int) -> int { sum=0; i=1; while(i<=n) { sum+=i; i+=1; } return sum; }
 * Tests loop with Phi nodes and backward branch
 *
 * Xm:
 *   @entry:
 *     v1 =i64 const.i64 #0     // sum = 0
 *     v2 =i64 const.i64 #1     // i = 1
 *     jmp @loop
 *   @loop:                      // preds: entry, body
 *     v3 = phi [v1, @entry], [v5, @body]   // sum
 *     v4 = phi [v2, @entry], [v6, @body]   // i
 *     v7 =i64 le v4, v0        // i <= n
 *     br v7 @body @exit
 *   @body:
 *     v5 =i64 add v3, v4       // sum + i
 *     v8 =i64 const.i64 #1
 *     v6 =i64 add v4, v8       // i + 1
 *     jmp @loop
 *   @exit:
 *     ret v3
 */
static void test_loop_sum(void) {
    fprintf(stderr, "  test_loop_sum...");

    XmFunc *func = xm_func_new("sum");
    func->num_params = 1;
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);  // v0 = n

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmBlock *loop = xm_func_add_block(func, "loop");
    XmBlock *body = xm_func_add_block(func, "body");
    XmBlock *exit_blk = xm_func_add_block(func, "exit");

    // @entry: sum=0, i=1, jmp @loop
    XmRef c0 = xm_const_i64(func, 0);
    XmRef v_sum_init = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c0);
    XmRef c1 = xm_const_i64(func, 1);
    XmRef v_i_init = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c1);
    xm_block_set_jmp(entry, loop);

    // Set up predecessors for @loop before adding Phis
    xm_block_add_pred(loop, entry, func->arena);
    xm_block_add_pred(loop, body, func->arena);

    // @loop: Phi nodes
    XmPhi *phi_sum = xm_add_phi(func, loop, XR_REP_I64);
    XmPhi *phi_i = xm_add_phi(func, loop, XR_REP_I64);

    // Pre-allocate registers for Phi dsts so they have known mappings
    XmRef sum_ref = phi_sum->dst;
    XmRef i_ref = phi_i->dst;

    // le v4, v0
    XmRef v_cond = xm_emit(func, loop, XM_LE, XR_REP_I64, i_ref, p0);
    xm_block_set_br(loop, v_cond, body, exit_blk);

    // @body: sum += i, i += 1, jmp @loop
    XmRef v_new_sum = xm_emit(func, body, XM_ADD, XR_REP_I64, sum_ref, i_ref);
    XmRef c1_body = xm_const_i64(func, 1);
    XmRef v_one = xm_emit_unary(func, body, XM_CONST_I64, XR_REP_I64, c1_body);
    XmRef v_new_i = xm_emit(func, body, XM_ADD, XR_REP_I64, i_ref, v_one);
    xm_block_set_jmp(body, loop);

    // @exit: ret sum
    xm_block_set_ret(exit_blk, sum_ref);

    // Set Phi args: [entry_val, body_val]
    xm_phi_set_arg(phi_sum, 0, v_sum_init);  // from entry
    xm_phi_set_arg(phi_sum, 1, v_new_sum);   // from body
    xm_phi_set_arg(phi_i, 0, v_i_init);      // from entry
    xm_phi_set_arg(phi_i, 1, v_new_i);       // from body

    fprintf(stderr, " Xm:\n");
    xm_print_func(stderr, func);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    fprintf(stderr, "    code_size=%u bytes\n", res.code_size);

    // sum(0) = 0, sum(1) = 1, sum(5) = 15, sum(10) = 55, sum(100) = 5050
    int64_t r;
    r = jit_call1(res.code, 0);
    fprintf(stderr, "    sum(0)=%lld\n", (long long) r);
    assert(r == 0);
    r = jit_call1(res.code, 1);
    fprintf(stderr, "    sum(1)=%lld\n", (long long) r);
    assert(r == 1);
    r = jit_call1(res.code, 5);
    fprintf(stderr, "    sum(5)=%lld\n", (long long) r);
    assert(r == 15);
    r = jit_call1(res.code, 10);
    fprintf(stderr, "    sum(10)=%lld\n", (long long) r);
    assert(r == 55);
    r = jit_call1(res.code, 100);
    fprintf(stderr, "    sum(100)=%lld\n", (long long) r);
    assert(r == 5050);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "  PASS\n");
}

/*
 * Test 13:(a: int, b: int) -> int { if (a > b) return a - b; else return b - a; }
 * Tests non-trivial if/else with different computations in each branch
 */
static void test_if_else_diff(void) {
    fprintf(stderr, "  test_if_else_diff...");

    XmFunc *func = xm_func_new("diff");
    func->num_params = 2;
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);
    XmRef p1 = xm_new_vreg(func, XR_REP_I64);

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmBlock *then_blk = xm_func_add_block(func, "then");
    XmBlock *else_blk = xm_func_add_block(func, "else");

    XmRef v_cmp = xm_emit(func, entry, XM_GT, XR_REP_I64, p0, p1);
    xm_block_set_br(entry, v_cmp, then_blk, else_blk);

    XmRef v_then = xm_emit(func, then_blk, XM_SUB, XR_REP_I64, p0, p1);
    xm_block_set_ret(then_blk, v_then);

    XmRef v_else = xm_emit(func, else_blk, XM_SUB, XR_REP_I64, p1, p0);
    xm_block_set_ret(else_blk, v_else);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);

    assert(jit_call2(res.code, 10, 3) == 7);
    assert(jit_call2(res.code, 3, 10) == 7);
    assert(jit_call2(res.code, 5, 5) == 0);
    assert(jit_call2(res.code, -2, 3) == 5);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test 14: Iterative fibonacci
 * fn fib(n: int) -> int {
 *   if (n <= 1) return n;
 *   a=0; b=1; i=2; while(i<=n) { t=a+b; a=b; b=t; i++; } return b;
 * }
 *
 * Xm:
 *   @entry:
 *     v1 =i64 const.i64 #1
 *     v2 =i64 le v0, v1          // n <= 1?
 *     br v2 @base @loop_init
 *   @base:
 *     ret v0                      // return n
 *   @loop_init:
 *     v3 =i64 const.i64 #0       // a = 0
 *     v4 =i64 const.i64 #1       // b = 1
 *     v5 =i64 const.i64 #2       // i = 2
 *     jmp @loop
 *   @loop:  ; preds: loop_init, body
 *     va = phi [v3, @loop_init], [vb_phi, @body]
 *     vb = phi [v4, @loop_init], [vt, @body]
 *     vi = phi [v5, @loop_init], [vi_next, @body]
 *     v6 =i64 le vi, v0
 *     br v6 @body @done
 *   @body:
 *     vt = add va, vb
 *     vi_next = add vi, #1
 *     jmp @loop
 *   @done:
 *     ret vb
 */
static void test_fibonacci(void) {
    fprintf(stderr, "  test_fibonacci...");

    XmFunc *func = xm_func_new("fib");
    func->num_params = 1;
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);  // v0 = n

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmBlock *base = xm_func_add_block(func, "base");
    XmBlock *loop_init = xm_func_add_block(func, "loop_init");
    XmBlock *loop = xm_func_add_block(func, "loop");
    XmBlock *body = xm_func_add_block(func, "body");
    XmBlock *done = xm_func_add_block(func, "done");

    // @entry: if (n <= 1) goto base else goto loop_init
    XmRef c1_entry = xm_const_i64(func, 1);
    XmRef v1 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c1_entry);
    XmRef v_cmp = xm_emit(func, entry, XM_LE, XR_REP_I64, p0, v1);
    xm_block_set_br(entry, v_cmp, base, loop_init);

    // @base: return n
    xm_block_set_ret(base, p0);

    // @loop_init: a=0, b=1, i=2
    XmRef c0 = xm_const_i64(func, 0);
    XmRef v_a_init = xm_emit_unary(func, loop_init, XM_CONST_I64, XR_REP_I64, c0);
    XmRef c1 = xm_const_i64(func, 1);
    XmRef v_b_init = xm_emit_unary(func, loop_init, XM_CONST_I64, XR_REP_I64, c1);
    XmRef c2 = xm_const_i64(func, 2);
    XmRef v_i_init = xm_emit_unary(func, loop_init, XM_CONST_I64, XR_REP_I64, c2);
    xm_block_set_jmp(loop_init, loop);

    // Set up preds for @loop
    xm_block_add_pred(loop, loop_init, func->arena);
    xm_block_add_pred(loop, body, func->arena);

    // @loop: Phi nodes
    XmPhi *phi_a = xm_add_phi(func, loop, XR_REP_I64);
    XmPhi *phi_b = xm_add_phi(func, loop, XR_REP_I64);
    XmPhi *phi_i = xm_add_phi(func, loop, XR_REP_I64);
    XmRef a_ref = phi_a->dst;
    XmRef b_ref = phi_b->dst;
    XmRef i_ref = phi_i->dst;

    XmRef v_loop_cmp = xm_emit(func, loop, XM_LE, XR_REP_I64, i_ref, p0);
    xm_block_set_br(loop, v_loop_cmp, body, done);

    // @body: t = a+b, a'=b, b'=t, i'=i+1
    XmRef v_t = xm_emit(func, body, XM_ADD, XR_REP_I64, a_ref, b_ref);
    XmRef c1_body = xm_const_i64(func, 1);
    XmRef v_one = xm_emit_unary(func, body, XM_CONST_I64, XR_REP_I64, c1_body);
    XmRef v_i_next = xm_emit(func, body, XM_ADD, XR_REP_I64, i_ref, v_one);
    xm_block_set_jmp(body, loop);

    // @done: return b
    xm_block_set_ret(done, b_ref);

    // Set Phi args
    xm_phi_set_arg(phi_a, 0, v_a_init);  // from loop_init
    xm_phi_set_arg(phi_a, 1, b_ref);     // from body: a' = b
    xm_phi_set_arg(phi_b, 0, v_b_init);  // from loop_init
    xm_phi_set_arg(phi_b, 1, v_t);       // from body: b' = t = a+b
    xm_phi_set_arg(phi_i, 0, v_i_init);  // from loop_init
    xm_phi_set_arg(phi_i, 1, v_i_next);  // from body

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    fprintf(stderr, " code_size=%u\n", res.code_size);

    // fib(0)=0, fib(1)=1, fib(2)=1, fib(5)=5, fib(10)=55, fib(20)=6765, fib(30)=832040
    int64_t r;
    r = jit_call1(res.code, 0);
    fprintf(stderr, "    fib(0)=%lld\n", (long long) r);
    assert(r == 0);
    r = jit_call1(res.code, 1);
    fprintf(stderr, "    fib(1)=%lld\n", (long long) r);
    assert(r == 1);
    r = jit_call1(res.code, 2);
    fprintf(stderr, "    fib(2)=%lld\n", (long long) r);
    assert(r == 1);
    r = jit_call1(res.code, 5);
    fprintf(stderr, "    fib(5)=%lld\n", (long long) r);
    assert(r == 5);
    r = jit_call1(res.code, 10);
    fprintf(stderr, "    fib(10)=%lld\n", (long long) r);
    assert(r == 55);
    r = jit_call1(res.code, 20);
    fprintf(stderr, "    fib(20)=%lld\n", (long long) r);
    assert(r == 6765);
    r = jit_call1(res.code, 30);
    fprintf(stderr, "    fib(30)=%lld\n", (long long) r);
    assert(r == 832040);
    r = jit_call1(res.code, 40);
    fprintf(stderr, "    fib(40)=%lld\n", (long long) r);
    assert(r == 102334155);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "  PASS\n");
}

/*
 * Test 15: Loop sum with safepoint at back-edge
 * Same as test_loop_sum but with XM_SAFEPOINT before jmp @loop
 * Verifies safepoint codegen (reductions decrement) doesn't break loops
 *
 * Xm:
 *   @entry: v1=0, v2=1, jmp @loop
 *   @loop: phi sum/i, le i<=n, br → @body/@exit
 *   @body: sum+=i, i+=1, safepoint, jmp @loop
 *   @exit: ret sum
 */
static void test_loop_sum_safepoint(void) {
    fprintf(stderr, "  test_loop_sum_safepoint...");

    XmFunc *func = xm_func_new("sum_sp");
    func->num_params = 1;
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);  // v0 = n

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmBlock *loop = xm_func_add_block(func, "loop");
    XmBlock *body = xm_func_add_block(func, "body");
    XmBlock *exit_blk = xm_func_add_block(func, "exit");

    // @entry
    XmRef c0 = xm_const_i64(func, 0);
    XmRef v_sum_init = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c0);
    XmRef c1 = xm_const_i64(func, 1);
    XmRef v_i_init = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c1);
    xm_block_set_jmp(entry, loop);

    xm_block_add_pred(loop, entry, func->arena);
    xm_block_add_pred(loop, body, func->arena);

    // @loop: Phi + condition
    XmPhi *phi_sum = xm_add_phi(func, loop, XR_REP_I64);
    XmPhi *phi_i = xm_add_phi(func, loop, XR_REP_I64);
    XmRef sum_ref = phi_sum->dst;
    XmRef i_ref = phi_i->dst;
    XmRef v_cond = xm_emit(func, loop, XM_LE, XR_REP_I64, i_ref, p0);
    xm_block_set_br(loop, v_cond, body, exit_blk);

    // @body: sum+=i, i+=1, safepoint, jmp @loop
    XmRef v_new_sum = xm_emit(func, body, XM_ADD, XR_REP_I64, sum_ref, i_ref);
    XmRef c1_body = xm_const_i64(func, 1);
    XmRef v_one = xm_emit_unary(func, body, XM_CONST_I64, XR_REP_I64, c1_body);
    XmRef v_new_i = xm_emit(func, body, XM_ADD, XR_REP_I64, i_ref, v_one);
    // Safepoint at loop back-edge
    XmRef none = XM_REF_NONE;
    xm_emit(func, body, XM_SAFEPOINT, XR_REP_VOID, none, none);
    xm_block_set_jmp(body, loop);

    // @exit
    xm_block_set_ret(exit_blk, sum_ref);

    // Phi args
    xm_phi_set_arg(phi_sum, 0, v_sum_init);
    xm_phi_set_arg(phi_sum, 1, v_new_sum);
    xm_phi_set_arg(phi_i, 0, v_i_init);
    xm_phi_set_arg(phi_i, 1, v_new_i);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    fprintf(stderr, " code_size=%u\n", res.code_size);

    int64_t r;
    r = jit_call1(res.code, 0);
    assert(r == 0);
    r = jit_call1(res.code, 1);
    assert(r == 1);
    r = jit_call1(res.code, 5);
    assert(r == 15);
    r = jit_call1(res.code, 10);
    assert(r == 55);
    r = jit_call1(res.code, 100);
    assert(r == 5050);
    // Large N to trigger safepoint reset (> 4000 iterations)
    r = jit_call1(res.code, 10000);
    assert(r == 50005000);
    fprintf(stderr, "    sum_sp(10000)=%lld\n", (long long) r);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "  PASS\n");
}

/*
 * Test: InsertWriteBarriers pass + codegen for BARRIER_FWD
 *
 * Build Xm with a STORE_FIELD, run the pass, verify a BARRIER_FWD was
 * inserted, then codegen and execute to confirm the barrier stub is reachable.
 *
 * Xm (before pass):
 *   @entry:
 *     v1 =ptr store_field v0, v0   // dummy store (obj field write)
 *     v2 =i64 const.i64 #99
 *     ret v2
 *
 * After pass:
 *   @entry:
 *     v1 =ptr store_field v0, v0
 *     barrier_fwd v0, v0            // <-- inserted by pass
 *     v2 =i64 const.i64 #99
 *     ret v2
 */
static void test_write_barrier(void) {
    fprintf(stderr, "  test_write_barrier...");

    XmFunc *func = xm_func_new("wb_test");
    func->num_params = 1;
    XmRef p0 = xm_new_vreg(func, XR_REP_PTR);  // v0 = dummy ptr param

    XmBlock *entry = xm_func_add_block(func, "entry");

    // Emit a STORE_FIELD: ins->rep carries XrValue tag (not Xm machine type).
    // Must use XR_TAG_PTR (13) to trigger barrier insertion.
    xm_emit(func, entry, XM_STORE_FIELD, XR_TAG_PTR, p0, p0);

    // Load a constant and return it
    XmRef c99 = xm_const_i64(func, 99);
    XmRef v2 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c99);
    xm_block_set_ret(entry, v2);

    // Verify: before pass, nins == 2 (STORE_FIELD + CONST_I64)
    assert(entry->nins == 2);

    // Run InsertWriteBarriers pass
    xm_insert_write_barriers(func);

    // Verify: after pass, nins == 3 (STORE_FIELD + BARRIER_FWD + CONST_I64)
    assert(entry->nins == 3);
    assert(entry->ins[0].op == XM_STORE_FIELD);
    assert(entry->ins[1].op == XM_BARRIER_FWD);
    assert(entry->ins[2].op == XM_CONST_I64);
    fprintf(stderr, " pass OK");

    // Codegen and execute (coro=0 → barrier CBZ skips the stub call)
    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);

    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    fprintf(stderr, " codegen OK (size=%u)", res.code_size);

    // Allocate a fake object buffer (GCHeader + 1 field = 32 bytes)
    // STORE_FIELD now actually writes to memory at GC_HEADER_SIZE offset
    uint8_t fake_obj[48];
    memset(fake_obj, 0, sizeof(fake_obj));
    int64_t result = jit_call1(res.code, (int64_t) (intptr_t) fake_obj);
    assert(result == 99);
    fprintf(stderr, " exec OK (ret=%lld)", (long long) result);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/*
 * Test: DCE removes dead instructions
 *
 * Xm:
 *   @entry:
 *     v1 =i64 const.i64 #10     // dead (unused)
 *     v2 =i64 const.i64 #20     // dead (unused)
 *     v3 =i64 const.i64 #42     // used by ret
 *     ret v3
 *
 * After DCE: only v3 remains (v1, v2 eliminated)
 */
static void test_dce(void) {
    fprintf(stderr, "  test_dce...");

    XmFunc *func = xm_func_new("dce_test");
    func->num_params = 0;

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmRef c10 = xm_const_i64(func, 10);
    XmRef c20 = xm_const_i64(func, 20);
    XmRef c42 = xm_const_i64(func, 42);
    xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c10);  // v0 dead
    xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c20);  // v1 dead
    XmRef v2 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c42);
    xm_block_set_ret(entry, v2);

    assert(entry->nins == 3);
    xm_pass_dce(func);
    assert(entry->nins == 1);
    assert(entry->ins[0].op == XM_CONST_I64);
    fprintf(stderr, " nins: 3→%u", entry->nins);

    // Execute to verify correctness
    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    assert(jit_call0(res.code) == 42);
    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/*
 * Test: Constant Propagation + Folding
 *
 * Xm:
 *   @entry:
 *     v0 =i64 const.i64 #10
 *     v1 =i64 const.i64 #20
 *     v2 =i64 add v0, v1      → folded to const.i64 #30
 *     v3 =i64 const.i64 #5
 *     v4 =i64 mul v2, v3      → folded to const.i64 #150
 *     ret v4
 */
static void test_const_fold(void) {
    fprintf(stderr, "  test_const_fold...");

    XmFunc *func = xm_func_new("fold_test");
    func->num_params = 0;

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmRef c10 = xm_const_i64(func, 10);
    XmRef c20 = xm_const_i64(func, 20);
    XmRef c5 = xm_const_i64(func, 5);

    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c10);
    XmRef v1 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c20);
    XmRef v2 = xm_emit(func, entry, XM_ADD, XR_REP_I64, v0, v1);
    XmRef v3 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c5);
    XmRef v4 = xm_emit(func, entry, XM_MUL, XR_REP_I64, v2, v3);
    xm_block_set_ret(entry, v4);

    assert(entry->nins == 5);

    // Run constant propagation + folding (SCCP is the drop-in replacement
    // for the older xm_pass_const_prop / branch_simp / remove_unreachable trio)
    xm_pass_sccp(func);

    // v2 (ADD) should be folded to CONST_I64, v4 (MUL) also
    assert(entry->ins[2].op == XM_CONST_I64);  // ADD → CONST
    assert(entry->ins[4].op == XM_CONST_I64);  // MUL → CONST
    fprintf(stderr, " folded OK");

    // Run DCE to clean up dead intermediates
    xm_pass_dce(func);
    fprintf(stderr, " dce→%u ins", entry->nins);

    // Execute
    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    int64_t result = jit_call0(res.code);
    assert(result == 150);
    fprintf(stderr, " exec=%lld", (long long) result);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/*
 * Test: Full pipeline (xm_run_pipeline at -O1)
 * Same as const_fold but via the pipeline runner.
 */
static void test_pipeline(void) {
    fprintf(stderr, "  test_pipeline...");

    XmFunc *func = xm_func_new("pipe_test");
    func->num_params = 1;
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);  // v0 = param

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmRef c3 = xm_const_i64(func, 3);
    XmRef c7 = xm_const_i64(func, 7);

    XmRef v1 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c3);
    XmRef v2 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c7);
    XmRef v3 = xm_emit(func, entry, XM_ADD, XR_REP_I64, v1, v2);  // 3+7=10
    XmRef v4 = xm_emit(func, entry, XM_MUL, XR_REP_I64, p0, v3);  // param * 10
    xm_block_set_ret(entry, v4);

    uint32_t orig_nins = entry->nins;
    xm_rebuild_preds(func);
    xm_run_pipeline(func, XM_OPT_BASIC);
    fprintf(stderr, " nins: %u→%u", orig_nins, entry->nins);

    // Execute: param=5, expect 5*10=50
    skip_auto_pipeline = true;
    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    int64_t result = jit_call1(res.code, 5);
    assert(result == 50);
    fprintf(stderr, " exec=%lld", (long long) result);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/*
 * Test: CSE eliminates duplicate computations
 *
 * Xm:
 *   @entry:
 *     v1 =i64 add v0, v0       // param + param
 *     v2 =i64 add v0, v0       // same computation → CSE replaces with MOV from v1
 *     v3 =i64 mul v1, v2       // should use same value for both
 *     ret v3
 *
 * After CSE+DCE: v2 eliminated, v3 = mul v1, v1
 */
static void test_cse(void) {
    fprintf(stderr, "  test_cse...");

    XmFunc *func = xm_func_new("cse_test");
    func->num_params = 1;
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);  // v0 = param

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmRef v1 = xm_emit(func, entry, XM_ADD, XR_REP_I64, p0, p0);  // p + p
    XmRef v2 = xm_emit(func, entry, XM_ADD, XR_REP_I64, p0, p0);  // same
    XmRef v3 = xm_emit(func, entry, XM_MUL, XR_REP_I64, v1, v2);
    xm_block_set_ret(entry, v3);

    assert(entry->nins == 3);
    xm_rebuild_preds(func);
    xm_run_pipeline(func, XM_OPT_FULL);
    fprintf(stderr, " nins: 3→%u", entry->nins);

    // Execute: param=5, expect (5+5)*(5+5) = 100
    skip_auto_pipeline = true;
    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    int64_t result = jit_call1(res.code, 5);
    assert(result == 100);
    fprintf(stderr, " exec=%lld", (long long) result);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/*
 * Test: LICM hoists loop-invariant computation to preheader
 *
 * Computes: sum = 0; for i=1..n: sum += (3 * 7)
 * The multiply (3*7=21) is loop-invariant and should be hoisted.
 * Result: n * 21
 *
 * Xm (before LICM):
 *   @entry: jmp @loop
 *   @loop:  phi i, sum; if i<=n goto @body else @exit
 *   @body:  c3=const 3; c7=const 7; t=mul c3,c7; sum+=t; i++; jmp @loop
 *   @exit:  ret sum
 *
 * After LICM: c3, c7, and mul are hoisted to @entry.
 */
static void test_licm(void) {
    fprintf(stderr, "  test_licm...");

    XmFunc *func = xm_func_new("licm_test");
    func->num_params = 1;
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);  // v0 = n

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmBlock *loop = xm_func_add_block(func, "loop");
    XmBlock *body = xm_func_add_block(func, "body");
    XmBlock *exit_ = xm_func_add_block(func, "exit");

    // @entry: init sum=0, i=1, jmp @loop
    XmRef c0 = xm_const_i64(func, 0);
    XmRef c1 = xm_const_i64(func, 1);
    XmRef init_sum = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c0);
    XmRef init_i = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c1);
    xm_block_set_jmp(entry, loop);

    // @loop: add predecessors first, then create phi nodes
    xm_block_add_pred(loop, entry, func->arena);
    xm_block_add_pred(loop, body, func->arena);
    XmPhi *phi_i_node = xm_add_phi(func, loop, XR_REP_I64);
    XmPhi *phi_sum_node = xm_add_phi(func, loop, XR_REP_I64);
    XmRef phi_i = phi_i_node->dst;
    XmRef phi_sum = phi_sum_node->dst;
    XmRef cmp = xm_emit(func, loop, XM_LE, XR_REP_I64, phi_i, p0);
    xm_block_set_br(loop, cmp, body, exit_);

    // @body: invariant computation 3*7, then sum += result, i++
    XmRef c3_ref = xm_const_i64(func, 3);
    XmRef c7_ref = xm_const_i64(func, 7);
    XmRef vc3 = xm_emit_unary(func, body, XM_CONST_I64, XR_REP_I64, c3_ref);
    XmRef vc7 = xm_emit_unary(func, body, XM_CONST_I64, XR_REP_I64, c7_ref);
    XmRef product = xm_emit(func, body, XM_MUL, XR_REP_I64, vc3, vc7);
    XmRef new_sum = xm_emit(func, body, XM_ADD, XR_REP_I64, phi_sum, product);
    XmRef inc_ref = xm_const_i64(func, 1);
    XmRef vc1 = xm_emit_unary(func, body, XM_CONST_I64, XR_REP_I64, inc_ref);
    XmRef new_i = xm_emit(func, body, XM_ADD, XR_REP_I64, phi_i, vc1);
    xm_block_set_jmp(body, loop);

    // Wire phi args: pred 0 = entry, pred 1 = body
    xm_phi_set_arg(phi_i_node, 0, init_i);
    xm_phi_set_arg(phi_i_node, 1, new_i);
    xm_phi_set_arg(phi_sum_node, 0, init_sum);
    xm_phi_set_arg(phi_sum_node, 1, new_sum);

    // @exit: ret sum
    xm_block_set_ret(exit_, phi_sum);

    uint32_t body_nins_before = body->nins;

    // Run full pipeline (includes LICM at -O2)
    xm_rebuild_preds(func);
    xm_run_pipeline(func, XM_OPT_FULL);
    fprintf(stderr, " body: %u→%u", body_nins_before, body->nins);

    // Execute: n=10, expect 10*21=210
    skip_auto_pipeline = true;
    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    int64_t result = jit_call1(res.code, 10);
    fprintf(stderr, " exec=%lld", (long long) result);
    assert(result == 210);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/*
 * Test: floating-point arithmetic (FMUL + FADD)
 * f(x: f64) = x * 2.0 + 1.0
 * f(3.0) = 7.0
 */
static void test_fp_basic(void) {
    fprintf(stderr, "  test_fp_basic...");

    XmFunc *func = xm_func_new("fp_basic");
    func->num_params = 1;
    XmRef p0 = xm_new_vreg(func, XR_REP_F64);  // v0 = x (in d0)

    XmBlock *entry = xm_func_add_block(func, "entry");

    XmRef c2 = xm_const_f64(func, 2.0);
    XmRef c1 = xm_const_f64(func, 1.0);

    XmRef two = xm_emit_unary(func, entry, XM_CONST_F64, XR_REP_F64, c2);
    XmRef mul = xm_emit(func, entry, XM_FMUL, XR_REP_F64, p0, two);

    XmRef one = xm_emit_unary(func, entry, XM_CONST_F64, XR_REP_F64, c1);
    XmRef add = xm_emit(func, entry, XM_FADD, XR_REP_F64, mul, one);

    xm_block_set_ret(entry, add);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    fprintf(stderr, " code=%u", res.code_size);

    // Call: f(3.0) = 3.0 * 2.0 + 1.0 = 7.0
    // JIT returns bits in x0 (via FMOV Xd, Dn)
    int64_t raw = jit_call1_f64(res.code, 3.0);
    double val;
    memcpy(&val, &raw, 8);
    fprintf(stderr, " result=%.1f", val);
    assert(val == 7.0);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/*
 * Test: mixed-type arithmetic via XM_RT_ADD
 * f(x: i64, y: f64) = x + y  (integer + float → float)
 * f(3, 2.5) = 5.5
 */
static void test_rt_mixed_add(void) {
    fprintf(stderr, "  test_rt_mixed_add...");

    XmFunc *func = xm_func_new("rt_mixed_add");
    func->num_params = 2;
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);  // x: i64
    XmRef p1 = xm_new_vreg(func, XR_REP_F64);  // y: f64

    XmBlock *entry = xm_func_add_block(func, "entry");

    // result = RT_ADD(p0, p1)  — mixed i64 + f64 → f64
    XmRef result = xm_emit(func, entry, XM_RT_ADD, XR_REP_F64, p0, p1);
    entry->ins[entry->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;

    // Return result as raw bits (FMOV to GPR handled by codegen)
    xm_block_set_ret(entry, result);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    fprintf(stderr, " code=%u", res.code_size);

    // Call: f(coro=0, x=3, y=2.5)
    // x is in x1 (GP), y is in d0 (FP) per our calling convention
    // Actually our JIT calling convention passes params in alloc_regs:
    // param0 → x1 (GP), param1 → d0 (FP)
    // But the test harness uses (intptr_t coro, int64_t p0, ...)
    // For FP param, we need to pass via the FP reg. Let's use a different approach:
    // Encode the double as raw i64 bits in a GP register, then the regalloc
    // will put it in an FP register.

    // Actually, the param assignment: p0 is i64 → GP (x1), p1 is f64 → FP (d0)
    // We need to call with the right registers. Use inline asm or a trampoline.
    // For simplicity, let's construct the Xm differently: load constants directly.

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);

    // Re-create with constants to avoid calling convention issues
    func = xm_func_new("rt_mixed_add2");
    func->num_params = 0;
    entry = xm_func_add_block(func, "entry");

    // v0 = 3 (i64)
    XmRef ci = xm_const_i64(func, 3);
    XmRef vi = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, ci);

    // v1 = 2.5 (f64)
    XmRef cf = xm_const_f64(func, 2.5);
    XmRef vf = xm_emit_unary(func, entry, XM_CONST_F64, XR_REP_F64, cf);

    // v2 = RT_ADD(vi, vf) → f64 (3 + 2.5 = 5.5)
    result = xm_emit(func, entry, XM_RT_ADD, XR_REP_F64, vi, vf);
    entry->ins[entry->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;

    xm_block_set_ret(entry, result);

    xm_code_alloc_init(&alloc);
    res = safe_codegen(func, &alloc);
    assert(res.success);
    fprintf(stderr, " code2=%u", res.code_size);

    int64_t raw = jit_call0(res.code);

    double val;
    memcpy(&val, &raw, 8);
    fprintf(stderr, " result=%.1f", val);
    assert(val == 5.5);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/*
 * Test: SelectRepresentations eliminates redundant BOX/UNBOX
 * f(x) = UNBOX_I64(BOX_I64(x + 1))
 * After select_rep: UNBOX(BOX(x+1)) → x+1
 * f(10) = 11
 */
static void test_select_rep(void) {
    fprintf(stderr, "  test_select_rep...");

    XmFunc *func = xm_func_new("select_rep");
    func->num_params = 1;
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);

    XmBlock *entry = xm_func_add_block(func, "entry");

    // v1 = x + 1
    XmRef c1 = xm_const_i64(func, 1);
    XmRef one = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c1);
    XmRef add = xm_emit(func, entry, XM_ADD, XR_REP_I64, p0, one);

    // v2 = BOX_I64(v1)
    XmRef boxed = xm_emit_unary(func, entry, XM_BOX_I64, XR_REP_TAGGED, add);

    // v3 = UNBOX_I64(v2) — should be eliminated by select_rep
    XmRef unboxed = xm_emit_unary(func, entry, XM_UNBOX_I64, XR_REP_I64, boxed);

    xm_block_set_ret(entry, unboxed);

    // Run pass: should convert UNBOX(BOX(x+1)) → MOV(x+1)
    xm_pass_select_rep(func);

    // Count remaining BOX/UNBOX instructions
    uint32_t box_count = 0;
    XmBlock *blk = func->blocks[0];
    for (uint32_t i = 0; i < blk->nins; i++) {
        if (blk->ins[i].op == XM_BOX_I64 || blk->ins[i].op == XM_UNBOX_I64)
            box_count++;
    }
    fprintf(stderr, " box_count=%u", box_count);
    assert(box_count <= 1);  // UNBOX should be eliminated, BOX may remain

    // Compile and execute
    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);

    int64_t result = jit_call1(res.code, 10);
    fprintf(stderr, " f(10)=%lld", (long long) result);
    assert(result == 11);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/*
 * Test: function inlining
 * Caller: f(x) = x + CALL(g, x)
 * Callee: g(y) = y * 2
 * After inlining: f(x) = x + (x * 2) = 3x
 * f(10) = 30
 */
static void test_inline_basic(void) {
    fprintf(stderr, "  test_inline_basic...");

    // Build callee: g(y) = y * 2
    XmFunc *callee = xm_func_new("g");
    callee->num_params = 1;
    // return type now derived from callee->proto->return_type_info (no field on XmFunc)
    XmRef g_p0 = xm_new_vreg(callee, XR_REP_I64);  // param y

    XmBlock *g_entry = xm_func_add_block(callee, "g.entry");
    XmRef c2 = xm_const_i64(callee, 2);
    XmRef two = xm_emit_unary(callee, g_entry, XM_CONST_I64, XR_REP_I64, c2);
    XmRef g_result = xm_emit(callee, g_entry, XM_MUL, XR_REP_I64, g_p0, two);
    xm_block_set_ret(g_entry, g_result);

    // Build caller: f(x) = x + <placeholder for g(x)>
    XmFunc *caller = xm_func_new("f");
    caller->num_params = 1;
    XmRef f_p0 = xm_new_vreg(caller, XR_REP_I64);  // param x

    XmBlock *f_entry = xm_func_add_block(caller, "f.entry");

    // Emit a NOP as placeholder for the call (will be replaced by inlining)
    XmRef call_result = xm_emit_unary(caller, f_entry, XM_NOP, XR_REP_I64, f_p0);

    // f_result = x + call_result
    XmRef f_result = xm_emit(caller, f_entry, XM_ADD, XR_REP_I64, f_p0, call_result);
    xm_block_set_ret(f_entry, f_result);

    // Inline g into f at the NOP instruction (index 0)
    XmRef call_args[] = {f_p0};  // g(x) — pass x as argument
    XmRef inlined = xm_inline_function(caller, f_entry, 0, callee, call_args, 1);
    fprintf(stderr, " inlined=%s", xm_ref_is_none(inlined) ? "NONE" : "ok");
    assert(!xm_ref_is_none(inlined));

    // Patch: replace call_result references with inlined result
    // The ADD instruction is now in the continuation block (inline.cont)
    // Find the continuation block and patch its ADD's arg
    XmBlock *cont = NULL;
    for (uint32_t bi = 0; bi < caller->nblk; bi++) {
        if (caller->blocks[bi]->label && strstr(caller->blocks[bi]->label, "inline.cont")) {
            cont = caller->blocks[bi];
            break;
        }
    }
    assert(cont != NULL);
    // Patch ADD: replace old call_result with inlined return value
    for (uint32_t i = 0; i < cont->nins; i++) {
        for (int a = 0; a < 2; a++) {
            if (cont->ins[i].args[a] == call_result) {
                cont->ins[i].args[a] = inlined;
            }
        }
    }

    // Compile and execute
    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(caller, &alloc);
    fprintf(stderr, " success=%d", res.success);
    assert(res.success);
    fprintf(stderr, " code=%u", res.code_size);

    int64_t r = jit_call1(res.code, 10);
    fprintf(stderr, " f(10)=%lld", (long long) r);
    assert(r == 30);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(caller);
    xm_func_destroy(callee);
    fprintf(stderr, "\n  PASS\n");
}

/*
 * Test: OSR entry into a loop mid-iteration
 * Function: sum_loop(count, step, idx_start)
 *   sum = 0
 *   loop: count--; idx += step; sum += idx; if count > 0 goto loop
 *   return sum
 *
 * Normal call: sum_loop(5, 1, 0) → 1+2+3+4+5 = 15
 * OSR entry: enter at loop header with count=3, step=1, idx=7, sum=10
 *   iteration 1: count=2, idx=8, sum=18
 *   iteration 2: count=1, idx=9, sum=27
 *   iteration 3: count=0, idx=10, sum=37
 *   return 37
 */
static void test_osr_entry(void) {
    fprintf(stderr, "  test_osr_entry...");

    // Build: f(count, step, idx_start) → sum
    XmFunc *func = xm_func_new("sum_loop");
    func->num_params = 3;
    XmRef p_count = xm_new_vreg(func, XR_REP_I64);  // vreg 0: count
    XmRef p_step = xm_new_vreg(func, XR_REP_I64);   // vreg 1: step
    XmRef p_idx = xm_new_vreg(func, XR_REP_I64);    // vreg 2: idx_start

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmBlock *loop = xm_func_add_block(func, "loop");
    XmBlock *exit = xm_func_add_block(func, "exit");

    // entry: sum=0, goto loop
    XmRef c0 = xm_const_i64(func, 0);
    XmRef zero = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c0);
    xm_block_set_jmp(entry, loop);
    xm_block_add_pred(loop, entry, func->arena);

    // Add back-edge pred BEFORE creating Phis (so narg=2)
    xm_block_add_pred(loop, loop, func->arena);

    // loop: Phi nodes for count, idx, sum
    loop->is_loop_header = true;
    XmPhi *phi_count = xm_add_phi(func, loop, XR_REP_I64);
    XmPhi *phi_idx = xm_add_phi(func, loop, XR_REP_I64);
    XmPhi *phi_sum = xm_add_phi(func, loop, XR_REP_I64);

    // pred 0 = entry, pred 1 = loop (back-edge)
    xm_phi_set_arg(phi_count, 0, p_count);
    xm_phi_set_arg(phi_idx, 0, p_idx);
    xm_phi_set_arg(phi_sum, 0, zero);

    // count--
    XmRef c1 = xm_const_i64(func, 1);
    XmRef one = xm_emit_unary(func, loop, XM_CONST_I64, XR_REP_I64, c1);
    XmRef new_count = xm_emit(func, loop, XM_SUB, XR_REP_I64, phi_count->dst, one);

    // idx += step
    XmRef new_idx = xm_emit(func, loop, XM_ADD, XR_REP_I64, phi_idx->dst, p_step);

    // sum += new_idx
    XmRef new_sum = xm_emit(func, loop, XM_ADD, XR_REP_I64, phi_sum->dst, new_idx);

    // if new_count > 0: loop, else exit
    XmRef c0b = xm_const_i64(func, 0);
    XmRef zero2 = xm_emit_unary(func, loop, XM_CONST_I64, XR_REP_I64, c0b);
    XmRef cond = xm_emit(func, loop, XM_LT, XR_REP_I64, zero2, new_count);

    // Back-edge phi args (pred already added above)
    xm_phi_set_arg(phi_count, 1, new_count);
    xm_phi_set_arg(phi_idx, 1, new_idx);
    xm_phi_set_arg(phi_sum, 1, new_sum);

    xm_block_set_br(loop, cond, loop, exit);
    xm_block_add_pred(exit, loop, func->arena);

    // exit: return sum
    xm_block_set_ret(exit, new_sum);

    // Set bc_slot for each vreg so OSR stub can load from values[] array.
    // Without this, OSR stub sees bc_slot=-1 and skips loading live vregs.
    for (uint32_t v = 0; v < func->nvreg; v++) {
        func->vregs[v].bc_slot = (int16_t) v;
    }

    // Compile. We call xm_codegen_arm64 directly because preds+phis are
    // already wired manually, and xm_rebuild_preds (run by safe_codegen)
    // would re-allocate phi args while OSR snapshot collection runs inside
    // codegen — keep them consistent by skipping the rebuild.
    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = xm_codegen_native(func, &alloc);
    assert(res.success);
    fprintf(stderr, " code=%u nosr=%u", res.code_size, res.nosr);

    // Test 1: normal call — sum_loop(5, 1, 0) = 1+2+3+4+5 = 15
    int64_t r1 = jit_call3(res.code, 5, 1, 0);
    fprintf(stderr, " normal=%lld", (long long) r1);
    assert(r1 == 15);

    // Test 2: OSR entry — enter loop with count=3, step=1, idx=7, sum=10
    // Expected: iteration 1: idx=8,sum=18; iter 2: idx=9,sum=27; iter 3: idx=10,sum=37
    if (res.nosr > 0) {
        XmOsrEntry *osr = &res.osr_entries[0];
        void *osr_code = (uint8_t *) res.code + osr->entry_offset;

        // Prepare values array indexed by vreg
        // We need to know which vregs the OSR stub loads.
        // vreg 0=count, 1=step, 2=idx_start
        // phi_count->dst, phi_idx->dst, phi_sum->dst are the loop-carried vregs
        // The stub loads based on whatever vregs are live at the loop header.
        int64_t values[32] = {0};
        // Map: p_count(vreg0)=doesn't matter, p_step(vreg1)=1, p_idx(vreg2)=doesn't matter
        values[XM_REF_INDEX(p_step)] = 1;
        // phi vregs: phi_count->dst, phi_idx->dst, phi_sum->dst
        values[XM_REF_INDEX(phi_count->dst)] = 3;
        values[XM_REF_INDEX(phi_idx->dst)] = 7;
        values[XM_REF_INDEX(phi_sum->dst)] = 10;
        // Also set constant "1" vreg and "0" vreg
        values[XM_REF_INDEX(one)] = 1;
        values[XM_REF_INDEX(zero2)] = 0;

        XrValue osr_result;
        jit_env_reset();
        bool ok = xm_jit_osr_enter(osr_code, &g_jit_coro, values, XR_SLOT_I64, &osr_result);
        fprintf(stderr, " osr_ok=%d osr=%lld", ok, (long long) osr_result.i);
        assert(ok);
        assert(osr_result.i == 37);
    } else {
        fprintf(stderr, " (no OSR entries)");
    }

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/*
 * Test: deoptimization via GUARD_NONNULL
 * f(x) = guard_nonnull(x); return x + 1
 * f(10) = 11 (guard passes)
 * f(0) = DEOPT_MARKER (guard fails)
 */
static void test_deopt_guard(void) {
    fprintf(stderr, "  test_deopt_guard...");

    XmFunc *func = xm_func_new("deopt_guard");
    func->num_params = 1;
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);

    XmBlock *entry = xm_func_add_block(func, "entry");

    // guard_nonnull(p0) -> deopt if p0 == 0
    xm_emit_unary(func, entry, XM_GUARD_NONNULL, XR_REP_VOID, p0);

    // return p0 + 1
    XmRef c1 = xm_const_i64(func, 1);
    XmRef one = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c1);
    XmRef result = xm_emit(func, entry, XM_ADD, XR_REP_I64, p0, one);
    xm_block_set_ret(entry, result);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    fprintf(stderr, " code=%u", res.code_size);

    // Guard passes: f(10) = 11
    int64_t r1 = jit_call1(res.code, 10);
    fprintf(stderr, " f(10)=%lld", (long long) r1);
    assert(r1 == 11);

    // Guard fails: f(0) = DEOPT_MARKER
    int64_t r2 = jit_call1(res.code, 0);
    fprintf(stderr, " f(0)=0x%llx", (unsigned long long) r2);
    assert(r2 == (int64_t) 0xDEAD0001DEAD0001LL);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/*
 * Test: LOAD_FIELD — load a 64-bit payload from a fake Json object's field[1]
 *
 * Simulates XrJson layout: XrGCHeader(16B) + XrValue fields[]
 * Each XrValue = 16B (8B payload + 4B tag + 4B pad)
 * fields[1].payload is at byte_offset 32 (= 16 + 1*16)
 *
 * Xm:
 *   @entry:
 *     v1 =i64 load.field v0, #32   // load at byte_offset 32
 *     ret v1
 */
static void test_load_field(void) {
    fprintf(stderr, "  test_load_field...");

    XmFunc *func = xm_func_new("load_field");
    func->num_params = 1;
    XmRef p0 = xm_new_vreg(func, XR_REP_PTR);  // v0 = obj ptr

    XmBlock *entry = xm_func_add_block(func, "entry");

    // Json field[1] byte_offset = GCHeader(16) + 1 * XrValue(16) = 32
    XmRef byte_off = xm_const_i64(func, 32);
    XmRef v1 = xm_emit(func, entry, XM_LOAD_FIELD, XR_REP_I64, p0, byte_off);
    xm_block_set_ret(entry, v1);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    fprintf(stderr, " code=%u", res.code_size);

    // Build a fake Json object: GCHeader(16B) + field[0](16B) + field[1](16B)
    // XM_LOAD_FIELD codegen loads at (byte_off + XM_XRVALUE_PAYLOAD_OFFSET),
    // so byte_off=32 reads fields[1].payload at byte 32+8=40.
    uint8_t fake_obj[64];
    memset(fake_obj, 0, sizeof(fake_obj));
    int64_t expected = 0x12345678LL;
    memcpy(fake_obj + 32 + 8, &expected, 8);  // fields[1].payload

    int64_t result = jit_call1(res.code, (int64_t) (intptr_t) fake_obj);
    fprintf(stderr, " result=0x%llx", (long long) result);
    assert(result == expected);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/*
 * Test: STORE_FIELD + LOAD_FIELD roundtrip (Json layout)
 *
 * Json field[2] byte_offset = GCHeader(16) + 2 * XrValue(16) = 48
 *
 * Xm:
 *   @entry:
 *     v2 =i64 const.i64 #999
 *     store.field v0, v2 [byte_offset=48]
 *     v3 =i64 load.field v0, #48
 *     ret v3
 */
static void test_store_load_field(void) {
    fprintf(stderr, "  test_store_load_field...");

    XmFunc *func = xm_func_new("store_load_field");
    func->num_params = 1;
    XmRef p0 = xm_new_vreg(func, XR_REP_PTR);  // v0 = obj ptr

    XmBlock *entry = xm_func_add_block(func, "entry");

    // v2 = 999
    XmRef c999 = xm_const_i64(func, 999);
    XmRef v2 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c999);

    // Json field[2] byte_offset = 16 + 2*16 = 48
    // STORE_FIELD: args[0]=obj, args[1]=value, dst=const(byte_offset)
    XmRef off_store = xm_const_i64(func, 48);
    xm_emit_raw(func, entry, XM_STORE_FIELD, XR_REP_VOID, off_store, p0, v2);

    // LOAD_FIELD: args[0]=obj, args[1]=const(byte_offset)
    XmRef off_load = xm_const_i64(func, 48);
    XmRef v3 = xm_emit(func, entry, XM_LOAD_FIELD, XR_REP_I64, p0, off_load);
    xm_block_set_ret(entry, v3);

    // Run write barrier pass (STORE_FIELD triggers it)
    xm_insert_write_barriers(func);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    fprintf(stderr, " code=%u", res.code_size);

    // Build fake Json object: GCHeader(16B) + 3 fields (48B)
    uint8_t fake_obj[80];
    memset(fake_obj, 0, sizeof(fake_obj));

    int64_t result = jit_call1(res.code, (int64_t) (intptr_t) fake_obj);
    fprintf(stderr, " result=%lld", (long long) result);
    assert(result == 999);

    // Verify the store actually wrote payload at byte 48+8=56
    // (STORE_FIELD writes at byte_offset + XM_XRVALUE_PAYLOAD_OFFSET).
    int64_t stored;
    memcpy(&stored, fake_obj + 48 + 8, 8);
    assert(stored == 999);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/*
 * Test: CALL_C — call a C function from JIT code
 *
 * C helper: jit_test_add42(coro, x) -> x + 42
 * Xm:
 *   @entry:
 *     v1 =i64 call.c @jit_test_add42, v0
 *     ret v1
 */
static XrJitResult jit_test_add42(void *coro, int64_t x) {
    (void) coro;
    return (XrJitResult) {x + 42, 3};
}

static void test_call_c(void) {
    fprintf(stderr, "  test_call_c...");

    XmFunc *func = xm_func_new("call_c_test");
    func->num_params = 1;
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);  // v0 = arg

    XmBlock *entry = xm_func_add_block(func, "entry");

    // CALL_C: args[0] = func ptr (const), args[1] = extra arg (v0)
    XmRef fn_ref = xm_const_ptr(func, (void *) jit_test_add42);
    XmRef v1 = xm_emit(func, entry, XM_CALL_C, XR_REP_I64, fn_ref, p0);
    xm_block_set_ret(entry, v1);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    fprintf(stderr, " code=%u", res.code_size);

    // call_c_test(100) → jit_test_add42(coro=0, 100) → 142
    int64_t r1 = jit_call1(res.code, 100);
    fprintf(stderr, " f(100)=%lld", (long long) r1);
    assert(r1 == 142);

    int64_t r2 = jit_call1(res.code, 0);
    fprintf(stderr, " f(0)=%lld", (long long) r2);
    assert(r2 == 42);

    int64_t r3 = jit_call1(res.code, -10);
    fprintf(stderr, " f(-10)=%lld", (long long) r3);
    assert(r3 == 32);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/*
 * Test: GUARD_CLASS — check shape_id, deopt on mismatch
 *
 * Fake object: GCHeader with extra=42 (shape_id)
 * Xm:
 *   @entry:
 *     guard.class v0, #42        // deopt if shape_id != 42
 *     v1 =i64 const.i64 #1
 *     ret v1
 *
 * Pass shape_id=42 → return 1
 * Pass shape_id=99 → deopt (return DEOPT_MARKER)
 */
static void test_guard_class(void) {
    fprintf(stderr, "  test_guard_class...");

    XmFunc *func = xm_func_new("guard_class");
    func->num_params = 1;
    XmRef p0 = xm_new_vreg(func, XR_REP_PTR);  // v0 = obj ptr

    XmBlock *entry = xm_func_add_block(func, "entry");

    // GUARD_CLASS: args[0]=obj, args[1]=expected_shape_id
    XmRef expected_shape = xm_const_i64(func, 42);
    xm_emit_void(func, entry, XM_GUARD_CLASS, p0, expected_shape);

    XmRef c1 = xm_const_i64(func, 1);
    XmRef v1 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c1);
    xm_block_set_ret(entry, v1);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    fprintf(stderr, " code=%u", res.code_size);

    // Build fake GCHeader: extra (shape_id) at offset 10
    // XrGCHeader: gc_next(8B) + type(1B) + marked(1B) + extra(2B) + objsize(4B) = 16B
    uint8_t fake_obj[32];
    memset(fake_obj, 0, sizeof(fake_obj));

    // Set shape_id = 42 at offset 10 (little-endian uint16)
    uint16_t shape_id = 42;
    memcpy(fake_obj + 10, &shape_id, 2);

    int64_t r1 = jit_call1(res.code, (int64_t) (intptr_t) fake_obj);
    fprintf(stderr, " match=%lld", (long long) r1);
    assert(r1 == 1);

    // Change shape_id to 99 → should deopt
    shape_id = 99;
    memcpy(fake_obj + 10, &shape_id, 2);
    int64_t r2 = jit_call1(res.code, (int64_t) (intptr_t) fake_obj);
    fprintf(stderr, " mismatch=0x%llx", (unsigned long long) r2);
    assert(r2 == (int64_t) 0xDEAD0001DEAD0001LL);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/*
 * Test: CALL_C with const_ptr arg — simulates OP_NEWJSON path
 *
 * C helper: jit_test_make_obj(coro, shape_ptr) -> shape_ptr + 100
 *   (simulates object allocation returning a pointer)
 *
 * Xm:
 *   @entry:
 *     v0 =ptr call.c @jit_test_make_obj, @shape_ptr_const
 *     v1 =i64 load.field v0, #16  // read at byte_offset 16 (Json field[0])
 *     ret v1
 *
 * Tests that CALL_C correctly handles const_ptr for both fn and arg.
 */
static XrJitResult jit_test_make_obj(void *coro, int64_t shape_raw) {
    (void) coro;
    return (XrJitResult) {shape_raw, 5};
}

static void test_call_c_const_arg(void) {
    fprintf(stderr, "  test_call_c_const_arg...");

    // Prepare a fake "object" buffer with a known value at byte offset 16
    static uint8_t fake_buf[64];
    memset(fake_buf, 0, sizeof(fake_buf));
    int64_t magic = 0xCAFE;
    // LOAD_FIELD reads at byte_offset + XM_XRVALUE_PAYLOAD_OFFSET (8),
    // so for byte_off=16 we must place the payload at byte 16+8=24.
    memcpy(fake_buf + 16 + 8, &magic, 8);

    XmFunc *func = xm_func_new("call_c_const_arg");
    func->num_params = 0;

    XmBlock *entry = xm_func_add_block(func, "entry");

    // CALL_C with both args as const_ptr
    XmRef fn_ref = xm_const_ptr(func, (void *) jit_test_make_obj);
    XmRef shape_ref = xm_const_ptr(func, (void *) fake_buf);
    XmRef v0 = xm_emit(func, entry, XM_CALL_C, XR_REP_PTR, fn_ref, shape_ref);

    // LOAD_FIELD at byte_offset 16 (Json field[0])
    XmRef off = xm_const_i64(func, 16);
    XmRef v1 = xm_emit(func, entry, XM_LOAD_FIELD, XR_REP_I64, v0, off);
    xm_block_set_ret(entry, v1);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    fprintf(stderr, " code=%u", res.code_size);

    int64_t result = jit_call0(res.code);
    fprintf(stderr, " result=0x%llx", (long long) result);
    assert(result == 0xCAFE);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/* ====================================================================
 * Phase 8: x64 / ARM64 codegen hardening regressions (docs/tasks/010)
 *
 * The JIT runs on whatever the host machine provides (ARM64 here); on
 * x64 hosts the same Xm exercises the §10.1–§10.6 fixes (FP scratch
 * contract, non-commutative SUB/FSUB/FDIV alias-aware emission, double-
 * scratch fallback, deopt+spill copy, CALL_SELF_DIRECT fast path, OSR
 * multi-entry under pressure). When run on ARM64 they still validate
 * IR-level correctness so any future cross-backend miscompile is caught.
 * ==================================================================== */

/*
 * test_int_sub_chain: f(x, y) = x - (x - y) == y
 *
 * Forces arg0 (x) to remain live after the first SUB, which is exactly
 * the alias-aware emission case the §10.2 fix targets — without it x64
 * would emit "mov rd, rn ; sub rd, rm" and lose arg0 when the regalloc
 * picks dst == arg1.
 */
static void test_int_sub_chain(void) {
    fprintf(stderr, "  test_int_sub_chain...");

    XmFunc *func = xm_func_new("int_sub_chain");
    func->num_params = 2;
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);
    XmRef p1 = xm_new_vreg(func, XR_REP_I64);

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmRef inner = xm_emit(func, entry, XM_SUB, XR_REP_I64, p0, p1);     // x - y
    XmRef outer = xm_emit(func, entry, XM_SUB, XR_REP_I64, p0, inner);  // x - (x-y)
    xm_block_set_ret(entry, outer);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    fprintf(stderr, " code=%u", res.code_size);

    int64_t r1 = jit_call2(res.code, 100, 7);
    int64_t r2 = jit_call2(res.code, -3, -10);
    int64_t r3 = jit_call2(res.code, 0, 42);
    fprintf(stderr, " f(100,7)=%lld f(-3,-10)=%lld f(0,42)=%lld", (long long) r1, (long long) r2,
            (long long) r3);
    assert(r1 == 7);
    assert(r2 == -10);
    assert(r3 == 42);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/*
 * test_fp_noncommutative_chain: f(x, y) = (x - y) / y
 *
 * FSUB then FDIV — both non-commutative, with arg1 (y) reused after
 * the first op. Pre-§10.2 fix on x64 this was silently miscompiled.
 */
static void test_fp_noncommutative_chain(void) {
    fprintf(stderr, "  test_fp_noncommutative_chain...");

    XmFunc *func = xm_func_new("fp_noncomm_chain");
    func->num_params = 2;
    XmRef p0 = xm_new_vreg(func, XR_REP_F64);
    XmRef p1 = xm_new_vreg(func, XR_REP_F64);

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmRef diff = xm_emit(func, entry, XM_FSUB, XR_REP_F64, p0, p1);   // x - y
    XmRef quo = xm_emit(func, entry, XM_FDIV, XR_REP_F64, diff, p1);  // (x-y)/y
    xm_block_set_ret(entry, quo);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    fprintf(stderr, " code=%u", res.code_size);

    int64_t r1 = jit_call2_f64(res.code, 10.0, 4.0);  // 1.5
    int64_t r2 = jit_call2_f64(res.code, 21.0, 7.0);  // 2.0
    int64_t r3 = jit_call2_f64(res.code, -3.0, 3.0);  // -2.0
    double f1, f2, f3;
    memcpy(&f1, &r1, 8);
    memcpy(&f2, &r2, 8);
    memcpy(&f3, &r3, 8);
    fprintf(stderr, " f(10,4)=%g f(21,7)=%g f(-3,3)=%g", f1, f2, f3);
    assert(f1 == 1.5);
    assert(f2 == 2.0);
    assert(f3 == -2.0);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/* C helper for test_call_c_fp_live_across_call: returns 16.0 to make the
 * expected sum a clean 136.0 = (0+1+...+15) + 16. */
static XrJitResult jit_test_fp16_const(void *coro, int64_t x_raw) {
    (void) coro;
    double x;
    memcpy(&x, &x_raw, 8);
    double result = x * 2.0;
    int64_t result_raw;
    memcpy(&result_raw, &result, 8);
    return (XrJitResult) {result_raw, 4};
}

/*
 * test_call_c_fp_live_across_call: 16 f64 vregs live across a CALL_C.
 *
 * On x64 prior to §10.1 (xmm15 demoted from allocatable), 16 simultaneously
 * live FP vregs would crowd into xmm15 — the same register the call_c
 * stub reserves as scratch — and silently corrupt one value across the
 * call. After the fix only xmm0..xmm14 are allocatable, so the regalloc
 * is forced to spill the 16th vreg. The expected result 136.0 catches
 * any silent miscompile.
 */
static void test_call_c_fp_live_across_call(void) {
    fprintf(stderr, "  test_call_c_fp_live_across_call...");
#if !(defined(__x86_64__) || defined(_M_X64))
    // ARM64 regalloc cannot spill FP vregs; with only 16 D regs we cannot
    // build a scenario that keeps 16 f64 vregs simultaneously live. The
    // §10.1 fix is x64-specific (xmm15 demoted from allocatable), so SKIP
    // here and rely on x64 host runs for actual verification.
    fprintf(stderr, " SKIP (x64-specific: ARM64 FP regalloc has no spill)\n");
    return;
#endif
    XmFunc *func = xm_func_new("fp_live_call_c");
    func->num_params = 16;
    XmRef ps[16];
    for (int i = 0; i < 16; i++)
        ps[i] = xm_new_vreg(func, XR_REP_F64);

    XmBlock *entry = xm_func_add_block(func, "entry");

    // CALL_C(jit_test_fp16_const, 8.0) -> 16.0; result kept live too.
    XmRef fn = xm_const_ptr(func, (void *) jit_test_fp16_const);
    XmRef c8 = xm_const_f64(func, 8.0);
    XmRef c8v = xm_emit_unary(func, entry, XM_CONST_F64, XR_REP_F64, c8);
    XmRef call_res = xm_emit(func, entry, XM_CALL_C, XR_REP_F64, fn, c8v);

    // Now sum p0..p15 + call_res. All 16 ps[i] must remain live across
    // the CALL_C above; the regalloc decides where to spill them.
    XmRef acc = ps[0];
    for (int i = 1; i < 16; i++) {
        acc = xm_emit(func, entry, XM_FADD, XR_REP_F64, acc, ps[i]);
    }
    acc = xm_emit(func, entry, XM_FADD, XR_REP_F64, acc, call_res);
    xm_block_set_ret(entry, acc);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    fprintf(stderr, " code=%u", res.code_size);

    int64_t buf[16];
    for (int i = 0; i < 16; i++) {
        double v = (double) i;
        memcpy(&buf[i], &v, 8);
    }
    int64_t raw = jit_calln(res.code, buf);
    double result;
    memcpy(&result, &raw, 8);
    fprintf(stderr, " result=%g", result);
    // 0+1+...+15 = 120; helper(8.0) = 16.0; total 136.0
    assert(result == 136.0);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/*
 * test_binop_pressure_chain: 12 i64 params, chained ADD/SUB.
 *
 * 12 simultaneously live GP vregs > x64 allocatable count (11), so the
 * regalloc must spill. Combined with non-commutative SUB this exercises
 * the §10.3 double-scratch fallback (push-stash / load-arg / pop-rd /
 * sub rd, scratch on x64). On ARM64 with 28 allocatable GP this still
 * validates IR correctness end-to-end.
 *
 * f(p0..p11) = ((p0+p1) - (p2+p3)) + ((p4+p5) - (p6+p7))
 *            + ((p8+p9) - (p10+p11))
 */
static void test_binop_pressure_chain(void) {
    fprintf(stderr, "  test_binop_pressure_chain...");

    XmFunc *func = xm_func_new("binop_pressure");
    func->num_params = 12;
    XmRef ps[12];
    for (int i = 0; i < 12; i++)
        ps[i] = xm_new_vreg(func, XR_REP_I64);

    XmBlock *entry = xm_func_add_block(func, "entry");
    // pair-add: a01, a23, ... a1011
    XmRef pair[6];
    for (int i = 0; i < 6; i++)
        pair[i] = xm_emit(func, entry, XM_ADD, XR_REP_I64, ps[i * 2], ps[i * 2 + 1]);
    // sub-pair: pair[0]-pair[1], pair[2]-pair[3], pair[4]-pair[5]
    XmRef diff0 = xm_emit(func, entry, XM_SUB, XR_REP_I64, pair[0], pair[1]);
    XmRef diff1 = xm_emit(func, entry, XM_SUB, XR_REP_I64, pair[2], pair[3]);
    XmRef diff2 = xm_emit(func, entry, XM_SUB, XR_REP_I64, pair[4], pair[5]);
    // sum the three diffs
    XmRef s01 = xm_emit(func, entry, XM_ADD, XR_REP_I64, diff0, diff1);
    XmRef total = xm_emit(func, entry, XM_ADD, XR_REP_I64, s01, diff2);
    xm_block_set_ret(entry, total);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    fprintf(stderr, " code=%u", res.code_size);

    // Inputs: 1..12. Expected:
    //   pair = (3, 7, 11, 15, 19, 23)
    //   diff = (-4, -4, -4)
    //   total = -12
    int64_t buf[12];
    for (int i = 0; i < 12; i++)
        buf[i] = i + 1;
    int64_t r1 = jit_calln(res.code, buf);
    fprintf(stderr, " f(1..12)=%lld", (long long) r1);
    assert(r1 == -12);

    // Inputs all zero -> 0
    for (int i = 0; i < 12; i++)
        buf[i] = 0;
    int64_t r2 = jit_calln(res.code, buf);
    assert(r2 == 0);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/*
 * test_deopt_with_spill_pressure: 12 i64 params + GUARD_NONNULL on p0.
 *
 * Forces deopt stub to copy spill slots into jit_ctx->deopt_spill_save
 * (the §10.4 fix). On guard pass, the function returns the sum of all
 * 12 params; on guard fail it returns DEOPT_MARKER.
 */
static void test_deopt_with_spill_pressure(void) {
    fprintf(stderr, "  test_deopt_with_spill_pressure...");

    XmFunc *func = xm_func_new("deopt_spill_pressure");
    func->num_params = 12;
    XmRef ps[12];
    for (int i = 0; i < 12; i++)
        ps[i] = xm_new_vreg(func, XR_REP_I64);

    XmBlock *entry = xm_func_add_block(func, "entry");

    // Sum half before the guard so the regalloc keeps all 12 ps live
    // through the guard, which is when the deopt-spill copy matters.
    XmRef partial = ps[0];
    for (int i = 1; i < 6; i++)
        partial = xm_emit(func, entry, XM_ADD, XR_REP_I64, partial, ps[i]);

    // Guard: deopt if p0 == 0
    xm_emit_unary(func, entry, XM_GUARD_NONNULL, XR_REP_VOID, ps[0]);

    // After guard, fold in the rest. ps[6..11] must remain live across
    // the guard, which is exactly what the spill-copy loop handles.
    XmRef acc = partial;
    for (int i = 6; i < 12; i++)
        acc = xm_emit(func, entry, XM_ADD, XR_REP_I64, acc, ps[i]);
    xm_block_set_ret(entry, acc);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    fprintf(stderr, " code=%u", res.code_size);

    // success path: sum 1..12 = 78
    int64_t buf[12];
    for (int i = 0; i < 12; i++)
        buf[i] = i + 1;
    int64_t r1 = jit_calln(res.code, buf);
    fprintf(stderr, " sum=%lld", (long long) r1);
    assert(r1 == 78);

    // deopt path: p0 == 0 triggers GUARD_NONNULL
    buf[0] = 0;
    int64_t r2 = jit_calln(res.code, buf);
    fprintf(stderr, " deopt=0x%llx", (unsigned long long) r2);
    assert(r2 == (int64_t) 0xDEAD0001DEAD0001LL);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/*
 * test_call_self_direct: f(n) = (n == 0) ? 0 : n + f(n - 1)
 *
 * Exercises XM_CALL_SELF_DIRECT register-passing → fast_entry path
 * (PATCH_CALL_SELF_FAST). Both the safepoint id write and the
 * frame stack-map restore at lines 374-376 of xm_codegen_call.c are
 * verified by every recursive iteration completing successfully.
 */
static void test_call_self_direct(void) {
    fprintf(stderr, "  test_call_self_direct...");

    XmFunc *func = xm_func_new("call_self");
    func->num_params = 1;
    XmRef p0 = xm_new_vreg(func, XR_REP_I64);

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmBlock *base = xm_func_add_block(func, "base");
    XmBlock *rec = xm_func_add_block(func, "rec");

    // entry: cmp = (p0 == 0); br cmp, @base, @rec
    XmRef cz = xm_const_i64(func, 0);
    XmRef zero = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, cz);
    XmRef cmp = xm_emit(func, entry, XM_EQ, XR_REP_I64, p0, zero);
    xm_block_set_br(entry, cmp, base, rec);

    // @base: ret 0
    xm_block_set_ret(base, zero);

    // @rec: n_minus_1 = p0 - 1; rec = CALL_SELF_DIRECT(n_minus_1); ret p0 + rec
    XmRef c1 = xm_const_i64(func, 1);
    XmRef one = xm_emit_unary(func, rec, XM_CONST_I64, XR_REP_I64, c1);
    XmRef nm1 = xm_emit(func, rec, XM_SUB, XR_REP_I64, p0, one);
    XmRef call_res = xm_emit(func, rec, XM_CALL_SELF_DIRECT, XR_REP_I64, nm1, XM_NONE);
    rec->ins[rec->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
    // Bind the single argument in the call_arg_pool so emit_call_args_from_pool
    // sets up the correct tag-pack and (for x64) jit_ctx->call_args[0].
    XmRef pool_args[1] = {nm1};
    xm_func_bind_call_args(func, call_res, pool_args, 1);
    XmRef sum = xm_emit(func, rec, XM_ADD, XR_REP_I64, p0, call_res);
    xm_block_set_ret(rec, sum);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = safe_codegen(func, &alloc);
    assert(res.success);
    fprintf(stderr, " code=%u fast=%u", res.code_size, res.fast_entry_offset);

    int64_t r0 = jit_call1(res.code, 0);
    int64_t r1 = jit_call1(res.code, 1);
    int64_t r5 = jit_call1(res.code, 5);
    int64_t r10 = jit_call1(res.code, 10);
    fprintf(stderr, " f(0)=%lld f(1)=%lld f(5)=%lld f(10)=%lld", (long long) r0, (long long) r1,
            (long long) r5, (long long) r10);
    assert(r0 == 0);
    assert(r1 == 1);
    assert(r5 == 15);   // 5+4+3+2+1
    assert(r10 == 55);  // 1+...+10

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/*
 * test_osr_entry_pressure: f(count, p1, p2, p3, p4) = count * (p1+p2+p3+p4)
 *
 * Loop with 2 phi vregs (count, sum) + 4 loop-invariant params kept live
 * across the back edge + a chain of ADDs in the body. On x64 hosts this
 * exercises the §10.5 OSR stub two-pass register loading and the §10.6
 * "no residual frame state" reentrancy. On ARM64 (no Blueprint -> no OSR
 * entries), only the normal-entry path is verified, but it still locks
 * down loop-header regalloc under high vreg pressure.
 *
 * Run the same compiled code 4× with different inputs to verify no
 * stale frame state leaks between calls.
 */
static void test_osr_entry_pressure(void) {
    fprintf(stderr, "  test_osr_entry_pressure...");

    XmFunc *func = xm_func_new("osr_pressure");
    func->num_params = 5;
    XmRef p_count = xm_new_vreg(func, XR_REP_I64);
    XmRef p1 = xm_new_vreg(func, XR_REP_I64);
    XmRef p2 = xm_new_vreg(func, XR_REP_I64);
    XmRef p3 = xm_new_vreg(func, XR_REP_I64);
    XmRef p4 = xm_new_vreg(func, XR_REP_I64);

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmBlock *loop = xm_func_add_block(func, "loop");
    XmBlock *body = xm_func_add_block(func, "body");
    XmBlock *exit_ = xm_func_add_block(func, "exit");

    // entry: sum = 0; jmp @loop
    XmRef cz = xm_const_i64(func, 0);
    XmRef zero = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, cz);
    xm_block_set_jmp(entry, loop);
    xm_block_add_pred(loop, entry, func->arena);
    xm_block_add_pred(loop, body, func->arena);  // back-edge, declared early

    // @loop (header) -> phi nodes
    loop->is_loop_header = true;
    XmPhi *phi_count = xm_add_phi(func, loop, XR_REP_I64);
    XmPhi *phi_sum = xm_add_phi(func, loop, XR_REP_I64);
    xm_phi_set_arg(phi_count, 0, p_count);  // entry value
    xm_phi_set_arg(phi_sum, 0, zero);

    // cond = (phi_count > 0) ? body : exit
    XmRef cz_b = xm_const_i64(func, 0);
    XmRef zb = xm_emit_unary(func, loop, XM_CONST_I64, XR_REP_I64, cz_b);
    XmRef cond = xm_emit(func, loop, XM_LT, XR_REP_I64, zb, phi_count->dst);
    xm_block_set_br(loop, cond, body, exit_);
    xm_block_add_pred(body, loop, func->arena);
    xm_block_add_pred(exit_, loop, func->arena);

    // @body: sum += p1+p2+p3+p4 ; count -= 1 ; jmp @loop
    // Chain ADDs to keep p1..p4 + intermediates all live at end of body.
    XmRef s12 = xm_emit(func, body, XM_ADD, XR_REP_I64, p1, p2);
    XmRef s34 = xm_emit(func, body, XM_ADD, XR_REP_I64, p3, p4);
    XmRef delta = xm_emit(func, body, XM_ADD, XR_REP_I64, s12, s34);
    XmRef new_sum = xm_emit(func, body, XM_ADD, XR_REP_I64, phi_sum->dst, delta);
    XmRef c1 = xm_const_i64(func, 1);
    XmRef one = xm_emit_unary(func, body, XM_CONST_I64, XR_REP_I64, c1);
    XmRef new_count = xm_emit(func, body, XM_SUB, XR_REP_I64, phi_count->dst, one);
    xm_block_set_jmp(body, loop);
    xm_phi_set_arg(phi_count, 1, new_count);
    xm_phi_set_arg(phi_sum, 1, new_sum);

    // @exit: ret phi_sum
    xm_block_set_ret(exit_, phi_sum->dst);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    // Match test_osr_entry: bypass safe_codegen because phi args are
    // already wired manually and xm_rebuild_preds would re-allocate
    // them mid-snapshot.
    XmCodegenResult res = xm_codegen_native(func, &alloc);
    assert(res.success);
    fprintf(stderr, " code=%u nosr=%u", res.code_size, res.nosr);

    // Run the same compiled code 4 rounds with different inputs to
    // verify no stale frame state leaks across calls.
    struct {
        int64_t count, p1, p2, p3, p4, expected;
    } cases[] = {
        {3, 1, 2, 3, 4, 30},   // 3 * 10
        {5, 1, 1, 1, 1, 20},   // 5 * 4
        {0, 7, 8, 9, 10, 0},   // loop body never runs
        {4, -1, 2, -3, 4, 8},  // 4 * 2
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int64_t r =
            jit_call5(res.code, cases[i].count, cases[i].p1, cases[i].p2, cases[i].p3, cases[i].p4);
        fprintf(stderr, " r%zu=%lld", i, (long long) r);
        assert(r == cases[i].expected);
    }

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/*
 * test_spill_only_param_init: 24-param ADD chain that forces several
 * params to be spill-only at function entry. Validates that the LSRA
 * spill-slot allocator does not reuse a slot across two vregs whose
 * full live ranges overlap.
 *
 * f(p0..p23) = p0 + p1 + ... + p23.
 *
 * 24 simultaneously live i64 params exceed ARM64 (22) and x64 (11)
 * allocatable GP. Before the fix, two distinct params (e.g. p1 with
 * seg[0]=[0,44)/r1 seg[1]=[44,50)/r-1 and p22 with seg[0]=[0,44)/r-1
 * seg[1]=[44,50)/r-1) could be assigned the same spill slot because
 * the reuse check used the LsRange sibling's end (which is only the
 * spill-segment end), not the full vreg's last-live position. Pos 44
 * then saves p1's reg into the slot, clobbering the value p22 still
 * needs to read in [0, 50).
 *
 * The fix: don't reuse spill slots across distinct vregs. Phi bundles
 * still reuse safely because their members are non-overlapping by
 * construction.
 */
static void test_spill_only_param_init(void) {
    fprintf(stderr, "  test_spill_only_param_init...");

    enum {
        NPARAM = 24
    };
    XmFunc *func = xm_func_new("spill_only_param");
    func->num_params = NPARAM;
    XmRef ps[NPARAM];
    for (int i = 0; i < NPARAM; i++)
        ps[i] = xm_new_vreg(func, XR_REP_I64);

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmRef acc = ps[0];
    for (int i = 1; i < NPARAM; i++)
        acc = xm_emit(func, entry, XM_ADD, XR_REP_I64, acc, ps[i]);
    xm_block_set_ret(entry, acc);

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);
    XmCodegenResult res = xm_codegen_native(func, &alloc);
    assert(res.success);
    fprintf(stderr, " code=%u", res.code_size);

    int64_t buf[NPARAM];

    for (int i = 0; i < NPARAM; i++)
        buf[i] = 1;
    int64_t r1 = jit_calln(res.code, buf);
    fprintf(stderr, " ones=%lld", (long long) r1);
    assert(r1 == 24);

    for (int i = 0; i < NPARAM; i++)
        buf[i] = i + 1;
    int64_t r2 = jit_calln(res.code, buf);
    fprintf(stderr, " seq=%lld", (long long) r2);
    assert(r2 == 300);  // 1+2+...+24

    for (int i = 0; i < NPARAM; i++)
        buf[i] = (i % 2 == 0) ? -(i + 1) : (i + 1);
    int64_t r3 = jit_calln(res.code, buf);
    fprintf(stderr, " mix=%lld", (long long) r3);
    assert(r3 == 12);

    xm_code_alloc_destroy(&alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

/*
 * test_alloc_inline: XM_ALLOC with inline bump-pointer fast path
 *
 * Xm:
 *     v0 =ptr alloc const(5), const(48)  // type=5(Json), size=48
 *     ret v0
 *
 * Creates a fake coro with a minimal immix heap to test the fast path.
 */
static void test_alloc_inline(void) {
    fprintf(stderr, "  test_alloc_inline...");

    // Reuse global fake env so the JIT prologue can deref coro->jit_ctx.
    // We only need to attach a fake coro_gc + immix heap on top of it.
    static uint8_t fake_gc[512];
    // Oversized buffer to manually align to 16KB boundary.
    // The inline alloc_post writes block->local_allgc (offset 24),
    // alloc_count (40), alloc_bytes (48) — all must be within the block.
    static uint8_t heap_raw[16384 * 2];
    memset(fake_gc, 0, sizeof(fake_gc));
    memset(heap_raw, 0, sizeof(heap_raw));
    uintptr_t aligned = ((uintptr_t) heap_raw + 16383) & ~(uintptr_t) 0x3FFF;
    uint8_t *heap_buf = (uint8_t *) aligned;

    // gc->immix.cursor at offset 256 (past the block header)
    char *cursor = (char *) heap_buf + 256;
    memcpy(fake_gc + 0, &cursor, 8);
    // gc->immix.limit at offset 8
    char *limit = (char *) heap_buf + 16384;
    memcpy(fake_gc + 8, &limit, 8);
    // gc->currentwhite = 0x01 at XM_GC_CURRENTWHITE_OFFSET (109)
    fake_gc[109] = 0x01;

    jit_env_reset();
    g_jit_coro.coro_gc = (struct XrCoroGC *) fake_gc;

    XmFunc *func = xm_func_new("alloc_inline");
    func->num_params = 0;

    XmBlock *entry = xm_func_add_block(func, "entry");

    // XM_ALLOC: type=5 (Json), size=48
    XmRef type_ref = xm_const_i64(func, 5);
    XmRef size_ref = xm_const_i64(func, 48);
    XmRef v0 = xm_emit(func, entry, XM_ALLOC, XR_REP_PTR, type_ref, size_ref);
    xm_block_set_ret(entry, v0);

    XmCodeAlloc code_alloc;
    xm_code_alloc_init(&code_alloc);
    XmCodegenResult res = safe_codegen(func, &code_alloc);
    assert(res.success);
    fprintf(stderr, " code=%u", res.code_size);

    // Call directly: jit_calln would re-zero jit_ctx (we want our fake
    // coro_gc to stay attached). The fake env is already in g_jit_ctx.
    int64_t args[] = {};
    int64_t result = ((JitFn) res.code)(jit_test_coro(), args);
    fprintf(stderr, " result=%p", (void *) result);

    // Fast path should return original cursor (heap_buf + 256)
    assert(result == (int64_t) (intptr_t) cursor);

    // Verify cursor advanced by 48
    char *new_cursor;
    memcpy(&new_cursor, fake_gc + 0, 8);
    assert(new_cursor == cursor + 48);
    fprintf(stderr, " cursor_advanced=%d", (int) (new_cursor - cursor));

    // Verify GC header at result:
    uint8_t *hdr = (uint8_t *) (intptr_t) result;
    // gc_next = 0 (8 bytes at offset 0)
    int64_t gc_next = 0;
    memcpy(&gc_next, hdr, 8);
    assert(gc_next == 0);
    // type = 5 at offset 8
    assert(hdr[8] == 5);
    // marked = currentwhite (0x01) at offset 9
    assert(hdr[9] == 0x01);
    // extra = 0 at offset 10-11
    assert(hdr[10] == 0 && hdr[11] == 0);
    // objsize = 48 at offset 12
    uint32_t objsize = 0;
    memcpy(&objsize, hdr + 12, 4);
    assert(objsize == 48);
    fprintf(stderr, " header_ok");

    xm_code_alloc_destroy(&code_alloc);
    xm_func_destroy(func);
    fprintf(stderr, "\n  PASS\n");
}

int main(void) {
    xr_test_suppress_dialogs();
    signal(SIGSEGV, crash_handler);
#ifdef SIGBUS
    signal(SIGBUS, crash_handler);
#endif

    jit_env_init();

    fprintf(stderr, "=== test_jit_e2e ===\n");

    // Phase 2: basic single-block tests
    test_return_constant();
    test_add_two_params();
    test_sub();
    test_mul();
    test_add_with_const();
    test_compare();
    test_negate();
    test_bitwise();

    // Phase 3: control flow + MOD + fibonacci
    test_if_else_max();
    test_if_else_abs();
    test_mod();
    test_loop_sum();
    test_if_else_diff();
    test_fibonacci();

    // Phase 3a: safepoint
    test_loop_sum_safepoint();

    // Phase 3b: write barrier
    test_write_barrier();

    // Phase 4: optimization passes
    test_dce();
    test_const_fold();
    test_pipeline();
    test_cse();
    test_licm();

    // Phase 4b: floating-point codegen
    test_fp_basic();

    // Phase 4c: mixed-type runtime helpers
    test_rt_mixed_add();

    // Phase 4d: SelectRepresentations
    test_select_rep();

    // Phase 5: function inlining
    test_inline_basic();

    // Phase 5b: OSR entry
    test_osr_entry();

    // Phase 5c: deoptimization
    test_deopt_guard();

    // Phase 6: object field access + C calls + guards
    test_load_field();
    test_store_load_field();
    test_call_c();
    test_guard_class();
    test_call_c_const_arg();

    // Phase 8: x64/ARM64 codegen hardening regressions (docs/tasks/010 §10.1–§10.6)
    test_int_sub_chain();
    test_fp_noncommutative_chain();
    test_call_c_fp_live_across_call();
    test_binop_pressure_chain();
    test_deopt_with_spill_pressure();
    test_call_self_direct();
    test_osr_entry_pressure();
    test_spill_only_param_init();

    // Phase 7: ALLOC codegen (inline bump-pointer)
    test_alloc_inline();

    fprintf(stderr, "All tests passed!\n");
    return 0;
}
