/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * integer_sources_source_shape.h - Original integer helpers and failure codes
 *
 * KEY CONCEPT:
 *   Source helper signatures, promoted operands and independent failure codes
 *   retain all widths and signedness after the producing Source owners die.
 */
#ifndef INTEGER_SOURCES_SOURCE_SHAPE_H
#define INTEGER_SOURCES_SOURCE_SHAPE_H
#include "xir/xxir_types.h"
typedef struct IntegerSourceHelper {
    const char *name;
    XrXirType left, right, result;
    XrXirOp operation;
    int64_t immediate;
} IntegerSourceHelper;
static unsigned integer_sources_family(const char *name) {
    return !strcmp(name, "integer_division") ? 1u :
        !strcmp(name, "integer_comparisons") ? 2u : !strcmp(name, "integer_arithmetic") ? 3u : 0u;
}
static XrXirType integer_sources_operand_type(const XrXirFunction *fn, uint32_t id, uint32_t before) {
    CHECK(id < fn->parameter_count + before);
    return id < fn->parameter_count ? fn->parameters[id] : fn->instructions[id - fn->parameter_count].type;
}
static uint32_t integer_sources_helper(const XrXirModule *m, const IntegerSourceHelper *expected) {
    uint32_t found = UINT32_MAX;
    for (uint32_t f = 0; f < m->function_count; ++f) {
        const XrXirFunction *fn = &m->functions[f];
        if (fn->name_length != strlen(expected->name) || memcmp(fn->name, expected->name, fn->name_length)) continue;
        CHECK(found == UINT32_MAX && fn->parameter_count == 2 && fn->parameters[0] == expected->left &&
            fn->parameters[1] == expected->right && fn->result == expected->result);
        CHECK(!m->declarations->functions[f].exported && !m->declarations->functions[f].nominal_owner);
        unsigned operations = 0, returns = 0; uint32_t result = UINT32_MAX;
        for (uint32_t i = 0; i < fn->instruction_count; ++i) {
            const XrXirInstruction *op = &fn->instructions[i];
            if (op->op == expected->operation) {
                ++operations; CHECK(op->type == expected->result && op->immediate == expected->immediate);
                XrXirType target = xr_xir_integer_bits(expected->left) < xr_xir_integer_bits(expected->right) ?
                    expected->right : expected->left;
                CHECK(integer_sources_operand_type(fn, op->args[0], i) == target &&
                    integer_sources_operand_type(fn, op->args[1], i) == target);
                result = fn->parameter_count + i;
            }
            if (op->op == XR_XIR_RETURN) { ++returns; CHECK(result != UINT32_MAX && op->args[0] == result); }
        }
        CHECK(operations == 1 && returns == 1); found = f;
    }
    CHECK(found != UINT32_MAX); return found;
}
static void integer_sources_source_shape(const XrXirModule *m, unsigned family) {
    CHECK(m && m->declarations && family >= 1 && family <= 3);
    const char *names[] = {"i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64"};
    const XrXirType types[] = {XR_XIR_I8, XR_XIR_U8, XR_XIR_I16, XR_XIR_U16,
        XR_XIR_I32, XR_XIR_U32, XR_XIR_I64, XR_XIR_U64};
    const XrXirOp comparisons[] = {XR_XIR_EQUAL, XR_XIR_EQUAL, XR_XIR_LT_INT,
        XR_XIR_LE_INT, XR_XIR_GT_INT, XR_XIR_GE_INT};
    const XrXirOp arithmetic[] = {XR_XIR_ADD_INT, XR_XIR_SUB_INT, XR_XIR_MUL_INT};
    const char *arithmetic_names[] = {"add", "sub", "mul"};
    uint32_t helpers[50], count = 0; unsigned width = family == 1 ? 2u : family == 2 ? 6u : 3u;
    for (uint32_t t = 0; t < 8; ++t) for (unsigned op = 0; op < width; ++op) {
        char name[40]; int length;
        if (family == 1) length = snprintf(name, sizeof(name), "%s_%s", op ? "rem" : "div", names[t]);
        else if (family == 2) length = snprintf(name, sizeof(name), "op%u_%s", op, names[t]);
        else length = snprintf(name, sizeof(name), "%s_%s", arithmetic_names[op], names[t]);
        CHECK(length > 0 && (size_t)length < sizeof(name) && count < 50);
        IntegerSourceHelper expected = {name, types[t], types[t], family == 2 ? XR_XIR_BOOL : types[t],
            family == 1 ? (op ? XR_XIR_REM_INT : XR_XIR_DIV_INT) : family == 2 ? comparisons[op] : arithmetic[op],
            family == 2 && op == 1 ? 1 : 0};
        helpers[count++] = integer_sources_helper(m, &expected);
    }
    if (family != 1) {
        IntegerSourceHelper mixed = {"mixed", family == 2 ? XR_XIR_U32 : XR_XIR_U8,
            family == 2 ? XR_XIR_U64 : XR_XIR_U16, family == 2 ? XR_XIR_BOOL : XR_XIR_U16,
            family == 2 ? XR_XIR_LT_INT : XR_XIR_ADD_INT, 0};
        helpers[count++] = integer_sources_helper(m, &mixed);
        if (family == 2) {
            IntegerSourceHelper signed_mixed = {"signedMixed", XR_XIR_I8, XR_XIR_I16, XR_XIR_BOOL, XR_XIR_LT_INT, 0};
            helpers[count++] = integer_sources_helper(m, &signed_mixed);
        }
    }
    CHECK(count == (family == 1 ? 16u : family == 2 ? 50u : 25u));
    const XrXirFunction *answer = NULL; unsigned signatures = 0;
    for (uint32_t f = 0; f < m->function_count; ++f) {
        const XrXirFunction *fn = &m->functions[f];
        if (fn->parameter_count == 2) ++signatures;
        if (fn->name_length == 6 && !memcmp(fn->name, "answer", 6)) {
            CHECK(!answer && !fn->parameter_count && fn->result == XR_XIR_I64 && !m->declarations->functions[f].exported);
            answer = fn;
        }
    }
    CHECK(answer && signatures == count);
    unsigned checks = family == 1 ? 20u : family == 2 ? 148u : 25u, calls = 0, branches = 0, returns = 0;
    bool seen[149] = {false};
    for (uint32_t i = 0; i < answer->instruction_count; ++i) {
        const XrXirInstruction *op = &answer->instructions[i];
        if (op->op == XR_XIR_CALL) {
            ++calls; CHECK(op->args[1] == 2 && op->args[0] <= answer->operand_count && answer->operand_count - op->args[0] >= 2);
            bool known = false; for (uint32_t h = 0; h < count; ++h) if (op->immediate == helpers[h]) known = true;
            CHECK(known); const XrXirFunction *callee = &m->functions[op->immediate]; CHECK(op->type == callee->result);
            for (unsigned a = 0; a < 2; ++a)
                CHECK(integer_sources_operand_type(answer, answer->operands[op->args[0] + a], i) == callee->parameters[a]);
        }
        if (op->op == XR_XIR_BRANCH) { ++branches; CHECK(integer_sources_operand_type(answer, op->args[0], i) == XR_XIR_BOOL); }
        if (op->op == XR_XIR_RETURN) {
            ++returns; CHECK(op->args[0] < i); const XrXirInstruction *code = &answer->instructions[op->args[0]];
            CHECK(code->op == XR_XIR_CONST_INT && code->type == XR_XIR_I64 && code->immediate >= 0 && (uint64_t)code->immediate <= checks);
            unsigned value = (unsigned)code->immediate; CHECK(!seen[value]); seen[value] = true;
        }
    }
    CHECK(calls == checks && branches == checks + (family == 2 ? 75u : 0u) && returns == checks + 1);
    for (unsigned i = 0; i <= checks; ++i) CHECK(seen[i]);
}
#endif // INTEGER_SOURCES_SOURCE_SHAPE_H
