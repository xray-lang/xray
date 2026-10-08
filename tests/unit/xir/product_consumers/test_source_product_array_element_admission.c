/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_array_element_admission.c - Exact scalar Array get/set admission
 *
 * KEY CONCEPT:
 *   Semantic refusals preserve occupied owners after hostile input death.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"
#include "element_admission_cases.inc.c"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Actual Checked identity");
_Static_assert(XR_XIR_I64 == 2 && XR_XIR_U64 == 11 && XR_XIR_ARRAY_NEW == 73 &&
    XR_XIR_CELL_NEW == 59 && XR_XIR_CELL_PLACE == 71 && XR_XIR_ARRAY_SET == 75 &&
    XR_XIR_ARRAY_GET == 74 && XR_XIR_RETURN == 33, "Independent scalar Array identities");
_Static_assert(sizeof(element_admission_cases)/sizeof(element_admission_cases[0]) == 14, "Complete admission cases");
static const XrXirInstruction element_ops[8] = {
    {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42},
    {.op=XR_XIR_ARRAY_NEW,.type=256,.args={0,1}},
    {.op=XR_XIR_CELL_NEW,.type=257,.args={1}},
    {.op=XR_XIR_CELL_PLACE,.type=256,.args={2}},
    {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},
    {.op=XR_XIR_ARRAY_SET,.args={1,3}},
    {.op=XR_XIR_ARRAY_GET,.type=XR_XIR_I64,.args={3,4}},
    {.op=XR_XIR_RETURN,.args={6}}
};
static void change_case(unsigned c, XrXirInstruction *ops, uint32_t *operands) {
    switch (c) {
    case 0: break;
    case 1: ops[6].args[0] = 1; break;
    case 2: ops[6].type = XR_XIR_U64; break;
    case 3: ops[6].args[0] = 0; break;
    case 4: ops[6].args[1] = 1; break;
    case 5: operands[1] = 1; break;
    case 6: operands[1] = 0; break;
    case 7: operands[2] = 1; break;
    case 8: operands[3] = 1; break;
    case 9: ops[5].type = XR_XIR_I64; break;
    case 10: ops[3].type = XR_XIR_I64; break;
    case 11: ops[5].args[1] = 2; break;
    case 12: ops[5].args[0] = 4; break;
    case 13: ops[6].args[1] = UINT32_MAX; break;
    default: CHECK(false); break;
    }
}
static void exact_owner(const XrXirArtifact *owner, unsigned c) {
    CHECK(c < 2 && xr_xir_compile_artifact_verify(owner, NULL) == XR_XIR_OK);
    const XrXirModule *m = xr_xir_compile_artifact_module(owner);
    CHECK(m && m->stage == XR_XIR_CHECKED && m->function_count == 2 && m->types && m->types->count == 2);
    CHECK(m->declarations && !m->generics && !m->provenance && !m->defaults);
    CHECK(m->types->nodes[0].kind == XR_XIR_TYPE_ARRAY && m->types->nodes[0].element == XR_XIR_I64 &&
        m->types->nodes[1].kind == XR_XIR_TYPE_CELL && m->types->nodes[1].element == 256);
    const XrXirFunction *f = &m->functions[0];
    CHECK(f->instruction_count == 8 && f->result == XR_XIR_I64 && !f->parameter_count && f->operand_count == 4);
    CHECK(f->name_length == 13 && !memcmp(f->name, "element_place", 13));
    XrXirInstruction expected[8]; memcpy(expected, element_ops, sizeof(expected));
    uint32_t operands[4] = {0,3,4,0}; change_case(c, expected, operands);
    CHECK(!memcmp(f->instructions, expected, sizeof(expected)) && !memcmp(f->operands, operands, sizeof(operands)));
    const XrXirDeclarations *d = m->declarations;
    CHECK(d->module_count == 1 && !d->root_module && !d->entry_function && !d->slot_count && !d->literal_count);
    CHECK(d->modules[0].name_length == 4 && !memcmp(d->modules[0].name, "root", 4) && d->modules[0].initializer == 1);
    CHECK(!d->functions[0].module && d->functions[0].exported && !d->functions[1].module && !d->functions[1].exported);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(owner, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == element_admission_cases[c].length && !memcmp(packet.bytes, element_admission_cases[c].bytes, packet.length));
    xr_xir_compile_checked_packet_free(&packet);
}
static XrXirStatus build_case(const XrXirCompileContext *context, unsigned c,
    XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    char name[] = "element_place", init_name[] = "init", module_name[] = "root";
    uint32_t operands[4] = {0,3,4,0}; XrXirInstruction ops[8]; memcpy(ops, element_ops, sizeof(ops));
    change_case(c, ops, operands);
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
    XrXirInstruction saved_ops[8]; memcpy(saved_ops, ops, sizeof(ops));
    uint32_t saved_operands[4]; memcpy(saved_operands, operands, sizeof(operands));
    XrXirTypeNode saved_nodes[2]; memcpy(saved_nodes, nodes, sizeof(nodes));
    XrXirFunction saved_fn[2]; memcpy(saved_fn, fn, sizeof(fn));
    XrXirTypes saved_types = types; XrXirBlock saved_block = block, saved_init_block = init_block;
    XrXirInstruction saved_init_op = init_op; XrXirSourceModule saved_module = source_module;
    XrXirFunctionIdentity saved_identities[2]; memcpy(saved_identities, identities, sizeof(identities));
    XrXirDeclarations saved_declarations = declarations; XrXirModule saved_built = built;
    XrXirStatus status = xr_xir_compile_check(context, &built, output, diagnostic);
    CHECK(!memcmp(saved_ops, ops, sizeof(ops)) && !memcmp(saved_operands, operands, sizeof(operands)) &&
        !memcmp(saved_nodes, nodes, sizeof(nodes)) && !memcmp(saved_fn, fn, sizeof(fn)) &&
        !memcmp(&saved_types, &types, sizeof(types)) && !memcmp(&saved_block, &block, sizeof(block)) &&
        !memcmp(&saved_init_block, &init_block, sizeof(init_block)) && !memcmp(&saved_init_op, &init_op, sizeof(init_op)) &&
        !memcmp(&saved_module, &source_module, sizeof(source_module)) && !memcmp(saved_identities, identities, sizeof(identities)) &&
        !memcmp(&saved_declarations, &declarations, sizeof(declarations)) && !memcmp(&saved_built, &built, sizeof(built)));
    CHECK(!memcmp(name, "element_place", sizeof(name)) && !memcmp(init_name, "init", sizeof(init_name)) && !memcmp(module_name, "root", sizeof(module_name)));
    memset(name, 0xa5, sizeof(name)); memset(init_name, 0xa5, sizeof(init_name)); memset(module_name, 0xa5, sizeof(module_name));
    memset(ops, 0xa5, sizeof(ops)); memset(operands, 0xa5, sizeof(operands)); memset(nodes, 0xa5, sizeof(nodes));
    memset(&types, 0xa5, sizeof(types)); memset(&block, 0xa5, sizeof(block)); memset(fn, 0xa5, sizeof(fn));
    memset(&init_op, 0xa5, sizeof(init_op)); memset(&init_block, 0xa5, sizeof(init_block));
    memset(&source_module, 0xa5, sizeof(source_module)); memset(identities, 0xa5, sizeof(identities));
    memset(&declarations, 0xa5, sizeof(declarations)); memset(&built, 0xa5, sizeof(built)); return status;
}
int main(void) {
    const XrXirCompileContext *context = source_program_owner(67108864, 128000000);
    XrXirArtifact *owner = NULL; CHECK(build_case(context, 0, &owner, NULL) == XR_XIR_OK); exact_owner(owner, 0);
    unsigned transitions = 0, mismatches = 0;
    for (unsigned c = 0; c < 14; ++c) for (unsigned wire = 0; wire < 2; ++wire) for (unsigned occupied = 0; occupied < 2; ++occupied) {
        const ElementAdmissionCase *test = &element_admission_cases[c];
        size_t live = source_program_compile_live, bytes = source_program_compile_bytes;
        XrXirArtifact *output = occupied ? owner : NULL, *before = output; XrXirDiagnostic diagnostic = {0}; XrXirStatus status;
        if (wire) {
            uint8_t *input = xr_malloc(test->length); CHECK(input); memcpy(input, test->bytes, test->length);
            status = xr_xir_compile_checked_read(context, input, test->length, &output, &diagnostic);
            CHECK(!memcmp(input, test->bytes, test->length)); memset(input, 0xa5, test->length); xr_free(input);
        } else status = build_case(context, c, &output, &diagnostic);
        if (status == XR_XIR_OK) { CHECK(output && output != owner); exact_owner(output, c); xr_xir_compile_artifact_free(output); }
        else CHECK(output == before && diagnostic.status == status);
        CHECK(source_program_compile_live == live && source_program_compile_bytes == bytes); exact_owner(owner, 0);
        bool match = status == test->expected; mismatches += match ? 0u : 1u; ++transitions;
        printf("element-admission case=%s wire=%u occupied=%u expected=%u actual=%u input-dead=1 owner-preserved=1 physical-refund=1 result=%s\n",
            test->name, wire, occupied, test->expected, status, match ? "PASS" : "FAIL");
    }
    xr_xir_compile_artifact_free(owner); source_program_owners_free();
    CHECK(!source_program_compile_live && !source_program_compile_bytes && !source_program_compile_allocations && !source_program_compile_capacity && transitions == 56);
    printf("element-admission cases=14 transitions=%u mismatches=%u compiler-physical=0/0 table=0 result=%s\n",
        transitions, mismatches, mismatches ? "FAIL" : "PASS"); return mismatches ? 1 : 0;
}
