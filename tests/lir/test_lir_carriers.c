/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_carriers.c - Typed call and parallel edge fixtures for real C
 */
#include "lir/xlir_builder.h"
#include "lir/xlir_c.h"
#include "lir/xlir_sccp.h"
#include "lir/xlir_verify.h"
#include "vm/register/xrbc_lir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_LIR_OK)

static XrLirFunction function(XrLirModule *m, const XrLirType *parameters,
    uint32_t count, XrLirType result) {
    XrLirFunction f;
    const char *name = count ? "helper" : "root";
    XrLirFunctionSpec spec = {.name = name, .name_length = strlen(name), .parameters = parameters,
        .parameter_count = count, .result = result};
    OK(xr_lir_declare_function(m, &spec, &f)); OK(xr_lir_define_function(m, f)); return f;
}
static XrLirBlock block(XrLirModule *m, XrLirFunction f, const XrLirType *types, uint32_t count) {
    XrLirBlock b; OK(xr_lir_add_block(m, f, types, count, &b)); return b;
}
static XrLirValue constant(XrLirModule *m, XrLirBlock b, XrLirType type, uint64_t bits) {
    XrLirValue value; XrLirInstruction v = {.op = XR_LIR_CONST, .type = type, .bits = bits};
    OK(xr_lir_emit_value(m, b, &v, &value)); return value;
}
static void ret(XrLirModule *m, XrLirBlock b, XrLirValue value) {
    XrLirInstruction v = {.op = XR_LIR_RET, .type = XR_LIR_UNIT, .operands = &value, .operand_count = 1};
    OK(xr_lir_set_terminator(m, b, &v));
}
static void branch(XrLirModule *m, XrLirBlock from, XrLirBlock to, const XrLirValue *args) {
    XrLirInstruction v = {.op = XR_LIR_BR, .type = XR_LIR_UNIT,
        .edges = {{to, args, 3}}, .edge_count = 1};
    OK(xr_lir_set_terminator(m, from, &v));
}
static void build_helper(XrLirModule *m, XrLirFunction f, XrLirType type) {
    XrLirType types[] = {type, type, XR_LIR_I64};
    XrLirBlock entry = block(m, f, NULL, 0), loop = block(m, f, types, 3);
    XrLirBlock body = block(m, f, NULL, 0), done = block(m, f, NULL, 0);
    OK(xr_lir_set_entry(m, f, entry)); XrLirValue n, p[3];
    OK(xr_lir_function_parameter(m, f, 0, &n));
    for (uint32_t i = 0; i < 3; ++i) OK(xr_lir_block_parameter(m, loop, i, &p[i]));
    XrLirValue sentinel = constant(m, entry, type, type == XR_LIR_I8 ? 0 : 1);
    XrLirValue zero = constant(m, entry, XR_LIR_I64, 0), one = constant(m, entry, XR_LIR_I64, 1);
    XrLirValue initial[] = {n, sentinel, zero}; branch(m, entry, loop, initial);
    XrLirValue compare[] = {p[2], one}, condition;
    XrLirInstruction v = {.op = XR_LIR_ICMP, .type = XR_LIR_I1, .predicate = XR_LIR_LT,
        .operands = compare, .operand_count = 2};
    OK(xr_lir_emit_value(m, loop, &v, &condition));
    v = (XrLirInstruction){.op = XR_LIR_CONDBR, .type = XR_LIR_UNIT, .operands = &condition,
        .operand_count = 1, .edges = {{body, NULL, 0}, {done, NULL, 0}}, .edge_count = 2};
    OK(xr_lir_set_terminator(m, loop, &v));
    XrLirValue increment[] = {p[2], one}, next;
    v = (XrLirInstruction){.op = XR_LIR_ADD, .type = XR_LIR_I64, .operands = increment, .operand_count = 2};
    OK(xr_lir_emit_value(m, body, &v, &next));
    XrLirValue swapped[] = {p[1], p[0], next}; branch(m, body, loop, swapped);
    ret(m, done, p[1]);
}
static XrLirModule *make_carrier(XrLirType type) {
    XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m));
    XrLirType result = type == XR_LIR_U64 ? type : XR_LIR_I64;
    XrLirFunction root = function(m, NULL, 0, result), helper = {0};
    if (type != XR_LIR_U64) helper = function(m, &type, 1, type);
    XrLirBlock entry = block(m, root, NULL, 0); OK(xr_lir_set_entry(m, root, entry));
    if (type == XR_LIR_U64) ret(m, entry, constant(m, entry, type, UINT64_MAX));
    else {
        build_helper(m, helper, type);
        XrLirValue argument = constant(m, entry, type, type == XR_LIR_I8 ? 128 : 65535), called, widened;
        XrLirInstruction v = {.op = XR_LIR_CALL, .type = type, .callee = helper,
            .operands = &argument, .operand_count = 1};
        OK(xr_lir_emit_value(m, entry, &v, &called));
        v = (XrLirInstruction){.op = XR_LIR_INTCAST, .type = XR_LIR_I64, .operands = &called, .operand_count = 1};
        OK(xr_lir_emit_value(m, entry, &v, &widened)); ret(m, entry, widened);
    }
    OK(xr_lir_module_finish(m, NULL)); OK(xr_lir_verify(m, NULL)); return m;
}
static XrLirModule *sccp_and_retire(XrLirModule *m) {
    XrLirModule *out = NULL; OK(xr_lir_sccp(m, &out)); CHECK(out && out != m);
    xr_lir_module_destroy(m); OK(xr_lir_verify(out, NULL)); return out;
}
/* Record the actual receiver's permutation, not construction-time handles.
 * Returning the second parameter after one swap detects omitted and sequential copies. */
