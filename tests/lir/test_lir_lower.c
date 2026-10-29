/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_lower.c - Source-backed Cell promotion and fail-closed lowering
 */
#include "lir_source_fixture.h"
#include "xir/xxir_types.h"
#include "base/xmalloc.h"
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
    XrLirStatus status = xr_lir_lower_program_checked(&request, &output, &failure);
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
        "fn answer()->i64{const s=\"a\"+\"b\"\nreturn len(s)}\nprint(answer())\n",
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
    XrLirStatus status = xr_lir_lower_program_checked(&request, &output, &failure);
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
    rejected("fn answer()->i64{var n=40\nconst f=fn()->i64{n=n+1\nreturn n}\nreturn "
             "f()}\nprint(answer())\n",
             false, "FUNCTION_REF");
    rejected("class Box{value:i64\ninit(value){this.value=value}}\nfn answer()->i64{const "
             "b=Box(42)\nreturn b.value}\nprint(answer())\n",
             false, "CALL");
    rejected("print(1)\n", true, "overflow-checks");
    rejected("fn quotient(n:i64,d:i64)->i64{return n/d}\nprint(quotient(8,2))\n", false, "DIV_INT");
    rejected("fn remainder(n:i64,d:i64)->i64{return n%d}\nprint(remainder(8,2))\n", false, "REM_INT");
    rejected("fn convert(x:f64)->i64{return x as i64}\nprint(convert(1.0))\n", false,
             "floating to integer");
}
static void fixed_division_source(const char *text, bool slot_frontier) {
    SourceFixture fixture = source_fixture(text);
    const XrXirModule *closed = xr_xir_compile_artifact_module(fixture.closed);
    const XrXirDeclarations *declarations = closed->declarations;
    printf("Closed modules=%u root=%u slots=%u storage=%u dependencies=%u\n",
           declarations->module_count, declarations->root_module, declarations->slot_count,
           declarations->modules[0].storage, declarations->modules[0].dependency_count);
    for (uint32_t i = 0; i < declarations->slot_count; ++i)
        printf("Closed slot%u module=%u type=%u mutable=%u\n", i,
               declarations->slots[i].module, (unsigned)declarations->slots[i].type,
               declarations->slots[i].mutable);
    unsigned source_ops = 0, lir_ops = 0;
    for (uint32_t f = 0; f < closed->function_count; ++f) {
        const XrXirFunction *function = &closed->functions[f];
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            const XrXirInstruction *v = &function->instructions[i];
            if (v->op == XR_XIR_SLOT_LOAD || v->op == XR_XIR_SLOT_INIT || v->op == XR_XIR_SLOT_STORE)
                printf("Closed f%u instruction%u %s type=%u args=(%u,%u) immediate=%lld\n",
                       f, i, xr_xir_op_name(v->op), (unsigned)v->type,
                       v->args[0], v->args[1], (long long)v->immediate);
            if (v->op != XR_XIR_DIV_INT && v->op != XR_XIR_REM_INT)
                continue;
            CHECK(v->type == XR_XIR_I64 && v->args[1] >= function->parameter_count);
            uint32_t ordinal = v->args[1] - function->parameter_count;
            CHECK(ordinal < function->instruction_count);
            const XrXirInstruction *rhs = &function->instructions[ordinal];
            CHECK(rhs->op == XR_XIR_CONST_INT && rhs->type == XR_XIR_I64);
            CHECK(rhs->immediate != 0);
            CHECK(v->args[0] < function->parameter_count ||
                  function->instructions[v->args[0] - function->parameter_count].op != XR_XIR_CONST_INT);
            printf("Closed fixed divisor: %s rhs=%lld, dynamic numerator\n",
                   xr_xir_op_name(v->op), (long long)rhs->immediate);
            ++source_ops;
        }
    }
    if (slot_frontier) {
        CHECK(declarations->module_count == 1 && declarations->root_module == 0);
        CHECK(declarations->slot_count == 1 && declarations->slots[0].type == XR_XIR_I64);
        CHECK(!declarations->slots[0].mutable && !declarations->modules[0].dependency_count);
        CHECK(declarations->modules[0].storage == XR_XIR_MODULE_FRESH_STORAGE);
    }
    source_fixture_free(&fixture);
    CHECK(source_ops > 0);
    XrLirLowered output = lower_text(text);
    for (uint32_t f = 0; f < xr_lir_function_count(output.module); ++f) {
        XrLirFunction function;
        XrLirFunctionView view;
        OK(xr_lir_function_at(output.module, f, &function));
        OK(xr_lir_function_view(output.module, function, &view));
        uint8_t *constants = xr_calloc((size_t) view.value_count + 1, 1);
        CHECK(constants);
        for (uint32_t b = 0; b < view.block_count; ++b) {
            XrLirBlock block;
            XrLirBlockView bv;
            OK(xr_lir_block_at(output.module, function, b, &block));
            OK(xr_lir_block_view(output.module, block, &bv));
            for (uint32_t i = 0; i < bv.instruction_count; ++i) {
                XrLirInstructionView v;
                OK(xr_lir_instruction_view(output.module, block, i, &v));
                if (v.op == XR_LIR_CONST && v.type == XR_LIR_I64 && v.bits)
                    constants[v.result] = 1;
            }
        }
        for (uint32_t b = 0; b < view.block_count; ++b) {
            XrLirBlock block;
            XrLirBlockView bv;
            OK(xr_lir_block_at(output.module, function, b, &block));
            OK(xr_lir_block_view(output.module, block, &bv));
            for (uint32_t i = 0; i < bv.instruction_count; ++i) {
                XrLirInstructionView v;
                OK(xr_lir_instruction_view(output.module, block, i, &v));
                if (v.op != XR_LIR_DIV && v.op != XR_LIR_REM)
                    continue;
                CHECK(v.type == XR_LIR_I64 && constants[v.operands[1]]);
                CHECK(!constants[v.operands[0]] && v.source.path && v.source.line && v.source.column);
                ++lir_ops;
            }
        }
        xr_free(constants);
    }
    CHECK(lir_ops == source_ops);
    xr_lir_lowered_free(&output);
}
static void fixed_division(const char *fixtures) {
    fixed_division_source("fn quotient(n:i64)->i64{return n/2}\nprint(quotient(8))\n", false);
    fixed_division_source("fn remainder(n:i64)->i64{return n%2}\nprint(remainder(8))\n", false);
    char *original = read_file(fixtures, "../../../bench/native/integer_loop", "xr");
    fixed_division_source(original, true);
    xr_free(original);
    rejected("fn f(n:i64)->i64{return n/0}\nprint(f(8))\n", false, "DIV_INT");
    rejected("fn f(n:i64)->i64{return n%0}\nprint(f(8))\n", false, "REM_INT");
    fixed_division_source("fn q(n:i64)->i64{return n/(-1)}\n"
                          "print(q(-9223372036854775807-1))\n", false);
    fixed_division_source("fn r(n:i64)->i64{return n%(-1)}\n"
                          "print(r(-9223372036854775807-1))\n", false);
    rejected("fn f(n:i64,d:i64)->i64{return n/(d+2)}\nprint(f(8,1))\n", false, "DIV_INT");
    rejected("fn f(n:i64)->i64{return n/(3 as i64)}\nprint(f(8))\n", false, "DIV_INT");
    rejected("fn f(n:i64,flag:bool)->i64{return n%(flag?3:7)}\nprint(f(8,true))\n", false, "REM_INT");
}
static void floating_negation_source(void) {
    const char *text = "fn negative(x:f64)->f64{\nprint(13)\nreturn -x\n}\nnegative(1.25)\nprint(2)\n";
    SourceFixture fixture = source_fixture(text);
    const XrXirModule *closed = xr_xir_compile_artifact_module(fixture.closed);
    const XrXirSourceDebugView *debug = xr_xir_compile_source_snapshot_debug(fixture.source.snapshot);
    const XrXirSourceView *source = xr_xir_compile_source_snapshot_view(fixture.source.snapshot);
    CHECK(closed && debug && source);
    XrXirSourceRange expected = {0}; char *path = NULL; size_t path_length = 0;
    unsigned source_ops = 0, lir_ops = 0;
    for (uint32_t f = 0; f < closed->function_count; ++f) {
        const XrXirFunction *function = &closed->functions[f];
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            const XrXirInstruction *v = &function->instructions[i];
            if (v->op != XR_XIR_NEG_FLOAT) continue;
            CHECK(!source_ops++ && v->type == XR_XIR_F64 && function->parameter_count == 1);
            CHECK(v->args[0] < function->parameter_count &&
                function->parameters[v->args[0]] == XR_XIR_F64);
            uint32_t origin = UINT32_MAX;
            CHECK(xr_xir_compile_specialization_origin(fixture.closed, f, &origin) == XR_XIR_OK);
            CHECK(origin < debug->function_count && debug->functions[origin].points &&
                i < debug->functions[origin].instruction_count);
            expected = debug->functions[origin].points[i].range;
            CHECK(expected.line == 3 && expected.column > 0 && expected.module < source->module_count);
            const XrXirSourceQueryModule *module = &source->modules[expected.module];
            const char *borrowed = module->path ? module->path : module->identity;
            CHECK(borrowed && borrowed[0]); path_length = strlen(borrowed);
            CHECK(path_length < SIZE_MAX); path = xr_malloc(path_length + 1); CHECK(path);
            memcpy(path, borrowed, path_length + 1);
        }
    }
    CHECK(source_ops == 1 && path);
    XrLirLowerRequest request = {fixture.closed, fixture.source.snapshot, false};
    XrLirLowered output = {0}; XrLirLowerFailure failure = {0};
    XrLirStatus status = xr_lir_lower_program_checked(&request, &output, &failure);
    source_fixture_free(&fixture);
    CHECK(status == XR_LIR_OK && output.module && !failure.owned_path);
    OK(xr_lir_verify(output.module, NULL));
    for (uint32_t f = 0; f < xr_lir_function_count(output.module); ++f) {
        XrLirFunction function; XrLirFunctionView fv;
        OK(xr_lir_function_at(output.module, f, &function));
        OK(xr_lir_function_view(output.module, function, &fv));
        for (uint32_t b = 0; b < fv.block_count; ++b) {
            XrLirBlock block; XrLirBlockView bv;
            OK(xr_lir_block_at(output.module, function, b, &block));
            OK(xr_lir_block_view(output.module, block, &bv));
            for (uint32_t i = 0; i < bv.instruction_count; ++i) {
                XrLirInstructionView v; OK(xr_lir_instruction_view(output.module, block, i, &v));
                if (v.op != XR_LIR_FNEG) continue;
                CHECK(v.type == XR_LIR_F64 && fv.parameter_count == 1 && v.operands[0] == fv.parameters[0]);
                CHECK(v.source.path_length == path_length && !memcmp(v.source.path, path, path_length));
                CHECK(v.source.line == (uint32_t)expected.line && v.source.column == (uint32_t)expected.column);
                CHECK(v.source.end_line == (uint32_t)(expected.end_line ? expected.end_line : expected.line));
                CHECK(v.source.end_column == (uint32_t)(expected.end_line ? expected.end_column : expected.column));
                ++lir_ops;
            }
        }
    }
    CHECK(lir_ops == source_ops); xr_lir_lowered_free(&output); xr_free(path);
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
    CHECK(xr_lir_lower_program_checked(NULL, &output, NULL) == XR_LIR_INVALID_ARGUMENT);
    SourceFixture fixture = source_fixture("print(1)\n");
    XrLirLowerRequest request = {fixture.closed, fixture.source.snapshot, false};
    CHECK(xr_lir_lower_program_checked(&request, NULL, NULL) == XR_LIR_INVALID_ARGUMENT);
    OK(xr_lir_lower_program_checked(&request, &output, NULL));
    XrLirModule *module = output.module;
    CHECK(xr_lir_lower_program_checked(&request, &output, NULL) == XR_LIR_INVALID_ARGUMENT &&
          output.module == module);
    xr_lir_lowered_free(&output);
    source_fixture_free(&fixture);
    xr_lir_lowered_free(NULL);
    xr_lir_lower_failure_free(NULL);
}
static void literal_sources(void) {
    const char *text = "const n=7\nprint(n)\nprint(n)\n";
    SourceFixture fixture = source_fixture(text);
    const XrXirModule *original = xr_xir_compile_artifact_module(fixture.closed);
    CHECK(original->declarations->slot_count == 1);
    const XrXirFunction *init =
        &original->functions[original->declarations->modules[0].initializer];
    const XrXirSourceDebugView *debug = xr_xir_compile_source_snapshot_debug(fixture.source.snapshot);
    const XrXirSourceView *source = xr_xir_compile_source_snapshot_view(fixture.source.snapshot);
    uint32_t origin = UINT32_MAX;
    CHECK(xr_xir_compile_specialization_origin(fixture.closed,
        original->declarations->modules[0].initializer, &origin) == XR_XIR_OK);
    CHECK(debug && source && origin < debug->function_count);
    XrXirSourceRange expected[4] = {{0}};
    const char *borrowed = source->modules[0].path ? source->modules[0].path : source->modules[0].identity;
    size_t path_length = strlen(borrowed);
    char *path = xr_malloc(path_length + 1);
    CHECK(path);
    memcpy(path, borrowed, path_length + 1);
    unsigned loads = 0, inits = 0;
    for (uint32_t i = 0; i < init->instruction_count; ++i) {
        loads += init->instructions[i].op == XR_XIR_SLOT_LOAD;
        inits += init->instructions[i].op == XR_XIR_SLOT_INIT;
        if (init->instructions[i].op == XR_XIR_SLOT_LOAD) {
            XrXirSourceRange range = debug->functions[origin].points[i].range;
            CHECK(range.line == 2 || range.line == 3);
            expected[range.line] = range;
        }
    }
    CHECK(loads == 2 && inits == 1);
    XrLirLowerRequest request = {fixture.closed, fixture.source.snapshot, false};
    XrLirLowered output = {0};
    OK(xr_lir_lower_program_checked(&request, &output, NULL));
    CHECK(original->declarations->slot_count == 1);
    source_fixture_free(&fixture);
    unsigned found[4] = {0};
    uint32_t values[4] = {0};
    for (uint32_t f = 0; f < xr_lir_function_count(output.module); ++f) {
        XrLirFunction function;
        XrLirFunctionView view;
        OK(xr_lir_function_at(output.module, f, &function));
        OK(xr_lir_function_view(output.module, function, &view));
        for (uint32_t b = 0; b < view.block_count; ++b) {
            XrLirBlock block;
            XrLirBlockView bv;
            OK(xr_lir_block_at(output.module, function, b, &block));
            OK(xr_lir_block_view(output.module, block, &bv));
            for (uint32_t i = 0; i < bv.instruction_count; ++i) {
                XrLirInstructionView op;
                OK(xr_lir_instruction_view(output.module, block, i, &op));
                if (op.op != XR_LIR_CONST || op.bits != 7 ||
                    op.source.line < 2 || op.source.line > 3) continue;
                CHECK(op.type == XR_LIR_I64 && op.source.path && op.source.column > 0);
                XrXirSourceRange range = expected[op.source.line];
                CHECK(op.source.path_length == path_length && !memcmp(op.source.path, path, path_length));
                CHECK(op.source.column == (uint32_t)range.column);
                CHECK(op.source.end_line == (uint32_t)(range.end_line ? range.end_line : range.line));
                CHECK(op.source.end_column == (uint32_t)(range.end_line ? range.end_column : range.column));
                ++found[op.source.line];
                values[op.source.line] = op.result;
            }
        }
    }
    CHECK(found[2] == 1 && found[3] == 1 && values[2] != values[3]);
    xr_free(path);
    xr_lir_lowered_free(&output);
    output = lower_text("const n=0\nprint(n)\n");
    xr_lir_lowered_free(&output);
    rejected("const n=1+2\nprint(n)\n", false, "direct same-block i64 literal");
    rejected("fn value()->i64{return 7}\nconst n=value()\nprint(n)\n", false,
             "direct same-block i64 literal");
    rejected("const s=\"managed\"\nprint(7)\n", false, "module slots");
    static const char *constants[] = {
        "const n=7\nconst m=8\nprint(n+m)\n",
        "const n=7\nfn reader()->i64{return n}\nprint(reader())\n",
        "const n=7\nfn unused()->i64{return n}\nprint(1)\n"
    };
    for (size_t i=0; i<sizeof(constants)/sizeof(constants[0]); ++i) {
        output = lower_text(constants[i]); xr_lir_lowered_free(&output);
    }
    rejected("print(reader())\nconst n=7\nfn reader()->i64{return n}\n", false,
             "before complete publication");
    rejected("const n=7\nif(true){print(n)}\n", false, "slot read outside");
}
static void module_cells_source(const char *directory) {
    char *text = read_file(directory,
        "../../diff/cases/semantics/default_params/default_evaluated_per_call", "xr");
    SourceFixture fixture = source_fixture(text);
    xr_free(text);
    const XrXirModule *source = xr_xir_compile_artifact_module(fixture.closed);
    const XrXirDeclarations *d = source->declarations;
    CHECK(d->module_count == 1 && d->slot_count == 1 && d->slots[0].module == 0 &&
        d->slots[0].mutable && xr_xir_type_is_cell(source->types,d->slots[0].type) &&
        xr_xir_cell_element(source->types,d->slots[0].type) == XR_XIR_I64);
    CHECK(d->functions[d->modules[0].initializer].module == 0);
    XrLirLowerRequest request = {fixture.closed,fixture.source.snapshot,false};
    XrLirLowered out = {0};
    OK(xr_lir_lower_program_checked(&request,&out,NULL));
    source_fixture_free(&fixture);
    OK(xr_lir_verify(out.module,NULL));
    XrLirFunctionView root;
    OK(xr_lir_function_view(out.module,out.entry,&root));
    CHECK(root.result == XR_LIR_I64 && root.parameter_count == 0);
    XrLirBlock root_block; XrLirInstructionView root_call;
    OK(xr_lir_block_at(out.module,out.entry,0,&root_block));
    OK(xr_lir_instruction_view(out.module,root_block,0,&root_call));
    CHECK(root_call.op == XR_LIR_CALL && root_call.type == XR_LIR_I64);
    uint32_t storage_owner = root_call.callee;
    uint32_t cells=0, reads=0, writes=0;
    for (uint32_t f=0; f<xr_lir_function_count(out.module); ++f) {
        XrLirFunction function; XrLirFunctionView view;
        OK(xr_lir_function_at(out.module,f,&function));
        OK(xr_lir_function_view(out.module,function,&view));
        for (uint32_t b=0; b<view.block_count; ++b) {
            XrLirBlock block; XrLirBlockView body;
            OK(xr_lir_block_at(out.module,function,b,&block));
            OK(xr_lir_block_view(out.module,block,&body));
            for (uint32_t i=0; i<body.instruction_count; ++i) {
                XrLirInstructionView op;
                OK(xr_lir_instruction_view(out.module,block,i,&op));
                if (op.op == XR_LIR_CELL_NEW) {
                    CHECK(function.id == storage_owner && b == 0 && op.type == XR_LIR_I64_CELL);
                    ++cells;
                }
                reads += op.op == XR_LIR_CELL_READ;
                writes += op.op == XR_LIR_CELL_WRITE;
            }
        }
    }
    CHECK(cells == 1 && reads >= 2 && writes == 1);
    xr_lir_lowered_free(&out);
    rejected("fn read()->i64{return n}\nvar first=read()\nvar n=7\nprint(first)\n",
        false,"module initialization call before complete publication");
    rejected("if (true) {print(1)}\nvar n=7\nprint(n)\n",false,
        "module Cell publication outside initializer entry");
    rejected("var n=7\nfn bump(v:ref i64)->i64{v=v+1\nreturn v}\nprint(bump(ref n))\n",
        false,"function parameter type");
    rejected("var n=7\nfn outer()->i64{const f=fn()->i64{return n}\nreturn f()}\nprint(outer())\n",
        false,"FUNCTION_REF");
    fixture = source_fixture("var s=\"owned\"\nprint(7)\n");
    request = (XrLirLowerRequest){fixture.closed,fixture.source.snapshot,false};
    OK(xr_lir_lower_program_checked(&request,&out,NULL));
    source_fixture_free(&fixture);
    OK(xr_lir_verify(out.module,NULL));
    xr_lir_lowered_free(&out);
    puts("actual mutable module Cells retain identity, ordered publication and scoped plain calls");
}

