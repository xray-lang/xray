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
static void forward_string_owners(XrtLocationOwner *locations) {
    uint32_t code[] = {word(XR_RBC_LOADI,0,0,0,1),word(XR_RBC_LOADI,1,0,0,0),
        word(XR_RBC_EQ,0,1,0,0),0,word(XR_RBC_STRING_LITERAL,2,0,0,0),
        word(XR_RBC_STRING_TAKE,3,2,0,0),0,word(XR_RBC_STRING_LITERAL,4,0,0,0),
        word(XR_RBC_STRING_TAKE,3,4,0,0),word(XR_RBC_STRING_DROP,3,0,0,0),word(XR_RBC_RET0,0,0,0,0)};
    CHECK(xr_rbc_jump(&code[3],3,7,11)); CHECK(xr_rbc_jump(&code[6],6,9,11));
    XrRbcRegisterKind kinds[] = {XR_RBC_BITS,XR_RBC_BITS,XR_RBC_STRING,XR_RBC_STRING,XR_RBC_STRING};
    XrRbcFunction f = {.code=code,.code_count=11,.register_count=5,.register_kinds=kinds,.register_kind_count=5};
    admitted(&f,1,locations);code[0]=word(XR_RBC_LOADI,0,0,0,0);admitted(&f,1,locations);
    uint32_t saved=code[8];code[8]=word(XR_RBC_STRING_DROP,4,0,0,0);rejected(&f,1,locations);code[8]=saved;
    code[8]=word(XR_RBC_MOV,3,4,0,0);rejected(&f,1,locations);code[8]=saved;
    kinds[3]=XR_RBC_BITS;rejected(&f,1,locations);kinds[3]=XR_RBC_STRING;
    saved=code[5];code[5]=word(XR_RBC_STRING_RETAIN,3,2,0,0);rejected(&f,1,locations);code[5]=saved;
    saved=code[6];CHECK(xr_rbc_jump(&code[6],6,4,11));rejected(&f,1,locations);code[6]=saved;
    uint32_t swap[] = {word(XR_RBC_STRING_LITERAL,0,0,0,0),word(XR_RBC_STRING_LITERAL,1,0,0,0),
        word(XR_RBC_STRING_TAKE,2,0,0,0),word(XR_RBC_STRING_TAKE,0,1,0,0),
        word(XR_RBC_STRING_TAKE,1,2,0,0),word(XR_RBC_STRING_DROP,0,0,0,0),
        word(XR_RBC_STRING_DROP,1,0,0,0),word(XR_RBC_RET0,0,0,0,0)};
    XrRbcRegisterKind strings[] = {XR_RBC_STRING,XR_RBC_STRING,XR_RBC_STRING};
    f=(XrRbcFunction){.code=swap,.code_count=8,.register_count=3,.register_kinds=strings,.register_kind_count=3};
    admitted(&f,1,locations);strings[2]=XR_RBC_BITS;rejected(&f,1,locations);
}
static void loop_entry_obligations(XrtLocationOwner *locations) {
    uint32_t code[]={word(XR_RBC_LOADI,0,0,0,0),word(XR_RBC_LOADI,1,0,0,3),word(XR_RBC_LOADI,2,0,0,1),
        word(XR_RBC_LT,0,1,0,0),0,0,word(XR_RBC_STRING_LITERAL,3,0,0,0),word(XR_RBC_STRING_DROP,3,0,0,0),
        word(XR_RBC_ADD,0,0,2,0),0,word(XR_RBC_RET,0,0,0,0)};
    CHECK(xr_rbc_jump(&code[4],4,6,11)&&xr_rbc_jump(&code[5],5,10,11)&&xr_rbc_jump(&code[9],9,3,11));
    XrRbcRegisterKind kinds[]={XR_RBC_BITS,XR_RBC_BITS,XR_RBC_BITS,XR_RBC_STRING};
    XrRbcFunction functions[2]={{.code=code,.code_count=11,.register_count=4,.register_kinds=kinds,.register_kind_count=4}};
    const XrRbcLiteral literal={"not printed",11};
    const XrRbcModule module={.functions=functions,.function_count=1,.literals=&literal,.literal_count=1};
    for(unsigned run=0;run<2;++run) {
        XrRbcExecution *execution=NULL;CHECK(xr_rbc_module_prepare(&module,0,NULL,0,locations,&execution)==XR_RBC_OK);
        XrRbcOutcome *outcome=xr_rbc_execution_run(execution);CHECK(outcome&&xr_rbc_outcome_status(outcome)==XR_RBC_OK);
        xr_rbc_execution_free(&execution);uint64_t count=99;CHECK(xr_rbc_outcome_bits(outcome,&count)&&count==3);xr_rbc_outcome_free(&outcome);
    }
    uint32_t saved=code[7];code[7]=word(XR_RBC_LOADI,2,0,0,1);rejected(functions,1,locations);code[7]=saved;
    uint32_t head_owner[]={word(XR_RBC_STRING_LITERAL,3,0,0,0),word(XR_RBC_LOADI,0,0,0,0),
        word(XR_RBC_LOADI,1,0,0,3),word(XR_RBC_LOADI,2,0,0,1),word(XR_RBC_LT,0,1,0,0),0,0,
        word(XR_RBC_STRING_DROP,3,0,0,0),word(XR_RBC_ADD,0,0,2,0),0,word(XR_RBC_STRING_DROP,3,0,0,0),word(XR_RBC_RET,0,0,0,0)};
    CHECK(xr_rbc_jump(&head_owner[5],5,7,12)&&xr_rbc_jump(&head_owner[6],6,10,12)&&xr_rbc_jump(&head_owner[9],9,4,12));
    functions[0].code=head_owner;functions[0].code_count=12;rejected(functions,1,locations);
    uint32_t unproved[]={0,word(XR_RBC_STRING_LITERAL,3,0,0,0),word(XR_RBC_STRING_DROP,3,0,0,0),
        word(XR_RBC_LOADI,0,0,0,0),word(XR_RBC_LOADI,0,0,0,0),0,word(XR_RBC_RET,0,0,0,0)};
    CHECK(xr_rbc_jump(&unproved[0],0,4,7)&&xr_rbc_jump(&unproved[5],5,1,7));
    functions[0].code=unproved;functions[0].code_count=7;rejected(functions,1,locations);
    uint32_t degrade[]={word(XR_RBC_LOADI,0,0,0,0),word(XR_RBC_LOADI,1,0,0,3),word(XR_RBC_LOADI,2,0,0,1),
        word(XR_RBC_LOADI,5,0,0,12),word(XR_RBC_LT,0,1,0,0),0,0,word(XR_RBC_STRING_LITERAL,3,0,0,0),
        word(XR_RBC_STRING_DROP,3,0,0,0),word(XR_RBC_CALL,4,0,0,1),word(XR_RBC_ADD,0,0,2,0),0,word(XR_RBC_RET,0,0,0,0)};
    CHECK(xr_rbc_jump(&degrade[5],5,7,13)&&xr_rbc_jump(&degrade[6],6,12,13)&&xr_rbc_jump(&degrade[11],11,4,13));
    XrRbcRegisterKind windows[]={XR_RBC_BITS,XR_RBC_BITS,XR_RBC_BITS,XR_RBC_STRING,XR_RBC_CALL_WINDOW,XR_RBC_CALL_WINDOW};
    functions[0]=(XrRbcFunction){.code=degrade,.code_count=13,.register_count=6,.register_kinds=windows,.register_kind_count=6};
    uint32_t identity=word(XR_RBC_RET,0,0,0,0);functions[1]=(XrRbcFunction){.code=&identity,.code_count=1,.register_count=1,.parameter_count=1};
    rejected(functions,2,locations);
}
int main(void) {
    XrtLocationOwner *locations = NULL;
    CHECK(xrt_locations_create(NULL,0,&locations)==XRT_LOCATION_OK);
    loop_entry_obligations(locations);
    forward_string_owners(locations);
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
    uint32_t transferred_borrow[] = {code[0],word(XR_RBC_MOV,3,0,0,0),word(XR_RBC_STRING_TAKE,1,0,0,0),
        word(XR_RBC_STRING_DROP,1,0,0,0),word(XR_RBC_CALL,2,0,0,1),word(XR_RBC_STRING_TAKE,1,2,0,0),
        word(XR_RBC_STRING_DROP,1,0,0,0),word(XR_RBC_RET0,0,0,0,0)};
    functions[0].code=transferred_borrow;functions[0].code_count=8;rejected(functions,2,locations);
    functions[0].code_count=7;
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
