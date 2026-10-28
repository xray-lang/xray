/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_register_vm.c - Scalar bytecode behavior and rejection tests.
 *
 * KEY CONCEPT:
 *   Independent expected values exercise wrapping arithmetic, control flow,
 *   native calls and recursive VM calls on both dispatch implementations.
 */

#include "register_vm_programs.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CHECK(condition) XR_CHECK(condition, #condition)

static XrRbcStatus execute(const XrRbcModule *module, uint32_t entry,
    const uint64_t *arguments, size_t count, uint64_t *bits) {
    XrtLocationOwner *locations = NULL;
    CHECK(xrt_locations_create(NULL, 0, &locations) == XRT_LOCATION_OK);
    XrRbcExecution *execution = NULL;
    XrRbcStatus status = xr_rbc_module_prepare(module, entry, arguments, count, locations, &execution);
    xrt_locations_release(locations);
    if (status != XR_RBC_OK) return status;
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    CHECK(outcome);
    status = xr_rbc_outcome_status(outcome);
    xr_rbc_outcome_bits(outcome, bits);
    xr_rbc_execution_free(&execution);
    xr_rbc_outcome_free(&outcome);
    return status;
}

static void reject_prepare(const XrRbcModule *module, uint32_t entry,
    const uint64_t *arguments, size_t count) {
    XrtLocationOwner *owner = NULL;
    CHECK(xrt_locations_create(NULL, 0, &owner) == XRT_LOCATION_OK);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_module_prepare(module, entry, arguments, count, owner, &execution) == XR_RBC_INVALID);
    CHECK(!execution);
    xrt_locations_release(owner);
}

static void test_encoding(void) {
    uint32_t word = 0;
    CHECK(sizeof(word) == 4 && sizeof(uint64_t) == 8);
    CHECK(xr_rbc_encode(&word, (XrRbcOperands) {XR_RBC_ADD, 254, 253, 252, 0}));
    CHECK(word == (uint32_t) XR_RBC_ADD + (254u << 8) + (253u << 16) + (252u << 24));
    CHECK(xr_rbc_encode(&word, (XrRbcOperands) {XR_RBC_LOADK, 254, 0, 0, UINT16_MAX}));
    CHECK((word >> 16) == UINT16_MAX);
    CHECK(xr_rbc_encode(&word, (XrRbcOperands) {XR_RBC_LOADI, 0, 0, 0, INT16_MIN}));
    CHECK(xr_rbc_encode(&word, (XrRbcOperands) {XR_RBC_LOADI, 0, 0, 0, INT16_MAX}));
    CHECK(xr_rbc_encode(&word, (XrRbcOperands) {XR_RBC_JMP, 0, 0, 0, XR_RBC_JUMP_MIN}));
    CHECK(xr_rbc_encode(&word, (XrRbcOperands) {XR_RBC_JMP, 0, 0, 0, XR_RBC_JUMP_MAX}));
    XrRbcOperands rejected[] = {
        {XR_RBC_ADD, 256, 0, 0, 0},
        {XR_RBC_ADD, 0, 255, 0, 0},
        {XR_RBC_ADD, 0, 0, UINT32_MAX, 0},
        {XR_RBC_OP_COUNT, 0, 0, 0, 0},
        {XR_RBC_LOADK, 0, 0, 0, 65536},
        {XR_RBC_LOADK, 0, 0, 0, -1},
        {XR_RBC_LOADI, 0, 0, 0, 32768},
        {XR_RBC_LOADI, 0, 0, 0, -32769},
        {XR_RBC_JMP, 0, 0, 0, XR_RBC_JUMP_MAX + 1},
        {XR_RBC_JMP, 0, 0, 0, XR_RBC_JUMP_MIN - 1},
        {XR_RBC_RET0, 1, 0, 0, 0},
        {XR_RBC_MOV, 0, 0, 1, 0},
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        word = UINT32_C(0xdeadbeef);
        CHECK(!xr_rbc_encode(&word, rejected[i]));
        CHECK(word == UINT32_C(0xdeadbeef));
    }
    CHECK(!xr_rbc_encode(NULL, (XrRbcOperands) {XR_RBC_RET0, 0, 0, 0, 0}));
    CHECK(!xr_rbc_jump(&word, 0, 2, 2));
    CHECK(!xr_rbc_jump(&word, 2, 0, 2));
    CHECK(!xr_rbc_jump(&word, 0, SIZE_MAX - 1, SIZE_MAX));
    CHECK(!xr_rbc_jump(&word, SIZE_MAX - 1, 0, SIZE_MAX));
    CHECK(xr_rbc_jump(&word, 0, 8388608, 8388609));
    CHECK(xr_rbc_jump(&word, 8388607, 0, 8388608));
}

