/*
 * xray - Lightweight typed scripting with native concurrency
 * https://xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_cleanup_lower.inc.c - Lower checked synchronous cleanup frontiers
 */

static bool lower_cleanup_scalar(XrXirType type) {
    return type == XR_XIR_I64 || type == XR_XIR_BOOL;
}

static bool lower_cleanup_cell(const Lower *c, XrXirType type) {
    return xr_xir_cell_element(c->source->types, type) == XR_XIR_I64;
}

/* A captured binding is read in the callback, after all intervening stores. */
static const XrXirInstruction *lower_directory_cleanup_call(const Lower *c, uint32_t function) {
    const XrXirFunctionIdentity *ids = c->source->declarations->functions;
    if (function >= c->source->function_count || !ids[function].cleanup_owner) return NULL;
    const XrXirFunction *body = &c->source->functions[function];
    if (body->block_count != 1 || body->blocks[0].frontier || body->blocks[0].panic ||
        body->parameter_count != 1 || body->result != XR_XIR_UNIT || !lower_empty_error(body->error_type))
        return NULL;
    bool read = lower_cleanup_cell(c, body->parameters[0]);
    if ((!read && body->parameters[0] != XR_XIR_I64) || body->instruction_count != 2u + read ||
        body->operand_count != 1 || body->operands[0] != (uint32_t)read)
        return NULL;
    const XrXirInstruction *op = &body->instructions[read];
    if ((read && (body->instructions[0].op != XR_XIR_CELL_READ || body->instructions[0].args[0] ||
                    body->instructions[0].type != XR_XIR_I64)) ||
        op[1].op != XR_XIR_RETURN || op->op != XR_XIR_CALL || op->type != XR_XIR_BOOL ||
        op->args[0] || op->args[1] != 1 || op->immediate < 0 ||
        (uint64_t)op->immediate >= c->source->function_count)
        return NULL;
    uint32_t callee = (uint32_t)op->immediate;
    uint32_t fields[2];
    return ids[callee].native_callable == XR_NATIVE_CALLABLE_IO_DIR_CLOSE &&
        lower_host_abi(c, callee, fields) == XR_NATIVE_HOST_DIRECTORY ? op : NULL;
}

static bool lower_cleanup_literal(Lower *c, const XrXirFunction *f,
    const XrXirInstruction *literal, uint8_t **uses) {
    if (!literal || literal->op != XR_XIR_CONST_STRING) return false;
    if (!*uses) *uses = lower_literal_uses(c, f);
    uint32_t index = (uint32_t)(literal - f->instructions);
    return *uses && (*uses)[index] == 1 && lower_literal_valid(c, f, index);
}

/* This subset has no guest panic port. The two admitted output forms share
 * the host failure policy and direct literal output creates no String owner. */
static bool lower_cleanup_safe(Lower *c, const XrXirFunction *f, const XrXirInstruction *op,
    uint8_t **literal_uses) {
    uint32_t function = (uint32_t)(f - c->source->functions);
    switch (op->op) {
    case XR_XIR_CONST_BOOL: case XR_XIR_CONST_INT: case XR_XIR_PHI:
    case XR_XIR_ADD_INT: case XR_XIR_SUB_INT: case XR_XIR_MUL_INT:
        return lower_cleanup_scalar(op->type);
    case XR_XIR_CONST_STRING:
        return lower_cleanup_literal(c, f, op, literal_uses);
    case XR_XIR_SCALAR_COPY: case XR_XIR_COPY:
        return lower_cleanup_scalar(op->type) || lower_cleanup_cell(c, op->type);
    case XR_XIR_ARRAY_NEW:
        return op->args[1] == 0;
    case XR_XIR_ARRAY_LEN:
        return true;
    case XR_XIR_VIEW_CLEANUP:
        /* Retiring a temporary view loan releases only the recorded loan. */
        return true;
    case XR_XIR_CELL_PLACE: {
        const XrXirInstruction *cell = lower_place_definition(f, op->args[0]);
        return cell && cell->op == XR_XIR_CELL_NEW && lower_type(c, cell->type) == XR_LIR_ARRAY_CELL;
    }
    case XR_XIR_CELL_NEW:
        return lower_cleanup_cell(c, op->type) || lower_type(c, op->type) == XR_LIR_ARRAY_CELL;
    case XR_XIR_CELL_READ: {
        XrXirType type = xr_xir_operand_type(f, op->args[0]);
        return lower_cleanup_cell(c, type) || lower_type(c, type) == XR_LIR_ARRAY_CELL;
    }
    case XR_XIR_CELL_WRITE:
        return lower_cleanup_cell(c, xr_xir_operand_type(f, op->args[0]));
    case XR_XIR_EQUAL: case XR_XIR_EQ_INT: case XR_XIR_NE_INT:
    case XR_XIR_LT_INT: case XR_XIR_LE_INT: case XR_XIR_GT_INT: case XR_XIR_GE_INT:
        return lower_cleanup_scalar(xr_xir_operand_type(f, op->args[0]));
    case XR_XIR_ENUM_NEW:
        return op->type == lower_error_bound(c, function) && lower_is_plain_error_enum(c, op->type);
    case XR_XIR_PRINT: {
        if (op->args[1] != 1 || op->args[0] >= f->operand_count) return false;
        uint32_t value = f->operands[op->args[0]];
        return xr_xir_operand_type(f, value) == XR_XIR_I64 ||
            lower_cleanup_literal(c, f, lower_place_definition(f, value), literal_uses);
    }
    case XR_XIR_RETURN:
        return f->result == XR_XIR_UNIT || lower_cleanup_scalar(f->result);
    case XR_XIR_THROW:
        return lower_function_error(c, function) &&
            xr_xir_operand_type(f, op->args[0]) == lower_error_bound(c, function);
    case XR_XIR_JUMP: case XR_XIR_BRANCH:
    case XR_XIR_CLEANUP_REGISTER: case XR_XIR_CLEANUP_LEAVE:
        return true;
    default:
        return false;
    }
}

