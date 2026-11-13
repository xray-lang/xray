/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_lower_callable_scope.inc.c - Consume owned lexical extents on the SSA graph
 */
static bool lower_callable_type(const LowerFunction *f, XrXirType type) {
    XrLirCallableType signature;
    return xr_lir_callable_type_view(f->owner->module, lower_type(f->owner, type), &signature) == XR_LIR_OK;
}
static int lower_temporary_compare(const void *left, const void *right) {
    const LowerLexicalTemporary *a = left, *b = right;
    return (a->statement > b->statement) - (a->statement < b->statement);
}
static uint32_t lower_temporary_scope(const LowerLexical *lexical, uint32_t statement) {
    uint32_t first = 0, count = lexical->temporary_count;
    while (count) {
        uint32_t middle = first + count / 2;
        if (lexical->temporaries[middle].statement < statement) {
            count -= middle - first + 1;
            first = middle + 1;
        } else count = middle - first;
    }
    return first < lexical->temporary_count && lexical->temporaries[first].statement == statement ?
        lexical->temporaries[first].scope : UINT32_MAX;
}
static bool lower_lexical_contains(const LowerLexical *lexical, uint32_t parent, uint32_t child) {
    for (uint64_t steps = 0; steps <= lexical->count && child <= lexical->count; ++steps) {
        if (parent == child) return true;
        if (!child) break;
        child = lexical->parents[child];
    }
    return false;
}
static bool lower_lexical_needed(LowerFunction *f) {
    if (lower_callable_type(f, f->source->result)) return true;
    for (uint32_t i = 0; i < f->source->instruction_count; ++i) {
        const XrXirInstruction *op = &f->source->instructions[i];
        if ((op->op == XR_XIR_CALL || op->op == XR_XIR_CALL_INDIRECT ||
             op->op == XR_XIR_CELL_READ || op->op == XR_XIR_SLOT_LOAD) && lower_callable_type(f, op->type))
            return true;
        if (op->op == XR_XIR_FUNCTION_REF) {
            XirClosureStorage placement;
            if (xr_xir_compile_closure_placement(f->owner->request->closed, f->owner->function,
                    f->source->parameter_count + i, &placement) == XR_XIR_OK && placement == XIR_CLOSURE_OWNED)
                return true;
        }
    }
    return false;
}
static bool lower_lexical_storage(LowerFunction *f) {
    Lower *c = f->owner;
    LowerLexical *lexical = &f->lexical;
    uint32_t instructions = f->source->instruction_count, blocks = f->source->block_count;
    lexical->caps = lower_alloc(c, (size_t)lexical->count + 1, sizeof(*lexical->caps));
    lexical->parents = lower_alloc(c, (size_t)lexical->count + 1, sizeof(*lexical->parents));
    lexical->before = lower_alloc(c, instructions, sizeof(*lexical->before));
    lexical->after = lower_alloc(c, instructions, sizeof(*lexical->after));
    lexical->entries = lower_alloc(c, blocks, sizeof(*lexical->entries));
    lexical->next = lower_alloc(c, lexical->source.event_count, sizeof(*lexical->next));
    lexical->incoming = lower_alloc(c, blocks, sizeof(*lexical->incoming));
    lexical->outgoing = lower_alloc(c, blocks, sizeof(*lexical->outgoing));
    lexical->term_scope = lower_alloc(c, blocks, sizeof(*lexical->term_scope));
    lexical->term_target = lower_alloc(c, blocks, sizeof(*lexical->term_target));
    lexical->value_scopes = lower_alloc(c, f->value_count, sizeof(*lexical->value_scopes));
    if (!lexical->caps || !lexical->parents || !lexical->before || !lexical->after || !lexical->entries ||
        !lexical->next || !lexical->incoming || !lexical->outgoing || !lexical->term_scope ||
        !lexical->term_target || !lexical->value_scopes) return false;
    for (uint32_t i = 0; i < instructions; ++i) lexical->before[i] = lexical->after[i] = UINT32_MAX;
    for (uint32_t b = 0; b < blocks; ++b) {
        lexical->entries[b] = lexical->incoming[b] = lexical->outgoing[b] = UINT32_MAX;
        lexical->term_target[b] = UINT32_MAX;
    }
    lexical->incoming[0] = 0;
    for (uint32_t s = 1; s <= lexical->count; ++s) lexical->parents[s] = UINT32_MAX;
    return true;
}
static bool lower_lexical_index(LowerFunction *f) {
    LowerLexical *lexical = &f->lexical;
    const XirLexicalFunction *source = &lexical->source;
    for (uint32_t n = source->event_count; n; --n) {
        const XirLexicalEvent *event = &source->events[n - 1];
        uint32_t *head = NULL;
        if (event->position == XIR_LEXICAL_BLOCK_ENTRY) head = &lexical->entries[event->block];
        else if (event->position == XIR_LEXICAL_BEFORE) head = &lexical->before[event->instruction];
        else if (event->position == XIR_LEXICAL_AFTER) head = &lexical->after[event->instruction];
        else if (event->kind == XIR_LEXICAL_PANIC && event->target == UINT32_MAX) continue;
        else return lower_unsupported(f->owner, "owned function extent on an unsupported outcome");
        lexical->next[n - 1] = *head;
        *head = n - 1;
        if (event->kind == XIR_LEXICAL_ENTER) {
            uint32_t parent = event->statement ? lower_temporary_scope(lexical, event->statement) :
                event->target == UINT32_MAX ? 0 : event->target + 1;
            if (parent == UINT32_MAX || lexical->parents[event->scope + 1] != UINT32_MAX)
                return lower_unsupported(f->owner, "lexical scope has no unique actual entry");
            lexical->parents[event->scope + 1] = parent;
        } else if (event->kind == XIR_LEXICAL_TEMP_BEGIN) {
            uint32_t scope = lower_temporary_scope(lexical, event->statement);
            uint32_t parent = event->parent_statement ? lower_temporary_scope(lexical, event->parent_statement) :
                event->scope + 1;
            if (scope == UINT32_MAX || parent == UINT32_MAX)
                return lower_unsupported(f->owner, "full expression lost its numeric parent");
            lexical->parents[scope] = parent;
        }
    }
    return true;
}
static bool lower_lexical_inventory(LowerFunction *f) {
    LowerLexical *lexical = &f->lexical;
    bool needed = lower_lexical_needed(f);
    bool borrowed_callable = false;
    for (uint32_t p = 0; p < f->source->parameter_count && !needed; ++p)
        borrowed_callable |= lower_callable_type(f, f->source->parameters[p]);
    if (!needed && !borrowed_callable) return true;
    XrXirStatus status = xr_xir_compile_lexical_function(f->owner->request->closed,
        f->owner->function, &lexical->source);
    if (status != XR_XIR_OK) return !needed || lower_unsupported(f->owner, "owned function lexical extent is unresolved");
    for (uint32_t b = 0; b < lexical->source.binding_count; ++b) {
        const XirLexicalBinding *binding = &lexical->source.bindings[b];
        needed |= binding->role == XIR_LEXICAL_LOCAL && lower_callable_type(f, binding->physical);
    }
    if (!needed) { lexical->source = (XirLexicalFunction){0}; return true; }
    for (uint32_t b = 0; b < lexical->source.binding_count; ++b) {
        const XirLexicalBinding *binding = &lexical->source.bindings[b];
        if (binding->role == XIR_LEXICAL_LOCAL &&
            xr_xir_type_is_cell(f->owner->source->types, binding->physical) &&
            lower_callable_type(f, binding->logical))
            return lower_unsupported(f->owner, "owned mutable function binding extent");
    }
    for (uint32_t n = 0; n < lexical->source.event_count; ++n)
        lexical->temporary_count += lexical->source.events[n].kind == XIR_LEXICAL_TEMP_BEGIN;
    if (lexical->temporary_count >= UINT32_MAX - lexical->source.scope_count)
        return lower_unsupported(f->owner, "function lexical scope capacity");
    lexical->count = lexical->source.scope_count + lexical->temporary_count;
    lexical->temporaries = lower_alloc(f->owner, lexical->temporary_count, sizeof(*lexical->temporaries));
    if (lexical->temporary_count && !lexical->temporaries) return false;
    uint32_t at = 0;
    for (uint32_t n = 0; n < lexical->source.event_count; ++n) {
        const XirLexicalEvent *event = &lexical->source.events[n];
        if (event->kind == XIR_LEXICAL_TEMP_BEGIN) {
            lexical->temporaries[at] = (LowerLexicalTemporary){event->statement, lexical->source.scope_count + at + 1};
            ++at;
        }
    }
    if (lexical->temporary_count > 1)
        qsort(lexical->temporaries, lexical->temporary_count, sizeof(*lexical->temporaries), lower_temporary_compare);
    for (uint32_t n = 1; n < lexical->temporary_count; ++n)
        if (lexical->temporaries[n - 1].statement == lexical->temporaries[n].statement)
            return lower_unsupported(f->owner, "full expression has multiple declaration entries");
    return lower_lexical_storage(f) && lower_lexical_index(f);
}
static bool lower_lexical_enter(LowerFunction *f, uint32_t b, uint32_t scope) {
    LowerLexical *lexical = &f->lexical;
    uint32_t parent = lexical->parents[scope];
    if (parent != lexical->current || (parent && !lexical->caps[parent].id) || lexical->caps[scope].id)
        return lower_unsupported(f->owner, "lexical entry disagrees with the selected predecessor");
    XrLirInstruction enter = {.op=XR_LIR_FUNCTION_SCOPE_ENTER, .type=XR_LIR_FUNCTION_SCOPE,
        .operands=&lexical->caps[parent], .operand_count=parent != 0, .source=lower_source(f->owner)};
    if (!lower_build(f->owner, xr_lir_emit_value(f->owner->module, f->blocks[b], &enter, &lexical->caps[scope])))
        return false;
    lexical->current = scope;
    return true;
}
static bool lower_lexical_leave(LowerFunction *f, uint32_t b, XrLirValue *protected_value) {
    LowerLexical *lexical = &f->lexical;
    uint32_t scope = lexical->current;
    if (!scope || !lexical->caps[scope].id)
        return lower_unsupported(f->owner, "lexical retirement has no active entry");
    XrLirValue operands[2] = {lexical->caps[scope], protected_value ? *protected_value : (XrLirValue){0}};
    XrLirType type = protected_value ?
        f->owner->module->functions[f->function.id - 1].values[protected_value->id - 1].type : XR_LIR_UNIT;
    XrLirInstruction leave = {.op=XR_LIR_FUNCTION_SCOPE_LEAVE, .type=type,
        .operands=operands, .operand_count=protected_value ? 2u : 1u, .source=lower_source(f->owner)};
    bool good = lower_build(f->owner, protected_value ?
        xr_lir_emit_value(f->owner->module, f->blocks[b], &leave, protected_value) :
        xr_lir_emit_effect(f->owner->module, f->blocks[b], &leave));
    if (good) lexical->current = lexical->parents[scope];
    return good;
}
static bool lower_lexical_bind(LowerFunction *f, uint32_t b, const XirLexicalEvent *event) {
    LowerLexical *lexical = &f->lexical;
    const XirLexicalBinding *binding = &lexical->source.bindings[event->binding];
    if (binding->role != XIR_LEXICAL_LOCAL || !lower_callable_type(f, binding->physical)) return true;
    uint32_t scope = binding->scope + 1;
    if (!lower_lexical_contains(lexical, scope, lexical->current) || !lexical->caps[scope].id)
        return lower_unsupported(f->owner, "function binding has no live lexical owner");
    XrLirValue operands[2] = {lexical->caps[scope], {0}}, retained = {0};
    if (!lower_value(f, event->value, &operands[1])) return false;
    XrLirInstruction bind = {.op=XR_LIR_FUNCTION_BIND, .type=lower_type(f->owner, binding->physical),
        .operands=operands, .operand_count=2, .bits=binding->order, .source=lower_source(f->owner)};
    if (!lower_build(f->owner, xr_lir_emit_value(f->owner->module, f->blocks[b], &bind, &retained))) return false;
    uint32_t previous = lexical->value_scopes[event->value];
    /* A borrowed formal or a wider binding remains the canonical SSA carrier.
     * Only extending a temporary's lifetime replaces that shorter carrier. */
    if (previous && lower_lexical_contains(lexical, scope, previous)) {
        f->values[event->value] = retained;
        lexical->value_scopes[event->value] = scope;
    }
    return true;
}
static bool lower_lexical_event(LowerFunction *f, uint32_t b, const XirLexicalEvent *event) {
    LowerLexical *lexical = &f->lexical;
    uint32_t scope = event->scope + 1;
    switch (event->kind) {
        case XIR_LEXICAL_ENTER: return lower_lexical_enter(f, b, scope);
        case XIR_LEXICAL_TEMP_BEGIN:
            return lower_lexical_enter(f, b, lower_temporary_scope(lexical, event->statement));
        case XIR_LEXICAL_TEMP_END:
            scope = lower_temporary_scope(lexical, event->statement);
            /* fall through */
        case XIR_LEXICAL_LEAVE:
            if (scope != lexical->current)
                return lower_unsupported(f->owner, "lexical leave disagrees with its actual entry");
            return lower_lexical_leave(f, b, NULL);
        case XIR_LEXICAL_BIND: return lower_lexical_bind(f, b, event);
        case XIR_LEXICAL_RETURN: case XIR_LEXICAL_BREAK: case XIR_LEXICAL_CONTINUE:
            if (lexical->term_target[b] != UINT32_MAX || event->frontier || event->target_frontier)
                return lower_unsupported(f->owner, "function owner transfer requires an unsupported cleanup");
            lexical->term_target[b] = event->target == UINT32_MAX ? 0 : event->target + 1;
            return true;
        case XIR_LEXICAL_REPLACE: case XIR_LEXICAL_TAKE:
            if (lower_callable_type(f, lexical->source.bindings[event->binding].physical))
                return lower_unsupported(f->owner, "named function move transport");
            return true;
        default: return lower_unsupported(f->owner, "unsupported function lexical event");
    }
}
static bool lower_lexical_events(LowerFunction *f, uint32_t b, uint32_t head) {
    for (uint32_t n = head; n != UINT32_MAX; n = f->lexical.next[n])
        if (!lower_lexical_event(f, b, &f->lexical.source.events[n])) return false;
    return true;
}
static bool lower_lexical_block(LowerFunction *f, uint32_t b) {
    LowerLexical *lexical = &f->lexical;
    if (!lexical->count) return true;
    if (lexical->incoming[b] == UINT32_MAX)
        return lower_unsupported(f->owner, "function lexical entry has no selected predecessor");
    lexical->current = lexical->incoming[b];
    const XrXirBlock *block = &f->source->blocks[b];
    for (uint32_t p = 0; p < f->phis[b]; ++p)
        lexical->value_scopes[f->source->parameter_count + block->first + p] = lexical->current;
    return lower_lexical_events(f, b, lexical->entries[b]);
}
static void lower_lexical_value(LowerFunction *f, uint32_t i) {
    if (!f->lexical.count) return;
    const XrXirInstruction *op = &f->source->instructions[i];
    if (!lower_callable_type(f, op->type)) return;
    uint32_t result = f->source->parameter_count + i;
    f->lexical.value_scopes[result] = op->op == XR_XIR_COPY || op->op == XR_XIR_FUNCTION_WEAKEN ?
        f->lexical.value_scopes[op->args[0]] : f->lexical.current;
}
static bool lower_lexical_finish_body(LowerFunction *f, uint32_t b) {
    LowerLexical *lexical = &f->lexical;
    if (!lexical->count) return true;
    uint32_t i = f->source->blocks[b].first + f->source->blocks[b].count - 1;
    f->owner->instruction = i;
    if (lexical->after[i] != UINT32_MAX)
        return lower_unsupported(f->owner, "function extent continues beyond its terminator");
    if (!lower_lexical_events(f, b, lexical->before[i])) return false;
    lexical->term_scope[b] = lexical->current;
    uint32_t target = lexical->term_target[b];
    if (target != UINT32_MAX) {
        if (!lower_lexical_contains(lexical, target, lexical->current))
            return lower_unsupported(f->owner, "function owner transfer crosses an unrelated scope");
        lexical->current = target;
    }
    lexical->outgoing[b] = lexical->current;
    const XrXirInstruction *term = &f->source->instructions[i];
    for (uint32_t e = 0; e < lower_edge_count(term); ++e) {
        uint32_t to = term->targets[e];
        if (lexical->incoming[to] != UINT32_MAX && lexical->incoming[to] != lexical->current)
            return lower_unsupported(f->owner, "function scope differs across selected predecessors");
        lexical->incoming[to] = lexical->current;
    }
    return true;
}
static bool lower_lexical_terminator(LowerFunction *f, uint32_t b, XrLirValue *result) {
    LowerLexical *lexical = &f->lexical;
    if (!lexical->count) return true;
    lexical->current = lexical->term_scope[b];
    uint32_t target = lexical->term_target[b];
    if (target == UINT32_MAX) return true;
    while (lexical->current != target)
        if (!lower_lexical_leave(f, b, result)) return false;
    return true;
}
