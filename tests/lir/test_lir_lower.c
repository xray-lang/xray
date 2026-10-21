/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_lower.c - Source-backed Cell promotion and fail-closed lowering
 */
#include "lir_source_fixture.h"
#include "lir/xlir_lower.h"
#include "lir/xlir_builder.h"
#include "lir/xlir_verify.h"
#include "lir/xlir_print.h"

typedef struct Text {
    char bytes[65536];
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
static char *read_file(const char *directory, const char *name, const char *suffix) {
    char path[1024];
    int written = snprintf(path, sizeof(path), "%s/%s.%s", directory, name, suffix);
    CHECK(written > 0 && (size_t) written < sizeof(path));
    FILE *file = fopen(path, "rb");
    CHECK(file && fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    CHECK(length >= 0 && (uint64_t) length < SIZE_MAX && fseek(file, 0, SEEK_SET) == 0);
    char *text = xr_malloc((size_t) length + 1);
    CHECK(text && fread(text, 1, (size_t) length, file) == (size_t) length);
    CHECK(!ferror(file) && fclose(file) == 0);
    text[length] = 0;
    return text;
}
static XrLirLowered lower_text(const char *text) {
    SourceFixture fixture = source_fixture(text);
    XrLirLowered output = {0};
    XrLirLowerFailure failure = {0};
    XrLirLowerRequest request = {fixture.closed, fixture.source.snapshot, false};
    if (strchr(text, '?')) {
        const XrXirModule *source = xr_xir_compile_artifact_module(fixture.closed);
        unsigned phis = 0;
        for (uint32_t f = 0; f < source->function_count; ++f)
            for (uint32_t i = 0; i < source->functions[f].instruction_count; ++i)
                if (source->functions[f].instructions[i].op == XR_XIR_PHI)
                    ++phis;
        CHECK(phis > 0);
    }
    XrLirStatus status = xr_lir_lower_checked(&request, &output, &failure);
    source_fixture_free(&fixture);
    if (status != XR_LIR_OK)
        fprintf(stderr, "lower %u %u:%u: %s\n%s\n", (unsigned) status,
                failure.diagnostic.source.line, failure.diagnostic.source.column,
                failure.diagnostic.message, text);
    CHECK(status == XR_LIR_OK && output.module && output.entry.module == output.module &&
          output.entry.id);
    CHECK(!failure.owned_path && failure.diagnostic.status == XR_LIR_OK);
    OK(xr_lir_verify(output.module, NULL));
    XrLirFunctionView entry;
    OK(xr_lir_function_view(output.module, output.entry, &entry));
    CHECK(entry.name_length == 4 && !memcmp(entry.name, "main", 4) && entry.parameter_count == 0);
    XrLirBlock block;
    XrLirInstructionView instruction;
    OK(xr_lir_block_at(output.module, output.entry, 0, &block));
    OK(xr_lir_instruction_view(output.module, block, 0, &instruction));
    CHECK(instruction.op == XR_LIR_CALL);
    uint32_t initializer = instruction.callee;
    OK(xr_lir_instruction_view(output.module, block, 1, &instruction));
    CHECK(instruction.op == XR_LIR_CALL && instruction.callee != initializer);
    unsigned located = 0;
    for (uint32_t f = 0; f < xr_lir_function_count(output.module); ++f) {
        XrLirFunction function;
        XrLirFunctionView view;
        OK(xr_lir_function_at(output.module, f, &function));
        OK(xr_lir_function_view(output.module, function, &view));
        for (uint32_t b = 0; b < view.block_count; ++b) {
            XrLirBlockView bv;
            OK(xr_lir_block_at(output.module, function, b, &block));
            OK(xr_lir_block_view(output.module, block, &bv));
            for (uint32_t i = 0; i < bv.instruction_count; ++i) {
                OK(xr_lir_instruction_view(output.module, block, i, &instruction));
                if (instruction.source.path) {
                    CHECK(instruction.source.path_length == strlen(instruction.source.path));
                    CHECK(instruction.source.line && instruction.source.column);
                    ++located;
                }
            }
        }
    }
    if (!located)
        fprintf(stderr, "no retained source positions:\n%s\n", text);
    CHECK(located > 0);
    return output;
}
static void golden(const char *programs, const char *directory, const char *name) {
    char *source = read_file(programs, name, "xr");
    XrLirLowered output = lower_text(source);
    xr_free(source);
    Text text = {0}, again = {0};
    OK(xr_lir_print(output.module, write_text, &text));
    OK(xr_lir_print(output.module, write_text, &again));
    CHECK(text.length == again.length && !memcmp(text.bytes, again.bytes, text.length));
    char *expected = read_file(directory, name, "lir");
    if (strlen(expected) != text.length || memcmp(expected, text.bytes, text.length)) {
        fprintf(stderr, "golden mismatch %s; actual:\n%s", name, text.bytes);
        exit(1);
    }
    xr_free(expected);
    xr_lir_lowered_free(&output);
    CHECK(!output.module && !output.entry.id);
}
static void supported_control(void) {
    static const char *cases[] = {
        "fn branch(n:i64)->i64{var value=1\nif(n>0){value=2}else{value=3}\nreturn "
        "value}\nprint(branch(1))\n",
        "fn skip(n:i64)->i64{var i=0\nvar "
        "total=0\nwhile(i<n){i=i+1\nif(i==3){continue}\nif(i==8){break}\ntotal=total+i}\nreturn "
        "total}\nprint(skip(0))\nprint(skip(20))\n",
        "fn nested(n:i64)->i64{var i=0\nvar answer=0\nwhile(i<n){var j=0\nvar "
        "reset=1\nwhile(j<i){reset=reset+1\nj=j+1}\nanswer=answer+reset\ni=i+1}\nreturn "
        "answer}\nprint(nested(5))\n",
        "fn swap(n:i64)->i64{var a=1\nvar b=2\nvar i=0\nwhile(i<n){const "
        "t=a\na=b\nb=t\ni=i+1}\nreturn a*10+b}\nprint(swap(3))\n",
        "fn choose(a:bool,b:bool)->bool{return a && (b || !a)}\nfn "
        "result()->i64{if(choose(true,false)){return 1}\nreturn 2}\nprint(result())\n",
        "fn identity<T>(value:T)->T{return value}\nfn answer()->i64{return "
        "identity(42)}\nprint(answer())\n",
        "fn fraction(x:f64)->f64{return -(x+1.5)*2.0/3.0-0.5}\nfn "
        "compare()->i64{if(fraction(2.0)<0.0){return 1}\nreturn 0}\nprint(compare())\n",
        "fn widening(x:i64)->f64{return x as f64}\nfn compare()->i64{if(widening(2)==2.0){return "
        "1}\nreturn 0}\nprint(compare())\n",
        "fn unused()->string{return \"unreachable\"}\nprint(17)\n",
        "fn main()->string{return \"ordinary\"}\nprint(17)\n",
        "fn main()->i64{return 42}\nprint(main())\n",
        "fn effect(value:i64){print(value)}\neffect(7)\n",
        "fn choose_number(flag:bool,n:i64)->i64{var total=0\nvar i=0\nwhile(i<n){"
        "const value=flag ? i+10 : i+20\ntotal=total+value\ni=i+1}\nreturn "
        "total}\nprint(choose_number(true,3))\n",
        "fn flags(n:i64)->bool{var flag=true\nvar i=0\nwhile(i<n){flag=!flag\ni=i+1}\nreturn "
        "flag}\n"
        "fn answer()->i64{return flags(3) ? 10 : 20}\nprint(answer())\n",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        XrLirLowered output = lower_text(cases[i]);
        xr_lir_lowered_free(&output);
    }
}
static void rejected(const char *text, bool overflow, const char *message) {
    SourceFixture fixture = source_fixture(text);
    XrLirLowered output = {0};
    XrLirLowerFailure failure = {0};
    XrLirLowerRequest request = {fixture.closed, fixture.source.snapshot, overflow};
    XrLirStatus status = xr_lir_lower_checked(&request, &output, &failure);
    source_fixture_free(&fixture);
    if (status != XR_LIR_UNSUPPORTED || !strstr(failure.diagnostic.message, message))
        fprintf(stderr, "rejection %u: %s; expected %s\n", (unsigned) status,
                failure.diagnostic.message, message);
    CHECK(status == XR_LIR_UNSUPPORTED && failure.diagnostic.status == status);
    CHECK(strstr(failure.diagnostic.message, message) && !output.module && !output.entry.id);
    CHECK(failure.owned_path && failure.diagnostic.source.path == failure.owned_path);
    CHECK(strlen(failure.owned_path) == failure.diagnostic.source.path_length);
    CHECK(failure.diagnostic.source.line && failure.diagnostic.source.column);
    CHECK(failure.diagnostic.source.end_line >= failure.diagnostic.source.line);
    xr_lir_lower_failure_free(&failure);
    CHECK(!failure.owned_path && !failure.diagnostic.source.path);
}
static void unsupported(void) {
    rejected(
        "struct Pair{x:i64}\nfn answer()->i64{const p=Pair{x:42}\nreturn p.x}\nprint(answer())\n",
        false, "STRUCT_NEW");
    rejected("fn answer()->i64{const s=\"a\"+\"b\"\nreturn len(s)}\nprint(answer())\n", false,
             "CONST_STRING");
    rejected("fn answer()->i64{var n=40\nconst f=fn()->i64{n=n+1\nreturn n}\nreturn "
             "f()}\nprint(answer())\n",
             false, "FUNCTION_REF");
    rejected("class Box{value:i64\ninit(value){this.value=value}}\nfn answer()->i64{const "
             "b=Box(42)\nreturn b.value}\nprint(answer())\n",
             false, "CALL");
    rejected("print(1)\n", true, "overflow-checks");
    rejected("fn quotient(n:i64)->i64{return n/2}\nprint(quotient(8))\n", false, "DIV_INT");
    rejected("fn remainder(n:i64)->i64{return n%2}\nprint(remainder(8))\n", false, "REM_INT");
    rejected("fn convert(x:f64)->i64{return x as i64}\nprint(convert(1.0))\n", false,
             "floating to integer");
}
static void swapped_backedge(void) {
    XrLirLowered output =
        lower_text("fn swap(n:i64)->i64{var a=1\nvar b=2\nvar i=0\nwhile(i<n){const "
                   "t=a\na=b\nb=t\ni=i+1}\nreturn a*10+b}\nprint(swap(3))\n");
    bool checked = false;
    for (uint32_t f = 0; f < xr_lir_function_count(output.module); ++f) {
        XrLirFunction function;
        XrLirFunctionView view;
        OK(xr_lir_function_at(output.module, f, &function));
        OK(xr_lir_function_view(output.module, function, &view));
        if (!strstr(view.name, "_swap"))
            continue;
        for (uint32_t b = 0; b < view.block_count; ++b) {
            XrLirBlock block;
            XrLirBlockView bv;
            XrLirInstructionView term;
            OK(xr_lir_block_at(output.module, function, b, &block));
            OK(xr_lir_block_view(output.module, block, &bv));
            OK(xr_lir_instruction_view(output.module, block, bv.instruction_count - 1, &term));
            if (term.op != XR_LIR_BR || term.edges[0].target >= block.id || bv.parameter_count != 3)
                continue;
            CHECK(!checked && term.edges[0].argument_count == 3);
            CHECK(term.edges[0].arguments[0] == bv.parameters[1]);
            CHECK(term.edges[0].arguments[1] == bv.parameters[0]);
            CHECK(term.edges[0].arguments[2] != bv.parameters[2]);
            checked = true;
        }
    }
    CHECK(checked);
    xr_lir_lowered_free(&output);
}
static void argument_validation(void) {
    XrLirLowered output = {0};
    CHECK(xr_lir_lower_checked(NULL, &output, NULL) == XR_LIR_INVALID_ARGUMENT);
    SourceFixture fixture = source_fixture("print(1)\n");
    XrLirLowerRequest request = {fixture.closed, fixture.source.snapshot, false};
    CHECK(xr_lir_lower_checked(&request, NULL, NULL) == XR_LIR_INVALID_ARGUMENT);
    OK(xr_lir_lower_checked(&request, &output, NULL));
    XrLirModule *module = output.module;
    CHECK(xr_lir_lower_checked(&request, &output, NULL) == XR_LIR_INVALID_ARGUMENT &&
          output.module == module);
    xr_lir_lowered_free(&output);
    source_fixture_free(&fixture);
    xr_lir_lowered_free(NULL);
    xr_lir_lower_failure_free(NULL);
}
int main(int argc, char **argv) {
    CHECK(argc == 3);
    supported_control();
    swapped_backedge();
    unsupported();
    argument_validation();
    golden(argv[1], argv[2], "source-fib");
    golden(argv[1], argv[2], "source-sum");
    puts("source-backed scalar LIR lowering passed");
    return 0;
}