/* A direct leaf must also be safe before its own first registration. Each
 * registered helper is checked over its entire body by cleanup admission when
 * the owning registration puts it on the lowering queue. */
static bool lower_cleanup_direct_leaf(Lower *c, uint32_t function) {
    enum { UNKNOWN, SAFE, UNSAFE };
    if (!c->cleanup_leaves) {
        c->cleanup_leaves = lower_alloc(c, c->source->function_count, sizeof(*c->cleanup_leaves));
        if (!c->cleanup_leaves) return false;
    }
    uint8_t *proof = &c->cleanup_leaves[function];
    if (*proof != UNKNOWN) return *proof == SAFE;
    const XrXirFunction *f = &c->source->functions[function];
    uint8_t *literal_uses = NULL;
    bool safe = true;
    for (uint32_t b = 0; b < f->block_count && safe; ++b) {
        const XrXirBlock *block = &f->blocks[b];
        safe = !block->panic;
        for (uint32_t i = block->first; i < block->first + block->count && safe; ++i) {
            const XrXirInstruction *op = &f->instructions[i];
            safe = lower_cleanup_safe(c, f, op, &literal_uses) &&
                !(op->op == XR_XIR_CLEANUP_REGISTER &&
                  lower_directory_cleanup_call(c, (uint32_t)op->immediate));
        }
    }
    xr_free(literal_uses);
    *proof = safe ? SAFE : UNSAFE;
    return safe;
}

static bool lower_cleanup_admit(Lower *c, const XrXirFunction *f, const XrXirInstruction *op) {
    if (op->immediate < 0 || (uint64_t)op->immediate >= c->source->function_count ||
        op->args[0] > f->operand_count || op->args[1] > f->operand_count - op->args[0])
        return lower_unsupported(c, "invalid cleanup registration");
    uint32_t index = (uint32_t)op->immediate;
    const XrXirFunctionIdentity *ids = c->source->declarations->functions;
    const XrXirFunction *target = &c->source->functions[index];
    if (ids[index].cleanup_owner != c->function + 1 || ids[index].module != ids[c->function].module ||
        target->result != XR_XIR_UNIT || !lower_empty_error(target->error_type) ||
        target->parameter_count != op->args[1] || !target->block_count)
        return lower_unsupported(c, "invalid cleanup body role");
    for (uint32_t p = 0; p < target->parameter_count; ++p) {
        const XrXirType type = target->parameters[p];
        bool binding = lower_cleanup_cell(c, type);
        if (!binding && !lower_cleanup_scalar(type))
            return lower_unsupported(c, "cleanup capture is not a scalar or live scalar binding");
        c->scalar_bindings |= binding;
    }
    return lower_mark(c, index);
}

static bool lower_cleanup_arguments(LowerFunction *f, const XrXirInstruction *registration,
    XrLirValue **output) {
    uint32_t count = registration->args[1];
    if (!count) return true;
    XrLirValue *arguments = lower_alloc(f->owner, count, sizeof(*arguments));
    if (!arguments) return false;
    for (uint32_t a = 0; a < count; ++a) {
        uint32_t value = f->source->operands[registration->args[0] + a];
        if (!lower_value(f, value, &arguments[a])) { xr_free(arguments); return false; }
    }
    *output = arguments;
    return true;
}

static XrLirFunction lower_cleanup_target(const LowerFunction *f, const XrXirInstruction *registration) {
    return f->owner->functions[registration->immediate];
}

static uint32_t lower_cleanup_role(const LowerFunction *f, uint32_t frontier) {
    return frontier ? lower_cleanup_target(f, &f->source->instructions[frontier - 1]).id : 0;
}

static bool lower_cleanup_block(LowerFunction *f, uint32_t block, const XrXirInstruction *term) {
    Lower *c = f->owner;
    const XrXirFunction *source = f->source;
    uint32_t frontier = source->blocks[block].frontier;
    uint32_t role = lower_cleanup_role(f, frontier);
    if (term->op != XR_XIR_CLEANUP_REGISTER)
        return !frontier || lower_build(c,
            xr_lir_internal_cleanup_block(c->module, f->blocks[block], role, 0, NULL, 0));
    XrLirValue *arguments = NULL;
    if (!lower_cleanup_arguments(f, term, &arguments)) return false;
    XrLirFunction target = lower_cleanup_target(f, term);
    bool good = lower_build(c, xr_lir_internal_cleanup_block(c->module, f->blocks[block],
        role, target.id, arguments, term->args[1]));
    xr_free(arguments);
    return good;
}

