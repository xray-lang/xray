/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_raw_inventory_cases.h - Scalar proof parity and current raw boundaries
 */
#ifndef XIR_CELL_RAW_INVENTORY_CASES_H
#define XIR_CELL_RAW_INVENTORY_CASES_H
static uint32_t raw_inventory_calls,raw_inventory_fault;
static XrXirStatus raw_inventory_delegate_head(const XrXirCompileContext *context,const void *owner,
    uint32_t f,XrXirCellProofBodyHead *output) {
    ++raw_inventory_calls;return cell_proof_raw_head(context,owner,f,output);
}
static XrXirStatus raw_inventory_delegate_type(const XrXirCompileContext *context,const void *owner,
    uint32_t f,uint32_t v,XrXirType *output) {
    XrXirStatus status=cell_proof_raw_type(context,owner,f,v,output);
    if (status==XR_XIR_OK && f==0 && v==1 && ++raw_inventory_calls==2) *output=XR_XIR_I64;
    return status;
}
static XrXirStatus raw_inventory_delegate_shape(const XrXirCompileContext *context,const void *owner,
    uint32_t f,uint32_t i,XrXirCellProofInstructionShape *output) {
    if (raw_inventory_fault && f==0 && i==1) return XR_XIR_IO;
    return cell_proof_raw_shape(context,owner,f,i,output);
}
static void raw_inventory_refuse(const XrXirCompileContext *context,const XrXirCellProofBodyRead *body) {
    XrXirCellProvenance *output=NULL;
    CHECK(cell_proof_derive(context,body,&output,NULL)==XR_XIR_BAD_STRUCTURE && !output);
}
static size_t raw_inventory_case(uint32_t aliases,size_t failure,uint32_t less) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,0);
    XrXirInstruction operations[72]={0};XrXirBlock block={.count=aliases+5};
    CHECK(aliases<=33);memcpy(operations,fixture.entry,2*sizeof(*operations));
    for (uint32_t i=0;i<aliases;++i) operations[2+i]=(XrXirInstruction){.op=XR_XIR_COPY,.type=fixture.cell,.args={1+i,0}};
    operations[2+aliases]=fixture.entry[2];operations[3+aliases]=fixture.entry[3];
    operations[4+aliases]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={3+aliases,0}};
    fixture.actuals[0]=2+aliases;fixture.actuals[1]=1+aliases;
    fixture.functions[0].instructions=operations;fixture.functions[0].instruction_count=aliases+5;
    fixture.functions[0].blocks=&block;
    XrXirArtifact *checked=NULL;CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    XrXirCellProofBodyRead body=cell_proof_raw_body(&context,xr_xir_compile_artifact_module(checked));
    XrXirCellProvenance *old=NULL,*fresh=NULL;
    CHECK(raw_inventory_original_derive(&context,&body,&old,NULL)==XR_XIR_OK);
    size_t start=rp_attempts;XrCompileResourceStats before=rp_stats(&context);rp_injected=false;
    if (failure!=SIZE_MAX) rp_fail_at=start+failure;
    XrXirStatus status=cell_proof_derive(&context,&body,&fresh,NULL);size_t sites=rp_attempts-start;
    XrCompileResourceStats after=rp_stats(&context);uint64_t required=after.work-before.work;
    rp_fail_at=SIZE_MAX;
    if (failure!=SIZE_MAX) CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && !fresh);
    else {
        CHECK(status==XR_XIR_OK && fresh && cell_proof_matches(&context,old,fresh)==XR_XIR_OK);
        edge_probe_xr_xir_compile_cell_provenance_free(fresh);fresh=NULL;
        XrXirCellProofBodyRead delegated=body;delegated.head=raw_inventory_delegate_head;
        raw_inventory_calls=0;before=rp_stats(&context);
        CHECK(raw_inventory_original_derive(&context,&delegated,&fresh,NULL)==XR_XIR_OK);
        uint64_t scalar=rp_stats(&context).work-before.work;edge_probe_xr_xir_compile_cell_provenance_free(fresh);fresh=NULL;
        before=rp_stats(&context);raw_inventory_calls=0;
        CHECK(cell_proof_derive(&context,&delegated,&fresh,NULL)==XR_XIR_OK && raw_inventory_calls &&
            cell_proof_matches(&context,old,fresh)==XR_XIR_OK);
        CHECK(rp_stats(&context).work-before.work>=scalar);
        edge_probe_xr_xir_compile_cell_provenance_free(fresh);fresh=NULL;
        /* Compare exact delegated fees without charging the separate proof comparison. */
        before=rp_stats(&context);CHECK(cell_proof_derive(&context,&delegated,&fresh,NULL)==XR_XIR_OK);
        CHECK(rp_stats(&context).work-before.work==scalar);edge_probe_xr_xir_compile_cell_provenance_free(fresh);fresh=NULL;
        for (uint32_t mode=0;mode<2;++mode) {
            delegated=body;raw_inventory_calls=0;raw_inventory_fault=mode==0;
            if (mode==0) delegated.shape=raw_inventory_delegate_shape;
            else delegated.type=raw_inventory_delegate_type;
            CHECK(cell_proof_derive(&context,&delegated,&fresh,NULL)==(mode?XR_XIR_BAD_TYPE:XR_XIR_IO) && !fresh);
            raw_inventory_calls=0;
            CHECK(raw_inventory_original_derive(&context,&delegated,&fresh,NULL)==(mode?XR_XIR_BAD_TYPE:XR_XIR_IO) && !fresh);
        }
        raw_inventory_fault=0;
        XrXirCellProofBodyRead damaged=body;damaged.domain.function_count++;
        raw_inventory_refuse(&context,&damaged);damaged=body;
        damaged.domain.function_count=0;raw_inventory_refuse(&context,&damaged);damaged=body;
        damaged.domain.types=&effect_empty_types;raw_inventory_refuse(&context,&damaged);damaged=body;
        damaged.domain.declarations=NULL;raw_inventory_refuse(&context,&damaged);damaged=body;
        damaged.domain.generics=(const XrXirGeneric *)checked;raw_inventory_refuse(&context,&damaged);damaged=body;
        damaged.domain.defaults=(const XrXirDefaultTable *)checked;raw_inventory_refuse(&context,&damaged);
        XrXirCellProvenance *held=old;size_t attempts=rp_attempts;
        CHECK(cell_proof_derive(&context,&body,&held,NULL)==XR_XIR_BAD_STRUCTURE && held==old && rp_attempts==attempts);
        if (less<2) {
            uint64_t work=rp_stats(&context).work,limit=rp_caps().work;
            CHECK(required && work+required-less<limit &&
                xr_compile_resources_work(context.resources,limit-work-required+less)==XR_COMPILE_RESOURCE_OK);
            CHECK(cell_proof_derive(&context,&body,&fresh,NULL)==(less?XR_XIR_BUDGET:XR_XIR_OK));
            CHECK(less?!fresh:!!fresh);
        } else CHECK(cell_proof_derive(&context,&body,&fresh,NULL)==XR_XIR_OK);
    }
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));memset(operations,0xCE,sizeof(operations));
    if (fresh && less>=2) CHECK(cell_proof_matches(&context,old,fresh)==XR_XIR_OK);
    edge_probe_xr_xir_compile_cell_provenance_free(fresh);xr_xir_compile_cell_provenance_free(old);
    rp_injected=false;rp_owner_free(&context,baseline);rp_balanced(physical);
    CHECK(!rp_live && !rp_live_bytes && !rp_allocation_observer);return sites;
}
static void raw_inventory_empty(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    XrXirModule module={.stage=XR_XIR_CHECKED};
    XrXirCellProofBodyRead body=cell_proof_raw_body(&context,&module);
    XrXirCellProvenance *old=NULL,*fresh=NULL;
    CHECK(raw_inventory_original_derive(&context,&body,&old,NULL)==XR_XIR_OK);
    CHECK(cell_proof_derive(&context,&body,&fresh,NULL)==XR_XIR_OK &&
        cell_proof_matches(&context,old,fresh)==XR_XIR_OK);
    edge_probe_xr_xir_compile_cell_provenance_free(fresh);xr_xir_compile_cell_provenance_free(old);
    rp_owner_free(&context,baseline);rp_balanced(physical);
}
static void cell_raw_inventory_cases(void) {
    raw_inventory_empty();
    const uint32_t aliases[]={0,1,5,17,33};
    for (uint32_t i=0;i<5;++i) {
        size_t sites=raw_inventory_case(aliases[i],SIZE_MAX,2);
        fprintf(stdout,"RAW_CELL_INVENTORY aliases%u derive_sites%zu\n",aliases[i],sites);
        for (size_t site=0;site<sites;++site) (void)raw_inventory_case(aliases[i],site,2);
        for (uint32_t less=0;less<2;++less) (void)raw_inventory_case(aliases[i],SIZE_MAX,less);
    }
    puts("RAW_CELL_INVENTORY original full proof, current domain, delegated fee/order, all derive OOM, exact/minus1 and producer-dead physical0 PASS");
}
#endif // XIR_CELL_RAW_INVENTORY_CASES_H
