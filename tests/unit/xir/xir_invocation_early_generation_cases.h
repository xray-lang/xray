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

/* Count genuine owned reads for this one local request. The delegated
 * reader still verifies its complete canonical record and charges its work. */
static uint32_t conditional_generation_instruction_reads;
static bool conditional_generation_instruction_refused;
static XrXirStatus conditional_generation_counted_instruction(const EffectBodyRequest *request,
    uint32_t function,uint32_t ordinal,XrXirInstruction *storage,
    const XrXirInstruction **output,EffectBodyInstructionFields *fields) {
    ++conditional_generation_instruction_reads;
    if (conditional_generation_instruction_refused) return XR_XIR_BAD_STRUCTURE;
    return effect_body_owned_instruction(request,function,ordinal,storage,output,fields);
}

static XrXirStatus conditional_generation_decoded_controls(const XrXirCompileContext *context,
    EffectInvocationOwner *owner) {
    EffectInvocationFlow flow={.owner=owner,.node=owner->roots[0],.instruction=UINT32_MAX};
    XrXirStatus status=effect_invocation_origins(&flow);
    if (status!=XR_XIR_OK) { xr_compile_resources_free(flow.memory);return status; }
    uint32_t body=owner->nodes[flow.node].body,call=UINT32_MAX,reference=UINT32_MAX;
    for (uint32_t i=0;i<flow.function->instruction_count;++i) {
        XrXirInstruction storage;const XrXirInstruction *op=NULL;
        status=effect_invocation_flow_instruction(&flow,i,&storage,&op);
        if (status!=XR_XIR_OK) break;
        if (op->op==XR_XIR_CALL) call=i;
        if (op->op==XR_XIR_FUNCTION_REF) reference=i;
    }
    CHECK(status!=XR_XIR_OK || (call!=UINT32_MAX && reference!=UINT32_MAX && body==0));
    if (status==XR_XIR_OK) {
        flow.body.instruction=conditional_generation_counted_instruction;
        conditional_generation_instruction_reads=0;conditional_generation_instruction_refused=false;
        XrXirInstruction storage;const XrXirInstruction *op=NULL;
        status=effect_invocation_flow_instruction(&flow,call,&storage,&op);
        if (status==XR_XIR_OK) {
            CHECK(conditional_generation_instruction_reads==1);
            {
                uint64_t *arguments=NULL;
                status=effect_invocation_arguments(&flow,op,(uint32_t)op->immediate,
                    UINT32_MAX,NULL,&arguments);
                if (status==XR_XIR_OK) {
                    CHECK(arguments && conditional_generation_instruction_reads==1);
                    size_t argument_attempts=rp_attempts;XrCompileResourceStats argument_before=rp_stats(context);
                    uint64_t *occupied=arguments;
                    CHECK(effect_invocation_arguments(&flow,op,(uint32_t)op->immediate,
                        UINT32_MAX,NULL,&occupied)==XR_XIR_BAD_STRUCTURE && occupied==arguments &&
                        rp_attempts==argument_attempts && rp_stats(context).work==argument_before.work);
                    uint64_t *missing=NULL;
                    CHECK(effect_invocation_arguments(&flow,NULL,(uint32_t)op->immediate,
                        UINT32_MAX,NULL,&missing)==XR_XIR_BAD_STRUCTURE && !missing &&
                        rp_attempts==argument_attempts && rp_stats(context).work==argument_before.work);
                    XrXirInstruction wrong=*op;wrong.args[1]++;
                    CHECK(effect_invocation_arguments(&flow,&wrong,(uint32_t)op->immediate,
                        UINT32_MAX,NULL,&missing)==XR_XIR_BAD_STRUCTURE && !missing &&
                        rp_attempts==argument_attempts && conditional_generation_instruction_reads==1);
                }
                xr_compile_resources_free(arguments);
            }
            bool closed=false;
            CHECK(effect_invocation_closed_arguments(&flow,(uint32_t)op->immediate,op,&closed)==XR_XIR_OK &&
                closed && conditional_generation_instruction_reads==1);
            XrXirInstruction malformed=*op;malformed.args[1]++;
            closed=true;size_t attempts=rp_attempts;
            CHECK(effect_invocation_closed_arguments(&flow,(uint32_t)op->immediate,&malformed,&closed)==
                XR_XIR_BAD_STRUCTURE && closed && rp_attempts==attempts &&
                conditional_generation_instruction_reads==1);
            bool changed=false;uint32_t edges=owner->edge_count;
            attempts=rp_attempts;XrCompileResourceStats before=rp_stats(context);
            CHECK(effect_invocation_instruction_decoded(&flow,call,NULL,&changed)==XR_XIR_BAD_STRUCTURE &&
                !changed && owner->edge_count==edges && rp_attempts==attempts &&
                rp_stats(context).work==before.work);
            conditional_generation_instruction_refused=true;before=rp_stats(context);
            CHECK(effect_invocation_instruction(&flow,call,&changed)==XR_XIR_BAD_STRUCTURE &&
                !changed && owner->edge_count==edges && rp_attempts==attempts &&
                conditional_generation_instruction_reads==2 && rp_stats(context).work==before.work+1);
            conditional_generation_instruction_refused=false;
        }
        if (status==XR_XIR_OK) {
            uint32_t selected=UINT32_MAX;status=effect_invocation_site(&flow,reference,&selected);
            if (status==XR_XIR_OK) status=effect_invocation_flow_instruction(&flow,reference,&storage,&op);
            if (status==XR_XIR_OK) {
                uint32_t reads=conditional_generation_instruction_reads;bool authentic=false;
                CHECK(effect_invocation_ref_target(&flow,op,&authentic)==XR_XIR_OK &&
                    authentic && conditional_generation_instruction_reads==reads);
                XrXirInstruction malformed=*op;malformed.type=XR_XIR_I64;
                authentic=true;size_t attempts=rp_attempts;
                CHECK(effect_invocation_ref_target(&flow,&malformed,&authentic)==XR_XIR_BAD_STRUCTURE &&
                    !authentic && rp_attempts==attempts && conditional_generation_instruction_reads==reads);
                authentic=true;XrCompileResourceStats before=rp_stats(context);
                CHECK(effect_invocation_ref_target(&flow,NULL,&authentic)==XR_XIR_BAD_STRUCTURE &&
                    authentic && rp_stats(context).work==before.work && rp_attempts==attempts);
                uint64_t *input=NULL;
                status=effect_invocation_latent_input(&flow,reference,selected,op,&input);
                if (status==XR_XIR_OK) {
                    CHECK(conditional_generation_instruction_reads==reads && input);
                    uint64_t *occupied=input;size_t input_attempts=rp_attempts;
                    XrCompileResourceStats input_before=rp_stats(context);
                    CHECK(effect_invocation_latent_input(&flow,reference,selected,op,&occupied)==
                        XR_XIR_BAD_STRUCTURE && occupied==input && rp_attempts==input_attempts &&
                        rp_stats(context).work==input_before.work);
                }
                xr_compile_resources_free(input);
            }
        }
    }
    conditional_generation_instruction_refused=false;
    xr_compile_resources_free(flow.memory);return status;
}

