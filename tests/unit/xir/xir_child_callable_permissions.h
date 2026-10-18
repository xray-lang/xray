#include "xir/xxir_program_packet_internal.h"
#include "xir_construction_fixture.h"
/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_child_callable_permissions.h - Authentic task and local closure execution
 *
 * KEY CONCEPT:
 *   Local closure calls and transport proofs have independent obligations.
 */
#ifndef XIR_CHILD_CALLABLE_PERMISSIONS_H
#define XIR_CHILD_CALLABLE_PERMISSIONS_H

#include "xir_program_final_roots.h"
enum { XRCP_INIT, XRCP_ENTRY, XRCP_ROOT, XRCP_CHILD, XRCP_PURE, XRCP_RELAY, XRCP_ROOT_LEAF,
    XRCP_UNKNOWN, XRCP_MIXED, XRCP_FACTORY, XRCP_FUNCTIONS, XRCP_INSTANCES = 2 };
enum { XRCP_TASK = 256, XRCP_NONE, XRCP_REQUIRED, XRCP_UNRESOLVED, XRCP_BOTH, XRCP_CELL };
enum { XRCP_PURE_NONE, XRCP_PURE_ROOT, XRCP_PURE_UNKNOWN, XRCP_PURE_MIXED,
    XRCP_ROOT_VALUE, XRCP_CAPTURED, XRCP_CARRIERS };

typedef struct CpGraph {
    XrXirTypeNode nodes[6];
    XrXirTypes types;
    XrXirType none_parameter, unresolved_parameter;
    XrXirInstruction init[4], scalar[2], child[9], root[7], relay[2], leaf[3], unknown[3], mixed[6], unit;
    XrXirBlock init_block, scalar_block, child_blocks[3], root_blocks[3], pair_block, leaf_block, unknown_block, mixed_block, unit_block;
    uint32_t child_operands[2], root_operand;
    XrXirFunction functions[XRCP_FUNCTIONS];
    XrXirFunctionIdentity identities[XRCP_FUNCTIONS];
    XrXirSourceModule source;
    XrXirSlot slot;
    XrXirDeclarations declarations;
    XrXirModule module;
} CpGraph;

typedef struct CpWitness {
    XrXirInstance *instance;
    XrXirCall *root_call;
    XrXirCallView saved_root;
    XrXirValue values[XRCP_CARRIERS];
    unsigned calls[XRCP_FUNCTIONS], releases[XRCP_FUNCTIONS], probes, child_callbacks, typed_outputs, byte_outputs;
    unsigned await_actions;
    XrXirOutputSink sink;
} CpWitness;

typedef struct CpFixture {
    const XrXirCompileContext *context;
    XrXirArtifact *lowered;
    XrXirProgram *program;
    XrXirVmBinding bindings[XRCP_FUNCTIONS];
    XrXirCallEntry original[XRCP_FUNCTIONS];
    CpWitness witnesses[XRCP_INSTANCES];
    unsigned code_releases;
} CpFixture;

static CpFixture *cp_observed;
static XrXirAction cp_resume(XrXirCallView *view);
static void cp_release(XrXirCallView *view, XrXirCallStatus reason);

