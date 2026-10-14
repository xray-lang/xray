/* Same-operation initial owner sharing preserves every public fresh check. */
#ifndef XIR_INITIAL_ORDINARY_MATERIAL_CASES_H
#define XIR_INITIAL_ORDINARY_MATERIAL_CASES_H
#include "xir/xxir_source.h"
#include "toolchain/xcompiler_session.h"

/* The original complete checker supplies an actual sealed owner. No hand-made
 * certificate, fake TEMPLATE marker or caller-supplied roles select sharing. */
static XrXirStatus initial_ordinary_effects(const XrXirCompileContext *context,
    const XrXirModule *source,XrXirEffects **output) {
    XrXirConstruction *construction=NULL;
    XrXirStatus status=xir_fixture_construction(context,source->types,&construction);
    if (status==XR_XIR_OK) status=xr_xir_compile_verify_analyzed_effects_v2(context,
        source,construction,output,NULL);
    xr_xir_compile_construction_free(construction);return status;
}

static void initial_ordinary_fixture(OrdinaryMaterialFixture *fixture,uint32_t mode) {
    if (mode==1) ordinary_material_cell_fixture(fixture);else ordinary_material_fixture(fixture);
    /* These genuine formal definitions are a complete Library. A Program
     * entry cannot itself take Cell or Callable parameters. The initializer
     * remains its actual zero-argument Unit body and all CFGs stay intact. */
    fixture->source.linkage_kind=XR_XIR_LIBRARY;
    fixture->declarations.root_module=fixture->declarations.entry_function=UINT32_MAX;
    if (mode==4) {
        fixture->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,
            .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
        fixture->types.count=2;fixture->parameter=(XrXirType)257;
        fixture->functions[0].parameters=&fixture->parameter;fixture->functions[0].parameter_count=1;
        fixture->operations[1].args[0]=1;
    }
}

/* The dense certificate keeps its immediate ordinary overlay alive; that
 * overlay in turn keeps the original sealed Source material alive. */
static XrXirStatus initial_ordinary_owner(const XrXirCompileContext *context,
    const XrXirModule *source,EffectBodyMaterial *initial,bool tracked) {
    uint32_t references=initial->references;
    EffectContextOwner *owner=NULL;EffectContextForest *forest=NULL;
    XrXirStatus status=effect_context_owner_build(context,source,NULL,initial,&owner);
    if (status==XR_XIR_OK) {
        EffectBodyMaterial *ordinary=owner->ordinary->uses.generation;
        EffectBodyMaterial *dense=owner->forest->invocations->body;
        CHECK(ordinary->backbone==initial && ordinary->overlay && !initial->backbone);
        CHECK(dense->backbone==ordinary && !dense->overlay && effect_body_material_shape(dense));
        CHECK(dense->declared!=ordinary->declared && ordinary->declared!=initial->declared);
        CHECK(dense->cells==initial->cells && dense->physical==initial->physical);
        forest=owner->forest;owner->forest=NULL;
        XrXirEffects *summary=&owner->dense->uses;
        const EffectFinalFunctions *borrowed=(&forest->invocations->final->summary);
        CHECK(!summary->invocations && summary->result_borrow==borrowed &&
            summary->formula_terminals==borrowed->witnesses &&
            summary->constant_witnesses==borrowed->witnesses+(size_t)borrowed->count*2);
        for (uint32_t f=0;f<summary->count;++f) {
            const XrXirRootFormula *view=&summary->contracts[f].formula;
            const XrXirRootFormula *actual=&borrowed->functions[f].formula;
            CHECK(view->terms==actual->terms && view->term_count==actual->term_count &&
                view->constant_mask==actual->constant_mask);
        }
        effect_parameters_free(summary);
        CHECK(!summary->result_borrow && !summary->contracts &&
            !summary->formula_terminals && !summary->constant_witnesses);
        effect_context_owner_free(owner);owner=NULL;
        CHECK(ordinary->references==1 && initial->references==references+1);
        status=ordinary_material_cell_match(context,dense,tracked);
        if (status==XR_XIR_OK) status=effect_invocation_public_gate(context,forest->invocations,source->function_count);
        if (status==XR_XIR_OK) status=effect_invocation_forest_match(context,forest);
    }
    effect_context_forest_free(forest);effect_context_owner_free(owner);return status;
}

