/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_channel_blocking.inc.c - Owned blocking Channel results
 */
#include "xxir_channel_rendezvous_internal.h"
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
    if (send) { *value=(SourceValue){.id=UINT32_MAX,.type=XR_XIR_UNIT};return true; }
    *value=completion;return true;
}
