/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_pattern_array_tail.inc.c - Independent tail Array construction
 */
static bool source_pattern_array_tail(SourceContext *ctx,AstNode *node,
    SourceValue array,SourceValue first,SourceValue *output) {
    XrXirType cell_type;SourceValue seed,store,candidate;
    if (!source_cell_type(ctx,array.type,&cell_type) ||
        !source_recipe_group(ctx,(XrXirInstruction){XR_XIR_ARRAY_NEW,array.type,{0},{0},0,{0}},NULL,0,&seed) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_NEW,cell_type,{seed.id},{0},0,{0}},&store) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_PLACE,array.type,{store.id},{0},0,{0}},&candidate) ||
        !source_array_append_snapshot(ctx,node,candidate,array,first)) return false;
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_READ,array.type,{store.id},{0},0,{0}},output);
}
