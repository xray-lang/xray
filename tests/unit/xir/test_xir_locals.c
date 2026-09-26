/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_locals.c - Place admission and executable ownership checks
 *
 * KEY CONCEPT:
 *   Canonical packets must preserve place roles and dominance.
 */
#include "xir/xxir_vm.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_local_fixture.h"
#include "xir_local_cases.h"
static void rejected_places(const XrXirArtifact *artifact) {
    XrXirModule module = *xr_xir_artifact_module(artifact);
    XrXirFunction function = module.functions[0];
    XrXirInstruction ops[11]; module.functions = &function; module.function_count = 1; function.instructions = ops;
    for (unsigned mode = 0; mode < 9; ++mode) {
        memcpy(ops, xr_xir_artifact_module(artifact)->functions[0].instructions, sizeof(ops));
        if (mode == 0) ops[10].args[0] = 3;
        if (mode == 1) ops[1].args[0] = 0;
        if (mode == 2) ops[3].args[0] = 0;
        if (mode == 3) ops[3].args[1] = 2;
        if (mode == 4) ops[0].args[0] = 3;
        if (mode == 5) ops[1].op = XR_XIR_COPY;
        if (mode == 6) ops[0].type = XR_XIR_UNIT;
        if (mode == 7) { ops[3] = (XrXirInstruction) {XR_XIR_LOCAL_NEW, XR_XIR_STRING, {1}, {0}, 0}; ops[8].args[0] = 6; }
        if (mode == 8) { ops[8].op = XR_XIR_OWNED_LOCAL_READ; }
        CHECK(xr_xir_verify(&module, NULL, NULL) != XR_XIR_OK);
    }
}
static void rejected_phis(const XrXirArtifact *artifact) {
    XrXirModule module = *xr_xir_artifact_module(artifact);
    const XrXirFunction *original = &module.functions[2];
    XrXirFunction function = *original;
    XrXirInstruction ops[13]; uint32_t inputs[12];
    module.functions = &function; module.function_count = 1;
    function.instructions = ops; function.operands = inputs;
    for (unsigned mode = 0; mode < 14; ++mode) {
        memcpy(ops, original->instructions, sizeof(ops)); memcpy(inputs, original->operands, sizeof(inputs));
        if (mode == 0) inputs[0] = 3;
        if (mode == 1) inputs[2] = 0;
        if (mode == 2) inputs[1] = 2;
        if (mode == 3) inputs[1] = 6;
        if (mode == 4) inputs[3] = 14;
        if (mode == 5) inputs[3] = UINT32_MAX;
        if (mode == 6) ops[3].args[1] = 2;
        if (mode == 7) ops[3].args[1] = 3;
        if (mode == 8) ops[3].args[1] = 0;
        if (mode == 9) ops[3].args[0] = UINT32_MAX;
        if (mode == 10) ops[3].type = XR_XIR_UNIT;
        if (mode == 11) { ops[3].op = XR_XIR_COPY; ops[3].args[0] = 0; ops[3].args[1] = 0; }
        if (mode == 12) { ops[0].op = XR_XIR_LOCAL_NEW; ops[0].type = XR_XIR_STRING; inputs[1] = 3; }
        if (mode == 13) ops[3].targets[0] = 1;
        CHECK(xr_xir_verify(&module, NULL, NULL) != XR_XIR_OK);
    }
}
static void phi_layout_attacks(XrXirArtifact *artifact) {
    XrXirFunctionLayout *layout = (XrXirFunctionLayout *) xr_xir_artifact_layout(artifact, 2);
    uint32_t bytes = layout->frame_bytes, owned = layout->owned_count;
    CHECK(owned == 7 && layout->offsets[7] == layout->offsets[6] + 16);
    --layout->frame_bytes;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_LAYOUT);
    layout->frame_bytes = bytes;
    --layout->owned_count;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_LAYOUT);
    layout->owned_count = owned;
    uint32_t *offsets = (uint32_t *) layout->owned_offsets, saved = offsets[3];
    offsets[3] = layout->offsets[6];
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_LAYOUT);
    offsets[3] = saved;
    XrXirBudget budget = xr_xir_default_budget(); budget.frame_bytes = bytes - 1;
    CHECK(xr_xir_artifact_verify(artifact, &budget, NULL) == XR_XIR_BUDGET);
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_OK);
}
int main(int argc, char **argv) {
    XrXirArtifact *checked = local_fixture(), *decoded = NULL, *lowered = NULL;
    rejected_places(checked); rejected_phis(checked);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(decoded, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded);
    phi_layout_attacks(lowered);
    const XrXirFunction *function = &xr_xir_artifact_module(lowered)->functions[0];
    CHECK(function->instructions[0].op == XR_XIR_OWNED_LOCAL_NEW && function->instructions[1].op == XR_XIR_OWNED_LOCAL_READ);
    CHECK(function->instructions[3].op == XR_XIR_OWNED_LOCAL_WRITE);
    XrXirVmBinding binding; XrXirCallEntry entry;
    CHECK(xr_xir_vm_bind(lowered, 0, &binding, &entry) == XR_XIR_OK);
    local_cases(&entry);
    CHECK(xr_xir_vm_bind(lowered, 1, &binding, &entry) == XR_XIR_OK);
    numeric_cleanup(&entry);
    CHECK(xr_xir_vm_bind(lowered, 2, &binding, &entry) == XR_XIR_OK);
    phi_cases(&entry);
    XrXirModule changed = *xr_xir_artifact_module(lowered);
    XrXirFunction changed_function = *function;
    XrXirInstruction changed_ops[11]; memcpy(changed_ops, function->instructions, sizeof(changed_ops));
    changed_function.instructions = changed_ops; changed.functions = &changed_function; changed.function_count = 1;
    changed_ops[3].op = XR_XIR_SCALAR_LOCAL_WRITE;
    CHECK(xr_xir_verify(&changed, NULL, NULL) == XR_XIR_BAD_TYPE);
    changed_ops[3] = function->instructions[3]; changed_ops[0].op = XR_XIR_SCALAR_LOCAL_NEW;
    CHECK(xr_xir_verify(&changed, NULL, NULL) == XR_XIR_BAD_TYPE);
    if (argc == 2) {
        XrXirCSource source = {0};
        CHECK(xr_xir_emit_c(lowered, "fixture_local", 65536, &source) == XR_XIR_OK);
        FILE *file = fopen(argv[1], "wb"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length && fclose(file) == 0);
        xr_xir_c_source_free(&source);
    } else CHECK(argc == 1);
    xr_xir_artifact_free(lowered);
    puts("Local place roles, snapshots, suspension, cancellation and release passed");
    return 0;
}
