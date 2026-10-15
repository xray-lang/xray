/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_zero_edge_cases.h - Nonempty owned Cell seeds without propagation edges
 */
#ifndef XIR_CELL_ZERO_EDGE_CASES_H
#define XIR_CELL_ZERO_EDGE_CASES_H
typedef struct CellZeroEdgeFixture {
    XrXirTypeNode node;XrXirTypes types;XrXirInstruction init,ops[5];
    XrXirBlock blocks[2];XrXirFunction functions[2];
    XrXirFunctionIdentity identities[2];XrXirSourceModule source;
    XrXirDeclarations declarations;XrXirModule module;
} CellZeroEdgeFixture;
static void cell_zero_edge_fixture(CellZeroEdgeFixture *f,bool edge) {
    memset(f,0,sizeof(*f));f->node=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64};
    f->types=(XrXirTypes){&f->node,1,NULL,NULL};f->init=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->ops[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
    f->ops[1]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)256};
    if (edge) f->ops[2]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)256,.args={1,0}};
    f->ops[edge?3:2]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=XR_XIR_I64,.args={edge?2u:1u,0}};
    f->ops[edge?4:3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={edge?3u:2u,0}};
    f->blocks[0].count=1;f->blocks[1].count=edge?5:4;
    f->functions[0]=(XrXirFunction){.name="init",.name_length=4,.result=XR_XIR_UNIT,
        .blocks=&f->blocks[0],.block_count=1,.instructions=&f->init,.instruction_count=1};
    f->functions[1]=(XrXirFunction){.name="read",.name_length=4,.result=XR_XIR_I64,
        .blocks=&f->blocks[1],.block_count=1,.instructions=f->ops,.instruction_count=f->blocks[1].count};
    f->source=(XrXirSourceModule){"zero-edge",9,NULL,0,0};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .root_module=0,.entry_function=1};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.types=&f->types,.functions=f->functions,
        .function_count=2,.declarations=&f->declarations,.linkage_kind=XR_XIR_PROGRAM};
}
static XrXirStatus cell_zero_edge_operation(const XrXirCompileContext *context,bool edge,bool oracle) {
    CellZeroEdgeFixture producer,material;cell_zero_edge_fixture(&producer,edge);cell_zero_edge_fixture(&material,edge);
    XrXirStatus status=xir_fixture_verify(context,&producer.module,NULL);
    if (status==XR_XIR_OK) status=xir_fixture_verify(context,&material.module,NULL);
    XrXirCellProvenance *proof=NULL;
    if (status==XR_XIR_OK) status=xr_xir_compile_cell_provenance_verified(context,&producer.module,&proof,NULL);
    memset(&producer,0xcc,sizeof(producer));
    CellBodyReadFixture reader={.module=&material.module};
    XrXirCellProofBodyRead body=cell_body_read_request(context,&reader);
    if (status==XR_XIR_OK) status=xr_xir_compile_cell_provenance_body_matches(context,proof,&body,NULL);
    if (status==XR_XIR_OK && oracle) {
        CHECK(xr_xir_cell_provenance_origin(proof,1,1)==XR_XIR_CELL_ORIGIN_OWNED);
        if (edge) CHECK(xr_xir_cell_provenance_origin(proof,1,2)==XR_XIR_CELL_ORIGIN_OWNED);
        cell_proof_view(context,proof,1,1,false);
        uint32_t last=material.functions[1].instruction_count-1;
        material.functions[1].result=(XrXirType)256;material.ops[last].args[0]=1;
        XrXirDiagnostic diagnostic={0};
        CHECK(xr_xir_compile_cell_provenance_body_matches(context,proof,&body,&diagnostic)==XR_XIR_BAD_VALUE &&
            diagnostic.function==1 && diagnostic.instruction==last);
        material.functions[1].result=XR_XIR_I64;material.ops[last].args[0]=edge?3:2;
        status=xr_xir_compile_cell_provenance_body_matches(context,proof,&body,NULL);
    }
    xr_xir_compile_cell_provenance_free(proof);return status;
}
static void cell_zero_edge_literals(void) {
    for (uint32_t mode=0;mode<2;++mode) {
        XrXirCompileContext context=cell_proof_context(UINT64_C(33554432));
        uint64_t baseline=value_compile_stats(&context).live_bytes;
        XrXirStatus status=cell_zero_edge_operation(&context,mode!=0,true);
        if (status!=XR_XIR_OK) fprintf(stderr,"CELL_ZERO_EDGE mode%u status%u\n",mode,(uint32_t)status);
        CHECK(status==XR_XIR_OK);
        printf("CELL_ZERO_EDGE literal mode%u nonempty_owned_seed producer_dead replay_escape_rejected work%llu\n",
            mode,(unsigned long long)value_compile_stats(&context).work);
        cell_proof_close(&context,baseline);
    }
}
static void cell_zero_edge_resources(void) {
    for (uint32_t mode=0;mode<2;++mode) {
        size_t sites=0;XrCompileResourceStats measured={0};
        for (size_t pass=0;pass<=sites;++pass) {
            value_compile_fail_at=SIZE_MAX;XrXirCompileContext context=cell_proof_context(UINT64_C(33554432));
            uint64_t baseline=value_compile_stats(&context).live_bytes;size_t before=value_compile_calls;
            value_compile_injected=false;value_compile_fail_at=pass?before+pass-1:SIZE_MAX;
            XrXirStatus status=cell_zero_edge_operation(&context,mode!=0,false);
            value_compile_fail_at=SIZE_MAX;
            if (!pass) {
                CHECK(status==XR_XIR_OK);sites=value_compile_calls-before;measured=value_compile_stats(&context);
                CHECK(sites && measured.work && measured.allocated_bytes && measured.peak_bytes);
            } else {
                CHECK(value_compile_injected && status==XR_XIR_OUT_OF_MEMORY);
                uint64_t retained_work=value_compile_stats(&context).work;
                CHECK(value_compile_stats(&context).live_bytes==baseline &&
                    cell_zero_edge_operation(&context,mode!=0,false)==XR_XIR_OK &&
                    value_compile_stats(&context).work>retained_work);
            }
            cell_proof_close(&context,baseline);
        }
        uint64_t costs[3]={measured.allocated_bytes,measured.peak_bytes,measured.work};
        for (uint32_t axis=0;axis<3;++axis) for (uint32_t enough=0;enough<2;++enough) {
            XrCompileResourceLimits caps={33554432,33554432,33554432};uint64_t limit=costs[axis]-(enough?0:1);
            if (!axis) caps.allocated_bytes=limit;else if (axis==1) caps.live_bytes=limit;else caps.work=limit;
            XrXirCompileContext context=cell_active_context(caps);uint64_t baseline=value_compile_stats(&context).live_bytes;
            CHECK(cell_zero_edge_operation(&context,mode!=0,false)==(enough?XR_XIR_OK:XR_XIR_BUDGET));
            cell_proof_close(&context,baseline);
        }
        printf("CELL_ZERO_EDGE resources mode%u sites%zu same_owner_OOM_retry axes_minus_one_exact work%llu\n",
            mode,sites,(unsigned long long)measured.work);
    }
}
#endif