static void cp_graph(CpGraph *g) {
    *g = (CpGraph){0};
    g->none_parameter = (XrXirType)XRCP_NONE;
    g->unresolved_parameter = (XrXirType)XRCP_UNRESOLVED;
    g->nodes[0] = (XrXirTypeNode){.kind = XR_XIR_TYPE_TASK, .element = XR_XIR_I64};
    for (unsigned i = 1; i < 5; ++i)
        g->nodes[i] = (XrXirTypeNode){.kind = XR_XIR_TYPE_CALLABLE, .result = XR_XIR_I64};
    g->nodes[1].flags = XR_XIR_CALLABLE_ROOT_NONE;
    g->nodes[2].flags = XR_XIR_CALLABLE_ROOT_REQUIRED;
    g->nodes[3].flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    g->nodes[4].flags = XR_XIR_CALLABLE_ROOT_REQUIRED | XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    g->nodes[5] = (XrXirTypeNode){.kind = XR_XIR_TYPE_CELL, .element = XR_XIR_I64};
    g->types = (XrXirTypes){g->nodes, 6, NULL, NULL};
    g->unit = (XrXirInstruction){.op = XR_XIR_RETURN};
    g->unit_block = (XrXirBlock){.count = 1};
    g->init[0] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 7};
    g->init[1] = (XrXirInstruction){.op = XR_XIR_CELL_NEW, .type = (XrXirType)XRCP_CELL, .args = {0}};
    g->init[2] = (XrXirInstruction){.op = XR_XIR_SLOT_INIT, .args = {1}};
    g->init[3] = g->unit;
    g->init_block = (XrXirBlock){.count = 4};
    g->scalar[0] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 42};
    g->scalar[1] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {0}};
    g->scalar_block = (XrXirBlock){.count = 2};
    g->relay[0] = (XrXirInstruction){.op = XR_XIR_CALL_INDIRECT, .type = XR_XIR_I64, .immediate = 0};
    g->relay[1] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {1}};
    g->pair_block = (XrXirBlock){.count = 2};
    g->leaf[0] = (XrXirInstruction){.op = XR_XIR_SLOT_LOAD, .type = (XrXirType)XRCP_CELL};
    g->leaf[1] = (XrXirInstruction){.op = XR_XIR_CELL_READ, .type = XR_XIR_I64, .args = {0}};
    g->leaf[2] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {1}};
    g->leaf_block = (XrXirBlock){.count = 3};
    /* A known FUNCTION_REF has exact body authority even under a weaker type.
     * Only an unresolved argument supplies a genuinely unknown call target. */
    g->unknown[0] = (XrXirInstruction){.op = XR_XIR_CALL_INDIRECT, .type = XR_XIR_I64};
    g->unknown[1] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {1}};
    g->unknown_block = (XrXirBlock){.count = 2};
    g->mixed[0] = g->leaf[0];
    g->mixed[1] = (XrXirInstruction){.op = XR_XIR_CELL_READ, .type = XR_XIR_I64, .args = {1}};
    g->mixed[2] = g->unknown[0];
    g->mixed[3] = (XrXirInstruction){.op = XR_XIR_ADD_INT, .type = XR_XIR_I64, .args = {2, 3}};
    g->mixed[4] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {4}};
    g->mixed_block = (XrXirBlock){.count = 5};
    g->child_operands[0] = 0;
    g->child_operands[1] = 3;
    g->child[0] = (XrXirInstruction){.op = XR_XIR_FUNCTION_REF, .type = (XrXirType)XRCP_NONE, .immediate = XRCP_PURE};
    g->child[1] = (XrXirInstruction){.op = XR_XIR_FUNCTION_REF, .type = (XrXirType)XRCP_NONE,
        .args = {0, 1}, .immediate = XRCP_RELAY};
    g->child[2] = (XrXirInstruction){.op = XR_XIR_INVOKE_INDIRECT, .type = XR_XIR_I64,
        .immediate = 1, .targets = {1, 2}};
    g->child[3] = (XrXirInstruction){.op = XR_XIR_INVOKE_RESULT, .type = XR_XIR_I64, .immediate = 2};
    g->child[4] = (XrXirInstruction){.op = XR_XIR_PRINT, .args = {1, 1}};
    g->child[5] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {3}};
    g->child[6] = (XrXirInstruction){.op = XR_XIR_INVOKE_ERROR, .type = XR_XIR_ERROR, .immediate = 2};
    g->child[7] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = -999};
    g->child[8] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {7}};
    g->child_blocks[0] = (XrXirBlock){.count = 3};
    g->child_blocks[1] = (XrXirBlock){.first = 3, .count = 3};
    g->child_blocks[2] = (XrXirBlock){.first = 6, .count = 3};
    g->root[0] = (XrXirInstruction){.op = XR_XIR_GO, .type = (XrXirType)XRCP_TASK, .immediate = XRCP_CHILD};
    g->root[1] = (XrXirInstruction){.op = XR_XIR_TASK_AWAIT, .args = {0}, .targets = {1, 2}};
    g->root[2] = (XrXirInstruction){.op = XR_XIR_INVOKE_RESULT, .type = XR_XIR_I64, .immediate = 1};
    g->root[3] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {2}};
    g->root[4] = (XrXirInstruction){.op = XR_XIR_INVOKE_ERROR, .type = XR_XIR_ERROR, .immediate = 1};
    g->root[5] = (XrXirInstruction){.op = XR_XIR_THROW, .args = {4}};
    g->root_blocks[0] = (XrXirBlock){.count = 2};
    g->root_blocks[1] = (XrXirBlock){.first = 2, .count = 2};
    g->root_blocks[2] = (XrXirBlock){.first = 4, .count = 2};
    g->functions[XRCP_INIT] = (XrXirFunction){"init", 4, NULL, 0, XR_XIR_UNIT,
        &g->init_block, 1, g->init, 4, NULL, 0};
    g->functions[XRCP_ENTRY] = (XrXirFunction){"entry", 5, NULL, 0, XR_XIR_I64,
        &g->scalar_block, 1, g->scalar, 2, NULL, 0};
    g->functions[XRCP_ROOT] = (XrXirFunction){"root", 4, NULL, 0, XR_XIR_I64,
        g->root_blocks, 3, g->root, 6, NULL, 0};
    g->functions[XRCP_CHILD] = (XrXirFunction){"child", 5, NULL, 0, XR_XIR_I64,
        g->child_blocks, 3, g->child, 9, g->child_operands, 2};
    g->functions[XRCP_PURE] = (XrXirFunction){"pure", 4, NULL, 0, XR_XIR_I64,
        &g->scalar_block, 1, g->scalar, 2, NULL, 0};
    g->functions[XRCP_RELAY] = (XrXirFunction){"relay", 5, &g->none_parameter, 1, XR_XIR_I64,
        &g->pair_block, 1, g->relay, 2, NULL, 0};
    g->functions[XRCP_ROOT_LEAF] = (XrXirFunction){"rootLeaf", 8, NULL, 0, XR_XIR_I64,
        &g->leaf_block, 1, g->leaf, 3, NULL, 0};
    g->functions[XRCP_UNKNOWN] = (XrXirFunction){"unknown", 7, &g->unresolved_parameter, 1, XR_XIR_I64,
        &g->unknown_block, 1, g->unknown, 2, NULL, 0};
    g->functions[XRCP_MIXED] = (XrXirFunction){"mixed", 5, &g->unresolved_parameter, 1, XR_XIR_I64,
        &g->mixed_block, 1, g->mixed, 5, NULL, 0};
    g->functions[XRCP_FACTORY] = (XrXirFunction){"factory", 7, NULL, 0, XR_XIR_UNIT,
        &g->unit_block, 1, &g->unit, 1, NULL, 0};
    g->identities[XRCP_ROOT].exported = 1;
    g->identities[XRCP_FACTORY].exported = 1;
    g->source = (XrXirSourceModule){"childCallable", 13, NULL, 0, XRCP_INIT};
    /* Mutable module storage is an authentic Cell, never a naked scalar slot. */
    g->slot = (XrXirSlot){0, (XrXirType)XRCP_CELL, 1};
    g->declarations = (XrXirDeclarations){.modules = &g->source, .module_count = 1,
        .functions = g->identities, .slots = &g->slot, .slot_count = 1, .entry_function = XRCP_ENTRY};
    g->module = (XrXirModule){.stage = XR_XIR_BUILT, .functions = g->functions,
        .function_count = XRCP_FUNCTIONS, .declarations = &g->declarations, .types = &g->types,
        .linkage_kind = XR_XIR_PROGRAM};
}

