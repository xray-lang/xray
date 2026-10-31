/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_exported_const.inc.c - Actual public immutable slot reads
 *
 * KEY CONCEPT:
 *   Source imports read the real immutable slot through a direct dependency.
 *   Common verification and Instance receiving independently prove permission.
 */
static bool source_exported_const_syntax(SourceContext *ctx,AstNode *node) {
    return !ctx->interactive && node && node->type==AST_CONST_DECL && node->is_exported && node->as.var_decl.is_const;
}
static bool source_exported_const(SourceContext *ctx,const SourceName *name) {
    if (!name || (name->kind!=SOURCE_SLOT && name->kind!=SOURCE_UNIT_SLOT) || name->mutable || name->index>=ctx->slot_count)
        return false;
    if (!name->checked_library) return source_exported_const_syntax(ctx,name->node);
    uint32_t slot=name->index;
    return !ctx->slots[slot].mutable && ctx->slot_fact_present && ctx->slot_fact_present[slot] &&
        ctx->slot_facts[slot].slot==slot && ctx->slot_facts[slot].exported==1;
}
static bool source_exported_const_ground(SourceContext *ctx,AstNode *node,SourceExpectedType *output) {
    SourceName *binding=NULL,*target=NULL;
    if (node->type==AST_VARIABLE) {
        binding=visible_name(ctx,node->as.variable.name);
        if (binding && binding->kind==SOURCE_IMPORT)target=imported_declaration(ctx,binding,binding->imported);
    } else if (node->type==AST_MEMBER_ACCESS && node->as.member_access.object->type==AST_VARIABLE) {
        binding=visible_name(ctx,node->as.member_access.object->as.variable.name);
        if (binding && binding->kind==SOURCE_MODULE)target=imported_declaration(ctx,binding,node->as.member_access.name);
    }
    if(ctx->diagnostic.status!=XR_XIR_OK)return false;
    if(!source_exported_const(ctx,target))return true;
    if(source_name_ready(target))*output=(SourceExpectedType){true,target->type,false,false};
    else if(target->node && target->node->as.var_decl.type_annotation) {
        XrXirType type=XR_XIR_UNIT;
        if(!source_type(ctx,target->node->as.var_decl.type_annotation,&type))return false;
        *output=(SourceExpectedType){true,type,false,false};
    }
    return true;
}
static bool source_imported_slot_type(SourceContext *ctx,AstNode *node,const SourceName *target,XrXirType *type) {
    if(!source_exported_const(ctx,target) || !source_name_ready(target) ||
        !ctx->slot_fact_present || !ctx->slot_fact_present[target->index] ||
        ctx->slot_facts[target->index].slot!=target->index || ctx->slot_facts[target->index].exported!=1 ||
        ctx->slots[target->index].module!=target->module || ctx->slots[target->index].mutable ||
        ctx->slots[target->index].type!=target->type)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"imported value requires an actual public immutable slot");
    XrXirDeclarations declarations={.modules=ctx->modules,.module_count=ctx->module_count};
    if(ctx->module>=ctx->module_count || target->module>=ctx->module_count ||
        !xr_xir_module_imports(&declarations,ctx->module,target->module))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"imported slot requires a direct module dependency");
    *type=target->type;return true;
}
static bool source_imported_value(SourceContext *ctx,AstNode *node,SourceName *binding,
    SourceName *target,SourceExpectedType expected,SourceValue *value) {
    if (target && target->kind==SOURCE_IMPORT)target=imported_declaration(ctx,target,target->imported);
    if (!target || (target->kind!=SOURCE_SLOT && target->kind!=SOURCE_UNIT_SLOT))
        return source_function_value(ctx,node,binding,target,expected,value);
    XrXirType type=XR_XIR_UNIT;
    if(!source_imported_slot_type(ctx,node,target,&type) || !source_query_reference(ctx,node,binding,target,XR_XIR_SOURCE_READ))return false;
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_SLOT_LOAD,type,{0},{0},target->index,{0}},value);
}
