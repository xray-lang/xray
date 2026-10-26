/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_binding_cell.inc.c - Stable mutable binding storage
 *
 * KEY CONCEPT:
 *   Source names keep logical mutability; shared views use existing SSA aliases,
 *   and mutable Cell operations require a real stored owner.
 */
static bool source_shared_byte_slice(SourceContext *ctx, XrXirType type) {
    const XrXirTypeNode *node = xr_xir_type_node(&ctx->types, type);
    return node && node->kind == XR_XIR_TYPE_SLICE && !node->flags && node->element == XR_XIR_U8;
}
static bool source_binding_shared_alias(SourceContext *ctx, const SourceName *symbol, bool *alias) {
    *alias = false;
    if (!symbol || symbol->kind != SOURCE_LOCAL || !symbol->mutable || symbol->construction ||
        !source_shared_byte_slice(ctx, symbol->type)) return true;
    uint32_t value = symbol->index;
    XrXirType physical;
    const XrXirInstruction *producer = NULL;
    if (!source_borrow_value(ctx, symbol->node, &value, &physical, &producer)) return false;
    if (physical != symbol->type)
        return source_fail(ctx, symbol->node, XR_XIR_BAD_TYPE, "borrowed local requires its shared SSA value");
    *alias = true;
    return true;
}
static bool source_binding_cell(SourceContext *ctx, const SourceName *symbol, SourceValue *cell) {
    if (!symbol || !symbol->mutable || symbol->construction)
        return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "mutable Cell owner required");
    bool alias = false;
    if (!source_binding_shared_alias(ctx, symbol, &alias)) return false;
    if (alias) return source_fail(ctx, symbol->node, XR_XIR_BAD_TYPE,
        "borrowed local rebinding and ref storage are not admitted");
    if (symbol->kind == SOURCE_LOCAL) {
        if (!source_cell_type(ctx, symbol->type, &cell->type)) return false;
        cell->id = symbol->index;
        return true;
    }
    if ((symbol->kind != SOURCE_SLOT && symbol->kind != SOURCE_UNIT_SLOT) ||
        symbol->index >= ctx->slot_count)
        return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "binding has no stored Cell owner");
    XrXirType physical = ctx->slots[symbol->index].type;
    if (!xr_xir_type_is_cell(&ctx->types, physical) ||
        xr_xir_cell_element(&ctx->types, physical) != symbol->type)
        return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "module Cell element differs from its logical binding");
    return source_recipe_record(ctx, (XrXirInstruction){XR_XIR_SLOT_LOAD, physical,
        {0}, {0}, symbol->index, {0}}, cell);
}
static bool source_binding_read(SourceContext *ctx, const SourceName *symbol, SourceValue *value) {
    if (symbol->kind == SOURCE_UNIT_LOCAL) {
        *value = (SourceValue){UINT32_MAX, XR_XIR_UNIT};
        return true;
    }
    if ((symbol->kind==SOURCE_SLOT || symbol->kind==SOURCE_UNIT_SLOT) &&
        symbol->module!=ctx->bodies[ctx->function].module && ctx->history)
        return source_history_binding_read(ctx,symbol,value);
    bool alias = false;
    if (!source_binding_shared_alias(ctx, symbol, &alias)) return false;
    if (symbol->mutable && !alias) {
        SourceValue cell = {0};
        return source_binding_cell(ctx, symbol, &cell) &&
            source_recipe_record(ctx, (XrXirInstruction){XR_XIR_CELL_READ, symbol->type,
                {cell.id, 0}, {0}, 0, {0}}, value);
    }
    if (symbol->kind == SOURCE_LOCAL) {
        *value = (SourceValue){symbol->index, symbol->type};
        return true;
    }
    return source_recipe_record(ctx, (XrXirInstruction){XR_XIR_SLOT_LOAD, symbol->type,
        {0}, {0}, symbol->index, {0}}, value);
}
static bool source_binding_write(SourceContext *ctx, const SourceName *symbol, SourceValue value) {
    if (symbol->kind == SOURCE_UNIT_LOCAL) return true;
    if (value.type != symbol->type)
        return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "binding write changes its logical type");
    if ((symbol->kind==SOURCE_SLOT || symbol->kind==SOURCE_UNIT_SLOT) &&
        symbol->module!=ctx->bodies[ctx->function].module && ctx->history)
        return source_history_binding_write(ctx,symbol,value);
    SourceValue cell = {0};
    return source_binding_cell(ctx, symbol, &cell) &&
        source_recipe_record(ctx, (XrXirInstruction){XR_XIR_CELL_WRITE, XR_XIR_UNIT,
            {cell.id, value.type == XR_XIR_UNIT ? 0 : value.id}, {0}, 0, {0}}, NULL);
}
static bool source_binding_prepare(SourceContext *ctx, SourceName *symbol, SourceValue *initial) {
    symbol->type = initial->type;
    if (symbol->mutable) {
        XrXirType physical;
        if (!source_cell_type(ctx, initial->type, &physical) ||
            !source_recipe_record(ctx, (XrXirInstruction){XR_XIR_CELL_NEW, physical,
                {initial->type == XR_XIR_UNIT ? 0 : initial->id, 0}, {0}, 0, {0}}, initial)) return false;
    }
    ctx->slots[symbol->index].type = initial->type;
    if (symbol->type == XR_XIR_UNIT) symbol->kind = SOURCE_UNIT_SLOT;
    source_query_binding_type(ctx, symbol);
    return true;
}
