/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_metadata_domain_bridge_cases.h - Authentic common declaration proof adapters
 */
#ifndef XIR_METADATA_DOMAIN_BRIDGE_CASES_H
#define XIR_METADATA_DOMAIN_BRIDGE_CASES_H
#include "xir_constraint_packet_fixture.h"

/* Each relation is evaluated twice on the same finite ledger with fresh
 * query scratch. Equal fees show that both entries reach the same proof core. */
static XrXirStatus conditional_metadata_proof(const XrXirCompileContext *context,
    const XrXirModule *module,bool oracle) {
    XirMetadataDomain domain=xir_metadata_domain_from_module(module);
    XrXirProofContext raw={module,{XR_XIR_CONTEXT_FUNCTION,2,0}};
    XirMetadataProofContext metadata={&domain,raw.owner};
    XrCompileResourceStats a=rp_stats(context);
    XrXirStatus status=xr_xir_compile_context_constraints_verify(context,&raw);
    XrCompileResourceStats b=rp_stats(context);
    if (status==XR_XIR_OK) status=xir_metadata_context_constraints_verify(context,&metadata);
    XrCompileResourceStats c=rp_stats(context);
    if (status==XR_XIR_OK && oracle) {
        CHECK(c.work-b.work==b.work-a.work && c.allocated_bytes-b.allocated_bytes==b.allocated_bytes-a.allocated_bytes);
        CHECK(c.allocation_count-b.allocation_count==b.allocation_count-a.allocation_count);
    }
    XirConstraintScratch scratch={context->resources,NULL};
    if (status==XR_XIR_OK) status=xr_xir_compile_type_use_verify_scratch(context,&raw,XR_XIR_TYPE_PARAMETER_BASE,&scratch);
    xr_xir_constraint_scratch_free(&scratch);scratch=(XirConstraintScratch){context->resources,NULL};
    if (status==XR_XIR_OK) status=xir_metadata_type_use_verify_scratch(context,&metadata,XR_XIR_TYPE_PARAMETER_BASE,&scratch);
    xr_xir_constraint_scratch_free(&scratch);
    if (status==XR_XIR_OK) status=xr_xir_compile_type_markers_prove(context,&raw,XR_XIR_TYPE_PARAMETER_BASE,XR_XIR_CONSTRAINT_SENDABLE);
    if (status==XR_XIR_OK) status=xir_metadata_type_markers_prove(context,&metadata,XR_XIR_TYPE_PARAMETER_BASE,XR_XIR_CONSTRAINT_SENDABLE);
    if (status==XR_XIR_OK && oracle) {
        CHECK(domain.function_count==3 && module->generics[2].parameter_count==1);
        metadata.owner.declaration=3;
        CHECK(xir_metadata_context_constraints_verify(context,&metadata)==XR_XIR_BAD_STRUCTURE);
        metadata.owner=raw.owner;
        CHECK(xir_metadata_type_markers_prove(context,&metadata,(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1),XR_XIR_CONSTRAINT_SENDABLE)==XR_XIR_BAD_TYPE);
        XrXirModule absent=*module;absent.functions=NULL;
        raw.module=&absent;
        CHECK(xr_xir_compile_context_constraints_verify(context,&raw)==XR_XIR_BAD_STRUCTURE);
        raw.owner=(XrXirDeclarationContext){XR_XIR_CONTEXT_NOMINAL,0,0};
        CHECK(xr_xir_compile_context_constraints_verify(context,&raw)==XR_XIR_OK);
        raw.owner=(XrXirDeclarationContext){XR_XIR_CONTEXT_CLOSED,0,0};
        CHECK(xr_xir_compile_type_markers_prove(context,&raw,XR_XIR_I64,XR_XIR_CONSTRAINT_SENDABLE)==XR_XIR_OK);
    }
    return status;
}

