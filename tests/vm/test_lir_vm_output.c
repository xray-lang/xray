/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_vm_output.c - Real VM leaf failures returned to a surviving host
 *
 * KEY CONCEPT:
 *   Source and LIR are retired before invocation. Failure results own complete
 *   locations; host callbacks only exercise the synchronous nesting boundary.
 */
#include "../lir/lir_source_fixture.h"
#include "lir/xlir_builder.h"
#include "lir/xlir_lower.h"
#include "vm/register/xrbc_lir.h"
#include "lir_vm_result_report.h"
#include <fcntl.h>
#if defined(XR_OS_WINDOWS)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

static XrRbcProgram *from_source(const char *text) {
    SourceFixture source = source_fixture(text);
    XrLirLowered lowered = {0};
    XrLirLowerFailure failure = {0};
    XrLirLowerRequest request = {source.closed, source.source.snapshot, false};
    OK(xr_lir_lower_program_checked(&request, &lowered, &failure));
    source_fixture_free(&source);
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(lowered.module, lowered.entry, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_lowered_free(&lowered);
    xr_lir_lower_failure_free(&failure);
    return program;
}

static XrRbcProgram *from_lir(const char *path, bool known, bool point, uint32_t panic_code, bool string_output) {
    bool arithmetic = panic_code != 0;
    XrLirModule *module = NULL;
    OK(xr_lir_module_create(NULL, &module));
    XrLirFunction root = {0}, print = {0};
    XrLirFunctionSpec spec = {.name = "root", .name_length = 4,
        .result = arithmetic ? XR_LIR_I64 : XR_LIR_UNIT};
    OK(xr_lir_declare_function(module, &spec, &root));
    OK(xr_lir_define_function(module, root));
    XrLirType type = XR_LIR_I64, string_types[] = {XR_LIR_PTR, XR_LIR_U64};
    if (!arithmetic) {
        spec = (XrLirFunctionSpec) {.name = "xrt_print_i64", .name_length = 13,
            .parameters = &type, .parameter_count = 1, .result = XR_LIR_UNIT,
            .kind = XR_LIR_FUNCTION_EXTERNAL, .extern_kind = XR_LIR_EXTERN_PRINT_I64};
        if (string_output) spec = (XrLirFunctionSpec){.name="xrt_print_string", .name_length=16,
            .parameters=string_types, .parameter_count=2, .result=XR_LIR_UNIT,
            .kind=XR_LIR_FUNCTION_EXTERNAL, .extern_kind=XR_LIR_EXTERN_PRINT_STRING};
        OK(xr_lir_declare_function(module, &spec, &print));
    }
    XrLirBlock block = {0};
    OK(xr_lir_add_block(module, root, NULL, 0, &block));
    OK(xr_lir_set_entry(module, root, block));
    XrLirValue value = {0}, result = {0};
    XrLirSource source = known ? (XrLirSource) {path, strlen(path), 3, 2, 3, 2} : (XrLirSource) {0};
    XrLirInstruction instruction = {.op = XR_LIR_CONST, .type = XR_LIR_I64,
        .bits = 42, .source = source};
    OK(xr_lir_emit_value(module, block, &instruction, &value));
    // These failures unwind with a real live cell bank, not only scalar slots.
    XrLirValue cell;
    XrLirInstruction cell_instruction = {.op = XR_LIR_CELL_NEW, .type = XR_LIR_I64_CELL,
        .operands = &value, .operand_count = 1};
    OK(xr_lir_emit_value(module, block, &cell_instruction, &cell));
    cell_instruction = (XrLirInstruction) {.op = XR_LIR_CELL_READ, .type = XR_LIR_I64,
        .operands = &cell, .operand_count = 1};
    OK(xr_lir_emit_value(module, block, &cell_instruction, &value));
    if (arithmetic) {
        XrLirValue zero = {0};
        instruction.bits = 0;
        OK(xr_lir_emit_value(module, block, &instruction, &zero));
        XrLirValue arguments[] = {value, zero};
        if (known) source = (XrLirSource) {path, strlen(path), 7, 9, 8, 11};
        instruction = (XrLirInstruction) {.op = panic_code == 421 ? XR_LIR_REM : XR_LIR_DIV,
            .type = XR_LIR_I64, .operands = arguments, .operand_count = 2, .source = source};
        OK(xr_lir_emit_value(module, block, &instruction, &result));
        instruction = (XrLirInstruction) {.op = XR_LIR_RET, .type = XR_LIR_UNIT,
            .operands = &result, .operand_count = 1};
    } else {
        XrLirValue arguments[2] = {value, {0}};
        XrLirLiteral literal = {0};
        if (string_output) {
            const unsigned char bytes[] = {'A', 0, 0xc3, 0xa9};
            OK(xr_lir_add_literal(module, bytes, sizeof(bytes), &literal));
            OK(xr_lir_literal_address(module, block, literal, &arguments[0]));
            XrLirInstruction length = {.op=XR_LIR_CONST, .type=XR_LIR_U64, .bits=sizeof(bytes)};
            OK(xr_lir_emit_value(module, block, &length, &arguments[1]));
        }
        if (known) source = (XrLirSource) {path, strlen(path), 7, 9, point ? 7u : 8u, point ? 9u : 11u};
        instruction = (XrLirInstruction) {.op = XR_LIR_CALL, .type = XR_LIR_UNIT,
            .callee = print, .operands = arguments, .operand_count = string_output ? 2u : 1u, .source = source};
        OK(xr_lir_emit_effect(module, block, &instruction));
        if (string_output) OK(xr_lir_literal_address(module, block, literal, &arguments[0]));
        if (known) instruction.source.line = instruction.source.end_line = 8;
        OK(xr_lir_emit_effect(module, block, &instruction)); // Must never follow a failed first print.
        instruction = (XrLirInstruction) {.op = XR_LIR_RET, .type = XR_LIR_UNIT};
    }
    OK(xr_lir_set_terminator(module, block, &instruction));
    OK(xr_lir_module_finish(module, NULL));
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(module);
    return program;
}

static XrRbcExecution *prepare(XrRbcProgram *program) {
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK && execution);
    return execution;
}

