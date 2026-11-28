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
#include "xir/xxir_comptime_pending_internal.h"
#include "xir/xxir_construction_internal.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_type_description.h"
#include "base/xmalloc.h"
#include "lir/xlir_lower.h"
#include "lir/xlir_source.h"
#include "lir/xlir_test_plan.h"
#include "lir/xlir_builder.h"
#include "lir/xlir_internal.h"
#include "lir/xlir_rebuild.h"
#include "lir/xlir_verify.h"
#include "lir/xlir_print.h"
#include "lir/xlir_state.h"
#include "lir/xlir_callable.h"
#include "lir/xlir_value_image.h"
#include "runtime/native/xnative_value_image.h"

typedef struct Text {
    char bytes[65536];
    size_t length;
} Text;

static void source_fixture_rejected(const char *text, const char *message) {
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrXirCompileContext context = {0};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    context.limits = xr_xir_compile_default_limits();
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY, "lir-lowering", NULL};
    XrXirSourceRequest request = {.session = session, .authority = &authority, .context = &context,
        .linkage_kind = XR_XIR_PROGRAM, .target = XR_XIR_HOST_TARGET};
    XrXirSourceText input = {NULL, text, strlen(text)};
    XrXirSourceResult result = {0};
    XrXirSourceDiagnostic diagnostic = {0};
    CHECK(xr_xir_compile_source_check_text(&request, &input, &result, &diagnostic, NULL) == XR_XIR_BAD_TYPE);
    CHECK(!result.checked && !result.snapshot && strstr(diagnostic.message, message));
    xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(session);
    xr_compile_resources_release(context.resources);
}

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
    CHECK(entry.name_length == 4 && !memcmp(entry.name, "main", 4));
    CHECK(entry.parameter_count <= 1);
    bool stateful = entry.parameter_count == 1;
    if (stateful) CHECK(entry.parameter_types[0] == XR_LIR_STATE);
    XrLirBlock block;
    XrLirInstructionView instruction;
    OK(xr_lir_block_at(output.module, output.entry, 0, &block));
    OK(xr_lir_instruction_view(output.module, block, stateful ? 1 : 0, &instruction));
    CHECK(instruction.op == XR_LIR_CALL);
    uint32_t initializer = instruction.callee;
    OK(xr_lir_instruction_view(output.module, block, stateful ? 3 : 1, &instruction));
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
        "struct Pair{x:i64}\nfn answer()->i64{const p=Pair{x:42}\nreturn p.x}\nprint(answer())\n",
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
static void module_i64_groups(void) {
    XrLirLowered output = lower_text("var[a,_,b]=[1,2,3]\nconst[c]=[4]\nprint(a+b+c)\n");
    unsigned groups = 0, assertions = 0;
    for (uint32_t n = 0; n < xr_lir_function_count(output.module); ++n) {
        XrLirFunction function;
        XrLirFunctionView view;
        OK(xr_lir_function_at(output.module, n, &function));
        OK(xr_lir_function_view(output.module, function, &view));
        for (uint32_t b = 0; b < view.block_count; ++b) {
            XrLirBlock block;
            XrLirBlockView body;
            unsigned checked = 0;
            OK(xr_lir_block_at(output.module, function, b, &block));
            OK(xr_lir_block_view(output.module, block, &body));
            for (uint32_t i = 0; i < body.instruction_count; ++i) {
                XrLirInstructionView instruction;
                OK(xr_lir_instruction_view(output.module, block, i, &instruction));
                if (instruction.op == XR_LIR_ASSERT_CONDITION) { ++assertions; ++checked; }
                if (instruction.op != XR_LIR_STATE_GROUP_INITIALIZE) continue;
                CHECK(checked && i);
                --checked;
                XrLirInstructionView tuple;
                OK(xr_lir_instruction_view(output.module, block, i - 1, &tuple));
                CHECK(tuple.op == XR_LIR_RECORD_NEW && tuple.result == instruction.operands[1]);
                CHECK(tuple.operand_count == (instruction.bits ? 1u : 2u));
                CHECK(tuple.source.line == instruction.source.line && tuple.source.column == instruction.source.column);
                ++groups;
            }
        }
    }
    CHECK(groups == 2 && assertions == 2);
    xr_lir_lowered_free(&output);
    output = lower_text("var[a,b]=[1,2]\nconst[c]=[3]\nprint(17)\n");
    XrLirFunctionView entry;
    OK(xr_lir_function_view(output.module, output.entry, &entry));
    CHECK(entry.parameter_count == 1 && entry.parameter_types[0] == XR_LIR_STATE);
    xr_lir_lowered_free(&output);
    rejected("const[a,b]=[true,false]\nprint(a)\n", false, "module group requires I64 slot payloads");
    rejected("var value:i64?=1\nconst(a,b)=(value,value)\nprint(a)\n", false,
        "module group requires I64 slot payloads");
}
static void floating_conversion_source(void) {
    SourceFixture fixture = source_fixture(
        "fn convert(x:f64)->i64{return x as i64}\nprint(convert(1.0))\n");
    const XrXirModule *closed = xr_xir_compile_artifact_module(fixture.closed);
    const XrXirSourceDebugView *debug = xr_xir_compile_source_snapshot_debug(fixture.source.snapshot);
    CHECK(closed && debug);
    XrXirSourceRange expected = {0};
    unsigned source_conversions = 0;
    for (uint32_t f = 0; f < closed->function_count; ++f) {
        const XrXirFunction *function = &closed->functions[f];
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            if (function->instructions[i].op != XR_XIR_CONVERT_NUMBER) continue;
            uint32_t origin = UINT32_MAX;
            CHECK(!source_conversions++);
            CHECK(xr_xir_compile_specialization_origin(fixture.closed, f, &origin) == XR_XIR_OK);
            CHECK(origin < debug->function_count && debug->functions[origin].points &&
                i < debug->functions[origin].instruction_count);
            expected = debug->functions[origin].points[i].range;
        }
    }
    CHECK(source_conversions == 1 && expected.line == 1 && expected.column > 0);
    XrLirLowered output = {0};
    XrLirLowerRequest request = {fixture.closed, fixture.source.snapshot, false};
    OK(xr_lir_lower_program_checked(&request, &output, NULL));
    source_fixture_free(&fixture);
    OK(xr_lir_verify(output.module, NULL));
    unsigned conversions = 0;
    for (uint32_t f = 0; f < xr_lir_function_count(output.module); ++f) {
        XrLirFunction function;
        XrLirFunctionView fv;
        OK(xr_lir_function_at(output.module, f, &function));
        OK(xr_lir_function_view(output.module, function, &fv));
        for (uint32_t b = 0; b < fv.block_count; ++b) {
            XrLirBlock block;
            XrLirBlockView bv;
            OK(xr_lir_block_at(output.module, function, b, &block));
            OK(xr_lir_block_view(output.module, block, &bv));
            for (uint32_t i = 0; i < bv.instruction_count; ++i) {
                XrLirInstructionView instruction;
                OK(xr_lir_instruction_view(output.module, block, i, &instruction));
                if (instruction.op != XR_LIR_FTOI) continue;
                XrLirType input;
                CHECK(!conversions++ && instruction.type == XR_LIR_I64 &&
                    instruction.operand_count == 1);
                CHECK(fv.parameter_count == 1 && fv.parameter_types[0] == XR_LIR_F64 &&
                    fv.result == XR_LIR_I64 && instruction.operands[0] == fv.parameters[0]);
                XrLirValue value = {output.module, function.id, instruction.operands[0]};
                OK(xr_lir_value_type(output.module, value, &input));
                CHECK(input == XR_LIR_F64 && instruction.source.line == (uint32_t) expected.line &&
                    instruction.source.column == (uint32_t) expected.column);
                CHECK(instruction.source.end_line == (uint32_t)
                    (expected.end_line ? expected.end_line : expected.line));
                CHECK(instruction.source.end_column == (uint32_t)
                    (expected.end_line ? expected.end_column : expected.column));
            }
        }
    }
    CHECK(conversions == 1);
    xr_lir_lowered_free(&output);
}

