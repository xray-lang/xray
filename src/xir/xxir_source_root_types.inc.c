/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_root_types.inc.c - Owned callable identities on value flow
 */
static XrXirType source_root_operand(SourceContext *ctx, uint32_t function, uint32_t id) {
    return xr_xir_operand_type(&ctx->functions[function],id);
}
static bool source_root_set(SourceContext *ctx, uint32_t function, uint32_t instruction,
    XrXirType type, bool *changed) {
    XrXirInstruction *op = (XrXirInstruction *)ctx->functions[function].instructions + instruction;
    if (op->type != type) { op->type = type; *changed = true; }
    return true;
}
static bool source_root_join_type(SourceContext *ctx, XrXirType left,
    XrXirType right, XrXirType *output) {
    if (left == right) { *output = left; return true; }
    const XrXirTypeNode *a = xr_xir_callable_signature(&ctx->types,left);
    const XrXirTypeNode *b = xr_xir_callable_signature(&ctx->types,right);
    if (!a || !b || a->result != b->result || a->parameter_count != b->parameter_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"inferred callable flow requires exact nested identities");
    XrXirTypeNode joined = *a;
    if (!xr_xir_callable_flags_join(a->flags,b->flags,&joined.flags))
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"inferred callable flow has invalid bounds");
    for (uint32_t p = 0; p < a->parameter_count; ++p) {
        if (!source_work(ctx,NULL)) return false;
        if (a->parameters[p].type != b->parameters[p].type || a->parameters[p].mode != b->parameters[p].mode)
            return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"inferred callable parameters must remain exact");
    }
    return source_intern_type(ctx,joined,output);
}
static bool source_root_copy_bound(SourceContext *ctx, uint32_t f, uint32_t value,
    XrXirType bound, bool *changed) {
    const XrXirFunction *body = &ctx->functions[f];
    if (value < body->parameter_count || value - body->parameter_count >= body->instruction_count)
        return source_root_operand(ctx,f,value) == bound;
    uint32_t i = value - body->parameter_count;
    SourceRootValue *flow = &ctx->bodies[f].root_values[i];
    if (flow->kind != 1 && flow->kind != 2 && flow->kind != 4) return source_root_operand(ctx,f,value) == bound;
    XrXirInstruction *op = (XrXirInstruction *)body->instructions + i;
    if (op->op != XR_XIR_COPY && op->op != XR_XIR_FUNCTION_WEAKEN) return false;
    XrXirType actual = source_root_operand(ctx,f,op->args[0]);
    XrXirOp operation = XR_XIR_COPY;
    if (actual != bound || (flow->kind == 1 && xr_xir_callable_signature(&ctx->types,bound))) {
        XrXirStatus status = xr_xir_compile_callable_weakening(&ctx->compile,&ctx->types,actual,bound);
        if (status != XR_XIR_OK) return source_fail(ctx,NULL,status,"inferred value cannot satisfy its fixed or joined bound");
        operation = XR_XIR_FUNCTION_WEAKEN;
    }
    if (op->op != operation) { op->op = operation; *changed = true; }
    return source_root_set(ctx,f,i,bound,changed);
}
/* A join's private copy exposes its original input to the equation and receives
 * the common bound only after all predecessors have been considered. */
static uint32_t source_root_join_origin(SourceContext *ctx, uint32_t f, uint32_t id) {
    const XrXirFunction *body = &ctx->functions[f];
    if (id >= body->parameter_count && id - body->parameter_count < body->instruction_count &&
        (ctx->bodies[f].root_values[id - body->parameter_count].kind == 2 ||
        ctx->bodies[f].root_values[id - body->parameter_count].kind == 4)) {
        const XrXirInstruction *op = &body->instructions[id - body->parameter_count];
        if (op->op == XR_XIR_COPY || op->op == XR_XIR_FUNCTION_WEAKEN) return op->args[0];
    }
    return id;
}
static bool source_root_group(SourceContext *ctx, uint32_t f, uint32_t i, bool *changed) {
    const XrXirFunction *body = &ctx->functions[f];
    XrXirInstruction *op = (XrXirInstruction *)body->instructions + i;
    if (!op->args[1]) return true;
    uint32_t first = op->op == XR_XIR_PHI ? 1 : 0, step = op->op == XR_XIR_PHI ? 2 : 1;
    XrXirType common = source_root_operand(ctx,f,source_root_join_origin(ctx,f,body->operands[op->args[0] + first]));
    for (uint32_t a = first + step; a < op->args[1]; a += step) {
        if (!source_work(ctx,NULL) || !source_root_join_type(ctx,common,
            source_root_operand(ctx,f,source_root_join_origin(ctx,f,body->operands[op->args[0] + a])),&common)) return false;
    }
    for (uint32_t a = first; a < op->args[1]; a += step) {
        if (!source_work(ctx,NULL) || !source_root_copy_bound(ctx,f,body->operands[op->args[0] + a],common,changed))
            return source_fail(ctx,NULL,ctx->diagnostic.status == XR_XIR_OK ? XR_XIR_BAD_TYPE : ctx->diagnostic.status,
                "joined callable input has no admitted normalization");
    }
    XrXirType result = common;
    if (op->op == XR_XIR_ARRAY_NEW) {
        XrXirTypeNode array = *xr_xir_type_node(&ctx->types,op->type);
        array.element = common;
        if (!source_intern_type(ctx,array,&result)) return false;
    }
    return source_root_set(ctx,f,i,result,changed);
}
static bool source_root_cell(SourceContext *ctx, uint32_t f, uint32_t i, bool *changed) {
    const XrXirFunction *body = &ctx->functions[f];
    const XrXirInstruction *op = &body->instructions[i];
    uint32_t id = body->parameter_count + i;
    XrXirType common = source_root_operand(ctx,f,source_root_join_origin(ctx,f,op->args[0]));
    for (uint32_t a = 0; a < body->instruction_count; ++a) {
        if (!source_work(ctx,NULL)) return false;
        const XrXirInstruction *write = &body->instructions[a];
        if (write->op == XR_XIR_CELL_WRITE && write->args[0] == id &&
            !source_root_join_type(ctx,common,source_root_operand(ctx,f,
                source_root_join_origin(ctx,f,write->args[1])),&common)) return false;
    }
    if (!source_root_copy_bound(ctx,f,op->args[0],common,changed))
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"inferred cell initializer cannot share its bound");
    for (uint32_t a = 0; a < body->instruction_count; ++a) {
        if (!source_work(ctx,NULL)) return false;
        const XrXirInstruction *write = &body->instructions[a];
        if (write->op == XR_XIR_CELL_WRITE && write->args[0] == id &&
            !source_root_copy_bound(ctx,f,write->args[1],common,changed))
            return source_fail(ctx,NULL,ctx->diagnostic.status == XR_XIR_OK ? XR_XIR_BAD_TYPE : ctx->diagnostic.status,
                "inferred cell write cannot share its bound");
    }
    XrXirType result;
    return source_cell_type(ctx,common,&result) && source_root_set(ctx,f,i,result,changed);
}

