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

static uint32_t conditional_generation_head_reads;
static bool conditional_generation_head_refused;
static XrXirStatus conditional_generation_counted_callee_head(const EffectBodyRequest *request,
    uint32_t function,EffectBodyFunctionHead *output,EffectBodyFunctionDimensions *dimensions,EffectBodyFunctionSignature *signature) {
    ++conditional_generation_head_reads;
    if (conditional_generation_head_refused) return XR_XIR_BAD_STRUCTURE;
    return effect_body_owned_head(request,function,output,dimensions,signature);
}

static XrXirStatus conditional_generation_decoded_controls(const XrXirCompileContext *context,
    EffectInvocationWorkspace *owner) {
    EffectInvocationFlow flow={.owner=owner,.read=&owner->read,.node=owner->final.roots[0],.instruction=UINT32_MAX};
    XrXirStatus status=effect_invocation_discovery_origins(&flow);
    if (status!=XR_XIR_OK) { xr_compile_resources_free(flow.memory);return status; }
    uint32_t body=owner->final.nodes[flow.node].body,call=UINT32_MAX,reference=UINT32_MAX;
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
            CHECK(flow.body.head==effect_body_owned_head);
            flow.body.head=conditional_generation_counted_callee_head;
            conditional_generation_head_reads=0;conditional_generation_head_refused=false;
            bool closed=false;EffectBodyFunctionHead callee;
            CHECK(effect_invocation_closed_arguments(&flow,(uint32_t)op->immediate,op,&closed,&callee)==XR_XIR_OK &&
                closed && conditional_generation_instruction_reads==1 && conditional_generation_head_reads==1);
            CHECK(xir_compile_work(context,sizeof(EffectInvocationActual)));
            EffectInvocationActual actual={(uint32_t)op->immediate,UINT32_MAX,NULL};
            uint64_t *current=NULL;
            CHECK(effect_invocation_arguments_read(&flow,op,&actual,&callee,&current)==XR_XIR_OK &&
                current && conditional_generation_head_reads==1);
            size_t current_attempts=rp_attempts;XrCompileResourceStats current_before=rp_stats(context);
            uint64_t *occupied=current;
            CHECK(effect_invocation_arguments_read(&flow,op,&actual,&callee,&occupied)==XR_XIR_BAD_STRUCTURE &&
                occupied==current && rp_attempts==current_attempts && rp_stats(context).work==current_before.work);
            xr_compile_resources_free(current);current=NULL;
            conditional_generation_head_refused=true;current_before=rp_stats(context);
            CHECK(effect_invocation_arguments(&flow,op,(uint32_t)op->immediate,UINT32_MAX,NULL,&current)==
                XR_XIR_BAD_STRUCTURE && !current && rp_attempts==current_attempts &&
                rp_stats(context).work==current_before.work && conditional_generation_head_reads==2);
            conditional_generation_head_refused=false;
            XrXirInstruction malformed=*op;malformed.args[1]++;
            closed=true;size_t attempts=rp_attempts;
            CHECK(effect_invocation_closed_arguments(&flow,(uint32_t)op->immediate,&malformed,&closed,&callee)==
                XR_XIR_BAD_STRUCTURE && closed && rp_attempts==attempts &&
                conditional_generation_instruction_reads==1 && conditional_generation_head_reads==3);
            flow.body.head=effect_body_owned_head;
            bool changed=false;uint32_t edges=owner->final.edge_count;
            attempts=rp_attempts;XrCompileResourceStats before=rp_stats(context);
            CHECK(effect_invocation_instruction_decoded(&flow,call,NULL,&changed)==XR_XIR_BAD_STRUCTURE &&
                !changed && owner->final.edge_count==edges && rp_attempts==attempts &&
                rp_stats(context).work==before.work);
            conditional_generation_instruction_refused=true;before=rp_stats(context);
            CHECK(effect_invocation_instruction(&flow,call,&changed)==XR_XIR_BAD_STRUCTURE &&
                !changed && owner->final.edge_count==edges && rp_attempts==attempts &&
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
    conditional_generation_dimension_heads,conditional_generation_signature_heads;
static bool conditional_generation_shape_refused,conditional_generation_dimensions_refused;
static XrXirStatus conditional_generation_counted_shape(const EffectBodyRequest *request,
    uint32_t function,uint32_t ordinal,EffectBodyInstructionShape *output) {
    ++conditional_generation_shape_reads;
    if (conditional_generation_shape_refused) return XR_XIR_BAD_STRUCTURE;
    return effect_body_owned_shape(request,function,ordinal,output);
}
static XrXirStatus conditional_generation_counted_head(const EffectBodyRequest *request,
    uint32_t function,EffectBodyFunctionHead *output,EffectBodyFunctionDimensions *dimensions,EffectBodyFunctionSignature *signature) {
    if (dimensions) {
        ++conditional_generation_dimension_heads;
        if (conditional_generation_dimensions_refused) return XR_XIR_BAD_STRUCTURE;
    } else if (signature) ++conditional_generation_signature_heads;
    else ++conditional_generation_full_heads;
    return effect_body_owned_head(request,function,output,dimensions,signature);
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
    EffectInvocationWorkspace *owner) {
    CHECK(owner->read.producer_enabled==(owner->final.site_count!=0) && !owner->pending);
    CHECK(owner->final.site_count==1);
    EffectBodyRequest read=effect_body_owned_request(context,owner->final.material);
    read.shape=conditional_generation_counted_shape;
    EffectInvocationFlow flow={.owner=owner,.read=&owner->read,.node=owner->final.roots[0],.instruction=UINT32_MAX};
    uint32_t body=owner->final.nodes[flow.node].body,expected=0;
    const EffectPhysicalFunction *physical=&owner->final.physical->functions[body];
    for (uint32_t r=0;r<physical->count;++r) {
        EffectPhysicalRecord record=physical->records[r];
        if (record.value>=physical->parameters && (record.mask&EFFECT_PHYSICAL_CALLABLE) &&
            !(record.mask&EFFECT_PHYSICAL_REFERENCE)) ++expected;
    }
    CHECK(expected && body==0);
    conditional_generation_shape_reads=0;conditional_generation_shape_refused=false;
    CHECK(effect_invocation_discovery_origins_read(&flow,&read)==XR_XIR_OK &&
        conditional_generation_shape_reads==expected);
    xr_compile_resources_free(flow.memory);flow.memory=NULL;
    conditional_generation_shape_refused=true;conditional_generation_shape_reads=0;
    CHECK(effect_invocation_discovery_origins_read(&flow,&read)==XR_XIR_BAD_STRUCTURE &&
        conditional_generation_shape_reads==1);
    xr_compile_resources_free(flow.memory);flow.memory=NULL;
    conditional_generation_shape_refused=false;
    read.head=conditional_generation_counted_head;
    uint32_t dimensions=owner->final.count;
    for (uint32_t n=0;n<owner->final.count;++n)
        if (owner->final.nodes[n].root!=owner->final.nodes[n].body) ++dimensions;
    conditional_generation_full_heads=conditional_generation_dimension_heads=conditional_generation_signature_heads=0;
    conditional_generation_dimensions_refused=false;
    XrXirStatus producer_status=effect_invocation_discovery_verify(owner,&read);
    /* Each literal has four declaration roots and three actual keys: the
     * latent REF, direct apply and executing callback. The parameterless init
     * and all actual keys use a foreign root. Only entry and both apply keys
     * have callable rows; their scalar RETURNs still read a full head.
     * The three atom-zero facts are REF, COPY and the apply callable formal.
     * All are checked in the frame, independently, and at their replayed put;
     * the REF seed additionally checks its site. Every site check reads one
     * signature and one dimension head. The callable actual is read twice,
     * once for its cause and once for its transfer. Origin replay reads each
     * full body and foreign-root dimensions; edge inputs read every target,
     * and nonlatent results read their target before the scalar early return.
     * REF authentication, two closure passes and seed capture inspection each
     * add a signature read, even with zero captures. */
    enum { declarations=4,actual_keys=3,keys=declarations+actual_keys,
        foreign_roots=1+actual_keys,callable_nodes=1+2,edges=1+2,executing_edges=2,
        producer_facts=1+1+1,reference_seeds=1,replayed_puts=producer_facts,
        actual_source_reads=1+1,reference_closure_passes=2 };
    uint32_t site_reads=2*producer_facts+reference_seeds+replayed_puts;
    uint32_t full_reads=keys+callable_nodes+edges+executing_edges+actual_source_reads;
    uint32_t dimension_reads=keys+2*foreign_roots+site_reads+actual_source_reads;
    uint32_t signature_reads=site_reads+reference_seeds+reference_closure_passes+reference_seeds;
    fprintf(stderr,"generation real producer replay status%u full%u dimensions%u signatures%u\n",
        (unsigned)producer_status,conditional_generation_full_heads,
        conditional_generation_dimension_heads,conditional_generation_signature_heads);
    CHECK(read.domain.function_count==declarations && owner->final.count==keys &&
        dimensions==keys+foreign_roots);
    CHECK(producer_status==XR_XIR_OK && conditional_generation_full_heads==full_reads &&
        conditional_generation_dimension_heads==dimension_reads &&
        conditional_generation_signature_heads==signature_reads);
    size_t attempts=rp_attempts;uint32_t count=owner->final.count;
    conditional_generation_dimensions_refused=true;
    CHECK(effect_invocation_discovery_verify(owner,&read)==XR_XIR_BAD_STRUCTURE &&
        owner->final.count==count && !owner->pending && rp_attempts==attempts);
    conditional_generation_dimensions_refused=false;
    uint32_t root=owner->final.nodes[0].root;owner->final.nodes[0].root=UINT32_MAX;
    CHECK(effect_invocation_discovery_verify(owner,&read)==XR_XIR_BAD_STRUCTURE &&
        owner->final.count==count && !owner->pending && rp_attempts==attempts);
    owner->final.nodes[0].root=root;
    CHECK(effect_invocation_discovery_verify(owner,&read)==XR_XIR_OK);
}

/* This independent literal request selects all four domains freshly through
 * the actual product selector/writer. No test-only reader or permission source
 * is introduced; external product gate and trace entry points are unchanged. */
static XrXirStatus conditional_generation_publication_stage(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,EffectFinalFunctions **output) {
    if (!xir_compile_context_valid(work) || !certificate || !certificate->final ||
        !output || *output) return XR_XIR_BAD_STRUCTURE;
    EffectFinalFunctions *publication=NULL;
    XrXirStatus status=effect_invocation_result_read_new(work,certificate,&publication);
    EffectInvocationCertificate probe=*certificate;
    if (status!=XR_XIR_OK) return status;
    EffectFinalFunctions *staged=publication;EffectInvocationFinal final=*certificate->final;
    final.summary=*staged;publication=&final.summary;probe.final=&final;
    probe.terms=(EffectTerms){.remaining=work};
    EffectInvocationProjectionSink sink={work->resources,effect_body_count(certificate->body),
        &publication,effect_invocation_publication_write};
    if (status==XR_XIR_OK) status=effect_invocation_certificate_project(work,&probe,publication,&sink);
    if (status==XR_XIR_OK) status=effect_invocation_publication_match(work,&probe,publication);
    *staged=final.summary;publication=staged;
    effect_terms_free(&probe.terms);
    if (status!=XR_XIR_OK) { effect_invocation_result_free(publication);return status; }
    *output=publication;return XR_XIR_OK;
}

/* A poisoned reserved tail is not an admitted equation. Every real node
 * must be completely initialized before its ordinal enters count or queue. */
static void conditional_generation_reserved_controls(const XrXirCompileContext *context,
    EffectInvocationWorkspace *owner) {
    CHECK(owner->primitives && owner->primitive_count==effect_body_count(owner->final.material) &&
        owner->final.count<owner->final.capacity);
    EffectInvocationPrimitive saved=owner->primitives[0],actual={0};
    CHECK(effect_invocation_primitive_read(context,owner->final.material,0,&actual)==XR_XIR_OK);
    owner->primitives[0]=(EffectInvocationPrimitive){.facts={true,true}};
    EffectInvocationPrimitive current={0};
    CHECK(effect_invocation_primitive_read(context,owner->final.material,0,&current)==XR_XIR_OK &&
        current.facts.requires_root==actual.facts.requires_root && current.facts.unresolved==actual.facts.unresolved &&
        effect_invocation_witness_equal(&current.witnesses[0],&actual.witnesses[0]) &&
        effect_invocation_witness_equal(&current.witnesses[1],&actual.witnesses[1]));
    owner->primitives[0]=saved;
    invocation_primitive_allocation_controls(context,owner->final.material);
    size_t bytes=(size_t)(owner->final.capacity-owner->final.count)*sizeof(*owner->final.nodes);
    memset(owner->final.nodes+owner->final.count,0xA5,bytes);
    uint32_t count=owner->final.count,pending=owner->pending;size_t attempts=rp_attempts;
    XrCompileResourceStats before=rp_stats(context);bool changed=false;
    CHECK(effect_invocation_capacity(owner)==XR_XIR_OK);
    CHECK(effect_invocation_enqueue(owner,count)==XR_XIR_BAD_STRUCTURE);
    CHECK(effect_invocation_expand(owner,count,&changed)==XR_XIR_BAD_STRUCTURE && !changed);
    XrXirRootEffectWitness witness={XR_XIR_ROOT_CAUSE_INITIALIZER,UINT32_MAX,UINT32_MAX,UINT32_MAX,0};
    CHECK(effect_invocation_local(owner,count,0,witness)==XR_XIR_BAD_STRUCTURE);
    CHECK(owner->final.count==count && owner->pending==pending && rp_attempts==attempts &&
        rp_stats(context).work==before.work && rp_stats(context).live_bytes==before.live_bytes);
    const unsigned char *tail=(const unsigned char *)(owner->final.nodes+count);
    for (size_t b=0;b<bytes;++b) CHECK(tail[b]==0xA5);
}

/* These constructor outputs remain private until the complete request succeeds.
 * Refused occupied outputs cannot change either owner or spend another fee. */
static void conditional_generation_publication_controls(const XrXirCompileContext *context,
    EffectInvocationCertificate *certificate) {
    EffectInvocationFinal *owner=certificate->final;
    CHECK(owner->material==certificate->body && owner->physical==certificate->body->physical);
    size_t attempts=rp_attempts;XrCompileResourceStats before=rp_stats(context);
    XrXirRootEffectWitness *witnesses=certificate->final->summary.witnesses;
    CHECK(effect_invocation_certificate_project(context,certificate,NULL,NULL)==XR_XIR_BAD_STRUCTURE &&
        certificate->final->summary.witnesses==witnesses);
    EffectFinalFunctions occupied={0};EffectFinalFunctions *output=&occupied;
    CHECK(conditional_generation_publication_stage(context,certificate,&output)==XR_XIR_BAD_STRUCTURE &&
        output==&occupied && !occupied.functions && !occupied.witnesses && !occupied.count &&
        rp_attempts==attempts && rp_stats(context).work==before.work &&
        rp_stats(context).live_bytes==before.live_bytes);
    RootParameterMark physical=rp_mark();uint64_t live=rp_stats(context).live_bytes;
    output=NULL;CHECK(conditional_generation_publication_stage(context,certificate,&output)==XR_XIR_OK);
    CHECK(output && output->count==certificate->final->summary.count);
    for (uint32_t f=0;f<output->count;++f) for (uint32_t domain=0;domain<4;++domain) {
        XrXirRootEffectWitness expected={0};bool symbolic=false;
        uint32_t atom=UINT32_MAX;XrXirRootEffectWitness selected={0};
        CHECK(effect_invocation_projection_select(context,certificate,f,domain,&atom,&selected)==XR_XIR_OK);
        CHECK(xir_compile_work(context,sizeof(EffectInvocationProjectedCause)));
        const EffectInvocationProjectedCause projection={atom,&selected};
        CHECK(effect_invocation_public_transform(context,certificate,f,&projection,&expected,&symbolic)==XR_XIR_OK);
        CHECK(effect_invocation_witness_equal(&expected,&output->witnesses[(size_t)domain*output->count+f]));
    }
    uint32_t mask=UINT32_C(0x87654321);
    CHECK(effect_invocation_certificate_node_mask(context,certificate,owner->count,&mask)==
        XR_XIR_BAD_STRUCTURE && mask==UINT32_C(0x87654321));
    effect_invocation_result_free(output);
    CHECK(rp_stats(context).live_bytes==live);rp_balanced(physical);
}

/* A receiver refuses or consumes only the current fresh four-domain values.
 * This wrapper counts literal callbacks; it does not carry a permission fact. */
static uint32_t conditional_generation_sink_calls,conditional_generation_sink_refuse;
static XrXirStatus conditional_generation_counted_publication(const XrXirCompileContext *context,
    const EffectInvocationCertificate *certificate,uint32_t function,
    const uint32_t *selected,EffectFinalFunctions *result,void *receiver) {
    ++conditional_generation_sink_calls;
    if (function==conditional_generation_sink_refuse) return XR_XIR_BAD_STRUCTURE;
    return effect_invocation_publication_write(context,certificate,function,selected,result,receiver);
}

static void conditional_generation_sink_boundary(const XrXirCompileContext *context,
    EffectInvocationCertificate *certificate) {
    RootParameterMark physical=rp_mark();uint64_t entry_live=rp_stats(context).live_bytes;
    EffectFinalFunctions *publication=NULL;
    CHECK(effect_invocation_result_read_new(context,certificate,&publication)==XR_XIR_OK);
    XrCompileResourceStats before=rp_stats(context);size_t attempts=rp_attempts;
    EffectInvocationCertificate probe={.resources=certificate->resources,.body=certificate->body,
        .origins=certificate->origins,.declarations=certificate->declarations,
        .declared=certificate->declared};
    EffectFinalFunctions *staged=publication;EffectInvocationFinal final=*certificate->final;
    final.summary=*staged;publication=&final.summary;probe.final=&final;
    probe.terms.remaining=context;
    EffectInvocationProjectionSink sink={context->resources,effect_body_count(certificate->body),
        &publication,conditional_generation_counted_publication};
    conditional_generation_sink_calls=0;conditional_generation_sink_refuse=1;
    ++sink.count;
    CHECK(effect_invocation_certificate_project(context,&probe,publication,&sink)==XR_XIR_BAD_STRUCTURE);
    --sink.count;sink.resources=NULL;
    CHECK(effect_invocation_certificate_project(context,&probe,publication,&sink)==XR_XIR_BAD_STRUCTURE);
    sink.resources=context->resources;sink.owner=NULL;
    CHECK(effect_invocation_certificate_project(context,&probe,publication,&sink)==XR_XIR_BAD_STRUCTURE);
    sink.owner=&publication;sink.write=NULL;
    CHECK(effect_invocation_certificate_project(context,&probe,publication,&sink)==XR_XIR_BAD_STRUCTURE);
    CHECK(!publication->witnesses &&
        !conditional_generation_sink_calls && rp_attempts==attempts && rp_stats(context).work==before.work);
    sink.write=conditional_generation_counted_publication;
    CHECK(effect_invocation_certificate_project(context,&probe,publication,&sink)==XR_XIR_BAD_STRUCTURE &&
        conditional_generation_sink_calls==2 && publication->witnesses);
    CHECK(certificate->final->summary.witnesses);
    *staged=final.summary;publication=staged;
    effect_invocation_result_free(publication);effect_terms_free(&probe.terms);
    CHECK(rp_stats(context).live_bytes==entry_live);rp_balanced(physical);
    conditional_generation_sink_refuse=UINT32_MAX;
}

/* This is the actual post-death receiving owner, not an expected mirror.
 * Each mutation is refused by a fresh complete replay, then restored before
 * the real destructor follows its independently owned storage pointers. */
static void conditional_generation_result_rejections(const XrXirCompileContext *context,
    EffectInvocationCertificate *certificate) {
    EffectFinalFunctions *result=(&certificate->final->summary);
    CHECK(result && result->count && result->count==result->capacity &&
        effect_invocation_publication_match(context,certificate,result)==XR_XIR_OK);
    uint32_t count=result->count,total=result->term_count;
    result->count=count+1;
    CHECK(effect_invocation_publication_match(context,certificate,result)==XR_XIR_BAD_STRUCTURE);
    result->count=count;result->capacity=count+1;
    CHECK(effect_invocation_publication_match(context,certificate,result)==XR_XIR_BAD_STRUCTURE);
    result->capacity=count;result->term_count=total+1;
    CHECK(effect_invocation_publication_match(context,certificate,result)==XR_XIR_BAD_STRUCTURE);
    result->term_count=total;
    EffectFinalFunction *record=&result->functions[0];
    uint32_t capacity=record->capacity,mask=record->formula.constant_mask;
    record->capacity=capacity+1;
    CHECK(effect_invocation_publication_match(context,certificate,result)==XR_XIR_BAD_STRUCTURE);
    record->capacity=capacity;record->formula.constant_mask=mask^XR_XIR_CALLABLE_ROOT_REQUIRED;
    CHECK(effect_invocation_publication_match(context,certificate,result)==XR_XIR_BAD_STRUCTURE);
    record->formula.constant_mask=mask;
    for (uint32_t f=0;f<count;++f) {
        record=&result->functions[f];
        CHECK(record->formula.terms==record->storage && record->formula.term_count==record->capacity);
        if (!record->capacity) continue;
        const XrXirRootTerm *span=record->formula.terms;
        record->formula.terms=NULL;
        CHECK(effect_invocation_publication_match(context,certificate,result)==XR_XIR_BAD_STRUCTURE);
        record->formula.terms=span;
        XrXirRootTerm *storage=record->storage;record->storage=NULL;
        CHECK(effect_invocation_publication_match(context,certificate,result)==XR_XIR_BAD_STRUCTURE);
        record->storage=storage;
        uint32_t index=storage[0].index;storage[0].index^=UINT32_C(0x80000000);
        CHECK(effect_invocation_publication_match(context,certificate,result)==XR_XIR_BAD_STRUCTURE);
        storage[0].index=index;
    }
    XrXirRootEffectWitness unknown=result->witnesses[count];
    result->witnesses[count].cause=unknown.cause==XR_XIR_ROOT_CAUSE_NONE?XR_XIR_ROOT_CAUSE_CALL:XR_XIR_ROOT_CAUSE_NONE;
    CHECK(effect_invocation_publication_match(context,certificate,result)==XR_XIR_BAD_STRUCTURE);
    result->witnesses[count]=unknown;
    XrXirRootEffectWitness witness=result->witnesses[0];result->witnesses[0].distance^=1;
    CHECK(effect_invocation_publication_match(context,certificate,result)==XR_XIR_BAD_STRUCTURE);
    result->witnesses[0]=witness;
    RootParameterMark physical=rp_mark();XrXirCompileContext foreign=rp_owner(rp_caps());
    uint64_t live=rp_stats(&foreign).live_bytes,work=rp_stats(&foreign).work;
    CHECK(effect_invocation_publication_match(&foreign,certificate,result)==XR_XIR_BAD_STRUCTURE &&
        rp_stats(&foreign).live_bytes==live && rp_stats(&foreign).work==work);
    rp_owner_free(&foreign,live);rp_balanced(physical);
    CHECK(effect_invocation_publication_match(context,certificate,result)==XR_XIR_OK);
}

static void conditional_generation_paired_matches(const XrXirCompileContext *context,
    const EffectInvocationCertificate *certificate,const EffectFinalFunctions *publication) {
    CHECK(publication && publication->resources==context->resources &&
        publication->count==certificate->final->summary.count);
    for (uint32_t f=0;f<publication->count;++f) {
        const XrXirRootFormula *expected=&certificate->final->summary.functions[f].formula;
        const XrXirRootFormula *actual=&publication->functions[f].formula;
        CHECK(actual->constant_mask==expected->constant_mask && actual->term_count==expected->term_count);
        for (uint32_t t=0;t<actual->term_count;++t)
            CHECK(actual->terms[t].kind==expected->terms[t].kind && actual->terms[t].index==expected->terms[t].index);
        for (uint32_t domain=0;domain<4;++domain) {
            XrXirRootEffectWitness witness={0};bool symbolic=false;
            uint32_t atom=UINT32_MAX;XrXirRootEffectWitness selected={0};
            CHECK(effect_invocation_projection_select(context,certificate,f,domain,&atom,&selected)==XR_XIR_OK);
            CHECK(xir_compile_work(context,sizeof(EffectInvocationProjectedCause)));
            const EffectInvocationProjectedCause projection={atom,&selected};
            CHECK(effect_invocation_public_transform(context,certificate,f,&projection,&witness,&symbolic)==XR_XIR_OK &&
                effect_invocation_witness_equal(&witness,&publication->witnesses[(size_t)domain*publication->count+f]));
        }
    }
}

/* Raw correspondence belongs to explicit intake and external full matching.
 * The equation reader never follows a raw producer after receiving its input. */
static void conditional_generation_owned_input_controls(const XrXirCompileContext *context,
    const XrXirModule *source,XrXirEffects *effects,EffectGraph *graph) {
    EffectBodyMaterial *material=graph->material;EffectBodyRequest input={0};
    CHECK(effect_invocation_collect(context,source,effects,graph,material->declared,&input)==XR_XIR_OK);
    CHECK(input.owner==material && input.domain.types==material->domain.types);
    EffectBodyRequest occupied=input;size_t attempts=rp_attempts;
    CHECK(effect_invocation_collect(context,source,effects,graph,material->declared,&occupied)==XR_XIR_BAD_STRUCTURE &&
        occupied.owner==material && rp_attempts==attempts);
    EffectBodyRequest partial={.domain={.function_count=1}};
    CHECK(effect_invocation_collect(context,source,effects,graph,material->declared,&partial)==XR_XIR_BAD_STRUCTURE &&
        !partial.owner && partial.domain.function_count==1 && rp_attempts==attempts);
    EffectInvocationWorkspace *unchanged=NULL;EffectBodyRequest mixed=input;
    mixed.head=effect_body_raw_head;
    CHECK(effect_invocations_create(context,&mixed,effects,graph,material->declared,&unchanged)==XR_XIR_BAD_STRUCTURE &&
        !unchanged && rp_attempts==attempts);
    mixed=input;mixed.domain.function_count=source->function_count-1;
    CHECK(effect_invocations_create(context,&mixed,effects,graph,material->declared,&unchanged)==XR_XIR_BAD_STRUCTURE &&
        !unchanged && rp_attempts==attempts);
    XrXirInstruction *operations=(XrXirInstruction *)source->functions[0].instructions;
    XrXirOp opcode=operations[0].op;operations[0].op=XR_XIR_SUSPEND;
    CHECK(effect_invocation_current_input_match(context,source,material)==XR_XIR_BAD_STRUCTURE &&
        rp_attempts==attempts);
    XrXirInstruction storage;const XrXirInstruction *actual=NULL;
    CHECK(effect_body_request_instruction(&input,0,0,&storage,&actual)==XR_XIR_OK && actual->op==opcode);
    operations[0].op=opcode;
    int64_t immediate=operations[0].immediate;operations[0].immediate^=INT64_C(1);
    CHECK(effect_body_source_match(context,material,source)==XR_XIR_BAD_STRUCTURE);
    operations[0].immediate=immediate;
    CHECK(effect_body_source_match(context,material,source)==XR_XIR_OK);
    uint32_t function=0;
    while (function<material->physical->count && !material->physical->functions[function].count) ++function;
    CHECK(function<material->physical->count);
    EffectPhysicalFunction *physical=&material->physical->functions[function];
    uint32_t count=physical->count;physical->count=count-1;
    CHECK(effect_body_physical_match(context,material->physical,material)==XR_XIR_BAD_STRUCTURE);
    physical->count=count;
    uint32_t mask=physical->records[0].mask;physical->records[0].mask=0;
    CHECK(effect_body_physical_match(context,material->physical,material)==XR_XIR_BAD_STRUCTURE);
    physical->records[0].mask=mask;
    CHECK(effect_body_physical_match(context,material->physical,material)==XR_XIR_OK);
    RootParameterMark retained=rp_mark();XrXirCompileContext foreign=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&foreign).live_bytes;
    CHECK(effect_invocations_create(&foreign,&input,effects,graph,material->declared,&unchanged)==XR_XIR_BAD_STRUCTURE &&
        !unchanged);
    rp_owner_free(&foreign,baseline);rp_balanced(retained);
}

typedef struct ConditionalGenerationRawBoundary {
    const XrXirModule *source;
    EffectFinalFunctions **publication;
} ConditionalGenerationRawBoundary;

static XrXirStatus conditional_generation_raw_boundary_write(const XrXirCompileContext *context,
    const EffectInvocationCertificate *certificate,uint32_t function,const uint32_t *selected,
    EffectFinalFunctions *result,void *receiver) {
    ConditionalGenerationRawBoundary *boundary=receiver;
    XrXirStatus status=effect_invocation_publication_write(context,certificate,function,selected,
        result,boundary->publication);
    if (status==XR_XIR_OK && !function) {
        if (!xir_compile_work(context,sizeof(XrXirOp))) return XR_XIR_BUDGET;
        XrXirInstruction *operations=(XrXirInstruction *)boundary->source->functions[0].instructions;
        operations[0].op=XR_XIR_SUSPEND;
    }
    return status;
}

/* A callback may change its external raw producer without changing the
 * immutable receiving body. The explicit post-callback boundary must reject
 * that producer before a contract or public witness can be published. */
static void conditional_generation_raw_callback_controls(const XrXirCompileContext *context,
    const XrXirModule *source,const EffectInvocationCertificate *certificate) {
    uint64_t live=rp_stats(context).live_bytes;RootParameterMark retained=rp_mark();
    XrXirInstruction *operations=(XrXirInstruction *)source->functions[0].instructions;
    XrXirOp opcode=operations[0].op;EffectFinalFunctions *publication=NULL;
    CHECK(effect_invocation_result_read_new(context,certificate,&publication)==XR_XIR_OK);
    EffectInvocationCertificate probe=*certificate;
    EffectFinalFunctions *staged=publication;EffectInvocationFinal final=*certificate->final;
    final.summary=*staged;publication=&final.summary;probe.final=&final;
    probe.terms=(EffectTerms){.types=certificate->terms.types,.remaining=context};
    ConditionalGenerationRawBoundary boundary={source,&publication};
    EffectInvocationProjectionSink sink={context->resources,source->function_count,
        &boundary,conditional_generation_raw_boundary_write};
    CHECK(effect_invocation_current_input_match(context,source,certificate->body)==XR_XIR_OK);
    CHECK(effect_invocation_certificate_project(context,&probe,publication,&sink)==XR_XIR_OK);
    CHECK(operations[0].op==XR_XIR_SUSPEND &&
        effect_invocation_current_input_match(context,source,certificate->body)==XR_XIR_BAD_STRUCTURE);
    CHECK(effect_invocation_publication_match(context,certificate,(&certificate->final->summary))==XR_XIR_OK);
    operations[0].op=opcode;
    CHECK(effect_invocation_current_input_match(context,source,certificate->body)==XR_XIR_OK);
    *staged=final.summary;publication=staged;
    effect_invocation_result_free(publication);effect_terms_free(&probe.terms);
    CHECK(rp_stats(context).live_bytes==live);rp_balanced(retained);
}

static XrXirStatus conditional_generation_case(const XrXirCompileContext *context,uint32_t mode,bool oracle) {
    if (mode>=3) return XR_XIR_BAD_STRUCTURE;
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,mode==1);
    XrXirInstruction entry[6]={0};conditional_generation_alias_fixture(&fixture,entry,mode==1);
    XrXirArtifact *checked=NULL;XrXirEffects effects={0};EffectGraph graph={0};
    EffectInvocationWorkspace *owner=NULL;EffectFinalFunctions *solution=NULL;
    EffectInvocationCertificate *certificate=NULL;EffectBodyMaterial *material=NULL;
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,NULL);
    fprintf(stderr,"DIAG xir_invocation_early_generation_cases.h:8 status%u XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,NULL);\n",(unsigned)status);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(checked,NULL);
    fprintf(stderr,"DIAG xir_invocation_early_generation_cases.h:10 status%u if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(checked,NULL);\n",(unsigned)status);
    const XrXirModule *source=checked?xr_xir_compile_artifact_module(checked):NULL;
    if (status==XR_XIR_OK) status=invocation_core_storage(context,source,&effects);
    fprintf(stderr,"DIAG xir_invocation_early_generation_cases.h:13 status%u if (status==XR_XIR_OK) status=invocation_core_storage(context,source,&effects);\n",(unsigned)status);
    if (status==XR_XIR_OK) status=effect_parameters_derive(source,&effects,&graph,context);
    fprintf(stderr,"DIAG xir_invocation_early_generation_cases.h:15 status%u if (status==XR_XIR_OK) status=effect_parameters_derive(source,&effects,&graph,context);\n",(unsigned)status);
    if (status==XR_XIR_OK) material=graph.material;
    fprintf(stderr,"DIAG xir_invocation_early_generation_cases.h:17 status%u if (status==XR_XIR_OK) material=graph.material;\n",(unsigned)status);
    if (status==XR_XIR_OK && oracle) conditional_generation_control_boundary(context,source,&effects,&graph);
    fprintf(stderr,"DIAG xir_invocation_early_generation_cases.h:19 status%u if (status==XR_XIR_OK && oracle) conditional_generation_control_boundary(context,source,&effects,&graph);\n",(unsigned)status);
    if (status==XR_XIR_OK) status=effect_graph_build_material(source,&effects,&graph,(XrXirCompileContext *)context);
    fprintf(stderr,"DIAG xir_invocation_early_generation_cases.h:21 status%u if (status==XR_XIR_OK) status=effect_graph_build_material(source,&effects,&graph,(XrXirCompileContext *)context);\n",(unsigned)status);
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
    fprintf(stderr,"DIAG xir_invocation_early_generation_cases.h:33 status%u EffectBodyMaterial *later=NULL;status=effect_body_material_create(context,source,&later);\n",(unsigned)status);
        if (status==XR_XIR_OK) status=effect_body_source_match(context,later,source);
    fprintf(stderr,"DIAG xir_invocation_early_generation_cases.h:35 status%u if (status==XR_XIR_OK) status=effect_body_source_match(context,later,source);\n",(unsigned)status);
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
    if (status==XR_XIR_OK && oracle) conditional_generation_owned_input_controls(context,source,&effects,&graph);
    fprintf(stderr,"DIAG xir_invocation_early_generation_cases.h:47 status%u if (status==XR_XIR_OK && oracle) conditional_generation_owned_input_controls(context,source,&effects,&graph);\n",(unsigned)status);
    if (status==XR_XIR_OK) status=effect_invocations_receive(context,source,&effects,&graph,material->declared,&owner);
    fprintf(stderr,"DIAG xir_invocation_early_generation_cases.h:49 status%u if (status==XR_XIR_OK) status=effect_invocations_receive(context,source,&effects,&graph,material->declared,&owner);\n",(unsigned)status);
    if (status==XR_XIR_OK && oracle) conditional_generation_reserved_controls(context,owner);
    fprintf(stderr,"DIAG xir_invocation_early_generation_cases.h:51 status%u if (status==XR_XIR_OK && oracle) conditional_generation_reserved_controls(context,owner);\n",(unsigned)status);
    if (status==XR_XIR_OK && oracle) status=conditional_generation_decoded_controls(context,owner);
    fprintf(stderr,"DIAG xir_invocation_early_generation_cases.h:53 status%u if (status==XR_XIR_OK && oracle) status=conditional_generation_decoded_controls(context,owner);\n",(unsigned)status);
    if (status==XR_XIR_OK) status=effect_invocations_solve(owner);
    fprintf(stderr,"DIAG xir_invocation_early_generation_cases.h:55 status%u if (status==XR_XIR_OK) status=effect_invocations_solve(owner);\n",(unsigned)status);
    if (status==XR_XIR_OK && oracle) conditional_generation_projected_controls(context,owner);
    fprintf(stderr,"DIAG xir_invocation_early_generation_cases.h:57 status%u if (status==XR_XIR_OK && oracle) conditional_generation_projected_controls(context,owner);\n",(unsigned)status);
    if (status==XR_XIR_OK) status=effect_invocation_stage(owner,&solution);
    fprintf(stderr,"DIAG xir_invocation_early_generation_cases.h:59 status%u if (status==XR_XIR_OK) status=effect_invocation_stage(owner,&solution);\n",(unsigned)status);
    EffectInvocationSealRequest request={&owner,&solution,NULL};
    EffectInvocationProjectionSink sink;
    if (status==XR_XIR_OK && !xir_compile_work(context,sizeof(sink))) status=XR_XIR_BUDGET;
    fprintf(stderr,"DIAG xir_invocation_early_generation_cases.h:63 status%u if (status==XR_XIR_OK && !xir_compile_work(context,sizeof(sink))) status=XR_XIR_BUDGET;\n",(unsigned)status);
    if (status==XR_XIR_OK) {
        sink=(EffectInvocationProjectionSink){context->resources,source->function_count,
            &solution,effect_invocation_publication_write};
        status=effect_invocation_seal(context,&request,&certificate,&sink);
    fprintf(stderr,"DIAG xir_invocation_early_generation_cases.h:68 status%u status=effect_invocation_seal(context,&request,&certificate,&sink);\n",(unsigned)status);
    }
    if (status==XR_XIR_OK && oracle) CHECK(!owner && !solution && certificate->body==material &&
        certificate->declared==material->declared && !effects.generation &&
        certificate->final->material==material && certificate->final->cells==material->cells);
    if (status==XR_XIR_OK && oracle) conditional_generation_raw_callback_controls(context,source,certificate);
    fprintf(stderr,"DIAG xir_invocation_early_generation_cases.h:74 status%u if (status==XR_XIR_OK && oracle) conditional_generation_raw_callback_controls(context,source,certificate);\n",(unsigned)status);
    EffectFinalFunctions *publication=certificate?(&certificate->final->summary):NULL;
    effect_invocation_free(owner);effect_invocation_result_free(solution);
    effect_graph_free(&graph);invocation_core_storage_free(&effects);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    if (status==XR_XIR_OK) {
        EffectBodyRequest read=effect_body_owned_request(context,certificate->body);
        XrXirInstruction storage;const XrXirInstruction *operation=NULL;
        status=effect_body_request_instruction(&read,0,0,&storage,&operation);
    fprintf(stderr,"DIAG xir_invocation_early_generation_cases.h:83 status%u status=effect_body_request_instruction(&read,0,0,&storage,&operation);\n",(unsigned)status);
        if (status==XR_XIR_OK && oracle) CHECK(operation->op==(mode==1?XR_XIR_SLOT_LOAD:XR_XIR_CONST_INT) &&
            (mode==1 || operation->immediate==42));
        if (status==XR_XIR_OK) status=effect_body_physical_match(context,material->physical,material);
    fprintf(stderr,"DIAG xir_invocation_early_generation_cases.h:87 status%u if (status==XR_XIR_OK) status=effect_body_physical_match(context,material->physical,material);\n",(unsigned)status);
        EffectCellBodyAdapter adapter={0};XrXirCellProofBodyRead cells={0};
        if (status==XR_XIR_OK) status=effect_cell_body_owned(context,material,&adapter,&cells);
    fprintf(stderr,"DIAG xir_invocation_early_generation_cases.h:90 status%u if (status==XR_XIR_OK) status=effect_cell_body_owned(context,material,&adapter,&cells);\n",(unsigned)status);
        if (status==XR_XIR_OK) status=xr_xir_compile_cell_provenance_body_matches(context,material->cells,&cells,NULL);
    fprintf(stderr,"DIAG xir_invocation_early_generation_cases.h:92 status%u if (status==XR_XIR_OK) status=xr_xir_compile_cell_provenance_body_matches(context,material->cells,&cells,NULL);\n",(unsigned)status);
        if (status==XR_XIR_OK && oracle) {
            conditional_generation_publication_controls(context,certificate);
            conditional_generation_sink_boundary(context,certificate);
            conditional_generation_result_rejections(context,certificate);
            conditional_generation_paired_matches(context,certificate,publication);
        }
        uint32_t mask=UINT32_MAX;
        if (status==XR_XIR_OK) status=effect_invocation_certificate_node_mask(context,certificate,
            certificate->final->roots[0],&mask);
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
