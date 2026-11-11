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
    return source_effect_value_refresh(ctx,function,instruction);
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
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (a->parameters[p].type != b->parameters[p].type || a->parameters[p].mode != b->parameters[p].mode || a->parameters[p].escaping != b->parameters[p].escaping)
            return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"inferred callable parameters must remain exact");
    }
    return source_intern_type(ctx,joined,output);
}
static bool source_root_copy_bound(SourceContext *ctx, uint32_t f, uint32_t value,
    XrXirType bound, bool *changed) {
    const XrXirFunction *body = &ctx->functions[f];
    if (ctx->bodies[f].checked_library) return source_root_operand(ctx,f,value)==bound;
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
    if (operation == XR_XIR_FUNCTION_WEAKEN) {
        const XrXirTypeNode *signature = xr_xir_callable_signature(&ctx->types,bound);
        XrXirStatus status = xir_effects_construction_weaken(&ctx->compile,ctx->root_refinement,f,i,
            signature->flags & (XR_XIR_CALLABLE_ROOT_REQUIRED | XR_XIR_CALLABLE_ROOT_UNRESOLVED),
            !(signature->flags & XR_XIR_CALLABLE_NO_SUSPEND));
        if (status != XR_XIR_OK) return source_fail(ctx,NULL,status,"callable conversion metadata is unavailable");
    }
    return source_root_set(ctx,f,i,bound,changed);
}
/* A join's private copy exposes its original input to the equation and receives
 * the common bound only after all predecessors have been considered. */
static uint32_t source_root_join_origin(SourceContext *ctx, uint32_t f, uint32_t id) {
    const XrXirFunction *body = &ctx->functions[f];
    if (ctx->bodies[f].checked_library) return id;
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
        if (!((ctx)->diagnostic.status == XR_XIR_OK) || !source_root_join_type(ctx,common,
            source_root_operand(ctx,f,source_root_join_origin(ctx,f,body->operands[op->args[0] + a])),&common)) return false;
    }
    for (uint32_t a = first; a < op->args[1]; a += step) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK) || !source_root_copy_bound(ctx,f,body->operands[op->args[0] + a],common,changed))
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
    XrXirType current = xr_xir_cell_element(&ctx->types,op->type);
    if (xr_xir_callable_signature(&ctx->types,current) &&
        !source_root_join_type(ctx,current,common,&common)) return false;
    for (uint32_t a = 0; a < body->instruction_count; ++a) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        const XrXirInstruction *write = &body->instructions[a];
        if (write->op == XR_XIR_CELL_WRITE && write->args[0] == id &&
            !source_root_join_type(ctx,common,source_root_operand(ctx,f,
                source_root_join_origin(ctx,f,write->args[1])),&common)) return false;
    }
    if (!source_root_copy_bound(ctx,f,op->args[0],common,changed))
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"inferred cell initializer cannot share its bound");
    for (uint32_t a = 0; a < body->instruction_count; ++a) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        const XrXirInstruction *write = &body->instructions[a];
        if (write->op == XR_XIR_CELL_WRITE && write->args[0] == id &&
            !source_root_copy_bound(ctx,f,write->args[1],common,changed))
            return source_fail(ctx,NULL,ctx->diagnostic.status == XR_XIR_OK ? XR_XIR_BAD_TYPE : ctx->diagnostic.status,
                "inferred cell write cannot share its bound");
    }
    XrXirType result;
    return source_cell_type(ctx,common,&result) && source_root_set(ctx,f,i,result,changed);
}

static bool source_root_cell_fixed(SourceContext *ctx, uint32_t function, uint32_t value) {
    const XrXirFunction *body = &ctx->functions[function];
    const SourceFunction *source = &ctx->bodies[function];
    if (source->checked_library || !source->root_values) return true;
    if (value < body->parameter_count)
        return value >= source->root_capture_count || !source->root_captures[value];
    uint8_t kind = source->root_values[value - body->parameter_count].kind;
    return kind == 1 || kind == 3;
}

