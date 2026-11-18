/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_assertion.c - Typed effects, intrinsic entries and retired messages
 */
#include "lir/xlir_builder.h"
#include "lir/xlir_verify.h"
#include "vm/register/xrbc_lir.h"
#include "vm/register/xrbc_code_owner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#define OK(c) CHECK((c)==XR_LIR_OK)
static XrLirModule *assertion_module(bool condition, bool indirect, XrLirFunction *root) {
    XrLirModule *module=NULL; OK(xr_lir_module_create(NULL,&module));
    XrLirType parameters[]={XR_LIR_I1,XR_LIR_STRING};
    XrLirCallableParameter args[]={{.type=XR_LIR_I1},{.type=XR_LIR_STRING}};
    XrLirCallableType signature={.parameters=args,.parameter_count=2,.result=XR_LIR_UNIT,.flags=XR_LIR_CALLABLE_NO_SUSPEND};
    XrLirType strong=0,weak=0; OK(xr_lir_callable_type(module,&signature,&strong));
    signature.flags=0; OK(xr_lir_callable_type(module,&signature,&weak));
    CHECK(xr_lir_callable_invocation_source(&signature));
    XrLirFunction target={0};
    XrLirFunctionSpec spec={.name="assert-entry",.name_length=12,.parameters=parameters,.parameter_count=2,
        .result=XR_LIR_UNIT,.kind=XR_LIR_FUNCTION_EXTERNAL,.extern_kind=XR_LIR_EXTERN_ASSERT_CONDITION};
    OK(xr_lir_declare_function(module,&spec,&target));
    OK(xr_lir_set_closure_target(module,target,(XrLirClosureTarget){.callable=strong}));
    spec=(XrLirFunctionSpec){.name="root",.name_length=4,.result=XR_LIR_UNIT};
    OK(xr_lir_declare_function(module,&spec,root)); OK(xr_lir_define_function(module,*root));
    XrLirBlock block={0}; OK(xr_lir_add_block(module,*root,NULL,0,&block)); OK(xr_lir_set_entry(module,*root,block));
    XrLirValue inputs[3]={{0}};
    XrLirInstruction constant={.op=XR_LIR_CONST,.type=XR_LIR_I1,.bits=condition};
    OK(xr_lir_emit_value(module,block,&constant,&inputs[1]));
    static const uint8_t text[]={'a',0,0xe4,0xb8,0xad};
    XrLirLiteral literal={0}; OK(xr_lir_add_literal(module,text,sizeof(text),&literal));
    XrLirInstruction message={.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=literal.id};
    OK(xr_lir_emit_value(module,block,&message,&inputs[2]));
    if (indirect) {
        XrLirInstruction reference={.op=XR_LIR_FUNCTION_REF,.type=strong,.callee=target};
        OK(xr_lir_emit_value(module,block,&reference,&inputs[0]));
        XrLirValue original=inputs[0];
        XrLirInstruction weaken={.op=XR_LIR_FUNCTION_WEAKEN,.type=weak,.operands=&original,.operand_count=1};
        OK(xr_lir_emit_value(module,block,&weaken,&inputs[0]));
    }
    XrLirInstruction assertion={.op=indirect?XR_LIR_CALL_INDIRECT:XR_LIR_ASSERT_CONDITION,.type=XR_LIR_UNIT,
        .operands=indirect?inputs:inputs+1,.operand_count=indirect?3u:2u,
        .source={"assert-kernel.xr",sizeof("assert-kernel.xr")-1,7,3,7,18}};
    OK(xr_lir_emit_effect(module,block,&assertion));
    XrLirInstruction drop={.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=inputs+2,.operand_count=1};
    OK(xr_lir_emit_effect(module,block,&drop));
    XrLirInstruction ret={.op=XR_LIR_RET,.type=XR_LIR_UNIT};
    OK(xr_lir_set_terminator(module,block,&ret)); OK(xr_lir_module_finish(module,NULL));
    return module;
}
static void assertion_execute(bool condition, bool indirect) {
    XrLirFunction root={0}; XrLirModule *module=assertion_module(condition,indirect,&root);
    XrRbcProgram *program=NULL; XrRbcCompileDiagnostic diagnostic={0};
    XrRbcCompileStatus status=xr_rbc_compile(module,root,&program,&diagnostic);
    if (status!=XR_RBC_COMPILE_OK) fprintf(stderr,"assertion bytecode %u: %s\n",(unsigned)status,diagnostic.message);
    CHECK(status==XR_RBC_COMPILE_OK && program);
    const XrRbcModule *view=xr_rbc_program_view(program);
    if (indirect) {
        size_t count=xr_rbc_module_callable_entry_count(view);
        const XrRbcCallableEntry *table=xr_rbc_module_callable_entries(view);
        CHECK(count==view->function_count+1 && table[count-1].kind==XR_RBC_CALLABLE_ENTRY_ASSERT_CONDITION &&
            table[count-1].physical_function==UINT32_MAX && table[count-1].producer==xr_rbc_module_code_owner(view));
    }
    XrRbcExecution *execution=NULL; CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
    xr_lir_module_destroy(module); xr_rbc_program_destroy(program);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution); xr_rbc_execution_free(&execution);
    if (condition) CHECK(xr_rbc_outcome_status(outcome)==XR_RBC_OK);
    else {
        uint32_t code=0,source=0; const char *bytes=NULL; size_t length=0; const XrtLocationOwner *locations=NULL;
        CHECK(xr_rbc_outcome_message_panic(outcome,&code,&bytes,&length,&source,&locations));
        CHECK(code==445 && length==5 && !memcmp(bytes,"a\0\xe4\xb8\xad",5));
        const XrtSourceLocation *location=xrt_locations_get(locations,source);
        CHECK(location && location->line==7 && location->column==3 &&
            location->path_length==sizeof("assert-kernel.xr")-1 && !memcmp(location->path,"assert-kernel.xr",location->path_length));
    }
    xr_rbc_outcome_free(&outcome);
}
static void invalid_entries(void) {
    XrLirModule *module=NULL; XrLirFunction target={0}; OK(xr_lir_module_create(NULL,&module));
    XrLirType parameters[]={XR_LIR_I64,XR_LIR_STRING};
    XrLirFunctionSpec spec={.name="wrong",.name_length=5,.parameters=parameters,.parameter_count=2,
        .result=XR_LIR_UNIT,.kind=XR_LIR_FUNCTION_EXTERNAL,.extern_kind=XR_LIR_EXTERN_ASSERT_CONDITION};
    CHECK(xr_lir_declare_function(module,&spec,&target)==XR_LIR_INVALID_ARGUMENT && !target.id);
    xr_lir_module_destroy(module);
    XrLirCallableParameter args[]={{.type=XR_LIR_I1},{.type=XR_LIR_STRING}};
    XrLirCallableType signature={.parameters=args,.parameter_count=2,.result=XR_LIR_UNIT};
    CHECK(xr_lir_callable_invocation_source(&signature));
    signature.flags=XR_LIR_CALLABLE_C_ABI|XR_LIR_CALLABLE_NO_SUSPEND;
    CHECK(!xr_lir_callable_invocation_source(&signature));
    signature.flags=0; args[1].mode=XR_PARAM_MOVE; CHECK(!xr_lir_callable_invocation_source(&signature));
    args[1].mode=XR_PARAM_READ; args[0].type=XR_LIR_I64; CHECK(!xr_lir_callable_invocation_source(&signature));
}
int main(void) {
    invalid_entries(); assertion_execute(true,false); assertion_execute(false,false);
    assertion_execute(true,true); assertion_execute(false,true);
    puts("Typed assertion effects, entries, sources and retired messages passed");
    return 0;
}
