/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_edge_growth_cases.h - Full ownership and reverse identity over growth
 */
#ifndef XIR_CELL_EDGE_GROWTH_CASES_H
#define XIR_CELL_EDGE_GROWTH_CASES_H
enum { CELL_GROW_COPIES=257,CELL_GROW_ROWS=CELL_GROW_COPIES+4 };
static XrCompileResources *cell_grow_sampling_owner;
static uint64_t cell_grow_allocation_work[10];
static size_t cell_grow_allocation_samples;
static void cell_grow_allocation_observe(size_t bytes) {
    struct EdgeFields {uint32_t from,to,next,reverse;bool identity;};
    if(!cell_grow_sampling_owner || cell_grow_allocation_samples>=10 || bytes<sizeof(CompileAllocation))return;
    size_t capacity=(size_t)1<<cell_grow_allocation_samples;
    if(bytes-sizeof(CompileAllocation)!=capacity*sizeof(struct EdgeFields))return;
    /* The allocation wrapper holds this same thread's resource lock. Reading
     * its actual counter avoids re-entering the lock through the public API. */
    cell_grow_allocation_work[cell_grow_allocation_samples++]=cell_grow_sampling_owner->stats.work;
}
typedef struct CellGrowFixture {
    CellActiveFixture active;
    XrXirInstruction chain[CELL_GROW_ROWS];
} CellGrowFixture;
static void cell_grow_fixture(CellGrowFixture *f,unsigned mode) {
    CHECK(mode<2);memset(f,0,sizeof(*f));cell_active_fixture(&f->active,0);
    CellActiveFixture *a=&f->active;
    f->chain[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
    f->chain[1]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)256};
    for(uint32_t i=0;i<CELL_GROW_COPIES;++i)
        f->chain[2+i]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)256,.args={1+i}};
    if(!mode){
        f->chain[CELL_GROW_COPIES+2]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=XR_XIR_I64,.args={CELL_GROW_COPIES+1}};
        f->chain[CELL_GROW_COPIES+3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={CELL_GROW_COPIES+2}};
        a->functions[1].instructions=f->chain;a->functions[1].instruction_count=CELL_GROW_ROWS;
        a->block.count=CELL_GROW_ROWS;
    }else{
        a->slot=(XrXirSlot){.type=(XrXirType)256,.mutable=1};
        a->declarations.slots=&a->slot;a->declarations.slot_count=1;
        f->chain[CELL_GROW_COPIES+2]=(XrXirInstruction){.op=XR_XIR_SLOT_INIT,.args={CELL_GROW_COPIES+1}};
        f->chain[CELL_GROW_COPIES+3]=(XrXirInstruction){.op=XR_XIR_RETURN};
        a->functions[0].instructions=f->chain;a->functions[0].instruction_count=CELL_GROW_ROWS;
        a->initial.count=CELL_GROW_ROWS;
        a->operations[0]=(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=(XrXirType)256};
        a->operations[1]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=XR_XIR_I64};
        a->operations[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};
        a->block.count=3;a->functions[1].instruction_count=3;
    }
}
static XrXirStatus cell_grow_operation(const XrXirCompileContext *context,unsigned mode) {
    CellGrowFixture f;cell_grow_fixture(&f,mode);XrXirCellProvenance *proof=NULL;
    XrXirStatus status=xr_xir_compile_cell_provenance_verified(context,&f.active.module,&proof,NULL);
    if(status!=XR_XIR_OK){CHECK(!proof);return status;}
    memset(&f,0xce,sizeof(f));uint32_t function=mode?0:1;
    for(uint32_t v=1;v<=CELL_GROW_COPIES+1 && status==XR_XIR_OK;++v){
        uint8_t expected=XR_XIR_CELL_ORIGIN_OWNED|(mode?XR_XIR_CELL_ORIGIN_MODULE:0);
        CHECK(xr_xir_cell_provenance_origin(proof,function,v)==expected);
        XrXirCellOriginView view={0};status=xr_xir_compile_cell_origin_view(context,proof,function,v,&view);
        if(status==XR_XIR_OK)CHECK(view.intrinsic_mask==(mode?XR_XIR_CELL_ACCESS_ROOT:0) &&
            !view.word_count && !view.parameter_count && !view.dependencies && !view.parameters);
    }
    if(status==XR_XIR_OK && mode)CHECK(xr_xir_cell_provenance_origin(proof,1,0)==XR_XIR_CELL_ORIGIN_MODULE);
    xr_xir_compile_cell_provenance_free(proof);return status;
}
static void cell_edge_growth_cases(void) {
    const XrCompileResourceLimits caps={8388608,4194304,4000000};
    for(unsigned mode=0;mode<2;++mode){
        XrXirCompileContext context=cell_active_context(caps);uint64_t baseline=value_compile_stats(&context).live_bytes;
        CellGrowFixture f;cell_grow_fixture(&f,mode);XrXirDiagnostic diagnostic={0};
        XrXirStatus verified=xir_fixture_verify(&context,&f.active.module,&diagnostic);
        if(verified!=XR_XIR_OK)fprintf(stderr,"Cell growth full-check mode%u status%u f%u i%u reason%u\n",mode,(unsigned)verified,diagnostic.function,diagnostic.instruction,(unsigned)diagnostic.reason);
        CHECK(verified==XR_XIR_OK);cell_proof_close(&context,baseline);
        context=cell_active_context(caps);baseline=value_compile_stats(&context).live_bytes;
        size_t attempts=value_compile_calls;CHECK(!value_compile_allocation_observer);
        cell_grow_sampling_owner=context.resources;cell_grow_allocation_samples=0;
        value_compile_allocation_observer=cell_grow_allocation_observe;
        XrXirStatus sampled=cell_grow_operation(&context,mode);
        value_compile_allocation_observer=NULL;cell_grow_sampling_owner=NULL;
        CHECK(sampled==XR_XIR_OK && cell_grow_allocation_samples==10);
        size_t sites=value_compile_calls-attempts;CHECK(sites);XrCompileResourceStats measured=value_compile_stats(&context);
        cell_proof_close(&context,baseline);
        for(size_t ordinal=0;ordinal<sites;++ordinal){context=cell_active_context(caps);
            baseline=value_compile_stats(&context).live_bytes;size_t blocks=value_compile_live,bytes=value_compile_bytes;
            XrCompileResources *identity=context.resources;value_compile_fail_at=value_compile_calls+ordinal;value_compile_injected=false;
            XrXirStatus status=cell_grow_operation(&context,mode);value_compile_fail_at=SIZE_MAX;
            CHECK(status==XR_XIR_OUT_OF_MEMORY && value_compile_injected && context.resources==identity &&
                value_compile_live==blocks && value_compile_bytes==bytes);
            CHECK(cell_grow_operation(&context,mode)==XR_XIR_OK && context.resources==identity);
            cell_proof_close(&context,baseline);}
        for(size_t point=0;point<cell_grow_allocation_samples;++point){
            XrCompileResourceLimits finite=caps;finite.work=cell_grow_allocation_work[point];
            context=cell_active_context(finite);baseline=value_compile_stats(&context).live_bytes;
            size_t blocks=value_compile_live,bytes=value_compile_bytes;XrCompileResources *identity=context.resources;
            CHECK(cell_grow_operation(&context,mode)==XR_XIR_BUDGET && context.resources==identity &&
                value_compile_stats(&context).work==finite.work && value_compile_live==blocks && value_compile_bytes==bytes);
            CHECK(cell_grow_operation(&context,mode)==XR_XIR_BUDGET && context.resources==identity &&
                value_compile_live==blocks && value_compile_bytes==bytes);cell_proof_close(&context,baseline);}
        for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){
            XrCompileResourceLimits finite=caps;
            if(!axis)finite.allocated_bytes=measured.allocated_bytes-minus;
            else if(axis==1)finite.live_bytes=measured.peak_bytes-minus;else finite.work=measured.work-minus;
            context=cell_active_context(finite);baseline=value_compile_stats(&context).live_bytes;
            CHECK(cell_grow_operation(&context,mode)==(minus?XR_XIR_BUDGET:XR_XIR_OK));cell_proof_close(&context,baseline);}
        fprintf(stderr,"Cell edge growth mode%u:257 actual copies, complete258 origin/access oracles after producer death, every%zu actual OOM same finite-owner retry, six axes%llu/%llu/%llu exact/minusone PASS\n",
            mode,sites,(unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
        fprintf(stderr,"Cell edge growth mode%u:all10 actual allocation-admitted work cutoffs clean both old/new buffers and reject same-owner exhausted retry PASS\n",mode);
    }
}
#endif // XIR_CELL_EDGE_GROWTH_CASES_H
