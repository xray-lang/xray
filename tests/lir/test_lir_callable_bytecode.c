/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_callable_bytecode.c - Real environments and borrowed stack captures
 */
#include "lir/xlir_builder.h"
#include "lir/xlir_internal.h"
#include "lir/xlir_verify.h"
#include "vm/register/xrbc_lir.h"
#include "xray_native_function.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_LIR_OK)

typedef struct CArrayFixture { XrLirModule *module; } CArrayFixture;
static XrLirValue ca_value(CArrayFixture *c, XrLirBlock block, XrLirOp op,
    XrLirType type, const XrLirValue *arguments, uint32_t count) {
    XrLirValue result = {0};
    XrLirInstruction instruction = {.op=op, .type=type, .operands=arguments, .operand_count=count};
    OK(xr_lir_emit_value(c->module, block, &instruction, &result));
    return result;
}
static XrLirValue ca_constant(CArrayFixture *c, XrLirBlock block, int64_t bits) {
    XrLirValue result = {0};
    XrLirInstruction instruction = {.op=XR_LIR_CONST, .type=XR_LIR_I64, .bits=(uint64_t)bits};
    OK(xr_lir_emit_value(c->module, block, &instruction, &result));
    return result;
}
static XrLirValue ca_binary(CArrayFixture *c, XrLirBlock block, XrLirOp op,
    XrLirValue left, XrLirValue right) {
    XrLirValue arguments[] = {left,right};
    return ca_value(c,block,op,XR_LIR_I64,arguments,2);
}
static void ca_return(CArrayFixture *c, XrLirBlock block, XrLirValue value) {
    XrLirInstruction instruction = {.op=XR_LIR_RET, .type=XR_LIR_UNIT, .operands=&value, .operand_count=1};
    OK(xr_lir_set_terminator(c->module,block,&instruction));
}
#include "lir_callable_fixture.h"
#include "lir_callable_bytecode_cases.h"

