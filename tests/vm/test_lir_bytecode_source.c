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
#include "lir/xlir_builder.h"
#include "lir/xlir_verify.h"
#include "lir/xlir_c.h"
#include "lir/xlir_sccp.h"
#include "vm/register/xrbc_lir.h"
#include "f64_family_vectors.h"
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

static const char i64_equality_source[] =
    "fn equality(x:i64,y:i64)->i64{if(x==y){return 1}\n"
    "if(x!=y){return 2}\nreturn 3}\nprint(equality(1,2))\n";

static void i64_equality(void) {
    static const uint64_t vectors[][3] = {
        {0,0,1},{1,2,2},{1,1,1},{0,UINT64_MAX,2},{UINT64_MAX,UINT64_MAX,1},
        {UINT64_C(0x8000000000000000),UINT64_C(0x8000000000000000),1},
        {UINT64_C(0x8000000000000000),INT64_MAX,2},{INT64_MAX,INT64_MAX,1}
    };
    XrRbcProgram *program = callable(i64_equality_source);
    for (unsigned i=0;i<sizeof(vectors)/sizeof(*vectors);++i) {
        uint64_t result = 99;
        CHECK(execute(program,vectors[i],2,&result) == XR_RBC_OK && result == vectors[i][2]);
    }
    xr_rbc_program_destroy(program);
}

static uint32_t equality_callee(XrLirModule *module) {
    uint32_t selected = 0, mask = 0, comparisons = 0;
    for (uint32_t n=0;n<xr_lir_function_count(module);++n) {
        XrLirFunction f; XrLirFunctionView fv;
        OK(xr_lir_function_at(module,n,&f)); OK(xr_lir_function_view(module,f,&fv));
        for (uint32_t b=0;b<fv.block_count;++b) {
            XrLirBlock block; XrLirBlockView bv;
            OK(xr_lir_block_at(module,f,b,&block)); OK(xr_lir_block_view(module,block,&bv));
            for (uint32_t i=0;i<bv.instruction_count;++i) {
                XrLirInstructionView v; OK(xr_lir_instruction_view(module,block,i,&v));
                if (v.op!=XR_LIR_ICMP) continue;
                CHECK((!selected || selected==f.id) && fv.parameter_count==2 &&
                    fv.parameter_types[0]==XR_LIR_I64 && fv.parameter_types[1]==XR_LIR_I64 &&
                    fv.result==XR_LIR_I64 && (v.predicate==XR_LIR_EQ || v.predicate==XR_LIR_NE));
                selected=f.id;mask|=1u<<v.predicate;++comparisons;
            }
        }
    }
    CHECK(selected && comparisons==2 && mask==((1u<<XR_LIR_EQ)|(1u<<XR_LIR_NE)));
    return selected;
}

