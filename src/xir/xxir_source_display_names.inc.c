/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_display_names.inc.c - Own actual declaration generic spellings
 */
static bool source_display_names_copy(SourceContext *ctx,XrXirSourceDeclarationSyntax *fact) {
    uint32_t count=fact->type_parameter_count;
    if(!count&&!fact->type_parameters)return true;
    if(!count||!fact->type_parameters||count>65536)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"display generic names are missing");
    const char **names=source_alloc(ctx,count,sizeof(*names));if(!names)return false;
    for(uint32_t p=0;p<count;++p) {
        if(!source_work(ctx,NULL))return false;
        if(!fact->type_parameters[p]||!*fact->type_parameters[p])
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"display generic spelling is missing");
        names[p]=source_owned_text(ctx,fact->type_parameters[p]);if(!names[p])return false;
    }
    fact->type_parameters=names;return true;
}
static bool source_display_names(SourceContext *ctx,uint32_t declaration,XrGenericParam **parameters,uint32_t count) {
    if(!count)return true;
    if(!parameters||count>65536||!declaration||declaration>ctx->query.declaration_count||
        count!=ctx->query.declarations[declaration-1].generic_parameter_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"display generic owner diverged");
    const char **names=source_alloc(ctx,count,sizeof(*names));if(!names)return false;
    for(uint32_t p=0;p<count;++p) {
        if(!source_work(ctx,NULL))return false;
        if(!parameters[p]||!parameters[p]->name||!*parameters[p]->name)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"display generic spelling is missing");
        names[p]=source_owned_text(ctx,parameters[p]->name);if(!names[p])return false;
    }
    XrXirSourceDeclarationSyntax *fact=(XrXirSourceDeclarationSyntax *)&ctx->syntax.declarations[declaration-1];
    fact->type_parameters=names;fact->type_parameter_count=count;return true;
}
static bool source_display_nominal_names(SourceContext *ctx,const SourceName *owner) {
    if(!owner||!owner->node||!owner->declaration||owner->checked_library)return true;
    AstNode *node=owner->node;XrGenericParam **parameters=NULL;int count=0;
    switch(node->type) {
    case AST_STRUCT_DECL:parameters=node->as.struct_decl.type_params;count=node->as.struct_decl.type_param_count;break;
    case AST_CLASS_DECL:parameters=node->as.class_decl.type_params;count=node->as.class_decl.type_param_count;break;
    case AST_ENUM_DECL:parameters=node->as.enum_decl.type_params;count=node->as.enum_decl.type_param_count;break;
    case AST_INTERFACE_DECL:parameters=node->as.interface_decl.type_params;count=node->as.interface_decl.type_param_count;break;
    default:return true;
    }
    if(count<0)return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"display generic count is invalid");
    return source_display_names(ctx,owner->declaration,parameters,(uint32_t)count);
}
static bool source_display_interface_names(SourceContext *ctx,const SourceName *owner) {
    if(!owner||!owner->node||owner->checked_library)return true;
    InterfaceDeclNode *decl=&owner->node->as.interface_decl;
    for(int m=0;m<decl->method_count;++m) {
        if(!source_work(ctx,NULL))return false;
        AstNode *node=decl->methods[m];
        if(!node||node->type!=AST_INTERFACE_METHOD)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"display interface method is invalid");
        InterfaceMethodNode *method=&node->as.interface_method;
        if(decl->type_param_count<0||method->type_param_count<0||(uint32_t)method->type_param_count>65536||
            (uint32_t)decl->type_param_count>65536u-(uint32_t)method->type_param_count)
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"display interface generic count is invalid");
        uint32_t prefix=(uint32_t)decl->type_param_count,count=prefix+(uint32_t)method->type_param_count;
        XrGenericParam **parameters=count?source_alloc(ctx,count,sizeof(*parameters)):NULL;
        if(count&&!parameters)return false;
        for(uint32_t p=0;p<count;++p) {
            if(!source_work(ctx,node))return false;
            parameters[p]=p<prefix?decl->type_params[p]:method->type_params[p-prefix];
        }
        if(!source_display_names(ctx,ctx->interface_member_declarations[owner->index][m],parameters,count))return false;
    }
    return true;
}
static bool source_display_names_publish(SourceContext *ctx) {
    for(uint32_t f=0;f<ctx->function_count;++f) {
        if(!source_work(ctx,NULL))return false;
        const SourceFunction *body=&ctx->bodies[f];
        /* Synthetic lexical bodies inherit binders from their actual declaration
         * owner; their own query declaration introduces no type parameters. */
        bool inherited=body->node && (body->node->type==AST_FIELD_DECL || body->node->type==AST_COMPTIME_EXPR);
        uint32_t declaration=inherited?body->generic_owner:body->declaration;
        if(body->declaration&&body->node&&
            !source_display_names(ctx,declaration,body->type_parameters,body->type_parameter_count))return false;
    }
    for(uint32_t d=0;d<ctx->nominals.count;++d)
        if(!source_work(ctx,NULL)||!source_display_nominal_names(ctx,ctx->nominal_sources[d]))return false;
    for(uint32_t d=0;d<ctx->interfaces.count;++d)
        if(!source_work(ctx,NULL)||!source_display_nominal_names(ctx,ctx->interface_sources[d])||
            !source_display_interface_names(ctx,ctx->interface_sources[d]))return false;
    for(uint32_t d=0;d<ctx->query.declaration_count;++d) {
        if(!source_work(ctx,NULL))return false;
        const XrXirSourceDeclaration *parameter=&ctx->query.declarations[d];
        if(parameter->kind!=XR_XIR_SOURCE_TYPE_PARAMETER||!parameter->parent)continue;
        uint32_t parent=parameter->parent;
        if(parent>ctx->query.declaration_count)return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"display parameter parent is invalid");
        XrXirSourceDeclarationSyntax *fact=(XrXirSourceDeclarationSyntax *)&ctx->syntax.declarations[parent-1];
        if(fact->type_parameters||ctx->query.declarations[parent-1].generic_parameter_count!=1)continue;
        *fact=(XrXirSourceDeclarationSyntax){.name=fact->name,.role=fact->role,.flags=fact->flags,
            .type_parameters=&parameter->name,.type_parameter_count=1};
        if(!source_display_names_copy(ctx,fact))return false;
    }
    return true;
}
