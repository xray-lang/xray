/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_root_refine.inc.c - Joint value and root closure on prepared XIR
 *
 * KEY CONCEPT:
 *   Only owned Source value-flow identities select refinement. A fixed callable
 *   stays fixed, and no unclosed graph fact is published as permission.
 */
#include "xxir_source_root_types.inc.c"
/* Nested identities cannot weaken. A fixed composite freezes its construction
 * dependencies; independent direct reference sites can still retain precision. */
static bool source_root_lock(SourceContext *ctx, uint32_t f) {
    const XrXirFunction *body = &ctx->functions[f];
    SourceRootValue *flow = ctx->bodies[f].root_values;
    bool changed = true;
    for (uint32_t i = 0; i < body->instruction_count; ++i) {
        if (!source_work(ctx,NULL)) return false;
        const XrXirInstruction *op = &body->instructions[i];
        if (flow[i].kind == 1 && !xr_xir_type_is_callable(&ctx->types,op->type) && source_root_type(ctx,op->type,0))
            flow[i].kind = 3;
        if (op->op == XR_XIR_ARRAY_SET || op->op == XR_XIR_ARRAY_PUSH) {
            bool table = xr_xir_op_uses_operand_table(op->op);
            uint32_t path = table ? body->operands[op->args[0]] : op->args[0];
            if (source_root_type(ctx,source_root_operand(ctx,f,path),0)) {
                uint32_t count = table ? op->args[1] : 2;
                for (uint32_t a = 0; a < count; ++a) {
                    uint32_t input = table ? body->operands[op->args[0] + a] : op->args[a];
                    if (input >= body->parameter_count && input - body->parameter_count < body->instruction_count)
                        flow[input - body->parameter_count].kind = 3;
                }
            }
        }
    }
    while (changed) {
        changed = false;
        for (uint32_t i = 0; i < body->instruction_count; ++i) {
            if (!source_work(ctx,NULL)) return false;
            if (flow[i].kind != 3) continue;
            const XrXirInstruction *op = &body->instructions[i];
            bool table = xr_xir_op_uses_operand_table(op->op);
            uint32_t count = table ? op->args[1] : source_recipe_roles[op->op].values;
            if (op->op == XR_XIR_RETURN) count = body->result != XR_XIR_UNIT;
            for (uint32_t a = 0; a < count; ++a) {
                if (!source_work(ctx,NULL)) return false;
                if (op->op == XR_XIR_PHI && !(a % 2)) continue;
                uint32_t input = table ? body->operands[op->args[0] + a] : op->args[a];
                if (input >= body->parameter_count && input - body->parameter_count < body->instruction_count &&
                    flow[input - body->parameter_count].kind != 3) {
                    flow[input - body->parameter_count].kind = 3; changed = true;
                }
            }
        }
    }
    return ctx->diagnostic.status == XR_XIR_OK;
}
static bool source_root_reference(SourceContext *ctx, uint32_t f, uint32_t i,
    const XrXirEffects *effects, bool *changed) {
    const XrXirInstruction *op = &ctx->functions[f].instructions[i];
    uint32_t target = (uint32_t)op->immediate;
    const XrXirRootEffects *facts = xr_xir_effects_root(effects,target);
    XrXirRootEffects contextual={0};
    XrXirDeclarations declarations;XrXirModule view=source_module_view(ctx,&declarations);
    XrXirStatus status=xir_effects_context_refresh(&ctx->compile,&view,effects);
    if (status!=XR_XIR_OK) return source_fail(ctx,NULL,status,"context generation rebuild failed");
    if (xir_effects_context_available(&ctx->compile,effects) &&
        (!ctx->has_generics || !ctx->generics[f].parameter_count)) {
        uint32_t mask=0;
        status=xir_effects_reference_root(&ctx->compile,effects,&view,f,i,&mask);
        if (status==XR_XIR_OK) {
            contextual=(XrXirRootEffects){!!(mask&XR_XIR_CALLABLE_ROOT_REQUIRED),
                !!(mask&XR_XIR_CALLABLE_ROOT_UNRESOLVED)};facts=&contextual;
        } else return source_fail(ctx,NULL,status,"authentic reference context is unavailable");
    }
    const XrXirTypeNode *signature = xr_xir_callable_signature(&ctx->types,op->type);
    if (!facts || !signature) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"inferred reference lost its authentic target");
    XrXirTypeNode refined = *signature;
    refined.flags &= XR_XIR_CALLABLE_NO_SUSPEND;
    refined.flags |= facts->requires_root ? XR_XIR_CALLABLE_ROOT_REQUIRED : 0;
    refined.flags |= facts->unresolved ? XR_XIR_CALLABLE_ROOT_UNRESOLVED : 0;
    if (!(refined.flags & XR_XIR_CALLABLE_ROOT_MASK)) refined.flags |= XR_XIR_CALLABLE_ROOT_NONE;
    if (ctx->bodies[target].infer_result && (!ctx->has_generics || !ctx->generics[target].parameter_count))
        refined.result = ctx->functions[target].result;
    XrXirType type;
    return source_intern_type(ctx,refined,&type) && source_root_set(ctx,f,i,type,changed);
}
static bool source_root_tuple(SourceContext *ctx, uint32_t f, uint32_t i, bool *changed) {
    const XrXirFunction *body = &ctx->functions[f];
    const XrXirInstruction *op = &body->instructions[i];
    XrXirTypeNode tuple = *xr_xir_tuple_signature(&ctx->types,op->type);
    XrXirCallableParameter *fields = tuple.parameter_count ? source_alloc(ctx,tuple.parameter_count,sizeof(*fields)) : NULL;
    if (tuple.parameter_count && !fields) return false;
    uint32_t a = 0; bool differs = false;
    for (uint32_t p = 0; p < tuple.parameter_count; ++p) {
        if (!source_work(ctx,NULL)) return false;
        fields[p] = tuple.parameters[p];
        if (fields[p].type != XR_XIR_UNIT) fields[p].type = source_root_operand(ctx,f,body->operands[op->args[0] + a++]);
        if (fields[p].type != tuple.parameters[p].type) differs = true;
    }
    if (!differs) return true;
    tuple.parameters = fields; XrXirType type;
    return source_intern_type(ctx,tuple,&type) && source_root_set(ctx,f,i,type,changed);
}
static bool source_root_instruction(SourceContext *ctx, uint32_t f, uint32_t i,
    const XrXirEffects *effects, bool *changed) {
    const XrXirFunction *body = &ctx->functions[f];
    XrXirInstruction *op = (XrXirInstruction *)body->instructions + i;
    SourceRootValue *flow = &ctx->bodies[f].root_values[i];
    if (flow->kind == 3 || flow->kind == 4) return true;
    if (!source_root_type(ctx,flow->declared,0)) return ctx->diagnostic.status == XR_XIR_OK;
    if (op->op == XR_XIR_FUNCTION_REF) return source_root_reference(ctx,f,i,effects,changed);
    if (flow->kind == 1) {
        if (op->op == XR_XIR_COPY || op->op == XR_XIR_FUNCTION_WEAKEN)
            return source_root_copy_bound(ctx,f,body->parameter_count + i,flow->declared,changed);
        return true;
    }
    XrXirType type = op->type;
    switch (op->op) {
    case XR_XIR_COPY: case XR_XIR_FUNCTION_WEAKEN:
        if (flow->kind == 2 && flow->peer != UINT32_MAX) return true;
        type = source_root_operand(ctx,f,op->args[0]);
        if (flow->kind == 2 && op->op != XR_XIR_COPY) { op->op = XR_XIR_COPY; *changed = true; }
        break;
    case XR_XIR_PHI: case XR_XIR_ARRAY_NEW:
        return source_root_group(ctx,f,i,changed);
    case XR_XIR_CELL_NEW: return source_root_cell(ctx,f,i,changed);
    case XR_XIR_CELL_READ: case XR_XIR_CELL_PLACE:
        type = xr_xir_cell_element(&ctx->types,source_root_operand(ctx,f,op->args[0])); break;
    case XR_XIR_NULLABLE_SOME: {
        XrXirTypeNode nullable = *xr_xir_type_node(&ctx->types,op->type);
        nullable.element = source_root_operand(ctx,f,op->args[0]);
        if (!source_intern_type(ctx,nullable,&type)) return false; break;
    }
    case XR_XIR_NULLABLE_UNWRAP:
        type = xr_xir_nullable_element(&ctx->types,source_root_operand(ctx,f,op->args[0])); break;
    case XR_XIR_SLOT_LOAD: case XR_XIR_SLOT_PLACE:
        type = ctx->slots[op->immediate].type; break;
    case XR_XIR_PLACE_READ:
        type = source_root_operand(ctx,f,op->args[0]); break;
    case XR_XIR_ARRAY_RESERVE:
        type = source_root_operand(ctx,f,op->args[0]); break;
    case XR_XIR_ARRAY_GET:
        type = xr_xir_array_element(&ctx->types,source_root_operand(ctx,f,op->args[0])); break;
    case XR_XIR_TUPLE_NEW: return source_root_tuple(ctx,f,i,changed);
    case XR_XIR_TUPLE_FIELD: {
        const XrXirTypeNode *tuple = xr_xir_tuple_signature(&ctx->types,source_root_operand(ctx,f,op->args[0]));
        if (!tuple || (uint64_t)op->immediate >= tuple->parameter_count) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"tuple flow lost its field");
        type = tuple->parameters[op->immediate].type; break;
    }
    case XR_XIR_CALL_INDIRECT: case XR_XIR_INVOKE_INDIRECT: {
        const XrXirTypeNode *signature = xr_xir_callable_signature(&ctx->types,source_root_operand(ctx,f,(uint32_t)op->immediate));
        if (!signature) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"callable flow lost its result");
        type = signature->result; break;
    }
    case XR_XIR_INVOKE_RESULT: type = body->instructions[op->immediate].type; break;
    default: return true;
    }
    return source_root_set(ctx,f,i,type,changed);
}
static bool source_root_results(SourceContext *ctx, uint32_t f, bool *changed) {
    XrXirFunction *body = &ctx->functions[f];
    if (!ctx->bodies[f].infer_result || body->result == XR_XIR_UNIT) return true;
    XrXirType result = XR_XIR_UNIT;
    for (uint32_t i = 0; i < body->instruction_count; ++i) {
        if (!source_work(ctx,NULL)) return false;
        const XrXirInstruction *op = &body->instructions[i];
        if (op->op != XR_XIR_RETURN) continue;
        XrXirType type = source_root_operand(ctx,f,op->args[0]);
        if (result == XR_XIR_UNIT) result = type;
        else if (!source_root_join_type(ctx,result,type,&result)) return false;
    }
    if (body->result != result) { body->result = result; *changed = true; }
    return true;
}
/* Captured values belong to this exact closure construction. Ordinary explicit
 * parameters have no such edge and therefore keep their declared upper bound. */
