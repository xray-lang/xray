/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_iteration.inc.c - Owned container snapshots and per-iteration bindings
 *
 * KEY CONCEPT:
 *   One evaluated logical container feeds the existing loop and cleanup machinery.
 */
static bool source_iteration_binding(SourceContext *ctx, AstNode *node,
    const char *name, SourceValue value) {
    if (source_text_same(ctx, NULL, name, "_")) return true;
    SourceName *symbol = source_alloc(ctx,1,sizeof(*symbol));
    if (!symbol) return false;
    *symbol = (SourceName){.next=ctx->locals,.name=name,.imported=NULL,.node=node,.kind=value.type==XR_XIR_UNIT ? SOURCE_UNIT_LOCAL : SOURCE_LOCAL,
            .index=value.id,.module=ctx->module,.type=value.type,.mutable=false,
            .construction=false,.declaration=0,.inferred=false,.checked_library=false};
    ctx->locals = symbol;
    if (!source_query_declare(ctx,symbol,XR_XIR_SOURCE_BINDING,
        ctx->bodies[ctx->function].declaration,source_query_range(ctx,node,name))) return false;
    if(!source_hir_value_declared(ctx,node,symbol,value))return false;
    source_query_binding_type(ctx,symbol);
    return true;
}
static bool source_for_in_body(SourceContext *ctx, AstNode *node,
    SourceValue index, SourceValue element) {
    ForInStmtNode *loop = &node->as.for_in_stmt;
    SourceName *saved = ctx->locals, *scope = ctx->scope;
    XrHirBodyScope typed_scope={0};if(!source_hir_scope_enter(ctx,node,&typed_scope))return false;
    ctx->scope = saved;
    bool ok = source_iteration_binding(ctx,node,loop->item_name,loop->is_keyvalue ? index : element) &&
        (!loop->is_keyvalue || source_iteration_binding(ctx,node,loop->value_name,element)) &&
        scoped_statement(ctx,loop->body);
    ctx->locals=saved;ctx->scope=scope;source_hir_scope_leave(ctx,typed_scope);
    return ok;
}
#include "xxir_source_iterator_loop.inc.c"
#include "xxir_source_iterable.inc.c"

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
static bool source_map_iteration_types(SourceContext *ctx, AstNode *node,
    SourceValue receiver, XrXirType *key, XrXirType *element) {
    const XrXirTypeNode *map=xr_xir_type_node(&ctx->types,receiver.type);
    if(!map || map->kind!=XR_XIR_TYPE_MAP || !node->as.for_in_stmt.is_keyvalue)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Map for-in requires a direct key/value binding");
    XrXirType first=map->element,second=map->result;
    const XrNativeTypeDeclaration *native=source_hash_native(ctx,receiver.type);
    uint32_t declaration=0;
    if(!native || native->id!=XR_NATIVE_DECLARATION_MAP)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Map iteration requires its governed declaration");
    if(!source_native_hash_declaration(ctx,native,&declaration)) return false;
    *key=first;*element=second;return true;
}
static bool source_map_iteration(SourceContext *ctx, AstNode *node, SourceValue receiver) {
    XrXirType key_type,element_type;
    SourceValue length,zero,key,element;
    if(!source_map_iteration_types(ctx,node,receiver,&key_type,&element_type) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_HASH_LEN,XR_XIR_I64,{receiver.id},{0},0,{0}},&length) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},&zero)) return false;
    SourceCounter counter;
    if(!source_counter_open(ctx,zero,length,false,&counter)) return false;
    bool ok=source_recipe_record(ctx,(XrXirInstruction){XR_XIR_HASH_KEY_AT,key_type,
        {receiver.id,counter.index.id},{0},0,{0}},&key) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_MAP_VALUE_AT,element_type,
        {receiver.id,counter.index.id},{0},0,{0}},&element) && source_for_in_body(ctx,node,key,element);
    ctx->loop=counter.loop.parent;
    return ok && source_counter_close(ctx,&counter);
}
/* `a..b` and `a..=b` count an i64 cursor; both endpoints are evaluated once, start first. */
static bool source_range_bounds(SourceContext *ctx, AstNode *node, SourceValue *start, SourceValue *end) {
    RangeNode *range = &node->as.for_in_stmt.collection->as.range;
    if (!range->start || !range->end) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"range requires both endpoints");
    SourceExpectedType expected = {true,XR_XIR_I64, false, false};
    if (!source_plan_expression(ctx,range->start,expected,start) || !source_plan_expression(ctx,range->end,expected,end)) return false;
    if (start->type != XR_XIR_I64 || end->type != XR_XIR_I64)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"range endpoints must be i64");
    return true;
}
static bool source_for_in(SourceContext *ctx, AstNode *node) {
    ForInStmtNode *syntax = &node->as.for_in_stmt;
    if (syntax->label || syntax->is_tuple_head || syntax->item_type)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,
            "for-in requires an unlabelled single or direct pair binding without annotation");
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
        if (!source_iteration_receiver(ctx,node,&array)) return false;
        const XrXirTypeNode *iter=xr_xir_type_node(&ctx->types,array.type);
        if(iter && iter->kind==XR_XIR_TYPE_ITERATOR) return source_iterator_loop(ctx,node,array);
        if(iter && iter->kind==XR_XIR_TYPE_MAP && syntax->is_keyvalue)
            return source_map_iteration(ctx,node,array);
        if (!xr_xir_type_is_array(&ctx->types,array.type)) {
            SourceValue iterator;
            return source_iteration_factory(ctx,node,array,&iterator) && source_iterator_loop(ctx,node,iterator);
        }
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
