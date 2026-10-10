/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_effect_bindings.inc.c - Actual argument identities before loss
 *
 * KEY CONCEPT:
 *   A real argument COPY preserves its input identity. Complete shared checking
 *   authenticates its consuming edge and derives whether that role is variable.
 */
static bool source_effect_work(SourceContext *ctx, uint64_t units) {
    return xir_compile_work(&ctx->compile,units) ||
        source_fail(ctx,NULL,XR_XIR_BUDGET,"symbolic callable effect budget exhausted");
}
static bool source_effect_argument_recipe(SourceContext *ctx, AstNode *node,
    XrXirType actual, XrXirType declared, SourceConversionRecipe *recipe) {
    XrXirStatus status = xr_xir_compile_callable_weakening(&ctx->compile,
        &ctx->types,actual,declared);
    if (status != XR_XIR_OK) return source_fail(ctx,node,status,
        "callback argument requires exact shape and an admitted advertised bound");
    *recipe = (SourceConversionRecipe){true,true,XR_XIR_COPY,actual,actual,true,declared};
    return true;
}

/* The role is recorded on this real producer. A fixed Fn8 input remains Fn8;
 * this helper never follows a weakening opcode or resolves a target body. */
static bool source_effect_argument_bound(SourceContext *ctx, SourceValue value,
    XrXirType declared, bool *present) {
    *present = false;
    const XrXirFunction *function = &ctx->functions[ctx->function];
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (value.id < function->parameter_count || value.id - function->parameter_count >= body->count)
        return true;
    if (!source_work(ctx,NULL)) return false;
    SourceInstructionRecipe *producer = &body->recipes[value.id - function->parameter_count];
    if (producer->root.kind != 5) return true;
    if (producer->instruction.op != XR_XIR_COPY || producer->instruction.type != value.type)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"callback argument lost its actual COPY producer");
    XrXirStatus status = xr_xir_compile_callable_weakening(&ctx->compile,
        &ctx->types,value.type,declared);
    if (status != XR_XIR_OK) return source_fail(ctx,NULL,status,
        "callback argument no longer satisfies the final declaration shape");
    if (!source_work(ctx,NULL)) return false;
    producer->root.declared = declared;
    *present = true; return true;
}

static uint32_t source_effect_value_mode(const XrXirInstruction *op,
    const SourceRootValue *root) {
    if (op->op == XR_XIR_FUNCTION_WEAKEN) return XR_XIR_EFFECT_VALUE_FIXED;
    if (op->op == XR_XIR_FUNCTION_REF) return XR_XIR_EFFECT_VALUE_AUTHENTIC_REF;
    if (op->op == XR_XIR_COPY && root && root->kind == 5) return XR_XIR_EFFECT_VALUE_CALL_BIND;
    switch (op->op) {
    case XR_XIR_COPY: case XR_XIR_PHI: case XR_XIR_LOCAL_NEW: case XR_XIR_LOCAL_READ:
    case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN: case XR_XIR_SCALAR_LOCAL_NEW:
    case XR_XIR_SCALAR_LOCAL_READ: case XR_XIR_OWNED_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_READ:
        return XR_XIR_EFFECT_VALUE_PROPAGATE;
    default: return 0;
    }
}

/* Only the existing Source-owned record follows an actual producer change.
 * A missing record is not synthesized here: the complete original records pass
 * still owns membership and capacity. Pre-bottom advertisements remain owned
 * by the effects refinement snapshot; CALL_BIND keeps its consuming bound. */