static void observe(XrLirModule *m, XrLirType type, bool report) {
    XrLirFunction root; XrLirFunctionView fv;
    OK(xr_lir_function_at(m, 0, &root)); OK(xr_lir_function_view(m, root, &fv));
    CHECK(fv.parameter_count == 0 && fv.result == (type == XR_LIR_U64 ? type : XR_LIR_I64));
    if (type == XR_LIR_U64) {
        CHECK(xr_lir_function_count(m) == 1);
        if (report) puts("{\"carrier\":\"u64\",\"root\":\"u64(void)\"}");
        return;
    }
    XrLirFunction helper; OK(xr_lir_function_at(m, 1, &helper));
    OK(xr_lir_function_view(m, helper, &fv));
    CHECK(helper.id == 2 && fv.parameter_count == 1 && fv.parameter_types[0] == type && fv.result == type);
    XrLirBlock loop = {0}; XrLirBlockView phi = {0};
    for (uint32_t i = 0; i < fv.block_count; ++i) {
        XrLirBlock b; XrLirBlockView bv; OK(xr_lir_block_at(m, helper, i, &b));
        OK(xr_lir_block_view(m, b, &bv));
        if (bv.parameter_count == 3) { CHECK(!loop.id); loop = b; phi = bv; }
    }
    CHECK(loop.id); unsigned backedges = 0, returns = 0, calls = 0;
    for (uint32_t i = 0; i < 3; ++i) {
        XrLirType actual; XrLirValue value = {m, helper.id, phi.parameters[i]};
        OK(xr_lir_value_type(m, value, &actual)); CHECK(actual == (i == 2 ? XR_LIR_I64 : type));
    }
    XrLirInstructionView comparison; OK(xr_lir_instruction_view(m, loop, 0, &comparison));
    CHECK(comparison.op == XR_LIR_ICMP && comparison.predicate == XR_LIR_LT &&
        comparison.operands[0] == phi.parameters[2]);
    uint32_t one = comparison.operands[1]; unsigned limits = 0, increments = 0, initial = 0;
    XrLirFunctionView root_view; OK(xr_lir_function_view(m, root, &root_view));
    for (uint32_t i = 0; i < root_view.block_count; ++i) {
        XrLirBlock b; XrLirBlockView bv; OK(xr_lir_block_at(m, root, i, &b)); OK(xr_lir_block_view(m, b, &bv));
        for (uint32_t j = 0; j < bv.instruction_count; ++j) {
            XrLirInstructionView v; OK(xr_lir_instruction_view(m, b, j, &v));
            calls += v.op == XR_LIR_CALL && v.callee == helper.id;
        }
    }
    CHECK(calls == 1);
    for (uint32_t i = 0; i < fv.block_count; ++i) {
        XrLirBlock b; XrLirBlockView bv; OK(xr_lir_block_at(m, helper, i, &b)); OK(xr_lir_block_view(m, b, &bv));
        for (uint32_t j = 0; j < bv.instruction_count; ++j) {
            XrLirInstructionView instruction; OK(xr_lir_instruction_view(m, b, j, &instruction));
            if (instruction.result == one) {
                CHECK(instruction.op == XR_LIR_CONST && instruction.type == XR_LIR_I64 && instruction.bits == 1);
                ++limits;
            }
            if (instruction.op == XR_LIR_ADD) {
                CHECK(instruction.type == XR_LIR_I64 && instruction.operands[0] == phi.parameters[2] &&
                    instruction.operands[1] == one); ++increments;
            }
        }
        XrLirInstructionView v; OK(xr_lir_instruction_view(m, b, bv.instruction_count - 1, &v));
        if (v.op == XR_LIR_RET) { CHECK(v.operands[0] == phi.parameters[1]); ++returns; }
        if (v.op != XR_LIR_BR || v.edges[0].target != loop.id) continue;
        if (b.id == fv.entry) { CHECK(v.edges[0].arguments[0] == fv.parameters[0]); ++initial; continue; }
        CHECK(v.edges[0].argument_count == 3 && v.edges[0].arguments[0] == phi.parameters[1] &&
            v.edges[0].arguments[1] == phi.parameters[0]); ++backedges;
        if (report) printf("{\"carrier\":\"%s\",\"loop\":%u,\"parameters\":[%u,%u,%u],"
            "\"backedgeArguments\":[%u,%u,%u],\"returned\":%u,\"iterations\":1}\n",
            xr_lir_type_name(type), loop.id, phi.parameters[0], phi.parameters[1], phi.parameters[2],
            v.edges[0].arguments[0], v.edges[0].arguments[1], v.edges[0].arguments[2], phi.parameters[1]);
    }
    CHECK(backedges == 1 && returns == 1 && limits == 1 && increments == 1 && initial == 1);
}
static XrCompileResources *resources_new(void) {
    XrCompileResourceLimits limits = {UINT64_MAX, UINT64_MAX, UINT64_MAX}; XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK); return resources;
}
static void emit_c(XrLirModule *m, const char *path) {
    XrLirFunction root; OK(xr_lir_function_at(m, 0, &root)); XrCompileResources *resources = resources_new();
    XrLirCRequest request = {.module = m, .entry = root, .resources = resources, .translation_unit = path};
    XrLirCOutput out = {0}; CHECK(xr_lir_emit_c(&request, &out, NULL) == XR_LIR_C_OK);
    xr_lir_module_destroy(m); xr_compile_resources_release(resources);
    FILE *file = fopen(path, "wb"); CHECK(file && out.bytes && out.length);
    CHECK(fwrite(out.bytes, 1, out.length, file) == out.length && fclose(file) == 0);
    xr_lir_c_output_free(&out);
}
static XrLirModule *transport(XrLirType type) {
    XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m));
    XrLirFunction f = function(m, NULL, 0, type); XrLirBlock b = block(m, f, NULL, 0);
    OK(xr_lir_set_entry(m, f, b)); ret(m, b, constant(m, b, type, 0));
    OK(xr_lir_module_finish(m, NULL)); OK(xr_lir_verify(m, NULL)); return m;
}
static void test_frontier(void) {
    const XrLirType native_rejected[] = {XR_LIR_F32, XR_LIR_F64, XR_LIR_PTR};
    for (size_t i = 0; i < sizeof(native_rejected) / sizeof(*native_rejected); ++i) {
        XrLirModule *m = transport(native_rejected[i]); XrLirFunction root; OK(xr_lir_function_at(m, 0, &root));
        XrCompileResources *resources = resources_new(); XrLirCOutput out = {0}; XrLirCDiagnostic diagnostic;
        XrLirCRequest request = {.module = m, .entry = root, .resources = resources, .translation_unit = "frontier.c"};
        CHECK(xr_lir_emit_c(&request, &out, &diagnostic) == XR_LIR_C_UNSUPPORTED);
        CHECK(diagnostic.status == XR_LIR_C_UNSUPPORTED && !out.bytes && !out.length);
        xr_lir_module_destroy(m); xr_compile_resources_release(resources);
    }
    const XrLirType vm_types[] = {XR_LIR_I8, XR_LIR_U16, XR_LIR_U64, XR_LIR_F32, XR_LIR_PTR, XR_LIR_F64};
    for (size_t i = 0; i < sizeof(vm_types) / sizeof(*vm_types); ++i) {
        XrLirModule *m = transport(vm_types[i]); XrLirFunction root; OK(xr_lir_function_at(m, 0, &root));
        XrRbcProgram *program = NULL; XrRbcCompileStatus status = xr_rbc_compile(m, root, &program, NULL);
        if (vm_types[i] == XR_LIR_F64) CHECK(status == XR_RBC_COMPILE_OK && program);
        else CHECK(status == XR_RBC_COMPILE_UNSUPPORTED && !program);
        xr_lir_module_destroy(m); xr_rbc_program_destroy(program);
    }
}
int main(int argc, char **argv) {
    const XrLirType types[] = {XR_LIR_I8, XR_LIR_U16, XR_LIR_U64};
    if (argc == 1) {
        test_frontier();
        for (size_t i = 0; i < 3; ++i) {
            XrLirModule *m = make_carrier(types[i]); observe(m, types[i], false);
            m = sccp_and_retire(m); observe(m, types[i], false); xr_lir_module_destroy(m);
        }
        puts("Typed carriers, parallel edges and explicit backend frontiers passed"); return 0;
    }
    CHECK(argc == 4); XrLirType type = XR_LIR_TYPE_INVALID;
    for (size_t i = 0; i < 3; ++i) if (!strcmp(argv[1], xr_lir_type_name(types[i]))) type = types[i];
    CHECK(type != XR_LIR_TYPE_INVALID && (!strcmp(argv[2], "before") || !strcmp(argv[2], "after")));
    XrLirModule *m = make_carrier(type);
    if (!strcmp(argv[2], "after")) m = sccp_and_retire(m);
    observe(m, type, true); emit_c(m, argv[3]); return 0;
}
