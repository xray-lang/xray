/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_returned_producer_cases.h - Real Checked return and barrier controls
 */
typedef struct InvocationReturnedFixture {
    XrXirTypeNode nodes[3];XrXirTypes types;XrXirType capture_type;
    XrXirInstruction entry[4],factory[4],pure[4],relay[2],init;
    uint32_t capture;XrXirBlock entry_block,factory_block,pure_block,two,one;
    XrXirFunction functions[5];XrXirFunctionIdentity identities[5];
    XrXirSourceModule source;XrXirDeclarations declarations;XrXirModule module;
} InvocationReturnedFixture;

/* Capture operands belong to the factory's real frame. Its returned Function
 * retains the owned Cell rather than retaining a pointer into that frame. */
static void invocation_returned_capture_fixture(InvocationReturnedFixture *f,
    uint32_t mode,XrXirType result) {
    bool cell=mode>=6;f->capture_type=cell?(XrXirType)258:XR_XIR_I64;
    f->factory[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=40};
    f->factory[1]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=result,.immediate=2,.args={0,1}};
    f->factory[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
    f->capture=0;f->factory_block.count=3;
    f->pure[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=2};
    f->pure[1]=(XrXirInstruction){.op=XR_XIR_ADD_INT,.type=XR_XIR_I64,.args={0,1}};
    f->pure[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
    f->pure_block.count=3;
    if (!cell) return;
    f->factory[1]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=f->capture_type};
    f->factory[2]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=result,.immediate=2,.args={0,1}};
    f->factory[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
    f->factory_block.count=4;f->capture=1;
    f->pure[0]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=XR_XIR_I64};
    f->pure[1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
    f->pure[2]=(XrXirInstruction){.op=XR_XIR_CELL_WRITE,.args={0,2}};
    f->pure[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
    f->pure_block.count=4;
}

/* One explicit UNKNOWN boundary, one explicit WEAKEN boundary, and a
 * non-producing recursive factory remain distinct from genuine return data. */
static void invocation_returned_fixture(InvocationReturnedFixture *f,uint32_t mode) {
    memset(f,0,sizeof(*f));
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,
        .flags=XR_XIR_CALLABLE_ROOT_NONE};
    f->nodes[1]=f->nodes[0];f->nodes[1].flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64};
    f->types=(XrXirTypes){f->nodes,mode>=6?3:2,NULL,NULL};
    XrXirType result=(XrXirType)(mode==2?257:256);
    f->entry[0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=result,.immediate=(mode==1 || mode==7)?3:1};
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
    f->factory_block=f->pure_block=f->two;
    if (mode==4) {
        f->factory[0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=result,.immediate=1};
        /* This unrelated genuine terminal cannot root the factory's SCC. */
        f->relay[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=result,.immediate=2};
    }
    if (mode>=5) invocation_returned_capture_fixture(f,mode,result);
    f->init=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->functions[0]=(XrXirFunction){.name="entry",.name_length=5,.result=XR_XIR_I64,
        .instructions=f->entry,.instruction_count=f->entry_block.count,.blocks=&f->entry_block,.block_count=1};
    f->functions[1]=(XrXirFunction){.name="factory",.name_length=7,.result=result,
        .instructions=f->factory,.instruction_count=f->factory_block.count,
        .blocks=&f->factory_block,.block_count=1,.operands=mode>=5?&f->capture:NULL,
        .operand_count=mode>=5?1:0};
    f->functions[2]=(XrXirFunction){.name="pure",.name_length=4,.result=XR_XIR_I64,
        .parameters=mode>=5?&f->capture_type:NULL,.parameter_count=mode>=5?1:0,
        .instructions=f->pure,.instruction_count=f->pure_block.count,
        .blocks=&f->pure_block,.block_count=1};
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

/* A real Cell environment has its own rooted cause, independent of the
 * site's shortest identity cause. Missing KNOWN, invented ROOT/UNKNOWN and
 * duplicated atoms must fail the same complete producer replay. */