static XrRbcProgram *compile(XrLirModule *module, XrLirFunction root) {
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic = {0};
    XrRbcCompileStatus status = xr_rbc_compile(module,root,&program,&diagnostic);
    if (status != XR_RBC_COMPILE_OK) fprintf(stderr,"compile %u: %s\n",(unsigned)status,diagnostic.message);
    CHECK(status == XR_RBC_COMPILE_OK && program);
    return program;
}
static void execute(XrRbcProgram *program, uint64_t expected) {
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program,NULL,0,&execution) == XR_RBC_OK);
    xr_rbc_program_destroy(program);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    uint64_t bits = 0;
    CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK && xr_rbc_outcome_bits(outcome,&bits) && bits == expected);
    xr_rbc_outcome_free(&outcome);
}
static void pure_captures(void) {
    for (unsigned capture = 0; capture < 3; ++capture) {
        XrLirModule *module = NULL;
        XrLirFunction root = {0};
        if (capture) callable_fixture_capture(&module,&root,capture);
        else callable_fixture(&module,&root);
        XrRbcProgram *program = compile(module,root);
        xr_lir_module_destroy(module);
        if (capture) callable_frame_receiving(program);
        else {
            callable_raw_receiving(program);
            callable_narrow_argument(program);
        }
        /* Raw callers keep these exact immutable assets alive themselves;
         * losing the optional lease does not grant an escaping Fn lifetime. */
        XrRbcModule raw = *xr_rbc_program_view(program);
        raw.code_lease = (XrRbcCodeLease){0};
        XrRbcExecution *borrowed = NULL;
        CHECK(xr_rbc_module_prepare(&raw,0,NULL,0,xr_rbc_program_locations(program),&borrowed) == XR_RBC_OK);
        XrRbcOutcome *outcome = xr_rbc_execution_run(borrowed);
        uint64_t bits = 0;
        CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK && xr_rbc_outcome_bits(outcome,&bits) && bits == 42);
        xr_rbc_execution_free(&borrowed); xr_rbc_outcome_free(&outcome);
        execute(program,42);
    }
}
static void direct_weakened_formal(void) {
    CArrayFixture c={0}; OK(xr_lir_module_create(NULL,&c.module));
    XrLirCallableParameter parameter={.type=XR_LIR_I64};
    XrLirCallableType signature={.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64,
        .flags=XR_LIR_CALLABLE_NO_SUSPEND};
    XrLirType strong=0,weak=0,scalar=XR_LIR_I64;
    OK(xr_lir_callable_type(c.module,&signature,&strong));
    signature.flags=0; OK(xr_lir_callable_type(c.module,&signature,&weak));
    XrLirFunction target={0},use={0},root={0};
    XrLirFunctionSpec spec={.name="plus_two",.name_length=8,.parameters=&scalar,.parameter_count=1,.result=scalar};
    OK(xr_lir_declare_function(c.module,&spec,&target));
    OK(xr_lir_set_closure_target(c.module,target,(XrLirClosureTarget){.callable=strong}));
    OK(xr_lir_define_function(c.module,target)); XrLirBlock block={0};
    OK(xr_lir_add_block(c.module,target,NULL,0,&block)); OK(xr_lir_set_entry(c.module,target,block));
    XrLirValue input={0}; OK(xr_lir_function_parameter(c.module,target,0,&input));
    ca_return(&c,block,ca_binary(&c,block,XR_LIR_ADD,input,ca_constant(&c,block,2)));
    XrLirType parameters[]={weak,scalar};
    spec=(XrLirFunctionSpec){.name="use_weak",.name_length=8,.parameters=parameters,.parameter_count=2,.result=scalar};
    OK(xr_lir_declare_function(c.module,&spec,&use)); OK(xr_lir_define_function(c.module,use));
    OK(xr_lir_add_block(c.module,use,NULL,0,&block)); OK(xr_lir_set_entry(c.module,use,block));
    XrLirValue arguments[2]={{0}};
    OK(xr_lir_function_parameter(c.module,use,0,&arguments[0]));
    OK(xr_lir_function_parameter(c.module,use,1,&arguments[1]));
    ca_return(&c,block,ca_value(&c,block,XR_LIR_CALL_INDIRECT,scalar,arguments,2));
    spec=(XrLirFunctionSpec){.name="root",.name_length=4,.result=scalar};
    OK(xr_lir_declare_function(c.module,&spec,&root)); OK(xr_lir_define_function(c.module,root));
    OK(xr_lir_add_block(c.module,root,NULL,0,&block)); OK(xr_lir_set_entry(c.module,root,block));
    XrLirInstruction instruction={.op=XR_LIR_FUNCTION_REF,.type=strong,.callee=target};
    OK(xr_lir_emit_value(c.module,block,&instruction,&arguments[0]));
    arguments[1]=ca_constant(&c,block,40);
    instruction=(XrLirInstruction){.op=XR_LIR_CALL,.type=scalar,.callee=use,.operands=arguments,.operand_count=2};
    XrLirValue result={0}; OK(xr_lir_emit_value(c.module,block,&instruction,&result));
    ca_return(&c,block,result); OK(xr_lir_module_finish(c.module,NULL));
    XrRbcProgram *program=compile(c.module,root);
    xr_lir_module_destroy(c.module); execute(program,42);
}
static void owned_zero_capture(void) {
    CArrayFixture c={0};
    OK(xr_lir_module_create(NULL,&c.module));
    XrLirCallableParameter parameter={.type=XR_LIR_I64};
    XrLirCallableType signature={.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64};
    XrLirType callable=0,integer=XR_LIR_I64;
    OK(xr_lir_callable_type(c.module,&signature,&callable));
    XrLirFunction target={0},root={0};
    XrLirFunctionSpec spec={.name="plus_one",.name_length=8,.parameters=&integer,.parameter_count=1,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module,&spec,&target));
    OK(xr_lir_set_closure_target(c.module,target,(XrLirClosureTarget){.callable=callable}));
    OK(xr_lir_define_function(c.module,target));
    XrLirBlock block={0};
    OK(xr_lir_add_block(c.module,target,NULL,0,&block));
    OK(xr_lir_set_entry(c.module,target,block));
    XrLirValue input={0};
    OK(xr_lir_function_parameter(c.module,target,0,&input));
    ca_return(&c,block,ca_binary(&c,block,XR_LIR_ADD,input,ca_constant(&c,block,1)));
    spec=(XrLirFunctionSpec){.name="root",.name_length=4,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module,&spec,&root)); OK(xr_lir_define_function(c.module,root));
    OK(xr_lir_add_block(c.module,root,NULL,0,&block)); OK(xr_lir_set_entry(c.module,root,block));
    XrLirInstruction instruction={.op=XR_LIR_FUNCTION_REF,.type=callable,.callee=target,.bits=XR_LIR_CLOSURE_OWNED};
    XrLirValue function={0};
    OK(xr_lir_emit_value(c.module,block,&instruction,&function));
    XrLirValue arguments[]={function,ca_constant(&c,block,41)};
    ca_return(&c,block,ca_value(&c,block,XR_LIR_CALL_INDIRECT,XR_LIR_I64,arguments,2));
    OK(xr_lir_module_finish(c.module,NULL));
    XrRbcProgram *program=compile(c.module,root);
    const XrRbcModule *module=xr_rbc_program_view(program);
    const XrRbcValueCode *code=module->borrowed_value_codes[0];
    bool found=false;
    for (size_t n=0;n<code->access_count;++n) {
        XrRbcValueAccess access=code->accesses[n];
        if (access.operation!=XR_RBC_VALUE_FUNCTION_NEW) continue;
        const XrLirValueHome *home=&code->homes->values[access.result];
        CHECK(home->transport==XR_LIR_CALLABLE_TRANSPORT_OWNED && home->owner_count==1 && home->environment_type);
        const XrtValueImageType *environment=&module->borrowed_value_image->types[home->environment_type-1];
        CHECK(environment->kind==XRT_VALUE_ENVIRONMENT && !environment->capture_count && !environment->size);
        found=true;
    }
    CHECK(found && !module->functions[0].environment_size);
    XrRbcModule raw=*module;
    raw.code_lease=(XrRbcCodeLease){0};
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_module_prepare(&raw,0,NULL,0,xr_rbc_program_locations(program),&execution)==XR_RBC_INVALID && !execution);
    xr_lir_module_destroy(c.module);
    execute(program,42);
}
static void supported_and_rejected_captures(void) {
    XrLirModule *module=NULL; XrLirFunction root={0};
    callable_fixture_capture(&module,&root,3);
    XrRbcProgram *program=compile(module,root);
    xr_lir_module_destroy(module); execute(program,42);
    callable_fixture_capture(&module,&root,4);
    program=compile(module,root);
    XrRbcModule raw=*xr_rbc_program_view(program);
    raw.code_lease=(XrRbcCodeLease){0};
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_module_prepare(&raw,0,NULL,0,xr_rbc_program_locations(program),&execution)==XR_RBC_INVALID && !execution);
    xr_lir_module_destroy(module); execute(program,42);
}
static void owned_duplicate_edge(void) {
    CArrayFixture c={0};
    OK(xr_lir_module_create(NULL,&c.module));
    XrLirCallableParameter parameter={.type=XR_LIR_I64};
    XrLirCallableType signature={.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64};
    XrLirType callable=0,integer=XR_LIR_I64;
    OK(xr_lir_callable_type(c.module,&signature,&callable));
    XrLirFunction target={0},root={0};
    XrLirFunctionSpec spec={.name="identity",.name_length=8,.parameters=&integer,.parameter_count=1,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module,&spec,&target));
    OK(xr_lir_set_closure_target(c.module,target,(XrLirClosureTarget){.callable=callable}));
    OK(xr_lir_define_function(c.module,target));
    XrLirBlock entry={0};
    OK(xr_lir_add_block(c.module,target,NULL,0,&entry)); OK(xr_lir_set_entry(c.module,target,entry));
    XrLirValue input={0}; OK(xr_lir_function_parameter(c.module,target,0,&input)); ca_return(&c,entry,input);
    spec=(XrLirFunctionSpec){.name="duplicate",.name_length=9,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module,&spec,&root)); OK(xr_lir_define_function(c.module,root));
    OK(xr_lir_add_block(c.module,root,NULL,0,&entry)); OK(xr_lir_set_entry(c.module,root,entry));
    XrLirType types[]={callable,callable}; XrLirBlock join={0};
    OK(xr_lir_add_block(c.module,root,types,2,&join));
    XrLirInstruction instruction={.op=XR_LIR_FUNCTION_REF,.type=callable,.callee=target,.bits=XR_LIR_CLOSURE_OWNED};
    XrLirValue function={0}; OK(xr_lir_emit_value(c.module,entry,&instruction,&function));
    XrLirValue number=ca_constant(&c,entry,14),duplicates[]={function,function};
    instruction=(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,.edges={{join,duplicates,2}},.edge_count=1};
    OK(xr_lir_set_terminator(c.module,entry,&instruction));
    XrLirValue sources[]={{0},{0},function};
    OK(xr_lir_block_parameter(c.module,join,0,&sources[0]));
    OK(xr_lir_block_parameter(c.module,join,1,&sources[1]));
    XrLirValue sum={0};
    for (unsigned n=0;n<3;++n) {
        XrLirValue arguments[]={sources[n],number};
        XrLirValue result=ca_value(&c,join,XR_LIR_CALL_INDIRECT,XR_LIR_I64,arguments,2);
        sum=n ? ca_binary(&c,join,XR_LIR_ADD,sum,result) : result;
    }
    ca_return(&c,join,sum);
    OK(xr_lir_module_finish(c.module,NULL));
    XrRbcProgram *program=compile(c.module,root);
    xr_lir_module_destroy(c.module); execute(program,42);
}

