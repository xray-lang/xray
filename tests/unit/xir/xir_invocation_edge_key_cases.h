/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_edge_key_cases.h - Exact edge keys and request-local constant rows
 */
#ifndef XIR_INVOCATION_EDGE_KEY_CASES_H
#define XIR_INVOCATION_EDGE_KEY_CASES_H

/* This storage-only literal has no permission owner or certificate. Five
 * independent field changes must remain distinct even in colliding buckets. */
static void invocation_edge_key_literals(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    const EffectInvocationEdgeKey keys[6]={{1,2,3,4,0},{2,2,3,4,0},{1,3,3,4,0},
        {1,2,4,4,0},{1,2,3,5,0},{1,2,3,4,1}};
    EffectInvocationEdge edges[6];EffectInvocationWorkspace owner={.read={.work=&context,.final=&owner.final},.final={.edges=edges}};
    CHECK(effect_invocation_edge_index_reserve(&owner)==XR_XIR_OK && owner.edge_index_capacity==16);
    for (uint32_t e=0;e<6;++e) {
        size_t at=SIZE_MAX;CHECK(effect_invocation_edge_slot(&owner,owner.edge_index,
            owner.edge_index_capacity,&keys[e],&at)==XR_XIR_OK && !owner.edge_index[at]);
        CHECK(xir_compile_work(&context,sizeof(*edges)+sizeof(uint32_t)));
        edges[e]=(EffectInvocationEdge){keys[e].caller,keys[e].target,keys[e].instruction,
            keys[e].producer,UINT32_MAX,UINT32_MAX,keys[e].latent!=0};
        owner.edge_index[at]=e+1;++owner.final.edge_count;
    }
    for (uint32_t e=0;e<6;++e) {
        size_t at=SIZE_MAX;CHECK(effect_invocation_edge_slot(&owner,owner.edge_index,
            owner.edge_index_capacity,&keys[e],&at)==XR_XIR_OK && owner.edge_index[at]==e+1);
    }
    uint32_t saved[16];memcpy(saved,owner.edge_index,sizeof(saved));
    for (uint32_t i=0;i<16;++i) owner.edge_index[i]=UINT32_MAX;
    size_t unchanged=SIZE_MAX;
    CHECK(effect_invocation_edge_slot(&owner,owner.edge_index,16,&keys[0],&unchanged)==
        XR_XIR_BAD_STRUCTURE && unchanged==SIZE_MAX);
    memcpy(owner.edge_index,saved,sizeof(saved));
    CHECK(effect_invocation_edge_slot(&owner,owner.edge_index,15,&keys[0],&unchanged)==
        XR_XIR_BAD_STRUCTURE && unchanged==SIZE_MAX);
    xr_compile_resources_free(owner.edge_index);rp_owner_free(&context,baseline);rp_balanced(physical);
}

/* Equal body coordinates deliberately share one chain. Every complete input
 * remains distinct, and malformed scratch cannot publish a numeric answer. */
