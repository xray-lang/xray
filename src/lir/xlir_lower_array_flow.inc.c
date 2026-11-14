/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_lower_array_flow.inc.c - Array binding liveness in the existing SSA state graph
 *
 * KEY CONCEPT:
 *   Address-observed Cells retain physical storage. Value-only Array bindings
 *   and snapshots follow the same live-in equations as scalar local state.
 */
static bool lower_grouped_operands(XrXirOp op) {
    return xr_xir_op_uses_operand_table(op);
}
static uint32_t lower_operand_count(const XrXirFunction *f, const XrXirInstruction *op) {
    static const uint8_t arity[] = {0,
#define XR_XIR_OP(name, stages, rule, operands, edges, terminal) operands,
#include "../xir/xxir_ops.def"
#undef XR_XIR_OP
    };
    if (op->op == XR_XIR_VIEW_CLEANUP) return 0;
    return lower_grouped_operands(op->op) ? op->args[1] :
        op->op == XR_XIR_RETURN ? f->result != XR_XIR_UNIT : arity[op->op];
}
static uint32_t lower_operand_at(const XrXirFunction *f, const XrXirInstruction *op, uint32_t a) {
    return lower_grouped_operands(op->op) ? f->operands[op->args[0] + a] : op->args[a];
}
static bool lower_array_place_consumer(XrXirOp op, uint32_t a) {
    return a == 0 && (op == XR_XIR_ARRAY_GET || op == XR_XIR_ARRAY_LEN ||
        op == XR_XIR_ARRAY_SET || op == XR_XIR_ARRAY_PUSH ||
        op == XR_XIR_PLACE_READ || op == XR_XIR_PLACE_WRITE || op == XR_XIR_MAP_GET ||
        op == XR_XIR_MAP_SET || op == XR_XIR_HASH_LEN || op == XR_XIR_HASH_CONTAINS ||
        op == XR_XIR_HASH_DELETE || op == XR_XIR_HASH_CLEAR);
}

static bool lower_array_flow_inventory(LowerFunction *f) {
    Lower *c = f->owner;
    const XrXirFunction *source = f->source;
    bool needed = c->arrays || c->string_appends || c->maps;
    for (uint32_t p = 0; !needed && source->block_count > 1 && p < source->parameter_count; ++p)
        needed = source->parameters[p] == XR_XIR_STRING;
    for (uint32_t i = 0; !needed && i < source->instruction_count; ++i) {
        XrXirType type = source->instructions[i].type;
        XrXirType element = xr_xir_cell_element(c->source->types, type);
        needed = (source->block_count > 1 && (type == XR_XIR_STRING || element == XR_XIR_STRING)) ||
            xr_xir_type_is_callable(c->source->types, element);
    }
    if (!needed) return true;
    f->promoted_arrays = lower_alloc(c, f->value_count, 1);
    f->array_values = lower_alloc(c, f->value_count, sizeof(*f->array_values));
    f->array_places = lower_alloc(c, f->value_count, sizeof(*f->array_places));
    f->value_blocks = lower_alloc(c, f->value_count, sizeof(*f->value_blocks));
    if (f->value_count && (!f->promoted_arrays || !f->array_values || !f->array_places || !f->value_blocks))
        return false;
    for (uint32_t v = 0; v < f->value_count; ++v)
        f->array_values[v] = f->array_places[v] = f->value_blocks[v] = UINT32_MAX;
    for (uint32_t b = 0; b < source->block_count; ++b) if (f->reachable[b]) {
        const XrXirBlock *block = &source->blocks[b];
        for (uint32_t i = block->first; i < block->first + block->count; ++i) {
            uint32_t value = source->parameter_count + i;
            const XrXirInstruction *op = &source->instructions[i];
            f->value_blocks[value] = b;
            f->promoted_arrays[value] = op->op == XR_XIR_CELL_NEW &&
                (lower_type(c, op->type) == XR_LIR_ARRAY_CELL || lower_type(c, op->type) == XR_LIR_VIEW_CELL ||
                 lower_type(c, op->type) == XR_LIR_MAP_STRING_I64_CELL ||
                 xr_xir_type_is_callable(c->source->types, xr_xir_cell_element(c->source->types, op->type)) ||
                 (source->block_count > 1 && xr_xir_cell_element(c->source->types, op->type) == XR_XIR_STRING)) &&
                lower_module_cell(c, c->function, value) == UINT32_MAX;
        }
    }
    /* Only identity-free uses may remove a Cell. An ordinary call or a Cell
     * alias keeps the original binding for its synchronous borrower. */
    for (uint32_t b = 0; b < source->block_count; ++b) if (f->reachable[b]) {
        const XrXirBlock *block = &source->blocks[b];
        for (uint32_t i = block->first; i < block->first + block->count; ++i) {
            const XrXirInstruction *op = &source->instructions[i];
            uint32_t count = lower_operand_count(source, op);
            for (uint32_t a = op->op == XR_XIR_PHI ? 1u : 0u; a < count;
                 a += op->op == XR_XIR_PHI ? 2u : 1u) {
                uint32_t value = lower_operand_at(source, op, a);
                bool callable = xr_xir_type_is_callable(c->source->types,
                    xr_xir_cell_element(c->source->types, xr_xir_operand_type(source, value)));
                if (f->promoted_arrays[value] && !(a == 0 &&
                    (op->op == XR_XIR_CELL_READ || (!callable && (op->op == XR_XIR_CELL_WRITE ||
                     op->op == XR_XIR_CELL_STRING_APPEND || op->op == XR_XIR_CELL_PLACE)))))
                    f->promoted_arrays[value] = 0;
                if (value < source->parameter_count) continue;
                const XrXirInstruction *definition = &source->instructions[value - source->parameter_count];
                if (definition->op == XR_XIR_CELL_PLACE && !lower_array_place_consumer(op->op, a))
                    f->promoted_arrays[definition->args[0]] = 0;
            }
        }
    }
    return true;
}