static void invocation_returned_environment_rejections(EffectInvocationOwner *owner,
    uint32_t index,uint32_t values) {
    EffectInvocationNode *node=&owner->nodes[index];EffectInvocationBasis basis={0};
    CHECK(effect_invocation_basis(owner,node->root,&basis)==XR_XIR_OK && owner->site_count==1);
    uint32_t known=1+owner->sites[0].binding*(basis.parameters+3)+basis.parameters+2;
    const EffectInvocationProducer *returned=effect_invocation_producer_find_atom(node,values,0,known);
    CHECK(returned && returned->distance==1 && returned->instruction==values-1);
    uint32_t at=(uint32_t)(returned-node->producers);
    EffectInvocationProducer original=node->producers[at];
    node->producers[at].atom=known-2;
    CHECK(effect_invocation_producers_verify(owner)==XR_XIR_BAD_STRUCTURE);node->producers[at]=original;
    node->producers[at].atom=known-1;
    CHECK(effect_invocation_producers_verify(owner)==XR_XIR_BAD_STRUCTURE);node->producers[at]=original;
    node->producers[at].atom=UINT32_MAX;
    CHECK(effect_invocation_producers_verify(owner)==XR_XIR_BAD_STRUCTURE);node->producers[at]=original;
    uint32_t last=node->producer_count-1;EffectInvocationProducer tail=node->producers[last];
    node->producers[at]=tail;--node->producer_count;
    CHECK(effect_invocation_producers_verify(owner)==XR_XIR_BAD_STRUCTURE);
    ++node->producer_count;node->producers[last]=tail;node->producers[at]=original;
    const EffectInvocationProducer *identity=effect_invocation_producer_find(node,values,0);
    CHECK(identity);uint32_t identity_at=(uint32_t)(identity-node->producers);
    EffectInvocationProducer identity_copy=node->producers[identity_at];
    node->producers[identity_at]=original;
    CHECK(effect_invocation_producers_verify(owner)==XR_XIR_BAD_STRUCTURE);
    node->producers[identity_at]=identity_copy;
    node->producers[at].next_value=values;node->producers[at].distance=2;
    CHECK(effect_invocation_producers_verify(owner)==XR_XIR_BAD_STRUCTURE);node->producers[at]=original;
    CHECK(effect_invocation_producers_verify(owner)==XR_XIR_OK);
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
        EffectInvocationProducer forged[2]={{0,0,index,2,edge,0,1,0},
            {2,0,index,0,UINT32_MAX,1,2,0}};
        node->producers=forged;node->producer_count=node->producer_capacity=2;
        CHECK(effect_invocation_producers_verify(owner)==XR_XIR_BAD_STRUCTURE);
        node->producers=NULL;node->producer_count=node->producer_capacity=0;
        CHECK(effect_invocation_producers_verify(owner)==XR_XIR_OK);return;
    }
    uint32_t values=owner->module->functions[1].instruction_count;
    const EffectInvocationProducer *returned=effect_invocation_producer_find(node,values,0);
    CHECK(returned && returned->distance==1 && returned->instruction==values-1 && returned->next_node==index);
    EffectInvocationProducer *fact=&node->producers[returned-node->producers];
    EffectInvocationProducer original=*fact;
    fact->distance=0;
    invocation_returned_rejection_status(owner,mode,"distance");*fact=original;
    fact->next_value=values;
    invocation_returned_rejection_status(owner,mode,"next-value");*fact=original;
    fact->site=UINT32_MAX;
    invocation_returned_rejection_status(owner,mode,"site");*fact=original;
    CHECK(effect_invocation_producers_verify(owner)==XR_XIR_OK);
    if (mode>=6) invocation_returned_environment_rejections(owner,index,values);
}

/* The producer and Checked Artifact have already died. Full certified
 * current-body keys still carry the Cell column and the real callback edge. */
