/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_binding_homes.c - Stable binding owners and instruction-local pending slots
 */
#include "lir/xlir_value_homes.h"
#include "lir/xlir_state.h"
#include "lir/xlir_cell.h"
#include "lir/xlir_value_image.h"
#include "lir/xlir_rebuild.h"
#include "lir/xlir_inline.h"
#include "lir/xlir_sccp.h"
#include "lir/xlir_string_elide.h"
#include "xray_native_function.h"
#include "runtime/native/xnative_array_backing.h"
#include "runtime/native/xnative_map_backing.h"
#include "runtime/native/xnative_capture_backing.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); abort(); } } while (0)
#define OK(c) CHECK((c) == XR_LIR_OK)
#define HOME(c) CHECK((c) == XR_LIR_VALUE_LAYOUT_OK)
#define MANAGED(c) CHECK((c) == XRT_MANAGED_OK)
#include "lir_array_fixture.h"
#include "lir_array_repeat_fixture.h"
#include "lir_array_ref_fixture.h"
#include "lir_array_cell_cfg_cases.h"
#include "lir_array_block_cell_cases.h"

typedef struct CArrayFixture { XrLirModule *module; XrLirType element; } CArrayFixture;
static XrLirValue ca_value(CArrayFixture *c, XrLirBlock block, XrLirOp op,
    XrLirType type, const XrLirValue *args, uint32_t count) {
    XrLirValue result={0}; XrLirInstruction instruction={.op=op,.type=type,.operands=args,.operand_count=count};
    OK(xr_lir_emit_value(c->module,block,&instruction,&result)); return result;
}
static XrLirValue number(CArrayFixture *c,XrLirBlock block,XrLirType type,uint64_t bits) {
    XrLirValue result={0}; XrLirInstruction instruction={.op=XR_LIR_CONST,.type=type,.bits=bits};
    OK(xr_lir_emit_value(c->module,block,&instruction,&result)); return result;
}
static XrLirValue ca_constant(CArrayFixture *c,XrLirBlock block,int64_t value) { return number(c,block,XR_LIR_I64,(uint64_t)value); }
static XrLirValue ca_binary(CArrayFixture *c,XrLirBlock block,XrLirOp op,XrLirValue left,XrLirValue right) {
    XrLirValue args[]={left,right}; return ca_value(c,block,op,XR_LIR_I64,args,2);
}
static void ca_effect(CArrayFixture *c,XrLirBlock block,XrLirOp op,XrLirValue value) {
    XrLirInstruction instruction={.op=op,.type=XR_LIR_UNIT,.operands=&value,.operand_count=1};
    OK(xr_lir_emit_effect(c->module,block,&instruction));
}
static void ca_return(CArrayFixture *c,XrLirBlock block,XrLirValue value) {
    XrLirInstruction instruction={.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=value.id ? &value : NULL,
        .operand_count=value.id ? 1u : 0u}; OK(xr_lir_set_terminator(c->module,block,&instruction));
}
static XrLirValue ca_text(CArrayFixture *c,XrLirBlock block,const char *bytes,size_t count) {
    XrLirLiteral literal={0}; XrLirValue result={0}; OK(xr_lir_add_literal(c->module,bytes,count,&literal));
    XrLirInstruction instruction={.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=literal.id};
    OK(xr_lir_emit_value(c->module,block,&instruction,&result)); return result;
}
#include "lir_map_fixture.h"
#include "lir_callable_fixture.h"
#include "lir_callable_factory_fixture.h"
#include "lir_string_fixture.h"
static XrLirValue string_role(CArrayFixture *c,XrLirBlock block,XrLirStringOperation operation,
    XrLirType result,const XrLirValue *arguments,uint32_t count) {
    uint32_t id=0; OK(xr_lir_string_operation(c->module,operation,&id));
    XrLirValue value={0}; XrLirInstruction instruction={.op=XR_LIR_CALL_INTRINSIC,.type=result,
        .intrinsic=id,.operands=arguments,.operand_count=count};
    if (result==XR_LIR_UNIT) OK(xr_lir_emit_effect(c->module,block,&instruction));
    else OK(xr_lir_emit_value(c->module,block,&instruction,&value));
    return value;
}
static XrLirModule *string_roles_fixture(bool borrowed_rhs) {
    CArrayFixture c={0}; OK(xr_lir_module_create(NULL,&c.module));
    OK(xr_lir_string_catalog_install(c.module,false,false,false));
    XrLirType parameters[]={XR_LIR_STRING_CELL,XR_LIR_STRING_REF,XR_LIR_STRING};
    XrLirFunction function={0}; XrLirFunctionSpec spec={.name="string_roles",.name_length=12,
        .parameters=parameters,.parameter_count=borrowed_rhs ? 3u : 2u,.result=XR_LIR_UNIT};
    OK(xr_lir_declare_function(c.module,&spec,&function)); OK(xr_lir_define_function(c.module,function));
    XrLirBlock block={0}; OK(xr_lir_add_block(c.module,function,NULL,0,&block));
    OK(xr_lir_set_entry(c.module,function,block)); XrLirValue inputs[3]={{0}};
    for (uint32_t p=0;p<spec.parameter_count;++p) OK(xr_lir_function_parameter(c.module,function,p,&inputs[p]));
    XrLirValue text=borrowed_rhs ? inputs[2] : ca_text(&c,block,"a\0z",3);
    XrLirValue cell=ca_value(&c,block,XR_LIR_CELL_NEW,XR_LIR_STRING_CELL,&text,1);
    XrLirValue snapshot=ca_value(&c,block,XR_LIR_CELL_READ,XR_LIR_STRING,&cell,1),write[]={inputs[0],borrowed_rhs ? text : snapshot};
    XrLirInstruction instruction={.op=XR_LIR_CELL_WRITE,.type=XR_LIR_UNIT,.operands=write,.operand_count=2};
    OK(xr_lir_emit_effect(c.module,block,&instruction)); ca_effect(&c,block,XR_LIR_STRING_DROP,snapshot);
    XrLirValue borrowed=string_role(&c,block,XR_LIR_STRING_OP_BORROW_CELL,XR_LIR_STRING_REF,&cell,1);
    snapshot=string_role(&c,block,XR_LIR_STRING_OP_READ_REF,XR_LIR_STRING,&inputs[1],1);
    write[0]=borrowed; write[1]=borrowed_rhs ? text : snapshot;
    (void)string_role(&c,block,XR_LIR_STRING_OP_WRITE_REF,XR_LIR_UNIT,write,2);
    ca_effect(&c,block,XR_LIR_STRING_DROP,snapshot);
    XrLirValue append[]={inputs[0],text};
    (void)string_role(&c,block,XR_LIR_STRING_OP_APPEND_CELL,XR_LIR_UNIT,append,2);
    append[0]=inputs[1]; (void)string_role(&c,block,XR_LIR_STRING_OP_APPEND_REF,XR_LIR_UNIT,append,2);
    append[0]=cell; (void)string_role(&c,block,XR_LIR_STRING_OP_APPEND_CELL,XR_LIR_UNIT,append,2);
    if (!borrowed_rhs) ca_effect(&c,block,XR_LIR_STRING_DROP,text);
    ca_effect(&c,block,XR_LIR_STRING_CELL_DROP,cell);
    ca_return(&c,block,(XrLirValue){0}); OK(xr_lir_module_finish(c.module,NULL)); return c.module;
}
static XrLirModule *string_state_fixture(bool borrowed_rhs) {
    CArrayFixture c={0}; OK(xr_lir_module_create(NULL,&c.module));
    OK(xr_lir_string_catalog_install(c.module,false,false,false));
    XrLirStateModule module={0};
    XrLirStateSlot slot={.payload=XR_LIR_STRING,.mutable=true,.size=sizeof(void *),.alignment=_Alignof(void *)};
    XrLirStateLayout layout={.modules=&module,.module_count=1,.slots=&slot,.slot_count=1,
        .payload_size=sizeof(void *),.payload_alignment=_Alignof(void *)};
    OK(xr_lir_set_state_layout(c.module,&layout));
    XrLirType parameters[]={XR_LIR_STRING,XR_LIR_STATE}; XrLirFunction function={0};
    XrLirFunctionSpec spec={.name="string_state",.name_length=12,.parameters=borrowed_rhs ? parameters : &parameters[1],
        .parameter_count=borrowed_rhs ? 2u : 1u,.result=XR_LIR_UNIT};
    OK(xr_lir_declare_function(c.module,&spec,&function)); OK(xr_lir_set_state_entry(c.module,function));
    OK(xr_lir_define_function(c.module,function)); XrLirBlock block={0};
    OK(xr_lir_add_block(c.module,function,NULL,0,&block)); OK(xr_lir_set_entry(c.module,function,block));
    XrLirValue state={0},incoming={0}; OK(xr_lir_function_parameter(c.module,function,borrowed_rhs ? 1u : 0u,&state));
    if (borrowed_rhs) OK(xr_lir_function_parameter(c.module,function,0,&incoming));
    ca_effect(&c,block,XR_LIR_MODULE_BEGIN,state);
    XrLirValue text=ca_text(&c,block,"state",5),arguments[]={state,text};
    XrLirValue reference=ca_value(&c,block,XR_LIR_STATE_CONSTRUCT,XR_LIR_STATE_REF,arguments,2);
    ca_effect(&c,block,XR_LIR_STATE_PUBLISH,reference);
    XrLirValue snapshot=ca_value(&c,block,XR_LIR_STATE_READ,XR_LIR_STRING,&reference,1);
    arguments[0]=reference; arguments[1]=borrowed_rhs ? incoming : snapshot;
    XrLirInstruction instruction={.op=XR_LIR_STATE_WRITE,.type=XR_LIR_UNIT,.operands=arguments,.operand_count=2};
    OK(xr_lir_emit_effect(c.module,block,&instruction)); ca_effect(&c,block,XR_LIR_STRING_DROP,snapshot);
    arguments[1]=text; (void)string_role(&c,block,XR_LIR_STRING_OP_APPEND_STATE,XR_LIR_UNIT,arguments,2);
    XrLirValue borrowed=string_role(&c,block,XR_LIR_STRING_OP_BORROW_STATE,XR_LIR_STRING_REF,&reference,1);
    arguments[0]=borrowed; (void)string_role(&c,block,XR_LIR_STRING_OP_APPEND_REF,XR_LIR_UNIT,arguments,2);
    ca_effect(&c,block,XR_LIR_STRING_DROP,text); ca_effect(&c,block,XR_LIR_MODULE_READY,state);
    ca_return(&c,block,(XrLirValue){0}); OK(xr_lir_module_finish(c.module,NULL)); return c.module;
}
static XrLirModule *borrowed_map_fixture(bool writes) {
    CArrayFixture c={0}; OK(xr_lir_module_create(NULL,&c.module));
    OK(xr_lir_map_catalog_install(c.module,false,false,false,false,false));
    XrLirType parameters[]={XR_LIR_MAP_STRING_I64_CELL,XR_LIR_MAP_STRING_I64_REF,XR_LIR_MAP_STRING_I64,XR_LIR_STATE};
    if (writes) {
        XrLirStateModule module={0};
        XrLirStateSlot slot={.payload=XR_LIR_MAP_STRING_I64,.mutable=true,.size=sizeof(void *),.alignment=_Alignof(void *)};
        OK(xr_lir_set_state_layout(c.module,&(XrLirStateLayout){.modules=&module,.module_count=1,.slots=&slot,
            .slot_count=1,.payload_size=sizeof(void *),.payload_alignment=_Alignof(void *)}));
    }
    XrLirFunction function={0}; XrLirFunctionSpec spec={.name="borrowed_map",.name_length=12,
        .parameters=writes ? parameters : &parameters[2],.parameter_count=writes ? 4u : 1u,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module,&spec,&function));
    OK(xr_lir_define_function(c.module,function)); XrLirBlock block={0};
    OK(xr_lir_add_block(c.module,function,NULL,0,&block)); OK(xr_lir_set_entry(c.module,function,block));
    XrLirValue inputs[4]={{0}};
    for (uint32_t p=0;p<spec.parameter_count;++p) OK(xr_lir_function_parameter(c.module,function,p,&inputs[p]));
    XrLirValue incoming=inputs[writes ? 2 : 0];
    if (writes) {
        XrLirValue cell=ca_value(&c,block,XR_LIR_CELL_NEW,XR_LIR_MAP_STRING_I64_CELL,&incoming,1);
        XrLirValue args[]={inputs[0],incoming};
        XrLirInstruction write={.op=XR_LIR_CELL_WRITE,.type=XR_LIR_UNIT,.operands=args,.operand_count=2};
        OK(xr_lir_emit_effect(c.module,block,&write));
        args[0]=inputs[1]; write.op=XR_LIR_MAP_REF_WRITE; OK(xr_lir_emit_effect(c.module,block,&write));
        XrLirValue snapshot=ca_value(&c,block,XR_LIR_CELL_READ,XR_LIR_MAP_STRING_I64,&cell,1);
        ca_effect(&c,block,XR_LIR_MAP_DROP,snapshot); ca_effect(&c,block,XR_LIR_MAP_CELL_DROP,cell);
        ca_effect(&c,block,XR_LIR_MODULE_BEGIN,inputs[3]);
        snapshot=ca_value(&c,block,XR_LIR_MAP_RETAIN,XR_LIR_MAP_STRING_I64,&incoming,1);
        args[0]=inputs[3]; args[1]=snapshot;
        XrLirValue state=ca_value(&c,block,XR_LIR_STATE_CONSTRUCT,XR_LIR_STATE_REF,args,2);
        ca_effect(&c,block,XR_LIR_STATE_PUBLISH,state); ca_effect(&c,block,XR_LIR_MAP_DROP,snapshot);
        args[0]=state; args[1]=incoming; write.op=XR_LIR_STATE_WRITE;
        OK(xr_lir_emit_effect(c.module,block,&write));
        snapshot=ca_value(&c,block,XR_LIR_STATE_READ,XR_LIR_MAP_STRING_I64,&state,1);
        ca_effect(&c,block,XR_LIR_MAP_DROP,snapshot); ca_effect(&c,block,XR_LIR_MODULE_READY,inputs[3]);
    }
    ca_return(&c,block,map_fixture_call(&c,block,XR_LIR_MAP_OP_LEN,XR_LIR_I64,&incoming,1));
    if (writes) {
        /* Mutable Ref formals belong to an internal typed call, never to the
         * injected entry. Supply their real caller bindings and capability. */
        XrLirType parameter=XR_LIR_STATE; XrLirFunction entry={0};
        spec=(XrLirFunctionSpec){.name="entry",.name_length=5,.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64};
        OK(xr_lir_declare_function(c.module,&spec,&entry)); OK(xr_lir_set_state_entry(c.module,entry));
        OK(xr_lir_define_function(c.module,entry)); block=(XrLirBlock){0};
        OK(xr_lir_add_block(c.module,entry,NULL,0,&block)); OK(xr_lir_set_entry(c.module,entry,block));
        XrLirValue state={0}; OK(xr_lir_function_parameter(c.module,entry,0,&state));
        XrLirValue map=map_fixture_call(&c,block,XR_LIR_MAP_OP_NEW,XR_LIR_MAP_STRING_I64,NULL,0);
        XrLirValue cell=ca_value(&c,block,XR_LIR_CELL_NEW,XR_LIR_MAP_STRING_I64_CELL,&map,1);
        XrLirValue reference=ca_value(&c,block,XR_LIR_MAP_BORROW,XR_LIR_MAP_STRING_I64_REF,&cell,1);
        XrLirValue arguments[]={cell,reference,map,state},result={0};
        XrLirInstruction call={.op=XR_LIR_CALL,.type=XR_LIR_I64,.callee=function,.operands=arguments,.operand_count=4};
        OK(xr_lir_emit_value(c.module,block,&call,&result));
        ca_effect(&c,block,XR_LIR_MAP_CELL_DROP,cell); ca_effect(&c,block,XR_LIR_MAP_DROP,map);
        ca_return(&c,block,result);
    }
    XrLirDiagnostic diagnostic={0}; XrLirStatus status=xr_lir_module_finish(c.module,&diagnostic);
    if (status!=XR_LIR_OK) fprintf(stderr,"borrowed Map homes writes=%u f%u b%u i%u: %s\n",
        writes ? 1u : 0u,diagnostic.function,diagnostic.block,diagnostic.instruction,
        diagnostic.message[0] ? diagnostic.message : "no diagnostic");
    OK(status); return c.module;
}
static XrLirValue role(CArrayFixture *c,XrLirBlock block,uint32_t operation,
    XrLirType result,const XrLirValue *args,uint32_t count) {
    uint32_t id=0;
    if (operation>=XR_LIR_MAP_OP_NEW) {
        XrLirType receiver=operation>=XR_LIR_MAP_OP_SET ? XR_LIR_MAP_STRING_I64_REF : XR_LIR_MAP_STRING_I64;
        OK(xr_lir_map_operation(c->module,(XrLirMapOperation)operation,receiver,&id));
    } else {
        XrLirType element=c->element;
        if (operation==XR_LIR_ARRAY_OP_SET || operation==XR_LIR_ARRAY_OP_PUSH) {
            OK(xr_lir_array_mutator(c->module,element,XR_LIR_ARRAY_REF,(XrLirArrayOperation)operation,&id));
        } else OK(xr_lir_array_operation(c->module,element,(XrLirArrayOperation)operation,&id));
    }
    XrLirValue value={0}; XrLirInstruction instruction={.op=XR_LIR_CALL_INTRINSIC,.type=result,
        .intrinsic=id,.operands=args,.operand_count=count};
    if (result==XR_LIR_UNIT) OK(xr_lir_emit_effect(c->module,block,&instruction));
    else OK(xr_lir_emit_value(c->module,block,&instruction,&value));
    return value;
}
static XrLirModule *roles_fixture(XrLirType element,bool map) {
    CArrayFixture c={.element=element}; XrLirType optional=0;
    if (map) {
        OK(xr_lir_module_create(NULL,&c.module)); OK(xr_lir_map_catalog_install(c.module,false,false,false,false,false));
        OK(xr_lir_nullable_type(c.module,XR_LIR_I64,&optional));
    } else OK(xr_lir_array_module_create(NULL,&c.module));
    XrLirType value_type=map ? XR_LIR_MAP_STRING_I64 : XR_LIR_ARRAY;
    XrLirType parameters[]={map ? XR_LIR_MAP_STRING_I64_CELL : XR_LIR_ARRAY_CELL,
        map ? XR_LIR_MAP_STRING_I64_REF : XR_LIR_ARRAY_REF};
    XrLirFunction function={0}; XrLirFunctionSpec spec={.name="roles",.name_length=5,
        .parameters=parameters,.parameter_count=2,.result=XR_LIR_UNIT}; OK(xr_lir_declare_function(c.module,&spec,&function));
    XrLirValue inputs[2]={{0}};
    for (uint32_t p=0;p<2;++p) {
        OK(xr_lir_function_parameter(c.module,function,p,&inputs[p]));
        if (!map) OK(xr_lir_set_array_shape(c.module,inputs[p],&(XrLirArrayShape){element}));
    }
    OK(xr_lir_define_function(c.module,function)); XrLirBlock block={0};
    OK(xr_lir_add_block(c.module,function,NULL,0,&block)); OK(xr_lir_set_entry(c.module,function,block));
    if (map) {
        XrLirValue empty=role(&c,block,XR_LIR_MAP_OP_NEW,XR_LIR_MAP_STRING_I64,NULL,0);
        ca_effect(&c,block,XR_LIR_MAP_DROP,empty);
    }
    XrLirValue snapshot=ca_value(&c,block,XR_LIR_CELL_READ,value_type,&inputs[0],1),write[]={inputs[0],snapshot};
    XrLirInstruction instruction={.op=XR_LIR_CELL_WRITE,.type=XR_LIR_UNIT,.operands=write,.operand_count=2};
    OK(xr_lir_emit_effect(c.module,block,&instruction)); ca_effect(&c,block,map ? XR_LIR_MAP_DROP : XR_LIR_ARRAY_DROP,snapshot);
    XrLirValue borrowed=ca_value(&c,block,map ? XR_LIR_MAP_BORROW : XR_LIR_ARRAY_BORROW,parameters[1],&inputs[0],1);
    snapshot=ca_value(&c,block,map ? XR_LIR_MAP_REF_READ : XR_LIR_ARRAY_REF_READ,value_type,&inputs[1],1);
    write[0]=inputs[1]; write[1]=snapshot;
    instruction=(XrLirInstruction){.op=map ? XR_LIR_MAP_REF_WRITE : XR_LIR_ARRAY_REF_WRITE,
        .type=XR_LIR_UNIT,.operands=write,.operand_count=2}; OK(xr_lir_emit_effect(c.module,block,&instruction));
    XrLirValue fill=map || element==XR_LIR_STRING ? ca_text(&c,block,"key",3) : number(&c,block,element,7);
    XrLirValue zero=ca_constant(&c,block,0),args[]={snapshot,map ? fill : zero};
    (void)role(&c,block,map ? XR_LIR_MAP_OP_LEN : XR_LIR_ARRAY_OP_LEN,XR_LIR_I64,args,1);
    XrLirValue item=role(&c,block,map ? XR_LIR_MAP_OP_GET : XR_LIR_ARRAY_OP_GET,map ? optional : element,args,2);
    if (!map && element==XR_LIR_STRING) ca_effect(&c,block,XR_LIR_STRING_DROP,item);
    if (map) {
        (void)role(&c,block,XR_LIR_MAP_OP_INDEX,XR_LIR_I64,args,2); (void)role(&c,block,XR_LIR_MAP_OP_CONTAINS,XR_LIR_I1,args,2);
        XrLirValue set[]={borrowed,fill,ca_constant(&c,block,42)};
        (void)role(&c,block,XR_LIR_MAP_OP_SET,XR_LIR_UNIT,set,3); args[0]=inputs[1];
        (void)role(&c,block,XR_LIR_MAP_OP_REMOVE,XR_LIR_I1,args,2); (void)role(&c,block,XR_LIR_MAP_OP_CLEAR,XR_LIR_UNIT,args,1);
    } else {
        XrLirValue set[]={borrowed,zero,fill},push[]={inputs[1],fill},repeat[]={ca_constant(&c,block,2),fill};
        (void)role(&c,block,XR_LIR_ARRAY_OP_SET,XR_LIR_UNIT,set,3); (void)role(&c,block,XR_LIR_ARRAY_OP_PUSH,XR_LIR_UNIT,push,2);
        XrLirValue repeated=role(&c,block,XR_LIR_ARRAY_OP_REPEAT,XR_LIR_ARRAY,repeat,2); ca_effect(&c,block,XR_LIR_ARRAY_DROP,repeated);
        XrLirValue display=role(&c,block,XR_LIR_ARRAY_OP_DISPLAY,XR_LIR_STRING,&snapshot,1); ca_effect(&c,block,XR_LIR_STRING_DROP,display);
    }
    ca_effect(&c,block,map ? XR_LIR_MAP_DROP : XR_LIR_ARRAY_DROP,snapshot);
    if (map || element==XR_LIR_STRING) ca_effect(&c,block,XR_LIR_STRING_DROP,fill);
    ca_return(&c,block,(XrLirValue){0}); OK(xr_lir_module_finish(c.module,NULL)); return c.module;
}