static uint64_t run_function(XrRbcFunction function, const uint64_t *args, size_t count) {
    XrRbcModule module = {&function, 1, NULL, 0, NULL, 0};
    uint64_t result = UINT64_MAX;
    CHECK(execute(&module, 0, args, count, &result) == XR_RBC_OK);
    return result;
}

static void test_workloads(bool timing) {
    uint32_t fib_code[13], loop_code[10], countdown_code[11];
    XrRbcFunction fib = make_fib(fib_code), loop = make_loop(loop_code);
    const uint64_t inputs[] = {0, 1, 2, 10, 20, 32};
    const uint64_t expected[] = {0, 1, 1, 55, 6765, 2178309};
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        clock_t start = clock();
        CHECK(run_function(fib, inputs + i, 1) == expected[i]);
        if (timing && inputs[i] == 32)
            printf("fib(32),%s,%.3f ms (coarse)\n", xr_rbc_dispatch_name(),
                   (double) (clock() - start) * 1000.0 / CLOCKS_PER_SEC);
    }
    uint64_t count = 1000000;
    CHECK(run_function(loop, &count, 1) == UINT64_C(499999500000));
    count = 0;
    CHECK(run_function(loop, &count, 1) == 0);
    count = 10000;
    CHECK(run_function(make_countdown(countdown_code), &count, 1) == count);
}

static void test_arithmetic(void) {
    const struct {
        XrRbcOp op;
        uint64_t lhs, rhs, result;
    } cases[] = {
        {XR_RBC_ADD, INT64_MAX, 1, (uint64_t) INT64_MIN},
        {XR_RBC_SUB, (uint64_t) INT64_MIN, 1, INT64_MAX},
        {XR_RBC_MUL, UINT64_MAX, 2, UINT64_MAX - 1},
        {XR_RBC_DIV, (uint64_t) INT64_MIN, UINT64_MAX, (uint64_t) INT64_MIN},
        {XR_RBC_REM, (uint64_t) INT64_MIN, UINT64_MAX, 0},
        {XR_RBC_DIV, (uint64_t) -17, 5, (uint64_t) -3},
        {XR_RBC_REM, (uint64_t) -17, 5, (uint64_t) -2},
        {XR_RBC_AND, 0xf0, 0x3c, 0x30},
        {XR_RBC_OR, 0xf0, 0x3c, 0xfc},
        {XR_RBC_XOR, 0xf0, 0x3c, 0xcc},
        {XR_RBC_SHL, UINT64_MAX, 65, UINT64_MAX - 1},
        {XR_RBC_SHR, UINT64_C(0x8000000000000000), 63, UINT64_MAX},
        {XR_RBC_SHR, UINT64_MAX, 64, UINT64_MAX},
        {XR_RBC_USHR, UINT64_MAX, 63, 1},
        {XR_RBC_USHR, UINT64_MAX, 64, UINT64_MAX},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint32_t code[2];
        uint64_t args[2] = {cases[i].lhs, cases[i].rhs};
        emit(code, 0, cases[i].op, 0, 0, 1);
        emit(code, 1, XR_RBC_RET, 0, 0, 0);
        CHECK(run_function((XrRbcFunction) {code, 2, NULL, 0, 2, 2, NULL, 0, 0}, args, 2) == cases[i].result);
    }
    uint32_t code[2];
    uint64_t arg = (uint64_t) INT64_MIN;
    emit(code, 0, XR_RBC_NEG, 0, 0, 0);
    emit(code, 1, XR_RBC_RET, 0, 0, 0);
    CHECK(run_function((XrRbcFunction) {code, 2, NULL, 0, 1, 1, NULL, 0, 0}, &arg, 1) == arg);
    immediate(code, 0, XR_RBC_LOADI, 0, INT16_MIN);
    CHECK(run_function((XrRbcFunction) {code, 2, NULL, 0, 1, 0, NULL, 0, 0}, NULL, 0) == (uint64_t) INT16_MIN);
}