static XrXirStatus conditional_metadata_roots(const XrXirCompileContext *context,
    const XrXirModule *module,bool oracle) {
    if (module->function_count!=4) return XR_XIR_BAD_STRUCTURE;
    XrXirRootEffects ar[4]={0},br[4]={0};
    XrXirRootEffectWitness aw[4]={0},bw[4]={0},au[4]={0},bu[4]={0};
    XrXirEffects raw={.count=4,.outcomes_work={.root=ar,.root_witnesses=aw,.unresolved_witnesses=au}};
    XrXirEffects metadata={.count=4,.outcomes_work={.root=br,.root_witnesses=bw,.unresolved_witnesses=bu}};
    XirMetadataDomain domain=xir_metadata_domain_from_module(module);
    XrXirStatus status=XR_XIR_OK;
    for (uint32_t f=0;f<4 && status==XR_XIR_OK;++f) {
        const XrXirFunction *body=&module->functions[f];
        for (uint32_t i=0;i<body->instruction_count && status==XR_XIR_OK;++i) {
            const XrXirInstruction *op=&body->instructions[i];
            EffectRootSeedRecord record={&domain,op,f,i,XR_XIR_UNIT};
            if (op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT)
                record.callable_type=xr_xir_operand_type(body,(uint32_t)op->immediate);
            status=effect_root_seed(module,&raw,f,i,context);
            if (status==XR_XIR_OK) status=effect_root_seed_record(&record,&metadata,context);
        }
    }
    if (status==XR_XIR_OK && oracle) {
        CHECK(!memcmp(ar,br,sizeof(ar)) && !memcmp(aw,bw,sizeof(aw)) && !memcmp(au,bu,sizeof(au)));
        CHECK(ar[0].requires_root && !ar[0].unresolved && ar[1].unresolved);
        CHECK(!ar[2].requires_root && !ar[2].unresolved && ar[3].requires_root);
        XrXirInstruction invalid={.op=(XrXirOp)UINT16_MAX};
        EffectRootSeedRecord record={&domain,&invalid,0,0,XR_XIR_UNIT};
        CHECK(effect_root_seed_record(&record,&metadata,context)==XR_XIR_BAD_STRUCTURE);
        record.function=4;CHECK(effect_root_seed_record(&record,&metadata,context)==XR_XIR_BAD_STRUCTURE);
    }
    return status;
}

static XrXirStatus conditional_metadata_default(const XrXirCompileContext *context,
    bool oracle) {
    XrXirInstruction call[2]={{.op=XR_XIR_CALL_DEFAULT,.type=XR_XIR_I64,.targets={1,0}},
        {.op=XR_XIR_RETURN,.args={0,0}}};
    XrXirInstruction value[2]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_RETURN,.args={0,0}}};
    XrXirInstruction done={.op=XR_XIR_RETURN};XrXirType parameter=XR_XIR_I64;
    XrXirBlock two={.count=2},one={.count=1};
    XrXirFunction functions[4]={
        {.name="entry",.name_length=5,.result=XR_XIR_I64,.blocks=&two,.block_count=1,.instructions=call,.instruction_count=2},
        {.name="owner",.name_length=5,.parameters=&parameter,.parameter_count=1,.result=XR_XIR_I64,
         .blocks=&one,.block_count=1,.instructions=&done,.instruction_count=1},
        {.name="default",.name_length=7,.result=XR_XIR_I64,.blocks=&two,.block_count=1,.instructions=value,.instruction_count=2},
        {.name="init",.name_length=4,.result=XR_XIR_UNIT,.blocks=&one,.block_count=1,.instructions=&done,.instruction_count=1}};
    XrXirFunctionIdentity identities[4]={0};XrXirSourceModule source={"metadata",8,NULL,0,3, 0};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities};
    XrXirDefaultBinding binding={XR_XIR_DEFAULT_PARAMETER,1,0,2};
    XrXirDefaultTable defaults={&binding,1};
    XrXirModule built={.stage=XR_XIR_BUILT,.functions=functions,.function_count=4,
        .declarations=&declarations,.defaults=&defaults,.linkage_kind=XR_XIR_PROGRAM};
    XrXirArtifact *checked=NULL;XrXirStatus status=xir_fixture_check(context,&built,&checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(checked,NULL);
    const XrXirModule *module=xr_xir_compile_artifact_module(checked);
    XirMetadataDomain domain=xir_metadata_domain_from_module(module);
    const XrXirDefaultBinding *a=NULL,*b=NULL;
    if (status==XR_XIR_OK) status=xr_xir_compile_default_lookup(context,module,1,0,&a);
    if (status==XR_XIR_OK) status=xir_metadata_default_lookup(context,&domain,1,0,&b);
    if (status==XR_XIR_OK) status=xr_xir_compile_call_type_matches(context,module,0,&module->functions[0].instructions[0],XR_XIR_I64,XR_XIR_I64);
    if (status==XR_XIR_OK) status=xir_metadata_call_type_matches(context,&domain,0,&module->functions[0].instructions[0],XR_XIR_I64,XR_XIR_I64);
    if (status==XR_XIR_OK && oracle) {
        CHECK(a==b && a && a->owner==1 && a->ordinal==0 && a->function==2);
        CHECK(xir_metadata_call_type_matches(context,&domain,0,&module->functions[0].instructions[0],XR_XIR_I64,XR_XIR_BOOL)==XR_XIR_BAD_TYPE);
        CHECK(xir_metadata_default_lookup(context,&domain,1,1,&b)==XR_XIR_OK && !b);
        XrXirDefaultTable invalid={NULL,1};domain.defaults=&invalid;b=a;
        CHECK(xir_metadata_default_lookup(context,&domain,1,0,&b)==XR_XIR_BAD_STRUCTURE && b==a);
        XrXirInstruction invalid_call=module->functions[0].instructions[0];invalid_call.type_arguments[1]=1;
        CHECK(xir_metadata_call_type_matches(context,&domain,0,&invalid_call,XR_XIR_I64,XR_XIR_I64)==XR_XIR_BAD_STRUCTURE);
    }
    xr_xir_compile_artifact_free(checked);return status;
}