/* These are literal owned-reader observations, never a permission cache. */
static uint32_t conditional_generation_shape_reads,conditional_generation_full_heads,
    conditional_generation_dimension_heads;
static bool conditional_generation_shape_refused,conditional_generation_dimensions_refused;
static XrXirStatus conditional_generation_counted_shape(const EffectBodyRequest *request,
    uint32_t function,uint32_t ordinal,EffectBodyInstructionShape *output) {
    ++conditional_generation_shape_reads;
    if (conditional_generation_shape_refused) return XR_XIR_BAD_STRUCTURE;
    return effect_body_owned_shape(request,function,ordinal,output);
}
static XrXirStatus conditional_generation_counted_head(const EffectBodyRequest *request,
    uint32_t function,EffectBodyFunctionHead *output,EffectBodyFunctionDimensions *dimensions) {
    if (dimensions) {
        ++conditional_generation_dimension_heads;
        if (conditional_generation_dimensions_refused) return XR_XIR_BAD_STRUCTURE;
    } else ++conditional_generation_full_heads;
    return effect_body_owned_head(request,function,output,dimensions);
}

static void conditional_generation_alias_fixture(InvocationDeferredFixture *fixture,
    XrXirInstruction *entry,uint32_t mode) {
    uint32_t reference=mode?1:2,count=fixture->entry_block.count;
    for (uint32_t i=0;i<=reference;++i) entry[i]=fixture->entry[i];
    entry[reference+1]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)257,.args={reference,0}};
    for (uint32_t i=reference+1;i<count;++i) entry[i+1]=fixture->entry[i];
    ++entry[count].args[0];++fixture->actuals[0];++fixture->entry_block.count;
    fixture->functions[0].instructions=entry;fixture->functions[0].instruction_count=count+1;
}

