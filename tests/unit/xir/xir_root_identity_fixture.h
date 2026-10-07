/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_root_identity_fixture.h - Real VM task, state and cleanup execution
 *
 * KEY CONCEPT:
 *   Independent scalar results and output traces constrain authenticated
 *   root and child execution without consulting the driver's private flags.
 */
#ifndef XIR_ROOT_IDENTITY_FIXTURE_H
#define XIR_ROOT_IDENTITY_FIXTURE_H
#include "xir/xxir_declarations.h"
#include "xir/xxir_instance_value.h"

enum {
    RI_ROOT, RI_CHILD, RI_INIT, RI_ROOT_EXIT, RI_CHILD_EXIT, RI_READ, RI_ENTRY,
    RI_FUNCTIONS, RI_INSTANCES = 2
};
enum { RI_TASK_TYPE = 256, RI_FUNCTION_TYPE, RI_CELL_TYPE };

typedef struct RiGraph {
    XrXirTypeNode nodes[3];
    XrXirTypes types;
    XrXirType parameter;
    XrXirInstruction root[14], child[7], init[5], root_exit[7], child_exit[3], read[2], entry[2];
    XrXirBlock root_blocks[4], child_blocks[2], init_block, root_exit_block, child_exit_block, scalar_block;
    uint32_t root_operands[2], root_exit_operand, child_exit_operand;
    XrXirFunction functions[RI_FUNCTIONS];
    XrXirFunctionIdentity identities[RI_FUNCTIONS];
    XrXirSourceModule source;
    XrXirSlot slots[2];
    XrXirDeclarations declarations;
    XrXirModule module;
} RiGraph;

typedef struct RiTrace { XrXirLifecycleEvent event; uint32_t index; } RiTrace;
typedef struct RiWitness {
    XrXirInstance *instance, *peer;
    XrXirCall *root_call;
    XrXirCallView saved_root;
    const XrXirCallView *active_view;
    XrXirCallView *returned_view;
    XrXirValue function;
    unsigned calls[RI_FUNCTIONS], releases[RI_FUNCTIONS], output_count, lifecycle_count;
    unsigned child_local, await_count, host_suspend_count, root_cleanup_reads, child_cleanup_denials;
    unsigned copied_view_denials, altered_view_denials, release_denials, trace_denials, output_denials;
    int64_t outputs[24], root_cleanup_slot;
    RiTrace lifecycle[12];
    bool saved_root_live;
} RiWitness;
typedef struct RiFixture {
    XrXirArtifact *lowered;
    XrXirProgram *program;
    XrXirVmBinding bindings[RI_FUNCTIONS];
    XrXirCallEntry original[RI_FUNCTIONS];
    RiWitness witnesses[RI_INSTANCES];
    unsigned code_releases;
} RiFixture;

/* This pointer routes test observations only. Every operation still consumes
 * the real supplied view; it never grants or synthesizes execution authority. */
static RiFixture *ri_observed;
static XrXirAction ri_observe_resume(XrXirCallView *view);
static void ri_observe_release(XrXirCallView *view, XrXirCallStatus reason);