static XrXirStatus initial_ordinary_operation(const XrXirCompileContext *context,uint32_t mode) {
    OrdinaryMaterialFixture fixture;initial_ordinary_fixture(&fixture,mode);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;EffectOrdinaryContexts *ordinary=NULL;
    EffectBodyMaterial *material=NULL;XrXirEffects refinement={.resources=context->resources,.count=2};
    const XrXirModule *source=&fixture.source;
    XrXirStatus status=mode==5?xir_fixture_check(context,source,&checked,NULL):XR_XIR_OK;
    if (status==XR_XIR_OK && checked) source=xr_xir_compile_artifact_module(checked);
    if (status==XR_XIR_OK) status=initial_ordinary_effects(context,source,&effects);
    EffectBodyMaterial *initial=status==XR_XIR_OK?effects->invocations->body:NULL;
    uint32_t references=initial?initial->references:0;
    if (status==XR_XIR_OK && mode==1) status=initial_ordinary_owner(context,source,initial,true);
    if (status==XR_XIR_OK) status=effect_ordinary_build(context,source,initial,&ordinary);
    if (status==XR_XIR_OK) {
        material=ordinary->uses.generation;ordinary->uses.generation=NULL;
        CHECK(material->backbone==initial && material->overlay && initial->references==references+1);
        CHECK(material->functions!=initial->functions && !material->functions[0].blocks &&
            !material->functions[0].block_count && initial->functions[0].block_count==1);
        CHECK(material->cells==initial->cells && material->physical==initial->physical);
        CHECK(material->domain.generics && material->declared!=initial->declared);
        if (!source->generics) CHECK(!initial->generics && material->generics);
        if (mode==4) CHECK(initial->functions[0].initial_roles[0]==XR_XIR_EFFECT_PARAMETER_FIXED &&
            material->functions[0].initial_roles[0]==XR_XIR_EFFECT_PARAMETER_VARIABLE);
        if (mode==2 || mode==3) {
            EffectOrdinaryView view={0};ordinary->terms.remaining=context;
            status=effect_ordinary_view(ordinary,source,&view);view.input.initial=initial;
            if (status==XR_XIR_OK && mode==2)
                ((XrXirInstruction *)ordinary->functions[0].instructions)[0].immediate=43;
            if (status==XR_XIR_OK && mode==3) status=xir_effects_refinement_capture(context,&view.module,&refinement);
            EffectBodyMaterial *fresh=NULL;
            if (status==XR_XIR_OK) status=effect_body_material_generation_input(context,&view.module,
                refinement.refinement,&view.input,&fresh);
            if (status==XR_XIR_OK) {
                CHECK(fresh && !fresh->backbone && fresh->cells!=initial->cells && initial->references==references+1);
                effect_body_material_free(material);material=fresh;
            } else effect_body_material_free(fresh);
        }
        effect_ordinary_free(ordinary);ordinary=NULL;
        xr_xir_compile_effects_free(effects);effects=NULL;
        xr_xir_compile_artifact_free(checked);checked=NULL;
        memset(&fixture,0xCE,sizeof(fixture));
        EffectBodyRequest read=effect_body_owned_request(context,material);
        XrXirInstruction storage={0};const XrXirInstruction *operation=NULL;
        if (status==XR_XIR_OK) status=effect_body_request_instruction(&read,0,0,&storage,&operation);
        if (status==XR_XIR_OK) CHECK(mode==1?operation->op==XR_XIR_CELL_READ:
            operation->op==XR_XIR_CONST_INT && operation->immediate==(mode==2?43:42));
        if (status==XR_XIR_OK) status=effect_body_initial_roles_match(context,material,0);
        if (status==XR_XIR_OK) status=ordinary_material_cell_match(context,material,mode==1);
    }
    effect_refinement_free(refinement.refinement);effect_body_material_free(material);
    effect_ordinary_free(ordinary);xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);
    return status;
}

