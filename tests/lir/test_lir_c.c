/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_c.c - Independent scalar C fixtures and owned publication checks
 */
#include "lir/xlir_builder.h"
#include "lir/xlir_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_LIR_OK)

static XrLirModule *module;
static XrLirFunction root;

static XrLirFunction function(const char *name, const XrLirType *parameters, uint32_t count,
                              XrLirType result) {
    XrLirFunction f;
    XrLirFunctionSpec spec = {.name = name, .name_length = strlen(name), .parameters = parameters,
        .parameter_count = count, .result = result};
    OK(xr_lir_declare_function(module, &spec, &f));
    OK(xr_lir_define_function(module, f));
    return f;
}
static XrLirBlock block(XrLirFunction f, const XrLirType *parameters, uint32_t count) {
    XrLirBlock b;
    OK(xr_lir_add_block(module, f, parameters, count, &b));
    return b;
}
static XrLirValue constant(XrLirBlock b, XrLirType type, uint64_t bits) {
    XrLirValue result;
    XrLirInstruction v = {.op = XR_LIR_CONST, .type = type, .bits = bits};
    OK(xr_lir_emit_value(module, b, &v, &result));
    return result;
}
static XrLirValue operation(XrLirBlock b, XrLirOp op, XrLirType type, XrLirValue a,
                            XrLirValue other) {
    XrLirValue operands[] = {a, other}, result;
    XrLirInstruction v = {.op = op, .type = type, .operands = operands,
        .operand_count = other.id ? 2 : 1};
    OK(xr_lir_emit_value(module, b, &v, &result));
    return result;
}
static XrLirValue compare(XrLirBlock b, XrLirPredicate predicate, XrLirValue a, XrLirValue other) {
    XrLirValue operands[] = {a, other}, result;
    XrLirInstruction v = {.op = XR_LIR_ICMP, .type = XR_LIR_I1, .operands = operands,
        .operand_count = 2, .predicate = predicate};
    OK(xr_lir_emit_value(module, b, &v, &result));
    return result;
}
static XrLirValue call(XrLirBlock b, XrLirFunction f, const XrLirValue *args, uint32_t count) {
    XrLirValue result;
    XrLirInstruction v = {.op = XR_LIR_CALL, .type = XR_LIR_I64, .callee = f,
        .operands = args, .operand_count = count};
    OK(xr_lir_emit_value(module, b, &v, &result));
    return result;
}
static void ret(XrLirBlock b, XrLirValue value) {
    XrLirInstruction v = {.op = XR_LIR_RET, .type = XR_LIR_UNIT,
        .operands = value.id ? &value : NULL, .operand_count = value.id ? 1 : 0};
    OK(xr_lir_set_terminator(module, b, &v));
}
static void branch(XrLirBlock from, XrLirBlock to, const XrLirValue *values, uint32_t count) {
    XrLirInstruction v = {.op = XR_LIR_BR, .type = XR_LIR_UNIT, .edge_count = 1,
        .edges = {{to, values, count}}};
    OK(xr_lir_set_terminator(module, from, &v));
}
static void conditional(XrLirBlock from, XrLirValue condition, XrLirEdge a, XrLirEdge b) {
    XrLirInstruction v = {.op = XR_LIR_CONDBR, .type = XR_LIR_UNIT,
        .operands = &condition, .operand_count = 1, .edge_count = 2, .edges = {a, b}};
    OK(xr_lir_set_terminator(module, from, &v));
}
static XrLirBlock root_block(void) {
    root = function("explicit root", NULL, 0, XR_LIR_I64);
    XrLirBlock b = block(root, NULL, 0);
    OK(xr_lir_set_entry(module, root, b));
    return b;
}
static void arithmetic(void) {
    XrLirBlock b = root_block();
    XrLirValue total = constant(b, XR_LIR_I64, 0);
    for (XrLirType type = XR_LIR_I8; type <= XR_LIR_U64; ++type) {
        unsigned ordinal = type <= XR_LIR_I64 ? (unsigned) (type - XR_LIR_I8)
            : (unsigned) (type - XR_LIR_U8);
        unsigned width = 8u << ordinal;
        uint64_t mask = width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
        XrLirValue one = constant(b, type, 1);
        XrLirValue maximum = constant(b, type, mask);
        XrLirValue zero = operation(b, XR_LIR_ADD, type, maximum, one);
        XrLirValue minus_one = operation(b, XR_LIR_SUB, type, zero, one);
        XrLirValue neg = operation(b, XR_LIR_NEG, type, minus_one, (XrLirValue) {0});
        XrLirValue product = operation(b, XR_LIR_MUL, type, maximum, maximum);
        XrLirValue sum = operation(b, XR_LIR_ADD, type, product, neg);
        XrLirValue widened = operation(b, XR_LIR_INTCAST, XR_LIR_I64, sum, (XrLirValue) {0});
        total = operation(b, XR_LIR_ADD, XR_LIR_I64, total, widened);
    }
    /* uint16 multiplication must not first overflow the promoted signed int. */
    XrLirValue u16 = constant(b, XR_LIR_U16, UINT16_MAX);
    XrLirValue product = operation(b, XR_LIR_MUL, XR_LIR_U16, u16, u16);
    XrLirValue wide = operation(b, XR_LIR_INTCAST, XR_LIR_I64, product, (XrLirValue) {0});
    ret(b, operation(b, XR_LIR_ADD, XR_LIR_I64, total, wide));
}
static void extremes(void) {
    XrLirBlock b = root_block();
    XrLirValue minimum = constant(b, XR_LIR_I64, UINT64_C(0x8000000000000000));
    XrLirValue neg = operation(b, XR_LIR_NEG, XR_LIR_I64, minimum, (XrLirValue) {0});
    XrLirValue diff = operation(b, XR_LIR_SUB, XR_LIR_I64, neg, minimum);
    XrLirValue negative = constant(b, XR_LIR_I8, 255);
    XrLirValue extended = operation(b, XR_LIR_INTCAST, XR_LIR_I64, negative, (XrLirValue) {0});
    XrLirValue narrow = operation(b, XR_LIR_INTCAST, XR_LIR_U8, minimum, (XrLirValue) {0});
    XrLirValue z = operation(b, XR_LIR_INTCAST, XR_LIR_I64, narrow, (XrLirValue) {0});
    diff = operation(b, XR_LIR_ADD, XR_LIR_I64, diff, extended);
    ret(b, operation(b, XR_LIR_ADD, XR_LIR_I64, diff, z));
}
static void overflow(void) {
    XrLirBlock b = root_block();
    XrLirValue maximum = constant(b, XR_LIR_I64, INT64_MAX);
    XrLirValue one = constant(b, XR_LIR_I64, 1), two = constant(b, XR_LIR_I64, 2);
    XrLirValue minimum = operation(b, XR_LIR_ADD, XR_LIR_I64, maximum, one);
    XrLirValue minus_two = operation(b, XR_LIR_MUL, XR_LIR_I64, maximum, two);
    ret(b, operation(b, XR_LIR_SUB, XR_LIR_I64, minus_two, minimum));
}
static void fib(void) {
    XrLirType i64 = XR_LIR_I64;
    XrLirFunction f = function("fib", &i64, 1, i64);
    XrLirBlock entry = block(f, NULL, 0), base = block(f, NULL, 0), recursive = block(f, NULL, 0);
    OK(xr_lir_set_entry(module, f, entry));
    XrLirValue n;
    OK(xr_lir_function_parameter(module, f, 0, &n));
    XrLirValue two = constant(entry, i64, 2);
    XrLirValue condition = compare(entry, XR_LIR_LT, n, two);
    conditional(entry, condition, (XrLirEdge) {base, NULL, 0}, (XrLirEdge) {recursive, NULL, 0});
    ret(base, n);
    XrLirValue one = constant(recursive, i64, 1);
    XrLirValue nm1 = operation(recursive, XR_LIR_SUB, i64, n, one);
    XrLirValue nm2 = operation(recursive, XR_LIR_SUB, i64, n, two);
    XrLirValue a = call(recursive, f, &nm1, 1), b = call(recursive, f, &nm2, 1);
    ret(recursive, operation(recursive, XR_LIR_ADD, i64, a, b));
    XrLirBlock r = root_block();
    XrLirValue argument = constant(r, i64, 32);
    ret(r, call(r, f, &argument, 1));
}
static void parallel(bool same_target, bool selected) {
    XrLirBlock entry = root_block();
    XrLirType types[] = {XR_LIR_I64, XR_LIR_I64, XR_LIR_I64};
    XrLirBlock loop = block(root, types, 3), exit_block = block(root, NULL, 0);
    XrLirValue x, y, iteration;
    OK(xr_lir_block_parameter(module, loop, 0, &x));
    OK(xr_lir_block_parameter(module, loop, 1, &y));
    OK(xr_lir_block_parameter(module, loop, 2, &iteration));
    XrLirValue a = constant(entry, XR_LIR_I64, 12), b = constant(entry, XR_LIR_I64, 34);
    XrLirValue zero = constant(entry, XR_LIR_I64, 0);
    XrLirValue initial[] = {a, b, zero};
    if (same_target) {
        XrLirValue other[] = {b, a, zero};
        XrLirValue cond = constant(entry, XR_LIR_I1, selected ? 1 : 0);
        conditional(entry, cond, (XrLirEdge) {loop, initial, 3}, (XrLirEdge) {loop, other, 3});
        branch(loop, exit_block, NULL, 0);
    } else {
        branch(entry, loop, initial, 3);
        XrLirValue one = constant(loop, XR_LIR_I64, 1), three = constant(loop, XR_LIR_I64, 3);
        XrLirValue next = operation(loop, XR_LIR_ADD, XR_LIR_I64, iteration, one);
        XrLirValue cond = compare(loop, XR_LIR_LT, next, three);
        XrLirValue swapped[] = {selected ? x : y, x, next};
        conditional(loop, cond, (XrLirEdge) {loop, swapped, 3}, (XrLirEdge) {exit_block, NULL, 0});
    }
    XrLirValue hundred = constant(exit_block, XR_LIR_I64, 100);
    XrLirValue high = operation(exit_block, XR_LIR_MUL, XR_LIR_I64, x, hundred);
    ret(exit_block, operation(exit_block, XR_LIR_ADD, XR_LIR_I64, high, y));
}
static void unused_float(void) {
    XrLirFunction unused = function("main", NULL, 0, XR_LIR_F64);
    XrLirBlock b = block(unused, NULL, 0);
    OK(xr_lir_set_entry(module, unused, b));
    ret(b, constant(b, XR_LIR_F64, UINT64_C(0x3ff0000000000000)));
    b = root_block();
    ret(b, constant(b, XR_LIR_I64, 42));
}
static void small_result(bool unit) {
    root = function("selected", NULL, 0, unit ? XR_LIR_UNIT : XR_LIR_I1);
    XrLirBlock b = block(root, NULL, 0);
    OK(xr_lir_set_entry(module, root, b));
    ret(b, unit ? (XrLirValue) {0} : constant(b, XR_LIR_I1, 1));
}
static void output_call(const char *kind) {
    XrLirBlock b = root_block();
    XrLirType i64 = XR_LIR_I64;
    XrLirFunction print;
    XrLirFunctionSpec spec = {.name = "xrt_print_i64", .name_length = 13,
        .parameters = &i64, .parameter_count = 1, .result = XR_LIR_UNIT,
        .kind = XR_LIR_FUNCTION_EXTERNAL, .extern_kind = XR_LIR_EXTERN_PRINT_I64};
    OK(xr_lir_declare_function(module, &spec, &print));
    XrLirValue value = constant(b, XR_LIR_I64, 5);
    XrLirInstruction call = {.op = XR_LIR_CALL, .type = XR_LIR_UNIT,
        .callee = print, .operands = &value, .operand_count = 1};
    if (strcmp(kind, "output-unknown")) {
        static const char path[] = "odd\"\\\303\251\n.xr";
        call.source = (XrLirSource) {path, sizeof(path) - 1, 4, 3, 4, 3};
        if (!strcmp(kind, "output-range")) {
            call.source.end_line = 5;
            call.source.end_column = 8;
        }
    }
    OK(xr_lir_emit_effect(module, b, &call));
    ret(b, constant(b, XR_LIR_I64, 0));
}
static void build(const char *name) {
    OK(xr_lir_module_create(NULL, &module));
    if (!strcmp(name, "arithmetic")) arithmetic();
    else if (!strcmp(name, "extremes")) extremes();
    else if (!strcmp(name, "overflow")) overflow();
    else if (!strcmp(name, "fib")) fib();
    else if (!strcmp(name, "swap")) parallel(false, false);
    else if (!strcmp(name, "self")) parallel(false, true);
    else if (!strcmp(name, "same-true")) parallel(true, true);
    else if (!strcmp(name, "same-false")) parallel(true, false);
    else if (!strcmp(name, "unused")) unused_float();
    else if (!strcmp(name, "unit")) small_result(true);
    else if (!strcmp(name, "bool")) small_result(false);
    else if (!strncmp(name, "output-", 7)) output_call(name);
    else CHECK(false);
    OK(xr_lir_module_finish(module, NULL));
}
static XrCompileResources *resources(void) {
    XrCompileResourceLimits limits = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
    XrCompileResources *r = NULL;
    CHECK(xr_compile_resources_new(&limits, &r) == XR_COMPILE_RESOURCE_OK);
    return r;
}
static void emit(const char *name, const char *path) {
    build(name);
    XrCompileResources *r = resources();
    XrLirCRequest request = {module, root, r, "scalar-test"};
    XrLirCOutput output = {0};
    XrLirCDiagnostic diagnostic = {0};
    XrLirCStatus status = xr_lir_emit_c(&request, &output, &diagnostic);
    if (status != XR_LIR_C_OK)
        fprintf(stderr, "C status %u: %s\n", (unsigned) status, diagnostic.message);
    CHECK(status == XR_LIR_C_OK && output.bytes && output.length);
    CHECK(output.entry_result == (!strcmp(name, "unit") ? XR_LIR_UNIT
        : !strcmp(name, "bool") ? XR_LIR_I1 : XR_LIR_I64));
    xr_lir_module_destroy(module);
    module = NULL;
    xr_compile_resources_release(r);
    /* The generated text is consumed only after both producers are gone. */
    CHECK(strstr(output.bytes, "xray_lir_entry(void)") && !strstr(output.bytes, "({"));
    if (!strncmp(name, "output-", 7)) {
        CHECK(strstr(output.bytes, "xrt_print_i64(v1, UINT32_C("));
        if (!strcmp(name, "output-unknown"))
            CHECK(strstr(output.bytes, "{ NULL, UINT32_C(0) }") &&
                  strstr(output.bytes, "xrt_print_i64(v1, UINT32_C(0));"));
        else
            CHECK(strstr(output.bytes, "\\303\\251\\012.xr") &&
                  strstr(output.bytes, "xrt_print_i64(v1, UINT32_C(1));"));
    }
    if (path) {
        FILE *file = fopen(path, "wb");
        CHECK(file && fwrite(output.bytes, 1, output.length, file) == output.length);
        CHECK(fclose(file) == 0);
    }
    xr_lir_c_output_free(&output);
    CHECK(!output.bytes && !output.length && !output.entry_result);
}
static void reject(void) {
    OK(xr_lir_module_create(NULL, &module));
    XrLirBlock b = root_block();
    XrLirValue one = constant(b, XR_LIR_I64, 1);
    ret(b, operation(b, XR_LIR_DIV, XR_LIR_I64, one, one));
    OK(xr_lir_module_finish(module, NULL));
    XrCompileResources *r = resources();
    XrLirCRequest request = {module, root, r, "rejected"};
    XrLirCOutput output = {0};
    XrLirCDiagnostic diagnostic;
    CHECK(xr_lir_emit_c(&request, &output, &diagnostic) == XR_LIR_C_UNSUPPORTED);
    CHECK(diagnostic.message[0] && !output.bytes && !output.length);
    request.entry = (XrLirFunction) {0};
    CHECK(xr_lir_emit_c(&request, &output, &diagnostic) == XR_LIR_C_BAD_ARGUMENT);
    CHECK(!output.bytes);
    xr_lir_module_destroy(module);
    xr_compile_resources_release(r);
}
int main(int argc, char **argv) {
    if (argc == 3) {
        emit(argv[1], argv[2]);
        return 0;
    }
    CHECK(argc == 1);
    const char *cases[] = {"arithmetic", "extremes", "overflow", "fib", "swap", "self",
        "same-true", "same-false", "unused", "unit", "bool",
        "output-unknown", "output-point", "output-range"};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        emit(cases[i], NULL);
    reject();
    puts("owned scalar C publication and rejection passed");
    return 0;
}
