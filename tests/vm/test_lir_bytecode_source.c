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
#include <fenv.h>
#if defined(XR_ARCH_X86_64)
#include <xmmintrin.h>
#endif


// The sole guest result is owned; this test helper consumes it after inspection.
static XrRbcStatus execute(XrRbcProgram *program, const uint64_t *arguments,
    size_t count, uint64_t *bits) {
    XrRbcExecution *execution = NULL;
    XrRbcStatus status = xr_rbc_program_prepare(program, arguments, count, &execution);
    if (status != XR_RBC_OK) return status;
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    CHECK(outcome);
    status = xr_rbc_outcome_status(outcome);
    xr_rbc_outcome_bits(outcome, bits);
    xr_rbc_execution_free(&execution);
    xr_rbc_outcome_free(&outcome);
    return status;
}

// Select the actual original callee before construction; never inspect an erased module.
static uint32_t source_callee(const XrXirModule *module) {
    const XrXirFunction *initializer =
        &module->functions[module->declarations->modules[0].initializer];
    uint32_t selected = UINT32_MAX;
    for (uint32_t i = 0; i < initializer->instruction_count; ++i) {
        const XrXirInstruction *op = &initializer->instructions[i];
        if (op->op != XR_XIR_CALL) continue;
        CHECK(selected == UINT32_MAX && op->immediate >= 0 &&
            (uint64_t) op->immediate < module->function_count);
        selected = (uint32_t) op->immediate;
    }
    CHECK(selected != UINT32_MAX);
    return selected;
}

static XrLirLowered lower(const char *text, bool local) {
    SourceFixture source = source_fixture(text);
    XrLirLowerRequest request = {source.closed, source.source.snapshot, false};
    XrLirLowered result = {0};
    XrLirLowerFailure failure = {0};
    XrLirStatus status = local
        ? xr_lir_lower_local_checked(&request,
            source_callee(xr_xir_compile_artifact_module(source.closed)), &result, &failure)
        : xr_lir_lower_program_checked(&request, &result, &failure);
    if (status != XR_LIR_OK) fprintf(stderr, "lower: %s\n", failure.diagnostic.message);
    CHECK(status == XR_LIR_OK);
    source_fixture_free(&source);
    xr_lir_lower_failure_free(&failure);
    OK(xr_lir_verify(result.module, NULL));
    return result;
}

static XrRbcProgram *callable(const char *text) {
    XrLirLowered lowered = lower(text, true);
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic = {0};
    CHECK(xr_rbc_compile(lowered.module, lowered.entry, &program,
        &diagnostic) == XR_RBC_COMPILE_OK && program);
    // Neither Source nor LIR remains alive during any execution below.
    xr_lir_lowered_free(&lowered);
    return program;
}

static uint64_t run(XrRbcProgram *program, uint64_t argument) {
    uint64_t result = UINT64_MAX;
    CHECK(execute(program, &argument, 1, &result) == XR_RBC_OK);
    return result;
}

static void i64_identity_cast(void) {
    XrLirLowered lowered = lower("fn identity(x:i64)->i64{\nreturn x as i64\n}\nidentity(17)\n", true);
    XrLirFunction selected = lowered.entry;
    XrLirFunctionView function;
    OK(xr_lir_function_view(lowered.module, selected, &function));
    uint32_t casts = 0;
    for (uint32_t b = 0; b < function.block_count; ++b) {
        XrLirBlock block;
        XrLirBlockView view;
        OK(xr_lir_block_at(lowered.module, selected, b, &block));
        OK(xr_lir_block_view(lowered.module, block, &view));
        for (uint32_t n = 0; n < view.instruction_count; ++n) {
            XrLirInstructionView instruction;
            OK(xr_lir_instruction_view(lowered.module, block, n, &instruction));
            if (instruction.op != XR_LIR_INTCAST) continue;
            XrLirType source;
            OK(xr_lir_value_type(lowered.module,
                (XrLirValue){lowered.module, selected.id, instruction.operands[0]}, &source));
            CHECK(source == XR_LIR_I64 && instruction.type == XR_LIR_I64 &&
                instruction.source.line == 2);
            ++casts;
        }
    }
    CHECK(casts == 1);
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(lowered.module, selected, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_lowered_free(&lowered);
    const uint64_t values[] = {0, 1, INT64_MAX, UINT64_C(0x8000000000000000),
        UINT64_MAX, UINT64_C(0xa5a55a5adeadbeef)};
    for (size_t n = 0; n < sizeof(values) / sizeof(values[0]); ++n)
        CHECK(run(program, values[n]) == values[n]);
    xr_rbc_program_destroy(program);
}

static void metadata_lifetime(void) {
    XrLirLowered lowered = lower("fn value(x:i64)->i64{return x+1}\nvalue(9)\n", false);
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(lowered.module, lowered.entry, &program, NULL) == XR_RBC_COMPILE_OK);
    XrtLocationOwner *owner = xr_rbc_program_locations(program);
    CHECK(owner && xrt_locations_count(owner) && xrt_locations_retain(owner));
    XrtSourceLocation expected = *xrt_locations_get(owner, 1);
    char *path = xr_malloc(expected.path_length);
    CHECK(path);
    memcpy(path, expected.path, expected.path_length);
    // Source/session were retired by lower; now retire the last producer too.
    xr_lir_lowered_free(&lowered);
    uint64_t result = UINT64_MAX;
    CHECK(execute(program, NULL, 0, &result) == XR_RBC_OK && result == 0);
    xr_rbc_program_destroy(program);
    const XrtSourceLocation *actual = xrt_locations_get(owner, 1);
    CHECK(actual && actual->path_length == expected.path_length &&
        !memcmp(actual->path, path, actual->path_length));
    CHECK(actual->line == expected.line && actual->column == expected.column &&
        actual->end_line == expected.end_line && actual->end_column == expected.end_column);
    xr_free(path);
    xrt_locations_release(owner);
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
    XrLirLowered lowered = lower("fn main()->i64{print(99)\nreturn 7}\n", false);
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic = {0};
    CHECK(xr_rbc_compile(lowered.module, lowered.entry, &program, &diagnostic) ==
        XR_RBC_COMPILE_OK && program);
    xr_lir_lowered_free(&lowered);
    uint64_t result = UINT64_MAX;
    CHECK(execute(program, NULL, 0, &result) == XR_RBC_OK && result == 0);
    xr_rbc_program_destroy(program);

    lowered = lower("print(1)\nfn value(x:f64)->f64{return x+1.0}\nprint(2)\nvalue(3.0)\n", false);
    program = NULL;
    CHECK(xr_rbc_compile(lowered.module, lowered.entry, &program, &diagnostic) ==
        XR_RBC_COMPILE_UNSUPPORTED && !program);
    xr_lir_lowered_free(&lowered);
    CHECK(diagnostic.message[0] && diagnostic.path[0] && diagnostic.line == 2 &&
        diagnostic.column && diagnostic.block && diagnostic.instruction);
    lowered = lower("fn value(x:f64)->f64{\nreturn x+1.0\n}\nvalue(3.0)\n", false);
    CHECK(xr_rbc_compile(lowered.module, lowered.entry, &program, &diagnostic) ==
        XR_RBC_COMPILE_UNSUPPORTED && !program);
    xr_lir_lowered_free(&lowered);
    CHECK(diagnostic.path[0] && diagnostic.line == 2 && diagnostic.column);
}

