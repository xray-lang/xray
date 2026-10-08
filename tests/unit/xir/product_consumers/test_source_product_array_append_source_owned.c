/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_array_append_source_owned.c - Detached scalar append owners
 *
 * KEY CONCEPT:
 *   Positive admission requirements remain failures when unavailable, while
 *   their real source owners and diagnostics are still released completely.
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Actual Checked identity");

static void append_shape(const XrXirArtifact *owner) {
    const XrXirModule *module = xr_xir_compile_artifact_module(owner);
    CHECK(module && module->stage == XR_XIR_CHECKED);
    unsigned found = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (function->name_length != 10 || memcmp(function->name, "appendNine", 10)) continue;
        ++found; CHECK(function->parameter_count == 1 && function->parameters[0] == XR_XIR_I64 && function->result == XR_XIR_I64);
        unsigned appends = 0, arrays = 0, lengths = 0; uint32_t receiver = UINT32_MAX;
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            const XrXirInstruction *op = &function->instructions[i];
            if (op->op == XR_XIR_ARRAY_NEW) {
                ++arrays; CHECK(xr_xir_array_element(module->types, op->type) == XR_XIR_I64 && !op->args[1]);
            }
            if (op->op == XR_XIR_ARRAY_PUSH) {
                ++appends; CHECK(op->type == XR_XIR_UNIT && op->args[1] == 0);
                CHECK(op->args[0] >= function->parameter_count && op->args[0] - function->parameter_count < i);
                const XrXirInstruction *place = &function->instructions[op->args[0] - function->parameter_count];
                CHECK(place->op == XR_XIR_CELL_PLACE && xr_xir_array_element(module->types, place->type) == XR_XIR_I64);
                if (receiver == UINT32_MAX) receiver = place->args[0];
                CHECK(place->args[0] == receiver);
            }
            if (op->op == XR_XIR_ARRAY_LEN) { ++lengths; CHECK(op->type == XR_XIR_I64); }
        }
        CHECK(appends == 9 && arrays == 1 && lengths == 1);
        CHECK(receiver >= function->parameter_count && receiver - function->parameter_count < function->instruction_count);
        const XrXirInstruction *cell = &function->instructions[receiver - function->parameter_count];
        CHECK(cell->op == XR_XIR_CELL_NEW && xr_xir_array_element(module->types,
            xr_xir_cell_element(module->types, cell->type)) == XR_XIR_I64);
        CHECK(cell->args[0] >= function->parameter_count && cell->args[0] - function->parameter_count < receiver - function->parameter_count);
        CHECK(function->instructions[cell->args[0] - function->parameter_count].op == XR_XIR_ARRAY_NEW);
    }
    CHECK(found == 1);
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    char root[2048], file[2048];
    CHECK(strlen(argv[1]) < sizeof(root) && strlen(argv[2]) < sizeof(file));
    strcpy(root, argv[1]); strcpy(file, argv[2]);
    const XrXirCompileContext *context = source_program_owner(67108864, 128000000);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, root};
    XrXirSourceProductRequest request = {{session, file, &authority, context, argv[3], NULL, XR_XIR_PROGRAM, NULL},
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product = NULL; XrXirSourceProductDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_product_build(&request, &product, &diagnostic);
    XrXirArtifact *checked = NULL, *lowered = NULL; XrXirCheckedPacket golden = {0};
    if (status == XR_XIR_OK) {
        XrXirSourceProductPacketView packet = {0}; CHECK(product);
        CHECK(xr_xir_compile_source_product_verify(product, 16777216, NULL) == XR_XIR_OK);
        CHECK(xr_xir_compile_source_product_packet(product, XR_XIR_SOURCE_PRODUCT_CLOSED, &packet) == XR_XIR_OK);
        CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
        append_shape(checked); CHECK(xr_xir_compile_checked_write(checked, &golden, NULL) == XR_XIR_OK);
    } else CHECK(!product && diagnostic.status == status && diagnostic.source.message[0]);
    XrXirSourceProductDiagnostic saved = diagnostic;
    char owned_path[2048] = {0};
    if (diagnostic.source_path) {
        CHECK(strlen(diagnostic.source_path) < sizeof(owned_path));
        strcpy(owned_path, diagnostic.source_path);
    }
    xr_compile_session_free(session);
    xr_xir_compile_source_product_free(product);
    memset(root, 0xa5, sizeof(root)); memset(file, 0xa5, sizeof(file));
    memset(&request, 0xa5, sizeof(request)); memset(&authority, 0xa5, sizeof(authority));
    CHECK(!memcmp(&saved, &diagnostic, sizeof(saved)));
    if (owned_path[0]) CHECK(!strcmp(owned_path, diagnostic.source_path));
    if (checked) {
        CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_OK); append_shape(checked);
        XrXirCheckedPacket repeated = {0};
        CHECK(xr_xir_compile_checked_write(checked, &repeated, NULL) == XR_XIR_OK);
        CHECK(repeated.length == golden.length && !memcmp(repeated.bytes, golden.bytes, golden.length));
        xr_xir_compile_checked_packet_free(&repeated); xr_xir_compile_checked_packet_free(&golden);
        XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
        CHECK(xr_xir_compile_lower(checked, &target, &lowered, NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(checked);
        CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(lowered);
        puts("array-append-source-owned appends=9 exact-owned-packet=PASS detached-Checked=PASS detached-Lowered=PASS session-dead=1 product-dead=1 input-path-dead=1");
    } else printf("array-append-source-owned required=0 actual=%u stage=%u line=%d column=%d session-dead=1 product-dead=1 input-path-dead=1 path=%s message=%s\n",
        status, diagnostic.stage, diagnostic.source.line, diagnostic.source.column, diagnostic.source_path ? diagnostic.source_path : "(none)", diagnostic.source.message);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    source_program_owners_free();
    CHECK(!source_program_compile_allocations && !source_program_compile_capacity);
    printf("array-append-source-owned required=0 actual=%u compiler-physical=0/0 table=0 result=%s\n", status,
        status == XR_XIR_OK ? "PASS" : "FAIL");
    return status == XR_XIR_OK ? 0 : 1;
}