static void i64_equality_c(const char *path) {
    XrLirLowered local=lower(i64_equality_source,true);
    CHECK(equality_callee(local.module)==local.entry.id);xr_lir_lowered_free(&local);
    XrLirLowered whole=lower(i64_equality_source,false);
    uint32_t callee=equality_callee(whole.module);
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrLirCRequest request={whole.module,whole.entry,resources,"source-i64-equality",NULL};
    XrLirCOutput output={0};CHECK(xr_lir_emit_c(&request,&output,NULL)==XR_LIR_C_OK);
    xr_lir_lowered_free(&whole);xr_compile_resources_release(resources);
    FILE *file=fopen(path,"wb");CHECK(file);
    CHECK(fwrite(output.bytes,1,output.length,file)==output.length && fclose(file)==0);
    xr_lir_c_output_free(&output);
    printf("{\"callee\":%u,\"sourceLocalBothPredicates\":true,\"sourceWholeBothPredicates\":true}\n",callee);
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

    lowered = lower("print(1)\nfn value(x:i64)->f64{return x as f64}\nprint(2)\nvalue(3)\n", false);
    program = NULL;
    CHECK(xr_rbc_compile(lowered.module, lowered.entry, &program, &diagnostic) ==
        XR_RBC_COMPILE_UNSUPPORTED && !program);
    xr_lir_lowered_free(&lowered);
    CHECK(diagnostic.message[0] && diagnostic.path[0] && diagnostic.line == 2 &&
        diagnostic.column && diagnostic.block && diagnostic.instruction);
    lowered = lower("fn value(x:i64)->f64{\nreturn x as f64\n}\nvalue(3)\n", false);
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

static const char f64_negation_source[] =
    "fn negate(x:f64)->f64{return -x}\nnegate(1.25)\n";

static void f64_negation(void) {
    static const uint64_t vectors[][2] = {
#define XR_FNEG(input, expected) {UINT64_C(0x##input),UINT64_C(0x##expected)},
#include "f64_negate_vectors.def"
#undef XR_FNEG
    };
    XrRbcProgram *program = callable(f64_negation_source);
    const int modes[] = {FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
    fenv_t original; CHECK(fegetenv(&original) == 0);
#if defined(XR_ARCH_X86_64)
    unsigned original_control = _mm_getcsr();
    const unsigned control_cases = 4;
#else
    const unsigned control_cases = 1;
#endif
    for (unsigned m=0;m<4;++m) for (unsigned c=0;c<control_cases;++c) {
        CHECK(fesetround(modes[m]) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
            feraiseexcept(FE_INEXACT) == 0);
#if defined(XR_ARCH_X86_64)
        unsigned mask = (1u<<6)|(1u<<15);
        _mm_setcsr((_mm_getcsr() & ~mask) | ((c&1u)<<6) | (((c>>1)&1u)<<15));
        unsigned expected_control = _mm_getcsr();
#endif
        int flags = fetestexcept(FE_ALL_EXCEPT);
        for (unsigned i=0;i<sizeof(vectors)/sizeof(*vectors);++i) {
            CHECK(run(program,vectors[i][0]) == vectors[i][1]);
            CHECK(fegetround() == modes[m] && fetestexcept(FE_ALL_EXCEPT) == flags);
#if defined(XR_ARCH_X86_64)
            CHECK(_mm_getcsr() == expected_control);
#endif
        }
    }
    CHECK(fesetenv(&original) == 0);
#if defined(XR_ARCH_X86_64)
    _mm_setcsr(original_control);
#endif
    uint64_t input = UINT64_C(0xfff0000000000001);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program,&input,1,&execution) == XR_RBC_OK);
    input = 0;
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    CHECK(outcome && xr_rbc_outcome_status(outcome) == XR_RBC_OK);
    xr_rbc_execution_free(&execution); xr_rbc_program_destroy(program);
    uint64_t result = 0;
    CHECK(xr_rbc_outcome_bits(outcome,&result) && result == UINT64_C(0x7ff8000000000000));
    xr_rbc_outcome_free(&outcome);
}

static uint32_t negation_callee(XrLirModule *module) {
    uint32_t selected = 0;
    for (uint32_t n = 0; n < xr_lir_function_count(module); ++n) {
        XrLirFunction f; XrLirFunctionView fv;
        OK(xr_lir_function_at(module,n,&f)); OK(xr_lir_function_view(module,f,&fv));
        for (uint32_t b = 0; b < fv.block_count; ++b) {
            XrLirBlock block; XrLirBlockView bv;
            OK(xr_lir_block_at(module,f,b,&block)); OK(xr_lir_block_view(module,block,&bv));
            for (uint32_t i = 0; i < bv.instruction_count; ++i) {
                XrLirInstructionView v; OK(xr_lir_instruction_view(module,block,i,&v));
                if (v.op != XR_LIR_FNEG) continue;
                CHECK(!selected && fv.parameter_count == 1 && fv.parameter_types[0] == XR_LIR_F64 &&
                    fv.result == XR_LIR_F64 && v.type == XR_LIR_F64 && v.operand_count == 1 &&
                    v.operands[0] == fv.parameters[0] && v.source.line == 1);
                selected = f.id;
            }
        }
    }
    CHECK(selected);
    return selected;
}

