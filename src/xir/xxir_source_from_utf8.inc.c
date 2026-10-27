/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_from_utf8.inc.c - Strict decoding through an ordinary throwing callee
 */
static bool source_from_utf8_receiver(SourceContext *ctx,AstNode *callee) {
    MemberAccessNode *member=&callee->as.member_access;
    return member->object && member->object->type==AST_VARIABLE &&
        source_text_same(ctx,callee,member->object->as.variable.name,"string") &&
        !source_native_type_shadowed(ctx,"string") && source_text_same(ctx,callee,member->name,"fromUtf8");
}
static bool source_from_utf8_reference(SourceContext *ctx,AstNode *node,
    const XrNativeMemberDeclaration *member,XrXirType slice) {
    if (!source_string_declaration(ctx)) return false;
    uint32_t declaration=0;
    for (uint32_t i=0;i<ctx->query.declaration_count;++i) {
        const XrXirSourceDeclaration *row=&ctx->query.declarations[i];
        if (row->kind==XR_XIR_SOURCE_MEMBER && row->parent==ctx->string_declaration &&
            row->native_identity==member->id) {declaration=row->id;break;}
    }
    if (!declaration) {
        SourceName symbol={0};symbol.name=source_owned_text(ctx,member->name);
        const char *signature=source_owned_text(ctx,member->signature);
        XrXirSourceType *parameter=source_alloc(ctx,1,sizeof(*parameter));
        if (!symbol.name || !signature || !parameter) return false;
        *parameter=(XrXirSourceType){slice,0,true};
        XrXirSourceRange range={ctx->string_module,(int)member->line,(int)member->column,
            (int)member->line,(int)(member->column+source_text_size(ctx,member->name))};
        if (!source_query_declare(ctx,&symbol,XR_XIR_SOURCE_MEMBER,ctx->string_declaration,range)) return false;
        declaration=symbol.declaration;
        XrXirSourceDeclaration *row=(XrXirSourceDeclaration *)&ctx->query.declarations[declaration-1];
        row->native_identity=member->id;row->signature=signature;row->exported=true;
        row->type=(XrXirSourceType){XR_XIR_STRING,0,true};row->parameters=parameter;row->parameter_count=1;
    }
    return source_query_target_token_reference(ctx,node->as.member_access.object,ctx->string_declaration,XR_XIR_SOURCE_READ) &&
        source_query_target_token_reference(ctx,node,declaration,XR_XIR_SOURCE_CALL);
}
static bool source_from_utf8_call(SourceContext *ctx,AstNode *node,SourceValue *output) {
    CallExprNode *call=&node->as.call_expr;
    const XrNativeCallableDeclaration *native=xr_native_callable_by_id(1);
    if (!native || !source_native_admit(ctx,native->owner) || !source_native_admit(ctx,native->error_e_type) ||
        call->arg_count!=1 || !call->arguments || call->type_arg_count || call->default_arg_count ||
        (call->arg_accesses && call->arg_accesses[0]!=XR_CALL_ARG_PLAIN))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"strict UTF-8 conversion requires its canonical static signature");
    SourceName *error=source_prelude_name(ctx,native->error_e_type->name);
    if (!error || error->kind!=SOURCE_NOMINAL || error->index>=ctx->nominals.count ||
        ctx->nominals.declarations[error->index].native.native_id!=native->error_e_type->id)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"strict UTF-8 conversion requires the canonical error owner");
    XrXirType slice;
    if (!source_intern_type(ctx,(XrXirTypeNode){XR_XIR_TYPE_SLICE,XR_XIR_U8,NULL,0,XR_XIR_UNIT,0,0,{0}},&slice)) return false;
    SourceValue argument;
    if (!source_plan_expression(ctx,call->arguments[0],(SourceExpectedType){true,slice,false,false},&argument)) return false;
    if (argument.type!=slice) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"strict UTF-8 conversion requires Slice<u8>");
    uint32_t target=UINT32_MAX;
    for (uint32_t f=0;f<ctx->function_count;++f)
        if (ctx->identities[f].native_callable==native->id && ctx->identities[f].module==ctx->module) {target=f;break;}
    if (target==UINT32_MAX) {
        if (ctx->function_count>=ctx->function_capacity) return source_fail(ctx,node,XR_XIR_BUDGET,"native callable inventory exhausted");
        XrXirType *parameters=source_alloc(ctx,1,sizeof(*parameters));
        const char *name=source_owned_text(ctx,native->member->name);
        if (!parameters || !name) return false;
        *parameters=slice;target=ctx->function_count++;
        ctx->functions[target]=(XrXirFunction){.name=name,.name_length=(uint32_t)strlen(name),
            .parameters=parameters,.parameter_count=1,.result=XR_XIR_STRING,.error_type=error->type};
        ctx->identities[target]=(XrXirFunctionIdentity){.module=ctx->module,
            .promises=XR_XIR_FUNCTION_NO_SUSPEND,.native_callable=native->id};
        ctx->bodies[target].module=ctx->module;
    }
    return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_CALL,XR_XIR_STRING,{0},{0},target,{0}},&argument,1,output) &&
        source_from_utf8_reference(ctx,call->callee,native->member,slice);
}
