/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_effects.c - Verified root facts, owned causes and failure boundaries
 */
#include "base/xmalloc.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_source.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_effect_analysis_owner.h"
#include "xir/xxir_effects.c"
#include "xir_root_effects_builtin.h"
#include "xir_root_effects_oracles.h"

static XrXirArtifact *root_source(const char *file,XrXirStatus expected) {
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%s",XR_ROOT_EFFECT_FIXTURES,file)>0);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(effect_context.resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_ROOT_EFFECT_FIXTURES};
    XrXirSourceRequest request={session,path,&authority,&effect_context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};XrXirSourceFailure failure_path={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&source,&diagnostic,&failure_path);
    if(status!=expected)fprintf(stderr,"root source %s status%u %d:%d %s\n",
        file,status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==expected);
    CHECK(expected==XR_XIR_OK?(source.checked!=NULL):(!source.checked&&!source.snapshot));
    XrXirArtifact *checked=source.checked;source.checked=NULL;
    xr_xir_compile_source_failure_free(&failure_path);xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);
    return checked;
}
static XrXirArtifact *root_replay(XrXirArtifact *source) {
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source,&packet,NULL)==XR_XIR_OK);
    effect_artifact_free(source);XrXirArtifact *checked=NULL;
    CHECK(xr_xir_compile_checked_read(&effect_context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);return checked;
}
static void root_occupied(XrXirArtifact *artifact) {
    XrXirEffects *summary=NULL;CHECK(effect_analyze(artifact,&summary)==XR_XIR_OK);
    XrXirEffects *output=summary;EffectMark mark=effect_mark();XrCompileResourceStats before=effect_stats(&effect_context);
    uint8_t bytes[sizeof(XrXirEffects)];memcpy(bytes,summary,sizeof(bytes));attempts=0;
    CHECK(xr_xir_compile_effects_analyze(artifact,&output)==XR_XIR_BAD_STRUCTURE);
    CHECK(output==summary&&!attempts&&!memcmp(bytes,summary,sizeof(bytes)));effect_mark_check(mark);
    XrCompileResourceStats after=effect_stats(&effect_context);
    CHECK(before.allocated_bytes==after.allocated_bytes&&before.held_bytes==after.held_bytes&&before.allocation_count==after.allocation_count);
    XrXirArtifact wrong=*artifact;wrong.module.stage=XR_XIR_BUILT;
    CHECK(xr_xir_compile_effects_analyze(&wrong,&output)==XR_XIR_BAD_STAGE&&output==summary);
    CHECK(xr_xir_compile_effects_analyze(NULL,&output)==XR_XIR_BAD_STRUCTURE&&output==summary);
    CHECK(xr_xir_compile_effects_analyze(artifact,NULL)==XR_XIR_BAD_STRUCTURE);effect_mark_check(mark);
    effect_summary_free(summary);
}
static void root_failures(const XrXirArtifact *artifact) {
    XrXirEffects *summary=NULL;attempts=0;fail_at=SIZE_MAX;
    CHECK(effect_analyze(artifact,&summary)==XR_XIR_OK);size_t sites=attempts;
    effect_summary_free(summary);CHECK(sites&&effect_balanced());
    size_t selected=0;
    for(size_t point=0;point<sites;++point){
        if(!effect_full_faults && point && point+1!=sites)continue;
        ++selected;
        EffectMark mark=effect_mark();XrXirCompileContext context=effect_owner_new(effect_caps());
        uint64_t baseline=effect_stats(&context).held_bytes;XrXirArtifact *copy=NULL;
        CHECK(effect_reproduce(artifact,&context,&copy)==XR_XIR_OK);EffectMark retained=effect_mark();
        summary=NULL;attempts=0;injected=false;fail_at=point;
        XrXirStatus status=xr_xir_compile_effects_analyze(copy,&summary);
        CHECK(injected&&status==XR_XIR_OUT_OF_MEMORY&&!summary);effect_mark_check(retained);
        XrCompileResourceStats paid=effect_stats(&context);fail_at=SIZE_MAX;
        CHECK(xr_xir_compile_effects_analyze(copy,&summary)==XR_XIR_OK&&summary);
        XrCompileResourceStats retried=effect_stats(&context);
        CHECK(retried.allocated_bytes>paid.allocated_bytes&&retried.allocation_count>paid.allocation_count);
        xr_xir_compile_effects_free(summary);effect_mark_check(retained);
        xr_xir_compile_artifact_free(copy);effect_owner_free(&context,baseline);effect_mark_check(mark);
    }
    printf("root facts %zu allocation sites, %zu selected actual failures, retry and physical0\n",sites,selected);
}
static void root_detached_owner(const XrXirArtifact *artifact) {
    EffectMark mark=effect_mark();XrXirCompileContext context=effect_owner_new(effect_caps());
    XrXirArtifact *copy=NULL;XrXirEffects *summary=NULL;
    CHECK(effect_reproduce(artifact,&context,&copy)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(copy,&summary)==XR_XIR_OK);
    uint32_t f=root_find(xr_xir_compile_artifact_module(copy),"mixedRelay");
    CHECK(f<xr_xir_compile_artifact_module(copy)->function_count);
    xr_xir_compile_artifact_free(copy);xr_compile_resources_release(context.resources);context.resources=NULL;
    root_fact(summary,f,true,true);
    CHECK(xr_xir_effects_read(summary,f).root_witness.distance==1);
    XrXirEffectFacts parameter_read=xr_xir_effects_read(summary,f);
    const XrXirRootEffectWitness *parameter=(parameter_read.status==XR_XIR_OK && parameter_read.unresolved_witness.cause!=0?&parameter_read.unresolved_witness:NULL);
    CHECK(parameter->distance==0 && parameter->cause==XR_XIR_ROOT_CAUSE_PARAMETER);
    CHECK(parameter->slot==0 && parameter->callee==UINT32_MAX);
    xr_xir_compile_effects_free(summary);effect_mark_check(mark);
}
static void root_source_cases(bool resources) {
    XrXirArtifact *checked=root_replay(root_source("root.xr",XR_XIR_OK));root_source_oracles(checked,false);
    root_occupied(checked);
    if(resources){
        root_failures(checked);effect_memory_boundaries(checked);
        effect_analysis_reuse(checked);
    }
    root_detached_owner(checked);
    XrXirArtifact *specialized=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(checked,&specialized,NULL, NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(specialized,&target, NULL,&lowered,NULL)==XR_XIR_OK);
    CHECK(specialized->control_summary &&
        xir_effects_control_summary(lowered->program_final)==specialized->control_summary);
    effect_artifact_free(specialized);root_source_oracles(lowered,true);effect_artifact_free(lowered);
    XrXirEffects *summary=NULL;CHECK(effect_analyze(checked,&summary)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(checked);
    uint32_t mixed=root_find(module,"mixedRelay"),scalar=root_find(module,"mutableRead");
    CHECK(mixed<module->function_count&&scalar<module->function_count);
    XrXirRootEffectWitness known=xr_xir_effects_read(summary,mixed).root_witness;
    XrXirRootEffectWitness unknown=xr_xir_effects_read(summary,mixed).unresolved_witness;
    effect_artifact_free(checked);root_fact(summary,mixed,true,true);root_fact(summary,scalar,true,false);
    XrXirEffectFacts owned_read=xr_xir_effects_read(summary,mixed);
    const XrXirRootEffectWitness *owned=(owned_read.status==XR_XIR_OK && owned_read.root_witness.cause!=0?&owned_read.root_witness:NULL);
    CHECK(owned->cause==known.cause&&owned->instruction==known.instruction&&owned->callee==known.callee&&owned->distance==known.distance);
    owned_read=xr_xir_effects_read(summary,mixed);owned=(owned_read.status==XR_XIR_OK && owned_read.unresolved_witness.cause!=0?&owned_read.unresolved_witness:NULL);
    CHECK(owned->cause==unknown.cause&&owned->instruction==unknown.instruction&&owned->callee==unknown.callee&&owned->distance==unknown.distance);
    effect_summary_free(summary);CHECK(effect_balanced());
    puts("root literal source/replay/Checked specialization/Lowered facts; producer-dead owned witnesses PASS");
}
static void root_builtin_cases(void) {
    for(unsigned mode=0;mode<2;++mode){
        XrXirArtifact *artifact=root_replay(root_builtin(mode));XrXirEffects *effects=NULL;
        CHECK(effect_analyze(artifact,&effects)==XR_XIR_OK);
        root_fact(effects,0,true,false);root_fact(effects,1,false,false);root_fact(effects,2,true,false);
        root_fact(effects,3,false,false);root_fact(effects,4,false,false);root_fact(effects,5,mode==0,false);
        root_fact(effects,6,false,true);root_fact(effects,7,false,false);root_fact(effects,8,true,false);
        CHECK(xr_xir_effects_read(effects,6).control.suspend==XR_XIR_EFFECT_NONE);
        XrXirEffectFacts w_read=xr_xir_effects_read(effects,6);
        const XrXirRootEffectWitness *w=(w_read.status==XR_XIR_OK && w_read.unresolved_witness.cause!=0?&w_read.unresolved_witness:NULL);
        CHECK(w&&w->cause==XR_XIR_ROOT_CAUSE_INDIRECT&&w->instruction==0&&w->distance==0);
        if(!mode){w_read=xr_xir_effects_read(effects,5);w=(w_read.status==XR_XIR_OK && w_read.root_witness.cause!=0?&w_read.root_witness:NULL);CHECK(w->cause==XR_XIR_ROOT_CAUSE_CALL&&w->callee==2&&w->instruction==0&&w->distance==1);}
        root_paths(xr_xir_compile_artifact_module(artifact),effects);effect_summary_free(effects);effect_artifact_free(artifact);
    }
    CHECK(!root_builtin(2));CHECK(!root_builtin(3));
    XrXirArtifact *group=root_replay(root_group());XrXirEffects *effects=NULL;
    CHECK(effect_analyze(group,&effects)==XR_XIR_OK);
    root_fact(effects,0,true,false);root_fact(effects,1,false,false);root_fact(effects,2,true,false);
    XrXirEffectFacts w_read=xr_xir_effects_read(effects,0);
    const XrXirRootEffectWitness *w=(w_read.status==XR_XIR_OK && w_read.root_witness.cause!=0?&w_read.root_witness:NULL);
    CHECK(w&&w->cause==XR_XIR_ROOT_CAUSE_INITIALIZER&&w->instruction==2&&w->slot==1&&w->distance==0);
    effect_summary_free(effects);effect_artifact_free(group);root_permutation();
    puts("root authentic defaults/noSuspend indirect/reference/group first-slot/edge permutation PASS");
}
static void root_var_atomic_case(void) {
    XrXirArtifact *checked=root_replay(root_source("rejected/mutable_atomic_binding.xr",XR_XIR_OK));
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK);
    XrXirArtifact *specialized=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(checked,&specialized,NULL, NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(specialized,NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(specialized,&target, NULL,&lowered,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
    XrXirArtifact *stages[3]={checked,specialized,lowered};
    for (uint32_t stage=0;stage<3;++stage) {
        const XrXirModule *module=xr_xir_compile_artifact_module(stages[stage]);
        uint32_t read=root_find(module,"read");CHECK(read<module->function_count);
        XrXirEffects *effects=NULL;CHECK(effect_analyze(stages[stage],&effects)==XR_XIR_OK);
        root_fact(effects,read,true,false);
        XrXirEffectFacts witness_read=xr_xir_effects_read(effects,read);
        const XrXirRootEffectWitness *witness=(witness_read.status==XR_XIR_OK && witness_read.root_witness.cause!=0?&witness_read.root_witness:NULL);
        CHECK(witness && witness->cause==XR_XIR_ROOT_CAUSE_MUTABLE_SLOT && witness->distance==0);
        CHECK(witness->slot<module->declarations->slot_count);
        const XrXirSlot *slot=&module->declarations->slots[witness->slot];
        CHECK(slot->mutable && xr_xir_type_is_cell(module->types,slot->type));
        CHECK(xr_xir_type_is_atomic(module->types,xr_xir_cell_element(module->types,slot->type)));
        CHECK(xr_xir_atomic_element(module->types,xr_xir_cell_element(module->types,slot->type))==XR_XIR_I64);
        effect_summary_free(effects);
    }
    effect_artifact_free(lowered);effect_artifact_free(specialized);effect_artifact_free(checked);
    puts("direct var Atomic binding Source/Checked/specialization/full recheck/Lowered exact mutable-slot ROOT PASS");
}
static void root_go_rejections(void) {
    CHECK(!root_source("rejected/root_child.xr",XR_XIR_BAD_TYPE));
    CHECK(!root_source("rejected/root_parent.xr",XR_XIR_BAD_TYPE));
    puts("root GO child/parent BAD_TYPE remain fail closed PASS");
}
static XrXirStatus parameter_summary_try(const XrXirCompileContext *context,
    const XrXirModule *module, XrXirEffectParameter *parameters) {
    XrXirFunctionEffectContract contracts[3] = {0};
    for (uint32_t f = 0; f < 3; ++f)
        contracts[f] = (XrXirFunctionEffectContract){.parameter_count = 1, .parameters = &parameters[f]};
    EffectParameterGraph graph = {.work = context, .module = *module};
    XrXirStatus status = effect_parameter_graph_prepare(&graph, contracts);
    if (status == XR_XIR_OK) status = effect_parameter_graph_collect(&graph);
    XrXirFunction headers[3];
    memcpy(headers, module->functions, sizeof(headers));
    for (uint32_t f = 0; f < 3; ++f) {
        headers[f].instructions = NULL;
        headers[f].instruction_count = UINT32_MAX;
    }
    graph.module.functions = headers;
    if (status == XR_XIR_OK) status = effect_parameter_graph_finish(&graph, contracts);
    effect_parameter_graph_free(&graph);
    return status;
}
static void parameter_summary_kernel(void) {
    EffectMark baseline = effect_mark();
    XrXirTypeNode callable = {.kind = XR_XIR_TYPE_CALLABLE, .element = XR_XIR_NEVER,
        .result = XR_XIR_UNIT, .flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    XrXirTypes types = {.nodes = &callable, .count = 1};
    XrXirType parameter = XR_XIR_CONSTRUCTED_TYPE_BASE;
    uint32_t operand = 0;
    XrXirInstruction rows[3][2] = {
        {{.op = XR_XIR_CALL, .immediate = 1, .type = XR_XIR_CONSTRUCTED_TYPE_BASE, .args = {0, 1}},
            {.op = XR_XIR_RETURN}},
        {{.op = XR_XIR_FUNCTION_REF, .immediate = 2, .type = XR_XIR_CONSTRUCTED_TYPE_BASE, .args = {0, 1}},
            {.op = XR_XIR_RETURN, .args = {1}}},
        {{.op = XR_XIR_CALL_INDIRECT, .immediate = 0, .type = XR_XIR_UNIT}, {.op = XR_XIR_RETURN}}
    };
    XrXirFunction functions[3] = {0};
    for (uint32_t f = 0; f < 3; ++f)
        functions[f] = (XrXirFunction){.error_type = XR_XIR_NEVER, .parameters = &parameter, .parameter_count = 1,
            .instructions = rows[f], .instruction_count = 2, .operands = &operand, .operand_count = 1};
    functions[1].result = XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirModule module = {.stage = XR_XIR_CHECKED, .types = &types, .functions = functions, .function_count = 3};
    XrXirEffectParameter parameters[3] = {
        {XR_XIR_EFFECT_PARAMETER_VARIABLE, 0},
        {XR_XIR_EFFECT_PARAMETER_VARIABLE, 0},
        {XR_XIR_EFFECT_PARAMETER_VARIABLE, 0}
    }, empty[3];
    memcpy(empty, parameters, sizeof(empty));
    attempts = 0;
    CHECK(parameter_summary_try(&effect_context, &module, parameters) == XR_XIR_OK);
    size_t sites = attempts;
    CHECK(parameters[0].kind == XR_XIR_EFFECT_PARAMETER_FIXED && parameters[0].uses == XR_XIR_EFFECT_USE_FIXED);
    CHECK(parameters[1].kind == XR_XIR_EFFECT_PARAMETER_FIXED && parameters[1].uses ==
        (XR_XIR_EFFECT_USE_CONST_CAPTURE | XR_XIR_EFFECT_USE_RETURN | XR_XIR_EFFECT_USE_INVOKE));
    CHECK(parameters[2].kind == XR_XIR_EFFECT_PARAMETER_VARIABLE && parameters[2].uses == XR_XIR_EFFECT_USE_INVOKE);
    effect_mark_check(baseline);
    for (size_t point = 0; point < sites; ++point) {
        memcpy(parameters, empty, sizeof(parameters));
        attempts = 0; injected = false; fail_at = point;
        CHECK(parameter_summary_try(&effect_context, &module, parameters) == XR_XIR_OUT_OF_MEMORY);
        CHECK(injected && !memcmp(parameters, empty, sizeof(parameters)));
        fail_at = SIZE_MAX;
        effect_mark_check(baseline);
    }
    XrXirCompileContext finite = effect_owner_new((XrCompileResourceLimits){XR_COMPILE_MEMORY_FINITE, 128, true});
    uint64_t held = effect_stats(&finite).held_bytes;
    CHECK(parameter_summary_try(&finite, &module, parameters) == XR_XIR_MEMORY_LIMIT);
    CHECK(!memcmp(parameters, empty, sizeof(parameters)));
    effect_owner_free(&finite, held);
    effect_mark_check(baseline);
    printf("parameter summaries: capture/forward masks, body-free closure, %zu allocation failures and finite owner PASS\n", sites);
}
static void parameter_summary_owner(void) {
    EffectMark baseline = effect_mark();
    XrXirEffectParameter values[3] = {
        {XR_XIR_EFFECT_PARAMETER_FIXED, XR_XIR_EFFECT_USE_STORE},
        {XR_XIR_EFFECT_PARAMETER_VARIABLE, XR_XIR_EFFECT_USE_INVOKE},
        {XR_XIR_EFFECT_PARAMETER_CONDITIONAL_STATIC, XR_XIR_EFFECT_USE_FORWARD}
    };
    XrXirFunctionEffectContract contracts[3] = {
        {.parameter_count = 2, .parameters = values}, {0},
        {.parameter_count = 1, .parameters = values + 2}
    };
    EffectParameterSummary summary = {0};
    attempts = 0;
    CHECK(effect_parameter_summary_copy(&effect_context, contracts, 3, &summary) == XR_XIR_OK);
    size_t sites = attempts;
    CHECK(summary.count == 3 && summary.total == 3 && summary.rows[1].count == 0);
    CHECK(effect_parameter_summary_shape(&effect_context, &summary, 3) == XR_XIR_OK);
    EffectParameterSummary saved = summary;
    attempts = 0;
    CHECK(effect_parameter_summary_copy(&effect_context, contracts, 3, &summary) == XR_XIR_BAD_STRUCTURE);
    CHECK(!attempts && !memcmp(&summary, &saved, sizeof(summary)));
    uint32_t kind = UINT32_MAX;
    CHECK(effect_parameter_summary_get(&summary, 0, 0, &kind) == XR_XIR_OK && kind == XR_XIR_EFFECT_PARAMETER_FIXED);
    CHECK(effect_parameter_summary_get(&summary, 0, 1, &kind) == XR_XIR_OK && kind == XR_XIR_EFFECT_PARAMETER_VARIABLE);
    CHECK(effect_parameter_summary_get(&summary, 2, 0, &kind) == XR_XIR_OK && kind == XR_XIR_EFFECT_PARAMETER_CONDITIONAL_STATIC);
    kind = UINT32_MAX;
    CHECK(effect_parameter_summary_get(&summary, 1, 0, &kind) == XR_XIR_BAD_STRUCTURE && kind == UINT32_MAX);
    CHECK(effect_parameter_summary_get(&summary, 3, 0, &kind) == XR_XIR_BAD_STRUCTURE && kind == UINT32_MAX);
    CHECK(effect_parameter_summary_get(&summary, 0, 2, &kind) == XR_XIR_BAD_STRUCTURE && kind == UINT32_MAX);
    effect_parameter_summary_clear(&summary);
    effect_mark_check(baseline);
    for (size_t point = 0; point < sites; ++point) {
        attempts = 0; injected = false; fail_at = point;
        CHECK(effect_parameter_summary_copy(&effect_context, contracts, 3, &summary) == XR_XIR_OUT_OF_MEMORY);
        CHECK(injected && !summary.resources && !summary.rows && !summary.values && !summary.count && !summary.total);
        fail_at = SIZE_MAX;
        effect_mark_check(baseline);
    }
    XrXirEffectParameter finite_values[32] = {0};
    XrXirFunctionEffectContract large = {.parameter_count = 32, .parameters = finite_values};
    XrXirCompileContext finite = effect_owner_new((XrCompileResourceLimits){XR_COMPILE_MEMORY_FINITE, 128, true});
    uint64_t held = effect_stats(&finite).held_bytes;
    CHECK(effect_parameter_summary_copy(&finite, &large, 1, &summary) == XR_XIR_MEMORY_LIMIT);
    CHECK(!summary.resources && !summary.rows && !summary.values && !summary.count && !summary.total);
    effect_owner_free(&finite, held);
    values[0].kind = UINT32_MAX;
    CHECK(effect_parameter_summary_copy(&effect_context, contracts, 3, &summary) == XR_XIR_BAD_STRUCTURE);
    CHECK(!summary.rows && !summary.values);
    values[0].kind = XR_XIR_EFFECT_PARAMETER_FIXED;
    effect_mark_check(baseline);

    XrXirStatus status = XR_XIR_OK;
    XrXirFunctionEffectContract *owned = xir_compile_calloc(&effect_context, 3, sizeof(*owned), &status);
    XrXirEffectParameter *inputs = xir_compile_alloc(&effect_context, sizeof(values), &status);
    CHECK(status == XR_XIR_OK && owned && inputs);
    memcpy(owned, contracts, sizeof(contracts));
    memcpy(inputs, values, sizeof(values));
    owned[0].parameters = inputs;
    owned[2].parameters = inputs + 2;
    CHECK(effect_parameter_summary_copy(&effect_context, owned, 3, &summary) == XR_XIR_OK);
    CHECK(effect_parameter_summary_matches(&effect_context, &summary, owned, 3) == XR_XIR_OK);
    inputs[0].kind = XR_XIR_EFFECT_PARAMETER_VARIABLE;
    CHECK(effect_parameter_summary_matches(&effect_context, &summary, owned, 3) == XR_XIR_BAD_STRUCTURE);
    inputs[0] = values[0];
    inputs[0].uses ^= XR_XIR_EFFECT_USE_INVOKE;
    CHECK(effect_parameter_summary_matches(&effect_context, &summary, owned, 3) == XR_XIR_BAD_STRUCTURE);
    xr_compile_resources_free(inputs);
    xr_compile_resources_free(owned);
    memset(values, 0xCE, sizeof(values));
    memset(contracts, 0xCE, sizeof(contracts));
    attempts = 0;
    CHECK(effect_parameter_summary_get(&summary, 0, 0, &kind) == XR_XIR_OK && kind == XR_XIR_EFFECT_PARAMETER_FIXED);
    CHECK(effect_parameter_summary_get(&summary, 0, 1, &kind) == XR_XIR_OK && kind == XR_XIR_EFFECT_PARAMETER_VARIABLE);
    CHECK(!attempts && summary.values[0].uses == XR_XIR_EFFECT_USE_STORE);
    CHECK(effect_parameter_summary_shape(&effect_context, &summary, 3) == XR_XIR_OK);
    effect_parameter_summary_clear(&summary);
    effect_mark_check(baseline);

    memset(contracts, 0, sizeof(contracts));
    CHECK(effect_parameter_summary_copy(&effect_context, contracts, 3, &summary) == XR_XIR_OK);
    CHECK(summary.count == 3 && !summary.total && !summary.values);
    CHECK(effect_parameter_summary_shape(&effect_context, &summary, 3) == XR_XIR_OK);
    effect_parameter_summary_clear(&summary);
    effect_mark_check(baseline);
    printf("owned parameter rows: %zu allocation failures, detached scalar reads and finite owner PASS\n", sites);
}
static void source_reference_summary_kernel(void) {
    EffectMark baseline = effect_mark();
    XrXirFunction functions[3] = {
        {.instruction_count = 3}, {.instruction_count = UINT32_MAX}, {.instruction_count = 2}
    };
    XrXirGeneric generics[3] = {{0}, {.parameter_count = 1}, {0}};
    XrXirModule module = {.functions = functions, .function_count = 3, .generics = generics};
    EffectSourceSummary summary = {0};
    attempts = 0;
    CHECK(effect_source_summary_begin(&effect_context, &module, &summary) == XR_XIR_OK);
    size_t sites = attempts;
    CHECK(summary.count == 3 && summary.total == 5 && !summary.rows[1].count);
    uint32_t mask = UINT32_MAX;
    CHECK(effect_source_summary_read(&summary, 0, 1, &mask) == XR_XIR_BAD_STRUCTURE && mask == UINT32_MAX);
    summary.masks[1] = EFFECT_SOURCE_REFERENCE_PRESENT;
    summary.masks[3] = EFFECT_SOURCE_REFERENCE_PRESENT | XR_XIR_CALLABLE_ROOT_REQUIRED | XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    memset(functions, 0xCE, sizeof(functions));
    memset(generics, 0xCE, sizeof(generics));
    attempts = 0;
    CHECK(effect_source_summary_read(&summary, 0, 1, &mask) == XR_XIR_OK && !mask);
    CHECK(effect_source_summary_read(&summary, 2, 0, &mask) == XR_XIR_OK &&
        mask == (XR_XIR_CALLABLE_ROOT_REQUIRED | XR_XIR_CALLABLE_ROOT_UNRESOLVED));
    CHECK(!attempts);
    mask = UINT32_MAX;
    CHECK(effect_source_summary_read(&summary, 1, 0, &mask) == XR_XIR_BAD_STRUCTURE && mask == UINT32_MAX);
    CHECK(effect_source_summary_read(&summary, 3, 0, &mask) == XR_XIR_BAD_STRUCTURE && mask == UINT32_MAX);
    CHECK(effect_source_summary_read(&summary, 0, 3, &mask) == XR_XIR_BAD_STRUCTURE && mask == UINT32_MAX);
    CHECK(effect_source_summary_read(&summary, 0, 0, &mask) == XR_XIR_BAD_STRUCTURE && mask == UINT32_MAX);
    summary.masks[1] |= 64u;
    CHECK(effect_source_summary_read(&summary, 0, 1, &mask) == XR_XIR_BAD_STRUCTURE && mask == UINT32_MAX);
    EffectSourceSummary saved = summary;
    CHECK(effect_source_summary_begin(&effect_context, &module, &summary) == XR_XIR_BAD_STRUCTURE);
    CHECK(!attempts && !memcmp(&summary, &saved, sizeof(summary)));
    effect_source_summary_clear(&summary);
    effect_mark_check(baseline);

    functions[0] = (XrXirFunction){.instruction_count = 200};
    functions[1] = (XrXirFunction){.instruction_count = 200};
    module.function_count = 2;
    module.generics = NULL;
    for (size_t point = 0; point < sites; ++point) {
        attempts = 0; injected = false; fail_at = point;
        CHECK(effect_source_summary_begin(&effect_context, &module, &summary) == XR_XIR_OUT_OF_MEMORY);
        CHECK(injected && !summary.resources && !summary.rows && !summary.masks && !summary.count && !summary.total);
        fail_at = SIZE_MAX;
        effect_mark_check(baseline);
    }
    XrXirCompileContext finite = effect_owner_new((XrCompileResourceLimits){XR_COMPILE_MEMORY_FINITE, 128, true});
    uint64_t held = effect_stats(&finite).held_bytes;
    CHECK(effect_source_summary_begin(&finite, &module, &summary) == XR_XIR_MEMORY_LIMIT);
    CHECK(!summary.resources && !summary.rows && !summary.masks && !summary.count && !summary.total);
    effect_owner_free(&finite, held);
    functions[0].instruction_count = functions[1].instruction_count = 0;
    CHECK(effect_source_summary_begin(&effect_context, &module, &summary) == XR_XIR_OK);
    CHECK(summary.count == 2 && !summary.total && !summary.masks);
    effect_source_summary_clear(&summary);
    effect_mark_check(baseline);
    printf("source reference summaries: %zu allocation failures, body-free reads and finite owner PASS\n", sites);
}
static void control_summary_streamed(void) {
    EffectMark baseline = effect_mark();
    XrXirInstruction rows[3][2] = {
        {{.op = XR_XIR_CALL, .immediate = 1}, {.op = XR_XIR_RETURN}},
        {{.op = XR_XIR_CALL, .immediate = 2}, {.op = XR_XIR_RETURN}},
        {{.op = XR_XIR_SUSPEND}, {.op = XR_XIR_RETURN}}
    };
    XrXirFunction functions[3] = {0};
    for (uint32_t f = 0; f < 3; ++f)
        functions[f] = (XrXirFunction){.name = "f", .name_length = 1, .error_type = XR_XIR_NEVER,
            .instructions = rows[f], .instruction_count = 2};
    XrXirModule module = {.stage = XR_XIR_CHECKED, .functions = functions, .function_count = 3};
    XirControlBuilder *builder = NULL;
    EffectControlSummary *summary = NULL, *retained = NULL;
    CHECK(xir_effect_control_builder_new(&effect_context, &module, NULL, &builder) == XR_XIR_OK);
    const uint32_t order[] = {1, 0, 2};
    for (uint32_t at = 0; at < 3; ++at) {
        uint32_t f = order[at];
        CHECK(xir_effect_control_begin(builder, &module, f) == XR_XIR_OK);
        CHECK(xir_effect_control_begin(builder, &module, f) == XR_XIR_BAD_STRUCTURE);
        CHECK(xir_effect_control_instruction(builder, &module, f, 1) == XR_XIR_BAD_STRUCTURE);
        CHECK(xir_effect_control_end(builder, &module, f) == XR_XIR_BAD_STRUCTURE);
        CHECK(xir_effect_control_finish(builder, &module, &summary) == XR_XIR_BAD_STRUCTURE && !summary);
        for (uint32_t i = 0; i < 2; ++i)
            CHECK(xir_effect_control_instruction(builder, &module, f, i) == XR_XIR_OK);
        CHECK(xir_effect_control_end(builder, &module, f) == XR_XIR_OK);
        CHECK(xir_effect_control_begin(builder, &module, f) == XR_XIR_BAD_STRUCTURE);
    }
    memset(rows, 0xCE, sizeof(rows));
    for (uint32_t f = 0; f < 3; ++f) {
        functions[f].instructions = NULL;
        functions[f].instruction_count = UINT32_MAX;
    }
    CHECK(xir_effect_control_finish(builder, &module, &summary) == XR_XIR_OK);
    CHECK(xir_effect_control_finish(builder, &module, &retained) == XR_XIR_BAD_STRUCTURE && !retained);
    xir_effect_control_builder_free(builder);
    CHECK(summary->facts[0].suspend == XR_XIR_EFFECT_MAY && summary->causes[0].callee == 1);
    CHECK(summary->causes[0].distance == 2 && summary->causes[1].callee == 2);
    CHECK(xir_effect_control_retain(&effect_context, summary, &retained) == XR_XIR_OK);
    CHECK(xir_effect_control_retain(&effect_context, summary, &retained) == XR_XIR_BAD_STRUCTURE);
    xir_effect_control_free(summary);
    CHECK(retained->causes[2].cause == XR_XIR_EFFECT_CAUSE_SUSPEND);
    xir_effect_control_free(retained);
    effect_mark_check(baseline);
    puts("streamed control summaries close without bodies and retain independent ownership PASS");
}
static void control_summary_kernel(void) {
    parameter_summary_kernel();
    parameter_summary_owner();
    source_reference_summary_kernel();
    control_summary_streamed();
    const uint32_t count = 8193;
    XrXirFunction *functions = xr_calloc(count, sizeof(*functions));
    XrXirInstruction (*rows)[2] = xr_calloc(count, sizeof(*rows));
    XrXirBlock *blocks = xr_calloc(count, sizeof(*blocks));
    CHECK(functions && rows && blocks);
    for (uint32_t f = 0; f < count; ++f) {
        rows[f][0] = (XrXirInstruction){.op = XR_XIR_CALL, .immediate = f + 1};
        rows[f][1] = (XrXirInstruction){.op = XR_XIR_RETURN};
        blocks[f] = (XrXirBlock){.count = 2};
        functions[f] = (XrXirFunction){.name = "f", .name_length = 1, .error_type = XR_XIR_NEVER,
            .blocks = &blocks[f], .block_count = 1, .instructions = rows[f], .instruction_count = 2};
    }
    rows[count - 1][0] = (XrXirInstruction){.op = XR_XIR_SUSPEND};
    XrXirModule module = {.stage = XR_XIR_BUILT, .functions = functions, .function_count = count};
    EffectControlSummary *summary = NULL;
    EffectMark baseline = effect_mark();
    CHECK(effect_control_summary_build(&effect_context, &module, NULL, &summary) == XR_XIR_OK);
    for (uint32_t f = 0; f < count; ++f) {
        CHECK(summary->facts[f].suspend == XR_XIR_EFFECT_MAY);
        CHECK(summary->facts[f].throws == XR_XIR_EFFECT_NONE);
        CHECK(summary->causes[f].distance == count - f - 1);
    }
    EffectControlSummary *occupied = summary;
    size_t before = attempts;
    CHECK(effect_control_summary_build(&effect_context, &module, NULL, &occupied) == XR_XIR_BAD_STRUCTURE);
    CHECK(occupied == summary && attempts == before);
    xir_effect_control_free(summary);
    effect_mark_check(baseline);

    module.function_count = 4;
    rows[2][0].immediate = 0;
    rows[3][0].immediate = 3;
    summary = NULL;
    CHECK(effect_control_summary_build(&effect_context, &module, NULL, &summary) == XR_XIR_OK);
    for (uint32_t f = 0; f < 4; ++f) CHECK(summary->facts[f].suspend == XR_XIR_EFFECT_NONE);
    xir_effect_control_free(summary);

    XrXirInstruction terminal[3] = {
        {.op = XR_XIR_CALL}, {.op = XR_XIR_SUSPEND}, {.op = XR_XIR_RETURN}
    };
    functions[2].instructions = terminal;
    functions[2].instruction_count = blocks[2].count = 3;
    summary = NULL;
    attempts = 0;
    CHECK(effect_control_summary_build(&effect_context, &module, NULL, &summary) == XR_XIR_OK);
    size_t sites = attempts;
    for (uint32_t f = 0; f < 4; ++f)
        CHECK(summary->facts[f].suspend == (f < 3 ? XR_XIR_EFFECT_MAY : XR_XIR_EFFECT_NONE));
    CHECK(summary->causes[0].callee == 1 && summary->causes[0].distance == 2);
    xir_effect_control_free(summary);
    effect_mark_check(baseline);
    for (size_t point = 0; point < sites; ++point) {
        summary = NULL;
        attempts = 0;
        injected = false;
        fail_at = point;
        CHECK(effect_control_summary_build(&effect_context, &module, NULL, &summary) == XR_XIR_OUT_OF_MEMORY);
        CHECK(injected && !summary);
        fail_at = SIZE_MAX;
        effect_mark_check(baseline);
    }
    XrXirCompileContext finite = effect_owner_new((XrCompileResourceLimits){XR_COMPILE_MEMORY_FINITE, 128, true});
    uint64_t held = effect_stats(&finite).held_bytes;
    summary = NULL;
    CHECK(effect_control_summary_build(&finite, &module, NULL, &summary) == XR_XIR_MEMORY_LIMIT && !summary);
    effect_owner_free(&finite, held);
    effect_mark_check(baseline);

    summary = NULL;
    CHECK(effect_control_summary_build(&effect_context, &module, NULL, &summary) == XR_XIR_OK);
    memset(functions, 0xCE, count * sizeof(*functions));
    memset(rows, 0xCE, count * sizeof(*rows));
    memset(terminal, 0xCE, sizeof(terminal));
    xr_free(functions);
    xr_free(rows);
    xr_free(blocks);
    CHECK(summary->facts[0].suspend == XR_XIR_EFFECT_MAY && summary->causes[0].callee == 1);
    xir_effect_control_free(summary);
    effect_mark_check(baseline);
    printf("control summaries: iterative long chain, seeded/unseeded SCC, %zu allocation failures and detached owner PASS\n", sites);
}

int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && (!strcmp(argv[1],"--var-atomic") || !strcmp(argv[1],"--ordinary") ||
        !strcmp(argv[1],"--full") || !strcmp(argv[1],"--control-summary"))));
    if (argc == 2 && !strcmp(argv[1], "--control-summary")) {
        effect_case_begin();control_summary_kernel();effect_case_end();return 0;
    }
    effect_full_faults=argc==2 && !strcmp(argv[1],"--full");
    bool resources=argc==1 || effect_full_faults;
    effect_case_begin();root_var_atomic_case();effect_case_end();
    if (argc==2 && !strcmp(argv[1],"--var-atomic")) return 0;
    effect_case_begin();root_source_cases(resources);effect_case_end();
    effect_case_begin();root_builtin_cases();effect_case_end();
    effect_case_begin();root_go_rejections();effect_case_end();
    puts("owned root fact queries and independent causes PASS; GO consumer/full qualification remain separate");return 0;
}
