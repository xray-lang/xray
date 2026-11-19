/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_set_fixture.h - One real Set maker, READ call and ref-binding mutation
 */
#ifndef LIR_SET_FIXTURE_H
#define LIR_SET_FIXTURE_H
#include "lir/xlir_state.h"
#include <string.h>
typedef struct SetLirFixture {
    XrLirModule *module;
    XrLirFunction entry, make, read, write;
    XrLirValue binding, reference, snapshot;
} SetLirFixture;
static XrLirValue set_fixture_value(XrLirBlock block, XrLirInstruction op) {
    XrLirValue value={0}; OK(xr_lir_emit_value((XrLirModule *)block.module,block,&op,&value)); return value;
}
static void set_fixture_effect(XrLirBlock block, XrLirInstruction op) {
    OK(xr_lir_emit_effect((XrLirModule *)block.module,block,&op));
}
static void set_fixture_return(XrLirBlock block, XrLirValue value) {
    XrLirInstruction op={.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=value.id?&value:NULL,.operand_count=value.id?1u:0u};
    OK(xr_lir_set_terminator((XrLirModule *)block.module,block,&op));
}
static XrLirFunction set_fixture_function(SetLirFixture *c, const XrLirFunctionSpec *spec, XrLirBlock *block) {
    XrLirFunction function={0}; OK(xr_lir_declare_function(c->module,spec,&function));
    OK(xr_lir_define_function(c->module,function));
    OK(xr_lir_add_block(c->module,function,NULL,0,block)); OK(xr_lir_set_entry(c->module,function,*block));
    return function;
}
static SetLirFixture set_lir_fixture(bool module_storage) {
    SetLirFixture c={0}; OK(xr_lir_module_create(NULL,&c.module)); XrLirBlock block={0};
    XrLirFunctionSpec spec={.name="make_set",.name_length=8,.result=XR_LIR_SET_I64};
    c.make=set_fixture_function(&c,&spec,&block);
    XrLirValue initial=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_SET_NEW,.type=XR_LIR_SET_I64});
    set_fixture_return(block,initial);
    XrLirType parameter=XR_LIR_SET_I64;
    spec=(XrLirFunctionSpec){.name="read_set",.name_length=8,.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64};
    c.read=set_fixture_function(&c,&spec,&block); XrLirValue formal={0};
    OK(xr_lir_function_parameter(c.module,c.read,0,&formal));
    XrLirValue length=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_SET_LEN,.type=XR_LIR_I64,.operands=&formal,.operand_count=1});
    set_fixture_return(block,length);
    parameter=XR_LIR_SET_I64_REF;
    spec=(XrLirFunctionSpec){.name="write_set",.name_length=9,.parameters=&parameter,.parameter_count=1,.result=XR_LIR_UNIT};
    c.write=set_fixture_function(&c,&spec,&block); OK(xr_lir_function_parameter(c.module,c.write,0,&formal));
    XrLirValue key=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=42});
    XrLirValue edit[]={formal,key};
    set_fixture_effect(block,(XrLirInstruction){.op=XR_LIR_SET_ADD,.type=XR_LIR_UNIT,.operands=edit,.operand_count=2});
    set_fixture_return(block,(XrLirValue){0});
    parameter=XR_LIR_STATE;
    spec=(XrLirFunctionSpec){.name="entry",.name_length=5,.parameters=module_storage?&parameter:NULL,
        .parameter_count=module_storage?1u:0u,.result=XR_LIR_I64};
    c.entry=set_fixture_function(&c,&spec,&block); XrLirValue state={0};
    if (module_storage) {
        XrLirStateModule module={0};
        XrLirStateSlot slot={.payload=XR_LIR_SET_I64,.mutable=true,.size=8,.alignment=8};
        OK(xr_lir_set_state_layout(c.module,&(XrLirStateLayout){.modules=&module,.module_count=1,
            .slots=&slot,.slot_count=1,.payload_size=8,.payload_alignment=8}));
        OK(xr_lir_set_state_entry(c.module,c.entry)); OK(xr_lir_function_parameter(c.module,c.entry,0,&state));
        set_fixture_effect(block,(XrLirInstruction){.op=XR_LIR_MODULE_BEGIN,.type=XR_LIR_UNIT,.operands=&state,.operand_count=1});
    }
    initial=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_SET_I64,.callee=c.make});
    if (module_storage) {
        XrLirValue construct[]={state,initial};
        c.reference=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_STATE_CONSTRUCT,.type=XR_LIR_STATE_REF,
            .operands=construct,.operand_count=2});
        set_fixture_effect(block,(XrLirInstruction){.op=XR_LIR_STATE_PUBLISH,.type=XR_LIR_UNIT,.operands=&c.reference,.operand_count=1});
        set_fixture_effect(block,(XrLirInstruction){.op=XR_LIR_MODULE_READY,.type=XR_LIR_UNIT,.operands=&state,.operand_count=1});
    } else c.reference=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=XR_LIR_SET_I64_CELL,
        .operands=&initial,.operand_count=1});
    set_fixture_effect(block,(XrLirInstruction){.op=XR_LIR_SET_DROP,.type=XR_LIR_UNIT,.operands=&initial,.operand_count=1});
    c.binding=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_SET_BORROW,.type=XR_LIR_SET_I64_REF,
        .operands=&c.reference,.operand_count=1});
    set_fixture_effect(block,(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_UNIT,.callee=c.write,.operands=&c.binding,.operand_count=1});
    c.snapshot=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_SET_REF_READ,.type=XR_LIR_SET_I64,
        .operands=&c.binding,.operand_count=1});
    length=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_I64,.callee=c.read,
        .operands=&c.snapshot,.operand_count=1});
    set_fixture_effect(block,(XrLirInstruction){.op=XR_LIR_SET_DROP,.type=XR_LIR_UNIT,.operands=&c.snapshot,.operand_count=1});
    if (!module_storage) set_fixture_effect(block,(XrLirInstruction){.op=XR_LIR_SET_CELL_DROP,.type=XR_LIR_UNIT,
        .operands=&c.reference,.operand_count=1});
    set_fixture_return(block,length); return c;
}
#endif // LIR_SET_FIXTURE_H
