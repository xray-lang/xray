/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_arm64_emit.c - Unit tests for ARM64 instruction encoding
 *
 * Verification strategy: encode instructions and compare against
 * known-good encodings from ARM reference manual / objdump.
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "../../../src/jit/xm_arm64.h"
#include "../../../src/jit/xm_code_alloc.h"
#include "../test_win_compat.h"

/* ========== Encoding Tests ========== */

static void test_add_sub(void) {
    fprintf(stderr, "  test_add_sub...");

    // ADD X0, X1, X2  â†?0x8B020020
    uint32_t inst = a64_add(A64_X0, A64_X1, A64_X2);
    assert(inst == 0x8B020020);

    // SUB X3, X4, X5  â†?0xCB050083
    inst = a64_sub(A64_X3, A64_X4, A64_X5);
    assert(inst == 0xCB050083);

    // ADD X0, X0, #1  â†?0x91000400
    inst = a64_add_imm(A64_X0, A64_X0, 1);
    assert(inst == 0x91000400);

    // SUB X0, X0, #1  â†?0xD1000400
    inst = a64_sub_imm(A64_X0, A64_X0, 1);
    assert(inst == 0xD1000400);

    fprintf(stderr, " PASS\n");
}

static void test_mul_div(void) {
    fprintf(stderr, "  test_mul_div...");

    // MUL X0, X1, X2  â†?MADD X0, X1, X2, XZR â†?0x9B027C20
    uint32_t inst = a64_mul(A64_X0, A64_X1, A64_X2);
    assert(inst == 0x9B027C20);

    // SDIV X0, X1, X2  â†?0x9AC20C20
    inst = a64_sdiv(A64_X0, A64_X1, A64_X2);
    assert(inst == 0x9AC20C20);

    fprintf(stderr, " PASS\n");
}

static void test_logic(void) {
    fprintf(stderr, "  test_logic...");

    // AND X0, X1, X2  â†?0x8A020020
    uint32_t inst = a64_and(A64_X0, A64_X1, A64_X2);
    assert(inst == 0x8A020020);

    // ORR X0, X1, X2  â†?0xAA020020
    inst = a64_orr(A64_X0, A64_X1, A64_X2);
    assert(inst == 0xAA020020);

    // EOR X0, X1, X2  â†?0xCA020020
    inst = a64_eor(A64_X0, A64_X1, A64_X2);
    assert(inst == 0xCA020020);

    // MOV X0, X1 (= ORR X0, XZR, X1) â†?0xAA0103E0
    inst = a64_mov(A64_X0, A64_X1);
    assert(inst == 0xAA0103E0);

    fprintf(stderr, " PASS\n");
}

static void test_cmp_cset(void) {
    fprintf(stderr, "  test_cmp_cset...");

    // CMP X0, X1 (= SUBS XZR, X0, X1) â†?0xEB01001F
    uint32_t inst = a64_cmp(A64_X0, A64_X1);
    assert(inst == 0xEB01001F);

    // CSET X0, LT â†?CSINC X0, XZR, XZR, GE â†?0x9A9FA7E0
    inst = a64_cset(A64_X0, A64_CC_LT);
    assert(inst == 0x9A9FA7E0);

    fprintf(stderr, " PASS\n");
}

static void test_movz_movk(void) {
    fprintf(stderr, "  test_movz_movk...");

    // MOVZ X0, #42  â†?0xD2800540
    uint32_t inst = a64_movz(A64_X0, 42, 0);
    assert(inst == 0xD2800540);

    // MOVK X0, #0x1234, LSL #16  â†?0xF2A24680
    inst = a64_movk(A64_X0, 0x1234, 16);
    assert(inst == 0xF2A24680);

    fprintf(stderr, " PASS\n");
}

static void test_branch(void) {
    fprintf(stderr, "  test_branch...");

    // RET â†?0xD65F03C0
    uint32_t inst = a64_ret();
    assert(inst == 0xD65F03C0);

    // NOP â†?0xD503201F
    inst = a64_nop();
    assert(inst == 0xD503201F);

    // B +4 (offset 1 instruction) â†?0x14000001
    inst = a64_b(1);
    assert(inst == 0x14000001);

    // B.EQ +8 (offset 2 instructions) â†?0x54000040
    inst = a64_bcond(A64_CC_EQ, 2);
    assert(inst == 0x54000040);

    fprintf(stderr, " PASS\n");
}

static void test_load_store(void) {
    fprintf(stderr, "  test_load_store...");

    // LDR X0, [X1, #0]  â†?0xF9400020
    uint32_t inst = a64_ldr(A64_X0, A64_X1, 0);
    assert(inst == 0xF9400020);

    // STR X0, [X1, #0]  â†?0xF9000020
    inst = a64_str(A64_X0, A64_X1, 0);
    assert(inst == 0xF9000020);

    fprintf(stderr, " PASS\n");
}

/* ========== Integration: Generate and Execute ========== */

#ifdef __aarch64__
static void test_emit_and_execute(void) {
    fprintf(stderr, "  test_emit_and_execute...");

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);

    // Generate: fn() -> int { return 42; }
    // MOVZ X0, #42
    // RET
    void *code = xm_code_alloc(&alloc, 64, 16);
    assert(code != NULL);

#ifdef __APPLE__
    xm_code_make_writable(code, 64);