static void conditional_generation_projected_controls(const XrXirCompileContext *context,
    EffectInvocationOwner *owner) {
    CHECK(!owner->producer_enabled && !owner->pending);
    EffectBodyRequest read=effect_body_owned_request(context,owner->material);
    read.shape=conditional_generation_counted_shape;
    EffectInvocationFlow flow={.owner=owner,.node=owner->roots[0],.instruction=UINT32_MAX};
    uint32_t body=owner->nodes[flow.node].body,expected=0;
    const EffectPhysicalFunction *physical=&owner->physical->functions[body];
    for (uint32_t r=0;r<physical->count;++r) {
        EffectPhysicalRecord record=physical->records[r];
        if (record.value>=physical->parameters && (record.mask&EFFECT_PHYSICAL_CALLABLE) &&
            !(record.mask&EFFECT_PHYSICAL_REFERENCE)) ++expected;
    }
    CHECK(expected && body==0);
    conditional_generation_shape_reads=0;conditional_generation_shape_refused=false;
    CHECK(effect_invocation_origins_read(&flow,&read)==XR_XIR_OK &&
        conditional_generation_shape_reads==expected);
    xr_compile_resources_free(flow.memory);flow.memory=NULL;
    conditional_generation_shape_refused=true;conditional_generation_shape_reads=0;
    CHECK(effect_invocation_origins_read(&flow,&read)==XR_XIR_BAD_STRUCTURE &&
        conditional_generation_shape_reads==1);
    xr_compile_resources_free(flow.memory);flow.memory=NULL;
    conditional_generation_shape_refused=false;
    read.head=conditional_generation_counted_head;
    uint32_t dimensions=owner->count;
    for (uint32_t n=0;n<owner->count;++n)
        if (owner->nodes[n].root!=owner->nodes[n].body) ++dimensions;
    conditional_generation_full_heads=conditional_generation_dimension_heads=0;
    conditional_generation_dimensions_refused=false;
    CHECK(effect_invocation_producers_verify_read(owner,&read)==XR_XIR_OK &&
        conditional_generation_full_heads==read.domain.function_count &&
        conditional_generation_dimension_heads==dimensions);
    size_t attempts=rp_attempts;uint32_t count=owner->count;
    conditional_generation_dimensions_refused=true;
    CHECK(effect_invocation_producers_verify_read(owner,&read)==XR_XIR_BAD_STRUCTURE &&
        owner->count==count && !owner->pending && rp_attempts==attempts);
    conditional_generation_dimensions_refused=false;
    uint32_t root=owner->nodes[0].root;owner->nodes[0].root=UINT32_MAX;
    CHECK(effect_invocation_producers_verify_read(owner,&read)==XR_XIR_BAD_STRUCTURE &&
        owner->count==count && !owner->pending && rp_attempts==attempts);
    owner->nodes[0].root=root;
    CHECK(effect_invocation_producers_verify_read(owner,&read)==XR_XIR_OK);
}