static bool lower_cleanup_call(LowerFunction *f, XrLirBlock block, uint32_t frontier) {
    Lower *c = f->owner;
    const XrXirInstruction *registration = &f->source->instructions[frontier - 1];
    XrLirValue *arguments = NULL;
    if (!lower_cleanup_arguments(f, registration, &arguments)) return false;
    uint32_t at = c->instruction;
    c->instruction = frontier - 1;
    XrLirInstruction invoke = {.op = XR_LIR_CALL, .type = XR_LIR_UNIT,
        .callee = lower_cleanup_target(f, registration), .operands = arguments,
        .operand_count = registration->args[1], .source = lower_source(c)};
    bool good = lower_build(c, xr_lir_emit_effect(c->module, block, &invoke));
    c->instruction = at;
    xr_free(arguments);
    return good;
}

static bool lower_cleanup_unwind(LowerFunction *f, XrLirBlock block, uint32_t frontier, uint32_t target,
    bool *directory_closed) {
    bool ordinary_cleanup = false;
    while (frontier != target) {
        uint32_t parent, handler;
        if (xr_xir_cleanup_frontier(f->source, frontier, &parent, &handler) != XR_XIR_OK || handler)
            return lower_unsupported(f->owner, "invalid synchronous cleanup frontier");
        const XrXirInstruction *registration = &f->source->instructions[frontier - 1];
        if (lower_directory_cleanup_call(f->owner, (uint32_t)registration->immediate)) {
            if (!directory_closed || ordinary_cleanup)
                return lower_unsupported(f->owner, "directory cleanup requires a complete normal exit");
            *directory_closed = true;
        } else {
            if (directory_closed && *directory_closed)
                return lower_unsupported(f->owner, "mixed directory cleanup frontier");
            ordinary_cleanup = true;
        }
        if (!lower_cleanup_call(f, block, frontier)) return false;
        frontier = parent;
    }
    return true;
}

static bool lower_cleanup_frame_type(const Lower *c, XrXirType type) {
    return type == XR_XIR_UNIT || lower_cleanup_scalar(type) || lower_cleanup_cell(c, type);
}


/* The first checked Array site has no earlier owning input to retire. A
 * forward-only body with one producer cannot bring a previous Array back
 * to the split. All non-scalar values belong to this result and its local
 * binding. The failure suffix can therefore run in the current activation. */
static const XrXirInstruction *lower_empty_array_site(const Lower *c, const XrXirFunction *f) {
    uint32_t function = (uint32_t)(f - c->source->functions);
    if ((c->closure_types && c->closure_types[function]) ||
        c->source->declarations->functions[function].cleanup_owner ||
        !lower_empty_error(f->error_type) || !lower_cleanup_frame_type(c, f->result))
        return NULL;
    for (uint32_t p = 0; p < f->parameter_count; ++p)
        if (!lower_cleanup_scalar(f->parameters[p])) return NULL;

    const XrXirInstruction *site = NULL;
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrXirBlock *block = &f->blocks[b];
        if (block->panic || !block->count) return NULL;
        const XrXirInstruction *term = &f->instructions[block->first + block->count - 1];
        for (uint32_t e = 0; e < lower_edge_count(c, term); ++e)
            if (term->targets[e] <= b) return NULL;
        for (const XrXirInstruction *op = &f->instructions[block->first]; op < term; ++op) {
            if (op->op != XR_XIR_ARRAY_NEW) continue;
            if (site || op->args[1] || !block->frontier) return NULL;
            site = op;
        }
    }
    if (!site || !lower_array_element(c, site->type)) return NULL;
    for (uint32_t i = 0; i < f->instruction_count; ++i) {
        const XrXirInstruction *op = &f->instructions[i];
        if (op == site || lower_cleanup_frame_type(c, op->type)) continue;
        if (op->op == XR_XIR_CELL_NEW &&
            xr_xir_cell_element(c->source->types, op->type) == site->type) continue;
        if ((op->op == XR_XIR_CELL_READ || op->op == XR_XIR_CELL_PLACE) && op->type == site->type)
            continue;
        return NULL;
    }
    return site;
}

static const XrXirInstruction *lower_allocation_cleanup_print(const Lower *c, uint32_t role) {
    if (role >= c->source->function_count || !c->source->declarations->functions[role].cleanup_owner)
        return NULL;
    const XrXirFunction *body = &c->source->functions[role];
    if (body->block_count != 1 || body->blocks[0].frontier || body->blocks[0].panic ||
        body->parameter_count > 1 || body->result != XR_XIR_UNIT || !lower_empty_error(body->error_type) ||
        body->instruction_count < 2 || body->instruction_count > 3 || body->operand_count != 1)
        return NULL;
    const XrXirInstruction *first = body->instructions;
    const XrXirInstruction *print = first + (body->instruction_count - 2);
    if (print->op != XR_XIR_PRINT || print->args[0] || print->args[1] != 1 || print[1].op != XR_XIR_RETURN)
        return NULL;
    if (print == first)
        return body->parameter_count == 1 && body->parameters[0] == XR_XIR_I64 &&
            !body->operands[0] ? print : NULL;
    if (first->type != XR_XIR_I64 || body->operands[0] != body->parameter_count) return NULL;
    if (first->op == XR_XIR_CONST_INT) return body->parameter_count ? NULL : print;
    return first->op == XR_XIR_CELL_READ && !first->args[0] && body->parameter_count == 1 &&
        lower_cleanup_cell(c, body->parameters[0]) ? print : NULL;
}

static bool lower_allocation_cleanup_shape(const Lower *c, const XrXirFunction *f, uint32_t frontier) {
    while (frontier) {
        uint32_t parent, handler;
        if (xr_xir_cleanup_frontier(f, frontier, &parent, &handler) != XR_XIR_OK || handler ||
            !lower_allocation_cleanup_print(c, (uint32_t)f->instructions[frontier - 1].immediate)) return false;
        frontier = parent;
    }
    return true;
}

