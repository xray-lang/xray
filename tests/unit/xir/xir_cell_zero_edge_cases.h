/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_zero_edge_cases.h - Seeded Cell origins without propagation edges
 */
#ifndef XIR_CELL_ZERO_EDGE_CASES_H
#define XIR_CELL_ZERO_EDGE_CASES_H

static void cell_zero_edge_fixture(CellProofFixture *f) {
    cell_proof_fixture(f);
    f->entry[2]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=XR_XIR_I64,.args={1}};
    f->entry[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT,.args={2}};
    f->entry_blocks[0]=(XrXirBlock){.count=4};
    f->functions[1].block_count=1;f->functions[1].instruction_count=4;
    f->functions[1].operands=NULL;f->functions[1].operand_count=0;
    f->module.function_count=3;
}

/* Both the owned allocation and the unused reference parameter are real
 * tracked Cells. Their reads require the origins seeded by the first walk. */
static XrXirStatus cell_zero_edge_operation(const XrXirCompileContext *context) {
    CellProofFixture fixture;cell_zero_edge_fixture(&fixture);
    XrXirCellProvenance *proof=NULL;
    XrXirStatus status=xr_xir_compile_cell_provenance_verified(context,&fixture.module,&proof,NULL);
    if (status!=XR_XIR_OK) { CHECK(!proof);return status; }
    CHECK(xr_xir_cell_provenance_origin(proof,1,1)==XR_XIR_CELL_ORIGIN_OWNED);
    CHECK(xr_xir_cell_provenance_origin(proof,2,0)==XR_XIR_CELL_ORIGIN_SCOPED);
    CHECK(xr_xir_cell_provenance_role(proof,2,0)==XR_XIR_CELL_PROOF_UNKNOWN);
    memset(&fixture,0xCC,sizeof(fixture));
    for (unsigned mode=0;mode<2 && status==XR_XIR_OK;++mode) {
        XrXirCellOriginView output={77,NULL,77,NULL,77},sentinel=output;
        status=xr_xir_compile_cell_origin_view(context,proof,mode?2:1,mode?0:1,&output);
        if (status!=XR_XIR_OK) CHECK(!memcmp(&output,&sentinel,sizeof(output)));
        else if (mode) CHECK(!output.intrinsic_mask && output.parameter_count==1 &&
            output.parameters[0]==0 && output.word_count==1 && output.dependencies[0]==1);
        else CHECK(!output.intrinsic_mask && !output.parameter_count && !output.word_count &&
            !output.parameters && !output.dependencies);
    }
    xr_xir_compile_cell_provenance_free(proof);return status;
}

/* Escaping either independent Cell still reaches the final sink validator;
 * an empty adjacency does not confer a permission to return it. */
static void cell_zero_edge_sinks(void) {
    XrXirCompileContext context=cell_proof_context(33554432);
    uint64_t baseline=value_compile_stats(&context).live_bytes;
    CellProofFixture fixture;cell_zero_edge_fixture(&fixture);
    CHECK(xir_fixture_verify(&context,&fixture.module,NULL)==XR_XIR_OK);
    for (unsigned mode=0;mode<2;++mode) {
        cell_zero_edge_fixture(&fixture);
        if (mode) { fixture.functions[2].result=(XrXirType)256;fixture.read[1].args[0]=0; }
        else { fixture.functions[1].result=(XrXirType)256;fixture.entry[3].args[0]=1; }
        XrXirCellProvenance *proof=NULL;XrXirDiagnostic diagnostic={0};
        CHECK(xr_xir_compile_cell_provenance_verified(&context,&fixture.module,&proof,&diagnostic)==XR_XIR_BAD_VALUE &&
            !proof && diagnostic.function==(mode?2u:1u) && diagnostic.instruction==(mode?1u:3u));
        CHECK(xir_fixture_verify(&context,&fixture.module,NULL)==XR_XIR_BAD_TYPE);
    }
    cell_proof_close(&context,baseline);
}

static void cell_zero_edge_cases(void) {
    cell_zero_edge_sinks();
    const XrCompileResourceLimits caps={33554432,33554432,33554432};
    XrXirCompileContext context=cell_active_context(caps);
    uint64_t baseline=value_compile_stats(&context).live_bytes;size_t before=value_compile_calls;
    CHECK(cell_zero_edge_operation(&context)==XR_XIR_OK);
    size_t sites=value_compile_calls-before;XrCompileResourceStats exact=value_compile_stats(&context);
    CHECK(sites && exact.work);cell_proof_close(&context,baseline);
    for (size_t point=0;point<sites;++point) {
        context=cell_active_context(caps);baseline=value_compile_stats(&context).live_bytes;
        size_t physical=value_compile_live,bytes=value_compile_bytes;
        value_compile_fail_at=value_compile_calls+point;value_compile_injected=false;
        CHECK(cell_zero_edge_operation(&context)==XR_XIR_OUT_OF_MEMORY && value_compile_injected &&
            value_compile_live==physical && value_compile_bytes==bytes &&
            value_compile_stats(&context).live_bytes==baseline);
        value_compile_fail_at=SIZE_MAX;
        CHECK(cell_zero_edge_operation(&context)==XR_XIR_OK);cell_proof_close(&context,baseline);
    }
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits=caps;
        if (!axis) limits.allocated_bytes=exact.allocated_bytes-minus;
        else if (axis==1) limits.live_bytes=exact.peak_bytes-minus;
        else limits.work=exact.work-minus;
        context=cell_active_context(limits);baseline=value_compile_stats(&context).live_bytes;
        CHECK(cell_zero_edge_operation(&context)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        cell_proof_close(&context,baseline);
    }
    printf("CELL_ZERO_EDGE sites%zu allocated%llu peak%llu work%llu: owned/scoped seeds, sinks and resource exits passed\n",
        sites,(unsigned long long)exact.allocated_bytes,(unsigned long long)exact.peak_bytes,
        (unsigned long long)exact.work);
}
#endif // XIR_CELL_ZERO_EDGE_CASES_H
