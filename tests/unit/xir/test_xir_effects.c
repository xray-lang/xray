/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_effects.c - Recursive effects, dynamic calls and bounded ownership
 */
#include "base/xmalloc.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static size_t attempts, fail_at = SIZE_MAX, live;
static void *effect_calloc(size_t n, size_t size) {
    if (attempts++ == fail_at) return NULL;
    void *p = xr_calloc(n, size); if (p) ++live; return p;
}
static void effect_free(void *p) {
    if (p) { CHECK(live); --live; } xr_free(p);
}
#undef xr_calloc
#undef xr_free
#define xr_calloc(n, size) effect_calloc(n, size)
#define xr_free(p) effect_free(p)
#include "xir/xxir_effects.c"
#undef xr_calloc
#undef xr_free
static XrXirArtifact *effect_fixture(bool suspends) {
    XrXirType parameter = (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirTypeNode node = {0}; node.kind = XR_XIR_TYPE_CALLABLE; node.result = XR_XIR_UNIT;
    XrXirTypes types = {&node, 1, NULL};
    XrXirInstruction a[] = {{XR_XIR_CALL, XR_XIR_UNIT, {0}, {0}, 1, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirInstruction b[] = {{XR_XIR_CALL, XR_XIR_UNIT, {0}, {0}, 0, {0}},
        {XR_XIR_SUSPEND, XR_XIR_UNIT, {0}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    if (!suspends) b[1] = b[2];
    XrXirInstruction dynamic[] = {{XR_XIR_CALL_INDIRECT, XR_XIR_UNIT, {0}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirInstruction handled[] = {{XR_XIR_INVOKE_INDIRECT, XR_XIR_UNIT, {0}, {1, 2}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}},
        {XR_XIR_INVOKE_ERROR, XR_XIR_ERROR, {0}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirInstruction rethrow[4]; memcpy(rethrow, handled, sizeof(handled));
    rethrow[3] = (XrXirInstruction) {XR_XIR_THROW, XR_XIR_UNIT, {3}, {0}, 0, {0}};
    XrXirInstruction reference[] = {{XR_XIR_FUNCTION_REF, (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE, {0}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirBlock pair = {0, 2, 0}, triple = {0, suspends ? 3u : 2u, 0};
    XrXirBlock branches[] = {{0, 1, 0}, {1, 1, 0}, {2, 2, 0}};
    XrXirInstruction init = {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}};
    XrXirBlock init_block = {0, 1, 0};
    XrXirInstruction entry[] = {{XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirFunction functions[] = {
        {"a", 1, NULL, 0, XR_XIR_UNIT, &pair, 1, a, 2, NULL, 0},
        {"b", 1, NULL, 0, XR_XIR_UNIT, &triple, 1, b, triple.count, NULL, 0},
        {"dynamic", 7, &parameter, 1, XR_XIR_UNIT, &pair, 1, dynamic, 2, NULL, 0},
        {"handled", 7, &parameter, 1, XR_XIR_UNIT, branches, 3, handled, 4, NULL, 0},
        {"rethrow", 7, &parameter, 1, XR_XIR_UNIT, branches, 3, rethrow, 4, NULL, 0},
        {"reference", 9, NULL, 0, XR_XIR_UNIT, &pair, 1, reference, 2, NULL, 0},
        {"init", 4, NULL, 0, XR_XIR_UNIT, &init_block, 1, &init, 1, NULL, 0},
        {"entry", 5, NULL, 0, XR_XIR_I64, &pair, 1, entry, 2, NULL, 0}};
    XrXirSourceModule source = {"root", 4, NULL, 0, 6};
    XrXirFunctionIdentity identities[8] = {{0}};
    XrXirDeclarations declarations = {&source, 1, identities, NULL, 0, NULL, 0, 0, 7};
    XrXirModule module = {XR_XIR_BUILT, functions, 8, &declarations, NULL, &types, NULL};
    XrXirArtifact *artifact = NULL; XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_check(&module, NULL, &artifact, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "effect fixture %u: f=%u b=%u i=%u\n", status,
        diagnostic.function, diagnostic.block, diagnostic.instruction);
    CHECK(status == XR_XIR_OK); return artifact;
}
static void effect_expect(const XrXirEffects *effects, uint32_t f, XrXirEffect suspend, XrXirEffect throws) {
    const XrXirFunctionEffects *fact = xr_xir_effects_function(effects, f);
    CHECK(fact && fact->suspend == suspend && fact->throws == throws);
}
static void effect_cases(bool suspends) {
    XrXirArtifact *artifact = effect_fixture(suspends);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(artifact, NULL, &packet, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(artifact); artifact = NULL;
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &artifact, NULL) == XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);
    XrXirArtifact *lowered = NULL;
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(artifact, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    for (unsigned stage = 0; stage < 2; ++stage) {
        XrXirEffects *effects = NULL;
        CHECK(xr_xir_effects_analyze(stage ? lowered : artifact, NULL, &effects) == XR_XIR_OK);
        for (uint32_t f = 0; f < 2; ++f)
            effect_expect(effects, f, suspends ? XR_XIR_EFFECT_MAY : XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_NONE);
        effect_expect(effects, 2, XR_XIR_EFFECT_UNKNOWN, XR_XIR_EFFECT_UNKNOWN);
        effect_expect(effects, 3, XR_XIR_EFFECT_UNKNOWN, XR_XIR_EFFECT_NONE);
        effect_expect(effects, 4, XR_XIR_EFFECT_UNKNOWN, XR_XIR_EFFECT_MAY);
        effect_expect(effects, 5, XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_NONE);
        CHECK(!xr_xir_effects_function(effects, 8)); xr_xir_effects_free(effects); CHECK(!live);
    }
    xr_xir_artifact_free(lowered);
    size_t sites = 0;
    for (size_t attempt = 0; attempt <= sites; ++attempt) {
        attempts = 0; fail_at = attempt ? attempt - 1 : SIZE_MAX;
        XrXirEffects *effects = NULL;
        XrXirStatus status = xr_xir_effects_analyze(artifact, NULL, &effects);
        if (!attempt) { CHECK(status == XR_XIR_OK && effects); sites = attempts; }
        else CHECK(status == XR_XIR_OUT_OF_MEMORY && !effects);
        xr_xir_effects_free(effects); CHECK(!live);
    }
    fail_at = SIZE_MAX;
    XrXirBudget budget = xr_xir_default_budget(); budget.work = 1;
    XrXirEffects *effects = NULL;
    CHECK(xr_xir_effects_analyze(artifact, &budget, &effects) == XR_XIR_BUDGET && !effects && !live);
    CHECK(xr_xir_effects_analyze(artifact, NULL, &effects) == XR_XIR_OK);
    xr_xir_artifact_free(artifact);
    effect_expect(effects, 4, XR_XIR_EFFECT_UNKNOWN, XR_XIR_EFFECT_MAY);
    xr_xir_effects_free(effects); CHECK(!live);
    printf("Control effects: cycle seed %u, %zu allocation failure sites released\n", suspends, sites);
}
static void effect_long_cycle(void) {
    enum { COUNT = 128 };
    XrXirFunction functions[COUNT]; XrXirBlock blocks[COUNT]; XrXirInstruction ops[COUNT][3];
    for (uint32_t f = 0; f < COUNT; ++f) {
        ops[f][0] = (XrXirInstruction) {XR_XIR_CALL, XR_XIR_UNIT, {0}, {0}, (f + 1) % COUNT, {0}};
        ops[f][1] = (XrXirInstruction) {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}};
        ops[f][2] = (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}};
        blocks[f] = (XrXirBlock) {0, 3, 0};
        functions[f] = (XrXirFunction) {"cycle", 5, NULL, 0, XR_XIR_UNIT, &blocks[f], 1, ops[f], 3, NULL, 0};
    }
    ops[COUNT - 1][1] = (XrXirInstruction) {XR_XIR_SUSPEND, XR_XIR_UNIT, {0}, {0}, 0, {0}};
    XrXirModule module = {XR_XIR_BUILT, functions, COUNT, NULL, NULL, NULL, NULL};
    XrXirArtifact *artifact = NULL;
    CHECK(xr_xir_check(&module, NULL, &artifact, NULL) == XR_XIR_OK);
    XrXirEffects *effects = NULL;
    CHECK(xr_xir_effects_analyze(artifact, NULL, &effects) == XR_XIR_OK);
    for (uint32_t f = 0; f < COUNT; ++f) effect_expect(effects, f, XR_XIR_EFFECT_MAY, XR_XIR_EFFECT_NONE);
    xr_xir_effects_free(effects); effects = NULL; CHECK(!live);
    XrXirBudget proof = xr_xir_default_budget(), budget = proof;
    CHECK(xr_xir_verify_remaining(xr_xir_artifact_module(artifact), &proof, NULL) == XR_XIR_OK);
    budget.work -= proof.work;
    CHECK(xr_xir_effects_analyze(artifact, &budget, &effects) == XR_XIR_BUDGET && !effects && !live);
    budget = xr_xir_default_budget(); budget.metadata_bytes -= proof.metadata_bytes;
    CHECK(xr_xir_effects_analyze(artifact, &budget, &effects) == XR_XIR_BUDGET && !effects && !live);
    budget = xr_xir_default_budget(); budget.scratch_bytes = 100;
    proof = budget;
    CHECK(xr_xir_verify_remaining(xr_xir_artifact_module(artifact), &proof, NULL) == XR_XIR_OK);
    CHECK(xr_xir_effects_analyze(artifact, &budget, &effects) == XR_XIR_BUDGET && !effects && !live);
    xr_xir_artifact_free(artifact);
}
int main(void) {
    effect_long_cycle();
    effect_cases(false); effect_cases(true);
    XrXirEffects *effects = NULL;
    CHECK(xr_xir_effects_analyze(NULL, NULL, &effects) == XR_XIR_BAD_STAGE && !effects);
    CHECK(!xr_xir_effects_function(NULL, 0));
    puts("Control effect fixed points, error edges and physical ownership passed");
    return 0;
}