static bool lower_allocation_cleanup_chain(LowerFunction *f, XrLirBlock block, uint32_t frontier) {
    Lower *c = f->owner;
    while (frontier) {
        const XrXirInstruction *registration = &f->source->instructions[frontier - 1];
        uint32_t role = (uint32_t)registration->immediate;
        const XrXirInstruction *print = lower_allocation_cleanup_print(c, role);
        if (!print) return false;
        const XrXirFunction *body = &c->source->functions[role];
        const XrXirInstruction *first = body->instructions;
        uint32_t function = c->function, instruction = c->instruction;
        c->function = role;
        c->instruction = 0;
        XrLirSource source = lower_source(c);
        c->instruction = (uint32_t)(print - first);
        XrLirSource print_source = lower_source(c);
        c->function = function;
        c->instruction = instruction;

        XrLirValue value = {0};
        if (body->parameter_count &&
            !lower_value(f, f->source->operands[registration->args[0]], &value)) return false;
        if (print != first) {
            XrLirInstruction input = {.op=XR_LIR_CONST,.type=XR_LIR_I64,.source=source};
            if (first->op == XR_XIR_CELL_READ) {
                input.op = XR_LIR_CELL_READ;
                input.operands = &value;
                input.operand_count = 1;
            } else input.bits = (uint64_t)first->immediate;
            XrLirValue evaluated = {0};
            if (!lower_build(c, xr_lir_emit_value(c->module, block, &input, &evaluated))) return false;
            value = evaluated;
        }
        XrLirInstruction output = {.op=XR_LIR_CALL,.type=XR_LIR_UNIT,.callee=c->print,
            .operands=&value,.operand_count=1,.source=print_source};
        if (!lower_build(c, xr_lir_emit_effect(c->module, block, &output))) return false;
        uint32_t parent, handler;
        if (xr_xir_cleanup_frontier(f->source, frontier, &parent, &handler) != XR_XIR_OK || handler)
            return false;
        frontier = parent;
    }
    return true;
}

static bool lower_checked_empty_array(LowerFunction *f, uint32_t b, uint32_t result,
    XrLirType element, uint32_t intrinsic) {
    Lower *c = f->owner;
    uint32_t frontier = f->source->blocks[b].frontier;
    if (!f->exits) {
        f->exits = lower_alloc(c, f->source->block_count, sizeof(*f->exits));
        if (!f->exits) return false;
    }
    XrLirBlock check = f->blocks[b], normal = {0}, failure = {0};
    XrLirType array_type = XR_LIR_ARRAY, failure_type = XR_LIR_U64;
    XrLirValue status = {0};
    XrLirArrayShape shape = {element};
    XrLirSource source = lower_source(c);
    if (!lower_build(c, xr_lir_add_block(c->module, f->function, &array_type, 1, &normal)) ||
        !lower_build(c, xr_lir_add_block(c->module, f->function, &failure_type, 1, &failure)) ||
        !lower_build(c, xr_lir_block_parameter(c->module, normal, 0, &f->values[result])) ||
        !lower_build(c, xr_lir_set_array_shape(c->module, f->values[result], &shape)) ||
        !lower_build(c, xr_lir_block_parameter(c->module, failure, 0, &status)) ||
        !lower_build(c, xr_lir_internal_cleanup_block(c->module, check, lower_cleanup_role(f, frontier),
            0, NULL, 0))) return false;
    XrLirInstruction construct = {.op=XR_LIR_CHECKED_EMPTY_ARRAY,.type=array_type,.bits=intrinsic,
        .edge_count=2,.edges={{.target=normal},{.target=failure}},.source=source};
    XrLirInstruction fail = {.op=XR_LIR_MANAGED_FAILURE,.type=XR_LIR_UNIT,
        .operands=&status,.operand_count=1,.source=source};
    if (!lower_build(c, xr_lir_set_terminator(c->module, check, &construct)) ||
        !lower_allocation_cleanup_chain(f, failure, frontier) ||
        !lower_build(c, xr_lir_set_terminator(c->module, failure, &fail))) return false;
    f->blocks[b] = normal;
    return true;
}

static bool lower_directory_read(const Lower *c, const XrXirFunction *f, const XrXirInstruction *call) {
    if (call->op != XR_XIR_CALL ||
        xr_xir_nullable_element(c->source->types, call->type) != XR_XIR_STRING ||
        call->args[1] != 1 || call->args[0] >= f->operand_count || call->immediate < 0 ||
        (uint64_t)call->immediate >= c->source->function_count) return false;
    uint32_t callee = (uint32_t)call->immediate, fields[2];
    return c->source->declarations->functions[callee].native_callable == XR_NATIVE_CALLABLE_IO_DIR_NEXT &&
        lower_host_abi(c, callee, fields) == XR_NATIVE_HOST_DIRECTORY &&
        xr_xir_operand_type(f, f->operands[call->args[0]]) == XR_XIR_I64;
}

static bool lower_directory_frontier(const Lower *c, const XrXirFunction *f, uint32_t frontier) {
    while (frontier) {
        uint32_t parent, handler;
        if (xr_xir_cleanup_frontier(f, frontier, &parent, &handler) != XR_XIR_OK || handler ||
            !lower_directory_cleanup_call(c, (uint32_t)f->instructions[frontier - 1].immediate)) return false;
        frontier = parent;
    }
    return true;
}

