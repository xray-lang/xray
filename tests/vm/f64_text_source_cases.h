/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * f64_text_source_cases.h - Complete Source text production and retired consumers
 */
#ifndef F64_TEXT_SOURCE_CASES_H
#define F64_TEXT_SOURCE_CASES_H
#include "base/xchecks.h"
static const char f64_text_source[]=
    "fn display(value: f64) -> string { return \"v=${value}\" }\n"
    "print(display(3.5))\nprint(-0.0)\n"
    "print(\"${1.0 / 0.0} ${-1.0 / 0.0} ${0.0 / 0.0}\")\n";
static const struct { uint64_t bits; const char *text; } f64_text_witnesses[]={
#define F64_TEXT_32(bits, text)
#define F64_TEXT_64(bits, text) {bits,text},
#define XIR_FLOAT_FORMAT(width, bits, text) F64_TEXT_##width(bits,text)
#include "../unit/xir/xir_float_format_vectors.def"
#undef XIR_FLOAT_FORMAT
#undef F64_TEXT_64
#undef F64_TEXT_32
};
XR_STATIC_ASSERT(sizeof(f64_text_source)-1==138,"The complete Source text remains unchanged");
XR_STATIC_ASSERT(sizeof(f64_text_witnesses)/sizeof(f64_text_witnesses[0])==290,"Every original binary64 text witness is present");
static void source_f64_text_result(const XrtString *value,unsigned index) {
    size_t length=0;
    const char *bytes=xrt_string_bytes(value,&length),*expected=f64_text_witnesses[index].text;
    CHECK(length==strlen(expected)+2 && !memcmp(bytes,"v=",2) && !memcmp(bytes+2,expected,length-2));
}
static void source_f64_text_vectors(void) {
    XrRbcProgram *program=callable(f64_text_source);
    for(unsigned run=0;run<2;++run)
        for(unsigned i=0;i<sizeof(f64_text_witnesses)/sizeof(f64_text_witnesses[0]);++i) {
            XrRbcExecution *execution=NULL;
            CHECK(xr_rbc_program_prepare(program,&f64_text_witnesses[i].bits,1,&execution)==XR_RBC_OK);
            XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
            CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_OK);
            xr_rbc_execution_free(&execution);
            XrtString *escaped=NULL;
            CHECK(xr_rbc_outcome_take_string(outcome,&escaped) && escaped);
            xr_rbc_outcome_free(&outcome);
            source_f64_text_result(escaped,i);xrt_string_owned_drop(&escaped);CHECK(!escaped);
        }
    xr_rbc_program_destroy(program);
}
static uint32_t source_f64_text_callee(const XrLirModule *module) {
    uint32_t selected=0,count=0;
    for(uint32_t n=0;n<xr_lir_function_count(module);++n) {
        XrLirFunction function={0};XrLirFunctionView view={0};
        OK(xr_lir_function_at(module,n,&function));OK(xr_lir_function_view(module,function,&view));
        if(view.parameter_count!=1 || view.parameter_types[0]!=XR_LIR_F64 || view.result!=XR_LIR_STRING)continue;
        for(uint32_t b=0;b<view.block_count;++b) {
            XrLirBlock block={0};XrLirBlockView body={0};
            OK(xr_lir_block_at(module,function,b,&block));OK(xr_lir_block_view(module,block,&body));
            for(uint32_t i=0;i<body.instruction_count;++i) {
                XrLirInstructionView instruction={0};OK(xr_lir_instruction_view(module,block,i,&instruction));
                if(instruction.op==XR_LIR_STRING_F64){selected=function.id;++count;}
            }
        }
    }
    CHECK(selected && count==1);return selected;
}
static void source_f64_text_c(const char *directory) {
    source_f64_text_vectors();
    XrLirLowered local=lower(f64_text_source,true);
    CHECK(source_f64_text_callee(local.module)==local.entry.id);xr_lir_lowered_free(&local);
    XrLirLowered whole=lower(f64_text_source,false);uint32_t callee=source_f64_text_callee(whole.module);
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrLirCRequest request={whole.module,whole.entry,resources,"source-f64-text",NULL};
    XrLirCOutput output={0};XrLirCDiagnostic diagnostic={0};
    XrLirCStatus status=xr_lir_emit_c(&request,&output,&diagnostic);
    if(status!=XR_LIR_C_OK)fprintf(stderr,"text C status=%u: %s\n",(unsigned)status,diagnostic.message);
    CHECK(status==XR_LIR_C_OK && output.entry_result==XR_LIR_I64);
    xr_lir_lowered_free(&whole);xr_compile_resources_release(resources);
    char path[1024];int length=snprintf(path,sizeof(path),"%s/whole.c",directory);
    CHECK(length>0 && (size_t)length<sizeof(path));FILE *file=fopen(path,"wb");CHECK(file);
    CHECK(fwrite(output.bytes,1,output.length,file)==output.length && fclose(file)==0);
    xr_lir_c_output_free(&output);
    printf("{\"sourceBytes\":138,\"oracleBytes\":24,\"independentVectors\":290,\"rbcEntries\":580,\"callee\":%u}\n",callee);
}
#endif // F64_TEXT_SOURCE_CASES_H