static void cp_transport_rejection(const XrXirCompileContext *context) {
    CpGraph g;
    cp_graph(&g);
    g.root_operand = 0;
    g.root[0] = (XrXirInstruction){.op = XR_XIR_FUNCTION_REF, .type = (XrXirType)XRCP_NONE, .immediate = XRCP_PURE};
    g.root[1] = (XrXirInstruction){.op = XR_XIR_GO, .type = (XrXirType)XRCP_TASK,
        .args = {0, 1}, .immediate = XRCP_RELAY};
    g.root[2] = (XrXirInstruction){.op = XR_XIR_TASK_AWAIT, .args = {1}, .targets = {1, 2}};
    g.root[3] = (XrXirInstruction){.op = XR_XIR_INVOKE_RESULT, .type = XR_XIR_I64, .immediate = 2};
    g.root[4] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {3}};
    g.root[5] = (XrXirInstruction){.op = XR_XIR_INVOKE_ERROR, .type = XR_XIR_ERROR, .immediate = 2};
    g.root[6] = (XrXirInstruction){.op = XR_XIR_THROW, .args = {5}};
    g.root_blocks[0].count = 3;
    g.root_blocks[1].first = 3;
    g.root_blocks[2].first = 5;
    g.functions[XRCP_ROOT].instruction_count = 7;
    g.functions[XRCP_ROOT].operands = &g.root_operand;
    g.functions[XRCP_ROOT].operand_count = 1;
    XrXirArtifact *rejected = NULL;
    XrXirDiagnostic d = {0};
    XrXirStatus status = xir_fixture_check(context, &g.module, &rejected, &d);
    printf("child callable transport status=%u function=%u instruction=%u reason=%u\n",
        status, d.function, d.instruction, d.reason);
    CHECK(status == XR_XIR_BAD_TYPE && !rejected);
}

