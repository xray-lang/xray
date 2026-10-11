/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_specialize_provenance_take_cases.h - Unique INSTANCE subtree receivers
 */
#ifndef XIR_SPECIALIZE_PROVENANCE_TAKE_CASES_H
#define XIR_SPECIALIZE_PROVENANCE_TAKE_CASES_H

static const char provenance_take_text[] =
    "fn identity<T>(value:T)->T{return value}\n"
    "export fn run()->i64{return identity<i64>(41)}\nprint(run())\n";

static XrXirStatus provenance_take_fixture(const XrXirCompileContext *context,
    XrXirArtifact **output) {
    XrCompilerSession *session=NULL;
    XrCompilerSessionStatus opened=xr_compile_session_new(context->resources,&session);
    if(opened!=XR_COMPILER_SESSION_OK)
        return opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"provenance-take",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,provenance_take_text,sizeof(provenance_take_text)-1};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};char *failure=NULL;
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&source,&diagnostic,&failure);
    xr_compile_session_free(session);
    if(status==XR_XIR_OK)status=xr_xir_compile_specialize(source.checked,output,NULL);
    xr_xir_compile_source_result_free(&source);xr_compile_resources_free(failure);
    return status;
}

/* The public borrowed transition remains an independent canonical peer. The
 * receiving tree must survive every producer and packet used to create it. */