/* The real closed Runner environment adds an ordinary function and rewrites
 * its requirement call. This is a legal different generation, not BADS or an
 * existential reason to share the initial physical/cell inventory. */
static XrXirStatus initial_ordinary_expansion(const XrXirCompileContext *context) {
    ConditionalRawFixture fixture;conditional_raw_fixture(&fixture,2);
    XrXirInstruction entry[2]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_RETURN,.args={0,0}}};
    XrXirFunction functions[5]={0};XrXirGeneric generics[5]={0};
    XrXirFunctionIdentity identities[5]={0};XrXirBlock blocks[5]={0};
    memcpy(functions,fixture.functions,sizeof(fixture.functions));
    memcpy(generics,fixture.generics,sizeof(fixture.generics));
    memcpy(identities,fixture.identities,sizeof(fixture.identities));
    functions[4]=(XrXirFunction){.name="entry",.name_length=5,.result=XR_XIR_I64,
        .instructions=entry,.instruction_count=2};
    for (uint32_t f=0;f<5;++f) {
        blocks[f]=(XrXirBlock){.first=0,.count=functions[f].instruction_count};
        functions[f].blocks=&blocks[f];functions[f].block_count=1;
    }
    fixture.declarations.functions=identities;fixture.declarations.entry_function=4;
    fixture.module.functions=functions;fixture.module.function_count=5;fixture.module.generics=generics;
    XrXirEffects *effects=NULL;EffectOrdinaryContexts *ordinary=NULL;
    XrXirStatus status=initial_ordinary_effects(context,&fixture.module,&effects);
    EffectBodyMaterial *initial=status==XR_XIR_OK?effects->invocations->body:NULL;
    if (status==XR_XIR_OK) status=effect_ordinary_build(context,&fixture.module,initial,&ordinary);
    if (status==XR_XIR_OK) {
        CHECK(ordinary->count>5 && ordinary->uses.generation && !ordinary->uses.generation->backbone);
        CHECK(ordinary->uses.generation->cells!=initial->cells && initial->references==1);
        xr_xir_compile_effects_free(effects);effects=NULL;
        memset(&fixture,0xCE,sizeof(fixture));memset(functions,0xCE,sizeof(functions));
        EffectBodyMaterial *material=ordinary->uses.generation;
        status=effect_body_initial_roles_match(context,material,ordinary->count-1);
        if (status==XR_XIR_OK) status=ordinary_material_cell_match(context,material,false);
    }
    effect_ordinary_free(ordinary);xr_xir_compile_effects_free(effects);return status;
}

static XrXirStatus initial_ordinary_resource_operation(const XrXirCompileContext *context,uint32_t mode) {
    return mode==6?initial_ordinary_expansion(context):initial_ordinary_operation(context,mode);
}

/* Only the actual Source producer supplies TEMPLATE evidence and its complete
 * contracts. A plain Checked Library correctly has no context owner. */
static void initial_ordinary_source(const XrXirCompileContext *context,XrXirSourceResult *output) {
    static const char text[]=
        "fn answer()->i64{return 42}\n"
        "export fn references()->i64{const first=answer;const second=answer;return 0}\n"
        "print(references())\n";
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"reference-refresh",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,text,sizeof(text)-1};
    XrXirSourceDiagnostic diagnostic={0};char *failure=NULL;
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,output,&diagnostic,&failure);
    xr_compile_session_free(session);
    if (status!=XR_XIR_OK) fprintf(stderr,"INITIAL_REFRESH_SOURCE status=%u line=%d column=%d %s\n",
        (unsigned)status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_resources_free(failure);
    CHECK(status==XR_XIR_OK && output->checked);
}

