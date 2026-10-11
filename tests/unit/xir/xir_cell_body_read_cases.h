/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_body_read_cases.h - Independent physical replay of owned Cell facts
 */
#ifndef XIR_CELL_BODY_READ_CASES_H
#define XIR_CELL_BODY_READ_CASES_H
typedef struct CellBodyReadFixture {
    CellClassFactFixture body;
    const XrXirModule *module;
    XrCompileResources *resources;
    unsigned failure;
} CellBodyReadFixture;
static XrXirStatus cell_body_read_head(const XrXirCompileContext *context,const void *owner,
    uint32_t f,XrXirCellProofBodyHead *output) {
    const CellBodyReadFixture *fixture=owner;
    if (!fixture || !fixture->module || fixture->resources!=context->resources ||
        f>=fixture->module->function_count || !output) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *function=&fixture->module->functions[f];
    if (xr_compile_resources_work(context->resources,8+sizeof(*output))!=XR_COMPILE_RESOURCE_OK) return XR_XIR_BUDGET;
    *output=(XrXirCellProofBodyHead){function->parameters,function->parameter_count,function->result,
        function->instruction_count,function->operands,function->operand_count};return XR_XIR_OK;
}
static XrXirStatus cell_body_read_instruction(const XrXirCompileContext *context,const void *owner,
    uint32_t f,uint32_t i,XrXirInstruction *output) {
    const CellBodyReadFixture *fixture=owner;
    if (!fixture || !fixture->module || fixture->resources!=context->resources ||
        f>=fixture->module->function_count || !output ||
        i>=fixture->module->functions[f].instruction_count) return XR_XIR_BAD_STRUCTURE;
    if (fixture->failure==1 && f==3 && i==13) return XR_XIR_IO;
    if (xr_compile_resources_work(context->resources,3+sizeof(*output))!=XR_COMPILE_RESOURCE_OK) return XR_XIR_BUDGET;
    *output=fixture->module->functions[f].instructions[i];return XR_XIR_OK;
}
static XrXirStatus cell_body_read_type(const XrXirCompileContext *context,const void *owner,
    uint32_t f,uint32_t value,XrXirType *output) {
    const CellBodyReadFixture *fixture=owner;
    if (!fixture || !fixture->module || fixture->resources!=context->resources ||
        f>=fixture->module->function_count || !output) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *function=&fixture->module->functions[f];
    if (value>=(uint64_t)function->parameter_count+function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    if (xr_compile_resources_work(context->resources,2+sizeof(*output))!=XR_COMPILE_RESOURCE_OK) return XR_XIR_BUDGET;
    *output=fixture->failure==2 && f==3 && value==13 ? XR_XIR_I64 : xr_xir_operand_type(function,value);
    return XR_XIR_OK;
}
static XrXirStatus cell_body_read_shape(const XrXirCompileContext *context,const void *owner,
    uint32_t f,uint32_t i,XrXirCellProofInstructionShape *output) {
    const CellBodyReadFixture *fixture=owner;
    if (!fixture || !fixture->module || fixture->resources!=context->resources ||
        f>=fixture->module->function_count || !output ||
        i>=fixture->module->functions[f].instruction_count) return XR_XIR_BAD_STRUCTURE;
    if (fixture->failure==3 && f==3 && i==13) return XR_XIR_IO;
    if (xr_compile_resources_work(context->resources,3+sizeof(*output))!=XR_COMPILE_RESOURCE_OK) return XR_XIR_BUDGET;
    const XrXirInstruction *operation=&fixture->module->functions[f].instructions[i];
    *output=(XrXirCellProofInstructionShape){operation->op,operation->type};
    if (fixture->failure==4 && f==3 && i==13) output->type=XR_XIR_I64;
    if (fixture->failure==5 && f==3 && i==13) output->op=XR_XIR_INVALID;
    return XR_XIR_OK;
}
static XrXirCellProofBodyRead cell_body_read_request(const XrXirCompileContext *context,
    CellBodyReadFixture *fixture) {
    fixture->resources=context->resources;
    return (XrXirCellProofBodyRead){xir_metadata_domain_from_module(fixture->module),context->resources,
        fixture,cell_body_read_head,cell_body_read_instruction,cell_body_read_type,cell_body_read_shape};
}
static void cell_body_read_attacks(const XrXirCompileContext *context,XrXirCellProvenance *proof,
    CellBodyReadFixture *fixture,XrXirCellProofBodyRead *request) {
    XrXirCellClassOriginView view={0},shape={0};
    CHECK(xr_xir_compile_cell_class_origin_view(context,proof,3,13,&view)==XR_XIR_OK);
    CHECK(view.word_count==1 && view.root_count==2 && view.dependencies[0]==3);
    uint64_t *words=(uint64_t *)view.dependencies;
    /* The one-bit omission is shape-valid. Only fresh full propagation can
     * prove that both real PHI inputs remain represented in this row. */
    words[0]=1;
    CHECK(xr_xir_compile_cell_class_origin_view(context,proof,3,13,&shape)==XR_XIR_OK && shape.dependencies[0]==1);
    CHECK(xr_xir_compile_cell_provenance_body_matches(context,proof,request,NULL)==XR_XIR_BAD_STRUCTURE);
    words[0]=3;
    CHECK(xr_xir_compile_cell_provenance_body_matches(context,proof,request,NULL)==XR_XIR_OK);
    XrXirCellClassRoot *roots=(XrXirCellClassRoot *)view.roots;
    CHECK(roots[0].value==1 && roots[1].value==2);
    roots[1].value=1;shape=(XrXirCellClassOriginView){0};
    CHECK(xr_xir_compile_cell_class_origin_view(context,proof,3,13,&shape)==XR_XIR_OK);
    CHECK(xr_xir_compile_cell_provenance_body_matches(context,proof,request,NULL)==XR_XIR_BAD_STRUCTURE);
    roots[1].value=2;
    fixture->failure=1;XrXirDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_cell_provenance_body_matches(context,proof,request,&diagnostic)==XR_XIR_IO &&
        diagnostic.status==XR_XIR_IO && diagnostic.function==3 && diagnostic.instruction==13);
    fixture->failure=2;
    CHECK(xr_xir_compile_cell_provenance_body_matches(context,proof,request,NULL)==XR_XIR_BAD_TYPE);
    fixture->failure=3;diagnostic=(XrXirDiagnostic){0};
    CHECK(xr_xir_compile_cell_provenance_body_matches(context,proof,request,&diagnostic)==XR_XIR_IO &&
        diagnostic.status==XR_XIR_IO && diagnostic.function==3 && diagnostic.instruction==13);
    fixture->failure=4;
    CHECK(xr_xir_compile_cell_provenance_body_matches(context,proof,request,NULL)==XR_XIR_BAD_TYPE);
    fixture->failure=5;
    CHECK(xr_xir_compile_cell_provenance_body_matches(context,proof,request,NULL)==XR_XIR_BAD_STRUCTURE);
    fixture->failure=0;
    CHECK(xr_xir_compile_cell_provenance_body_matches(context,proof,request,NULL)==XR_XIR_OK);
    size_t sites=value_compile_calls,physical=value_compile_live,bytes=value_compile_bytes;
    XrXirCompileContext foreign=cell_proof_context(33554432);
    XrCompileResourceStats before=value_compile_stats(&foreign);
    CHECK(xr_xir_compile_cell_provenance_body_matches(&foreign,proof,request,NULL)==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats after=value_compile_stats(&foreign);
    CHECK(!memcmp(&before,&after,sizeof(before)) && value_compile_calls==sites+1);
    xr_compile_resources_release(foreign.resources);
    CHECK(value_compile_live==physical && value_compile_bytes==bytes);
}
static XrXirStatus cell_body_read_operation(const XrXirCompileContext *context,unsigned mode,bool oracle) {
    CHECK(mode<8);CellClassFactFixture producer={0};CellBodyReadFixture material={0};
    CellActiveFixture large_source={0},large_material={0};
    CellInterleavedFixture scalar_source={0},scalar_material={0};const XrXirModule *source=NULL;
    if (mode<6) {
        cell_class_fact_fixture(&producer,mode);cell_class_fact_fixture(&material.body,mode);
        source=&producer.module;material.module=&material.body.module;
    } else if (mode==6) {
        cell_active_fixture(&large_source,0);cell_active_fixture(&large_material,0);
        source=&large_source.module;material.module=&large_material.module;
    } else {
        cell_zero_reference_fixture(&scalar_source);cell_zero_reference_fixture(&scalar_material);
        source=&scalar_source.module;material.module=&scalar_material.module;
    }
    XrXirCellProofBodyRead request=cell_body_read_request(context,&material);
    XrXirCellProvenance *proof=NULL;
    XrXirStatus status=xir_fixture_verify(context,source,NULL);
    if (status==XR_XIR_OK) status=xir_fixture_verify(context,material.module,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_cell_provenance_verified(context,source,&proof,NULL);
    memset(&producer,0xCC,sizeof(producer));
    memset(&large_source,0xCC,sizeof(large_source));memset(&scalar_source,0xCC,sizeof(scalar_source));
    if (status==XR_XIR_OK) status=xr_xir_compile_cell_provenance_body_matches(context,proof,&request,NULL);
    if (status==XR_XIR_OK && oracle && mode==2) cell_body_read_attacks(context,proof,&material,&request);
    memset(&material,0xCC,sizeof(material));
    memset(&large_material,0xCC,sizeof(large_material));memset(&scalar_material,0xCC,sizeof(scalar_material));
    if (status==XR_XIR_OK && mode<6) status=cell_class_fact_view(context,proof,mode);
    if (status==XR_XIR_OK && mode>=6) {
        const uint32_t large_counts[]={1,513},scalar_counts[]={4,3,3,2,3};
        const uint32_t *counts=mode==6?large_counts:scalar_counts;uint32_t functions=mode==6?2u:5u;
        for (uint32_t f=0;f<functions && status==XR_XIR_OK;++f)
            for (uint32_t value=0;value<counts[f] && status==XR_XIR_OK;++value) {
                XrXirCellClassOriginView view={0};
                status=xr_xir_compile_cell_class_origin_view(context,proof,f,value,&view);
                if (status==XR_XIR_OK) CHECK(!view.cells.intrinsic_mask && !view.cells.dependencies &&
                    !view.cells.parameter_count && !view.cells.word_count && !view.residual_intrinsic_mask &&
                    !view.roots && !view.root_count && !view.dependencies && !view.word_count);
            }
    }
    xr_xir_compile_cell_provenance_free(proof);return status;
}
static void cell_body_read_cases(void) {
    const XrCompileResourceLimits caps={33554432,33554432,33554432};
    for (unsigned mode=0;mode<8;++mode) {
        XrXirCompileContext context=cell_active_context(caps);uint64_t baseline=value_compile_stats(&context).live_bytes;
        CHECK(cell_body_read_operation(&context,mode,true)==XR_XIR_OK);cell_proof_close(&context,baseline);
        context=cell_active_context(caps);baseline=value_compile_stats(&context).live_bytes;
        size_t start=value_compile_calls;CHECK(cell_body_read_operation(&context,mode,false)==XR_XIR_OK);
        size_t sites=value_compile_calls-start;XrCompileResourceStats exact=value_compile_stats(&context);
        CHECK(sites && exact.work);cell_proof_close(&context,baseline);
        fprintf(stdout,"CELL_BODY_READ_CENSUS mode%u sites%zu allocated%llu live%llu peak%llu work%llu\n",mode,sites,
            (unsigned long long)exact.allocated_bytes,(unsigned long long)exact.live_bytes,
            (unsigned long long)exact.peak_bytes,(unsigned long long)exact.work);
        for (size_t point=0;point<sites;++point) {
            context=cell_active_context(caps);baseline=value_compile_stats(&context).live_bytes;
            size_t physical=value_compile_live,bytes=value_compile_bytes;
            value_compile_fail_at=value_compile_calls+point;value_compile_injected=false;
            CHECK(cell_body_read_operation(&context,mode,false)==XR_XIR_OUT_OF_MEMORY && value_compile_injected &&
                value_compile_live==physical && value_compile_bytes==bytes);
            value_compile_fail_at=SIZE_MAX;CHECK(cell_body_read_operation(&context,mode,false)==XR_XIR_OK);
            cell_proof_close(&context,baseline);
        }
        for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
            XrCompileResourceLimits limits=caps;
            if (!axis) limits.allocated_bytes=exact.allocated_bytes-minus;
            else if (axis==1) limits.live_bytes=exact.peak_bytes-minus;
            else limits.work=exact.work-minus;
            context=cell_active_context(limits);baseline=value_compile_stats(&context).live_bytes;
            CHECK(cell_body_read_operation(&context,mode,false)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
            cell_proof_close(&context,baseline);
        }
    }
}
#endif // XIR_CELL_BODY_READ_CASES_H
