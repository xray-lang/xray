/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Binding producers retain declaration order while consuming typed actions. */
#include "xxir_source_recursive_binding.inc.c"
static bool source_binding(SourceContext *ctx, AstNode *node, bool top) {
    VarDeclNode *decl = &node->as.var_decl;
    if (decl->attr_count || (!decl->initializer && (decl->is_const || !decl->type_annotation)))
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "binding needs an initializer or explicit mutable type and no attributes");
    SourceValue initial;
    XrXirType annotation = XR_XIR_UNIT;
    if (decl->type_annotation && !source_type(ctx, decl->type_annotation, &annotation)) return false;
    if (decl->initializer) {
        AstNode *saved_initializer=ctx->recursive_initializer;
        const char *saved_name=ctx->recursive_name;
        bool recursive=!top && source_recursive_binding_readonly(ctx,node);
        const char *recursive_name=recursive?source_owned_text(ctx,decl->name):NULL;
        if(recursive&&!recursive_name)return false;
        ctx->recursive_initializer=recursive?decl->initializer:NULL;
        ctx->recursive_name=recursive_name;
        bool built=source_plan_expression(ctx,decl->initializer,
            (SourceExpectedType){decl->type_annotation!=NULL,annotation,false,false},&initial);
        ctx->recursive_initializer=saved_initializer;ctx->recursive_name=saved_name;
        if(!built)return false;
    } else if (!source_default_value(ctx, node, annotation, &initial)) return false;

    SourceName *symbol;
    bool initial_some = false;
    if (top) {
        symbol = find_name(ctx, ctx->names[ctx->module], decl->name);
        if (!symbol || symbol->kind != SOURCE_SLOT) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "binding declaration missing");
        symbol->inferred=!decl->type_annotation;symbol->annotation=annotation;symbol->has_annotation=decl->type_annotation!=NULL;
        if (symbol->inferred && !source_root_join_record(ctx,&initial)) return false;
        if (!source_binding_prepare(ctx, symbol, &initial)) return false;
        XrHirBindingAction publication;
        if(!source_hir_binding_use(ctx,node,symbol,XR_HIR_BINDING_PUBLISH,initial,&publication))return false;
        uint32_t before=ctx->bodies[ctx->function].count;
        return source_recipe_record(ctx,(XrXirInstruction){publication.operation,XR_XIR_UNIT,
            {publication.physical==XR_XIR_UNIT?0:initial.id},{0},publication.location,{0}},NULL)&&
            source_hir_binding_emitted(ctx,&publication,NULL,before);
    }
    for (SourceName *p = ctx->locals; p != ctx->scope; p = p->next) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (source_text_same(ctx, NULL, p->name, decl->name)) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "duplicate local name");
    }
    symbol = source_alloc(ctx, 1, sizeof(*symbol));
    if (!symbol) return false;
    XrXirType logical_type=initial.type;
    *symbol=(SourceName){.node=node,.kind=logical_type==XR_XIR_UNIT&&decl->is_const?SOURCE_UNIT_LOCAL:SOURCE_LOCAL,
        /* owned: This AST loan ends with Source construction; HIR/history copy the name. */
        .name=decl->name,.type=logical_type,.module=ctx->module,.mutable=!decl->is_const,.inferred=!decl->type_annotation,
        .annotation=annotation,.has_annotation=decl->type_annotation!=NULL};
    bool direct_borrowed=false;
    if(!decl->is_const&&!source_local_borrowed_storage(ctx,symbol,&direct_borrowed))return false;
    symbol->direct_borrowed=direct_borrowed;
    if(!source_query_declare(ctx,symbol,XR_XIR_SOURCE_BINDING,ctx->bodies[ctx->function].declaration,
        source_query_range(ctx,node,symbol->name)))return false;
    XrHirBodyBinding binding={0};if(!source_hir_binding_register(ctx,symbol,&binding))return false;
    XrHirBindingAction initialization;
    if(!decl->type_annotation&&!decl->is_const&&!source_root_join_record(ctx,&initial))return false;
    if(!source_hir_binding_use(ctx,node,symbol,XR_HIR_BINDING_INITIALIZE,initial,&initialization))return false;
    uint32_t before=ctx->bodies[ctx->function].count;
    if(initialization.operation!=XR_XIR_INVALID&&
        !source_recipe_record(ctx,(XrXirInstruction){initialization.operation,initialization.physical,
            {initialization.logical==XR_XIR_UNIT?0:initial.id,0},{0},0,{0}},&initial))return false;
    if(!source_hir_binding_emitted(ctx,&initialization,&initial,before))return false;
    if(initialization.operation!=XR_XIR_INVALID&&decl->type_annotation&&initial.type!=XR_XIR_UNIT)
        ctx->bodies[ctx->function].recipes[initial.id-ctx->functions[ctx->function].parameter_count].root.kind=1;
    symbol->index=initialization.route==XR_HIR_BINDING_UNIT?UINT32_MAX:initial.id;
    symbol->next=ctx->locals;ctx->locals=symbol;
    source_query_binding_type(ctx,symbol);
    return source_facts_assign(ctx, symbol, initial_some);
}
#include "xxir_source_repl.inc.c"
/* A nested function is a closure value bound immutably from its declaration on. */
static bool source_nested_function(SourceContext *ctx, AstNode *node) {
    FunctionDeclNode *decl = &node->as.function_decl;
    SourceName *symbol = NULL;
    for (SourceName *p = ctx->locals; p != ctx->scope; p = p->next) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (p->kind == SOURCE_PENDING && p->node == node) symbol = p;
        else if (source_text_same(ctx, NULL, p->name, decl->name))
            return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "duplicate local name");
    }
    SourceValue value;
    if (!source_closure(ctx, node, (SourceExpectedType){false, XR_XIR_UNIT, false, false}, &value)) return false;
    if (!symbol) {
        symbol = source_alloc(ctx, 1, sizeof(*symbol));
        if (!symbol) return false;
        /* owned: The graph outlives this temporary binding; snapshot publication copies its name. */
        *symbol = (SourceName){.next=ctx->locals,.name=decl->name,.imported=NULL,.node=node,.kind=SOURCE_PENDING,
            .index=UINT32_MAX,.module=ctx->module,.type=XR_XIR_UNIT,.mutable=false,
            .construction=false,.declaration=0,.inferred=false,.checked_library=false};
        if (!symbol->name) return false;
        ctx->locals = symbol;
    }
    symbol->kind = SOURCE_LOCAL; symbol->index = value.id; symbol->type = value.type;
    symbol->module = ctx->module; symbol->mutable = false; symbol->construction = false; symbol->inferred = true;
    if (!symbol->declaration && !source_query_declare(ctx, symbol, XR_XIR_SOURCE_BINDING, ctx->bodies[ctx->function].declaration,
        source_query_range(ctx, node, symbol->name))) return false;
    if(!source_hir_value_declared(ctx,node,symbol,value))return false;
    source_query_binding_type(ctx, symbol);
    return true;
}
/* Reserve every nested function name of a block before its first statement runs. */
static bool source_block_functions(SourceContext *ctx, AstNode *block) {
    for (int i = 0; i < block->as.block.count; ++i) {
        AstNode *node = block->as.block.statements[i];
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (!node || node->type != AST_FUNCTION_DECL) continue;
        for (SourceName *p = ctx->locals; p != ctx->scope; p = p->next) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            if (source_text_same(ctx, NULL, p->name, node->as.function_decl.name))
                return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "duplicate local name");
        }
        SourceName *symbol = source_alloc(ctx, 1, sizeof(*symbol));
        if (!symbol) return false;
        *symbol = (SourceName){.next=ctx->locals,.name=node->as.function_decl.name,.imported=NULL,.node=node,.kind=SOURCE_PENDING,
            .index=UINT32_MAX,.module=ctx->module,.type=XR_XIR_UNIT,.mutable=false,
            .construction=false,.declaration=0,.inferred=false,.checked_library=false};
        ctx->locals = symbol;
        if (!source_query_declare(ctx,symbol,XR_XIR_SOURCE_BINDING,ctx->bodies[ctx->function].declaration,
            source_query_range(ctx,node,symbol->name))) return false;
    }
    return true;
}