static uint32_t initial_ordinary_reference_body(const XrXirCompileContext *context,
    const XrXirModule *source,uint32_t sites[2]) {
    uint32_t selected=UINT32_MAX,count=0;
    for (uint32_t f=0;f<source->function_count;++f) {
        CHECK(xir_compile_work(context,1));
        const XrXirFunction *body=&source->functions[f];
        if (body->name_length==10 && !memcmp(body->name,"references",10)) {
            CHECK(selected==UINT32_MAX);selected=f;
        }
    }
    CHECK(selected!=UINT32_MAX);
    const XrXirFunction *body=&source->functions[selected];
    for (uint32_t i=0;i<body->instruction_count;++i) {
        CHECK(xir_compile_work(context,1));
        const XrXirInstruction *op=&body->instructions[i];
        if (op->op!=XR_XIR_FUNCTION_REF) continue;
        CHECK(count<2 && op->immediate>=0 && (uint64_t)op->immediate<source->function_count);
        const XrXirFunction *target=&source->functions[(uint32_t)op->immediate];
        CHECK(target->name_length==6 && !memcmp(target->name,"answer",6));sites[count++]=i;
    }
    CHECK(count==2 && sites[0]<sites[1]);return selected;
}

/* Source retains the original UNKNOWN signature when it interns the precise
 * NONE signature. Select it by full callable shape, never by a guessed ID. */
static XrXirType initial_ordinary_wide(const XrXirCompileContext *context,
    const XrXirTypes *types,XrXirType narrow) {
    const XrXirTypeNode *original=xr_xir_callable_signature(types,narrow);
    CHECK(original && original->result==XR_XIR_I64 && !original->parameter_count &&
        (original->flags&XR_XIR_CALLABLE_ROOT_MASK)==XR_XIR_CALLABLE_ROOT_NONE);
    for (uint32_t t=0;t<types->count;++t) {
        CHECK(xir_compile_work(context,1));
        const XrXirTypeNode *candidate=&types->nodes[t];
        if (candidate->kind!=XR_XIR_TYPE_CALLABLE || candidate->parameter_count ||
            candidate->result!=original->result ||
            candidate->flags!=(XR_XIR_CALLABLE_ROOT_UNRESOLVED|
                (original->flags&XR_XIR_CALLABLE_NO_SUSPEND))) continue;
        CHECK(t<=UINT32_MAX-XR_XIR_CONSTRUCTED_TYPE_BASE);
        XrXirType wide=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);
        CHECK(xr_xir_compile_callable_weakening(context,types,narrow,wide)==XR_XIR_OK);
        return wide;
    }
    CHECK(false);return XR_XIR_UNIT;
}

/* Advance two genuine reference sites before an outer invocation publication,
 * then restore them in the same order. Each changed input needs a new context;
 * the original certificate remains an independent immutable proof. */
