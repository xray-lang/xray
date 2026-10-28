/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_string_admission.c - Reject missing, forged and retired String owners
 */
#include "vm/register/xrbc.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)

static uint32_t word(XrRbcOp op, uint32_t a, uint32_t b, uint32_t c, int64_t immediate) {
    uint32_t result = 0;
    CHECK(xr_rbc_encode(&result,(XrRbcOperands){op,a,b,c,immediate}));
    return result;
}
static void rejected(XrRbcFunction *functions, size_t count, XrtLocationOwner *locations) {
    const XrRbcLiteral literal = {"not printed",11};
    const XrRbcModule module = {.functions=functions,.function_count=count,.literals=&literal,.literal_count=1};
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_module_prepare(&module,0,NULL,0,locations,&execution)==XR_RBC_INVALID && !execution);
}
static void admitted(XrRbcFunction *functions, size_t count, XrtLocationOwner *locations) {
    const XrRbcLiteral literal = {"not printed",11};
    const XrRbcModule module = {.functions=functions,.function_count=count,.literals=&literal,.literal_count=1};
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_module_prepare(&module,0,NULL,0,locations,&execution)==XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_OK);
    xr_rbc_execution_free(&execution);xr_rbc_outcome_free(&outcome);
}
int main(void) {
    XrtLocationOwner *locations = NULL;
    CHECK(xrt_locations_create(NULL,0,&locations)==XRT_LOCATION_OK);
    uint32_t code[] = {word(XR_RBC_STRING_LITERAL,0,0,0,0),word(XR_RBC_STRING_DROP,0,0,0,0),
        word(XR_RBC_RET0,0,0,0,0)};
    XrRbcRegisterKind kinds[] = {XR_RBC_STRING,XR_RBC_CALL_WINDOW,XR_RBC_CALL_WINDOW};
    XrRbcFunction functions[2] = {{.code=code,.code_count=3,.register_count=1}};
    rejected(functions,1,locations);
    functions[0].register_kinds=kinds;functions[0].register_kind_count=1;
    admitted(functions,1,locations);
    kinds[0]=XR_RBC_BITS;rejected(functions,1,locations);kinds[0]=XR_RBC_STRING;
    functions[0].code_count=2;code[1]=word(XR_RBC_RET0,0,0,0,0);
    rejected(functions,1,locations);
    functions[0].code_count=3;code[1]=word(XR_RBC_STRING_DROP,0,0,0,0);
    code[2]=word(XR_RBC_STRING_PRINT,0,0,0,0);
    uint32_t after_drop[] = {code[0],code[1],code[2],word(XR_RBC_RET0,0,0,0,0)};
    functions[0].code=after_drop;functions[0].code_count=4;rejected(functions,1,locations);
    uint32_t call[] = {code[0],word(XR_RBC_MOV,3,0,0,0),word(XR_RBC_CALL,2,0,0,1),
        word(XR_RBC_STRING_TAKE,1,2,0,0),code[1],word(XR_RBC_STRING_DROP,1,0,0,0),word(XR_RBC_RET0,0,0,0,0)};
    uint32_t callee_code[] = {word(XR_RBC_STRING_RETAIN,1,0,0,0),word(XR_RBC_RET,1,0,0,0)};
    XrRbcRegisterKind call_kinds[] = {XR_RBC_STRING,XR_RBC_STRING,XR_RBC_CALL_WINDOW,XR_RBC_CALL_WINDOW};
    XrRbcRegisterKind callee_kinds[] = {XR_RBC_STRING,XR_RBC_STRING};
    functions[0]=(XrRbcFunction){.code=call,.code_count=7,.register_count=4,
        .register_kinds=call_kinds,.register_kind_count=4};
    functions[1]=(XrRbcFunction){.code=callee_code,.code_count=2,.register_count=2,.parameter_count=1,
        .register_kinds=callee_kinds,.register_kind_count=2,.result_kind=XR_RBC_STRING};
    admitted(functions,2,locations);
    uint32_t borrowed_call[] = {code[0],word(XR_RBC_MOV,3,0,0,0),code[1],word(XR_RBC_CALL,2,0,0,1),
        word(XR_RBC_STRING_TAKE,1,2,0,0),word(XR_RBC_STRING_DROP,1,0,0,0),word(XR_RBC_RET0,0,0,0,0)};
    functions[0].code=borrowed_call;
    rejected(functions,2,locations);
    uint32_t borrowed_return[] = {word(XR_RBC_RET,0,0,0,0)};
    functions[0].code=call;
    functions[1].code=borrowed_return;functions[1].code_count=1;functions[1].result_kind=XR_RBC_STRING;
    rejected(functions,2,locations);
    uint32_t cell[] = {code[0],word(XR_RBC_STRING_CELL_NEW,1,0,0,0),code[1],
        word(XR_RBC_CELL_READ,2,1,0,0),word(XR_RBC_STRING_DROP,2,0,0,0),
        word(XR_RBC_STRING_CELL_DROP,1,0,0,0),word(XR_RBC_RET0,0,0,0,0)};
    XrRbcRegisterKind cell_registers[] = {XR_RBC_STRING,XR_RBC_STRING_CELL,XR_RBC_STRING};
    XrRbcRegisterKind cell_kinds[] = {XR_RBC_STRING};
    functions[0]=(XrRbcFunction){.code=cell,.code_count=7,.register_count=3,.cell_count=1,
        .register_kinds=cell_registers,.register_kind_count=3,.cell_kinds=cell_kinds,.cell_kind_count=1};
    admitted(functions,1,locations);
    cell[3]=word(XR_RBC_STRING_CELL_DROP,1,0,0,0);cell[5]=word(XR_RBC_CELL_READ,2,1,0,0);
    rejected(functions,1,locations);
    cell[3]=word(XR_RBC_CELL_READ,2,1,0,0);cell[5]=word(XR_RBC_STRING_CELL_DROP,1,0,0,0);
    cell_kinds[0]=XR_RBC_BITS;rejected(functions,1,locations);cell_kinds[0]=XR_RBC_STRING;
    functions[0].result_kind=XR_RBC_STRING;
    cell[6]=word(XR_RBC_RET0,0,0,0,0);rejected(functions,1,locations);
    xrt_locations_release(locations);
    puts("String declarations, ownership and retired borrows reject before effects");
    return 0;
}
