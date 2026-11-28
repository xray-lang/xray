/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_text_call_fixture.h - Strings that reach a caller through a call
 */
#ifndef LIR_TEXT_CALL_FIXTURE_H
#define LIR_TEXT_CALL_FIXTURE_H
#include "lir_test_programs.h"

/* A function returning a new String with the decimal text of value. Its call
 * result starts no stationary owner chain, so a caller that passes it across
 * an edge exercises a real owner transfer. */
static XrLirFunction test_text_function(XrLirModule *m, const char *name, uint64_t value) {
    XrLirFunction f = test_function(m, name, NULL, 0, XR_LIR_STRING);
    XrLirBlock body = test_block(m, f, NULL, 0);
    OK(xr_lir_set_entry(m, f, body));
    XrLirValue number = test_constant(m, body, XR_LIR_I64, value), text = {0};
    XrLirInstruction create = {.op = XR_LIR_STRING_I64, .type = XR_LIR_STRING,
                               .operands = &number, .operand_count = 1};
    OK(xr_lir_emit_value(m, body, &create, &text));
    test_return(m, body, text);
    return f;
}
static XrLirValue test_text_call(XrLirModule *m, XrLirBlock block, XrLirFunction callee) {
    XrLirValue result = {0};
    XrLirInstruction instruction = {.op = XR_LIR_CALL, .type = XR_LIR_STRING, .callee = callee};
    OK(xr_lir_emit_value(m, block, &instruction, &result));
    return result;
}
#endif  // LIR_TEXT_CALL_FIXTURE_H
