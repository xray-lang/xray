/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_execution_fixture.h - Scalar execution verification
 *
 * KEY CONCEPT:
 *   Hand-authored admission fixtures, independent of the source frontend.
 */

#ifndef XIR_EXECUTION_FIXTURE_H
#define XIR_EXECUTION_FIXTURE_H
#include "xir/xxir.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static XrXirArtifact *uninitialized_leaf_fixture(void) {
    const XrXirType parameter = XR_XIR_I64;
    const XrXirInstruction ops[] = {
        {XR_XIR_LOCAL_UNINIT, XR_XIR_I64, {0}, {0}, 0},
        {XR_XIR_LOCAL_WRITE, XR_XIR_UNIT, {1, 0}, {0}, 0},
        {XR_XIR_LOCAL_READ, XR_XIR_I64, {1}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {3}, {0}, 0}
    };
    const XrXirBlock block = {0, 4};
    const XrXirFunction function = {"u", 1, &parameter, 1, XR_XIR_I64, &block, 1, ops, 4, NULL, 0};
    const XrXirModule built = {XR_XIR_BUILT, &function, 1, NULL, NULL, NULL, NULL};
    XrXirArtifact *checked = NULL, *lowered = NULL;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_lower(checked, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    return lowered;
}

static XrXirFunction phi_leaf_fixture(void) {
    static const XrXirType parameters[] = {XR_XIR_I64, XR_XIR_I64, XR_XIR_I64};
    static const XrXirInstruction ops[] = {
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 1},
        {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0}, {0}, 1},
        {XR_XIR_BRANCH, XR_XIR_UNIT, {5}, {2, 2}, 0},
        {XR_XIR_SUB_INT, XR_XIR_I64, {9, 10}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {7}, {0}, 0},
        {XR_XIR_PHI, XR_XIR_I64, {0, 4}, {0}, 0},
        {XR_XIR_PHI, XR_XIR_I64, {4, 4}, {0}, 0},
        {XR_XIR_PHI, XR_XIR_I64, {8, 4}, {0}, 0},
        {XR_XIR_ADD_INT, XR_XIR_I64, {11, 4}, {0}, 0},
        {XR_XIR_LT_INT, XR_XIR_BOOL, {11, 2}, {0}, 0},
        {XR_XIR_BRANCH, XR_XIR_UNIT, {13}, {2, 1}, 0}
    };
    static const uint32_t inputs[] = {0, 0, 2, 10, 0, 1, 2, 9, 0, 3, 2, 12};
    static const XrXirBlock blocks[] = {{0, 4}, {4, 2}, {6, 6}};
    return (XrXirFunction) {"phi", 3, parameters, 3, XR_XIR_I64, blocks, 3, ops, 12, inputs, 12};
}

