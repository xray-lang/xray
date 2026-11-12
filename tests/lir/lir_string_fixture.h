/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_string_fixture.h - One synchronous signature for local and module Strings
 */
#ifndef LIR_STRING_FIXTURE_H
#define LIR_STRING_FIXTURE_H
#include "lir/xlir_string.h"
#include "lir/xlir_state.h"
#include <stdio.h>

#define STRING_TRY(call) do { status=(call); if (status != XR_LIR_OK) { \
    if (status != XR_LIR_OUT_OF_MEMORY) fprintf(stderr,"String fixture %d status %u\n",__LINE__,(unsigned)status); \
    goto failed; } } while (0)
static XrLirStatus string_ref_fixture(XrLirModule **output) {
    XrLirModule *m = NULL;
    XrLirStatus status = xr_lir_module_create(NULL, &m);
    if (status != XR_LIR_OK) return status;
    STRING_TRY(xr_lir_string_catalog_install(m, false, false, false));
    XrLirStateModule module = {0};
    XrLirStateSlot slot = {.payload=XR_LIR_STRING,.mutable=true,.size=sizeof(void *),.alignment=_Alignof(void *)};
    XrLirStateLayout layout = {.modules=&module,.module_count=1,.slots=&slot,.slot_count=1,
        .payload_size=sizeof(void *),.payload_alignment=_Alignof(void *)};
    STRING_TRY(xr_lir_set_state_layout(m, &layout));
    XrLirType state_type=XR_LIR_STATE, reference_type=XR_LIR_STRING_REF;
    XrLirFunction root={0}, helper={0};
    XrLirFunctionSpec spec={.name="root",.name_length=4,.parameters=&state_type,.parameter_count=1,.result=XR_LIR_I64};
    STRING_TRY(xr_lir_declare_function(m,&spec,&root));
    STRING_TRY(xr_lir_set_state_entry(m,root));
    spec=(XrLirFunctionSpec){.name="append",.name_length=6,.parameters=&reference_type,.parameter_count=1,.result=XR_LIR_UNIT};
    STRING_TRY(xr_lir_declare_function(m,&spec,&helper));
    XrLirLiteral seed={0}, suffix={0};
    STRING_TRY(xr_lir_add_literal(m,"a",1,&seed));
    STRING_TRY(xr_lir_add_literal(m,"r",1,&suffix));
    XrLirBlock body={0};
    STRING_TRY(xr_lir_define_function(m,helper));
    STRING_TRY(xr_lir_add_block(m,helper,NULL,0,&body));
    STRING_TRY(xr_lir_set_entry(m,helper,body));
    XrLirValue args[2]={{0}}, text={0};
    STRING_TRY(xr_lir_function_parameter(m,helper,0,&args[0]));
    XrLirInstruction op={.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=suffix.id};
    STRING_TRY(xr_lir_emit_value(m,body,&op,&args[1]));
    uint32_t intrinsic;
    STRING_TRY(xr_lir_string_operation(m,XR_LIR_STRING_OP_APPEND_REF,&intrinsic));
    op=(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_UNIT,.intrinsic=intrinsic,.operands=args,.operand_count=2};
    STRING_TRY(xr_lir_emit_effect(m,body,&op));
    op=(XrLirInstruction){.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&args[1],.operand_count=1};
    STRING_TRY(xr_lir_emit_effect(m,body,&op));
    op=(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT};
    STRING_TRY(xr_lir_set_terminator(m,body,&op));
    STRING_TRY(xr_lir_define_function(m,root));
    STRING_TRY(xr_lir_add_block(m,root,NULL,0,&body));
    STRING_TRY(xr_lir_set_entry(m,root,body));
    STRING_TRY(xr_lir_function_parameter(m,root,0,&args[0]));
    op=(XrLirInstruction){.op=XR_LIR_MODULE_BEGIN,.type=XR_LIR_UNIT,.operands=args,.operand_count=1};
    STRING_TRY(xr_lir_emit_effect(m,body,&op));
    op=(XrLirInstruction){.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=seed.id};
    STRING_TRY(xr_lir_emit_value(m,body,&op,&text));
    XrLirValue cell={0}, state={0}, reference={0}, read={0}, length={0};
    op=(XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=XR_LIR_STRING_CELL,.operands=&text,.operand_count=1};
    STRING_TRY(xr_lir_emit_value(m,body,&op,&cell));
    args[1]=text;
    op=(XrLirInstruction){.op=XR_LIR_STATE_CONSTRUCT,.type=XR_LIR_STATE_REF,.operands=args,.operand_count=2};
    STRING_TRY(xr_lir_emit_value(m,body,&op,&state));
    op=(XrLirInstruction){.op=XR_LIR_STATE_PUBLISH,.type=XR_LIR_UNIT,.operands=&state,.operand_count=1};
    STRING_TRY(xr_lir_emit_effect(m,body,&op));
    for (uint32_t kind=0;kind<2;++kind) {
        STRING_TRY(xr_lir_string_operation(m,kind ? XR_LIR_STRING_OP_BORROW_STATE : XR_LIR_STRING_OP_BORROW_CELL,&intrinsic));
        XrLirValue binding=kind ? state : cell;
        op=(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_STRING_REF,.intrinsic=intrinsic,.operands=&binding,.operand_count=1};
        STRING_TRY(xr_lir_emit_value(m,body,&op,&reference));
        op=(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_UNIT,.callee=helper,.operands=&reference,.operand_count=1};
        STRING_TRY(xr_lir_emit_effect(m,body,&op));
    }
    STRING_TRY(xr_lir_string_operation(m,XR_LIR_STRING_OP_READ_REF,&intrinsic));
    op=(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_STRING,.intrinsic=intrinsic,.operands=&reference,.operand_count=1};
    STRING_TRY(xr_lir_emit_value(m,body,&op,&read));
    op=(XrLirInstruction){.op=XR_LIR_STRING_LEN,.type=XR_LIR_I64,.operands=&read,.operand_count=1};
    STRING_TRY(xr_lir_emit_value(m,body,&op,&length));
    op=(XrLirInstruction){.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&read,.operand_count=1};
    STRING_TRY(xr_lir_emit_effect(m,body,&op));
    op.operands=&text;
    STRING_TRY(xr_lir_emit_effect(m,body,&op));
    op=(XrLirInstruction){.op=XR_LIR_STRING_CELL_DROP,.type=XR_LIR_UNIT,.operands=&cell,.operand_count=1};
    STRING_TRY(xr_lir_emit_effect(m,body,&op));
    op=(XrLirInstruction){.op=XR_LIR_MODULE_READY,.type=XR_LIR_UNIT,.operands=args,.operand_count=1};
    STRING_TRY(xr_lir_emit_effect(m,body,&op));
    op=(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&length,.operand_count=1};
    STRING_TRY(xr_lir_set_terminator(m,body,&op));
    STRING_TRY(xr_lir_module_finish(m,NULL));
    *output=m;
    return XR_LIR_OK;
failed:
    xr_lir_module_destroy(m);
    return status;
}
#undef STRING_TRY
#endif // LIR_STRING_FIXTURE_H