static void test_branches(void) {
    for (int operation = XR_RBC_LT; operation <= XR_RBC_EQ; ++operation) {
        for (int64_t lhs = -1; lhs <= 1; ++lhs) {
            uint32_t code[6];
            uint64_t args[2] = {(uint64_t) lhs, 0};
            emit(code, 0, (XrRbcOp) operation, 0, 1, 0);
            CHECK(xr_rbc_jump(code + 1, 1, 4, 6));
            immediate(code, 2, XR_RBC_LOADI, 0, 0);
            emit(code, 3, XR_RBC_RET, 0, 0, 0);
            immediate(code, 4, XR_RBC_LOADI, 0, 1);
            emit(code, 5, XR_RBC_RET, 0, 0, 0);
            bool expected = operation == XR_RBC_LT   ? lhs < 0
                            : operation == XR_RBC_LE ? lhs <= 0
                                                     : lhs == 0;
            CHECK(run_function((XrRbcFunction) {code, 6, NULL, 0, 2, 2, NULL, 0, 0}, args, 2) ==
                  (uint64_t) expected);
        }
    }
}

static void test_call_windows(void) {
    uint32_t caller[7], callee[3];
    // A multi-argument call overlaps only the caller's two top slots.
    immediate(caller, 0, XR_RBC_LOADI, 0, 1000);
    immediate(caller, 1, XR_RBC_LOADI, 2, 17);
    immediate(caller, 2, XR_RBC_LOADI, 3, 25);
    immediate(caller, 3, XR_RBC_CALL, 1, 1);
    emit(caller, 4, XR_RBC_ADD, 0, 0, 1);
    emit(caller, 5, XR_RBC_RET, 0, 0, 0);
    emit(caller, 6, XR_RBC_RET0, 0, 0, 0);
    emit(callee, 0, XR_RBC_ADD, 2, 0, 1);
    immediate(callee, 1, XR_RBC_LOADI, 254, -10);
    emit(callee, 2, XR_RBC_RET, 2, 0, 0);
    XrRbcFunction functions[] = {{caller, 7, NULL, 0, 4, 0, NULL, 0, 0}, {callee, 3, NULL, 0, 255, 2, NULL, 0, 0}};
    XrRbcModule module = {functions, 2, NULL, 0, NULL, 0};
    uint64_t result = 0;
    CHECK(execute(&module, 0, NULL, 0, &result) == XR_RBC_OK && result == 1042);
    // Zero arguments place the callee just beyond the last caller register.
    functions[0].register_count = 2;
    functions[1].parameter_count = 0;
    immediate(caller, 1, XR_RBC_CALL, 1, 1);
    emit(caller, 2, XR_RBC_ADD, 0, 0, 1);
    emit(caller, 3, XR_RBC_RET, 0, 0, 0);
    immediate(callee, 0, XR_RBC_LOADI, 254, 42);
    emit(callee, 1, XR_RBC_RET, 254, 0, 0);
    CHECK(execute(&module, 0, NULL, 0, &result) == XR_RBC_OK && result == 1042);
}

static void test_rejections(void) {
    uint32_t code[2];
    uint64_t args[2] = {1, 0}, result = 42;
    emit(code, 0, XR_RBC_DIV, 0, 0, 1);
    emit(code, 1, XR_RBC_RET, 0, 0, 0);
    XrRbcFunction function = {code, 2, NULL, 0, 2, 2, NULL, 0, 0};
    XrRbcModule module = {&function, 1, NULL, 0, NULL, 0};
    CHECK(execute(&module, 0, args, 2, &result) == XR_RBC_ARITHMETIC_PANIC);
    CHECK(result == 42);
    reject_prepare(&module, 1, args, 2);
    reject_prepare(&module, 0, NULL, 2);
    reject_prepare(&module, 0, args, 1);
    function.register_count = 256;
    reject_prepare(&module, 0, args, 2);
    function.register_count = 2;
    immediate(code, 0, XR_RBC_JMP, 0, -2);
    reject_prepare(&module, 0, args, 2);
    immediate(code, 0, XR_RBC_JMP, 0, 1);
    reject_prepare(&module, 0, args, 2);
    immediate(code, 0, XR_RBC_CALL, 0, 99);
    reject_prepare(&module, 0, args, 2);
    immediate(code, 0, XR_RBC_CALL, 0, 0);
    reject_prepare(&module, 0, args, 2);
    immediate(code, 0, XR_RBC_CALLN, 0, 0);
    reject_prepare(&module, 0, args, 2);
    immediate(code, 0, XR_RBC_LOADK, 0, 0);
    reject_prepare(&module, 0, args, 2);
    emit(code, 0, XR_RBC_MOV, 0, 2, 0);
    reject_prepare(&module, 0, args, 2);
    emit(code, 0, XR_RBC_LT, 0, 1, 0);
    reject_prepare(&module, 0, args, 2);
    code[0] = 255;
    reject_prepare(&module, 0, args, 2);
    emit(code, 0, XR_RBC_RET0, 0, 0, 0);
    CHECK(execute(&module, 0, args, 2, &result) == XR_RBC_OK && result == 0);
}

