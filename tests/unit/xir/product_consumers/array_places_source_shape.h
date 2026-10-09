/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * array_places_source_shape.h - Original three-element Array store and read
 *
 * KEY CONCEPT:
 *   The original literal and index reach actual typed store/read instructions.
 */
#ifndef ARRAY_PLACES_SOURCE_SHAPE_H
#define ARRAY_PLACES_SOURCE_SHAPE_H
#include "xir/xxir_types.h"
#include "xir/xxir_declarations.h"
static const XrXirInstruction *array_places_source_operand(const XrXirFunction *fn, uint32_t id, uint32_t before) {
    CHECK(!fn->parameter_count && id < before); return &fn->instructions[id];
}
static void array_places_source_shape(const XrXirModule *module) {
    CHECK(module && module->types && module->declarations); unsigned answers = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        if (fn->name_length != 6 || memcmp(fn->name, "answer", 6)) continue;
        CHECK(!fn->parameter_count && fn->result == XR_XIR_I64 && !module->declarations->functions[f].exported); ++answers;
        unsigned arrays = 0, sets = 0, gets = 0, returns = 0; uint32_t root = UINT32_MAX, result = UINT32_MAX;
        for (uint32_t i = 0; i < fn->instruction_count; ++i) {
            const XrXirInstruction *op = &fn->instructions[i];
            if (op->op == XR_XIR_ARRAY_NEW) {
                const int64_t fills[3] = {10,20,30}; ++arrays;
                CHECK(xr_xir_array_element(module->types, op->type) == XR_XIR_I64 && op->args[1] == 3 &&
                    fn->operand_count >= 3 && op->args[0] <= fn->operand_count - 3);
                for (unsigned v = 0; v < 3; ++v) {
                    const XrXirInstruction *fill = array_places_source_operand(fn, fn->operands[op->args[0] + v], i);
                    CHECK(fill->op == XR_XIR_CONST_INT && fill->type == XR_XIR_I64 && fill->immediate == fills[v]);
                }
            }
            if (op->op == XR_XIR_ARRAY_SET) {
                ++sets; CHECK(op->type == XR_XIR_UNIT && op->args[1] == 3 && fn->operand_count >= 3 && op->args[0] <= fn->operand_count - 3);
                const uint32_t *args = fn->operands + op->args[0];
                const XrXirInstruction *place = array_places_source_operand(fn, args[0], i);
                const XrXirInstruction *index = array_places_source_operand(fn, args[1], i);
                const XrXirInstruction *value = array_places_source_operand(fn, args[2], i);
                CHECK(place->op == XR_XIR_CELL_PLACE && xr_xir_array_element(module->types, place->type) == XR_XIR_I64); root = place->args[0];
                CHECK(index->op == XR_XIR_CONST_INT && index->type == XR_XIR_I64 && index->immediate == 1);
                CHECK(value->op == XR_XIR_CONST_INT && value->type == XR_XIR_I64 && value->immediate == 42);
            }
            if (op->op == XR_XIR_ARRAY_GET) {
                ++gets; result = i;
                const XrXirInstruction *place = array_places_source_operand(fn, op->args[0], i);
                const XrXirInstruction *index = array_places_source_operand(fn, op->args[1], i);
                CHECK(sets == 1 && op->type == XR_XIR_I64 && place->op == XR_XIR_CELL_PLACE && place->args[0] == root);
                CHECK(index->op == XR_XIR_CONST_INT && index->type == XR_XIR_I64 && index->immediate == 1);
            }
            if (op->op == XR_XIR_RETURN) { ++returns; CHECK(result != UINT32_MAX && op->args[0] == result); }
        }
        CHECK(arrays == 1 && sets == 1 && gets == 1 && returns == 1);
    }
    CHECK(answers == 1);
}
#endif // ARRAY_PLACES_SOURCE_SHAPE_H