static void unsupported(void) {
    rejected(
        "fn answer()->i64{const values=Set<f64>()\nreturn len(values)}\nprint(answer())\n",
        false, "SET_NEW");
    rejected("class Box<T>{value:T; init(value:T){this.value=value}}\n"
             "const opaque=Box(fn(n:i64)->i64{return n})\n", false, "proven acyclic allocation layout");
    rejected("class Loop{edges:Array<Link>;init(){this.edges=[]}}\nstruct Link{target:Loop?}\n"
             "const root=Loop()\n", false, "proven acyclic allocation layout");
    rejected("class Box{value:i64; init(value:i64){this.value=value}}\n"
             "fn get(b:Box)->i64{return b.value}\n"
             "fn apply(f:fn(Box)->i64, b:Box)->i64{return f(b)}\nprint(apply(get, Box(42)))\n",
             false, "class callable parameter transport");
    rejected("class Box{value:i64; init(value:i64){this.value=value}}\n"
             "fn make()->Box{return Box(42)}\n"
             "fn apply(f:fn()->Box)->Box{return f()}\nprint(apply(make).value)\n",
             false, "class callable result transport");
    rejected("print(1)\n", true, "overflow-checks");
}

static void class_parent_views(const char *programs) {
    const char *names[] = {"class-parent-argument", "class-parent-assignment"};
    for (size_t n = 0; n < sizeof(names) / sizeof(*names); ++n) {
        char *text = read_file(programs, names[n], "xr");
        XrLirLowered output = lower_text(text);
        xr_lir_lowered_free(&output);
        xr_free(text);
    }
}

static void unsupported_terminators(const char *programs) {
    char *text = read_file(programs, "unsupported-cleanup-terminator", "xr");
    XrLirLowered cleanup = lower_text(text);
    xr_lir_lowered_free(&cleanup);
    xr_free(text);

    SourceFixture fixture = source_fixture("fn answer()->i64{return 42}\nprint(answer())\n");
    const XrXirModule *module = xr_xir_compile_artifact_module(fixture.closed);
    XrXirInstruction *terminator = NULL;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *body = &module->functions[f];
        if (body->name_length != 6 || memcmp(body->name, "answer", 6)) continue;
        CHECK(body->block_count == 1 && body->instruction_count);
        terminator = (XrXirInstruction *) &body->instructions[body->instruction_count - 1];
    }
    CHECK(terminator && terminator->op == XR_XIR_RETURN);
    XrXirInstruction saved = *terminator;
    /* A valid body opcode in a block's terminal position must also fail. */
    *terminator = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 42};
    XrLirLowered output = {0};
    XrLirLowerFailure failure = {0};
    XrLirLowerRequest request = {fixture.closed, fixture.source.snapshot, false};
    XrLirStatus status = xr_lir_lower_program_checked(&request, &output, &failure);
    *terminator = saved;
    CHECK(status == XR_LIR_UNSUPPORTED && failure.diagnostic.status == status);
    CHECK(!output.module && !output.entry.id &&
        strstr(failure.diagnostic.message, "block terminator CONST_INT"));
    xr_lir_lower_failure_free(&failure);
    source_fixture_free(&fixture);
}