static void initial_ordinary_refresh_controls(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;XrXirSourceResult result={0};
    initial_ordinary_source(&context,&result);
    const XrXirModule *original=xr_xir_compile_artifact_module(result.checked);
    CHECK(original && original->provenance && original->provenance->kind==XR_XIR_EVIDENCE_TEMPLATE &&
        original->provenance->contract_count==original->function_count);
    XrXirEffects *effects=NULL;
    XrXirStatus status=initial_ordinary_effects(&context,original,&effects);
    if (status!=XR_XIR_OK) fprintf(stderr,"INITIAL_REFRESH_EFFECTS status=%u\n",(unsigned)status);
    CHECK(status==XR_XIR_OK && effects);
    CHECK(effects->contexts && !effects->refinement &&
        effects->result_borrow==(&effects->invocations->final->summary) &&
        effects->contexts->dense->uses.result_borrow==(&effects->contexts->forest->invocations->final->summary));
    CHECK(effects->invocations && !effects->invocations->body->backbone);
    EffectInvocationCertificate *certificate=effects->invocations,saved_certificate=*certificate;
    EffectBodyMaterial *initial=certificate->body;uint32_t sites[2]={0};
    uint32_t selected=initial_ordinary_reference_body(&context,original,sites);
    uint64_t function_bytes=(uint64_t)original->function_count*sizeof(*original->functions);
    uint64_t instruction_bytes=(uint64_t)original->functions[selected].instruction_count*sizeof(XrXirInstruction);
    CHECK(function_bytes<=SIZE_MAX && instruction_bytes<=SIZE_MAX);
    XrXirFunction *functions=xir_compile_copy(&context,original->functions,
        (size_t)function_bytes,&status);
    XrXirInstruction *operations=xir_compile_copy(&context,original->functions[selected].instructions,
        (size_t)instruction_bytes,&status);
    CHECK(status==XR_XIR_OK && functions && operations);
    XrXirModule current=*original;current.functions=functions;functions[selected].instructions=operations;
    XrXirType narrow[2]={operations[sites[0]].type,operations[sites[1]].type};
    XrXirType wide[2]={initial_ordinary_wide(&context,original->types,narrow[0]),
        initial_ordinary_wide(&context,original->types,narrow[1])};
    for (uint32_t pass=0;pass<2;++pass) for (uint32_t site=0;site<2;++site) {
        EffectBodyMaterial saved=*initial;
        EffectBodyMaterial *old_ordinary=effects->contexts->ordinary->uses.generation;
        /* Dropping the previous context retires exactly its actual parent
         * edge. The certificate's own edge and every payload remain intact. */
        if (old_ordinary->backbone==initial) { CHECK(saved.references>1);--saved.references; }
        operations[sites[site]].type=pass?narrow[site]:wide[site];
        bool restored=pass==1 && site==1;
        XrXirStatus expected=restored?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
        CHECK(effect_body_source_match(&context,initial,&current)==expected);
        CHECK(xir_effects_context_refresh(&context,&current,effects)==XR_XIR_OK);
        CHECK(effects->invocations==certificate && effects->result_borrow==(&certificate->final->summary) &&
            effects->contexts->dense->uses.result_borrow==(&effects->contexts->forest->invocations->final->summary) &&
            !memcmp(certificate,&saved_certificate,sizeof(saved_certificate)) &&
            !memcmp(initial,&saved,sizeof(saved)) && !initial->backbone && !initial->overlay);
        EffectBodyMaterial *fresh=effects->contexts->ordinary->uses.generation;
        CHECK(fresh && fresh!=initial && !fresh->backbone && !fresh->overlay &&
            fresh->cells!=initial->cells && fresh->functions[selected].block_count==0);
        uint32_t mask=UINT32_MAX;
        CHECK(xir_effects_reference_root(&context,effects,&current,selected,sites[site],&mask)==XR_XIR_OK && !mask);
        CHECK(effect_body_source_match(&context,initial,original)==XR_XIR_OK);
        CHECK(effect_body_source_match(&context,initial,&current)==expected);
    }
    xr_compile_resources_free(operations);xr_compile_resources_free(functions);
    xr_xir_compile_source_result_free(&result);
    EffectBodyRequest read=effect_body_owned_request(&context,initial);
    for (uint32_t site=0;site<2;++site) {
        XrXirInstruction storage={0};const XrXirInstruction *operation=NULL;
        CHECK(effect_body_request_instruction(&read,selected,sites[site],&storage,&operation)==XR_XIR_OK &&
            operation->op==XR_XIR_FUNCTION_REF && operation->type==narrow[site]);
    }
    CHECK(ordinary_material_cell_match(&context,initial,false)==XR_XIR_OK);
    xr_xir_compile_effects_free(effects);rp_owner_free(&context,baseline);rp_balanced(physical);
}

/* Every node remains a fresh correspondence obligation. The record retains
 * its own four-domain witness and link; sharing node storage grants nothing. */