/* This independent literal request selects all four domains freshly through
 * the actual product selector/writer. No test-only reader or permission source
 * is introduced; external product gate and trace entry points are unchanged. */
static XrXirStatus conditional_generation_publication_stage(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,EffectInvocationPublication **output) {
    if (!certificate || !effect_body_material_shape(certificate->body) ||
        certificate->body->resources!=certificate->resources || certificate->resources!=work->resources || !certificate->solution ||
        !output || *output || !certificate->facts || !certificate->witnesses ||
        certificate->solution->count!=effect_body_count(certificate->body)) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationPublication *publication=NULL;XrXirStatus status=XR_XIR_OK;
    for (uint32_t f=0;f<certificate->solution->count && status==XR_XIR_OK;++f) {
        EffectInvocationProjectionSelection selected[4];
        status=effect_invocation_projection_domains(work,certificate,f,0,4,selected);
        if (status==XR_XIR_OK)
            status=effect_invocation_publication_write(work,certificate,f,selected,&publication);
    }
    if (status!=XR_XIR_OK) { effect_invocation_publication_free(publication);return status; }
    *output=publication;return XR_XIR_OK;
}

/* A poisoned reserved tail is not an admitted equation. Every real node
 * must be completely initialized before its ordinal enters count or queue. */
static void conditional_generation_reserved_controls(const XrXirCompileContext *context,
    EffectInvocationOwner *owner) {
    CHECK(owner->local_roots==owner->material->local_roots &&
        owner->local_witnesses==owner->material->local_witnesses && owner->count<owner->capacity);
    size_t bytes=(size_t)(owner->capacity-owner->count)*sizeof(*owner->nodes);
    memset(owner->nodes+owner->count,0xA5,bytes);
    uint32_t count=owner->count,pending=owner->pending;size_t attempts=rp_attempts;
    XrCompileResourceStats before=rp_stats(context);bool changed=false;
    CHECK(effect_invocation_capacity(owner)==XR_XIR_OK);
    CHECK(effect_invocation_enqueue(owner,count)==XR_XIR_BAD_STRUCTURE);
    CHECK(effect_invocation_expand(owner,count,&changed)==XR_XIR_BAD_STRUCTURE && !changed);
    XrXirRootEffectWitness witness={XR_XIR_ROOT_CAUSE_INITIALIZER,UINT32_MAX,UINT32_MAX,UINT32_MAX,0};
    CHECK(effect_invocation_local(owner,count,0,witness)==XR_XIR_BAD_STRUCTURE);
    CHECK(owner->count==count && owner->pending==pending && rp_attempts==attempts &&
        rp_stats(context).work==before.work && rp_stats(context).live_bytes==before.live_bytes);
    const unsigned char *tail=(const unsigned char *)(owner->nodes+count);
    for (size_t b=0;b<bytes;++b) CHECK(tail[b]==0xA5);
}

/* These constructor outputs remain private until the complete request succeeds.
 * Refused occupied outputs cannot change either owner or spend another fee. */
