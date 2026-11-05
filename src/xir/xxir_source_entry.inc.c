/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_entry.inc.c - Entry-file ownership of executable statements
 */
static bool source_module_declaration(const AstNode *node) {
    switch (node->type) {
    case AST_IMPORT_STMT: case AST_EXPORT_STMT:
    case AST_VAR_DECL: case AST_CONST_DECL: case AST_DESTRUCTURE_DECL:
    case AST_FUNCTION_DECL: case AST_CLASS_DECL: case AST_STRUCT_DECL:
    case AST_UNION_DECL: case AST_ENUM_DECL: case AST_INTERFACE_DECL:
    case AST_TYPE_ALIAS: case AST_GLOBAL_ASM:
        return true;
    default: return false;
    }
}
static bool source_module_entry_check(SourceContext *ctx, const AstNode *program) {
    if (ctx->linkage_kind==XR_XIR_PROGRAM && ctx->module==(uint32_t)ctx->graph->entry_index)
        return true;
    if (program->as.program.count<0 || (program->as.program.count && !program->as.program.statements))
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"module statement inventory is missing");
    /* Library roots have no program entry; their initializers are declarations
     * too. Inspect only direct children, so initializer and function bodies keep
     * their ordinary construction rules. */
    for (int i=0;i<program->as.program.count;++i) {
        AstNode *node=program->as.program.statements[i];
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (!node) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"module statement is missing");
        if (!source_module_declaration(node))
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,
                "E0335: imported modules allow only declarations and state initialization; move the statement to the entry file");
    }
    return true;
}