#endif

    uint32_t *p = (uint32_t *) code;
    p[0] = a64_movz(A64_X0, 42, 0);
    p[1] = a64_ret();

    xm_code_make_executable(code, 64);
    xm_code_flush_icache(code, 64);

    typedef int64_t (*Func)(void);
    int64_t result = ((Func) code)();
    assert(result == 42);

    xm_code_alloc_destroy(&alloc);
    fprintf(stderr, " PASS\n");
}

static void test_emit_add_function(void) {
    fprintf(stderr, "  test_emit_add_function...");

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);

    // Generate: fn(a: int, b: int) -> int { return a + b; }
    // ADD X0, X0, X1
    // RET
    void *code = xm_code_alloc(&alloc, 64, 16);
    assert(code != NULL);

#ifdef __APPLE__
    xm_code_make_writable(code, 64);
#endif

    uint32_t *p = (uint32_t *) code;
    p[0] = a64_add(A64_X0, A64_X0, A64_X1);
    p[1] = a64_ret();

    xm_code_make_executable(code, 64);
    xm_code_flush_icache(code, 64);

    typedef int64_t (*AddFunc)(int64_t, int64_t);
    assert(((AddFunc) code)(10, 20) == 30);
    assert(((AddFunc) code)(100, -58) == 42);
    assert(((AddFunc) code)(0, 0) == 0);

    xm_code_alloc_destroy(&alloc);
    fprintf(stderr, " PASS\n");
}

static void test_emit_factorial_like(void) {
    fprintf(stderr, "  test_emit_factorial_like...");

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);

    // Generate: fn(n: int) -> int { result=1; while(n>0) { result*=n; n--; } return result; }
    //
    //   MOVZ X1, #1          ; result = 1
    // loop:
    //   CMP X0, #0           ; compare n with 0
    //   B.LE done            ; if n <= 0, done (offset +4)
    //   MUL X1, X1, X0       ; result *= n
    //   SUB X0, X0, #1       ; n--
    //   B loop               ; goto loop (offset -4)
    // done:
    //   MOV X0, X1           ; return result
    //   RET

    void *code = xm_code_alloc(&alloc, 128, 16);
    assert(code != NULL);

#ifdef __APPLE__
    xm_code_make_writable(code, 128);
#endif

    A64Buf buf;
    a64_buf_init(&buf, (uint32_t *) code, 32);

    a64_buf_emit(&buf, a64_movz(A64_X1, 1, 0));           // 0: result = 1
    a64_buf_emit(&buf, a64_cmp_imm(A64_X0, 0));           // 1: cmp n, 0
    a64_buf_emit(&buf, a64_bcond(A64_CC_LE, 4));          // 2: b.le done (+4 â†?pc 6)
    a64_buf_emit(&buf, a64_mul(A64_X1, A64_X1, A64_X0));  // 3: result *= n
    a64_buf_emit(&buf, a64_sub_imm(A64_X0, A64_X0, 1));   // 4: n--
    a64_buf_emit(&buf, a64_b(-4));                        // 5: b loop (-4 â†?pc 1)
    a64_buf_emit(&buf, a64_mov(A64_X0, A64_X1));          // 6: return result
    a64_buf_emit(&buf, a64_ret());                        // 7: ret

    uint32_t code_size = a64_buf_offset(&buf);
    xm_code_make_executable(code, code_size);
    xm_code_flush_icache(code, code_size);

    typedef int64_t (*FactFunc)(int64_t);
    FactFunc fact = (FactFunc) code;

    assert(fact(0) == 1);
    assert(fact(1) == 1);
    assert(fact(5) == 120);
    assert(fact(10) == 3628800);

    xm_code_alloc_destroy(&alloc);
    fprintf(stderr, " PASS\n");
}

static void test_load_imm64(void) {
    fprintf(stderr, "  test_load_imm64...");

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);

    void *code = xm_code_alloc(&alloc, 128, 16);
    assert(code != NULL);

#ifdef __APPLE__
    xm_code_make_writable(code, 128);
#endif

    A64Buf buf;
    a64_buf_init(&buf, (uint32_t *) code, 32);

    // Load 0xDEADBEEFCAFEBABE into X0, then RET
    int count = a64_load_imm64(&buf, A64_X0, 0xDEADBEEFCAFEBABEULL);
    a64_buf_emit(&buf, a64_ret());

    assert(count == 4);  // all 4 chunks are non-zero

    uint32_t code_size = a64_buf_offset(&buf);
    xm_code_make_executable(code, code_size);
    xm_code_flush_icache(code, code_size);

    typedef uint64_t (*U64Func)(void);
    uint64_t result = ((U64Func) code)();
    assert(result == 0xDEADBEEFCAFEBABEULL);

    xm_code_alloc_destroy(&alloc);
    fprintf(stderr, " PASS\n");
}
#endif  // __aarch64__

int main(void) {
    xr_test_suppress_dialogs();
    fprintf(stderr, "=== test_arm64_emit ===\n");

    // Encoding correctness tests (all platforms)
    test_add_sub();
    test_mul_div();
    test_logic();
    test_cmp_cset();
    test_movz_movk();
    test_branch();
    test_load_store();

#ifdef __aarch64__
    // Execute tests (ARM64 only)
    test_emit_and_execute();
    test_emit_add_function();
    test_emit_factorial_like();
    test_load_imm64();
#else
    fprintf(stderr, "  (skipping execution tests on non-arm64)\n");
#endif

    fprintf(stderr, "All tests passed!\n");
    return 0;
}