static void ri_graph(RiGraph *g, bool fail_initialization) {
    *g = (RiGraph){0};
    g->parameter = XR_XIR_I64;
    g->nodes[0] = (XrXirTypeNode){.kind = XR_XIR_TYPE_TASK, .element = XR_XIR_I64};
    g->nodes[1] = (XrXirTypeNode){.kind = XR_XIR_TYPE_CALLABLE, .result = XR_XIR_I64};
    g->nodes[2] = (XrXirTypeNode){.kind = XR_XIR_TYPE_CELL, .element = XR_XIR_I64};
    g->types = (XrXirTypes){g->nodes, 3, NULL, NULL};
    g->root[0] = (XrXirInstruction){.op = XR_XIR_CLEANUP_REGISTER, .targets = {1}, .immediate = RI_ROOT_EXIT};
    g->root[1] = (XrXirInstruction){.op = XR_XIR_SLOT_LOAD, .type = XR_XIR_I64};
    g->root[2] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 1};
    g->root[3] = (XrXirInstruction){.op = XR_XIR_ADD_INT, .type = XR_XIR_I64, .args = {1, 2}};
    g->root[4] = (XrXirInstruction){.op = XR_XIR_SLOT_STORE, .args = {3}};
    g->root[5] = (XrXirInstruction){.op = XR_XIR_SLOT_LOAD, .type = XR_XIR_I64, .immediate = 1};
    g->root[6] = (XrXirInstruction){.op = XR_XIR_GO, .type = (XrXirType)RI_TASK_TYPE,
        .args = {0, 1}, .immediate = RI_CHILD};
    g->root[7] = (XrXirInstruction){.op = XR_XIR_TASK_AWAIT, .args = {6}, .targets = {2, 3}};
    g->root[8] = (XrXirInstruction){.op = XR_XIR_INVOKE_RESULT, .type = XR_XIR_I64, .immediate = 7};
    g->root[9] = (XrXirInstruction){.op = XR_XIR_PRINT, .args = {1, 1}};
    g->root[10] = (XrXirInstruction){.op = XR_XIR_SUSPEND};
    g->root[11] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {8}};
    g->root[12] = (XrXirInstruction){.op = XR_XIR_INVOKE_ERROR, .type = XR_XIR_ERROR, .immediate = 7};
    g->root[13] = (XrXirInstruction){.op = XR_XIR_THROW, .args = {12}};
    g->root_blocks[0] = (XrXirBlock){.count = 1};
    g->root_blocks[1] = (XrXirBlock){.first = 1, .count = 7, .frontier = 1};
    g->root_blocks[2] = (XrXirBlock){.first = 8, .count = 4, .frontier = 1};
    g->root_blocks[3] = (XrXirBlock){.first = 12, .count = 2, .frontier = 1};
    g->root_operands[0] = 5; g->root_operands[1] = 8;
    g->functions[RI_ROOT] = (XrXirFunction){.name = "root", .name_length = 4, .result = XR_XIR_I64,
        .blocks = g->root_blocks, .block_count = 4, .instructions = g->root, .instruction_count = 14,
        .operands = g->root_operands, .operand_count = 2};
    g->child[0] = (XrXirInstruction){.op = XR_XIR_CLEANUP_REGISTER, .targets = {1}, .immediate = RI_CHILD_EXIT};
    g->child[1] = (XrXirInstruction){.op = XR_XIR_SUSPEND};
    g->child[2] = (XrXirInstruction){.op = XR_XIR_LOCAL_NEW, .type = XR_XIR_I64, .args = {0}};
    g->child[3] = (XrXirInstruction){.op = XR_XIR_LOCAL_READ, .type = XR_XIR_I64, .args = {3}};
    g->child[4] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 2};
    g->child[5] = (XrXirInstruction){.op = XR_XIR_MUL_INT, .type = XR_XIR_I64, .args = {4, 5}};
    g->child[6] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {6}};
    g->child_blocks[0] = (XrXirBlock){.count = 1};
    g->child_blocks[1] = (XrXirBlock){.first = 1, .count = 6, .frontier = 1};
    g->functions[RI_CHILD] = (XrXirFunction){.name = "child", .name_length = 5,
        .parameters = &g->parameter, .parameter_count = 1, .result = XR_XIR_I64,
        .blocks = g->child_blocks, .block_count = 2, .instructions = g->child, .instruction_count = 7};
    g->init[0] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64};
    g->init[1] = (XrXirInstruction){.op = XR_XIR_SLOT_INIT, .args = {0}};
    g->init[2] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 21};
    g->init[3] = (XrXirInstruction){.op = XR_XIR_SLOT_INIT, .args = {2}, .immediate = 1};
    g->init[4] = (XrXirInstruction){.op = fail_initialization ? XR_XIR_MATCH_FAIL : XR_XIR_RETURN};
    g->init_block = (XrXirBlock){.count = 5};
    g->functions[RI_INIT] = (XrXirFunction){.name = "init", .name_length = 4,
        .blocks = &g->init_block, .block_count = 1, .instructions = g->init, .instruction_count = 5};
    g->root_exit[0] = (XrXirInstruction){.op = XR_XIR_SLOT_LOAD, .type = XR_XIR_I64};
    g->root_exit[1] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 10};
    g->root_exit[2] = (XrXirInstruction){.op = XR_XIR_ADD_INT, .type = XR_XIR_I64, .args = {0, 1}};
    g->root_exit[3] = (XrXirInstruction){.op = XR_XIR_SLOT_STORE, .args = {2}};
    g->root_exit[4] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 5};
    g->root_exit[5] = (XrXirInstruction){.op = XR_XIR_PRINT, .args = {0, 1}};
    g->root_exit[6] = (XrXirInstruction){.op = XR_XIR_RETURN};
    g->root_exit_operand = 4;
    g->root_exit_block = (XrXirBlock){.count = 7};
    g->functions[RI_ROOT_EXIT] = (XrXirFunction){.name = "root_exit", .name_length = 9,
        .blocks = &g->root_exit_block, .block_count = 1, .instructions = g->root_exit, .instruction_count = 7,
        .operands = &g->root_exit_operand, .operand_count = 1};
    g->child_exit[0] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 6};
    g->child_exit[1] = (XrXirInstruction){.op = XR_XIR_PRINT, .args = {0, 1}};
    g->child_exit[2] = (XrXirInstruction){.op = XR_XIR_RETURN};
    g->child_exit_block = (XrXirBlock){.count = 3};
    g->functions[RI_CHILD_EXIT] = (XrXirFunction){.name = "child_exit", .name_length = 10,
        .blocks = &g->child_exit_block, .block_count = 1, .instructions = g->child_exit, .instruction_count = 3,
        .operands = &g->child_exit_operand, .operand_count = 1};
    g->read[0] = (XrXirInstruction){.op = XR_XIR_SLOT_LOAD, .type = XR_XIR_I64};
    g->read[1] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {0}};
    g->entry[0] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64};
    g->entry[1] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {0}};
    g->scalar_block = (XrXirBlock){.count = 2};
    g->functions[RI_READ] = (XrXirFunction){.name = "read", .name_length = 4, .result = XR_XIR_I64,
        .blocks = &g->scalar_block, .block_count = 1, .instructions = g->read, .instruction_count = 2};
    g->functions[RI_ENTRY] = (XrXirFunction){.name = "main", .name_length = 4, .result = XR_XIR_I64,
        .blocks = &g->scalar_block, .block_count = 1, .instructions = g->entry, .instruction_count = 2};
    g->identities[RI_ROOT].exported = 1; g->identities[RI_READ].exported = 1;
    g->identities[RI_ROOT_EXIT].cleanup_owner = RI_ROOT + 1;
    g->identities[RI_CHILD_EXIT].cleanup_owner = RI_CHILD + 1;
    g->source = (XrXirSourceModule){"identity", 8, NULL, 0, RI_INIT};
    g->slots[0] = (XrXirSlot){0, XR_XIR_I64, 1};
    g->slots[1] = (XrXirSlot){0, XR_XIR_I64, 0};
    g->declarations = (XrXirDeclarations){.modules = &g->source, .module_count = 1,
        .functions = g->identities, .slots = g->slots, .slot_count = 2, .entry_function = RI_ENTRY};
    g->module = (XrXirModule){.stage = XR_XIR_BUILT, .functions = g->functions,
        .function_count = RI_FUNCTIONS, .declarations = &g->declarations, .types = &g->types};
}

