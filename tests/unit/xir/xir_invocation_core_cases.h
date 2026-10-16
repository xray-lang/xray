/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_core_cases.h - Authentic owned invocation certificate probes
 */
static XrXirStatus invocation_core_storage(const XrXirCompileContext *context,
    const XrXirModule *module,XrXirEffects *effects) {
    *effects=(XrXirEffects){.resources=context->resources,.count=module->function_count};
    XrXirStatus status=XR_XIR_OK;uint32_t count=effects->count;
    effects->outcomes_work.functions=xir_compile_calloc(context,count,sizeof(*effects->outcomes_work.functions),&status);
    if (status==XR_XIR_OK) effects->outcomes_work.task_creation=xir_compile_calloc(context,count,sizeof(*effects->outcomes_work.task_creation),&status);
    if (status==XR_XIR_OK) effects->outcomes_work.root=xir_compile_calloc(context,count,sizeof(*effects->outcomes_work.root),&status);
    if (status==XR_XIR_OK) effects->outcomes_work.root_witnesses=xir_compile_calloc(context,count,sizeof(*effects->outcomes_work.root_witnesses),&status);
    if (status==XR_XIR_OK) effects->outcomes_work.unresolved_witnesses=xir_compile_calloc(context,count,sizeof(*effects->outcomes_work.unresolved_witnesses),&status);
    return status;
}

static void invocation_core_storage_free(XrXirEffects *effects) {
    effect_parameters_free(effects);
    xr_compile_resources_free(effects->outcomes_work.functions);xr_compile_resources_free(effects->outcomes_work.task_creation);
    xr_compile_resources_free(effects->outcomes_work.root);xr_compile_resources_free(effects->outcomes_work.root_witnesses);
    xr_compile_resources_free(effects->outcomes_work.unresolved_witnesses);*effects=(XrXirEffects){0};
}

/* Full construction and Artifact checking precede the same-owner equation
 * lifecycle. No pre-existing ROOT formula is solved or lowered by this probe.
 * The certificate remains private and cannot admit a Program or runtime call. */
static void *invocation_retired_blocks[3];
static uint32_t invocation_retired_counts[3];
static void invocation_retired_observe(void *memory) {
    for (uint32_t i=0;i<3;++i) if (memory==invocation_retired_blocks[i]) ++invocation_retired_counts[i];
}
/* Locate the payload only within extents recorded by our real allocator.
 * No allocation prefix or runtime metadata is guessed or inspected. */
static void *invocation_retired_block(const void *payload) {
    if (!payload) return NULL;
    uintptr_t address=(uintptr_t)payload;
    for (size_t i=0;i<rp_live;++i) {
        uintptr_t begin=(uintptr_t)rp_blocks[i].pointer;
        if (address>=begin && address-begin<rp_blocks[i].bytes) return rp_blocks[i].pointer;
    }
    CHECK(false);return NULL;
}
static XrXirStatus invocation_retired_seal(const EffectInvocationDriverInput *input,
    EffectInvocationCertificate **output,EffectFinalFunctions **receiving) {
    EffectInvocationWorkspace *workspace=NULL;
    XrXirStatus status=effect_invocations_create(input->work,input->body,input->effects,
        input->graph,input->declared,&workspace);
    if (status==XR_XIR_OK) status=effect_invocations_solve(workspace);
    if (status==XR_XIR_OK) status=effect_invocation_stage(workspace,receiving);
    if (status==XR_XIR_OK) {
        CHECK(!rp_release_observer);
        invocation_retired_blocks[0]=invocation_retired_block(workspace);
        invocation_retired_blocks[1]=invocation_retired_block(workspace->read.consumers);
        invocation_retired_blocks[2]=invocation_retired_block(workspace->edge_index);
        CHECK(workspace->states && invocation_retired_block(workspace->states)==invocation_retired_blocks[1]);
        for (uint32_t n=0;n<workspace->final.count;++n)
            CHECK(workspace->states[n].expanded && !workspace->states[n].queued);
        memset(invocation_retired_counts,0,sizeof(invocation_retired_counts));
        rp_release_observer=invocation_retired_observe;
        EffectInvocationSealRequest request={&workspace,receiving,input->dense};
        status=effect_invocation_seal(input->work,&request,output,NULL);
        rp_release_observer=NULL;
        if (status==XR_XIR_OK) {
            CHECK(!workspace && !*receiving && *output && (*output)->final);
            for (uint32_t i=0;i<3;++i)
                CHECK(invocation_retired_counts[i]==(invocation_retired_blocks[i]?1u:0u));
        } else {
            CHECK(workspace && !*output);
            for (uint32_t i=0;i<3;++i) CHECK(!invocation_retired_counts[i]);
        }
    }
    effect_invocation_free(workspace);return status;
}

