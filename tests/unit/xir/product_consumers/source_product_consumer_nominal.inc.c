/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_product_consumer_nominal.inc.c - Original constructor field relationships
 *
 * KEY CONCEPT:
 *   Declaration field order and independent execution constrain construction.
 */
#include "xir/xxir_types.h"

static bool consumer_nominal_name(XrXirLiteral name, const char *text) {
    return name.length == strlen(text) && !memcmp(name.bytes, text, name.length);
}

static void constructor_shape(const XrXirModule *module) {
    unsigned counter = 0, pair = 0, swap = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            const XrXirInstruction *op = &function->instructions[i];
            if (op->op != XR_XIR_CLASS_NEW)
                continue;
            const XrXirTypeNode *node = xr_xir_type_node(module->types, op->type);
            CHECK(node && node->kind == XR_XIR_TYPE_NOMINAL && xr_xir_type_is_class(module->types, op->type));
            const XrXirNominalDeclaration *decl = &module->types->nominals->declarations[node->nominal.declaration];
            CHECK(op->args[1] == decl->field_count && op->args[0] <= function->operand_count);
            CHECK(op->args[1] <= function->operand_count - op->args[0]);
            for (uint32_t field = 0; field < decl->field_count; ++field)
                CHECK(xr_xir_operand_type(function, function->operands[op->args[0] + field]) == node->nominal.fields[field]);
            bool zero = false, truth = false;
            for (uint32_t p = 0; p < function->instruction_count; ++p) {
                const XrXirInstruction *producer = &function->instructions[p];
                zero |= producer->op == XR_XIR_CONST_INT && producer->type == XR_XIR_I64 && !producer->immediate;
                truth |= producer->op == XR_XIR_CONST_BOOL && producer->immediate == 1;
            }
            if (consumer_nominal_name(decl->name, "Counter")) {
                CHECK(decl->field_count == 1 && node->nominal.fields[0] == XR_XIR_I64 && zero);
                ++counter;
            } else if (consumer_nominal_name(decl->name, "Pair")) {
                CHECK(decl->field_count == 2 && node->nominal.fields[0] == XR_XIR_I64);
                CHECK(node->nominal.fields[1] == XR_XIR_BOOL && truth);
                ++pair;
            } else {
                CHECK(consumer_nominal_name(decl->name, "Swap") && decl->field_count == 2);
                CHECK(node->nominal.fields[0] == XR_XIR_I64 && node->nominal.fields[1] == XR_XIR_I64);
                CHECK(function->operands[op->args[0]] != function->operands[op->args[0] + 1]);
                ++swap;
            }
        }
    }
    CHECK(counter == 1 && pair == 1 && swap == 1);
}
