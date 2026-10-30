/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_f64_i64_conversion.c - Independent signed64 and binary64 witnesses
 */
#include "lir/xlir_builder.h"
#include "lir/xlir_c.h"
#include "lir/xlir_verify.h"
#include "runtime/native/xnative_execution.h"
#include "vm/register/xrbc_lir.h"
#include <fenv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(XR_ARCH_X86_64) || defined(XR_ARCH_X86)
#include <xmmintrin.h>
#endif

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_LIR_OK)
static const struct { unsigned width; bool sign; unsigned target; uint64_t input, output; } integers[] = {
#define XIR_F2F(source,target,input,output)
#define XIR_F2I(source,width,sign,input,status,output)
#define XIR_I2F(width,sign,target,input,output) {width,sign,target,input,output},
#include "../unit/xir/xir_float_vectors.def"
#undef XIR_F2F
#undef XIR_F2I
#undef XIR_I2F
};
static const struct { unsigned source, width; bool sign; uint64_t input; const char *status; uint64_t output; } fractions[] = {
#define XIR_F2F(source,target,input,output)
#define XIR_I2F(width,sign,target,input,output)
#define XIR_F2I(source,width,sign,input,status,output) {source,width,sign,input,#status,output},
#include "../unit/xir/xir_float_vectors.def"
#undef XIR_F2F
#undef XIR_F2I
#undef XIR_I2F
};
static const struct { uint64_t input, output; bool range; } boundaries[] = {
#define XR_F64_I64_BOUNDARY(input,output,range) {UINT64_C(0x##input),UINT64_C(0x##output),range},
#include "../vm/f64_i64_boundaries.def"
#undef XR_F64_I64_BOUNDARY
};
static int64_t signed_bits(uint64_t value) {
    return value <= INT64_MAX ? (int64_t)value : -1 - (int64_t)(UINT64_MAX - value);
}
static void environment_check(int round, int flags, unsigned csr) {
    CHECK(fegetround() == round && fetestexcept(FE_ALL_EXCEPT) == flags);
#if defined(XR_ARCH_X86_64) || defined(XR_ARCH_X86)
    CHECK(_mm_getcsr() == csr);
#else
    (void)csr;
#endif
}
static XrLirModule *conversion_module(XrLirOp op, XrLirType parameter,
    XrLirType result, unsigned arity, bool unused, XrLirFunction *function,
    XrLirStatus *emitted) {
    XrLirModule *module = NULL; OK(xr_lir_module_create(NULL,&module));
    XrLirFunctionSpec spec = {.name="convert",.name_length=7,.parameters=&parameter,
        .parameter_count=1,.result=unused?XR_LIR_I64:result};
    OK(xr_lir_declare_function(module,&spec,function)); OK(xr_lir_define_function(module,*function));
    XrLirBlock block = {0}; OK(xr_lir_add_block(module,*function,NULL,0,&block));
    OK(xr_lir_set_entry(module,*function,block));
    XrLirValue argument = {0}, value = {0}; OK(xr_lir_function_parameter(module,*function,0,&argument));
    XrLirValue operands[] = {argument,argument};
    XrLirInstruction cast = {.op=op,.type=result,.operands=operands,.operand_count=arity,
        .source={"convert.xr",10,7,9,7,19}};
    XrLirStatus status = xr_lir_emit_value(module,block,&cast,&value);
    if (emitted) *emitted=status;
    if (status == XR_LIR_OK) {
        if (unused) {
            XrLirInstruction constant = {.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=17};
            OK(xr_lir_emit_value(module,block,&constant,&value));
        }
        XrLirInstruction ret = {.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&value,.operand_count=1};
        OK(xr_lir_set_terminator(module,block,&ret));
    }
    return module;
}
static XrRbcProgram *conversion_program(XrLirOp op, bool unused) {
    XrLirFunction function = {0}; bool to_float=op==XR_LIR_ITOF;
    XrLirModule *module=conversion_module(op,to_float?XR_LIR_I64:XR_LIR_F64,
        to_float?XR_LIR_F64:XR_LIR_I64,1,unused,&function,NULL);
    OK(xr_lir_module_finish(module,NULL)); OK(xr_lir_verify(module,NULL));
    XrRbcProgram *program=NULL; XrRbcCompileDiagnostic diagnostic={0};
    CHECK(xr_rbc_compile(module,function,&program,&diagnostic)==XR_RBC_COMPILE_OK && program);
    xr_lir_module_destroy(module); return program;
}
static void rbc_result(XrRbcProgram *program,uint64_t input,uint64_t expected,bool range) {
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_program_prepare(program,&input,1,&execution)==XR_RBC_OK && execution);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution); CHECK(outcome);
    xr_rbc_execution_free(&execution); CHECK(!execution);
    uint64_t bits=UINT64_C(0x123456789abcdef0);
    if (range) {
        CHECK(xr_rbc_outcome_status(outcome)==XR_RBC_ARITHMETIC_PANIC);
        XrtArithmeticPanic facts={0}; const XrtLocationOwner *locations=NULL;
        CHECK(xr_rbc_outcome_arithmetic_panic(outcome,&facts,&locations));
        CHECK(facts.code==422 && facts.source_id==1 && locations);
        const XrtSourceLocation *location=xrt_locations_get(locations,1);
        CHECK(location && location->line==7 && location->column==9 && location->path_length==10);
        CHECK(!memcmp(location->path,"convert.xr",10));
        CHECK(!xr_rbc_outcome_bits(outcome,&bits) && bits==UINT64_C(0x123456789abcdef0));
    } else CHECK(xr_rbc_outcome_status(outcome)==XR_RBC_OK && xr_rbc_outcome_bits(outcome,&bits) && bits==expected);
    xr_rbc_outcome_free(&outcome); CHECK(!outcome);
}
typedef struct NativeInteger { uint64_t input; int64_t output; bool after; } NativeInteger;
static void native_integer(void *data) {
    NativeInteger *context=data;
    context->output=xrt_f64_to_i64(context->input,1);
    context->after=true;
}
static void native_result(XrtLocationOwner *locations,uint64_t input,uint64_t expected,bool range) {
    NativeInteger context={input,INT64_C(123456789),false};
    XrtExecutionFailure failure; memset(&failure,0xa5,sizeof(failure));
    unsigned char original[sizeof(failure)]; memcpy(original,&failure,sizeof(failure));
    XrtExecutionStatus status=xrt_run_embedded(locations,&failure,native_integer,&context);
    if (range) {
        CHECK(status==XRT_EXECUTION_FAILURE && failure.kind==XRT_FAILURE_ARITHMETIC_PANIC);
        CHECK(failure.detail.panic.code==422 && failure.detail.panic.source_id==1);
        CHECK(!context.after && context.output==INT64_C(123456789));
    } else CHECK(status==XRT_EXECUTION_COMPLETED && context.after &&
        (uint64_t)context.output==expected && !memcmp(&failure,original,sizeof(failure)));
}
static void environment_cases(XrtLocationOwner *locations) {
    fenv_t original; CHECK(fegetenv(&original)==0);
    const int rounds[]={FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
#if defined(XR_ARCH_X86_64) || defined(XR_ARCH_X86)
    unsigned original_csr=_mm_getcsr();
#endif
    XrRbcProgram *to_float=conversion_program(XR_LIR_ITOF,false);
    XrRbcProgram *to_integer=conversion_program(XR_LIR_FTOI,false);
    for (unsigned r=0;r<4;++r) for (unsigned mode=0;mode<4;++mode) {
        CHECK(fesetround(rounds[r])==0 && feclearexcept(FE_ALL_EXCEPT)==0);
        CHECK(feraiseexcept(FE_DIVBYZERO|FE_INEXACT)==0); unsigned csr=0;
#if defined(XR_ARCH_X86_64) || defined(XR_ARCH_X86)
        csr=(_mm_getcsr()&~UINT32_C(0x8040))|((mode&1)?UINT32_C(0x8000):0)|((mode&2)?UINT32_C(0x40):0);
        _mm_setcsr(csr);
#else
        (void)mode;
#endif
        int flags=fetestexcept(FE_ALL_EXCEPT),round=fegetround(); unsigned first=0,second=0;
        for (size_t i=0;i<sizeof(integers)/sizeof(*integers);++i) {
            if (integers[i].width!=64 || !integers[i].sign || integers[i].target!=64) continue;
            ++first; CHECK(xrt_i64_to_f64(signed_bits(integers[i].input))==integers[i].output);
            environment_check(round,flags,csr); rbc_result(to_float,integers[i].input,integers[i].output,false);
            environment_check(round,flags,csr);
        }
        for (size_t i=0;i<sizeof(fractions)/sizeof(*fractions);++i) {
            if (fractions[i].source!=64 || fractions[i].width!=64 || !fractions[i].sign) continue;
            ++second; bool range=!strcmp(fractions[i].status,"XR_XIR_NUMERIC_RANGE");
            CHECK(range || !strcmp(fractions[i].status,"XR_XIR_NUMERIC_OK"));
            native_result(locations,fractions[i].input,fractions[i].output,range);
            environment_check(round,flags,csr); rbc_result(to_integer,fractions[i].input,fractions[i].output,range);
            environment_check(round,flags,csr);
        }
        CHECK(first==49 && second==11);
        for (size_t i=0;i<sizeof(boundaries)/sizeof(*boundaries);++i) {
            native_result(locations,boundaries[i].input,boundaries[i].output,boundaries[i].range);
            environment_check(round,flags,csr); rbc_result(to_integer,boundaries[i].input,boundaries[i].output,boundaries[i].range);
            environment_check(round,flags,csr);
        }
    }
    xr_rbc_program_destroy(to_float); xr_rbc_program_destroy(to_integer);
    CHECK(fesetenv(&original)==0);
#if defined(XR_ARCH_X86_64) || defined(XR_ARCH_X86)
    _mm_setcsr(original_csr);
#endif
}
static void invalid_types(void) {
    const XrLirType parameters[]={XR_LIR_I64,XR_LIR_F32,XR_LIR_F64,XR_LIR_F64,XR_LIR_F64};
    const XrLirType results[]={XR_LIR_I64,XR_LIR_I64,XR_LIR_F64,XR_LIR_I64,XR_LIR_I64};
    const unsigned arities[]={1,1,1,0,2};
    for (unsigned i=0;i<5;++i) {
        XrLirFunction function={0}; XrLirStatus emitted=XR_LIR_OK;
        XrLirModule *module=conversion_module(XR_LIR_FTOI,parameters[i],results[i],arities[i],false,&function,&emitted);
        CHECK(emitted!=XR_LIR_OK || xr_lir_module_finish(module,NULL)!=XR_LIR_OK || xr_lir_verify(module,NULL)!=XR_LIR_OK);
        xr_lir_module_destroy(module);
    }
    XrRbcProgram *unused=conversion_program(XR_LIR_FTOI,true);
    rbc_result(unused,UINT64_C(0x7ff0000000000000),0,true);
    rbc_result(unused,UINT64_C(0xbfe0000000000000),17,false);
    xr_rbc_program_destroy(unused);
}
static uint32_t word(XrRbcOp op,uint32_t a,uint32_t b,uint32_t c) {
    uint32_t result=0; CHECK(xr_rbc_encode(&result,(XrRbcOperands){op,a,b,c,0})); return result;
}
static void raw_admission(XrtLocationOwner *locations) {
    _Static_assert(XR_RBC_FGE==49 && XR_RBC_STRING_F64==50 && XR_RBC_ITOF==51 && XR_RBC_FTOI==52,
        "Numeric conversions append without renumbering existing bytecode");
    for (unsigned direction=0;direction<2;++direction) {
        XrRbcOp op=direction?XR_RBC_FTOI:XR_RBC_ITOF;
        uint32_t code[]={word(op,0,0,0),word(XR_RBC_RET,0,0,0)};
        const uint32_t ids[]={1,1};
        XrRbcFunction f={.code=code,.code_count=2,.register_count=1,.parameter_count=1,
            .source_ids=ids,.source_id_count=2};
        XrRbcModule module={.functions=&f,.function_count=1};
        uint64_t input=direction?UINT64_C(0xc045600000000000):42;
        XrRbcExecution *execution=NULL;
        CHECK(xr_rbc_module_prepare(&module,0,&input,1,locations,&execution)==XR_RBC_OK && execution);
        XrRbcOutcome *outcome=xr_rbc_execution_run(execution); uint64_t result=0;
        CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_OK && xr_rbc_outcome_bits(outcome,&result));
        CHECK(result==(direction?(uint64_t)INT64_C(-42):UINT64_C(0x4045000000000000)));
        xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
        f.parameter_count=0;
        CHECK(xr_rbc_module_prepare(&module,0,NULL,0,locations,&execution)==XR_RBC_INVALID && !execution);
        f.parameter_count=1; code[0]|=UINT32_C(0x01000000);
        CHECK(xr_rbc_module_prepare(&module,0,&input,1,locations,&execution)==XR_RBC_INVALID && !execution);
        code[0]=word(op,1,0,0); code[1]=word(XR_RBC_RET,1,0,0); f.register_count=2;
        const XrRbcRegisterKind kinds[]={XR_RBC_BITS,XR_RBC_CALL_WINDOW};
        f.register_kinds=kinds; f.register_kind_count=2;
        CHECK(xr_rbc_module_prepare(&module,0,&input,1,locations,&execution)==XR_RBC_INVALID && !execution);
    }
}
static uint32_t jump(size_t from,size_t to,size_t count) {
    uint32_t result=0; CHECK(xr_rbc_jump(&result,from,to,count)); return result;
}
static void raw_initialization(XrtLocationOwner *locations) {
    uint32_t load=0;
    CHECK(xr_rbc_encode(&load,(XrRbcOperands){.op=XR_RBC_LOADI,.a=1,.immediate=1}));
    for (unsigned direction=0;direction<2;++direction) {
        XrRbcOp op=direction?XR_RBC_FTOI:XR_RBC_ITOF;
        uint32_t code[]={word(XR_RBC_EQ,0,0,0),jump(1,3,11),jump(2,6,11),load,jump(4,9,11),
            word(XR_RBC_RET0,0,0,0),jump(6,7,11),jump(7,8,11),jump(8,9,11),
            word(op,0,1,0),word(XR_RBC_RET,0,0,0)};
        XrRbcFunction function={.code=code,.code_count=11,.register_count=2,.parameter_count=1};
        XrRbcModule module={.functions=&function,.function_count=1};
        uint64_t input=42; XrRbcExecution *execution=NULL;
        /* The defined edge reaches the join first. A later empty predecessor
         * must remove that guarantee and schedule a new evaluation. */
        CHECK(xr_rbc_module_prepare(&module,0,&input,1,locations,&execution)==XR_RBC_INVALID && !execution);
        code[6]=load;
        CHECK(xr_rbc_module_prepare(&module,0,&input,1,locations,&execution)==XR_RBC_OK && execution);
        XrRbcOutcome *outcome=xr_rbc_execution_run(execution); uint64_t result=UINT64_MAX;
        CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_OK && xr_rbc_outcome_bits(outcome,&result));
        CHECK(result==(direction?0:UINT64_C(0x3ff0000000000000)));
        xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
        uint32_t cycle[]={word(op,0,1,0),load,jump(2,0,4),word(XR_RBC_RET0,0,0,0)};
        function.code=cycle; function.code_count=4;
        CHECK(xr_rbc_module_prepare(&module,0,&input,1,locations,&execution)==XR_RBC_INVALID && !execution);
        uint32_t window[]={word(XR_RBC_MOV,1,0,0),word(op,0,1,0),word(XR_RBC_RET,0,0,0)};
        const XrRbcRegisterKind kinds[]={XR_RBC_BITS,XR_RBC_CALL_WINDOW};
        function.code=window; function.code_count=3; function.register_kinds=kinds; function.register_kind_count=2;
        CHECK(xr_rbc_module_prepare(&module,0,&input,1,locations,&execution)==XR_RBC_INVALID && !execution);
    }
}
static void pure_integer_c(const char *path) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrLirModule *module=NULL; OK(xr_lir_module_create(NULL,&module));
    XrLirFunctionSpec spec={.name="pure",.name_length=4,.result=XR_LIR_F64};
    XrLirFunction function={0}; OK(xr_lir_declare_function(module,&spec,&function));
    OK(xr_lir_define_function(module,function));
    XrLirBlock block={0}; OK(xr_lir_add_block(module,function,NULL,0,&block));
    OK(xr_lir_set_entry(module,function,block));
    XrLirValue argument={0},value={0};
    XrLirInstruction constant={.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=INT64_MAX};
    OK(xr_lir_emit_value(module,block,&constant,&argument));
    XrLirInstruction cast={.op=XR_LIR_ITOF,.type=XR_LIR_F64,.operands=&argument,.operand_count=1};
    OK(xr_lir_emit_value(module,block,&cast,&value));
    XrLirInstruction ret={.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&value,.operand_count=1};
    OK(xr_lir_set_terminator(module,block,&ret)); OK(xr_lir_module_finish(module,NULL));
    XrLirCRequest request={.module=module,.entry=function,.resources=resources,.translation_unit="pure-convert.xr"};
    XrLirCOutput output={0}; XrLirCDiagnostic diagnostic={0};
    CHECK(xr_lir_emit_c(&request,&output,&diagnostic)==XR_LIR_C_OK && output.entry_result==XR_LIR_F64);
    xr_lir_module_destroy(module);
    CHECK(strstr(output.bytes,"#include \"xray_native_runtime.h\"") &&
        !strstr(output.bytes,"xrt_run_standalone(") && !strstr(output.bytes,"xrl_invoke("));
    FILE *file=NULL;
