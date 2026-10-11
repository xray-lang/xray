/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_early_generation_cases.h - Sole same-generation input lifetime
 */
#ifndef XIR_INVOCATION_EARLY_GENERATION_CASES_H
#define XIR_INVOCATION_EARLY_GENERATION_CASES_H

static void conditional_generation_ownership(const XrXirCompileContext *context,
    EffectBodyMaterial *material,const XrXirModule *source) {
    CHECK(material && material->physical && material->cells && material->declared &&
        material->resources==context->resources && material->physical->resources==context->resources);
    CHECK(effect_body_source_match(context,material,source)==XR_XIR_OK &&
        effect_body_physical_match(context,material->physical,material)==XR_XIR_OK);
    EffectCellBodyAdapter adapter={0};XrXirCellProofBodyRead read={0};
    CHECK(effect_cell_body_owned(context,material,&adapter,&read)==XR_XIR_OK &&
        xr_xir_compile_cell_provenance_body_matches(context,material->cells,&read,NULL)==XR_XIR_OK);
    EffectBodyMaterial *occupied=material;size_t attempts=rp_attempts;
    CHECK(effect_body_material_create(context,source,&occupied)==XR_XIR_BAD_STRUCTURE &&
        occupied==material && rp_attempts==attempts);
    /* Every exact source word is a separate independent correspondence obligation. */
    XrXirInstruction *operations=(XrXirInstruction *)source->functions[0].instructions;
    XrXirInstruction saved=operations[0];
    for (uint32_t field=0;field<10;++field) {
        conditional_compact_change(&operations[0],field);
        CHECK(effect_body_source_match(context,material,source)==XR_XIR_BAD_STRUCTURE);
        operations[0]=saved;
        CHECK(effect_body_source_match(context,material,source)==XR_XIR_OK);
    }
    uint32_t kind=material->evidence_kind;
    material->evidence_kind=XR_XIR_EVIDENCE_TEMPLATE;
    CHECK(effect_body_source_match(context,material,source)==XR_XIR_BAD_STRUCTURE);
    material->evidence_kind=kind;
    CHECK(material->functions[1].parameter_count && material->functions[1].initial_roles);
    uint8_t *roles=(uint8_t *)material->functions[1].initial_roles;
    uint8_t role=roles[0];roles[0]=(uint8_t)(role==XR_XIR_EFFECT_PARAMETER_FIXED ?
        XR_XIR_EFFECT_PARAMETER_VARIABLE:XR_XIR_EFFECT_PARAMETER_FIXED);
    CHECK(effect_body_source_match(context,material,source)==XR_XIR_BAD_STRUCTURE);
    roles[0]=role;CHECK(effect_body_source_match(context,material,source)==XR_XIR_OK);
    EffectInvocationCertificate boundary={.resources=context->resources,.body=material};
    boundary.terms.remaining=context;
    CHECK(effect_invocation_bind_bounds(&boundary,material->declared,material->physical)==XR_XIR_OK);
    EffectInvocationFunctionBounds *function=&material->declared->functions[1];
    CHECK(function->count && function->bounds && function->bounds[0].callable);
    bool callable=function->bounds[0].callable;function->bounds[0].callable=false;
    boundary.declared=NULL;
    CHECK(effect_invocation_bind_bounds(&boundary,material->declared,material->physical)==XR_XIR_BAD_STRUCTURE &&
        !boundary.declared);function->bounds[0].callable=callable;
    CHECK(effect_invocation_bind_bounds(&boundary,material->declared,material->physical)==XR_XIR_OK);
}