/* Independent literal expectations cover every function head/body field and
 * every declaration child used by the proof. No clone helper builds this
 * expected snapshot, and both generic and nominal/interface facts are checked. */
static void conditional_metadata_snapshot(const XrXirCompileContext *context,
    const EffectBodyMaterial *module) {
    CHECK(module->stage==XR_XIR_CHECKED && effect_body_count(module)==3 && module->functions &&
        module->declarations && module->declarations->functions && module->generics &&
        !module->domain.defaults && module->linkage_kind==XR_XIR_PROGRAM);
    const char *names[3]={"entry","init","generic"};uint32_t lengths[3]={5,4,7};
    const XrXirInstruction expected[2]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=41},
        {.op=XR_XIR_RETURN}};
    const XrXirFunctionIdentity identity={0};
    for (uint32_t f=0;f<3;++f) {
        const EffectBodyFunction *body=&module->functions[f];
        CHECK(body->name_length==lengths[f] && !memcmp(body->name,names[f],lengths[f]) &&
            !body->parameters && !body->parameter_count && !body->operands && !body->operand_count &&
            body->result==(f?XR_XIR_UNIT:XR_XIR_I64) && body->block_count==1 &&
            body->instruction_count==(f?1u:2u) && body->blocks && body->runs);
        const XrXirBlock block={.count=f?1u:2u};
        CHECK(!memcmp(body->blocks,&block,sizeof(block)));
        EffectBodyRead read=effect_body_compile_read(context);
        for (uint32_t i=0;i<body->instruction_count;++i) {
            XrXirInstruction actual;
            CHECK(effect_body_instruction_get(&read,body,i,&actual)==XR_XIR_OK);
            CHECK(!memcmp(&actual,&expected[f?1:i],sizeof(actual)));
        }
        CHECK(!memcmp(&module->declarations->functions[f],&identity,sizeof(identity)));
        const XrXirGeneric *generic=&module->generics[f];
        CHECK(generic->parameter_count==(f==2?1u:0u) && !generic->arguments &&
            !generic->argument_count && !generic->parameter_kinds);
        if (f!=2) CHECK(!generic->constraints);
    }
    const XrXirDeclarations *d=module->declarations;
    CHECK(d && d->module_count==1 && d->modules && d->functions && !d->slots && !d->slot_count &&
        !d->literals && !d->literal_count && !d->root_module && !d->entry_function && !d->implementations);
    CHECK(d->modules[0].name_length==5 && !memcmp(d->modules[0].name,"alpha",5) &&
        !d->modules[0].dependencies && !d->modules[0].dependency_count && d->modules[0].initializer==1);
    const XrXirTypes *types=module->domain.types;
    CHECK(types && !types->count && !types->nodes && types->nominals && types->nominals->count==1 &&
        types->nominals->declarations && types->interfaces && types->interfaces->count==2 && types->interfaces->declarations);
    const XrXirNominalDeclaration *nominal=types->nominals->declarations;
    CHECK(nominal->module.length==5 && !memcmp(nominal->module.bytes,"alpha",5) &&
        nominal->name.length==3 && !memcmp(nominal->name.bytes,"Box",3) && nominal->exported==1 &&
        nominal->parameter_count==1 && !nominal->fields && !nominal->field_count &&
        nominal->kind==XR_XIR_NOMINAL_STRUCT && !nominal->variants && !nominal->variant_count &&
        !nominal->flags && !nominal->native.native_id);
    const uint8_t fingerprint[32]={0};
    CHECK(!memcmp(nominal->native.source_fingerprint,fingerprint,sizeof(fingerprint)));
    if (types->nominals->identities) {
        const XrXirNominalIdentity *n=types->nominals->identities;
        CHECK(n->module.length==5 && !memcmp(n->module.bytes,"alpha",5) && n->name.length==3 &&
            !memcmp(n->name.bytes,"Box",3) && n->exported==1 && n->arity==1 && !n->fields &&
            !n->field_count && n->kind==XR_XIR_NOMINAL_STRUCT && !n->variants && !n->variant_count &&
            !n->flags && !n->native.native_id && !memcmp(n->native.source_fingerprint,fingerprint,32));
    }
    const XrXirConstraint *facts[3]={module->generics[2].constraints,nominal->constraints,
        types->interfaces->declarations[1].constraints};
    for (uint32_t c=0;c<3;++c) {
        CHECK(facts[c] && facts[c]->markers==XR_XIR_CONSTRAINT_SENDABLE && facts[c]->interface_count==1 && facts[c]->interfaces);
        const XrXirInterfaceApplication *application=facts[c]->interfaces;
        CHECK(!application->declaration && application->argument_count==1 && application->arguments &&
            application->arguments[0]==XR_XIR_TYPE_PARAMETER_BASE);
    }
    for (uint32_t i=0;i<2;++i) {
        const XrXirInterfaceDeclaration *interface=&types->interfaces->declarations[i];
        CHECK(interface->module.length==5 && !memcmp(interface->module.bytes,"alpha",5) &&
            interface->name.length==(i?5u:4u) && !memcmp(interface->name.bytes,i?"Needs":"Base",i?5:4) &&
            interface->exported==1 && interface->parameter_count==1 && interface->constraints &&
            !interface->parents && !interface->parent_count && !interface->methods && !interface->method_count);
        if (!i) CHECK(!interface->constraints[0].markers && !interface->constraints[0].interfaces &&
            !interface->constraints[0].interface_count);
    }
}

