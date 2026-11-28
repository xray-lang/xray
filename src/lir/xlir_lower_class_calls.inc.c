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
        call->immediate == (int64_t)body->parameter_count + producer;
}

typedef struct LowerClassCandidate {
    XirClassMethodTarget target;
    uint32_t depth;
} LowerClassCandidate;

typedef struct LowerClassCandidates {
    const XrXirCompileContext *memory;
    const XrXirTypes *types;
    LowerClassCandidate *items;
    uint32_t count, capacity;
} LowerClassCandidates;

static XrXirStatus lower_class_candidate(void *context, const XirClassMethodTarget *target) {
    LowerClassCandidates *list = context;
    if (list->count == list->capacity) {
        XrXirStatus status = XR_XIR_OK;
        void *memory = xir_compile_array_grow(list->memory, list->items,
            list->count, sizeof(*list->items), &list->capacity, &status);
        if (status != XR_XIR_OK) return status;
        list->items = memory;
    }
    const XrXirNominalTable *table = list->types->nominals;
    const XrXirTypeNode *node = xr_xir_type_node(list->types, target->receiver_type);
    if (!node || !table || node->nominal.declaration >= table->count) return XR_XIR_BAD_STRUCTURE;
    uint32_t id = node->nominal.declaration + 1, depth = 0;
    while (id) {
        if (id > table->count || depth == table->count) return XR_XIR_BAD_STRUCTURE;
        ++depth;
        id = table->declarations ? table->declarations[id - 1].parent : table->identities[id - 1].parent;
    }
    list->items[list->count++] = (LowerClassCandidate){*target, depth};
    return XR_XIR_OK;
}

static int lower_class_deepest_first(const void *left, const void *right) {
    const LowerClassCandidate *a = left, *b = right;
    if (a->depth != b->depth) return a->depth > b->depth ? -1 : 1;
    return (a->target.function > b->target.function) - (a->target.function < b->target.function);
}

static bool lower_class_target_fits(const Lower *c, const XrXirFunction *body,
    const XrXirInstruction *ref, const XirClassMethodTarget *target) {
    const XrXirInstruction *call = ref + 1;
    const XrXirFunction *callee = &c->source->functions[target->function];
    const XrXirTypeNode *signature = xr_xir_callable_signature(c->source->types, ref->type);
    if (!signature || !lower_empty_error(signature->element) || !lower_empty_error(callee->error_type) ||
        signature->parameter_count != call->args[1] || signature->parameter_count == UINT32_MAX ||
        callee->parameter_count != signature->parameter_count + 1 ||
        callee->parameters[0] != target->receiver_type || call->type != callee->result ||
        xr_xir_declared_parameter_mode(c->source->types, target->receiver_type,
            callee->consume_parameters, callee->consume_parameter_count, 0) != XR_PARAM_READ) return false;
    for (uint32_t p = 0; p < signature->parameter_count; ++p) {
        XrXirType expected = callee->parameters[p + 1];
        if ((expected != XR_XIR_I64 && expected != XR_XIR_BOOL && expected != XR_XIR_STRING &&
            !xr_xir_type_is_class(c->source->types, expected)) || expected != signature->parameters[p].type ||
            expected != xr_xir_operand_type(body, body->operands[call->args[0] + p]) ||
            xr_xir_declared_parameter_mode(c->source->types, expected, callee->consume_parameters,
                callee->consume_parameter_count, p + 1) != XR_PARAM_READ) return false;
    }
    return true;
}

static LowerClassFamily *lower_class_family_slot(LowerClassFamily *families, size_t capacity,
    uint32_t slot, XrXirType receiver) {
    size_t at = (xr_hash_int(slot) ^ xr_hash_int(receiver)) % capacity;
    while (families[at].slot && (families[at].slot != slot || families[at].receiver != receiver))
        if (++at == capacity) at = 0;
    return &families[at];
}

static LowerClassFamily *lower_class_family(Lower *c, const XrXirCompileContext *memory,
    uint32_t slot, XrXirType receiver, XrXirStatus *status) {
    size_t capacity = c->class_family_capacity;
    if (capacity && c->class_family_count < capacity / 2)
        return lower_class_family_slot(c->class_families, capacity, slot, receiver);
    if (capacity > SIZE_MAX / 2) { *status = XR_XIR_OVERFLOW; return NULL; }
    size_t next = capacity ? capacity * 2 : 16;
    LowerClassFamily *families = xir_compile_calloc(memory, next, sizeof(*families), status);
    if (!families) return NULL;
    for (size_t i = 0; i < capacity; ++i) {
        LowerClassFamily *old = &c->class_families[i];
        if (old->slot) *lower_class_family_slot(families, next, old->slot, old->receiver) = *old;
    }
    xr_compile_resources_free(c->class_families);
    c->class_families = families;
    c->class_family_capacity = next;
    return lower_class_family_slot(families, next, slot, receiver);
}

