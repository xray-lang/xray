/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_zero_tracked_cases.h - Owned empty physical domains after complete admission
 */
#ifndef XIR_CELL_ZERO_TRACKED_CASES_H
#define XIR_CELL_ZERO_TRACKED_CASES_H

/* An unused Cell descriptor does not classify the actual physical values.
 * The real reference captures an ordinary scalar and keeps its UNKNOWN ad. */
static void cell_zero_reference_fixture(CellInterleavedFixture *f) {
    cell_interleaved_fixture(f);f->capture=0;
    f->initial[1]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)257,
        .args={0,1},.immediate=2};
    f->initial[2]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=1};
    f->functions[2].parameters=&f->scalar;f->functions[4].parameters=&f->scalar;
    f->read[0]=(XrXirInstruction){.op=XR_XIR_COPY,.type=XR_XIR_I64};
}
static XrXirStatus cell_zero_operation(const XrXirCompileContext *context,unsigned mode) {
    CellActiveFixture large;CellInterleavedFixture reference;
    const uint32_t large_counts[]={1,513},reference_counts[]={4,3,3,2,3};
    const uint32_t *counts=mode?reference_counts:large_counts;
    uint32_t functions=mode?5u:2u;
    const XrXirModule *module;
    if(mode) { cell_zero_reference_fixture(&reference);module=&reference.module; }
    else { cell_active_fixture(&large,0);module=&large.module; }
    XrXirCellProvenance *proof=NULL;
    XrXirStatus status=xr_xir_compile_cell_provenance_verified(context,module,&proof,NULL);
    if(status!=XR_XIR_OK) { CHECK(!proof);return status; }
    XrXirCellProvenance *occupied=proof;
    CHECK(xr_xir_compile_cell_provenance_verified(context,module,&occupied,NULL)==XR_XIR_BAD_STRUCTURE &&
        occupied==proof);
    memset(&large,0xCC,sizeof(large));memset(&reference,0xCC,sizeof(reference));
    for(uint32_t f=0;f<functions && status==XR_XIR_OK;++f) {
        for(uint32_t v=0;v<counts[f] && status==XR_XIR_OK;++v) {
            XrXirCellOriginView view={77,NULL,77,NULL,77};
            status=xr_xir_compile_cell_origin_view(context,proof,f,v,&view);
            if(status!=XR_XIR_OK) break;
            CHECK(!view.intrinsic_mask && !view.dependencies && !view.word_count &&
                !view.parameters && !view.parameter_count);
            CHECK(!xr_xir_cell_provenance_origin(proof,f,v));
            XrXirCellAccessRequest request={f,v,NULL,0};uint32_t access=77;
            status=xr_xir_compile_cell_access(context,proof,&request,&access);
            if(status==XR_XIR_OK) CHECK(!access);
            else CHECK(access==77);
        }
        if(status==XR_XIR_OK) {
            XrXirCellOriginView output={77,NULL,77,NULL,77},sentinel=output;
            CHECK(xr_xir_compile_cell_origin_view(context,proof,f,counts[f],&output)==XR_XIR_BAD_STRUCTURE &&
                !memcmp(&output,&sentinel,sizeof(output)));
            CHECK(xr_xir_cell_provenance_origin(proof,f,counts[f])==XR_XIR_CELL_ORIGIN_UNKNOWN);
            CHECK(xr_xir_cell_provenance_role(proof,f,0)==XR_XIR_CELL_PROOF_UNKNOWN);
        }
    }
    if(status==XR_XIR_OK) {
        XrXirCellOriginView output={77,NULL,77,NULL,77},sentinel=output;
        CHECK(xr_xir_compile_cell_origin_view(context,proof,functions,0,&output)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&output,&sentinel,sizeof(output)));
        CHECK(xr_xir_cell_provenance_role(proof,functions,0)==XR_XIR_CELL_PROOF_UNKNOWN);
    }
    xr_xir_compile_cell_provenance_free(proof);return status;
}
static void cell_zero_full_check(unsigned mode,XrCompileResourceLimits caps) {
    XrXirCompileContext context=cell_active_context(caps);
    uint64_t baseline=value_compile_stats(&context).live_bytes;
    CellActiveFixture large;CellInterleavedFixture reference;const XrXirModule *module;
    if(mode) { cell_zero_reference_fixture(&reference);module=&reference.module; }
    else { cell_active_fixture(&large,0);module=&large.module; }
    XrXirDiagnostic d={XR_XIR_OK,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
    XrXirStatus status=xir_fixture_verify(&context,module,&d);
    if(status!=XR_XIR_OK) fprintf(stderr,"CELL_ZERO full mode%u status%u f%u b%u i%u reason%u\n",
        mode,(unsigned)status,d.function,d.block,d.instruction,(unsigned)d.reason);
    CHECK(status==XR_XIR_OK);cell_proof_close(&context,baseline);
}
static void cell_zero_verified_boundary(XrCompileResourceLimits caps) {
    XrXirCompileContext context=cell_active_context(caps);
    uint64_t baseline=value_compile_stats(&context).live_bytes;
    CellInterleavedFixture f;cell_zero_reference_fixture(&f);
    f.initial[1].immediate=5;
    CHECK(xir_fixture_verify(&context,&f.module,NULL)==XR_XIR_BAD_STRUCTURE);
    cell_zero_reference_fixture(&f);
    XrXirCallableParameter parameter={(XrXirType)256,XR_PARAM_REF};
    f.nodes[1].parameters=&parameter;f.nodes[1].parameter_count=1;
    f.initial[1].args[1]=0;
    CHECK(xir_fixture_verify(&context,&f.module,NULL)==XR_XIR_BAD_TYPE);
    cell_zero_reference_fixture(&f);XrXirCellProvenance *proof=NULL;
    CHECK(xr_xir_compile_cell_provenance_verified(&context,&f.module,&proof,NULL)==XR_XIR_OK);
    size_t physical=value_compile_live,bytes=value_compile_bytes;
    XrXirCompileContext foreign=cell_active_context(caps);
    XrXirCellOriginView output={77,NULL,77,NULL,77},sentinel=output;
    CHECK(xr_xir_compile_cell_origin_view(&foreign,proof,0,0,&output)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&output,&sentinel,sizeof(output)));
    xr_compile_resources_release(foreign.resources);
    CHECK(value_compile_live==physical && value_compile_bytes==bytes);
    xr_xir_compile_cell_provenance_free(proof);cell_proof_close(&context,baseline);
}
/* A body without Cell values still assigns the real unbound REF role in a
 * different body. Empty graph walks cannot omit this global call obligation. */