static bool source_root_cell_error(SourceContext *ctx, uint32_t function, uint32_t value,
    XrXirStatus status, const char *message) {
    if (ctx->diagnostic.status != XR_XIR_OK) return false;
    SourceFunction *body = &ctx->bodies[function];
    ctx->function = function; ctx->module = body->module;
    source_fail(ctx,body->node,status,message);
    uint32_t parameters = ctx->functions[function].parameter_count;
    if (value >= parameters && value-parameters < ctx->functions[function].instruction_count && body->debug_points) {
        XrXirSourceRange range = body->debug_points[value-parameters].range;
        if (range.line > 0) {
            ctx->diagnostic.module = range.module;
            ctx->diagnostic.line = range.line; ctx->diagnostic.column = range.column;
        }
    }
    return false;
}

static bool source_root_cell_set(SourceContext *ctx, uint32_t function, uint32_t value,
    XrXirType type, bool *changed) {
    const XrXirFunction *body = &ctx->functions[function];
    if (source_root_operand(ctx,function,value) == type) return true;
    if (source_root_cell_fixed(ctx,function,value))
        return source_root_cell_error(ctx,function,value,XR_XIR_BAD_TYPE,"shared Cell type conflicts with its fixed input");
    if (value >= body->parameter_count) return source_root_set(ctx,function,value-body->parameter_count,type,changed);
    ((XrXirType *)body->parameters)[value] = type; *changed = true;
    return true;
}

static bool source_root_cell_write(SourceContext *ctx, uint32_t function, uint32_t instruction, bool *changed) {
    const XrXirFunction *body = &ctx->functions[function];
    const XrXirInstruction *write = &body->instructions[instruction];
    XrXirType cell = source_root_operand(ctx,function,write->args[0]);
    if (!xr_xir_type_is_cell(&ctx->types,cell)) return true;
    XrXirType common = xr_xir_cell_element(&ctx->types,cell);
    if (!xr_xir_callable_signature(&ctx->types,common)) return true;
    XrXirType value = source_root_operand(ctx,function,source_root_join_origin(ctx,function,write->args[1]));
    if (!source_root_cell_fixed(ctx,function,write->args[0]) &&
        !source_root_join_type(ctx,common,value,&common)) return false;
    XrXirType joined;
    if (!source_cell_type(ctx,common,&joined) || !source_root_cell_set(ctx,function,write->args[0],joined,changed)) return false;
    if (!source_root_copy_bound(ctx,function,write->args[1],common,changed))
        return source_fail(ctx,NULL,ctx->diagnostic.status == XR_XIR_OK ? XR_XIR_BAD_TYPE : ctx->diagnostic.status,
            "captured Cell write cannot share its callable bound");
    return true;
}

/* An inferred Cell and every captured handle share one invariant payload type.
 * Storage already owns their identity relation; only the advertised callable
 * bound is joined here, independently of reaching contents or permissions. */