static void initial_ordinary_forest_literals(const XrXirCompileContext *context,
    EffectContextForest *forest) {
    CHECK(forest->count==2 && forest->nodes[0].parameter_count==1 &&
        forest->invocations && effect_invocation_forest_match(context,forest)==XR_XIR_OK);
    EffectForestNode node=forest->nodes[0];XrXirRootEffects facts=forest->facts[0];
    EffectContextCertificate record=forest->certificates[0];
    XrXirRootEffectWitness witness=forest->witnesses[0];
    XrXirType wrong_type=XR_XIR_I64;
    uint8_t wrong_kind=(uint8_t)(node.parameter_kinds[0]^1u);
    for (uint32_t attack=0;attack<14;++attack) {
        uint64_t live=rp_stats(context).live_bytes;
        switch (attack) {
            case 0: forest->nodes[0].declaration=UINT32_MAX;break;
            case 1: forest->nodes[0].owner=node.owner?0:UINT32_MAX;break;
            case 2: forest->nodes[0].constant_mask^=1u;break;
            case 3: ++forest->nodes[0].argument_count;break;
            case 4: ++forest->nodes[0].parameter_count;break;
            case 5: forest->facts[0].unresolved=!facts.unresolved;break;
            case 6: forest->nodes[0].physical_types=&wrong_type;break;
            case 7: forest->nodes[0].parameter_kinds=&wrong_kind;break;
            case 8: forest->certificates[0].witness.cause=
                (XrXirRootEffectCause)((uint32_t)record.witness.cause^1u);break;
            case 9: forest->certificates[0].next=forest->count;break;
            case 10: forest->certificates[0].parameter_owner=forest->count;break;
            case 11: forest->certificates[0].domain^=1u;break;
            case 12: forest->certificates[0].next_domain^=1u;break;
            case 13: forest->witnesses[0].cause=
                (XrXirRootEffectCause)((uint32_t)witness.cause^1u);break;
        }
        CHECK(effect_invocation_forest_match(context,forest)==XR_XIR_BAD_STRUCTURE &&
            rp_stats(context).live_bytes==live);
        forest->nodes[0]=node;forest->facts[0]=facts;
        forest->certificates[0]=record;forest->witnesses[0]=witness;
        CHECK(effect_invocation_forest_match(context,forest)==XR_XIR_OK &&
            rp_stats(context).live_bytes==live);
    }
    EffectForestNode target=forest->nodes[1];forest->nodes[1].owner=target.owner?0:UINT32_MAX;
    CHECK(effect_invocation_forest_match(context,forest)==XR_XIR_BAD_STRUCTURE);
    forest->nodes[1]=target;
    XrXirRootCauseTrace *occupied=(XrXirRootCauseTrace *)(uintptr_t)1;
    XrCompileResourceStats prepared=rp_stats(context);size_t attempts=rp_attempts;
    CHECK(effect_context_forest_trace(context,forest,0,&occupied)==XR_XIR_BAD_STRUCTURE &&
        occupied==(XrXirRootCauseTrace *)(uintptr_t)1 && rp_stats(context).work==prepared.work && rp_attempts==attempts);
    XrXirCompileContext foreign=rp_owner(rp_caps());prepared=rp_stats(&foreign);occupied=NULL;
    CHECK(effect_context_forest_trace(&foreign,forest,0,&occupied)==XR_XIR_BAD_STRUCTURE && !occupied &&
        rp_stats(&foreign).work==prepared.work && rp_stats(&foreign).live_bytes==prepared.live_bytes);
    rp_owner_free(&foreign,prepared.live_bytes);
    CHECK(effect_context_forest_trace(context,forest,0,&occupied)==XR_XIR_OK && occupied);
    xr_xir_compile_root_cause_trace_free(occupied);
}

/* Neither Source storage nor its producer summary is live for this fresh
 * reconstruction. The genuine retained forest owns all remaining inputs. */
