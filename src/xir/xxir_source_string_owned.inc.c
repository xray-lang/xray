/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_string_owned.inc.c - Ordinary owned string method recipes
 *
 * KEY CONCEPT: Repetition doubles complete UTF-8 strings in at most 64 rounds.
 */
#include "xxir_source_string_value.inc.c"
static bool source_string_repeat(SourceContext *ctx,AstNode *node,
    SourceValue receiver,SourceValue count,SourceValue *value) {
    XrXirType text_cell,number_cell;
    SourceValue empty,output,chunk,remaining,zero,one,bound,length,nonempty;
    if (!source_cell_type(ctx,XR_XIR_STRING,&text_cell) || !source_cell_type(ctx,XR_XIR_I64,&number_cell) ||
        !source_string_literal(ctx,node,"",0,&empty) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_NEW,text_cell,{empty.id},{0},0,{0}},&output) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_NEW,text_cell,{receiver.id},{0},0,{0}},&chunk) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_NEW,number_cell,{count.id},{0},0,{0}},&remaining) ||
        !source_bits_constant(ctx,XR_XIR_I64,0,&zero) || !source_bits_constant(ctx,XR_XIR_I64,1,&one) ||
        !source_bits_constant(ctx,XR_XIR_I64,64,&bound) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_STRING_LEN,XR_XIR_I64,{receiver.id},{0},0,{0}},&length) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_GT_INT,XR_XIR_BOOL,{length.id,zero.id},{0},0,{0}},&nonempty)) return false;
    /* An empty receiver must finish immediately even for the largest count. */
    SourceGuard present;
    if (!source_guard_open(ctx,nonempty,false,&present)) return false;
    SourceCounter counter;
    SourceValue number,positive;
    SourceGuard finished;
    if (!source_counter_open(ctx,zero,bound,false,&counter) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_READ,XR_XIR_I64,{remaining.id},{0},0,{0}},&number) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_GT_INT,XR_XIR_BOOL,{number.id,zero.id},{0},0,{0}},&positive) ||
        !source_guard_open(ctx,positive,true,&finished) || !source_counter_break(ctx) ||
        !source_guard_close(ctx,&finished)) return false;
    SourceValue bit,selected,piece,current,joined;
    SourceGuard append;
    if (!source_bits_binary(ctx,XR_XIR_AND_INT,number,one,&bit) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_NE_INT,XR_XIR_BOOL,{bit.id,zero.id},{0},0,{0}},&selected) ||
        !source_guard_open(ctx,selected,false,&append) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_READ,XR_XIR_STRING,{output.id},{0},0,{0}},&current) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_READ,XR_XIR_STRING,{chunk.id},{0},0,{0}},&piece) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONCAT_STRING,XR_XIR_STRING,{current.id,piece.id},{0},0,{0}},&joined) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_WRITE,XR_XIR_UNIT,{output.id,joined.id},{0},0,{0}},NULL) ||
        !source_guard_close(ctx,&append) || !source_bits_binary(ctx,XR_XIR_SHR_INT,number,one,&number) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_WRITE,XR_XIR_UNIT,{remaining.id,number.id},{0},0,{0}},NULL) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_GT_INT,XR_XIR_BOOL,{number.id,zero.id},{0},0,{0}},&positive)) return false;
    /* Avoid an unused doubling after the highest set bit. It can exceed the
     * byte budget even when the requested result itself fits exactly. */
    SourceGuard double_chunk;
    if (!source_guard_open(ctx,positive,false,&double_chunk) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_READ,XR_XIR_STRING,{chunk.id},{0},0,{0}},&piece) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONCAT_STRING,XR_XIR_STRING,{piece.id,piece.id},{0},0,{0}},&joined) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_WRITE,XR_XIR_UNIT,{chunk.id,joined.id},{0},0,{0}},NULL) ||
        !source_guard_close(ctx,&double_chunk) || !source_counter_close(ctx,&counter) ||
        !source_guard_close(ctx,&present)) return false;
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_READ,XR_XIR_STRING,{output.id},{0},0,{0}},value);
}
static bool source_string_owned_call(SourceContext *ctx,AstNode *node,SourceValue receiver,
    const XrNativeMemberDeclaration *member,SourceValue *value) {
    CallExprNode *call=&node->as.call_expr;
    bool repeat=member->operation==XR_NATIVE_OPERATION_STRING_REPEAT;
    if (receiver.type!=XR_XIR_STRING || !member->is_public || !member->is_method || member->is_static ||
        member->receiver!=XR_NATIVE_RECEIVER_READ || member->result!=XR_NATIVE_TERM_STRING ||
        member->parameter_count!=(repeat ? 1u : 0u) ||
        (repeat && member->parameters[0].type!=XR_NATIVE_TERM_I64) ||
        call->type_arg_count || call->default_arg_count || call->arg_count!=(repeat ? 1 : 0))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"owned string operation requires its exact ordinary declaration");
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_STRING);
    if (!source_primitive_member_reference(ctx,call->callee,native,member)) return false;
    if (!repeat) return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_COPY,XR_XIR_STRING,{receiver.id},{0},0,{0}},value);
    if (!call->arguments || (call->arg_accesses && call->arg_accesses[0]!=XR_CALL_ARG_PLAIN))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"string repeat requires a READ i64 count");
    SourceValue count;
    if (!source_plan_expression(ctx,call->arguments[0],(SourceExpectedType){true,XR_XIR_I64,false,false},&count)) return false;
    if (count.type!=XR_XIR_I64) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"string repeat count must be i64");
    return source_string_repeat(ctx,node,receiver,count,value);
}