static void test_prepare_unreachable(void) {
    uint32_t root = XR_RBC_RET0, code[] = {XR_RBC_RET0, XR_RBC_RET0, XR_RBC_RET0, XR_RBC_RET0};
    uint64_t constant = 42;
    XrRbcFunction functions[] = {
        {&root, 1, NULL, 0, 1, 0, NULL, 0, 0},
        {code, 4, &constant, 1, 3, 0, NULL, 0, 0}};
    const XrRbcNativeI64 native = xrt_print_i64;
    XrRbcModule module = {functions, 2, &native, 1, NULL, 0};
    const uint32_t invalid[] = {
        255, XR_RBC_MOV | (3u << 8), XR_RBC_MOV | (3u << 16), XR_RBC_MOV | (1u << 24),
        XR_RBC_ADD | (3u << 8), XR_RBC_ADD | (3u << 16), XR_RBC_ADD | (3u << 24),
        XR_RBC_NEG | (3u << 16), XR_RBC_LOADI | (3u << 8), XR_RBC_LOADK | (1u << 16),
        XR_RBC_CALL | (2u << 8) | (2u << 16), XR_RBC_CALL | (1u << 16),
        XR_RBC_CALLN | (1u << 8), XR_RBC_CALLN | (1u << 16),
        XR_RBC_RET | (3u << 8), XR_RBC_RET | (1u << 16), XR_RBC_RET0 | (1u << 8),
        XR_RBC_JMP | (UINT32_C(0xfffffe) << 8), XR_RBC_JMP | (3u << 8),
        XR_RBC_LT | (3u << 8), XR_RBC_LE | (3u << 16), XR_RBC_EQ | (1u << 24)};
    // The root never calls this function. Every malformed instruction must
    // still fail at preparation, without entering the dispatch loop.
    for (size_t n = 0; n < sizeof(invalid) / sizeof(invalid[0]); ++n) {
        code[0] = invalid[n];
        reject_prepare(&module, 0, NULL, 0);
    }
    for (uint32_t op = XR_RBC_LT; op <= XR_RBC_EQ; ++op) {
        code[0] = op;
        reject_prepare(&module, 0, NULL, 0); // The next word is not JMP.
        code[0] = XR_RBC_RET0;
        code[2] = op;
        immediate(code, 3, XR_RBC_JMP, 0, -4);
        reject_prepare(&module, 0, NULL, 0); // False continuation falls off.
        code[2] = code[3] = XR_RBC_RET0;
    }
    immediate(code, 3, XR_RBC_LOADI, 0, 0);
    reject_prepare(&module, 0, NULL, 0); // Unreachable ordinary fallthrough.
    code[3] = XR_RBC_RET0;
    XrRbcFunction valid = functions[1];
    for (unsigned kind = 0; kind < 6; ++kind) {
        functions[1] = valid;
        if (kind == 0) functions[1].code = NULL;
        if (kind == 1) functions[1].code_count = 0;
        if (kind == 2) functions[1].register_count = 0;
        if (kind == 3) functions[1].register_count = 256;
        if (kind == 4) functions[1].parameter_count = 4;
        if (kind == 5) functions[1].constants = NULL;
        reject_prepare(&module, 0, NULL, 0);
    }
    functions[1] = valid;
    XrtLocationOwner *owner = NULL;
    CHECK(xrt_locations_create(NULL, 0, &owner) == XRT_LOCATION_OK);
    XrRbcExecution *execution = NULL;
    immediate(code, 3, XR_RBC_JMP, 0, -4);
    CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0, owner, &execution) == XR_RBC_OK);
    xr_rbc_execution_free(&execution); // A valid unreachable loop is admitted.
    code[0] = XR_RBC_LT;
    immediate(code, 1, XR_RBC_JMP, 0, 1);
    CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0, owner, &execution) == XR_RBC_OK);
    xr_rbc_execution_free(&execution);
    xrt_locations_release(owner);
}

