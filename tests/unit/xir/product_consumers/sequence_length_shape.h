/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * sequence_length_shape.h - Original Unicode scalar input and retained String entry
 *
 * KEY CONCEPT:
 *   Original UTF-8 bytes include a supplementary and a combining scalar.
 */
#ifndef SEQUENCE_LENGTH_SHAPE_H
#define SEQUENCE_LENGTH_SHAPE_H
#include "xir/xxir_types.h"
static const unsigned char sequence_bytes[13] = {'A',0xc3,0xa9,0xe4,0xb8,0x96,0xf0,0x9f,0x98,0x80,'e',0xcc,0x81};
static uint32_t sequence_length_shape(const XrXirModule *m) {
    CHECK(m && m->declarations);
    uint32_t exported = UINT32_MAX, copied = UINT32_MAX, answer = UINT32_MAX;
    unsigned exact_literals = 0, length_operations = 0;
    for (uint32_t f = 0; f < m->function_count; ++f) {
        const XrXirFunction *fn = &m->functions[f];
        if (fn->name_length == 12 && !memcmp(fn->name, "sequenceCopy", 12)) {
            CHECK(exported == UINT32_MAX && !fn->parameter_count && fn->result == XR_XIR_STRING);
            CHECK(m->declarations->functions[f].exported && m->declarations->functions[f].module == m->declarations->root_module);
            exported = f;
        }
        if (fn->name_length == 6 && !memcmp(fn->name, "copied", 6)) {
            CHECK(copied == UINT32_MAX && !fn->parameter_count && fn->result == XR_XIR_STRING && !m->declarations->functions[f].exported);
            copied = f;
            for (uint32_t i = 0; i < fn->instruction_count; ++i) {
                const XrXirInstruction *op = &fn->instructions[i];
                if (op->op != XR_XIR_CONST_STRING) continue;
                CHECK(op->type == XR_XIR_STRING && op->immediate >= 0 && (uint64_t)op->immediate < m->declarations->literal_count);
                XrXirLiteral literal = m->declarations->literals[op->immediate];
                if (literal.length == sizeof(sequence_bytes) && !memcmp(literal.bytes, sequence_bytes, sizeof(sequence_bytes))) ++exact_literals;
            }
        }
        if (fn->name_length == 6 && !memcmp(fn->name, "answer", 6)) {
            CHECK(answer == UINT32_MAX && !fn->parameter_count && fn->result == XR_XIR_I64 && !m->declarations->functions[f].exported);
            answer = f;
            uint32_t result = UINT32_MAX, returned = UINT32_MAX;
            for (uint32_t i = 0; i < fn->instruction_count; ++i) {
                const XrXirInstruction *op = &fn->instructions[i];
                if (op->op == XR_XIR_STRING_LEN) {
                    ++length_operations; CHECK(op->type == XR_XIR_I64 && !op->immediate && op->args[0] < i);
                    CHECK(xr_xir_operand_type(fn, op->args[0]) == XR_XIR_STRING);
                    const XrXirInstruction *call = &fn->instructions[op->args[0]];
                    CHECK(call->op == XR_XIR_CALL && call->immediate >= 0 && (uint64_t)call->immediate < m->function_count);
                    const XrXirFunction *target = &m->functions[call->immediate];
                    CHECK(target->name_length == 6 && !memcmp(target->name, "copied", 6)); result = i;
                }
                if (op->op == XR_XIR_RETURN) { CHECK(returned == UINT32_MAX); returned = op->args[0]; }
            }
            CHECK(result != UINT32_MAX && returned == result);
        }
    }
    CHECK(exported != UINT32_MAX && copied != UINT32_MAX && answer != UINT32_MAX && exact_literals == 1 && length_operations == 1);
    return exported;
}
#endif // SEQUENCE_LENGTH_SHAPE_H
