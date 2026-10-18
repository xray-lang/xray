/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_channel_blocking.inc.c - Owned blocking results and explicit value errors
 */
#include "xxir_channel_rendezvous_internal.h"
static bool source_channel_send_closed(SourceContext *ctx,AstNode *node,SourceValue completion,SourceValue *value) {
    XrXirType error_type=XR_XIR_UNIT;SourceValue closed={0},condition={0},error={0};
    if (!source_channel_send_type(ctx,&error_type) ||
        !source_recipe_record(ctx,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=XR_XIR_CHANNEL_SEND_WAIT_CLOSED},&closed) ||
        !source_recipe_record(ctx,(XrXirInstruction){.op=XR_XIR_EQ_INT,.type=XR_XIR_BOOL,.args={completion.id,closed.id}},&condition)) return false;
    uint32_t owner=ctx->function,branch=ctx->bodies[owner].count,error_block=ctx->bodies[owner].block_count;
    uint32_t frontier=ctx->bodies[owner].frontier;
    if (!source_recipe_record(ctx,(XrXirInstruction){.op=XR_XIR_BRANCH,.type=XR_XIR_UNIT,.args={condition.id,0}},NULL) ||
        !begin_block(ctx) || !source_recipe_group(ctx,(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=error_type,.immediate=3},NULL,0,&error)) return false;
    XrXirDeclarations declarations;XrXirModule module=source_module_view(ctx,&declarations);
    XrXirProofContext proof={&module,{XR_XIR_CONTEXT_FUNCTION,owner,0}};
    XrXirStatus status=xr_xir_compile_type_markers_prove(&ctx->compile,&proof,error_type,XR_XIR_CONSTRAINT_ERROR);
    if (status!=XR_XIR_OK) return source_fail(ctx,node,status,"closed Channel send requires its authenticated enum value error");
    if (ctx->bodies[owner].error_context) {
        SourceConversionRecipe conversion={0};
        if (!source_conversion_plan(ctx,node,error.type,(SourceExpectedType){true,XR_XIR_ERROR,false,false},&conversion) ||
            !source_conversion_emit(ctx,&conversion,&error) || !source_error_edge(ctx,error)) return false;
    } else if (!source_recipe_record(ctx,(XrXirInstruction){.op=XR_XIR_THROW,.type=XR_XIR_UNIT,.args={error.id,0}},NULL)) return false;
    ctx->returned=true;uint32_t success=ctx->bodies[owner].block_count;
    ctx->bodies[owner].frontier=frontier;if (!begin_block(ctx)) return false;
    ctx->bodies[owner].recipes[branch].instruction.targets[0]=error_block;
    ctx->bodies[owner].recipes[branch].instruction.targets[1]=success;
    *value=(SourceValue){UINT32_MAX,XR_XIR_UNIT};return true;
}
static bool source_channel_blocking_call(SourceContext *ctx,AstNode *node,SourceValue receiver,
    const XrNativeMemberDeclaration *member,SourceValue *value) {
    CallExprNode *call=&node->as.call_expr;
    bool send=member->operation==XR_NATIVE_OPERATION_CHANNEL_SEND,receive=member->operation==XR_NATIVE_OPERATION_CHANNEL_RECV;
    if (!member->lowered || !member->is_public || !member->is_method || member->is_static ||
        call->arg_count!=(receive ? 0 : 1) || call->type_arg_count || call->default_arg_count ||
        (!receive && (!call->arguments || (call->arg_accesses && call->arg_accesses[0]!=XR_CALL_ARG_PLAIN))))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"blocking Channel operation requires its exact ordinary value arguments");
    XrXirType element=xr_xir_channel_element(&ctx->types,receiver.type),result=send ? XR_XIR_I64 : element;
    if (receive && !source_channel_recv_type(ctx,element,&result)) return false;
    SourceValue operands[2]={receiver},completion={0};
    if (!receive) {
        if (!source_plan_expression(ctx,call->arguments[0],(SourceExpectedType){true,element,false,false},&operands[1])) return false;
        if (operands[1].type!=element) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Channel argument differs from its declared element type");
    }
    XrXirInstruction op={.op=send ? XR_XIR_CHANNEL_SEND : receive ? XR_XIR_CHANNEL_RECV : XR_XIR_CHANNEL_RECV_OR,.type=result};
    bool recorded;
    if (receive) { op.args[0]=receiver.id;recorded=source_recipe_record(ctx,op,&completion); }
    else recorded=source_recipe_group(ctx,op,operands,element==XR_XIR_UNIT ? 1u : 2u,&completion);
    if (!recorded || !source_channel_member_reference(ctx,node,member)) return false;
    if (send) return source_channel_send_closed(ctx,node,completion,value);
    *value=completion;return true;
}