static void initial_ordinary_forest_controls(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    OrdinaryMaterialFixture fixture;initial_ordinary_fixture(&fixture,1);
    XrXirEffects *effects=NULL;EffectContextOwner *owner=NULL;
    CHECK(initial_ordinary_effects(&context,&fixture.source,&effects)==XR_XIR_OK);
    CHECK(effect_context_owner_build(&context,&fixture.source,NULL,effects->invocations->body,&owner)==XR_XIR_OK);
    EffectContextForest *forest=owner->forest;owner->forest=NULL;
    effect_context_owner_free(owner);xr_xir_compile_effects_free(effects);
    memset(&fixture,0xCE,sizeof(fixture));
    initial_ordinary_forest_literals(&context,forest);
    CHECK(ordinary_material_cell_match(&context,forest->invocations->body,true)==XR_XIR_OK);
    effect_context_forest_free(forest);rp_owner_free(&context,baseline);rp_balanced(physical);
}

static void initial_ordinary_controls(void) {
    initial_ordinary_forest_controls();
    initial_ordinary_refresh_controls();
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;OrdinaryMaterialFixture fixture;initial_ordinary_fixture(&fixture,0);
    XrXirEffects *effects=NULL;EffectOrdinaryContexts *ordinary=NULL;
    CHECK(initial_ordinary_effects(&context,&fixture.source,&effects)==XR_XIR_OK);
    EffectBodyMaterial *initial=effects->invocations->body;
    XrCompileResourceStats prepared=rp_stats(&context);size_t attempts=rp_attempts;
    ordinary=(EffectOrdinaryContexts *)(uintptr_t)1;
    CHECK(effect_ordinary_build(&context,&fixture.source,initial,&ordinary)==XR_XIR_BAD_STRUCTURE &&
        ordinary==(EffectOrdinaryContexts *)(uintptr_t)1 && rp_stats(&context).work==prepared.work && rp_attempts==attempts);
    ordinary=NULL;XrXirCompileContext foreign=rp_owner(rp_caps());XrCompileResourceStats foreign_before=rp_stats(&foreign);
    CHECK(effect_ordinary_build(&foreign,&fixture.source,initial,&ordinary)==XR_XIR_BAD_STRUCTURE && !ordinary);
    CHECK(rp_stats(&foreign).work==foreign_before.work && rp_stats(&foreign).live_bytes==foreign_before.live_bytes);
    rp_owner_free(&foreign,foreign_before.live_bytes);
    CHECK(effect_ordinary_build(&context,&fixture.source,initial,&ordinary)==XR_XIR_OK);
    EffectBodyMaterial *holder=ordinary->uses.generation;
    CHECK(holder->backbone==initial && holder->overlay && initial->references==2);
    EffectBodyMaterial *saved_parent=holder->backbone;
    holder->backbone=holder;CHECK(!effect_body_material_shape(holder));holder->backbone=saved_parent;
    uint32_t saved_count=holder->overlay->count;--holder->overlay->count;
    CHECK(!effect_body_material_shape(holder));holder->overlay->count=saved_count;
    XrXirStage saved_stage=holder->stage;holder->stage=XR_XIR_LOWERED;
    CHECK(!effect_body_material_shape(holder));holder->stage=saved_stage;
    holder->overlay->generics[0].parameter_count=1;
    CHECK(initial->generics==NULL);holder->overlay->generics[0].parameter_count=0;
    EffectOrdinaryView view={0};ordinary->terms.remaining=&context;
    CHECK(effect_ordinary_view(ordinary,&fixture.source,&view)==XR_XIR_OK);view.input.initial=initial;
    for (uint32_t word=0;word<10;++word) {
        XrXirInstruction original=fixture.operations[0];
        switch (word) {
            case 0: fixture.operations[0].op=XR_XIR_CONST_FLOAT;break;
            case 1: fixture.operations[0].type=XR_XIR_U64;break;
            case 2: fixture.operations[0].args[0]=17;break;
            case 3: fixture.operations[0].args[1]=17;break;
            case 4: fixture.operations[0].targets[0]=17;break;
            case 5: fixture.operations[0].targets[1]=17;break;
            case 6: fixture.operations[0].immediate=43;break;
            case 7: fixture.operations[0].immediate=INT64_C(4294967338);break;
            case 8: fixture.operations[0].type_arguments[0]=17;break;
            case 9: fixture.operations[0].type_arguments[1]=17;break;
        }
        EffectBodyMaterial *empty=NULL;uint64_t live=rp_stats(&context).live_bytes;
        CHECK(effect_body_material_generation_input(&context,&view.module,NULL,&view.input,&empty)==XR_XIR_BAD_STRUCTURE &&
            !empty && initial->references==2 && rp_stats(&context).live_bytes==live);
        fixture.operations[0]=original;
    }
    uint32_t block_count=fixture.blocks[0].count;fixture.blocks[0].count=1;
    EffectBodyMaterial *empty=NULL;
    CHECK(effect_body_material_generation_input(&context,&view.module,NULL,&view.input,&empty)==XR_XIR_BAD_STRUCTURE && !empty);
    fixture.blocks[0].count=block_count;
    CHECK(effect_body_material_shape(holder) && effect_body_source_match(&context,initial,&fixture.source)==XR_XIR_OK);
    effect_ordinary_free(ordinary);xr_xir_compile_effects_free(effects);
    rp_owner_free(&context,baseline);rp_balanced(physical);
}

