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
int main(int argc, char **argv) {
    XrXirArtifact *checked = local_fixture(), *decoded = NULL, *lowered = NULL;
    rejected_places(checked);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(decoded, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded);
    const XrXirFunction *function = &xr_xir_artifact_module(lowered)->functions[0];
    CHECK(function->instructions[0].op == XR_XIR_OWNED_LOCAL_NEW && function->instructions[1].op == XR_XIR_OWNED_LOCAL_READ);
    CHECK(function->instructions[3].op == XR_XIR_OWNED_LOCAL_WRITE);
    XrXirVmBinding binding; XrXirCallEntry entry;
    CHECK(xr_xir_vm_bind(lowered, 0, &binding, &entry) == XR_XIR_OK);
    local_cases(&entry);
    CHECK(xr_xir_vm_bind(lowered, 1, &binding, &entry) == XR_XIR_OK);
    numeric_cleanup(&entry);
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