static void invocation_returned_environment_owned(const XrXirCompileContext *context,
    const EffectInvocationCertificate *certificate,const XrXirEffects *published) {
    const EffectInvocationOwner *owner=certificate->equations;EffectInvocationBasis basis={0};
    uint32_t factory=owner->roots[1],entry=owner->roots[0];
    EffectInvocationOwner view=*owner;view.module=&certificate->bodies;view.work=context;
    CHECK(effect_invocation_basis(&view,owner->nodes[entry].root,&basis)==XR_XIR_OK);
    uint32_t known=1+owner->sites[0].binding*(basis.parameters+3)+basis.parameters+2;
    CHECK(effect_invocation_producer_find_atom(&owner->nodes[factory],4,0,known));
    CHECK(effect_invocation_producer_find_atom(&owner->nodes[entry],0,0,known));
    bool found=false;
    for (uint32_t e=owner->nodes[entry].edge_head;e!=UINT32_MAX;e=owner->edges[e].next) {
        const EffectInvocationEdge *edge=&owner->edges[e];
        if (edge->instruction!=1 || edge->producer!=0 || edge->latent) continue;
        const EffectInvocationNode *child=&owner->nodes[edge->target];
        CHECK(child->body==2 && child->parameter_count==1 && owner->sites[0].captures==1);
        CHECK(child->input[basis.fn_words+(basis.parameters+2)/64]==
            (UINT64_C(1)<<((basis.parameters+2)%64)));
        uint32_t mask=UINT32_MAX;
        CHECK(effect_invocation_certificate_node_mask(context,certificate,edge->target,&mask)==XR_XIR_OK && !mask);
        found=true;
    }
    CHECK(found);
    const EffectInvocationOwner *public_owner=published->invocations->equations;
    CHECK(effect_invocation_producer_find_atom(&public_owner->nodes[public_owner->roots[0]],0,0,known));
}

/* This literal is concrete raw XIR. Its real specialize fast path preserves
 * NULL provenance; the freshly derived certificate still owns every body,
 * physical type and declaration independently of the Lowered artifact. */
