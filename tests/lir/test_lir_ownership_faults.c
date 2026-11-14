/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_ownership_faults.c - Verifier and snapshot publication failure sweep
 */
#include "base/xmalloc.h"
#include "lir/xlir_ownership.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_LIR_OK)

static size_t attempt, fail_at = SIZE_MAX, live, live_bytes;
typedef union OwnershipAllocation {
    long double alignment;
    void *pointer;
    size_t bytes;
} OwnershipAllocation;

static void *ownership_allocate(size_t bytes) {
    if (attempt++ == fail_at || bytes > SIZE_MAX - sizeof(OwnershipAllocation)) return NULL;
    OwnershipAllocation *allocation = xr_malloc(sizeof(*allocation) + bytes);
    if (!allocation) return NULL;
    allocation->bytes = bytes;
    ++live; live_bytes += bytes;
    return allocation + 1;
}
static void *ownership_callocate(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) return NULL;
    void *memory = ownership_allocate(count * size);
    if (memory) memset(memory, 0, count * size);
    return memory;
}
static void ownership_free(void *memory) {
    if (!memory) return;
    OwnershipAllocation *allocation = (OwnershipAllocation *)memory - 1;
    CHECK(live && live_bytes >= allocation->bytes);
    --live; live_bytes -= allocation->bytes;
    xr_free(allocation);
}
#undef xr_malloc
#undef xr_calloc
#undef xr_free
#define xr_malloc(bytes) ownership_allocate(bytes)
#define xr_calloc(count, size) ownership_callocate(count, size)
#define xr_free(memory) ownership_free(memory)
#include "lir/xlir_verify.c"
#include "lir_array_fixture.h"
#include "lir_ownership_fixture.h"

static void sweep(XrLirModule *input) {
    XrLirOwnershipPlan *output = NULL;
    CHECK(!live && !live_bytes);
    attempt = 0;
    OK(xr_lir_verify(input, NULL));
    size_t verification = attempt;
    CHECK(!live && !live_bytes);
    attempt = 0;
    OK(xr_lir_ownership_plan_create(input, &output, NULL));
    size_t count = attempt;
    CHECK(count > verification && live && live_bytes);
    xr_lir_ownership_plan_destroy(output); output = NULL;
    CHECK(!live && !live_bytes);
    for (size_t point = 0; point < count; ++point) {
        attempt = 0; fail_at = point;
        XrLirDiagnostic diagnostic;
        CHECK(xr_lir_ownership_plan_create(input, &output, &diagnostic) == XR_LIR_OUT_OF_MEMORY);
        CHECK(!output && diagnostic.status == XR_LIR_OUT_OF_MEMORY && attempt > point &&
            !live && !live_bytes);
        fail_at = SIZE_MAX;
        OK(xr_lir_verify(input, NULL));
        CHECK(!live && !live_bytes);
    }
    OK(xr_lir_ownership_plan_create(input, &output, NULL));
    size_t occupied_count = live, occupied_bytes = live_bytes;
    XrLirOwnershipPlan *saved = output;
    attempt = 0; fail_at = 0;
    CHECK(xr_lir_ownership_plan_create(input, &output, NULL) == XR_LIR_INVALID_ARGUMENT &&
        output == saved && !attempt && live == occupied_count && live_bytes == occupied_bytes);
    fail_at = SIZE_MAX;
    xr_lir_module_destroy(input);
    XrLirOwnershipFact fact;
    OK(xr_lir_ownership_fact_at(output, 0, &fact));
    CHECK(fact.kind && live == occupied_count && live_bytes == occupied_bytes);
    xr_lir_ownership_plan_destroy(output);
    CHECK(!live && !live_bytes);
    printf("ownership snapshot failure points: %zu (verifier %zu)\n", count, verification);
}

static void rejected_after_collection(void) {
    XrLirModule *m = NULL;
    OK(ownership_fixture(XR_LIR_STRING, false, 2, &m));
    XrLirInstructionView *ret = &m->functions[0].blocks[0].instructions[3].view;
    uint32_t *operand = (uint32_t *)ret->operands;
    uint32_t saved = *operand;
    *operand = m->functions[0].parameters[0];
    XrLirOwnershipPlan *plan = NULL;
    XrLirDiagnostic diagnostic;
    CHECK(xr_lir_ownership_plan_create(m, &plan, &diagnostic) == XR_LIR_VERIFY_ERROR &&
        !plan && !live && !live_bytes);
    *operand = saved;
    OK(xr_lir_verify(m, NULL));
    xr_lir_module_destroy(m);
    CHECK(!live && !live_bytes);
}

int main(void) {
    attempt = 0;
    CHECK(!lir_plan_allocate(SIZE_MAX, 2) && !attempt && !live && !live_bytes);
    XrLirType elements[] = {XR_LIR_I64, XR_LIR_STRING, XR_LIR_U8, XR_LIR_U64};
    for (uint32_t i = 0; i < 4; ++i) {
        XrLirModule *input = NULL;
        OK(array_fixture_create(elements[i], &input));
        sweep(input);
    }
    for (uint32_t i = 0; i < 3; ++i) {
        XrLirModule *input = NULL;
        OK(ownership_fixture(i == 1 ? XR_LIR_MAP_STRING_I64 : XR_LIR_STRING, i == 2, 2, &input));
        sweep(input);
    }
    XrLirModule *grown = NULL;
    OK(ownership_fixture(XR_LIR_STRING, false, 24, &grown));
    sweep(grown);
    rejected_after_collection();
    puts("LIR ownership snapshot allocation failures passed");
    return 0;
}
