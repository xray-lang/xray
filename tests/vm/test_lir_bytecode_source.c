/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_bytecode_source.c - Actual Source lowering consumed by bytecode
 */
#include "../lir/lir_source_fixture.h"
#include "lir/xlir_lower.h"
#include "lir/xlir_verify.h"
#include "vm/register/xrbc_lir.h"

static XrLirLowered lower(const char *text) {
    SourceFixture source = source_fixture(text);
    XrLirLowerRequest request = {source.closed, source.source.snapshot, false};
    XrLirLowered result = {0};
    XrLirLowerFailure failure = {0};
    XrLirStatus status = xr_lir_lower_checked(&request, &result, &failure);
    if (status != XR_LIR_OK) fprintf(stderr, "lower: %s\n", failure.diagnostic.message);
    CHECK(status == XR_LIR_OK);
    source_fixture_free(&source);
    xr_lir_lower_failure_free(&failure);
    OK(xr_lir_verify(result.module, NULL));
    return result;
}

static XrLirFunction function_id(const XrLirModule *module, uint32_t id) {
    for (uint32_t i = 0; i < xr_lir_function_count(module); ++i) {
        XrLirFunction function;
        OK(xr_lir_function_at(module, i, &function));
        if (function.id == id) return function;
    }
    CHECK(false);
    return (XrLirFunction){0};
}

// Follow the returned entry's initializer call, then its one ordinary call.
// No synthetic display name is used as a source-language entry convention.
static XrLirFunction initialized_function(const XrLirLowered *lowered) {
    XrLirBlock block;
    XrLirInstructionView instruction;
    OK(xr_lir_block_at(lowered->module, lowered->entry, 0, &block));
    OK(xr_lir_instruction_view(lowered->module, block, 0, &instruction));
    CHECK(instruction.op == XR_LIR_CALL);
    XrLirFunction initializer = function_id(lowered->module, instruction.callee);
    XrLirFunctionView view;
    OK(xr_lir_function_view(lowered->module, initializer, &view));
    XrLirFunction selected = {0};
    for (uint32_t b = 0; b < view.block_count; ++b) {
        XrLirBlockView bv;
        OK(xr_lir_block_at(lowered->module, initializer, b, &block));
        OK(xr_lir_block_view(lowered->module, block, &bv));
        for (uint32_t i = 0; i < bv.instruction_count; ++i) {
            OK(xr_lir_instruction_view(lowered->module, block, i, &instruction));
            if (instruction.op != XR_LIR_CALL) continue;
            XrLirFunction function = function_id(lowered->module, instruction.callee);
            XrLirFunctionView callee;
            OK(xr_lir_function_view(lowered->module, function, &callee));
            if (callee.kind == XR_LIR_FUNCTION_EXTERNAL) continue;
            CHECK(!selected.id);
            selected = function;
        }
    }
    CHECK(selected.id);
    return selected;
}

static XrRbcProgram *callable(const char *text) {
    XrLirLowered lowered = lower(text);
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic = {0};
    CHECK(xr_rbc_compile(lowered.module, initialized_function(&lowered), &program,
        &diagnostic) == XR_RBC_COMPILE_OK && program);
    // Neither Source nor LIR remains alive during any execution below.
    xr_lir_lowered_free(&lowered);
    return program;
}

static uint64_t run(XrRbcProgram *program, uint64_t argument) {
    uint64_t result = UINT64_MAX;
    CHECK(xr_rbc_program_run(program, &argument, 1, &result) == XR_RBC_OK);
    return result;
}

static void numerical_oracles(void) {
    XrRbcProgram *fib = callable("fn fib(n:i64)->i64{if(n<2){return n}\n"
        "return fib(n-1)+fib(n-2)}\nprint(fib(10))\n");
    CHECK(run(fib, 0) == 0 && run(fib, 1) == 1 && run(fib, 10) == 55);
    xr_rbc_program_destroy(fib);
    XrRbcProgram *sum = callable("fn sum(n:i64)->i64{var i=0\nvar total=0\n"
        "while(i<n){total=total+i\ni=i+1}\nreturn total}\nprint(sum(20))\n");
    CHECK(run(sum, 0) == 0 && run(sum, 1) == 0 && run(sum, 20) == 190 &&
        run(sum, 1000) == 499500);
    xr_rbc_program_destroy(sum);
    XrRbcProgram *swap = callable("fn swap(n:i64)->i64{var a=1\nvar b=2\nvar i=0\n"
        "while(i<n){const t=a\na=b\nb=t\ni=i+1}\nreturn a*10+b}\nprint(swap(3))\n");
    CHECK(run(swap, 0) == 12 && run(swap, 1) == 21 && run(swap, 3) == 21);
    xr_rbc_program_destroy(swap);
    XrRbcProgram *calls = callable("fn two(a:i64,b:i64)->i64{return a*10+b}\n"
        "fn zero()->i64{return 7}\nfn value(n:i64)->i64{return two(n,n+1)+zero()}\n"
        "print(value(2))\n");
    CHECK(run(calls, 2) == 30 && run(calls, 4) == 52);
    xr_rbc_program_destroy(calls);
}

static void actual_entry_and_refusal(void) {
    XrLirLowered lowered = lower("fn main()->i64{print(99)\nreturn 7}\n");
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic = {0};
    CHECK(xr_rbc_compile(lowered.module, lowered.entry, &program, &diagnostic) ==
        XR_RBC_COMPILE_OK && program);
    xr_lir_lowered_free(&lowered);
    uint64_t result = UINT64_MAX;
    CHECK(xr_rbc_program_run(program, NULL, 0, &result) == XR_RBC_OK && result == 0);
    xr_rbc_program_destroy(program);

    lowered = lower("print(1)\nfn value(x:f64)->f64{return x+1.0}\nprint(2)\nvalue(3.0)\n");
    program = NULL;
    CHECK(xr_rbc_compile(lowered.module, lowered.entry, &program, &diagnostic) ==
        XR_RBC_COMPILE_UNSUPPORTED && !program);
    xr_lir_lowered_free(&lowered);
    CHECK(diagnostic.message[0] && diagnostic.path[0] && diagnostic.line == 4 &&
        diagnostic.column && diagnostic.block && diagnostic.instruction);
}

int main(void) {
    numerical_oracles();
    actual_entry_and_refusal();
    puts("Source-backed bytecode integration passed");
    return 0;
}