/* A directory result may pass through a local cell before its adjacent use.
 * The consuming patterns verify that there are no other uses of that cell. */
static const XrXirInstruction *lower_directory_operand(const Lower *c, const XrXirFunction *f,
    const XrXirBlock *block, uint32_t index) {
    const XrXirInstruction *use = &f->instructions[index];
    uint32_t id = f->parameter_count + index;
    if (index == block->first || use->args[0] != id - 1) return NULL;
    const XrXirInstruction *call = use - 1;
    if (call->op == XR_XIR_CELL_READ) {
        if (index - block->first < 3 || call->args[0] != id - 2 ||
            call[-1].op != XR_XIR_CELL_NEW || call[-1].args[0] != id - 3 ||
            xr_xir_cell_element(c->source->types, call[-1].type) != call->type ||
            call[-2].type != call->type) return NULL;
        call -= 2;
    }
    return lower_directory_read(c, f, call) ? call : NULL;
}

static const XrXirInstruction *lower_directory_tail(const Lower *c, const XrXirFunction *f, uint32_t b) {
    const XrXirBlock *block = &f->blocks[b];
    if (!block->frontier || block->panic || block->count < 2 ||
        xr_xir_nullable_element(c->source->types, f->result) != XR_XIR_STRING) return NULL;
    uint32_t at = block->first + block->count - 1;
    if (f->instructions[at].op != XR_XIR_RETURN) return NULL;
    const XrXirInstruction *call = lower_directory_operand(c, f, block, at);
    return call && call->type == f->result &&
        lower_directory_frontier(c, f, block->frontier) ? call : NULL;
}

static const XrXirInstruction *lower_directory_tail_frame(const Lower *c, const XrXirFunction *f) {
    if (!lower_empty_error(f->error_type) ||
        xr_xir_nullable_element(c->source->types, f->result) != XR_XIR_STRING) return NULL;
    for (uint32_t p = 0; p < f->parameter_count; ++p)
        if (!lower_cleanup_scalar(f->parameters[p])) return NULL;
    const XrXirInstruction *first_tail = NULL;
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrXirBlock *block = &f->blocks[b];
        const XrXirInstruction *tail = lower_directory_tail(c, f, b);
        if (block->panic || !block->count ||
            (f->instructions[block->first + block->count - 1].op == XR_XIR_RETURN && !tail)) return NULL;
        for (uint32_t i = block->first; i < block->first + block->count; ++i)
            if ((!tail || &f->instructions[i] < tail) && !lower_cleanup_frame_type(c, f->instructions[i].type))
                return NULL;
        if (!first_tail) first_tail = tail;
    }
    return first_tail;
}

static bool lower_directory_loop_value(LowerDirectoryLoop loop, const XrXirInstruction *op) {
    return loop.call && op >= loop.call && op <= loop.tag;
}

static bool lower_directory_projected_value(const XrXirInstruction *tail, LowerDirectoryLoop loop,
    const XrXirInstruction *op) {
    return (tail && op >= tail) || lower_directory_loop_value(loop, op);
}

static const XrXirInstruction *lower_directory_tag_call(const Lower *c, const XrXirFunction *f,
    const XrXirBlock *block, uint32_t tag_index) {
    const XrXirInstruction *tag = &f->instructions[tag_index];
    uint32_t value = f->parameter_count + tag_index;
    if (tag->op != XR_XIR_NULLABLE_IS_SOME || (tag->immediate != 0 && tag->immediate != 1) ||
        tag[1].op != XR_XIR_BRANCH || tag[1].args[0] != value) return NULL;
    return lower_directory_operand(c, f, block, tag_index);
}

static bool lower_directory_loop_repeats(Lower *c, const XrXirFunction *f, LowerDirectoryLoop site) {
    uint32_t start = site.tag[1].targets[site.tag->immediate != 0];
    uint32_t frontier = f->blocks[site.block].frontier;
    if (f->blocks[start].frontier != frontier) return false;
    uint8_t *seen = lower_alloc(c, f->block_count, 1);
    uint32_t *pending = lower_alloc(c, f->block_count, sizeof(*pending));
    bool repeats = false;
    if (seen && pending) {
        uint32_t count = 1;
        pending[0] = start;
        seen[start] = 1;
        for (uint32_t at = 0; at < count && !repeats; ++at) {
            uint32_t b = pending[at];
            repeats = b == site.block;
            const XrXirBlock *block = &f->blocks[b];
            const XrXirInstruction *term = &f->instructions[block->first + block->count - 1];
            uint32_t edges = term->op == XR_XIR_JUMP ? 1u : term->op == XR_XIR_BRANCH ? 2u : 0u;
            for (uint32_t e = 0; e < edges; ++e) {
                uint32_t target = term->targets[e];
                if (seen[target] || f->blocks[target].frontier != frontier) continue;
                seen[target] = 1;
                pending[count++] = target;
            }
        }
    }
    xr_free(pending);
    xr_free(seen);
    return repeats;
}

/* Constructing the array still requires an inactive cleanup frontier. */
static bool lower_directory_prefix_value(const Lower *c, const XrXirBlock *block, XrXirType type) {
    if (block->frontier) return false;
    if (type == XR_XIR_STRING || lower_array_element(c, type) == XR_LIR_STRING) return true;
    XrXirType element = xr_xir_cell_element(c->source->types, type);
    return lower_array_element(c, element) == XR_LIR_STRING;
}