static XrLirModule *loop_fixture(void) {
    CArrayFixture c={.element=XR_LIR_I64}; OK(xr_lir_array_module_create(NULL,&c.module));
    XrLirFunction function={0}; XrLirFunctionSpec spec={.name="loop",.name_length=4,.result=XR_LIR_UNIT};
    OK(xr_lir_declare_function(c.module,&spec,&function)); OK(xr_lir_define_function(c.module,function));
    XrLirBlock entry={0},body={0},exit={0};
    OK(xr_lir_add_block(c.module,function,NULL,0,&entry)); OK(xr_lir_add_block(c.module,function,NULL,0,&body));
    OK(xr_lir_add_block(c.module,function,NULL,0,&exit)); OK(xr_lir_set_entry(c.module,function,entry));
    XrLirInstruction branch={.op=XR_LIR_BR,.type=XR_LIR_UNIT,.edge_count=1,.edges={{body,NULL,0}}};
    OK(xr_lir_set_terminator(c.module,entry,&branch));
    XrLirValue array=role(&c,body,XR_LIR_ARRAY_OP_NEW,XR_LIR_ARRAY,NULL,0);
    XrLirValue cell=ca_value(&c,body,XR_LIR_CELL_NEW,XR_LIR_ARRAY_CELL,&array,1);
    ca_effect(&c,body,XR_LIR_ARRAY_DROP,array);
    XrLirValue borrow=ca_value(&c,body,XR_LIR_ARRAY_BORROW,XR_LIR_ARRAY_REF,&cell,1);
    XrLirValue args[]={borrow,ca_constant(&c,body,7)};
    (void)role(&c,body,XR_LIR_ARRAY_OP_PUSH,XR_LIR_UNIT,args,2);
    ca_effect(&c,body,XR_LIR_ARRAY_CELL_DROP,cell);
    XrLirValue condition=number(&c,body,XR_LIR_I1,0);
    branch=(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,.operands=&condition,.operand_count=1,
        .edge_count=2,.edges={{body,NULL,0},{exit,NULL,0}}};
    OK(xr_lir_set_terminator(c.module,body,&branch)); ca_return(&c,exit,(XrLirValue){0});
    OK(xr_lir_module_finish(c.module,NULL)); return c.module;
}

