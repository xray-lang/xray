/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_lower_view.inc.c - Natural readonly windows on actual bindings
 */
static XrLirType lower_slice_element(const Lower *c, XrXirType type) {
    const XrXirTypeNode *node = xr_xir_type_node(c->source->types, type);
    return node && node->kind == XR_XIR_TYPE_SLICE ? lower_type(c, node->element) : XR_LIR_TYPE_INVALID;
}
static bool lower_slice_call(LowerFunction *f, uint32_t block, XrLirSliceOperation role,
    XrLirType element, const XrLirValue *args, uint32_t count, XrLirValue *output) {
    Lower *c = f->owner; uint32_t id = 0; XrLirType binding = XR_LIR_TYPE_INVALID;
    if (role == XR_LIR_SLICE_OP_NEW) {
        binding = c->module->functions[f->function.id - 1].values[args[0].id - 1].type;
        if (count == 1 && binding != XR_LIR_STRING_CELL)
            return lower_unsupported(c, "readonly String byte view of a nonlocal binding");
    }
    if (!lower_build(c, xr_lir_slice_operation(c->module, role, element, binding, &id))) return false;
    const XrLirIntrinsic *entry = xr_lir_internal_slice_intrinsic(c->module, id);
    if (!entry) return lower_unsupported(c, "Slice role lost its receiving signature");
    XrLirType result = entry->result;
    XrLirInstruction call = {.op=XR_LIR_CALL_INTRINSIC,.type=result,.intrinsic=id,
        .operands=args,.operand_count=count,.source=lower_source(c)};
    return lower_build(c, result == XR_LIR_UNIT ? xr_lir_emit_effect(c->module, f->blocks[block], &call) :
        xr_lir_emit_value(c->module, f->blocks[block], &call, output));
}
static bool lower_slice_source(LowerFunction *f, uint32_t ordinal, XrLirValue *output) {
    if (ordinal >= f->value_count) return lower_unsupported(f->owner, "Slice cleanup source ordinal");
    if (f->cell_for[ordinal] != UINT32_MAX &&
        lower_slice_element(f->owner, xr_xir_operand_type(f->source, ordinal))) {
        *output = f->current[f->cell_for[ordinal]];
        return output->id != 0 || lower_unsupported(f->owner, "Slice local has no live SSA binding");
    }
    return lower_value(f, ordinal, output);
}
static bool lower_full_string_view(LowerFunction *f, const XrXirInstruction *op, XrLirType element) {
    Lower *c = f->owner;
    const uint32_t *args = f->source->operands + op->args[0];
    if (op->immediate != 1 || element != XR_LIR_U8 ||
        xr_xir_cell_element(c->source->types, xr_xir_operand_type(f->source, args[0])) != XR_XIR_STRING)
        return false;
    for (uint32_t a = 1; a < 3; ++a) {
        if (args[a] < f->source->parameter_count) return false;
        uint32_t index = args[a] - f->source->parameter_count;
        if (index >= f->source->instruction_count || !lower_byte_length_zero(&f->source->instructions[index]))
            return false;
    }
    return true;
}
static bool lower_view_empty_entry(LowerFunction *f, uint32_t block) {
    if (block || f->owner->instruction) return false;
    for (uint32_t p = 0; p < f->source->parameter_count; ++p)
        if (lower_slice_element(f->owner,f->source->parameters[p])) return false;
    for (uint32_t b = 0; b < f->source->block_count; ++b) if (f->reachable[b]) {
        const XrXirInstruction *term = lower_term(f->owner, f->source, b);
        if (!term) return false;
        for (uint32_t e = 0; e < lower_edge_count(f->owner,term); ++e)
            if (!term->targets[e]) return false;
    }
    /* No edge can revisit this activation entry, and no formal loan has a
     * logical death to preserve. Every local keeper is still truly empty. */
    return true;
}
static void *lower_view_reserve(Lower *c, void *storage, size_t count, size_t *capacity, size_t size) {
    if (count < *capacity) return storage;
    size_t next = *capacity ? *capacity : 4;
    if (next > SIZE_MAX / 2 || next * 2 > SIZE_MAX / size) {
        lower_error(c, XR_LIR_OUT_OF_MEMORY, "Slice cleanup storage capacity"); return NULL;
    }
    next *= 2;
    void *grown = xr_realloc(storage, next * size);
    if (!grown) { lower_error(c, XR_LIR_OUT_OF_MEMORY, "Slice cleanup storage allocation"); return NULL; }
    *capacity = next; return grown;
}
static bool lower_view_store(LowerFunction *f, uint32_t cell, XrLirValue value) {
    LowerViews *views = &f->views;
    if (!views->heads) {
        views->heads = lower_alloc(f->owner, f->cell_count, sizeof(*views->heads));
        if (!views->heads) return false;
        for (uint32_t n = 0; n < f->cell_count; ++n) views->heads[n] = SIZE_MAX;
    }
    void *grown = lower_view_reserve(f->owner, views->stores, views->store_count,
        &views->store_capacity, sizeof(*views->stores));
    if (!grown) return false;
    views->stores = grown;
    views->stores[views->store_count] = (LowerViewStore){value,views->heads[cell]};
    views->heads[cell] = views->store_count++;
    return true;
}
static bool lower_view_cleanup(LowerFunction *f, uint32_t block, const XrXirInstruction *op) {
    if (lower_view_empty_entry(f, block)) return true;
    for (uint32_t n = 0; n < op->args[1]; ++n) {
        uint32_t ordinal = f->source->operands[op->args[0] + n];
        if (ordinal >= f->value_count) return lower_unsupported(f->owner, "Slice cleanup source ordinal");
        if (lower_byte_length_elided_view(lower_byte_length_plan(f->owner), f->source, ordinal)) continue;
        if (ordinal >= f->source->parameter_count && f->value_blocks && f->value_blocks[ordinal] == UINT32_MAX) continue;
        XrLirType element = lower_slice_element(f->owner, xr_xir_operand_type(f->source, ordinal));
        if (!element) return lower_unsupported(f->owner, "Slice cleanup requires a natural readonly Array loan");
        LowerViews *views = &f->views;
        void *grown = lower_view_reserve(f->owner, views->cleanups, views->cleanup_count,
            &views->cleanup_capacity, sizeof(*views->cleanups));
        if (!grown) return false;
        views->cleanups = grown;
        uint32_t position = f->owner->module->functions[f->function.id - 1].blocks[f->blocks[block].id - 1].instruction_count;
        views->cleanups[views->cleanup_count++] = (LowerViewCleanup){
            .insertion={.block=f->blocks[block],.ordinal=position,.source=lower_source(f->owner)},.owner=ordinal};
    }
    return true;
}
static bool lower_view_local(LowerFunction *f, uint32_t block, uint32_t index, bool *handled) {
    const XrXirInstruction *op = &f->source->instructions[index];
    bool create = op->op == XR_XIR_LOCAL_NEW, read = op->op == XR_XIR_LOCAL_READ, write = op->op == XR_XIR_LOCAL_WRITE;
    if (!create && !read && !write) return true;
    XrXirType type = write ? xr_xir_operand_type(f->source, op->args[0]) : op->type;
    XrLirType element = lower_slice_element(f->owner, type);
    if (!element) return true;
    *handled = true;
    uint32_t result = f->source->parameter_count + index, cell = 0;
    if (!lower_cell_index(f, create ? result : op->args[0], &cell)) return false;
    XrLirValue input = f->current[cell];
    if (!read && !lower_slice_source(f, op->args[create ? 0 : 1], &input)) return false;
    if (!lower_slice_call(f, block, XR_LIR_SLICE_OP_BIND, element, &input, 1,
        read ? &f->values[result] : &f->current[cell])) return false;
    return read || lower_view_store(f, cell, f->current[cell]);
}
static bool lower_view_cleanups_finish(LowerFunction *f) {
    LowerViews *views = &f->views;
    if (!views->cleanup_count) return true;
    size_t count = 0;
    for (size_t n = 0; n < views->cleanup_count; ++n) {
        uint32_t cell = f->cell_for[views->cleanups[n].owner];
        if (cell == UINT32_MAX) {
            if (count == SIZE_MAX) return lower_unsupported(f->owner, "Slice cleanup capacity");
            ++count;
        } else {
            if (!views->heads || views->heads[cell] == SIZE_MAX) return lower_unsupported(f->owner, "Slice local has no receiving keeper");
            for (size_t store = views->heads[cell]; store != SIZE_MAX; store = views->stores[store].next) {
                if (count == SIZE_MAX) return lower_unsupported(f->owner, "Slice cleanup capacity");
                ++count;
            }
        }
    }
    XrLirSliceCleanupInsertion *items = lower_alloc(f->owner, count, sizeof(*items));
    if (!items) return false;
    size_t used = 0;
    for (size_t n = 0; n < views->cleanup_count; ++n) {
        LowerViewCleanup cleanup = views->cleanups[n]; uint32_t cell = f->cell_for[cleanup.owner];
        if (cell == UINT32_MAX) {
            cleanup.insertion.target = f->values[cleanup.owner];
            cleanup.insertion.formal = cleanup.owner < f->source->parameter_count;
            items[used++] = cleanup.insertion;
        } else {
            /* A local place owns its writer incarnations and live-in phis.
             * Independent LOCAL_READ copies retain their own keeper identity. */
            for (size_t store = views->heads[cell]; store != SIZE_MAX; store = views->stores[store].next) {
                cleanup.insertion.target = views->stores[store].value; items[used++] = cleanup.insertion;
            }
        }
    }
    bool good = lower_build(f->owner, xr_lir_internal_slice_cleanup_batch(f->owner->module, f->function, items, count));
    xr_free(items); return good;
}
static bool lower_view_instruction(LowerFunction *f, uint32_t block, uint32_t index, bool *handled) {
    Lower *c = f->owner; const XrXirInstruction *op = &f->source->instructions[index];
    uint32_t result = f->source->parameter_count + index;
    *handled = false;
    const LowerByteLength *byte_lengths = lower_byte_length_plan(c);
    if (byte_lengths && byte_lengths[index].omit) { *handled = true; return true; }
    if (byte_lengths && byte_lengths[index].consumer) {
        *handled = true;
        XrLirValue text;
        if (!lower_value(f, byte_lengths[index].receiver, &text)) return false;
        XrLirInstruction length = {.op=XR_LIR_STRING_BYTE_LEN,.type=XR_LIR_I64,
            .operands=&text,.operand_count=1,.source=lower_source(c)};
        uint32_t consumer = f->source->parameter_count + byte_lengths[index].consumer - 1;
        return lower_build(c, xr_lir_emit_value(c->module, f->blocks[block], &length, &f->values[consumer]));
    }
    if (op->op == XR_XIR_CALL && c->lossy_utf8 && xir_lossy_utf8_callable(c->source, (uint32_t)op->immediate)) {
        XR_CHECK(op->args[1] == 1 && op->args[0] < f->source->operand_count,
            "the admitted lossy decoder reads one Slice");
        XrLirValue bytes = {0};
        *handled = true;
        return lower_slice_source(f, f->source->operands[op->args[0]], &bytes) &&
            lower_slice_call(f, block, XR_LIR_SLICE_OP_LOSSY_UTF8, XR_LIR_U8, &bytes, 1, &f->values[result]);
    }
    if (op->op == XR_XIR_VIEW_CLEANUP) { *handled = true; return lower_view_cleanup(f, block, op); }
    if (!lower_view_local(f, block, index, handled)) return false;
    if (*handled) return true;
    XrLirValue args[4] = {{0}}; uint32_t count = 1;
    XrLirSliceOperation role; XrLirType element;
    if (op->op == XR_XIR_VIEW_NEW || op->op == XR_XIR_VIEW_REBORROW) {
        element = lower_slice_element(c, op->type);
        role = op->op == XR_XIR_VIEW_NEW ? XR_LIR_SLICE_OP_NEW : XR_LIR_SLICE_OP_REBORROW;
        if (role == XR_LIR_SLICE_OP_NEW && op->immediate) {
            if (!lower_full_string_view(f, op, element))
                return lower_unsupported(c, "readonly Array Slice requires normalized bounds");
        } else count = 3;
        for (uint32_t a = 0; a < count; ++a)
            if (!lower_slice_source(f, f->source->operands[op->args[0] + a], &args[a])) return false;
        if (role == XR_LIR_SLICE_OP_REBORROW) {
            XrLirInstruction open = {.op=XR_LIR_CONST,.type=XR_LIR_I1,.bits=(uint64_t)op->immediate,.source=lower_source(c)};
            if (!lower_build(c, xr_lir_emit_value(c->module, f->blocks[block], &open, &args[count++]))) return false;
        }
    } else if (op->op == XR_XIR_VIEW_LEN || op->op == XR_XIR_VIEW_GET || op->op == XR_XIR_VIEW_COPY || op->op == XR_XIR_VIEW_END) {
        element = lower_slice_element(c, xr_xir_operand_type(f->source, op->args[0]));
        if (!lower_slice_source(f, op->args[0], &args[0])) return false;
        role = op->op == XR_XIR_VIEW_LEN ? XR_LIR_SLICE_OP_LEN : op->op == XR_XIR_VIEW_GET ? XR_LIR_SLICE_OP_GET :
            op->op == XR_XIR_VIEW_COPY ? XR_LIR_SLICE_OP_COPY : XR_LIR_SLICE_OP_END;
        if (op->op == XR_XIR_VIEW_GET) {
            if (!lower_value(f, op->args[1], &args[count++])) return false;
        }
    } else if (op->op == XR_XIR_COPY && (element = lower_slice_element(c, op->type)) != 0) {
        role = XR_LIR_SLICE_OP_BIND;
        if (!lower_slice_source(f, op->args[0], &args[0])) return false;
    } else return true;
    *handled = true;
    if (!element) return lower_unsupported(c, "readonly Array Slice source type");
    return lower_slice_call(f, block, role, element, args, count, &f->values[result]);
}