static XrXirStatus invocation_returned_lowered_snapshot(const XrXirCompileContext *context,
    const EffectInvocationCertificate *certificate,const XrXirModule *module) {
    if (!certificate || certificate->resources!=context->resources || !module ||
        module->stage!=XR_XIR_LOWERED || module->provenance || module->generics || module->defaults ||
        module->function_count!=5 || !module->types || !module->declarations || !module->functions)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirModule *owned=&certificate->bodies;
    if (owned->stage!=module->stage || owned->linkage_kind!=module->linkage_kind ||
        owned->function_count!=module->function_count || owned->provenance || owned->generics || owned->defaults ||
        owned->types!=&certificate->terms.types || owned->functions==module->functions ||
        owned->declarations!=certificate->declarations || !certificate->declarations ||
        owned->declarations==module->declarations || certificate->terms.remaining)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_lowered_snapshot_match(context,owned->types,module->types);
    const XrXirDeclarations *a=owned->declarations,*b=module->declarations;
    if (status!=XR_XIR_OK) return status;
    if (a->module_count!=1 || b->module_count!=1 || a->slot_count || b->slot_count ||
        a->literal_count || b->literal_count || a->implementations || b->implementations ||
        a->root_module!=b->root_module || a->entry_function!=b->entry_function ||
        !a->functions || !b->functions || !a->modules || !b->modules || a->functions==b->functions)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context,2*(sizeof(*a)+5*sizeof(*a->functions)))) return XR_XIR_BUDGET;
    if (memcmp(a->functions,b->functions,5*sizeof(*a->functions))) return XR_XIR_BAD_STRUCTURE;
    if (a->modules[0].initializer!=b->modules[0].initializer || a->modules[0].dependency_count ||
        b->modules[0].dependency_count || a->modules[0].name==b->modules[0].name)
        return XR_XIR_BAD_STRUCTURE;
    status=effect_lowered_snapshot_literal(context,
        (XrXirLiteral){a->modules[0].name,a->modules[0].name_length},
        (XrXirLiteral){b->modules[0].name,b->modules[0].name_length});
    for (uint32_t f=0;status==XR_XIR_OK && f<5;++f) {
        const XrXirFunction *actual=&owned->functions[f],*source=&module->functions[f];
        if (actual->parameter_count!=source->parameter_count || actual->result!=source->result ||
            actual->block_count!=source->block_count || actual->instruction_count!=source->instruction_count ||
            actual->operand_count!=source->operand_count || actual->name==source->name ||
            (actual->parameter_count && actual->parameters==source->parameters) ||
            actual->instructions==source->instructions || actual->blocks==source->blocks ||
            (actual->operand_count && actual->operands==source->operands)) return XR_XIR_BAD_STRUCTURE;
        status=effect_lowered_snapshot_literal(context,
            (XrXirLiteral){actual->name,actual->name_length},(XrXirLiteral){source->name,source->name_length});
        if (status==XR_XIR_OK) status=effect_lowered_snapshot_vector(context,
            actual->parameters,source->parameters,actual->parameter_count);
        uint64_t bytes=(uint64_t)actual->instruction_count*sizeof(*actual->instructions)+
            (uint64_t)actual->block_count*sizeof(*actual->blocks)+
            (uint64_t)actual->operand_count*sizeof(*actual->operands);
        if (status==XR_XIR_OK && !xir_compile_work(context,bytes*2)) status=XR_XIR_BUDGET;
        if (status==XR_XIR_OK && (memcmp(actual->instructions,source->instructions,
            (size_t)actual->instruction_count*sizeof(*actual->instructions)) ||
            memcmp(actual->blocks,source->blocks,(size_t)actual->block_count*sizeof(*actual->blocks)) ||
            (actual->operand_count && memcmp(actual->operands,source->operands,
                (size_t)actual->operand_count*sizeof(*actual->operands))))) status=XR_XIR_BAD_STRUCTURE;
    }
    const EffectInvocationOwner *equations=certificate->equations;
    if (status==XR_XIR_OK && (!equations || equations->work || equations->module || equations->effects ||
        equations->graph || equations->declared || equations->pending || !certificate->solution ||
        certificate->solution->count!=module->function_count || !certificate->declared ||
        certificate->declared->resources!=context->resources)) status=XR_XIR_BAD_STRUCTURE;
    return status;
}

/* Closed physical specialization and lowering run their original complete
 * verification. No synthetic INSTANCE or cached input mask substitutes for
 * this real producer-to-receiver pipeline. */