static void f64_negation_c(const char *path) {
    XrLirLowered local = lower(f64_negation_source,true);
    CHECK(negation_callee(local.module) == local.entry.id);
    xr_lir_lowered_free(&local);
    XrLirLowered whole = lower(f64_negation_source,false);
    uint32_t callee = negation_callee(whole.module);
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits,&resources) == XR_COMPILE_RESOURCE_OK);
    XrLirCRequest request = {whole.module,whole.entry,resources,"source-fneg",NULL};
    XrLirCOutput output = {0};
    CHECK(xr_lir_emit_c(&request,&output,NULL) == XR_LIR_C_OK);
    xr_lir_lowered_free(&whole); xr_compile_resources_release(resources);
    FILE *file = fopen(path,"wb"); CHECK(file);
    CHECK(fwrite(output.bytes,1,output.length,file) == output.length && fclose(file) == 0);
    xr_lir_c_output_free(&output);
    printf("{\"callee\":%u,\"sourceLocalFneg\":true,\"sourceWholeFneg\":true}\n",callee);
}

static const char f64_arithmetic_source[] =
    "fn arithmetic(op:i64,x:f64,y:f64)->f64{if(op==0){return x+y}\n"
    "if(op==1){return x-y}\nif(op==2){return x*y}\nreturn x/y}\n"
    "arithmetic(0,1.25,2.5)\n";
static const char f64_relations_source[] =
    "fn relations(x:f64,y:f64)->i64{var flags=0\n"
    "if(x==y){flags=flags+1}\nif(x!=y){flags=flags+2}\nif(x<y){flags=flags+4}\n"
    "if(x<=y){flags=flags+8}\nif(x>y){flags=flags+16}\nif(x>=y){flags=flags+32}\n"
    "return flags}\nprint(relations(1.0,2.0))\n";

