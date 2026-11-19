/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_set_header_edge_fixture.h - READ formal copies into real selected-edge owners
 */
#ifndef LIR_SET_HEADER_EDGE_FIXTURE_H
#define LIR_SET_HEADER_EDGE_FIXTURE_H
#include "lir_set_fixture.h"

/* Modes zero and one return 3 and 4. Mode two duplicates one owned reference
 * instead of copying the READ formal and must fail independent verification. */
static SetLirFixture set_header_edge_fixture(unsigned mode) {
    SetLirFixture c={0}; OK(xr_lir_module_create(NULL,&c.module));
    XrLirBlock entry={0},join={0}; XrLirType parameter=XR_LIR_SET_I64;
    XrLirFunctionSpec spec={.name="copy_set_formal",.name_length=15,
        .parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64};
    c.read=set_fixture_function(&c,&spec,&entry); XrLirValue formal={0};
    OK(xr_lir_function_parameter(c.module,c.read,0,&formal));
    XrLirType types[]={XR_LIR_SET_I64,XR_LIR_SET_I64}; uint32_t count=mode?2u:1u;
    OK(xr_lir_add_block(c.module,c.read,types,count,&join));
    XrLirValue sources[]={formal,formal};
    if (mode==2) {
        sources[0]=set_fixture_value(entry,(XrLirInstruction){.op=XR_LIR_SET_RETAIN,
            .type=XR_LIR_SET_I64,.operands=&formal,.operand_count=1});
        sources[1]=sources[0];
    }
    OK(xr_lir_set_terminator(c.module,entry,&(XrLirInstruction){.op=XR_LIR_BR,
        .type=XR_LIR_UNIT,.edges={{join,sources,count}},.edge_count=1}));
    XrLirValue selected[2]={{0}};
    for (uint32_t p=0;p<count;++p) OK(xr_lir_block_parameter(c.module,join,p,&selected[p]));
    XrLirValue cell=set_fixture_value(join,(XrLirInstruction){.op=XR_LIR_CELL_NEW,
        .type=XR_LIR_SET_I64_CELL,.operands=&selected[0],.operand_count=1});
    set_fixture_effect(join,(XrLirInstruction){.op=XR_LIR_SET_DROP,.type=XR_LIR_UNIT,
        .operands=&selected[0],.operand_count=1});
    XrLirValue key=set_fixture_value(join,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=8});
    XrLirValue edit[]={cell,key};
    set_fixture_effect(join,(XrLirInstruction){.op=XR_LIR_SET_ADD,.type=XR_LIR_UNIT,.operands=edit,.operand_count=2});
    XrLirValue snapshot=set_fixture_value(join,(XrLirInstruction){.op=XR_LIR_CELL_READ,
        .type=XR_LIR_SET_I64,.operands=&cell,.operand_count=1});
    XrLirValue length=set_fixture_value(join,(XrLirInstruction){.op=XR_LIR_SET_LEN,
        .type=XR_LIR_I64,.operands=&snapshot,.operand_count=1});
    set_fixture_effect(join,(XrLirInstruction){.op=XR_LIR_SET_DROP,.type=XR_LIR_UNIT,.operands=&snapshot,.operand_count=1});
    set_fixture_effect(join,(XrLirInstruction){.op=XR_LIR_SET_CELL_DROP,.type=XR_LIR_UNIT,.operands=&cell,.operand_count=1});
    if (mode) {
        XrLirValue old=set_fixture_value(join,(XrLirInstruction){.op=XR_LIR_SET_LEN,
            .type=XR_LIR_I64,.operands=&selected[1],.operand_count=1});
        set_fixture_effect(join,(XrLirInstruction){.op=XR_LIR_SET_DROP,.type=XR_LIR_UNIT,
            .operands=&selected[1],.operand_count=1});
        XrLirValue add[]={length,old};
        length=set_fixture_value(join,(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=add,.operand_count=2});
    }
    set_fixture_return(join,length);
    spec=(XrLirFunctionSpec){.name="set_header_edge_root",.name_length=20,.result=XR_LIR_I64};
    c.entry=set_fixture_function(&c,&spec,&entry);
    XrLirValue empty=set_fixture_value(entry,(XrLirInstruction){.op=XR_LIR_SET_NEW,.type=XR_LIR_SET_I64});
    cell=set_fixture_value(entry,(XrLirInstruction){.op=XR_LIR_CELL_NEW,
        .type=XR_LIR_SET_I64_CELL,.operands=&empty,.operand_count=1});
    set_fixture_effect(entry,(XrLirInstruction){.op=XR_LIR_SET_DROP,.type=XR_LIR_UNIT,.operands=&empty,.operand_count=1});
    key=set_fixture_value(entry,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=7});
    edit[0]=cell; edit[1]=key;
    set_fixture_effect(entry,(XrLirInstruction){.op=XR_LIR_SET_ADD,.type=XR_LIR_UNIT,.operands=edit,.operand_count=2});
    snapshot=set_fixture_value(entry,(XrLirInstruction){.op=XR_LIR_CELL_READ,
        .type=XR_LIR_SET_I64,.operands=&cell,.operand_count=1});
    set_fixture_effect(entry,(XrLirInstruction){.op=XR_LIR_SET_CELL_DROP,.type=XR_LIR_UNIT,.operands=&cell,.operand_count=1});
    length=set_fixture_value(entry,(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_I64,
        .callee=c.read,.operands=&snapshot,.operand_count=1});
    XrLirValue old=set_fixture_value(entry,(XrLirInstruction){.op=XR_LIR_SET_LEN,.type=XR_LIR_I64,
        .operands=&snapshot,.operand_count=1});
    set_fixture_effect(entry,(XrLirInstruction){.op=XR_LIR_SET_DROP,.type=XR_LIR_UNIT,.operands=&snapshot,.operand_count=1});
    XrLirValue add[]={length,old};
    length=set_fixture_value(entry,(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=add,.operand_count=2});
    set_fixture_return(entry,length); return c;
}
#endif // LIR_SET_HEADER_EDGE_FIXTURE_H
