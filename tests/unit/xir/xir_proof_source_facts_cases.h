/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_proof_source_facts_cases.h - Immutable data in a fresh proof decoder
 *
 * KEY CONCEPT:
 *   Reconstructed bytes decide correspondence; a borrowed owner grants nothing.
 */
#ifndef XIR_PROOF_SOURCE_FACTS_CASES_H
#define XIR_PROOF_SOURCE_FACTS_CASES_H

static XrXirStatus proof_source_facts_fixture(const XrXirCompileContext *context,
    const char *text,XrXirArtifact **output) {
    XrCompilerSession *session=NULL;
    XrCompilerSessionStatus opened=xr_compile_session_new(context->resources,&session);
    if(opened!=XR_COMPILER_SESSION_OK)
        return opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"proof-source",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL,NULL};
    XrXirSourceText input={NULL,text,strlen(text)};XrXirSourceResult source={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&source,NULL,NULL);
    xr_compile_session_free(session);
    if(status==XR_XIR_OK)status=xr_xir_compile_specialize(source.checked,output,NULL);
    xr_xir_compile_source_result_free(&source);return status;
}

static const char proof_source_facts_text[]=
    "export fn plain(value:i64)->i64{return value+1}\nprint(plain(41))\n";

static XrXirStatus proof_source_facts_operation(const XrXirCompileContext *context,void *unused) {
    (void)unused;XrXirArtifact *donor=NULL,*decoded=NULL;XrXirCheckedPacket packet={0};
    XrXirStatus status=proof_source_facts_fixture(context,proof_source_facts_text,&donor);
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_write(donor,&packet,NULL);
    const XrXirArtifact *source=donor?donor->module.provenance->source:NULL;
    XirCheckedInput input={packet.bytes,packet.length,source};
    if(status==XR_XIR_OK)status=xir_checked_receive(context,&input,&decoded,NULL);
    if(status==XR_XIR_OK)CHECK(decoded->module.provenance->source->source_facts==source->source_facts);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(donor);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(decoded,NULL);
    xr_xir_compile_artifact_free(decoded);return status;
}

static void proof_source_facts_refuse(const XrXirCompileContext *context,
    const XirCheckedInput *input,XrXirStatus expected) {
    XrCompileResourceStats before=library_compile_stats(context);
    size_t blocks=source_program_compile_live,bytes=source_program_compile_bytes;
    XrXirArtifact *decoded=NULL;
    CHECK(xir_checked_receive(context,input,&decoded,NULL)==expected && !decoded);
    CHECK(library_compile_stats(context).live_bytes==before.live_bytes);
    CHECK(source_program_compile_live==blocks && source_program_compile_bytes==bytes);
}

