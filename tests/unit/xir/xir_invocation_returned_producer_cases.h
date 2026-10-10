/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_returned_producer_cases.h - Real Checked return and barrier controls
 */
typedef struct InvocationReturnedFixture {
    XrXirTypeNode nodes[2];XrXirTypes types;
    XrXirInstruction entry[4],factory[2],pure[2],relay[2],init;
    XrXirBlock entry_block,two,one;
    XrXirFunction functions[5];XrXirFunctionIdentity identities[5];
    XrXirSourceModule source;XrXirDeclarations declarations;XrXirModule module;
} InvocationReturnedFixture;

/* One explicit UNKNOWN boundary, one explicit WEAKEN boundary, and a
 * non-producing recursive factory remain distinct from genuine return data. */
static void invocation_returned_fixture(InvocationReturnedFixture *f,uint32_t mode) {
    memset(f,0,sizeof(*f));
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,
        .flags=XR_XIR_CALLABLE_ROOT_NONE};
    f->nodes[1]=f->nodes[0];f->nodes[1].flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    f->types=(XrXirTypes){f->nodes,2,NULL,NULL};
    XrXirType result=(XrXirType)(mode==2?257:256);
    f->entry[0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=result,.immediate=mode==1?3:1};
    f->entry[1]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64};
    f->entry[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
    f->entry_block=(XrXirBlock){.count=3};
    if (mode==3) {
        f->entry[1]=(XrXirInstruction){.op=XR_XIR_FUNCTION_WEAKEN,.type=result};
        f->entry[2]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=1};
        f->entry[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
        f->entry_block.count=4;
    }
    f->factory[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=result,.immediate=2};
    f->factory[1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->pure[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
    f->pure[1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->relay[0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=result,.immediate=1};
    f->relay[1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->two=(XrXirBlock){.count=2};f->one=(XrXirBlock){.count=1};
    if (mode==4) {
        f->factory[0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=result,.immediate=1};
        /* This unrelated genuine terminal cannot root the factory's SCC. */
        f->relay[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=result,.immediate=2};
    }
    f->init=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->functions[0]=(XrXirFunction){.name="entry",.name_length=5,.result=XR_XIR_I64,
        .instructions=f->entry,.instruction_count=f->entry_block.count,.blocks=&f->entry_block,.block_count=1};
    f->functions[1]=(XrXirFunction){.name="factory",.name_length=7,.result=result,
        .instructions=f->factory,.instruction_count=2,.blocks=&f->two,.block_count=1};
    f->functions[2]=(XrXirFunction){.name="pure",.name_length=4,.result=XR_XIR_I64,
        .instructions=f->pure,.instruction_count=2,.blocks=&f->two,.block_count=1};
    f->functions[3]=(XrXirFunction){.name="relay",.name_length=5,.result=result,
        .instructions=f->relay,.instruction_count=2,.blocks=&f->two,.block_count=1};
    f->functions[4]=(XrXirFunction){.name="init",.name_length=4,.result=XR_XIR_UNIT,
        .instructions=&f->init,.instruction_count=1,.blocks=&f->one,.block_count=1};
    f->source=(XrXirSourceModule){.name="returned",.name_length=8,.initializer=4};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,
        .functions=f->identities,.entry_function=0};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.types=&f->types,.functions=f->functions,.function_count=5,
        .declarations=&f->declarations,.linkage_kind=XR_XIR_PROGRAM};
}

static void invocation_returned_rejection_status(EffectInvocationOwner *owner,uint32_t mode,const char *mutation) {
    XrXirStatus status=effect_invocation_producers_verify(owner);
    if (status!=XR_XIR_BAD_STRUCTURE) {
        XrCompileResourceStats stats=rp_stats(owner->work);
        fprintf(stderr,"INVOCATION_RETURNED_REJECT mode=%u mutation=%s status=%u nodes=%u work=%llu allocated=%llu live=%llu\n",
            mode,mutation,status,owner->count,(unsigned long long)stats.work,
            (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.live_bytes);
    }
    CHECK(status==XR_XIR_BAD_STRUCTURE);
}

static void invocation_returned_rejections(EffectInvocationOwner *owner,uint32_t mode) {
    if (!owner->producer_enabled) return;
    uint32_t index=owner->roots[1];EffectInvocationNode *node=&owner->nodes[index];
    if (mode==4) {
        CHECK(!node->producer_count && !node->producer_capacity && !node->producers);
        uint32_t edge=UINT32_MAX;
        for (uint32_t e=node->edge_head;e!=UINT32_MAX;e=owner->edges[e].next)
            if (!owner->edges[e].latent && owner->edges[e].target==index && !owner->edges[e].instruction) edge=e;
        CHECK(edge!=UINT32_MAX && owner->site_count==1 && owner->sites[0].function==3);
        EffectInvocationProducer forged[2]={{0,0,index,2,edge,0,1},
            {2,0,index,0,UINT32_MAX,1,2}};
        node->producers=forged;node->producer_count=node->producer_capacity=2;
        CHECK(effect_invocation_producers_verify(owner)==XR_XIR_BAD_STRUCTURE);
        node->producers=NULL;node->producer_count=node->producer_capacity=0;
        CHECK(effect_invocation_producers_verify(owner)==XR_XIR_OK);return;
    }
    const EffectInvocationProducer *returned=effect_invocation_producer_find(node,2,0);
    CHECK(returned && returned->distance==1 && returned->instruction==1 && returned->next_node==index);
    EffectInvocationProducer *fact=&node->producers[returned-node->producers];
    EffectInvocationProducer original=*fact;
    fact->distance=0;
    invocation_returned_rejection_status(owner,mode,"distance");*fact=original;
    fact->next_value=2;
    invocation_returned_rejection_status(owner,mode,"next-value");*fact=original;
    fact->site=UINT32_MAX;
    invocation_returned_rejection_status(owner,mode,"site");*fact=original;
    CHECK(effect_invocation_producers_verify(owner)==XR_XIR_OK);
}

/* Public analysis and an independently requested private seal both use the
 * real Checked owner. The private returned data does not authorize execution. */
static XrXirStatus conditional_invocation_returned(const XrXirCompileContext *context,
    uint32_t mode,bool oracle) {
    if (mode>=5) return XR_XIR_BAD_STRUCTURE;
    InvocationReturnedFixture fixture;invocation_returned_fixture(&fixture,mode);
    XrXirArtifact *checked=NULL;XrXirEffects *published=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,&diagnostic);
    uint32_t phase=0;
    if (status==XR_XIR_OK) { phase=1;status=xr_xir_compile_artifact_verify(checked,&diagnostic); }
    if (status==XR_XIR_OK) { phase=2;status=xr_xir_compile_effects_analyze(checked,&published); }
    const XrXirModule *module=checked?xr_xir_compile_artifact_module(checked):NULL;
    XrXirEffects effects={0};EffectGraph graph={0};EffectInvocationDeclaredBounds *bounds=NULL;
    EffectInvocationOwner *owner=NULL;EffectInvocationSolution *solution=NULL;
    EffectInvocationCertificate *certificate=NULL;
    if (status==XR_XIR_OK) { phase=3;status=invocation_core_storage(context,module,&effects); }
    if (status==XR_XIR_OK) status=effect_graph_build(module,&effects,&graph,(XrXirCompileContext *)context);
    if (status==XR_XIR_OK) status=effect_parameters_derive(module,&effects,&graph,context);
    if (status==XR_XIR_OK) status=effect_invocation_bounds_new(context,&bounds);
    for (uint32_t f=0;status==XR_XIR_OK && f<module->function_count;++f)
        status=effect_invocation_bounds_capture(context,bounds,module->types,&module->functions[f],f,NULL);
    if (status==XR_XIR_OK) { phase=4;status=effect_invocations_create(context,module,&effects,&graph,bounds,&owner); }
    if (status==XR_XIR_OK) status=effect_invocations_solve(owner);
    if (status==XR_XIR_OK && oracle) invocation_returned_rejections(owner,mode);
    if (status==XR_XIR_OK) status=effect_invocation_stage(owner,&solution);
    if (status==XR_XIR_OK) {
        phase=5;EffectInvocationSealRequest request={&owner,&solution,NULL};
        status=effect_invocation_seal(context,&request,&certificate);
    }
    effect_invocation_solution_free(solution);effect_invocation_free(owner);
    effect_invocation_bounds_free(bounds);effect_graph_free(&graph);invocation_core_storage_free(&effects);
    xr_xir_compile_artifact_free(checked);checked=NULL;memset(&fixture,0,sizeof(fixture));
    if (status==XR_XIR_OK && oracle) {
        const EffectInvocationOwner *owned=certificate->equations;
        CHECK(!owned->work && !owned->module && !owned->graph && !owned->declared && !owned->effects);
        uint32_t mask=UINT32_MAX;
        CHECK(effect_invocation_certificate_node_mask(context,certificate,owned->roots[0],&mask)==XR_XIR_OK &&
            mask==(mode==2?XR_XIR_CALLABLE_ROOT_UNRESOLVED:0));
        CHECK(published->root[0].unresolved==(mode==2) && !published->root[0].requires_root);
        CHECK(certificate->bodies.function_count==5 && published->count==5);
        uint32_t callback=mode==3?2:1;bool found=false;
        const EffectInvocationNode *entry=&owned->nodes[owned->roots[0]];
        for (uint32_t e=entry->edge_head;e!=UINT32_MAX;e=owned->edges[e].next) {
            const EffectInvocationEdge *edge=&owned->edges[e];
            if (edge->instruction==callback && edge->producer!=UINT32_MAX && !edge->latent) {
                CHECK(owned->sites[edge->producer].target==2 && !owned->sites[edge->producer].captures);
                found=true;
            }
        }
        CHECK(found==(mode<2));
        if (mode<2) CHECK(effect_invocation_producer_find(entry,0,0));
        if (mode==4) CHECK(!effect_invocation_producer_find(&owned->nodes[owned->roots[1]],2,0));
        uint32_t original=UINT32_MAX;
        CHECK(effect_invocation_bounds_mask(context,certificate->declared,0,0,&original)==XR_XIR_OK &&
            original==(mode==2?XR_XIR_CALLABLE_ROOT_UNRESOLVED:0));
        const EffectInvocationOwner *public_owner=published->invocations->equations;
        if (mode<2) CHECK(effect_invocation_producer_find(&public_owner->nodes[public_owner->roots[0]],0,0));
    }
    xr_xir_compile_effects_free(published);effect_invocation_certificate_free(certificate);
    if (status!=XR_XIR_OK && oracle) fprintf(stderr,
        "INVOCATION_RETURNED mode=%u phase=%u status=%u f=%u b=%u i=%u\n",mode,phase,status,
        diagnostic.function,diagnostic.block,diagnostic.instruction);
    return status;
}
