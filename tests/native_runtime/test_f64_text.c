/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_f64_text.c - Independent decimal witnesses through native and RBC owners
 */
#include "lir/xlir_builder.h"
#include "lir/xlir_verify.h"
#include "base/xchecks.h"
#include "vm/register/xrbc_lir.h"
#include "runtime/native/xnative_execution.h"
#include "runtime/native/xnative_values.h"
#include "runtime/native/xnative_string_backing_ops.h"
#include "xray_native_values.h"
#include <fenv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(XR_ARCH_X86_64) || defined(XR_ARCH_X86)
#include <xmmintrin.h>
#endif

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)
#define OK(c) CHECK((c)==XR_LIR_OK)
static const struct { uint64_t bits; const char *text; } witnesses[] = {
#define F64_TEXT_32(bits, text)
#define F64_TEXT_64(bits, text) {bits,text},
#define XIR_FLOAT_FORMAT(width, bits, text) F64_TEXT_##width(bits,text)
#include "../unit/xir/xir_float_format_vectors.def"
#undef XIR_FLOAT_FORMAT
#undef F64_TEXT_64
#undef F64_TEXT_32
};
XR_STATIC_ASSERT(sizeof(witnesses)/sizeof(witnesses[0])==290,"Every original binary64 text witness is present");

