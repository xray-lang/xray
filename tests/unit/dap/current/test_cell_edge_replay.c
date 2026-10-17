/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cell_edge_replay.c - Exact graph storage from complete current reads
 */
#define main edge_original_checks
#include "../../xir/test_xir_cell_provenance.c"
#undef main
static uint32_t edge_observed_reads,edge_refuse_phase;
static size_t edge_attempts;
static XrXirStatus edge_observe(const XrXirCompileContext *context,const void *owner,
    uint32_t f,uint32_t i,XrXirInstruction *storage,const XrXirInstruction **output) {
    if (f==1 && !i) ++edge_observed_reads;
    if (f==1 && i==5 && edge_observed_reads==edge_refuse_phase) return XR_XIR_IO;
    return cell_body_read_instruction(context,owner,f,i,storage,output);
}
static XrXirStatus edge_views(const XrXirCompileContext *context,const XrXirCellProvenance *proof,uint32_t copies) {
    for (uint32_t v=0;v<=CA_VALUES+2u;++v) {
        XrXirCellOriginView view={0};
        XrXirStatus status=xr_xir_compile_cell_origin_view(context,proof,1,v,&view);
        if (status!=XR_XIR_OK) return status;
        CHECK(view.parameter_count==1 && view.word_count==1 && view.parameters[0]==0);
        CHECK(view.dependencies[0]==(v<=copies?UINT64_C(1):0));
        CHECK(xr_xir_cell_provenance_origin(proof,1,v)==(v<=copies?XR_XIR_CELL_ORIGIN_SCOPED:0));
    }
    CHECK(xr_xir_cell_provenance_role(proof,1,0)==XR_XIR_CELL_PROOF_UNKNOWN);return XR_XIR_OK;
}
static void edge_fixture(CellActiveFixture *fixture,uint32_t copies) {
    CHECK(copies<CA_VALUES);cell_active_fixture(fixture,1);
    for (uint32_t i=0;i<copies;++i)
        fixture->operations[i]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)256,.args={i}};
    fixture->operations[CA_VALUES].args[0]=copies;
}
static XrXirStatus edge_operation(const XrXirCompileContext *context,uint32_t copies,bool measure) {
    CellActiveFixture fixture;edge_fixture(&fixture,copies);XrXirCellProvenance *proof=NULL;
    XrXirStatus status=xr_xir_compile_cell_provenance_verified(context,&fixture.module,&proof,NULL);
    if (status==XR_XIR_OK) {
        CellBodyReadFixture reader={.module=&fixture.module};
        XrXirCellProofBodyRead read=cell_body_read_request(context,&reader);read.instruction=edge_observe;
        edge_observed_reads=0;
        status=xr_xir_compile_cell_provenance_body_matches(context,proof,&read,NULL);
        if (status==XR_XIR_OK) {
            if (measure) printf("Cell edge copies=%u full-row callback reads=%u\n",copies,edge_observed_reads);
            CHECK(edge_observed_reads==(copies<=64?3u:4u));
            memset(&fixture,0xCE,sizeof(fixture));status=edge_views(context,proof,copies);
        }
    }
    xr_xir_compile_cell_provenance_free(proof);return status;
}
static void edge_read_refusals(void) {
    const XrCompileResourceLimits caps={33554432,33554432,33554432};
    XrXirCompileContext context=cell_active_context(caps);uint64_t baseline=value_compile_stats(&context).live_bytes;
    CellActiveFixture fixture;edge_fixture(&fixture,65);XrXirCellProvenance *proof=NULL;
    CHECK(xr_xir_compile_cell_provenance_verified(&context,&fixture.module,&proof,NULL)==XR_XIR_OK);
    CellBodyReadFixture reader={.module=&fixture.module};
    XrXirCellProofBodyRead read=cell_body_read_request(&context,&reader);read.instruction=edge_observe;
    uint64_t retained=value_compile_stats(&context).live_bytes;size_t physical=value_compile_live,bytes=value_compile_bytes;
    for (uint32_t phase=2;phase<=3;++phase) {
        edge_refuse_phase=phase;edge_observed_reads=0;XrXirDiagnostic diagnostic={0};
        CHECK(xr_xir_compile_cell_provenance_body_matches(&context,proof,&read,&diagnostic)==XR_XIR_IO);
        CHECK(diagnostic.status==XR_XIR_IO && diagnostic.function==1 && diagnostic.instruction==5);
        CHECK(value_compile_stats(&context).live_bytes==retained && value_compile_live==physical && value_compile_bytes==bytes);
        edge_refuse_phase=0;edge_observed_reads=0;
        CHECK(xr_xir_compile_cell_provenance_body_matches(&context,proof,&read,NULL)==XR_XIR_OK);
        CHECK(edge_views(&context,proof,65)==XR_XIR_OK);
    }
    xr_xir_compile_cell_provenance_free(proof);cell_proof_close(&context,baseline);
    puts("Partial local edge records and final heap replay IO retain proof, precise coordinates, same-ledger retry and physical0/0 PASS");
}
static XrCompileResourceStats edge_run(XrCompileResourceLimits limits,uint32_t copies,
    size_t fail,XrXirStatus expected) {
    XrXirCompileContext context=cell_active_context(limits);uint64_t baseline=value_compile_stats(&context).live_bytes;
    size_t physical=value_compile_live,bytes=value_compile_bytes,start=value_compile_calls;
    value_compile_fail_at=fail==SIZE_MAX?SIZE_MAX:start+fail;value_compile_injected=false;
    CHECK(edge_operation(&context,copies,false)==expected);size_t attempts=value_compile_calls-start;
    value_compile_fail_at=SIZE_MAX;
    if (fail!=SIZE_MAX) {
        CHECK(value_compile_injected && attempts==fail+1 && value_compile_live==physical && value_compile_bytes==bytes);
        CHECK(value_compile_stats(&context).live_bytes==baseline);
        CHECK(edge_operation(&context,copies,false)==XR_XIR_OK);
    }
    XrCompileResourceStats stats=value_compile_stats(&context);cell_proof_close(&context,baseline);
    edge_attempts=attempts;return stats;
}
int main(int argc,char **argv) {
    bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    const XrCompileResourceLimits caps={33554432,33554432,33554432};
    const uint32_t copies[]={0,1,63,64,65,127};
    for (uint32_t c=0;c<sizeof(copies)/sizeof(*copies);++c) {
        XrXirCompileContext context=cell_active_context(caps);uint64_t baseline=value_compile_stats(&context).live_bytes;
        CellActiveFixture fixture;edge_fixture(&fixture,copies[c]);
        CHECK(xir_fixture_verify(&context,&fixture.module,NULL)==XR_XIR_OK);
        CHECK(edge_operation(&context,copies[c],true)==XR_XIR_OK);cell_proof_close(&context,baseline);
        XrCompileResourceStats measured=edge_run(caps,copies[c],SIZE_MAX,XR_XIR_OK);
        size_t sites=edge_attempts;CHECK(sites);
        for (size_t point=0;point<sites;++point) if (resources || !point || point+1==sites)
            (void)edge_run(caps,copies[c],point,XR_XIR_OUT_OF_MEMORY);
        XrCompileResourceLimits exact={measured.allocated_bytes,measured.peak_bytes,measured.work};
        (void)edge_run(exact,copies[c],SIZE_MAX,XR_XIR_OK);
        for (uint32_t axis=0;axis<3;++axis) {
            XrCompileResourceLimits smaller=exact;
            if (!axis) --smaller.allocated_bytes;else if (axis==1) --smaller.live_bytes;else --smaller.work;
            (void)edge_run(smaller,copies[c],SIZE_MAX,XR_XIR_BUDGET);
        }
        printf("Cell edge copies=%u sites=%zu injection=%s same-ledger retry, three axes, owned symbolic rows and physical0/0 PASS\n",
            copies[c],sites,resources?"ALL":"FIRST_LAST_ONLY");
    }
    for (unsigned mode=0;mode<3;++mode) {
        XrCompileResourceLimits tight=caps;tight.live_bytes=22186;
        XrXirCompileContext context=cell_active_context(tight);uint64_t baseline=value_compile_stats(&context).live_bytes;
        CHECK(cell_exact_operation(&context,mode)==XR_XIR_OK);cell_proof_close(&context,baseline);
    }
    edge_read_refusals();
    cell_proof_actual_edges();cell_proof_rejections();cell_proof_escape_boundaries();
    cell_proof_callable_modes();cell_proof_indirect_ref();
    puts("Complete Cell read/edge replay boundaries, original tight live storage and independent capture/cleanup/escape controls PASS");return 0;
}
