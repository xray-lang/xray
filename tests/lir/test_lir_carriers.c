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

static XrLirFunction function(XrLirModule *m, const char *name, const XrLirType *parameters,
    uint32_t count, XrLirType result) {
    XrLirFunction f;
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
    XrLirValue sentinel = constant(m, entry, type, type == XR_LIR_F64
        ? UINT64_C(0x3ff2000000000000) : type == XR_LIR_U16 ? 1 : 0);
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
static XrLirFunction comparator(XrLirModule *m, bool equal) {
    XrLirType types[] = {XR_LIR_PTR, XR_LIR_PTR};
    XrLirFunction f = function(m, equal ? "equal" : "different", types, 2, XR_LIR_I1);
    XrLirBlock b = block(m, f, NULL, 0); OK(xr_lir_set_entry(m, f, b));
    XrLirValue args[2], result;
    for (uint32_t i = 0; i < 2; ++i) OK(xr_lir_function_parameter(m, f, i, &args[i]));
    XrLirInstruction v = {.op = XR_LIR_ICMP, .type = XR_LIR_I1,
        .predicate = equal ? XR_LIR_EQ : XR_LIR_NE, .operands = args, .operand_count = 2};
    OK(xr_lir_emit_value(m, b, &v, &result)); ret(m, b, result); return f;
}
static void output_value(XrLirModule *m, XrLirBlock b) {
    XrLirType type = XR_LIR_I64; XrLirFunction print;
    XrLirFunctionSpec spec = {.name = "xrt_print_i64", .name_length = sizeof("xrt_print_i64") - 1,
        .parameters = &type, .parameter_count = 1, .result = XR_LIR_UNIT,
        .kind = XR_LIR_FUNCTION_EXTERNAL, .extern_kind = XR_LIR_EXTERN_PRINT_I64};
    OK(xr_lir_declare_function(m, &spec, &print));
    XrLirValue value = constant(m, b, type, 42);
    XrLirInstruction v = {.op = XR_LIR_CALL, .type = XR_LIR_UNIT, .callee = print,
        .operands = &value, .operand_count = 1, .source = {"pointer.xr", 10, 5, 1, 5, 9}};
    OK(xr_lir_emit_effect(m, b, &v));
}
static XrLirModule *make_carrier(XrLirType type, bool output, uint64_t root_bits) {
    XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m));
    XrLirType result = type == XR_LIR_U64 || type == XR_LIR_PTR || type == XR_LIR_F64 ? type : XR_LIR_I64;
    XrLirFunction root = function(m, "root", NULL, 0, result), helper = {0};
    if (type != XR_LIR_U64) helper = function(m, "helper", &type, 1, type);
    XrLirBlock entry = block(m, root, NULL, 0); OK(xr_lir_set_entry(m, root, entry));
    if (type == XR_LIR_U64) ret(m, entry, constant(m, entry, type, UINT64_MAX));
    else {
        build_helper(m, helper, type);
        uint64_t bits = type == XR_LIR_F64 ? root_bits : type == XR_LIR_I8 ? 128 : type == XR_LIR_U16 ? 65535 : 0;
        XrLirValue argument = constant(m, entry, type, bits), called, widened;
        XrLirInstruction v = {.op = XR_LIR_CALL, .type = type, .callee = helper,
            .operands = &argument, .operand_count = 1};
        OK(xr_lir_emit_value(m, entry, &v, &called));
        if (type == XR_LIR_PTR) {
            XrLirValue args[] = {called, argument}, unused;
            for (unsigned i = 0; i < 2; ++i) {
                XrLirFunction compare = comparator(m, i == 0);
                v = (XrLirInstruction){.op = XR_LIR_CALL, .type = XR_LIR_I1, .callee = compare,
                    .operands = args, .operand_count = 2};
                OK(xr_lir_emit_value(m, entry, &v, &unused));
            }
            if (output) output_value(m, entry);
            ret(m, entry, called);
        } else if (type == XR_LIR_F64) {
            if (output) output_value(m, entry);
            ret(m, entry, called);
        } else {
            CHECK(!output);
            v = (XrLirInstruction){.op = XR_LIR_INTCAST, .type = XR_LIR_I64, .operands = &called, .operand_count = 1};
            OK(xr_lir_emit_value(m, entry, &v, &widened)); ret(m, entry, widened);
        }
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
    XrLirType result = type == XR_LIR_U64 || type == XR_LIR_PTR || type == XR_LIR_F64 ? type : XR_LIR_I64;
    CHECK(fv.parameter_count == 0 && fv.result == result);
    if (type == XR_LIR_U64) {
        CHECK(xr_lir_function_count(m) == 1);
        if (report) puts("{\"carrier\":\"u64\",\"root\":\"u64(void)\"}");
        return;
    }
    XrLirFunction helper; OK(xr_lir_function_at(m, 1, &helper));
    OK(xr_lir_function_view(m, helper, &fv));
    CHECK(helper.id == 2 && fv.parameter_count == 1 && fv.parameter_types[0] == type && fv.result == type);
    if (type == XR_LIR_PTR) {
        for (uint32_t i = 2; i < 4; ++i) {
            XrLirFunction compare; XrLirFunctionView signature;
            OK(xr_lir_function_at(m, i, &compare)); OK(xr_lir_function_view(m, compare, &signature));
            CHECK(compare.id == i + 1 && signature.result == XR_LIR_I1 && signature.parameter_count == 2 &&
                signature.parameter_types[0] == type && signature.parameter_types[1] == type);
        }
    }
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
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false}; XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK); return resources;
}
static XrRbcProgram *program_new(XrLirModule *m, uint32_t ordinal) {
    XrLirFunction entry; XrRbcProgram *program = NULL;
    OK(xr_lir_function_at(m, ordinal, &entry));
    CHECK(xr_rbc_compile(m, entry, &program, NULL) == XR_RBC_COMPILE_OK && program);
    return program;
}
static void check_vm(XrRbcProgram *program, const uint64_t *args, size_t count, uint64_t expected) {
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, args, count, &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution); uint64_t bits = ~expected;
    CHECK(outcome && xr_rbc_outcome_status(outcome) == XR_RBC_OK);
    CHECK(xr_rbc_outcome_bits(outcome, &bits) && bits == expected);
    xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
}
static void dynamic_vm(XrRbcProgram *program) {
    const uint64_t inputs[] = {0, UINT64_C(0x8000000000000000),
        UINT64_C(0x3ff0000000000000), UINT64_C(0xbff0000000000000), 1,
        UINT64_C(0x8000000000000001), UINT64_C(0x000fffffffffffff), UINT64_C(0x0010000000000000),
        UINT64_C(0x7fefffffffffffff), UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
        UINT64_C(0x7ff8000000000000), UINT64_C(0x7ff8000000001234), UINT64_C(0xfff8000000001234),
        UINT64_C(0x7ff0000000000001), UINT64_C(0xfff0000000001234)};
    for (size_t i = 0; i < sizeof(inputs) / sizeof(*inputs); ++i)
        check_vm(program, &inputs[i], 1, inputs[i]);
}
static const struct { const char *name; uint64_t bits; } f64_roots[] = {
    {"f64", UINT64_C(0x8000000000000000)}, {"f64-zero", 0},
    {"f64-finite", UINT64_C(0x3ff0000000000000)}, {"f64-subnormal", 1},
    {"f64-inf", UINT64_C(0x7ff0000000000000)}, {"f64-nan", UINT64_C(0x7ff8000000000000)}
};
static void f64_owned_programs(void) {
    for (size_t i = 0; i < sizeof(f64_roots) / sizeof(*f64_roots); ++i) {
        for (unsigned mode = 0; mode < 2; ++mode) {
            for (unsigned output = 0; output < 2; ++output) {
                XrLirModule *m = make_carrier(XR_LIR_F64, output != 0, f64_roots[i].bits);
                if (mode) m = sccp_and_retire(m);
                observe(m, XR_LIR_F64, false);
                XrRbcProgram *helper = program_new(m, 1), *root = output ? NULL : program_new(m, 0);
                xr_lir_module_destroy(m);
                dynamic_vm(helper);
                if (root) check_vm(root, NULL, 0, f64_roots[i].bits);
                xr_rbc_program_destroy(helper); xr_rbc_program_destroy(root);
            }
        }
    }
}
static void reject_float_operations(void) {
    const struct { XrLirOp op; XrLirType input, result; uint32_t arity; } cases[] = {
        {XR_LIR_FADD, XR_LIR_F64, XR_LIR_F64, 2}, {XR_LIR_FSUB, XR_LIR_F64, XR_LIR_F64, 2},
        {XR_LIR_FMUL, XR_LIR_F64, XR_LIR_F64, 2}, {XR_LIR_FDIV, XR_LIR_F64, XR_LIR_F64, 2},
        {XR_LIR_FNEG, XR_LIR_F32, XR_LIR_F32, 1}, {XR_LIR_FCMP, XR_LIR_F64, XR_LIR_I1, 2},
        {XR_LIR_ITOF, XR_LIR_I64, XR_LIR_F64, 1}, {XR_LIR_FCAST, XR_LIR_F64, XR_LIR_F64, 1}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m));
        XrLirType root_type = cases[i].input == XR_LIR_F32 ? XR_LIR_UNIT : cases[i].result;
        XrLirFunction f = function(m, "refused", NULL, 0, root_type);
        XrLirBlock b = block(m, f, NULL, 0); OK(xr_lir_set_entry(m, f, b));
        XrLirValue a, value;
        XrLirInstruction initial = {.op = XR_LIR_CONST, .type = cases[i].input,
            .source = {"unsupported.xr", 14, 3, 7, 3, 9}};
        OK(xr_lir_emit_value(m, b, &initial, &a)); XrLirValue args[] = {a, a};
        XrLirInstruction v = {.op = cases[i].op, .type = cases[i].result,
            .operands = args, .operand_count = cases[i].arity,
            .predicate = cases[i].op == XR_LIR_FCMP ? XR_LIR_EQ : XR_LIR_PREDICATE_NONE,
            .source = {"unsupported.xr", 14, 3, 7, 3, 9}};
        OK(xr_lir_emit_value(m, b, &v, &value));
        if (root_type == XR_LIR_UNIT) {
            XrLirInstruction term = {.op = XR_LIR_RET, .type = XR_LIR_UNIT};
            OK(xr_lir_set_terminator(m, b, &term));
        } else ret(m, b, value);
        OK(xr_lir_module_finish(m, NULL)); OK(xr_lir_verify(m, NULL));
        XrCompileResources *r = resources_new(); XrLirCOutput out = {0}; XrLirCDiagnostic diagnostic;
        XrLirCRequest request = {.module = m, .entry = f, .resources = r, .translation_unit = "float-frontier.c", .control = NULL};
        CHECK(xr_lir_emit_c(&request, &out, &diagnostic) == XR_LIR_C_UNSUPPORTED);
        CHECK(!out.bytes && !out.length && diagnostic.source.line == 3);
        xr_lir_module_destroy(m); xr_compile_resources_release(r);
    }
}
static void emit_c(XrLirModule *m, const char *path) {
    XrLirFunction root; OK(xr_lir_function_at(m, 0, &root)); XrCompileResources *resources = resources_new();
    XrLirCRequest request = {.module = m, .entry = root, .resources = resources, .translation_unit = path, .control = NULL};
    XrLirFunctionView view; OK(xr_lir_function_view(m, root, &view));
    XrRbcProgram *helper = view.result == XR_LIR_F64 ? program_new(m, 1) : NULL;
    XrLirCOutput out = {0}; CHECK(xr_lir_emit_c(&request, &out, NULL) == XR_LIR_C_OK);
    CHECK(out.entry_result == view.result);
    xr_lir_module_destroy(m); xr_compile_resources_release(resources);
    if (helper) { dynamic_vm(helper); xr_rbc_program_destroy(helper); }
    FILE *file = fopen(path, "wb"); CHECK(file && out.bytes && out.length);
    CHECK(fwrite(out.bytes, 1, out.length, file) == out.length && fclose(file) == 0);
    xr_lir_c_output_free(&out);
}
static XrLirFunction negation_function(XrLirModule *m) {
    XrLirType type = XR_LIR_F64;
    XrLirFunction f = function(m, "negation", &type, 1, type);
    XrLirBlock b = block(m, f, NULL, 0); OK(xr_lir_set_entry(m, f, b));
    XrLirValue input, result; OK(xr_lir_function_parameter(m, f, 0, &input));
    XrLirInstruction v = {.op = XR_LIR_FNEG, .type = type, .operands = &input,
        .operand_count = 1, .source = {"negation.xr", 11, 7, 5, 7, 7}};
    OK(xr_lir_emit_value(m, b, &v, &result)); ret(m, b, result); return f;
}
static XrLirModule *make_negation(bool output, uint64_t bits) {
    XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m));
    XrLirFunction root = function(m, "root", NULL, 0, XR_LIR_F64);
    XrLirFunction helper = negation_function(m);
    XrLirBlock b = block(m, root, NULL, 0); OK(xr_lir_set_entry(m, root, b));
    XrLirValue argument = constant(m, b, XR_LIR_F64, bits), result;
    XrLirInstruction call = {.op = XR_LIR_CALL, .type = XR_LIR_F64,
        .callee = helper, .operands = &argument, .operand_count = 1};
    OK(xr_lir_emit_value(m, b, &call, &result));
    if (output) output_value(m, b);
    ret(m, b, result); OK(xr_lir_module_finish(m, NULL)); return m;
}
static void observe_negation(XrLirModule *m, bool report) {
    XrLirFunction helper; XrLirFunctionView fv;
    OK(xr_lir_function_at(m, 1, &helper)); OK(xr_lir_function_view(m, helper, &fv));
    CHECK(helper.id == 2 && fv.parameter_count == 1 && fv.parameter_types[0] == XR_LIR_F64 &&
        fv.result == XR_LIR_F64 && fv.block_count == 1);
    XrLirBlock b; XrLirInstructionView v, term;
    OK(xr_lir_block_at(m, helper, 0, &b)); OK(xr_lir_instruction_view(m, b, 0, &v));
    OK(xr_lir_instruction_view(m, b, 1, &term));
    CHECK(v.op == XR_LIR_FNEG && v.type == XR_LIR_F64 && v.operands[0] == fv.parameters[0]);
    CHECK(v.source.path_length == 11 && !memcmp(v.source.path, "negation.xr", 11) &&
        v.source.line == 7 && v.source.column == 5 && v.source.end_line == 7 && v.source.end_column == 7);
    CHECK(term.op == XR_LIR_RET && term.operands[0] == v.result);
    if (report) printf("{\"carrier\":\"f64-negation\",\"helper\":2,\"parameter\":%u,"
        "\"operand\":%u,\"returned\":%u,\"line\":7,\"column\":5}\n",
        fv.parameters[0], v.operands[0], v.result);
}
/* Native admission must not turn the old VM transport success path into a
 * bypass. This fixture requires the VM's distinct FNEG refusal explicitly. */