static bool lower_directory_name_path(Lower *c, const XrXirFunction *f, LowerDirectoryLoop site, uint32_t use) {
    uint8_t *incoming = lower_alloc(c, f->block_count, 1);
    if (!incoming) return false;
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrXirBlock *block = &f->blocks[b];
        const XrXirInstruction *term = &f->instructions[block->first + block->count - 1];
        uint32_t edges = lower_edge_count(c, term);
        for (uint32_t e = 0; e < edges; ++e) {
            uint32_t target = term->targets[e];
            if (incoming[target] < 2) ++incoming[target];
        }
    }
    uint32_t frontier = f->blocks[site.block].frontier;
    uint32_t b = site.tag[1].targets[site.tag->immediate != 0];
    bool known = false;
    for (uint32_t depth = 0; depth < f->block_count; ++depth) {
        const XrXirBlock *block = &f->blocks[b];
        if (incoming[b] != 1 || block->frontier != frontier) break;
        if (b == use) { known = true; break; }
        const XrXirInstruction *term = &f->instructions[block->first + block->count - 1];
        if (term->op != XR_XIR_JUMP) break;
        b = term->targets[0];
    }
    xr_free(incoming);
    return known;
}

/* Directory opening borrows its Path before the first registration. The
 * ordinary managed value flow retires the retained formal in that prefix. */
static bool lower_directory_open_formal(const Lower *c, const XrXirFunction *f, uint32_t formal) {
    if (!xr_xir_type_is_class(c->source->types, f->parameters[formal])) return false;
    bool opened = false;
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrXirBlock *block = &f->blocks[b];
        for (uint32_t i = block->first; i < block->first + block->count; ++i) {
            const XrXirInstruction *op = &f->instructions[i];
            if (xr_xir_op_uses_owner_slots(op->op)) continue;
            uint32_t count = xir_instruction_operand_count(f, op, c->source);
            const uint32_t *operands = op->args;
            if (count && xr_xir_op_uses_operand_table(op->op)) {
                if (op->args[0] > f->operand_count || count > f->operand_count - op->args[0]) return false;
                operands = f->operands + op->args[0];
            }
            uint32_t step = op->op == XR_XIR_PHI ? 2u : 1u;
            for (uint32_t a = step - 1; a < count; a += step) {
                if (operands[a] != formal) continue;
                if (block->frontier || opened || op->op != XR_XIR_CALL || op->args[1] != 1 ||
                    op->immediate < 0 || (uint64_t)op->immediate >= c->source->function_count) return false;
                uint32_t callee = (uint32_t)op->immediate, fields[2];
                if (c->source->declarations->functions[callee].native_callable != XR_NATIVE_CALLABLE_IO_DIR_OPEN ||
                    lower_host_abi(c, callee, fields) != XR_NATIVE_HOST_DIRECTORY) return false;
                opened = true;
            }
        }
    }
    return opened;
}

/* The tag controls the immediately following branch. An immutable result may
 * supply a length on its present path before that iteration retires it. */
static LowerDirectoryLoop lower_directory_loop(Lower *c, const XrXirFunction *f) {
    LowerDirectoryLoop site = {0}, absent = {0};
    if (f->result != XR_XIR_I64 || !lower_empty_error(f->error_type)) return absent;
    for (uint32_t p = 0; p < f->parameter_count; ++p)
        if (!lower_cleanup_scalar(f->parameters[p]) && !lower_directory_open_formal(c, f, p)) return absent;
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrXirBlock *block = &f->blocks[b];
        if (block->panic || !block->count) return absent;
        if (!block->frontier || block->count < 3) continue;
        uint32_t tag = block->first + block->count - 2;
        const XrXirInstruction *call = lower_directory_tag_call(c, f, block, tag);
        if (!call) continue;
        if (site.call || !lower_directory_frontier(c, f, block->frontier)) return absent;
        site = (LowerDirectoryLoop){.call=call,.tag=&f->instructions[tag],.block=b};
    }
    if (!site.call) return absent;
    uint32_t first = f->parameter_count + (uint32_t)(site.call - f->instructions);
    uint32_t last = f->parameter_count + (uint32_t)(site.tag - f->instructions);
    uint32_t name = UINT32_MAX, name_block = UINT32_MAX;
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrXirBlock *block = &f->blocks[b];
        const XrXirInstruction *term = &f->instructions[block->first + block->count - 1];
        if (term->op == XR_XIR_CLEANUP_LEAVE ||
            (term->op == XR_XIR_CLEANUP_REGISTER &&
             !lower_directory_cleanup_call(c, (uint32_t)term->immediate))) return absent;
        for (uint32_t i = block->first; i < block->first + block->count; ++i) {
            const XrXirInstruction *op = &f->instructions[i];
            if (lower_directory_loop_value(site, op)) continue;
            if (op->op == XR_XIR_NULLABLE_UNWRAP && op->type == XR_XIR_STRING &&
                op->args[0] == first && site.tag->args[0] == first) {
                if (site.unwrap || b == site.block) return absent;
                site.unwrap = op;
                name = f->parameter_count + i;
                name_block = b;
                continue;
            }
            if (site.unwrap && op->op == XR_XIR_STRING_LEN && op->args[0] == name) {
                if (site.length || b != name_block) return absent;
                site.length = op;
            }
            bool place = b != site.block && op->op == XR_XIR_CELL_PLACE &&
                lower_array_element(c, op->type) == XR_LIR_STRING &&
                lower_type(c, xr_xir_operand_type(f, op->args[0])) == XR_LIR_ARRAY_CELL;
            if ((!lower_cleanup_frame_type(c, op->type) && !lower_directory_prefix_value(c, block, op->type) &&
                 !place) ||
                op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_INVOKE_INDIRECT) return absent;
            if (op == site.tag + 1) continue;
            uint32_t count = xir_instruction_operand_count(f, op, c->source);
            const uint32_t *operands = op->args;
            if (xr_xir_op_uses_operand_table(op->op) && count) {
                if (op->args[0] > f->operand_count || count > f->operand_count - op->args[0]) return absent;
                operands = &f->operands[op->args[0]];
            }
            uint32_t step = op->op == XR_XIR_PHI ? 2u : 1u;
            for (uint32_t a = step - 1; a < count; a += step)
                if (operands[a] >= first && operands[a] <= last) return absent;
        }
    }
    if (site.unwrap) {
        if (!site.length || !lower_directory_name_path(c, f, site, name_block)) return absent;
        uint8_t *uses = lower_alloc(c, f->instruction_count, 1);
        if (!uses) return absent;
        lower_count_operand_uses(c, f, uses);
        bool one_use = uses[name - f->parameter_count] == 1;
        xr_free(uses);
        if (!one_use) return absent;
    }
    return lower_directory_loop_repeats(c, f, site) ? site : absent;
}