static void lower_class_prepare_family(Lower *c, const XrXirArtifact *closed,
    LowerClassFamily *family, uint32_t function, uint32_t instruction, XrXirStatus *status) {
    LowerClassCandidates candidates = {
        .memory=xr_xir_compile_artifact_context(closed),.types=c->source->types};
    *status = xir_compile_class_method_index_candidates(c->class_method_index, closed, function, instruction,
        lower_class_candidate, &candidates, NULL);
    if (*status == XR_XIR_BAD_TYPE || *status == XR_XIR_BAD_STRUCTURE || *status == XR_XIR_UNRESOLVED) {
        *status = XR_XIR_OK;
    } else if (*status == XR_XIR_OK && candidates.count) {
        uint32_t count = candidates.count - 1;
        uint32_t *overrides = xir_compile_calloc(candidates.memory, count, sizeof(*overrides), status);
        if (*status == XR_XIR_OK) {
            if (count > 1) qsort(candidates.items + 1, count, sizeof(*candidates.items), lower_class_deepest_first);
            for (uint32_t i = 0; i < count; ++i) overrides[i] = candidates.items[i + 1].target.function;
            family->call = (LowerClassCall){.callee=candidates.items[0].target.function + 1,
                .overrides=overrides,.override_count=count};
        }
    }
    xr_compile_resources_free(candidates.items);
}

static bool lower_class_target_index_fits(const Lower *c, const XrXirFunction *body,
    const XrXirInstruction *ref, uint32_t function) {
    XirClassMethodTarget target = {function, c->source->functions[function].parameters[0]};
    return lower_class_target_fits(c, body, ref, &target);
}

static XrXirStatus lower_class_call_site(Lower *c, const XrXirArtifact *closed,
    const XrXirFunction *body, uint32_t function, uint32_t instruction, LowerClassCall *output) {
    const XrXirInstruction *ref = &body->instructions[instruction];
    XirClassMethodTarget exact = {0};
    XrXirStatus status = xir_compile_class_method_index_target(c->class_method_index, closed,
        function, instruction, &exact, NULL);
    if (status == XR_XIR_OK) {
        if (lower_class_target_fits(c, body, ref, &exact))
            *output = (LowerClassCall){.callee=exact.function + 1,.receiver=ref->args[0]};
        return XR_XIR_OK;
    }
    if (status != XR_XIR_UNRESOLVED)
        return status == XR_XIR_BAD_TYPE || status == XR_XIR_BAD_STRUCTURE ? XR_XIR_OK : status;
    /* The index authenticated this site before returning UNRESOLVED. A method
     * family depends only on that checked slot and static receiver. */
    uint32_t slot = (uint32_t)ref->immediate + 1;
    XrXirType receiver = xr_xir_operand_type(body, ref->args[0]);
    status = XR_XIR_OK;
    LowerClassFamily *family = lower_class_family(c, xr_xir_compile_artifact_context(closed), slot, receiver, &status);
    if (!family) return status;
    if (!family->slot) {
        lower_class_prepare_family(c, closed, family, function, instruction, &status);
        if (status != XR_XIR_OK) return status;
        family->slot = slot;
        family->receiver = receiver;
        ++c->class_family_count;
    }
    const LowerClassCall *call = &family->call;
    if (!call->callee || !lower_class_target_index_fits(c, body, ref, call->callee - 1)) return XR_XIR_OK;
    for (uint32_t i = 0; i < call->override_count; ++i)
        if (!lower_class_target_index_fits(c, body, ref, call->overrides[i])) return XR_XIR_OK;
    *output = *call;
    output->receiver = ref->args[0];
    return XR_XIR_OK;
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
            LowerClassCall call = {0};
            status = lower_class_call_site(c, closed, body, function, i, &call);
            if (status != XR_XIR_OK || !call.callee) continue;
            if (!plan->calls)
                plan->calls = xir_compile_calloc(context,body->instruction_count,sizeof(*plan->calls),&status);
            if (status == XR_XIR_OK) plan->calls[i + 1] = call;
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
    for (size_t i = 0; i < c->class_family_capacity; ++i)
        xr_compile_resources_free((void *)c->class_families[i].call.overrides);
    xr_compile_resources_free(c->class_families);
    xir_compile_class_method_index_free(c->class_method_index);
}