/* The full high-order input remains unchanged. Erasing every receiving copy
 * and the stored local seed must not erase the real initializer declaration. */
static void invocation_final_initializer_erasure(const XrXirCompileContext *work,
    EffectInvocationCertificate *certificate) {
    const uint32_t function=3;
    EffectFinalFunctions *summary=&certificate->final->summary;
    EffectInvocationNode *node=&certificate->final->nodes[certificate->final->roots[function]];
    CHECK(node->cause_count==3 && summary->facts[function].requires_root);
    uint32_t intrinsic=node->intrinsic_mask,local=node->local_mask;
    EffectInvocationCause causes[6];memcpy(causes,node->causes,sizeof(causes));
    XrXirRootEffects facts=summary->facts[function],seed=certificate->body->local_roots[function];
    uint32_t constant=summary->functions[function].formula.constant_mask;
    XrXirRootEffectWitness witnesses[4],local_witnesses[2];
    for (uint32_t d=0;d<4;++d) {
        size_t at=(size_t)d*summary->count+function;witnesses[d]=summary->witnesses[at];
        summary->witnesses[at]=(XrXirRootEffectWitness){0};
    }
    for (uint32_t d=0;d<2;++d) {
        size_t at=(size_t)d*summary->count+function;local_witnesses[d]=certificate->body->local_witnesses[at];
        certificate->body->local_witnesses[at]=(XrXirRootEffectWitness){0};
    }
    node->intrinsic_mask=node->local_mask=0;
    for (uint32_t a=0;a<6;++a) node->causes[a]=(EffectInvocationCause){
        .next=UINT32_MAX,.edge=UINT32_MAX,.distance=UINT32_MAX};
    summary->facts[function]=(XrXirRootEffects){0};summary->functions[function].formula.constant_mask=0;
    certificate->body->local_roots[function]=(XrXirRootEffects){0};
    XrXirStatus status=effect_invocation_publication_match(work,certificate,summary);
    fprintf(stderr,"general Input initializer erasure status%u declaration%u\n",status,
        certificate->body->declarations->modules[0].initializer);
    CHECK(status==XR_XIR_BAD_STRUCTURE);
    node->intrinsic_mask=intrinsic;node->local_mask=local;memcpy(node->causes,causes,sizeof(causes));
    summary->facts[function]=facts;summary->functions[function].formula.constant_mask=constant;
    certificate->body->local_roots[function]=seed;
    for (uint32_t d=0;d<4;++d) summary->witnesses[(size_t)d*summary->count+function]=witnesses[d];
    for (uint32_t d=0;d<2;++d) certificate->body->local_witnesses[(size_t)d*summary->count+function]=local_witnesses[d];
    CHECK(effect_invocation_publication_match(work,certificate,summary)==XR_XIR_OK);
}

static EffectInvocationCertificate *invocation_trace_callback_target;
static uint32_t invocation_trace_callback_count;
static void invocation_trace_allocation_erase(void *memory) {
    CHECK(memory && invocation_trace_callback_target);
    rp_allocation_observer=NULL;++invocation_trace_callback_count;
    EffectInvocationCertificate *certificate=invocation_trace_callback_target;
    EffectFinalFunctions *summary=&certificate->final->summary;
    EffectInvocationNode *node=&certificate->final->nodes[certificate->final->roots[3]];
    node->intrinsic_mask=node->local_mask=0;
    for (uint32_t a=0;a<node->cause_count*2;++a) node->causes[a]=(EffectInvocationCause){
        .next=UINT32_MAX,.edge=UINT32_MAX,.distance=UINT32_MAX};
    summary->facts[3]=(XrXirRootEffects){0};summary->functions[3].formula.constant_mask=0;
    for (uint32_t d=0;d<4;++d) summary->witnesses[(size_t)d*summary->count+3]=(XrXirRootEffectWitness){0};
}

/* A real output allocation erases every receiving ROOT copy. Fresh length
 * proof must refuse before writing beyond the allocation's original extent. */