static void conditional_generation_publication_controls(const XrXirCompileContext *context,
    EffectInvocationCertificate *certificate) {
    EffectInvocationOwner *owner=certificate->equations;
    CHECK(owner->local_roots==certificate->body->local_roots &&
        owner->local_witnesses==certificate->body->local_witnesses);
    size_t attempts=rp_attempts;XrCompileResourceStats before=rp_stats(context);
    XrXirRootEffects *facts=certificate->facts;XrXirRootEffectWitness *witnesses=certificate->witnesses;
    uint32_t *atoms=certificate->projection_atoms;
    CHECK(effect_invocation_certificate_project(context,certificate,NULL)==XR_XIR_BAD_STRUCTURE &&
        certificate->facts==facts && certificate->witnesses==witnesses && certificate->projection_atoms==atoms);
    EffectInvocationPublication occupied={0};EffectInvocationPublication *output=&occupied;
    CHECK(conditional_generation_publication_stage(context,certificate,&output)==XR_XIR_BAD_STRUCTURE &&
        output==&occupied && !occupied.formulas && !occupied.witnesses && !occupied.count &&
        rp_attempts==attempts && rp_stats(context).work==before.work &&
        rp_stats(context).live_bytes==before.live_bytes);
    RootParameterMark physical=rp_mark();uint64_t live=rp_stats(context).live_bytes;
    output=NULL;CHECK(conditional_generation_publication_stage(context,certificate,&output)==XR_XIR_OK);
    CHECK(output && output->count==certificate->solution->count);
    for (uint32_t f=0;f<output->count;++f) for (uint32_t domain=0;domain<4;++domain) {
        XrXirRootEffectWitness expected={0};bool symbolic=false;
        CHECK(effect_invocation_public_selected(context,certificate,f,domain,&expected,&symbolic)==XR_XIR_OK);
        CHECK(effect_invocation_witness_equal(&expected,&output->witnesses[(size_t)domain*output->count+f]));
    }
    uint32_t mask=UINT32_C(0x87654321);
    CHECK(effect_invocation_certificate_node_mask(context,certificate,owner->count,&mask)==
        XR_XIR_BAD_STRUCTURE && mask==UINT32_C(0x87654321));
    effect_invocation_publication_free(output);
    CHECK(rp_stats(context).live_bytes==live);rp_balanced(physical);
}

/* A receiver refuses or consumes only the current fresh four-domain values.
 * This wrapper counts literal callbacks; it does not carry a permission fact. */
static uint32_t conditional_generation_sink_calls,conditional_generation_sink_refuse;
static XrXirStatus conditional_generation_counted_publication(const XrXirCompileContext *context,
    const EffectInvocationCertificate *certificate,uint32_t function,
    const EffectInvocationProjectionSelection *selected,void *receiver) {
    ++conditional_generation_sink_calls;
    if (function==conditional_generation_sink_refuse) return XR_XIR_BAD_STRUCTURE;
    return effect_invocation_publication_write(context,certificate,function,selected,receiver);
}

static void conditional_generation_sink_boundary(const XrXirCompileContext *context,
    EffectInvocationCertificate *certificate) {
    RootParameterMark physical=rp_mark();XrCompileResourceStats before=rp_stats(context);
    size_t attempts=rp_attempts;EffectInvocationPublication *publication=NULL;
    EffectInvocationCertificate probe={.resources=certificate->resources,.body=certificate->body,
        .origins=certificate->origins,.declarations=certificate->declarations,.parameter_kinds=certificate->parameter_kinds,
        .declared=certificate->declared,.equations=certificate->equations,.solution=certificate->solution};
    probe.terms.remaining=context;
    EffectInvocationProjectionSink sink={context->resources,effect_body_count(certificate->body),
        &publication,conditional_generation_counted_publication};
    conditional_generation_sink_calls=0;conditional_generation_sink_refuse=1;
    ++sink.count;
    CHECK(effect_invocation_certificate_project(context,&probe,&sink)==XR_XIR_BAD_STRUCTURE);
    --sink.count;sink.resources=NULL;
    CHECK(effect_invocation_certificate_project(context,&probe,&sink)==XR_XIR_BAD_STRUCTURE);
    sink.resources=context->resources;sink.owner=NULL;
    CHECK(effect_invocation_certificate_project(context,&probe,&sink)==XR_XIR_BAD_STRUCTURE);
    sink.owner=&publication;sink.write=NULL;
    CHECK(effect_invocation_certificate_project(context,&probe,&sink)==XR_XIR_BAD_STRUCTURE);
    CHECK(!probe.facts && !probe.witnesses && !probe.projection_atoms && !publication &&
        !conditional_generation_sink_calls && rp_attempts==attempts && rp_stats(context).work==before.work);
    sink.write=conditional_generation_counted_publication;
    CHECK(effect_invocation_certificate_project(context,&probe,&sink)==XR_XIR_BAD_STRUCTURE &&
        conditional_generation_sink_calls==2 && publication && probe.facts && probe.witnesses);
    CHECK(certificate->facts && certificate->witnesses && certificate->projection_atoms);
    effect_invocation_publication_free(publication);effect_terms_free(&probe.terms);
    CHECK(rp_stats(context).live_bytes==before.live_bytes);rp_balanced(physical);
    conditional_generation_sink_refuse=UINT32_MAX;
}