/* The opaque metadata owner is queried after the actual raw artifact dies.
 * These requests reach the same common proof core without a Module alias. */
static XrXirStatus conditional_metadata_owned_proof(const XrXirCompileContext *context,
    const EffectBodyMaterial *body,bool oracle) {
    XirMetadataProofContext proof={&body->domain,{XR_XIR_CONTEXT_FUNCTION,2,0}};
    XrXirStatus status=xir_metadata_context_constraints_verify(context,&proof);
    XirConstraintScratch scratch={context->resources,NULL};
    if (status==XR_XIR_OK) status=xir_metadata_type_use_verify_scratch(context,&proof,
        XR_XIR_TYPE_PARAMETER_BASE,&scratch);
    xr_xir_constraint_scratch_free(&scratch);
    if (status==XR_XIR_OK) status=xir_metadata_type_markers_prove(context,&proof,
        XR_XIR_TYPE_PARAMETER_BASE,XR_XIR_CONSTRAINT_SENDABLE);
    if (status==XR_XIR_OK && oracle) {
        CHECK(body->domain.function_count==3 && body->generics[2].parameter_count==1);
        proof.owner.declaration=3;
        CHECK(xir_metadata_context_constraints_verify(context,&proof)==XR_XIR_BAD_STRUCTURE);
        proof.owner.declaration=2;
        CHECK(xir_metadata_type_markers_prove(context,&proof,
            (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1),XR_XIR_CONSTRAINT_SENDABLE)==XR_XIR_BAD_TYPE);
        /* The original missing-function, nominal and closed controls now
         * use the owned metadata domain after the producer has died. */
        XirMetadataDomain absent=body->domain;absent.function_count=0;
        proof.domain=&absent;
        CHECK(xir_metadata_context_constraints_verify(context,&proof)==XR_XIR_BAD_STRUCTURE);
        proof.owner=(XrXirDeclarationContext){XR_XIR_CONTEXT_NOMINAL,0,0};
        CHECK(xir_metadata_context_constraints_verify(context,&proof)==XR_XIR_OK);
        proof.owner=(XrXirDeclarationContext){XR_XIR_CONTEXT_CLOSED,0,0};
        CHECK(xir_metadata_type_markers_prove(context,&proof,XR_XIR_I64,
            XR_XIR_CONSTRAINT_SENDABLE)==XR_XIR_OK);
    }
    return status;
}

/* The producer and its raw declaration children die before the copied domain
 * is queried. Only the existing receiving owner retains these records. */
