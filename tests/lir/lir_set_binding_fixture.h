/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_set_binding_fixture.h - Borrowed Set copies into actual typed bindings
 */
#ifndef LIR_SET_BINDING_FIXTURE_H
#define LIR_SET_BINDING_FIXTURE_H
#include "lir_set_fixture.h"
static XrLirValue set_binding_key(XrLirBlock block,int64_t value) {
    return set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=(uint64_t)value});
}
static void set_binding_add(XrLirBlock block,XrLirValue binding,int64_t key) {
    XrLirValue edit[]={binding,set_binding_key(block,key)};
    set_fixture_effect(block,(XrLirInstruction){.op=XR_LIR_SET_ADD,.type=XR_LIR_UNIT,.operands=edit,.operand_count=2});
}
static XrLirValue set_binding_new_cell(XrLirBlock block) {
    XrLirValue empty=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_SET_NEW,.type=XR_LIR_SET_I64});
    XrLirValue cell=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=XR_LIR_SET_I64_CELL,
        .operands=&empty,.operand_count=1});
    set_fixture_effect(block,(XrLirInstruction){.op=XR_LIR_SET_DROP,.type=XR_LIR_UNIT,.operands=&empty,.operand_count=1});
    return cell;
}
static XrLirValue set_binding_read(XrLirBlock block,XrLirValue binding) {
    return set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_SET_REF_READ,.type=XR_LIR_SET_I64,
        .operands=&binding,.operand_count=1});
}
static XrLirValue set_binding_len(XrLirBlock block,XrLirValue value) {
    return set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_SET_LEN,.type=XR_LIR_I64,.operands=&value,.operand_count=1});
}
/* Each mode copies a genuine READ formal, mutates the destination, and keeps
 * both the source snapshot and the destination's previous snapshot alive. */
