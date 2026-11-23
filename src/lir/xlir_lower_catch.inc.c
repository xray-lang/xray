/*
 * xray - Lightweight typed scripting with native concurrency
 * https://xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_lower_catch.inc.c - Carry a single exact error through ordinary catch
 */

static bool lower_is_plain_error_enum(const Lower *, XrXirType);

static XrXirType lower_catch_type(const Lower *c, const XrXirFunction *f, uint32_t value) {
    const LowerCatchPlan *plan = lower_catch_plan(c, f);
    if (!plan || value < f->parameter_count || value - f->parameter_count >= f->instruction_count)
        return XR_XIR_UNIT;
    return plan->types[value - f->parameter_count];
}

static XrXirType lower_error_operand(const Lower *c, const XrXirFunction *f, uint32_t value) {
    XrXirType type = xr_xir_operand_type(f, value);
    if (type != XR_XIR_ERROR) return type;
    XrXirType caught = lower_catch_type(c, f, value);
    return caught ? caught : type;
}

static uint32_t lower_invoke_failure_predecessor(const Lower *c, const XrXirFunction *f, uint32_t block) {
    uint32_t optional = lower_optional_error_predecessor(c, block);
    const LowerCatchPlan *plan = lower_catch_plan(c, f);
    return optional || !plan ? optional : plan->predecessors[block];
}

/* Closed Checked verifies exact incoming PHI pairs, the ownership of each
 * marker by its call, and the guard of every narrowing. Accept only the
 * empty, dedicated landings that forward the call's own error. */
static XrXirType lower_catch_join_type(const Lower *c, const XrXirFunction *f, uint32_t block) {
    const XrXirBlock *join = &f->blocks[block];
    const XrXirInstruction *phi = &f->instructions[join->first];
    if (join->panic || join->frontier || !phi->args[1]) return XR_XIR_UNIT;
    XrXirType exact = XR_XIR_NEVER;
    for (uint32_t a = 0; a < phi->args[1]; a += 2) {
        const uint32_t *input = &f->operands[phi->args[0] + a];
        const XrXirBlock *landing = &f->blocks[input[0]];
        const XrXirInstruction *marker = &f->instructions[landing->first];
        if (landing->count != 2 || landing->panic || landing->frontier ||
            input[1] != f->parameter_count + landing->first || marker->op != XR_XIR_INVOKE_ERROR ||
            marker->type != XR_XIR_ERROR || marker[1].op != XR_XIR_JUMP || marker[1].targets[0] != block)
            return XR_XIR_UNIT;
        const XrXirInstruction *call = &f->instructions[marker->immediate];
        if (call->op != XR_XIR_INVOKE || call->targets[1] != input[0]) return XR_XIR_UNIT;
        XrXirType error = c->source->functions[call->immediate].error_type;
        if (lower_empty_error(error)) continue;
        if (exact != XR_XIR_NEVER && exact != error) return XR_XIR_UNIT;
        exact = error;
    }
    return exact == XR_XIR_NEVER || lower_is_plain_error_enum(c, exact) ? exact : XR_XIR_UNIT;
}

static bool lower_catch_errors(Lower *c, const XrXirFunction *f) {
    if (lower_catch_plan(c, f)) return true;
    LowerCatchPlan *plan = NULL;
    for (uint32_t b = 0; b < f->block_count; ++b) {
        uint32_t first = f->blocks[b].first;
        const XrXirInstruction *phi = &f->instructions[first];
        if (phi->op != XR_XIR_PHI || phi->type != XR_XIR_ERROR || lower_optional_error_join(c, b))
            continue;
        XrXirType exact = lower_catch_join_type(c, f, b);
        if (!exact) continue;
        if (!plan) {
            if (!c->catch_errors)
                c->catch_errors = lower_alloc(c, c->source->function_count, sizeof(*c->catch_errors));
            if (!c->catch_errors) return false;
            plan = &c->catch_errors[c->function];
            plan->types = lower_alloc(c, f->instruction_count, sizeof(*plan->types));
            plan->branches = lower_alloc(c, f->block_count, sizeof(*plan->branches));
            plan->predecessors = lower_alloc(c, f->block_count, sizeof(*plan->predecessors));
            if (!plan->types || !plan->branches || !plan->predecessors) return false;
        }
        plan->types[first] = exact;
    }
    if (!plan) return true;
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrXirInstruction *term = lower_term(c, f, b);
        if (!term) return false;
        if (term->op == XR_XIR_INVOKE) {
            uint32_t failed = term->targets[1];
            const XrXirInstruction *exit = lower_term(c, f, failed);
            if (!exit) return false;
            if (exit->op == XR_XIR_JUMP && plan->types[f->blocks[exit->targets[0]].first])
                plan->predecessors[failed] = b + 1;
        } else if (term->op == XR_XIR_BRANCH) {
            const XrXirInstruction *test = lower_place_definition(f, term->args[0]);
            if (!test || test->op != XR_XIR_ERROR_IS) continue;
            XrXirType exact = lower_catch_type(c, f, test->args[0]);
            if (!exact || exact == XR_XIR_NEVER) continue;
            uint32_t target = term->targets[exact == (XrXirType)test->immediate ? 0 : 1];
            plan->branches[b] = (XrXirInstruction){.op = XR_XIR_JUMP, .type = XR_XIR_UNIT,
                .targets = {target}};
        }
    }
    return true;
}

static void lower_free_error_plans(Lower *c) {
    for (uint32_t f = 0; f < c->source->function_count && (c->optional_errors || c->catch_errors); ++f) {
        if (c->optional_errors) xr_free(c->optional_errors[f]);
        if (c->catch_errors) {
            xr_free(c->catch_errors[f].types);
            xr_free(c->catch_errors[f].branches);
            xr_free(c->catch_errors[f].predecessors);
        }
    }
    xr_free(c->optional_errors);
    xr_free(c->catch_errors);
}

static bool lower_catch_value(LowerFunction *f, uint32_t block, const XrXirInstruction *op,
    XrLirValue *result) {
    if (op->op == XR_XIR_ERROR_NARROW) return lower_value(f, op->args[0], result);
    Lower *c = f->owner;
    XrXirType exact = lower_catch_type(c, f->source, op->args[0]);
    XrLirInstruction select = {.op = XR_LIR_CONST, .type = XR_LIR_I1,
        .bits = exact == (XrXirType)op->immediate, .source = lower_source(c)};
    return lower_build(c, xr_lir_emit_value(c->module, f->blocks[block], &select, result));
}
