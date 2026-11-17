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
#include "lir/xlir_map.h"
#include "vm/register/xrbc_lir.h"
#include "vm/register/xrbc_code_owner.h"
#include "xray_native_function.h"
#include "runtime/native/xnative_capture_backing.h"
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
#include "lir_callable_construction_fixture.h"

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
static void frame_construction_proofs(void) {
    for (unsigned shape=0;shape<4;++shape) {
        XrLirModule *module=NULL; XrLirFunction root={0}; XrLirValue reference={0};
        callable_construction_fixture(shape,&module,&root,&reference);
        XrRbcProgram *program=NULL; XrRbcCompileDiagnostic diagnostic={0};
        XrRbcCompileStatus status=xr_rbc_compile(module,root,&program,&diagnostic);
        if (shape>=2) {
            CHECK(status==XR_RBC_COMPILE_UNSUPPORTED && !program);
            CHECK(strstr(diagnostic.message,"single activation construction"));
            xr_lir_module_destroy(module); continue;
        }
        CHECK(status==XR_RBC_COMPILE_OK && program);
        xr_lir_module_destroy(module);
        CallableRawFixture raw=callable_raw_fixture(program);
        callable_raw_positive(&raw,program); callable_raw_free(&raw);
        execute(program,42);
    }
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
static XrLirValue map_capture_operation(CArrayFixture *c,XrLirBlock block,XrLirMapOperation role,
    XrLirType type,const XrLirValue *arguments,uint32_t count) {
    uint32_t intrinsic=0;
    OK(xr_lir_map_operation(c->module,role,role==XR_LIR_MAP_OP_SET ?
        XR_LIR_MAP_STRING_I64_REF : XR_LIR_MAP_STRING_I64,&intrinsic));
    XrLirInstruction op={.op=XR_LIR_CALL_INTRINSIC,.type=type,.intrinsic=intrinsic,
        .operands=arguments,.operand_count=count};
    XrLirValue result={0};
    if (type==XR_LIR_UNIT) OK(xr_lir_emit_effect(c->module,block,&op));
    else OK(xr_lir_emit_value(c->module,block,&op,&result));
    return result;
}
static void map_capture_drop(CArrayFixture *c,XrLirBlock block,XrLirOp operation,XrLirValue value) {
    XrLirInstruction drop={.op=operation,.type=XR_LIR_UNIT,.operands=&value,.operand_count=1};
    OK(xr_lir_emit_effect(c->module,block,&drop));
}
static XrLirModule *map_capture_fixture(unsigned mode,XrLirFunction *root) {
    CArrayFixture c={0}; OK(xr_lir_module_create(NULL,&c.module));
    OK(xr_lir_map_catalog_install(c.module,false,false,false,false,false));
    XrLirCallableType signature={.result=XR_LIR_I64};
    XrLirType reader=0,factory=0,map_type=XR_LIR_MAP_STRING_I64;
    OK(xr_lir_callable_type(c.module,&signature,&reader));
    if (mode==1) {
        signature.result=reader; OK(xr_lir_callable_type(c.module,&signature,&factory));
    }
    XrLirFunction read={0},make={0}; XrLirBlock block={0};
    XrLirFunctionSpec spec={.name="map_length",.name_length=10,.parameters=&map_type,
        .parameter_count=1,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module,&spec,&read));
    OK(xr_lir_set_closure_target(c.module,read,(XrLirClosureTarget){reader,1,false}));
    OK(xr_lir_define_function(c.module,read)); OK(xr_lir_add_block(c.module,read,NULL,0,&block));
    OK(xr_lir_set_entry(c.module,read,block)); XrLirValue map={0};
    OK(xr_lir_function_parameter(c.module,read,0,&map));
    XrLirValue length={0};
    if (mode==9) {
        XrLirFunction forward={0}; XrLirBlock body={0};
        XrLirFunctionSpec helper={.name="forward_map",.name_length=11,.parameters=&map_type,
            .parameter_count=1,.result=XR_LIR_I64};
        OK(xr_lir_declare_function(c.module,&helper,&forward)); OK(xr_lir_define_function(c.module,forward));
        OK(xr_lir_add_block(c.module,forward,NULL,0,&body)); OK(xr_lir_set_entry(c.module,forward,body));
        XrLirValue formal={0}; OK(xr_lir_function_parameter(c.module,forward,0,&formal));
        XrLirValue cell=ca_value(&c,body,XR_LIR_CELL_NEW,XR_LIR_MAP_STRING_I64_CELL,&formal,1);
        XrLirValue copied=ca_value(&c,body,XR_LIR_CELL_READ,map_type,&cell,1);
        XrLirValue result=map_capture_operation(&c,body,XR_LIR_MAP_OP_LEN,XR_LIR_I64,&copied,1);
        map_capture_drop(&c,body,XR_LIR_MAP_DROP,copied); map_capture_drop(&c,body,XR_LIR_MAP_CELL_DROP,cell);
        ca_return(&c,body,result);
        XrLirInstruction call={.op=XR_LIR_CALL,.type=XR_LIR_I64,.callee=forward,.operands=&map,.operand_count=1};
        OK(xr_lir_emit_value(c.module,block,&call,&length));
    } else {
    bool binding=mode>=6;
    if (binding) {
        XrLirValue initial=map;
        if (mode!=6) {
            XrLirMapLiteral literal={"empty-map.xr",12,NULL,0}; uint32_t ordinal=0;
            OK(xr_lir_map_literal_add(c.module,&literal,&ordinal));
            XrLirInstruction empty={.op=XR_LIR_MAP_LITERAL_NEW,.type=map_type,.bits=ordinal};
            initial=(XrLirValue){0}; OK(xr_lir_emit_value(c.module,block,&empty,&initial));
        }
        XrLirValue cell=ca_value(&c,block,XR_LIR_CELL_NEW,XR_LIR_MAP_STRING_I64_CELL,&initial,1);
        if (mode!=6) {
            map_capture_drop(&c,block,XR_LIR_MAP_DROP,initial);
            XrLirValue target=mode==8 ? ca_value(&c,block,XR_LIR_MAP_BORROW,XR_LIR_MAP_STRING_I64_REF,&cell,1) : cell;
            XrLirValue arguments[]={target,map};
            XrLirInstruction write={.op=mode==8 ? XR_LIR_MAP_REF_WRITE : XR_LIR_CELL_WRITE,
                .type=XR_LIR_UNIT,.operands=arguments,.operand_count=2};
            OK(xr_lir_emit_effect(c.module,block,&write));
        }
        map=ca_value(&c,block,XR_LIR_CELL_READ,map_type,&cell,1);
        map_capture_drop(&c,block,XR_LIR_MAP_CELL_DROP,cell);
    } else if (mode==5) {
        XrLirMapLiteralOrigin origin={1,1,1};
        XrLirMapLiteral literal={"spread-map.xr",13,&origin,1}; uint32_t ordinal=0;
        OK(xr_lir_map_literal_add(c.module,&literal,&ordinal));
        XrLirInstruction spread={.op=XR_LIR_MAP_LITERAL_NEW,.type=map_type,.bits=ordinal,
            .operands=&map,.operand_count=1};
        XrLirValue copied={0}; OK(xr_lir_emit_value(c.module,block,&spread,&copied)); map=copied;
    } else if (mode!=4) map=ca_value(&c,block,XR_LIR_MAP_RETAIN,map_type,&map,1);
    length=map_capture_operation(&c,block,XR_LIR_MAP_OP_LEN,XR_LIR_I64,&map,1);
    if (mode!=4) map_capture_drop(&c,block,XR_LIR_MAP_DROP,map);
    }
    ca_return(&c,block,length);
    if (mode==1) {
        spec=(XrLirFunctionSpec){.name="nested_map",.name_length=10,.parameters=&map_type,
            .parameter_count=1,.result=reader};
        OK(xr_lir_declare_function(c.module,&spec,&make));
        OK(xr_lir_set_closure_target(c.module,make,(XrLirClosureTarget){factory,1,false}));
        OK(xr_lir_define_function(c.module,make)); OK(xr_lir_add_block(c.module,make,NULL,0,&block));
        OK(xr_lir_set_entry(c.module,make,block)); OK(xr_lir_function_parameter(c.module,make,0,&map));
        map=ca_value(&c,block,XR_LIR_MAP_RETAIN,map_type,&map,1);
        XrLirInstruction reference={.op=XR_LIR_FUNCTION_REF,.type=reader,.callee=read,
            .operands=&map,.operand_count=1,.bits=XR_LIR_CLOSURE_OWNED};
        XrLirValue function={0}; OK(xr_lir_emit_value(c.module,block,&reference,&function));
        map_capture_drop(&c,block,XR_LIR_MAP_DROP,map); ca_return(&c,block,function);
    }
    XrLirType result_type=mode==1 ? factory : reader;
    bool snapshot=mode==2 || mode==3;
    spec=(XrLirFunctionSpec){.name="map_creator",.name_length=11,.result=snapshot ? XR_LIR_I64 : result_type};
    OK(xr_lir_declare_function(c.module,&spec,root)); OK(xr_lir_define_function(c.module,*root));
    OK(xr_lir_add_block(c.module,*root,NULL,0,&block)); OK(xr_lir_set_entry(c.module,*root,block));
    XrLirLiteral literal={0}; OK(xr_lir_add_literal(c.module,"x",1,&literal));
    XrLirInstruction op={.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=literal.id};
    XrLirValue key={0}; OK(xr_lir_emit_value(c.module,block,&op,&key));
    XrLirMapLiteralOrigin origin={0,1,1};
    XrLirMapLiteral description={"owned-map.xr",12,&origin,1}; uint32_t ordinal=0;
    OK(xr_lir_map_literal_add(c.module,&description,&ordinal));
    XrLirValue arguments[]={key,ca_constant(&c,block,1)};
    op=(XrLirInstruction){.op=XR_LIR_MAP_LITERAL_NEW,.type=map_type,.bits=ordinal,
        .operands=arguments,.operand_count=2};
    OK(xr_lir_emit_value(c.module,block,&op,&map));
    XrLirValue cell=ca_value(&c,block,XR_LIR_CELL_NEW,XR_LIR_MAP_STRING_I64_CELL,&map,1);
    op=(XrLirInstruction){.op=XR_LIR_FUNCTION_REF,.type=result_type,.callee=mode==1 ? make : read,
        .operands=&map,.operand_count=1,.bits=mode==3 ? XR_LIR_CLOSURE_FRAME : XR_LIR_CLOSURE_OWNED};
    XrLirValue function={0}; OK(xr_lir_emit_value(c.module,block,&op,&function));
    XrLirValue result=function;
    if (snapshot) {
        XrLirValue before=ca_value(&c,block,XR_LIR_CALL_INDIRECT,XR_LIR_I64,&function,1);
        XrLirLiteral other_literal={0}; OK(xr_lir_add_literal(c.module,"y",1,&other_literal));
        op=(XrLirInstruction){.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=other_literal.id};
        XrLirValue other={0}; OK(xr_lir_emit_value(c.module,block,&op,&other));
        XrLirValue ref=ca_value(&c,block,XR_LIR_MAP_BORROW,XR_LIR_MAP_STRING_I64_REF,&cell,1);
        XrLirValue update[]={ref,other,ca_constant(&c,block,2)};
        (void)map_capture_operation(&c,block,XR_LIR_MAP_OP_SET,XR_LIR_UNIT,update,3);
        XrLirValue after=ca_value(&c,block,XR_LIR_CALL_INDIRECT,XR_LIR_I64,&function,1);
        XrLirValue current=ca_value(&c,block,XR_LIR_MAP_REF_READ,map_type,&ref,1);
        length=map_capture_operation(&c,block,XR_LIR_MAP_OP_LEN,XR_LIR_I64,&current,1);
        map_capture_drop(&c,block,XR_LIR_MAP_DROP,current);
        map_capture_drop(&c,block,XR_LIR_STRING_DROP,other);
        result=ca_binary(&c,block,XR_LIR_ADD,ca_binary(&c,block,XR_LIR_ADD,
            ca_binary(&c,block,XR_LIR_MUL,before,ca_constant(&c,block,100)),
            ca_binary(&c,block,XR_LIR_MUL,after,ca_constant(&c,block,10))),length);
    }
    map_capture_drop(&c,block,XR_LIR_MAP_CELL_DROP,cell);
    map_capture_drop(&c,block,XR_LIR_MAP_DROP,map);
    map_capture_drop(&c,block,XR_LIR_STRING_DROP,key); ca_return(&c,block,result);
    OK(xr_lir_module_finish(c.module,NULL)); return c.module;
}
static void map_capture_retain_failure(const XrtFunction *function) {
    XrtEnvironmentBorrow environment={0};
    CHECK(xrt_environment_borrow_owned(function->owner,&environment)==XRT_MANAGED_OK);
    const XrtValuePlan *plan=NULL; const void *bytes=NULL;
    CHECK(xrt_environment_capture_borrow(&environment,0,&plan,&bytes)==XRT_MANAGED_OK);
    XrtValuePlanSpec spec={0}; CHECK(xrt_value_plan_view(plan,&spec)==XRT_MANAGED_OK && spec.kind==XRT_VALUE_MAP_STRING_I64);
    XrtManagedHeader *map=NULL; memcpy(&map,bytes,sizeof(map)); CHECK(map);
    int32_t references=atomic_load(&map->references); atomic_store(&map->references,-INT32_MAX);
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_owned_function_prepare(function,NULL,0,&execution)==XR_RBC_OK);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
    CHECK(xr_rbc_outcome_status(outcome)==XR_RBC_VALUE_FAILURE);
    XrXirValueStatus failure; uint32_t source; const XrtLocationOwner *locations;
    CHECK(xr_rbc_outcome_value_failure(outcome,&failure,&source,&locations) && failure==XR_XIR_VALUE_REFCOUNT_LIMIT);
    xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
    CHECK(atomic_load(&map->references)==-INT32_MAX); atomic_store(&map->references,references);
}
static void owned_map_captures(void) {
    for (unsigned mode=0;mode<10;++mode) {
        XrLirFunction root={0}; XrLirModule *input=map_capture_fixture(mode,&root);
        if (mode==3) {
            XrRbcProgram *rejected=NULL; XrRbcCompileDiagnostic diagnostic={0};
            CHECK(xr_rbc_compile(input,root,&rejected,&diagnostic)==XR_RBC_COMPILE_UNSUPPORTED && !rejected);
            CHECK(strstr(diagnostic.message,"operation has no admitted bytecode implementation"));
            xr_lir_module_destroy(input); continue;
        }
        XrRbcProgram *program=compile(input,root); xr_lir_module_destroy(input);
        const XrRbcModule *module=xr_rbc_program_view(program); bool found=false;
        for (size_t f=0;f<module->function_count;++f) if (module->functions[f].capture_count) {
            const XrRbcFunction *function=&module->functions[f];
            if (function->captures[0].kind!=XR_RBC_CALLABLE_REFERENCE) continue;
            const XrRbcValueCode *code=xr_rbc_module_value_code(module,(uint32_t)f);
            CHECK(function->captures[0].type==XR_RBC_MAP_STRING_I64 && code && code->homes && code->parameters);
            uint32_t parameter=code->parameters[0]; const XrLirValueHome *home=&code->homes->values[parameter];
            CHECK(home->parameter==1 && home->owner_count==1 && home->size==sizeof(XrtManagedHeader *) &&
                home->offset==SIZE_MAX && home->first_owner==SIZE_MAX && home->temporary_offset==SIZE_MAX &&
                home->temporary_first_owner==SIZE_MAX && code->locations[parameter].kind==XR_RBC_VALUE_NOWHERE);
            CHECK(code->homes->owned_frame_needed==(mode!=4 && mode!=9));
            found=true;
        }
        CHECK(found);
        XrRbcModule raw=*module; raw.code_lease=(XrRbcCodeLease){0}; XrRbcExecution *foreign=NULL;
        CHECK(xr_rbc_module_prepare(&raw,0,NULL,0,xr_rbc_program_locations(program),&foreign)==XR_RBC_INVALID && !foreign);
        if (mode==2) { execute(program,112); continue; }
        XrRbcExecution *execution=NULL;
        CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK); xr_rbc_program_destroy(program);
        XrRbcOutcome *outcome=xr_rbc_execution_run(execution); xr_rbc_execution_free(&execution);
        for (unsigned nested=0;nested<=(mode==1 ? 1u : 0u);++nested) {
            XrtFunction function={0};
            CHECK(xr_rbc_outcome_take_function(outcome,&function) && function.owner); xr_rbc_outcome_free(&outcome);
            if (!mode || mode>=6) map_capture_retain_failure(&function);
            CHECK(xr_rbc_owned_function_prepare(&function,NULL,0,&execution)==XR_RBC_OK);
            xrt_managed_drop(function.owner);
            outcome=xr_rbc_execution_run(execution); xr_rbc_execution_free(&execution);
        }
        uint64_t length=0;
        CHECK(xr_rbc_outcome_status(outcome)==XR_RBC_OK && xr_rbc_outcome_bits(outcome,&length) && length==1);
        xr_rbc_outcome_free(&outcome);
    }
}
#include "lir_map_borrow_fixture.h"
#include "lir_slice_boundary_root_fixture.h"
static void slice_call_boundaries(void) {
    const XrLirType elements[]={XR_LIR_I64,XR_LIR_U8};
    for (size_t family=0;family<2;++family) {
        SliceBoundaryFixture raw=slice_boundary_fixture(elements[family],false);
        OK(xr_lir_module_finish(raw.module,NULL));
        XrRbcProgram *rejected=NULL; XrRbcCompileDiagnostic diagnostic={0};
        CHECK(xr_rbc_compile(raw.module,raw.fanout,&rejected,&diagnostic)==XR_RBC_COMPILE_UNSUPPORTED && !rejected);
        xr_lir_module_destroy(raw.module);
        XrLirFunction root={0}; SliceBoundaryFixture c=slice_boundary_root_fixture(elements[family],&root);
        OK(xr_lir_module_finish(c.module,NULL)); XrRbcProgram *program=compile(c.module,root);
        const XrRbcModule *module=xr_rbc_program_view(program);
        bool reader=false,formal_edge=false,returning=false;
        for (uint32_t f=0;f<module->function_count;++f) {
            const XrRbcValueCode *code=xr_rbc_module_value_code(module,f); CHECK(code && code->homes);
            const XrLirValueHomes *homes=code->homes;
            if (homes->source_function==c.read.id) {
                CHECK(!homes->owned_frame_needed && !homes->frame.payload_size && !homes->workspace_count); reader=true;
            }
            for (size_t n=0;n<code->access_count;++n)
                CHECK(code->accesses[n].operation!=XR_RBC_VALUE_SLICE ||
                    code->accesses[n].index!=XR_LIR_SLICE_OP_FORMAL_CLEANUP);
            for (size_t n=0;n<homes->call_count;++n) if (homes->calls[n].view_result_workspace) {
                const XrLirHomeCall *call=&homes->calls[n];
                CHECK(call->view_result_parameter!=UINT32_MAX && call->view_result_source_argument);
                returning=true;
            }
            for (size_t n=0;n<homes->transfer_count;++n) {
                const XrLirHomeTransfer *edge=&homes->transfers[n];
                if (edge->source_storage!=XR_LIR_SLICE_STORAGE_BORROWED_FORMAL) continue;
                CHECK(edge->borrowed_owner_workspace<homes->workspace_count);
                const XrLirHomeWorkspace *stage=&homes->workspaces[edge->borrowed_owner_workspace];
                CHECK(stage->kind==XR_LIR_HOME_WORKSPACE_VIEW_BORROW_STAGE && stage->temporary_offset==SIZE_MAX &&
                    stage->temporary_first_owner==SIZE_MAX); formal_edge=true;
            }
        }
        CHECK(reader && formal_edge && returning);
        XrRbcModule foreign=*module; foreign.code_lease=(XrRbcCodeLease){0}; XrRbcExecution *execution=NULL;
        CHECK(xr_rbc_module_prepare(&foreign,0,NULL,0,xr_rbc_program_locations(program),&execution)==XR_RBC_INVALID && !execution);
        xr_lir_module_destroy(c.module); execute(program,68);
    }
}
static XrLirSource ca_source(void) {
    const char *path="frame-scalar-cell.xr";
    return (XrLirSource){path,strlen(path),3,1,3,9};
}
#include "lir_frame_scalar_cell_fixture.h"
static void late_frame_scalar_cells(void) {
    FrameScalarCellFixture c=frame_scalar_cell_fixture(FRAME_SCALAR_CELL_VALID);
    OK(xr_lir_module_finish(c.module,NULL));
    XrRbcProgram *program=compile(c.module,c.root);
    const XrRbcModule *module=xr_rbc_program_view(program);
    bool sparse=false;
    for (size_t f=0;f<module->function_count;++f) {
        const XrRbcFunction *function=&module->functions[f];
        if (function->cell_count!=3) continue;
        uint32_t mask=0;
        for (size_t pc=0;pc<function->code_count;++pc) {
            uint32_t word=function->code[pc];
            if ((word&255)!=XR_RBC_CELL_NEW) continue;
            CHECK((word>>16)<3 && !(mask&(1u<<(word>>16)))); mask|=1u<<(word>>16);
        }
        CHECK(mask==7); sparse=true;
    }
    CHECK(sparse); xr_lir_module_destroy(c.module); execute(program,73);
    const FrameScalarCellCase rejected[]={FRAME_SCALAR_CELL_LOOP,FRAME_SCALAR_CELL_UNINITIALIZED};
    for (size_t n=0;n<2;++n) {
        c=frame_scalar_cell_fixture(rejected[n]);
        CHECK(xr_lir_module_finish(c.module,NULL)!=XR_LIR_OK); xr_lir_module_destroy(c.module);
    }
}
#include "lir_slice_state_read_fixture.h"
#include "lir_slice_cleanup_bytecode_cases.h"
#include "lir_slice_callback_loan_fixture.h"
static void slice_callback_loan_execution(void) {
    const XrLirType elements[]={XR_LIR_I64,XR_LIR_U8};
    const SliceCallbackLoanCase shapes[]={SLICE_CALLBACK_LOAN_DIRECT,SLICE_CALLBACK_LOAN_INDIRECT,SLICE_CALLBACK_LOAN_DERIVED};
    for (size_t family=0;family<2;++family) for (size_t shape=0;shape<3;++shape) {
        SliceCallbackLoanFixture c=slice_callback_loan_fixture(elements[family],shapes[shape]);
        OK(xr_lir_module_finish(c.module,NULL));XrRbcProgram *program=compile(c.module,c.root);
        const XrRbcModule *module=xr_rbc_program_view(program);
        bool root=false,invoke=false,relay=false,forwarding=false;
        for (uint32_t f=0;f<module->function_count;++f) {
            const XrRbcValueCode *code=xr_rbc_module_value_code(module,f);CHECK(code && code->homes);
            size_t direct=0,indirect=0;
            for (size_t n=0;n<code->access_count;++n) {
                direct+=code->accesses[n].operation==XR_RBC_VALUE_CALL;
                indirect+=code->accesses[n].operation==XR_RBC_VALUE_FUNCTION_CALL;
            }
            if (code->homes->source_function==c.root.id) {
                CHECK(direct==1 && indirect==(shapes[shape]==SLICE_CALLBACK_LOAN_INDIRECT?1u:0u));root=true;
            } else if (code->homes->source_function==c.invoke.id) {CHECK(indirect==1);invoke=true;}
            else if (code->homes->source_function==c.relay.id) {CHECK(direct==1);relay=true;}
            else if (indirect) {CHECK(indirect==1 && !direct);forwarding=true;}
        }
        CHECK(root && invoke && relay && forwarding==(shapes[shape]==SLICE_CALLBACK_LOAN_INDIRECT));
        xr_lir_module_destroy(c.module);execute(program,4);
    }
}
static void slice_state_reads(void) {
    const XrLirType elements[]={XR_LIR_I64,XR_LIR_U8};
    for (size_t family=0;family<2;++family) for (unsigned mutable=0;mutable<2;++mutable)
        for (unsigned phase=SLICE_STATE_UNINITIALIZED;phase<=SLICE_STATE_READY;++phase) {
        SliceStateReadFixture c=phase==SLICE_STATE_INITIALIZING ?
            slice_state_read_fixture_phase(elements[family],mutable!=0,SLICE_STATE_INITIALIZING) :
            slice_state_read_fixture(elements[family],mutable!=0,phase==SLICE_STATE_READY);
        OK(xr_lir_module_finish(c.module,NULL)); XrRbcProgram *program=compile(c.module,c.root);
        const XrRbcModule *module=xr_rbc_program_view(program);
        const XrRbcValueCode *code=xr_rbc_module_value_code(module,0); CHECK(code && code->homes);
        const XrLirHomeBinding *binding=&code->homes->bindings[c.reference.id];
        CHECK(binding->kind==XR_LIR_HOME_BINDING_STATE_BORROW && binding->source==c.reference.id &&
            binding->offset==SIZE_MAX && binding->first_owner==SIZE_MAX);
        CHECK(code->locations[c.reference.id].kind!=XR_RBC_VALUE_NOWHERE && !code->homes->values[c.reference.id].type);
        bool read_role=false;
        for (size_t n=0;n<code->access_count;++n) {
            const XrRbcValueAccess *access=&code->accesses[n];
            if (access->operation!=XR_RBC_VALUE_SLICE || access->index!=XR_LIR_SLICE_OP_NEW) continue;
            CHECK(access->source==XR_LIR_STATE_REF && access->count==3 &&
                code->arguments[access->first]==c.reference.id); read_role=true;
        }
        CHECK(read_role); xr_lir_module_destroy(c.module);
        XrRbcExecution *execution=NULL; CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
        const XrtLocationOwner *expected_locations=xr_rbc_program_locations(program);
        xr_rbc_program_destroy(program); XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
        xr_rbc_execution_free(&execution);
        if (phase!=SLICE_STATE_UNINITIALIZED) {
            uint64_t bits=0; bool received=xr_rbc_outcome_bits(outcome,&bits);
            bool valid=xr_rbc_outcome_status(outcome)==XR_RBC_OK && received && bits==25;
            if (!valid) fprintf(stderr,"Slice State family=%u mutable=%u phase=%u: status=%u bitsReceived=%u bits=%llu\n",
                (unsigned)elements[family],mutable,phase,(unsigned)xr_rbc_outcome_status(outcome),
                (unsigned)received,(unsigned long long)bits);
            CHECK(valid);
        } else {
            XrXirValueStatus failure=XR_XIR_VALUE_OK; uint32_t source=UINT32_MAX;
            const XrtLocationOwner *locations=NULL;
            bool received=xr_rbc_outcome_value_failure(outcome,&failure,&source,&locations);
            bool valid=xr_rbc_outcome_status(outcome)==XR_RBC_VALUE_FAILURE && received &&
                failure==XR_XIR_VALUE_BAD_ARGUMENT && !source && locations==expected_locations;
            if (!valid) fprintf(stderr,"Slice State family=%u mutable=%u phase=%u: status=%u failureReceived=%u failure=%u source=%u locationsMatch=%u\n",
                (unsigned)elements[family],mutable,phase,(unsigned)xr_rbc_outcome_status(outcome),
                (unsigned)received,(unsigned)failure,source,(unsigned)(locations==expected_locations));
            CHECK(valid);
        }
        xr_rbc_outcome_free(&outcome);
    }
}
static void borrowed_map_binding_frontier(void) {
    for (unsigned operation=0;operation<6;++operation) for (unsigned retained=0;retained<2;++retained)
        for (unsigned closure_target=0;closure_target<2;++closure_target) {
        XrLirFunction root={0},helper={0};
        XrLirModule *module=map_binding_formal_fixture(operation,retained!=0,closure_target!=0,&root,&helper);
        XrRbcProgram *program=NULL; XrRbcCompileDiagnostic diagnostic={0};
        XrRbcCompileStatus status=xr_rbc_compile(module,root,&program,&diagnostic);
        if (status!=XR_RBC_COMPILE_OK) fprintf(stderr,"Map binding operation=%u retained=%u closure=%u: %s\n",
            operation,retained,closure_target,diagnostic.message);
        CHECK(status==XR_RBC_COMPILE_OK && program);
        if (closure_target) {
            const XrRbcModule *received=xr_rbc_program_view(program);
            const XrRbcCallableEntry *entries=xr_rbc_module_callable_entries(received);
            const XrtValueImage *image=xr_rbc_module_value_image(received); bool found=false;
            CHECK(entries && image);
            for (size_t f=0;f<received->function_count;++f) if (entries[f].source_function==helper.id) {
                CHECK(entries[f].signature_type && entries[f].signature_type<=image->type_count &&
                    image->types[entries[f].signature_type-1].kind==XRT_VALUE_FUNCTION && !entries[f].environment_type);
                found=true;
            }
            CHECK(found);
        }
        xr_lir_module_destroy(module); execute(program,1);
    }
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
    frame_construction_proofs();
    direct_weakened_formal();
    owned_zero_capture();
    supported_and_rejected_captures();
    owned_map_captures();
    borrowed_map_binding_frontier();
    slice_call_boundaries();
    late_frame_scalar_cells();
    slice_state_reads();
    slice_cleanup_bytecode();
    slice_callback_loan_execution();
    owned_duplicate_edge();
    scope_receiving();
    scope_keep_formal(false); scope_keep_formal(true);
    puts("callable bytecode: Frame borrows, real Owned environments, scope CFG and raw receiving PASS");
    return 0;
}