static XrXirStatus invocation_returned_lowered(const XrXirCompileContext *context,
    const XrXirArtifact *checked,bool oracle) {
    XrXirArtifact *specialized=NULL,*lowered=NULL;XrXirEffects *effects=NULL;
    XrXirDiagnostic diagnostic={0};uint32_t phase=0;
    const XrXirModule *raw=xr_xir_compile_artifact_module(checked);
    if (oracle) CHECK(raw && raw->stage==XR_XIR_CHECKED && !raw->provenance &&
        !raw->generics && !raw->defaults && raw->types && !raw->types->interfaces &&
        raw->declarations && !raw->declarations->implementations);
    XrXirStatus status=xr_xir_compile_specialize(checked,&specialized,&diagnostic);
    if (status==XR_XIR_OK && oracle) {
        const XrXirModule *closed=xr_xir_compile_artifact_module(specialized);
        CHECK(closed && closed->stage==XR_XIR_CHECKED && !closed->provenance &&
            !closed->generics && !closed->defaults && closed->function_count==raw->function_count);
    }
    if (status==XR_XIR_OK) { phase=1;status=xr_xir_compile_artifact_verify(specialized,&diagnostic); }
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if (status==XR_XIR_OK) { phase=2;status=xr_xir_compile_lower(specialized,&target,&lowered,&diagnostic); }
    if (status==XR_XIR_OK) { phase=3;status=xr_xir_compile_artifact_verify(lowered,&diagnostic); }
    if (status==XR_XIR_OK) { phase=4;status=xr_xir_compile_effects_analyze(lowered,&effects); }
    if (status==XR_XIR_OK) status=invocation_returned_lowered_snapshot(context,effects->invocations,
        xr_xir_compile_artifact_module(lowered));
    if (status==XR_XIR_OK && oracle) {
        const EffectInvocationCertificate *certificate=effects->invocations;
        const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
        uint32_t entry=module->declarations->entry_function;
        CHECK(module->stage==XR_XIR_LOWERED && !module->provenance && entry<effects->count && certificate);
        CHECK(!effects->root[entry].requires_root && !effects->root[entry].unresolved);
        const EffectInvocationOwner *owner=certificate->equations;
        uint32_t node=owner->roots[entry];bool found=false;
        for (uint32_t e=owner->nodes[node].edge_head;e!=UINT32_MAX;e=owner->edges[e].next) {
            const EffectInvocationEdge *edge=&owner->edges[e];
            if (edge->instruction!=1 || edge->latent || edge->producer==UINT32_MAX) continue;
            CHECK(edge->producer<owner->site_count && owner->sites[edge->producer].captures==1);
            const EffectInvocationSite *site=&owner->sites[edge->producer];
            const XrXirFunction *factory=&certificate->bodies.functions[site->function];
            CHECK(site->instruction<factory->instruction_count &&
                factory->instructions[site->instruction].op==XR_XIR_FUNCTION_REF);
            uint32_t mask=UINT32_MAX;
            CHECK(effect_invocation_certificate_node_mask(context,certificate,edge->target,&mask)==XR_XIR_OK && !mask);
            found=true;
        }
        CHECK(found);
    }
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(specialized);
    if (status==XR_XIR_OK && oracle) {
        uint32_t mask=UINT32_MAX,entry=effects->invocations->bodies.declarations->entry_function;
        CHECK(effect_invocation_certificate_node_mask(context,effects->invocations,
            effects->invocations->equations->roots[entry],&mask)==XR_XIR_OK && !mask);
    }
    if (status!=XR_XIR_OK && oracle) fprintf(stderr,
        "INVOCATION_RETURN_LOWERED phase=%u status=%u f=%u b=%u i=%u\n",phase,status,
        diagnostic.function,diagnostic.block,diagnostic.instruction);
    xr_xir_compile_effects_free(effects);return status;
}

/* Public analysis and an independently requested private seal both use the
 * real Checked owner. The private returned data does not authorize execution. */
static XrXirStatus conditional_invocation_returned(const XrXirCompileContext *context,
    uint32_t mode,bool oracle) {
    if (mode>=8) return XR_XIR_BAD_STRUCTURE;
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
    if (status==XR_XIR_OK && mode>=5) status=invocation_returned_lowered(context,checked,oracle);
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
                CHECK(owned->sites[edge->producer].target==2 &&
                    owned->sites[edge->producer].captures==(mode>=5?1u:0u));
                found=true;
            }
        }
        CHECK(found==(mode<2 || mode>=5));
        if (mode<2 || mode>=5) CHECK(effect_invocation_producer_find(entry,0,0));
        if (mode==4) CHECK(!effect_invocation_producer_find(&owned->nodes[owned->roots[1]],2,0));
        uint32_t original=UINT32_MAX;
        CHECK(effect_invocation_bounds_mask(context,certificate->declared,0,0,&original)==XR_XIR_OK &&
            original==(mode==2?XR_XIR_CALLABLE_ROOT_UNRESOLVED:0));
        const EffectInvocationOwner *public_owner=published->invocations->equations;
        if (mode<2 || mode>=5) CHECK(effect_invocation_producer_find(&public_owner->nodes[public_owner->roots[0]],0,0));
        if (mode>=6) invocation_returned_environment_owned(context,certificate,published);
    }
    xr_xir_compile_effects_free(published);effect_invocation_certificate_free(certificate);
    if (status!=XR_XIR_OK && oracle) fprintf(stderr,
        "INVOCATION_RETURNED mode=%u phase=%u status=%u f=%u b=%u i=%u\n",mode,phase,status,
        diagnostic.function,diagnostic.block,diagnostic.instruction);
    return status;
}