static void conditional_generation_paired_matches(const XrXirCompileContext *context,
    const EffectInvocationCertificate *certificate,const EffectInvocationPublication *publication) {
    CHECK(publication && publication->resources==context->resources &&
        publication->count==certificate->solution->count);
    for (uint32_t f=0;f<publication->count;++f) {
        const XrXirRootFormula *expected=&certificate->solution->formulas[f];
        const XrXirRootFormula *actual=&publication->formulas[f];
        CHECK(actual->constant_mask==expected->constant_mask && actual->term_count==expected->term_count);
        for (uint32_t t=0;t<actual->term_count;++t)
            CHECK(actual->terms[t].kind==expected->terms[t].kind && actual->terms[t].index==expected->terms[t].index);
        for (uint32_t domain=0;domain<4;++domain) {
            XrXirRootEffectWitness witness={0};bool symbolic=false;
            CHECK(effect_invocation_public_selected(context,certificate,f,domain,&witness,&symbolic)==XR_XIR_OK &&
                effect_invocation_witness_equal(&witness,&publication->witnesses[(size_t)domain*publication->count+f]));
        }
    }
}

static XrXirStatus conditional_generation_case(const XrXirCompileContext *context,uint32_t mode,bool oracle) {
    if (mode>=3) return XR_XIR_BAD_STRUCTURE;
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,mode==1);
    XrXirInstruction entry[6]={0};conditional_generation_alias_fixture(&fixture,entry,mode==1);
    XrXirArtifact *checked=NULL;XrXirEffects effects={0};EffectGraph graph={0};
    EffectInvocationOwner *owner=NULL;EffectInvocationSolution *solution=NULL;
    EffectInvocationCertificate *certificate=NULL;EffectBodyMaterial *material=NULL;
    EffectInvocationPublication *publication=NULL;
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
    if (status==XR_XIR_OK && oracle) conditional_generation_reserved_controls(context,owner);
    if (status==XR_XIR_OK && oracle) status=conditional_generation_decoded_controls(context,owner);
    if (status==XR_XIR_OK) status=effect_invocations_solve(owner);
    if (status==XR_XIR_OK && oracle) conditional_generation_projected_controls(context,owner);
    if (status==XR_XIR_OK) status=effect_invocation_stage(owner,&solution);
    EffectInvocationSealRequest request={&owner,&solution,NULL};
    EffectInvocationProjectionSink sink;
    if (status==XR_XIR_OK && !xir_compile_work(context,sizeof(sink))) status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK) {
        sink=(EffectInvocationProjectionSink){context->resources,source->function_count,
            &publication,effect_invocation_publication_write};
        status=effect_invocation_seal(context,&request,&certificate,&sink);
    }
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
        if (status==XR_XIR_OK && oracle) {
            conditional_generation_publication_controls(context,certificate);
            conditional_generation_sink_boundary(context,certificate);
            conditional_generation_paired_matches(context,certificate,publication);
        }
        uint32_t mask=UINT32_MAX;
        if (status==XR_XIR_OK) status=effect_invocation_certificate_node_mask(context,certificate,
            certificate->equations->roots[0],&mask);
        if (status==XR_XIR_OK && oracle) CHECK(mask==(mode==1?XR_XIR_CALLABLE_ROOT_REQUIRED:0));
    }
    effect_invocation_publication_free(publication);effect_invocation_certificate_free(certificate);return status;
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