typedef struct SourceRootCaptureEdge { uint32_t function, instruction, next; } SourceRootCaptureEdge;
typedef struct SourceRootSlotEdge { uint32_t function, value, next; } SourceRootSlotEdge;
struct SourceRootFlow {
    uint32_t *capture_heads;
    SourceRootCaptureEdge *captures;
    uint32_t *slot_heads;
    SourceRootSlotEdge *slots;
    SourceName **slot_names;
};
#include "xxir_source_root_slots.inc.c"
static bool source_root_value_bottom(SourceContext *ctx, uint32_t f, uint32_t i) {
    SourceRootValue *flow = &ctx->bodies[f].root_values[i];
    if (flow->kind == 1 || flow->kind == 3 || !source_root_type(ctx,flow->declared,0))
        return ctx->diagnostic.status == XR_XIR_OK;
    XrXirInstruction *op = (XrXirInstruction *)ctx->functions[f].instructions + i;
    switch (op->op) {
    case XR_XIR_FUNCTION_REF: case XR_XIR_COPY: case XR_XIR_FUNCTION_WEAKEN:
    case XR_XIR_PHI: case XR_XIR_CELL_NEW: case XR_XIR_CELL_READ: case XR_XIR_CELL_PLACE:
    case XR_XIR_NULLABLE_SOME: case XR_XIR_NULLABLE_UNWRAP:
    case XR_XIR_TUPLE_NEW: case XR_XIR_TUPLE_FIELD: case XR_XIR_ARRAY_NEW: case XR_XIR_ARRAY_GET:
    case XR_XIR_SLOT_LOAD: case XR_XIR_SLOT_PLACE: case XR_XIR_PLACE_READ: case XR_XIR_ARRAY_RESERVE:
        return source_root_bottom_type(ctx,flow->declared,0,&op->type);
    default: return true;
    }
}
static bool source_root_prepare(SourceContext *ctx) {
    if (ctx->root_flow) return true;
    SourceRootFlow *flow = source_alloc(ctx,1,sizeof(*flow));
    if (!flow) return false;
    uint32_t count = 0;
    for (uint32_t f = 0; f < ctx->function_count; ++f) {
        if (ctx->bodies[f].root_values && !source_root_lock(ctx,f)) return false;
        for (uint32_t i = 0; i < ctx->functions[f].instruction_count; ++i) {
            if (!source_work(ctx,NULL)) return false;
            if (ctx->bodies[f].root_values && !source_root_value_bottom(ctx,f,i)) return false;
            const XrXirInstruction *op = &ctx->functions[f].instructions[i];
            if ((op->op == XR_XIR_FUNCTION_REF || op->op==XR_XIR_CLEANUP_REGISTER) && op->args[1]) {
                if (count == UINT32_MAX) return source_fail(ctx,NULL,XR_XIR_BUDGET,"capture flow count exhausted");
                ++count;
            }
        }
    }
    flow->capture_heads = source_alloc(ctx,ctx->function_count,sizeof(*flow->capture_heads));
    flow->captures = count ? source_alloc(ctx,count,sizeof(*flow->captures)) : NULL;
    if (!flow->capture_heads || (count && !flow->captures)) return false;
    for (uint32_t f = 0; f < ctx->function_count; ++f) {
        if (!source_work(ctx,NULL)) return false;
        flow->capture_heads[f] = UINT32_MAX;
        XrXirType *parameters = (XrXirType *)ctx->functions[f].parameters;
        for (uint32_t p = 0; p < ctx->bodies[f].root_capture_count; ++p)
            if (ctx->bodies[f].root_captures[p] && !source_root_bottom_type(ctx,parameters[p],0,&parameters[p])) return false;
    }
    uint32_t at = 0;
    for (uint32_t f = 0; f < ctx->function_count; ++f)
        for (uint32_t i = 0; i < ctx->functions[f].instruction_count; ++i) {
            if (!source_work(ctx,NULL)) return false;
            const XrXirInstruction *op = &ctx->functions[f].instructions[i];
            if ((op->op != XR_XIR_FUNCTION_REF && op->op!=XR_XIR_CLEANUP_REGISTER) || !op->args[1]) continue;
            if (op->immediate<0 || (uint64_t)op->immediate>=ctx->function_count ||
                (op->op==XR_XIR_CLEANUP_REGISTER && ctx->identities[op->immediate].cleanup_owner!=f+1))
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"capture edge lost its lexical owner");
            uint32_t target = (uint32_t)op->immediate;
            flow->captures[at] = (SourceRootCaptureEdge){f,i,flow->capture_heads[target]};
            flow->capture_heads[target] = at++;
        }
    ctx->root_flow = flow; return source_root_slots_prepare(ctx);
}
static bool source_root_captures(SourceContext *ctx, bool *changed) {
    SourceRootFlow *flow = ctx->root_flow;
    for (uint32_t target = 0; target < ctx->function_count; ++target) {
        if (!source_work(ctx,NULL)) return false;
        if (!ctx->bodies[target].root_values) continue;
        XrXirType *parameters = (XrXirType *)ctx->functions[target].parameters;
        for (uint32_t p = 0; p < ctx->bodies[target].root_capture_count; ++p) {
            if (!ctx->bodies[target].root_captures[p]) continue;
            XrXirType common = XR_XIR_UNIT;
            for (uint32_t e = flow->capture_heads[target]; e != UINT32_MAX; e = flow->captures[e].next) {
                if (!source_work(ctx,NULL)) return false;
                SourceRootCaptureEdge edge = flow->captures[e];
                const XrXirFunction *caller = &ctx->functions[edge.function];
                const XrXirInstruction *reference = &caller->instructions[edge.instruction];
                XrXirType actual = source_root_operand(ctx,edge.function,caller->operands[reference->args[0] + p]);
                if (common == XR_XIR_UNIT) common = actual;
                else if (!source_root_capture_join(ctx,common,actual,0,&common)) return false;
            }
            if (common != XR_XIR_UNIT && parameters[p] != common) { parameters[p] = common; *changed = true; }
        }
    }
    return true;
}
static bool source_root_query_sync(SourceContext *ctx) {
    for (SourceRootQuery *query = ctx->root_queries; query; query = query->next) {
        if (!source_work(ctx,NULL)) return false;
        ctx->function = query->function; ctx->module = ctx->bodies[query->function].module;
        XrXirType type = source_root_operand(ctx,query->function,query->value);
        if (query->cell) type = xr_xir_cell_element(&ctx->types,type);
        if (query->expression)
            ((XrXirSourceExpression *)ctx->query.expressions)[query->record].type = source_query_type(ctx,type);
        else
            ((XrXirSourceDeclaration *)ctx->query.declarations)[query->record].type = source_query_type(ctx,type);
    }
    for (uint32_t f = 0; f < ctx->function_count; ++f) {
        if (!source_work(ctx,NULL)) return false;
        if (!ctx->bodies[f].infer_result || !ctx->bodies[f].declaration) continue;
        ctx->function = f; ctx->module = ctx->bodies[f].module;
        ((XrXirSourceDeclaration *)ctx->query.declarations)[ctx->bodies[f].declaration - 1].type = source_query_type(ctx,ctx->functions[f].result);
    }
    return true;
}
static XrXirStatus source_root_refine(void *opaque, const XrXirEffects *effects, bool *changed) {
    SourceContext *ctx = opaque;
    uint32_t function = ctx->function, module = ctx->module;
    bool again = true;
    *changed = false;
    if (!ctx->root_flow) {
        XrXirDeclarations declarations;XrXirModule view=source_module_view(ctx,&declarations);
        XrXirStatus status=xir_effects_refinement_capture(&ctx->compile,&view,effects);
        if (status!=XR_XIR_OK) {
            source_fail(ctx,NULL,status,"pre-bottom advertisements could not be owned");goto failure;
        }
    }
    if (!source_root_prepare(ctx)) goto failure;
    while (again) {
        again = false;
        for (uint32_t f = 0; f < ctx->function_count; ++f) {
            if (!source_work(ctx,NULL)) goto failure;
            if (!ctx->bodies[f].root_values) continue;
            ctx->function = f; ctx->module = ctx->bodies[f].module;
            for (uint32_t i = 0; i < ctx->functions[f].instruction_count; ++i)
                if (!source_work(ctx,NULL) || !source_root_instruction(ctx,f,i,effects,&again)) goto failure;
            if (!source_root_results(ctx,f,&again)) goto failure;
        }
        if (!source_root_captures(ctx,&again) || !source_root_slots(ctx,&again)) goto failure;
        if (again) *changed = true;
    }
    if (!source_effect_sync(ctx,effects,changed) || !source_root_query_sync(ctx)) goto failure;
    ctx->function = function; ctx->module = module;
    return XR_XIR_OK;
failure:
    ctx->function = function; ctx->module = module;
    return ctx->diagnostic.status == XR_XIR_OK ? XR_XIR_BAD_TYPE : ctx->diagnostic.status;
}
