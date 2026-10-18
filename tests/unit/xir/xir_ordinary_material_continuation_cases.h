/* Complete same-operation ownership; no public artifact result is cached. */
#ifndef XIR_ORDINARY_MATERIAL_CONTINUATION_CASES_H
#define XIR_ORDINARY_MATERIAL_CONTINUATION_CASES_H

typedef struct OrdinaryMaterialFixture {
    XrXirTypeNode nodes[2];
    XrXirType parameter;
    XrXirTypes types;
    XrXirInstruction operations[2],initializer;
    XrXirBlock blocks[2];
    XrXirFunction functions[2];
    XrXirFunctionIdentity identities[2];
    XrXirSourceModule source_module;
    XrXirDeclarations declarations;
    XrXirModule source;
} OrdinaryMaterialFixture;
static void ordinary_material_fixture(OrdinaryMaterialFixture *f) {
    memset(f,0,sizeof(*f));
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64};
    f->types=(XrXirTypes){.nodes=f->nodes,.count=1};
    f->operations[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
    f->operations[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0,0}};
    f->initializer=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->blocks[0]=(XrXirBlock){.first=0,.count=2};
    f->blocks[1]=(XrXirBlock){.first=0,.count=1};
    f->functions[0]=(XrXirFunction){.name="answer",.name_length=6,.result=XR_XIR_I64,
        .instructions=f->operations,.instruction_count=2,.blocks=&f->blocks[0],.block_count=1};
    f->functions[1]=(XrXirFunction){.name="init",.name_length=4,.result=XR_XIR_UNIT,
        .instructions=&f->initializer,.instruction_count=1,.blocks=&f->blocks[1],.block_count=1};
    f->source_module=(XrXirSourceModule){"same",4,NULL,0,1, 0,XR_XIR_MODULE_FRESH_STORAGE};
    f->declarations=(XrXirDeclarations){.modules=&f->source_module,.module_count=1,.functions=f->identities};
    f->source=(XrXirModule){.stage=XR_XIR_BUILT,.types=&f->types,.functions=f->functions,.function_count=2,
        .declarations=&f->declarations,.linkage_kind=XR_XIR_PROGRAM};
}

/* The formal itself and its physical read supply nonempty Cell provenance;
 * merely keeping an unused Cell descriptor in the pool cannot satisfy this. */
static void ordinary_material_cell_fixture(OrdinaryMaterialFixture *f) {
    ordinary_material_fixture(f);f->parameter=(XrXirType)256;
    f->functions[0].parameters=&f->parameter;f->functions[0].parameter_count=1;
    f->operations[0]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=XR_XIR_I64,.args={0,0}};
    f->operations[1].args[0]=1;
}

/* A separate full private dense input owns all rewritten instruction memory. */
static XrXirStatus ordinary_material_dense(const XrXirCompileContext *context,
    const OrdinaryMaterialFixture *fixture,EffectOrdinaryContexts *ordinary,EffectOrdinaryContexts **output) {
    XrXirStatus status=XR_XIR_OK;
    EffectOrdinaryContexts *dense=xir_compile_calloc(context,1,sizeof(*dense),&status);
    if (!dense) return status;
    dense->dense=true;dense->terms.remaining=context;dense->base_count=2;dense->source_type_count=1;
    dense->uses=(XrXirEffects){.resources=context->resources,.count=2};
    status=effect_terms_seed(&dense->terms,&ordinary->terms.types);
    if (status==XR_XIR_OK) status=effect_ordinary_capacity(dense,2);
    for (uint32_t f=0;f<2 && status==XR_XIR_OK;++f) {
        dense->nodes[f]=ordinary->nodes[f];dense->count=f+1;
        status=effect_ordinary_function(dense,&fixture->source,f);
        if (status==XR_XIR_OK && !xir_compile_work(context,sizeof(const XrXirType *))) status=XR_XIR_BUDGET;
        if (status==XR_XIR_OK) dense->nodes[f].physical_types=dense->nodes[f].source_parameters;
    }
    if (status!=XR_XIR_OK) { effect_ordinary_free(dense);return status; }
    *output=dense;return XR_XIR_OK;
}

/* Read through the same complete owned adapter after the producer owners
 * are gone. Fresh derivation compares every stored provider fact and bitmap. */