static void conditional_generation_control_boundary(const XrXirCompileContext *context,
    const XrXirModule *source,XrXirEffects *effects,EffectGraph *graph) {
    EffectBodyMaterial *generation=effects->generation;
    EffectPhysicalDomain *physical=graph->physical;uint32_t count=effects->count;
    size_t attempts=rp_attempts;XrCompileResourceStats before=rp_stats(context);
    effects->generation=NULL;
    CHECK(effect_graph_build_material(source,effects,graph,(XrXirCompileContext *)context)==XR_XIR_BAD_STRUCTURE);
    effects->generation=generation;graph->physical=NULL;
    CHECK(effect_graph_build_material(source,effects,graph,(XrXirCompileContext *)context)==XR_XIR_BAD_STRUCTURE);
    graph->physical=physical;effects->count=count+1;
    CHECK(effect_graph_build_material(source,effects,graph,(XrXirCompileContext *)context)==XR_XIR_BAD_STRUCTURE);
    effects->count=count;
    CHECK(!graph->heads && !graph->queue && !graph->queued && !graph->edges &&
        !graph->references && !graph->reference_heads && graph->material==generation &&
        !graph->physical_owned && rp_attempts==attempts && rp_stats(context).work==before.work);
    XrXirCompileContext foreign=rp_owner(rp_caps());uint64_t live=rp_stats(&foreign).live_bytes;
    attempts=rp_attempts;before=rp_stats(&foreign);
    CHECK(effect_graph_build_material(source,effects,graph,&foreign)==XR_XIR_BAD_STRUCTURE &&
        rp_attempts==attempts && rp_stats(&foreign).work==before.work && graph->material==generation);
    rp_owner_free(&foreign,live);
}

static XrXirStatus conditional_generation_case(const XrXirCompileContext *context,uint32_t mode,bool oracle) {
    if (mode>=3) return XR_XIR_BAD_STRUCTURE;
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,mode==1);
    XrXirArtifact *checked=NULL;XrXirEffects effects={0};EffectGraph graph={0};
    EffectInvocationOwner *owner=NULL;EffectInvocationSolution *solution=NULL;
    EffectInvocationCertificate *certificate=NULL;EffectBodyMaterial *material=NULL;
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(checked,NULL);
    const XrXirModule *source=checked?xr_xir_compile_artifact_module(checked):NULL;
    if (status==XR_XIR_OK) status=invocation_core_storage(context,source,&effects);
    if (status==XR_XIR_OK) status=effect_parameters_derive(source,&effects,&graph,context);
    if (status==XR_XIR_OK) material=graph.material;
    if (status==XR_XIR_OK && oracle) conditional_generation_control_boundary(context,source,&effects,&graph);
    if (status==XR_XIR_OK) status=effect_graph_build_material(source,&effects,&graph,(XrXirCompileContext *)context);
    if (status==XR_XIR_OK && oracle) {
        CHECK(material==effects.generation && material==graph.material && !graph.physical_owned &&
            effects.cells==material->cells && material->cells);
        conditional_generation_ownership(context,material,source);
    }
    if (status==XR_XIR_OK && mode==2) {
        /* A real later source generation receives a new complete owner. The
         * previous scalar payload and bound records remain independently owned. */
        XrXirInstruction *operations=(XrXirInstruction *)source->functions[0].instructions;
        int64_t immediate=operations[0].immediate;operations[0].immediate=43;
        EffectBodyMaterial *later=NULL;status=effect_body_material_create(context,source,&later);
        if (status==XR_XIR_OK) status=effect_body_source_match(context,later,source);
        if (status==XR_XIR_OK && oracle) {
            CHECK(later!=material && later->cells!=material->cells && later->declared!=material->declared);
            EffectBodyRequest old=effect_body_owned_request(context,material);
            XrXirInstruction storage;const XrXirInstruction *operation=NULL;
            CHECK(effect_body_request_instruction(&old,0,0,&storage,&operation)==XR_XIR_OK &&
                operation->immediate==42);
            CHECK(effect_body_source_match(context,material,source)==XR_XIR_BAD_STRUCTURE);
        }
        operations[0].immediate=immediate;effect_body_material_free(later);
    }
    if (status==XR_XIR_OK) status=effect_invocations_create(context,source,&effects,&graph,material->declared,&owner);
    if (status==XR_XIR_OK) status=effect_invocations_solve(owner);
    if (status==XR_XIR_OK) status=effect_invocation_stage(owner,&solution);
    EffectInvocationSealRequest request={&owner,&solution,NULL};
    if (status==XR_XIR_OK) status=effect_invocation_seal(context,&request,&certificate);
    if (status==XR_XIR_OK && oracle) CHECK(!owner && !solution && certificate->body==material &&
        certificate->declared==material->declared && !effects.generation &&
        certificate->equations->material==material && certificate->equations->cells==material->cells);
    effect_invocation_free(owner);effect_invocation_solution_free(solution);
    effect_graph_free(&graph);invocation_core_storage_free(&effects);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    if (status==XR_XIR_OK) {
        EffectBodyRequest read=effect_body_owned_request(context,certificate->body);
        XrXirInstruction storage;const XrXirInstruction *operation=NULL;
        status=effect_body_request_instruction(&read,0,0,&storage,&operation);
        if (status==XR_XIR_OK && oracle) CHECK(operation->op==(mode==1?XR_XIR_SLOT_LOAD:XR_XIR_CONST_INT) &&
            (mode==1 || operation->immediate==42));
        if (status==XR_XIR_OK) status=effect_body_physical_match(context,material->physical,material);
        EffectCellBodyAdapter adapter={0};XrXirCellProofBodyRead cells={0};
        if (status==XR_XIR_OK) status=effect_cell_body_owned(context,material,&adapter,&cells);
        if (status==XR_XIR_OK) status=xr_xir_compile_cell_provenance_body_matches(context,material->cells,&cells,NULL);
        uint32_t mask=UINT32_MAX;
        if (status==XR_XIR_OK) status=effect_invocation_certificate_node_mask(context,certificate,
            certificate->equations->roots[0],&mask);
        if (status==XR_XIR_OK && oracle) CHECK(mask==(mode==1?XR_XIR_CALLABLE_ROOT_REQUIRED:0));
    }
    effect_invocation_certificate_free(certificate);return status;
}

