/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_invoke.inc.c - Function-local explicit error continuations
 */
static bool source_error_edge(SourceContext *ctx, SourceValue value) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (!body->block_count && !begin_block(ctx)) return false;
    SourceErrorContext *handler = body->error_context;
    SourceErrorEdge *edge = source_alloc(ctx,1,sizeof(*edge));
    if (!edge || !handler || handler->count >= UINT32_MAX/2)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"error continuation budget exhausted");
    *edge = (SourceErrorEdge){handler->edges,body->block_count-1,body->count,value};
    handler->edges = edge; ++handler->count;
    bool leaving = body->frontier != handler->frontier;
    return emit_raw(ctx,(XrXirInstruction){leaving ? XR_XIR_CLEANUP_ERROR : XR_XIR_JUMP,
        XR_XIR_UNIT,{leaving ? value.id : 0},{0},leaving ? handler->frontier : 0,{0}},NULL);
}
static bool emit(SourceContext *ctx, XrXirInstruction op, SourceValue *result) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (!body->error_context || (op.op != XR_XIR_CALL && op.op != XR_XIR_CALL_INDIRECT))
        return emit_raw(ctx,op,result);
    if (!body->block_count && !begin_block(ctx)) return false;
    uint32_t origin = body->count, error = body->block_count;
    if (error == UINT32_MAX) return source_fail(ctx,NULL,XR_XIR_BUDGET,"invoke block identity exhausted");
    op.op = op.op == XR_XIR_CALL ? XR_XIR_INVOKE : XR_XIR_INVOKE_INDIRECT;
    op.targets[0] = error+1; op.targets[1] = error;
    if (!emit_raw(ctx,op,NULL) || !begin_block(ctx)) return false;
    SourceValue caught;
    if (!emit_raw(ctx,(XrXirInstruction){XR_XIR_INVOKE_ERROR,XR_XIR_ERROR,{0},{0},origin,{0}},&caught) ||
        !source_error_edge(ctx,caught) || !begin_block(ctx)) return false;
    if (op.type == XR_XIR_UNIT) {
        if (result) *result = (SourceValue){0,XR_XIR_UNIT};
        return true;
    }
    return emit_raw(ctx,(XrXirInstruction){XR_XIR_INVOKE_RESULT,op.type,{0},{0},origin,{0}},result);
}