static XrXirStatus ordinary_material_cell_match(const XrXirCompileContext *context,
    EffectBodyMaterial *material,bool tracked) {
    XrXirCellOriginView origin={0};
    XrXirStatus status=xr_xir_compile_cell_origin_view(context,material->cells,0,0,&origin);
    if (status==XR_XIR_OK && tracked) {
        CHECK(!origin.intrinsic_mask && origin.word_count==1 && origin.parameter_count==1 &&
            origin.parameters[0]==0 && origin.dependencies[0]==UINT64_C(1));
        CHECK(xr_xir_cell_provenance_origin(material->cells,0,0)==XR_XIR_CELL_ORIGIN_SCOPED);
    } else if (status==XR_XIR_OK) CHECK(!origin.intrinsic_mask && !origin.word_count && !origin.parameter_count);
    EffectCellBodyAdapter adapter={0};XrXirCellProofBodyRead read={0};
    if (status==XR_XIR_OK) status=effect_cell_body_owned(context,material,&adapter,&read);
    if (status==XR_XIR_OK) status=xr_xir_compile_cell_provenance_body_matches(context,material->cells,&read,NULL);
    return status;
}

static XrXirStatus ordinary_material_operation(const XrXirCompileContext *context,uint32_t mode) {
    OrdinaryMaterialFixture f;
    if (mode==3) ordinary_material_cell_fixture(&f);else ordinary_material_fixture(&f);
    EffectOrdinaryContexts *ordinary=NULL,*dense=NULL;
    EffectBodyMaterial *material=NULL;EffectOrdinaryView view={0};
    XrXirEffects refinement={.resources=context->resources,.count=2};
    XrXirStatus status=effect_ordinary_build(context,&f.source,NULL,NULL,&ordinary);
    if (status==XR_XIR_OK) status=ordinary_material_dense(context,&f,ordinary,&dense);
    if (status==XR_XIR_OK) status=effect_ordinary_view(dense,&f.source,&view);
    EffectBodyMaterial *basis=ordinary?ordinary->uses.generation:NULL;
    if (status==XR_XIR_OK) {
        CHECK(basis && basis->references==1 && dense->functions[0].instructions!=ordinary->functions[0].instructions);
        view.input.basis=ordinary;
        if (mode==1) ((XrXirInstruction *)dense->functions[0].instructions)[0].immediate=43;
        if (mode==2) status=xir_effects_refinement_capture(context,&view.module,&refinement);
        if (mode==3) {
            CHECK(xr_xir_cell_provenance_origin(basis->cells,0,0)==XR_XIR_CELL_ORIGIN_SCOPED);
            /* A legitimate new physical vector and complete instruction row
             * require the original writer, including a new nontracked proof. */
            ((XrXirType *)dense->functions[0].parameters)[0]=XR_XIR_I64;
            ((XrXirInstruction *)dense->functions[0].instructions)[0]=
                (XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
        }
        if (status==XR_XIR_OK)
            status=effect_body_material_generation_input(context,&view.module,refinement.refinement,&view.input,&material);
    }
    if (status==XR_XIR_OK) {
        CHECK(material!=basis && (mode?basis->references==1 && !material->backbone:
            basis->references==2 && material->backbone==basis));
        if (!mode) CHECK(material->functions==basis->functions && material->cells==basis->cells &&
            material->physical==basis->physical && material->declared!=basis->declared &&
            material->domain.types==&material->terms.types &&
            material->declared->functions!=basis->declared->functions);
        if (mode==3) CHECK(material->cells!=basis->cells && material->physical!=basis->physical &&
            material->functions[0].parameters[0]==XR_XIR_I64 &&
            xr_xir_cell_provenance_origin(material->cells,0,0)==0);
        EffectBodyRequest read=effect_body_owned_request(context,material);
        XrXirInstruction op={0};const XrXirInstruction *actual=NULL;
        status=effect_body_request_instruction(&read,0,0,&op,&actual);
        if (status==XR_XIR_OK) CHECK(actual->immediate==(mode==1?43:42));
        effect_ordinary_free(dense);dense=NULL;effect_ordinary_free(ordinary);ordinary=NULL;
        effect_refinement_free(refinement.refinement);refinement.refinement=NULL;
        memset(&f,0xCE,sizeof(f));CHECK(material->references==1);
        if (status==XR_XIR_OK) status=effect_body_initial_roles_match(context,material,0);
        if (status==XR_XIR_OK) status=effect_body_request_instruction(&read,0,0,&op,&actual);
        if (status==XR_XIR_OK) CHECK(actual->immediate==(mode==1?43:42));
        if (status==XR_XIR_OK && mode==3) status=ordinary_material_cell_match(context,material,false);
    }
    effect_refinement_free(refinement.refinement);
    effect_body_material_free(material);effect_ordinary_free(dense);effect_ordinary_free(ordinary);
    return status;
}

/* This full execution owner reaches both parameter derivation and ROOT
 * publication. A real callable formal supplies a nonempty pre-bottom record;
 * no synthetic certificate or hand-supplied permission owner is constructed. */
static XrXirStatus ordinary_material_owner_operation(const XrXirCompileContext *context,uint32_t mode) {
    OrdinaryMaterialFixture f;
    if (mode==2) ordinary_material_cell_fixture(&f);else ordinary_material_fixture(&f);
    if (mode==1) {
        f.nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,
            .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
        f.types.count=2;f.parameter=(XrXirType)257;
        f.functions[0].parameters=&f.parameter;f.functions[0].parameter_count=1;
        f.operations[1].args[0]=1;
    }
    EffectContextOwner *owner=NULL;
    XrXirStatus status=effect_context_owner_build(context,&f.source,NULL,NULL,&owner);
    EffectContextForest *forest=NULL;
    if (status==XR_XIR_OK) {
        EffectBodyMaterial *basis=owner->ordinary->uses.generation;
        EffectInvocationCertificate *certificate=owner->forest->invocations;
        EffectBodyMaterial *holder=certificate->body;
        CHECK(holder!=basis && holder->backbone==basis && basis->references==2 && holder->references==1);
        CHECK(holder->functions==basis->functions && holder->cells==basis->cells &&
            holder->physical==basis->physical && certificate->declared==holder->declared &&
            holder->declared!=basis->declared && holder->declared->functions!=basis->declared->functions);
        CHECK(holder->domain.types==&holder->terms.types && !holder->terms.remaining &&
            holder->terms.memory==basis->terms.memory);
        if (mode==1) {
            CHECK(basis->declared->functions[0].count==1 && holder->declared->functions[0].count==1);
            const EffectInvocationValueBound *original=basis->declared->functions[0].bounds;
            EffectInvocationValueBound *current=holder->declared->functions[0].bounds;
            CHECK(current!=original && original[0].value==0 && original[0].callable &&
                original[0].mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED && current[0].mask==original[0].mask);
            /* Temporary test mutation proves storage separation only. Restore
             * it before any proof consumer; it cannot be used as admission. */
            current[0].mask=0;CHECK(original[0].mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
            current[0].mask=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
        }
        status=effect_invocation_public_gate(context,certificate);
        forest=owner->forest;owner->forest=NULL;
        effect_context_owner_free(owner);owner=NULL;
        memset(&f,0xCE,sizeof(f));CHECK(basis->references==1 && holder->references==1);
        EffectBodyRequest read=effect_body_owned_request(context,holder);
        XrXirInstruction storage={0};const XrXirInstruction *actual=NULL;
        if (status==XR_XIR_OK) status=effect_body_request_instruction(&read,0,0,&storage,&actual);
        if (status==XR_XIR_OK) CHECK(mode==2 ? actual->op==XR_XIR_CELL_READ && actual->args[0]==0 : actual->immediate==42);
        if (status==XR_XIR_OK && mode==2) status=ordinary_material_cell_match(context,holder,true);
        if (status==XR_XIR_OK) status=effect_body_initial_roles_match(context,holder,0);
        if (status==XR_XIR_OK) status=effect_invocation_public_gate(context,certificate);
    }
    effect_context_forest_free(forest);effect_context_owner_free(owner);return status;
}

static XrXirStatus ordinary_material_resource_operation(const XrXirCompileContext *context,uint32_t mode) {
    return mode<4?ordinary_material_operation(context,mode):ordinary_material_owner_operation(context,mode-4);
}

/* Each physical word is independently perturbed against a fixed literal;
 * even unused words remain part of exact current request correspondence. */
static void ordinary_material_word_controls(const XrXirCompileContext *context,
    EffectOrdinaryView *view,EffectBodyMaterial *basis) {
    uint32_t references=basis->references;basis->references=UINT32_MAX;
    CHECK(effect_body_material_retain(context,basis)==XR_XIR_BUDGET && basis->references==UINT32_MAX);
    basis->references=references;
    XrXirInstruction *operation=(XrXirInstruction *)view->module.functions[0].instructions;
    XrXirInstruction original=operation[0];
    for (uint32_t field=0;field<10;++field) {
        operation[0]=original;
        switch (field) {
            case 0: operation[0].op=XR_XIR_CONST_FLOAT;break;
            case 1: operation[0].type=XR_XIR_U64;break;
            case 2: operation[0].args[0]=UINT32_C(0x10203040);break;
            case 3: operation[0].args[1]=UINT32_C(0x89ABCDEF);break;
            case 4: operation[0].targets[0]=UINT32_C(0x10203040);break;
            case 5: operation[0].targets[1]=UINT32_C(0x89ABCDEF);break;
            case 6: operation[0].immediate=43;break;
            case 7: operation[0].immediate=INT64_C(4294967338);break;
            case 8: operation[0].type_arguments[0]=UINT32_C(0x10203040);break;
            case 9: operation[0].type_arguments[1]=UINT32_C(0x89ABCDEF);break;
        }
        bool same=true;
        CHECK(effect_body_ordinary_compare(context,basis,&view->module,&view->input,&same)==XR_XIR_OK &&
            !same && basis->references==1);
    }
    operation[0]=original;
    bool same=true;
    CHECK(effect_body_ordinary_compare(context,basis,&view->module,&view->input,&same)==XR_XIR_OK && same);
}

static void ordinary_material_metadata_controls(const XrXirCompileContext *context,
    EffectOrdinaryView *view,EffectBodyMaterial *basis) {
    bool same=true;
    XrXirTypeNode *node=(XrXirTypeNode *)view->module.types->nodes;
    uint32_t flags=node[0].flags;node[0].flags=UINT32_C(0x80000000);
    CHECK(effect_body_ordinary_compare(context,basis,&view->module,&view->input,&same)==XR_XIR_OK && !same);
    node[0].flags=flags;
    XrXirFunction *function=&view->input.contexts->functions[0];
    XrXirType result=function->result;function->result=XR_XIR_BOOL;same=true;
    CHECK(effect_body_ordinary_compare(context,basis,&view->module,&view->input,&same)==XR_XIR_OK && !same);
    function->result=result;
    uint32_t length=function->name_length;function->name_length=0;same=true;
    CHECK(effect_body_ordinary_compare(context,basis,&view->module,&view->input,&same)==XR_XIR_OK && !same);
    function->name_length=length;
    XrXirGeneric *generics=(XrXirGeneric *)view->module.generics;
    uint32_t kinds[1]={XR_XIR_BINDER_TYPE};generics[0].parameter_kinds=kinds;same=true;
    CHECK(effect_body_ordinary_compare(context,basis,&view->module,&view->input,&same)==XR_XIR_OK && !same);
    generics[0].parameter_kinds=NULL;
    XrXirFunctionIdentity *identities=(XrXirFunctionIdentity *)view->module.declarations->functions;
    uint32_t module=identities[0].module;identities[0].module=1;same=true;
    CHECK(effect_body_ordinary_compare(context,basis,&view->module,&view->input,&same)==XR_XIR_OK && !same);
    identities[0].module=module;
    CHECK(basis->references==1);
}

static void ordinary_material_controls(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;OrdinaryMaterialFixture f;ordinary_material_fixture(&f);
    EffectOrdinaryContexts *ordinary=NULL,*dense=NULL;
    CHECK(effect_ordinary_build(&context,&f.source,NULL,NULL,&ordinary)==XR_XIR_OK);
    CHECK(ordinary_material_dense(&context,&f,ordinary,&dense)==XR_XIR_OK);
    EffectOrdinaryView view={0};CHECK(effect_ordinary_view(dense,&f.source,&view)==XR_XIR_OK);
    view.input.basis=ordinary;EffectBodyMaterial *basis=ordinary->uses.generation,*empty=NULL,*occupied=basis;
    XrCompileResourceStats initial=rp_stats(&context);size_t attempts=rp_attempts;
    CHECK(effect_body_material_generation_input(&context,&view.module,NULL,&view.input,&occupied)==XR_XIR_BAD_STRUCTURE && occupied==basis);
    CHECK(rp_stats(&context).work==initial.work && rp_attempts==attempts && basis->references==1);
    const XrXirInstruction *instructions=dense->functions[0].instructions;
    dense->functions[0].instructions=NULL;
    CHECK(effect_body_material_generation_input(&context,&view.module,NULL,&view.input,&empty)==XR_XIR_BAD_STRUCTURE && !empty && basis->references==1);
    dense->functions[0].instructions=instructions;
    uint32_t declaration=dense->nodes[0].declaration;dense->nodes[0].declaration=2;
    CHECK(effect_body_material_generation_input(&context,&view.module,NULL,&view.input,&empty)==XR_XIR_BAD_STRUCTURE && !empty && basis->references==1);
    dense->nodes[0].declaration=declaration;
    XrXirCompileContext foreign=rp_owner(rp_caps());XrCompileResourceStats foreign_before=rp_stats(&foreign);
    CHECK(effect_body_material_generation_input(&foreign,&view.module,NULL,&view.input,&empty)==XR_XIR_BAD_STRUCTURE && !empty);
    CHECK(rp_stats(&foreign).work==foreign_before.work && rp_stats(&foreign).live_bytes==foreign_before.live_bytes);
    rp_owner_free(&foreign,foreign_before.live_bytes);
    ordinary_material_word_controls(&context,&view,basis);
    ordinary_material_metadata_controls(&context,&view,basis);
    f.operations[0].immediate=99;
    CHECK(effect_body_material_generation_input(&context,&view.module,NULL,&view.input,&empty)==XR_XIR_BAD_STRUCTURE &&
        !empty && basis->references==1);
    f.operations[0].immediate=42;
    CHECK(effect_body_material_generation_input(&context,&view.module,NULL,&view.input,&empty)==XR_XIR_OK &&
        empty!=basis && empty->backbone==basis && basis->references==2 &&
        empty->declared!=basis->declared && empty->functions==basis->functions);
    effect_body_material_free(empty);CHECK(basis->references==1);
    uint64_t remaining=rp_caps().work-rp_stats(&context).work;
    uint64_t retain_work=5*sizeof(basis->references)+sizeof(basis)+2;
    CHECK(remaining>retain_work && xir_compile_work(&context,remaining-retain_work+1));
    CHECK(effect_body_material_retain(&context,basis)==XR_XIR_BUDGET && basis->references==1);
    effect_ordinary_free(dense);effect_ordinary_free(ordinary);
    rp_owner_free(&context,baseline);rp_balanced(physical);
}

/* A real full operation is measured afresh, every allocation ordinal is
 * injected and each trial retries on its unchanged cumulative resource owner. */
static void conditional_ordinary_material_resources(void) {
    ordinary_material_controls();
    for (uint32_t mode=0;mode<7;++mode) {
        size_t sites=0;XrCompileResourceStats census={0};
        for (size_t pass=0;pass<=sites;++pass) {
            RootParameterMark physical=rp_mark();rp_fail_at=SIZE_MAX;rp_injected=false;
            XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
            size_t start=rp_attempts;rp_fail_at=pass?start+pass-1:SIZE_MAX;
            XrXirStatus status=ordinary_material_resource_operation(&context,mode);
            if (!pass) {
                CHECK(status==XR_XIR_OK);sites=rp_attempts-start;census=rp_stats(&context);
                CHECK(sites && sites<512);
                printf("ORDINARY_MATERIAL_CENSUS mode=%u sites=%zu work=%llu allocated=%llu peak=%llu\n",mode,sites,
                    (unsigned long long)census.work,(unsigned long long)census.allocated_bytes,(unsigned long long)census.peak_bytes);
            } else CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected);
            CHECK(rp_stats(&context).live_bytes==baseline);
            rp_fail_at=SIZE_MAX;CHECK(ordinary_material_resource_operation(&context,mode)==XR_XIR_OK);
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
                CHECK(ordinary_material_resource_operation(&context,mode)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
                rp_owner_free(&context,baseline);
            }
            rp_balanced(physical);
        }
    }
}
#endif // XIR_ORDINARY_MATERIAL_CONTINUATION_CASES_H