static bool source_effect_value_refresh(SourceContext *ctx, uint32_t f, uint32_t i) {
    if (!ctx->effect_evidence || ctx->bodies[f].library_effect) return true;
    if (!source_effect_work(ctx,4)) return false;
    SourceFunction *body = &ctx->bodies[f];
    const XrXirFunction *function = &ctx->functions[f];
    if (ctx->effect_evidence->contract_count != ctx->function_count ||
        !ctx->effect_evidence->contracts || i >= function->instruction_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"prepared effect record owner changed shape");
    XrXirFunctionEffectContract *contract = &ctx->effect_evidence->contracts[f];
    if (contract->value_count != body->effect_value_capacity ||
        body->effect_value_capacity > function->instruction_count ||
        !!body->effect_values != !!body->effect_value_capacity ||
        contract->values != (contract->value_count ? body->effect_values : NULL))
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"prepared effect value storage changed shape");
    uint32_t first = 0, last = contract->value_count;
    while (first < last) {
        if (!source_effect_work(ctx,3)) return false;
        uint32_t middle = first + (last - first) / 2;
        if (body->effect_values[middle].instruction < i) first = middle + 1;
        else last = middle;
    }
    const XrXirInstruction *op = &function->instructions[i];
    const SourceRootValue *root = body->root_values ? &body->root_values[i] : NULL;
    uint32_t mode = source_effect_value_mode(op,root);
    if (first == contract->value_count || body->effect_values[first].instruction != i) {
        if (mode && root && xr_xir_callable_signature(&ctx->types,root->declared))
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"prepared callable producer lost its effect record");
        return true;
    }
    if (!mode || !xr_xir_callable_signature(&ctx->types,op->type))
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"prepared effect value lost its callable producer");
    if (!source_effect_work(ctx,3)) return false;
    body->effect_values[first] = (XrXirRootValueIdentity){i,mode,
        mode == XR_XIR_EFFECT_VALUE_CALL_BIND ? root->declared : op->type};
    return true;
}

static bool source_effect_records(SourceContext *ctx, uint32_t f, bool prepare) {
    SourceFunction *body = &ctx->bodies[f];
    const XrXirFunction *function = &ctx->functions[f];
    uint32_t values = 0, bindings = 0;
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        if (!source_work(ctx,NULL)) return false;
        const XrXirInstruction *op = &function->instructions[i];
        const SourceRootValue *root = body->root_values ? &body->root_values[i] : NULL;
        uint32_t mode = source_effect_value_mode(op,root);
        if (mode && xr_xir_callable_signature(&ctx->types,op->type)) {
            if (!prepare) {
                if (values >= body->effect_value_capacity)
                    return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"effect value storage changed shape");
                if (!source_effect_work(ctx,3)) return false;
                body->effect_values[values] = (XrXirRootValueIdentity){i,mode,
                    mode == XR_XIR_EFFECT_VALUE_CALL_BIND ? root->declared : op->type};
            }
            ++values;
        }
    }
    /* Grouping by the actual opcode family makes the serialized record order
     * canonical without a second mutable sorting buffer. */
    for (uint32_t family = XR_XIR_EFFECT_BINDING_DIRECT;
        family <= XR_XIR_EFFECT_BINDING_REQUIREMENT; ++family) {
      for (uint32_t i = 0; i < function->instruction_count; ++i) {
        if (!source_work(ctx,NULL)) return false;
        const XrXirInstruction *op = &function->instructions[i];
        uint32_t actual_family = op->op == XR_XIR_CALL || op->op == XR_XIR_INVOKE ?
            XR_XIR_EFFECT_BINDING_DIRECT : op->op == XR_XIR_FUNCTION_REF ?
            XR_XIR_EFFECT_BINDING_CAPTURE : op->op == XR_XIR_CALL_REQUIREMENT ?
            XR_XIR_EFFECT_BINDING_REQUIREMENT : 0;
        if (actual_family != family || !body->root_values) continue;
        for (uint32_t p = 0; p < op->args[1]; ++p) {
            if (!source_work(ctx,NULL)) return false;
            uint32_t value = function->operands[op->args[0] + p];
            if (value < function->parameter_count || value - function->parameter_count >= function->instruction_count)
                continue;
            const SourceRootValue *producer = &body->root_values[value - function->parameter_count];
            if (producer->kind != 5) continue;
            if (!prepare) {
                if (bindings >= body->effect_binding_capacity)
                    return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"effect binding storage changed shape");
                if (!source_effect_work(ctx,4)) return false;
                body->effect_bindings[bindings] = (XrXirEffectCallBinding){family,i,p,value};
            }
            if (bindings == UINT32_MAX) return source_fail(ctx,NULL,XR_XIR_BUDGET,"effect binding count exhausted");
            ++bindings;
        }
      }
    }
    if (prepare) {
        if (!source_effect_work(ctx,2)) return false;
        body->effect_value_capacity = values; body->effect_binding_capacity = bindings;
        body->effect_values = values ? source_alloc(ctx,values,sizeof(*body->effect_values)) : NULL;
        body->effect_bindings = bindings ? source_alloc(ctx,bindings,sizeof(*body->effect_bindings)) : NULL;
        if ((values && !body->effect_values) || (bindings && !body->effect_bindings)) return false;
    } else {
        XrXirFunctionEffectContract *contract = &ctx->effect_evidence->contracts[f];
        if (!source_effect_work(ctx,4)) return false;
        contract->value_count = values; contract->values = values ? body->effect_values : NULL;
        contract->binding_count = bindings; contract->bindings = bindings ? body->effect_bindings : NULL;
    }
    return true;
}

