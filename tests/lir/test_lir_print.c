/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_print.c - Stable scalar text independent of host newline handling
 */
#include "lir_test_programs.h"
#include "lir/xlir_print.h"

typedef struct Text {
    char bytes[16384];
    size_t length;
} Text;

static bool write_text(void *context, const char *bytes, size_t length) {
    Text *text = context;
    CHECK(length < sizeof(text->bytes) - text->length);
    memcpy(text->bytes + text->length, bytes, length);
    text->length += length;
    text->bytes[text->length] = 0;
    return true;
}

static bool reject_text(void *context, const char *bytes, size_t length) {
    bool *called = context;
    (void) bytes;
    (void) length;
    CHECK(!*called);
    *called = true;
    return false;
}

static Text render(XrLirModule *module) {
    Text text = {0};
    OK(xr_lir_print(module, write_text, &text));
    return text;
}

static void golden(const char *directory, const char *name, XrLirModule *module) {
    Text actual = render(module), again = render(module);
    CHECK(actual.length == again.length && !memcmp(actual.bytes, again.bytes, actual.length));
    char path[1024];
    int written = snprintf(path, sizeof(path), "%s/%s.lir", directory, name);
    CHECK(written > 0 && (size_t) written < sizeof(path));
    FILE *file = fopen(path, "rb");
    CHECK(file);
    char expected[16384];
    size_t length = fread(expected, 1, sizeof(expected), file);
    CHECK(!ferror(file) && fclose(file) == 0);
    if (length != actual.length || memcmp(expected, actual.bytes, length)) {
        fprintf(stderr, "golden mismatch %s; actual:\n%s", name, actual.bytes);
        exit(1);
    }
    bool called = false;
    CHECK(xr_lir_print(module, reject_text, &called) == XR_LIR_OUTPUT_ERROR && called);
    again = render(module);
    CHECK(actual.length == again.length && !memcmp(actual.bytes, again.bytes, actual.length));
}

static void constants_and_names(void) {
    XrLirModule *module = NULL;
    OK(xr_lir_module_create(NULL, &module));
    XrLirFunction function = test_function(module, "space \"name\"\\\n\xc3\xa9", NULL, 0, XR_LIR_UNIT);
    XrLirBlock entry = test_block(module, function, NULL, 0);
    OK(xr_lir_set_entry(module, function, entry));
    const uint64_t bits[] = {1, 128, 32768, UINT64_C(0x80000000),
        UINT64_C(0x8000000000000000), 255, 65535, UINT64_C(0xffffffff), UINT64_MAX,
        UINT64_C(0x80000000), UINT64_C(0x7ff8000000000000), 0};
    for (unsigned i = 0; i < sizeof(bits) / sizeof(bits[0]); ++i)
        test_constant(module, entry, (XrLirType) (XR_LIR_I1 + i), bits[i]);
    test_return(module, entry, (XrLirValue) {0});
    OK(xr_lir_module_finish(module, NULL));
    Text text = render(module);
    xr_lir_module_destroy(module);
    CHECK(strstr(text.bytes, "@\"space \\\"name\\\"\\\\\\x0a\\xc3\\xa9\""));
    CHECK(strstr(text.bytes, "const i8 -128") &&
          strstr(text.bytes, "const i16 -32768") &&
          strstr(text.bytes, "const i32 -2147483648") &&
          strstr(text.bytes, "const i64 -9223372036854775808") &&
          strstr(text.bytes, "const u64 18446744073709551615") &&
          strstr(text.bytes, "const f32 0x80000000") &&
          strstr(text.bytes, "const f64 0x7ff8000000000000") &&
          strstr(text.bytes, "const ptr null"));
    CHECK(!strchr(text.bytes, '\r'));
}

static void entry_and_same_target(void) {
    XrLirModule *module = NULL;
    OK(xr_lir_module_create(NULL, &module));
    XrLirType boolean = XR_LIR_I1, integer = XR_LIR_I64;
    XrLirFunction function = test_function(module, "choose", &boolean, 1, integer);
    XrLirBlock join = test_block(module, function, &integer, 1);
    XrLirBlock entry = test_block(module, function, NULL, 0);
    OK(xr_lir_set_entry(module, function, entry));
    XrLirValue condition = {0}, answer = {0};
    OK(xr_lir_function_parameter(module, function, 0, &condition));
    OK(xr_lir_block_parameter(module, join, 0, &answer));
    XrLirValue a = test_constant(module, entry, integer, 11);
    XrLirValue b = test_constant(module, entry, integer, 29);
    XrLirInstruction branch = {.op = XR_LIR_CONDBR, .type = XR_LIR_UNIT,
        .operands = &condition, .operand_count = 1,
        .edges = {{join, &a, 1}, {join, &b, 1}}, .edge_count = 2};
    OK(xr_lir_set_terminator(module, entry, &branch));
    test_return(module, join, answer);
    OK(xr_lir_module_finish(module, NULL));
    Text text = render(module);
    CHECK(strstr(text.bytes, "entry ^b1\n") &&
          strstr(text.bytes, "condbr %0, ^b0(%2), ^b0(%3)"));
    xr_lir_module_destroy(module);
}

static void intrinsic_format(void) {
    XrLirType type = XR_LIR_F64;
    XrLirIntrinsic intrinsic = {71, &type, 1, type, true};
    XrLirRegistry registry = {&intrinsic, 1};
    XrLirModule *module = NULL;
    OK(xr_lir_module_create(&registry, &module));
    XrLirFunction function = test_function(module, "float_identity", &type, 1, type);
    XrLirBlock entry = test_block(module, function, NULL, 0);
    OK(xr_lir_set_entry(module, function, entry));
    XrLirValue parameter = {0}, converted = {0}, result = {0};
    OK(xr_lir_function_parameter(module, function, 0, &parameter));
    XrLirInstruction instruction = {.op = XR_LIR_FCAST, .type = type,
        .operands = &parameter, .operand_count = 1};
    OK(xr_lir_emit_value(module, entry, &instruction, &converted));
    instruction = (XrLirInstruction) {.op = XR_LIR_CALL_INTRINSIC, .type = type,
        .intrinsic = 71, .operands = &converted, .operand_count = 1};
    OK(xr_lir_emit_value(module, entry, &instruction, &result));
    test_return(module, entry, result);
    OK(xr_lir_module_finish(module, NULL));
    Text text = render(module);
    CHECK(strstr(text.bytes, "fcast f64") && strstr(text.bytes, "call_intrinsic #71"));
    xr_lir_module_destroy(module);
}

static void invalid_arguments(void) {
    Text text = {0};
    XrLirModule *module = NULL;
    CHECK(xr_lir_print(NULL, write_text, &text) == XR_LIR_INVALID_ARGUMENT);
    OK(xr_lir_module_create(NULL, &module));
    CHECK(xr_lir_print(module, write_text, &text) == XR_LIR_INVALID_ARGUMENT && !text.length);
    OK(xr_lir_module_finish(module, NULL));
    CHECK(xr_lir_print(module, NULL, &text) == XR_LIR_INVALID_ARGUMENT && !text.length);
    text = render(module);
    CHECK(!strcmp(text.bytes, "module {\n}\n"));
    xr_lir_module_destroy(module);
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    XrLirModule *fib = test_fib(), *sum = test_sum();
    golden(argv[1], "fib", fib);
    golden(argv[1], "sum", sum);
    xr_lir_module_destroy(fib);
    xr_lir_module_destroy(sum);
    constants_and_names();
    entry_and_same_target();
    intrinsic_format();
    invalid_arguments();
    puts("scalar LIR text, LF goldens and sink failures passed");
    return 0;
}
