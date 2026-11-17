/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_forwarding_fixture.h - Actual local binding handoffs and observable gaps
 */
#ifndef LIR_ARRAY_FORWARDING_FIXTURE_H
#define LIR_ARRAY_FORWARDING_FIXTURE_H
#include "lir/xlir_array.h"
enum ArrayForwardMode { ARRAY_FORWARD_LOCAL, ARRAY_FORWARD_NEW_CALL, ARRAY_FORWARD_READ_CALL,
    ARRAY_FORWARD_CELL_CALL, ARRAY_FORWARD_REF, ARRAY_FORWARD_WRITE, ARRAY_FORWARD_SNAPSHOT };
typedef struct ArrayForwardFixture {
    XrLirModule *module;
    XrLirFunction function;
    XrLirBlock block;
    XrLirValue initial, cell, result, saved;
} ArrayForwardFixture;
static XrLirValue array_forward_value(XrLirModule *module, XrLirBlock block, XrLirInstruction instruction) {
    XrLirValue value={0}; OK(xr_lir_emit_value(module,block,&instruction,&value)); return value;
}
static void array_forward_effect(XrLirModule *module, XrLirBlock block, XrLirInstruction instruction) {
    OK(xr_lir_emit_effect(module,block,&instruction));
}
static void array_forward_return(XrLirModule *module, XrLirBlock block, XrLirValue value) {
    OK(xr_lir_set_terminator(module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,
        .operands=value.id?&value:NULL,.operand_count=value.id?1u:0u}));
}
static ArrayForwardFixture array_forward_fixture(XrLirType element, enum ArrayForwardMode mode) {
    ArrayForwardFixture c={0}; OK(xr_lir_array_module_create(NULL,&c.module));
    XrLirFunction observe={0}; XrLirBlock block={0};
    if (mode==ARRAY_FORWARD_NEW_CALL || mode==ARRAY_FORWARD_READ_CALL || mode==ARRAY_FORWARD_CELL_CALL) {
        XrLirType type=XR_LIR_ARRAY_CELL;
        XrLirFunctionSpec spec={.name="observe_array",.name_length=13,.result=XR_LIR_UNIT,
            .parameters=mode==ARRAY_FORWARD_CELL_CALL?&type:NULL,.parameter_count=mode==ARRAY_FORWARD_CELL_CALL?1u:0u};
        OK(xr_lir_declare_function(c.module,&spec,&observe));
        if (mode==ARRAY_FORWARD_CELL_CALL) {
            XrLirValue input={0}; OK(xr_lir_function_parameter(c.module,observe,0,&input));
            OK(xr_lir_set_array_shape(c.module,input,&(XrLirArrayShape){element}));
        }
        OK(xr_lir_define_function(c.module,observe)); OK(xr_lir_add_block(c.module,observe,NULL,0,&block));
        OK(xr_lir_set_entry(c.module,observe,block)); array_forward_return(c.module,block,(XrLirValue){0});
    }
    XrLirFunctionSpec spec={.name="forward_array",.name_length=13,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module,&spec,&c.function)); OK(xr_lir_define_function(c.module,c.function));
    OK(xr_lir_add_block(c.module,c.function,NULL,0,&c.block)); OK(xr_lir_set_entry(c.module,c.function,c.block));
    block=c.block; uint32_t make=0;
    OK(xr_lir_array_operation(c.module,element,XR_LIR_ARRAY_OP_NEW,&make));
    c.initial=array_forward_value(c.module,block,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_ARRAY,.intrinsic=make});
    if (mode==ARRAY_FORWARD_SNAPSHOT) c.saved=array_forward_value(c.module,block,(XrLirInstruction){.op=XR_LIR_ARRAY_RETAIN,
        .type=XR_LIR_ARRAY,.operands=&c.initial,.operand_count=1});
    c.cell=array_forward_value(c.module,block,(XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=XR_LIR_ARRAY_CELL,
        .operands=&c.initial,.operand_count=1});
    if (mode==ARRAY_FORWARD_NEW_CALL) array_forward_effect(c.module,block,(XrLirInstruction){.op=XR_LIR_CALL,
        .type=XR_LIR_UNIT,.callee=observe});
    array_forward_effect(c.module,block,(XrLirInstruction){.op=XR_LIR_ARRAY_DROP,.type=XR_LIR_UNIT,
        .operands=&c.initial,.operand_count=1});
    XrLirValue receiver=c.cell;
    if (mode==ARRAY_FORWARD_REF) receiver=array_forward_value(c.module,block,(XrLirInstruction){.op=XR_LIR_ARRAY_BORROW,
        .type=XR_LIR_ARRAY_REF,.operands=&c.cell,.operand_count=1});
    if (mode==ARRAY_FORWARD_CELL_CALL) array_forward_effect(c.module,block,(XrLirInstruction){.op=XR_LIR_CALL,
        .type=XR_LIR_UNIT,.callee=observe,.operands=&c.cell,.operand_count=1});
    if (mode==ARRAY_FORWARD_WRITE) {
        XrLirValue replacement=array_forward_value(c.module,block,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_ARRAY,.intrinsic=make});
        XrLirValue args[]={c.cell,replacement};
        array_forward_effect(c.module,block,(XrLirInstruction){.op=XR_LIR_CELL_WRITE,.type=XR_LIR_UNIT,.operands=args,.operand_count=2});
        array_forward_effect(c.module,block,(XrLirInstruction){.op=XR_LIR_ARRAY_DROP,.type=XR_LIR_UNIT,.operands=&replacement,.operand_count=1});
    }
    if (element!=XR_LIR_STRING) {
        XrLirValue key=array_forward_value(c.module,block,(XrLirInstruction){.op=XR_LIR_CONST,.type=element,.bits=7});
        uint32_t push=0; XrLirType binding=mode==ARRAY_FORWARD_REF?XR_LIR_ARRAY_REF:XR_LIR_ARRAY_CELL;
        OK(xr_lir_array_mutator(c.module,element,binding,XR_LIR_ARRAY_OP_PUSH,&push));
        XrLirValue args[]={receiver,key};
        array_forward_effect(c.module,block,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_UNIT,
            .intrinsic=push,.operands=args,.operand_count=2});
    }
    c.result=array_forward_value(c.module,block,(XrLirInstruction){.op=XR_LIR_CELL_READ,.type=XR_LIR_ARRAY,
        .operands=&c.cell,.operand_count=1});
    if (mode==ARRAY_FORWARD_READ_CALL) array_forward_effect(c.module,block,(XrLirInstruction){.op=XR_LIR_CALL,
        .type=XR_LIR_UNIT,.callee=observe});
    array_forward_effect(c.module,block,(XrLirInstruction){.op=XR_LIR_ARRAY_CELL_DROP,.type=XR_LIR_UNIT,
        .operands=&c.cell,.operand_count=1});
    uint32_t len=0; OK(xr_lir_array_operation(c.module,element,XR_LIR_ARRAY_OP_LEN,&len));
    XrLirValue answer=array_forward_value(c.module,block,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_I64,
        .intrinsic=len,.operands=&c.result,.operand_count=1});
    array_forward_effect(c.module,block,(XrLirInstruction){.op=XR_LIR_ARRAY_DROP,.type=XR_LIR_UNIT,
        .operands=&c.result,.operand_count=1});
    if (c.saved.id) {
        XrLirValue previous=array_forward_value(c.module,block,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_I64,
            .intrinsic=len,.operands=&c.saved,.operand_count=1});
        XrLirValue args[]={answer,previous};
        answer=array_forward_value(c.module,block,(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=args,.operand_count=2});
        array_forward_effect(c.module,block,(XrLirInstruction){.op=XR_LIR_ARRAY_DROP,.type=XR_LIR_UNIT,
            .operands=&c.saved,.operand_count=1});
    }
    array_forward_return(c.module,block,answer); OK(xr_lir_module_finish(c.module,NULL)); return c;
}
#endif // LIR_ARRAY_FORWARDING_FIXTURE_H
