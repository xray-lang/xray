/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * register_string_library_cases.h - Actual String bindings survive nested frame growth
 */
#include "lir/xlir_string.h"
static void test_raw_string_references(void) {
    for (uint32_t attack = 0; attack < 6; ++attack) {
        uint32_t code[11], child[132];
        XrRbcRegisterKind kinds[] = {XR_RBC_STRING, XR_RBC_STRING_CELL, XR_RBC_STRING_REF,
            XR_RBC_STRING, XR_RBC_BITS, XR_RBC_CALL_WINDOW, XR_RBC_CALL_WINDOW};
        XrRbcRegisterKind child_kinds[] = {XR_RBC_STRING_REF, XR_RBC_STRING, XR_RBC_STRING_CELL};
        XrRbcRegisterKind cells[64];
        for (uint32_t n = 0; n < 64; ++n) cells[n] = XR_RBC_STRING;
        XrRbcLiteral literals[] = {{"a",1},{"r",1}};
        XrRbcArrayLocation operands[] = {{1,false},{2,false}};
        XrRbcArrayLocation child_operands[] = {{0,false},{1,false}};
        XrRbcAggregateAccess accesses[] = {
            {.operation=XR_RBC_STRING_LIBRARY,.slot=XR_LIR_STRING_OP_BORROW_CELL,
             .first=0,.count=1,.result={2,false}},
            {.operation=XR_RBC_STRING_LIBRARY,.slot=XR_LIR_STRING_OP_READ_REF,
             .first=1,.count=1,.result={3,false}}};
        XrRbcAggregateAccess append = {.operation=XR_RBC_STRING_LIBRARY,
            .slot=XR_LIR_STRING_OP_APPEND_REF,.count=2};
        immediate(code,0,XR_RBC_STRING_LITERAL,0,0);
        emit(code,1,XR_RBC_STRING_CELL_NEW,1,0,0);
        emit(code,2,XR_RBC_STRING_DROP,0,0,0);
        immediate(code,3,XR_RBC_AGGREGATE_ACCESS,0,0);
        emit(code,4,XR_RBC_MOV,6,2,0);
        immediate(code,5,XR_RBC_CALL,5,1);
        immediate(code,6,XR_RBC_AGGREGATE_ACCESS,0,1);
        emit(code,7,XR_RBC_STRING_LEN,4,3,0);
        emit(code,8,XR_RBC_STRING_DROP,3,0,0);
        emit(code,9,XR_RBC_STRING_CELL_DROP,1,0,0);
        emit(code,10,XR_RBC_RET,4,0,0);
        immediate(child,0,XR_RBC_STRING_LITERAL,1,1);
        for (uint32_t n = 0; n < 64; ++n) {
            emit(child,1+2*n,XR_RBC_STRING_CELL_NEW,2,1,n);
            emit(child,2+2*n,XR_RBC_STRING_CELL_DROP,2,0,0);
        }
        immediate(child,129,XR_RBC_AGGREGATE_ACCESS,0,0);
        emit(child,130,XR_RBC_STRING_DROP,1,0,0);
        emit(child,131,XR_RBC_RET0,0,0,0);
        XrRbcFunction functions[] = {
            {.code=code,.code_count=11,.register_count=7,.cell_count=1,
             .register_kinds=kinds,.register_kind_count=7,.cell_kinds=cells,.cell_kind_count=1,
             .aggregate_accesses=accesses,.aggregate_access_count=2,
             .aggregate_operands=operands,.aggregate_operand_count=2},
            {.code=child,.code_count=132,.register_count=3,.parameter_count=1,.cell_count=64,
             .register_kinds=child_kinds,.register_kind_count=3,.cell_kinds=cells,.cell_kind_count=64,
             .aggregate_accesses=&append,.aggregate_access_count=1,
             .aggregate_operands=child_operands,.aggregate_operand_count=2}};
        XrRbcModule module = {.functions=functions,.function_count=2,.literals=literals,.literal_count=2};
        if (attack == 1) accesses[0].slot = XR_LIR_STRING_OP_END;
        if (attack == 2) operands[0].index = 0;
        if (attack == 3) {
            emit(code,4,XR_RBC_STRING_CELL_DROP,1,0,0);
            immediate(code,5,XR_RBC_AGGREGATE_ACCESS,0,1);
            emit(code,6,XR_RBC_STRING_LEN,4,3,0);
            emit(code,7,XR_RBC_STRING_DROP,3,0,0);
            emit(code,8,XR_RBC_RET,4,0,0);
            functions[0].code_count = 9;
        }
        if (attack == 4) immediate(code,3,XR_RBC_LOADI,2,1);
        if (attack == 5) accesses[0].count = 2;
        if (attack) reject_prepare(&module,0,NULL,0);
        else {
            uint64_t result = 0;
            CHECK(execute(&module,0,NULL,0,&result) == XR_RBC_OK && result == 2);
        }
    }
}