static void class_layout_sources(void) {
    XrLirLowered output = lower_text(
        "class Box<T>{value:T; init(value:T){this.value=value}}\n"
        "class Pair<A,B>{first:A;second:B;init(a:A,b:B){this.first=a;this.second=b}}\n"
        "var number=Box(42)\nvar word=Box(\"hello\")\nvar pair=Pair(\"year\",2026)\n"
        "print(number.value)\nprint(word.value)\nprint(pair.first)\nprint(pair.second)\n");
    XrLirStateLayout state = {0};
    OK(xr_lir_state_layout_view(output.module, &state));
    CHECK(state.slot_count == 3);
    XrLirValueLayoutKey keys[3] = {{0}};
    for (uint32_t n = 0; n < state.slot_count; ++n) {
        XrLirType row = 0;
        CHECK(xr_lir_class_payload(output.module, state.slots[n].payload, &row));
        CHECK(state.slots[n].size == sizeof(void *) && state.slots[n].alignment == _Alignof(void *));
        XrLirRecordIdentity identity = {0};
        OK(xr_lir_record_identity(output.module, row, &identity));
        CHECK(identity.argument_count == (n == 2 ? 2u : 1u));
        CHECK(identity.arguments[0].type == (XrLirType)(n ? XR_LIR_STRING : XR_LIR_I64));
        if (n == 2) CHECK(identity.arguments[1].type == XR_LIR_I64);
        keys[n].type = state.slots[n].payload;
    }
    CHECK(keys[0].type != keys[1].type && keys[1].type != keys[2].type);

    uint32_t constructs = 0, reads = 0;
    for (uint32_t f = 0; f < xr_lir_function_count(output.module); ++f) {
        XrLirFunction function;
        XrLirFunctionView view;
        OK(xr_lir_function_at(output.module, f, &function));
        OK(xr_lir_function_view(output.module, function, &view));
        for (uint32_t b = 0; b < view.block_count; ++b) {
            XrLirBlock block;
            XrLirBlockView body;
            OK(xr_lir_block_at(output.module, function, b, &block));
            OK(xr_lir_block_view(output.module, block, &body));
            for (uint32_t i = 0; i < body.instruction_count; ++i) {
                XrLirInstructionView op;
                OK(xr_lir_instruction_view(output.module, block, i, &op));
                constructs += op.op == XR_LIR_CLASS_NEW;
                reads += op.op == XR_LIR_CLASS_FIELD;
            }
        }
    }
    CHECK(constructs == 3 && reads == 4);

    XrtBillingOwner *billing = NULL;
    CHECK(xrt_billing_owner_new(NULL, SIZE_MAX, &billing) == XRT_MANAGED_OK);
    XrLirValueLayouts *layouts = NULL;
    CHECK(xr_lir_value_layouts_create(output.module, keys, 3, billing, &layouts) == XR_LIR_VALUE_LAYOUT_OK);
    XrLirValueImage *image = NULL;
    CHECK(xr_lir_value_image_create(layouts, billing, &image) == XR_LIR_VALUE_LAYOUT_OK);
    uint32_t ids[3] = {0};
    for (size_t i = 0; i < 3; ++i) {
        const XrtValuePlan *plan = NULL;
        CHECK(xr_lir_value_layouts_lookup(layouts, keys[i], &plan, &ids[i]) == XR_LIR_VALUE_LAYOUT_OK);
    }
    CHECK(ids[0] && ids[0] != ids[1] && ids[1] != ids[2]);
    xr_lir_value_layouts_destroy(layouts);
    xr_lir_lowered_free(&output);
    XrtValueImagePlans *bound = NULL;
    CHECK(xrt_value_image_bind(billing, xr_lir_value_image_view(image), (XrtManagedLease){0}, &bound) == XRT_MANAGED_OK);
    xr_lir_value_image_destroy(image);
    for (size_t i = 0; i < 3; ++i) {
        const XrtValuePlan *plan = NULL;
        XrtValuePlanSpec owner, payload;
        CHECK(xrt_value_image_get(bound, ids[i], &plan) == XRT_MANAGED_OK);
        CHECK(xrt_value_plan_view(plan, &owner) == XRT_MANAGED_OK && owner.kind == XRT_VALUE_CLASS &&
            owner.size == sizeof(XrtManagedHeader *) && owner.owned_count == 1 && owner.owned_offsets[0] == 0);
        CHECK(xrt_value_plan_view(owner.element, &payload) == XRT_MANAGED_OK && payload.kind == XRT_VALUE_RECORD);
        CHECK(payload.field_count == (i == 2 ? 2u : 1u) && payload.owned_count == (i ? 1u : 0u));
        if (i) CHECK(payload.owned_offsets[0] == 0);
    }
    xrt_value_image_drop(&bound);
    xrt_billing_owner_drop(billing);
}
static void inherited_class_layout_sources(void) {
    const char *text =
        "class Animal{name:string;init(name:string){this.name=name}}\n"
        "class Dog extends Animal{age:i64;init(name:string,age:i64){super(name);this.age=age}}\n"
        "var animal=Animal(\"Cat\")\nvar dog=Dog(\"Rex\",7)\n"
        "print(animal.name)\nprint(dog.name)\nprint(dog.age)\n";
    SourceFixture fixture = source_fixture(text);
    const XrXirModule *closed = xr_xir_compile_artifact_module(fixture.closed);
    const XrXirCompileContext *context = xr_xir_compile_artifact_context(fixture.closed);
    uint32_t creates = 0, derived = 0;
    for (uint32_t f = 0; f < closed->function_count; ++f) {
        const XrXirFunction *function = &closed->functions[f];
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            const XrXirInstruction *op = &function->instructions[i];
            if (op->op != XR_XIR_CLASS_NEW) continue;
            XrXirTypeDescription type;
            CHECK(xr_xir_compile_type_description(context, closed->types, op->type, &type) == XR_XIR_OK);
            uint32_t parent = type.definition ? type.definition->parent : type.identity->parent;
            CHECK(op->args[1] == (parent ? 2u : 1u));
            XrXirFieldDescription name;
            CHECK(xr_xir_compile_description_field(context, &type, 0, &name) == XR_XIR_OK);
            CHECK(name.type == XR_XIR_STRING && name.name.length == 4 && !memcmp(name.name.bytes, "name", 4));
            if (parent) {
                XrXirFieldDescription age;
                CHECK(xr_xir_compile_description_field(context, &type, 1, &age) == XR_XIR_OK);
                CHECK(age.type == XR_XIR_I64 && age.name.length == 3 && !memcmp(age.name.bytes, "age", 3));
                ++derived;
            }
            ++creates;
        }
    }
    CHECK(creates == 2 && derived == 1);
    XrLirLowerRequest request = {fixture.closed, fixture.source.snapshot, false};
    XrLirLowered output = {0};
    OK(xr_lir_lower_program_checked(&request, &output, NULL));
    source_fixture_free(&fixture);
    XrLirStateLayout state = {0};
    OK(xr_lir_state_layout_view(output.module, &state));
    CHECK(state.slot_count == 2);
    XrLirType root = state.slots[0].payload, child = state.slots[1].payload;
    CHECK(root != child && !xr_lir_class_parent(output.module, root) && xr_lir_class_parent(output.module, child) == root);
    XrtBillingOwner *billing = NULL;
    CHECK(xrt_billing_owner_new(NULL, SIZE_MAX, &billing) == XRT_MANAGED_OK);
    XrLirValueLayouts *layouts = NULL;
    XrLirValueLayoutKey key = {.type=child};
    CHECK(xr_lir_value_layouts_create(output.module, &key, 1, billing, &layouts) == XR_LIR_VALUE_LAYOUT_OK);
    const XrtValuePlan *plan = NULL; uint32_t child_id = 0;
    CHECK(xr_lir_value_layouts_lookup(layouts, key, &plan, &child_id) == XR_LIR_VALUE_LAYOUT_OK);
    XrLirValueImage *image = NULL;
    CHECK(xr_lir_value_image_create(layouts, billing, &image) == XR_LIR_VALUE_LAYOUT_OK);
    xr_lir_value_layouts_destroy(layouts); xr_lir_lowered_free(&output);
    XrtValueImagePlans *bound = NULL;
    const XrtValueImage *raw = xr_lir_value_image_view(image);
    uint32_t parent_id = raw->types[child_id - 1].class_parent;
    CHECK(parent_id && parent_id < child_id);
    CHECK(xrt_value_image_bind(billing, raw, (XrtManagedLease){0}, &bound) == XRT_MANAGED_OK);
    xr_lir_value_image_destroy(image);
    XrtBoundClass parent_type = {0}, child_type = {0};
    CHECK(xrt_value_image_class(bound, parent_id, &parent_type) == XRT_MANAGED_OK);
    CHECK(xrt_value_image_class(bound, child_id, &child_type) == XRT_MANAGED_OK);
    XrtValuePlanSpec spec;
    CHECK(xrt_value_plan_view(child_type.plan, &spec) == XRT_MANAGED_OK && spec.class_parent == parent_type.plan);
    xrt_value_image_drop(&bound); xrt_billing_owner_drop(billing);
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
static void dynamic_division_source(const char *text) {
    XrLirLowered output = lower_text(text);
    unsigned operations = 0;
    for (uint32_t f = 0; f < xr_lir_function_count(output.module); ++f) {
        XrLirFunction function; XrLirFunctionView view;
        OK(xr_lir_function_at(output.module, f, &function));
        OK(xr_lir_function_view(output.module, function, &view));
        for (uint32_t b = 0; b < view.block_count; ++b) {
            XrLirBlock block; XrLirBlockView bv;
            OK(xr_lir_block_at(output.module, function, b, &block));
            OK(xr_lir_block_view(output.module, block, &bv));
            for (uint32_t i = 0; i < bv.instruction_count; ++i) {
                XrLirInstructionView v;
                OK(xr_lir_instruction_view(output.module, block, i, &v));
                if (v.op == XR_LIR_DIV || v.op == XR_LIR_REM) {
                    CHECK(v.type == XR_LIR_I64 && v.source.path && v.source.line && v.source.column);
                    ++operations;
                }
            }
        }
    }
    CHECK(operations > 0);
    xr_lir_lowered_free(&output);
}
static void fixed_division(const char *fixtures) {
    dynamic_division_source("fn quotient(n:i64,d:i64)->i64{return n/d}\nprint(quotient(8,2))\n");
    dynamic_division_source("fn remainder(n:i64,d:i64)->i64{return n%d}\nprint(remainder(8,2))\n");
    fixed_division_source("fn quotient(n:i64)->i64{return n/2}\nprint(quotient(8))\n", false);
    fixed_division_source("fn remainder(n:i64)->i64{return n%2}\nprint(remainder(8))\n", false);
    char *original = read_file(fixtures, "../../../bench/native/integer_loop", "xr");
    fixed_division_source(original, true);
    xr_free(original);
    dynamic_division_source("fn f(n:i64)->i64{return n/0}\nprint(f(8))\n");
    dynamic_division_source("fn f(n:i64)->i64{return n%0}\nprint(f(8))\n");
    fixed_division_source("fn q(n:i64)->i64{return n/(-1)}\n"
                          "print(q(-9223372036854775807-1))\n", false);
    fixed_division_source("fn r(n:i64)->i64{return n%(-1)}\n"
                          "print(r(-9223372036854775807-1))\n", false);
    dynamic_division_source("fn f(n:i64,d:i64)->i64{return n/(d+2)}\nprint(f(8,1))\n");
    dynamic_division_source("fn f(n:i64)->i64{return n/(3 as i64)}\nprint(f(8))\n");
    dynamic_division_source("fn f(n:i64,flag:bool)->i64{return n%(flag?3:7)}\nprint(f(8,true))\n");
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

    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, true};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before, after;
    CHECK(xr_compile_resources_stats(resources, &before) == XR_COMPILE_RESOURCE_OK);
    XrLirSourceRequest input = {resources, "unopened.xr", NULL, false, NULL, NULL, XR_XIR_HOST_TARGET};
    XrLirSourceFailure source_failure = {0};
    output.entry.id = 1;
    CHECK(xr_lir_compile_source(&input, &output, &source_failure) == XR_LIR_SOURCE_BAD_ARGUMENT);
    CHECK(!output.module && !output.entry.module && output.entry.id == 1);
    CHECK(source_failure.stage == XR_LIR_SOURCE_OK &&
        xr_xir_compile_source_failure_empty(&source_failure.source_failure));
    CHECK(xr_compile_resources_stats(resources, &after) == XR_COMPILE_RESOURCE_OK);
    CHECK(after.held_bytes == before.held_bytes &&
        after.allocation_count == before.allocation_count);
    xr_lir_source_failure_free(&source_failure);
    xr_compile_resources_release(resources);
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
                if (op.op != XR_LIR_STATE_READ || op.source.line < 2 || op.source.line > 3) continue;
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
    output = lower_text("const n=1+2\nprint(n)\n");
    xr_lir_lowered_free(&output);
    output = lower_text("fn value()->i64{return 7}\nconst n=value()\nprint(n)\n");
    xr_lir_lowered_free(&output);
    output = lower_text("const s=\"managed\"\nprint(7)\n");
    xr_lir_lowered_free(&output);
    static const char *constants[] = {
        "const n=7\nconst m=8\nprint(n+m)\n",
        "const n=7\nfn reader()->i64{return n}\nprint(reader())\n",
        "const n=7\nfn unused()->i64{return n}\nprint(1)\n"
    };
    for (size_t i=0; i<sizeof(constants)/sizeof(constants[0]); ++i) {
        output = lower_text(constants[i]); xr_lir_lowered_free(&output);
    }
    output = lower_text("print(reader())\nconst n=7\nfn reader()->i64{return n}\n");
    xr_lir_lowered_free(&output);
    output = lower_text("const n=7\nif(true){print(n)}\n");
    xr_lir_lowered_free(&output);
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
    CHECK(root.result == XR_LIR_I64 && root.parameter_count == 1 &&
        root.parameter_types[0] == XR_LIR_STATE);
    XrLirBlock root_block; XrLirInstructionView root_call;
    OK(xr_lir_block_at(out.module,out.entry,0,&root_block));
    OK(xr_lir_instruction_view(out.module,root_block,1,&root_call));
    CHECK(root_call.op == XR_LIR_CALL && root_call.type == XR_LIR_UNIT);
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
                if (op.op == XR_LIR_STATE_CONSTRUCT) {
                    CHECK(function.id == storage_owner && op.type == XR_LIR_STATE_REF);
                    ++cells;
                }
                reads += op.op == XR_LIR_STATE_READ;
                writes += op.op == XR_LIR_STATE_WRITE;
            }
        }
    }
    CHECK(cells == 1 && reads >= 2 && writes == 1);
    xr_lir_lowered_free(&out);
    out = lower_text("fn read()->i64{return n}\nvar first=read()\nvar n=7\nprint(first)\n");
    xr_lir_lowered_free(&out);
    out = lower_text("if (true) {print(1)}\nvar n=7\nprint(n)\n");
    xr_lir_lowered_free(&out);
    out = lower_text("var n=7\nfn bump(v:ref i64)->i64{v=v+1\nreturn v}\nprint(bump(ref n))\n");
    unsigned ref_formals = 0;
    for (uint32_t index = 0; index < xr_lir_function_count(out.module); ++index) {
        XrLirFunction function;
        XrLirFunctionView view;
        OK(xr_lir_function_at(out.module, index, &function));
        OK(xr_lir_function_view(out.module, function, &view));
        for (uint32_t p = 0; p < view.parameter_count; ++p)
            ref_formals += view.parameter_types[p] == XR_LIR_I64_REF;
    }
    CHECK(ref_formals == 1);
    xr_lir_lowered_free(&out);
    out = lower_text("var n=7\nfn outer()->i64{const f=fn()->i64{return n}\nreturn f()}\nprint(outer())\n");
    unsigned environments = 0;
    for (uint32_t index = 0; index < xr_lir_function_count(out.module); ++index) {
        XrLirFunction function;
        XrLirFunctionView view;
        OK(xr_lir_function_at(out.module, index, &function));
        OK(xr_lir_function_view(out.module, function, &view));
        for (uint32_t b = 0; b < view.block_count; ++b) {
            XrLirBlock block;
            XrLirBlockView body;
            OK(xr_lir_block_at(out.module, function, b, &block));
            OK(xr_lir_block_view(out.module, block, &body));
            for (uint32_t i = 0; i < body.instruction_count; ++i) {
                XrLirInstructionView instruction;
                OK(xr_lir_instruction_view(out.module, block, i, &instruction));
                if (instruction.op != XR_LIR_FUNCTION_REF) continue;
                XrLirClosureTarget target = {0};
                XrLirType state = XR_LIR_TYPE_INVALID;
                OK(xr_lir_closure_target_view(out.module, (XrLirFunction){out.module, instruction.callee}, &target));
                CHECK(!target.capture_count && target.state_capture && instruction.operand_count == 1);
                OK(xr_lir_value_type(out.module,
                    (XrLirValue){out.module, function.id, instruction.operands[0]}, &state));
                CHECK(state == XR_LIR_STATE);
                ++environments;
            }
        }
    }
    CHECK(environments == 1);
    xr_lir_lowered_free(&out);
    fixture = source_fixture("var s=\"owned\"\nprint(7)\n");
    request = (XrLirLowerRequest){fixture.closed,fixture.source.snapshot,false};
    OK(xr_lir_lower_program_checked(&request,&out,NULL));
    source_fixture_free(&fixture);
    OK(xr_lir_verify(out.module,NULL));
    xr_lir_lowered_free(&out);
    puts("actual mutable module Cells retain identity, ordered publication and scoped plain calls");
}

