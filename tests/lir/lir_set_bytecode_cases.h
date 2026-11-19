/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_set_bytecode_cases.h - Real Set calls, bindings and selected-edge owners
 */
#ifndef LIR_SET_BYTECODE_CASES_H
#define LIR_SET_BYTECODE_CASES_H
#include "lir_set_binding_fixture.h"
#include "lir_set_header_edge_fixture.h"
#include "lir_set_block_cell_fixture.h"
static XrLirValue set_edge_owned(XrLirBlock block,const int64_t *keys,size_t count) {
    XrLirValue cell=set_binding_new_cell(block);
    XrLirValue reference=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_SET_BORROW,
        .type=XR_LIR_SET_I64_REF,.operands=&cell,.operand_count=1});
    for (size_t n=0;n<count;++n) set_binding_add(block,reference,keys[n]);
    XrLirValue result=set_binding_read(block,reference);
    set_fixture_effect(block,(XrLirInstruction){.op=XR_LIR_SET_CELL_DROP,.type=XR_LIR_UNIT,
        .operands=&cell,.operand_count=1});
    return result;
}
static SetLirFixture set_edge_fixture(bool condition) {
    SetLirFixture c={0}; OK(xr_lir_module_create(NULL,&c.module));
    XrLirBlock make_block={0};
    XrLirFunctionSpec make_spec={.name="make_right_set",.name_length=14,.result=XR_LIR_SET_I64};
    XrLirFunction make=set_fixture_function(&c,&make_spec,&make_block);
    const int64_t right_keys[]={7};
    set_fixture_return(make_block,set_edge_owned(make_block,right_keys,1));
    XrLirType parameters[]={XR_LIR_SET_I64,XR_LIR_I1};
    XrLirFunctionSpec spec={.name="select_set",.name_length=10,.parameters=parameters,
        .parameter_count=2,.result=XR_LIR_SET_I64};
    XrLirBlock entry={0},left={0},right={0},join={0};
    c.read=set_fixture_function(&c,&spec,&entry);
    OK(xr_lir_add_block(c.module,c.read,NULL,0,&left)); OK(xr_lir_add_block(c.module,c.read,NULL,0,&right));
    OK(xr_lir_add_block(c.module,c.read,parameters,1,&join));
    XrLirValue formal={0},flag={0},selected={0};
    OK(xr_lir_function_parameter(c.module,c.read,0,&formal));
    OK(xr_lir_function_parameter(c.module,c.read,1,&flag));
    OK(xr_lir_block_parameter(c.module,join,0,&selected));
    OK(xr_lir_set_terminator(c.module,entry,&(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
        .operands=&flag,.operand_count=1,.edges={{left,NULL,0},{right,NULL,0}},.edge_count=2}));
    XrLirValue owned=set_fixture_value(left,(XrLirInstruction){.op=XR_LIR_SET_RETAIN,.type=XR_LIR_SET_I64,
        .operands=&formal,.operand_count=1});
    OK(xr_lir_set_terminator(c.module,left,&(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
        .edges={{join,&owned,1}},.edge_count=1}));
    owned=set_fixture_value(right,(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_SET_I64,.callee=make});
    OK(xr_lir_set_terminator(c.module,right,&(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
        .edges={{join,&owned,1}},.edge_count=1})); set_fixture_return(join,selected);
    spec=(XrLirFunctionSpec){.name="set_edge_root",.name_length=13,.result=XR_LIR_I64};
    c.entry=set_fixture_function(&c,&spec,&entry);
    const int64_t input_keys[]={1,3}; XrLirValue input=set_edge_owned(entry,input_keys,2);
    flag=set_fixture_value(entry,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I1,.bits=condition});
    XrLirValue args[]={input,flag};
    selected=set_fixture_value(entry,(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_SET_I64,
        .callee=c.read,.operands=args,.operand_count=2});
    XrLirValue before=set_fixture_value(entry,(XrLirInstruction){.op=XR_LIR_SET_LEN,.type=XR_LIR_I64,.operands=&input,.operand_count=1});
    XrLirValue after=set_fixture_value(entry,(XrLirInstruction){.op=XR_LIR_SET_LEN,.type=XR_LIR_I64,.operands=&selected,.operand_count=1});
    XrLirValue lengths[]={before,after};
    XrLirValue sum=set_fixture_value(entry,(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=lengths,.operand_count=2});
    set_fixture_effect(entry,(XrLirInstruction){.op=XR_LIR_SET_DROP,.type=XR_LIR_UNIT,.operands=&selected,.operand_count=1});
    set_fixture_effect(entry,(XrLirInstruction){.op=XR_LIR_SET_DROP,.type=XR_LIR_UNIT,.operands=&input,.operand_count=1});
    set_fixture_return(entry,sum); return c;
}
static void set_bytecode_run(SetLirFixture c,uint64_t expected) {
    OK(xr_lir_module_finish(c.module,NULL)); XrRbcProgram *program=compile(c.module,c.entry);
    const XrRbcModule *module=xr_rbc_program_view(program); bool reader=false,stage=false;
    for (size_t f=0;f<module->function_count;++f) {
        const XrRbcValueCode *code=xr_rbc_module_value_code(module,(uint32_t)f);
        CHECK(code && code->homes);
        if (code->homes->source_function==c.read.id && c.make.id) {
            CHECK(!code->homes->owned_frame_needed && !code->homes->frame.slot_count);
            reader=true;
        }
        for (size_t n=0;n<code->access_count;++n) {
            XrRbcValueAccess access=code->accesses[n];
            if (access.operation==XR_RBC_VALUE_SET) {
                CHECK(access.other && access.other<=code->homes->event_count &&
                    code->homes->events[access.other-1].kind==XR_LIR_OWNERSHIP_SET_OPERATION);
            }
            if (access.operation!=XR_RBC_VALUE_BINDING_NEW && access.operation!=XR_RBC_VALUE_BINDING_WRITE) continue;
            if (access.window==UINT32_MAX) continue;
            CHECK(access.window<code->homes->workspace_count);
            const XrLirHomeWorkspace *workspace=&code->homes->workspaces[access.window];
            CHECK(workspace->kind==XR_LIR_HOME_WORKSPACE_VALUE && workspace->value &&
                workspace->temporary_offset==SIZE_MAX && workspace->temporary_first_owner==SIZE_MAX);
            stage=true;
        }
    }
    if (c.make.id) CHECK(reader);
    if (c.write.id && !c.make.id) CHECK(stage);
    XrRbcModule raw=*module; raw.code_lease=(XrRbcCodeLease){0}; XrRbcExecution *foreign=NULL;
    CHECK(xr_rbc_module_prepare(&raw,0,NULL,0,xr_rbc_program_locations(program),&foreign)==XR_RBC_INVALID && !foreign);
    xr_lir_module_destroy(c.module); execute(program,expected);
}
static void set_bytecode_boundaries(void) {
    set_bytecode_run(set_header_edge_fixture(0),3);
    set_bytecode_run(set_header_edge_fixture(1),4);
    set_bytecode_run(set_block_cell_fixture(0),3);
    for (unsigned state=0;state<2;++state) set_bytecode_run(set_lir_fixture(state!=0),1);
    for (unsigned mode=0;mode<3;++mode) set_bytecode_run(set_binding_formal_fixture(mode),6);
    set_bytecode_run(set_edge_fixture(false),3); set_bytecode_run(set_edge_fixture(true),4);
}
#endif // LIR_SET_BYTECODE_CASES_H