static void f64_transport(void) {
    XrLirLowered whole = lower("fn identity(x:f64)->f64{return x}\nidentity(1.25)\n", false);
    XrRbcProgram *entry = NULL;
    CHECK(xr_rbc_compile(whole.module, whole.entry, &entry, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_lowered_free(&whole);
    uint64_t entry_result = UINT64_MAX;
    CHECK(execute(entry, NULL, 0, &entry_result) == XR_RBC_OK && entry_result == 0);
    xr_rbc_program_destroy(entry);
    XrRbcProgram *identity = callable("fn identity(x:f64)->f64{return x}\nidentity(1.25)\n");
    XrRbcProgram *swap = callable("fn swap(x:f64,y:f64)->f64{var a=x\nvar b=y\nvar i=0\n"
        "while(i<3){const t=a\na=b\nb=t\ni=i+1}\nreturn b}\nswap(1.0,2.0)\n");
    XrRbcProgram *recurse = callable("fn echo(n:i64,x:f64)->f64{if(n==0){return x}\n"
        "return echo(n-1,x)}\necho(32,1.0)\n");
    const uint64_t cases[] = {0, UINT64_C(0x8000000000000000),
        UINT64_C(0x3ff0000000000000), UINT64_C(0xbff0000000000000),
        1, UINT64_C(0x8000000000000001), UINT64_C(0x7ff0000000000000),
        UINT64_C(0xfff0000000000000), UINT64_C(0x7ff8000000000000),
        UINT64_C(0x7ff8deadbeef1234), UINT64_C(0x7ff0000000000001),
        UINT64_C(0xfff0000000000001)};
    const int modes[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    fenv_t original;
    CHECK(fegetenv(&original) == 0);
#if defined(XR_ARCH_X86_64)
    unsigned original_control = _mm_getcsr();
    const unsigned control_cases = 4;
#else
    const unsigned control_cases = 1;
#endif
    for (size_t mode = 0; mode < sizeof(modes) / sizeof(modes[0]); ++mode) {
        CHECK(fesetround(modes[mode]) == 0);
        for (unsigned control = 0; control < control_cases; ++control) {
            CHECK(feclearexcept(FE_ALL_EXCEPT) == 0 && feraiseexcept(FE_INEXACT) == 0);
#if defined(XR_ARCH_X86_64)
            unsigned mask = (1u << 6) | (1u << 15);
            _mm_setcsr((_mm_getcsr() & ~mask) | ((control & 1u) << 6) |
                (((control >> 1) & 1u) << 15));
            unsigned expected_control = _mm_getcsr();
#endif
            int flags = fetestexcept(FE_ALL_EXCEPT);
            for (size_t n = 0; n < sizeof(cases) / sizeof(cases[0]); ++n) {
                uint64_t value = cases[n], result = ~value;
                CHECK(execute(identity, &value, 1, &result) == XR_RBC_OK &&
                    result == value);
                uint64_t arguments[] = {value, cases[(n + 1) % (sizeof(cases) / sizeof(cases[0]))]};
                result = ~value;
                CHECK(execute(swap, arguments, 2, &result) == XR_RBC_OK &&
                    result == value);
                arguments[0] = 32;
                arguments[1] = value;
                result = ~value;
                CHECK(execute(recurse, arguments, 2, &result) == XR_RBC_OK &&
                    result == value);
                CHECK(fegetround() == modes[mode] && fetestexcept(FE_ALL_EXCEPT) == flags);
#if defined(XR_ARCH_X86_64)
                CHECK(_mm_getcsr() == expected_control);
#endif
            }
        }
    }
    CHECK(fesetenv(&original) == 0);
#if defined(XR_ARCH_X86_64)
    _mm_setcsr(original_control);
#endif
    xr_rbc_program_destroy(identity);
    xr_rbc_program_destroy(swap);
    xr_rbc_program_destroy(recurse);
}

int main(void) {
    i64_identity_cast();
    metadata_lifetime();
    numerical_oracles();
    actual_entry_and_refusal();
    f64_transport();
    puts("Source-backed bytecode integration passed");
    return 0;
}