static void module_call_closure(const char *text) {
    SourceFixture fixture = source_fixture(text);
    const XrXirModule *source = xr_xir_compile_artifact_module(fixture.closed);
    uint32_t init = source->declarations->modules[0].initializer;
    uint32_t entry = source->declarations->entry_function;
    CHECK(source->function_count < 64 && source->declarations->slot_count == 1);
    uint32_t counts[64] = {0};
    bool hidden[64] = {0};
    for (uint32_t f = 0; f < source->function_count; ++f) {
        bool reader = source->functions[f].name_length == 9 &&
            !memcmp(source->functions[f].name, "readstate", 9);
        hidden[f] = reader;
        counts[f] = source->functions[f].parameter_count + reader;
    }
    counts[init] = 1;
    hidden[init] = true;
    XrLirLowerRequest request = {fixture.closed, fixture.source.snapshot, false};
    XrLirLowered out = {0};
    OK(xr_lir_lower_program_checked(&request, &out, NULL));
    source_fixture_free(&fixture);
    OK(xr_lir_verify(out.module, NULL));
    unsigned selected = 0;
    for (uint32_t n = 0; n < xr_lir_function_count(out.module); ++n) {
        XrLirFunction function; XrLirFunctionView view;
        OK(xr_lir_function_at(out.module, n, &function));
        OK(xr_lir_function_view(out.module, function, &view));
        uint32_t original = 0;
        for (; original < 64; ++original) {
            char prefix[32];
            int length = snprintf(prefix, sizeof(prefix), "f%u_", original);
            CHECK(length > 0 && (size_t)length < sizeof(prefix));
            if (view.name_length >= (size_t)length && !memcmp(view.name, prefix, (size_t)length)) break;
        }
        if (original == 64) continue;
        if (view.parameter_count != counts[original])
            fprintf(stderr, "closure signature %.*s expected %u actual %u (init %u entry %u)\n",
                (int)view.name_length, view.name, counts[original], view.parameter_count, init, entry);
        CHECK(original < 64 && view.parameter_count == counts[original]);
        if (hidden[original])
            CHECK(view.parameter_types[view.parameter_count-1] == XR_LIR_STATE);
        ++selected;
        for (uint32_t b = 0; b < view.block_count; ++b) {
            XrLirBlock block; XrLirBlockView bv;
            OK(xr_lir_block_at(out.module, function, b, &block));
            OK(xr_lir_block_view(out.module, block, &bv));
            for (uint32_t i = 0; i < bv.instruction_count; ++i) {
                XrLirInstructionView op;
                OK(xr_lir_instruction_view(out.module, block, i, &op));
                if (op.op != XR_LIR_CALL) continue;
                XrLirFunction callee = {out.module, op.callee}; XrLirFunctionView target;
                OK(xr_lir_function_view(out.module, callee, &target));
                CHECK(op.operand_count == target.parameter_count);
            }
        }
    }
    CHECK(selected >= 3);
    xr_lir_lowered_free(&out);
}

