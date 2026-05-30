/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_arm64_dispatch.c - Unit tests for ARM64 Xm dispatch diagnostics
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../src/jit/xm_codegen_internal.h"
#include "../test_win_compat.h"

#ifdef __aarch64__
static void emit_single(XmOp op, CodegenCtx *ctx, uint32_t *code, uint32_t cap) {
    memset(ctx, 0, sizeof(*ctx));
    a64_buf_init(&ctx->buf, code, cap);

    XmIns ins;
    memset(&ins, 0, sizeof(ins));
    ins.op = op;
    ins.dst = XM_NONE;
    ins.args[0] = XM_NONE;
    ins.args[1] = XM_NONE;

    a64_emit_xm_ins(ctx, &ins);
}

static void test_nop_emits_declared_instruction(void) {
    fprintf(stderr, "  test_nop_emits_declared_instruction...");

    uint32_t code[4] = {0};
    CodegenCtx ctx;
    emit_single(XM_NOP, &ctx, code, 4);

    assert(!ctx.had_error);
    assert(ctx.buf.count == 1);
    assert(code[0] == a64_nop());

    fprintf(stderr, " PASS\n");
}

static void test_marker_zero_emit_is_allowed(void) {
    fprintf(stderr, "  test_marker_zero_emit_is_allowed...");

    uint32_t code[4] = {0};
    CodegenCtx ctx;
    emit_single(XM_PHI, &ctx, code, 4);

    assert(!ctx.had_error);
    assert(ctx.buf.count == 0);

    fprintf(stderr, " PASS\n");
}
#endif

int main(void) {
    xr_test_suppress_dialogs();
    fprintf(stderr, "=== test_arm64_dispatch ===\n");

#ifdef __aarch64__
    test_nop_emits_declared_instruction();
    test_marker_zero_emit_is_allowed();
#else
    fprintf(stderr, "  (skipping ARM64 dispatch tests on non-arm64)\n");
#endif

    fprintf(stderr, "All tests passed!\n");
    return 0;
}