static void f64_family(void) {
    XrRbcProgram *arithmetic=callable(f64_arithmetic_source),*relations=callable(f64_relations_source);
    const int modes[]={FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
    fenv_t original;CHECK(fegetenv(&original)==0);
#if defined(XR_ARCH_X86_64)
    unsigned original_control=_mm_getcsr();const unsigned control_cases=4;
#else
    const unsigned control_cases=1;
#endif
    for(unsigned m=0;m<4;++m)for(unsigned c=0;c<control_cases;++c) {
        CHECK(fesetround(modes[m])==0 && feclearexcept(FE_ALL_EXCEPT)==0 && feraiseexcept(FE_INEXACT)==0);
#if defined(XR_ARCH_X86_64)
        unsigned mask=(1u<<6)|(1u<<15);
        _mm_setcsr((_mm_getcsr()&~mask)|((c&1u)<<6)|(((c>>1)&1u)<<15));
        unsigned control=_mm_getcsr();
#endif
        int flags=fetestexcept(FE_ALL_EXCEPT);uint32_t arithmetic_count=0,relation_count=0;
        for(unsigned i=0;i<sizeof(float_arithmetic_witnesses)/sizeof(*float_arithmetic_witnesses);++i) {
            if(float_arithmetic_witnesses[i].width!=64)continue;
            uint64_t args[]={float_arithmetic_witnesses[i].operation,
                float_arithmetic_witnesses[i].left,float_arithmetic_witnesses[i].right},result=0;
            CHECK(execute(arithmetic,args,3,&result)==XR_RBC_OK && result==float_arithmetic_witnesses[i].expected);
            CHECK(fegetround()==modes[m] && fetestexcept(FE_ALL_EXCEPT)==flags);
#if defined(XR_ARCH_X86_64)
            CHECK(_mm_getcsr()==control);
#endif
            ++arithmetic_count;
        }
        for(unsigned a=0;a<26;++a)for(unsigned b=0;b<26;++b) {
            uint64_t args[]={float_order_witnesses[a].bits,float_order_witnesses[b].bits},result=0;
            CHECK(execute(relations,args,2,&result)==XR_RBC_OK && result==float_relation_mask(a,b));
            CHECK(fegetround()==modes[m] && fetestexcept(FE_ALL_EXCEPT)==flags);
#if defined(XR_ARCH_X86_64)
            CHECK(_mm_getcsr()==control);
#endif
            ++relation_count;
        }
        CHECK(arithmetic_count==3216 && relation_count==676);
    }
    CHECK(fesetenv(&original)==0);
#if defined(XR_ARCH_X86_64)
    _mm_setcsr(original_control);
#endif
    xr_rbc_program_destroy(arithmetic);xr_rbc_program_destroy(relations);
}

static uint32_t float_family_callee(XrLirModule *module,bool relation) {
    uint32_t selected=0,count=0,mask=0;
    for(uint32_t n=0;n<xr_lir_function_count(module);++n) {
        XrLirFunction f;XrLirFunctionView fv;OK(xr_lir_function_at(module,n,&f));
        OK(xr_lir_function_view(module,f,&fv));
        for(uint32_t b=0;b<fv.block_count;++b) {
            XrLirBlock block;XrLirBlockView bv;OK(xr_lir_block_at(module,f,b,&block));
            OK(xr_lir_block_view(module,block,&bv));
            for(uint32_t i=0;i<bv.instruction_count;++i) {
                XrLirInstructionView v;OK(xr_lir_instruction_view(module,block,i,&v));
                unsigned bit=0;
                if(relation) {if(v.op==XR_LIR_FCMP)bit=1u<<v.predicate;}
                else switch(v.op) {
                    case XR_LIR_FADD:bit=1;break;case XR_LIR_FSUB:bit=2;break;
                    case XR_LIR_FMUL:bit=4;break;case XR_LIR_FDIV:bit=8;break;default:break;
                }
                if(!bit)continue;
                CHECK((!selected || selected==f.id) && fv.parameter_count==(relation?2u:3u) &&
                    fv.result==(relation?XR_LIR_I64:XR_LIR_F64));
                if(relation)CHECK(fv.parameter_types[0]==XR_LIR_F64 && fv.parameter_types[1]==XR_LIR_F64);
                else CHECK(fv.parameter_types[0]==XR_LIR_I64 && fv.parameter_types[1]==XR_LIR_F64 &&
                    fv.parameter_types[2]==XR_LIR_F64);
                selected=f.id;mask|=bit;++count;
            }
        }
    }
    CHECK(selected && count==(relation?6u:4u) && mask==(relation?126u:15u));
    return selected;
}

static void f64_family_c(const char *directory) {
    const char *sources[]={f64_arithmetic_source,f64_relations_source};uint32_t callees[2]={0};
    for(unsigned i=0;i<2;++i) {
        XrLirLowered local=lower(sources[i],true);
        CHECK(float_family_callee(local.module,i!=0)==local.entry.id);xr_lir_lowered_free(&local);
        XrLirLowered whole=lower(sources[i],false);callees[i]=float_family_callee(whole.module,i!=0);
        XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrCompileResources *resources=NULL;
        CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
        XrLirCRequest request={whole.module,whole.entry,resources,"source-f64-family",NULL};
        XrLirCOutput output={0};CHECK(xr_lir_emit_c(&request,&output,NULL)==XR_LIR_C_OK);
        xr_lir_lowered_free(&whole);xr_compile_resources_release(resources);
        char path[1024];int size=snprintf(path,sizeof(path),"%s/%s.c",directory,i?"relations":"arithmetic");
        CHECK(size>0 && (size_t)size<sizeof(path));FILE *file=fopen(path,"wb");CHECK(file);
        CHECK(fwrite(output.bytes,1,output.length,file)==output.length && fclose(file)==0);
        xr_lir_c_output_free(&output);
    }
    printf("{\"arithmeticCallee\":%u,\"relationsCallee\":%u,\"arithmeticOps\":4,\"relationPredicates\":6}\n",callees[0],callees[1]);
}

static void f64_family_whole(void) {
    XrLirLowered whole=lower(f64_relations_source,false);XrRbcProgram *program=NULL;
    CHECK(xr_rbc_compile(whole.module,whole.entry,&program,NULL)==XR_RBC_COMPILE_OK);
    xr_lir_lowered_free(&whole);uint64_t result=99;
    CHECK(execute(program,NULL,0,&result)==XR_RBC_OK && result==0);xr_rbc_program_destroy(program);
}

static void owned_string_result(const char *path) {
    XrLirLowered lowered = lower("fn produce()->string{return \"é\\0${73}\"}\nproduce()\n",true);
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(lowered.module,lowered.entry,&program,NULL)==XR_RBC_COMPILE_OK);
    XrCompileResources *resources = NULL;
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrLirCRequest request = {lowered.module,lowered.entry,resources,"owned-string-result",NULL};
    XrLirCOutput output = {0};
    CHECK(xr_lir_emit_c(&request,&output,NULL)==XR_LIR_C_OK && output.entry_result==XR_LIR_STRING);
    xr_lir_lowered_free(&lowered);
    xr_compile_resources_release(resources);
    for (unsigned run=0; run<2; ++run) {
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
        XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
        CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_OK);
        xr_rbc_execution_free(&execution);
        if (run) {xr_rbc_program_destroy(program);program=NULL;}
        uint64_t bits = 99;
        CHECK(!xr_rbc_outcome_bits(outcome,&bits) && bits==99);
        const XrtString *borrowed = NULL;
        CHECK(xr_rbc_outcome_string(outcome,&borrowed));
        size_t length = 0;
        const char *bytes = xrt_string_bytes(borrowed,&length);
        CHECK(length==5 && !memcmp(bytes,"\xc3\xa9\0" "73",5));
        if (!run) {
            XrtString *owned = NULL;
            CHECK(xr_rbc_outcome_take_string(outcome,&owned));
            CHECK(!xr_rbc_outcome_string(outcome,&borrowed));
            xr_rbc_outcome_free(&outcome);
            bytes=xrt_string_bytes(owned,&length);
            CHECK(length==5 && !memcmp(bytes,"\xc3\xa9\0" "73",5));
            xrt_string_owned_drop(&owned);CHECK(!owned);
        } else xr_rbc_outcome_free(&outcome);
    }
    if (path) {
        FILE *file=fopen(path,"wb");CHECK(file);
        CHECK(fwrite(output.bytes,1,output.length,file)==output.length && fclose(file)==0);
    }
    xr_lir_c_output_free(&output);
}