static void module_call_closures(void) {
    module_call_closure("fn leaf(n:i64)->i64{return n+1}\n"
        "fn middle(n:i64)->i64{return leaf(n)}\nfn top(n:i64)->i64{return middle(n)}\n"
        "print(top(40))\nvar state=7\nprint(state)\n");
    module_call_closure("fn leaf(s:string)->string{return s+\"!\"}\n"
        "fn middle(s:string)->string{return leaf(s)}\nfn top(s:string)->string{return middle(s)}\n"
        "print(top(\"value\"))\nvar state=7\nprint(state)\n");
    module_call_closure("fn down(n:i64)->i64{if(n==0){return 9}\nreturn down(n-1)}\n"
        "print(down(4))\nvar state=7\nprint(state)\n");
    module_call_closure("fn even(n:i64)->i64{if(n==0){return 1}\nreturn odd(n-1)}\n"
        "fn odd(n:i64)->i64{if(n==0){return 0}\nreturn even(n-1)}\n"
        "print(even(4))\nprint(odd(4))\nvar state=7\nprint(state)\n");
    module_call_closure("fn leaf(n:i64)->i64{print(n)\nreturn n+1}\n"
        "fn top(n:i64)->i64{return leaf(n)}\nfn unused()->i64{return state}\n"
        "print(top(6))\nvar state=7\nfn readstate()->i64{return state}\nprint(readstate())\n");
    static const char *const deferred_reads[] = {
        "fn leaf()->i64{return state}\nfn middle()->i64{return leaf()}\n"
        "fn top()->i64{return middle()}\nprint(top())\nvar state=7\n",
        "fn leaf(flag:bool)->i64{if(flag){return state}\nreturn 1}\n"
        "fn top()->i64{return leaf(false)}\nprint(top())\nvar state=7\n",
        "fn first(n:i64)->i64{if(n==0){return state}\nreturn second(n-1)}\n"
        "fn second(n:i64)->i64{return first(n)}\nprint(second(0))\nvar state=7\n",
        "fn leaf()->i64{var local=1\nreturn local}\n"
        "fn top()->i64{return leaf()}\nprint(top())\nvar state=7\n"
    };
    for (size_t i = 0; i < sizeof(deferred_reads) / sizeof(deferred_reads[0]); ++i) {
        XrLirLowered out = lower_text(deferred_reads[i]);
        xr_lir_lowered_free(&out);
    }
    puts("complete direct-call storage closures share physical signatures after Source retirement");
}

