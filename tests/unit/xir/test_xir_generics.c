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
#include "xir/xxir_constraint_proof.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_vm.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_consumer_context_owner.h"
#include "xir_generic_fixture.h"
#include "xir_nominal_generic_fixture.h"
#include "xir_nominal_expression_fixture.h"
#include "xir_nominal_checked_fixture.h"
#include "xir_nominal_field_closure_fixture.h"
static XrXirCompileContext generic_work_context(uint64_t work) {
    return consumer_context_limits((XrCompileResourceLimits){67108864,8388608,work});
}
static void generic_specialization_work_boundary(const XrXirArtifact *source) {
    const XrXirModule *module=xr_xir_compile_artifact_module(source);
    XrXirCompileContext measured=consumer_context_default();
    XrXirArtifact *probe=NULL,*closed=NULL;
    CHECK(xr_xir_compile_recheck(&measured,module,&probe,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(probe,NULL)==XR_XIR_OK);
    uint64_t prefix=consumer_context_stats(&measured).work;
    CHECK(xr_xir_compile_specialize(probe,&closed,NULL)==XR_XIR_OK);
    uint64_t total=consumer_context_stats(&measured).work;
    CHECK(total>prefix+1);xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(probe);
    XrXirCompileContext bounded=generic_work_context(total-1),original=bounded;
    probe=NULL;closed=NULL;
    CHECK(xr_xir_compile_recheck(&bounded,module,&probe,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(probe,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(probe,&closed,NULL)==XR_XIR_BUDGET && !closed);
    CHECK(!memcmp(&bounded,&original,sizeof(bounded)));
    xr_xir_compile_artifact_free(probe);
}
static void nominal_ordered_matching(void) {
    NominalFixture f; nominal_fixture(&f);
    XrXirConstraint constraints[2] = {{0}};
    for (unsigned i = 0; i < 2; ++i) {
        f.declarations[i].parameter_count = 2; f.declarations[i].constraints = constraints;
    }
    XrXirType t = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirType arguments[][2] = {
        {(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1),t},
        {XR_XIR_I64,XR_XIR_STRING}, {XR_XIR_STRING,XR_XIR_I64},
        {(XrXirType)256,t}, {(XrXirType)257,XR_XIR_STRING}};
    XrXirTypeNode nodes[] = {
        {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,2,{0,arguments[0],2,NULL,0}},
        {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,0,{0,arguments[1],2,NULL,0}},
        {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,0,{1,arguments[1],2,NULL,0}},
        {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,0,{0,arguments[2],2,NULL,0}},
        {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,2,{0,arguments[3],2,NULL,0}},
        {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,0,{0,arguments[4],2,NULL,0}}};
    XrXirTypes types = {nodes,6,&f.table, NULL}; XrXirCompileContext budget = consumer_context_default();
    CHECK(xr_xir_compile_types_structure_verify(&budget, &types) == XR_XIR_OK);
    for (unsigned i = 0; i < 4; ++i) {
        budget = consumer_context_default();
        XrXirType expected = (XrXirType)(i == 3 ? 260 : 256);
        XrXirType actual = (XrXirType)(i == 3 ? 261 : 257+i);
        CHECK(xr_xir_compile_type_substitution_matches(&budget, &types, arguments[2], 2, expected, actual) ==
            (i == 0 || i == 3 ? XR_XIR_OK : XR_XIR_BAD_TYPE));
    }
}
static void nominal_field_closure(void) {
    for (unsigned mode = 0; mode < 3; ++mode) {
        XrXirCompileContext measured=consumer_context_default();
        XrXirArtifact *prefix=nominal_field_closure_fixture(&measured,mode);
        uint64_t prefix_work=consumer_context_stats(&measured).work;
        xr_xir_compile_artifact_free(prefix);
        XrXirCompileContext bounded=mode==2?generic_work_context(prefix_work+10000):consumer_context_default();
        XrXirArtifact *checked = nominal_field_closure_fixture(&bounded, mode), *closed = NULL;
        CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_OK);
        XrXirStatus expected = !mode ? XR_XIR_OK : mode == 1 ? XR_XIR_BAD_TYPE : XR_XIR_BUDGET;
        CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == expected);
        xr_xir_compile_artifact_free(checked); checked=NULL;
        if (mode) { CHECK(!closed); continue; }
        const XrXirModule *module = xr_xir_compile_artifact_module(closed);
        CHECK(!module->generics && module->types->count == 4);
        CHECK(module->types->nodes[0].nominal.declaration == 0 &&
            module->types->nodes[0].nominal.arguments[0] == (XrXirType)XR_XIR_TYPE_PARAMETER_BASE &&
            !module->types->nodes[0].nominal.field_count);
        CHECK(module->types->nodes[1].nominal.declaration == 1 &&
            module->types->nodes[1].nominal.arguments[0] == XR_XIR_I64 &&
            module->types->nodes[1].nominal.fields[0] == (XrXirType)259);
        CHECK(xr_xir_type_is_atomic(module->types, (XrXirType)258) &&
            xr_xir_atomic_element(module->types, (XrXirType)258) == XR_XIR_I64);
        CHECK(module->types->nodes[3].nominal.declaration == 0 &&
            module->types->nodes[3].nominal.arguments[0] == XR_XIR_I64 &&
            module->types->nodes[3].nominal.fields[0] == XR_XIR_I64);
        XrXirArtifact *lowered = NULL;
        const XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
        CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(closed); closed=NULL;
        module = xr_xir_compile_artifact_module(lowered);
        CHECK(module->types->count == 3 && module->types->nodes[0].nominal.declaration == 1 &&
            module->types->nodes[0].nominal.arguments[0] == XR_XIR_I64 &&
            module->types->nodes[0].nominal.fields[0] == (XrXirType)258);
        CHECK(xr_xir_type_is_atomic(module->types, (XrXirType)257) &&
            xr_xir_atomic_element(module->types, (XrXirType)257) == XR_XIR_I64);
        CHECK(module->types->nodes[2].nominal.declaration == 0 &&
            module->types->nodes[2].nominal.arguments[0] == XR_XIR_I64 &&
            module->types->nodes[2].nominal.fields[0] == XR_XIR_I64);
        xr_xir_compile_artifact_free(lowered); lowered=NULL;
    }
}
static void nominal_definition_constraints(void) {
    XrXirArtifact *checked = nominal_expression_fixture(suite_context);
    const XrXirModule *module = xr_xir_compile_artifact_module(checked);
    const XrXirNominalDeclaration *declarations = module->types->nominals->declarations;
    ((XrXirConstraint *) declarations[0].constraints)[0].markers = XR_XIR_CONSTRAINT_SENDABLE;
    CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_BAD_TYPE);
    ((XrXirConstraint *) declarations[1].constraints)[0].markers = XR_XIR_CONSTRAINT_SENDABLE;
    CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_BAD_TYPE);
    ((XrXirConstraint *) module->generics[1].constraints)[0].markers = XR_XIR_CONSTRAINT_SENDABLE;
    CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_OK);
    XrXirArtifact *closed = NULL;
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed); closed=NULL; xr_xir_compile_artifact_free(checked); checked=NULL;
}
static void cross_pool_substitution(void) {
    XrXirType parameter = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE, actual = XR_XIR_STRING;
    XrXirTypeNode source_nodes[] = {{XR_XIR_TYPE_ARRAY, parameter, NULL, 0, XR_XIR_UNIT, 0, 1, {0}},
        {XR_XIR_TYPE_ARRAY, XR_XIR_I64, NULL, 0, XR_XIR_UNIT, 0, 0, {0}}};
    XrXirTypeNode target_nodes[] = {{XR_XIR_TYPE_ARRAY, XR_XIR_BOOL, NULL, 0, XR_XIR_UNIT, 0, 0, {0}},
        {XR_XIR_TYPE_ARRAY, XR_XIR_STRING, NULL, 0, XR_XIR_UNIT, 0, 0, {0}},
        {XR_XIR_TYPE_ARRAY, XR_XIR_I64, NULL, 0, XR_XIR_UNIT, 0, 0, {0}}};
    XrXirTypes source = {source_nodes, 2, NULL, NULL}, target = {target_nodes, 3, NULL, NULL};
    XrXirCompileContext budget = consumer_context_default();
    CHECK(xr_xir_compile_type_substitution_matches_between(&budget, &source, &target, &actual, 1, (XrXirType)256, (XrXirType)257) == XR_XIR_OK);
    CHECK(xr_xir_compile_type_substitution_matches_between(&budget, &source, &target, &actual, 1, (XrXirType)256, (XrXirType)256) == XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_substitution_matches_between(&budget, &source, &target, NULL, 0, (XrXirType)257, (XrXirType)258) == XR_XIR_OK);
    CHECK(xr_xir_compile_type_substitution_matches_between(&budget, &source, &target, NULL, 0, (XrXirType)257, (XrXirType)257) == XR_XIR_BAD_TYPE);
    budget=generic_work_context(1);
    CHECK(xr_xir_compile_type_substitution_matches_between(&budget, &source, &target, &actual, 1, (XrXirType)256, (XrXirType)257) == XR_XIR_BUDGET);
}
static void nominal_argument_visibility(void) {
    XrXirArtifact *checked = nominal_checked_fixture(suite_context, 2);
    XrXirModule module = *xr_xir_compile_artifact_module(checked);
    CHECK(module.types->count == 4 && xr_xir_type_is_atomic(module.types, (XrXirType)259));
    XrXirTypeNode nodes[5]; memcpy(nodes, module.types->nodes, 4 * sizeof(*nodes));
    XrXirType argument = (XrXirType)258;
    nodes[4] = (XrXirTypeNode) {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0,
        {0, &argument, 1, NULL, 0}};
    ((XrXirConstraint *) module.types->nominals->declarations[0].constraints)[0].markers = 0;
    XrXirTypes types = {nodes, 5, module.types->nominals, NULL}; module.types = &types;
    CHECK(xr_xir_compile_verify(suite_context, &module, NULL) == XR_XIR_OK);
    XrXirCompileContext budget = consumer_context_default();
    CHECK(xr_xir_compile_type_access(&budget, &module, 4, (XrXirType)260) == XR_XIR_BAD_TYPE);
    budget = consumer_context_default();
    CHECK(xr_xir_compile_type_access(&budget, &module, 8, (XrXirType)260) == XR_XIR_OK);
    ((XrXirNominalDeclaration *) types.nominals->declarations)[1].exported = 0;
    budget = consumer_context_default();
    CHECK(xr_xir_compile_type_access(&budget, &module, 8, (XrXirType)260) == XR_XIR_BAD_TYPE);
    xr_xir_compile_artifact_free(checked); checked=NULL;
}
static void nominal_expression_closure(void) {
    XrXirArtifact *checked = nominal_expression_fixture(suite_context), *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0}; XrXirArtifact *decoded = NULL;
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); checked=NULL;
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded); decoded=NULL;
    const XrXirModule *module = xr_xir_compile_artifact_module(closed);
    CHECK(module->types->count == 8 && module->function_count == 6);
    for (uint32_t f = 3; f < 6; ++f) {
        const XrXirFunction *function = &module->functions[f];
        const XrXirTypeNode *box = xr_xir_type_node(module->types, function->instructions[0].type);
        const XrXirTypeNode *outer = xr_xir_type_node(module->types, function->instructions[1].type);
        XrXirType expected = f == 3 ? XR_XIR_I64 : f == 4 ? XR_XIR_U8 : XR_XIR_STRING;
        CHECK(box && outer && box != outer && box->nominal.declaration == 0 && outer->nominal.declaration == 1);
        CHECK(box->nominal.arguments[0] == expected && box->nominal.fields[0] == expected);
        CHECK(outer->nominal.arguments[0] == expected && outer->nominal.fields[0] == function->instructions[0].type);
    }
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed); closed=NULL;
    CHECK(xr_xir_compile_artifact_module(lowered)->types->count == 6);
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(lowered); lowered=NULL;
}
static void nominal_function_closure(void) {
    for (unsigned mode = 0; mode < 2; ++mode) {
        XrXirArtifact *checked = nominal_generic_fixture(suite_context, mode != 0), *closed = NULL, *again = NULL, *lowered = NULL;
        CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(checked); checked=NULL;
        const XrXirModule *m = xr_xir_compile_artifact_module(closed);
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
        CHECK(xr_xir_compile_specialize(closed, &again, NULL) == XR_XIR_OK);
        XrXirCheckedPacket a = {0}, b = {0};
        CHECK(xr_xir_compile_checked_write(closed, &a, NULL) == XR_XIR_OK);
        CHECK(xr_xir_compile_checked_write(again, &b, NULL) == XR_XIR_OK);
        CHECK(a.length == b.length && !memcmp(a.bytes, b.bytes, a.length));
        xr_xir_compile_checked_packet_free(&a); xr_xir_compile_checked_packet_free(&b);
        xr_xir_compile_artifact_free(again); again=NULL;
        const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
        CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(closed); closed=NULL;
        CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
        m = xr_xir_compile_artifact_module(lowered);
        CHECK(m->types->nominals->identities && m->types->count == (mode ? 4u : 2u));
        CHECK(xr_xir_array_element(m->types, m->functions[2].instructions[0].type) == XR_XIR_I64);
        CHECK(xr_xir_array_element(m->types, m->functions[3].instructions[0].type) == XR_XIR_U8);
        xr_xir_compile_artifact_free(lowered); lowered=NULL;
    }
}
static void rehash_generic(XrXirCheckedPacket *packet) {
    XrSHA256Context sha; xr_sha256_init(&sha);
    xr_sha256_update(&sha, packet->bytes, 32);
    xr_sha256_update(&sha, packet->bytes + 64, packet->length - 64);
    xr_sha256_final(&sha, packet->bytes + 32);
}
static void error_erasure_packet(void) {
    _Static_assert(XR_XIR_ERROR_ERASE == 98 && XR_XIR_ERROR == 14, "error wire identities");
    XrXirType parameter = XR_XIR_ERROR;
    XrXirInstruction ops[] = {{XR_XIR_ERROR_ERASE, XR_XIR_ERROR, {0}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1}, {0}, 0, {0}}};
    XrXirBlock block = {0, 2, 0, 0};
    XrXirFunction function = {"erase", 5, &parameter, 1, XR_XIR_ERROR, &block, 1, ops, 2, NULL, 0};
    XrXirModule built = {XR_XIR_BUILT, &function, 1, NULL, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL, *decoded = NULL;
    CHECK(xr_xir_compile_check(suite_context, &built, &checked, NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); checked=NULL;
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded); decoded=NULL; decoded = NULL;
    const uint8_t instruction[32] = {98, 0, 0, 0, 14};
    size_t offset = 0; unsigned found = 0;
    for (size_t i = 64; i + sizeof(instruction) <= packet.length; ++i)
        if (!memcmp(packet.bytes + i, instruction, sizeof(instruction))) { offset = i; ++found; }
    CHECK(found == 1 && offset >= 88 && packet.bytes[offset - 28] == 14);
    const size_t positions[] = {offset - 28, offset + 4, offset + 8, offset + 16};
    const uint8_t replacements[] = {XR_XIR_I64, XR_XIR_I64, 1, 1};
    for (unsigned i = 0; i < 4; ++i) {
        uint8_t saved = packet.bytes[positions[i]];
        packet.bytes[positions[i]] = replacements[i]; rehash_generic(&packet);
        CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) != XR_XIR_OK && !decoded);
        packet.bytes[positions[i]] = saved;
    }
    rehash_generic(&packet);
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded); decoded=NULL; xr_xir_compile_checked_packet_free(&packet);
}
static void error_marker_definition(void) {
    XrXirConstraint constraint = {.markers = XR_XIR_CONSTRAINT_ERROR};
    XrXirType parameter = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirInstruction instruction = {XR_XIR_THROW,XR_XIR_UNIT,{0},{0},0, {0}};
    XrXirBlock block = {0,1, 0, 0};
    XrXirFunction function = {"e",1,&parameter,1,XR_XIR_I64,&block,1,&instruction,1,NULL,0};
    XrXirGeneric generic = {&constraint,1,NULL,0, NULL};
    XrXirModule built = {XR_XIR_BUILT,&function,1,NULL,&generic,NULL,NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked=NULL,*decoded=NULL;
    CHECK(xr_xir_compile_check(suite_context, &built, &checked, NULL)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); checked=NULL;
    CHECK(packet.length==205 && !packet.bytes[169] && packet.bytes[173]==XR_XIR_CONSTRAINT_ERROR);
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded); decoded=NULL; decoded=NULL;
    const uint8_t forged[]={0,XR_XIR_CONSTRAINT_SENDABLE,4};
    for (unsigned i=0;i<sizeof(forged);++i) {
        packet.bytes[173]=forged[i]; rehash_generic(&packet);
        CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL)==XR_XIR_BAD_TYPE && !decoded);
    }
    xr_xir_compile_checked_packet_free(&packet);
    XrXirCompileContext budget=generic_work_context(1);
    XrXirProofContext proof = {&built,{XR_XIR_CONTEXT_FUNCTION,0,0}};
    CHECK(xr_xir_compile_type_markers_prove(&budget, &proof, parameter, XR_XIR_CONSTRAINT_ERROR)==XR_XIR_BUDGET);
    budget=consumer_context_default();
    CHECK(xr_xir_compile_type_markers_prove(&budget, &proof, parameter, XR_XIR_CONSTRAINT_MASK)==XR_XIR_BAD_TYPE);
    constraint.markers = XR_XIR_CONSTRAINT_MASK;
    CHECK(xr_xir_compile_type_markers_prove(&budget, &proof, parameter, XR_XIR_CONSTRAINT_MASK)==XR_XIR_OK);
}
static void rejected_templates(void) {
    for (unsigned mode = 0; mode < 12; ++mode) {
        XrXirArtifact *checked = generic_fixture(suite_context);
        XrXirFunction *functions = (XrXirFunction *) checked->module.functions;
        XrXirGeneric *generics = (XrXirGeneric *) checked->module.generics;
        XrXirInstruction *caller = (XrXirInstruction *) functions[0].instructions;
        XrXirInstruction *body = (XrXirInstruction *) functions[1].instructions;
        if (mode == 0) ((XrXirConstraint *) generics[1].constraints)[0].markers = 8;
        if (mode == 1) caller[1].type_arguments[0] = 0;
        if (mode == 2) caller[1].type_arguments[1] = 0;
        if (mode == 3) ((XrXirType *) generics[0].arguments)[0] = XR_XIR_UNIT;
        if (mode == 4) ((XrXirType *) generics[0].arguments)[0] = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE;
        if (mode == 5) body[0].type = (XrXirType) (XR_XIR_TYPE_PARAMETER_BASE + 1);
        if (mode == 6) body[0].op = XR_XIR_SCALAR_COPY;
        if (mode == 7) body[0].op = XR_XIR_CONST_INT;
        if (mode == 8) caller[0].type = XR_XIR_BOOL;
        if (mode == 9) generics[0].argument_count = 2;
        if (mode == 10) body[0].type = (XrXirType) XR_XIR_TYPE_PARAMETER_LIMIT;
        if (mode == 11) ((XrXirType *) generics[0].arguments)[0] = (XrXirType) 0x40000003u;
        CHECK(xr_xir_compile_artifact_verify(checked, NULL) != XR_XIR_OK);
        XrXirArtifact *closed = NULL; XrXirCheckedPacket packet = {0};
        CHECK(xr_xir_compile_specialize(checked, &closed, NULL) != XR_XIR_OK && !closed);
        CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) != XR_XIR_OK && !packet.bytes);
        xr_xir_compile_artifact_free(checked); checked=NULL;
    }
}
static void forwarding(void) {
    XrXirArtifact *checked = generic_fixture(suite_context);
    XrXirFunction *functions = (XrXirFunction *) checked->module.functions;
    XrXirGeneric generics[2] = {checked->module.generics[0], checked->module.generics[1]};
    XrXirConstraint constraint = {0};
    const XrXirType t = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE;
    XrXirType parameters[] = {t, t}, types[] = {t, t, t};
    generics[0].parameter_count = 1; generics[0].constraints = &constraint; generics[0].arguments = types;
    XrXirFunction caller = functions[0];
    caller.parameters = parameters; caller.result = t;
    XrXirInstruction ops[4]; memcpy(ops, caller.instructions, sizeof(ops));
    for (unsigned i = 0; i < 3; ++i) ops[i].type = t;
    caller.instructions = ops;
    XrXirFunction views[] = {caller, functions[1]};
    XrXirModule module = {XR_XIR_BUILT, views, 2, NULL, generics, NULL, NULL, XR_XIR_PROGRAM, NULL};
    CHECK(xr_xir_compile_verify(suite_context, &module, NULL) == XR_XIR_BAD_TYPE);
    constraint.markers = XR_XIR_CONSTRAINT_SENDABLE;
    CHECK(xr_xir_compile_verify(suite_context, &module, NULL) == XR_XIR_OK);
    XrXirArtifact *valid = NULL, *forged = NULL;
    CHECK(xr_xir_compile_check(suite_context, &module, &valid, NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(valid, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(valid); valid=NULL;
    CHECK(packet.length > 64 && packet.bytes[packet.length - 64] == XR_XIR_CONSTRAINT_SENDABLE);
    packet.bytes[packet.length - 64] = 0; rehash_generic(&packet);
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &forged, NULL) == XR_XIR_BAD_TYPE && !forged);
    xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(checked); checked=NULL;
}
static void specialized_result(XrXirArtifact *lowered) {
    XrXirVmBinding bindings[3]; XrXirCallEntry entries[3];
    for (uint32_t f = 0; f < 3; ++f) CHECK(xr_xir_compile_vm_bind(lowered, f, &bindings[f], &entries[f]) == XR_XIR_OK);
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirDomainStats baseline = xr_xir_domain_stats(domain);
    XrXirValue arguments[] = {{XR_XIR_I64, 0, 7}, {0}}, result = {0};
    CHECK(xr_xir_string_new(domain, "generic", 7, &arguments[1]) == XR_XIR_VALUE_OK);
    XrXirCallAccounting accounting = {0};
    XrXirCallConfig config; CHECK(xr_xir_call_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); config.entries = entries; config.entry_count = 3; config.instance = NULL; config.byte_limit = 65536; config.poll_limit = 100; config.depth_limit = 8; config.accounting = &accounting; config.output = (XrXirOutputProvider) {0}; config.admission = (XrXirValueAdmission) {0};
    XrXirCall *call = NULL;
    CHECK(xr_xir_call_new(&config, 0, arguments, 2, &call) == XR_XIR_CALL_READY);
    CHECK(xr_xir_call_poll_bounded(call, UINT64_MAX).status == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_call_take_result(call, &result) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
    CHECK(!accounting.live_bytes && accounting.allocations == accounting.frees);
    xr_xir_value_drop(&arguments[1]); xr_xir_compile_artifact_free(lowered); lowered=NULL;
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&result, &bytes, &length) && length == 7 && !memcmp(bytes, "generic", 7));
    xr_xir_value_drop(&result);
    XrXirDomainStats stats = xr_xir_domain_stats(domain);
    CHECK(stats.live_bytes == baseline.live_bytes && stats.allocations == stats.frees + 1);
    xr_xir_domain_drop(domain);
}
static void recursive_closure(void) {
    XrXirArtifact *fixture = generic_fixture(suite_context), *checked = NULL, *closed = NULL;
    XrXirModule built = *xr_xir_compile_artifact_module(fixture); built.stage = XR_XIR_BUILT;
    XrXirFunction functions[2]; memcpy(functions, built.functions, sizeof(functions));
    XrXirGeneric generics[2]; memcpy(generics, built.generics, sizeof(generics));
    const XrXirType t = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE;
    const uint32_t operand = 0;
    XrXirInstruction body[] = {{XR_XIR_CALL, t, {0, 1}, {0}, 1, {0, 1}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1}, {0}, 0, {0}}};
    functions[1].instructions = body; functions[1].operands = &operand; functions[1].operand_count = 1;
    generics[1].arguments = &t; generics[1].argument_count = 1;
    built.functions = functions; built.generics = generics;
    CHECK(xr_xir_compile_check(suite_context, &built, &checked, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(fixture); fixture=NULL;
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); checked=NULL;
    const XrXirModule *module = xr_xir_compile_artifact_module(closed);
    CHECK(module->function_count == 3 && module->functions[1].instructions[0].immediate == 1 &&
        module->functions[2].instructions[0].immediate == 2);
    xr_xir_compile_artifact_free(closed); closed=NULL;
}
static void array_definition_constraints(void) {
    XrXirArtifact *fixture = generic_fixture(suite_context), *checked = NULL, *closed = NULL;
    XrXirModule built = *xr_xir_compile_artifact_module(fixture); built.stage = XR_XIR_BUILT;
    XrXirTypeNode nodes[] = {{XR_XIR_TYPE_ARRAY,XR_XIR_I64,NULL,0,XR_XIR_UNIT,0,0, {0}},
        {XR_XIR_TYPE_ARRAY,XR_XIR_STRING,NULL,0,XR_XIR_UNIT,0,0, {0}},
        {XR_XIR_TYPE_CALLABLE,XR_XIR_UNIT,NULL,0,XR_XIR_I64,0,0, {0}},
        {XR_XIR_TYPE_ARRAY,(XrXirType)258,NULL,0,XR_XIR_UNIT,0,0, {0}}};
    XrXirTypes types = {nodes,4, NULL, NULL}; built.types = &types;
    XrXirFunction functions[2]; memcpy(functions,built.functions,sizeof(functions)); built.functions = functions;
    XrXirGeneric generics[2]; memcpy(generics,built.generics,sizeof(generics)); built.generics = generics;
    XrXirInstruction ops[4]; memcpy(ops,functions[0].instructions,sizeof(ops)); functions[0].instructions = ops;
    XrXirType parameters[] = {(XrXirType)256,(XrXirType)257}, arguments[] = {(XrXirType)256,(XrXirType)257,(XrXirType)257};
    functions[0].parameters = parameters; functions[0].result = (XrXirType)257;
    generics[0].arguments = arguments;
    for (unsigned i = 0; i < 3; ++i) ops[i].type = arguments[i];
    CHECK(xr_xir_compile_check(suite_context, &built, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    const XrXirModule *specialized = xr_xir_compile_artifact_module(closed);
    CHECK(xr_xir_array_element(specialized->types,specialized->functions[1].parameters[0]) == XR_XIR_I64);
    CHECK(xr_xir_array_element(specialized->types,specialized->functions[2].parameters[0]) == XR_XIR_STRING);
    xr_xir_compile_artifact_free(closed); closed=NULL; xr_xir_compile_artifact_free(checked); checked=NULL;
    parameters[1] = arguments[1] = arguments[2] = functions[0].result = ops[1].type = ops[2].type = (XrXirType)259;
    CHECK(xr_xir_compile_verify(suite_context, &built, NULL) == XR_XIR_BAD_TYPE);
    XrXirConstraint constraint = {0};
    generics[0].parameter_count = 1; generics[0].constraints = &constraint;
    nodes[0].element = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE; nodes[0].parameter_span = 1;
    parameters[1] = functions[0].result = (XrXirType)256;
    for (unsigned i = 0; i < 3; ++i) arguments[i] = ops[i].type = (XrXirType)256;
    CHECK(xr_xir_compile_verify(suite_context, &built, NULL) == XR_XIR_BAD_TYPE);
    constraint.markers = XR_XIR_CONSTRAINT_SENDABLE;
    CHECK(xr_xir_compile_verify(suite_context, &built, NULL) == XR_XIR_OK);
    XrXirCompileContext budget = generic_work_context(3);
    CHECK(xr_xir_compile_type_satisfies(&budget, &built, 0, (XrXirType)256, (XrXirConstraint){.markers = XR_XIR_CONSTRAINT_SENDABLE}) == XR_XIR_BUDGET);
    xr_xir_compile_artifact_free(fixture); fixture=NULL;
}
#include "xir_array_generic_cases.h"

static void deep_body_substitution(void) {
    enum { DEPTH = 160 };
    XrXirArtifact *fixture = generic_fixture(suite_context), *checked = NULL, *closed = NULL;
    XrXirModule built = *xr_xir_compile_artifact_module(fixture); built.stage = XR_XIR_BUILT;
    XrXirTypeNode nodes[DEPTH];
    for (uint32_t i = 0; i < DEPTH; ++i)
        nodes[i] = (XrXirTypeNode) {XR_XIR_TYPE_ARRAY,
            (XrXirType) (i ? XR_XIR_CONSTRUCTED_TYPE_BASE + i - 1 : XR_XIR_TYPE_PARAMETER_BASE),
            NULL, 0, XR_XIR_UNIT, 0, 1, {0}};
    XrXirTypes types = {nodes, DEPTH, NULL, NULL}; built.types = &types;
    XrXirFunction functions[2]; memcpy(functions, built.functions, sizeof(functions)); built.functions = functions;
    XrXirInstruction body[] = {
        {XR_XIR_ARRAY_NEW, (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + DEPTH - 1), {0}, {0}, 0, {0}},
        functions[1].instructions[0], functions[1].instructions[1]};
    body[2].args[0] = 2;
    XrXirBlock block = {0, 3, 0, 0};
    functions[1].instructions = body; functions[1].instruction_count = 3; functions[1].blocks = &block;
    CHECK(xr_xir_compile_check(suite_context, &built, &checked, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(fixture); fixture=NULL; memset(nodes, 0xCC, sizeof(nodes));
    generic_specialization_work_boundary(checked);
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); checked=NULL;
    const XrXirModule *module = xr_xir_compile_artifact_module(closed);
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
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed); closed=NULL;
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(lowered); lowered=NULL;
}

int main(void) {
    consumer_context=consumer_context_default();
    consumer_context=consumer_context_default();
    error_erasure_packet();
    consumer_context=consumer_context_default();
    error_marker_definition();
    consumer_context=consumer_context_default();
    nominal_function_closure();
    consumer_context=consumer_context_default();
    nominal_expression_closure();
    consumer_context=consumer_context_default();
    nominal_field_closure();
    consumer_context=consumer_context_default();
    nominal_ordered_matching();
    consumer_context=consumer_context_default();
    nominal_definition_constraints();
    consumer_context=consumer_context_default();
    nominal_argument_visibility();
    consumer_context=consumer_context_default();
    cross_pool_substitution();
    consumer_context=consumer_context_default();
    deep_body_substitution();
    consumer_context=consumer_context_default();
    array_generic_cases();
    consumer_context=consumer_context_default();
    array_definition_constraints();
    consumer_context=consumer_context_default();
    rejected_templates(); forwarding(); recursive_closure();
    XrXirArtifact *checked = generic_fixture(suite_context), *decoded = NULL, *closed = NULL, *lowered = NULL;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(checked, &target, &lowered, NULL) == XR_XIR_BAD_STAGE && !lowered);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); checked=NULL;
    packet.bytes[8] = 1; packet.bytes[12] = 1; rehash_generic(&packet);
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_BAD_STRUCTURE && !decoded);
    packet.bytes[8] = 2; packet.bytes[12] = 2; rehash_generic(&packet);
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_BAD_STRUCTURE && !decoded);
    packet.bytes[12] = 3; rehash_generic(&packet);
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_BAD_STRUCTURE && !decoded);
    packet.bytes[12] = 4; rehash_generic(&packet);
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_BAD_STRUCTURE && !decoded);
    packet.bytes[12] = 5; rehash_generic(&packet);
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_BAD_STRUCTURE && !decoded);
    packet.bytes[12] = 6; rehash_generic(&packet);
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_BAD_STRUCTURE && !decoded);
    packet.bytes[8] = 3; packet.bytes[12] = 7; rehash_generic(&packet);
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_BAD_STRUCTURE && !decoded);
    packet.bytes[12] = 8; rehash_generic(&packet);
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_BAD_STRUCTURE && !decoded);
    packet.bytes[8] = XR_XIR_CHECKED_SCHEMA; packet.bytes[12] = XR_XIR_CHECKED_CONTRACT; rehash_generic(&packet);
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    XrXirCompileContext bounded=consumer_context_default();bounded.limits.functions=2;
    XrXirArtifact *limited=NULL;
    CHECK(xr_xir_compile_checked_read(&bounded,packet.bytes,packet.length,&limited,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(limited,&closed,NULL)==XR_XIR_BUDGET && !closed);
    xr_xir_compile_artifact_free(limited);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded); decoded=NULL;
    const XrXirModule *module = xr_xir_compile_artifact_module(closed);
    CHECK(module->stage == XR_XIR_CHECKED && !module->generics && module->function_count == 3);
    CHECK(module->functions[0].instructions[1].immediate == module->functions[0].instructions[2].immediate);
    CHECK(module->functions[0].instructions[0].immediate != module->functions[0].instructions[1].immediate);
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed); closed=NULL;
    module = xr_xir_compile_artifact_module(lowered);
    CHECK(module->functions[1].instructions[0].op == XR_XIR_SCALAR_COPY);
    CHECK(module->functions[2].instructions[0].op == XR_XIR_OWNED_RETAIN);
    specialized_result(lowered);
    puts("Generic definition constraints, packet ownership and Checked specialization passed");
    consumer_contexts_free();
    return 0;
}