// This explicit LIR fixture qualifies both physical consumers, not a Source producer.
static void frame_cell_result(const char *path) {
    static const unsigned char bytes[]={'c','e','l','l',0,0xc3,0xa9};
    XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));
    XrLirFunction root={0};XrLirFunctionSpec spec={.name="frame_result",.name_length=12,.result=XR_LIR_STRING};
    OK(xr_lir_declare_function(m,&spec,&root));OK(xr_lir_define_function(m,root));
    XrLirBlock blocks[3];for(unsigned i=0;i<3;++i)OK(xr_lir_add_block(m,root,NULL,0,&blocks[i]));
    OK(xr_lir_set_entry(m,root,blocks[0]));XrLirLiteral literal={0};
    OK(xr_lir_add_literal(m,bytes,sizeof(bytes),&literal));
    XrLirValue text,cell,condition;
    XrLirInstruction op={.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=literal.id};
    OK(xr_lir_emit_value(m,blocks[0],&op,&text));
    op=(XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=XR_LIR_STRING_CELL,.operands=&text,.operand_count=1};
    OK(xr_lir_emit_value(m,blocks[0],&op,&cell));
    op=(XrLirInstruction){.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&text,.operand_count=1};
    OK(xr_lir_emit_effect(m,blocks[0],&op));
    op=(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I1,.bits=1};
    OK(xr_lir_emit_value(m,blocks[0],&op,&condition));
    op=(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,.operands=&condition,.operand_count=1,
        .edges={{blocks[1],NULL,0},{blocks[2],NULL,0}},.edge_count=2};
    OK(xr_lir_set_terminator(m,blocks[0],&op));
    for(unsigned i=1;i<3;++i) {
        op=(XrLirInstruction){.op=XR_LIR_CELL_READ,.type=XR_LIR_STRING,.operands=&cell,.operand_count=1};
        OK(xr_lir_emit_value(m,blocks[i],&op,&text));
        op=(XrLirInstruction){.op=XR_LIR_STRING_CELL_DROP,.type=XR_LIR_UNIT,.operands=&cell,.operand_count=1};
        OK(xr_lir_emit_effect(m,blocks[i],&op));
        op=(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&text,.operand_count=1};
        OK(xr_lir_set_terminator(m,blocks[i],&op));
    }
    OK(xr_lir_module_finish(m,NULL));XrRbcProgram *program=NULL;
    CHECK(xr_rbc_compile(m,root,&program,NULL)==XR_RBC_COMPILE_OK);
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrLirCRequest request={m,root,resources,"frame-cell-result",NULL};XrLirCOutput output={0};
    CHECK(xr_lir_emit_c(&request,&output,NULL)==XR_LIR_C_OK && output.entry_result==XR_LIR_STRING);
    xr_lir_module_destroy(m);
    for(unsigned run=0;run<2;++run) {
        XrRbcExecution *execution=NULL;CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
        XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
        CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_OK);xr_rbc_execution_free(&execution);
        if(run){xr_rbc_program_destroy(program);program=NULL;}
        XrtString *owned=NULL;CHECK(xr_rbc_outcome_take_string(outcome,&owned));xr_rbc_outcome_free(&outcome);
        size_t length=0;const char *actual=xrt_string_bytes(owned,&length);
        CHECK(length==sizeof(bytes) && !memcmp(actual,bytes,sizeof(bytes)));xrt_string_owned_drop(&owned);
    }
    FILE *file=fopen(path,"wb");CHECK(file);
    CHECK(fwrite(output.bytes,1,output.length,file)==output.length && fclose(file)==0);
    xr_lir_c_output_free(&output);xr_compile_resources_release(resources);
}