static void cp_code_drop(void *opaque) {
    CpFixture *f = opaque;
    CHECK(f->lowered && !f->code_releases);
    ++f->code_releases;
    xr_xir_compile_artifact_free(f->lowered);
    f->lowered = NULL;
}

static void cp_build(CpFixture *f) {
    CHECK(!cp_observed);
    *f = (CpFixture){0};
    cp_observed = f;
    f->context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    CpGraph g;
    cp_graph(&g);
    CHECK(xr_xir_compile_types_structure_verify(f->context, &g.types) == XR_XIR_OK);
    CHECK(xr_xir_compile_declarations_verify(f->context, &g.declarations, &g.types,
        XRCP_FUNCTIONS, g.module.linkage_kind) == XR_XIR_OK);
    XrXirArtifact *checked = NULL, *decoded = NULL, *closed = NULL;
    XrXirDiagnostic d = {0};
    XrXirStatus status = xir_fixture_check(f->context, &g.module, &checked, &d);
    if (status != XR_XIR_OK) fprintf(stderr, "child graph status=%u function=%u instruction=%u reason=%u\n",
        status, d.function, d.instruction, d.reason);
    CHECK(status == XR_XIR_OK);
    XrXirProofContext proof = {xr_xir_compile_artifact_module(checked), {XR_XIR_CONTEXT_CLOSED, 0, 0}};
    CHECK(xr_xir_compile_type_markers_prove(f->context, &proof, (XrXirType)XRCP_NONE,
        XR_XIR_CONSTRAINT_SENDABLE) == XR_XIR_BAD_TYPE);
    cp_transport_rejection(f->context);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, &d) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    memset(&g, 0xa5, sizeof(g));
    CHECK(xr_xir_compile_checked_read(f->context, packet.bytes, packet.length, &decoded, &d) == XR_XIR_OK);
    memset(packet.bytes, 0xa5, packet.length);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded, &closed, &d) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);
    CHECK(xr_xir_compile_artifact_verify(closed, &d) == XR_XIR_OK);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &f->lowered, &d) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    const XrXirModule *module = xr_xir_compile_artifact_module(f->lowered);
    CHECK(module->function_count == XRCP_FUNCTIONS);
    XrXirCallEntry entries[XRCP_FUNCTIONS];
    for (uint32_t i = 0; i < XRCP_FUNCTIONS; ++i) {
        CHECK(xr_xir_compile_vm_bind(f->lowered, i, &f->bindings[i], &f->original[i]) == XR_XIR_OK);
        entries[i] = f->original[i];
        entries[i].resume = cp_resume;
        entries[i].release = cp_release;
    }
    const XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, target, entries, XRCP_FUNCTIONS,
        module->declarations, {f, cp_code_drop}, module->types, xir_program_packet_borrow(f->lowered)};
    XrXirProgramSpec retired=spec;retired.abi_version=29;
    size_t attempts=runtime_attempts;XrXirProgram *refused=NULL;
    CHECK(xr_xir_compile_program_seal(f->context,&retired,&refused)==XR_XIR_BAD_LAYOUT);
    CHECK(!refused&&runtime_attempts==attempts&&!f->code_releases);
    for(uint32_t i=0;i<XRCP_FUNCTIONS;++i)
        CHECK(module->declarations->functions[i].test_role==XR_XIR_TEST_ROLE_NONE&&
            !module->declarations->functions[i].test_timeout_seconds);
    CHECK(xr_xir_compile_program_seal(f->context, &spec, &f->program) == XR_XIR_OK);
    CHECK(!test_program_final_roots(f->program,XRCP_PURE).requires_root && !test_program_final_roots(f->program,XRCP_PURE).unresolved);
    CHECK(!test_program_final_roots(f->program,XRCP_RELAY).requires_root && !test_program_final_roots(f->program,XRCP_RELAY).unresolved);
    CHECK(f->program->permissions->entries[XRCP_RELAY].worker == XR_XIR_BAD_TYPE);
    CHECK(f->program->permissions->entries[XRCP_CHILD].worker == XR_XIR_OK);
    CHECK(test_program_final_roots(f->program,XRCP_ROOT_LEAF).requires_root && !test_program_final_roots(f->program,XRCP_ROOT_LEAF).unresolved);
    CHECK(!test_program_final_roots(f->program,XRCP_UNKNOWN).requires_root && test_program_final_roots(f->program,XRCP_UNKNOWN).unresolved);
    CHECK(test_program_final_roots(f->program,XRCP_MIXED).requires_root && test_program_final_roots(f->program,XRCP_MIXED).unresolved);
}
#endif // XIR_CHILD_CALLABLE_PERMISSIONS_H
