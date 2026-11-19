/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_set_block_cell_fixture.h - Repeated Set Header Cell lifetimes in a real loop
 */
#ifndef LIR_SET_BLOCK_CELL_FIXTURE_H
#define LIR_SET_BLOCK_CELL_FIXTURE_H
#include "lir_set_fixture.h"

/* The valid loop constructs and retires its Cell three times, returning three.
 * Invalid modes omit release, duplicate it, or read the retired binding. */
static SetLirFixture set_block_cell_fixture(unsigned invalid) {
    SetLirFixture c={0}; OK(xr_lir_module_create(NULL,&c.module));
    XrLirBlock entry={0},body={0},leave={0}; XrLirType scalar=XR_LIR_I64;
    XrLirFunctionSpec spec={.name="repeated_set_cell",.name_length=17,.result=XR_LIR_I64};
    c.entry=set_fixture_function(&c,&spec,&entry);
    OK(xr_lir_add_block(c.module,c.entry,&scalar,1,&body));
    OK(xr_lir_add_block(c.module,c.entry,&scalar,1,&leave));
    XrLirValue zero=set_fixture_value(entry,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64});
    OK(xr_lir_set_terminator(c.module,entry,&(XrLirInstruction){.op=XR_LIR_BR,
        .type=XR_LIR_UNIT,.edges={{body,&zero,1}},.edge_count=1}));
    XrLirValue index={0}; OK(xr_lir_block_parameter(c.module,body,0,&index));
    XrLirValue empty=set_fixture_value(body,(XrLirInstruction){.op=XR_LIR_SET_NEW,.type=XR_LIR_SET_I64});
    XrLirValue cell=set_fixture_value(body,(XrLirInstruction){.op=XR_LIR_CELL_NEW,
        .type=XR_LIR_SET_I64_CELL,.operands=&empty,.operand_count=1});
    set_fixture_effect(body,(XrLirInstruction){.op=XR_LIR_SET_DROP,.type=XR_LIR_UNIT,.operands=&empty,.operand_count=1});
    XrLirValue edit[]={cell,index};
    set_fixture_effect(body,(XrLirInstruction){.op=XR_LIR_SET_ADD,.type=XR_LIR_UNIT,.operands=edit,.operand_count=2});
    XrLirInstruction drop={.op=XR_LIR_SET_CELL_DROP,.type=XR_LIR_UNIT,.operands=&cell,.operand_count=1};
    if (invalid!=1) set_fixture_effect(body,drop);
    if (invalid==2) set_fixture_effect(body,drop);
    if (invalid==3) {
        XrLirValue read=set_fixture_value(body,(XrLirInstruction){.op=XR_LIR_CELL_READ,
            .type=XR_LIR_SET_I64,.operands=&cell,.operand_count=1});
        set_fixture_effect(body,(XrLirInstruction){.op=XR_LIR_SET_DROP,.type=XR_LIR_UNIT,.operands=&read,.operand_count=1});
    }
    XrLirValue one=set_fixture_value(body,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=1});
    XrLirValue add[]={index,one};
    XrLirValue next=set_fixture_value(body,(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=add,.operand_count=2});
    XrLirValue limit=set_fixture_value(body,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=3});
    XrLirValue compare[]={next,limit};
    XrLirValue again=set_fixture_value(body,(XrLirInstruction){.op=XR_LIR_ICMP,.type=XR_LIR_I1,
        .predicate=XR_LIR_LT,.operands=compare,.operand_count=2});
    OK(xr_lir_set_terminator(c.module,body,&(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
        .operands=&again,.operand_count=1,.edges={{body,&next,1},{leave,&next,1}},.edge_count=2}));
    XrLirValue result={0}; OK(xr_lir_block_parameter(c.module,leave,0,&result));
    set_fixture_return(leave,result); return c;
}
#endif // LIR_SET_BLOCK_CELL_FIXTURE_H