static void emit_negation(XrLirModule *m, const char *path, bool output) {
    observe_negation(m, path != NULL);
    XrLirFunction root; OK(xr_lir_function_at(m, 0, &root));
    XrRbcCompileDiagnostic diagnostic;
    for (uint32_t i = 0; i < 2; ++i) {
        XrLirFunction f; OK(xr_lir_function_at(m, i, &f)); XrRbcProgram *program = NULL;
        CHECK(xr_rbc_compile(m, f, &program, &diagnostic) == XR_RBC_COMPILE_UNSUPPORTED && !program);
        CHECK(diagnostic.line == 7 && diagnostic.column == 5 && !strcmp(diagnostic.path, "negation.xr"));
    }
    XrCompileResources *resources = resources_new(); XrLirCOutput out = {0};
    XrLirCRequest request = {.module = m, .entry = root, .resources = resources,
        .translation_unit = path ? path : "native-negation.c", .control = NULL};
    CHECK(xr_lir_emit_c(&request, &out, NULL) == XR_LIR_C_OK && out.entry_result == XR_LIR_F64);
    xr_lir_module_destroy(m); xr_compile_resources_release(resources);
    CHECK(diagnostic.line == 7 && !strcmp(diagnostic.path, "negation.xr"));
    const char *policy = strstr(out.bytes, "#pragma STDC FENV_ACCESS ON");
    const char *definitions = strstr(out.bytes, "double ");
    CHECK(policy && definitions && policy < definitions && strstr(out.bytes, "#pragma fenv_access(on)"));
    const char *header = strstr(out.bytes, "#include \"xray_native_runtime.h\"");
    CHECK(output ? header && header < policy : !header);
    CHECK(!strstr(policy, "#include "));
    if (path) {
        FILE *file = fopen(path, "wb"); CHECK(file);
        CHECK(fwrite(out.bytes, 1, out.length, file) == out.length && fclose(file) == 0);
    }
    xr_lir_c_output_free(&out);
}
static void unselected_negation(bool integer_root) {
    XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m)); (void)negation_function(m);
    XrLirType type = integer_root ? XR_LIR_I64 : XR_LIR_F64;
    XrLirFunction root = function(m, "selected", NULL, 0, type);
    XrLirBlock b = block(m, root, NULL, 0); OK(xr_lir_set_entry(m, root, b));
    ret(m, b, constant(m, b, type, 0)); OK(xr_lir_module_finish(m, NULL));
    XrCompileResources *resources = resources_new(); XrLirCOutput out = {0};
    XrLirCRequest request = {.module = m, .entry = root, .resources = resources,
        .translation_unit = "unselected-negation.c", .control = NULL};
    CHECK(xr_lir_emit_c(&request, &out, NULL) == XR_LIR_C_OK);
    CHECK(!strstr(out.bytes, "FENV_ACCESS") && !strstr(out.bytes, "fenv_access"));
    CHECK(integer_root ? !strstr(out.bytes, "double") : strstr(out.bytes, "double") != NULL);
    xr_lir_module_destroy(m); xr_compile_resources_release(resources); xr_lir_c_output_free(&out);
}
static void native_negation(void) {
    for (unsigned nan = 0; nan < 2; ++nan)
        for (unsigned output = 0; output < 2; ++output)
            for (unsigned after = 0; after < 2; ++after) {
                XrLirModule *m = make_negation(output != 0, nan ? UINT64_C(0x7ff8000000000000) : 0);
                if (after) m = sccp_and_retire(m);
                emit_negation(m, NULL, output != 0);
            }
    unselected_negation(true); unselected_negation(false);
}
static XrLirModule *transport(XrLirType type) {
    XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m));
    XrLirFunction f = function(m, "transport", NULL, 0, type); XrLirBlock b = block(m, f, NULL, 0);
    OK(xr_lir_set_entry(m, f, b)); ret(m, b, constant(m, b, type, 0));
    OK(xr_lir_module_finish(m, NULL)); OK(xr_lir_verify(m, NULL)); return m;
}
static void test_frontier(void) {
    const XrLirType native_rejected[] = {XR_LIR_F32};
    for (size_t i = 0; i < sizeof(native_rejected) / sizeof(*native_rejected); ++i) {
        XrLirModule *m = transport(native_rejected[i]); XrLirFunction root; OK(xr_lir_function_at(m, 0, &root));
        XrCompileResources *resources = resources_new(); XrLirCOutput out = {0}; XrLirCDiagnostic diagnostic;
        XrLirCRequest request = {.module = m, .entry = root, .resources = resources, .translation_unit = "frontier.c", .control = NULL};
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
    const XrLirType types[] = {XR_LIR_I8, XR_LIR_U16, XR_LIR_U64, XR_LIR_PTR};
    if (argc == 1) {
        test_frontier(); f64_owned_programs(); reject_float_operations(); native_negation();
        for (size_t i = 0; i < 4; ++i) {
            for (unsigned output = 0; output <= (types[i] == XR_LIR_PTR ? 1u : 0u); ++output) {
                XrLirModule *m = make_carrier(types[i], output != 0, 0); observe(m, types[i], false);
                m = sccp_and_retire(m); observe(m, types[i], false); xr_lir_module_destroy(m);
            }
        }
        puts("Typed carriers, parallel edges and explicit backend frontiers passed"); return 0;
    }
    CHECK(argc == 4); XrLirType type = XR_LIR_TYPE_INVALID;
    if (!strcmp(argv[1], "fneg-zero") || !strcmp(argv[1], "fneg-zero-output") ||
        !strcmp(argv[1], "fneg-nan") || !strcmp(argv[1], "fneg-nan-output")) {
        CHECK(!strcmp(argv[2], "before") || !strcmp(argv[2], "after"));
        bool effect = strstr(argv[1], "-output") != NULL;
        XrLirModule *m = make_negation(effect, strstr(argv[1], "-nan") ? UINT64_C(0x7ff8000000000000) : 0);
        if (!strcmp(argv[2], "after")) m = sccp_and_retire(m);
        emit_negation(m, argv[3], effect); return 0;
    }
    bool output = !strcmp(argv[1], "ptr-output");
    for (size_t i = 0; i < 4; ++i) if (!strcmp(argv[1], xr_lir_type_name(types[i]))) type = types[i];
    if (output) type = XR_LIR_PTR;
    CHECK(!strcmp(argv[2], "before") || !strcmp(argv[2], "after"));
    uint64_t root_bits = 0;
    for (size_t i = 0; i < sizeof(f64_roots) / sizeof(*f64_roots); ++i) {
        char name[40]; snprintf(name, sizeof(name), "%s-output", f64_roots[i].name);
        if (!strcmp(argv[1], f64_roots[i].name) || !strcmp(argv[1], name)) {
            type = XR_LIR_F64; root_bits = f64_roots[i].bits; output = !strcmp(argv[1], name);
        }
    }
    CHECK(type != XR_LIR_TYPE_INVALID);
    XrLirModule *m = make_carrier(type, output, root_bits);
    if (!strcmp(argv[2], "after")) m = sccp_and_retire(m);
    observe(m, type, true); emit_c(m, argv[3]); return 0;
}
