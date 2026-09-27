/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generics.c - Definition proof, specialization and template ownership
 *
 * KEY CONCEPT:
 *   Malformed templates fail before a concrete instance can hide their mistakes.
 */
#include "xir/xxir_generic.h"
#include "xir/xxir_types.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_vm.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_generic_fixture.h"
#include "xir_nominal_generic_fixture.h"
static void nominal_function_closure(void) {
    for (unsigned mode = 0; mode < 2; ++mode) {
        XrXirArtifact *checked = nominal_generic_fixture(mode != 0), *closed = NULL, *again = NULL, *lowered = NULL;
        CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
        xr_xir_artifact_free(checked);
        const XrXirModule *m = xr_xir_artifact_module(closed);
        CHECK(!m->generics && m->function_count == 4 && m->types->nominals->declarations);
        CHECK(m->declarations->modules[0].initializer == 1 && m->declarations->entry_function == 0);
        CHECK(m->declarations->functions[2].nominal_owner == 1 && !m->declarations->functions[3].nominal_owner);
        XrXirType first = m->functions[2].instructions[0].type, second = m->functions[3].instructions[0].type;
        CHECK(first != second && xr_xir_array_element(m->types, first) == XR_XIR_I64);
        CHECK(xr_xir_array_element(m->types, second) == XR_XIR_U8);
        if (mode) {
            CHECK(m->types->count == 5);
            CHECK(m->types->nodes[1].nominal.fields[0] == first);
            CHECK(m->types->nodes[2].nominal.fields[0] == second);
        }
        CHECK(xr_xir_specialize(closed, NULL, &again, NULL) == XR_XIR_OK);
        XrXirCheckedPacket a = {0}, b = {0};
        CHECK(xr_xir_checked_write(closed, NULL, &a, NULL) == XR_XIR_OK);
        CHECK(xr_xir_checked_write(again, NULL, &b, NULL) == XR_XIR_OK);
        CHECK(a.length == b.length && !memcmp(a.bytes, b.bytes, a.length));
        xr_xir_checked_packet_free(&a); xr_xir_checked_packet_free(&b);
        xr_xir_artifact_free(again);
        const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
        CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
        xr_xir_artifact_free(closed);
        CHECK(xr_xir_artifact_verify(lowered, NULL, NULL) == XR_XIR_OK);
        m = xr_xir_artifact_module(lowered);
        CHECK(m->types->nominals->identities && m->types->count == (mode ? 4u : 2u));
        CHECK(xr_xir_array_element(m->types, m->functions[2].instructions[0].type) == XR_XIR_I64);
        CHECK(xr_xir_array_element(m->types, m->functions[3].instructions[0].type) == XR_XIR_U8);
        xr_xir_artifact_free(lowered);
    }
}
static void rehash_generic(XrXirCheckedPacket *packet) {
    XrSHA256Context sha; xr_sha256_init(&sha);
    xr_sha256_update(&sha, packet->bytes, 32);
    xr_sha256_update(&sha, packet->bytes + 64, packet->length - 64);
    xr_sha256_final(&sha, packet->bytes + 32);
}
static void rejected_templates(void) {
    for (unsigned mode = 0; mode < 12; ++mode) {
        XrXirArtifact *checked = generic_fixture();
        XrXirFunction *functions = (XrXirFunction *) checked->module.functions;
        XrXirGeneric *generics = (XrXirGeneric *) checked->module.generics;
        XrXirInstruction *caller = (XrXirInstruction *) functions[0].instructions;
        XrXirInstruction *body = (XrXirInstruction *) functions[1].instructions;
        if (mode == 0) ((uint32_t *) generics[1].constraints)[0] = 2;
        if (mode == 1) caller[1].targets[0] = 0;
        if (mode == 2) caller[1].targets[1] = 0;
        if (mode == 3) ((XrXirType *) generics[0].arguments)[0] = XR_XIR_UNIT;
        if (mode == 4) ((XrXirType *) generics[0].arguments)[0] = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE;
        if (mode == 5) body[0].type = (XrXirType) (XR_XIR_TYPE_PARAMETER_BASE + 1);
        if (mode == 6) body[0].op = XR_XIR_SCALAR_COPY;
        if (mode == 7) body[0].op = XR_XIR_CONST_INT;
        if (mode == 8) caller[0].type = XR_XIR_BOOL;
        if (mode == 9) generics[0].argument_count = 2;
        if (mode == 10) body[0].type = (XrXirType) XR_XIR_TYPE_PARAMETER_LIMIT;
        if (mode == 11) ((XrXirType *) generics[0].arguments)[0] = (XrXirType) 0x40000003u;
        CHECK(xr_xir_artifact_verify(checked, NULL, NULL) != XR_XIR_OK);
        XrXirArtifact *closed = NULL; XrXirCheckedPacket packet = {0};
        CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) != XR_XIR_OK && !closed);
        CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) != XR_XIR_OK && !packet.bytes);
        xr_xir_artifact_free(checked);
    }
}
static void forwarding(void) {
    XrXirArtifact *checked = generic_fixture();
    XrXirFunction *functions = (XrXirFunction *) checked->module.functions;
    XrXirGeneric generics[2] = {checked->module.generics[0], checked->module.generics[1]};
    uint32_t constraint = 0;
    const XrXirType t = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE;
    XrXirType parameters[] = {t, t}, types[] = {t, t, t};
    generics[0].parameter_count = 1; generics[0].constraints = &constraint; generics[0].arguments = types;
    XrXirFunction caller = functions[0];
    caller.parameters = parameters; caller.result = t;
    XrXirInstruction ops[4]; memcpy(ops, caller.instructions, sizeof(ops));
    for (unsigned i = 0; i < 3; ++i) ops[i].type = t;
    caller.instructions = ops;
    XrXirFunction views[] = {caller, functions[1]};
    XrXirModule module = {XR_XIR_BUILT, views, 2, NULL, generics, NULL};
    CHECK(xr_xir_verify(&module, NULL, NULL) == XR_XIR_BAD_TYPE);
    constraint = XR_XIR_CONSTRAINT_SENDABLE;
    CHECK(xr_xir_verify(&module, NULL, NULL) == XR_XIR_OK);
    XrXirArtifact *valid = NULL, *forged = NULL;
    CHECK(xr_xir_check(&module, NULL, &valid, NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(valid, NULL, &packet, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(valid);
    CHECK(packet.length > 44 && packet.bytes[packet.length - 40] == XR_XIR_CONSTRAINT_SENDABLE);
    packet.bytes[packet.length - 40] = 0; rehash_generic(&packet);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &forged, NULL) == XR_XIR_BAD_TYPE && !forged);
    xr_xir_checked_packet_free(&packet);
    xr_xir_artifact_free(checked);
}
static void specialized_result(XrXirArtifact *lowered) {
    XrXirVmBinding bindings[3]; XrXirCallEntry entries[3];
    for (uint32_t f = 0; f < 3; ++f) CHECK(xr_xir_vm_bind(lowered, f, &bindings[f], &entries[f]) == XR_XIR_OK);
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirDomainStats baseline = xr_xir_domain_stats(domain);
    XrXirValue arguments[] = {{XR_XIR_I64, 0, 7}, {0}}, result = {0};
    CHECK(xr_xir_string_new(domain, "generic", 7, &arguments[1]) == XR_XIR_VALUE_OK);
    XrXirCallAccounting accounting = {0};
    XrXirCallConfig config = {entries, 3, NULL, 65536, 100, 8, &accounting, {NULL, NULL}, {0}};
    XrXirCall *call = NULL;
    CHECK(xr_xir_call_new(&config, 0, arguments, 2, &call) == XR_XIR_CALL_READY);
    CHECK(xr_xir_call_poll(call).status == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_call_take_result(call, &result) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
    CHECK(!accounting.live_bytes && accounting.allocations == accounting.frees);
    xr_xir_value_drop(&arguments[1]); xr_xir_artifact_free(lowered);
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&result, &bytes, &length) && length == 7 && !memcmp(bytes, "generic", 7));
    xr_xir_value_drop(&result);
    XrXirDomainStats stats = xr_xir_domain_stats(domain);
    CHECK(stats.live_bytes == baseline.live_bytes && stats.allocations == stats.frees + 1);
    xr_xir_domain_drop(domain);
}
static void recursive_closure(void) {
    XrXirArtifact *fixture = generic_fixture(), *checked = NULL, *closed = NULL;
    XrXirModule built = *xr_xir_artifact_module(fixture); built.stage = XR_XIR_BUILT;
    XrXirFunction functions[2]; memcpy(functions, built.functions, sizeof(functions));
    XrXirGeneric generics[2]; memcpy(generics, built.generics, sizeof(generics));
    const XrXirType t = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE;
    const uint32_t operand = 0;
    XrXirInstruction body[] = {{XR_XIR_CALL, t, {0, 1}, {0, 1}, 1},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1}, {0}, 0}};
    functions[1].instructions = body; functions[1].operands = &operand; functions[1].operand_count = 1;
    generics[1].arguments = &t; generics[1].argument_count = 1;
    built.functions = functions; built.generics = generics;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(fixture);
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    const XrXirModule *module = xr_xir_artifact_module(closed);
    CHECK(module->function_count == 3 && module->functions[1].instructions[0].immediate == 1 &&
        module->functions[2].instructions[0].immediate == 2);
    xr_xir_artifact_free(closed);
}
static void array_definition_constraints(void) {
    XrXirArtifact *fixture = generic_fixture(), *checked = NULL, *closed = NULL;
    XrXirModule built = *xr_xir_artifact_module(fixture); built.stage = XR_XIR_BUILT;
    XrXirTypeNode nodes[] = {{XR_XIR_TYPE_ARRAY,XR_XIR_I64,NULL,0,XR_XIR_UNIT,0,0, {0}},
        {XR_XIR_TYPE_ARRAY,XR_XIR_STRING,NULL,0,XR_XIR_UNIT,0,0, {0}},
        {XR_XIR_TYPE_CALLABLE,XR_XIR_UNIT,NULL,0,XR_XIR_I64,0,0, {0}},
        {XR_XIR_TYPE_ARRAY,(XrXirType)258,NULL,0,XR_XIR_UNIT,0,0, {0}}};
    XrXirTypes types = {nodes,4, NULL}; built.types = &types;
    XrXirFunction functions[2]; memcpy(functions,built.functions,sizeof(functions)); built.functions = functions;
    XrXirGeneric generics[2]; memcpy(generics,built.generics,sizeof(generics)); built.generics = generics;
    XrXirInstruction ops[4]; memcpy(ops,functions[0].instructions,sizeof(ops)); functions[0].instructions = ops;
    XrXirType parameters[] = {(XrXirType)256,(XrXirType)257}, arguments[] = {(XrXirType)256,(XrXirType)257,(XrXirType)257};
    functions[0].parameters = parameters; functions[0].result = (XrXirType)257;
    generics[0].arguments = arguments;
    for (unsigned i = 0; i < 3; ++i) ops[i].type = arguments[i];
    CHECK(xr_xir_check(&built,NULL,&checked,NULL) == XR_XIR_OK);
    CHECK(xr_xir_specialize(checked,NULL,&closed,NULL) == XR_XIR_OK);
    const XrXirModule *specialized = xr_xir_artifact_module(closed);
    CHECK(xr_xir_array_element(specialized->types,specialized->functions[1].parameters[0]) == XR_XIR_I64);
    CHECK(xr_xir_array_element(specialized->types,specialized->functions[2].parameters[0]) == XR_XIR_STRING);
    xr_xir_artifact_free(closed); xr_xir_artifact_free(checked);
    parameters[1] = arguments[1] = arguments[2] = functions[0].result = ops[1].type = ops[2].type = (XrXirType)259;
    CHECK(xr_xir_verify(&built,NULL,NULL) == XR_XIR_BAD_TYPE);
    uint32_t constraint = 0;
    generics[0].parameter_count = 1; generics[0].constraints = &constraint;
    nodes[0].element = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE; nodes[0].parameter_span = 1;
    parameters[1] = functions[0].result = (XrXirType)256;
    for (unsigned i = 0; i < 3; ++i) arguments[i] = ops[i].type = (XrXirType)256;
    CHECK(xr_xir_verify(&built,NULL,NULL) == XR_XIR_BAD_TYPE);
    constraint = XR_XIR_CONSTRAINT_SENDABLE;
    CHECK(xr_xir_verify(&built,NULL,NULL) == XR_XIR_OK);
    XrXirBudget budget = xr_xir_default_budget(); budget.work = 2;
    CHECK(xr_xir_type_satisfies(&built,0,(XrXirType)256,XR_XIR_CONSTRAINT_SENDABLE,&budget) == XR_XIR_BUDGET);
    xr_xir_artifact_free(fixture);
}
#include "xir_array_generic_cases.h"