static bool source_effect_prepare(SourceContext *ctx) {
    if (ctx->effect_evidence) return true;
    XrXirProvenance *evidence = source_alloc(ctx,1,sizeof(*evidence));
    if (!evidence) return false;
    if (!source_effect_work(ctx,2)) return false;
    evidence->kind = XR_XIR_EVIDENCE_TEMPLATE; evidence->contract_count = ctx->function_count;
    evidence->contracts = source_alloc(ctx,ctx->function_count,sizeof(*evidence->contracts));
    if (!evidence->contracts) return false;
    ctx->effect_evidence = evidence;
    for (uint32_t f = 0; f < ctx->function_count; ++f) {
        if (!source_work(ctx,NULL)) return false;
        SourceFunction *body = &ctx->bodies[f];
        const XrXirFunction *function = &ctx->functions[f];
        XrXirFunctionEffectContract *contract = &evidence->contracts[f];
        if (body->library_effect) {
            if (!body->checked_library ||
                body->library_effect->parameter_count != function->parameter_count)
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"imported effect parameters changed shape");
            if (!source_copy_bytes(ctx,NULL,contract,body->library_effect,sizeof(*contract))) return false;
            continue;
        }
        uint64_t terms = (uint64_t)function->parameter_count + function->instruction_count;
        if (terms > UINT32_MAX) return source_fail(ctx,NULL,XR_XIR_BUDGET,"effect term capacity exhausted");
        body->effect_parameters = function->parameter_count ?
            source_alloc(ctx,function->parameter_count,sizeof(*body->effect_parameters)) : NULL;
        body->effect_terms = terms ? source_alloc(ctx,(size_t)terms,sizeof(*body->effect_terms)) : NULL;
        if ((function->parameter_count && !body->effect_parameters) || (terms && !body->effect_terms)) return false;
        if (!source_effect_work(ctx,2)) return false;
        contract->parameter_count = function->parameter_count; contract->parameters = body->effect_parameters;
        for (uint32_t p = 0; p < function->parameter_count; ++p) {
            if (!source_effect_work(ctx,2)) return false;
            body->effect_parameters[p] = (XrXirEffectParameter){
                xr_xir_callable_signature(&ctx->types,function->parameters[p]) ?
                    XR_XIR_EFFECT_PARAMETER_VARIABLE : XR_XIR_EFFECT_PARAMETER_FIXED,0};
        }
        if (!source_effect_records(ctx,f,true) || !source_effect_records(ctx,f,false)) return false;
    }
    return true;
}

static bool source_effect_fixed_arguments(SourceContext *ctx,
    const XrXirEffects *effects, uint32_t f, bool *changed) {
    const XrXirFunctionEffectContract *contract = &ctx->effect_evidence->contracts[f];
    const XrXirFunction *function = &ctx->functions[f];
    for (uint32_t b = 0; b < contract->binding_count; ++b) {
        if (!source_work(ctx,NULL)) return false;
        XrXirEffectCallBinding binding = contract->bindings[b];
        if (binding.family != XR_XIR_EFFECT_BINDING_DIRECT &&
            binding.family != XR_XIR_EFFECT_BINDING_CAPTURE) continue;
        const XrXirInstruction *call = &function->instructions[binding.instruction];
        const XrXirFunctionEffectContract *callee = xir_effects_contract(effects,(uint32_t)call->immediate);
        if (!callee || binding.parameter >= callee->parameter_count)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"callback lost its prepared callee parameter");
        if (callee->parameters[binding.parameter].kind != XR_XIR_EFFECT_PARAMETER_FIXED) continue;
        uint32_t i = binding.value - function->parameter_count;
        SourceRootValue *root = &ctx->bodies[f].root_values[i];
        XrXirInstruction *copy = (XrXirInstruction *)function->instructions + i;
        if (root->kind != 5 || copy->op != XR_XIR_COPY)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"fixed callback lost its actual argument COPY");
        XrXirType actual = xr_xir_operand_type(function,copy->args[0]);
        XrXirStatus status = xr_xir_compile_callable_weakening(&ctx->compile,
            &ctx->types,actual,root->declared);
        if (status != XR_XIR_OK) return source_fail(ctx,NULL,status,"fixed callback requires its declared bound");
        if (!source_effect_work(ctx,3)) return false;
        copy->op = XR_XIR_FUNCTION_WEAKEN; copy->type = root->declared; root->kind = 1;
        *changed = true;
    }
    return true;
}