static SetLirFixture set_binding_formal_fixture(unsigned mode) {
    OK(mode<=2?XR_LIR_OK:XR_LIR_INVALID_ARGUMENT);
    SetLirFixture c={0};OK(xr_lir_module_create(NULL,&c.module));XrLirBlock block={0};
    if (mode==2) {
        XrLirStateModule module={0};XrLirStateSlot slot={.payload=XR_LIR_SET_I64,.mutable=true,.size=8,.alignment=8};
        OK(xr_lir_set_state_layout(c.module,&(XrLirStateLayout){.modules=&module,.module_count=1,.slots=&slot,
            .slot_count=1,.payload_size=8,.payload_alignment=8}));
    }
    XrLirType parameters[2]={mode==1?XR_LIR_SET_I64_REF:XR_LIR_SET_I64,
        mode==2?XR_LIR_STATE:XR_LIR_SET_I64};
    XrLirFunctionSpec spec={.name="copy_set",.name_length=8,.parameters=parameters,
        .parameter_count=mode?2u:1u,.result=XR_LIR_I64};
    c.write=set_fixture_function(&c,&spec,&block);XrLirValue formal={0},target={0},state={0};
    OK(xr_lir_function_parameter(c.module,c.write,mode==1?1u:0u,&formal));
    if (!mode) target=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=XR_LIR_SET_I64_CELL,
        .operands=&formal,.operand_count=1});
    else {
        OK(xr_lir_function_parameter(c.module,c.write,mode==1?0u:1u,&target));
        if (mode==2) {
            state=target;target=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_STATE_REFERENCE,
                .type=XR_LIR_STATE_REF,.operands=&state,.operand_count=1});
        }
        XrLirValue write[]={target,formal};
        set_fixture_effect(block,(XrLirInstruction){.op=mode==1?XR_LIR_SET_REF_WRITE:XR_LIR_STATE_WRITE,
            .type=XR_LIR_UNIT,.operands=write,.operand_count=2});
    }
    XrLirValue reference=target;
    if (mode!=1) reference=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_SET_BORROW,
        .type=XR_LIR_SET_I64_REF,.operands=&target,.operand_count=1});
    set_binding_add(block,reference,3);XrLirValue snapshot=set_binding_read(block,reference);
    XrLirValue length=set_binding_len(block,snapshot);
    set_fixture_effect(block,(XrLirInstruction){.op=XR_LIR_SET_DROP,.type=XR_LIR_UNIT,.operands=&snapshot,.operand_count=1});
    if (!mode) {
        set_fixture_effect(block,(XrLirInstruction){.op=XR_LIR_SET_CELL_DROP,.type=XR_LIR_UNIT,
            .operands=&target,.operand_count=1});
    }
    set_fixture_return(block,length);
    XrLirType state_type=XR_LIR_STATE;
    spec=(XrLirFunctionSpec){.name="entry",.name_length=5,.parameters=mode==2?&state_type:NULL,
        .parameter_count=mode==2?1u:0u,.result=XR_LIR_I64};
    c.entry=set_fixture_function(&c,&spec,&block);state=(XrLirValue){0};
    if (mode==2) {
        OK(xr_lir_set_state_entry(c.module,c.entry));
        OK(xr_lir_function_parameter(c.module,c.entry,0,&state));
        set_fixture_effect(block,(XrLirInstruction){.op=XR_LIR_MODULE_BEGIN,.type=XR_LIR_UNIT,.operands=&state,.operand_count=1});
    }
    XrLirValue first=set_binding_new_cell(block);
    XrLirValue first_ref=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_SET_BORROW,
        .type=XR_LIR_SET_I64_REF,.operands=&first,.operand_count=1});
    set_binding_add(block,first_ref,1);set_binding_add(block,first_ref,2);
    XrLirValue incoming=set_binding_read(block,first_ref),second={0};
    if (mode==2) {
        XrLirValue empty=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_SET_NEW,.type=XR_LIR_SET_I64});
        XrLirValue construct[]={state,empty};second=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_STATE_CONSTRUCT,
            .type=XR_LIR_STATE_REF,.operands=construct,.operand_count=2});
        set_fixture_effect(block,(XrLirInstruction){.op=XR_LIR_STATE_PUBLISH,.type=XR_LIR_UNIT,.operands=&second,.operand_count=1});
        set_fixture_effect(block,(XrLirInstruction){.op=XR_LIR_MODULE_READY,.type=XR_LIR_UNIT,.operands=&state,.operand_count=1});
        set_fixture_effect(block,(XrLirInstruction){.op=XR_LIR_SET_DROP,.type=XR_LIR_UNIT,.operands=&empty,.operand_count=1});
    } else second=set_binding_new_cell(block);
    XrLirValue second_ref=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_SET_BORROW,
        .type=XR_LIR_SET_I64_REF,.operands=&second,.operand_count=1});
    set_binding_add(block,second_ref,9);XrLirValue old=set_binding_read(block,second_ref);
    XrLirValue args[]={mode==1?second_ref:incoming,mode==1?incoming:state};
    XrLirValue result=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_I64,.callee=c.write,
        .operands=args,.operand_count=mode?2u:1u});
    XrLirValue counts[]={result,set_binding_len(block,incoming)};
    result=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=counts,.operand_count=2});
    counts[0]=result;counts[1]=set_binding_len(block,old);
    result=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=counts,.operand_count=2});
    XrLirValue owned[]={incoming,old};
    for (uint32_t n=0;n<2;++n) set_fixture_effect(block,(XrLirInstruction){.op=XR_LIR_SET_DROP,
        .type=XR_LIR_UNIT,.operands=&owned[n],.operand_count=1});
    set_fixture_effect(block,(XrLirInstruction){.op=XR_LIR_SET_CELL_DROP,.type=XR_LIR_UNIT,.operands=&first,.operand_count=1});
    if (mode!=2) {
        set_fixture_effect(block,(XrLirInstruction){.op=XR_LIR_SET_CELL_DROP,.type=XR_LIR_UNIT,
            .operands=&second,.operand_count=1});
    }
    set_fixture_return(block,result);return c;
}
#endif // LIR_SET_BINDING_FIXTURE_H