// Two symbolic Cell parameters can denote the same actual frame storage.
static void frame_cell_alias(const char *path) {
    XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));XrLirFunction root,callee;
    XrLirType types[]={XR_LIR_STRING_CELL,XR_LIR_STRING_CELL,XR_LIR_I64};
    XrLirFunctionSpec spec={.name="alias_root",.name_length=10,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(m,&spec,&root));OK(xr_lir_define_function(m,root));
    spec=(XrLirFunctionSpec){.name="explicit_hidden_alias",.name_length=21,.parameters=types,.parameter_count=3,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(m,&spec,&callee));OK(xr_lir_define_function(m,callee));
    XrLirBlock entry,blocks[4];OK(xr_lir_add_block(m,root,NULL,0,&entry));OK(xr_lir_set_entry(m,root,entry));
    for(unsigned i=0;i<4;++i)OK(xr_lir_add_block(m,callee,i==1?&types[2]:NULL,i==1?1u:0u,&blocks[i]));
    OK(xr_lir_set_entry(m,callee,blocks[0]));XrLirLiteral seed={0},replacement={0};
    OK(xr_lir_add_literal(m,"old",3,&seed));OK(xr_lir_add_literal(m,"alias",5,&replacement));
    XrLirValue text,cell,n,result;
    XrLirInstruction op={.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=seed.id};
    OK(xr_lir_emit_value(m,entry,&op,&text));op=(XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=XR_LIR_STRING_CELL,.operands=&text,.operand_count=1};
    OK(xr_lir_emit_value(m,entry,&op,&cell));op=(XrLirInstruction){.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&text,.operand_count=1};
    OK(xr_lir_emit_effect(m,entry,&op));op=(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=3};
    OK(xr_lir_emit_value(m,entry,&op,&n));XrLirValue arguments[]={cell,cell,n};
    op=(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_I64,.callee=callee,.operands=arguments,.operand_count=3};
    OK(xr_lir_emit_value(m,entry,&op,&result));op=(XrLirInstruction){.op=XR_LIR_STRING_CELL_DROP,.type=XR_LIR_UNIT,.operands=&cell,.operand_count=1};
    OK(xr_lir_emit_effect(m,entry,&op));op=(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&result,.operand_count=1};
    OK(xr_lir_set_terminator(m,entry,&op));XrLirValue parameters[3],zero,one,iteration,condition,next,length;
    for(unsigned i=0;i<3;++i)OK(xr_lir_function_parameter(m,callee,i,&parameters[i]));
    op=(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64};OK(xr_lir_emit_value(m,blocks[0],&op,&zero));
    op.bits=1;OK(xr_lir_emit_value(m,blocks[0],&op,&one));
    op=(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,.edges={{blocks[1],&zero,1}},.edge_count=1};
    OK(xr_lir_set_terminator(m,blocks[0],&op));OK(xr_lir_block_parameter(m,blocks[1],0,&iteration));
    XrLirValue operands[]={iteration,parameters[2]};
    op=(XrLirInstruction){.op=XR_LIR_ICMP,.type=XR_LIR_I1,.predicate=XR_LIR_LT,.operands=operands,.operand_count=2};
    OK(xr_lir_emit_value(m,blocks[1],&op,&condition));
    op=(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,.operands=&condition,.operand_count=1,
        .edges={{blocks[2],NULL,0},{blocks[3],NULL,0}},.edge_count=2};OK(xr_lir_set_terminator(m,blocks[1],&op));
    op=(XrLirInstruction){.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=replacement.id};
    OK(xr_lir_emit_value(m,blocks[2],&op,&text));operands[0]=parameters[1];operands[1]=text;
    op=(XrLirInstruction){.op=XR_LIR_CELL_WRITE,.type=XR_LIR_UNIT,.operands=operands,.operand_count=2};
    OK(xr_lir_emit_effect(m,blocks[2],&op));op=(XrLirInstruction){.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&text,.operand_count=1};
    OK(xr_lir_emit_effect(m,blocks[2],&op));operands[0]=iteration;operands[1]=one;
    op=(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=operands,.operand_count=2};
    OK(xr_lir_emit_value(m,blocks[2],&op,&next));
    op=(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,.edges={{blocks[1],&next,1}},.edge_count=1};
    OK(xr_lir_set_terminator(m,blocks[2],&op));
    op=(XrLirInstruction){.op=XR_LIR_CELL_READ,.type=XR_LIR_STRING,.operands=&parameters[0],.operand_count=1};
    OK(xr_lir_emit_value(m,blocks[3],&op,&text));op=(XrLirInstruction){.op=XR_LIR_STRING_LEN,.type=XR_LIR_I64,.operands=&text,.operand_count=1};
    OK(xr_lir_emit_value(m,blocks[3],&op,&length));op=(XrLirInstruction){.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&text,.operand_count=1};
    OK(xr_lir_emit_effect(m,blocks[3],&op));op=(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&length,.operand_count=1};
    OK(xr_lir_set_terminator(m,blocks[3],&op));OK(xr_lir_module_finish(m,NULL));
    XrRbcProgram *program=NULL;CHECK(xr_rbc_compile(m,root,&program,NULL)==XR_RBC_COMPILE_OK);
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrLirCRequest request={m,root,resources,"frame-alias",NULL};XrLirCOutput output={0};
    CHECK(xr_lir_emit_c(&request,&output,NULL)==XR_LIR_C_OK);xr_lir_module_destroy(m);
    for(unsigned i=0;i<2;++i){uint64_t bits=99;CHECK(execute(program,NULL,0,&bits)==XR_RBC_OK&&bits==5);}
    xr_rbc_program_destroy(program);FILE *file=fopen(path,"wb");CHECK(file);
    CHECK(fwrite(output.bytes,1,output.length,file)==output.length&&fclose(file)==0);
    xr_lir_c_output_free(&output);xr_compile_resources_release(resources);
}

