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
/* A counting loop over [first, bound) or [first, bound] on an i64 cursor cell. The emitter is left in the
 * body block with the loop registered for break and continue; the body reads `index` once per iteration. */
typedef struct SourceCounter {
    SourceValue cursor, index, bound;
    uint32_t head, branch, last_branch;
    bool inclusive;
    SourceLoop loop;
} SourceCounter;
static bool source_counter_open(SourceContext *ctx, SourceValue first, SourceValue bound, bool inclusive,
    SourceCounter *counter) {
    XrXirType cursor_type;
    SourceValue condition;
    if (!source_cell_type(ctx,XR_XIR_I64,&cursor_type) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_NEW,cursor_type,{first.id},{0},0,{0}},&counter->cursor)) return false;
    SourceFunction *body = &ctx->bodies[ctx->function];
    counter->head = body->block_count; counter->bound = bound; counter->inclusive = inclusive; counter->last_branch = 0;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{counter->head},0,{0}},NULL) || !begin_block(ctx)) return false;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_READ,XR_XIR_I64,{counter->cursor.id},{0},0,{0}},&counter->index) ||
        !source_recipe_record(ctx,(XrXirInstruction){inclusive ? XR_XIR_LE_INT : XR_XIR_LT_INT,XR_XIR_BOOL,{counter->index.id,bound.id},{0},0,{0}},&condition))
        return false;
    uint32_t entry = body->block_count;
    counter->branch = body->count;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_BRANCH,XR_XIR_UNIT,{condition.id},{entry,0},0,{0}},NULL) || !begin_block(ctx)) return false;
    counter->loop = (SourceLoop){ctx->loop,NULL,NULL,body->frontier}; ctx->loop = &counter->loop;
    return true;
}
/* Leaves the loop through the exit block that continue-free paths, breaks and the bound test all reach. */
static bool source_counter_close(SourceContext *ctx, SourceCounter *counter) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    ctx->loop = counter->loop.parent;
    if (!ctx->returned || counter->loop.continues) {
        uint32_t step = body->block_count;
        if ((!ctx->returned && !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{step},0,{0}},NULL)) ||
            !patch_exits(ctx,counter->loop.continues,step) || !begin_block(ctx)) return false;
        if (counter->inclusive) {
            /* The end bound itself may be the largest i64, so stop before stepping past it. */
            SourceValue more;
            if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_LT_INT,XR_XIR_BOOL,{counter->index.id,counter->bound.id},{0},0,{0}},&more)) return false;
            counter->last_branch = body->count;
            uint32_t next_block = body->block_count;
            if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_BRANCH,XR_XIR_UNIT,{more.id},{next_block,0},0,{0}},NULL) || !begin_block(ctx)) return false;
        }
        SourceValue one, next;
        if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},1,{0}},&one) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ADD_INT,XR_XIR_I64,{counter->index.id,one.id},{0},0,{0}},&next) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_WRITE,XR_XIR_UNIT,{counter->cursor.id,next.id},{0},0,{0}},NULL) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{counter->head},0,{0}},NULL)) return false;
    }
    uint32_t exit = body->block_count;
    if (!begin_block(ctx)) return false;
    body->recipes[counter->branch].instruction.targets[1] = exit;
    if (counter->last_branch) body->recipes[counter->last_branch].instruction.targets[1] = exit;
    return patch_exits(ctx,counter->loop.breaks,exit);
}
/* A break out of the innermost counter loop; the emitter must start a new block afterwards. */
static bool source_counter_break(SourceContext *ctx) {
    SourcePatch *patch = source_alloc(ctx, 1, sizeof(*patch));
    if (!patch) return false;
    *patch = (SourcePatch) {ctx->loop->breaks, ctx->bodies[ctx->function].count}; ctx->loop->breaks = patch;
    ctx->returned = true;
    return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {0}, 0, {0}}, NULL);
}
/* `a..b` and `a..=b` count an i64 cursor; both endpoints are evaluated once, start first. */
static bool source_range_bounds(SourceContext *ctx, AstNode *node, SourceValue *start, SourceValue *end) {
    RangeNode *range = &node->as.for_in_stmt.collection->as.range;
    if (!range->start || !range->end) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"range requires both endpoints");
    SourceExpectedType expected = {true,XR_XIR_I64, false};
    if (!source_plan_expression(ctx,range->start,expected,start) || !source_plan_expression(ctx,range->end,expected,end)) return false;
    if (start->type != XR_XIR_I64 || end->type != XR_XIR_I64)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"range endpoints must be i64");
    return true;
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
    bool counting = syntax->collection && syntax->collection->type == AST_RANGE;
    bool inclusive = counting && syntax->collection->as.range.inclusive_end;
    if (counting && syntax->is_keyvalue)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"a range for-in binds one name");
    SourceValue array = {0}, bound, first;
    XrXirType element_type = XR_XIR_I64;
    if (counting) {
        if (!source_range_bounds(ctx,node,&first,&bound)) return false;
    } else {
        if (!expression(ctx,syntax->collection,&array)) return false;
        if (!xr_xir_type_is_array(&ctx->types,array.type))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"for-in requires a proved built-in Array value");
        element_type = xr_xir_array_element(&ctx->types,array.type);
        if (!source_native_array_declaration(ctx) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ARRAY_LEN,XR_XIR_I64,{array.id},{0},0,{0}},&bound) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},&first)) return false;
    }
    SourceCounter counter;
    if (!source_counter_open(ctx,first,bound,inclusive,&counter)) return false;
    SourceValue element = counter.index;
    if (!counting && !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ARRAY_GET,element_type,{array.id,counter.index.id},{0},0,{0}},&element)) return false;
    bool ok = source_for_in_body(ctx,node,counter.index,element);
    ctx->loop = counter.loop.parent;
    return ok && source_counter_close(ctx,&counter);
}