static XrXirArtifact *fixture_checked(void) {
    const XrXirType add_parameters[] = {XR_XIR_BOOL, XR_XIR_I64, XR_XIR_I64};
    const XrXirType eq_parameters[] = {XR_XIR_I64, XR_XIR_I64};
    const XrXirType bool_parameters[] = {XR_XIR_BOOL};
    const XrXirBlock add_blocks[] = {{0, 1}, {1, 3}, {4, 2}};
    const XrXirBlock pair_blocks[] = {{0, 2}};
    const XrXirBlock unit_blocks[] = {{0, 1}};
    const XrXirBlock loop_blocks[] = {{0, 1}, {1, 1}};
    const XrXirBlock reverse_blocks[] = {{0, 1}, {1, 1}, {2, 4}};
    const XrXirInstruction add[] = {
        {XR_XIR_BRANCH, XR_XIR_UNIT, {0, 0}, {1, 2}, 0},
        {XR_XIR_ADD_INT, XR_XIR_I64, {1, 2}, {0, 0}, 0},
        {XR_XIR_COPY, XR_XIR_I64, {4, 0}, {0, 0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {5, 0}, {0, 0}, 0},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0, 0}, {0, 0}, -7},
        {XR_XIR_RETURN, XR_XIR_UNIT, {7, 0}, {0, 0}, 0}
    };
    const XrXirInstruction eq[] = {
        {XR_XIR_EQ_INT, XR_XIR_BOOL, {0, 1}, {0, 0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2, 0}, {0, 0}, 0}
    };
    const XrXirInstruction copy[] = {
        {XR_XIR_COPY, XR_XIR_BOOL, {0, 0}, {0, 0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1, 0}, {0, 0}, 0}
    };
    const XrXirInstruction unit[] = {{XR_XIR_RETURN, XR_XIR_UNIT, {0, 0}, {0, 0}, 0}};
    const XrXirInstruction minimum[] = {
        {XR_XIR_CONST_INT, XR_XIR_I64, {0, 0}, {0, 0}, INT64_MIN},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0, 0}, {0, 0}, 0}
    };
    const XrXirInstruction loop[] = {
        {XR_XIR_JUMP, XR_XIR_UNIT, {0, 0}, {1, 0}, 0},
        {XR_XIR_JUMP, XR_XIR_UNIT, {0, 0}, {1, 0}, 0}
    };
    const XrXirInstruction reverse[] = {
        {XR_XIR_JUMP, XR_XIR_UNIT, {0, 0}, {2, 0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {4, 0}, {0, 0}, 0},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0, 0}, {0, 0}, 7},
        {XR_XIR_EQ_INT, XR_XIR_BOOL, {2, 2}, {0, 0}, 0},
        {XR_XIR_COPY, XR_XIR_BOOL, {3, 0}, {0, 0}, 0},
        {XR_XIR_JUMP, XR_XIR_UNIT, {0, 0}, {1, 0}, 0}
    };
    const XrXirInstruction boolean[] = {
        {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0, 0}, {0, 0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0, 0}, {0, 0}, 0}
    };
    const XrXirInstruction maximum[] = {
        {XR_XIR_CONST_INT, XR_XIR_I64, {0, 0}, {0, 0}, INT64_MAX},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0, 0}, {0, 0}, 0}
    };
    const XrXirInstruction numeric0[] = {
        {XR_XIR_SUB_INT, XR_XIR_I64, {0, 1}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2, 0}, {0}, 0}
    };
    const XrXirInstruction numeric1[] = {
        {XR_XIR_MUL_INT, XR_XIR_I64, {0, 1}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2, 0}, {0}, 0}
    };
    const XrXirInstruction numeric2[] = {
        {XR_XIR_DIV_INT, XR_XIR_I64, {0, 1}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2, 0}, {0}, 0}
    };
    const XrXirInstruction numeric3[] = {
        {XR_XIR_REM_INT, XR_XIR_I64, {0, 1}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2, 0}, {0}, 0}
    };
    const XrXirInstruction numeric4[] = {
        {XR_XIR_NE_INT, XR_XIR_BOOL, {0, 1}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2, 0}, {0}, 0}
    };
    const XrXirInstruction numeric5[] = {
        {XR_XIR_LE_INT, XR_XIR_BOOL, {0, 1}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2, 0}, {0}, 0}
    };
    const XrXirInstruction numeric6[] = {
        {XR_XIR_GT_INT, XR_XIR_BOOL, {0, 1}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2, 0}, {0}, 0}
    };
    const XrXirInstruction numeric7[] = {
        {XR_XIR_GE_INT, XR_XIR_BOOL, {0, 1}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2, 0}, {0}, 0}
    };
    const XrXirInstruction bitwise0[] = {
        {XR_XIR_AND_INT, XR_XIR_I64, {0, 1}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2, 0}, {0}, 0}
    };
    const XrXirInstruction bitwise1[] = {
        {XR_XIR_OR_INT, XR_XIR_I64, {0, 1}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2, 0}, {0}, 0}
    };
    const XrXirInstruction bitwise2[] = {
        {XR_XIR_XOR_INT, XR_XIR_I64, {0, 1}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2, 0}, {0}, 0}
    };
    const XrXirInstruction bitwise3[] = {
        {XR_XIR_SHL_INT, XR_XIR_I64, {0, 1}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2, 0}, {0}, 0}
    };
    const XrXirInstruction bitwise4[] = {
        {XR_XIR_SHR_INT, XR_XIR_I64, {0, 1}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2, 0}, {0}, 0}
    };
    const XrXirFunction functions[] = {
        {"add", 3, add_parameters, 3, XR_XIR_I64, add_blocks, 3, add, 6, NULL, 0},
        {"eq", 2, eq_parameters, 2, XR_XIR_BOOL, pair_blocks, 1, eq, 2, NULL, 0},
        {"copy", 4, bool_parameters, 1, XR_XIR_BOOL, pair_blocks, 1, copy, 2, NULL, 0},
        {"unit", 4, NULL, 0, XR_XIR_UNIT, unit_blocks, 1, unit, 1, NULL, 0},
        {"min", 3, NULL, 0, XR_XIR_I64, pair_blocks, 1, minimum, 2, NULL, 0},
        {"loop", 4, NULL, 0, XR_XIR_UNIT, loop_blocks, 2, loop, 2, NULL, 0},
        {"reverse", 7, NULL, 0, XR_XIR_BOOL, reverse_blocks, 3, reverse, 6, NULL, 0},
        {"false", 5, NULL, 0, XR_XIR_BOOL, pair_blocks, 1, boolean, 2, NULL, 0},
        {"max", 3, NULL, 0, XR_XIR_I64, pair_blocks, 1, maximum, 2, NULL, 0},
        {"numeric0", 8, eq_parameters, 2, XR_XIR_I64, pair_blocks, 1, numeric0, 2, NULL, 0},
        {"numeric1", 8, eq_parameters, 2, XR_XIR_I64, pair_blocks, 1, numeric1, 2, NULL, 0},
        {"numeric2", 8, eq_parameters, 2, XR_XIR_I64, pair_blocks, 1, numeric2, 2, NULL, 0},
        {"numeric3", 8, eq_parameters, 2, XR_XIR_I64, pair_blocks, 1, numeric3, 2, NULL, 0},
        {"numeric4", 8, eq_parameters, 2, XR_XIR_BOOL, pair_blocks, 1, numeric4, 2, NULL, 0},
        {"numeric5", 8, eq_parameters, 2, XR_XIR_BOOL, pair_blocks, 1, numeric5, 2, NULL, 0},
        {"numeric6", 8, eq_parameters, 2, XR_XIR_BOOL, pair_blocks, 1, numeric6, 2, NULL, 0},
        {"numeric7", 8, eq_parameters, 2, XR_XIR_BOOL, pair_blocks, 1, numeric7, 2, NULL, 0},
        {"bitwise0", 8, eq_parameters, 2, XR_XIR_I64, pair_blocks, 1, bitwise0, 2, NULL, 0},
        {"bitwise1", 8, eq_parameters, 2, XR_XIR_I64, pair_blocks, 1, bitwise1, 2, NULL, 0},
        {"bitwise2", 8, eq_parameters, 2, XR_XIR_I64, pair_blocks, 1, bitwise2, 2, NULL, 0},
        {"bitwise3", 8, eq_parameters, 2, XR_XIR_I64, pair_blocks, 1, bitwise3, 2, NULL, 0},
        {"bitwise4", 8, eq_parameters, 2, XR_XIR_I64, pair_blocks, 1, bitwise4, 2, NULL, 0},
        phi_leaf_fixture(),
#define XIR_INTEGER_CASE(id, type, op, rhs, result, count, left, right, expected) \
        {"integer" #id, sizeof("integer" #id) - 1, (const XrXirType[]) {type, rhs}, count, result, pair_blocks, 1, \
            (const XrXirInstruction[]) {{op, result, {0, count == 2 ? 1 : 0}, {0}, 0}, \
                {XR_XIR_RETURN, XR_XIR_UNIT, {count}, {0}, 0}}, 2, NULL, 0},
#include "xir_integer_cases.def"
#undef XIR_INTEGER_CASE
#define FLOAT_PARAMETERS_0(type) NULL
#define FLOAT_PARAMETERS_1(type) (const XrXirType[]) {type}
#define FLOAT_PARAMETERS_2(type) (const XrXirType[]) {type, type}
#define XIR_FLOAT_FUNCTION(id, type, op, result, count, immediate) \
        {"floating" #id, sizeof("floating" #id) - 1, FLOAT_PARAMETERS_##count(type), count, result, pair_blocks, 1, \
            (const XrXirInstruction[]) {{op, result, {0, count == 2 ? 1 : 0}, {0}, immediate}, \
                {XR_XIR_RETURN, XR_XIR_UNIT, {count}, {0}, 0}}, 2, NULL, 0},
#include "xir_float_functions.def"
#undef XIR_FLOAT_FUNCTION
#undef FLOAT_PARAMETERS_0
#undef FLOAT_PARAMETERS_1
#undef FLOAT_PARAMETERS_2
    };
    const XrXirModule module = {XR_XIR_BUILT, functions, sizeof(functions) / sizeof(functions[0]), NULL, NULL, NULL, NULL};
    XrXirArtifact *artifact = NULL;
    CHECK(xr_xir_check(&module, NULL, &artifact, NULL) == XR_XIR_OK);
    return artifact;
}

static XrXirArtifact *fixture_lowered(void) {
    XrXirArtifact *checked = fixture_checked(), *lowered = NULL;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirCheckedPacket packet = {0}; XrXirArtifact *decoded = NULL;
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    CHECK(xr_xir_lower(decoded, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_checked_packet_free(&packet); xr_xir_artifact_free(decoded);
    xr_xir_artifact_free(checked);
    return lowered;
}
#endif
