/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_edge_growth_cases.h - Ordered growth, bounded failures and complete owned proofs
 */
#ifndef XIR_CELL_EDGE_GROWTH_CASES_H
#define XIR_CELL_EDGE_GROWTH_CASES_H

static XrXirStatus edge_kernel(XrXirCompileContext *context,uint32_t count) {
    XrXirCellProvenance proof={.value_count=128};uint32_t heads[128],reverse[128];
    for(uint32_t i=0;i<128;++i)heads[i]=reverse[i]=UINT32_MAX;
    CellProofBuild build={.context=context,.proof=&proof,.heads=heads,.reverse_heads=reverse};
    for(uint32_t i=0;i<count;++i) {
        uint32_t from=i%64,to=(i+1)%64,old_head=heads[from],old_reverse=reverse[to];
        uint32_t old_count=build.edge_count,old_capacity=build.edge_capacity;CellProofEdge *old=build.edges;
        if(!cell_proof_edge(&build,from,to,(i&1)!=0)) {
            CHECK(build.edge_count==old_count && heads[from]==old_head && reverse[to]==old_reverse);
            if(build.status==XR_XIR_OUT_OF_MEMORY)CHECK(build.edge_capacity==old_capacity && build.edges==old);
            else CHECK(build.status==XR_XIR_BUDGET && build.edge_capacity>=old_capacity);
            break;
        }
        CHECK(build.edge_count==i+1 && heads[from]==i && reverse[to]==i);
        CellProofEdge edge=build.edges[i];CHECK(edge.from==from && edge.to==to && edge.identity==((i&1)!=0) &&
            edge.next==old_head && edge.reverse==old_reverse);
        for(uint32_t e=0;e<=i;++e)CHECK(build.edges[e].from==e%64 && build.edges[e].to==(e+1)%64 &&
            build.edges[e].identity==((e&1)!=0));
    }
    CHECK(build.status!=XR_XIR_OK || build.edge_count==count);
    if(!count)CHECK(!build.edges && !build.edge_capacity);
    xr_compile_resources_free(build.edges);return build.status;
}
static void edge_kernel_cases(uint32_t count) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    size_t before=rp_attempts;CHECK(edge_kernel(&context,count)==XR_XIR_OK);size_t sites=rp_attempts-before;
    XrCompileResourceStats required=rp_stats(&context);rp_owner_free(&context,baseline);rp_balanced(physical);
    CHECK(count || !sites);
    for(size_t site=0;site<sites;++site) {
        context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;rp_fail_at=rp_attempts+site;rp_injected=false;
        CHECK(edge_kernel(&context,count)==XR_XIR_OUT_OF_MEMORY && rp_injected);rp_fail_at=SIZE_MAX;rp_injected=false;
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    if(count)for(uint32_t axis=0;axis<3;++axis)for(uint32_t less=0;less<2;++less) {
        XrCompileResourceLimits limits=rp_caps();
        if(axis==0)limits.allocated_bytes=required.allocated_bytes-less;
        else if(axis==1)limits.live_bytes=required.peak_bytes-less;
        else limits.work=required.work-less;
        context=rp_owner(limits);baseline=rp_stats(&context).live_bytes;
        CHECK(edge_kernel(&context,count)==(less?XR_XIR_BUDGET:XR_XIR_OK));rp_owner_free(&context,baseline);rp_balanced(physical);
    }
}
static uint32_t edge_delegate_reads,edge_delegate_refuse=UINT32_MAX;
static XrXirStatus edge_delegate(const XrXirCompileContext *context,const void *owner,uint32_t f,uint32_t i,
    XrXirInstruction *storage,const XrXirInstruction **output) {
    ++edge_delegate_reads;if(f==0 && i==edge_delegate_refuse)return XR_XIR_BAD_STRUCTURE;
    return effect_cell_body_instruction(context,owner,f,i,storage,output);
}
static size_t edge_owned(uint32_t aliases,size_t fail_at) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,0);
    XrXirInstruction ops[72]={0};XrXirBlock block={.count=aliases+5};
    memcpy(ops,fixture.entry,2*sizeof(*ops));
    for(uint32_t i=0;i<aliases;++i)ops[2+i]=(XrXirInstruction){.op=XR_XIR_COPY,.type=fixture.cell,.args={1+i,0}};
    ops[2+aliases]=fixture.entry[2];ops[3+aliases]=fixture.entry[3];
    ops[4+aliases]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={3+aliases,0}};
    fixture.actuals[0]=2+aliases;fixture.actuals[1]=1+aliases;
    fixture.functions[0].instructions=ops;fixture.functions[0].instruction_count=aliases+5;fixture.functions[0].blocks=&block;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(&context,&fixture.module,&checked,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"edge aliases%u fixture status%u f%u i%u reason%u\n",aliases,status,diagnostic.function,diagnostic.instruction,diagnostic.reason);
    CHECK(status==XR_XIR_OK && xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    XrXirCellProvenance *old=NULL,*fresh=NULL;
    XrXirCellProofBodyRead baseline_body=cell_proof_raw_body(&context,xr_xir_compile_artifact_module(checked));
    CHECK(edge_original_derive(&context,&baseline_body,&old,&diagnostic)==XR_XIR_OK && old);
    XrXirCellProofBodyRead raw=cell_proof_raw_body(&context,xr_xir_compile_artifact_module(checked));
    CHECK(cell_proof_derive(&context,&raw,&fresh,&diagnostic)==XR_XIR_OK && cell_proof_matches(&context,old,fresh)==XR_XIR_OK);
    edge_probe_xr_xir_compile_cell_provenance_free(fresh);fresh=NULL;
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));memset(ops,0xCE,sizeof(ops));memset(&block,0xCE,sizeof(block));
    EffectCellBodyAdapter adapter={0};XrXirCellProofBodyRead read={0};
    CHECK(effect_cell_body_owned(&context,effects->invocations->body,&adapter,&read)==XR_XIR_OK);
    size_t start=rp_attempts;rp_injected=false;if(fail_at!=SIZE_MAX)rp_fail_at=start+fail_at;
    status=cell_proof_derive(&context,&read,&fresh,&diagnostic);size_t sites=rp_attempts-start;
    rp_fail_at=SIZE_MAX;
    if(fail_at!=SIZE_MAX)CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && !fresh);
    else {
        CHECK(status==XR_XIR_OK && fresh && cell_proof_matches(&context,old,fresh)==XR_XIR_OK);
        edge_probe_xr_xir_compile_cell_provenance_free(fresh);fresh=NULL;
        XrXirCellProofBodyRead delegated=read;delegated.span=NULL;delegated.range=NULL;delegated.range_row=NULL;delegated.range_shape=NULL;
        delegated.instruction=edge_delegate;edge_delegate_reads=0;
        CHECK(cell_proof_derive(&context,&delegated,&fresh,&diagnostic)==XR_XIR_OK && edge_delegate_reads &&
            cell_proof_matches(&context,old,fresh)==XR_XIR_OK);edge_probe_xr_xir_compile_cell_provenance_free(fresh);fresh=NULL;
        edge_delegate_refuse=0;CHECK(cell_proof_derive(&context,&delegated,&fresh,&diagnostic)==XR_XIR_BAD_STRUCTURE && !fresh);edge_delegate_refuse=UINT32_MAX;
    }
    rp_injected=false;xr_xir_compile_cell_provenance_free(old);xr_xir_compile_effects_free(effects);
    rp_owner_free(&context,baseline);rp_balanced(physical);CHECK(!rp_live && !rp_live_bytes);return sites;
}
static void cell_edge_cases(void) {
    const uint32_t counts[]={0,1,4,5,8,9,16,17,32,33,64,65};
    for(uint32_t i=0;i<sizeof(counts)/sizeof(*counts);++i)edge_kernel_cases(counts[i]);
    const uint32_t aliases[]={0,1,5,17,33};
    for(uint32_t i=0;i<sizeof(aliases)/sizeof(*aliases);++i) {
        size_t sites=edge_owned(aliases[i],SIZE_MAX);
        for(size_t site=0;site<sites;++site)edge_owned(aliases[i],site);
        printf("single pass genuine aliases%u complete candidate allocation prefixes%zu\n",aliases[i],sites);
    }
}

#endif // XIR_CELL_EDGE_GROWTH_CASES_H