static XrXirStatus conditional_metadata_case(const XrXirCompileContext *context,bool oracle) {
    XrXirArtifact *checked=NULL;
    XrXirStatus status=constraint_packet_fixture(context,XR_XIR_CONSTRAINT_SENDABLE,&checked);
    if (status==XR_XIR_OK && oracle) constraint_packet_owned(checked,XR_XIR_CONSTRAINT_SENDABLE);
    EffectBodyMaterial *copy=NULL;
    if (status==XR_XIR_OK)
        status=effect_body_material_create(context,xr_xir_compile_artifact_module(checked),&copy);
    EffectTerms comparison={.remaining=context};
    if (status==XR_XIR_OK) comparison.types=copy->terms.types;
    if (status==XR_XIR_OK) status=effect_terms_snapshot_match(&comparison,
        xr_xir_compile_artifact_module(checked)->types,xr_xir_compile_artifact_module(checked)->types->count);
    effect_terms_free(&comparison);
    if (status==XR_XIR_OK) status=effect_body_source_match(context,copy,
        xr_xir_compile_artifact_module(checked));
    if (status==XR_XIR_OK) status=conditional_metadata_proof(context,
        xr_xir_compile_artifact_module(checked),oracle);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    if (status==XR_XIR_OK && oracle) conditional_metadata_snapshot(context,copy);
    if (status==XR_XIR_OK) status=conditional_metadata_owned_proof(context,copy,oracle);
    effect_body_material_free(copy);
    if (status==XR_XIR_OK) status=conditional_metadata_default(context,oracle);
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,1);
    if (status==XR_XIR_OK) status=xir_fixture_check(context,&fixture.module,&checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(checked,NULL);
    if (status==XR_XIR_OK) status=conditional_metadata_roots(context,xr_xir_compile_artifact_module(checked),oracle);
    xr_xir_compile_artifact_free(checked);return status;
}

static void conditional_metadata_literals(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    CHECK(conditional_metadata_case(&context,true)==XR_XIR_OK);
    rp_owner_free(&context,baseline);rp_balanced(physical);
}

static void conditional_metadata_resources(void) {
    size_t sites=0;XrCompileResourceStats single={0};
    for (size_t pass=0;pass<=sites;++pass) {
        RootParameterMark physical=rp_mark();rp_fail_at=SIZE_MAX;rp_attempts=0;rp_injected=false;
        XrXirCompileContext context=rp_owner(rp_caps());XrCompileResourceStats entry=rp_stats(&context);
        RootParameterMark retained=rp_mark();XrCompileResources *identity=context.resources;
        rp_attempts=0;rp_fail_at=pass?pass-1:SIZE_MAX;
        XrXirStatus status=conditional_metadata_case(&context,false);
        if (!pass) { CHECK(status==XR_XIR_OK);sites=rp_attempts;single=rp_stats(&context); }
        else {
            CHECK(rp_injected && status==XR_XIR_OUT_OF_MEMORY);
            ConditionalRetryTrial retry={.which=72,.ordinal=pass-1,.sites=sites,.resources=identity,
                .initial=entry,.single=single,.retained=retained,.first=status};
            conditional_retry_operation(&context,&retry);
        }
        rp_fail_at=SIZE_MAX;rp_owner_free(&context,entry.live_bytes);rp_balanced(physical);
    }
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;size_t attempts=rp_attempts;
    CHECK(conditional_metadata_case(&context,false)==XR_XIR_OK);
    XrCompileResourceStats census=rp_stats(&context);size_t census_sites=rp_attempts-attempts;
    rp_owner_free(&context,baseline);rp_balanced(physical);
    uint64_t measured[3]={census.allocated_bytes,census.peak_bytes,census.work};
    for (uint32_t axis=0;axis<3;++axis) for (uint32_t pass=0;pass<2;++pass) {
        XrCompileResourceLimits limits=rp_caps();uint64_t value=measured[axis]-(pass?0:1);
        if (!axis) limits.allocated_bytes=value;else if (axis==1) limits.live_bytes=value;else limits.work=value;
        physical=rp_mark();context=rp_owner(limits);XrCompileResourceStats entry=rp_stats(&context);
        RootParameterMark retained=rp_mark();XrCompileResources *identity=context.resources;
        XrXirStatus status=conditional_metadata_case(&context,false);
        CHECK(status==(pass?XR_XIR_OK:XR_XIR_BUDGET));
        ConditionalRetryTrial retry={.which=72,.ordinal=SIZE_MAX,.sites=census_sites,.resources=identity,
            .initial=entry,.single=census,.retained=retained,.first=status};
        conditional_retry_operation(&context,&retry);
        rp_owner_free(&context,entry.live_bytes);rp_balanced(physical);
    }
}
#endif // XIR_METADATA_DOMAIN_BRIDGE_CASES_H
