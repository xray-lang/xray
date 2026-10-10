/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_cell_provenance.c - Independent origin, scope and physical owner oracles
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_cell_provenance_internal.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_value_compile_owner.h"
#include "xir_cell_provenance_fixture.h"
static XrXirCompileContext cell_proof_context(uint64_t work) {
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    XrCompileResourceLimits limits = {33554432,33554432,work};
    CHECK(xr_compile_resources_new(&limits,&context.resources) == XR_COMPILE_RESOURCE_OK);
    return context;
}
static void cell_proof_close(XrXirCompileContext *context, uint64_t baseline) {
    CHECK(value_compile_stats(context).live_bytes == baseline);
    xr_compile_resources_release(context->resources); context->resources = NULL;
    CHECK(!value_compile_live && !value_compile_bytes);
}
static void cell_proof_view(const XrXirCompileContext *context, XrXirCellProvenance *proof,
    uint32_t function, uint32_t value, bool dependency) {
    XrXirCellOriginView view = {0};
    CHECK(xr_xir_compile_cell_origin_view(context,proof,function,value,&view) == XR_XIR_OK);
    CHECK(!view.intrinsic_mask);
    if (dependency) CHECK(view.parameter_count == 1 && view.word_count == 1 &&
        view.parameters[0] == 0 && view.dependencies[0] == 1);
    else CHECK(!view.word_count && !view.parameter_count && !view.dependencies && !view.parameters);
}
static void cell_proof_actual_edges(void) {
    CellProofFixture fixture; cell_proof_fixture(&fixture);
    XrXirCompileContext context = cell_proof_context(33554432);
    uint64_t baseline = value_compile_stats(&context).live_bytes;
    CHECK(xir_fixture_verify(&context,&fixture.module,NULL) == XR_XIR_OK);
    XrXirCellProvenance *proof = NULL;
    CHECK(xr_xir_compile_cell_provenance_verified(&context,&fixture.module,&proof,NULL) == XR_XIR_OK);
    CHECK(xr_xir_cell_provenance_origin(proof,1,1) == XR_XIR_CELL_ORIGIN_OWNED);
    CHECK(xr_xir_cell_provenance_role(proof,2,0) == XR_XIR_CELL_PROOF_SCOPED_REF);
    CHECK(xr_xir_cell_provenance_role(proof,3,0) == XR_XIR_CELL_PROOF_OWNED_CAPTURE);
    CHECK(xr_xir_cell_provenance_role(proof,4,0) == XR_XIR_CELL_PROOF_LEXICAL_CLEANUP);
    CHECK(xr_xir_cell_provenance_role(proof,5,0) == XR_XIR_CELL_PROOF_UNKNOWN);
    cell_proof_view(&context,proof,1,1,false); cell_proof_view(&context,proof,2,0,true);
    cell_proof_view(&context,proof,3,0,false); cell_proof_view(&context,proof,4,0,true);
    cell_proof_view(&context,proof,5,0,true);
    uint8_t owner = XR_XIR_CELL_ACCESS_ROOT; uint32_t access = 99;
    XrXirCellAccessRequest request = {2,0,&owner,1};
    CHECK(xr_xir_compile_cell_access(&context,proof,&request,&access) == XR_XIR_OK && access == 1);
    owner = 0; CHECK(xr_xir_compile_cell_access(&context,proof,&request,&access) == XR_XIR_OK && !access);
    request.parameter_owners = NULL; request.parameter_count = 0;
    CHECK(xr_xir_compile_cell_access(&context,proof,&request,&access) == XR_XIR_OK && access == 2);
    XrXirCellProvenance *occupied = proof;
    CHECK(xr_xir_compile_cell_provenance_verified(&context,&fixture.module,&occupied,NULL) == XR_XIR_BAD_STRUCTURE && occupied == proof);
    uint32_t offsets[7] = {0}; uint8_t roles[4] = {0};
    CHECK(xr_xir_compile_cell_roles_verified(&context,&fixture.module,offsets,roles,NULL) == XR_XIR_OK);
    const uint32_t expected_offsets[] = {0,0,0,1,2,3,4}; const uint8_t expected_roles[] = {2,1,3,0};
    CHECK(!memcmp(offsets,expected_offsets,sizeof(offsets)) && !memcmp(roles,expected_roles,sizeof(roles)));
    /* Every source buffer dies while the finite proof remains independently owned. */
    memset(&fixture,0xCC,sizeof(fixture)); cell_proof_view(&context,proof,2,0,true);
    XrXirCompileContext foreign = cell_proof_context(33554432);
    XrXirCellOriginView sentinel = {77,NULL,77,NULL,77}, unchanged = sentinel;
    CHECK(xr_xir_compile_cell_origin_view(&foreign,proof,2,0,&sentinel) == XR_XIR_BAD_STRUCTURE &&
        !memcmp(&sentinel,&unchanged,sizeof(sentinel)));
    xr_compile_resources_release(foreign.resources);
    xr_xir_compile_cell_provenance_free(proof); cell_proof_close(&context,baseline);
}
static void cell_proof_rejections(void) {
    XrXirCompileContext context = cell_proof_context(33554432);
    uint64_t baseline = value_compile_stats(&context).live_bytes;
    CellProofFixture fixture; cell_proof_fixture(&fixture);
    fixture.entry[3].immediate = 3;
    CHECK(xir_fixture_verify(&context,&fixture.module,NULL) == XR_XIR_BAD_TYPE);
    cell_proof_fixture(&fixture);
    uint32_t capture = 0;
    XrXirInstruction recursive[] = {
        {XR_XIR_FUNCTION_REF,(XrXirType)257,{0,1},{0},3,{0}},
        {XR_XIR_CELL_READ,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0,{0}}};
    fixture.functions[2].instructions = recursive; fixture.functions[2].instruction_count = 3;
    fixture.functions[2].blocks = &fixture.three; fixture.functions[2].operands = &capture; fixture.functions[2].operand_count = 1;
    CHECK(xir_fixture_verify(&context,&fixture.module,NULL) == XR_XIR_BAD_VALUE);
    cell_proof_fixture(&fixture);
    recursive[0].immediate = 5;
    fixture.functions[5].instructions = recursive; fixture.functions[5].instruction_count = 3;
    fixture.functions[5].blocks = &fixture.three; fixture.functions[5].operands = &capture; fixture.functions[5].operand_count = 1;
    CHECK(xir_fixture_verify(&context,&fixture.module,NULL) == XR_XIR_BAD_VALUE);
    /* A real CELL_NEW incoming edge roots the same recursive capture shape. */
    cell_proof_fixture(&fixture); recursive[0].immediate = 3;
    fixture.functions[3].instructions = recursive; fixture.functions[3].instruction_count = 3;
    fixture.functions[3].blocks = &fixture.three; fixture.functions[3].operands = &capture; fixture.functions[3].operand_count = 1;
    CHECK(xir_fixture_verify(&context,&fixture.module,NULL) == XR_XIR_OK);
    XrXirCellProvenance *proof = NULL;
    CHECK(xr_xir_compile_cell_provenance_verified(&context,&fixture.module,&proof,NULL) == XR_XIR_OK);
    CHECK(xr_xir_cell_provenance_origin(proof,3,0) == XR_XIR_CELL_ORIGIN_OWNED);
    xr_xir_compile_cell_provenance_free(proof);
    cell_proof_fixture(&fixture);
    XrXirSlot slot = {0,(XrXirType)256,1}; fixture.declarations.slots = &slot; fixture.declarations.slot_count = 1;
    XrXirInstruction initializer[] = {
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},7,{0}},
        {XR_XIR_CELL_NEW,(XrXirType)256,{0},{0},0,{0}},
        {XR_XIR_SLOT_INIT,XR_XIR_UNIT,{1},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirBlock four = {0,4,0,0}; fixture.functions[0].instructions = initializer;
    fixture.functions[0].instruction_count = 4; fixture.functions[0].blocks = &four;
    fixture.entry[1] = (XrXirInstruction){XR_XIR_SLOT_LOAD,(XrXirType)256,{0},{0},0,{0}};
    CHECK(xir_fixture_verify(&context,&fixture.module,NULL) == XR_XIR_BAD_VALUE);
    cell_proof_close(&context,baseline);
}
static void cell_proof_resource_failures(void) {
    CellProofFixture fixture; cell_proof_fixture(&fixture);
    XrXirCompileContext context = cell_proof_context(33554432);
    uint64_t baseline = value_compile_stats(&context).live_bytes;
    CHECK(xir_fixture_verify(&context,&fixture.module,NULL) == XR_XIR_OK);
    cell_proof_close(&context,baseline);
    context = cell_proof_context(33554432); baseline = value_compile_stats(&context).live_bytes;
    size_t before = value_compile_calls; XrXirCellProvenance *proof = NULL;
    CHECK(xr_xir_compile_cell_provenance_verified(&context,&fixture.module,&proof,NULL) == XR_XIR_OK);
    size_t sites = value_compile_calls-before; uint64_t exact_work = value_compile_stats(&context).work;
    CHECK(sites && exact_work); xr_xir_compile_cell_provenance_free(proof); cell_proof_close(&context,baseline);
    for (size_t point = 0; point < sites; ++point) {
        context = cell_proof_context(33554432); baseline = value_compile_stats(&context).live_bytes;
        size_t physical_blocks = value_compile_live, physical_bytes = value_compile_bytes;
        value_compile_fail_at = value_compile_calls+point; value_compile_injected = false; proof = NULL;
        CHECK(xr_xir_compile_cell_provenance_verified(&context,&fixture.module,&proof,NULL) == XR_XIR_OUT_OF_MEMORY &&
            value_compile_injected && !proof && value_compile_live == physical_blocks && value_compile_bytes == physical_bytes);
        value_compile_fail_at = SIZE_MAX; cell_proof_close(&context,baseline);
    }
    for (uint32_t one_less = 0; one_less < 2; ++one_less) {
        context = cell_proof_context(exact_work-one_less); baseline = value_compile_stats(&context).live_bytes; proof = NULL;
        XrXirStatus status = xr_xir_compile_cell_provenance_verified(&context,&fixture.module,&proof,NULL);
        CHECK(one_less ? status == XR_XIR_BUDGET && !proof : status == XR_XIR_OK && proof);
        xr_xir_compile_cell_provenance_free(proof); cell_proof_close(&context,baseline);
    }
}
static void cell_proof_escape_boundaries(void) {
    XrXirCompileContext context = cell_proof_context(33554432);
    uint64_t baseline = value_compile_stats(&context).live_bytes;
    CellProofFixture fixture; cell_proof_fixture(&fixture);
    XrXirInstruction returning[] = {
        {XR_XIR_COPY,(XrXirType)256,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}}};
    fixture.functions[5].result = (XrXirType)256; fixture.functions[5].instructions = returning;
    CHECK(xir_fixture_verify(&context,&fixture.module,NULL) != XR_XIR_OK);
    cell_proof_fixture(&fixture);
    XrXirTypeNode nodes[3]; memcpy(nodes,fixture.nodes,sizeof(fixture.nodes));
    nodes[2] = (XrXirTypeNode){.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_I64};
    fixture.types.nodes = nodes; fixture.types.count = 3;
    uint32_t actual = 0;
    XrXirInstruction go[] = {
        {XR_XIR_GO,(XrXirType)258,{0,1},{0},2,{0}},
        {XR_XIR_CELL_READ,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0,{0}}};
    fixture.functions[5].instructions = go; fixture.functions[5].instruction_count = 3;
    fixture.functions[5].blocks = &fixture.three; fixture.functions[5].operands = &actual; fixture.functions[5].operand_count = 1;
    CHECK(xir_fixture_verify(&context,&fixture.module,NULL) != XR_XIR_OK);
    cell_proof_fixture(&fixture); memcpy(nodes,fixture.nodes,sizeof(fixture.nodes));
    nodes[2] = (XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)256};
    fixture.types.nodes = nodes; fixture.types.count = 3;
    XrXirInstruction aggregate[] = {
        {XR_XIR_ARRAY_NEW,(XrXirType)258,{0,1},{0},0,{0}},
        {XR_XIR_CELL_READ,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0,{0}}};
    fixture.functions[5].instructions = aggregate; fixture.functions[5].instruction_count = 3;
    fixture.functions[5].blocks = &fixture.three; fixture.functions[5].operands = &actual; fixture.functions[5].operand_count = 1;
    CHECK(xir_fixture_verify(&context,&fixture.module,NULL) != XR_XIR_OK);
    cell_proof_close(&context,baseline);
}
static void cell_proof_callable_modes(void) {
    XrXirCompileContext context = cell_proof_context(33554432);
    uint64_t baseline = value_compile_stats(&context).live_bytes;
    XrXirCallableParameter parameter = {XR_XIR_I64,XR_PARAM_READ};
    XrXirTypeNode nodes[] = {
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_UNIT},
        {.kind=XR_XIR_TYPE_CALLABLE,.parameters=&parameter,.parameter_count=1,
            .result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED}};
    XrXirTypes types = {nodes,3,NULL,NULL};
    const struct {XrXirType type; uint32_t mode; XrXirStatus status;} cases[] = {
        {XR_XIR_I64,XR_PARAM_READ,XR_XIR_OK},
        {(XrXirType)256,XR_PARAM_REF,XR_XIR_OK},
        {(XrXirType)257,XR_PARAM_REF,XR_XIR_OK},
        {(XrXirType)256,XR_PARAM_READ,XR_XIR_BAD_TYPE},
        {XR_XIR_I64,XR_PARAM_REF,XR_XIR_BAD_TYPE},
        {(XrXirType)256,XR_PARAM_MOVE,XR_XIR_BAD_TYPE},
        {(XrXirType)256,99,XR_XIR_BAD_TYPE}};
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        parameter = (XrXirCallableParameter){cases[i].type,cases[i].mode};
        CHECK(xr_xir_compile_type_descriptors_verify(&context,&types) == cases[i].status);
    }
    cell_proof_close(&context,baseline);
}
static void cell_proof_indirect_ref(void) {
    CellProofFixture fixture; cell_proof_fixture(&fixture);
    XrXirCompileContext context = cell_proof_context(33554432);
    uint64_t baseline = value_compile_stats(&context).live_bytes;
    XrXirCallableParameter parameter = {(XrXirType)256,XR_PARAM_REF};
    fixture.nodes[1].parameters = &parameter; fixture.nodes[1].parameter_count = 1;
    fixture.entry[2].args[1] = 0; fixture.entry[2].immediate = 2;
    fixture.entry[3].args[0] = 0;
    fixture.entry[4].args[0] = 1; fixture.entry[4].args[1] = 1;
    XrXirDiagnostic diagnostic = {XR_XIR_OK,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
    XrXirStatus status = xir_fixture_verify(&context,&fixture.module,&diagnostic);
    if (status != XR_XIR_OK)
        fprintf(stderr,"indirect-ref baseline status=%u diagnostic=%u function=%u block=%u instruction=%u reason=%u\n",
            (unsigned)status,(unsigned)diagnostic.status,diagnostic.function,diagnostic.block,
            diagnostic.instruction,(unsigned)diagnostic.reason);
    CHECK(status == XR_XIR_OK);
    XrXirCellProvenance *proof = NULL;
    CHECK(xr_xir_compile_cell_provenance_verified(&context,&fixture.module,&proof,NULL) == XR_XIR_OK);
    CHECK(xr_xir_cell_provenance_role(proof,2,0) == XR_XIR_CELL_PROOF_SCOPED_REF);
    CHECK(xr_xir_cell_provenance_role(proof,3,0) == XR_XIR_CELL_PROOF_UNKNOWN);
    CHECK(xr_xir_cell_provenance_role(proof,5,0) == XR_XIR_CELL_PROOF_UNKNOWN);
    CHECK(xr_xir_cell_provenance_origin(proof,2,0) == XR_XIR_CELL_ORIGIN_SCOPED);
    cell_proof_view(&context,proof,2,0,true);
    xr_xir_compile_cell_provenance_free(proof);
    parameter.mode = XR_PARAM_READ;
    CHECK(xir_fixture_verify(&context,&fixture.module,NULL) == XR_XIR_BAD_TYPE);
    parameter.mode = XR_PARAM_REF; parameter.type = XR_XIR_I64;
    CHECK(xir_fixture_verify(&context,&fixture.module,NULL) == XR_XIR_BAD_TYPE);
    /* A capture prefix cannot reuse the unbound scoped-ref protocol. */
    parameter.type = (XrXirType)256;
    XrXirType captured_parameters[] = {(XrXirType)256,(XrXirType)256};
    fixture.functions[3].parameters = captured_parameters; fixture.functions[3].parameter_count = 2;
    XrXirInstruction read[] = {{XR_XIR_CELL_READ,XR_XIR_I64,{1},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0,{0}}};
    fixture.functions[3].instructions = read;
    fixture.entry[2].args[1] = 1; fixture.entry[2].immediate = 3;
    CHECK(xir_fixture_verify(&context,&fixture.module,NULL) == XR_XIR_BAD_TYPE);
    cell_proof_close(&context,baseline);
}
#include "xir_cell_active_domain_cases.h"
int main(void) {
    cell_active_domains();
    cell_proof_callable_modes(); cell_proof_indirect_ref();
    cell_proof_actual_edges(); cell_proof_rejections(); cell_proof_escape_boundaries(); cell_proof_resource_failures();
    puts("Cell actual-edge roles, owned versus scoped provenance, noescape and physical resource exits passed");
    return 0;
}