static XrLirSource lower_directory_source(Lower *c, uint32_t function, const XrXirInstruction *call) {
    uint32_t owner = c->function, at = c->instruction;
    c->function = function;
    c->instruction = (uint32_t)(call - c->source->functions[function].instructions);
    XrLirSource source = lower_call_point(c);
    c->instruction = at;
    c->function = owner;
    return source;
}

static bool lower_directory_close_chain(LowerFunction *f, XrLirBlock block, uint32_t frontier,
    XrLirValue *status) {
    Lower *c = f->owner;
    while (frontier) {
        const XrXirInstruction *registration = &f->source->instructions[frontier - 1];
        uint32_t role = (uint32_t)registration->immediate;
        const XrXirInstruction *close = lower_directory_cleanup_call(c, role);
        XrLirValue input[2] = {{0}, *status};
        uint32_t capture = f->source->operands[registration->args[0]];
        if (!close || !lower_value(f, capture, &input[0])) return false;
        XrLirSource source = lower_directory_source(c, role, close);
        if (lower_cleanup_cell(c, c->source->functions[role].parameters[0])) {
            XrLirInstruction load = {.op=XR_LIR_CELL_READ,.type=XR_LIR_I64,
                .operands=input,.operand_count=1,.source=source};
            XrLirValue loaded = {0};
            if (!lower_build(c, xr_lir_emit_value(c->module, block, &load, &loaded))) return false;
            input[0] = loaded;
        }
        XrLirInstruction step = {.op=XR_LIR_HOST_DIRECTORY_CLOSE_CHECKED,.type=XR_LIR_U64,
            .bits=lower_cleanup_target(f, registration).id,.operands=input,
            .operand_count=status->id ? 2u : 1u,.source=source};
        if (!lower_build(c, xr_lir_emit_value(c->module, block, &step, status))) return false;
        uint32_t parent, handler;
        if (xr_xir_cleanup_frontier(f->source, frontier, &parent, &handler) != XR_XIR_OK || handler) return false;
        frontier = parent;
    }
    return true;
}

static bool lower_directory_split(LowerFunction *f, uint32_t b, const XrXirInstruction *call,
    XrLirBlock *normal, XrLirValue *result) {
    Lower *c = f->owner;
    XrLirSource source = lower_directory_source(c, c->function, call);
    XrLirType type = 0, status = XR_LIR_U64;
    XrLirBlock failure = {0};
    XrLirValue token = {0}, failed[2] = {{0}};
    if (!lower_build(c, xr_lir_nullable_type(c->module, XR_LIR_STRING, &type)) ||
        !lower_value(f, f->source->operands[call->args[0]], &token) ||
        !lower_build(c, xr_lir_add_block(c->module, f->function, &type, 1, normal)) ||
        !lower_build(c, xr_lir_add_block(c->module, f->function, &status, 1, &failure)) ||
        !lower_build(c, xr_lir_block_parameter(c->module, *normal, 0, result)) ||
        !lower_build(c, xr_lir_block_parameter(c->module, failure, 0, &failed[0])) ||
        !lower_directory_close_chain(f, failure, f->source->blocks[b].frontier, &failed[1]) ||
        (f->directory_loop.call && !lower_directory_cells(f, b, failure, source))) return false;
    XrLirInstruction next = {.op=XR_LIR_HOST_DIRECTORY_NEXT_CHECKED,.type=type,.operands=&token,
        .operand_count=1,.edge_count=2,.edges={{.target=*normal},{.target=failure}},.source=source};
    XrLirInstruction fail = {.op=XR_LIR_HOST_DIRECTORY_FAIL,.type=XR_LIR_UNIT,
        .operands=failed,.operand_count=2,.source=source};
    return lower_build(c, xr_lir_set_terminator(c->module, f->blocks[b], &next)) &&
        lower_build(c, xr_lir_set_terminator(c->module, failure, &fail));
}