static XrRbcOutcome *invoke(XrRbcProgram *program) {
    XrRbcExecution *execution = prepare(program);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    CHECK(outcome && !xr_rbc_execution_run(execution));
    xr_rbc_execution_free(&execution);
    CHECK(!execution);
    return outcome;
}

static void disable_output(bool buffered) {
#if defined(XR_OS_WINDOWS)
    if (buffered) {
        int input = _open("NUL", _O_RDONLY | _O_BINARY);
        CHECK(input >= 0 && _dup2(input, _fileno(stdout)) == 0 && _close(input) == 0);
        CHECK(SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE) _get_osfhandle(_fileno(stdout))));
    } else CHECK(SetStdHandle(STD_OUTPUT_HANDLE, INVALID_HANDLE_VALUE));
#else
    if (buffered) {
        int input = open("/dev/null", O_RDONLY);
        CHECK(input >= 0 && dup2(input, STDOUT_FILENO) >= 0 && close(input) == 0);
    } else CHECK(close(STDOUT_FILENO) == 0);
#endif
}

static void next_execution(void) {
    // Restore a usable host-owned output before testing another real guest leaf.
    // Pending CRT bytes, if any, can only reach the null sink after restoration.
#if defined(XR_OS_WINDOWS)
    int sink = _open("NUL", _O_WRONLY | _O_BINARY);
    CHECK(sink >= 0 && _dup2(sink, _fileno(stdout)) == 0 && _close(sink) == 0);
    CHECK(SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE) _get_osfhandle(_fileno(stdout))));
#else
    int sink = open("/dev/null", O_WRONLY);
    CHECK(sink >= 0);
    if (sink != STDOUT_FILENO)
        CHECK(dup2(sink, STDOUT_FILENO) >= 0 && close(sink) == 0);
#endif
    clearerr(stdout);
    CHECK(fflush(stdout) == 0);
    XrRbcProgram *next = from_source("print(1234)\n");
    XrRbcOutcome *outcome = invoke(next);
    CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK);
    xr_rbc_program_destroy(next);
    uint64_t bits = UINT64_MAX;
    CHECK(xr_rbc_outcome_bits(outcome, &bits) && !bits);
    xr_rbc_outcome_free(&outcome);
}

