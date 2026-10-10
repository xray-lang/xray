/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_exact_edge_cases.h - Tight live storage with full physical origin queries
 */
#ifndef XIR_CELL_EXACT_EDGE_CASES_H
#define XIR_CELL_EXACT_EDGE_CASES_H

typedef struct CellExactFixture {
    CellActiveFixture active;
    XrXirInstruction initial[5], places[5]; uint32_t operands[2];
} CellExactFixture;
static void cell_exact_fixture(CellExactFixture *f,unsigned mode) {
    CHECK(mode<3);memset(f,0,sizeof(*f));cell_active_fixture(&f->active,mode==2?2:1);
    CellActiveFixture *a=&f->active;
    if(mode==1) {
        a->operations[CA_VALUES-1]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)256};
        a->operations[CA_VALUES].args[0]=CA_VALUES;
    } else if(mode==2) {
        f->initial[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=40};
        f->initial[1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
        f->initial[2]=(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)256,.args={0,2}};
        f->initial[3]=(XrXirInstruction){.op=XR_XIR_SLOT_INIT,.args={2}};
        f->initial[4]=(XrXirInstruction){.op=XR_XIR_RETURN};
        f->operands[0]=0;f->operands[1]=1;a->initial.count=5;
        a->functions[0].instructions=f->initial;a->functions[0].instruction_count=5;
        a->functions[0].operands=f->operands;a->functions[0].operand_count=2;
        f->places[0]=(XrXirInstruction){.op=XR_XIR_SLOT_PLACE,.type=(XrXirType)256};
        f->places[1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
        f->places[2]=(XrXirInstruction){.op=XR_XIR_INDEX_PLACE,.type=XR_XIR_I64,.args={0,1}};
        f->places[3]=(XrXirInstruction){.op=XR_XIR_PLACE_READ,.type=XR_XIR_I64,.args={2}};
        f->places[4]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={3}};
        a->block.count=5;a->functions[1].instructions=f->places;a->functions[1].instruction_count=5;
    }
}
static XrXirStatus cell_exact_operation(const XrXirCompileContext *context,unsigned mode) {
    CellExactFixture f;cell_exact_fixture(&f,mode);XrXirCellProvenance *proof=NULL;
    XrXirStatus status=xr_xir_compile_cell_provenance_verified(context,&f.active.module,&proof,NULL);
    if(status!=XR_XIR_OK) { CHECK(!proof);return status; }
    uint32_t values=mode==2?5u:CA_OPS+1u;memset(&f,0xCC,sizeof(f));
    for(uint32_t v=0;v<values && status==XR_XIR_OK;++v) {
        XrXirCellOriginView view={0};status=xr_xir_compile_cell_origin_view(context,proof,1,v,&view);
        if(status!=XR_XIR_OK) break;
        bool dependency=mode!=2 && (!v || (mode==1 && v==CA_VALUES));
        uint32_t root=mode==2 && (v==0 || v==2)?XR_XIR_CELL_ACCESS_ROOT:0;
        CHECK(view.intrinsic_mask==root);
        if(mode!=2) CHECK(view.parameter_count==1 && view.parameters[0]==0 && view.word_count==1 &&
            view.dependencies[0]==(dependency?UINT64_C(1):0));
        else CHECK(!view.parameter_count && !view.word_count && !view.parameters && !view.dependencies);
        CHECK(xr_xir_cell_provenance_origin(proof,1,v)==(root?XR_XIR_CELL_ORIGIN_MODULE:
            dependency?XR_XIR_CELL_ORIGIN_SCOPED:0));
    }
    if(status==XR_XIR_OK) {
        XrXirCellOriginView sentinel={77,NULL,77,NULL,77},before=sentinel;
        CHECK(xr_xir_compile_cell_origin_view(context,proof,1,values,&sentinel)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&sentinel,&before,sizeof(sentinel)));
        CHECK(xr_xir_cell_provenance_role(proof,1,0)==XR_XIR_CELL_PROOF_UNKNOWN);
    }
    xr_xir_compile_cell_provenance_free(proof);return status;
}
static void cell_exact_full_check(unsigned mode,XrCompileResourceLimits caps) {
    CellExactFixture f;cell_exact_fixture(&f,mode);XrXirCompileContext context=cell_active_context(caps);
    uint64_t baseline=value_compile_stats(&context).live_bytes;
    XrXirDiagnostic d={XR_XIR_OK,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
    XrXirStatus status=xir_fixture_verify(&context,&f.active.module,&d);
    if(status!=XR_XIR_OK) fprintf(stderr,"CELL_EXACT full mode%u status%u f%u b%u i%u reason%u\n",
        mode,(unsigned)status,d.function,d.block,d.instruction,(unsigned)d.reason);
    CHECK(status==XR_XIR_OK);cell_proof_close(&context,baseline);
}
static void cell_exact_edges(void) {
    const XrCompileResourceLimits caps={33554432,33554432,33554432};
    for(unsigned mode=0;mode<3;++mode) {
        cell_exact_full_check(mode,caps);XrXirCompileContext context=cell_active_context(caps);
        uint64_t baseline=value_compile_stats(&context).live_bytes;size_t start=value_compile_calls;
        CHECK(cell_exact_operation(&context,mode)==XR_XIR_OK);
        size_t sites=value_compile_calls-start;XrCompileResourceStats exact=value_compile_stats(&context);
        CHECK(sites && exact.work);
        fprintf(stdout,"CELL_EXACT_CENSUS mode%u sites%zu allocated%llu live%llu peak%llu work%llu\n",
            mode,sites,(unsigned long long)exact.allocated_bytes,(unsigned long long)exact.live_bytes,
            (unsigned long long)exact.peak_bytes,(unsigned long long)exact.work);
        cell_proof_close(&context,baseline);
        for(size_t point=0;point<sites;++point) {
            context=cell_active_context(caps);baseline=value_compile_stats(&context).live_bytes;
            size_t physical=value_compile_live,bytes=value_compile_bytes;
            value_compile_fail_at=value_compile_calls+point;value_compile_injected=false;
            CHECK(cell_exact_operation(&context,mode)==XR_XIR_OUT_OF_MEMORY && value_compile_injected &&
                value_compile_live==physical && value_compile_bytes==bytes);
            value_compile_fail_at=SIZE_MAX;
            CHECK(cell_exact_operation(&context,mode)==XR_XIR_OK);cell_proof_close(&context,baseline);
        }
        for(unsigned axis=0;axis<3;++axis) for(unsigned minus=0;minus<2;++minus) {
            XrCompileResourceLimits limits=caps;
            if(axis==0) limits.allocated_bytes=exact.allocated_bytes-minus;
            else if(axis==1) limits.live_bytes=exact.peak_bytes-minus;
            else limits.work=exact.work-minus;
            context=cell_active_context(limits);baseline=value_compile_stats(&context).live_bytes;
            CHECK(cell_exact_operation(&context,mode)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
            cell_proof_close(&context,baseline);
        }
        /* Fixed live-only request is the actual old wide failure's remaining
         * 22186 bytes, below all original maxima; this is not a new budget. */
        XrCompileResourceLimits tight=caps;tight.live_bytes=22186;
        context=cell_active_context(tight);baseline=value_compile_stats(&context).live_bytes;
        CHECK(cell_exact_operation(&context,mode)==XR_XIR_OK);cell_proof_close(&context,baseline);
    }
}
#endif // XIR_CELL_EXACT_EDGE_CASES_H