static bool source_root_cell_identities(SourceContext *ctx, const XrXirEffects *effects, bool *changed) {
    XirStorageFlowView view = {0};
    XrXirStatus status = xir_storage_flow_view(xir_effects_prepared_storage(effects),&view);
    if (status != XR_XIR_OK) return source_fail(ctx,NULL,status,"shared Cell identities are unavailable");
    for (uint32_t e = 0; e < view.identity_count; ++e) {
        if (ctx->diagnostic.status != XR_XIR_OK) return false;
        XirStorageIdentity edge = view.identities[e];
        if (edge.source_function >= ctx->function_count || edge.target_function >= ctx->function_count ||
            edge.source >= view.functions[edge.source_function].physical_values ||
            edge.target >= view.functions[edge.target_function].physical_values)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"shared Cell identity left its physical owner");
        XrXirType left = source_root_operand(ctx,edge.source_function,edge.source);
        XrXirType right = source_root_operand(ctx,edge.target_function,edge.target);
        if (!xr_xir_type_is_cell(&ctx->types,left) || !xr_xir_type_is_cell(&ctx->types,right)) continue;
        XrXirType a = xr_xir_cell_element(&ctx->types,left), b = xr_xir_cell_element(&ctx->types,right);
        if (!xr_xir_callable_signature(&ctx->types,a) || !xr_xir_callable_signature(&ctx->types,b)) continue;
        bool fixed_left = source_root_cell_fixed(ctx,edge.source_function,edge.source);
        bool fixed_right = source_root_cell_fixed(ctx,edge.target_function,edge.target);
        XrXirType common;
        if (fixed_left || fixed_right) {
            common = fixed_left ? a : b;
            status = xr_xir_compile_callable_weakening(&ctx->compile,&ctx->types,fixed_left ? b : a,common);
            if (status != XR_XIR_OK) return source_root_cell_error(ctx,
                fixed_left ? edge.source_function : edge.target_function, fixed_left ? edge.source : edge.target,
                status,"shared Cell cannot satisfy its fixed callable bound");
        } else if (!source_root_join_type(ctx,a,b,&common)) return false;
        XrXirType cell;
        if (!source_cell_type(ctx,common,&cell) ||
            !source_root_cell_set(ctx,edge.source_function,edge.source,cell,changed) ||
            !source_root_cell_set(ctx,edge.target_function,edge.target,cell,changed)) return false;
    }
    return true;
}

/* Only an inferred capture receives a private bottom. Explicit callable
 * parameters remain declared upper bounds, including a fixed UNKNOWN. */
static bool source_root_bottom_type(SourceContext *ctx, XrXirType original,
    uint32_t depth, XrXirType *output) {
    const XrXirTypeNode *node = xr_xir_type_node(&ctx->types,original);
    *output = original;
    if (!node) return true;
    if (!((ctx)->diagnostic.status == XR_XIR_OK) || depth >= 128)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"inferred capture nesting exhausted");
    XrXirTypeNode bottom = *node;
    if (bottom.kind == XR_XIR_TYPE_CALLABLE) {
        bottom.flags = (bottom.flags & (XR_XIR_CALLABLE_NO_SUSPEND | XR_XIR_CALLABLE_REST |
            XR_XIR_CALLABLE_C_ABI)) | XR_XIR_CALLABLE_ROOT_NONE;
    } else if (bottom.kind == XR_XIR_TYPE_MAP) {
        if (!source_root_bottom_type(ctx,bottom.element,depth + 1,&bottom.element) ||
            !source_root_bottom_type(ctx,bottom.result,depth + 1,&bottom.result)) return false;
    } else if (bottom.kind == XR_XIR_TYPE_CELL || bottom.kind == XR_XIR_TYPE_SET ||
               bottom.kind == XR_XIR_TYPE_ARRAY || bottom.kind == XR_XIR_TYPE_NULLABLE) {
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
    if (!((ctx)->diagnostic.status == XR_XIR_OK) || depth >= 128)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"capture shape join exhausted");
    if (!a || !b || a->kind != b->kind || a->parameter_count != b->parameter_count ||
        (a->kind != XR_XIR_TYPE_CELL && a->kind != XR_XIR_TYPE_ARRAY &&
         a->kind != XR_XIR_TYPE_NULLABLE && a->kind != XR_XIR_TYPE_TUPLE &&
         a->kind != XR_XIR_TYPE_MAP && a->kind != XR_XIR_TYPE_SET))
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
    } else {
        if (!source_root_capture_join(ctx,joined.element,right_shape.element,depth + 1,&joined.element)) return false;
        if (joined.kind == XR_XIR_TYPE_MAP &&
            !source_root_capture_join(ctx,joined.result,right_shape.result,depth + 1,&joined.result)) return false;
    }
    return source_intern_type(ctx,joined,output);
}