static void frame_cells_source(const char *directory) {
    char *text=read_file(directory,"../../diff/cases/semantics/string_owning_parameter","xr");
    SourceFixture fixture=source_fixture(text);xr_free(text);
    XrLirLowerRequest request={fixture.closed,fixture.source.snapshot,false};
    XrLirLowered out={0};OK(xr_lir_lower_program_checked(&request,&out,NULL));
    source_fixture_free(&fixture);OK(xr_lir_verify(out.module,NULL));
    XrLirStateLayout layout;
    OK(xr_lir_state_layout_view(out.module, &layout));
    unsigned total_cells=0,total_drops=0,multiple_blocks=0,module_strings=0;
    for(uint32_t n=0;n<xr_lir_function_count(out.module);++n) {
        XrLirFunction f;XrLirFunctionView v;
        OK(xr_lir_function_at(out.module,n,&f));OK(xr_lir_function_view(out.module,f,&v));
        unsigned owners=0,returns=0,drops=0;
        for(uint32_t b=0;b<v.block_count;++b) {
            XrLirBlock block;XrLirBlockView bv;
            OK(xr_lir_block_at(out.module,f,b,&block));OK(xr_lir_block_view(out.module,block,&bv));
            XrLirInstructionView term;OK(xr_lir_instruction_view(out.module,block,bv.instruction_count-1,&term));
            returns+=term.op==XR_LIR_RET;
            for(uint32_t i=0;i<bv.instruction_count;++i) {
                XrLirInstructionView op;OK(xr_lir_instruction_view(out.module,block,i,&op));
                if(op.op==XR_LIR_CELL_NEW && op.type==XR_LIR_STRING_CELL) {
                    CHECK(block.id==v.entry);++owners;
                }
                if(op.op==XR_LIR_STRING_CELL_DROP) {CHECK(term.op==XR_LIR_RET);++drops;}
                if(op.op==XR_LIR_STATE_CONSTRUCT) {
                    CHECK(op.bits < layout.slot_count);
                    module_strings += layout.slots[op.bits].payload == XR_LIR_STRING;
                }
            }
        }
        CHECK(drops==owners*returns);
        if(owners) {CHECK(v.block_count>1);++multiple_blocks;}
        total_cells+=owners;total_drops+=drops;
    }
    CHECK(total_cells==0 && total_drops==0 && multiple_blocks==0 && module_strings==3);
    xr_lir_lowered_free(&out);
    static const char *const programs[]={
        "var text=\"seed\"\nvar i=0\nwhile(i<3){text=\"v${i}\"\ni=i+1}\nprint(text)\n",
        "var text=\"seed\"\nfn read(n:i64)->string{if(n<0){return text}\n"
        "if(n>0){text=\"changed\"\nreturn text}\nreturn text}\nprint(read(-1))\nprint(read(1))\nprint(read(0))\n",
        "var text=\"outer\"\nfn frame(n:i64)->i64{var local=\"frame\"\n"
        "const answer=recurse(n)\nreturn answer+len(local)}\n"
        "fn recurse(n:i64)->i64{if(n==0){return len(text)}\nreturn frame(n-1)}\n"
        "print(recurse(80))\nprint(text)\n"
    };
    for(size_t i=0;i<sizeof(programs)/sizeof(programs[0]);++i) {
        fixture=source_fixture(programs[i]);request=(XrLirLowerRequest){fixture.closed,fixture.source.snapshot,false};
        OK(xr_lir_lower_program_checked(&request,&out,NULL));source_fixture_free(&fixture);
        OK(xr_lir_verify(out.module,NULL));xr_lir_lowered_free(&out);
    }
    puts("original String module Cells belong to the execution independently of CFG frames");
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
    XrLirBlockView body;
    OK(xr_lir_block_view(output.module, block, &body));
    uint32_t selected[3], calls = 0, begins = 0, ready = 0;
    for (uint32_t i = 0; i < body.instruction_count; ++i) {
        XrLirInstructionView instruction;
        OK(xr_lir_instruction_view(output.module, block, i, &instruction));
        begins += instruction.op == XR_LIR_MODULE_BEGIN;
        ready += instruction.op == XR_LIR_MODULE_READY;
        if (instruction.op != XR_LIR_CALL) continue;
        CHECK(calls < 3);
        selected[calls] = instruction.callee;
        for (uint32_t j = 0; j < calls; ++j) CHECK(selected[j] != selected[calls]);
        ++calls;
    }
    CHECK(calls == 3 && begins == 2 && ready == 2);
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
    dynamic_division_source("const divisor=0\nfn f(x:i64)->i64{return x/divisor}\nprint(f(1))\n");
    output = lower_text("print(f(1))\nconst divisor=2\nfn f(x:i64)->i64{return x/divisor}\n");
    xr_lir_lowered_free(&output);
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
static void callable_source(void) {
    XrLirLowered declared = lower_text("fn use(callback:escaping fn()->i64)->i64{return callback()}\n"
        "print(use(fn()->i64{return 42}))\n");
    unsigned declared_count = 0;
    for (uint32_t n = 0; n < xr_lir_function_count(declared.module); ++n) {
        XrLirFunction function;
        XrLirFunctionView view;
        OK(xr_lir_function_at(declared.module, n, &function));
        OK(xr_lir_function_view(declared.module, function, &view));
        for (uint32_t p = 0; p < view.callable_parameter_count; ++p) {
            XrLirFunctionCallableParameter row = view.callable_parameters[p];
            if (!row.contract.escaping) continue;
            CHECK(row.parameter < view.parameter_count && row.contract.type == view.parameter_types[row.parameter]);
            CHECK(row.contract.mode == XR_PARAM_READ);
            XrLirClosureTarget target = {0};
            OK(xr_lir_closure_target_view(declared.module, function, &target));
            CHECK(!target.callable);
            ++declared_count;
        }
    }
    CHECK(declared_count == 1);
    xr_lir_lowered_free(&declared);
    XrLirLowered binding = lower_text("fn answer()->i64{var n=40\nconst f=fn()->i64{n=n+1\nreturn n}\nreturn "
        "f()}\nprint(answer())\n");
    unsigned binding_count = 0;
    for (uint32_t n = 0; n < xr_lir_function_count(binding.module); ++n) {
        XrLirFunction function;
        XrLirFunctionView view;
        OK(xr_lir_function_at(binding.module, n, &function));
        OK(xr_lir_function_view(binding.module, function, &view));
        for (uint32_t b = 0; b < view.block_count; ++b) {
            XrLirBlock block;
            XrLirBlockView body;
            OK(xr_lir_block_at(binding.module, function, b, &block));
            OK(xr_lir_block_view(binding.module, block, &body));
            for (uint32_t i = 0; i < body.instruction_count; ++i) {
                XrLirInstructionView instruction;
                OK(xr_lir_instruction_view(binding.module, block, i, &instruction));
                binding_count += instruction.op == XR_LIR_CELL_NEW && instruction.type == XR_LIR_I64_CELL;
            }
        }
    }
    CHECK(binding_count == 1);
    xr_lir_lowered_free(&binding);
    XrLirLowered output = lower_text("fn apply(cb:fn(i64)->i64,x:i64)->i64{return cb(x)}\n"
        "print(apply(fn(x:i64)->i64{return x+2},40))\n");
    unsigned references = 0, invocations = 0;
    for (uint32_t n = 0; n < xr_lir_function_count(output.module); ++n) {
        XrLirFunction function;
        XrLirFunctionView view;
        OK(xr_lir_function_at(output.module, n, &function));
        OK(xr_lir_function_view(output.module, function, &view));
        for (uint32_t b = 0; b < view.block_count; ++b) {
            XrLirBlock block;
            XrLirBlockView body;
            OK(xr_lir_block_at(output.module, function, b, &block));
            OK(xr_lir_block_view(output.module, block, &body));
            for (uint32_t i = 0; i < body.instruction_count; ++i) {
                XrLirInstructionView instruction;
                OK(xr_lir_instruction_view(output.module, block, i, &instruction));
                references += instruction.op == XR_LIR_FUNCTION_REF;
                invocations += instruction.op == XR_LIR_CALL_INDIRECT;
            }
        }
    }
    CHECK(references == 1 && invocations == 1);
    xr_lir_lowered_free(&output);
}
static void assertion_declaration_and_scope_sources(const char *programs) {
    const char *fixtures[] = {"named","ordinary"};
    for (size_t n = 0; n < sizeof(fixtures) / sizeof(*fixtures); ++n) {
        char path[128];
        int length = snprintf(path,sizeof(path),"../../fixtures/lir_cli/assertions/%s",fixtures[n]);
        CHECK(length > 0 && (size_t)length < sizeof(path));
        char *text = read_file(programs,path,"xr");
        XrLirLowered output = lower_text(text); xr_free(text);
        unsigned assertions = 0, weakenings = 0, bindings = 0;
        for (uint32_t f = 0; f < xr_lir_function_count(output.module); ++f) {
            XrLirFunction function; XrLirFunctionView view;
            OK(xr_lir_function_at(output.module,f,&function));
            OK(xr_lir_function_view(output.module,function,&view));
            for (uint32_t b = 0; b < view.block_count; ++b) {
                XrLirBlock block; XrLirBlockView body;
                OK(xr_lir_block_at(output.module,function,b,&block));
                OK(xr_lir_block_view(output.module,block,&body));
                for (uint32_t i = 0; i < body.instruction_count; ++i) {
                    XrLirInstructionView op;
                    OK(xr_lir_instruction_view(output.module,block,i,&op));
                    assertions += op.op == XR_LIR_ASSERT_CONDITION;
                    weakenings += op.op == XR_LIR_FUNCTION_WEAKEN;
                    bindings += op.op == XR_LIR_FUNCTION_BIND;
                }
            }
        }
        CHECK(n ? weakenings && bindings : assertions == 1);
        xr_lir_lowered_free(&output);
    }
}
static uint32_t class_method_checked_function(const XrXirModule *module, const char *name) {
    for (uint32_t f=0; f<module->function_count; ++f)
        if (module->functions[f].name_length==strlen(name) &&
            !memcmp(module->functions[f].name,name,strlen(name))) return f;
    CHECK(false);
    return 0;
}

static uint32_t class_method_site_count(const XrXirModule *module) {
    uint32_t sites = 0;
    for (uint32_t f=0; f<module->function_count; ++f)
        for (uint32_t i=0; i<module->functions[f].instruction_count; ++i)
            sites += module->functions[f].instructions[i].op == XR_XIR_CLASS_METHOD_REF;
    return sites;
}

static bool class_method_lir_name(const XrLirFunctionView *view, const char *suffix) {
    size_t length = strlen(suffix);
    return length < view->name_length && view->name[view->name_length - length - 1] == '_' &&
        !memcmp(view->name + view->name_length - length,suffix,length);
}

static void class_method_local_control(void) {
    SourceFixture f = source_fixture("class Probe{n:i64;init(n:i64){this.n=n} method()->i64{return this.n}}\n"
        "fn read(v:Probe)->i64{return v.method()}\nprint(read(Probe(42)))\n");
    XrLirLowerRequest request = {f.closed, f.source.snapshot, false};
    XrLirLowered output = {0};
    XrLirLowerFailure failure = {0};
    const XrXirModule *module = xr_xir_compile_artifact_module(f.closed);
    uint32_t function = class_method_checked_function(module,"read");
    CHECK(class_method_site_count(module) == 1);
    OK(xr_lir_lower_program_checked(&request,&output,NULL));
    xr_lir_lowered_free(&output);
    XrLirStatus status = xr_lir_lower_local_checked(&request,function,&output,&failure);
    CHECK(status==XR_LIR_UNSUPPORTED && !output.module && !output.entry.id);
    CHECK(strstr(failure.diagnostic.message,"CLASS_METHOD_REF"));
    xr_free(failure.owned_path);
    source_fixture_free(&f);
}

static void class_method_evaluation_control(void) {
    SourceFixture f = source_fixture(
        "class Probe{n:i64;init(n:i64){this.n=n} method()->i64{return this.n}}\n"
        "fn read(v:Probe)->i64{return v.method()}\n"
        "fn make<T>()->i64{return comptime {const v:Array<T>=[];len(v)+42}}\n"
        "export fn run()->i64{return make<i8>()+make<string>()-42+read(Probe(40))}\n"
        "print(run())\n");
    const XrXirConstruction *original = f.source.checked->construction;
    XrXirComptimeTable table = *xr_xir_compile_construction_comptime(original);
    CHECK(table.binding_count > 1);
    --table.binding_count;
    XrXirConstructionSpec spec = {original->rows,original->count,
        original->materializations,original->materialization_count,&table,NULL};
    const XrXirCompileContext *context = xr_xir_compile_artifact_context(f.source.checked);
    XrXirModule built = f.source.checked->module;
    built.stage = XR_XIR_BUILT;
    XrXirConstruction *construction = NULL;
    CHECK(xr_xir_compile_construction_new_v6(context,built.types,&spec,
        xr_xir_compile_construction_slots(original),&construction,NULL)==XR_XIR_OK);
    XrXirArtifact *unresolved = NULL, *evaluation = NULL;
    CHECK(xr_xir_compile_check_v2(context,&built,construction,&unresolved,NULL)==XR_XIR_OK);
    xr_xir_compile_construction_free(construction);
    XirComptimePending *pending = NULL;
    CHECK(xr_xir_compile_comptime_pending(unresolved,&pending,NULL,NULL)==XR_XIR_OK && pending);
    CHECK(xr_xir_compile_comptime_pending_specialize(pending,&evaluation,NULL,NULL)==XR_XIR_OK);
    const XrXirModule *module = xr_xir_compile_artifact_module(evaluation);
    CHECK(module->stage==XR_XIR_CHECKED && module->linkage_kind==XR_XIR_PROGRAM);
    CHECK(module->provenance->kind==XR_XIR_EVIDENCE_EVALUATION && class_method_site_count(module)>0);
    XrLirLowerRequest request = {evaluation,f.source.snapshot,false};
    XrLirLowered output = {0};
    XrLirLowerFailure failure = {0};
    XrLirStatus status = xr_lir_lower_program_checked(&request,&output,&failure);
    CHECK(status==XR_LIR_VERIFY_ERROR && !output.module && !output.entry.id);
    xr_free(failure.owned_path);
    xr_xir_compile_artifact_free(evaluation);
    xr_xir_compile_comptime_pending_free(pending);
    xr_xir_compile_artifact_free(unresolved);
    source_fixture_free(&f);
}

static void class_method_state_control(void) {
    SourceFixture f = source_fixture(
        "var offset:i64=2\n"
        "class Counter{raw:i64;init(raw:i64){this.raw=raw} total()->i64{return this.raw+offset}}\n"
        "fn project(value:Counter)->i64{return value.total()}\n"
        "print(project(Counter(40)))\noffset=3\nprint(project(Counter(40)))\n");
    XrLirLowerRequest request = {f.closed,f.source.snapshot,false};
    XrLirLowered output = {0};
    OK(xr_lir_lower_program_checked(&request,&output,NULL));
    unsigned calls = 0;
    for (uint32_t d=0; d<xr_lir_function_count(output.module); ++d) {
        XrLirFunction function = {0};
        XrLirFunctionView view = {0};
        OK(xr_lir_function_at(output.module,d,&function));
        OK(xr_lir_function_view(output.module,function,&view));
        if (!class_method_lir_name(&view,"project")) continue;
        CHECK(view.parameter_count==2);
        uint32_t receiver = 0;
        for (uint32_t b=0; b<view.block_count; ++b) {
            XrLirBlock block = {0};
            XrLirBlockView body = {0};
            OK(xr_lir_block_at(output.module,function,b,&block));
            OK(xr_lir_block_view(output.module,block,&body));
            for (uint32_t i=0; i<body.instruction_count; ++i) {
                XrLirInstructionView op = {0};
                OK(xr_lir_instruction_view(output.module,block,i,&op));
                CHECK(op.op!=XR_LIR_CALL_INDIRECT && op.op!=XR_LIR_FUNCTION_REF);
                if (op.op==XR_LIR_VALUE_RETAIN && op.operand_count==1 && op.operands[0]==view.parameters[0]) receiver=op.result;
                if (op.op!=XR_LIR_CALL) continue;
                XrLirFunctionView callee = {0};
                OK(xr_lir_function_view(output.module,(XrLirFunction){output.module,op.callee},&callee));
                CHECK(class_method_lir_name(&callee,"total") && callee.parameter_count==2 && op.operand_count==2);
                CHECK(receiver && op.operands[0]==receiver && op.operands[1]==view.parameters[1]);
                CHECK(callee.parameter_types[0]==view.parameter_types[0] &&
                      callee.parameter_types[1]==view.parameter_types[1]);
                ++calls;
            }
        }
    }
    CHECK(calls==1);
    xr_lir_lowered_free(&output);
    source_fixture_free(&f);
}

#include "lir_lower_array_cases.h"
#include "lir_fixed_lower_cases.h"
#include "lir_view_lower_cases.h"

static void checked_empty_array_contract(void) {
    SourceFixture fixture = source_fixture(
        "fn construct(first: i64, second: i64) {\n"
        "    defer { print(first) }\n"
        "    const names: Array<string> = []\n"
        "    print(len(names))\n"
        "    print(second)\n"
        "}\n"
        "construct(7, 13)\n");
    XrLirLowerRequest request = {fixture.closed, fixture.source.snapshot, false};
    XrLirLowered lowered = {0};
    OK(xr_lir_lower_program_checked(&request, &lowered, NULL));
    source_fixture_free(&fixture);

    XrLirModule *module = lowered.module;
    XrLirInstructionView *site = NULL;
    XrLirFunctionData *activation = NULL;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        XrLirFunctionData *function = &module->functions[f];
        for (uint32_t b = 0; b < function->block_count; ++b) {
            XrLirBlockData *block = &function->blocks[b];
            XrLirInstructionView *last = &block->instructions[block->instruction_count - 1].view;
            if (last->op != XR_LIR_CHECKED_EMPTY_ARRAY) continue;
            CHECK(!site);
            site = last;
            activation = function;
        }
    }
    CHECK(site && activation && activation->parameter_count == 2);
    XrLirBlockData *failure = &activation->blocks[site->edges[1].target - 1];
    CHECK(failure->instruction_count == 2);
    XrLirInstructionView *print = &failure->instructions[0].view;
    XrLirInstructionView *terminate = &failure->instructions[1].view;
    CHECK(print->op == XR_LIR_CALL && print->operand_count == 1 &&
        print->operands[0] == activation->parameters[0] && terminate->op == XR_LIR_MANAGED_FAILURE);

    uint64_t family = site->bits;
    uint32_t incompatible = 0;
    OK(xr_lir_array_operation(module, XR_LIR_I64, XR_LIR_ARRAY_OP_NEW, &incompatible));
    CHECK(incompatible != family);
    site->bits = incompatible;
    XrLirDiagnostic diagnostic = {0};
    CHECK(xr_lir_verify(module, &diagnostic) == XR_LIR_VERIFY_ERROR &&
        strstr(diagnostic.message, "edge"));
    site->bits = family;

    const uint32_t *capture = print->operands;
    print->operands = &activation->parameters[1];
    CHECK(xr_lir_verify(module, &diagnostic) == XR_LIR_VERIFY_ERROR);
    print->operands = capture;

    XrLirSource original = terminate->source;
    ++terminate->source.line;
    ++terminate->source.end_line;
    CHECK(xr_lir_verify(module, &diagnostic) == XR_LIR_VERIFY_ERROR);
    terminate->source = original;

    OK(xr_lir_verify(module, NULL));
    XrLirModule *copy = NULL;
    OK(xr_lir_rebuild(module, &copy));
    OK(xr_lir_verify(copy, NULL));
    xr_lir_module_destroy(copy);
    xr_lir_lowered_free(&lowered);
}