static void check_failure(XrRbcOutcome *outcome, const char *path, bool known, bool point,
    bool buffered, bool pipe) {
    CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OUTPUT_FAILURE);
    XrtArithmeticPanic panic = {88, 77};
    const XrtLocationOwner *wrong = NULL;
    CHECK(!xr_rbc_outcome_arithmetic_panic(outcome, &panic, &wrong));
    CHECK(panic.code == 88 && panic.source_id == 77 && !wrong);
    uint64_t bits = 991;
    CHECK(!xr_rbc_outcome_bits(outcome, &bits) && bits == 991);
    XrtOutputFailure facts = {.source_id = 99};
    const XrtLocationOwner *owner = NULL;
    CHECK(!xr_rbc_outcome_output_failure(outcome, NULL, &owner) && !owner);
    CHECK(!xr_rbc_outcome_output_failure(outcome, &facts, NULL) && facts.source_id == 99);
    CHECK(xr_rbc_outcome_output_failure(outcome, &facts, &owner));
    CHECK(facts.io.failed && facts.io.code && facts.io.domain != XRT_IO_NONE);
    CHECK(facts.io.broken_pipe == pipe);
    CHECK(facts.operation == (buffered ? XRT_OUTPUT_FLUSH : XRT_OUTPUT_WRITE));
    if (known) {
        const XrtSourceLocation *location = xrt_locations_get(owner, facts.source_id);
        CHECK(location && location->path_length == strlen(path));
        CHECK(!memcmp(location->path, path, strlen(path)));
        CHECK(location->line == 7 && location->column == 9);
        CHECK(location->end_line == (point ? 7u : 8u) && location->end_column == (point ? 9u : 11u));
    } else CHECK(!facts.source_id && !xrt_locations_count(owner));
}

static XrtSourceLocation source_print_location(XrRbcProgram *program) {
    XrtLocationOwner *owner = xr_rbc_program_locations(program);
    for (uint32_t id = 1; id <= xrt_locations_count(owner); ++id) {
        const XrtSourceLocation *location = xrt_locations_get(owner, id);
        if (location->line != 2 || location->column != 1) continue;
        XrtSourceLocation copy = *location;
        char *path = xr_malloc(copy.path_length);
        CHECK(path);
        memcpy(path, copy.path, copy.path_length);
        copy.path = path;
        return copy;
    }
    CHECK(false);
    return (XrtSourceLocation) {0};
}

static void check_source_failure(XrRbcOutcome *outcome, const XrtSourceLocation *expected) {
    XrtOutputFailure failure = {0};
    const XrtLocationOwner *owner = NULL;
    CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OUTPUT_FAILURE);
    CHECK(xr_rbc_outcome_output_failure(outcome, &failure, &owner));
    CHECK(failure.io.failed && !failure.io.broken_pipe && failure.operation == XRT_OUTPUT_WRITE);
    const XrtSourceLocation *actual = xrt_locations_get(owner, failure.source_id);
    CHECK(actual && actual->path_length == expected->path_length);
    CHECK(!memcmp(actual->path, expected->path, expected->path_length));
    CHECK(actual->line == 2 && actual->column == 1 && actual->end_line == expected->end_line &&
        actual->end_column == expected->end_column);
}

static void check_arithmetic_panic(XrRbcOutcome *outcome, uint32_t code, const char *path) {
    XrtArithmeticPanic facts;
    const XrtLocationOwner *owner = NULL;
    CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_ARITHMETIC_PANIC);
    CHECK(xr_rbc_outcome_arithmetic_panic(outcome, &facts, &owner));
    CHECK(facts.code == code && facts.source_id == 3);
    const XrtSourceLocation *source = xrt_locations_get(owner, facts.source_id);
    CHECK(source && source->path_length == strlen(path));
    CHECK(!memcmp(source->path, path, source->path_length));
    CHECK(source->line == 7 && source->column == 9 && source->end_line == 8 && source->end_column == 11);
}

static int report_panic(const char *path, uint32_t code, bool bad_sink, bool program_first) {
    XrRbcProgram *program = from_lir(path, true, false, code, false);
    XrRbcOutcome *outcome = invoke(program);
    if (program_first) xr_rbc_program_destroy(program);
    check_arithmetic_panic(outcome, code, path);
    if (bad_sink) {
#if defined(XR_OS_WINDOWS)
        CHECK(SetStdHandle(STD_ERROR_HANDLE, INVALID_HANDLE_VALUE));
#else
        CHECK(close(STDERR_FILENO) == 0);
#endif
    }
    int result = lir_vm_result_report(outcome, xr_rbc_outcome_status(outcome));
    xr_rbc_outcome_free(&outcome);
    if (!program_first) xr_rbc_program_destroy(program);
    return result;
}