static void proof_source_facts_controls(void) {
    LibraryCompileOwner owner={0},foreign={0};
    CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    XrXirArtifact *donor=NULL,*different=NULL,*alien=NULL,*decoded=NULL,*raw=NULL;
    CHECK(proof_source_facts_fixture(&owner.context,proof_source_facts_text,&donor)==XR_XIR_OK);
    CHECK(proof_source_facts_fixture(&owner.context,
        "export fn plain(value:i64)->i64{return value+2}\nprint(plain(41))\n",&different)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(donor,&packet,NULL)==XR_XIR_OK);
    const XrXirArtifact *source=donor->module.provenance->source;
    XirCheckedInput input={packet.bytes,packet.length,source};
    XrXirArtifact *occupied=(XrXirArtifact *)(uintptr_t)1;
    XrCompileResourceStats before=library_compile_stats(&owner.context);
    CHECK(xir_checked_receive(&owner.context,(const XirCheckedInput *)(uintptr_t)1,&occupied,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(occupied==(XrXirArtifact *)(uintptr_t)1);
    CHECK(library_compile_stats(&owner.context).work==before.work);
    proof_source_facts_refuse(&owner.context,NULL,XR_XIR_BAD_STRUCTURE);
    input.source_owner=different->module.provenance->source;
    proof_source_facts_refuse(&owner.context,&input,XR_XIR_BAD_STRUCTURE);
    /* The global physical observer nests owners: retain this exact outer
     * baseline until the foreign ledger and all its allocations are retired. */
    CHECK(library_compile_owner_new(&foreign,&library_compile_limits)==XR_XIR_OK);
    CHECK(proof_source_facts_fixture(&foreign.context,proof_source_facts_text,&alien)==XR_XIR_OK);
    input.source_owner=alien->module.provenance->source;
    proof_source_facts_refuse(&owner.context,&input,XR_XIR_BAD_STRUCTURE);
    xr_xir_compile_artifact_free(alien);alien=NULL;library_compile_owner_drop(&foreign);
    XrXirArtifact missing=*source;missing.source_facts=NULL;input.source_owner=&missing;
    proof_source_facts_refuse(&owner.context,&input,XR_XIR_BAD_STRUCTURE);
    input.source_owner=source;
    uint8_t *bytes=(uint8_t *)packet.bytes;bytes[32]^=1;
    proof_source_facts_refuse(&owner.context,&input,XR_XIR_BAD_STRUCTURE);bytes[32]^=1;
    before=library_compile_stats(&owner.context);
    CHECK(xir_checked_receive(&owner.context,&input,&decoded,NULL)==XR_XIR_OK);
    uint64_t borrowed=library_compile_stats(&owner.context).work-before.work;
    CHECK(decoded->module.provenance->source->source_facts==source->source_facts);
    before=library_compile_stats(&owner.context);
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&raw,NULL)==XR_XIR_OK);
    uint64_t fresh=library_compile_stats(&owner.context).work-before.work;
    CHECK(raw->module.provenance->source->source_facts!=source->source_facts && borrowed<fresh);
    xr_xir_compile_artifact_free(raw);xr_xir_compile_artifact_free(different);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(donor);
    CHECK(xr_xir_compile_artifact_verify(decoded,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);library_compile_owner_drop(&owner);
    CHECK(!source_program_compile_live && !source_program_compile_bytes);
    printf("Fresh proof Source data: different canonical body, foreign ledger, missing owner, digest and occupied output rejected; producer death PASS; borrowed%llu fresh%llu\n",
        (unsigned long long)borrowed,(unsigned long long)fresh);
}
static void proof_source_facts_resources(bool complete) {
    LibraryCompileOwner owner={0};
    CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    source_program_compile_attempts=0;
    CHECK(proof_source_facts_operation(&owner.context,NULL)==XR_XIR_OK);
    size_t sites=source_program_compile_attempts;
    XrCompileResourceStats required=library_compile_stats(&owner.context);
    CHECK(sites && required.live_bytes==owner.baseline.live_bytes);
    library_compile_owner_drop(&owner);
    size_t tested=0;
    for(size_t site=0;site<sites;++site) {
        if(!complete && site!=0 && site!=sites/2 && site+1!=sites)continue;
        ++tested;
        CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
        size_t blocks=source_program_compile_live,bytes=source_program_compile_bytes;
        source_program_compile_attempts=0;source_program_compile_fail_at=site;
        source_program_compile_injected=false;
        XrXirStatus status=proof_source_facts_operation(&owner.context,NULL);
        source_program_compile_fail_at=SIZE_MAX;
        if(status!=XR_XIR_OUT_OF_MEMORY || !source_program_compile_injected)
            fprintf(stderr,"Fresh Source proof fault%zu/%zu status%u injected%u\n",site,sites,
                (unsigned)status,(unsigned)source_program_compile_injected);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && source_program_compile_injected);
        XrCompileResourceStats paid=library_compile_stats(&owner.context);
        CHECK(paid.live_bytes==owner.baseline.live_bytes);
        CHECK(source_program_compile_live==blocks && source_program_compile_bytes==bytes);
        CHECK(proof_source_facts_operation(&owner.context,NULL)==XR_XIR_OK);
        XrCompileResourceStats after=library_compile_stats(&owner.context);
        CHECK(after.live_bytes==owner.baseline.live_bytes && after.allocated_bytes>paid.allocated_bytes && after.work>paid.work);
        CHECK(source_program_compile_live==blocks && source_program_compile_bytes==bytes);
        library_compile_owner_drop(&owner);
    }
    for(unsigned axis=0;axis<3;++axis)for(unsigned less=0;less<2;++less) {
        XrCompileResourceLimits limits=library_compile_limits;
        uint64_t exact=axis==0?required.allocated_bytes:axis==1?required.peak_bytes:required.work;
        uint64_t *cap=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        CHECK(exact);*cap=exact-less;
        XrXirStatus status=library_compile_owner_new(&owner,&limits);
        if(status==XR_XIR_OK)status=proof_source_facts_operation(&owner.context,NULL);
        if(status!=(less?XR_XIR_BUDGET:XR_XIR_OK))
            fprintf(stderr,"Fresh Source proof axis%u less%u exact%llu status%u\n",axis,less,
                (unsigned long long)exact,(unsigned)status);
        CHECK(status==(less?XR_XIR_BUDGET:XR_XIR_OK));library_compile_owner_drop(&owner);
    }
    CHECK(!source_program_compile_live && !source_program_compile_bytes);
    printf("Fresh Source proof compiler sites%zu/%zu: every selected OOM entered, same finite owner retry, six exact/minus-one axes and physical0 PASS\n",tested,sites);
}

#endif // XIR_PROOF_SOURCE_FACTS_CASES_H