static void deep_body_substitution(void) {
    enum { DEPTH = 160 };
    XrXirArtifact *fixture = generic_fixture(), *checked = NULL, *closed = NULL;
    XrXirModule built = *xr_xir_artifact_module(fixture); built.stage = XR_XIR_BUILT;
    XrXirTypeNode nodes[DEPTH];
    for (uint32_t i = 0; i < DEPTH; ++i)
        nodes[i] = (XrXirTypeNode) {XR_XIR_TYPE_ARRAY,
            (XrXirType) (i ? XR_XIR_CONSTRUCTED_TYPE_BASE + i - 1 : XR_XIR_TYPE_PARAMETER_BASE),
            NULL, 0, XR_XIR_UNIT, 0, 1, {0}};
    XrXirTypes types = {nodes, DEPTH, NULL}; built.types = &types;
    XrXirFunction functions[2]; memcpy(functions, built.functions, sizeof(functions)); built.functions = functions;
    XrXirInstruction body[] = {
        {XR_XIR_ARRAY_NEW, (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + DEPTH - 1), {0}, {0}, 0},
        functions[1].instructions[0], functions[1].instructions[1]};
    body[2].args[0] = 2;
    XrXirBlock block = {0, 3};
    functions[1].instructions = body; functions[1].instruction_count = 3; functions[1].blocks = &block;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(fixture); memset(nodes, 0xCC, sizeof(nodes));
    XrXirBudget budget = xr_xir_default_budget(); budget.work = 20000;
    XrXirBudget original = budget;
    CHECK(xr_xir_artifact_verify(checked, &budget, NULL) == XR_XIR_OK);
    CHECK(xr_xir_specialize(checked, &budget, &closed, NULL) == XR_XIR_BUDGET && !closed);
    CHECK(!memcmp(&budget, &original, sizeof(budget)));
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    const XrXirModule *module = xr_xir_artifact_module(closed);
    CHECK(module->function_count == 3 && module->types->count == DEPTH * 2);
    for (uint32_t f = 1; f < 3; ++f) {
        XrXirType type = module->functions[f].instructions[0].type;
        for (uint32_t i = 0; i < DEPTH; ++i) {
            CHECK(xr_xir_type_is_array(module->types, type) && !xr_xir_type_span(module->types, type));
            type = xr_xir_array_element(module->types, type);
        }
        CHECK(type == (f == 1 ? XR_XIR_I64 : XR_XIR_STRING));
    }
    CHECK(module->functions[0].instructions[1].immediate == module->functions[0].instructions[2].immediate);
    XrXirArtifact *lowered = NULL;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed);
    CHECK(xr_xir_artifact_verify(lowered, NULL, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(lowered);
}

int main(void) {
    nominal_function_closure();
    deep_body_substitution();
    array_generic_cases();
    array_definition_constraints();
    rejected_templates(); forwarding(); recursive_closure();
    XrXirArtifact *checked = generic_fixture(), *decoded = NULL, *closed = NULL, *lowered = NULL;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(checked, &target, NULL, &lowered, NULL) == XR_XIR_BAD_STAGE && !lowered);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    packet.bytes[8] = 1; packet.bytes[12] = 1; rehash_generic(&packet);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_BAD_STRUCTURE && !decoded);
    packet.bytes[8] = 2; packet.bytes[12] = 2; rehash_generic(&packet);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_BAD_STRUCTURE && !decoded);
    packet.bytes[12] = 3; rehash_generic(&packet);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_BAD_STRUCTURE && !decoded);
    packet.bytes[12] = 4; rehash_generic(&packet);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_BAD_STRUCTURE && !decoded);
    packet.bytes[12] = 5; rehash_generic(&packet);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_BAD_STRUCTURE && !decoded);
    packet.bytes[12] = 6; rehash_generic(&packet);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_BAD_STRUCTURE && !decoded);
    packet.bytes[8] = 3; packet.bytes[12] = 7; rehash_generic(&packet);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_BAD_STRUCTURE && !decoded);
    packet.bytes[12] = 8; rehash_generic(&packet);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_BAD_STRUCTURE && !decoded);
    packet.bytes[8] = XR_XIR_CHECKED_SCHEMA; packet.bytes[12] = XR_XIR_CHECKED_CONTRACT; rehash_generic(&packet);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);
    XrXirBudget budget = xr_xir_default_budget(); budget.functions = 2;
    CHECK(xr_xir_specialize(decoded, &budget, &closed, NULL) == XR_XIR_BUDGET && !closed);
    CHECK(xr_xir_specialize(decoded, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded);
    const XrXirModule *module = xr_xir_artifact_module(closed);
    CHECK(module->stage == XR_XIR_CHECKED && !module->generics && module->function_count == 3);
    CHECK(module->functions[0].instructions[1].immediate == module->functions[0].instructions[2].immediate);
    CHECK(module->functions[0].instructions[0].immediate != module->functions[0].instructions[1].immediate);
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed);
    module = xr_xir_artifact_module(lowered);
    CHECK(module->functions[1].instructions[0].op == XR_XIR_SCALAR_COPY);
    CHECK(module->functions[2].instructions[0].op == XR_XIR_OWNED_RETAIN);
    specialized_result(lowered);
    puts("Generic definition constraints, packet ownership and Checked specialization passed");
    return 0;
}