static XrXirStatus provenance_take_operation(const XrXirCompileContext *context,void *opaque) {
    (void)opaque;
    XrXirArtifact *donor=NULL,*borrowed=NULL,*taken=NULL,*reread=NULL,*lowered=NULL;
    XrXirProvenance *owned=NULL;XrXirEffects *source_effects=NULL;
    XrXirCheckedPacket expected={0},actual={0};
    XrXirStatus status=provenance_take_fixture(context,&donor);
    if(status==XR_XIR_OK) {
        CHECK(donor->module.provenance && donor->module.provenance->kind==XR_XIR_EVIDENCE_INSTANCE);
        status=xr_xir_compile_recheck_v2(context,&donor->module,donor->construction,&borrowed,NULL);
    }
    if(status==XR_XIR_OK)
        status=xr_xir_compile_artifact_verify_owned_effects(donor->module.provenance->source,&source_effects,NULL);
    if(status==XR_XIR_OK) {
        XrXirModule view=donor->module;
        owned=(XrXirProvenance *)donor->module.provenance;donor->module.provenance=NULL;
        XrXirProvenance *identity=owned;
        status=xr_xir_compile_recheck_provenance_take(context,&view,
            &(XirProvenanceReceiving){donor->construction,source_effects},&owned,&taken,NULL);
        CHECK(!owned);
        if(status==XR_XIR_OK)CHECK(taken->module.provenance==identity);
    }
    xr_xir_compile_artifact_free(donor);donor=NULL;
    xr_xir_compile_effects_free(source_effects);source_effects=NULL;
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_write(borrowed,&expected,NULL);
    xr_xir_compile_artifact_free(borrowed);borrowed=NULL;
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(taken,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_write(taken,&actual,NULL);
    if(status==XR_XIR_OK)CHECK(expected.length==actual.length && !memcmp(expected.bytes,actual.bytes,actual.length));
    xr_xir_compile_artifact_free(taken);taken=NULL;
    xr_xir_compile_checked_packet_free(&expected);
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_read(context,actual.bytes,actual.length,&reread,NULL);
    xr_xir_compile_checked_packet_free(&actual);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if(status==XR_XIR_OK)status=xr_xir_compile_lower_take(&reread,&target,&lowered,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(lowered,NULL);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(reread);
    xr_xir_compile_artifact_free(taken);xr_xir_compile_artifact_free(borrowed);
    xr_xir_compile_artifact_free(donor);xr_xir_compile_provenance_free(owned);
    xr_xir_compile_checked_packet_free(&expected);xr_xir_compile_checked_packet_free(&actual);
    return status;
}

static void provenance_take_requests(const XrXirCompileContext *context,XrXirArtifact *donor,
    const XrXirCompileContext *foreign) {
    XrXirProvenance *owned=(XrXirProvenance *)donor->module.provenance;
    XrXirArtifact *sentinel=(XrXirArtifact *)(uintptr_t)1;
    XrCompileResourceStats entry=library_compile_stats(context),other=library_compile_stats(foreign);
    size_t attempts=source_program_compile_attempts;
    CHECK(xr_xir_compile_recheck_provenance_take(context,&donor->module,&(XirProvenanceReceiving){donor->construction,NULL},&owned,&sentinel,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(sentinel==(XrXirArtifact *)(uintptr_t)1 && owned==donor->module.provenance);
    sentinel=NULL;
    CHECK(xr_xir_compile_recheck_provenance_take(context,&donor->module,&(XirProvenanceReceiving){donor->construction,NULL},
        &owned,(XrXirArtifact **)(void *)&owned,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(owned==donor->module.provenance);
    CHECK(xr_xir_compile_recheck_provenance_take(foreign,&donor->module,&(XirProvenanceReceiving){donor->construction,NULL},&owned,&sentinel,NULL)==XR_XIR_BAD_STRUCTURE);
    XrXirModule invalid=donor->module;invalid.stage=XR_XIR_BUILT;
    CHECK(xr_xir_compile_recheck_provenance_take(context,&invalid,&(XirProvenanceReceiving){donor->construction,NULL},&owned,&sentinel,NULL)==XR_XIR_BAD_STAGE);
    invalid=donor->module;invalid.provenance=NULL;
    CHECK(xr_xir_compile_recheck_provenance_take(context,&invalid,&(XirProvenanceReceiving){donor->construction,NULL},&owned,&sentinel,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(!sentinel && owned==donor->module.provenance);
    CHECK(source_program_compile_attempts==attempts);
    XrCompileResourceStats after=library_compile_stats(context),foreign_after=library_compile_stats(foreign);
    CHECK(after.work==entry.work && after.allocated_bytes==entry.allocated_bytes && after.live_bytes==entry.live_bytes);
    CHECK(foreign_after.work==other.work && foreign_after.allocated_bytes==other.allocated_bytes && foreign_after.live_bytes==other.live_bytes);
}

static void provenance_take_rejection(const XrXirCompileContext *context) {
    XrCompileResourceStats baseline=library_compile_stats(context);
    XrXirArtifact *donor=NULL,*output=NULL;
    CHECK(provenance_take_fixture(context,&donor)==XR_XIR_OK);
    CHECK(donor->module.function_count && donor->module.function_count<=16);
    XrXirFunction functions[16];
    memcpy(functions,donor->module.functions,donor->module.function_count*sizeof(*functions));
    functions[0].instruction_count=0;
    XrXirModule invalid=donor->module;invalid.functions=functions;
    XrXirProvenance *owned=(XrXirProvenance *)donor->module.provenance;donor->module.provenance=NULL;
    CHECK(xr_xir_compile_recheck_provenance_take(context,&invalid,&(XirProvenanceReceiving){donor->construction,NULL},&owned,&output,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(!owned && !output);xr_xir_compile_artifact_free(donor);
    CHECK(library_compile_stats(context).live_bytes==baseline.live_bytes);
}


/* These views borrow the actual complete Template. Each mutation is restored
 * before the next request, so no earlier proof or pointer grants a later one. */
static void provenance_source_fields(const XrXirCompileContext *context,
    const XrXirArtifact *source,const XrXirEffects *effects) {
    const XrXirModule *module=&source->module;
    CHECK(module->function_count && module->function_count<=16);
    XrXirFunction functions[16];memcpy(functions,module->functions,module->function_count*sizeof(*functions));
    XrXirModule changed=*module;changed.functions=functions;
    uint32_t body=UINT32_MAX,row=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)
        for(uint32_t i=0;i<module->functions[f].instruction_count;++i)
            if(module->functions[f].instructions[i].op==XR_XIR_CONST_INT &&
                module->functions[f].instructions[i].type==XR_XIR_I64) { body=f;row=i;break; }
    CHECK(body!=UINT32_MAX && row!=UINT32_MAX);
    const XrXirFunction *original=&module->functions[body];
    CHECK(original->instruction_count<=64 && original->block_count && original->block_count<=16);
    XrXirInstruction operations[64];memcpy(operations,original->instructions,original->instruction_count*sizeof(*operations));
    functions[body].instructions=operations;operations[row].immediate^=1;
    /* A different legal constant has no new permissions, but it is still a
     * different Source and must not consume the old owning derivation. */
    CHECK(xr_xir_compile_structure_verify_v2(context,&changed,source->construction,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_verify_source_effects_v2(context,&changed,source->construction,effects,NULL)==XR_XIR_BAD_STRUCTURE);
    operations[row]=original->instructions[row];
    XrXirBlock blocks[16];memcpy(blocks,original->blocks,original->block_count*sizeof(*blocks));
    functions[body].blocks=blocks;blocks[0].count^=1;
    CHECK(xir_effects_source_body_match(context,&changed,effects)==XR_XIR_BAD_STRUCTURE);
    blocks[0]=original->blocks[0];
    CHECK(xr_xir_compile_verify_source_effects_v2(context,&changed,source->construction,effects,NULL)==XR_XIR_OK);
}

static void provenance_source_contracts(const XrXirCompileContext *context,
    const XrXirArtifact *source,const XrXirEffects *effects) {
    const XrXirModule *module=&source->module;const XrXirProvenance *template=module->provenance;
    CHECK(template && template->kind==XR_XIR_EVIDENCE_TEMPLATE && template->contract_count<=16 && template->contract_count);
    XrXirFunctionEffectContract contracts[16];
    memcpy(contracts,template->contracts,template->contract_count*sizeof(*contracts));
    XrXirProvenance changed_evidence=*template;changed_evidence.contracts=contracts;
    XrXirModule changed=*module;changed.provenance=&changed_evidence;
    contracts[0].formula.constant_mask^=XR_XIR_CALLABLE_ROOT_REQUIRED;
    /* The bit is shape-valid. The original full permission checker, rather
     * than the body matcher, rejects its false advertised ROOT formula. */
    CHECK(xr_xir_compile_structure_verify_v2(context,&changed,source->construction,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_verify_source_effects_v2(context,&changed,source->construction,effects,NULL)==XR_XIR_BAD_TYPE);
    contracts[0]=template->contracts[0];
    CHECK(xr_xir_compile_verify_source_effects_v2(context,&changed,source->construction,effects,NULL)==XR_XIR_OK);
}

static void provenance_source_requests(const XrXirCompileContext *context,
    XrXirArtifact *donor,XrXirEffects *effects,const XrXirCompileContext *foreign) {
    const XrXirArtifact *source=donor->module.provenance->source;
    XrCompileResourceStats entry=library_compile_stats(context),other=library_compile_stats(foreign);
    size_t attempts=source_program_compile_attempts;
    CHECK(xir_effects_source_body_match(foreign,&source->module,effects)==XR_XIR_BAD_STRUCTURE);
    CHECK(xir_effects_source_body_match(context,&source->module,NULL)==XR_XIR_BAD_STRUCTURE);
    XrXirProvenance *owned=(XrXirProvenance *)donor->module.provenance;
    XrXirArtifact *output=NULL;
    CHECK(xr_xir_compile_recheck_provenance_take(context,&donor->module,
        (const XirProvenanceReceiving *)(const void *)&owned,&owned,&output,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(owned==donor->module.provenance && !output);
    CHECK(xr_xir_compile_recheck_provenance_take(context,&donor->module,
        (const XirProvenanceReceiving *)(const void *)&output,&owned,&output,NULL)==XR_XIR_BAD_STRUCTURE);
    XrXirProvenance *sentinel=(XrXirProvenance *)(uintptr_t)1;
    CHECK(xr_xir_compile_provenance_copy_source_effects(context,owned,effects,&sentinel)==XR_XIR_BAD_STRUCTURE);
    CHECK(sentinel==(XrXirProvenance *)(uintptr_t)1 && owned==donor->module.provenance);
    CHECK(source_program_compile_attempts==attempts);
    XrCompileResourceStats after=library_compile_stats(context),foreign_after=library_compile_stats(foreign);
    CHECK(after.work==entry.work && after.allocated_bytes==entry.allocated_bytes && after.live_bytes==entry.live_bytes);
    CHECK(foreign_after.work==other.work && foreign_after.allocated_bytes==other.allocated_bytes && foreign_after.live_bytes==other.live_bytes);
}

/* The Effects owner is an owned physical Source, not a retained raw Module.
 * The producer can die, yet a new full matching peer may be consumed. Freeing
 * those facts before public final verification proves none escape the call. */
static void provenance_source_death(const XrXirCompileContext *context,
    XrXirArtifact **donor,XrXirEffects **effects) {
    XrXirArtifact *peer=NULL,*taken=NULL;
    CHECK(xr_xir_compile_recheck_v2(context,&(*donor)->module,(*donor)->construction,&peer,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(*donor);*donor=NULL;
    const XrXirArtifact *source=peer->module.provenance->source;
    CHECK(xr_xir_compile_verify_source_effects_v2(context,&source->module,source->construction,*effects,NULL)==XR_XIR_OK);
    XrXirModule view=peer->module;XrXirProvenance *owned=(XrXirProvenance *)view.provenance;
    peer->module.provenance=NULL;
    CHECK(xr_xir_compile_recheck_provenance_take(context,&view,
        &(XirProvenanceReceiving){peer->construction,*effects},&owned,&taken,NULL)==XR_XIR_OK);
    CHECK(!owned && taken);xr_xir_compile_artifact_free(peer);
    xr_xir_compile_effects_free(*effects);*effects=NULL;
    CHECK(xr_xir_compile_artifact_verify(taken,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(taken);
}

/* Another genuine derivation on this same ledger has identical dimensions.
 * Its changed constant still forbids reuse for the original receiving tree.
 * Once accepted, that failure must consume and free the unique subtree. */
static void provenance_source_mismatched(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    XrXirArtifact *donor=NULL,*output=NULL;XrXirEffects *different=NULL;
    CHECK(provenance_take_fixture(&owner.context,&donor)==XR_XIR_OK);
    const XrXirArtifact *source=donor->module.provenance->source;
    CHECK(source->module.function_count && source->module.function_count<=16);
    XrXirFunction functions[16];
    memcpy(functions,source->module.functions,source->module.function_count*sizeof(*functions));
    uint32_t body=UINT32_MAX,row=UINT32_MAX;
    for(uint32_t f=0;f<source->module.function_count;++f)
        for(uint32_t i=0;i<functions[f].instruction_count;++i)
            if(functions[f].instructions[i].op==XR_XIR_CONST_INT && functions[f].instructions[i].type==XR_XIR_I64) {
                body=f;row=i;break;
            }
    CHECK(body!=UINT32_MAX && row!=UINT32_MAX && functions[body].instruction_count<=64);
    XrXirInstruction operations[64];
    memcpy(operations,functions[body].instructions,functions[body].instruction_count*sizeof(*operations));
    functions[body].instructions=operations;operations[row].immediate^=1;
    XrXirModule changed=source->module;changed.functions=functions;
    CHECK(xr_xir_compile_verify_owned_effects_v2(&owner.context,&changed,source->construction,&different,NULL)==XR_XIR_OK);
    XrXirModule view=donor->module;XrXirProvenance *owned=(XrXirProvenance *)view.provenance;
    donor->module.provenance=NULL;
    CHECK(xr_xir_compile_recheck_provenance_take(&owner.context,&view,
        &(XirProvenanceReceiving){donor->construction,different},&owned,&output,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(!owned && !output);xr_xir_compile_effects_free(different);xr_xir_compile_artifact_free(donor);
    library_compile_owner_drop(&owner);
}

static void provenance_source_controls(void) {
    LibraryCompileOwner owner={0},foreign={0};
    CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    CHECK(library_compile_owner_new(&foreign,&library_compile_limits)==XR_XIR_OK);
    XrXirArtifact *donor=NULL;XrXirEffects *effects=NULL;
    CHECK(provenance_take_fixture(&owner.context,&donor)==XR_XIR_OK);
    const XrXirArtifact *source=donor->module.provenance->source;
    CHECK(xr_xir_compile_artifact_verify_owned_effects(source,&effects,NULL)==XR_XIR_OK);
    provenance_source_requests(&owner.context,donor,effects,&foreign.context);
    provenance_source_fields(&owner.context,source,effects);
    provenance_source_contracts(&owner.context,source,effects);
    provenance_source_death(&owner.context,&donor,&effects);
    CHECK(!donor && !effects);library_compile_owner_drop(&foreign);library_compile_owner_drop(&owner);
    provenance_source_mismatched();
}

static void provenance_take_controls(void) {
    provenance_source_controls();
    LibraryCompileOwner owner={0},foreign={0};
    CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    CHECK(library_compile_owner_new(&foreign,&library_compile_limits)==XR_XIR_OK);
    XrXirArtifact *donor=NULL;CHECK(provenance_take_fixture(&owner.context,&donor)==XR_XIR_OK);
    provenance_take_requests(&owner.context,donor,&foreign.context);
    xr_xir_compile_artifact_free(donor);library_compile_owner_drop(&foreign);
    provenance_take_rejection(&owner.context);
    XrCompileResourceStats before=library_compile_stats(&owner.context);
    size_t blocks=source_program_compile_live,bytes=source_program_compile_bytes;
    source_program_compile_attempts=0;source_program_compile_fail_at=0;source_program_compile_injected=false;
    CHECK(provenance_take_operation(&owner.context,NULL)==XR_XIR_OUT_OF_MEMORY);
    source_program_compile_fail_at=SIZE_MAX;CHECK(source_program_compile_injected);
    CHECK(source_program_compile_live==blocks && source_program_compile_bytes==bytes);
    CHECK(library_compile_stats(&owner.context).live_bytes==before.live_bytes);
    CHECK(provenance_take_operation(&owner.context,NULL)==XR_XIR_OK);
    XrCompileResourceStats after=library_compile_stats(&owner.context);
    CHECK(after.work>before.work && after.allocated_bytes>before.allocated_bytes && after.live_bytes==before.live_bytes);
    library_compile_owner_drop(&owner);
}

/* Every original allocation boundary is faulted, followed by the same complete
 * success on the still-cumulative ledger. Only the physical injection is reset. */
static void provenance_take_resources(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    source_program_compile_attempts=0;
    CHECK(provenance_take_operation(&owner.context,NULL)==XR_XIR_OK);
    size_t sites=source_program_compile_attempts;CHECK(sites);
    XrCompileResourceStats required=library_compile_stats(&owner.context);
    CHECK(required.live_bytes==owner.baseline.live_bytes);library_compile_owner_drop(&owner);
    for(size_t site=0;site<sites;++site) {
        CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
        size_t blocks=source_program_compile_live,bytes=source_program_compile_bytes;
        source_program_compile_attempts=0;source_program_compile_fail_at=site;source_program_compile_injected=false;
        XrXirStatus status=provenance_take_operation(&owner.context,NULL);
        source_program_compile_fail_at=SIZE_MAX;
        if(status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"provenance-take fault %zu/%zu status%u\n",site,sites,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && source_program_compile_injected);
        XrCompileResourceStats failed=library_compile_stats(&owner.context);
        CHECK(failed.live_bytes==owner.baseline.live_bytes);
        CHECK(source_program_compile_live==blocks && source_program_compile_bytes==bytes);
        source_program_compile_attempts=0;source_program_compile_injected=false;
        CHECK(provenance_take_operation(&owner.context,NULL)==XR_XIR_OK && !source_program_compile_injected);
        CHECK(source_program_compile_attempts==sites);
        XrCompileResourceStats retry=library_compile_stats(&owner.context);
        CHECK(retry.work>failed.work && retry.allocated_bytes>failed.allocated_bytes && retry.live_bytes==owner.baseline.live_bytes);
        library_compile_owner_drop(&owner);
    }
    for(unsigned axis=0;axis<3;++axis)for(unsigned less=0;less<2;++less) {
        XrCompileResourceLimits limits=library_compile_limits;
        uint64_t exact=axis==0?required.allocated_bytes:axis==1?required.peak_bytes:required.work;
        uint64_t *cap=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        CHECK(exact);*cap=exact-less;
        XrXirStatus status=library_compile_owner_new(&owner,&limits);
        if(status==XR_XIR_OK)status=provenance_take_operation(&owner.context,NULL);
        if(status!=(less?XR_XIR_BUDGET:XR_XIR_OK))fprintf(stderr,"provenance-take axis%u less%u exact%llu status%u\n",axis,less,(unsigned long long)exact,status);
        CHECK(status==(less?XR_XIR_BUDGET:XR_XIR_OK));library_compile_owner_drop(&owner);
    }
    fprintf(stderr,"provenance-take compilerOOM=%zu sameowner-retry=%zu allocated=%llu peak=%llu work=%llu threeaxes/physicalbaseline PASS\n",
        sites,sites,(unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,(unsigned long long)required.work);
}
#endif // XIR_SPECIALIZE_PROVENANCE_TAKE_CASES_H
