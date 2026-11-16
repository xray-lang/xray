/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_slice_state_read_fixture.h - Real module Array loans without write references
 */
#ifndef LIR_SLICE_STATE_READ_FIXTURE_H
#define LIR_SLICE_STATE_READ_FIXTURE_H
#include "lir/xlir_array.h"
#include "lir/xlir_state.h"
#include "lir/xlir_view.h"
typedef struct SliceStateReadFixture {
    XrLirModule *module;
    XrLirFunction root;
    XrLirBlock block;
    XrLirValue reference, view;
} SliceStateReadFixture;
static XrLirValue slice_state_read_value(SliceStateReadFixture *c, XrLirInstruction op) {
    XrLirValue value = {0}; OK(xr_lir_emit_value(c->module,c->block,&op,&value)); return value;
}
static void slice_state_read_effect(SliceStateReadFixture *c, XrLirInstruction op) {
    OK(xr_lir_emit_effect(c->module,c->block,&op));
}
/* Ready=false leaves a real initialized slot in its initializing module.
 * Both consumers must reject its READ without publishing a Slice keeper. */
static SliceStateReadFixture slice_state_read_fixture(XrLirType element, bool mutable, bool ready) {
    SliceStateReadFixture c = {0}; OK(xr_lir_module_create(NULL,&c.module));
    OK(xr_lir_slice_catalog_install(c.module)); XrLirType slice = 0;
    OK(xr_lir_slice_type(c.module,element,&slice));
    XrLirStateModule module = {0};
    XrLirStateSlot slot = {.payload=XR_LIR_ARRAY,.mutable=mutable,.size=8,.alignment=8,.array_element=element};
    OK(xr_lir_set_state_layout(c.module,&(XrLirStateLayout){.modules=&module,.module_count=1,
        .slots=&slot,.slot_count=1,.payload_size=8,.payload_alignment=8}));
    XrLirType parameter = XR_LIR_STATE;
    XrLirFunctionSpec spec = {.name="read_state_slice",.name_length=16,.parameters=&parameter,
        .parameter_count=1,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module,&spec,&c.root)); OK(xr_lir_set_state_entry(c.module,c.root));
    OK(xr_lir_define_function(c.module,c.root)); XrLirValue state = {0};
    OK(xr_lir_function_parameter(c.module,c.root,0,&state));
    OK(xr_lir_add_block(c.module,c.root,NULL,0,&c.block)); OK(xr_lir_set_entry(c.module,c.root,c.block));
    slice_state_read_effect(&c,(XrLirInstruction){.op=XR_LIR_MODULE_BEGIN,.type=XR_LIR_UNIT,
        .operands=&state,.operand_count=1});
    XrLirValue values[3];
    for (uint32_t n = 0; n < 3; ++n)
        values[n] = slice_state_read_value(&c,(XrLirInstruction){.op=XR_LIR_CONST,.type=element,.bits=11u*(n+1)});
    uint32_t id = 0; OK(xr_lir_array_operation(c.module,element,XR_LIR_ARRAY_OP_NEW,&id));
    XrLirValue array = slice_state_read_value(&c,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,
        .type=XR_LIR_ARRAY,.intrinsic=id,.operands=values,.operand_count=3});
    XrLirValue initial[] = {state,array};
    if (mutable) {
        c.reference = slice_state_read_value(&c,(XrLirInstruction){.op=XR_LIR_STATE_CONSTRUCT,
            .type=XR_LIR_STATE_REF,.operands=initial,.operand_count=2});
        slice_state_read_effect(&c,(XrLirInstruction){.op=XR_LIR_STATE_PUBLISH,.type=XR_LIR_UNIT,
            .operands=&c.reference,.operand_count=1});
    } else {
        slice_state_read_effect(&c,(XrLirInstruction){.op=XR_LIR_STATE_INITIALIZE,.type=XR_LIR_UNIT,
            .operands=initial,.operand_count=2});
        c.reference = slice_state_read_value(&c,(XrLirInstruction){.op=XR_LIR_STATE_REFERENCE,
            .type=XR_LIR_STATE_REF,.operands=&state,.operand_count=1});
    }
    slice_state_read_effect(&c,(XrLirInstruction){.op=XR_LIR_ARRAY_DROP,.type=XR_LIR_UNIT,
        .operands=&array,.operand_count=1});
    if (ready) slice_state_read_effect(&c,(XrLirInstruction){.op=XR_LIR_MODULE_READY,.type=XR_LIR_UNIT,
        .operands=&state,.operand_count=1});
    XrLirValue first = slice_state_read_value(&c,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=0});
    XrLirValue last = slice_state_read_value(&c,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=3});
    XrLirValue range[] = {c.reference,first,last};
    OK(xr_lir_slice_operation(c.module,XR_LIR_SLICE_OP_NEW,element,XR_LIR_STATE_REF,&id));
    c.view = slice_state_read_value(&c,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,
        .type=slice,.intrinsic=id,.operands=range,.operand_count=3});
    XrLirValue length = {0}, item = {0};
    OK(xr_lir_slice_operation(c.module,XR_LIR_SLICE_OP_LEN,element,0,&id));
    length = slice_state_read_value(&c,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,
        .type=XR_LIR_I64,.intrinsic=id,.operands=&c.view,.operand_count=1});
    XrLirValue index = slice_state_read_value(&c,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=1});
    XrLirValue read[] = {c.view,index}; OK(xr_lir_slice_operation(c.module,XR_LIR_SLICE_OP_GET,element,0,&id));
    item = slice_state_read_value(&c,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,
        .type=element,.intrinsic=id,.operands=read,.operand_count=2});
    if (element == XR_LIR_U8) item = slice_state_read_value(&c,(XrLirInstruction){.op=XR_LIR_INTCAST,
        .type=XR_LIR_I64,.operands=&item,.operand_count=1});
    OK(xr_lir_slice_operation(c.module,XR_LIR_SLICE_OP_END,element,0,&id));
    slice_state_read_effect(&c,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_UNIT,
        .intrinsic=id,.operands=&c.view,.operand_count=1});
    XrLirValue parts[] = {length,item};
    XrLirValue total = slice_state_read_value(&c,(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,
        .operands=parts,.operand_count=2});
    XrLirInstruction finish = {.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&total,.operand_count=1};
    OK(xr_lir_set_terminator(c.module,c.block,&finish));
    return c;
}
#endif // LIR_SLICE_STATE_READ_FIXTURE_H