static void invocation_node_key_literals(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    uint64_t inputs[6][2]={{1,3},{2,3},{1,4},{1,8},{1,3},{1,3}};
    EffectInvocationNode nodes[6]={0};EffectInvocationState states[6]={0};
    for (uint32_t n=0;n<6;++n) nodes[n]=(EffectInvocationNode){.root=n==4?2:1,
        .body=n==5?3:2,.parameter_count=1,.input=inputs[n]};
    EffectInvocationWorkspace owner={.read={.work=&context,.final=&owner.final,
        .body={.domain={.function_count=4}}},.states=states,
        .final={.nodes=nodes,.count=6,.capacity=6}};
    CHECK(effect_invocation_node_index_reserve(&owner)==XR_XIR_OK && owner.node_index_capacity==4);
    for (uint32_t n=0;n<6;++n) {
        EffectInvocationNodeLookup key={nodes[n].root,nodes[n].body,1,inputs[n],2};
        uint32_t actual=UINT32_MAX;
        CHECK(effect_invocation_node_find(&owner,&key,&actual)==XR_XIR_OK && actual==n);
    }
    uint64_t missing[2]={1,9};EffectInvocationNodeLookup key={1,2,1,missing,2};
    uint32_t actual=42;
    CHECK(effect_invocation_node_find(&owner,&key,&actual)==XR_XIR_OK && actual==UINT32_MAX);
    uint32_t saved=owner.node_index[2];owner.node_index[2]=UINT32_MAX;actual=42;
    CHECK(effect_invocation_node_find(&owner,&key,&actual)==XR_XIR_BAD_STRUCTURE && actual==42);
    owner.node_index[2]=saved;owner.node_index_capacity=3;
    CHECK(effect_invocation_node_find(&owner,&key,&actual)==XR_XIR_BAD_STRUCTURE && actual==42);
    owner.node_index_capacity=4;key.parameters=2;
    CHECK(effect_invocation_node_find(&owner,&key,&actual)==XR_XIR_BAD_STRUCTURE && actual==42);
    key.parameters=1;uint32_t held=states[saved-1].lookup_next;states[saved-1].lookup_next=saved;
    CHECK(effect_invocation_node_find(&owner,&key,&actual)==XR_XIR_BAD_STRUCTURE && actual==42);
    states[saved-1].lookup_next=held;nodes[saved-1].body=3;
    CHECK(effect_invocation_node_find(&owner,&key,&actual)==XR_XIR_BAD_STRUCTURE && actual==42);
    nodes[saved-1].body=2;actual=42;
    CHECK(effect_invocation_node_find(&owner,&key,&actual)==XR_XIR_OK && actual==UINT32_MAX);
    xr_compile_resources_free(owner.node_index);rp_owner_free(&context,baseline);rp_balanced(physical);
}

/* The branch selects two genuine FnRef producers of the same target. Their
 * PHI reaches one indirect instruction, so producer identity cannot be lost. */
typedef struct InvocationEdgePhiFixture {
    XrXirTypeNode callable;XrXirTypes types;XrXirType parameter;
    XrXirInstruction subject[8],pure;XrXirBlock blocks[4],one;
    uint32_t operands[4];XrXirFunction functions[3];
    XrXirSourceModule source;XrXirFunctionIdentity identities[3];
    XrXirDeclarations declarations;XrXirModule module;
} InvocationEdgePhiFixture;

static void invocation_edge_phi_fixture(InvocationEdgePhiFixture *f) {
    memset(f,0,sizeof(*f));
    f->callable=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_UNIT,
        .flags=XR_XIR_CALLABLE_ROOT_NONE};
    f->types=(XrXirTypes){.nodes=&f->callable,.count=1};f->parameter=XR_XIR_BOOL;
    f->subject[0]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,2}};
    f->subject[1]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.immediate=2};
    f->subject[2]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}};
    f->subject[3]=f->subject[1];f->subject[4]=f->subject[2];
    f->subject[5]=(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)256,.args={0,4}};
    f->subject[6]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.immediate=6};
    f->subject[7]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->operands[0]=1;f->operands[1]=2;f->operands[2]=2;f->operands[3]=4;
    f->blocks[0]=(XrXirBlock){.count=1};f->blocks[1]=(XrXirBlock){.first=1,.count=2};
    f->blocks[2]=(XrXirBlock){.first=3,.count=2};f->blocks[3]=(XrXirBlock){.first=5,.count=3};
    f->pure=f->subject[7];f->one=(XrXirBlock){.count=1};
    f->functions[0]=(XrXirFunction){.name="init",.name_length=4,.result=XR_XIR_UNIT,
        .blocks=&f->one,.block_count=1,.instructions=&f->pure,.instruction_count=1};
    f->functions[1]=(XrXirFunction){.name="subject",.name_length=7,.result=XR_XIR_UNIT,
        .parameters=&f->parameter,.parameter_count=1,.blocks=f->blocks,.block_count=4,
        .instructions=f->subject,.instruction_count=8,.operands=f->operands,.operand_count=4};
    f->functions[2]=f->functions[0];f->functions[2].name="pure";
    f->source=(XrXirSourceModule){.name="edges",.name_length=5,.initializer=0};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->module=(XrXirModule){.stage=XR_XIR_CHECKED,.types=&f->types,.functions=f->functions,
        .function_count=3,.declarations=&f->declarations,.linkage_kind=XR_XIR_LIBRARY};
}