static bool lower_array_flow_values(LowerFunction *f) {
    Lower *c = f->owner;
    if (!f->array_values) return true;
    const XrXirFunction *source = f->source;
    for (uint32_t b = 0; b < source->block_count; ++b) if (f->reachable[b]) {
        const XrXirBlock *block = &source->blocks[b];
        for (uint32_t i = block->first; i < block->first + block->count; ++i) {
            const XrXirInstruction *op = &source->instructions[i];
            if (op->op == XR_XIR_CELL_PLACE && f->promoted_arrays[op->args[0]])
                f->array_places[source->parameter_count + i] = f->cell_for[op->args[0]];
            uint32_t count = lower_operand_count(source, op);
            for (uint32_t a = op->op == XR_XIR_PHI ? 1u : 0u; a < count;
                 a += op->op == XR_XIR_PHI ? 2u : 1u) {
                uint32_t value = lower_operand_at(source, op, a);
                uint32_t use_block = op->op == XR_XIR_PHI ? source->operands[op->args[0] + a - 1] : b;
                bool parameter = value < source->parameter_count;
                if ((parameter && (source->parameters[value] != XR_XIR_STRING || !use_block)) ||
                    f->value_blocks[value] == use_block ||
                    f->array_values[value] != UINT32_MAX) continue;
                const XrXirInstruction *definition = parameter ? NULL :
                    &source->instructions[value - source->parameter_count];
                XrXirType type = parameter ? source->parameters[value] : definition->type;
                if (!lower_managed_type(lower_type(c, type)) || (definition &&
                    (definition->op == XR_XIR_CELL_PLACE || definition->op == XR_XIR_SLOT_PLACE ||
                     definition->op == XR_XIR_LOCAL_NEW))) continue;
                XR_DCHECK(f->cell_count < f->value_count, "each managed state has a distinct source value");
                uint32_t cell = f->cell_count++;
                f->array_values[value] = cell;
                f->cell_types[cell] = lower_type(c, type);
                f->cell_elements[cell] = lower_array_element(c, type);
                if (parameter) f->value_blocks[value] = 0;
            }
        }
    }
    return true;
}

static void lower_array_flow_liveness(LowerFunction *f, uint32_t b, uint32_t i) {
    if (!f->array_values) return;
    const XrXirFunction *source = f->source;
    const XrXirInstruction *op = &source->instructions[i];
    uint32_t result = source->parameter_count + i;
    if (f->array_values[result] != UINT32_MAX)
        f->define[(size_t)b * f->cell_count + f->array_values[result]] = 1;
    uint32_t count = lower_operand_count(source, op);
    for (uint32_t a = op->op == XR_XIR_PHI ? 1u : 0u; a < count;
         a += op->op == XR_XIR_PHI ? 2u : 1u) {
        uint32_t value = lower_operand_at(source, op, a);
        uint32_t block = op->op == XR_XIR_PHI ? source->operands[op->args[0] + a - 1] : b;
        uint32_t cell = f->array_values[value];
        if (cell != UINT32_MAX && f->value_blocks[value] != block)
            f->use[(size_t)block * f->cell_count + cell] = 1;
    }
    if (op->op == XR_XIR_ARRAY_GET || op->op == XR_XIR_ARRAY_LEN || op->op == XR_XIR_ARRAY_SET ||
        op->op == XR_XIR_ARRAY_PUSH || op->op == XR_XIR_PLACE_READ || op->op == XR_XIR_PLACE_WRITE ||
        op->op == XR_XIR_MAP_GET || op->op == XR_XIR_MAP_SET || op->op == XR_XIR_HASH_LEN ||
        op->op == XR_XIR_HASH_CONTAINS || op->op == XR_XIR_HASH_DELETE || op->op == XR_XIR_HASH_CLEAR) {
        uint32_t receiver = lower_operand_at(source, op, 0);
        uint32_t cell = f->array_places[receiver];
        if (cell == UINT32_MAX) return;
        size_t at = (size_t)b * f->cell_count + cell;
        if (op->op != XR_XIR_PLACE_WRITE && !f->define[at]) f->use[at] = 1;
        if (op->op == XR_XIR_ARRAY_SET || op->op == XR_XIR_ARRAY_PUSH || op->op == XR_XIR_PLACE_WRITE ||
            op->op == XR_XIR_MAP_SET || op->op == XR_XIR_HASH_DELETE || op->op == XR_XIR_HASH_CLEAR)
            f->define[at] = 1;
    }
}
