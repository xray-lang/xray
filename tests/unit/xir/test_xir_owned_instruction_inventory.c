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
 *   Allocation faults, finite held-memory limits and corrupted extents preserve ownership.
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
    fixture->source.head.error_type=XR_XIR_NEVER;
}
static XrXirCompileContext inventory_owner(XrCompileResourceLimits caps) {
    XrCompileResources *resources=NULL;CHECK(xr_compile_resources_new(&caps,&resources)==XR_COMPILE_RESOURCE_OK);
    return (XrXirCompileContext){resources,{64,64,64,1024,UINT64_C(16777216)}};
}
static XrCompileResourceStats inventory_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static void inventory_equal(const XrXirCompileContext *context,const XirOwnedInputFunction *input,
    const XrXirInstruction *expected) {
    CHECK(xir_input_function_resources(input)==context->resources);
    CHECK(xir_input_function_shape(input)==XR_XIR_OK);
    XirInputInstructionSpan span={0};
    CHECK(xir_input_function_instruction_span(input,&span)==XR_XIR_OK);
    CHECK(span.rows==input->instructions&&span.count==INVENTORY_ROWS);
    for(uint32_t i=0;i<INVENTORY_ROWS;++i) {
        XrXirInstruction stack,copy;const XrXirInstruction *row=NULL;
        CHECK(xir_input_instruction_ref(input,i,&stack,&row)==XR_XIR_OK);
        CHECK(row==&input->instructions[i]);
        CHECK(xir_input_instruction(input,i,&copy)==XR_XIR_OK);
        for(uint32_t field=0;field<XIR_INPUT_WORDS;++field) {
            CHECK(xir_input_instruction_word(row,field)==xir_input_instruction_word(&expected[i],field));
            CHECK(xir_input_instruction_word(&copy,field)==xir_input_instruction_word(&expected[i],field));
            CHECK(xir_input_instruction_word(&span.rows[i],field)==xir_input_instruction_word(&expected[i],field));
        }
        XirInputInstructionShape shape={0};XrXirType type=XR_XIR_UNIT;
        CHECK(xir_input_instruction_shape(input,i,&shape)==XR_XIR_OK);
        CHECK(shape.op==expected[i].op&&shape.type==expected[i].type);
        CHECK(xir_input_function_value_type(input,i+2,&type)==XR_XIR_OK&&type==expected[i].type);
    }
    XrXirInstruction output={.op=XR_XIR_RETURN},saved=output;
    CHECK(xir_input_instruction(input,INVENTORY_ROWS,&output)==XR_XIR_BAD_STRUCTURE);
    CHECK(!memcmp(&output,&saved,sizeof(output)));
}
static void inventory_reads(XirOwnedInputFunction *input) {
    XirInputInstructionSpan output={NULL,UINT32_MAX};
    ++input->head.instruction_count;
    CHECK(xir_input_function_instruction_span(input,&output)==XR_XIR_BAD_STRUCTURE);
    CHECK(!output.rows&&output.count==UINT32_MAX);
    --input->head.instruction_count;
    CHECK(xir_input_function_instruction_span(input,&output)==XR_XIR_OK);
    CHECK(output.rows==input->instructions&&output.count==INVENTORY_ROWS);
    XrXirInstruction stack;const XrXirInstruction *row=input->instructions,*saved=row;
    ++input->head.instruction_count;
    CHECK(xir_input_instruction_ref(input,16,&stack,&row)==XR_XIR_BAD_STRUCTURE&&row==saved);
    --input->head.instruction_count;
    CHECK(xir_input_instruction_ref(input,16,&stack,&row)==XR_XIR_OK&&row==input->instructions+16);
    XrXirInstruction *storage=input->instructions;input->instructions=NULL;
    CHECK(xir_input_function_head(input,&(XirInputFunctionHead){0})==XR_XIR_BAD_STRUCTURE);
    input->instructions=storage;
    XrXirOp opcode=storage[3].op;storage[3].op=XR_XIR_INVALID;
    CHECK(xir_input_function_shape(input)==XR_XIR_BAD_STRUCTURE);storage[3].op=opcode;
}
static void inventory_faults(void) {
    InventoryFixture fixture;inventory_fixture(&fixture);
    XrXirCompileContext context=inventory_owner((XrCompileResourceLimits){XR_COMPILE_MEMORY_ORDINARY,0,false});
    uint64_t baseline=inventory_stats(&context).held_bytes;size_t physical=physical_live;
    allocation_calls=0;XirOwnedInputFunction *input=NULL;
    CHECK(xir_input_function_new(&context,&fixture.source,&input)==XR_XIR_OK);
    size_t sites=allocation_calls;xir_input_function_free(input);input=NULL;
    for(size_t i=0;i<sites;++i) {
        allocation_calls=0;fail_at=i;
        CHECK(xir_input_function_new(&context,&fixture.source,&input)==XR_XIR_OUT_OF_MEMORY&&!input);
        CHECK(inventory_stats(&context).held_bytes==baseline&&physical_live==physical);
        fail_at=SIZE_MAX;
        CHECK(xir_input_function_new(&context,&fixture.source,&input)==XR_XIR_OK&&input);
        xir_input_function_free(input);input=NULL;
    }
    xr_compile_resources_release(context.resources);CHECK(!physical_live);
    printf("Owned instruction inventory: %zu complete allocation faults and same-owner recovery passed\n",sites);
}
static void inventory_limits(void) {
    InventoryFixture fixture;inventory_fixture(&fixture);
    XrXirCompileContext context=inventory_owner((XrCompileResourceLimits){XR_COMPILE_MEMORY_ORDINARY,0,false});
    XirOwnedInputFunction *input=NULL;CHECK(xir_input_function_new(&context,&fixture.source,&input)==XR_XIR_OK);
    XrCompileResourceStats measured=inventory_stats(&context);xir_input_function_free(input);
    xr_compile_resources_release(context.resources);CHECK(!physical_live);
    for(unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits caps={XR_COMPILE_MEMORY_FINITE,measured.peak_bytes-minus,false};
        context=inventory_owner(caps);input=NULL;
        CHECK(xir_input_function_new(&context,&fixture.source,&input)==(minus?XR_XIR_MEMORY_LIMIT:XR_XIR_OK));
        CHECK(minus?!input:input!=NULL);xir_input_function_free(input);
        xr_compile_resources_release(context.resources);CHECK(!physical_live);
    }
    printf("Owned instruction inventory: held memory exact/minus-one passed\n");
}
int main(void) {
    InventoryFixture fixture;inventory_fixture(&fixture);
    XrXirInstruction expected[INVENTORY_ROWS];memcpy(expected,fixture.instructions,sizeof(expected));
    XrXirCompileContext context=inventory_owner((XrCompileResourceLimits){XR_COMPILE_MEMORY_ORDINARY,0,false});
    XirOwnedInputFunction *input=NULL,*copy=NULL;
    CHECK(xir_input_function_new(&context,&fixture.source,&input)==XR_XIR_OK);
    CHECK(xir_input_function_clone(&context,input,&copy)==XR_XIR_OK);
    CHECK(copy->instructions!=input->instructions&&input->instructions!=fixture.instructions);
    memset(&fixture,0xCE,sizeof(fixture));xir_input_function_free(input);
    inventory_equal(&context,copy,expected);inventory_reads(copy);
    xr_compile_resources_release(context.resources);
    CHECK(xir_input_function_resources(copy));xir_input_function_free(copy);CHECK(!physical_live);
    inventory_faults();inventory_limits();
    puts("Owned instruction inventory: direct ordinals, all fields, producer death and physical release passed");
    return 0;
}