static void invocation_edge_published_keys(const EffectInvocationFinal *owner,uint32_t mode) {
    CHECK(owner->material && owner->material->resources);
    uint32_t calls=0,latent=0,producers=0,target=UINT32_MAX;
    for (uint32_t e=0;e<owner->edge_count;++e) {
        const EffectInvocationEdge *edge=&owner->edges[e];
        for (uint32_t old=0;old<e;++old) {
            const EffectInvocationEdge *prior=&owner->edges[old];
            CHECK(edge->caller!=prior->caller || edge->target!=prior->target ||
                edge->instruction!=prior->instruction || edge->producer!=prior->producer ||
                edge->latent!=prior->latent);
        }
        if (owner->nodes[edge->caller].body!=1) continue;
        CHECK(owner->nodes[edge->target].body==2);
        if (target==UINT32_MAX) target=edge->target;else CHECK(target==edge->target);
        if (mode==2) {
            CHECK(edge->producer<2);
            CHECK(edge->latent?(owner->sites[edge->producer].function==1 &&
                edge->instruction==owner->sites[edge->producer].instruction):edge->instruction==6);
            producers|=UINT32_C(1)<<edge->producer;
        } else CHECK(edge->producer==UINT32_MAX && edge->instruction>=516 && edge->instruction<772 &&
            edge->latent==(mode==1));
        if (edge->latent) ++latent;else ++calls;
    }
    bool expected=mode==2?(calls==2 && latent==2 && producers==3):
        mode==1?(calls==0 && latent==256 && !producers):(calls==256 && !latent && !producers);
    if (!expected) {
        fprintf(stderr,"INVOCATION_EDGE_KEYS mode=%u calls=%u latent=%u producers=%u edges=%u nodes=%u sites=%u\n",
            mode,calls,latent,producers,owner->edge_count,owner->count,owner->site_count);
        for (uint32_t n=0;n<owner->count;++n)
            fprintf(stderr,"INVOCATION_EDGE_NODE mode=%u node=%u body=%u root=%u\n",
                mode,n,owner->nodes[n].body,owner->nodes[n].root);
    }
    CHECK(expected);
}

/* Retained numeric spans never become permission. The sealed current body
 * independently rejects both a smaller and a larger span after producer death. */
static void invocation_edge_dimension_controls(const XrXirCompileContext *context,
    XrXirEffects *effects) {
    const EffectInvocationCertificate *certificate=effects->invocations;
    EffectInvocationFinal *owner=certificate->final;uint32_t pure=UINT32_MAX;
    for (uint32_t n=0;n<owner->count;++n) {
        EffectInvocationNode *node=&owner->nodes[n];
        CHECK(node->instruction_count==certificate->body->functions[node->body].instruction_count);
        if (node->body==2) pure=n;
    }
    CHECK(pure<owner->count);EffectInvocationNode *node=&owner->nodes[pure];
    InvocationDirectRead delegated={context,effects,UINT32_MAX,false};
    XirEffectRuntimeRead read={.owner=&delegated,.work=invocation_direct_work};
    CHECK(effect_runtime_closed(certificate,pure,&read)==XR_XIR_OK);
    uint32_t held=node->instruction_count;CHECK(held && held<UINT32_MAX);
    node->instruction_count=held-1;
    CHECK(effect_runtime_closed(certificate,pure,&read)==XR_XIR_BAD_STRUCTURE);
    node->instruction_count=held+1;
    CHECK(effect_runtime_closed(certificate,pure,&read)==XR_XIR_BAD_STRUCTURE);
    node->instruction_count=held;CHECK(effect_runtime_closed(certificate,pure,&read)==XR_XIR_OK);
}

