/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_error_finite_growth_cases.h - Error proof growth callback boundaries
 */
#ifndef XIR_ERROR_FINITE_GROWTH_CASES_H
#define XIR_ERROR_FINITE_GROWTH_CASES_H
static EffectErrorFinite *finite_growth_proof;
static EffectErrorFiniteView *finite_growth_view;
static EffectErrorFiniteFact *finite_growth_old;
static uint32_t finite_growth_count,finite_growth_kind,finite_growth_callbacks;
static void finite_growth_allocated(void *memory,size_t bytes) {
    (void)memory;(void)bytes;++finite_growth_callbacks;
    CHECK(!finite_growth_view->work && !finite_growth_view->input.work);
    ++finite_growth_proof->rows[0].blocks;
}
static void finite_growth_released(void *memory,size_t bytes) {
    uintptr_t begin=(uintptr_t)memory,payload=(uintptr_t)finite_growth_old;
    if(payload<begin || payload-begin>=bytes) return;
    pc_free_hook=NULL;++finite_growth_callbacks;
    CHECK(!finite_growth_view->work && !finite_growth_view->input.work);
    CHECK(!finite_growth_proof->facts && !finite_growth_proof->count && !finite_growth_proof->capacity);
    memset(finite_growth_old,0xA5,(size_t)finite_growth_count*sizeof(*finite_growth_old));
    if(finite_growth_kind==1) ++finite_growth_proof->rows[0].blocks;
}
static void finite_growth_case(unsigned kind) {
    CHECK(kind<4 && !pc_allocate_hook && !pc_free_hook);
    XrXirInstruction ops[2][2]={
        {{.op=XR_XIR_CALL,.immediate=1},{.op=XR_XIR_RETURN}},
        {{.op=XR_XIR_CALL,.immediate=0},{.op=XR_XIR_RETURN}}};
    XrXirBlock blocks[2]={{.count=2},{.count=2}};
    XrXirFunction functions[2]={{.instructions=ops[0],.instruction_count=2,.blocks=&blocks[0],.block_count=1},
        {.instructions=ops[1],.instruction_count=2,.blocks=&blocks[1],.block_count=1}};
    XrXirModule module={.functions=functions,.function_count=2};
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    EffectErrorFinite *proof=finite_prepare(&context,&module,NULL,0);
    CHECK(finite_build(&context,proof,&module)==XR_XIR_OK && proof->count==2);
    finite_verify(&context,proof,XR_XIR_OK);
    /* Lowering the advertised capacity keeps the real owned allocation and
     * its exact facts, while forcing the next insertion to grow it. */
    proof->capacity=proof->count;EffectErrorFiniteView view={0};
    CHECK(error_finite_view_open(&context,proof,&view)==XR_XIR_OK);
    finite_growth_proof=proof;finite_growth_view=&view;finite_growth_old=proof->facts;
    finite_growth_count=proof->count;finite_growth_kind=kind;finite_growth_callbacks=0;
    uint32_t row_blocks=proof->rows[0].blocks;uint64_t physical=live_bytes;
    if(!kind) pc_allocate_hook=finite_growth_allocated;
    else if(kind<3) pc_free_hook=finite_growth_released;
    else { fail_at=attempts;injected=false; }
    XrXirStatus status=error_finite_grow(&context,proof,&view);
    pc_allocate_hook=NULL;pc_free_hook=NULL;fail_at=SIZE_MAX;
    CHECK(!view.work && !view.input.work);
    if(kind<2) {
        CHECK(status==XR_XIR_BAD_STRUCTURE && finite_growth_callbacks==1);
        proof->rows[0].blocks=row_blocks;
        if(!kind) CHECK(live_bytes==physical && proof->facts==finite_growth_old && proof->count==2);
        else CHECK(!proof->facts && !proof->count && !proof->capacity);
    } else if(kind==2) CHECK(status==XR_XIR_OK && finite_growth_callbacks==1 && proof->count==2);
    else {
        CHECK(status==XR_XIR_OUT_OF_MEMORY && injected && !finite_growth_callbacks);
        CHECK(live_bytes==physical && proof->facts==finite_growth_old && proof->count==2);
        CHECK(error_finite_view_open(&context,proof,&view)==XR_XIR_OK);
        CHECK(error_finite_grow(&context,proof,&view)==XR_XIR_OK && !view.work);
        injected=false;
    }
    finite_growth_proof=NULL;finite_growth_view=NULL;finite_growth_old=NULL;
    CHECK(error_finite_derive(&context,proof)==XR_XIR_OK);
    finite_verify(&context,proof,XR_XIR_OK);
    finite_clear(proof);pc_release(&context,baseline);
    printf("finite Error growth kind%u: real callback boundary and independent closure passed\n",kind);
}
#endif // XIR_ERROR_FINITE_GROWTH_CASES_H