static void test_native_print(void) {
    uint32_t code[4];
    const uint64_t constants[] = {(uint64_t) INT64_MIN};
    immediate(code, 0, XR_RBC_LOADK, 253, 0);
    immediate(code, 1, XR_RBC_LOADI, 254, 0);
    immediate(code, 2, XR_RBC_CALLN, 252, 0);
    emit(code, 3, XR_RBC_RET0, 0, 0, 0);
    XrRbcFunction function = {code, 4, constants, 1, 255, 0, NULL, 0, 0};
    const XrRbcNativeI64 natives[] = {xrt_print_i64};
    XrRbcModule module = {&function, 1, natives, 1, NULL, 0};
    uint64_t result;
    CHECK(execute(&module, 0, NULL, 0, &result) == XR_RBC_OK);
}

static void unregistered(int64_t value, uint32_t id) {
    (void) value;
    (void) id;
    CHECK(false);
}

static void test_owned_execution(void) {
    uint32_t code[2];
    emit(code, 0, XR_RBC_RET, 0, 0, 0);
    XrRbcFunction function = {code, 1, NULL, 0, 1, 1, NULL, 0, 0};
    XrRbcModule module = {&function, 1, NULL, 0, NULL, 0};
    XrtLocationOwner *owner = NULL;
    CHECK(xrt_locations_create(NULL, 0, &owner) == XRT_LOCATION_OK);
    uint64_t argument = UINT64_C(0x7ff0000000000001), bits = 123;
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_module_prepare(&module, 0, &argument, 1, owner, &execution) == XR_RBC_OK);
    XrRbcExecution *same = execution;
    CHECK(xr_rbc_module_prepare(&module, 0, &argument, 1, owner, &execution) == XR_RBC_INVALID);
    CHECK(execution == same);
    argument = 0;
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    CHECK(outcome && !xr_rbc_execution_run(execution) && !xr_rbc_execution_run(NULL));
    xr_rbc_execution_free(&execution);
    CHECK(!execution && xr_rbc_outcome_bits(outcome, &bits) && bits == UINT64_C(0x7ff0000000000001));
    XrtOutputFailure facts = {{true, true, XRT_IO_ERRNO, 27}, XRT_OUTPUT_WRITE, 77};
    const XrtLocationOwner *borrowed = owner;
    CHECK(!xr_rbc_outcome_output_failure(outcome, &facts, &borrowed));
    CHECK(facts.source_id == 77 && facts.io.code == 27 && facts.io.domain == XRT_IO_ERRNO &&
        facts.io.failed && facts.io.broken_pipe && facts.operation == XRT_OUTPUT_WRITE && borrowed == owner);
    CHECK(!xr_rbc_outcome_output_failure(NULL, &facts, &borrowed));
    CHECK(facts.source_id == 77 && borrowed == owner);
    XrtArithmeticPanic panic = {99, 88};
    CHECK(!xr_rbc_outcome_arithmetic_panic(outcome, &panic, &borrowed));
    CHECK(panic.code == 99 && panic.source_id == 88 && borrowed == owner);
    CHECK(!xr_rbc_outcome_arithmetic_panic(NULL, &panic, &borrowed));
    CHECK(panic.code == 99 && panic.source_id == 88 && borrowed == owner);
    xr_rbc_outcome_free(&outcome);
    CHECK(!outcome && !xr_rbc_outcome_bits(NULL, &bits));
    CHECK(bits == UINT64_C(0x7ff0000000000001));
    CHECK(xr_rbc_module_prepare(&module, 0, &argument, 1, owner, &execution) == XR_RBC_OK);
    xr_rbc_execution_free(&execution); // Unrun preparation owns and frees its pending result.
    CHECK(!execution);
    const XrRbcNativeI64 native[] = {unregistered};
    module.natives = native;
    module.native_count = 1;
    CHECK(xr_rbc_module_prepare(&module, 0, &argument, 1, owner, &execution) == XR_RBC_INVALID);
    CHECK(!execution);
    const XrRbcNativeI64 actual[] = {xrt_print_i64};
    module.natives = actual;
    function.parameter_count = function.register_count = 3;
    function.code_count = 2;
    immediate(code, 0, XR_RBC_CALLN, 0, 0);
    emit(code, 1, XR_RBC_RET0, 0, 0, 0);
    uint64_t args[] = {0, 55, UINT64_C(1) << 32};
    CHECK(xr_rbc_module_prepare(&module, 0, args, 3, owner, &execution) == XR_RBC_OK);
    outcome = xr_rbc_execution_run(execution);
    CHECK(outcome && xr_rbc_outcome_status(outcome) == XR_RBC_INVALID);
    panic = (XrtArithmeticPanic) {88, 77};
    borrowed = owner;
    CHECK(!xr_rbc_outcome_arithmetic_panic(outcome, &panic, &borrowed));
    CHECK(panic.code == 88 && panic.source_id == 77 && borrowed == owner);
    xr_rbc_execution_free(&execution);
    xr_rbc_outcome_free(&outcome);
    args[2] = 1; // Nonzero ID is invalid for a genuine empty image, never unknown.
    CHECK(execute(&module, 0, args, 3, &bits) == XR_RBC_INVALID);
    function.register_count = function.parameter_count = 2;
    CHECK(execute(&module, 0, args, 2, &bits) == XR_RBC_INVALID);
    function.register_count = 255;
    immediate(code, 0, XR_RBC_CALLN, 253, 0);
    CHECK(execute(&module, 0, args, 2, &bits) == XR_RBC_INVALID);
    xrt_locations_release(owner);
}

