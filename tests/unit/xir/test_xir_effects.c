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
#include "xir/xxir_generic.h"
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
static size_t match_attempts, match_fail=SIZE_MAX;
static XrXirStatus effect_match(const XrXirModule *module, uint32_t caller,
    const XrXirInstruction *call, XrXirType type, XrXirType actual, XrXirBudget *remaining) {
    if (match_attempts++==match_fail) return XR_XIR_OUT_OF_MEMORY;
    return xr_xir_call_type_matches(module,caller,call,type,actual,remaining);
}
#define xr_xir_call_type_matches effect_match
#undef xr_calloc
#undef xr_free
#define xr_calloc(n, size) effect_calloc(n, size)
#define xr_free(p) effect_free(p)
#include "xir/xxir_effects.c"
#undef xr_calloc
#undef xr_free
#undef xr_xir_call_type_matches
#include "xir_enum_ops_fixture.h"
#include "xir_enum_generic_fixture.h"
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
static size_t effect_failures(XrXirArtifact *artifact) {
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
    return sites;
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
        effect_expect(effects, 4, XR_XIR_EFFECT_UNKNOWN, XR_XIR_EFFECT_UNKNOWN);
        effect_expect(effects, 5, XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_NONE);
        CHECK(!xr_xir_effects_function(effects, 8)); xr_xir_effects_free(effects); CHECK(!live);
    }
    xr_xir_artifact_free(lowered);
    size_t sites = effect_failures(artifact);
    XrXirBudget budget = xr_xir_default_budget(); budget.work = 1;
    XrXirEffects *effects = NULL;
    CHECK(xr_xir_effects_analyze(artifact, &budget, &effects) == XR_XIR_BUDGET && !effects && !live);
    CHECK(xr_xir_effects_analyze(artifact, NULL, &effects) == XR_XIR_OK);
    xr_xir_artifact_free(artifact);
    effect_expect(effects, 4, XR_XIR_EFFECT_UNKNOWN, XR_XIR_EFFECT_UNKNOWN);
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
static void effect_enum_ownership(void) {
    XrXirArtifact *original=enum_ops_checked(0,false), *artifact=NULL;
    XrXirModule built=*xr_xir_artifact_module(original); built.stage=XR_XIR_BUILT;
    XrXirFunction functions[3]; memcpy(functions,built.functions,sizeof(functions));
    XrXirInstruction ops[11]; memcpy(ops,functions[2].instructions,sizeof(ops));
    ops[10]=(XrXirInstruction){XR_XIR_THROW,XR_XIR_UNIT,{2},{0},0,{0}};
    functions[2].instructions=ops; built.functions=functions;
    CHECK(xr_xir_check(&built,NULL,&artifact,NULL)==XR_XIR_OK); xr_xir_artifact_free(original);
    size_t sites=effect_failures(artifact); XrXirEffects *effects=NULL;
    CHECK(xr_xir_effects_analyze(artifact,NULL,&effects)==XR_XIR_OK);
    xr_xir_artifact_free(artifact);
    CHECK(xr_xir_effects_error(effects,2,(XrXirType)256,1));
    CHECK(!xr_xir_effects_error(effects,2,(XrXirType)256,0));
    CHECK(!xr_xir_effects_error_unknown(effects,2));
    xr_xir_effects_free(effects); CHECK(!live);
    artifact=enum_ops_lowered(false); sites+=effect_failures(artifact); xr_xir_artifact_free(artifact);
    printf("Error atoms: %zu allocation failure sites released across checked and lowered\n",sites);
}
static void effect_extreme_budgets(void) {
    XrXirEffects effects={0}; effects.words=UINT32_MAX/64+1; effects.atom_count=UINT32_MAX-1;
    XrXirFunction function={0}; function.parameter_count=UINT32_MAX-1; function.instruction_count=1;
    function.block_count=UINT32_MAX;
    XrXirModule module={0}; module.functions=&function; module.function_count=1;
    XrXirBudget budget=xr_xir_default_budget(); budget.scratch_bytes=UINT64_MAX; budget.work=UINT64_MAX;
    ErrorFlow flow={0}; flow.module=&module; flow.effects=&effects; flow.remaining=&budget;
    attempts=0; CHECK(error_function(&flow,0)==XR_XIR_BUDGET && !attempts && !live);
    flow.values=UINT32_MAX; flow.stride=(size_t)UINT32_MAX*effects.words;
    budget.work=UINT64_MAX;
    CHECK(error_edge(&flow,0,0,NULL,true)==XR_XIR_BUDGET && !attempts && !live);
}
static void effect_generic_errors(void) {
    enum_generic_cases();
    XrXirArtifact *source=enum_generic_checked(0), *artifact=NULL;
    XrXirModule built=*xr_xir_artifact_module(source); built.stage=XR_XIR_BUILT;
    XrXirFunction functions[4]; memcpy(functions,built.functions,sizeof(functions));
    XrXirInstruction ops[2]; memcpy(ops,functions[2].instructions,sizeof(ops));
    ops[1]=(XrXirInstruction){XR_XIR_THROW,XR_XIR_UNIT,{1},{0},0,{0}};
    functions[2].instructions=ops; built.functions=functions;
    CHECK(xr_xir_check(&built,NULL,&artifact,NULL)==XR_XIR_OK); xr_xir_artifact_free(source);
    size_t allocations=effect_failures(artifact);
    match_attempts=0; XrXirEffects *effects=NULL;
    CHECK(xr_xir_effects_analyze(artifact,NULL,&effects)==XR_XIR_OK);
    CHECK(xr_xir_effects_error(effects,1,(XrXirType)257,1));
    CHECK(!xr_xir_effects_error(effects,1,(XrXirType)256,1));
    CHECK(!xr_xir_effects_error_unknown(effects,1) && !xr_xir_effects_error_unidentified(effects,1));
    size_t sites=match_attempts; CHECK(sites); xr_xir_effects_free(effects);
    for (size_t i=0;i<sites;++i) {
        match_attempts=0; match_fail=i; effects=NULL;
        CHECK(xr_xir_effects_analyze(artifact,NULL,&effects)==XR_XIR_OUT_OF_MEMORY && !effects && !live);
    }
    match_fail=SIZE_MAX;
    EffectErrorAtom atoms[]={{(XrXirType)256,1},{(XrXirType)257,1}};
    uint64_t errors[4]={0,0,4,0}, out=0;
    XrXirEffects facts={0}; facts.count=4; facts.atom_count=2; facts.words=1; facts.atoms=atoms; facts.errors=errors;
    ErrorFlow flow={0}; flow.module=xr_xir_artifact_module(artifact);
    flow.function=&flow.module->functions[1]; flow.effects=&facts;
    XrXirBudget budget=xr_xir_default_budget(); budget.metadata_bytes=0; flow.remaining=&budget;
    CHECK(error_call(&flow,&flow.function->instructions[1],&out)==XR_XIR_BUDGET && !out);
    xr_xir_artifact_free(artifact); CHECK(!live);
    printf("Generic error substitution: %zu allocation failures, %zu matcher failures released\n",allocations,sites);
}
int main(void) {
    effect_generic_errors();
    effect_extreme_budgets();
    effect_enum_ownership();
    effect_long_cycle();
    effect_cases(false); effect_cases(true);
    XrXirEffects *effects = NULL;
    CHECK(xr_xir_effects_analyze(NULL, NULL, &effects) == XR_XIR_BAD_STAGE && !effects);
    CHECK(!xr_xir_effects_function(NULL, 0));
    puts("Control effect fixed points, error edges and physical ownership passed");
    return 0;
}
