/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_array_element_place_frames.c - Complete Checked frame refusals
 *
 * KEY CONCEPT:
 *   Every shortened input preserves occupied output and refunds physical storage.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_types.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"
#include "element_place_frame.inc.c"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Actual Checked identity");
_Static_assert(XR_XIR_CONST_INT == 2 && XR_XIR_ARRAY_NEW == 73 && XR_XIR_CELL_NEW == 59 &&
    XR_XIR_CELL_PLACE == 71 && XR_XIR_ARRAY_SET == 75 && XR_XIR_ARRAY_GET == 74 &&
    XR_XIR_RETURN == 33 && XR_XIR_TYPE_ARRAY == 2 && XR_XIR_TYPE_CELL == 3, "Independent frame identities");
static const XrXirInstruction element_place_ops[8] = {
    {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42},
    {.op=XR_XIR_ARRAY_NEW,.type=256,.args={0,1}},
    {.op=XR_XIR_CELL_NEW,.type=257,.args={1}},
    {.op=XR_XIR_CELL_PLACE,.type=256,.args={2}},
    {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},
    {.op=XR_XIR_ARRAY_SET,.args={1,3}},
    {.op=XR_XIR_ARRAY_GET,.type=XR_XIR_I64,.args={3,4}},
    {.op=XR_XIR_RETURN,.args={6}}
};
static void exact_owner(const XrXirArtifact *owner) {
    CHECK(xr_xir_compile_artifact_verify(owner, NULL) == XR_XIR_OK);
    const XrXirModule *module = xr_xir_compile_artifact_module(owner);
    CHECK(module && module->stage == XR_XIR_CHECKED && module->function_count == 2 && module->types && module->types->count == 2);
    CHECK(module->declarations && !module->generics && !module->provenance && !module->defaults);
    const XrXirDeclarations *d = module->declarations;
    CHECK(d->module_count == 1 && !d->root_module && !d->entry_function && !d->slot_count && !d->literal_count);
    CHECK(d->modules[0].name_length == 4 && !memcmp(d->modules[0].name, "root", 4) && !d->modules[0].dependency_count && d->modules[0].initializer == 1);
    CHECK(!d->functions[0].module && d->functions[0].exported && !d->functions[1].module && !d->functions[1].exported);
    CHECK(module->types->nodes[0].kind == XR_XIR_TYPE_ARRAY && module->types->nodes[0].element == XR_XIR_I64);
    CHECK(module->types->nodes[1].kind == XR_XIR_TYPE_CELL && module->types->nodes[1].element == 256);
    const XrXirFunction *fn = &module->functions[0]; const uint32_t operands[4] = {0,3,4,0};
    CHECK(fn->name_length == 13 && !memcmp(fn->name, "element_place", 13) && !fn->parameter_count && fn->result == XR_XIR_I64);
    CHECK(fn->instruction_count == 8 && !memcmp(fn->instructions, element_place_ops, sizeof(element_place_ops)));
    CHECK(fn->operand_count == 4 && !memcmp(fn->operands, operands, sizeof(operands)));
    CHECK(fn->block_count == 1 && !fn->blocks[0].first && fn->blocks[0].count == 8 && !fn->blocks[0].panic && !fn->blocks[0].frontier);
    const XrXirFunction *init = &module->functions[1];
    CHECK(init->name_length == 4 && !memcmp(init->name, "init", 4) && !init->parameter_count && init->result == XR_XIR_UNIT);
    CHECK(init->instruction_count == 1 && init->instructions[0].op == XR_XIR_RETURN && !init->instructions[0].args[0]);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(owner, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == sizeof(element_place_frame) && !memcmp(packet.bytes, element_place_frame, packet.length));
    xr_xir_compile_checked_packet_free(&packet);
}
static XrXirArtifact *built_owner(const XrXirCompileContext *context) {
    char name[] = "element_place", init_name[] = "init", module_name[] = "root"; uint32_t operands[4] = {0,3,4,0};
    XrXirInstruction ops[8]; memcpy(ops, element_place_ops, sizeof(ops));
    XrXirTypeNode nodes[2] = {{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},{.kind=XR_XIR_TYPE_CELL,.element=256}};
    XrXirTypes types = {.nodes=nodes,.count=2}; XrXirBlock block = {.count=8};
    XrXirInstruction init_op = {.op=XR_XIR_RETURN}; XrXirBlock init_block = {.count=1};
    XrXirFunction fn[2] = {{.name=name,.name_length=13,.result=XR_XIR_I64,.blocks=&block,.block_count=1,
        .instructions=ops,.instruction_count=8,.operands=operands,.operand_count=4},
        {.name=init_name,.name_length=4,.blocks=&init_block,.block_count=1,.instructions=&init_op,.instruction_count=1}};
    XrXirSourceModule source_module = {.name=module_name,.name_length=4,.initializer=1};
    XrXirFunctionIdentity identities[2] = {{.exported=1},{0}};
    XrXirDeclarations declarations = {.modules=&source_module,.module_count=1,.functions=identities};
    XrXirModule built = {.stage=XR_XIR_BUILT,.functions=fn,.function_count=2,.types=&types,.declarations=&declarations};
    XrXirArtifact *owner = NULL; CHECK(xr_xir_compile_check(context, &built, &owner, NULL) == XR_XIR_OK && owner);
    CHECK(!memcmp(ops, element_place_ops, sizeof(ops)) && !memcmp(name, "element_place", sizeof(name)));
    CHECK(operands[0] == 0 && operands[1] == 3 && operands[2] == 4 && !operands[3]);
    memset(name, 0xa5, sizeof(name)); memset(operands, 0xa5, sizeof(operands)); memset(ops, 0xa5, sizeof(ops));
    memset(nodes, 0xa5, sizeof(nodes)); memset(&types, 0xa5, sizeof(types)); memset(&block, 0xa5, sizeof(block));
    memset(fn, 0xa5, sizeof(fn)); memset(&built, 0xa5, sizeof(built)); memset(&declarations, 0xa5, sizeof(declarations));
    memset(&source_module, 0xa5, sizeof(source_module)); memset(identities, 0xa5, sizeof(identities));
    memset(module_name, 0xa5, sizeof(module_name)); memset(init_name, 0xa5, sizeof(init_name));
    memset(&init_op, 0xa5, sizeof(init_op)); memset(&init_block, 0xa5, sizeof(init_block)); return owner;
}
static void reframe(uint8_t *bytes, size_t length) {
    CHECK(length >= 64); uint64_t body = (uint64_t)(length - 64);
    for (unsigned i = 0; i < 8; ++i) bytes[24 + i] = (uint8_t)(body >> (8 * i));
    XrSHA256Context sha; xr_sha256_init(&sha); xr_sha256_update(&sha, bytes, 32);
    xr_sha256_update(&sha, bytes + 64, length - 64); xr_sha256_final(&sha, bytes + 32);
}
static void refuse(const XrXirCompileContext *context, XrXirArtifact *owner,
    const uint8_t *bytes, size_t length, const char *phase, size_t index) {
    for (unsigned occupied = 0; occupied < 2; ++occupied) {
        uint8_t input[sizeof(element_place_frame) + 1]; CHECK(length <= sizeof(input)); memcpy(input, bytes, length);
        XrXirArtifact *output = occupied ? owner : NULL, *before = output; XrXirDiagnostic diagnostic = {0};
        size_t live = source_program_compile_live, physical = source_program_compile_bytes;
        XrXirStatus status = xr_xir_compile_checked_read(context, input, length, &output, &diagnostic);
        CHECK(status == XR_XIR_BAD_STRUCTURE && diagnostic.status == status && output == before);
        CHECK(!memcmp(input, bytes, length)); memset(input, 0xa5, sizeof(input));
        CHECK(source_program_compile_live == live && source_program_compile_bytes == physical); exact_owner(owner);
        CHECK(source_program_compile_live == live && source_program_compile_bytes == physical);
        printf("element-frame phase=%s index=%zu occupied=%u expected=1 actual=%u input-dead=1 owner-preserved=1 physical-refund=1 result=PASS\n",
            phase, index, occupied, status);
    }
}
int main(void) {
    const XrXirCompileContext *context = source_program_owner(67108864, 128000000);
    XrXirArtifact *owner = built_owner(context); exact_owner(owner);
    uint8_t *input = xr_malloc(sizeof(element_place_frame)); CHECK(input); memcpy(input, element_place_frame, sizeof(element_place_frame));
    XrXirArtifact *decoded = NULL;
    CHECK(xr_xir_compile_checked_read(context, input, sizeof(element_place_frame), &decoded, NULL) == XR_XIR_OK && decoded);
    CHECK(!memcmp(input, element_place_frame, sizeof(element_place_frame))); memset(input, 0xa5, sizeof(element_place_frame)); xr_free(input);
    exact_owner(decoded); xr_xir_compile_artifact_free(decoded);
    size_t transitions = 0; uint8_t bytes[sizeof(element_place_frame) + 1];
    for (size_t length = 0; length < sizeof(element_place_frame); ++length) {
        refuse(context, owner, element_place_frame, length, "prefix", length); transitions += 2;
    }
    for (size_t length = 64; length < sizeof(element_place_frame); ++length) {
        memcpy(bytes, element_place_frame, length); reframe(bytes, length);
        refuse(context, owner, bytes, length, "reframed", length); transitions += 2;
    }
    for (size_t offset = 32; offset < sizeof(element_place_frame); ++offset) {
        memcpy(bytes, element_place_frame, sizeof(element_place_frame)); bytes[offset] ^= 1;
        refuse(context, owner, bytes, sizeof(element_place_frame), "digest", offset); transitions += 2;
    }
    memcpy(bytes, element_place_frame, sizeof(element_place_frame)); bytes[sizeof(element_place_frame)] = 0;
    reframe(bytes, sizeof(bytes)); refuse(context, owner, bytes, sizeof(bytes), "trailing", sizeof(element_place_frame)); transitions += 2;
    CHECK(transitions == 2 * (3 * sizeof(element_place_frame) - 95));
    xr_xir_compile_artifact_free(owner); source_program_owners_free();
    CHECK(!source_program_compile_live && !source_program_compile_bytes && !source_program_compile_allocations && !source_program_compile_capacity);
    printf("element-frames bytes=%zu transitions=%zu mismatches=0 detached-Built=1 detached-wire=1 compiler-physical=0/0 table=0 result=PASS\n",
        sizeof(element_place_frame), transitions);
    return 0;
}