static void test_unknown_arithmetic_panic(void) {
    uint32_t code[2];
    emit(code, 1, XR_RBC_RET, 0, 0, 0);
    XrRbcFunction function = {code, 2, NULL, 0, 2, 2, NULL, 0, 0};
    XrRbcModule module = {&function, 1, NULL, 0, NULL, 0};
    uint64_t arguments[] = {42, 0};
    for (unsigned op = 0; op < 2; ++op) {
        emit(code, 0, op ? XR_RBC_REM : XR_RBC_DIV, 0, 0, 1);
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_module_prepare(&module, 0, arguments, 2, NULL, &execution) == XR_RBC_INVALID);
        CHECK(!execution);
        XrtLocationOwner *owner = NULL;
        CHECK(xrt_locations_create(NULL, 0, &owner) == XRT_LOCATION_OK && owner);
        CHECK(xr_rbc_module_prepare(&module, 0, arguments, 2, owner, &execution) == XR_RBC_OK);
        xrt_locations_release(owner);
        XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
        xr_rbc_execution_free(&execution);
        XrtArithmeticPanic facts;
        const XrtLocationOwner *borrowed = NULL;
        CHECK(xr_rbc_outcome_arithmetic_panic(outcome, &facts, &borrowed));
        CHECK(facts.code == (op ? 421u : 420u) && facts.source_id == 0 && borrowed);
        CHECK(!xrt_locations_count(borrowed) && !xrt_locations_get(borrowed, 0));
        xr_rbc_outcome_free(&outcome);
    }
}

