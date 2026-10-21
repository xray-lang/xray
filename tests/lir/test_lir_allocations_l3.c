/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_allocations.c - Observe physical cleanup after construction failures
 */
#include "base/xchecks.h"
#include "base/xmalloc.h"
#include "lir/xlir_builder.h"
#include "lir/xlir_verify.h"
#include <stdio.h>
#include <stdlib.h>

#define REQUIRE(c)                                                                                 \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%d: %s\n", __LINE__, #c);                                             \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static void *allocated[128];
static size_t live, attempts, fail_at = SIZE_MAX;

static void *observed_allocate(size_t size) {
    if (attempts++ == fail_at)
        return NULL;
    void *memory = xr_malloc(size);
    REQUIRE(memory != NULL);
    size_t slot = 0;
    while (slot < 128 && allocated[slot])
        ++slot;
    REQUIRE(slot < 128);
    allocated[slot] = memory;
    ++live;
    return memory;
}
static void observed_free(void *memory) {
    if (!memory)
        return;
    size_t slot = 0;
    while (slot < 128 && allocated[slot] != memory)
        ++slot;
    REQUIRE(slot < 128 && live);
    allocated[slot] = NULL;
    --live;
    xr_free(memory);
}

/* Observe the real implementation rather than a model of its cleanup paths. */
#undef xr_malloc
#undef xr_free
#define xr_malloc(size) observed_allocate(size)
#define xr_free(memory) observed_free(memory)
#include "lir/xlir.c"

#define TRY(expression)                                                                            \
    do {                                                                                           \
        status = (expression);                                                                     \
        if (status != XR_LIR_OK)                                                                   \
            goto done;                                                                             \
    } while (0)
static XrLirStatus build_allocating_graph(void) {
    XrLirModule *m = NULL;
    XrLirStatus status;
    XrLirType type = XR_LIR_I64;
    XrLirIntrinsic intrinsic = {19, &type, 1, type, true};
    XrLirRegistry registry = {&intrinsic, 1};
    TRY(xr_lir_module_create(&registry, &m));
    XrLirFunctionSpec spec = {.name = "owned",
                              .name_length = 5,
                              .parameters = &type,
                              .parameter_count = 1,
                              .result = type};
    XrLirFunction function = {0};
    TRY(xr_lir_declare_function(m, &spec, &function));
    TRY(xr_lir_define_function(m, function));
    XrLirBlock entry = {0}, join = {0};
    TRY(xr_lir_add_block(m, function, NULL, 0, &entry));
    TRY(xr_lir_add_block(m, function, &type, 1, &join));
    TRY(xr_lir_set_entry(m, function, entry));
    XrLirValue input = {0}, answer = {0}, value = {0}, sum = {0};
    TRY(xr_lir_function_parameter(m, function, 0, &input));
    TRY(xr_lir_block_parameter(m, join, 0, &answer));
    XrLirInstruction instruction = {
        .op = XR_LIR_CONST, .type = type, .bits = 7, .source = {"oom.xr", 6, 1, 1, 1, 2}};
    TRY(xr_lir_emit_value(m, entry, &instruction, &value));
    XrLirValue operands[] = {input, value};
    instruction = (XrLirInstruction) {
        .op = XR_LIR_ADD, .type = type, .operands = operands, .operand_count = 2};
    TRY(xr_lir_emit_value(m, entry, &instruction, &sum));
    instruction = (XrLirInstruction) {
        .op = XR_LIR_BR, .type = XR_LIR_UNIT, .edges = {{join, &sum, 1}}, .edge_count = 1};
    TRY(xr_lir_set_terminator(m, entry, &instruction));
    instruction = (XrLirInstruction) {
        .op = XR_LIR_RET, .type = XR_LIR_UNIT, .operands = &answer, .operand_count = 1};
    TRY(xr_lir_set_terminator(m, join, &instruction));
    TRY(xr_lir_module_finish(m, NULL));
    TRY(xr_lir_verify(m, NULL));
done:
    if (status != XR_LIR_OK && m)
        REQUIRE(xr_lir_module_finish(m, NULL) != XR_LIR_OK);
    xr_lir_module_destroy(m);
    REQUIRE(live == 0);
    return status;
}

static void registry_transfer_allocations(void) {
    fail_at = SIZE_MAX;
    attempts = 0;
    XrLirType type = XR_LIR_I64;
    XrLirIntrinsic intrinsic = {91, &type, 1, type, true};
    XrLirRegistry registry = {&intrinsic, 1};
    XrLirModule *source = NULL, *copy = NULL;
    REQUIRE(xr_lir_module_create(&registry, &source) == XR_LIR_OK);
    REQUIRE(xr_lir_module_finish(source, NULL) == XR_LIR_OK);
    REQUIRE(xr_lir_intrinsic_at(source, 0, &intrinsic) == XR_LIR_OK);
    size_t baseline = live;
    attempts = 0;
    REQUIRE(xr_lir_module_create(&registry, &copy) == XR_LIR_OK);
    size_t total = attempts;
    xr_lir_module_destroy(copy);
    REQUIRE(live == baseline && total >= 3);
    for (size_t i = 0; i < total; ++i) {
        fail_at = i;
        attempts = 0;
        copy = NULL;
        REQUIRE(xr_lir_module_create(&registry, &copy) == XR_LIR_OUT_OF_MEMORY && !copy);
        REQUIRE(live == baseline);
    }
    fail_at = SIZE_MAX;
    xr_lir_module_destroy(source);
    REQUIRE(!live);
}

int main(void) {
    REQUIRE(build_allocating_graph() == XR_LIR_OK);
    size_t successful_attempts = attempts;
    const size_t points[] = {0, 1, 4, 7, 10, 13, 17, 21, 25};
    size_t exercised = 0;
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); ++i) {
        if (points[i] >= successful_attempts)
            continue;
        fail_at = points[i];
        attempts = 0;
        REQUIRE(build_allocating_graph() == XR_LIR_OUT_OF_MEMORY);
        ++exercised;
    }
    fail_at = successful_attempts - 1;
    attempts = 0;
    REQUIRE(build_allocating_graph() == XR_LIR_OUT_OF_MEMORY);
    REQUIRE(exercised >= 5);
    fail_at = SIZE_MAX;
    attempts = 0;
    REQUIRE(build_allocating_graph() == XR_LIR_OK);
    registry_transfer_allocations();
    puts("scalar LIR allocation-failure cleanup passed");
    return 0;
}
