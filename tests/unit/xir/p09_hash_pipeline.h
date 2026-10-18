/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_hash_pipeline.h - Independent descriptors and a real owned call boundary
 */
#ifndef P09_HASH_PIPELINE_H
#define P09_HASH_PIPELINE_H
#include "xir_construction_fixture.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_map.h"
#include "xir/xxir_program_internal.h"
#include "xir/xxir_array.h"
#include "xir_hash86_golden.h"
typedef struct HashPipelineFixture {
    XrXirTypeNode nodes[3]; XrXirTypes types;
    XrXirInstruction init, entry[2], echo, call[2], set_call[2];
    XrXirBlock blocks[6]; XrXirFunction functions[6];
    XrXirType parameter, set_parameter; uint32_t operand;
    XrXirSourceModule source; XrXirFunctionIdentity identities[6];
    XrXirDeclarations declarations; XrXirModule module;
} HashPipelineFixture;
static void hash_pipeline_fixture(HashPipelineFixture *f) {
    memset(f, 0, sizeof(*f));
    f->nodes[0] = (XrXirTypeNode){.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_I64};
    f->nodes[1] = (XrXirTypeNode){.kind = XR_XIR_TYPE_MAP, .element = XR_XIR_STRING, .result = (XrXirType)256};
    f->nodes[2] = (XrXirTypeNode){.kind = XR_XIR_TYPE_SET, .element = XR_XIR_F64};
    f->types = (XrXirTypes){f->nodes, 3, NULL, NULL};
    f->init = (XrXirInstruction){XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}};
    f->entry[0] = (XrXirInstruction){XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 42, {0}};
    f->entry[1] = (XrXirInstruction){XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}};
    f->echo = (XrXirInstruction){XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}};
    f->call[0] = (XrXirInstruction){XR_XIR_CALL, (XrXirType)257, {0, 1}, {0}, 2, {0}};
    f->call[1] = (XrXirInstruction){XR_XIR_RETURN, XR_XIR_UNIT, {1}, {0}, 0, {0}};
    f->parameter = (XrXirType)257;
    f->set_parameter = (XrXirType)258;
    f->set_call[0] = (XrXirInstruction){XR_XIR_CALL, (XrXirType)258, {0, 1}, {0}, 4, {0}};
    f->set_call[1] = f->call[1];
    for (unsigned i = 0; i < 6; ++i) f->blocks[i] = (XrXirBlock){0, i == 1 || i == 3 || i == 5 ? 2 : 1, 0, 0};
    f->functions[0] = (XrXirFunction){"init", 4, NULL, 0, XR_XIR_UNIT, &f->blocks[0], 1, &f->init, 1, NULL, 0};
    f->functions[1] = (XrXirFunction){"entry", 5, NULL, 0, XR_XIR_I64, &f->blocks[1], 1, f->entry, 2, NULL, 0};
    f->functions[2] = (XrXirFunction){"echo", 4, &f->parameter, 1, f->parameter, &f->blocks[2], 1, &f->echo, 1, NULL, 0};
    f->functions[3] = (XrXirFunction){"relay", 5, &f->parameter, 1, f->parameter, &f->blocks[3], 1, f->call, 2, &f->operand, 1};
    f->functions[4] = (XrXirFunction){"setecho", 7, &f->set_parameter, 1, f->set_parameter, &f->blocks[4], 1, &f->echo, 1, NULL, 0};
    f->functions[5] = (XrXirFunction){"setrelay", 8, &f->set_parameter, 1, f->set_parameter, &f->blocks[5], 1, f->set_call, 2, &f->operand, 1};
    f->source = (XrXirSourceModule){"hash", 4, NULL, 0, 0};
    f->identities[2].exported = 1; f->identities[3].exported = 1;
    f->identities[4].exported = 1; f->identities[5].exported = 1;
    f->declarations = (XrXirDeclarations){.modules = &f->source, .module_count = 1,
        .functions = f->identities, .entry_function = 1};
    f->module = (XrXirModule){XR_XIR_BUILT, f->functions, 6, &f->declarations, NULL, &f->types,
        NULL, XR_XIR_PROGRAM, NULL};
}
static void hash_pipeline_known_bytes(const XrXirCompileContext *context) {
    HashPipelineFixture f; hash_pipeline_fixture(&f);
    XrXirFunction function = f.functions[2];
    XrXirModule library = {XR_XIR_BUILT, &function, 1, NULL, NULL, &f.types, NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL, *decoded = NULL; XrXirCheckedPacket packet = {0};
    CHECK(xir_fixture_check(context, &library, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    if (packet.length != sizeof(hash86_golden) || memcmp(packet.bytes, hash86_golden, packet.length)) {
        size_t at = 0;
        while (at < packet.length && at < sizeof(hash86_golden) && packet.bytes[at] == hash86_golden[at]) ++at;
        fprintf(stderr, "hash independent packet actual=%zu expected=%zu first=%zu\n", packet.length, sizeof(hash86_golden), at);
    }
    CHECK(packet.length == sizeof(hash86_golden) && !memcmp(packet.bytes, hash86_golden, packet.length));
    CHECK(xr_xir_compile_checked_read(context, hash86_golden, sizeof(hash86_golden), &decoded, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded); xr_xir_compile_checked_packet_free(&packet); xr_xir_compile_artifact_free(checked);
}
static void hash_pipeline_definition_proof(const XrXirCompileContext *context) {
    XrXirType key = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirType value = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE + 1), type = (XrXirType)256;
    XrXirTypeNode node = {.kind = XR_XIR_TYPE_MAP, .element = key, .result = value, .parameter_span = 2};
    XrXirTypes types = {&node, 1, NULL, NULL};
    XrXirConstraint constraints[] = {{.markers = XR_XIR_CONSTRAINT_HASHABLE}, {0}};
    XrXirGeneric generic = {.constraints = constraints, .parameter_count = 2};
    XrXirInstruction ret = {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}};
    XrXirBlock block = {0, 1, 0, 0};
    XrXirFunction echo = {"generic", 7, &type, 1, type, &block, 1, &ret, 1, NULL, 0};
    XrXirModule module = {XR_XIR_BUILT, &echo, 1, NULL, &generic, &types, NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL;
    CHECK(xir_fixture_check(context, &module, &checked, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); checked = NULL;
    constraints[0].markers = 0;
    CHECK(xir_fixture_check(context, &module, &checked, NULL) == XR_XIR_BAD_TYPE && !checked);
    constraints[0].markers = XR_XIR_CONSTRAINT_EQUAL;
    CHECK(xir_fixture_check(context, &module, &checked, NULL) == XR_XIR_BAD_TYPE && !checked);
    constraints[0].markers = XR_XIR_CONSTRAINT_HASHABLE;
    node.parameter_span = 1;
    CHECK(xir_fixture_check(context, &module, &checked, NULL) == XR_XIR_BAD_TYPE && !checked);
}
static XrXirArtifact *hash_pipeline_lower(const XrXirCompileContext *context) {
    hash_pipeline_definition_proof(context);
    hash_pipeline_known_bytes(context);
    HashPipelineFixture f; hash_pipeline_fixture(&f);
    XrXirArtifact *checked = NULL, *read = NULL, *specialized = NULL, *lowered = NULL;
    XrXirDiagnostic diagnostic = {0}; XrXirCheckedPacket packet = {0};
    XrXirStatus status = xir_fixture_check(context, &f.module, &checked, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "hash Checked status=%u function=%u instruction=%u\n",
        status, diagnostic.function, diagnostic.instruction);
    CHECK(status == XR_XIR_OK);
    memset(&f, 0xcc, sizeof(f));
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &read, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xcc, packet.length); xr_xir_compile_checked_packet_free(&packet);
    const XrXirTypes *types = xr_xir_compile_artifact_module(read)->types;
    CHECK(types->count == 3 && types->nodes[1].kind == XR_XIR_TYPE_MAP &&
        types->nodes[1].element == XR_XIR_STRING && types->nodes[1].result == (XrXirType)256 &&
        types->nodes[2].kind == XR_XIR_TYPE_SET && types->nodes[2].element == XR_XIR_F64);
    CHECK(xr_xir_compile_specialize(read, &specialized, NULL) == XR_XIR_OK);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(specialized, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(specialized); xr_xir_compile_artifact_free(read);
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
    return lowered;
}
#endif // P09_HASH_PIPELINE_H
