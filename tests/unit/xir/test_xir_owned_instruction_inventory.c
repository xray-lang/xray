/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_owned_instruction_inventory.c - Direct storage ownership and bounded reads
 *
 * KEY CONCEPT:
 *   Receiving isolates all initialized fields from a mutable producer.
 *   Allocation faults, exact limits and corrupted extents preserve ownership.
 */
#include "base/xmalloc.h"
#include "xir/xxir_input_owned.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);} } while(0)
static size_t allocation_calls,fail_at=SIZE_MAX,physical_live;
static void *inventory_malloc(size_t size) {
    if(allocation_calls++==fail_at)return NULL;
    void *memory=xr_malloc(size);if(memory)++physical_live;return memory;
}
static void inventory_free(void *memory) {
    if(memory){CHECK(physical_live);--physical_live;}xr_free(memory);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(size) inventory_malloc(size)
#define xr_free(memory) inventory_free(memory)
#include "base/xcompile_resources.c"
#include "xir/xxir_input_owned.c"

enum { INVENTORY_ROWS=17 };
typedef struct InventoryFixture {
    char name[10];
    XrXirType parameters[2];
    XrXirBlock block;
    uint32_t operands[3];
    XrXirInstruction instructions[INVENTORY_ROWS];
    XirInputFunctionReceive source;
} InventoryFixture;
static void inventory_fixture(InventoryFixture *fixture) {
    memset(fixture,0,sizeof(*fixture));memcpy(fixture->name,"inventory",9);
    fixture->parameters[0]=XR_XIR_I64;fixture->parameters[1]=XR_XIR_BOOL;
    fixture->block=(XrXirBlock){.first=0,.count=INVENTORY_ROWS};
    for(uint32_t i=0;i<3;++i)fixture->operands[i]=i+7;
    for(uint32_t i=0;i<INVENTORY_ROWS;++i)fixture->instructions[i]=(XrXirInstruction){
        .op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.args={i+3,i+5},.targets={i+9,i+11},
        .immediate=INT64_MIN+(int64_t)i,.type_arguments={i+13,i+17}};
    fixture->source=(XirInputFunctionReceive){
        {fixture->name,9,fixture->parameters,2,XR_XIR_I64,&fixture->block,1,INVENTORY_ROWS,fixture->operands,3},
        fixture->instructions};
}
static XrXirCompileContext inventory_owner(XrCompileResourceLimits caps) {
    XrCompileResources *resources=NULL;CHECK(xr_compile_resources_new(&caps,&resources)==XR_COMPILE_RESOURCE_OK);
    return (XrXirCompileContext){resources,{64,64,64,1024,UINT64_C(16777216)}};
}
static XrCompileResourceStats inventory_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static bool inventory_consume(const void *owner,uint64_t units) {
    return xir_compile_work((const XrXirCompileContext *)owner,units);
}
static void inventory_equal(const XrXirCompileContext *context,const XirOwnedInputFunction *input,
    const XrXirInstruction *expected) {
    XirInputWork work={context,inventory_consume};
    CHECK(xir_input_function_shape(&work,input)==XR_XIR_OK);
    XirInputInstructionSpan span={0};
    CHECK(xir_input_function_instruction_span(&work,input,&span)==XR_XIR_OK);
    CHECK(span.rows==input->instructions&&span.count==INVENTORY_ROWS);
    for(uint32_t i=0;i<INVENTORY_ROWS;++i) {
        XrXirInstruction stack,copy;const XrXirInstruction *row=NULL;
        CHECK(xir_input_instruction_ref(&work,input,i,&stack,&row)==XR_XIR_OK);
        CHECK(row==&input->instructions[i]);
        CHECK(xir_input_instruction(&work,input,i,&copy)==XR_XIR_OK);
        for(uint32_t field=0;field<XIR_INPUT_WORDS;++field) {
            CHECK(xir_input_instruction_word(row,field)==xir_input_instruction_word(&expected[i],field));
            CHECK(xir_input_instruction_word(&copy,field)==xir_input_instruction_word(&expected[i],field));
            CHECK(xir_input_instruction_word(&span.rows[i],field)==xir_input_instruction_word(&expected[i],field));
        }
        XirInputInstructionShape shape={0};XrXirType type=XR_XIR_UNIT;
        CHECK(xir_input_instruction_shape(&work,input,i,&shape)==XR_XIR_OK);
        CHECK(shape.op==expected[i].op&&shape.type==expected[i].type);
        CHECK(xir_input_function_value_type(&work,input,i+2,&type)==XR_XIR_OK&&type==expected[i].type);
    }
    XrXirInstruction output={.op=XR_XIR_RETURN},saved=output;
    CHECK(xir_input_instruction(&work,input,INVENTORY_ROWS,&output)==XR_XIR_BAD_STRUCTURE);
    CHECK(!memcmp(&output,&saved,sizeof(output)));
}
typedef struct InventoryDebit { uint64_t remaining,spent;XirOwnedInputFunction *poison; } InventoryDebit;
static bool inventory_debit(const void *owner,uint64_t units) {
    InventoryDebit *debit=(InventoryDebit *)owner;
    if(units>debit->remaining)return false;
    debit->remaining-=units;debit->spent+=units;
    if(debit->poison)++debit->poison->head.instruction_count;
    return true;
}
static void inventory_reads(const XrXirCompileContext *context,XirOwnedInputFunction *input) {
    XirInputWork work={context,inventory_consume};XirInputReadCosts costs={0};
    CHECK(xir_input_function_read_costs(&work,input,&costs)==XR_XIR_OK);
    CHECK(costs.instructions==INVENTORY_ROWS&&costs.row==4+sizeof(const XrXirInstruction *));
    uint64_t span_units=4+sizeof(XirInputInstructionSpan);
    for(unsigned minus=0;minus<2;++minus) {
        InventoryDebit budget={span_units-minus,0,NULL};XirInputWork span_work={&budget,inventory_debit};
        XirInputInstructionSpan output={NULL,UINT32_MAX};
        CHECK(xir_input_function_instruction_span(&span_work,input,&output)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        CHECK(minus?(!output.rows&&output.count==UINT32_MAX):
            (output.rows==input->instructions&&output.count==INVENTORY_ROWS));
        CHECK(budget.spent==(minus?0:span_units));
    }
    InventoryDebit poisoned={span_units,0,input};XirInputWork span_work={&poisoned,inventory_debit};
    XirInputInstructionSpan unchanged={NULL,UINT32_MAX};
    CHECK(xir_input_function_instruction_span(&span_work,input,&unchanged)==XR_XIR_BAD_STRUCTURE);
    CHECK(!unchanged.rows&&unchanged.count==UINT32_MAX);
    --input->head.instruction_count;
    for(unsigned minus=0;minus<2;++minus) {
        InventoryDebit debit={costs.row-minus,0,NULL};XirInputWork reading={&debit,inventory_debit};
        XrXirInstruction stack;const XrXirInstruction *sentinel=input->instructions,*row=sentinel;
        CHECK(xir_input_instruction_ref(&reading,input,16,&stack,&row)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        CHECK(minus?row==sentinel:row==&input->instructions[16]);
        CHECK(debit.spent==(minus?0:costs.row));
    }
    InventoryDebit debit={costs.row,0,input};XirInputWork reading={&debit,inventory_debit};
    XrXirInstruction stack;const XrXirInstruction *row=input->instructions,*saved=row;
    CHECK(xir_input_instruction_ref(&reading,input,16,&stack,&row)==XR_XIR_BAD_STRUCTURE&&row==saved);
    --input->head.instruction_count;
    XrXirInstruction *storage=input->instructions;input->instructions=NULL;
    CHECK(xir_input_function_head(&work,input,&(XirInputFunctionHead){0})==XR_XIR_BAD_STRUCTURE);
    input->instructions=storage;
    XrXirOp opcode=storage[3].op;storage[3].op=XR_XIR_INVALID;
    CHECK(xir_input_function_shape(&work,input)==XR_XIR_BAD_STRUCTURE);storage[3].op=opcode;
}
static void inventory_faults(void) {
    InventoryFixture fixture;inventory_fixture(&fixture);
    XrXirCompileContext context=inventory_owner((XrCompileResourceLimits){67108864,8388608,4000000});
    uint64_t baseline=inventory_stats(&context).live_bytes;size_t physical=physical_live;
    allocation_calls=0;XirOwnedInputFunction *input=NULL;
    CHECK(xir_input_function_new(&context,&fixture.source,&input)==XR_XIR_OK);
    size_t sites=allocation_calls;xir_input_function_free(input);input=NULL;
    for(size_t i=0;i<sites;++i) {
        allocation_calls=0;fail_at=i;
        CHECK(xir_input_function_new(&context,&fixture.source,&input)==XR_XIR_OUT_OF_MEMORY&&!input);
        CHECK(inventory_stats(&context).live_bytes==baseline&&physical_live==physical);
        fail_at=SIZE_MAX;
        CHECK(xir_input_function_new(&context,&fixture.source,&input)==XR_XIR_OK&&input);
        xir_input_function_free(input);input=NULL;
    }
    xr_compile_resources_release(context.resources);CHECK(!physical_live);
    printf("Owned instruction inventory: %zu complete allocation faults and same-owner recovery passed\n",sites);
}
static void inventory_limits(void) {
    InventoryFixture fixture;inventory_fixture(&fixture);
    XrXirCompileContext context=inventory_owner((XrCompileResourceLimits){67108864,8388608,4000000});
    XirOwnedInputFunction *input=NULL;CHECK(xir_input_function_new(&context,&fixture.source,&input)==XR_XIR_OK);
    XrCompileResourceStats measured=inventory_stats(&context);xir_input_function_free(input);
    xr_compile_resources_release(context.resources);CHECK(!physical_live);
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits caps={67108864,8388608,4000000};
        if(axis==0)caps.allocated_bytes=measured.allocated_bytes-minus;
        if(axis==1)caps.live_bytes=measured.peak_bytes-minus;
        if(axis==2)caps.work=measured.work-minus;
        context=inventory_owner(caps);input=NULL;
        CHECK(xir_input_function_new(&context,&fixture.source,&input)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        CHECK(minus?!input:input!=NULL);xir_input_function_free(input);
        xr_compile_resources_release(context.resources);CHECK(!physical_live);
    }
    printf("Owned instruction inventory: three resource axes exact/minus-one passed\n");
}
int main(void) {
    InventoryFixture fixture;inventory_fixture(&fixture);
    XrXirInstruction expected[INVENTORY_ROWS];memcpy(expected,fixture.instructions,sizeof(expected));
    XrXirCompileContext context=inventory_owner((XrCompileResourceLimits){67108864,8388608,4000000});
    XirOwnedInputFunction *input=NULL,*copy=NULL;
    CHECK(xir_input_function_new(&context,&fixture.source,&input)==XR_XIR_OK);
    CHECK(xir_input_function_clone(&context,input,&copy)==XR_XIR_OK);
    CHECK(copy->instructions!=input->instructions&&input->instructions!=fixture.instructions);
    memset(&fixture,0xCE,sizeof(fixture));xir_input_function_free(input);
    inventory_equal(&context,copy,expected);inventory_reads(&context,copy);
    xr_compile_resources_release(context.resources);
    CHECK(xir_input_function_resources(copy));xir_input_function_free(copy);CHECK(!physical_live);
    inventory_faults();inventory_limits();
    puts("Owned instruction inventory: direct ordinals, all fields, producer death and physical release passed");
    return 0;
}
