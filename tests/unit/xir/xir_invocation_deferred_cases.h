/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_deferred_cases.h - Real REF callback actuals and sealed frontiers
 */
typedef struct InvocationDeferredFixture {
    XrXirCallableParameter ref;
    XrXirTypeNode nodes[2];XrXirTypes types;
    XrXirType parameters[2],cell;
    XrXirInstruction entry[5],apply[2],read[2],init[4];
    uint32_t actuals[2],reference;
    XrXirBlock entry_block,two,init_block;
    XrXirFunction functions[4];XrXirFunctionIdentity identities[4];
    XrXirSourceModule source;XrXirSlot slot;XrXirDeclarations declarations;XrXirModule module;
} InvocationDeferredFixture;

/* These are ordinary declaration-complete raw modules, not asserted formula
 * probes. Built checking and owned verification must accept every literal
 * before the private common invocation owner receives it. */
static void invocation_deferred_fixture(InvocationDeferredFixture *f,uint32_t mode) {
    memset(f,0,sizeof(*f));f->cell=(XrXirType)256;
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64};
    f->ref=(XrXirCallableParameter){f->cell,XR_PARAM_REF};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,
        .parameters=&f->ref,.parameter_count=1,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->types=(XrXirTypes){f->nodes,2,NULL,NULL};
    f->parameters[0]=(XrXirType)257;f->parameters[1]=f->cell;
    f->entry[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
    f->entry[1]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=f->cell,.args={0,0}};
    f->entry[2]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)257,.immediate=2};
    f->entry[3]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=1,.args={0,2}};
    f->entry[4]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={3,0}};
    f->actuals[0]=2;f->actuals[1]=1;f->entry_block=(XrXirBlock){.count=5};
    if (mode) {
        f->entry[0]=(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=f->cell};
        f->entry[1]=f->entry[2];f->entry[2]=f->entry[3];
        f->entry[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
        f->actuals[0]=1;f->actuals[1]=0;f->entry_block.count=4;
    }
    f->reference=1;
    f->apply[0]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.args={0,1}};
    f->apply[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
    f->read[0]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=XR_XIR_I64};
    f->read[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
    f->two=(XrXirBlock){.count=2};f->init_block=(XrXirBlock){.count=1};
    f->init[0]=(XrXirInstruction){.op=XR_XIR_RETURN};
    if (mode) {
        f->init[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
        f->init[1]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=f->cell};
        f->init[2]=(XrXirInstruction){.op=XR_XIR_SLOT_INIT,.args={1,0}};
        f->init[3]=(XrXirInstruction){.op=XR_XIR_RETURN};f->init_block.count=4;
    }
    f->functions[0]=(XrXirFunction){.name="entry",.name_length=5,.result=XR_XIR_I64,
        .blocks=&f->entry_block,.block_count=1,.instructions=f->entry,.instruction_count=f->entry_block.count,
        .operands=f->actuals,.operand_count=2};
    f->functions[1]=(XrXirFunction){.name="apply",.name_length=5,.parameters=f->parameters,.parameter_count=2,
        .result=XR_XIR_I64,.blocks=&f->two,.block_count=1,.instructions=f->apply,.instruction_count=2,
        .operands=&f->reference,.operand_count=1};
    f->functions[2]=(XrXirFunction){.name="read",.name_length=4,.parameters=&f->cell,.parameter_count=1,
        .result=XR_XIR_I64,.blocks=&f->two,.block_count=1,.instructions=f->read,.instruction_count=2};
    f->functions[3]=(XrXirFunction){.name="init",.name_length=4,.result=XR_XIR_UNIT,
        .blocks=&f->init_block,.block_count=1,.instructions=f->init,.instruction_count=f->init_block.count};
    f->source=(XrXirSourceModule){"deferred",8,NULL,0,3};f->slot=(XrXirSlot){0,f->cell,1};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .entry_function=0,.slots=mode?&f->slot:NULL,.slot_count=mode?1:0};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=4,
        .types=&f->types,.declarations=&f->declarations,.linkage_kind=XR_XIR_PROGRAM};
}

static void invocation_deferred_rejections(const XrXirCompileContext *context,
    EffectInvocationOwner **owner,EffectInvocationSolution **solution) {
    EffectInvocationOwner *original=*owner;EffectInvocationSolution *staged=*solution;
    CHECK(original->deferred_count==1 && original->deferred_capacity>=2);
    EffectInvocationDeferred *record=&original->deferred_calls[0];
    EffectInvocationBasis basis={0};
    CHECK(effect_invocation_basis(original,original->nodes[record->node].root,&basis)==XR_XIR_OK);
    CHECK(record->parameter==0 && record->instruction==0 && record->count==1 && basis.parameters==2);
    CHECK(!record->arguments[0] && record->arguments[basis.fn_words]==UINT64_C(18));
    EffectInvocationSealRequest request={owner,solution,NULL};EffectInvocationCertificate *out=NULL;
    uint64_t actual=record->arguments[basis.fn_words];
    record->arguments[basis.fn_words]|=UINT64_C(1)<<basis.parameters;
    CHECK(effect_invocation_seal(context,&request,&out)==XR_XIR_BAD_STRUCTURE && !out &&
        *owner==original && *solution==staged && original->module && original->effects);
    record->arguments[basis.fn_words]=actual;
    original->deferred_count=0;
    CHECK(effect_invocation_seal(context,&request,&out)==XR_XIR_BAD_STRUCTURE && !out &&
        *owner==original && *solution==staged);
    original->deferred_count=1;
    original->deferred_calls[1]=*record;original->deferred_count=2;
    CHECK(effect_invocation_seal(context,&request,&out)==XR_XIR_BAD_STRUCTURE && !out &&
        *owner==original && *solution==staged);
    original->deferred_count=1;
    record->parameter=1;
    CHECK(effect_invocation_seal(context,&request,&out)==XR_XIR_BAD_STRUCTURE && !out &&
        *owner==original && *solution==staged);
    record->parameter=0;
    uint32_t constant=staged->formulas[0].constant_mask;
    staged->formulas[0].constant_mask^=XR_XIR_CALLABLE_ROOT_REQUIRED;
    CHECK(effect_invocation_seal(context,&request,&out)==XR_XIR_BAD_STRUCTURE && !out &&
        *owner==original && *solution==staged);
    staged->formulas[0].constant_mask=constant;
    XrXirRootTerm *term=(XrXirRootTerm *)staged->formulas[1].terms;
    CHECK(staged->formulas[1].term_count==1 && term && term->kind==XR_XIR_ROOT_TERM_CONTEXT_CALL);
    term->kind=XR_XIR_ROOT_TERM_PARAMETER;
    CHECK(effect_invocation_seal(context,&request,&out)==XR_XIR_BAD_STRUCTURE && !out &&
        *owner==original && *solution==staged);
    term->kind=XR_XIR_ROOT_TERM_CONTEXT_CALL;
}