static void test_source_maps(void) {
    uint32_t caller[5], callee[2], ids[5] = {0}, callee_ids[] = {1, 0}, bad_ids[] = {2, 0};
    immediate(caller, 0, XR_RBC_LOADI, 1, 17);
    immediate(caller, 1, XR_RBC_LOADI, 2, 0);
    immediate(caller, 2, XR_RBC_CALLN, 0, 0);
    immediate(caller, 3, XR_RBC_CALL, 0, 1);
    emit(caller, 4, XR_RBC_RET, 0, 0, 0);
    emit(callee, 0, XR_RBC_DIV, 0, 0, 1);
    emit(callee, 1, XR_RBC_RET, 0, 0, 0);
    XrRbcFunction functions[] = {
        {caller, 5, NULL, 0, 3, 0, ids, 5, 0},
        {callee, 2, NULL, 0, 2, 2, callee_ids, 2, 0},
        {callee, 2, NULL, 0, 2, 2, callee_ids, 2, 0}};
    XrRbcNativeI64 native = xrt_print_i64;
    XrRbcModule module = {functions, 3, &native, 1, NULL, 0};
    XrtSourceLocation location = {"raw.xr", 6, 7, 3, 7, 4};
    XrtLocationOwner *owner = NULL;
    CHECK(xrt_locations_create(&location, 1, &owner) == XRT_LOCATION_OK);
    // A print precedes CALL; even an unreachable callee's bad metadata must reject preparation.
    for (size_t f = 0; f < 3; ++f) {
        XrRbcFunction original = functions[f];
        for (unsigned kind = 0; kind < 4; ++kind) {
            functions[f].source_ids = kind == 0 ? NULL : kind == 3 ? bad_ids : callee_ids;
            functions[f].source_id_count = kind == 0 ? 1 : kind == 1 ? 0 : kind == 2 ? 1 : 2;
            // For the root, ensure the ID test has a full map rather than a shape error.
            uint32_t full_bad[] = {0, 0, 0, 0, 2};
            if (kind == 3 && !f) {
                functions[f].source_ids = full_bad;
                functions[f].source_id_count = 5;
            }
            XrRbcExecution *execution = NULL;
            CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0, owner, &execution) == XR_RBC_INVALID);
            CHECK(!execution);
        }
        functions[f] = original;
    }
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0, owner, &execution) == XR_RBC_OK);
    xr_rbc_execution_free(&execution);
    uint32_t invalid = XR_RBC_OP_COUNT;
    functions[0] = (XrRbcFunction) {&invalid, 1, NULL, 0, 1, 0, NULL, 0, 0};
    module.function_count = 1;
    CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0, owner, &execution) == XR_RBC_INVALID);
    CHECK(!execution);
    xrt_locations_release(owner);
}


static void test_cells(void) {
    uint32_t code[7];
    immediate(code, 0, XR_RBC_LOADI, 0, 7);
    immediate(code, 1, XR_RBC_CELL_NEW, 0, 0);
    emit(code, 2, XR_RBC_MOV, 1, 0, 0);
    immediate(code, 3, XR_RBC_LOADI, 2, 42);
    emit(code, 4, XR_RBC_CELL_WRITE, 1, 2, 0);
    emit(code, 5, XR_RBC_CELL_READ, 2, 0, 0);
    emit(code, 6, XR_RBC_RET, 2, 0, 0);
    XrRbcFunction function = {code, 7, NULL, 0, 3, 0, NULL, 0, 1};
    CHECK(run_function(function, NULL, 0) == 42);
    emit(code, 4, XR_RBC_CELL_READ, 2, 1, 0);
    CHECK(run_function(function, NULL, 0) == 7);
    CHECK(run_function(function, NULL, 0) == 7);
    XrRbcModule module = {&function, 1, NULL, 0, NULL, 0};
    uint64_t bits = 999;
    function.cell_count = 2;
    immediate(code, 2, XR_RBC_LOADI, 1, 2);
    CHECK(execute(&module, 0, NULL, 0, &bits) == XR_RBC_INVALID && bits == 999);
    emit(code, 2, XR_RBC_MOV, 1, 0, 0);
    function.cell_count = 1;
    // Capacity alone cannot initialize a cell. Nor can an earlier retired
    // activation authorize a later read of its numeric index.
    immediate(code, 1, XR_RBC_LOADI, 0, 1);
    CHECK(execute(&module, 0, NULL, 0, &bits) == XR_RBC_INVALID && bits == 999);
    const int64_t handles[] = {0, 2, -1};
    for (size_t i = 0; i < sizeof(handles) / sizeof(handles[0]); ++i) {
        immediate(code, 0, XR_RBC_LOADI, 0, handles[i]);
        emit(code, 1, XR_RBC_MOV, 1, 0, 0);
        CHECK(execute(&module, 0, NULL, 0, &bits) == XR_RBC_INVALID && bits == 999);
    }
    immediate(code, 0, XR_RBC_LOADI, 0, 7);
    immediate(code, 1, XR_RBC_CELL_NEW, 0, 1);
    reject_prepare(&module, 0, NULL, 0); // Ordinal is outside its descriptor.
    function.cell_count = 2;
    CHECK(execute(&module, 0, NULL, 0, &bits) == XR_RBC_INVALID && bits == 999);
    function.cell_count = 65537;
    reject_prepare(&module, 0, NULL, 0);
    uint32_t parent[5], child[3];
    immediate(parent, 0, XR_RBC_LOADI, 0, 7);
    immediate(parent, 1, XR_RBC_CELL_NEW, 0, 0);
    immediate(parent, 2, XR_RBC_CALL, 2, 1);
    emit(parent, 3, XR_RBC_CELL_READ, 1, 2, 0);
    emit(parent, 4, XR_RBC_RET, 1, 0, 0);
    immediate(child, 0, XR_RBC_LOADI, 0, 17);
    immediate(child, 1, XR_RBC_CELL_NEW, 0, 0);
    emit(child, 2, XR_RBC_RET, 0, 0, 0);
    XrRbcFunction functions[] = {{parent, 5, NULL, 0, 3, 0, NULL, 0, 1},
        {child, 3, NULL, 0, 1, 0, NULL, 0, 1}};
    module = (XrRbcModule) {functions, 2, NULL, 0, NULL, 0};
    CHECK(execute(&module, 0, NULL, 0, &bits) == XR_RBC_INVALID && bits == 999);
    uint32_t word = 919;
    CHECK(xr_rbc_encode(&word, (XrRbcOperands){XR_RBC_CELL_NEW, 254, 0, 0, UINT16_MAX}));
    CHECK(!xr_rbc_encode(&word, (XrRbcOperands){XR_RBC_CELL_NEW, 0, 0, 0, 65536}));
    CHECK(!xr_rbc_encode(&word, (XrRbcOperands){XR_RBC_CELL_READ, 0, 0, 1, 0}));
}


