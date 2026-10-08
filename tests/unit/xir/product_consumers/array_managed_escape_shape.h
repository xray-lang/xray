/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * array_managed_escape_shape.h - Public managed append result metadata oracle
 *
 * KEY CONCEPT:
 *   Nine copies of the exact three-byte String enter one owned Array result.
 */
#ifndef ARRAY_MANAGED_ESCAPE_SHAPE_H
#define ARRAY_MANAGED_ESCAPE_SHAPE_H
#include "xir/xxir_types.h"
#include "xir/xxir_declarations.h"
static uint32_t array_managed_escape_shape(const XrXirModule *module) {
    CHECK(module && module->declarations && module->types);
    uint32_t found = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        if (fn->name_length != 13 || memcmp(fn->name, "managedValues", 13)) continue;
        CHECK(found == UINT32_MAX && fn->parameter_count == 1 && fn->parameters[0] == XR_XIR_I64);
        CHECK(xr_xir_array_element(module->types, fn->result) == XR_XIR_STRING && xr_xir_type_is_owned(module->types, fn->result));
        CHECK(module->declarations->functions[f].exported && module->declarations->functions[f].module == module->declarations->root_module);
        unsigned appends = 0, arrays = 0, returns = 0; uint32_t receiver = UINT32_MAX, element = UINT32_MAX;
        for (uint32_t i = 0; i < fn->instruction_count; ++i) {
            const XrXirInstruction *op = &fn->instructions[i];
            if (op->op == XR_XIR_ARRAY_NEW) {
                ++arrays; CHECK(op->type == fn->result && !op->args[1]);
            }
            if (op->op == XR_XIR_ARRAY_PUSH) {
                ++appends; CHECK(op->type == XR_XIR_UNIT);
                if (element == UINT32_MAX) element = op->args[1];
                CHECK(op->args[1] == element && element >= fn->parameter_count && element - fn->parameter_count < i);
                const XrXirInstruction *text = &fn->instructions[element - fn->parameter_count];
                CHECK(text->op == XR_XIR_CONST_STRING && text->type == XR_XIR_STRING && text->immediate >= 0);
                CHECK((uint64_t)text->immediate < module->declarations->literal_count);
                const XrXirLiteral *literal = &module->declarations->literals[text->immediate]; const char expected[] = {'a', 0, 'b'};
                CHECK(literal->length == 3 && !memcmp(literal->bytes, expected, 3));
                CHECK(op->args[0] >= fn->parameter_count && op->args[0] - fn->parameter_count < i);
                const XrXirInstruction *place = &fn->instructions[op->args[0] - fn->parameter_count];
                CHECK(place->op == XR_XIR_CELL_PLACE && place->type == fn->result);
                if (receiver == UINT32_MAX) receiver = place->args[0];
                CHECK(place->args[0] == receiver);
            }
            if (op->op == XR_XIR_RETURN) {
                ++returns; CHECK(xr_xir_operand_type(fn, op->args[0]) == fn->result);
            }
        }
        CHECK(appends == 9 && arrays == 1 && returns == 1 && receiver >= fn->parameter_count);
        CHECK(receiver - fn->parameter_count < fn->instruction_count);
        const XrXirInstruction *cell = &fn->instructions[receiver - fn->parameter_count];
        CHECK(cell->op == XR_XIR_CELL_NEW && xr_xir_cell_element(module->types, cell->type) == fn->result);
        CHECK(cell->args[0] >= fn->parameter_count && cell->args[0] - fn->parameter_count < fn->instruction_count);
        CHECK(fn->instructions[cell->args[0] - fn->parameter_count].op == XR_XIR_ARRAY_NEW); found = f;
    }
    CHECK(found != UINT32_MAX); return found;
}
#endif // ARRAY_MANAGED_ESCAPE_SHAPE_H
