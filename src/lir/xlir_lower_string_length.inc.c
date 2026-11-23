/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_lower_string_length.inc.c - Cached lengths of checked, temporary byte views
 */
static bool lower_byte_length_zero(const XrXirInstruction *op) {
    return op->op == XR_XIR_CONST_INT && op->type == XR_XIR_I64 && !op->immediate;
}

static bool lower_byte_length_shape(const Lower *c, const XrXirFunction *f, uint32_t at) {
    const XrXirInstruction *root = &f->instructions[at];
    const XrXirInstruction *view = root + 3, *length = root + 4;
    uint32_t value = f->parameter_count + at;
    if (view->op != XR_XIR_VIEW_NEW || view->immediate != 1 || view->args[1] != 3 ||
        length->op != XR_XIR_VIEW_LEN || length->args[0] != value + 3 ||
        !lower_byte_length_zero(root + 1) || !lower_byte_length_zero(root + 2)) return false;
    const XrXirTypeNode *slice = xr_xir_type_node(c->source->types, view->type);
    if (!slice || slice->kind != XR_XIR_TYPE_SLICE || slice->element != XR_XIR_U8) return false;
    const uint32_t *operands = f->operands + view->args[0];
    if (operands[1] != value + 1 || operands[2] != value + 2) return false;
    if (root->op == XR_XIR_CELL_NEW)
        return operands[0] == value && lower_module_cell(c, c->function, value) == UINT32_MAX &&
            xr_xir_cell_element(c->source->types, root->type) == XR_XIR_STRING;
    return root->op == XR_XIR_CELL_READ && root->type == XR_XIR_STRING && operands[0] == root->args[0];
}

static void lower_byte_length_uses(const Lower *c, const XrXirFunction *f, LowerByteLength *plan) {
    static const uint8_t arity[] = {0,
#define XR_XIR_OP(name, stages, rule, operands, edges, terminal) operands,
#include "../xir/xxir_ops.def"
#undef XR_XIR_OP
    };
    for (uint32_t i = 0; i < f->instruction_count; ++i) {
        const XrXirInstruction *op = &f->instructions[i];
        /* Cleanup entries retire owners. They never consume a readable view. */
        if (op->op == XR_XIR_VIEW_CLEANUP) continue;
        bool group = xr_xir_op_uses_operand_table(op->op);
        const uint32_t *args = group ? f->operands + op->args[0] : op->args;
        uint32_t count = group ? op->args[1] : arity[op->op];
        if (op->op == XR_XIR_RETURN) count = f->result != XR_XIR_UNIT;
        if (op->op == XR_XIR_CELL_NEW || op->op == XR_XIR_CELL_READ || op->op == XR_XIR_CELL_WRITE ||
            op->op == XR_XIR_CELL_LOCAL_WRITE) {
            XrXirType type = op->op == XR_XIR_CELL_NEW ? op->type : xr_xir_operand_type(f, args[0]);
            count = xr_xir_cell_payload_operands(c->source->types, type, op->op);
        } else if (op->op == XR_XIR_NULLABLE_SOME && xr_xir_nullable_is_unit(c->source->types, op->type)) {
            count = 0;
        } else if (op->op == XR_XIR_SLOT_INIT || op->op == XR_XIR_SLOT_STORE) {
            const XrXirDeclarations *d = c->source->declarations;
            count = xr_xir_slot_payload_operands(d->slots, d->slot_count, op);
        }
        uint32_t first = op->op == XR_XIR_PHI ? 1u : 0u, step = first + 1;
        for (uint32_t p = first; p < count; p += step) {
            uint32_t value = args[p];
            if (value >= f->parameter_count && value - f->parameter_count < f->instruction_count) {
                LowerByteLength *used = &plan[value - f->parameter_count];
                if (used->uses < 2) ++used->uses;
            }
        }
    }
}

static bool lower_byte_lengths(Lower *c, const XrXirFunction *f) {
    LowerByteLength *plan = NULL;
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrXirBlock *block = &f->blocks[b];
        for (uint32_t n = 0; n + 4 < block->count; ++n) {
            uint32_t at = block->first + n;
            if (!lower_byte_length_shape(c, f, at)) continue;
            if (!plan) {
                if (!c->byte_lengths) c->byte_lengths = lower_alloc(c, c->source->function_count,
                    sizeof(*c->byte_lengths));
                if (!c->byte_lengths) return false;
                plan = lower_alloc(c, f->instruction_count, sizeof(*plan));
                if (!plan) return false;
                c->byte_lengths[c->function] = plan;
                lower_byte_length_uses(c, f, plan);
            }
            const XrXirInstruction *root = &f->instructions[at];
            bool temporary = root->op == XR_XIR_CELL_NEW;
            if (plan[at + 3].uses != 1 || (temporary && plan[at].uses != 1)) continue;
            /* The temporary's input is alive at the original CELL_NEW use.
             * An existing binding keeps the real CELL_READ snapshot instead. */
            uint32_t anchor = temporary ? at : at + 3;
            plan[anchor].consumer = at + 5;
            plan[anchor].receiver = temporary ? root->args[0] : f->parameter_count + at;
            plan[at + 1].omit = plan[at + 1].uses == 1;
            plan[at + 2].omit = plan[at + 2].uses == 1;
            plan[at + 3].omit = temporary;
            plan[at + 4].omit = true;
        }
    }
    return true;
}

static const LowerByteLength *lower_byte_length_plan(const Lower *c) {
    return c->byte_lengths ? c->byte_lengths[c->function] : NULL;
}

static bool lower_byte_length_elided_view(const LowerByteLength *plan, const XrXirFunction *f, uint32_t value) {
    if (!plan || value < f->parameter_count) return false;
    uint32_t at = value - f->parameter_count;
    return at < f->instruction_count && f->instructions[at].op == XR_XIR_VIEW_NEW &&
        (plan[at].omit || plan[at].consumer);
}
