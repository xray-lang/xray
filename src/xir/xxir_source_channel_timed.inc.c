/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_channel_timed.inc.c - Once-evaluated timed Channel requests
 */
static bool source_channel_timed_call(SourceContext *ctx,AstNode *node,SourceValue receiver,
    const XrNativeMemberDeclaration *member,SourceValue *value) {
    CallExprNode *call=&node->as.call_expr;
    bool send=member->operation==XR_NATIVE_OPERATION_CHANNEL_SEND_TIMEOUT;
    uint32_t count=send ? 2u : 1u;
    if (!member->lowered || !member->is_public || !member->is_method || member->is_static ||
        call->arg_count!=(int)count || call->type_arg_count || call->default_arg_count || !call->arguments)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"timed Channel operation requires its exact ordinary value arguments");
    for (uint32_t a=0;a<count;++a) if (call->arg_accesses && call->arg_accesses[a]!=XR_CALL_ARG_PLAIN)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"timed Channel arguments require ordinary value access");
    XrXirType element=xr_xir_channel_element(&ctx->types,receiver.type),result=XR_XIR_UNIT;
    if (send ? !source_channel_send_type(ctx,&result) : !source_channel_recv_type(ctx,element,&result)) return false;
    SourceValue operands[3]={receiver},message={0};uint32_t physical=1;
    if (send) {
        if (!source_plan_expression(ctx,call->arguments[0],(SourceExpectedType){true,element,false,false},&message)) return false;
        if (message.type!=element) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Channel message differs from its declared element type");
        if (element!=XR_XIR_UNIT) operands[physical++]=message;
    }
    if (!source_plan_expression(ctx,call->arguments[count-1],(SourceExpectedType){true,XR_XIR_I64,false,false},&operands[physical])) return false;
    if (operands[physical].type!=XR_XIR_I64) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Channel timeout requires i64 milliseconds");
    return source_recipe_group(ctx,(XrXirInstruction){.op=send ? XR_XIR_CHANNEL_SEND_TIMEOUT : XR_XIR_CHANNEL_RECV_TIMEOUT,.type=result},
        operands,physical+1,value) && source_channel_member_reference(ctx,node,member);
}