static void invocation_final_trace_callback(const XrXirCompileContext *work,
    EffectInvocationCertificate *certificate) {
    EffectFinalFunctions *summary=&certificate->final->summary;
    EffectInvocationNode *node=&certificate->final->nodes[certificate->final->roots[3]];
    EffectInvocationNode saved=*node;EffectInvocationCause causes[6];
    CHECK(node->cause_count==3);memcpy(causes,node->causes,sizeof(causes));
    XrXirRootEffects facts=summary->facts[3];uint32_t constant=summary->functions[3].formula.constant_mask;
    XrXirRootEffectWitness witnesses[4];
    for (uint32_t d=0;d<4;++d) witnesses[d]=summary->witnesses[(size_t)d*summary->count+3];
    RootParameterMark retained=rp_mark();XrCompileResourceStats before=rp_stats(work);
    invocation_trace_callback_target=certificate;invocation_trace_callback_count=0;
    rp_allocation_observer=invocation_trace_allocation_erase;XrXirRootCauseTrace *output=NULL;
    CHECK(effect_invocation_public_trace(work,certificate,3,&facts,&output)==XR_XIR_BAD_STRUCTURE &&
        !output && !rp_allocation_observer && invocation_trace_callback_count==1);
    invocation_trace_callback_target=NULL;rp_balanced(retained);
    XrCompileResourceStats paid=rp_stats(work);CHECK(paid.work>before.work && paid.allocated_bytes>before.allocated_bytes);
    *node=saved;memcpy(node->causes,causes,sizeof(causes));
    summary->facts[3]=facts;summary->functions[3].formula.constant_mask=constant;
    for (uint32_t d=0;d<4;++d) summary->witnesses[(size_t)d*summary->count+3]=witnesses[d];
    CHECK(effect_invocation_public_trace(work,certificate,3,&facts,&output)==XR_XIR_OK && output);
    CHECK(output->counts[0]==1 && !output->counts[1] && output->facts.requires_root);
    CHECK(rp_stats(work).allocated_bytes>paid.allocated_bytes && rp_stats(work).work>paid.work);
    xr_xir_compile_root_cause_trace_free(output);rp_balanced(retained);
}