/* Every actual allocation, retry and exact/minus-one axis stays on its real
 * receiving owner. Normal generation never silently executes this full FI. */
static void conditional_initial_ordinary_resources(void) {
    initial_ordinary_controls();
    for (uint32_t mode=0;mode<7;++mode) {
        size_t sites=0;XrCompileResourceStats census={0};
        for (size_t pass=0;pass<=sites;++pass) {
            RootParameterMark physical=rp_mark();rp_fail_at=SIZE_MAX;rp_injected=false;
            XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
            size_t start=rp_attempts;rp_fail_at=pass?start+pass-1:SIZE_MAX;
            XrXirStatus status=initial_ordinary_resource_operation(&context,mode);
            if (!pass) {
                CHECK(status==XR_XIR_OK);sites=rp_attempts-start;census=rp_stats(&context);
                CHECK(sites && sites<512);
                printf("INITIAL_ORDINARY_CENSUS mode=%u sites=%zu work=%llu allocated=%llu peak=%llu\n",mode,sites,
                    (unsigned long long)census.work,(unsigned long long)census.allocated_bytes,(unsigned long long)census.peak_bytes);
            } else CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected);
            CHECK(rp_stats(&context).live_bytes==baseline);
            rp_fail_at=SIZE_MAX;CHECK(initial_ordinary_resource_operation(&context,mode)==XR_XIR_OK);
            rp_owner_free(&context,baseline);rp_balanced(physical);
        }
        for (uint32_t axis=0;axis<3;++axis) {
            RootParameterMark physical=rp_mark();XrCompileResourceLimits caps=rp_caps();
            if (axis==0) caps.allocated_bytes=census.allocated_bytes;
            if (axis==1) caps.live_bytes=census.peak_bytes;
            if (axis==2) caps.work=census.work;
            for (uint32_t minus=0;minus<2;++minus) {
                XrCompileResourceLimits trial=caps;
                if (minus) { if (axis==0) --trial.allocated_bytes;if (axis==1) --trial.live_bytes;if (axis==2) --trial.work; }
                rp_fail_at=SIZE_MAX;rp_injected=false;XrXirCompileContext context=rp_owner(trial);
                uint64_t baseline=rp_stats(&context).live_bytes;
                CHECK(initial_ordinary_resource_operation(&context,mode)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
                rp_owner_free(&context,baseline);
            }
            rp_balanced(physical);
        }
    }
}
#endif // XIR_INITIAL_ORDINARY_MATERIAL_CASES_H
