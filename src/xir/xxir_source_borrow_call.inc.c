/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_borrow_call.inc.c - Known exclusive roots before call emission
 *
 * KEY CONCEPT:
 *   A callback retaining the same mutable Cell conflicts with a whole-root ref.
 *   Unknown dynamic identities remain subject to the common runtime guard.
 */
typedef struct SourceBorrowOrigin { uint32_t kind, identity; } SourceBorrowOrigin;

static bool source_borrow_value(SourceContext *ctx, AstNode *node, uint32_t *value,
    XrXirType *type, const XrXirInstruction **producer) {
    const SourceFunction *body = &ctx->bodies[ctx->function];
    const XrXirFunction *function = &ctx->functions[ctx->function];
    for (;;) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (*value < function->parameter_count) {
            *type = function->parameters[*value]; *producer = NULL; return true;
        }
        uint32_t index = *value - function->parameter_count;
        if (index >= body->count)
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"call borrow value has no source producer");
        const XrXirInstruction *op = &body->recipes[index].instruction;
        if (op->op != XR_XIR_COPY && op->op != XR_XIR_OWNED_RETAIN && op->op != XR_XIR_FUNCTION_WEAKEN) {
            *type = op->type; *producer = op; return true;
        }
        if (op->args[0] >= *value)
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"call borrow alias is not an earlier source value");
        *value = op->args[0];
    }
}

static bool source_borrow_origin(SourceContext *ctx, AstNode *node, uint32_t value,
    SourceBorrowOrigin *origin) {
    *origin = (SourceBorrowOrigin){0};
    const XrXirInstruction *op = NULL; XrXirType type;
    if (!source_borrow_value(ctx,node,&value,&type,&op)) return false;
    if (!xr_xir_type_is_cell(&ctx->types,type)) return true;
    /* Projected identities require their selector overlap proof, not just a
     * common owner. Keep disjoint fields and finite keys out of this rule. */
    if (op && op->op == XR_XIR_CELL_PROJECT) return true;
    if (op && op->op == XR_XIR_SLOT_LOAD) {
        *origin = (SourceBorrowOrigin){2,(uint32_t)op->immediate}; return true;
    }
    *origin = (SourceBorrowOrigin){1,value}; return true;
}

static bool source_borrow_callback(SourceContext *ctx, AstNode *node,
    SourceBorrowOrigin reference, uint32_t callback, bool *conflict) {
    *conflict = false;
    const XrXirInstruction *op = NULL; XrXirType type;
    if (!source_borrow_value(ctx,node,&callback,&type,&op)) return false;
    if (!op || op->op != XR_XIR_FUNCTION_REF) return true;
    const SourceFunction *body = &ctx->bodies[ctx->function];
    if (op->args[0] > body->operand_count || op->args[1] > body->operand_count - op->args[0])
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"callable capture operands exceed their source owner");
    for (uint32_t c = 0; c < op->args[1]; ++c) {
        SourceBorrowOrigin capture;
        if (!source_borrow_origin(ctx,node,body->operands[op->args[0] + c],&capture)) return false;
        if (reference.kind == capture.kind && reference.identity == capture.identity) {
            *conflict = true; return true;
        }
    }
    return true;
}

static bool source_call_borrows(SourceContext *ctx, const XrXirInstruction *call) {
    if (call->op != XR_XIR_CALL && call->op != XR_XIR_CALL_INDIRECT) return true;
    const SourceFunction *body = &ctx->bodies[ctx->function];
    AstNode *node = ctx->active_expression ? ctx->active_expression->syntax : body->node;
    uint32_t first = call->args[0], count = call->args[1];
    if (first > body->operand_count || count > body->operand_count - first)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"call operands exceed their source owner");
    for (uint32_t a = 0; a < count; ++a) {
        SourceBorrowOrigin reference;
        if (!source_borrow_origin(ctx,node,body->operands[first + a],&reference)) return false;
        if (!reference.kind) continue;
        for (uint32_t c = 0; c < count; ++c) {
            bool conflict;
            if (!source_borrow_callback(ctx,node,reference,body->operands[first + c],&conflict)) return false;
            if (conflict) return source_fail(ctx,node,XR_XIR_BAD_TYPE,
                "ref argument conflicts with a callable capture of the same mutable root");
        }
        if (call->op == XR_XIR_CALL_INDIRECT) {
            bool conflict;
            if (!source_borrow_callback(ctx,node,reference,(uint32_t)call->immediate,&conflict)) return false;
            if (conflict) return source_fail(ctx,node,XR_XIR_BAD_TYPE,
                "ref argument conflicts with a callable capture of the same mutable root");
        }
    }
    return true;
}
