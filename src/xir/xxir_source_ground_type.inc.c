/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_ground_type.inc.c - Declaration type facts without evaluating values
 */
static bool source_ground_type(SourceContext *ctx,AstNode *node,uint32_t depth,SourceExpectedType *output) {
    *output=(SourceExpectedType){false,XR_XIR_UNIT, false};
    if (!node || !source_work(ctx,node)) return node==NULL;
    if (depth>=128) return source_fail(ctx,node,XR_XIR_BUDGET,"ground type collection depth exhausted");
    if (node->type==AST_GROUPING) return source_ground_type(ctx,node->as.grouping,depth+1,output);
    if (node->type==AST_VARIABLE || node->type==AST_THIS_EXPR) {
        SourceName *binding=visible_name(ctx,node->type==AST_VARIABLE ? node->as.variable.name : "this");
        if (binding && (binding->kind==SOURCE_LOCAL || binding->kind==SOURCE_SLOT))
            *output=(SourceExpectedType){true,source_symbol_type(ctx,binding), false};
        return true;
    }
    if (node->type==AST_INDEX_GET) {
        SourceExpectedType receiver;
        if (!source_ground_type(ctx,node->as.index_get.array,depth+1,&receiver)) return false;
        if (receiver.present && xr_xir_type_is_array(&ctx->types,receiver.type))
            *output=(SourceExpectedType){true,xr_xir_array_element(&ctx->types,receiver.type), false};
        return true;
    }
    AstNode *member=node;
    bool call=node->type==AST_CALL_EXPR;
    if (call) member=node->as.call_expr.callee;
    if (call && member && member->type==AST_VARIABLE) {
        SourceName *binding=visible_name(ctx,member->as.variable.name);
        if (binding && (binding->kind==SOURCE_LOCAL || binding->kind==SOURCE_SLOT)) {
            const XrXirTypeNode *signature=xr_xir_callable_signature(&ctx->types,binding->type);
            if (signature) *output=(SourceExpectedType){true,signature->result, false};
        }
        return true;
    }
    if (!member || member->type!=AST_MEMBER_ACCESS) return true;
    SourceExpectedType receiver;
    if (!source_ground_type(ctx,member->as.member_access.object,depth+1,&receiver)) return false;
    if (!call && receiver.present && xr_xir_tuple_signature(&ctx->types,receiver.type)) {
        uint32_t field=0;
        if (!source_tuple_index(ctx,member,member->as.member_access.name,&field)) return false;
        const XrXirTypeNode *tuple=xr_xir_tuple_signature(&ctx->types,receiver.type);
        if (field>=tuple->parameter_count)
            return source_fail(ctx,member,XR_XIR_BAD_TYPE,"Tuple field index exceeds its ordered arity");
        *output=(SourceExpectedType){true,tuple->parameters[field].type,false};return true;
    }
    if (!receiver.present || !xr_xir_type_is_nominal(&ctx->types,receiver.type)) return true;
    const XrXirTypeNode *found=xr_xir_type_node(&ctx->types,receiver.type);
    SourceSubstitution substitution={found->nominal.arguments,found->nominal.argument_count};
    uint32_t owner=found->nominal.declaration;
    if (call) {
        SourceName *method=source_method_find(ctx,receiver.type,member->as.member_access.name);
        if (!method || ctx->generics[method->index].parameter_count!=substitution.count) return true;
        if (!source_substitute(ctx,&substitution,ctx->functions[method->index].result,0,&output->type)) return false;
        output->present=true;return true;
    }
    const XrXirNominalDeclaration *declaration=&ctx->nominals.declarations[owner];
    for (uint32_t f=0;f<declaration->field_count;++f) {
        if (!source_work(ctx,node)) return false;
        const XrXirNominalField *field=&declaration->fields[f];
        const char *name=member->as.member_access.name;
        if (source_text_size(ctx, name)!=field->name.length || !source_span_same(ctx, NULL, name, field->name.bytes, field->name.length)) continue;
        if (!source_substitute(ctx,&substitution,field->type,0,&output->type)) return false;
        output->present=true;break;
    }
    return true;
}