static bool lower_directory_next(LowerFunction *f, uint32_t b, const XrXirInstruction *call) {
    Lower *c = f->owner;
    XrLirSource exit = lower_source(c);
    XrLirBlock normal = {0};
    XrLirValue success[2] = {{0}};
    if (!lower_directory_split(f, b, call, &normal, &success[0]) ||
        !lower_directory_close_chain(f, normal, f->source->blocks[b].frontier, &success[1])) return false;
    XrLirInstruction guard = {.op=XR_LIR_HOST_DIRECTORY_RETURN_GUARD,.type=XR_LIR_UNIT,
        .operands=success,.operand_count=2,.source=exit};
    XrLirInstruction ret = {.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=success,.operand_count=1,.source=exit};
    return lower_build(c, xr_lir_emit_effect(c->module, normal, &guard)) &&
        lower_build(c, xr_lir_set_terminator(c->module, normal, &ret));
}

/* Physical failure may leave this activation after its defer callbacks. Keep
 * frames with owners, loans or callable scopes on the existing refusal path
 * until they can retire those obligations on the new successor. */
static bool lower_cleanup_scalar_frame(const Lower *c, const XrXirFunction *f) {
    if (!lower_empty_error(f->error_type) || !lower_cleanup_frame_type(c, f->result)) return false;
    for (uint32_t p = 0; p < f->parameter_count; ++p)
        if (!lower_cleanup_scalar(f->parameters[p])) return false;
    for (uint32_t i = 0; i < f->instruction_count; ++i)
        if (!lower_cleanup_frame_type(c, f->instructions[i].type)) return false;
    return true;
}

static bool lower_cleanup_exit(LowerFunction *f, uint32_t block, const XrXirInstruction *term,
    bool *directory_closed) {
    uint32_t frontier = f->source->blocks[block].frontier;
    if (!frontier) return true;
    uint32_t target = 0;
    if (term->op == XR_XIR_CLEANUP_LEAVE || term->op == XR_XIR_CLEANUP_ERROR)
        target = (uint32_t)term->immediate;
    else if (term->op != XR_XIR_RETURN && term->op != XR_XIR_THROW && term->op != XR_XIR_PANIC_ERROR) return true;
    bool normal = (term->op == XR_XIR_RETURN || term->op == XR_XIR_CLEANUP_LEAVE) && !target;
    if (!lower_cleanup_unwind(f, f->blocks[block], frontier, target, normal ? directory_closed : NULL))
        return false;
    if (!*directory_closed || f->directory_scalar_frame) return true;
    f->directory_scalar_frame = lower_cleanup_scalar_frame(f->owner, f->source);
    return f->directory_scalar_frame || lower_unsupported(f->owner, "directory cleanup requires a scalar frame");
}

static bool lower_cleanup_arithmetic(const XrXirInstruction *op) {
    return (op->op == XR_XIR_DIV_INT || op->op == XR_XIR_REM_INT) && op->type == XR_XIR_I64;
}

static bool lower_cleanup_arithmetic_edge(LowerFunction *f, uint32_t b, const XrXirInstruction *op) {
    Lower *c = f->owner;
    const uint32_t frontier = f->source->blocks[b].frontier;
    const uint32_t role = lower_cleanup_role(f, frontier);
    XrLirSource source = lower_source(c);
    XrLirBlock check = f->blocks[b], nonzero = {0}, zero = {0};
    XrLirValue arguments[2] = {{0}}, condition = {0};
    if (!f->exits) {
        f->exits = lower_alloc(c, f->source->block_count, sizeof(*f->exits));
        if (!f->exits) return false;
    }
    XrLirInstruction constant = {.op = XR_LIR_CONST, .type = XR_LIR_I64, .source = source};
    XrLirInstruction compare = {.op = XR_LIR_ICMP, .type = XR_LIR_I1, .predicate = XR_LIR_NE,
        .operands = arguments, .operand_count = 2, .source = source};
    if (!lower_value(f, op->args[1], &arguments[0]) ||
        !lower_build(c, xr_lir_emit_value(c->module, check, &constant, &arguments[1])) ||
        !lower_build(c, xr_lir_emit_value(c->module, check, &compare, &condition)) ||
        !lower_build(c, xr_lir_add_block(c->module, f->function, NULL, 0, &nonzero)) ||
        !lower_build(c, xr_lir_add_block(c->module, f->function, NULL, 0, &zero)) ||
        !lower_build(c, xr_lir_internal_cleanup_block(c->module, check, role, 0, NULL, 0)) ||
        !lower_build(c, xr_lir_internal_cleanup_block(c->module, zero, role, 0, NULL, 0))) return false;
    XrLirInstruction branch = {.op = XR_LIR_CONDBR, .type = XR_LIR_UNIT, .operands = &condition,
        .operand_count = 1, .edge_count = 2, .edges = {{.target = nonzero}, {.target = zero}}, .source = source};
    XrLirInstruction panic = {.op = XR_LIR_ARITHMETIC_PANIC, .type = XR_LIR_UNIT,
        .bits = op->op == XR_XIR_DIV_INT ? 420 : 421, .source = source};
    if (!lower_build(c, xr_lir_set_terminator(c->module, check, &branch)) ||
        !lower_cleanup_unwind(f, zero, frontier, 0, NULL) ||
        !lower_build(c, xr_lir_set_terminator(c->module, zero, &panic))) return false;
    f->blocks[b] = nonzero;
    return true;
}
