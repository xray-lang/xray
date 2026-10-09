/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_root_slots.inc.c - Root storage shares inferred value identities
 */
static uint32_t source_root_slot_owner(SourceContext *ctx, uint32_t function, uint32_t value) {
    const XrXirFunction *body = &ctx->functions[function];
    for (uint32_t step = 0; step < body->instruction_count; ++step) {
        if (!source_work(ctx,NULL)) return UINT32_MAX;
        if (value < body->parameter_count || value - body->parameter_count >= body->instruction_count)
            return UINT32_MAX;
        const XrXirInstruction *op = &body->instructions[value - body->parameter_count];
        if (op->op == XR_XIR_SLOT_LOAD && op->immediate >= 0 && (uint64_t)op->immediate < ctx->slot_count)
            return (uint32_t)op->immediate;
        if (op->op != XR_XIR_COPY) return UINT32_MAX;
        value = op->args[0];
    }
    return UINT32_MAX;
}
static bool source_root_slot_initial(SourceContext *ctx, uint32_t function,
    uint32_t slot, uint32_t owner, uint32_t *value) {
    *value = owner;
    if (!ctx->slots[slot].mutable) return true;
    const XrXirFunction *body = &ctx->functions[function];
    for (uint32_t step = 0; step < body->instruction_count; ++step) {
        if (!source_work(ctx,NULL)) return false;
        if (owner < body->parameter_count || owner - body->parameter_count >= body->instruction_count) break;
        const XrXirInstruction *op = &body->instructions[owner - body->parameter_count];
        if (op->op == XR_XIR_CELL_NEW) { *value = op->args[0]; return true; }
        if (op->op != XR_XIR_COPY) break;
        owner = op->args[0];
    }
    return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"inferred module binding lost its Cell initializer");
}
static bool source_root_slot_link(SourceContext *ctx, SourceRootFlow *flow,
    uint32_t function, uint32_t slot, uint32_t value, uint32_t *at) {
    if (!source_work(ctx,NULL)) return false;
    flow->slots[*at] = (SourceRootSlotEdge){function,value,flow->slot_heads[slot]};
    flow->slot_heads[slot] = (*at)++;
    const XrXirFunction *body = &ctx->functions[function];
    if (ctx->bodies[function].root_values && value >= body->parameter_count &&
        value - body->parameter_count < body->instruction_count &&
        ctx->bodies[function].root_values[value - body->parameter_count].kind == 2)
        ctx->bodies[function].root_values[value - body->parameter_count].kind = 4;
    return true;
}
static bool source_root_slots_prepare(SourceContext *ctx) {
    SourceRootFlow *flow = ctx->root_flow;
    if (!ctx->slot_count) return true;
    uint64_t count = 0;
    for (uint32_t f = 0; f < ctx->function_count; ++f)
        for (uint32_t i = 0; i < ctx->functions[f].instruction_count; ++i) {
            if (!source_work(ctx,NULL)) return false;
            const XrXirInstruction *op = &ctx->functions[f].instructions[i];
            if (op->op == XR_XIR_SLOT_INIT || op->op == XR_XIR_CELL_WRITE) ++count;
            else if (op->op == XR_XIR_SLOT_GROUP_INIT) count += op->args[1];
        }
    if (count > UINT32_MAX) return source_fail(ctx,NULL,XR_XIR_BUDGET,"slot value flow count exhausted");
    flow->slot_heads = source_alloc(ctx,ctx->slot_count,sizeof(*flow->slot_heads));
    flow->slot_names = source_alloc(ctx,ctx->slot_count,sizeof(*flow->slot_names));
    flow->slots = count ? source_alloc(ctx,(size_t)count,sizeof(*flow->slots)) : NULL;
    if (!flow->slot_heads || !flow->slot_names || (count && !flow->slots)) return false;
    for (uint32_t slot = 0; slot < ctx->slot_count; ++slot) flow->slot_heads[slot] = UINT32_MAX;
    /* Internal Checked core modules have no source namespace. The name table
     * is allocated for the source graph, independently of artifact modules. */
    for (uint32_t m = 0; m < (uint32_t)ctx->graph->spec_count; ++m)
        for (SourceName *name = ctx->names[m]; name; name = name->next) {
            if (!source_work(ctx,NULL)) return false;
            if ((name->kind == SOURCE_SLOT || name->kind == SOURCE_UNIT_SLOT) && name->index < ctx->slot_count)
                flow->slot_names[name->index] = name;
        }
    uint32_t at = 0;
    for (uint32_t f = 0; f < ctx->function_count; ++f) {
        const XrXirFunction *body = &ctx->functions[f];
        for (uint32_t i = 0; i < body->instruction_count; ++i) {
            if (!source_work(ctx,NULL)) return false;
            const XrXirInstruction *op = &body->instructions[i];
            if (op->op == XR_XIR_SLOT_INIT) {
                uint32_t slot = (uint32_t)op->immediate;
                SourceName *name = flow->slot_names[slot];
                if (!name || !name->inferred || !source_root_type(ctx,name->type,0)) continue;
                uint32_t value;
                if (!source_root_slot_initial(ctx,f,slot,op->args[0],&value) ||
                    !source_root_slot_link(ctx,flow,f,slot,value,&at)) return false;
            } else if (op->op == XR_XIR_CELL_WRITE) {
                uint32_t slot = source_root_slot_owner(ctx,f,op->args[0]);
                if (ctx->diagnostic.status != XR_XIR_OK) return false;
                if (slot == UINT32_MAX) continue;
                SourceName *name = flow->slot_names[slot];
                if (name && name->inferred && source_root_type(ctx,name->type,0) &&
                    !source_root_slot_link(ctx,flow,f,slot,op->args[1],&at)) return false;
            } else if (op->op == XR_XIR_SLOT_GROUP_INIT) {
                uint32_t slot = (uint32_t)((uint64_t)op->immediate >> 32), payload = 0;
                uint32_t bindings = (uint32_t)op->immediate;
                for (uint32_t s = 0; s < bindings; ++s) {
                    if (ctx->slots[slot + s].type == XR_XIR_UNIT) continue;
                    uint32_t owner = body->operands[op->args[0] + payload++];
                    SourceName *name = flow->slot_names[slot + s];
                    if (!name || !name->inferred || !source_root_type(ctx,name->type,0)) continue;
                    uint32_t value;
                    if (!source_root_slot_initial(ctx,f,slot + s,owner,&value) ||
                        !source_root_slot_link(ctx,flow,f,slot + s,value,&at)) return false;
                }
            }
        }
    }
    return true;
}
static bool source_root_slots(SourceContext *ctx, bool *changed) {
    SourceRootFlow *flow = ctx->root_flow;
    for (uint32_t slot = 0; slot < ctx->slot_count; ++slot) {
        if (!source_work(ctx,NULL)) return false;
        SourceName *name = flow->slot_names[slot];
        if (!name || !name->inferred || !source_root_type(ctx,name->type,0)) continue;
        XrXirType common = XR_XIR_UNIT;
        for (uint32_t e = flow->slot_heads[slot]; e != UINT32_MAX; e = flow->slots[e].next) {
            if (!source_work(ctx,NULL)) return false;
            SourceRootSlotEdge edge = flow->slots[e];
            XrXirType type = source_root_operand(ctx,edge.function,source_root_join_origin(ctx,edge.function,edge.value));
            if (common == XR_XIR_UNIT) common = type;
            else if (!source_root_join_type(ctx,common,type,&common)) return false;
        }
        if (common == XR_XIR_UNIT) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"inferred slot has no initializer identity");
        for (uint32_t e = flow->slot_heads[slot]; e != UINT32_MAX; e = flow->slots[e].next) {
            if (!source_work(ctx,NULL)) return false;
            SourceRootSlotEdge edge = flow->slots[e];
            if (!source_root_copy_bound(ctx,edge.function,edge.value,common,changed))
                return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"slot input cannot share its inferred bound");
        }
        XrXirType physical = common;
        if (ctx->slots[slot].mutable && !source_cell_type(ctx,common,&physical)) return false;
        if (ctx->slots[slot].type != physical || name->type != common) {
            ctx->slots[slot].type = physical; name->type = common; *changed = true;
        }
        if (name->declaration)
            ((XrXirSourceDeclaration *)ctx->query.declarations)[name->declaration - 1].type = source_query_type(ctx,common);
    }
    return ctx->diagnostic.status == XR_XIR_OK;
}