/* Both public admission and the infer remain genuine. Error summaries are
 * independently recomputed from owned input after every producer is dead. */
static XrXirStatus invocation_edge_operation(const XrXirCompileContext *context,
    uint32_t mode,bool oracle) {
    CHECK(mode<4);
    if (mode==3) return conditional_class_cell_case(context,0,oracle);
    CellIndexFixture cells;InvocationEdgePhiFixture phi;
    const XrXirModule *module=NULL;
    if (mode==2) { invocation_edge_phi_fixture(&phi);module=&phi.module; }
    else { ci_fixture(&cells,mode?CI_GO:CI_CALL,mode!=0,true);module=&cells.module; }
    XrXirEffects *effects=NULL;XrXirStatus status=xir_fixture_verify(context,module,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_infer_verified(context,module,&effects);
    if (status==XR_XIR_OK) {
        invocation_edge_published_keys(effects->invocations->final,mode);
        CHECK(effect_final_outcomes(effects)->words==1 && effect_final_outcomes(effects)->errors[1]==(mode==2?UINT64_C(0):mode?UINT64_C(4):UINT64_C(12)));
    }
    if (status==XR_XIR_OK && oracle) {
        CHECK(effect_body_source_match(context,effects->invocations->body,module)==XR_XIR_OK);
        XrXirInstruction *op=mode==2?&phi.subject[6]:&cells.subject[516];XrXirInstruction held=*op;
        for (uint32_t field=0;field<10;++field) {
            conditional_compact_change(op,field);
            CHECK(effect_body_source_match(context,effects->invocations->body,module)==XR_XIR_BAD_STRUCTURE);
            *op=held;CHECK(effect_body_source_match(context,effects->invocations->body,module)==XR_XIR_OK);
        }
    }
    memset(&cells,0xCE,sizeof(cells));memset(&phi,0xCE,sizeof(phi));
    if (status==XR_XIR_OK && oracle) invocation_edge_dimension_controls(context,effects);
    EffectTerms terms={0};terms.remaining=context;ErrorFlow flow={0};XrXirEffects replay={0};
    if (status==XR_XIR_OK) status=owned_error_replay_begin(context,effects,&replay);
    if (status==XR_XIR_OK) {
        flow.body=effect_body_owned_request(context,effects->invocations->body);
        terms.types=*flow.body.domain.types;flow.effects=&replay;
        flow.remaining=(XrXirCompileContext *)context;flow.terms=&terms;
        flow.summary_count=effects->count;flow.invocation=UINT32_MAX;
        status=error_invocation_contexts_new(&flow,&flow.invocations);
    }
    if (status==XR_XIR_OK && mode==2) {
        CHECK(flow.invocations);status=error_invocation_summaries(&flow);
        if (status==XR_XIR_OK) status=error_invocation_functions(&flow);
    } else if (status==XR_XIR_OK) {
        CHECK(!flow.invocations);status=error_function(&flow,1);
    }
    if (status==XR_XIR_OK) CHECK(replay.outcomes_work.errors[1]==
        (mode==2?UINT64_C(0):mode?UINT64_C(4):UINT64_C(12)));
    error_storage_free(&flow);error_invocation_contexts_free(flow.invocations);
    effect_terms_free(&terms);effect_legacy_error_oracle_free(&replay);xr_xir_compile_effects_free(effects);return status;
}

static void invocation_edge_poison_allocation(void *memory) {
    for(size_t at=0;at<rp_live;++at) {
        if(rp_blocks[at].pointer!=memory) continue;
        memset(memory,0xA7,rp_blocks[at].bytes);return;
    }
    CHECK(false);
}

static void invocation_edge_literals(void) {
    invocation_edge_key_literals();invocation_node_key_literals();
    for (uint32_t mode=0;mode<4;++mode) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        context.limits.instructions=CI_MAX_OPS;uint64_t baseline=rp_stats(&context).live_bytes;
        CHECK(invocation_edge_operation(&context,mode,true)==XR_XIR_OK);
        CHECK(!rp_allocation_observer);
        rp_allocation_observer=invocation_edge_poison_allocation;
        CHECK(invocation_edge_operation(&context,mode,true)==XR_XIR_OK);
        rp_allocation_observer=NULL;
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
}

static void invocation_edge_resources(void) {
    for (uint32_t mode=0;mode<4;++mode) {
        size_t sites=0;XrCompileResourceStats census={0},entry={0};
        for (size_t pass=0;pass<=sites;++pass) {
            RootParameterMark physical=rp_mark();rp_fail_at=SIZE_MAX;rp_injected=false;
            XrXirCompileContext context=rp_owner(rp_caps());context.limits.instructions=CI_MAX_OPS;
            entry=rp_stats(&context);RootParameterMark retained=rp_mark();size_t first=rp_attempts;
            rp_fail_at=pass?first+pass-1:SIZE_MAX;
            XrXirStatus status=invocation_edge_operation(&context,mode,false);
            if (!pass) {
                CHECK(status==XR_XIR_OK);sites=rp_attempts-first;census=rp_stats(&context);CHECK(sites);
                printf("INVOCATION_EDGE_CENSUS mode=%u sites=%zu work=%llu allocated=%llu peak=%llu node_bytes=%zu actual_bytes=%zu head_bytes=%zu\n",
                    mode,sites,(unsigned long long)(census.work-entry.work),
                    (unsigned long long)(census.allocated_bytes-entry.allocated_bytes),
                    (unsigned long long)census.peak_bytes,sizeof(EffectInvocationNode),
                    sizeof(EffectInvocationActual),sizeof(EffectBodyFunctionHead));
            } else {
                CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && rp_attempts-first==pass);
                CHECK(rp_stats(&context).live_bytes==entry.live_bytes);rp_balanced(retained);
                XrCompileResourceStats consumed=rp_stats(&context);
                uint64_t work=census.work-entry.work,bytes=census.allocated_bytes-entry.allocated_bytes;
                bool fits=work<=context.resources->limits.work-consumed.work &&
                    bytes<=context.resources->limits.allocated_bytes-consumed.allocated_bytes;
                rp_fail_at=SIZE_MAX;size_t retry=rp_attempts;
                status=invocation_edge_operation(&context,mode,false);
                CHECK(status==(fits?XR_XIR_OK:XR_XIR_BUDGET));
                CHECK(rp_stats(&context).live_bytes==entry.live_bytes);rp_balanced(retained);
                if (fits) CHECK(rp_stats(&context).work-consumed.work==work &&
                    rp_stats(&context).allocated_bytes-consumed.allocated_bytes==bytes &&
                    rp_stats(&context).allocation_count-consumed.allocation_count==census.allocation_count-entry.allocation_count &&
                    rp_attempts-retry==sites);
            }
            rp_fail_at=SIZE_MAX;rp_owner_free(&context,entry.live_bytes);rp_balanced(physical);
        }
        uint64_t measured[3]={census.allocated_bytes,census.peak_bytes,census.work};
        for (uint32_t axis=0;axis<3;++axis) for (uint32_t minus=0;minus<2;++minus) {
            XrCompileResourceLimits limits=rp_caps();uint64_t limit=measured[axis]-minus;
            if (axis==0) limits.allocated_bytes=limit;
            else if (axis==1) limits.live_bytes=limit;
            else limits.work=limit;
            RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(limits);
            context.limits.instructions=CI_MAX_OPS;uint64_t baseline=rp_stats(&context).live_bytes;
            CHECK(invocation_edge_operation(&context,mode,false)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
            rp_owner_free(&context,baseline);rp_balanced(physical);
        }
    }
}
#endif // XIR_INVOCATION_EDGE_KEY_CASES_H