typedef struct Nested {
    XrRbcExecution *execution;
    XrRbcOutcome *outcome;
    unsigned after_inner, after_outer;
    bool outer_panic;
} Nested;

static void nested_root(void *data) {
    Nested *nested = data;
    nested->outcome = xr_rbc_execution_run(nested->execution);
    ++nested->after_inner;
    if (nested->outer_panic) xrt_arithmetic_panic(421, 1);
    xrt_print_i64(555, 1);
    ++nested->after_outer;
}

static void nested_execution(const char *path, bool string_output) {
    XrtSourceLocation source = {"outer.xr", 8, 33, 2, 0, 0};
    XrtLocationOwner *owner = NULL;
    CHECK(xrt_locations_create(&source, 1, &owner) == XRT_LOCATION_OK);
    disable_output(false);
    for (unsigned kind = 0; kind < 6; ++kind) {
        unsigned inner = kind % 3;
        XrRbcProgram *program = inner == 0 ? from_source("") :
            from_lir(path, true, false, inner == 1 ? 420u : 0u, string_output);
        Nested *nested = xr_calloc(1, sizeof(*nested));
        XrtExecutionFailure *failure = xr_calloc(1, sizeof(*failure));
        CHECK(nested && failure);
        nested->outer_panic = kind >= 3;
        nested->execution = prepare(program);
        CHECK(xrt_run_embedded(owner, failure, nested_root, nested) == XRT_EXECUTION_FAILURE);
        CHECK(nested->after_inner == 1 && !nested->after_outer && xrt_locations_get(owner, 1)->line == 33);
        if (nested->outer_panic) {
            CHECK(failure->kind == XRT_FAILURE_ARITHMETIC_PANIC);
            CHECK(failure->detail.panic.code == 421 && failure->detail.panic.source_id == 1);
        } else {
            CHECK(failure->kind == XRT_FAILURE_OUTPUT);
            CHECK(failure->detail.output.source_id == 1 && failure->detail.output.io.failed);
        }
        xr_rbc_execution_free(&nested->execution);
        xr_rbc_program_destroy(program);
        CHECK(nested->outcome && xr_rbc_outcome_status(nested->outcome) ==
            (inner == 0 ? XR_RBC_OK : inner == 1 ? XR_RBC_ARITHMETIC_PANIC : XR_RBC_OUTPUT_FAILURE));
        if (inner == 1) check_arithmetic_panic(nested->outcome, 420, path);
        if (inner == 2) check_failure(nested->outcome, path, true, false, false, false);
        xr_rbc_outcome_free(&nested->outcome);
        xr_free(failure);
        xr_free(nested);
    }
    xrt_locations_release(owner);
    next_execution();
}