static void text_equal(const XrtManagedHeader *value, const char *expected) {
    const XrtStringBacking *text=(const XrtStringBacking *)value;
    CHECK(text->length==strlen(expected) && !memcmp(text->bytes,expected,text->length));
}
static void retire_text(XrtManagedHeader **owner) {
    XrtManagedHeader *value=*owner; *owner=NULL; xrt_managed_drop(value);
}
static void text_require(XrtManagedStatus status) {
    if(status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,0);
}
typedef struct NativeText {
    unsigned index;
    XrtValueRoot strings;
    XrtManagedHeader *escaped;
    bool take;
} NativeText;
static void native_text(void *data) {
    NativeText *context=data;
    XrtManagedRoot *root=xrt_native_managed_root(0); XrtManagedFrame *frame=NULL;
    text_require(xrt_managed_frame_enter(root,1,&frame));
    const XrtBackingTypes *types=NULL;
    text_require(xrt_native_values_backings(&context->strings,&types));
    text_require(xrt_string_backing_f64_create(frame,0,types,witnesses[context->index].bits));
    XrtManagedHeader *value=NULL; text_require(xrt_managed_slot_borrow(frame,0,&value));
    text_equal(value,witnesses[context->index].text);
    if(context->take) text_require(xrt_managed_slot_take(frame,0,&context->escaped));
    text_require(xrt_managed_frame_leave(root,frame));
}
static XrLirModule *text_module(XrLirType parameter, XrLirType result, unsigned arity,
    XrLirFunction *function,XrLirStatus *rejected) {
    XrLirModule *module=NULL;
    OK(xr_lir_module_create(NULL,&module));
    XrLirFunctionSpec spec={.name="format",.name_length=6,.parameters=&parameter,
        .parameter_count=1,.result=result};
    OK(xr_lir_declare_function(module,&spec,function));
    OK(xr_lir_define_function(module,*function));
    XrLirBlock block={0};
    OK(xr_lir_add_block(module,*function,NULL,0,&block));
    OK(xr_lir_set_entry(module,*function,block));
    XrLirValue argument={0},value={0};
    OK(xr_lir_function_parameter(module,*function,0,&argument));
    XrLirValue arguments[]={argument,argument};
    XrLirInstruction format={.op=XR_LIR_STRING_F64,.type=result,.operands=arguments,.operand_count=arity};
    XrLirStatus status=xr_lir_emit_value(module,block,&format,&value);
    if(rejected) { *rejected=status;return module; }
    OK(status);
    XrLirInstruction ret={.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&value,.operand_count=1};
    OK(xr_lir_set_terminator(module,block,&ret));
    return module;
}
static XrRbcProgram *text_program(void) {
    XrLirFunction function={0};
    XrLirModule *module=text_module(XR_LIR_F64,XR_LIR_STRING,1,&function,NULL);
    OK(xr_lir_module_finish(module,NULL));
    OK(xr_lir_verify(module,NULL));
    XrRbcProgram *program=NULL;
    XrRbcCompileDiagnostic diagnostic={0};
    CHECK(xr_rbc_compile(module,function,&program,&diagnostic)==XR_RBC_COMPILE_OK && program);
    xr_lir_module_destroy(module);
    return program;
}
static XrRbcOutcome *rbc_text(XrRbcProgram *program, unsigned index) {
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_program_prepare(program,&witnesses[index].bits,1,&execution)==XR_RBC_OK && execution);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
    CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_OK);
    xr_rbc_execution_free(&execution);
    CHECK(!execution);
    return outcome;
}
static void invalid_types(void) {
    const XrLirType parameters[]={XR_LIR_I64,XR_LIR_STRING,XR_LIR_F64,XR_LIR_F64,XR_LIR_F64};
    const XrLirType results[]={XR_LIR_STRING,XR_LIR_STRING,XR_LIR_F64,XR_LIR_STRING,XR_LIR_STRING};
    const unsigned arities[]={1,1,1,0,2};
    for(unsigned i=0;i<5;++i) {
        XrLirFunction function={0};
        XrLirStatus status=XR_LIR_OK;
        XrLirModule *module=text_module(parameters[i],results[i],arities[i],&function,&status);
        CHECK(status==XR_LIR_VERIFY_ERROR);
        XrRbcProgram *program=NULL;
        CHECK(xr_rbc_compile(module,function,&program,NULL)!=XR_RBC_COMPILE_OK && !program);
        xr_lir_module_destroy(module);
    }
}
static void retired_results(XrtLocationOwner *locations) {
    for(unsigned order=0;order<2;++order) {
        XrRbcProgram *program=text_program();
        XrRbcOutcome *outcome=rbc_text(program,1);
        XrtManagedHeader *escaped=NULL;
        if(!order) CHECK(xr_rbc_outcome_take_string(outcome,&escaped) && escaped);
        xr_rbc_program_destroy(program);
        if(order) {
            const XrtManagedHeader *borrowed=NULL;
            CHECK(xr_rbc_outcome_string(outcome,&borrowed));text_equal(borrowed,witnesses[1].text);
            CHECK(xr_rbc_outcome_take_string(outcome,&escaped) && escaped);
        }
        xr_rbc_outcome_free(&outcome);text_equal(escaped,witnesses[1].text);
        retire_text(&escaped);CHECK(!escaped);
        NativeText context={.index=1,.take=true};XrtExecutionFailure failure={0};
        CHECK(xrt_run_embedded_owned(locations,&failure,native_text,&context,&context.strings)==XRT_EXECUTION_COMPLETED);
        CHECK(xrt_value_root_empty(&context.strings) && context.escaped);
        text_equal(context.escaped,witnesses[1].text);
        retire_text(&context.escaped);CHECK(!context.escaped);
    }
}
static void environment_check(int round,unsigned csr) {
    CHECK(fegetround()==round);
#if defined(XR_ARCH_X86_64) || defined(XR_ARCH_X86)
    CHECK((_mm_getcsr() & ~0x3fu)==(csr & ~0x3fu));
#else
    (void)csr;
#endif
}
static void environment_cases(XrtLocationOwner *locations) {
    fenv_t original;
    CHECK(fegetenv(&original)==0);
    const int rounds[]={FE_TONEAREST};
#if defined(XR_ARCH_X86_64) || defined(XR_ARCH_X86)
    unsigned original_csr=_mm_getcsr();
#endif
    XrRbcProgram *program=text_program();
    for(unsigned r=0;r<1;++r) for(unsigned mode=0;mode<1;++mode) {
        CHECK(fesetround(rounds[r])==0 && feclearexcept(FE_ALL_EXCEPT)==0);
        CHECK(feraiseexcept(FE_DIVBYZERO|FE_INEXACT)==0);
        unsigned csr=0;
#if defined(XR_ARCH_X86_64) || defined(XR_ARCH_X86)
        csr=(_mm_getcsr()&~UINT32_C(0x8040)) | ((mode&1)?UINT32_C(0x8000):0) |
            ((mode&2)?UINT32_C(0x40):0);
        _mm_setcsr(csr);
#else
        (void)mode;
#endif
        int round=fegetround();
        for(unsigned i=0;i<sizeof(witnesses)/sizeof(witnesses[0]);++i) {
            NativeText context={.index=i};XrtExecutionFailure failure={0};
            CHECK(xrt_run_embedded_owned(locations,&failure,native_text,&context,&context.strings)==XRT_EXECUTION_COMPLETED);
            environment_check(round,csr);
            CHECK(xrt_value_root_empty(&context.strings));
            XrRbcOutcome *outcome=rbc_text(program,i);XrtManagedHeader *escaped=NULL;
            environment_check(round,csr);
            CHECK(xr_rbc_outcome_take_string(outcome,&escaped));
            xr_rbc_outcome_free(&outcome);text_equal(escaped,witnesses[i].text);
            retire_text(&escaped);CHECK(!escaped);
            environment_check(round,csr);
        }
    }
    xr_rbc_program_destroy(program);
    CHECK(fesetenv(&original)==0);
#if defined(XR_ARCH_X86_64) || defined(XR_ARCH_X86)
    _mm_setcsr(original_csr);
#endif
}
int main(void) {
    XrtLocationOwner *locations=NULL;
    CHECK(xrt_locations_create(NULL,0,&locations)==XRT_LOCATION_OK);
    invalid_types();environment_cases(locations);retired_results(locations);
    xrt_locations_release(locations);
    puts("290 literal F64 text witnesses preserve native/RBC owners and floating state");
    return 0;
}