static size_t attempts,fail_at=SIZE_MAX,live;
static struct { void *memory; size_t bytes; } buffers[4096];
static size_t home_live_bytes(void) {
    size_t bytes=0;
    for (size_t i=0;i<4096;++i) if (buffers[i].memory) {
        CHECK(buffers[i].bytes<=SIZE_MAX-bytes); bytes+=buffers[i].bytes;
    }
    return bytes;
}
static size_t buffer_index(void *memory) {
    for (size_t i=0;i<4096;++i) if (buffers[i].memory==memory) return i;
    CHECK(false); return 0;
}
static void *home_allocate(size_t bytes) {
    if (++attempts==fail_at) return NULL;
    void *memory=xr_malloc(bytes);
    if (memory) { size_t i=buffer_index(NULL); buffers[i].memory=memory; buffers[i].bytes=bytes; ++live; } return memory;
}
static void home_free(void *memory) {
    if (!memory) return;
    size_t i=buffer_index(memory); buffers[i].memory=NULL; CHECK(live); --live; xr_free(memory);
}
static void *home_callocate(size_t count,size_t bytes) {
    CHECK(!bytes || count<=SIZE_MAX/bytes); void *memory=home_allocate(count*bytes);
    if (memory) memset(memory,0,count*bytes);
    return memory;
}
static void *home_resize(void *old,size_t bytes) {
    if (++attempts==fail_at) return NULL;
    void *memory=xr_malloc(bytes); if (!memory) return NULL;
    size_t i=buffer_index(old);
    if (old) { memcpy(memory,old,bytes<buffers[i].bytes ? bytes : buffers[i].bytes); xr_free(old); } else ++live;
    buffers[i].memory=memory; buffers[i].bytes=bytes; return memory;
}
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(bytes) home_allocate(bytes)
#define xr_calloc(count,bytes) home_callocate(count,bytes)
#define xr_realloc(old,bytes) home_resize(old,bytes)
#define xr_free(memory) home_free(memory)
#include "lir/xlir_value_homes.c"
#include "lir/xlir_array_region_binding.c"

