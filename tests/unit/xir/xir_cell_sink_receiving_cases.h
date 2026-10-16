/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_sink_receiving_cases.h - Owned Cell use coordinates and growth failures
 */
#ifndef XIR_CELL_SINK_RECEIVING_CASES_H
#define XIR_CELL_SINK_RECEIVING_CASES_H
static void cell_sink_fixture(CellActiveFixture *f,bool rejected) {
    cell_active_fixture(f,1);
    for (uint32_t i=0;i<CA_VALUES;++i)
        f->operations[i]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=XR_XIR_I64};
    if (rejected) f->operations[CA_OPS-1].args[0]=0;
}
static XrXirStatus cell_sink_operation(const XrXirCompileContext *context,bool rejected) {
    CellActiveFixture f;cell_sink_fixture(&f,rejected);
    XrXirCellProvenance *proof=NULL;
    XrXirStatus status=xr_xir_compile_cell_provenance_verified(context,&f.module,&proof,NULL);
    if (status!=XR_XIR_OK) { CHECK(!proof);return status; }
    CHECK(!rejected && proof);memset(&f,0xCC,sizeof(f));
    XrXirCellOriginView view={0};
    status=xr_xir_compile_cell_origin_view(context,proof,1,0,&view);
    if (status==XR_XIR_OK)
        CHECK(!view.intrinsic_mask && view.parameter_count==1 && view.parameters[0]==0 &&
            view.word_count==1 && view.dependencies[0]==1);
    xr_xir_compile_cell_provenance_free(proof);return status;
}
/* More than five hundred reads grow receiving storage repeatedly. A final
 * retention attempt still rejects, and every failed allocation stays atomic. */
static void cell_sink_receiving_cases(void) {
    const XrCompileResourceLimits caps={33554432,33554432,33554432};
    XrXirCompileContext context=cell_active_context(caps);
    uint64_t baseline=value_compile_stats(&context).live_bytes;
    CellActiveFixture f;cell_sink_fixture(&f,false);
    CHECK(xir_fixture_verify(&context,&f.module,NULL)==XR_XIR_OK);
    cell_proof_close(&context,baseline);
    for (unsigned mode=0;mode<2;++mode) {
        XrXirStatus expected=mode?XR_XIR_BAD_VALUE:XR_XIR_OK;
        context=cell_active_context(caps);baseline=value_compile_stats(&context).live_bytes;
        size_t start=value_compile_calls;
        CHECK(cell_sink_operation(&context,mode!=0)==expected);
        size_t sites=value_compile_calls-start;XrCompileResourceStats exact=value_compile_stats(&context);
        CHECK(sites && exact.work);cell_proof_close(&context,baseline);
        for (size_t point=0;point<sites;++point) {
            context=cell_active_context(caps);baseline=value_compile_stats(&context).live_bytes;
            size_t physical=value_compile_live,bytes=value_compile_bytes;
            value_compile_fail_at=value_compile_calls+point;value_compile_injected=false;
            CHECK(cell_sink_operation(&context,mode!=0)==XR_XIR_OUT_OF_MEMORY && value_compile_injected &&
                value_compile_live==physical && value_compile_bytes==bytes);
            value_compile_fail_at=SIZE_MAX;XrCompileResourceStats spent=value_compile_stats(&context);
            CHECK(cell_sink_operation(&context,mode!=0)==expected &&
                value_compile_stats(&context).work>spent.work &&
                value_compile_stats(&context).allocated_bytes>spent.allocated_bytes);
            cell_proof_close(&context,baseline);
        }
        for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
            XrCompileResourceLimits limits=caps;
            if (!axis) limits.allocated_bytes=exact.allocated_bytes-minus;
            else if (axis==1) limits.live_bytes=exact.peak_bytes-minus;
            else limits.work=exact.work-minus;
            context=cell_active_context(limits);baseline=value_compile_stats(&context).live_bytes;
            CHECK(cell_sink_operation(&context,mode!=0)==(minus?XR_XIR_BUDGET:expected));
            cell_proof_close(&context,baseline);
        }
        printf("CELL_SINK_RECEIVING mode%u sites%zu allocated%llu peak%llu work%llu physical0\n",
            mode,sites,(unsigned long long)exact.allocated_bytes,
            (unsigned long long)exact.peak_bytes,(unsigned long long)exact.work);
    }
}
#endif // XIR_CELL_SINK_RECEIVING_CASES_H