static void constant_string_branch(const char *directory) {
    XrLirModule *modules[2]={NULL,NULL}; OK(xr_lir_module_create(NULL,&modules[0]));
    XrLirModule *m=modules[0];XrLirFunction f={0};
    XrLirFunctionSpec spec={.name="constant_string",.name_length=15,.result=XR_LIR_STRING};
    OK(xr_lir_declare_function(m,&spec,&f));OK(xr_lir_define_function(m,f));
    XrLirBlock blocks[4];XrLirType type=XR_LIR_STRING;
    for(unsigned i=0;i<4;++i)OK(xr_lir_add_block(m,f,i==3?&type:NULL,i==3?1u:0u,&blocks[i]));
    OK(xr_lir_set_entry(m,f,blocks[0]));
    XrLirValue condition;XrLirInstruction truth={.op=XR_LIR_CONST,.type=XR_LIR_I1,.bits=1};
    OK(xr_lir_emit_value(m,blocks[0],&truth,&condition));
    XrLirInstruction branch={.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,.operands=&condition,.operand_count=1,
        .edges={{blocks[1],NULL,0},{blocks[2],NULL,0}},.edge_count=2};
    OK(xr_lir_set_terminator(m,blocks[0],&branch));
    for(unsigned i=1;i<=2;++i) {
        XrLirLiteral literal={0};const char *bytes=i==1?"kept":"dead";OK(xr_lir_add_literal(m,bytes,4,&literal));
        XrLirValue value;XrLirInstruction string={.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=literal.id};
        OK(xr_lir_emit_value(m,blocks[i],&string,&value));
        XrLirInstruction edge={.op=XR_LIR_BR,.type=XR_LIR_UNIT,.edges={{blocks[3],&value,1}},.edge_count=1};
        OK(xr_lir_set_terminator(m,blocks[i],&edge));
    }
    XrLirValue result;OK(xr_lir_block_parameter(m,blocks[3],0,&result));
    XrLirInstruction ret={.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&result,.operand_count=1};
    OK(xr_lir_set_terminator(m,blocks[3],&ret));OK(xr_lir_module_finish(m,NULL));
    OK(xr_lir_sccp(m,&modules[1]));
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    for(unsigned i=0;i<2;++i) {
        XrLirFunction root;XrLirFunctionView view;OK(xr_lir_function_at(modules[i],0,&root));
        OK(xr_lir_function_view(modules[i],root,&view));CHECK(view.block_count==(i?3u:4u));
        OK(xr_lir_verify(modules[i],NULL));
        XrLirCRequest request={modules[i],root,resources,"constant-string",NULL};XrLirCOutput output={0};
        CHECK(xr_lir_emit_c(&request,&output,NULL)==XR_LIR_C_OK&&output.entry_result==XR_LIR_STRING);
        XrRbcProgram *program=NULL;CHECK(xr_rbc_compile(modules[i],root,&program,NULL)==XR_RBC_COMPILE_OK);
        xr_lir_module_destroy(modules[i]);modules[i]=NULL;
        XrRbcExecution *execution=NULL;CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
        XrRbcOutcome *outcome=xr_rbc_execution_run(execution);CHECK(outcome&&xr_rbc_outcome_status(outcome)==XR_RBC_OK);
        xr_rbc_execution_free(&execution);xr_rbc_program_destroy(program);
        const XrtString *text=NULL;CHECK(xr_rbc_outcome_string(outcome,&text));
        size_t length=0;const char *bytes=xrt_string_bytes(text,&length);CHECK(length==4&&!memcmp(bytes,"kept",4));
        xr_rbc_outcome_free(&outcome);
        if(directory) {
            char path[1024];int size=snprintf(path,sizeof(path),"%s/%s.c",directory,i?"sccp":"raw");
            CHECK(size>0&&(size_t)size<sizeof(path));FILE *file=fopen(path,"wb");CHECK(file);
            CHECK(fwrite(output.bytes,1,output.length,file)==output.length&&fclose(file)==0);
        }
        xr_lir_c_output_free(&output);
    }
    xr_compile_resources_release(resources);
}