static XrLirValue scope_enter(CArrayFixture *c, XrLirBlock block, XrLirValue parent) {
    return ca_value(c,block,XR_LIR_FUNCTION_SCOPE_ENTER,XR_LIR_FUNCTION_SCOPE,
        parent.id ? &parent : NULL,parent.id ? 1u : 0u);
}
static void scope_leave(CArrayFixture *c, XrLirBlock block, XrLirValue scope) {
    XrLirInstruction instruction = {.op=XR_LIR_FUNCTION_SCOPE_LEAVE,.type=XR_LIR_UNIT,
        .operands=&scope,.operand_count=1};
    OK(xr_lir_emit_effect(c->module,block,&instruction));
}
static void scope_keep_formal(bool owned) {
    CArrayFixture c={0}; OK(xr_lir_module_create(NULL,&c.module));
    XrLirCallableParameter parameter={.type=XR_LIR_I64};
    XrLirCallableType signature={.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64};
    XrLirType callable=0,scalar=XR_LIR_I64;
    OK(xr_lir_callable_type(c.module,&signature,&callable));
    XrLirFunction target={0},helper={0},root={0};
    XrLirFunctionSpec spec={.name="plus_one",.name_length=8,.parameters=&scalar,.parameter_count=1,.result=scalar};
    OK(xr_lir_declare_function(c.module,&spec,&target));
    OK(xr_lir_set_closure_target(c.module,target,(XrLirClosureTarget){.callable=callable}));
    OK(xr_lir_define_function(c.module,target)); XrLirBlock block={0};
    OK(xr_lir_add_block(c.module,target,NULL,0,&block)); OK(xr_lir_set_entry(c.module,target,block));
    XrLirValue input={0}; OK(xr_lir_function_parameter(c.module,target,0,&input));
    ca_return(&c,block,ca_binary(&c,block,XR_LIR_ADD,input,ca_constant(&c,block,1)));
    XrLirType parameters[]={callable,scalar};
    spec=(XrLirFunctionSpec){.name="keep_formal",.name_length=11,.parameters=parameters,.parameter_count=2,.result=scalar};
    OK(xr_lir_declare_function(c.module,&spec,&helper)); OK(xr_lir_define_function(c.module,helper));
    OK(xr_lir_add_block(c.module,helper,NULL,0,&block)); OK(xr_lir_set_entry(c.module,helper,block));
    XrLirValue callback={0}; OK(xr_lir_function_parameter(c.module,helper,0,&callback));
    OK(xr_lir_function_parameter(c.module,helper,1,&input));
    XrLirValue outer=scope_enter(&c,block,(XrLirValue){0}),inner=scope_enter(&c,block,outer);
    XrLirValue operands[]={outer,callback};
    XrLirValue alias=ca_value(&c,block,XR_LIR_FUNCTION_BIND,callable,operands,2);
    operands[0]=inner; operands[1]=alias;
    alias=ca_value(&c,block,XR_LIR_FUNCTION_SCOPE_LEAVE,callable,operands,2);
    operands[0]=outer; operands[1]=alias;
    alias=ca_value(&c,block,XR_LIR_FUNCTION_SCOPE_LEAVE,callable,operands,2);
    XrLirValue arguments[]={alias,input};
    ca_return(&c,block,ca_value(&c,block,XR_LIR_CALL_INDIRECT,scalar,arguments,2));
    spec=(XrLirFunctionSpec){.name="root",.name_length=4,.result=scalar};
    OK(xr_lir_declare_function(c.module,&spec,&root)); OK(xr_lir_define_function(c.module,root));
    OK(xr_lir_add_block(c.module,root,NULL,0,&block)); OK(xr_lir_set_entry(c.module,root,block));
    XrLirInstruction instruction={.op=XR_LIR_FUNCTION_REF,.type=callable,.callee=target,
        .bits=owned ? XR_LIR_CLOSURE_OWNED : XR_LIR_CLOSURE_FRAME};
    OK(xr_lir_emit_value(c.module,block,&instruction,&arguments[0])); arguments[1]=ca_constant(&c,block,41);
    instruction=(XrLirInstruction){.op=XR_LIR_CALL,.type=scalar,.callee=helper,.operands=arguments,.operand_count=2};
    XrLirValue result={0}; OK(xr_lir_emit_value(c.module,block,&instruction,&result));
    ca_return(&c,block,result); OK(xr_lir_module_finish(c.module,NULL));
    XrRbcProgram *program=compile(c.module,root); xr_lir_module_destroy(c.module);
    if (!owned) { CallableRawFixture raw=callable_raw_fixture(program); callable_raw_positive(&raw,program); callable_raw_free(&raw); }
    execute(program,42);
}
static XrRbcProgram *scoped_code(bool owned) {
    CArrayFixture c = {0};
    OK(xr_lir_module_create(NULL,&c.module));
    XrLirCallableParameter parameter = {.type=XR_LIR_I64};
    XrLirCallableType signature = {.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64};
    XrLirType callable = 0, integer = XR_LIR_I64;
    OK(xr_lir_callable_type(c.module,&signature,&callable));
    XrLirFunction target = {0}, root = {0};
    XrLirFunctionSpec spec = {.name="plus_one",.name_length=8,.parameters=&integer,
        .parameter_count=1,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module,&spec,&target));
    OK(xr_lir_set_closure_target(c.module,target,(XrLirClosureTarget){.callable=callable}));
    OK(xr_lir_define_function(c.module,target));
    XrLirBlock block = {0};
    OK(xr_lir_add_block(c.module,target,NULL,0,&block));
    OK(xr_lir_set_entry(c.module,target,block));
    XrLirValue input = {0};
    OK(xr_lir_function_parameter(c.module,target,0,&input));
    ca_return(&c,block,ca_binary(&c,block,XR_LIR_ADD,input,ca_constant(&c,block,1)));
    spec = (XrLirFunctionSpec){.name="root",.name_length=4,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module,&spec,&root));
    OK(xr_lir_define_function(c.module,root));
    XrLirBlock entry = {0}, left = {0}, right = {0}, join = {0};
    OK(xr_lir_add_block(c.module,root,NULL,0,&entry));
    OK(xr_lir_add_block(c.module,root,NULL,0,&left));
    OK(xr_lir_add_block(c.module,root,NULL,0,&right));
    OK(xr_lir_add_block(c.module,root,NULL,0,&join));
    OK(xr_lir_set_entry(c.module,root,entry));
    XrLirValue outer = scope_enter(&c,entry,(XrLirValue){0});
    XrLirValue inner = scope_enter(&c,entry,outer), code = {0};
    XrLirInstruction instruction = {.op=XR_LIR_FUNCTION_REF,.type=callable,.callee=target,
        .bits=owned ? XR_LIR_CLOSURE_OWNED : XR_LIR_CLOSURE_FRAME};
    OK(xr_lir_emit_value(c.module,entry,&instruction,&code));
    XrLirValue bind[] = {outer,code};
    code = ca_value(&c,entry,XR_LIR_FUNCTION_BIND,callable,bind,2);
    bind[1]=code;
    instruction=(XrLirInstruction){.op=XR_LIR_FUNCTION_BIND,.type=callable,.operands=bind,.operand_count=2,.bits=1};
    OK(xr_lir_emit_value(c.module,entry,&instruction,&code));
    XrLirValue condition = {0};
    instruction = (XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I1,.bits=1};
    OK(xr_lir_emit_value(c.module,entry,&instruction,&condition));
    instruction = (XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,.operands=&condition,
        .operand_count=1,.edges={{left,NULL,0},{right,NULL,0}},.edge_count=2};
    OK(xr_lir_set_terminator(c.module,entry,&instruction));
    XrLirBlock branches[] = {left,right};
    for (unsigned n = 0; n < 2; ++n) {
        scope_leave(&c,branches[n],inner);
        instruction = (XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
            .edges={{join,NULL,0}},.edge_count=1};
        OK(xr_lir_set_terminator(c.module,branches[n],&instruction));
    }
    XrLirValue arguments[] = {code,ca_constant(&c,join,41)};
    XrLirValue result = ca_value(&c,join,XR_LIR_CALL_INDIRECT,XR_LIR_I64,arguments,2);
    scope_leave(&c,join,outer);
    ca_return(&c,join,result);
    OK(xr_lir_module_finish(c.module,NULL));
    XrRbcProgram *program = compile(c.module,root);
    xr_lir_module_destroy(c.module);
    return program;
}
static void scope_receiving(void) {
    XrRbcProgram *program=scoped_code(false);
    const XrRbcModule *original=xr_rbc_program_view(program);
    CHECK(original->function_count==2 && original->function_scope_count==2);
    CHECK(original->function_scopes[0].count==2);
    CallableRawFixture raw=callable_raw_fixture(program);
    callable_raw_positive(&raw,program);
    const XrRbcFunction *root=&original->functions[0];
    XrRbcAggregateAccess *aggregate=xr_malloc(root->aggregate_access_count*sizeof(*aggregate));
    CHECK(aggregate);
    for (unsigned mutation=0;mutation<9;++mutation) {
        callable_raw_reset(&raw);
        XrRbcFunctionScopes scopes[2]; uint32_t parents[2];
        memcpy(scopes,original->function_scopes,sizeof(scopes));
        memcpy(parents,scopes[0].parents,sizeof(parents)); scopes[0].parents=parents;
        memcpy(aggregate,root->aggregate_accesses,root->aggregate_access_count*sizeof(*aggregate));
        raw.module.function_scopes=scopes; raw.functions[0].aggregate_accesses=aggregate;
        if (mutation==0) raw.module.function_scopes=NULL;
        if (mutation==1) --raw.module.function_scope_count;
        if (mutation==2) scopes[0].parents=NULL;
        if (mutation==3) parents[0]=1;
        if (mutation==4) parents[1]=3;
        bool changed=mutation<5;
        for (size_t n=0;n<raw.code.access_count;++n)
            if (mutation==5 && raw.accesses[n].operation==XR_RBC_VALUE_FUNCTION_ALIAS && raw.accesses[n].other) {
                raw.accesses[n].other=2; changed=true;
            }
        for (size_t n=0;n<root->aggregate_access_count;++n)
            if (mutation==6 && aggregate[n].operation==XR_RBC_FUNCTION_SCOPE_ENTER && aggregate[n].destination==2) {
                aggregate[n].source=0; changed=true;
            }
        if (mutation>=7) for (size_t pc=0;pc+1<root->code_count;++pc) {
            uint32_t word=raw.words[pc],scope=mutation==7 ? 2u : 1u;
            bool leave=(word&255)==XR_RBC_VALUE_ACCESS ?
                raw.accesses[word>>16].operation==XR_RBC_VALUE_FUNCTION_SCOPE_EXIT && raw.accesses[word>>16].other==scope :
                (word&255)==XR_RBC_AGGREGATE_ACCESS && aggregate[word>>16].operation==XR_RBC_FUNCTION_SCOPE_LEAVE &&
                aggregate[word>>16].destination==scope;
            if (!leave) continue;
            CHECK(xr_rbc_jump(&raw.words[pc],pc,pc+1,root->code_count)); changed=true; break;
        }
        CHECK(changed);
        XrRbcExecution *execution=NULL;
        CHECK(xr_rbc_module_prepare(&raw.module,0,NULL,0,xr_rbc_program_locations(program),&execution)==XR_RBC_INVALID && !execution);
    }
    callable_raw_reset(&raw);
    size_t call_pc=SIZE_MAX,leave_pc=SIZE_MAX;
    for (size_t pc=0;pc<root->code_count;++pc) {
        uint32_t word=raw.words[pc];
        if ((word&255)!=XR_RBC_VALUE_ACCESS) continue;
        XrRbcValueAccess access=raw.accesses[word>>16];
        if (access.operation==XR_RBC_VALUE_FUNCTION_CALL) call_pc=pc;
        if (access.operation==XR_RBC_VALUE_FUNCTION_SCOPE_EXIT && access.other==1) leave_pc=pc;
    }
    CHECK(call_pc!=SIZE_MAX && leave_pc!=SIZE_MAX && call_pc<leave_pc);
    uint32_t word=raw.words[call_pc]; raw.words[call_pc]=raw.words[leave_pc]; raw.words[leave_pc]=word;
    XrRbcExecution *stale=NULL;
    CHECK(xr_rbc_module_prepare(&raw.module,0,NULL,0,xr_rbc_program_locations(program),&stale)==XR_RBC_INVALID);
    CHECK(!stale);
    callable_raw_reset(&raw);
    uint32_t first_identity=UINT32_MAX; bool duplicate=false;
    for (size_t n=0;n<raw.code.access_count;++n) {
        XrRbcValueAccess *access=&raw.accesses[n];
        if (access->operation!=XR_RBC_VALUE_FUNCTION_ALIAS || access->other!=1) continue;
        if (first_identity==UINT32_MAX) first_identity=access->window;
        else { access->window=first_identity; duplicate=true; break; }
    }
    CHECK(duplicate);
    CHECK(xr_rbc_module_prepare(&raw.module,0,NULL,0,xr_rbc_program_locations(program),&stale)==XR_RBC_INVALID);
    CHECK(!stale);
    xr_free(aggregate); callable_raw_free(&raw); execute(program,42);
    execute(scoped_code(true),42);
}

int main(void) {
    pure_captures();
    direct_weakened_formal();
    owned_zero_capture();
    supported_and_rejected_captures();
    owned_duplicate_edge();
    scope_receiving();
    scope_keep_formal(false); scope_keep_formal(true);
    puts("callable bytecode: Frame borrows, real Owned environments, scope CFG and raw receiving PASS");
    return 0;
}