static void ri_code_drop(void *owner) {
    RiFixture *f = owner;
    CHECK(!f->code_releases && f->lowered);
    ++f->code_releases;
    xr_xir_compile_artifact_free(f->lowered); f->lowered = NULL;
}
static void ri_fixture(RiFixture *f, bool fail_initialization) {
    CHECK(!ri_observed); *f = (RiFixture){0}; ri_observed = f;
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    RiGraph graph; ri_graph(&graph, fail_initialization);
    XrXirArtifact *checked = NULL, *decoded = NULL, *closed = NULL;
    XrXirCheckedPacket packet = {0}; XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_check(context, &graph.module, &checked, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "identity check status=%u function=%u op=%u reason=%u\n",
        status, diagnostic.function, diagnostic.instruction, diagnostic.reason);
    CHECK(status == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(checked, &packet, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); memset(&graph, 0xa5, sizeof(graph));
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &decoded, &diagnostic) == XR_XIR_OK);
    memset(packet.bytes, 0xa5, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded, &closed, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);
    CHECK(xr_xir_compile_artifact_verify(closed, &diagnostic) == XR_XIR_OK);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &f->lowered, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    XrXirCallEntry entries[RI_FUNCTIONS];
    for (uint32_t i = 0; i < RI_FUNCTIONS; ++i) {
        CHECK(xr_xir_compile_vm_bind(f->lowered, i, &f->bindings[i], &f->original[i]) == XR_XIR_OK);
        entries[i] = f->original[i];
        entries[i].resume = ri_observe_resume; entries[i].release = ri_observe_release;
    }
    const XrXirModule *module = xr_xir_compile_artifact_module(f->lowered);
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, target, entries, RI_FUNCTIONS,
        module->declarations, {f, ri_code_drop}, module->types, xr_xir_compile_program_proof(f->lowered)};
    CHECK(xr_xir_compile_program_seal(context, &spec, &f->program) == XR_XIR_OK && f->program);
}
#endif // XIR_ROOT_IDENTITY_FIXTURE_H