static void borrowed_map_stages(void) {
    for (unsigned writes=0;writes<2;++writes) {
        XrLirModule *module=borrowed_map_fixture(writes!=0); XrtBillingOwner *billing=NULL;
        MANAGED(xrt_billing_owner_new(NULL,SIZE_MAX,&billing));
        XrLirValueLayoutKey roots[]={{XR_LIR_MAP_STRING_I64,0,0},{XR_LIR_I64,0,0}};
        XrLirValueLayouts *set=NULL; HOME(xr_lir_value_layouts_create(module,roots,2,billing,&set));
        XrLirOwnershipPlan *plan=NULL; OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=module},&plan,NULL));
        XrLirValueHomes *homes=NULL; XrLirValueHomesRequest request={module,{module,1},set,plan};
        attempts=0; HOME(xr_lir_value_homes_create(&request,&homes)); size_t sites=attempts;
        uint32_t incoming=writes ? 3u : 1u;
        const XrLirValueHome *formal=&homes->values[incoming];
        CHECK(formal->parameter==incoming && formal->size==sizeof(XrtManagedHeader *) &&
            formal->offset==SIZE_MAX && formal->first_owner==SIZE_MAX &&
            formal->temporary_offset==SIZE_MAX && formal->temporary_first_owner==SIZE_MAX);
        if (!writes) CHECK(!homes->workspace_count && !homes->owned_frame_needed && !homes->frame.slot_count && !homes->frame.payload_size);
        else {
            CHECK(homes->workspace_count==4 && homes->owned_frame_needed);
            unsigned kinds_seen=0; bool state_write=false,state_read=false;
            for (size_t w=0;w<homes->workspace_count;++w) {
                const XrLirHomeWorkspace *stage=&homes->workspaces[w]; size_t matching=0;
                CHECK(stage->kind==XR_LIR_HOME_WORKSPACE_VALUE && stage->type==formal->type &&
                    stage->temporary_offset==SIZE_MAX && stage->temporary_first_owner==SIZE_MAX &&
                    stage->first_owner<homes->frame.slot_count && homes->frame.owned_offsets[stage->first_owner]==stage->offset);
                for (size_t e=0;e<homes->event_count;++e) {
                    const XrLirHomeEvent *event=&homes->events[e];
                    if (event->block!=stage->block || event->instruction!=stage->instruction || event->source!=stage->value) continue;
                    CHECK((event->kind==XR_LIR_OWNERSHIP_BINDING_NEW || event->kind==XR_LIR_OWNERSHIP_BINDING_WRITE) &&
                        event->target==incoming && homes->bindings[event->source].type==stage->type); ++matching;
                    XrLirInstructionView op; OK(xr_lir_instruction_view(module,(XrLirBlock){module,1,event->block},event->instruction-1,&op));
                    kinds_seen|=op.op==XR_LIR_CELL_NEW ? 1u : op.op==XR_LIR_CELL_WRITE ? 2u :
                        op.op==XR_LIR_MAP_REF_WRITE ? 4u : op.op==XR_LIR_STATE_WRITE ? 8u : 0u;
                    if (op.op==XR_LIR_STATE_WRITE) {
                        const XrLirHomeBinding *binding=&homes->bindings[event->source];
                        CHECK(binding->kind==XR_LIR_HOME_BINDING_STATE_BORROW && binding->source==event->source &&
                            binding->offset==SIZE_MAX && binding->first_owner==SIZE_MAX); state_write=true;
                    }
                    const XrLirHomeBinding *binding=&homes->bindings[event->source];
                    CHECK(binding->first_owner==SIZE_MAX || binding->first_owner!=stage->first_owner);
                }
                CHECK(matching==1);
                for (size_t v=1;v<=homes->value_count;++v) CHECK(homes->values[v].first_owner!=stage->first_owner &&
                    homes->values[v].temporary_first_owner!=stage->first_owner);
            }
            for (size_t e=0;e<homes->event_count;++e) {
                const XrLirHomeEvent *event=&homes->events[e];
                state_read|=event->kind==XR_LIR_OWNERSHIP_BINDING_READ &&
                    homes->bindings[event->source].kind==XR_LIR_HOME_BINDING_STATE_BORROW;
            }
            CHECK(kinds_seen==15 && state_write && state_read);
        }
        xr_lir_value_homes_destroy(homes); homes=NULL; CHECK(!live);
        for (size_t point=1;point<=sites;++point) {
            attempts=0; fail_at=point;
            CHECK(xr_lir_value_homes_create(&request,&homes)==XR_LIR_VALUE_LAYOUT_OOM && !homes && !live);
        }
        fail_at=SIZE_MAX; xr_lir_ownership_plan_destroy(plan); xr_lir_value_layouts_destroy(set); xr_lir_module_destroy(module);
        CHECK(xrt_billing_owner_stats(billing).blocks==1); xrt_billing_owner_drop(billing);
    }
}

