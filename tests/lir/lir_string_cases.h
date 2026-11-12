/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_string_cases.h - Borrowed String identity, retirement and receiving ownership
 */
#include "lir_string_fixture.h"
static void string_catalog_owners(void) {
    XrLirModule *module = NULL, *copies[3] = {0};
    OK(string_ref_fixture(&module));
    OK(xr_lir_rebuild(module,&copies[0]));
    OK(xr_lir_sccp(module,&copies[1]));
    OK(xr_lir_inline(module,&copies[2]));
    module->intrinsics[0].operation=XR_LIR_STRING_OP_END;
    CHECK(xr_lir_verify(module,NULL) == XR_LIR_VERIFY_ERROR);
    xr_lir_module_destroy(module);
    for (uint32_t n=0;n<3;++n) {
        OK(xr_lir_verify(copies[n],NULL));
        uint32_t operation=0;
        OK(xr_lir_string_operation(copies[n],XR_LIR_STRING_OP_BORROW_STATE,&operation));
        CHECK(operation);
        xr_lir_module_destroy(copies[n]);
    }
}
static void string_reference_lifetime(void) {
    for (uint32_t attack=0;attack<2;++attack) {
        XrLirModule *m=NULL;
        OK(xr_lir_module_create(NULL,&m));
        OK(xr_lir_string_catalog_install(m,false,false,false));
        XrLirFunction function=test_function(m,"reference",NULL,0,XR_LIR_I64);
        XrLirBlock block=test_block(m,function,NULL,0);
        OK(xr_lir_set_entry(m,function,block));
        XrLirLiteral literal={0};
        OK(xr_lir_add_literal(m,"value",5,&literal));
        XrLirValue text={0},cell={0},reference={0},read={0},length={0};
        XrLirInstruction op={.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=literal.id};
        OK(xr_lir_emit_value(m,block,&op,&text));
        op=(XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=XR_LIR_STRING_CELL,.operands=&text,.operand_count=1};
        OK(xr_lir_emit_value(m,block,&op,&cell));
        uint32_t intrinsic;
        OK(xr_lir_string_operation(m,XR_LIR_STRING_OP_BORROW_CELL,&intrinsic));
        op=(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_STRING_REF,.intrinsic=intrinsic,
            .operands=&cell,.operand_count=1};
        OK(xr_lir_emit_value(m,block,&op,&reference));
        XrLirInstruction drop={.op=XR_LIR_STRING_CELL_DROP,.type=XR_LIR_UNIT,.operands=&cell,.operand_count=1};
        if (attack) OK(xr_lir_emit_effect(m,block,&drop));
        OK(xr_lir_string_operation(m,XR_LIR_STRING_OP_READ_REF,&intrinsic));
        op=(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_STRING,.intrinsic=intrinsic,
            .operands=&reference,.operand_count=1};
        OK(xr_lir_emit_value(m,block,&op,&read));
        op=(XrLirInstruction){.op=XR_LIR_STRING_LEN,.type=XR_LIR_I64,.operands=&read,.operand_count=1};
        OK(xr_lir_emit_value(m,block,&op,&length));
        if (!attack) OK(xr_lir_emit_effect(m,block,&drop));
        op=(XrLirInstruction){.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&read,.operand_count=1};
        OK(xr_lir_emit_effect(m,block,&op));
        op.operands=&text;
        OK(xr_lir_emit_effect(m,block,&op));
        test_return(m,block,length);
        CHECK(xr_lir_verify(m,NULL) == (attack ? XR_LIR_VERIFY_ERROR : XR_LIR_OK));
        xr_lir_module_destroy(m);
    }
}