/* Only an inferred capture receives a private bottom. Explicit callable
 * parameters remain declared upper bounds, including a fixed UNKNOWN. */
static bool source_root_bottom_type(SourceContext *ctx, XrXirType original,
    uint32_t depth, XrXirType *output) {
    const XrXirTypeNode *node = xr_xir_type_node(&ctx->types,original);
    *output = original;
    if (!node) return true;
    if (!source_work(ctx,NULL) || depth >= 128)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"inferred capture nesting exhausted");
    XrXirTypeNode bottom = *node;
    if (bottom.kind == XR_XIR_TYPE_CALLABLE) {
        bottom.flags = (bottom.flags & XR_XIR_CALLABLE_NO_SUSPEND) | XR_XIR_CALLABLE_ROOT_NONE;
    } else if (bottom.kind == XR_XIR_TYPE_CELL || bottom.kind == XR_XIR_TYPE_ARRAY || bottom.kind == XR_XIR_TYPE_NULLABLE) {
        if (!source_root_bottom_type(ctx,bottom.element,depth + 1,&bottom.element)) return false;
    } else if (bottom.kind == XR_XIR_TYPE_TUPLE) {
        XrXirCallableParameter *fields = bottom.parameter_count ? source_alloc(ctx,bottom.parameter_count,sizeof(*fields)) : NULL;
        if (bottom.parameter_count && !fields) return false;
        for (uint32_t p = 0; p < bottom.parameter_count; ++p) {
            fields[p] = bottom.parameters[p];
            if (!source_root_bottom_type(ctx,fields[p].type,depth + 1,&fields[p].type)) return false;
        }
        bottom.parameters = fields;
    } else return true;
    return source_intern_type(ctx,bottom,output);
}

static bool source_root_capture_join(SourceContext *ctx, XrXirType left,
    XrXirType right, uint32_t depth, XrXirType *output) {
    if (left == right || xr_xir_type_is_callable(&ctx->types,left))
        return source_root_join_type(ctx,left,right,output);
    const XrXirTypeNode *a = xr_xir_type_node(&ctx->types,left), *b = xr_xir_type_node(&ctx->types,right);
    if (!source_work(ctx,NULL) || depth >= 128)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"capture shape join exhausted");
    if (!a || !b || a->kind != b->kind || a->parameter_count != b->parameter_count ||
        (a->kind != XR_XIR_TYPE_CELL && a->kind != XR_XIR_TYPE_ARRAY &&
         a->kind != XR_XIR_TYPE_NULLABLE && a->kind != XR_XIR_TYPE_TUPLE))
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"capture value shapes cannot change");
    XrXirTypeNode joined = *a, right_shape = *b;
    if (joined.kind == XR_XIR_TYPE_TUPLE) {
        XrXirCallableParameter *fields = joined.parameter_count ? source_alloc(ctx,joined.parameter_count,sizeof(*fields)) : NULL;
        if (joined.parameter_count && !fields) return false;
        for (uint32_t p = 0; p < joined.parameter_count; ++p) {
            fields[p] = joined.parameters[p];
            if (!source_root_capture_join(ctx,fields[p].type,right_shape.parameters[p].type,depth + 1,&fields[p].type)) return false;
        }
        joined.parameters = fields;
    } else if (!source_root_capture_join(ctx,joined.element,right_shape.element,depth + 1,&joined.element)) return false;
    return source_intern_type(ctx,joined,output);
}