static XrLirValueLayouts *layouts(XrLirModule *module,XrtBillingOwner *billing) {
    XrLirValueLayoutKey roots[32]={{XR_LIR_UNIT,0,0},{XR_LIR_I1,0,0},{XR_LIR_I64,0,0},{XR_LIR_U8,0,0},{XR_LIR_U64,0,0},
        {XR_LIR_STRING,0,0},{XR_LIR_MAP_STRING_I64,0,0},{XR_LIR_ARRAY,XR_LIR_I64,0},{XR_LIR_ARRAY,XR_LIR_STRING,0},
        {XR_LIR_ARRAY,XR_LIR_U8,0},{XR_LIR_ARRAY,XR_LIR_U64,0}}; size_t count=11;
    for (uint32_t i=0;i<xr_lir_constructed_type_count(module);++i) {
        XrLirType type=XR_LIR_TYPE_COUNT+i; XrLirTypeView shape; OK(xr_lir_type_view(module,type,&shape)); CHECK(count<32);
        if (shape.kind==XR_LIR_TYPE_NULLABLE) roots[count++]=(XrLirValueLayoutKey){type,0,0};
    }
    XrLirValueLayouts *set=NULL; HOME(xr_lir_value_layouts_create(module,roots,count,billing,&set)); return set;
}
static unsigned kinds,array_roles,map_roles,literal_caches,string_roles,string_stages;
static void check_projection(const XrLirValueHomes *homes,XrtBillingOwner *billing,
    const XrtBackingTypes *types,const XrtValueKind *workspace_kinds,uint32_t string_type) {
    for (uint32_t v=0;v<=homes->value_count;++v) {
        const XrLirValueHome *home=&homes->values[v];
        if (home->literal_cache_owner==SIZE_MAX) { CHECK(home->literal_cache_offset==SIZE_MAX); continue; }
        CHECK(v && home->type && !home->parameter && home->size==sizeof(XrtManagedHeader *) && home->owner_count==1);
        CHECK(home->literal_cache_owner<homes->frame.slot_count &&
            homes->frame.owned_offsets[home->literal_cache_owner]==home->literal_cache_offset);
        CHECK(home->literal_cache_offset<=homes->frame.payload_size &&
            home->size<=homes->frame.payload_size-home->literal_cache_offset &&
            !(home->literal_cache_offset%_Alignof(XrtManagedHeader *)));
        CHECK(home->literal_cache_owner!=home->first_owner && home->literal_cache_owner!=home->temporary_first_owner &&
            home->literal_cache_offset!=home->offset && home->literal_cache_offset!=home->temporary_offset);
        ++literal_caches;
    }
    for (uint32_t v=0;homes->bindings && v<=homes->value_count;++v) {
        const XrLirHomeBinding *row=&homes->bindings[v];
        if (!row->kind) { CHECK(!row->type); continue; }
        kinds|=1u<<row->kind; CHECK(row->type && !homes->values[v].type);
        if (row->kind==XR_LIR_HOME_BINDING_LOCAL) {
            CHECK(!row->parameter && !row->source && row->first_owner<homes->frame.slot_count);
            CHECK(homes->frame.owned_offsets[row->first_owner]==row->offset);
        } else CHECK(row->offset==SIZE_MAX && row->first_owner==SIZE_MAX);
        if (row->kind==XR_LIR_HOME_BINDING_PARAMETER) CHECK(row->parameter && !row->source);
        if (row->kind==XR_LIR_HOME_BINDING_CELL_BORROW) CHECK(row->source && homes->bindings[row->source].type==row->type);
        if (row->kind==XR_LIR_HOME_BINDING_STATE_BORROW) CHECK(row->source && !homes->values[row->source].type);
    }
    size_t expected=0;
    for (size_t i=0;i<homes->event_count;++i) {
        const XrLirHomeEvent *event=&homes->events[i];
        if (event->kind==XR_LIR_OWNERSHIP_ARRAY_OPERATION) {
            CHECK(event->operation && event->operation<=XR_LIR_ARRAY_OP_DISPLAY); array_roles|=1u<<event->operation;
            expected+=event->operation==XR_LIR_ARRAY_OP_SET || event->operation==XR_LIR_ARRAY_OP_PUSH;
        }
        if (event->kind==XR_LIR_OWNERSHIP_MAP_OPERATION) {
            CHECK(event->operation>=XR_LIR_MAP_OP_NEW && event->operation<=XR_LIR_MAP_OP_CLEAR);
            map_roles|=1u<<(event->operation-XR_LIR_MAP_OP_NEW);
            expected+=event->operation==XR_LIR_MAP_OP_SET || event->operation==XR_LIR_MAP_OP_REMOVE || event->operation==XR_LIR_MAP_OP_CLEAR;
        }
        if (event->operation>=XR_LIR_STRING_OP_APPEND_OWNED && event->operation<XR_LIR_STRING_OP_END)
            string_roles|=1u<<(event->operation-XR_LIR_STRING_OP_APPEND_OWNED);
        bool borrowed_copy=(event->kind==XR_LIR_OWNERSHIP_BINDING_NEW || event->kind==XR_LIR_OWNERSHIP_BINDING_WRITE) &&
            event->target && homes->values[event->target].parameter && homes->values[event->target].type==string_type;
        if (event->kind==XR_LIR_OWNERSHIP_STRING_OPERATION || borrowed_copy) {
            if (borrowed_copy) {
                const XrLirValueHome *source=&homes->values[event->target];
                CHECK(source->offset==SIZE_MAX && source->first_owner==SIZE_MAX && source->temporary_first_owner==SIZE_MAX);
                CHECK(event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_NONE); ++string_stages;
            } else {
                CHECK(event->operation==XR_LIR_STRING_OP_APPEND_CELL || event->operation==XR_LIR_STRING_OP_APPEND_STATE ||
                    event->operation==XR_LIR_STRING_OP_APPEND_REF);
                CHECK(event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_BORROW);
            }
            CHECK(event->source && event->target);
            const XrLirHomeBinding *binding=&homes->bindings[event->source];
            CHECK(binding->kind && binding->type==homes->values[event->target].type);
            size_t matched=0;
            for (size_t w=0;w<homes->workspace_count;++w) {
                const XrLirHomeWorkspace *workspace=&homes->workspaces[w];
                if (workspace->block!=event->block || workspace->instruction!=event->instruction) continue;
                CHECK(workspace->kind==XR_LIR_HOME_WORKSPACE_VALUE && workspace->type==binding->type &&
                    workspace->value==event->source && workspace->first_owner!=binding->first_owner &&
                    workspace->temporary_offset==SIZE_MAX && workspace->temporary_first_owner==SIZE_MAX &&
                    workspace->first_owner!=homes->values[event->target].first_owner &&
                    workspace->first_owner!=homes->values[event->target].temporary_first_owner);
                ++matched;
            }
            CHECK(matched==1); ++expected;
        }
    }
    CHECK(homes->workspace_count==expected);
    for (size_t i=0;i<homes->workspace_count;++i) {
        XrLirHomeWorkspace row=homes->workspaces[i]; CHECK(row.block && row.instruction && row.type);
        CHECK(row.first_owner<homes->frame.slot_count && homes->frame.owned_offsets[row.first_owner]==row.offset);
        for (size_t j=0;j<i;++j) CHECK(row.offset!=homes->workspaces[j].offset);
    }
    XrtManagedRoot *root=NULL; XrtManagedFrame *frame=NULL;
    MANAGED(xrt_managed_root_new(billing,&root)); MANAGED(xrt_managed_frame_enter_layout(root,&homes->frame,&frame));
    void *payload=NULL; size_t bytes=0; MANAGED(xrt_managed_frame_payload(frame,&payload,&bytes)); CHECK(bytes==homes->frame.payload_size);
    for (size_t i=0;i<homes->frame.slot_count;++i) {
        XrtManagedHeader *owner=(XrtManagedHeader *)(uintptr_t)1;
        memcpy(&owner,(unsigned char *)payload+homes->frame.owned_offsets[i],sizeof(owner)); CHECK(!owner);
    }
    for (size_t i=0;i<homes->workspace_count;++i) {
        XrLirHomeWorkspace row=homes->workspaces[i]; XrtValueKind kind=workspace_kinds[i];
        XrtManagedOps *ops=NULL; size_t extent=sizeof(XrtArrayBacking);
        switch (kind) {
        case XRT_VALUE_STRING: ops=types->string; extent=sizeof(XrtStringBacking)+1; break;
        case XRT_VALUE_ARRAY_I64: ops=types->arrays[XRT_BACKING_ARRAY_I64]; break;
        case XRT_VALUE_ARRAY_STRING: ops=types->arrays[XRT_BACKING_ARRAY_STRING]; break;
        case XRT_VALUE_ARRAY_U8: ops=types->arrays[XRT_BACKING_ARRAY_U8]; break;
        case XRT_VALUE_ARRAY_U64: ops=types->arrays[XRT_BACKING_ARRAY_U64]; break;
        case XRT_VALUE_MAP_STRING_I64: ops=types->map; extent=sizeof(XrtMapBacking); break;
        default: CHECK(false);
        }
        XrtManagedSlotLocation location; MANAGED(xrt_managed_slot_location(frame,row.first_owner,&location));
        CHECK((void *)location.owner==(unsigned char *)payload+row.offset && !*location.owner);
        XrtBillingStats before=xrt_billing_owner_stats(billing); XrtManagedHeader *pending=NULL;
        xrt_billing_owner_fail_after(billing,0);
        CHECK(xrt_managed_slot_prepare(frame,row.first_owner,ops,extent,&pending)==XRT_MANAGED_OOM && !pending && !*location.owner);
        xrt_billing_owner_fail_after(billing,SIZE_MAX);
        MANAGED(xrt_managed_slot_prepare(frame,row.first_owner,ops,extent,&pending));
        CHECK(pending && !*location.owner); MANAGED(xrt_managed_slot_abort(frame,row.first_owner));
        XrtBillingStats after=xrt_billing_owner_stats(billing); CHECK(after.bytes==before.bytes && after.blocks==before.blocks);
        pending=NULL; MANAGED(xrt_managed_slot_prepare(frame,row.first_owner,ops,extent,&pending));
        MANAGED(xrt_managed_slot_commit(frame,row.first_owner)); CHECK(*location.owner==pending);
        MANAGED(xrt_managed_slot_drop(frame,row.first_owner)); CHECK(!*location.owner);
        after=xrt_billing_owner_stats(billing); CHECK(after.bytes==before.bytes && after.blocks==before.blocks);
    }
    MANAGED(xrt_managed_root_destroy(&root));
}
static void project(XrLirModule *module,bool faults) {
    XrtBillingOwner *billing=NULL; MANAGED(xrt_billing_owner_new(NULL,SIZE_MAX,&billing));
    XrLirValueLayouts *set=layouts(module,billing); const XrLirOwnershipTypeFact *facts=NULL; uint32_t count=0;
    const XrtValuePlan *string_plan=NULL; uint32_t string_type=0;
    HOME(xr_lir_value_layouts_lookup(set,(XrLirValueLayoutKey){XR_LIR_STRING,0,0},&string_plan,&string_type));
    HOME(xr_lir_value_layouts_ownership(set,&facts,&count));
    XrLirOwnershipPlan *plan=NULL; OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){module,facts,count},&plan,NULL));
    size_t functions=xr_lir_function_count(module); CHECK(functions<=8); XrLirValueHomes *homes[8]={0};
    size_t owned_blocks[8]={0},owned_bytes[8]={0};
    XrtValueKind workspace_kinds[8][8]={{0}};
    for (uint32_t f=0;f<functions;++f) {
        XrLirFunction function={0}; OK(xr_lir_function_at(module,f,&function));
        /* Earlier functions remain owned until the source-retirement checks. */
        size_t baseline_blocks=live,baseline_bytes=home_live_bytes();
        XrLirValueHomesRequest request={module,function,set,plan}; attempts=0;
        HOME(xr_lir_value_homes_create(&request,&homes[f])); size_t total=attempts;
        CHECK(live>=baseline_blocks && home_live_bytes()>=baseline_bytes);
        owned_blocks[f]=live-baseline_blocks; owned_bytes[f]=home_live_bytes()-baseline_bytes;
        if (faults) {
            xr_lir_value_homes_destroy(homes[f]); homes[f]=NULL;
            CHECK(live==baseline_blocks && home_live_bytes()==baseline_bytes);
            for (size_t n=1;n<=total;++n) {
                attempts=0; fail_at=n;
                CHECK(xr_lir_value_homes_create(&request,&homes[f])==XR_LIR_VALUE_LAYOUT_OOM && !homes[f]);
                CHECK(live==baseline_blocks && home_live_bytes()==baseline_bytes);
            }
            fail_at=SIZE_MAX; HOME(xr_lir_value_homes_create(&request,&homes[f]));
            CHECK(live>=baseline_blocks && home_live_bytes()>=baseline_bytes);
            CHECK(live-baseline_blocks==owned_blocks[f] && home_live_bytes()-baseline_bytes==owned_bytes[f]);
            printf("binding home allocation rollback sites: %zu\n",total);
        }
        CHECK(homes[f] && homes[f]->workspace_count<=8);
        for (size_t i=0;i<homes[f]->workspace_count;++i) {
            XrLirValueLayoutKey key; const XrtValuePlan *payload_plan=NULL; XrtValuePlanSpec payload_spec;
            HOME(xr_lir_value_layouts_at(set,homes[f]->workspaces[i].type-1,&key,&payload_plan));
            MANAGED(xrt_value_plan_view(payload_plan,&payload_spec)); workspace_kinds[f][i]=payload_spec.kind;
        }
    }
    XrtBackingTypes types={0}; MANAGED(xrt_backing_types_init(billing,&types));
    xr_lir_module_destroy(module); xr_lir_ownership_plan_destroy(plan); xr_lir_value_layouts_destroy(set);
    for (size_t f=0;f<functions;++f) {
        CHECK(homes[f]); check_projection(homes[f],billing,&types,workspace_kinds[f],string_type);
        size_t before_blocks=live,before_bytes=home_live_bytes();
        CHECK(before_blocks>=owned_blocks[f] && before_bytes>=owned_bytes[f]);
        xr_lir_value_homes_destroy(homes[f]);
        CHECK(live==before_blocks-owned_blocks[f] && home_live_bytes()==before_bytes-owned_bytes[f]);
    }
    xrt_backing_types_drop(&types); CHECK(!live && !home_live_bytes());
    XrtBillingStats stats=xrt_billing_owner_stats(billing); CHECK(stats.blocks==1); xrt_billing_owner_drop(billing);
}
static void string_borrow_only(XrLirType type) {
    XrLirModule *module=NULL; OK(xr_lir_module_create(NULL,&module));
    XrLirFunction function={0};
    XrLirFunctionSpec spec={.name="borrow_only",.name_length=11,.parameters=&type,.parameter_count=1,.result=XR_LIR_UNIT};
    OK(xr_lir_declare_function(module,&spec,&function)); OK(xr_lir_define_function(module,function));
    XrLirBlock block={0}; OK(xr_lir_add_block(module,function,NULL,0,&block)); OK(xr_lir_set_entry(module,function,block));
    XrLirValue parameter={0}; OK(xr_lir_function_parameter(module,function,0,&parameter));
    CArrayFixture fixture={.module=module}; ca_return(&fixture,block,(XrLirValue){0}); OK(xr_lir_module_finish(module,NULL));
    CHECK(xr_lir_string_binding(module,parameter) && !xr_lir_string_binding(NULL,parameter));
    XrLirOwnershipPlan *plan=NULL; OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=module},&plan,NULL));
    XrtBillingOwner *billing=NULL; MANAGED(xrt_billing_owner_new(NULL,SIZE_MAX,&billing));
    XrLirValueLayoutKey root={XR_LIR_UNIT,0,0}; XrLirValueLayouts *missing=NULL;
    HOME(xr_lir_value_layouts_create(module,&root,1,billing,&missing));
    XrLirValueHomes *homes=NULL;
    CHECK(xr_lir_value_homes_create(&(XrLirValueHomesRequest){module,function,missing,plan},&homes)==XR_LIR_VALUE_LAYOUT_ARGUMENT && !homes && !live);
    xr_lir_value_layouts_destroy(missing); XrLirValueLayouts *set=layouts(module,billing);
    HOME(xr_lir_value_homes_create(&(XrLirValueHomesRequest){module,function,set,plan},&homes));
    const XrLirHomeBinding *binding=&homes->bindings[parameter.id];
    CHECK(binding->kind==XR_LIR_HOME_BINDING_PARAMETER && binding->parameter==1 && binding->type &&
        binding->offset==SIZE_MAX && binding->first_owner==SIZE_MAX && !homes->values[parameter.id].type &&
        !homes->values[parameter.id].owner_count && !homes->frame.slot_count && !homes->workspace_count && !homes->owned_frame_needed);
    xr_lir_module_destroy(module); xr_lir_ownership_plan_destroy(plan); xr_lir_value_layouts_destroy(set);
    CHECK(binding->parameter==1 && binding->offset==SIZE_MAX);
    xr_lir_value_homes_destroy(homes); CHECK(!live && xrt_billing_owner_stats(billing).blocks==1); xrt_billing_owner_drop(billing);
}
static void string_projections(void) {
    string_borrow_only(XR_LIR_STRING_CELL); string_borrow_only(XR_LIR_STRING_REF);
    XrLirStatus (*transforms[])(const XrLirModule *,XrLirModule **)={xr_lir_rebuild,xr_lir_inline,xr_lir_sccp,xr_lir_string_elide};
    XrLirModule *module=string_roles_fixture(false),*copies[4]={0};
    for (size_t i=0;i<4;++i) OK(transforms[i](module,&copies[i]));
    project(module,true);
    for (size_t i=0;i<4;++i) project(copies[i],true);
    project(string_roles_fixture(true),true);
    project(string_state_fixture(false),true); project(string_state_fixture(true),true);
    module=NULL; OK(string_ref_fixture(&module)); project(module,true);
    CHECK(string_roles==((1u<<1)|(1u<<2)|(1u<<3)|(1u<<4)|(1u<<5)|(1u<<6)|(1u<<7)));
    CHECK(string_stages==4);
}
static void callable_projection(bool owned,bool scopes) {
    XrLirModule *module=NULL; XrLirFunction root={0};
    if (owned && scopes) callable_factory_fixture_mode(&module,&root,false,true);
    else if (owned) callable_factory_fixture(&module,&root,false);
    else callable_fixture(&module,&root);
    XrLirClosureTarget closure; OK(xr_lir_closure_target_view(module,(XrLirFunction){module,1},&closure));
    XrLirValueLayoutKey roots[]={{closure.callable,0,0},{closure.callable,0,1}};
    XrtBillingOwner *billing=NULL; MANAGED(xrt_billing_owner_new(NULL,SIZE_MAX,&billing));
    size_t baseline=xrt_billing_owner_stats(billing).bytes;
    XrLirValueLayouts *layouts=NULL; HOME(xr_lir_value_layouts_create(module,roots,2,billing,&layouts));
    const XrtValuePlan *signature=NULL,*environment=NULL; uint32_t signature_id=0,environment_id=0;
    HOME(xr_lir_value_layouts_lookup(layouts,roots[0],&signature,&signature_id));
    HOME(xr_lir_value_layouts_lookup(layouts,roots[1],&environment,&environment_id));
    XrtValuePlanSpec shape; MANAGED(xrt_value_plan_view(environment,&shape));
    CHECK(shape.kind==XRT_VALUE_ENVIRONMENT && shape.entry.ordinal==1 && shape.entry.signature==signature &&
        shape.capture_count==(owned ? 1u : 0u) && shape.size==(owned ? 8u : 0u));
    XrLirValueImage *image=NULL; HOME(xr_lir_value_image_create(layouts,billing,&image));
    const XrtValueImage *raw=xr_lir_value_image_view(image);
    CHECK(raw->types[environment_id-1].kind==XRT_VALUE_ENVIRONMENT && raw->types[environment_id-1].entry_signature==signature_id);
    XrLirOwnershipPlan *plan=NULL;
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=module},&plan,NULL));
    XrLirValueHomes *homes[3]={0}; size_t owners=0,borrows=0,exits=0,releases=0,keeps=0,copies=0,returns=0;
    for (uint32_t f=0;f<3;++f) {
        HOME(xr_lir_value_homes_create(&(XrLirValueHomesRequest){module,{module,f+1},layouts,plan},&homes[f]));
        if (!homes[f]) continue;
        for (uint32_t i=0;i<homes[f]->value_count;++i) {
            const XrLirValueHome *home=&homes[f]->values[i+1];
            if (home->type!=signature_id) continue;
            CHECK(home->size==sizeof(XrtFunction));
            if (home->transport==XR_LIR_CALLABLE_TRANSPORT_OWNED) {
                ++owners; CHECK(home->owner_count==1 && home->first_owner!=SIZE_MAX && home->temporary_first_owner!=SIZE_MAX);
                CHECK(homes[f]->frame.owned_offsets[home->first_owner]==home->offset+offsetof(XrtFunction,owner));
                if (home->environment_type) CHECK(home->environment_type==environment_id);
            } else {
                ++borrows; CHECK(!home->owner_count && home->first_owner==SIZE_MAX && home->temporary_first_owner==SIZE_MAX);
                CHECK(home->byte_count==1 && homes[f]->bytes[home->first_bytes].offset==0 &&
                    homes[f]->bytes[home->first_bytes].size==sizeof(XrtFunction));
                if (home->parameter) CHECK(home->offset==SIZE_MAX);
            }
        }
        for (size_t e=0;e<homes[f]->event_count;++e) {
            const XrLirHomeEvent *event=&homes[f]->events[e];
            exits+=event->kind==XR_LIR_OWNERSHIP_CALLABLE_SCOPE_EXIT;
            releases+=event->kind==XR_LIR_OWNERSHIP_RELEASE;
            if (event->kind==XR_LIR_OWNERSHIP_CALLABLE_SCOPE_EXIT) {
                CHECK(event->transfer_mode==(event->source ? XR_LIR_OWNERSHIP_TRANSFER_MOVE : XR_LIR_OWNERSHIP_TRANSFER_NONE));
                keeps+=event->source!=0;
            }
            if (event->kind==XR_LIR_OWNERSHIP_CALLABLE_VALUE && event->operation==XR_LIR_FUNCTION_BIND) {
                CHECK(event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_COPY); ++copies;
            }
            if (event->kind==XR_LIR_OWNERSHIP_RETURN_TRANSFER) {
                CHECK(event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_MOVE); ++returns;
            }
        }
        CHECK(owned || (!homes[f]->owned_frame_needed && !homes[f]->frame.slot_count));
    }
    CHECK(owned ? owners>0 : owners==0); CHECK(owned || borrows>0);
    CHECK(!scopes || (exits && releases && keeps && copies)); CHECK(!owned || returns);
    if (!owned) {
        XrtCaptureType *type=NULL; MANAGED(xrt_capture_type_new(billing,environment,&type));
        XrtManagedRoot *root_owner=NULL; XrtManagedFrame *frame=NULL;
        MANAGED(xrt_managed_root_new(billing,&root_owner)); MANAGED(xrt_managed_frame_enter(root_owner,1,&frame));
        MANAGED(xrt_environment_create(frame,0,type,NULL));
        XrtManagedHeader *owner=NULL; MANAGED(xrt_managed_slot_borrow(frame,0,&owner));
        static const unsigned char entry=0;
        XrtFunction input={&entry,(uintptr_t)(owner+1),owner}, output={0};
        bool copied=false; size_t calls=xrt_billing_owner_stats(billing).allocations;
        for (uint32_t i=0;i<homes[1]->value_count && !copied;++i) {
            const XrLirValueHome *home=&homes[1]->values[i+1];
            if (!home->parameter || home->type!=signature_id) continue;
            for (size_t n=0;n<home->byte_count;++n) {
                XrLirHomeBytes span=homes[1]->bytes[home->first_bytes+n];
                memcpy((unsigned char *)&output+span.offset,(const unsigned char *)&input+span.offset,span.size);
            }
            copied=true;
        }
        CHECK(copied && output.entry==input.entry && output.environment==input.environment && output.owner==owner &&
            atomic_load(&owner->references)==1 && xrt_billing_owner_stats(billing).allocations==calls);
        MANAGED(xrt_managed_root_destroy(&root_owner)); xrt_capture_type_drop(&type);
    }
    if (homes[1]) {
        XrtBillingStats held=xrt_billing_owner_stats(billing);
        for (size_t allowed=0;;++allowed) {
            XrLirValueLayouts *failed=NULL; xrt_billing_owner_fail_after(billing,allowed);
            XrLirValueLayoutStatus status=xr_lir_value_layouts_create(module,roots,2,billing,&failed);
            xrt_billing_owner_fail_after(billing,SIZE_MAX);
            if (status==XR_LIR_VALUE_LAYOUT_OK) { xr_lir_value_layouts_destroy(failed); break; }
            CHECK(status==XR_LIR_VALUE_LAYOUT_OOM && !failed);
            CHECK(xrt_billing_owner_stats(billing).bytes==held.bytes && xrt_billing_owner_stats(billing).blocks==held.blocks);
        }
    }
    CHECK(xrt_value_plan_retain((XrtValuePlan *)environment));
    xr_lir_module_destroy(module); xr_lir_ownership_plan_destroy(plan); xr_lir_value_layouts_destroy(layouts);
    MANAGED(xrt_value_plan_view(environment,&shape)); CHECK(shape.entry.ordinal==1);
    CHECK(xr_lir_value_image_view(image)->types[environment_id-1].entry_ordinal==1);
    for (size_t f=0;f<3;++f) xr_lir_value_homes_destroy(homes[f]);
    xr_lir_value_image_destroy(image); xrt_value_plan_drop((XrtValuePlan *)environment);
    CHECK(xrt_billing_owner_stats(billing).bytes==baseline && xrt_billing_owner_stats(billing).blocks==1);
    xrt_billing_owner_drop(billing);
}
static void callable_targets_and_join(unsigned shape) {
    bool mixed=shape==1,branching=shape!=0;
    CArrayFixture c={0}; OK(xr_lir_module_create(NULL,&c.module));
    XrLirCallableParameter argument={.type=XR_LIR_I64}; XrLirType callable=0,integer=XR_LIR_I64;
    OK(xr_lir_callable_type(c.module,&(XrLirCallableType){.parameters=&argument,.parameter_count=1,.result=XR_LIR_I64},&callable));
    XrLirFunction targets[2]={{0}};
    for (uint32_t n=0;n<2;++n) {
        XrLirFunctionSpec spec={.name=n ? "second" : "first",.name_length=n ? 6u : 5u,
            .parameters=&integer,.parameter_count=1,.result=XR_LIR_I64};
        OK(xr_lir_declare_function(c.module,&spec,&targets[n]));
        OK(xr_lir_set_closure_target(c.module,targets[n],(XrLirClosureTarget){.callable=callable}));
        OK(xr_lir_define_function(c.module,targets[n])); XrLirBlock block={0};
        OK(xr_lir_add_block(c.module,targets[n],NULL,0,&block)); OK(xr_lir_set_entry(c.module,targets[n],block));
        XrLirValue input={0}; OK(xr_lir_function_parameter(c.module,targets[n],0,&input)); ca_return(&c,block,input);
    }
    XrLirFunction identity={0}; XrLirFunctionCallableParameter contract={.contract={.type=callable,.escaping=true}};
    XrLirFunctionSpec identity_spec={.name="identity",.name_length=8,.parameters=&callable,.parameter_count=1,
        .result=callable,.callable_parameters=&contract,.callable_parameter_count=1};
    OK(xr_lir_declare_function(c.module,&identity_spec,&identity)); OK(xr_lir_define_function(c.module,identity));
    XrLirBlock identity_body={0}; XrLirValue borrowed={0};
    OK(xr_lir_add_block(c.module,identity,NULL,0,&identity_body)); OK(xr_lir_set_entry(c.module,identity,identity_body));
    OK(xr_lir_function_parameter(c.module,identity,0,&borrowed));
    XrLirValue scope=callable_fixture_scope(&c,identity_body,(XrLirValue){0}), bind_args[]={scope,borrowed};
    XrLirValue bound=ca_value(&c,identity_body,XR_LIR_FUNCTION_BIND,callable,bind_args,2);
    XrLirValue kept=callable_fixture_leave(&c,identity_body,scope,bound,callable); ca_return(&c,identity_body,kept);
    XrLirFunction root={0}; XrLirFunctionSpec root_spec={.name="root",.name_length=4,.result=branching ? XR_LIR_I64 : callable};
    OK(xr_lir_declare_function(c.module,&root_spec,&root)); OK(xr_lir_define_function(c.module,root));
    XrLirBlock entry={0},branches[2]={{0}},join={0};
    OK(xr_lir_add_block(c.module,root,NULL,0,&entry)); OK(xr_lir_set_entry(c.module,root,entry));
    XrLirValue condition={0};
    XrLirValue weakened={0};
    if (branching) {
        for (uint32_t n=0;n<2;++n) OK(xr_lir_add_block(c.module,root,NULL,0,&branches[n]));
        XrLirType parameters[]={callable,callable};
        OK(xr_lir_add_block(c.module,root,parameters,shape==2 ? 2u : 1u,&join));
        condition=number(&c,entry,XR_LIR_I1,1);
    } else branches[0]=branches[1]=entry;
    XrLirValue values[2]={{0}};
    for (uint32_t n=0;n<2;++n) {
        XrLirInstruction reference={.op=XR_LIR_FUNCTION_REF,.type=callable,.callee=targets[n],
            .bits=shape==2 || (mixed && n) ? XR_LIR_CLOSURE_OWNED : XR_LIR_CLOSURE_FRAME};
        OK(xr_lir_emit_value(c.module,shape==2 ? entry : branches[n],&reference,&values[n]));
        if (branching && shape!=2) {
            XrLirInstruction edge={.op=XR_LIR_BR,.type=XR_LIR_UNIT,.edges={{join,&values[n],1}},.edge_count=1};
            OK(xr_lir_set_terminator(c.module,branches[n],&edge));
        }
    }
    if (shape==2) weakened=ca_value(&c,entry,XR_LIR_FUNCTION_WEAKEN,callable,&values[0],1);
    if (branching) {
        if (shape==2) for (uint32_t n=0;n<2;++n) {
            XrLirValue duplicate[]={values[0],values[0]};
            XrLirInstruction edge={.op=XR_LIR_BR,.type=XR_LIR_UNIT,.edges={{join,duplicate,2}},.edge_count=1};
            OK(xr_lir_set_terminator(c.module,branches[n],&edge));
        }
        XrLirInstruction branch={.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,.operands=&condition,.operand_count=1,
            .edges={{branches[0],NULL,0},{branches[1],NULL,0}},.edge_count=2};
        OK(xr_lir_set_terminator(c.module,entry,&branch));
    }
    XrLirValue selected={0},second_parameter={0};
    if (branching) {
        OK(xr_lir_block_parameter(c.module,join,0,&selected));
        if (shape==2) OK(xr_lir_block_parameter(c.module,join,1,&second_parameter));
        XrLirValue args[]={selected,ca_constant(&c,join,41)};
        XrLirValue result=ca_value(&c,join,XR_LIR_CALL_INDIRECT,XR_LIR_I64,args,2);
        if (shape==2) {
            args[0]=values[0];
            result=ca_binary(&c,join,XR_LIR_ADD,result,ca_value(&c,join,XR_LIR_CALL_INDIRECT,XR_LIR_I64,args,2));
        }
        ca_return(&c,join,result);
    } else ca_return(&c,entry,values[0]);
    OK(xr_lir_module_finish(c.module,NULL));
    XrLirCallableFacts *facts=NULL; OK(xr_lir_callable_facts_create(c.module,&facts));
    XrLirCallableFunctionFacts rows; OK(xr_lir_callable_facts_function(facts,root.id,&rows));
    CHECK(branching ? rows.values[selected.id].transport==(mixed ? XR_LIR_CALLABLE_TRANSPORT_MIXED :
        shape==2 ? XR_LIR_CALLABLE_TRANSPORT_OWNED : XR_LIR_CALLABLE_TRANSPORT_FRAME) :
        rows.values[values[0].id].materialize && rows.values[values[0].id].constructor_storage==XR_LIR_CLOSURE_FRAME);
    CHECK(branching || (rows.values[values[1].id].transport==XR_LIR_CALLABLE_TRANSPORT_FRAME && !rows.values[values[1].id].materialize));
    XrtBillingOwner *billing=NULL; MANAGED(xrt_billing_owner_new(NULL,SIZE_MAX,&billing));
    XrLirValueLayoutKey keys[]={{callable,0,0},{callable,0,1},{callable,0,2},{callable,0,1},{XR_LIR_I1,0,0}};
    XrLirValueLayouts *layouts=NULL; HOME(xr_lir_value_layouts_create(c.module,keys,5,billing,&layouts));
    const XrtValuePlan *first=NULL,*second=NULL,*duplicate=NULL; uint32_t first_id=0,second_id=0,id=0;
    HOME(xr_lir_value_layouts_lookup(layouts,keys[1],&first,&first_id));
    HOME(xr_lir_value_layouts_lookup(layouts,keys[2],&second,&second_id));
    HOME(xr_lir_value_layouts_lookup(layouts,keys[3],&duplicate,&id));
    CHECK(first!=second && first_id!=second_id && duplicate==first && id==first_id);
    CHECK(xr_lir_value_layouts_get(layouts,(XrLirValueLayoutKey){XR_LIR_I64,0,1},&duplicate)==XR_LIR_VALUE_LAYOUT_ARGUMENT && duplicate==first);
    XrLirValueImage *image=NULL; HOME(xr_lir_value_image_create(layouts,billing,&image));
    const XrtValueImage *raw=xr_lir_value_image_view(image);
    CHECK(raw->types[first_id-1].entry_ordinal==1 && raw->types[second_id-1].entry_ordinal==2 &&
        raw->types[first_id-1].entry_signature==raw->types[second_id-1].entry_signature);
    XrLirOwnershipPlan *plan=NULL; OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=c.module},&plan,NULL));
    if (shape==2) {
        bool seen=false; size_t copies=0;
        for (uint32_t n=0;n<xr_lir_ownership_fact_count(plan);++n) {
            XrLirOwnershipFact fact; OK(xr_lir_ownership_fact_at(plan,n,&fact));
            if (fact.kind==XR_LIR_OWNERSHIP_BLOCK_OWNER && fact.value==second_parameter.id) { CHECK(fact.argument==1);seen=true; }
            if (fact.kind==XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER) {
                CHECK(fact.value==values[0].id && fact.argument<2 && fact.transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_COPY); ++copies;
            }
        }
        CHECK(seen && copies==4);
    }
    XrLirValueHomes *identity_homes=NULL;
    HOME(xr_lir_value_homes_create(&(XrLirValueHomesRequest){c.module,identity,layouts,plan},&identity_homes));
    CHECK(identity_homes && !identity_homes->owned_frame_needed && !identity_homes->frame.slot_count &&
        !identity_homes->values[borrowed.id].owner_count && identity_homes->values[borrowed.id].parameter==1);
    bool borrowed_return=false,borrowed_bind=false,borrowed_keep=false;
    for (size_t e=0;e<identity_homes->event_count;++e) {
        const XrLirHomeEvent *event=&identity_homes->events[e];
        if (event->kind==XR_LIR_OWNERSHIP_RETURN_TRANSFER) {
            CHECK(event->source==kept.id && event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_COPY); borrowed_return=true;
        }
        if (event->kind==XR_LIR_OWNERSHIP_CALLABLE_VALUE && event->source==bound.id) {
            CHECK(event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_BORROW); borrowed_bind=true;
        }
        if (event->kind==XR_LIR_OWNERSHIP_CALLABLE_SCOPE_EXIT) {
            CHECK(event->source==bound.id && event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_BORROW); borrowed_keep=true;
        }
    }
    CHECK(borrowed_return && borrowed_bind && borrowed_keep); xr_lir_value_homes_destroy(identity_homes);
    XrLirValueHomes *homes=NULL;
    attempts=0;
    XrLirValueLayoutStatus status=xr_lir_value_homes_create(&(XrLirValueHomesRequest){c.module,root,layouts,plan},&homes);
    size_t allocation_count=attempts;
    if (mixed) CHECK(status==XR_LIR_VALUE_LAYOUT_UNSUPPORTED && !homes);
    else if (!branching) {
        HOME(status); CHECK(homes->frame.slot_count==2 && homes->values[values[0].id].environment_type==first_id &&
            homes->values[values[0].id].owner_count==1 && !homes->values[values[1].id].owner_count);
        bool materialize=false;
        for (size_t e=0;e<homes->event_count;++e) materialize|=homes->events[e].kind==XR_LIR_OWNERSHIP_CALLABLE_MATERIALIZE;
        CHECK(materialize);
    } else if (shape==2) {
        HOME(status); CHECK(homes->transfer_count==4 && homes->edge_count==2 && homes->values[values[0].id].owner_count==1);
        for (size_t t=0;t<homes->transfer_count;++t) {
            XrLirHomeTransfer transfer=homes->transfers[t];
            CHECK(transfer.source==values[0].id && (transfer.target==selected.id || transfer.target==second_parameter.id) &&
                transfer.transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_COPY);
        }
        bool weakened_copy=false;
        for (size_t e=0;e<homes->event_count;++e) {
            const XrLirHomeEvent *event=&homes->events[e];
            if (event->kind==XR_LIR_OWNERSHIP_CALLABLE_VALUE && event->source==weakened.id) {
                CHECK(event->target==values[0].id && event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_COPY); weakened_copy=true;
            }
        }
        CHECK(weakened_copy);
        xr_lir_value_homes_destroy(homes); homes=NULL; CHECK(!live);
        for (size_t n=1;n<=allocation_count;++n) {
            attempts=0; fail_at=n;
            CHECK(xr_lir_value_homes_create(&(XrLirValueHomesRequest){c.module,root,layouts,plan},&homes)==XR_LIR_VALUE_LAYOUT_OOM &&
                !homes && !live);
        }
        fail_at=SIZE_MAX;
        printf("callable selected-copy allocation rollback sites: %zu\n",allocation_count);
    } else {
        HOME(status); CHECK(!homes->owned_frame_needed && !homes->frame.slot_count && homes->transfer_count==2);
        for (size_t t=0;t<homes->transfer_count;++t) CHECK(homes->transfers[t].transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_BORROW);
    }
    xr_lir_value_homes_destroy(homes); xr_lir_ownership_plan_destroy(plan); xr_lir_callable_facts_destroy(facts);
    xr_lir_value_image_destroy(image); xr_lir_value_layouts_destroy(layouts); xr_lir_module_destroy(c.module);
    CHECK(xrt_billing_owner_stats(billing).blocks==1); xrt_billing_owner_drop(billing);
    puts(mixed ? "mixed callable join remains explicit and unsupported" : "same-signature targets stay distinct and only a checked code-only escape materializes");
}
#include "lir_slice_homes_cases.h"
#include "lir_slice_call_cases.h"
#include "lir_slice_boundary_cases.h"
#include "lir_scoped_cell_homes_cases.h"
#include "lir_array_set_region_fixture.h"
static void array_set_region_receiving(void) {
    ArraySetRegionFixture c = array_set_region_build(NULL, 3, ARRAY_SET_REGION_LOCAL);
    c = array_set_region_build(c.module, 6, ARRAY_SET_REGION_LOCAL);
    OK(xr_lir_module_finish(c.module, NULL));
    XrtBillingOwner *billing = NULL; MANAGED(xrt_billing_owner_new(NULL, SIZE_MAX, &billing));
    XrLirValueLayoutKey roots[] = {{XR_LIR_UNIT,0,0},{XR_LIR_I1,0,0},{XR_LIR_I64,0,0},{XR_LIR_ARRAY,XR_LIR_I64,0}};
    XrLirValueLayouts *layouts = NULL; HOME(xr_lir_value_layouts_create(c.module, roots, 4, billing, &layouts));
    XrLirOwnershipPlan *plan = NULL; OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=c.module}, &plan, NULL));
    CHECK(xr_lir_ownership_array_region_count(plan) == 2);
    XrLirValueHomesRequest request = {c.module,c.function,layouts,plan}; XrLirValueHomes *homes = NULL;
    size_t baseline = live, bytes = home_live_bytes(); attempts = 0;
    HOME(xr_lir_value_homes_create(&request, &homes)); size_t sites = attempts;
    CHECK(homes && homes->array_region_count == 1 && homes->array_region_step_count && sites && live > baseline);
    xr_lir_value_homes_destroy(homes); homes = NULL;
    CHECK(live == baseline && home_live_bytes() == bytes);
    for (size_t point = 1; point <= sites; ++point) {
        attempts = 0; fail_at = point;
        CHECK(xr_lir_value_homes_create(&request, &homes) == XR_LIR_VALUE_LAYOUT_OOM && !homes &&
            attempts >= point && live == baseline && home_live_bytes() == bytes);
    }
    fail_at = SIZE_MAX; HOME(xr_lir_value_homes_create(&request, &homes));
    uint32_t count = homes->array_region_step_count;
    XrLirArraySetRegionStep first = homes->array_region_steps[0], last = homes->array_region_steps[count - 1];
    xr_lir_module_destroy(c.module); xr_lir_ownership_plan_destroy(plan); xr_lir_value_layouts_destroy(layouts);
    CHECK(homes->array_regions[0].canonical_value == c.header_array.id && homes->array_regions[0].body_block_count == c.count &&
        homes->array_region_steps[0].source == first.source && homes->array_region_steps[count - 1].target == last.target &&
        first.kind == XR_LIR_ARRAY_SET_REGION_ENTRY_MOVE && last.kind == XR_LIR_ARRAY_SET_REGION_EXIT_MOVE);
    xr_lir_value_homes_destroy(homes); CHECK(live == baseline && home_live_bytes() == bytes);
    CHECK(xrt_billing_owner_stats(billing).blocks == 1); xrt_billing_owner_drop(billing);
}
#include "lir_array_region_binding_cases.h"
int main(void) {
    for (unsigned shape=0;shape<4;++shape) array_region_binding_projection(shape);
    array_set_region_receiving();
    borrowed_map_stages();
    string_projections();
    scoped_cells();
    slice_homes();
    slice_callback_frontier();
    slice_boundaries();
    const XrLirType elements[]={XR_LIR_I64,XR_LIR_STRING,XR_LIR_U8,XR_LIR_U64};
    for (size_t i=0;i<4;++i) {
        XrLirModule *module=NULL; OK(array_fixture_create(elements[i],&module)); project(module,false);
        module=NULL; OK(array_ref_fixture(elements[i],&module)); project(module,false);
        module=NULL; OK(array_repeat_fixture(elements[i],&module)); project(module,false);
        project(roles_fixture(elements[i],false),true);
    }
    project(loop_fixture(),true);
    project(map_fixture(1),true); project(map_state_fixture(),false); project(roles_fixture(XR_LIR_I64,true),true);
    CHECK(kinds==((1u<<XR_LIR_HOME_BINDING_LOCAL)|(1u<<XR_LIR_HOME_BINDING_PARAMETER)|
        (1u<<XR_LIR_HOME_BINDING_CELL_BORROW)|(1u<<XR_LIR_HOME_BINDING_STATE_BORROW)));
    CHECK(array_roles==254 && map_roles==255 && literal_caches); array_cell_cfg(); array_block_cells();
    callable_projection(false,false); callable_projection(true,false); callable_projection(true,true);
    callable_targets_and_join(0); callable_targets_and_join(1); callable_targets_and_join(2); callable_targets_and_join(3);
    puts("binding homes: four Array families, Map roles, real pending slots, loop lifetime, input retirement and allocation rollback passed");
    return 0;
}