int main(int argc, char **argv) {
    CHECK(argc >= 2);
    const char *mode = argv[1];
    if (!strcmp(mode, "literal-repeat") || !strcmp(mode, "string-repeat")) {
        XrRbcProgram *program = from_source(!strcmp(mode,"string-repeat") ?
            "print(\"A\\0\xc3\xa9\")\nprint(7)\nprint(\"\")\n" : "const n=7\nprint(n)\nprint(99)\n");
        for (unsigned i = 0; i < 2; ++i) {
            XrRbcOutcome *outcome = invoke(program);
            uint64_t bits = UINT64_MAX;
            CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK);
            CHECK(xr_rbc_outcome_bits(outcome, &bits) && !bits);
            xr_rbc_outcome_free(&outcome);
        }
        xr_rbc_program_destroy(program);
        return 0;
    }
    if (!strcmp(mode, "offset-source")) {
        XrRbcProgram *program = from_source("import time\n"
            "fn offsets(){\n"
            "print(time.localOffsetAt(0))\n"
            "print(time.localOffsetAt(1700000000))\n"
            "print(time.localOffsetAt(1719792000))\n"
            "const before=time.now()/1000\n"
            "const actual=time.localOffset()\n"
            "const after=time.now()/1000\n"
            "print(before)\n"
            "print(actual)\n"
            "print(after)\n"
            "}\n"
            "offsets()\n");
        for (unsigned i=0;i<2;++i) {
            XrRbcOutcome *outcome=invoke(program);CHECK(xr_rbc_outcome_status(outcome)==XR_RBC_OK);
            xr_rbc_outcome_free(&outcome);
        }
        xr_rbc_program_destroy(program);return 0;
    }
    if (!strcmp(mode, "clock-source")) {
        XrRbcProgram *program = from_source("import time\nfn check()->i64{\n"
            "const a=time.nanos()\nconst b=time.nanos()\n"
            "if(a>0 && b>=a && time.clock()>=0 && time.now()>0){return 1}\n"
            "return 0}\nprint(check())\n");
        for (unsigned i=0; i<2; ++i) {
            XrRbcOutcome *outcome = invoke(program);
            CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK);
            xr_rbc_outcome_free(&outcome);
        }
        xr_rbc_program_destroy(program);
        return 0;
    }
    if (!strcmp(mode, "source")) {
        XrRbcProgram *program = from_source(
            "fn sum(a:i64,b:i64,c:i64,d:i64)->i64{return a+b+c+d}\n"
            "fn work(n:i64)->i64{var a=1\nvar b=2\nvar i=0\n"
            "while(i<n){const t=a\na=b\nb=t\ni=i+1}\n"
            "print(sum(a,b,3,4))\nreturn a*10+b}\nprint(work(3))\n");
        XrRbcOutcome *outcome = invoke(program);
        xr_rbc_program_destroy(program);
        CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK);
        xr_rbc_outcome_free(&outcome);
        return 0;
    }
    char path[401];
    memset(path, 'p', sizeof(path));
    memcpy(path + sizeof(path) - 9, "\xc3\xa9\n\"\\.xr", 8);
    path[sizeof(path) - 1] = 0;
    if (!strcmp(mode, "panic-div") || !strcmp(mode, "panic-rem") || !strcmp(mode, "panic-bad-sink"))
        return report_panic(path, !strcmp(mode, "panic-rem") ? 421u : 420u,
            !strcmp(mode, "panic-bad-sink"), argc == 2 || strcmp(argv[2], "outcome-first") != 0);
    if (!strcmp(mode, "nested") || !strcmp(mode,"nested-string"))
        nested_execution(path,!strcmp(mode,"nested-string"));
    else {
        bool string_output = !strncmp(mode,"string-",7);
        const char *kind = string_output ? mode+7 : mode;
        bool literal = !strcmp(kind, "literal-failure");
        bool source_case = literal || !strcmp(kind, "source-failure");
        bool point = !strcmp(kind, "point"), known = strcmp(kind, "unknown") != 0;
        bool buffered = !strcmp(kind, "flush") || !strcmp(kind, "pipe-flush");
        bool pipe = !strcmp(kind, "pipe") || !strcmp(kind, "pipe-flush");
        XrRbcProgram *program = source_case ?
            from_source(literal ? "const n=7\nprint(n)\nprint(99)\n" : string_output ?
                "fn fail()->i64{\nprint(\"A\\0\xc3\xa9\")\nreturn 1}\nprint(fail())\n" :
                "fn fail()->i64{\nprint(42)\nreturn 1}\nprint(fail())\n") :
            from_lir(path, known, point, false, string_output);
        XrtSourceLocation expected = source_case ? source_print_location(program) : (XrtSourceLocation) {0};
        XrRbcExecution *execution = prepare(program);
        static char buffer[128];
        CHECK(setvbuf(stdout, buffer, _IOFBF, sizeof(buffer)) == 0);
        if (buffered) CHECK(fputs("pending host prefix", stdout) >= 0);
        if (!pipe) disable_output(buffered);
        XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
        xr_rbc_execution_free(&execution);
        bool program_first = argc == 2 || strcmp(argv[2], "outcome-first") != 0;
        if (program_first) xr_rbc_program_destroy(program);
        if (source_case) check_source_failure(outcome, &expected);
        else check_failure(outcome, path, known, point, buffered, pipe);
        xr_free((void *) expected.path);
        xr_rbc_outcome_free(&outcome);
        if (!program_first) xr_rbc_program_destroy(program);
        CHECK(!outcome && !execution);
        next_execution();
    }
    fputs("HOST_ALIVE\n", stderr);
    return 0;
}
