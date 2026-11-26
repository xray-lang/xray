/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License.
 *
 * xlir_lower_class_calls.inc.c - Direct calls to authenticated class methods
 */

static const LowerClassCall *lower_class_call(const Lower *c, uint32_t function, uint32_t consumer) {
    const LowerClassCall *calls = c->class_calls ? c->class_calls[function].calls : NULL;
    return calls && consumer < c->source->functions[function].instruction_count &&
        calls[consumer].callee ? &calls[consumer] : NULL;
}

static bool lower_class_call_status(Lower *c, XrXirStatus status) {
    if (status == XR_XIR_OK) return true;
    lower_error(c, status == XR_XIR_OUT_OF_MEMORY || status == XR_XIR_MEMORY_LIMIT || status == XR_XIR_OVERFLOW ?
        XR_LIR_OUT_OF_MEMORY : XR_LIR_VERIFY_ERROR, "cannot prepare Checked class method calls");
    return false;
}

static void lower_class_note_use(const XrXirFunction *body, uint8_t *uses, uint32_t value) {
    if (value < body->parameter_count) return;
    uint32_t instruction = value - body->parameter_count;
    if (instruction < body->instruction_count && body->instructions[instruction].op == XR_XIR_CLASS_METHOD_REF &&
        uses[instruction] < 2) ++uses[instruction];
}

static void lower_class_instruction_uses(const Lower *c, const XrXirFunction *body, uint8_t *uses) {
    for (uint32_t i = 0; i < body->instruction_count; ++i) {
        const XrXirInstruction *op = &body->instructions[i];
        if (op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_INVOKE_INDIRECT)
            lower_class_note_use(body,uses,(uint32_t)op->immediate);
        uint32_t count = xir_instruction_operand_count(body,op,c->source);
        if (xr_xir_op_uses_operand_table(op->op)) {
            uint32_t first = op->op == XR_XIR_PHI ? 1 : 0;
            uint32_t step = op->op == XR_XIR_PHI ? 2 : 1;
            for (uint32_t a = first; a < count; a += step)
                lower_class_note_use(body,uses,body->operands[op->args[0] + a]);
        } else {
            for (uint32_t a = 0; a < count; ++a)
                lower_class_note_use(body,uses,op->args[a]);
        }
    }
}

static void lower_class_lexical_uses(const XrXirFunction *body, const XirLexicalFunction *lexical,
    uint8_t *uses) {
    for (uint32_t n = 0; n < lexical->binding_count; ++n)
        lower_class_note_use(body,uses,lexical->bindings[n].value);
    for (uint32_t n = 0; n < lexical->event_count; ++n) {
        const XirLexicalEvent *event = &lexical->events[n];
        lower_class_note_use(body,uses,event->value);
        uint32_t producer = event->instruction;
        if (event->position == XIR_LEXICAL_BEFORE && producer) --producer;
        else if (event->position != XIR_LEXICAL_AFTER) continue;
        if (producer >= body->instruction_count || body->instructions[producer].op != XR_XIR_CLASS_METHOD_REF)
            continue;
        if (event->kind == XIR_LEXICAL_LEAVE || event->kind == XIR_LEXICAL_TEMP_END ||
            ((event->kind == XIR_LEXICAL_TAKE || event->kind == XIR_LEXICAL_REPLACE) &&
             event->value == body->instructions[producer].args[0]))
            uses[producer] = 2;
    }
}

static bool lower_class_immediate_call(const XrXirFunction *body, uint32_t producer) {
    const XrXirInstruction *call = &body->instructions[producer + 1];
    return body->instructions[producer].op == XR_XIR_CLASS_METHOD_REF && call->op == XR_XIR_CALL_INDIRECT &&
        call->immediate == (int64_t)body->parameter_count + producer && !call->args[1];
}

static bool lower_class_calls_plan(Lower *c, uint32_t function) {
    if (!c->allow_class_calls || (c->class_calls && c->class_calls[function].planned) ||
        c->source->declarations->functions[function].cleanup_owner)
        return true;
    const XrXirFunction *body = &c->source->functions[function];
    bool has_site = false;
    for (uint32_t b = 0; b < body->block_count && !has_site; ++b) {
        const XrXirBlock *block = &body->blocks[b];
        if (block->frontier || block->panic || block->count < 2) continue;
        for (uint32_t i = block->first; i < block->first + block->count - 1 && !has_site; ++i)
            has_site = lower_class_immediate_call(body,i);
    }
    if (!has_site) return true;

    const XrXirArtifact *closed = c->request->closed;
    const XrXirCompileContext *context = xr_xir_compile_artifact_context(closed);
    XrXirStatus status = XR_XIR_OK;
    if (!c->class_calls)
        c->class_calls = xir_compile_calloc(context,c->source->function_count,sizeof(*c->class_calls),&status);
    if (!lower_class_call_status(c,status)) return false;
    LowerClassCallPlan *plan = &c->class_calls[function];
    plan->planned = true;
    XirLexicalFunction lexical = {0};
    if (xr_xir_compile_lexical_function(closed,function,&lexical) != XR_XIR_OK) return true;

    uint8_t *uses = xir_compile_calloc(context,body->instruction_count,1,&status);
    if (!lower_class_call_status(c,status)) return false;
    lower_class_instruction_uses(c,body,uses);
    lower_class_lexical_uses(body,&lexical,uses);
    for (uint32_t b = 0; b < body->block_count && status == XR_XIR_OK; ++b) {
        const XrXirBlock *block = &body->blocks[b];
        if (block->frontier || block->panic || block->count < 2) continue;
        for (uint32_t i = block->first; i < block->first + block->count - 1 && status == XR_XIR_OK; ++i) {
            if (uses[i] != 1 || !lower_class_immediate_call(body,i)) continue;
            if (!c->class_method_index) {
                status = xir_compile_class_method_index_build(closed,&c->class_method_index,NULL);
                if (status != XR_XIR_OK) break;
            }
            XirClassMethodTarget target = {0};
            if (xir_compile_class_method_index_target(c->class_method_index,closed,function,i,&target,NULL) != XR_XIR_OK)
                continue;

            const XrXirInstruction *ref = &body->instructions[i];
            const XrXirInstruction *call = &body->instructions[i + 1];
            const XrXirFunction *callee = &c->source->functions[target.function];
            const XrXirTypeNode *signature = xr_xir_callable_signature(c->source->types,ref->type);
            if (!signature || signature->parameter_count || !lower_empty_error(signature->element) ||
                !lower_empty_error(callee->error_type) || callee->parameter_count != 1 ||
                callee->parameters[0] != target.receiver_type || call->type != callee->result ||
                xr_xir_declared_parameter_mode(c->source->types,target.receiver_type,
                    callee->consume_parameters,callee->consume_parameter_count,0) != XR_PARAM_READ)
                continue;
            if (!plan->calls)
                plan->calls = xir_compile_calloc(context,body->instruction_count,sizeof(*plan->calls),&status);
            if (status == XR_XIR_OK)
                plan->calls[i + 1] = (LowerClassCall){target.function + 1,ref->args[0]};
        }
    }
    xr_compile_resources_free(uses);
    return lower_class_call_status(c,status);
}

static void lower_class_calls_free(Lower *c) {
    if (c->class_calls) {
        for (uint32_t f = 0; f < c->source->function_count; ++f)
            xr_compile_resources_free(c->class_calls[f].calls);
        xr_compile_resources_free(c->class_calls);
    }
    xir_compile_class_method_index_free(c->class_method_index);
}