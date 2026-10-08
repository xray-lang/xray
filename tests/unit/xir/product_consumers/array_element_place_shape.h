/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * array_element_place_shape.h - Exact Array store and read metadata
 *
 * KEY CONCEPT:
 *   Six original indices reach real typed Array stores before element reads.
 */
#ifndef ARRAY_ELEMENT_PLACE_SHAPE_H
#define ARRAY_ELEMENT_PLACE_SHAPE_H
#include "xir/xxir_types.h"
#include "xir/xxir_declarations.h"
static const XrXirInstruction *element_place_operand(const XrXirFunction *fn, uint32_t id, uint32_t before) {
    CHECK(!fn->parameter_count && id < before); return &fn->instructions[id];
}
static void array_element_place_shape(const XrXirModule *module, uint32_t entries[6]) {
    const int64_t indices[6] = {0, 1, -1, INT64_MAX, INT64_MIN, 0};
    CHECK(module && module->declarations && module->types);
    for (unsigned c = 0; c < 6; ++c) entries[c] = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        if (fn->name_length != 6 || memcmp(fn->name, "place", 5) || fn->name[5] < '0' || fn->name[5] > '5') continue;
        unsigned c = (unsigned)(fn->name[5] - '0');
        CHECK(entries[c] == UINT32_MAX && !fn->parameter_count && fn->result == XR_XIR_I64);
        CHECK(module->declarations->functions[f].exported && module->declarations->functions[f].module == module->declarations->root_module);
        unsigned arrays = 0, sets = 0, gets = 0, returns = 0; uint32_t root = UINT32_MAX, result = UINT32_MAX;
        for (uint32_t i = 0; i < fn->instruction_count; ++i) {
            const XrXirInstruction *op = &fn->instructions[i];
            if (op->op == XR_XIR_ARRAY_NEW) {
                ++arrays; CHECK(xr_xir_array_element(module->types, op->type) == XR_XIR_I64 && op->args[1] == (c == 5 ? 0u : 1u));
                if (c != 5) {
                    CHECK(op->args[0] < fn->operand_count);
                    const XrXirInstruction *fill = element_place_operand(fn, fn->operands[op->args[0]], i);
                    CHECK(fill->op == XR_XIR_CONST_INT && fill->type == XR_XIR_I64 && fill->immediate == 42);
                }
            }
            if (op->op == XR_XIR_ARRAY_SET) {
                ++sets; CHECK(op->type == XR_XIR_UNIT && op->args[1] == 3 && fn->operand_count >= 3 && op->args[0] <= fn->operand_count - 3);
                const uint32_t *args = fn->operands + op->args[0];
                const XrXirInstruction *place = element_place_operand(fn, args[0], i);
                const XrXirInstruction *index = element_place_operand(fn, args[1], i);
                const XrXirInstruction *value = element_place_operand(fn, args[2], i);
                CHECK(place->op == XR_XIR_CELL_PLACE && xr_xir_array_element(module->types, place->type) == XR_XIR_I64); root = place->args[0];
                CHECK(index->op == XR_XIR_CONST_INT && index->type == XR_XIR_I64 && index->immediate == indices[c]);
                CHECK(value->op == XR_XIR_CONST_INT && value->type == XR_XIR_I64 && value->immediate == 42);
            }
            if (op->op == XR_XIR_ARRAY_GET) {
                ++gets; result = i;
                const XrXirInstruction *array = element_place_operand(fn, op->args[0], i);
                const XrXirInstruction *index = element_place_operand(fn, op->args[1], i);
                CHECK(sets == 1 && op->type == XR_XIR_I64 && array->op == XR_XIR_CELL_PLACE && array->args[0] == root);
                CHECK(index->op == XR_XIR_CONST_INT && index->type == XR_XIR_I64 && index->immediate == indices[c]);
            }
            if (op->op == XR_XIR_RETURN) { ++returns; CHECK(result != UINT32_MAX && op->args[0] == result); }
        }
        CHECK(arrays == 1 && sets == 1 && gets == 1 && returns == 1); entries[c] = f;
    }
    for (unsigned c = 0; c < 6; ++c) CHECK(entries[c] != UINT32_MAX);
}
#endif // ARRAY_ELEMENT_PLACE_SHAPE_H
