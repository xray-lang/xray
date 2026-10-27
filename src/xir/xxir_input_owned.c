/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_input_owned.c - Exclusive initialized instruction storage and direct ordinal reads
 *
 * KEY CONCEPT:
 *   Receiving copies actual initialized records and owns each metadata span.
 *   Every ordinal names its actual instruction; compression belongs to evidence.
 */
#include "xxir_input_owned.h"
#include "xxir_compile_memory.h"

/* Allocation extents remain separate from descriptive metadata so destruction
 * never follows a corrupted count or a substituted public metadata pointer. */
typedef struct XirInputFunctionExtent {
    const char *name;
    const XrXirType *parameters;
    const XrXirBlock *blocks;
    const uint32_t *operands;
    XrXirInstruction *instructions;
    uint32_t name_length,parameters_count,blocks_count,instructions_count,operands_count;
} XirInputFunctionExtent;
struct XirOwnedInputFunction {
    XrCompileResources *resources;
    XirInputFunctionHead head;
    XrXirInstruction *instructions;
    XirInputFunctionExtent extent;
};
static bool input_function_storage_extent(const XirOwnedInputFunction *input) {
    if(!input||!input->resources)return false;
    const XirInputFunctionHead *head=&input->head;const XirInputFunctionExtent *extent=&input->extent;
    return head->name==extent->name&&head->name_length==extent->name_length&&
        head->parameters==extent->parameters&&head->parameter_count==extent->parameters_count&&
        head->blocks==extent->blocks&&head->block_count==extent->blocks_count&&
        head->instruction_count==extent->instructions_count&&head->operands==extent->operands&&
        head->operand_count==extent->operands_count&&input->instructions==extent->instructions&&
        (!head->instruction_count||input->instructions);
}
XR_FUNC XrCompileResources *xir_input_function_resources(const XirOwnedInputFunction *input) {
    return input_function_storage_extent(input)?input->resources:NULL;
}
XR_FUNC XrXirStatus xir_input_function_instruction_span(
    const XirOwnedInputFunction *input,XirInputInstructionSpan *output) {
    if(!input_function_storage_extent(input)||!output)return XR_XIR_BAD_STRUCTURE;
    if(!input_function_storage_extent(input))return XR_XIR_BAD_STRUCTURE;
    *output=(XirInputInstructionSpan){input->instructions,input->head.instruction_count};
    return XR_XIR_OK;
}
static XrXirStatus input_head_shape(const XirInputFunctionHead *head) {
    if(!head)return XR_XIR_BAD_STRUCTURE;
    if(head->error_type==XR_XIR_UNIT)return XR_XIR_BAD_TYPE;
    if((head->name_length&&!head->name)||(head->parameter_count&&!head->parameters)||
        (head->block_count&&!head->blocks)||(head->operand_count&&!head->operands)||
        head->parameter_count>UINT32_MAX-head->instruction_count ||
        head->block_count>head->instruction_count)return XR_XIR_BAD_STRUCTURE;
    uint32_t first=0;
    for(uint32_t b=0;b<head->block_count;++b) {
        const XrXirBlock *block=&head->blocks[b];
        if(block->first!=first || !block->count || block->count>head->instruction_count-first)
            return XR_XIR_BAD_STRUCTURE;
        first+=block->count;
    }
    return first==head->instruction_count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
XR_FUNC XrXirStatus xir_input_function_head_shape(const XirInputFunctionHead *head) {
    return input_head_shape(head);
}

static XrXirStatus input_source_shape(const XirInputFunctionReceive *source) {
    if(!source)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=input_head_shape(&source->head);if(status!=XR_XIR_OK)return status;
    if(source->head.instruction_count&&!source->instructions)return XR_XIR_BAD_STRUCTURE;
    for(uint32_t i=0;i<source->head.instruction_count;++i) {
        XrXirOp opcode=source->instructions[i].op;
        if(opcode<=XR_XIR_INVALID||opcode>=XR_XIR_OP_COUNT)return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
XR_FUNC void xir_input_function_free(XirOwnedInputFunction *input) {
    if(!input)return;
    xr_compile_resources_free((void *)input->extent.name);
    xr_compile_resources_free((void *)input->extent.parameters);
    xr_compile_resources_free((void *)input->extent.blocks);
    xr_compile_resources_free((void *)input->extent.operands);
    xr_compile_resources_free(input->extent.instructions);xr_compile_resources_free(input);
}
static XrXirStatus input_records_equal(
    const XirInputFunctionReceive *source,const XirOwnedInputFunction *input) {
    if(!input_function_storage_extent(input)||source->head.instruction_count!=input->head.instruction_count||
        (source->head.instruction_count&&!source->instructions))return XR_XIR_BAD_STRUCTURE;
    for(uint32_t i=0;i<input->head.instruction_count;++i)for(uint32_t word=0;word<XIR_INPUT_WORDS;++word) {
        if(xir_input_instruction_word(&source->instructions[i],word)!=
            xir_input_instruction_word(&input->instructions[i],word))return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XrXirStatus input_metadata_equal(
    const XirInputFunctionHead *a,const XirInputFunctionHead *b) {
    if(a->name_length!=b->name_length || a->parameter_count!=b->parameter_count || a->result!=b->result ||
        a->block_count!=b->block_count || a->instruction_count!=b->instruction_count || a->operand_count!=b->operand_count ||
        a->error_type!=b->error_type)
        return XR_XIR_BAD_STRUCTURE;
    for(uint32_t i=0;i<a->name_length;++i) {
        if(a->name[i]!=b->name[i])return XR_XIR_BAD_STRUCTURE;
    }
    for(uint32_t i=0;i<a->parameter_count;++i) {
        if(a->parameters[i]!=b->parameters[i])return XR_XIR_BAD_STRUCTURE;
    }
    for(uint32_t i=0;i<a->block_count;++i) {
        if(a->blocks[i].first!=b->blocks[i].first || a->blocks[i].count!=b->blocks[i].count ||
            a->blocks[i].panic!=b->blocks[i].panic || a->blocks[i].frontier!=b->blocks[i].frontier)
            return XR_XIR_BAD_STRUCTURE;
    }
    for(uint32_t i=0;i<a->operand_count;++i) {
        if(a->operands[i]!=b->operands[i])return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XirOwnedInputFunction *input_copy_head(const XrXirCompileContext *context,
    const XirInputFunctionHead *source,XrXirStatus *status) {

    XirInputFunctionHead current=*source;
    XirOwnedInputFunction *input=xir_compile_calloc(context,1,sizeof(*input),status);
    if(!input)return NULL;

    if(*status==XR_XIR_OK) {
        input->resources=context->resources;
        input->head=current;
        input->extent=(XirInputFunctionExtent){.name_length=current.name_length,
            .parameters_count=current.parameter_count,.blocks_count=current.block_count,
            .instructions_count=current.instruction_count,.operands_count=current.operand_count};
        input->head.name=NULL;input->head.parameters=NULL;input->head.blocks=NULL;input->head.operands=NULL;
        input->head.name=xir_compile_copy(context,current.name,current.name_length,status);
        input->extent.name=input->head.name;
        input->head.parameters=xir_compile_copy(context,current.parameters,
            (size_t)current.parameter_count*sizeof(*current.parameters),status);
        input->extent.parameters=input->head.parameters;
        input->head.blocks=xir_compile_copy(context,current.blocks,
            (size_t)current.block_count*sizeof(*current.blocks),status);
        input->extent.blocks=input->head.blocks;
        input->head.operands=xir_compile_copy(context,current.operands,
            (size_t)current.operand_count*sizeof(*current.operands),status);
        input->extent.operands=input->head.operands;
    }
    if(*status!=XR_XIR_OK){xir_input_function_free(input);return NULL;}
    return input;
}

XR_FUNC XrXirStatus xir_input_function_new(const XrXirCompileContext *context,
    const XirInputFunctionReceive *source,XirOwnedInputFunction **output) {
    if(!xir_compile_context_valid(context)||!source||!output||*output)return XR_XIR_BAD_STRUCTURE;
    if(source->head.parameter_count>context->limits.parameters||source->head.block_count>context->limits.blocks||
        source->head.instruction_count>context->limits.instructions||
        (uint64_t)source->head.parameter_count>SIZE_MAX/sizeof(*source->head.parameters)||
        (uint64_t)source->head.block_count>SIZE_MAX/sizeof(*source->head.blocks)||
        (uint64_t)source->head.operand_count>SIZE_MAX/sizeof(*source->head.operands)||
        (uint64_t)source->head.instruction_count>SIZE_MAX/sizeof(*source->instructions))return XR_XIR_BUDGET;
    XrXirStatus status=input_source_shape(source);if(status!=XR_XIR_OK)return status;
    XirOwnedInputFunction *input=input_copy_head(context,&source->head,&status);
    if(!input)return status;

    if(status==XR_XIR_OK) {
        input->instructions=xir_compile_copy(context,source->instructions,
            (size_t)input->extent.instructions_count*sizeof(*input->instructions),&status);
        input->extent.instructions=input->instructions;
    }
    if(status==XR_XIR_OK)status=input_source_shape(source);
    if(status==XR_XIR_OK)status=xir_input_function_shape(input);
    if(status==XR_XIR_OK)status=input_metadata_equal(&source->head,&input->head);
    if(status==XR_XIR_OK)status=input_records_equal(source,input);

    if(status!=XR_XIR_OK){xir_input_function_free(input);return status;}
    *output=input;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_input_function_clone(const XrXirCompileContext *context,
    const XirOwnedInputFunction *source,XirOwnedInputFunction **output) {
    if(!xir_compile_context_valid(context)||!input_function_storage_extent(source)||!output||*output)
        return XR_XIR_BAD_STRUCTURE;
    XirInputFunctionReceive receiving={source->head,source->instructions};
    return xir_input_function_new(context,&receiving,output);
}
XR_FUNC XrXirStatus xir_input_function_shape(const XirOwnedInputFunction *input) {
    if(!input_function_storage_extent(input))return XR_XIR_BAD_STRUCTURE;
    XirInputFunctionReceive receiving={input->head,input->instructions};
    return input_source_shape(&receiving);
}
XR_FUNC XrXirStatus xir_input_function_head(
    const XirOwnedInputFunction *input,XirInputFunctionHead *output) {
    if(!input_function_storage_extent(input)||!output)return XR_XIR_BAD_STRUCTURE;
    if(!input_function_storage_extent(input))return XR_XIR_BAD_STRUCTURE;
    *output=input->head;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_input_function_head_ref(
    const XirOwnedInputFunction *input,const XirInputFunctionHead **output) {
    if(!input_function_storage_extent(input)||!output)return XR_XIR_BAD_STRUCTURE;
    if(!input_function_storage_extent(input))return XR_XIR_BAD_STRUCTURE;
    *output=&input->head;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_input_function_visit(
    const XirOwnedInputFunction *input,const XirInputFunctionVisitor *visitor) {
    if(!input_function_storage_extent(input)||!visitor||!visitor->owner||!visitor->head||!visitor->row)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=xir_input_function_shape(input);if(status!=XR_XIR_OK)return status;
    status=visitor->head(visitor->owner,&input->head,input->head.instruction_count);
    for(uint32_t i=0;status==XR_XIR_OK&&i<input->head.instruction_count;++i) {
        XrXirInstruction stack;const XrXirInstruction *row=NULL;
        status=xir_input_instruction_ref(input,i,&stack,&row);
        if(status==XR_XIR_OK)status=visitor->row(visitor->owner,i,row);
    }
    return status;
}
XR_FUNC XrXirStatus xir_input_function_receive_read(
    const XirOwnedInputFunction *input,XirInputFunctionReceive *output) {
    if(!input_function_storage_extent(input)||!output)return XR_XIR_BAD_STRUCTURE;
    if(!input_function_storage_extent(input))return XR_XIR_BAD_STRUCTURE;
    *output=(XirInputFunctionReceive){input->head,input->instructions};return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_input_instruction_ref(const XirOwnedInputFunction *input,
    uint32_t instruction,XrXirInstruction *stack,const XrXirInstruction **output) {
    if(!input_function_storage_extent(input)||!stack||!output||instruction>=input->head.instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    if(!input_function_storage_extent(input)||instruction>=input->head.instruction_count)return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *row=&input->instructions[instruction];
    if(row->op<=XR_XIR_INVALID||row->op>=XR_XIR_OP_COUNT)return XR_XIR_BAD_STRUCTURE;
    *output=row;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_input_instruction_shape(
    const XirOwnedInputFunction *input,uint32_t instruction,XirInputInstructionShape *output) {
    if(!output)return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction stack;const XrXirInstruction *row=NULL;
    XrXirStatus status=xir_input_instruction_ref(input,instruction,&stack,&row);
    if(status!=XR_XIR_OK)return status;
    *output=(XirInputInstructionShape){row->op,row->type};return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_input_function_value_type(
    const XirOwnedInputFunction *input,uint32_t value,XrXirType *output) {
    if(!input_function_storage_extent(input)||!output)return XR_XIR_BAD_STRUCTURE;
    if(!input_function_storage_extent(input)||value>=(uint64_t)input->head.parameter_count+input->head.instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    XrXirType type;
    if(value<input->head.parameter_count)type=input->head.parameters[value];
    else {
        XrXirInstruction stack;const XrXirInstruction *row=NULL;
        XrXirStatus status=xir_input_instruction_ref(input,value-input->head.parameter_count,&stack,&row);
        if(status!=XR_XIR_OK)return status;
        type=row->type;
    }
    *output=type;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_input_instruction(const XirOwnedInputFunction *input,
    uint32_t instruction,XrXirInstruction *output) {
    if(!output)return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction stack;const XrXirInstruction *row=NULL;
    XrXirStatus status=xir_input_instruction_ref(input,instruction,&stack,&row);
    if(status!=XR_XIR_OK)return status;
    *output=*row;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_input_function_raw_match(const XrXirCompileContext *context,
    const XrXirFunction *source,const XirOwnedInputFunction *input) {
    if(!xir_compile_context_valid(context)||!source||!input||
        (source->instruction_count&&!source->instructions))return XR_XIR_BAD_STRUCTURE;

    XirInputFunctionHead head={source->name,source->name_length,source->parameters,source->parameter_count,
        source->result,source->blocks,source->block_count,source->instruction_count,source->operands,source->operand_count,source->error_type};
    XrXirStatus status=input_head_shape(&head);
    if(status==XR_XIR_OK)status=xir_input_function_shape(input);
    if(status==XR_XIR_OK)status=input_metadata_equal(&head,&input->head);
    if(status!=XR_XIR_OK)return status;
    for(uint32_t i=0;i<head.instruction_count;++i) {
        XrXirInstruction stack;const XrXirInstruction *receiving;
        status=xir_input_instruction_ref(input,i,&stack,&receiving);if(status!=XR_XIR_OK)return status;
        const XrXirInstruction *actual=&source->instructions[i];
        for(uint32_t word=0;word<XIR_INPUT_WORDS;++word) {
            if(xir_input_instruction_word(actual,word)!=xir_input_instruction_word(receiving,word))
                return XR_XIR_BAD_STRUCTURE;
        }
    }
    return XR_XIR_OK;
}