static XrXirStatus conditional_invocation_deferred(const XrXirCompileContext *context,
    uint32_t mode,bool oracle) {
    if (mode>=2) return XR_XIR_BAD_STRUCTURE;
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,mode);
    XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};uint32_t phase=0;
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,&diagnostic);
    if (status==XR_XIR_OK) { phase=1;status=xr_xir_compile_artifact_verify(checked,&diagnostic); }
    const XrXirModule *module=checked?xr_xir_compile_artifact_module(checked):NULL;
    XrXirEffects effects={0};EffectGraph graph={0};EffectInvocationDeclaredBounds *bounds=NULL;
    EffectInvocationOwner *owner=NULL;EffectInvocationSolution *solution=NULL;
    EffectInvocationCertificate *certificate=NULL;
    if (status==XR_XIR_OK) { phase=2;status=invocation_core_storage(context,module,&effects); }
    if (status==XR_XIR_OK) { phase=3;status=effect_graph_build(module,&effects,&graph,(XrXirCompileContext *)context); }
    if (status==XR_XIR_OK) { phase=4;status=effect_parameters_derive(module,&effects,&graph,context); }
    if (status==XR_XIR_OK) { phase=5;status=effect_invocation_bounds_new(context,&bounds); }
    for (uint32_t f=0;status==XR_XIR_OK && f<module->function_count;++f)
        status=effect_invocation_bounds_capture(context,bounds,module->types,&module->functions[f],f,NULL);
    if (status==XR_XIR_OK) { phase=6;status=effect_invocations_create(context,module,&effects,&graph,bounds,&owner); }
    if (status==XR_XIR_OK) { phase=7;status=effect_invocations_solve(owner); }
    if (status==XR_XIR_OK) { phase=8;status=effect_invocation_stage(owner,&solution); }
    if (status==XR_XIR_OK && oracle) invocation_deferred_rejections(context,&owner,&solution);
    EffectInvocationSealRequest request={&owner,&solution,NULL};
    if (status==XR_XIR_OK) { phase=9;status=effect_invocation_seal(context,&request,&certificate); }
    if (status!=XR_XIR_OK && oracle) fprintf(stderr,
        "INVOCATION_DEFERRED mode=%u phase=%u status=%u f=%u b=%u i=%u\n",mode,phase,status,
        diagnostic.function,diagnostic.block,diagnostic.instruction);
    effect_invocation_free(owner);effect_invocation_solution_free(solution);
    effect_graph_free(&graph);invocation_core_storage_free(&effects);
    effect_invocation_bounds_free(bounds);xr_xir_compile_artifact_free(checked);
    memset(&fixture,0xCE,sizeof(fixture));
    EffectInvocationCauseTrace *trace=NULL;
    if (status==XR_XIR_OK) {
        uint32_t caller=UINT32_MAX,formal=UINT32_MAX;
        status=effect_invocation_certificate_node_mask(context,certificate,certificate->equations->roots[0],&caller);
        if (status==XR_XIR_OK) status=effect_invocation_certificate_node_mask(context,certificate,
            certificate->equations->roots[1],&formal);
        if (status==XR_XIR_OK) status=effect_invocation_certificate_trace(context,certificate,1,&trace);
        if (status==XR_XIR_OK && oracle) {
            CHECK(caller==(mode?XR_XIR_CALLABLE_ROOT_REQUIRED:0) && formal==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
            const XrXirRootFormula *formula=&certificate->solution->formulas[1];
            CHECK(!formula->constant_mask && formula->term_count==1 &&
                formula->terms[0].kind==XR_XIR_ROOT_TERM_CONTEXT_CALL && !formula->terms[0].index);
            CHECK(certificate->equations->deferred_count==1 &&
                certificate->equations->deferred_calls[0].arguments[1]==UINT64_C(18));
            CHECK(!certificate->equations->work && !certificate->equations->module &&
                !certificate->equations->effects && !certificate->equations->graph && !certificate->equations->declared);
            CHECK(trace && !trace->counts[0] && trace->counts[1]==1 &&
                trace->steps[0].witness.cause==XR_XIR_ROOT_CAUSE_CONTEXT_CALL &&
                !trace->steps[0].witness.instruction && !trace->steps[0].witness.distance);
        }
    }
    effect_invocation_certificate_free(certificate);
    if (status==XR_XIR_OK && oracle) CHECK(trace && trace->facts.unresolved && trace->counts[1]==1);
    effect_invocation_trace_free(trace);return status;
}
