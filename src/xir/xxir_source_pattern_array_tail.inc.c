/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_pattern_array_tail.inc.c - Independent tail Array construction
 */
static bool source_array_append_snapshot(SourceContext *ctx,
    SourceValue candidate, SourceValue array, SourceValue zero) {
    XrXirType element = xr_xir_array_element(&ctx->types,array.type);
    SourceValue length, payload;
    if (!((ctx)->diagnostic.status == XR_XIR_OK) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ARRAY_LEN,XR_XIR_I64,
            {array.id},{0},0,{0}},&length)) return false;
    SourceCounter counter;
    return source_counter_open(ctx,zero,length,false,&counter) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ARRAY_GET,element,
            {array.id,counter.index.id},{0},0,{0}},&payload) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ARRAY_PUSH,XR_XIR_UNIT,
            {candidate.id,payload.id},{0},0,{0}},NULL) &&
        source_counter_close(ctx,&counter);
}

static bool source_pattern_array_tail(SourceContext *ctx,
    SourceValue array,SourceValue first,SourceValue *output) {
    XrXirType cell_type;SourceValue seed,store,candidate;
    if (!source_cell_type(ctx,array.type,&cell_type) ||
        !source_recipe_group(ctx,(XrXirInstruction){XR_XIR_ARRAY_NEW,array.type,{0},{0},0,{0}},NULL,0,&seed) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_NEW,cell_type,{seed.id},{0},0,{0}},&store) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_PLACE,array.type,{store.id},{0},0,{0}},&candidate) ||
        !source_array_append_snapshot(ctx,candidate,array,first)) return false;
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_READ,array.type,{store.id},{0},0,{0}},output);
}