int main(int argc, char **argv) {
    if(argc==3&&!strcmp(argv[1],"--frame-cell-result")){frame_cell_result(argv[2]);return 0;}
    if(argc==3&&!strcmp(argv[1],"--frame-cell-alias")){frame_cell_alias(argv[2]);return 0;}
    if(argc==3&&!strcmp(argv[1],"--f64-family-c")){f64_family_c(argv[2]);return 0;}
    if(argc==2&&!strcmp(argv[1],"--f64-family-rbc")){f64_family_whole();return 0;}
    if(argc==3&&!strcmp(argv[1],"--i64-equality-c")){i64_equality_c(argv[2]);return 0;}
    if(argc==3&&!strcmp(argv[1],"--fneg-c")){f64_negation_c(argv[2]);return 0;}
    if(argc==3&&!strcmp(argv[1],"--constant-branch")){constant_string_branch(argv[2]);return 0;}
    CHECK(argc==1 || argc==2);
    if(argc==1)constant_string_branch(NULL);
    owned_string_result(argc==2 ? argv[1] : NULL);
    if (argc==2) return 0;
    i64_identity_cast();
    i64_equality();
    metadata_lifetime();
    numerical_oracles();
    actual_entry_and_refusal();
    f64_transport();
    f64_negation();
    f64_family();
    puts("Source-backed bytecode integration passed");
    return 0;
}
