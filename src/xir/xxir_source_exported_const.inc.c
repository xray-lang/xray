/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_exported_const.inc.c - Ordinary owner functions for public constants
 *
 * KEY CONCEPT:
 *   Source imports denote the real readonly slot, while an ordinary function
 *   loads it within its actual module under the common verifier's guards.
 */
static bool source_exported_const_syntax(SourceContext *ctx,AstNode *node) {
    if (ctx->interactive || !node || node->type!=AST_CONST_DECL || !node->is_exported ||
        !node->as.var_decl.is_const || !node->as.var_decl.type_annotation ||
        node->as.var_decl.type_annotation->kind!=XR_TREF_SCALAR) return false;
    XrXirType type;SourceInteger literal;
    return source_type(ctx,node->as.var_decl.type_annotation,&type) && type==XR_XIR_I64 &&
        source_direct_integer(ctx,node->as.var_decl.initializer,&literal) && literal.present;
}
static bool source_exported_const(SourceContext *ctx,const SourceName *name) {
    if (!name || name->kind!=SOURCE_SLOT || name->mutable || name->index>=ctx->slot_count) return false;
    if (!name->checked_library) return source_exported_const_syntax(ctx,name->node);
    uint32_t slot=name->index;
    return ctx->slots[slot].type==XR_XIR_I64 && !ctx->slots[slot].mutable &&
        ctx->slot_fact_present && ctx->slot_fact_present[slot] &&
        ctx->slot_facts[slot].slot==slot && ctx->slot_facts[slot].exported==1;
}
static bool source_exported_const_ground(SourceContext *ctx,AstNode *node,SourceExpectedType *output) {
    SourceName *binding=NULL,*target=NULL;
    if (node->type==AST_VARIABLE) {
        binding=visible_name(ctx,node->as.variable.name);
        if (binding && binding->kind==SOURCE_IMPORT)
            target=imported_declaration(ctx,binding,binding->imported);
    } else if (node->type==AST_MEMBER_ACCESS && node->as.member_access.object->type==AST_VARIABLE) {
        binding=visible_name(ctx,node->as.member_access.object->as.variable.name);
        if (binding && binding->kind==SOURCE_MODULE)
            target=imported_declaration(ctx,binding,node->as.member_access.name);
    }
    if (ctx->diagnostic.status!=XR_XIR_OK) return false;
    if (source_exported_const(ctx,target)) *output=(SourceExpectedType){true,XR_XIR_I64,false,false};
    return ctx->diagnostic.status==XR_XIR_OK;
}
static bool source_imported_value(SourceContext *ctx,AstNode *node,SourceName *binding,
    SourceName *target,SourceExpectedType expected,SourceValue *value) {
    if (target && target->kind==SOURCE_IMPORT) target=imported_declaration(ctx,target,target->imported);
    if (!target || target->kind!=SOURCE_SLOT)
        return source_function_value(ctx,node,binding,target,expected,value);
    if (!source_exported_const(ctx,target) || !target->reader || target->reader-1>=ctx->function_count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"imported value requires an actual public readonly integer slot");
    if (!source_query_reference(ctx,node,binding,target,XR_XIR_SOURCE_READ)) return false;
    return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_CALL,XR_XIR_I64,{0},{0},target->reader-1,{0}},NULL,0,value);
}
static bool source_exported_const_reserve(SourceContext *ctx) {
    for (uint32_t m=0;m<(uint32_t)ctx->graph->spec_count;++m) {
        ctx->module=m;
        for (SourceName *name=ctx->names[m];name;name=name->next) {
            if (!source_work(ctx,name->node)) return false;
            if (!source_exported_const(ctx,name)) {
                if (ctx->diagnostic.status!=XR_XIR_OK) return false;
                continue;
            }
            if (name->reader || ctx->function_count>=ctx->function_capacity ||
                ctx->function_count>=ctx->compile.limits.functions)
                return source_fail(ctx,name->node,XR_XIR_BUDGET,"constant reader inventory exhausted");
            uint32_t f=ctx->function_count++,owner=ctx->slots[name->index].module;
            char *label=source_alloc(ctx,48,1);
            if (!label || source_format(ctx,label,48,"$constant%u",f)!=XR_DIAG_OK)
                return source_fail(ctx,name->node,XR_XIR_BUDGET,"constant reader label exhausted");
            ctx->functions[f]=(XrXirFunction){label,(uint32_t)source_text_size(ctx,label),NULL,0,
                XR_XIR_I64,NULL,0,NULL,0,NULL,0,XR_XIR_NEVER};
            ctx->identities[f]=(XrXirFunctionIdentity){.module=owner,.exported=1,.promises=XR_XIR_FUNCTION_NO_SUSPEND};
            ctx->bodies[f].module=owner;name->reader=f+1;
        }
    }
    return true;
}
static bool source_exported_const_finish(SourceContext *ctx) {
    for (uint32_t m=0;m<(uint32_t)ctx->graph->spec_count;++m) {
        for (SourceName *name=ctx->names[m];name;name=name->next) {
            if (!source_work(ctx,name->node)) return false;
            if (name->kind!=SOURCE_SLOT || !name->reader) continue;
            uint32_t slot=name->index,f=name->reader-1;
            if (!source_exported_const(ctx,name) || slot>=ctx->slot_count || f>=ctx->function_count ||
                ctx->slots[slot].type!=XR_XIR_I64 || ctx->slots[slot].mutable ||
                ctx->identities[f].module!=ctx->slots[slot].module)
                return source_fail(ctx,name->node,XR_XIR_BAD_STRUCTURE,"constant reader lost its actual slot owner");
            ctx->function=f;ctx->module=ctx->slots[slot].module;ctx->locals=ctx->scope=NULL;
            ctx->loop=NULL;ctx->facts=NULL;ctx->epochs=NULL;ctx->flow_alternatives=0;ctx->returned=false;
            SourceValue value;
            if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_SLOT_LOAD,XR_XIR_I64,{0},{0},slot,{0}},&value) ||
                !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{value.id},{0},0,{0}},NULL)) return false;
            ctx->returned=true;if (!finish_body(ctx)) return false;
        }
    }
    return true;
}
