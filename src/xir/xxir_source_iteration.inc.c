/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_iteration.inc.c - Owned Array snapshots and per-iteration bindings
 *
 * KEY CONCEPT:
 *   One evaluated logical Array feeds the existing loop and cleanup machinery.
 */
static bool source_iteration_binding(SourceContext *ctx, AstNode *node,
    const char *name, SourceValue value) {
    if (source_text_same(ctx, NULL, name, "_")) return true;
    SourceName *symbol = source_alloc(ctx,1,sizeof(*symbol));
    if (!symbol) return false;
    *symbol = (SourceName){ctx->locals,name,NULL,node,SOURCE_LOCAL,value.id,ctx->module,
        value.type,false,false,0};
    ctx->locals = symbol;
    if (!source_query_declare(ctx,symbol,XR_XIR_SOURCE_BINDING,
        ctx->bodies[ctx->function].declaration,source_query_range(ctx,node,name))) return false;
    source_query_binding_type(ctx,symbol);
    return true;
}
static bool source_for_in_body(SourceContext *ctx, AstNode *node,
    SourceValue index, SourceValue element) {
    ForInStmtNode *loop = &node->as.for_in_stmt;
    SourceName *saved = ctx->locals, *scope = ctx->scope;
    ctx->scope = saved;
    bool ok = source_iteration_binding(ctx,node,loop->item_name,loop->is_keyvalue ? index : element) &&
        (!loop->is_keyvalue || source_iteration_binding(ctx,node,loop->value_name,element)) &&
        scoped_statement(ctx,loop->body);
    ctx->locals = saved; ctx->scope = scope;
    return ok;
}
static bool source_for_in(SourceContext *ctx, AstNode *node) {
    ForInStmtNode *syntax = &node->as.for_in_stmt;
    if (syntax->label || syntax->is_tuple_head || syntax->item_type)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,
            "Array for-in requires an unlabelled single or direct pair binding without annotation");
    if (!syntax->item_name || (syntax->is_keyvalue && !syntax->value_name) || !syntax->body)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"for-in binding shape is invalid");
    if (syntax->is_keyvalue && !source_text_same(ctx, NULL, syntax->item_name, "_") && source_text_same(ctx, NULL, syntax->item_name, syntax->value_name))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"for-in binding names must be distinct");
    SourceValue array, length, zero, cursor;
    if (!expression(ctx,syntax->collection,&array)) return false;
    if (!xr_xir_type_is_array(&ctx->types,array.type))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"for-in requires a proved built-in Array value");
    XrXirType element_type = xr_xir_array_element(&ctx->types,array.type), cursor_type;
    if (!source_native_array_declaration(ctx) || !source_cell_type(ctx,XR_XIR_I64,&cursor_type) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ARRAY_LEN,XR_XIR_I64,{array.id},{0},0,{0}},&length) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},&zero) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_NEW,cursor_type,{zero.id},{0},0,{0}},&cursor)) return false;
    SourceFunction *body = &ctx->bodies[ctx->function];
    uint32_t head = body->block_count;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{head},0,{0}},NULL) || !begin_block(ctx)) return false;
    SourceValue index, condition;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_READ,XR_XIR_I64,{cursor.id},{0},0,{0}},&index) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_LT_INT,XR_XIR_BOOL,{index.id,length.id},{0},0,{0}},&condition)) return false;
    uint32_t branch = body->count, entry = body->block_count;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_BRANCH,XR_XIR_UNIT,{condition.id},{entry,0},0,{0}},NULL) || !begin_block(ctx)) return false;
    SourceValue element;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ARRAY_GET,element_type,{array.id,index.id},{0},0,{0}},&element)) return false;
    SourceLoop loop = {ctx->loop,NULL,NULL,body->frontier}; ctx->loop = &loop;
    bool ok = source_for_in_body(ctx,node,index,element); ctx->loop = loop.parent;
    if (!ok) return false;
    if (!ctx->returned || loop.continues) {
        uint32_t step = body->block_count;
        if ((!ctx->returned && !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{step},0,{0}},NULL)) ||
            !patch_exits(ctx,loop.continues,step) || !begin_block(ctx)) return false;
        SourceValue one, next;
        if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},1,{0}},&one) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ADD_INT,XR_XIR_I64,{index.id,one.id},{0},0,{0}},&next) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_WRITE,XR_XIR_UNIT,{cursor.id,next.id},{0},0,{0}},NULL) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{head},0,{0}},NULL)) return false;
    }
    uint32_t exit = body->block_count;
    if (!begin_block(ctx)) return false;
    body->recipes[branch].instruction.targets[1] = exit;
    return patch_exits(ctx,loop.breaks,exit);
}