/* Imported metadata is independently derived again. A mismatch fails;
 * Source recipes must never rewrite an authenticated library contract. */
static bool source_effect_library_match(SourceContext *ctx,
    const XrXirEffects *effects, uint32_t function) {
    if (!source_work(ctx,NULL)) return false;
    const XrXirFunctionEffectContract *stored = ctx->bodies[function].library_effect;
    const XrXirFunctionEffectContract *derived = xir_effects_contract(effects,function);
    if (!derived || derived->parameter_count != stored->parameter_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"imported effect parameters changed shape");
    for (uint32_t p = 0; p < derived->parameter_count; ++p) {
        if (!source_effect_work(ctx,4)) return false;
        if (derived->parameters[p].kind != stored->parameters[p].kind ||
            derived->parameters[p].uses != stored->parameters[p].uses)
            return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"imported effect parameter facts do not match");
    }
    if (!source_effect_work(ctx,2)) return false;
    if (derived->formula.constant_mask != stored->formula.constant_mask ||
        derived->formula.term_count != stored->formula.term_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"imported effect formula does not match");
    for (uint32_t t = 0; t < derived->formula.term_count; ++t) {
        if (!source_effect_work(ctx,4)) return false;
        if (derived->formula.terms[t].kind != stored->formula.terms[t].kind ||
            derived->formula.terms[t].index != stored->formula.terms[t].index)
            return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"imported effect term does not match");
    }
    return true;
}

static bool source_effect_sync(SourceContext *ctx, const XrXirEffects *effects, bool *changed) {
    if (!ctx->effect_evidence || !xir_effects_contract(effects,0)) return true;
    for (uint32_t f = 0; f < ctx->function_count; ++f) {
        if (ctx->bodies[f].library_effect) {
            if (!source_effect_library_match(ctx,effects,f)) return false;
            continue;
        }
        if (!source_effect_fixed_arguments(ctx,effects,f,changed)) return false;
        const XrXirFunctionEffectContract *derived = xir_effects_contract(effects,f);
        XrXirFunctionEffectContract *stored = &ctx->effect_evidence->contracts[f];
        SourceFunction *body = &ctx->bodies[f];
        if (!derived || derived->parameter_count != stored->parameter_count)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"prepared effect owner changed its physical parameters");
        for (uint32_t p = 0; p < derived->parameter_count; ++p) {
            if (!source_effect_work(ctx,6)) return false;
            if (body->effect_parameters[p].kind != derived->parameters[p].kind ||
                body->effect_parameters[p].uses != derived->parameters[p].uses) *changed = true;
            body->effect_parameters[p] = derived->parameters[p];
        }
        if (!source_work(ctx,NULL)) return false;
        bool different = stored->formula.constant_mask != derived->formula.constant_mask ||
            stored->formula.term_count != derived->formula.term_count;
        for (uint32_t t = 0; t < derived->formula.term_count; ++t) {
            if (!source_effect_work(ctx,4)) return false;
            if (!different && (body->effect_terms[t].kind != derived->formula.terms[t].kind ||
                body->effect_terms[t].index != derived->formula.terms[t].index)) different = true;
            body->effect_terms[t] = derived->formula.terms[t];
        }
        if (different) *changed = true;
        if (!source_effect_work(ctx,3)) return false;
        stored->formula = (XrXirRootFormula){derived->formula.constant_mask,derived->formula.term_count,
            derived->formula.term_count ? body->effect_terms : NULL};
        if (!source_effect_records(ctx,f,false)) return false;
    }
    return true;
}
