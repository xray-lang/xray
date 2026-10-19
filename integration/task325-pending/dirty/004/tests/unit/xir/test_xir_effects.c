/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_effects.c - Recursive effects, dynamic calls and bounded ownership
 */
#include "xir_construction_fixture.h"
#include "base/xmalloc.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effects_compile_owner.h"
#include "xir/xxir_effects.c"
#include "xir_effects_enum_fixture.h"
#include "xir_effect_witness_cases.h"
_Static_assert(XR_XIR_CALLABLE_NO_SUSPEND == 1u && XR_XIR_CALLABLE_ROOT_NONE == 2u &&
    XR_XIR_CALLABLE_ROOT_UNRESOLVED == 8u && XR_XIR_CALLABLE_ROOT_MASK == 14u, "independent callable bits");
static XrXirArtifact *effect_fixture(bool suspends) {
    XrXirType parameter = (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirTypeNode node = {0}; node.kind = XR_XIR_TYPE_CALLABLE; node.result = XR_XIR_UNIT;
    node.flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    XrXirTypes types = {&node, 1, NULL, NULL};
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
    XrXirBlock pair = {0, 2, 0, 0}, triple = {0, suspends ? 3u : 2u, 0, 0};
    XrXirBlock branches[] = {{0, 1, 0, 0}, {1, 1, 0, 0}, {2, 2, 0, 0}};
    XrXirInstruction init = {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}};
    XrXirBlock init_block = {0, 1, 0, 0};
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
    XrXirDeclarations declarations = {&source, 1, identities, NULL, 0, NULL, 0, 0, 7, NULL};
    XrXirModule module = {XR_XIR_BUILT, functions, 8, &declarations, NULL, &types, NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *artifact = NULL; XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xir_fixture_check(&effect_context, &module, &artifact, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "effect fixture %u: f=%u b=%u i=%u\n", status,
        diagnostic.function, diagnostic.block, diagnostic.instruction);
    CHECK(status == XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_module(artifact)->types->nodes[0].flags == 8u);
    return artifact;
}
static void effect_expect(const XrXirEffects *effects, uint32_t f, XrXirEffect suspend, XrXirEffect throws) {
    XrXirEffectFacts fact_read=xr_xir_effects_read(effects, f);
    const XrXirFunctionEffects *fact=(fact_read.status==XR_XIR_OK?&fact_read.control:NULL);
    CHECK(fact && fact->suspend == suspend && fact->throws == throws);
}
static void effect_error_query_work(void) {
    XrXirArtifact *artifact=effect_fixture(false);XrXirEffects *effects=NULL;
    CHECK(effect_analyze(artifact,&effects)==XR_XIR_OK);
    effect_artifact_free(artifact);
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(effects->resources,&before)==XR_COMPILE_RESOURCE_OK);
    size_t calls=attempts;uint64_t work=77;
    CHECK(xr_xir_effects_error_query_work(NULL,0,&work)==XR_XIR_BAD_STRUCTURE && work==77);
    CHECK(xr_xir_effects_error_query_work(effects,8,&work)==XR_XIR_BAD_STRUCTURE && work==77);
    CHECK(xr_xir_effects_error_query_work(effects,0,NULL)==XR_XIR_BAD_STRUCTURE);
    for (uint32_t f=0;f<8;++f) {
        CHECK(xr_xir_effects_error_query_work(effects,f,&work)==XR_XIR_OK && work==2);
        CHECK(!xr_xir_effects_error(effects,f,XR_XIR_I64,0));
    }
    CHECK(xr_compile_resources_stats(effects->resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(calls==attempts && !memcmp(&before,&after,sizeof(before)));
    effect_summary_free(effects);CHECK(effect_balanced());
    puts("Immutable Error query work: producer retired, exact empty bound, invalid outputs preserved, no compiler debit PASS");
}
static size_t effect_failures(XrXirArtifact *artifact) {
    XrXirEffects *summary = NULL; attempts = 0; fail_at = SIZE_MAX;
    CHECK(effect_analyze(artifact,&summary) == XR_XIR_OK); size_t sites = attempts;
    effect_summary_free(summary); CHECK(effect_balanced());
    for (size_t point = 0; point < sites; ++point) {
        EffectMark mark = effect_mark(); XrXirCompileContext c = effect_owner_new(effect_caps());
        uint64_t baseline = effect_stats(&c).live_bytes; XrXirArtifact *copy = NULL;
        CHECK(effect_reproduce(artifact,&c,&copy) == XR_XIR_OK);
        EffectMark retained = effect_mark(); summary = NULL; attempts = 0; injected = false; fail_at = point;
        XrXirStatus status = xr_xir_compile_effects_analyze(copy,&summary);
        CHECK(injected && status == XR_XIR_OUT_OF_MEMORY && !summary);
        effect_mark_check(retained); fail_at = SIZE_MAX;
        xr_xir_compile_artifact_free(copy); effect_owner_free(&c,baseline); effect_mark_check(mark);
    }
    return sites;
}
static void effect_cases(bool suspends) {
    XrXirArtifact *artifact = effect_fixture(suspends);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(artifact, &packet, NULL) == XR_XIR_OK);
    effect_artifact_free(artifact); artifact = NULL;
    CHECK(xr_xir_compile_checked_read(&effect_context, packet.bytes, packet.length, &artifact, NULL) == XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    XrXirArtifact *lowered = NULL;
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(artifact, &target, &lowered, NULL) == XR_XIR_OK);
    for (unsigned stage = 0; stage < 2; ++stage) {
        XrXirEffects *effects = NULL;
        CHECK(effect_analyze(stage ? lowered : artifact, &effects) == XR_XIR_OK);
        effect_witness_paths(xr_xir_compile_artifact_module(stage ? lowered : artifact), effects);
        if (suspends) {
            XrXirEffectFacts a_read=xr_xir_effects_read(effects, 0);
            const XrXirEffectWitness *a=(a_read.status==XR_XIR_OK && a_read.suspend_witness.cause!=0?&a_read.suspend_witness:NULL);
            XrXirEffectFacts b_read=xr_xir_effects_read(effects, 1);
            const XrXirEffectWitness *b=(b_read.status==XR_XIR_OK && b_read.suspend_witness.cause!=0?&b_read.suspend_witness:NULL);
            CHECK(a && a->instruction == 0 && a->callee == 1 && a->distance == 1);
            CHECK(b && b->instruction == 1 && b->distance == 0);
        }
        for (uint32_t f = 0; f < 2; ++f)
            effect_expect(effects, f, suspends ? XR_XIR_EFFECT_MAY : XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_NONE);
        effect_expect(effects, 2, XR_XIR_EFFECT_UNKNOWN, XR_XIR_EFFECT_UNKNOWN);
        effect_expect(effects, 3, XR_XIR_EFFECT_UNKNOWN, XR_XIR_EFFECT_NONE);
        effect_expect(effects, 4, XR_XIR_EFFECT_UNKNOWN, XR_XIR_EFFECT_UNKNOWN);
        effect_expect(effects, 5, XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_NONE);
        CHECK(!(xr_xir_effects_read(effects, 8).status==XR_XIR_OK)); effect_summary_free(effects); CHECK(effect_balanced());
    }
    effect_artifact_free(lowered);
    size_t sites = effect_failures(artifact);
    effect_analysis_work_cut(artifact,1,false);
    XrXirEffects *effects = NULL;
    CHECK(effect_analyze(artifact, &effects) == XR_XIR_OK);
    effect_artifact_free(artifact);
    effect_expect(effects, 4, XR_XIR_EFFECT_UNKNOWN, XR_XIR_EFFECT_UNKNOWN);
    XrXirEffectFacts owned_read=xr_xir_effects_read(effects, 4);
    const XrXirEffectWitness *owned=(owned_read.status==XR_XIR_OK && owned_read.suspend_witness.cause!=0?&owned_read.suspend_witness:NULL);
    CHECK(owned && owned->cause == XR_XIR_EFFECT_CAUSE_INDIRECT && owned->instruction == 0);
    effect_summary_free(effects); CHECK(effect_balanced());
    printf("Control effects: cycle seed %u, %zu allocation failure sites released\n", suspends, sites);
}
static void effect_long_cycle(void) {
    enum { COUNT = 128 };
    XrXirFunction functions[COUNT + 2]; XrXirBlock blocks[COUNT]; XrXirInstruction ops[COUNT][3];
    for (uint32_t f = 0; f < COUNT; ++f) {
        ops[f][0] = (XrXirInstruction) {XR_XIR_CALL, XR_XIR_UNIT, {0}, {0}, (f + 1) % COUNT, {0}};
        ops[f][1] = (XrXirInstruction) {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}};
        ops[f][2] = (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}};
        blocks[f] = (XrXirBlock) {0, 3, 0, 0};
        functions[f] = (XrXirFunction) {"cycle", 5, NULL, 0, XR_XIR_UNIT, &blocks[f], 1, ops[f], 3, NULL, 0};
    }
    ops[COUNT - 1][1] = (XrXirInstruction) {XR_XIR_SUSPEND, XR_XIR_UNIT, {0}, {0}, 0, {0}};
    XrXirInstruction init = {.op=XR_XIR_RETURN,.type=XR_XIR_UNIT};
    XrXirInstruction entry[] = {{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},{.op=XR_XIR_RETURN,.type=XR_XIR_UNIT,.args={0}}};
    XrXirBlock init_block={0,1,0,0}, entry_block={0,2,0,0};
    functions[COUNT]=(XrXirFunction){"init",4,NULL,0,XR_XIR_UNIT,&init_block,1,&init,1,NULL,0};
    functions[COUNT+1]=(XrXirFunction){"entry",5,NULL,0,XR_XIR_I64,&entry_block,1,entry,2,NULL,0};
    XrXirSourceModule source={"cycle",5,NULL,0,COUNT}; XrXirFunctionIdentity ids[COUNT+2]={{0}};
    XrXirDeclarations declarations={&source,1,ids,NULL,0,NULL,0,0,COUNT+1,NULL};
    XrXirModule module = {XR_XIR_BUILT, functions, COUNT+2, &declarations, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *artifact = NULL;
    CHECK(xir_fixture_check(&effect_context, &module, &artifact, NULL) == XR_XIR_OK);
    XrXirEffects *effects = NULL;
    CHECK(effect_analyze(artifact, &effects) == XR_XIR_OK);
    for (uint32_t f = 0; f < COUNT; ++f) effect_expect(effects, f, XR_XIR_EFFECT_MAY, XR_XIR_EFFECT_NONE);
    effect_witness_paths(xr_xir_compile_artifact_module(artifact), effects);
    for (uint32_t f = 0; f < COUNT; ++f)
        CHECK(xr_xir_effects_read(effects, f).suspend_witness.distance == COUNT - 1 - f);
    effect_summary_free(effects); effects = NULL; CHECK(effect_balanced());
    effect_analysis_work_cut(artifact,0,true);
    effect_budget_boundaries(artifact);
    effect_artifact_free(artifact);
}
static void effect_enum_ownership(void) {
    XrXirArtifact *original=effect_enum_ops_checked(&effect_context, 0,false), *artifact=NULL;
    XrXirModule built=*xr_xir_compile_artifact_module(original); built.stage=XR_XIR_BUILT;
    XrXirFunction functions[3]; memcpy(functions,built.functions,sizeof(functions));
    XrXirInstruction ops[11]; memcpy(ops,functions[2].instructions,sizeof(ops));
    ops[10]=(XrXirInstruction){XR_XIR_THROW,XR_XIR_UNIT,{2},{0},0,{0}};
    functions[2].instructions=ops; built.functions=functions;
    CHECK(xir_fixture_check(&effect_context, &built, &artifact, NULL)==XR_XIR_OK); effect_artifact_free(original);
    size_t sites=effect_failures(artifact); XrXirEffects *effects=NULL;
    CHECK(effect_analyze(artifact,&effects)==XR_XIR_OK);
    effect_artifact_free(artifact);
    CHECK(xr_xir_effects_error(effects,2,(XrXirType)256,1));
    CHECK(!xr_xir_effects_error(effects,2,(XrXirType)256,0));
    CHECK(!xr_xir_effects_error_unknown(effects,2));
    effect_summary_free(effects); CHECK(effect_balanced());
    artifact=effect_enum_ops_lowered(&effect_context, false); sites+=effect_failures(artifact); effect_artifact_free(artifact);
    printf("Error atoms: %zu allocation failure sites released across checked and lowered\n",sites);
}
static void effect_extreme_budgets(void) {
    XrXirEffects effects={0}; effects.outcomes_work.words=UINT32_MAX/64+1; effects.outcomes_work.atom_count=UINT32_MAX-1;
    XrXirFunction function={0}; function.parameter_count=UINT32_MAX-1; function.instruction_count=1;
    function.block_count=UINT32_MAX;
    XrXirModule module={0}; module.functions=&function; module.function_count=1;
    ErrorFlow flow={0};flow.body=effect_body_raw_request(&effect_context,&module);
    flow.effects=&effects;flow.remaining=&effect_context;
    EffectMark mark=effect_mark(); attempts=0;
    CHECK(error_function(&flow,0)==XR_XIR_BUDGET && !attempts); effect_mark_check(mark);
    flow.values=UINT32_MAX; flow.stride=(size_t)UINT32_MAX*effects.outcomes_work.words;
    CHECK(error_edge(&flow,0,0,NULL,true)==XR_XIR_BUDGET && !attempts); effect_mark_check(mark);

}
static void effect_generic_errors(void) {
    effect_enum_generic_cases(&effect_context);
    XrXirArtifact *source=effect_enum_generic_checked(&effect_context, 0), *artifact=NULL;
    XrXirModule built=*xr_xir_compile_artifact_module(source); built.stage=XR_XIR_BUILT;
    XrXirFunction functions[4]; memcpy(functions,built.functions,sizeof(functions));
    XrXirInstruction ops[2]; memcpy(ops,functions[2].instructions,sizeof(ops));
    ops[1]=(XrXirInstruction){XR_XIR_THROW,XR_XIR_UNIT,{1},{0},0,{0}};
    functions[2].instructions=ops; built.functions=functions;
    CHECK(xir_fixture_check(&effect_context, &built, &artifact, NULL)==XR_XIR_OK); effect_artifact_free(source);
    size_t allocations=effect_failures(artifact);
    XrXirEffects *effects=NULL;
    CHECK(effect_analyze(artifact,&effects)==XR_XIR_OK);
    CHECK(xr_xir_effects_error(effects,1,(XrXirType)257,1));
    CHECK(!xr_xir_effects_error(effects,1,(XrXirType)256,1));
    CHECK(!xr_xir_effects_error_unknown(effects,1) && !xr_xir_effects_error_unidentified(effects,1));
    effect_summary_free(effects);
    EffectMark mark=effect_mark(); XrCompileResourceLimits caps=effect_caps();
    caps.allocated_bytes=caps.live_bytes=effect_baseline.live_bytes;
    XrXirCompileContext budget=effect_owner_new(caps);
    EffectTerms terms={0}; terms.types=*xr_xir_compile_artifact_module(artifact)->types; terms.remaining=&budget;
    XrXirType argument=XR_XIR_I64, output=XR_XIR_UNIT;
    XrXirGeneric arguments={0}; arguments.arguments=&argument; arguments.argument_count=1;
    CHECK(effect_terms_substitute(&terms,(XrXirType)256,&arguments,&output)==XR_XIR_BUDGET);
    CHECK(output==XR_XIR_UNIT); effect_terms_free(&terms);
    effect_owner_free(&budget,effect_baseline.live_bytes); effect_mark_check(mark);
    effect_artifact_free(artifact); CHECK(effect_balanced());
    printf("Generic error substitution: %zu allocation failures released\n",allocations);
}
static XrXirArtifact *effect_missing_fixture(bool wide) {
    XrXirArtifact *source = effect_enum_generic_checked(&effect_context, 0), *artifact = NULL;
    XrXirModule built = *xr_xir_compile_artifact_module(source); built.stage = XR_XIR_BUILT;
    XrXirFunction functions[4]; memcpy(functions, built.functions, sizeof(functions));
    XrXirInstruction make[2]; memcpy(make, functions[2].instructions, sizeof(make));
    make[1] = (XrXirInstruction){XR_XIR_THROW, XR_XIR_UNIT, {1}, {0}, 0, {0}};
    functions[2].instructions = make; functions[2].result = XR_XIR_I64;
    XrXirInstruction entry[3]; memcpy(entry, functions[1].instructions, sizeof(entry));
    entry[1].type = XR_XIR_I64;
    entry[2] = (XrXirInstruction){XR_XIR_RETURN, XR_XIR_UNIT, {1}, {0}, 0, {0}};
    XrXirBlock block = {0, 3, 0, 0};
    functions[1].instructions = entry; functions[1].instruction_count = 3; functions[1].blocks = &block;
    built.functions = functions;
    XrXirTypes types = *built.types; types.count = 1; built.types = &types;
    XrXirNominalTable table = *types.nominals;
    XrXirNominalDeclaration declaration = table.declarations[0];
    XrXirNominalVariant variants[61]; char names[61][8];
    if (wide) {
        for (uint32_t i = 0; i < 61; ++i) {
            int length = snprintf(names[i], sizeof(names[i]), "V%u", i); CHECK(length > 0);
            variants[i] = (XrXirNominalVariant){{names[i], (uint32_t)length}, i > 1 ? 1u : 0u, i == 1 ? 1u : 0u};
        }
        declaration.variants = variants; declaration.variant_count = 61;
        table.declarations = &declaration; types.nominals = &table;
    }
    CHECK(xir_fixture_check(&effect_context, &built, &artifact, NULL) == XR_XIR_OK);
    effect_artifact_free(source); return artifact;
}
static void effect_missing_errors(bool wide) {
    XrXirArtifact *artifact = effect_missing_fixture(wide);
    size_t sites = effect_failures(artifact); XrXirEffects *effects = NULL;
    CHECK(effect_analyze(artifact, &effects) == XR_XIR_OK);
    CHECK(effect_final_outcomes(effects)->words == (wide ? 2u : 1u));
    CHECK(effect_final_outcomes(effects)->atom_count == (wide ? 63u : 4u));
    effect_expect(effects, 1, XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY);
    CHECK(!xr_xir_effects_error_unknown(effects, 1) && !xr_xir_effects_error_unidentified(effects, 1));
    CHECK(!xr_xir_effects_error(effects, 1, (XrXirType)257, 1));
    CHECK(error_bit(effect_final_outcomes(effects)->errors + effect_final_outcomes(effects)->words, effect_final_outcomes(effects)->atom_count + 1));
    effect_artifact_free(artifact);
    CHECK(xr_xir_effects_error(effects, 2, (XrXirType)256, 1));
    effect_summary_free(effects); CHECK(effect_balanced());
    printf("Missing error identity, wide=%u: %zu allocation failures released\n", wide, sites);
}
static void effect_growing_cycle(void) {
    XrXirArtifact *source = effect_missing_fixture(false), *artifact = NULL;
    XrXirModule built = *xr_xir_compile_artifact_module(source); built.stage = XR_XIR_BUILT;
    XrXirFunction functions[4]; memcpy(functions, built.functions, sizeof(functions));
    XrXirTypeNode nodes[2] = {built.types->nodes[0], {0}};
    nodes[1].kind = XR_XIR_TYPE_ARRAY; nodes[1].element = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    nodes[1].parameter_span = 1;
    XrXirTypes types = {nodes, 2, built.types->nominals, NULL}; built.types = &types;
    XrXirInstruction ops[] = {
        {XR_XIR_ENUM_NEW, (XrXirType)256, {0, 1}, {0}, 1, {0}},
        {XR_XIR_ARRAY_NEW, (XrXirType)257, {1, 1}, {0}, 0, {0}},
        {XR_XIR_CALL, XR_XIR_I64, {2, 1}, {0}, 2, {0, 1}},
        {XR_XIR_THROW, XR_XIR_UNIT, {1}, {0}, 0, {0}}};
    uint32_t operands[] = {0, 0, 2}; XrXirBlock block = {0, 4, 0, 0};
    functions[2].instructions = ops; functions[2].instruction_count = 4; functions[2].blocks = &block;
    functions[2].operands = operands; functions[2].operand_count = 3; built.functions = functions;
    XrXirGeneric generics[4]; memcpy(generics, built.generics, sizeof(generics));
    XrXirType argument = (XrXirType)257;
    generics[2].arguments = &argument; generics[2].argument_count = 1; built.generics = generics;
    XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(&effect_context, &built, &artifact, &diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"growing fixture: status=%u function=%u instruction=%u\n",status,diagnostic.function,diagnostic.instruction);
    CHECK(status==XR_XIR_OK); effect_artifact_free(source);
    EffectMark mark=effect_mark(); XrXirCompileContext probe=effect_owner_new(effect_caps());
    uint64_t baseline=effect_stats(&probe).live_bytes; XrXirArtifact *copy=NULL;
    CHECK(effect_reproduce(artifact,&probe,&copy)==XR_XIR_OK);
    uint64_t setup=effect_stats(&probe).work;
    CHECK(xr_xir_compile_verify_v2(&probe, xr_xir_compile_artifact_module(copy), xr_xir_compile_artifact_construction(copy), NULL)==XR_XIR_OK);
    CHECK(effect_stats(&probe).work-setup<=200000);
    xr_xir_compile_artifact_free(copy); effect_owner_free(&probe,baseline); effect_mark_check(mark);
    XrCompileResourceLimits caps=effect_caps(); CHECK(setup<=caps.work-200000); caps.work=setup+200000;
    probe=effect_owner_new(caps); copy=NULL;
    CHECK(effect_reproduce(artifact,&probe,&copy)==XR_XIR_OK);
    EffectMark retained=effect_mark(); attempts=0; XrXirEffects *effects=NULL;
    CHECK(xr_xir_compile_effects_analyze(copy,&effects)==XR_XIR_BUDGET);
    CHECK(!effects && attempts>50); effect_mark_check(retained);
    printf("Growing generic error cycle: budget exhausted after %zu allocations, no published summary\n",attempts);
    xr_xir_compile_artifact_free(copy); effect_owner_free(&probe,baseline); effect_mark_check(mark);
    effect_artifact_free(artifact);
}
static void effect_term_shapes(void) {
    XrXirType t = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE, argument = XR_XIR_I64;
    XrXirCallableParameter parameters[] = {{(XrXirType)257, 0}, {(XrXirType)257, 1}};
    XrXirType nominal_argument = (XrXirType)258;
    XrXirTypeNode nodes[] = {
        {XR_XIR_TYPE_ARRAY, t, NULL, 0, XR_XIR_UNIT, 0, 1, {0}},
        {XR_XIR_TYPE_CELL, (XrXirType)256, NULL, 0, XR_XIR_UNIT, 0, 1, {0}},
        {XR_XIR_TYPE_CALLABLE, XR_XIR_UNIT, parameters, 1, (XrXirType)256, XR_XIR_CALLABLE_ROOT_UNRESOLVED, 1, {0}},
        {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 1, {0, &nominal_argument, 1, NULL, 0}},
        {XR_XIR_TYPE_CALLABLE, XR_XIR_UNIT, parameters + 1, 1, (XrXirType)256, XR_XIR_CALLABLE_ROOT_UNRESOLVED, 1, {0}}};
    XrXirGeneric arguments = {0}; arguments.argument_count = 1; arguments.arguments = &argument;
    XrXirCompileContext budget = effect_context; EffectMark mark = effect_mark();
    XrXirCompileLimits structural = budget.limits;
    EffectTerms pool = {0}; pool.types = (XrXirTypes){nodes, 5, NULL, NULL}; pool.remaining = &budget;
    attempts = 0; XrXirType result = XR_XIR_UNIT;
    CHECK(effect_terms_substitute(&pool, (XrXirType)259, &arguments, &result) == XR_XIR_OK);
    size_t sites = attempts;
    const XrXirTypeNode *nominal = xr_xir_type_node(&pool.types, result);
    CHECK(nominal && !nominal->parameter_span && nominal->kind == XR_XIR_TYPE_NOMINAL);
    XrXirType callable_id = nominal->nominal.arguments[0];
    XrXirTypeNode callable = *xr_xir_type_node(&pool.types, callable_id);
    CHECK(callable.kind == XR_XIR_TYPE_CALLABLE && callable.parameter_count == 1 && !callable.parameters[0].mode);
    CHECK(callable.flags == 8u);
    const XrXirTypeNode *cell = xr_xir_type_node(&pool.types, callable.parameters[0].type);
    CHECK(cell && cell->kind == XR_XIR_TYPE_CELL && cell->element == callable.result);
    const XrXirTypeNode *array = xr_xir_type_node(&pool.types, cell->element);
    CHECK(array && array->kind == XR_XIR_TYPE_ARRAY && array->element == XR_XIR_I64);
    XrXirType repeated = XR_XIR_UNIT; uint32_t count = pool.types.count;
    CHECK(effect_terms_substitute(&pool, (XrXirType)259, &arguments, &repeated) == XR_XIR_OK);
    CHECK(repeated == result && pool.types.count == count);
    CHECK(effect_terms_substitute(&pool, (XrXirType)260, &arguments, &repeated) == XR_XIR_OK);
    CHECK(repeated != callable_id && xr_xir_type_node(&pool.types, repeated)->parameters[0].mode == 1);
    effect_terms_free(&pool); effect_mark_check(mark); CHECK(!memcmp(&budget.limits,&structural,sizeof(structural)));
    for (size_t i = 0; i < sites; ++i) {
        pool = (EffectTerms){0}; pool.types = (XrXirTypes){nodes, 5, NULL, NULL};
        budget = effect_owner_new(effect_caps()); uint64_t baseline=effect_stats(&budget).live_bytes; pool.remaining = &budget;
        attempts = 0; injected=false; fail_at = i; result=XR_XIR_UNIT;
        CHECK(effect_terms_substitute(&pool, (XrXirType)259, &arguments, &result) == XR_XIR_OUT_OF_MEMORY);
        CHECK(result == XR_XIR_UNIT); effect_terms_free(&pool);
        CHECK(injected && !memcmp(&budget.limits,&structural,sizeof(structural)));
        fail_at=SIZE_MAX; effect_owner_free(&budget,baseline); effect_mark_check(mark);
    }
    fail_at = SIZE_MAX;
    printf("Structural error terms: %zu allocation failures released temporary term owners\n", sites);
}
static void effect_cleanup_ownership(void) {
    XrXirArtifact *artifact = effect_fixture(false);
    XrXirFunctionIdentity *ids = (XrXirFunctionIdentity *)artifact->module.declarations->functions;
    ids[5].cleanup_owner = 1;
    size_t sites = effect_failures(artifact);
    effect_artifact_free(artifact);
    printf("Cleanup verification and inference: %zu allocation failures released\n", sites);
}
static void effect_witness_competition(void) {
    XrXirArtifact *base = effect_fixture(true), *artifact = NULL;
    XrXirModule built = *xr_xir_compile_artifact_module(base); built.stage = XR_XIR_BUILT;
    XrXirFunction functions[8]; memcpy(functions, built.functions, sizeof(functions));
    XrXirInstruction ops[] = {
        {XR_XIR_CALL_INDIRECT, XR_XIR_UNIT, {0}, {0}, 0, {0}},
        {XR_XIR_CALL, XR_XIR_UNIT, {0}, {0}, 0, {0}},
        {XR_XIR_CALL, XR_XIR_UNIT, {0}, {0}, 1, {0}},
        {XR_XIR_CALL, XR_XIR_UNIT, {0}, {0}, 1, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirBlock block = {0, 5, 0, 0};
    functions[2].instructions = ops; functions[2].instruction_count = 5;
    functions[2].blocks = &block; built.functions = functions;
    CHECK(xir_fixture_check(&effect_context, &built, &artifact, NULL) == XR_XIR_OK);
    effect_artifact_free(base);
    XrXirEffects *effects = NULL;
    CHECK(effect_analyze(artifact, &effects) == XR_XIR_OK);
    effect_witness_paths(xr_xir_compile_artifact_module(artifact), effects);
    XrXirEffectFacts w_read=xr_xir_effects_read(effects, 2);
    const XrXirEffectWitness *w=(w_read.status==XR_XIR_OK && w_read.suspend_witness.cause!=0?&w_read.suspend_witness:NULL);
    CHECK(w && w->cause == XR_XIR_EFFECT_CAUSE_CALL && w->distance == 1);
    CHECK(w->callee == 1 && w->instruction == 3);
    CHECK(xr_xir_effects_read(effects, 2).control.suspend == XR_XIR_EFFECT_MAY);
    effect_summary_free(effects); CHECK(effect_balanced());
    CHECK(effect_failures(artifact) > 0);
    effect_artifact_free(artifact);
}
static void effect_declared_promises(void) {
    for (unsigned suspends = 0; suspends < 2; ++suspends) {
        XrXirArtifact *base = effect_fixture(suspends != 0);
        XrXirModule built = *xr_xir_compile_artifact_module(base); built.stage = XR_XIR_BUILT;
        XrXirDeclarations declarations = *built.declarations;
        XrXirFunctionIdentity ids[8]; memcpy(ids, declarations.functions, sizeof(ids));
        declarations.functions = ids; built.declarations = &declarations;
        for (uint32_t f = 0; f < 8; ++f) {
            ids[f].promises = XR_XIR_FUNCTION_NO_SUSPEND;
            XrXirArtifact *checked = NULL;
            XrXirDiagnostic diagnostic = {0};
            XrXirStatus status = xir_fixture_check(&effect_context, &built, &checked, &diagnostic);
            bool invalid = (f < 2 && suspends) || (f >= 2 && f <= 4);
            if (invalid) {
                CHECK(status == XR_XIR_BAD_TYPE && !checked);
                CHECK(diagnostic.reason == XR_XIR_DIAGNOSTIC_NO_SUSPEND && diagnostic.function == f);
                CHECK(diagnostic.block == 0 && diagnostic.instruction == (f == 1 ? 1u : 0u));
            } else {
                CHECK(status == XR_XIR_OK && checked);
                CHECK(effect_failures(checked) > 0);
                XrXirCheckedPacket packet = {0}; XrXirArtifact *decoded = NULL, *lowered = NULL;
                CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
                effect_artifact_free(checked);
                CHECK(xr_xir_compile_checked_read(&effect_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
                xr_xir_compile_checked_packet_free(&packet);
                CHECK(decoded->module.declarations->functions[f].promises == XR_XIR_FUNCTION_NO_SUSPEND);
                XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
                CHECK(xr_xir_compile_lower(decoded, &target, &lowered, NULL) == XR_XIR_OK);
                CHECK(lowered->module.declarations->functions[f].promises == XR_XIR_FUNCTION_NO_SUSPEND);
                effect_artifact_free(decoded); effect_artifact_free(lowered);
            }
            ids[f].promises = UINT32_MAX;
            CHECK(xir_fixture_verify(&effect_context, &built, NULL) == XR_XIR_BAD_STRUCTURE);
            ids[f].promises = 0;
        }
        effect_artifact_free(base); CHECK(effect_balanced());
    }
}
static void effect_qualified_callable(void) {
    XrXirArtifact *base = effect_fixture(false), *checked = NULL;
    XrXirModule built = *xr_xir_compile_artifact_module(base); built.stage = XR_XIR_BUILT;
    XrXirTypeNode node = built.types->nodes[0]; node.flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED | XR_XIR_CALLABLE_NO_SUSPEND;
    XrXirTypes types = {&node, 1, NULL, NULL}; built.types = &types;
    XrXirFunctionIdentity ids[8]; memcpy(ids, built.declarations->functions, sizeof(ids));
    ids[0].promises = ids[2].promises = XR_XIR_FUNCTION_NO_SUSPEND;
    XrXirDeclarations declarations = *built.declarations; declarations.functions = ids; built.declarations = &declarations;
    CHECK(xir_fixture_check(&effect_context, &built, &checked, NULL) == XR_XIR_OK);
    XrXirEffects *effects = NULL;
    CHECK(effect_analyze(checked, &effects) == XR_XIR_OK);
    effect_expect(effects, 2, XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_UNKNOWN);
    effect_expect(effects, 3, XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_NONE);
    effect_expect(effects, 4, XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_UNKNOWN);
    effect_witness_paths(xr_xir_compile_artifact_module(checked), effects);
    effect_summary_free(effects); CHECK(effect_balanced());
    CHECK(effect_failures(checked) > 0);
    effect_artifact_free(checked); checked = NULL;
    ids[0].promises = 0;
    CHECK(xir_fixture_check(&effect_context, &built, &checked, NULL) == XR_XIR_BAD_TYPE && !checked);
    ids[0].promises = XR_XIR_FUNCTION_NO_SUSPEND; node.flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    XrXirDiagnostic diagnostic = {0};
    CHECK(xir_fixture_check(&effect_context, &built, &checked, &diagnostic) == XR_XIR_BAD_TYPE && !checked);
    CHECK(diagnostic.reason == XR_XIR_DIAGNOSTIC_NO_SUSPEND && diagnostic.function == 2);
    /* A closed root upper does not supply an independent NoSuspend promise. */
    node.flags = 2; diagnostic = (XrXirDiagnostic){0};
    CHECK(xir_fixture_check(&effect_context, &built, &checked, &diagnostic) == XR_XIR_BAD_TYPE && !checked);
    CHECK(diagnostic.reason == XR_XIR_DIAGNOSTIC_NO_SUSPEND && diagnostic.function == 2);
    /* Bit16 is still outside both current root and suspension contracts. */
    node.flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED | 16u; diagnostic = (XrXirDiagnostic){0};
    CHECK(xir_fixture_check(&effect_context, &built, &checked, &diagnostic) == XR_XIR_BAD_TYPE && !checked);
    CHECK(diagnostic.status == XR_XIR_BAD_TYPE);
    /* Legacy 0 and 1 each lack the mandatory independent root upper. */
    const uint32_t legacy_flags[] = {0u, 1u};
    for (uint32_t i = 0; i < 2; ++i) {
        node.flags = legacy_flags[i]; diagnostic = (XrXirDiagnostic){0};
        CHECK(xir_fixture_check(&effect_context, &built, &checked, &diagnostic) == XR_XIR_BAD_TYPE && !checked);
        CHECK(diagnostic.status == XR_XIR_BAD_TYPE);
    }
    effect_artifact_free(base);
}
static void effect_mixed_callable_witness(void) {
    XrXirArtifact *base = effect_fixture(false), *checked = NULL;
    XrXirModule built = *xr_xir_compile_artifact_module(base); built.stage = XR_XIR_BUILT;
    XrXirTypeNode nodes[2] = {built.types->nodes[0], built.types->nodes[0]};
    nodes[1].flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED | XR_XIR_CALLABLE_NO_SUSPEND;
    XrXirTypes types = {nodes, 2, NULL, NULL}; built.types = &types;
    XrXirFunction functions[8]; memcpy(functions, built.functions, sizeof(functions)); built.functions = functions;
    XrXirType parameters[] = {(XrXirType)257, (XrXirType)256};
    XrXirInstruction ops[] = {{XR_XIR_CALL_INDIRECT, XR_XIR_UNIT, {0}, {0}, 0, {0}},
        {XR_XIR_CALL_INDIRECT, XR_XIR_UNIT, {0}, {0}, 1, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirBlock block = {0, 3, 0, 0};
    functions[2].parameters = parameters; functions[2].parameter_count = 2;
    functions[2].instructions = ops; functions[2].instruction_count = 3; functions[2].blocks = &block;
    CHECK(xir_fixture_check(&effect_context, &built, &checked, NULL) == XR_XIR_OK);
    XrXirEffects *effects = NULL;
    CHECK(effect_analyze(checked, &effects) == XR_XIR_OK);
    effect_expect(effects, 2, XR_XIR_EFFECT_UNKNOWN, XR_XIR_EFFECT_UNKNOWN);
    XrXirEffectFacts w_read=xr_xir_effects_read(effects, 2);
    const XrXirEffectWitness *w=(w_read.status==XR_XIR_OK && w_read.suspend_witness.cause!=0?&w_read.suspend_witness:NULL);
    CHECK(w && w->instruction == 1 && w->cause == XR_XIR_EFFECT_CAUSE_INDIRECT);
    effect_summary_free(effects); CHECK(effect_balanced());
    effect_artifact_free(checked); effect_artifact_free(base);
}
static void effect_callable_weakening(void) {
    XrXirCallableParameter parameters[2] = {{XR_XIR_I64, 0}, {XR_XIR_I64, 0}};
    XrXirTypeNode nodes[2] = {
        {XR_XIR_TYPE_CALLABLE, XR_XIR_UNIT, &parameters[0], 1, XR_XIR_I64, XR_XIR_CALLABLE_ROOT_UNRESOLVED, 0, {0}},
        {XR_XIR_TYPE_CALLABLE, XR_XIR_UNIT, &parameters[1], 1, XR_XIR_I64, XR_XIR_CALLABLE_ROOT_UNRESOLVED | XR_XIR_CALLABLE_NO_SUSPEND, 0, {0}}};
    XrXirTypes types = {nodes, 2, NULL, NULL};
    XrXirType source = (XrXirType)257;
    XrXirInstruction ops[] = {
        {XR_XIR_FUNCTION_WEAKEN, (XrXirType)256, {0}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1, 0}, {0}, 0, {0}}};
    XrXirBlock block = {0, 2, 0, 0};
    XrXirFunction function = {"weaken", 6, &source, 1, (XrXirType)256, &block, 1, ops, 2, NULL, 0};
    XrXirInstruction init={.op=XR_XIR_RETURN,.type=XR_XIR_UNIT}; XrXirBlock init_block={0,1,0,0};
    XrXirFunction functions[2]={function,{"init",4,NULL,0,XR_XIR_UNIT,&init_block,1,&init,1,NULL,0}};
    XrXirSourceModule source_module={"weaken",6,NULL,0,1}; XrXirFunctionIdentity ids[2]={{0}};
    XrXirDeclarations declarations={&source_module,1,ids,NULL,0,NULL,0,UINT32_MAX,UINT32_MAX,NULL};
    XrXirModule module = {XR_XIR_BUILT, functions, 2, &declarations, NULL, &types, NULL, XR_XIR_LIBRARY, NULL};
    XrXirArtifact *checked = NULL;
    CHECK(xir_fixture_check(&effect_context, &module, &checked, NULL) == XR_XIR_OK);
    effect_artifact_free(checked); checked = NULL;
    EffectMark mark=effect_mark();
    for (uint64_t units=1; units<=2; ++units) {
        XrCompileResourceLimits caps=effect_caps(); caps.work=effect_baseline.work+units;
        XrXirCompileContext c=effect_owner_new(caps);
        CHECK(xr_xir_compile_callable_weakening(&c,&types,(XrXirType)257,(XrXirType)256)==
            (units==1 ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(effect_stats(&c).work==caps.work); effect_owner_free(&c,effect_baseline.live_bytes); effect_mark_check(mark);
    }
    /* Equal contracts use one canonical identity; the unused node stays distinct. */
    for (unsigned equal=0;equal<2;++equal) {
        source=(XrXirType)256;
        nodes[0].flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED | (equal ? XR_XIR_CALLABLE_NO_SUSPEND : 0);
        nodes[1].flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED | (equal ? 0 : XR_XIR_CALLABLE_NO_SUSPEND);
        CHECK(xir_fixture_check(&effect_context,&module,&checked,NULL)==XR_XIR_OK && checked);
        effect_artifact_free(checked);checked=NULL;
    }
    for (unsigned bad = 0; bad < 6; ++bad) {
        source = (XrXirType)257;
        /* UNKNOWN and REQUIRED cannot acquire a pure destination bound. */
        nodes[0].flags = (bad <= 1 ? XR_XIR_CALLABLE_ROOT_NONE : XR_XIR_CALLABLE_ROOT_UNRESOLVED) |
            (bad == 1 || bad == 5 ? XR_XIR_CALLABLE_NO_SUSPEND : 0);
        nodes[1].flags = (bad == 1 ? XR_XIR_CALLABLE_ROOT_REQUIRED : XR_XIR_CALLABLE_ROOT_UNRESOLVED) |
            (bad == 5 ? 0 : XR_XIR_CALLABLE_NO_SUSPEND);
        nodes[0].result = bad == 2 ? XR_XIR_BOOL : XR_XIR_I64;
        parameters[0].type = bad == 3 ? XR_XIR_BOOL : XR_XIR_I64;
        nodes[0].parameter_count = bad == 4 ? 0 : 1;
        CHECK(xir_fixture_check(&effect_context, &module, &checked, NULL) != XR_XIR_OK && !checked);
    }
}
#define EFFECT_CASE(call) do { effect_case_begin(); call; effect_case_end(); } while (0)
#include "xir_effect_collector_cases.h"
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--error-query-work")));
    if (argc==2) { EFFECT_CASE(effect_error_query_work());return 0; }
    EFFECT_CASE(effect_error_query_work());
    EFFECT_CASE(collector_bare_case(false)); EFFECT_CASE(collector_bare_case(true));
    EFFECT_CASE(collector_declaration_cases());
    EFFECT_CASE(effect_callable_weakening());
    EFFECT_CASE(effect_qualified_callable()); EFFECT_CASE(effect_mixed_callable_witness());
    EFFECT_CASE(effect_declared_promises());
    EFFECT_CASE(effect_witness_competition());
    EFFECT_CASE(effect_cleanup_ownership());
    EFFECT_CASE(effect_term_shapes());
    EFFECT_CASE(effect_missing_errors(false)); EFFECT_CASE(effect_missing_errors(true)); EFFECT_CASE(effect_growing_cycle());
    EFFECT_CASE(effect_generic_errors());
    EFFECT_CASE(effect_extreme_budgets());
    EFFECT_CASE(effect_enum_ownership());
    EFFECT_CASE(effect_long_cycle());
    EFFECT_CASE(effect_cases(false)); EFFECT_CASE(effect_cases(true));
    XrXirEffects *effects = NULL;
    CHECK(xr_xir_compile_effects_analyze(NULL,&effects) == XR_XIR_BAD_STRUCTURE && !effects);
    XrXirEffects *sentinel=(XrXirEffects *)(uintptr_t)1;
    CHECK(xr_xir_compile_effects_analyze(NULL,&sentinel)==XR_XIR_BAD_STRUCTURE && sentinel==(XrXirEffects *)(uintptr_t)1);
    CHECK(!(xr_xir_effects_read(NULL,0).status==XR_XIR_OK));
    CHECK(!live && !live_bytes);
    puts("Control effect fixed points, error edges and physical ownership passed");
    return 0;
}