static void test_literal_validation(void) {
    uint32_t code[3];
    immediate(code, 0, XR_RBC_PRINT_LITERAL, 0, 0);
    emit(code, 1, XR_RBC_RET0, 0, 0, 0);
    emit(code, 2, XR_RBC_RET0, 0, 0, 0);
    XrRbcFunction functions[] = {{code + 2, 1, NULL, 0, 1, 0, NULL, 0, 0},
        {code, 2, NULL, 0, 1, 0, NULL, 0, 0}};
    XrRbcLiteral literal = {"A\0\xc3\xa9", 4};
    XrRbcModule module = {functions, 2, NULL, 0, &literal, 1};
    uint64_t bits = 99;
    CHECK(execute(&module, 0, NULL, 0, &bits) == XR_RBC_OK && !bits);
    // Even an unreachable literal instruction is checked before publication.
    immediate(code, 0, XR_RBC_PRINT_LITERAL, 0, 1);
    reject_prepare(&module, 0, NULL, 0);
    immediate(code, 0, XR_RBC_PRINT_LITERAL, 1, 0);
    reject_prepare(&module, 0, NULL, 0);
    immediate(code, 0, XR_RBC_PRINT_LITERAL, 0, 0);
    module.literals = NULL;
    reject_prepare(&module, 0, NULL, 0);
    module.literals = &literal;module.literal_count = 0;
    reject_prepare(&module, 0, NULL, 0);
    module.literal_count = 65537;
    reject_prepare(&module, 0, NULL, 0);
    module.literal_count = 1;literal.bytes = NULL;
    reject_prepare(&module, 0, NULL, 0);
    literal.length = 0;
    CHECK(execute(&module, 0, NULL, 0, &bits) == XR_RBC_OK && !bits);
    literal.bytes = "x";literal.length = SIZE_MAX;
    reject_prepare(&module, 0, NULL, 0);
    uint32_t word = 719;
    CHECK(xr_rbc_encode(&word, (XrRbcOperands){XR_RBC_PRINT_LITERAL, 0, 0, 0, UINT16_MAX}));
    uint32_t saved = word;
    CHECK(!xr_rbc_encode(&word, (XrRbcOperands){XR_RBC_PRINT_LITERAL, 0, 0, 0, 65536}));
    CHECK(word == saved);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--native-print") == 0) {
        test_native_print();
        return 0;
    }
    test_literal_validation();
    test_cells();
    test_encoding();
    test_arithmetic();
    test_branches();
    test_call_windows();
    test_rejections();
    test_prepare_unreachable();
    test_owned_execution();
    test_source_maps();
    test_unknown_arithmetic_panic();
    test_workloads(argc == 2 && strcmp(argv[1], "--timing") == 0);
    printf("register_vm: passed (%s)\n", xr_rbc_dispatch_name());
    return 0;
}