static void cell_zero_mixed_reference_roles(XrCompileResourceLimits caps) {
    for(unsigned mode=0;mode<3;++mode) {
        XrXirCompileContext context=cell_active_context(caps);
        uint64_t baseline=value_compile_stats(&context).live_bytes;
        CellInterleavedFixture f;cell_interleaved_fixture(&f);
        XrXirTypeNode nodes[3];memcpy(nodes,f.nodes,sizeof(f.nodes));
        XrXirCallableParameter parameter={f.cell,mode==2?XR_PARAM_READ:XR_PARAM_REF};
        nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.parameters=&parameter,
            .parameter_count=1,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
        f.types.nodes=nodes;f.types.count=3;
        f.constants[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)258,
            .immediate=mode==1?5:4};
        XrXirCellProvenance *proof=NULL;
        XrXirStatus expected=mode==1?XR_XIR_BAD_STRUCTURE:mode==2?XR_XIR_BAD_TYPE:XR_XIR_OK;
        CHECK(xr_xir_compile_cell_provenance_verified(&context,&f.module,&proof,NULL)==expected);
        if(!mode) {
            CHECK(proof && xr_xir_cell_provenance_role(proof,4,0)==XR_XIR_CELL_PROOF_SCOPED_REF);
            CHECK(!xr_xir_cell_provenance_origin(proof,1,0));
        } else CHECK(!proof);
        xr_xir_compile_cell_provenance_free(proof);cell_proof_close(&context,baseline);
    }
}
static void cell_zero_tracked_cases(void) {
    const XrCompileResourceLimits caps={33554432,33554432,33554432};
    for(unsigned mode=0;mode<2;++mode) {
        cell_zero_full_check(mode,caps);XrXirCompileContext context=cell_active_context(caps);
        uint64_t baseline=value_compile_stats(&context).live_bytes;size_t start=value_compile_calls;
        CHECK(cell_zero_operation(&context,mode)==XR_XIR_OK);
        size_t sites=value_compile_calls-start;XrCompileResourceStats exact=value_compile_stats(&context);
        CHECK(sites==2 && exact.work);
        fprintf(stdout,"CELL_ZERO_CENSUS mode%u sites%zu allocated%llu live%llu peak%llu work%llu\n",
            mode,sites,(unsigned long long)exact.allocated_bytes,(unsigned long long)exact.live_bytes,
            (unsigned long long)exact.peak_bytes,(unsigned long long)exact.work);
        cell_proof_close(&context,baseline);
        for(size_t point=0;point<sites;++point) {
            context=cell_active_context(caps);baseline=value_compile_stats(&context).live_bytes;
            size_t physical=value_compile_live,bytes=value_compile_bytes;
            value_compile_fail_at=value_compile_calls+point;value_compile_injected=false;
            CHECK(cell_zero_operation(&context,mode)==XR_XIR_OUT_OF_MEMORY && value_compile_injected &&
                value_compile_stats(&context).live_bytes==baseline &&
                value_compile_live==physical && value_compile_bytes==bytes);
            value_compile_fail_at=SIZE_MAX;
            CHECK(cell_zero_operation(&context,mode)==XR_XIR_OK);cell_proof_close(&context,baseline);
        }
        for(unsigned axis=0;axis<3;++axis) for(unsigned minus=0;minus<2;++minus) {
            XrCompileResourceLimits limits=caps;
            if(axis==0) limits.allocated_bytes=exact.allocated_bytes-minus;
            else if(axis==1) limits.live_bytes=exact.peak_bytes-minus;
            else limits.work=exact.work-minus;
            context=cell_active_context(limits);baseline=value_compile_stats(&context).live_bytes;
            CHECK(cell_zero_operation(&context,mode)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
            cell_proof_close(&context,baseline);
        }
    }
    cell_zero_verified_boundary(caps);
    cell_zero_mixed_reference_roles(caps);
}
#endif // XIR_CELL_ZERO_TRACKED_CASES_H
