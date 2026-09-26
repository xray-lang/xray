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
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

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
        {XR_XIR_ADD_I64, XR_XIR_I64, {1, 2}, {0, 0}, 0},
        {XR_XIR_COPY, XR_XIR_I64, {4, 0}, {0, 0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {5, 0}, {0, 0}, 0},
        {XR_XIR_CONST_I64, XR_XIR_I64, {0, 0}, {0, 0}, -7},
        {XR_XIR_RETURN, XR_XIR_UNIT, {7, 0}, {0, 0}, 0}
    };
    const XrXirInstruction eq[] = {
        {XR_XIR_EQ_I64, XR_XIR_BOOL, {0, 1}, {0, 0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2, 0}, {0, 0}, 0}
    };
    const XrXirInstruction copy[] = {
        {XR_XIR_COPY, XR_XIR_BOOL, {0, 0}, {0, 0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1, 0}, {0, 0}, 0}
    };
    const XrXirInstruction unit[] = {{XR_XIR_RETURN, XR_XIR_UNIT, {0, 0}, {0, 0}, 0}};
    const XrXirInstruction minimum[] = {
        {XR_XIR_CONST_I64, XR_XIR_I64, {0, 0}, {0, 0}, INT64_MIN},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0, 0}, {0, 0}, 0}
    };
    const XrXirInstruction loop[] = {
        {XR_XIR_JUMP, XR_XIR_UNIT, {0, 0}, {1, 0}, 0},
        {XR_XIR_JUMP, XR_XIR_UNIT, {0, 0}, {1, 0}, 0}
    };
    const XrXirInstruction reverse[] = {
        {XR_XIR_JUMP, XR_XIR_UNIT, {0, 0}, {2, 0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {4, 0}, {0, 0}, 0},
        {XR_XIR_CONST_I64, XR_XIR_I64, {0, 0}, {0, 0}, 7},
        {XR_XIR_EQ_I64, XR_XIR_BOOL, {2, 2}, {0, 0}, 0},
        {XR_XIR_COPY, XR_XIR_BOOL, {3, 0}, {0, 0}, 0},
        {XR_XIR_JUMP, XR_XIR_UNIT, {0, 0}, {1, 0}, 0}
    };
    const XrXirInstruction boolean[] = {
        {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0, 0}, {0, 0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0, 0}, {0, 0}, 0}
    };
    const XrXirInstruction maximum[] = {
        {XR_XIR_CONST_I64, XR_XIR_I64, {0, 0}, {0, 0}, INT64_MAX},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0, 0}, {0, 0}, 0}
    };
    const XrXirFunction functions[] = {
        {"add", 3, add_parameters, 3, XR_XIR_I64, add_blocks, 3, add, 6},
        {"eq", 2, eq_parameters, 2, XR_XIR_BOOL, pair_blocks, 1, eq, 2},
        {"copy", 4, bool_parameters, 1, XR_XIR_BOOL, pair_blocks, 1, copy, 2},
        {"unit", 4, NULL, 0, XR_XIR_UNIT, unit_blocks, 1, unit, 1},
        {"min", 3, NULL, 0, XR_XIR_I64, pair_blocks, 1, minimum, 2},
        {"loop", 4, NULL, 0, XR_XIR_UNIT, loop_blocks, 2, loop, 2},
        {"reverse", 7, NULL, 0, XR_XIR_BOOL, reverse_blocks, 3, reverse, 6},
        {"false", 5, NULL, 0, XR_XIR_BOOL, pair_blocks, 1, boolean, 2},
        {"max", 3, NULL, 0, XR_XIR_I64, pair_blocks, 1, maximum, 2}
    };
    const XrXirModule module = {XR_XIR_BUILT, functions, 9};
    XrXirArtifact *artifact = NULL;
    CHECK(xr_xir_check(&module, NULL, &artifact, NULL) == XR_XIR_OK);
    return artifact;
}

static XrXirArtifact *fixture_lowered(void) {
    XrXirArtifact *checked = fixture_checked(), *lowered = NULL;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(checked, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    return lowered;
}
#endif
