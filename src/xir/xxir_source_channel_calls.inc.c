/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_channel_calls.inc.c - Governed Channel construction and identity operations
 */
static bool source_channel_construct(SourceContext *ctx,AstNode *node,XrXirType element,
    AstNode *capacity,SourceValue *value) {
    if (source_native_type_shadowed(ctx,"Channel"))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"constructor does not bind the governed Channel declaration");
    const XrNativeTypeDeclaration *native=source_native_find(ctx,"Channel");
    if (!native || native->id!=XR_NATIVE_DECLARATION_CHANNEL)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Channel declaration authority is not admitted");
    SourceValue count={0}; XrXirType type=XR_XIR_UNIT;
    if (!source_channel_type(ctx,element,&type)) return false;
    if (capacity) {
        if (!source_plan_expression(ctx,capacity,(SourceExpectedType){true,XR_XIR_I64,false,false},&count)) return false;
    } else if (!source_recipe_record(ctx,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},&count)) return false;
    if (count.type!=XR_XIR_I64) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Channel capacity requires i64");
    return source_recipe_record(ctx,(XrXirInstruction){.op=XR_XIR_CHANNEL_NEW,.type=type,.args={count.id,0}},value) &&
        source_query_target_token_reference(ctx,node,ctx->channel_declaration,XR_XIR_SOURCE_CALL);
}
static bool source_channel_constructor_element(SourceContext *ctx,XrTypeRef *ref,XrXirType *element) {
    if (ref && ref->kind==XR_TREF_NULL) { *element=XR_XIR_UNIT; return true; }
    return ref && source_type(ctx,ref,element);
}
static bool source_channel_context_construct(SourceContext *ctx,AstNode *node,
    SourceExpectedType expected,SourceValue *value) {
    if (!expected.present || !xr_xir_type_is_channel(&ctx->types,expected.type))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Channel construction requires a contextual element type");
    return source_channel_construct(ctx,node,xr_xir_channel_element(&ctx->types,expected.type),node->as.channel_new.buffer_size,value);
}
static bool source_channel_explicit_construct(SourceContext *ctx,AstNode *node,SourceValue *value) {
    NewExprNode *call=&node->as.new_expr;
    if (call->is_type_namespace || call->module_name || call->type_arg_count!=1 || !call->type_args ||
        call->arg_count<0 || call->arg_count>1 || (call->arg_count && !call->arguments) ||
        (call->arg_count && call->arg_accesses && call->arg_accesses[0]!=XR_CALL_ARG_PLAIN))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Channel construction requires one element type and an optional ordinary capacity");
    XrXirType element=XR_XIR_UNIT;
    return source_channel_constructor_element(ctx,call->type_args[0],&element) &&
        source_channel_construct(ctx,node,element,call->arg_count ? call->arguments[0] : NULL,value);
}
static bool source_channel_inferred_construct(SourceContext *ctx,AstNode *node,
    SourceExpectedType expected,SourceValue *value) {
    CallExprNode *call=&node->as.call_expr;
    if (call->type_arg_count<0 || call->type_arg_count>1 || call->default_arg_count || call->arg_count<0 || call->arg_count>1 ||
        (call->arg_count && !call->arguments) || (call->arg_count && call->arg_accesses && call->arg_accesses[0]!=XR_CALL_ARG_PLAIN))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Channel construction requires an optional ordinary capacity and a known element type");
    XrXirType element=XR_XIR_UNIT;
    if (call->type_arg_count) {
        if (!call->type_args || !source_channel_constructor_element(ctx,call->type_args[0],&element)) return false;
    } else {
        if (!expected.present || !xr_xir_type_is_channel(&ctx->types,expected.type))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Channel construction requires an explicit or contextual element type");
        element=xr_xir_channel_element(&ctx->types,expected.type);
    }
    return source_channel_construct(ctx,node,element,call->arg_count ? call->arguments[0] : NULL,value);
}
static bool source_channel_recv_type(SourceContext *ctx,XrXirType element,XrXirType *type) {
    SourceName *symbol=source_prelude_name(ctx,"Recv");
    if (!symbol || symbol->kind!=SOURCE_NOMINAL ||
        ctx->nominals.declarations[symbol->index].native.native_id!=XR_NATIVE_DECLARATION_RECV)
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"Channel result requires the governed Recv declaration");
    XrXirType *argument=source_alloc(ctx,1,sizeof(*argument));if (!argument) return false;*argument=element;
    return source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,
        .nominal={symbol->index,argument,1,NULL,0}},type);
}
static bool source_channel_send_type(SourceContext *ctx,XrXirType *type) {
    SourceName *symbol=source_prelude_name(ctx,"SendResult");
    if (!symbol || symbol->kind!=SOURCE_NOMINAL ||
        ctx->nominals.declarations[symbol->index].native.native_id!=XR_NATIVE_DECLARATION_SEND_RESULT)
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"Channel result requires the governed SendResult declaration");
    *type=symbol->type;return true;
}
static XrXirSourceType source_channel_schema_type(SourceContext *ctx,XrNativeTypeTerm term) {
    XrXirType type=XR_XIR_UNIT;
    if (term==XR_NATIVE_TERM_ELEMENT)
        return (XrXirSourceType){(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,ctx->channel_declaration,true};
    if (term==XR_NATIVE_TERM_RECV_ELEMENT) {
        if (!source_channel_recv_type(ctx,(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,&type)) return (XrXirSourceType){0};
        return (XrXirSourceType){type,ctx->channel_declaration,true};
    }
    if (term==XR_NATIVE_TERM_SEND_RESULT) {
        if (!source_channel_send_type(ctx,&type)) return (XrXirSourceType){0};
        return (XrXirSourceType){type,0,true};
    }
    type=term==XR_NATIVE_TERM_I64 ? XR_XIR_I64 : term==XR_NATIVE_TERM_BOOL ? XR_XIR_BOOL : XR_XIR_UNIT;
    return (XrXirSourceType){type,0,term==XR_NATIVE_TERM_I64 || term==XR_NATIVE_TERM_BOOL || term==XR_NATIVE_TERM_UNIT};
}
static bool source_channel_member_reference(SourceContext *ctx,AstNode *node,
    const XrNativeMemberDeclaration *member) {
    if (!member->id || member->id>10 || member->parameter_count>2 || (member->parameter_count && !member->parameters))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Channel identity member shape is invalid");
    uint32_t index=member->id-1;
    if (!ctx->channel_members[index]) {
        SourceName symbol={0};symbol.name=source_owned_text(ctx,member->name);
        if (!symbol.name || !source_query_declare(ctx,&symbol,XR_XIR_SOURCE_MEMBER,ctx->channel_declaration,
            (XrXirSourceRange){ctx->channel_module,(int)member->line,(int)member->column,
                (int)member->line,(int)member->column+(int)source_text_size(ctx,member->name)})) return false;
        ctx->channel_members[index]=symbol.declaration;
        XrXirSourceType *parameters=member->parameter_count ? source_alloc(ctx,member->parameter_count,sizeof(*parameters)) : NULL;
        if (member->parameter_count && !parameters) return false;
        for (uint32_t p=0;p<member->parameter_count;++p) {
            parameters[p]=source_channel_schema_type(ctx,member->parameters[p].type);
            if (!parameters[p].known) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Channel parameter schema is not admitted");
        }
        XrXirSourceType result=source_channel_schema_type(ctx,member->result);
        if (!result.known) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Channel result schema is not admitted");
        XrXirSourceDeclaration *record=(XrXirSourceDeclaration *)&ctx->query.declarations[symbol.declaration-1];
        record->native_identity=member->id;record->signature=source_owned_text(ctx,member->signature);
        if (!record->signature) return false;
        record->exported=true;record->generic_parent=ctx->channel_declaration;record->generic_parent_count=1;
        record->parameters=parameters;record->parameter_count=member->parameter_count;record->type=result;
    }
    return source_query_target_token_reference(ctx,node,ctx->channel_members[index],
        member->is_method ? XR_XIR_SOURCE_CALL : XR_XIR_SOURCE_READ);
}
#include "xxir_source_channel_blocking.inc.c"
#include "xxir_source_channel_timed.inc.c"
static bool source_channel_try_call(SourceContext *ctx,AstNode *node,SourceValue receiver,
    const XrNativeMemberDeclaration *member,SourceValue *value) {
    CallExprNode *call=&node->as.call_expr;bool send=member->operation==XR_NATIVE_OPERATION_CHANNEL_TRY_SEND;
    if (!member->lowered || !member->is_public || !member->is_method || member->is_static ||
        call->arg_count!=(send ? 1 : 0) || call->type_arg_count || call->default_arg_count ||
        (send && (!call->arguments || (call->arg_accesses && call->arg_accesses[0]!=XR_CALL_ARG_PLAIN))))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Channel operation requires its exact ordinary value arguments");
    XrXirType element=xr_xir_channel_element(&ctx->types,receiver.type),result=XR_XIR_UNIT;
    if (!(send ? source_channel_send_type(ctx,&result) : source_channel_recv_type(ctx,element,&result))) return false;
    if (!send) return source_recipe_record(ctx,(XrXirInstruction){.op=XR_XIR_CHANNEL_TRY_RECV,.type=result,.args={receiver.id,0}},value) &&
        source_channel_member_reference(ctx,node,member);
    SourceValue operands[2]={receiver};
    if (!source_plan_expression(ctx,call->arguments[0],(SourceExpectedType){true,element,false,false},&operands[1])) return false;
    if (operands[1].type!=element) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Channel message differs from its declared element type");
    return source_recipe_group(ctx,(XrXirInstruction){.op=XR_XIR_CHANNEL_TRY_SEND,.type=result},
        operands,element==XR_XIR_UNIT ? 1u : 2u,value) && source_channel_member_reference(ctx,node,member);
}
static bool source_channel_member(SourceContext *ctx,AstNode *node,SourceValue receiver,
    bool method,SourceValue *value) {
    if (!source_native_channel_declaration(ctx)) return false;
    const char *name=method ? node->as.call_expr.callee->as.member_access.name : node->as.member_access.name;
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_CHANNEL);
    const XrNativeMemberDeclaration *member=source_native_member(ctx,native,name);
    if (method && member && (member->operation==XR_NATIVE_OPERATION_CHANNEL_TRY_SEND || member->operation==XR_NATIVE_OPERATION_CHANNEL_TRY_RECV))
        return source_channel_try_call(ctx,node,receiver,member,value);
    if (method && member && (member->operation==XR_NATIVE_OPERATION_CHANNEL_SEND_TIMEOUT ||
        member->operation==XR_NATIVE_OPERATION_CHANNEL_RECV_TIMEOUT)) return source_channel_timed_call(ctx,node,receiver,member,value);
    if (method && member && (member->operation==XR_NATIVE_OPERATION_CHANNEL_SEND || member->operation==XR_NATIVE_OPERATION_CHANNEL_RECV ||
        member->operation==XR_NATIVE_OPERATION_CHANNEL_RECV_OR)) return source_channel_blocking_call(ctx,node,receiver,member,value);
    XrXirOp operation=XR_XIR_INVALID;XrXirType type=XR_XIR_UNIT;
    if (member && member->operation==XR_NATIVE_OPERATION_CHANNEL_CAPACITY) { operation=XR_XIR_CHANNEL_CAPACITY;type=XR_XIR_I64; }
    else if (member && member->operation==XR_NATIVE_OPERATION_CHANNEL_IS_CLOSED) { operation=XR_XIR_CHANNEL_IS_CLOSED;type=XR_XIR_BOOL; }
    else if (member && member->operation==XR_NATIVE_OPERATION_CHANNEL_CLOSE) operation=XR_XIR_CHANNEL_CLOSE;
    if (!member || !member->lowered || !member->is_public || member->is_static || member->is_method!=method || operation==XR_XIR_INVALID)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Channel member has no admitted operation contract");
    if (method && (node->as.call_expr.arg_count || node->as.call_expr.type_arg_count || node->as.call_expr.default_arg_count))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Channel close accepts no arguments");
    return source_recipe_record(ctx,(XrXirInstruction){.op=operation,.type=type,.args={receiver.id,0}},value) &&
        source_channel_member_reference(ctx,node,member);
}