#if defined(XR_OS_WINDOWS)
    CHECK(fopen_s(&file,path,"wb")==0 && file);
#else
    file=fopen(path,"wb"); CHECK(file);
#endif
    CHECK(fwrite(output.bytes,1,output.length,file)==output.length && fclose(file)==0);
    xr_lir_c_output_free(&output);
    xr_compile_resources_release(resources);
}
static void retired_failure(void) {
    XrRbcProgram *program=conversion_program(XR_LIR_FTOI,false);
    uint64_t input=UINT64_C(0xfff0000000000001); XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_program_prepare(program,&input,1,&execution)==XR_RBC_OK && execution);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution); xr_rbc_program_destroy(program);
    CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_ARITHMETIC_PANIC);
    XrtArithmeticPanic facts={0}; const XrtLocationOwner *locations=NULL;
    CHECK(xr_rbc_outcome_arithmetic_panic(outcome,&facts,&locations) && facts.code==422 && facts.source_id==1);
    const XrtSourceLocation *location=xrt_locations_get(locations,1);
    CHECK(location && location->line==7 && location->column==9 && !memcmp(location->path,"convert.xr",10));
    xr_rbc_outcome_free(&outcome); CHECK(!outcome);
}
int main(int argc,char **argv) {
    if(argc==3 && !strcmp(argv[1],"--pure-c")) {pure_integer_c(argv[2]);return 0;}
    CHECK(argc==1);
    XrtSourceLocation location={"convert.xr",10,7,9,7,19}; XrtLocationOwner *locations=NULL;
    CHECK(xrt_locations_create(&location,1,&locations)==XRT_LOCATION_OK);
    invalid_types(); raw_admission(locations); raw_initialization(locations);
    environment_cases(locations); retired_failure();
    xrt_locations_release(locations);
    puts("49 signed64 conversions,11 original truncations and26 independent boundaries preserve native/RBC environments");
    return 0;
}