static XrLirTestPlan *compile_test_plan(const char *path, bool optimize) {
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrLirSourceRequest request = {resources, path, NULL, optimize, NULL, NULL, XR_XIR_HOST_TARGET};
    XrLirTestPlan *plan = NULL;
    XrLirSourceFailure failure = {0};
    XrLirSourceStatus status = xr_lir_compile_test_plan(&request, &plan, &failure);
    if (status != XR_LIR_SOURCE_OK)
        fprintf(stderr, "test plan: %u: %s %s\n", (unsigned)status, failure.source.message,
            failure.lower.diagnostic.message);
    CHECK(status == XR_LIR_SOURCE_OK && plan);
    CHECK(xr_lir_compile_test_plan(&request, &plan, &failure) == XR_LIR_SOURCE_BAD_ARGUMENT);
    xr_lir_source_failure_free(&failure);
    xr_compile_resources_release(resources);
    return plan;
}

static void test_role_inventory(const char *fixtures) {
    char path[2048];
    int length = snprintf(path, sizeof(path), "%s/test-role-session.xr", fixtures);
    CHECK(length > 0 && (size_t)length < sizeof(path));
    XrLirTestPlan *plain = compile_test_plan(path, false);
    XrLirTestPlan *optimized = compile_test_plan(path, true);
    XrLirFunction initializer = xr_lir_test_plan_initializer(optimized);
    XrLirFunction plain_initializer = xr_lir_test_plan_initializer(plain);
    CHECK(initializer.id && initializer.id == plain_initializer.id && initializer.module != plain_initializer.module);
    XrLirFunction state_entry = {0};
    OK(xr_lir_state_entry(initializer.module, &state_entry));
    CHECK(state_entry.id == initializer.id);
    static const struct {
        const char *name;
        XrXirTestRole role;
    } expected[] = {
        {"last_after_each", XR_XIR_TEST_ROLE_AFTER_EACH},
        {"z_first_test", XR_XIR_TEST_ROLE_TEST},
        {"suite_setup", XR_XIR_TEST_ROLE_BEFORE_ALL},
        {"skipped_test", XR_XIR_TEST_ROLE_SKIP},
        {"suite_teardown", XR_XIR_TEST_ROLE_AFTER_ALL},
        {"a_second_test", XR_XIR_TEST_ROLE_TEST},
        {"first_before_each", XR_XIR_TEST_ROLE_BEFORE_EACH}
    };
    uint32_t count = (uint32_t)(sizeof(expected) / sizeof(expected[0]));
    CHECK(xr_lir_test_plan_role_count(plain) == count && xr_lir_test_plan_role_count(optimized) == count);
    XrLirFunction ordinary = {0}, skipped = {0};
    for (uint32_t i = 0; i < xr_lir_function_count(initializer.module); ++i) {
        XrLirFunction f;
        XrLirFunctionView body;
        OK(xr_lir_function_at(initializer.module, i, &f));
        OK(xr_lir_function_view(initializer.module, f, &body));
        CHECK(!class_method_lir_name(&body, "imported_session_test"));
        if (class_method_lir_name(&body, "ordinary_helper")) ordinary = f;
        if (class_method_lir_name(&body, "skipped_test")) skipped = f;
    }
    CHECK(ordinary.id && skipped.id);
    for (uint32_t i = 0; i < count; ++i) {
        XrLirTestPlanRole role, original;
        OK(xr_lir_test_plan_role_at(optimized, i, &role));
        OK(xr_lir_test_plan_role_at(plain, i, &original));
        CHECK(role.role == expected[i].role && role.role == original.role);
        CHECK(role.name_length == strlen(expected[i].name) && !strcmp(role.name, expected[i].name));
        CHECK(original.name_length == role.name_length && !strcmp(original.name, role.name));
        CHECK(role.function.id == original.function.id);
        CHECK(xr_lir_test_plan_check_role(optimized, i, initializer) == XR_LIR_INVALID_ARGUMENT);
        CHECK(xr_lir_test_plan_check_role(optimized, i, ordinary) == XR_LIR_INVALID_ARGUMENT);
        CHECK(xr_lir_test_plan_check_role(optimized, i, skipped) == XR_LIR_INVALID_ARGUMENT);
        CHECK(xr_lir_test_plan_check_role(optimized, i, original.function) == XR_LIR_INVALID_ARGUMENT);
        if (role.role == XR_XIR_TEST_ROLE_SKIP) {
            CHECK(!role.function.module && !role.function.id);
            continue;
        }
        XrLirFunctionView body;
        CHECK(role.function.module == initializer.module);
        OK(xr_lir_test_plan_check_role(optimized, i, role.function));
        OK(xr_lir_function_view(initializer.module, role.function, &body));
        CHECK(class_method_lir_name(&body, expected[i].name) && body.result == XR_LIR_UNIT && !body.error);
        if (i == 1)
            CHECK(body.parameter_count == 1 && body.parameter_types[0] == XR_LIR_STATE);
    }
    XrLirTestPlanRole first, second;
    OK(xr_lir_test_plan_role_at(optimized, 0, &first));
    OK(xr_lir_test_plan_role_at(optimized, 1, &second));
    CHECK(xr_lir_test_plan_check_role(optimized, 0, second.function) == XR_LIR_INVALID_ARGUMENT);
    CHECK(xr_lir_test_plan_role_at(optimized, count, &first) == XR_LIR_INVALID_ARGUMENT);
    CHECK(first.role == expected[0].role && first.function.id && !strcmp(first.name, expected[0].name));
    CHECK(xr_lir_test_plan_role_at(optimized, 0, NULL) == XR_LIR_INVALID_ARGUMENT);
    CHECK(xr_lir_test_plan_check_role(optimized, count, initializer) == XR_LIR_INVALID_ARGUMENT);
    xr_lir_test_plan_free(optimized);
    xr_lir_test_plan_free(plain);
}

int main(int argc, char **argv) {
    CHECK(argc == 3);
    callable_source();
    assertion_declaration_and_scope_sources(argv[1]);
    class_layout_sources();
    class_method_local_control();
    class_method_evaluation_control();
    class_method_state_control();
    test_role_inventory(argv[1]);
    inherited_class_layout_sources();
    module_i64_groups();
    view_cold_checked_sources(argv[1]);
    fixed_sources();
    array_sources();
    checked_empty_array_contract();
    offset_sources();
    imported_time(argv[1]);
    clock_sources(argv[1]);
    supported_control();
    floating_negation_source();
    floating_conversion_source();
    swapped_backedge();
    fixed_division(argv[1]);
    unsupported();
    class_parent_views(argv[1]);
    unsupported_terminators(argv[1]);
    argument_validation();
    literal_sources();
    module_cells_source(argv[1]);
    module_call_closures();
    frame_cells_source(argv[1]);
    local_boundaries();
    golden(argv[1], argv[2], "source-fib");
    golden(argv[1], argv[2], "source-sum");
    puts("source-backed scalar LIR lowering passed");
    return 0;
}
