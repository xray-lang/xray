/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_string_value.inc.c - Governed owned String value operations
 *
 * The receiver is already an owned SSA snapshot before arguments complete.
 * Declaration-query metadata stays in the receiving Source arena.
 */
static bool source_string_value_reference(SourceContext *ctx,AstNode *node,
    const XrNativeMemberDeclaration *member,XrXirType result) {
    uint32_t declaration=0;
    for(uint32_t i=0;i<ctx->query.declaration_count;++i) {
        if(!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        const XrXirSourceDeclaration *row=&ctx->query.declarations[i];
        if(row->kind==XR_XIR_SOURCE_MEMBER && row->parent==ctx->string_declaration &&
            row->native_identity==member->id) {declaration=row->id;break;}
    }
    if(!declaration) {
        SourceName symbol={0};symbol.name=source_owned_text(ctx,member->name);
        const char *signature=source_owned_text(ctx,member->signature);
        XrXirSourceType *parameters=member->parameter_count?
            source_alloc(ctx,member->parameter_count,sizeof(*parameters)):NULL;
        if(!symbol.name || !signature || (member->parameter_count && !parameters)) return false;
        for(uint32_t p=0;p<member->parameter_count;++p) {
            if(!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            parameters[p]=(XrXirSourceType){XR_XIR_STRING,0,true};
        }
        XrXirSourceRange range={ctx->string_module,(int)member->line,(int)member->column,
            (int)member->line,(int)(member->column+source_text_size(ctx,member->name))};
        if(!source_query_declare(ctx,&symbol,XR_XIR_SOURCE_MEMBER,ctx->string_declaration,range)) return false;
        declaration=symbol.declaration;
        XrXirSourceDeclaration *row=(XrXirSourceDeclaration *)&ctx->query.declarations[declaration-1];
        row->native_identity=member->id;row->signature=signature;row->exported=true;
        row->type=(XrXirSourceType){result,0,true};row->parameters=parameters;row->parameter_count=member->parameter_count;
    }
    return source_query_target_token_reference(ctx,node,declaration,XR_XIR_SOURCE_CALL);
}
static bool source_string_value_call(SourceContext *ctx,AstNode *node,SourceValue receiver,
    const XrNativeMemberDeclaration *member,SourceValue *value) {
    const CallExprNode *call=&node->as.call_expr;
    bool copy=member->operation==XR_NATIVE_OPERATION_STRING_COPY_BYTES;
    uint32_t count=copy?0:2;
    if(receiver.type!=XR_XIR_STRING || !member->is_public || !member->is_method || member->is_static ||
        member->receiver!=XR_NATIVE_RECEIVER_READ || member->parameter_count!=count ||
        member->result!=(copy?XR_NATIVE_TERM_ARRAY_U8:XR_NATIVE_TERM_STRING) ||
        call->type_arg_count || call->default_arg_count || call->arg_count!=(int)count ||
        (count && (!call->arguments || !member->parameters)))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"owned string operation requires its exact ordinary declaration");
    SourceValue arguments[3]={receiver};
    for(uint32_t p=0;p<count;++p) {
        if(member->parameters[p].type!=XR_NATIVE_TERM_STRING || member->parameters[p].optional ||
            (call->arg_accesses && call->arg_accesses[p]!=XR_CALL_ARG_PLAIN))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"string replacement requires ordinary READ String arguments");
        if(!source_plan_expression(ctx,call->arguments[p],(SourceExpectedType){true,XR_XIR_STRING,false,false},arguments+p+1)) return false;
        if(arguments[p+1].type!=XR_XIR_STRING)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"string replacement requires String arguments");
    }
    XrXirType result=XR_XIR_STRING;
    if(copy && !source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_U8},&result)) return false;
    if(!source_string_value_reference(ctx,call->callee,member,result)) return false;
    if(copy) return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_STRING_COPY_BYTES,result,{receiver.id},{0},0,{0}},value);
    return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_STRING_REPLACE,XR_XIR_STRING,{0},{0},
        member->operation==XR_NATIVE_OPERATION_STRING_REPLACE_ALL?1:0,{0}},arguments,3,value);
}