static void conditional_generation_literals(void) {
    for (uint32_t mode=0;mode<3;++mode) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        CHECK(conditional_generation_case(&context,mode,true)==XR_XIR_OK);
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
}
static void conditional_generation_resources(void) {
    for (uint32_t mode=0;mode<3;++mode) {
        size_t sites=0;XrCompileResourceStats single={0};
        for (size_t pass=0;pass<=sites;++pass) {
            RootParameterMark physical=rp_mark();rp_fail_at=SIZE_MAX;rp_attempts=0;rp_injected=false;
            XrXirCompileContext context=rp_owner(rp_caps());XrCompileResourceStats initial=rp_stats(&context);
            RootParameterMark retained=rp_mark();XrCompileResources *identity=context.resources;
            rp_attempts=0;rp_fail_at=pass?pass-1:SIZE_MAX;
            XrXirStatus status=conditional_generation_case(&context,mode,false);
            if (!pass) { CHECK(status==XR_XIR_OK);sites=rp_attempts;single=rp_stats(&context); }
            else {
                CHECK(rp_injected && status==XR_XIR_OUT_OF_MEMORY);
                ConditionalRetryTrial retry={.which=97+mode,.ordinal=pass-1,.sites=sites,.resources=identity,
                    .initial=initial,.single=single,.retained=retained,.first=status};
                conditional_retry_operation(&context,&retry);
            }
            rp_fail_at=SIZE_MAX;rp_owner_free(&context,initial.live_bytes);rp_balanced(physical);
        }
        uint64_t measured[3]={single.allocated_bytes,single.peak_bytes,single.work};
        for (uint32_t axis=0;axis<3;++axis) for (uint32_t exact=0;exact<2;++exact) {
            RootParameterMark physical=rp_mark();XrCompileResourceLimits limits=rp_caps();
            CHECK(measured[axis]);uint64_t value=measured[axis]-(exact?0:1);
            if (!axis) limits.allocated_bytes=value;else if (axis==1) limits.live_bytes=value;else limits.work=value;
            XrXirCompileContext context=rp_owner(limits);XrCompileResourceStats initial=rp_stats(&context);
            RootParameterMark retained=rp_mark();XrCompileResources *identity=context.resources;
            XrXirStatus status=conditional_generation_case(&context,mode,false);
            CHECK(status==(exact?XR_XIR_OK:XR_XIR_BUDGET));
            ConditionalRetryTrial retry={.which=97+mode,.ordinal=SIZE_MAX,.sites=sites,.resources=identity,
                .initial=initial,.single=single,.retained=retained,.first=status};
            conditional_retry_operation(&context,&retry);
            rp_owner_free(&context,initial.live_bytes);rp_balanced(physical);
        }
    }
}
#endif // XIR_INVOCATION_EARLY_GENERATION_CASES_H