static void local_boundaries(void) {
    static const char *texts[] = {
        "fn selected(x:i64)->i64{return x+1}\nprint(selected(7))\n",
        "fn selected(x:i64)->i64{print(x)\nreturn x}\nprint(selected(7))\n",
        ("fn leaf(x:i64)->i64{print(x)\nreturn x}\n"
         "fn selected(x:i64)->i64{return leaf(x)}\nprint(selected(7))\n"),
        "const n=7\nfn selected(x:i64)->i64{return x+1}\nprint(selected(n))\n",
        "fn selected(x:i64)->i64{return x+1}\nprint(7)\n",
    };
    for (size_t t = 0; t < sizeof(texts) / sizeof(texts[0]); ++t) {
        SourceFixture fixture = source_fixture(texts[t]);
        const XrXirModule *source = xr_xir_compile_artifact_module(fixture.closed);
        uint32_t selected = UINT32_MAX;
        for (uint32_t f = 0; f < source->function_count; ++f)
            if (source->functions[f].name_length == 8 &&
                !memcmp(source->functions[f].name, "selected", 8)) selected = f;
        CHECK(selected != UINT32_MAX);
        XrLirLowerRequest request = {fixture.closed, fixture.source.snapshot, false};
        XrLirLowered output = {0};
        XrLirLowerFailure failure = {0};
        OK(xr_lir_lower_program_checked(&request, &output, NULL));
        xr_lir_lowered_free(&output);
        XrLirStatus status = xr_lir_lower_local_checked(&request, selected, &output, &failure);
        source_fixture_free(&fixture);
        if (!t) CHECK(status == XR_LIR_OK && output.module && output.entry.id);
        else {
            CHECK(status == XR_LIR_UNSUPPORTED && !output.module && !output.entry.id);
            if (!failure.owned_path || !failure.diagnostic.source.line)
                fprintf(stderr, "local refusal %zu: %s\n", t, failure.diagnostic.message);
            CHECK(failure.owned_path && failure.diagnostic.source.line);
        }
        xr_lir_lowered_free(&output);
        xr_lir_lower_failure_free(&failure);
    }
}
static void imported_time(const char *programs) {
    char stdlib[2048];
    int count = snprintf(stdlib, sizeof(stdlib), "%s/../../../stdlib", programs);
    CHECK(count > 0 && (size_t)count < sizeof(stdlib));
    SourceFixture fixture = source_fixture_stdlib("import time\nprint(1)\n", stdlib);
    const XrXirModule *source = xr_xir_compile_artifact_module(fixture.closed);
    CHECK(source->declarations->module_count == 2 && source->declarations->slot_count == 3);
    XrLirLowerRequest request = {fixture.closed, fixture.source.snapshot, false};
    XrLirLowered output = {0};
    OK(xr_lir_lower_program_checked(&request, &output, NULL));
    source_fixture_free(&fixture);
    OK(xr_lir_verify(output.module, NULL));
    XrLirBlock block;
    OK(xr_lir_block_at(output.module, output.entry, 0, &block));
    uint32_t selected[3];
    for (uint32_t i=0; i<3; ++i) {
        XrLirInstructionView instruction;
        OK(xr_lir_instruction_view(output.module, block, i, &instruction));
        CHECK(instruction.op == XR_LIR_CALL);
        selected[i] = instruction.callee;
        for (uint32_t j=0; j<i; ++j) CHECK(selected[j] != selected[i]);
    }
    xr_lir_lowered_free(&output);
}
static void clock_sources(const char *programs) {
    char stdlib[2048];
    int count=snprintf(stdlib, sizeof(stdlib), "%s/../../../stdlib", programs);
    CHECK(count > 0 && (size_t)count < sizeof(stdlib));
    SourceFixture fixture=source_fixture_stdlib("import time\nprint(time.nanos())\n"
        "print(time.monotonic())\nprint(time.now())\nprint(time.clock())\n", stdlib);
    XrLirLowered output={0};
    XrLirLowerRequest request={fixture.closed, fixture.source.snapshot, false};
    OK(xr_lir_lower_program_checked(&request, &output, NULL)); source_fixture_free(&fixture);
    unsigned clocks=0;
    for (uint32_t i=0; i<xr_lir_function_count(output.module); ++i) {
        XrLirFunction f; XrLirFunctionView v;
        OK(xr_lir_function_at(output.module, i, &f)); OK(xr_lir_function_view(output.module, f, &v));
        if (v.extern_kind >= XR_LIR_EXTERN_CLOCK_REALTIME) {
            CHECK(v.kind == XR_LIR_FUNCTION_EXTERNAL && v.result == XR_LIR_I64 && !v.parameter_count);
            clocks |= 1u << (v.extern_kind-XR_LIR_EXTERN_CLOCK_REALTIME);
        }
    }
    CHECK(clocks == 7); xr_lir_lowered_free(&output);
    rejected("const divisor=0\nfn f(x:i64)->i64{return x/divisor}\nprint(f(1))\n",false,"DIV_INT");
    rejected("print(f(1))\nconst divisor=2\nfn f(x:i64)->i64{return x/divisor}\n",false,"before complete publication");
}
static void offset_sources(void) {
    SourceFixture fixture=source_fixture("import time\nprint(time.localOffsetAt(1700000000))\nprint(time.localOffset())\n");
    XrLirLowered output={0};XrLirLowerRequest request={fixture.closed,fixture.source.snapshot,false};
    OK(xr_lir_lower_program_checked(&request,&output,NULL));source_fixture_free(&fixture);
    unsigned offsets=0;
    for(uint32_t i=0;i<xr_lir_function_count(output.module);++i) {
        XrLirFunction f;XrLirFunctionView v;
        OK(xr_lir_function_at(output.module,i,&f));OK(xr_lir_function_view(output.module,f,&v));
        if(v.extern_kind==XR_LIR_EXTERN_UTC_OFFSET_AT) {
            CHECK(v.kind==XR_LIR_FUNCTION_EXTERNAL && v.result==XR_LIR_I64 && v.parameter_count==1 && v.parameter_types[0]==XR_LIR_I64);
            ++offsets;
        }
    }
    CHECK(offsets==1);xr_lir_lowered_free(&output);
}
int main(int argc, char **argv) {
    CHECK(argc == 3);
    offset_sources();
    imported_time(argv[1]);
    clock_sources(argv[1]);
    supported_control();
    floating_negation_source();
    swapped_backedge();
    fixed_division(argv[1]);
    unsupported();
    argument_validation();
    literal_sources();
    module_cells_source(argv[1]);
    local_boundaries();
    golden(argv[1], argv[2], "source-fib");
    golden(argv[1], argv[2], "source-sum");
    puts("source-backed scalar LIR lowering passed");
    return 0;
}