#include "xir_invocation_nonroot_seed_cases.h"
static XrXirStatus conditional_invocation_core(const XrXirCompileContext *context,uint32_t mode,
    bool oracle,bool primitive_slot) {
    if (mode>=4) return XR_XIR_BAD_STRUCTURE;
    RootParameterBindingFixture fixture;rp_binding_fixture(&fixture,context,(mode&1)!=0);
    uint32_t self_operand=0;
    XrXirInstruction recursive[3]={{.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=1,.args={0,1}},
        {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=0},
        {.op=XR_XIR_RETURN,.args={2,0}}};
    XrXirBlock recursive_block={.count=3};
    XrXirEffectParameter recursive_parameter={XR_XIR_EFFECT_PARAMETER_VARIABLE,
        XR_XIR_EFFECT_USE_INVOKE|XR_XIR_EFFECT_USE_FORWARD};
    if (mode&2) {
        fixture.functions[1].instructions=recursive;fixture.functions[1].instruction_count=3;
        fixture.functions[1].blocks=&recursive_block;fixture.functions[1].operands=&self_operand;
        fixture.functions[1].operand_count=1;fixture.contracts[1].parameters=&recursive_parameter;
    }
    XrXirTypeNode types[3];XrXirInstruction slot_ops[4];XrXirBlock slot_block={0};
    XrXirSlot slot={.type=(XrXirType)258,.mutable=1};
    if (primitive_slot) {
        memcpy(types,fixture.nodes,sizeof(fixture.nodes));
        types[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64};
        fixture.types.nodes=types;fixture.types.count=3;
        uint32_t count=fixture.functions[1].instruction_count;
        memcpy(slot_ops,fixture.functions[1].instructions,(size_t)count*sizeof(*slot_ops));
        slot_ops[count]=slot_ops[count-1];
        slot_ops[count-1]=(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=(XrXirType)258};
        slot_block.count=count+1;fixture.functions[1].blocks=&slot_block;
        fixture.functions[1].instructions=slot_ops;fixture.functions[1].instruction_count=count+1;
        fixture.declarations.slots=&slot;fixture.declarations.slot_count=1;
        fixture.contracts[1].formula.constant_mask|=XR_XIR_CALLABLE_ROOT_REQUIRED;
        if (!(mode&1)) fixture.contracts[0].formula.constant_mask|=XR_XIR_CALLABLE_ROOT_REQUIRED;
    }
    XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,&diagnostic);
    uint32_t phase=0;
    if (status==XR_XIR_OK) { phase=1;status=xr_xir_compile_artifact_verify(checked,&diagnostic); }
    const XrXirModule *module=checked?xr_xir_compile_artifact_module(checked):NULL;
    XrXirEffects effects={0};EffectGraph graph={0};EffectInvocationDeclaredBounds *bounds=NULL;
    EffectFinalFunctions *receiving=NULL;
    EffectInvocationCertificate *certificate=NULL;
    if (status==XR_XIR_OK) { phase=2;status=invocation_core_storage(context,module,&effects); }
    if (status==XR_XIR_OK) { phase=3;status=effect_graph_build(module,&effects,&graph,(XrXirCompileContext *)context); }
    if (status==XR_XIR_OK) { phase=4;status=effect_parameters_derive(module,&effects,&graph,context); }
    if (status==XR_XIR_OK) { phase=5;status=effect_invocation_bounds_new(context,&bounds); }
    for (uint32_t f=0;status==XR_XIR_OK && f<module->function_count;++f)
        status=effect_invocation_bounds_capture(context,bounds,module->types,&module->functions[f],f,NULL);
    if (status==XR_XIR_OK) {
        phase=6;EffectBodyRequest owned_input={0};
        status=effect_invocation_collect(context,module,&effects,&graph,bounds,&owned_input);
        if (status==XR_XIR_OK) status=effect_invocation_current_input_match(context,module,graph.material);
        EffectInvocationDriverInput input={context,&owned_input,&effects,&graph,graph.material->declared,NULL};
        if (status==XR_XIR_OK) status=oracle?invocation_retired_seal(&input,&certificate,&receiving):
            effect_invocation_derive(&input,&certificate,NULL,&receiving);
    }
    if (status!=XR_XIR_OK && oracle) fprintf(stderr,
        "INVOCATION_CORE mode=%u phase=%u status=%u f=%u b=%u i=%u\n",mode,phase,status,
        diagnostic.function,diagnostic.block,diagnostic.instruction);
    effect_invocation_result_free(receiving);
    effect_graph_free(&graph);invocation_core_storage_free(&effects);
    effect_invocation_bounds_free(bounds);xr_xir_compile_artifact_free(checked);checked=NULL;
    memset(&fixture,0,sizeof(fixture));memset(recursive,0,sizeof(recursive));
    EffectInvocationCauseTrace *trace=NULL;
    XrXirRootCauseTrace *public_trace=NULL,*root_trace=NULL;
    if (status==XR_XIR_OK) {
        status=effect_invocation_closed_read(context,certificate->final);
        uint32_t caller_mask=UINT32_MAX,formal_mask=UINT32_MAX,edge_mask=UINT32_MAX;
        if (status==XR_XIR_OK) status=effect_invocation_certificate_node_mask(context,certificate,certificate->final->roots[0],&caller_mask);
        if (status==XR_XIR_OK) status=effect_invocation_certificate_node_mask(context,certificate,
            certificate->final->roots[1],&formal_mask);
        if (status==XR_XIR_OK) status=effect_invocation_certificate_edge_mask(context,certificate,0,2,1,&edge_mask);
        if (status==XR_XIR_OK) status=effect_invocation_certificate_trace(context,certificate,1,&trace);
        if (status==XR_XIR_OK) status=effect_invocation_public_trace(context,certificate,1,&certificate->final->summary.facts[1],&public_trace);
        if (status==XR_XIR_OK) status=effect_invocation_public_trace(context,certificate,3,&certificate->final->summary.facts[3],&root_trace);
        if (status==XR_XIR_OK && oracle) {
            if (primitive_slot) invocation_nonroot_slot_erasure(context,certificate);
            invocation_final_initializer_erasure(context,certificate);
            invocation_final_trace_callback(context,certificate);
            EffectInvocationFinal damaged=*certificate->final;
            damaged.physical=NULL;EffectInvocationRead refused={0};EffectInvocationScratch *replay_storage=NULL;size_t admission_attempts=rp_attempts;
            XrCompileResourceStats before=rp_stats(context);
            CHECK(effect_invocation_read_open(context,NULL,certificate->declared,&damaged,&replay_storage,&refused)==XR_XIR_BAD_STRUCTURE &&
                !refused.work && !refused.consumers && !refused.final && rp_attempts==admission_attempts &&
                rp_stats(context).work==before.work);
            refused.work=context;
            CHECK(effect_invocation_read_open(context,NULL,certificate->declared,certificate->final,&replay_storage,&refused)==XR_XIR_BAD_STRUCTURE &&
                refused.work==context && !refused.consumers && rp_attempts==admission_attempts &&
                rp_stats(context).work==before.work);
            CHECK(caller_mask==(primitive_slot && !(mode&1)?XR_XIR_CALLABLE_ROOT_REQUIRED:0) &&
                formal_mask==(XR_XIR_CALLABLE_ROOT_UNRESOLVED|(primitive_slot?XR_XIR_CALLABLE_ROOT_REQUIRED:0)) &&
                edge_mask==(primitive_slot?XR_XIR_CALLABLE_ROOT_REQUIRED:0));
            CHECK(certificate->final->summary.functions[1].formula.term_count==1 &&
                certificate->final->summary.functions[1].formula.terms[0].kind==XR_XIR_ROOT_TERM_PARAMETER &&
                certificate->final->summary.functions[1].formula.terms[0].index==0);
            CHECK(trace && trace->counts[0]==(primitive_slot?1u:0u) && trace->counts[1]==1 &&
                trace->steps[trace->counts[0]].function==1 &&
                trace->steps[trace->counts[0]].witness.cause==XR_XIR_ROOT_CAUSE_PARAMETER &&
                !trace->steps[trace->counts[0]].witness.distance && trace->steps[trace->counts[0]].witness.slot==0);
            CHECK(public_trace && public_trace->counts[0]==(primitive_slot?1u:0u) && public_trace->counts[1]==1 &&
                public_trace->facts.unresolved && effect_root_trace_storage(public_trace)[public_trace->counts[0]].cause==XR_XIR_ROOT_CAUSE_PARAMETER &&
                effect_root_trace_storage(public_trace)[public_trace->counts[0]].function==1 &&
                effect_root_trace_storage(public_trace)[public_trace->counts[0]].slot==0);
            if (primitive_slot) CHECK(trace->facts.requires_root && public_trace->facts.requires_root &&
                trace->steps[0].witness.cause==XR_XIR_ROOT_CAUSE_MUTABLE_SLOT && trace->steps[0].witness.slot==0 &&
                effect_root_trace_storage(public_trace)[0].cause==XR_XIR_ROOT_CAUSE_MUTABLE_SLOT);
            CHECK(root_trace && root_trace->counts[0]==1 && !root_trace->counts[1] && root_trace->facts.requires_root &&
                effect_root_trace_storage(root_trace)[0].function==3 &&
                effect_root_trace_storage(root_trace)[0].cause==XR_XIR_ROOT_CAUSE_INITIALIZER);
            size_t attempts=rp_attempts;EffectInvocationCertificate *occupied=certificate;
            EffectInvocationDriverInput empty={.work=context};
            CHECK(effect_invocation_derive(&empty,&occupied,NULL,&receiving)==XR_XIR_BAD_STRUCTURE &&
                occupied==certificate && rp_attempts==attempts);
            EffectInvocationCauseTrace *occupied_trace=trace;
            CHECK(effect_invocation_certificate_trace(context,certificate,1,&occupied_trace)==XR_XIR_BAD_STRUCTURE &&
                occupied_trace==trace && rp_attempts==attempts);
            uint32_t n=certificate->final->roots[0];
            certificate->final->nodes[n].intrinsic_mask|=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
            uint32_t untouched=UINT32_C(0x12345678);
            CHECK(effect_invocation_certificate_node_mask(context,certificate,n,&untouched)==XR_XIR_BAD_STRUCTURE &&
                untouched==UINT32_C(0x12345678));
            certificate->final->nodes[n].intrinsic_mask&=~XR_XIR_CALLABLE_ROOT_UNRESOLVED;
            RootParameterMark physical=rp_mark();XrXirCompileContext foreign=rp_owner(rp_caps());
            uint64_t baseline=rp_stats(&foreign).live_bytes;
            CHECK(effect_invocation_certificate_node_mask(&foreign,certificate,n,&untouched)==XR_XIR_BAD_STRUCTURE &&
                untouched==UINT32_C(0x12345678));
            rp_owner_free(&foreign,baseline);rp_balanced(physical);
        }
    }
    effect_invocation_certificate_free(certificate);
    if (status==XR_XIR_OK && oracle) CHECK(trace && trace->counts[1]==1 &&
        trace->facts.unresolved && trace->steps[trace->counts[0]].witness.cause==XR_XIR_ROOT_CAUSE_PARAMETER);
    xr_xir_compile_root_cause_trace_free(public_trace);xr_xir_compile_root_cause_trace_free(root_trace);
    effect_invocation_trace_free(trace);return status;
}
