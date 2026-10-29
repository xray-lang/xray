/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_lower_allocations.c - Representative unpublished lowering failures
 */
#include "lir_source_fixture.h"
#include "lir/xlir_lower.h"
#include "lir/xlir_verify.h"
#include "base/xchecks.h"

static size_t attempts, fail_at = SIZE_MAX;
typedef struct LowerBlock { void *pointer; size_t bytes; } LowerBlock;
static LowerBlock observed_lower_blocks[1024];
static size_t lower_live, lower_bytes;
static void injected_release(void *pointer) {
    if (pointer) {
        size_t i=0;
        while (i<lower_live && observed_lower_blocks[i].pointer != pointer) ++i;
        CHECK(i<lower_live);
        lower_bytes -= observed_lower_blocks[i].bytes;
        observed_lower_blocks[i] = observed_lower_blocks[--lower_live];
    }
    xr_free(pointer);
}

static void *injected_allocate(size_t bytes) {
    if (attempts++ == fail_at)
        return NULL;
    void *pointer = xr_malloc(bytes);
    if (pointer) {
        CHECK(lower_live<1024 && bytes<=SIZE_MAX-lower_bytes);
        observed_lower_blocks[lower_live++] = (LowerBlock){pointer,bytes}; lower_bytes += bytes;
    }
    return pointer;
}

#undef xr_malloc
#define xr_malloc(bytes) injected_allocate(bytes)
#undef xr_free
#define xr_free(pointer) injected_release(pointer)
#include "lir/xlir_lower.c"

static XrLirStatus run(const SourceFixture *fixture) {
    size_t prior_live=lower_live, prior_bytes=lower_bytes;
    XrLirLowerRequest request = {fixture->closed, fixture->source.snapshot, false};
    XrLirLowered output = {0};
    XrLirLowerFailure failure = {0};
    XrLirStatus status = xr_lir_lower_program_checked(&request, &output, &failure);
    if (status == XR_LIR_OK) {
        CHECK(output.module && output.entry.id);
        OK(xr_lir_verify(output.module, NULL));
    } else {
        CHECK(!output.module && !output.entry.id);
        CHECK(failure.diagnostic.status == status && failure.diagnostic.message[0]);
    }
    xr_lir_lowered_free(&output);
    xr_lir_lower_failure_free(&failure);
    CHECK(!output.module && !failure.owned_path);
    CHECK(lower_live==prior_live && lower_bytes==prior_bytes);
    return status;
}

static void supported_length(void) {
    SourceFixture fixture =
        source_fixture("fn answer()->i64{return len(\"unsupported\")}\nprint(answer())\n");
    attempts=0;fail_at=SIZE_MAX;OK(run(&fixture));
    size_t total=attempts;CHECK(total>1);
    const size_t points[]={0,total/2};
    for(size_t i=0;i<sizeof(points)/sizeof(points[0]);++i) {
        attempts=0;fail_at=points[i];CHECK(run(&fixture)==XR_LIR_OUT_OF_MEMORY);
        fail_at=SIZE_MAX;OK(run(&fixture));
    }
    source_fixture_free(&fixture);
}
static void diagnostic_copy_failure(void) {
    SourceFixture fixture = source_fixture(
        "struct Pair{x:i64}\nfn answer()->i64{const p=Pair{x:42}\nreturn p.x}\nprint(answer())\n");
    XrLirLowerRequest request = {fixture.closed, fixture.source.snapshot, false};
    XrLirLowered output = {0};
    XrLirLowerFailure failure = {0};
    attempts = 0;
    fail_at = SIZE_MAX;
    CHECK(xr_lir_lower_program_checked(&request, &output, &failure) == XR_LIR_UNSUPPORTED);
    CHECK(strstr(failure.diagnostic.message,"STRUCT_NEW"));
    CHECK(!output.module && failure.owned_path && failure.diagnostic.source.path == failure.owned_path);
    CHECK(attempts > 0);
    size_t path_point = attempts - 1;
    xr_lir_lower_failure_free(&failure);
    attempts = 0;
    fail_at = path_point;
    CHECK(xr_lir_lower_program_checked(&request, &output, &failure) == XR_LIR_OUT_OF_MEMORY);
    CHECK(!output.module && !output.entry.id && failure.diagnostic.status == XR_LIR_OUT_OF_MEMORY);
    CHECK(!failure.owned_path && !failure.diagnostic.source.path);
    xr_lir_lower_failure_free(&failure);
    source_fixture_free(&fixture);
    fail_at = SIZE_MAX;
}

// These are private structural controls, not claims of valid Source/Checked input.
static void literal_guard(const XrLirLowerRequest *request, const XrXirModule *source, bool good) {
    Lower c = {.request = request, .source = source, .instruction = UINT32_MAX};
    CHECK(lower_modules(&c));
    CHECK(lower_literal(&c) == good);
    CHECK(c.status == (good ? XR_LIR_OK : XR_LIR_UNSUPPORTED));
    lower_modules_free(&c);
}
static void literal_structure(void) {
    SourceFixture fixture = source_fixture("const n=7\nprint(n)\nprint(n)\n");
    XrLirLowerRequest request = {fixture.closed, fixture.source.snapshot, false};
    const XrXirModule *original = xr_xir_compile_artifact_module(fixture.closed);
    XrXirModule source = *original;
    XrXirDeclarations declarations = *source.declarations;
    XrXirSlot slots[2] = {declarations.slots[0], declarations.slots[0]};
    source.declarations = &declarations;
    declarations.slots = slots;
    XrXirFunction *functions = xr_malloc((size_t)(source.function_count + 1) * sizeof(*functions));
    CHECK(functions);
    memcpy(functions, source.functions, (size_t)source.function_count * sizeof(*functions));
    source.functions = functions;
    XrXirFunctionIdentity *identities = xr_malloc((source.function_count + 1) * sizeof(*identities));
    CHECK(identities);
    memset(identities, 0, (source.function_count + 1) * sizeof(*identities));
    memcpy(identities, declarations.functions, source.function_count * sizeof(*identities));
    declarations.functions = identities;
    uint32_t init = declarations.modules[0].initializer;
    XrXirFunction *f = &functions[init];
    XrXirInstruction *ops = xr_malloc((size_t)f->instruction_count * sizeof(*ops));
    CHECK(ops);
    const XrXirInstruction *original_ops = f->instructions;
    memcpy(ops, original_ops, (size_t)f->instruction_count * sizeof(*ops));
    f->instructions = ops;
    uint32_t publish = UINT32_MAX, read = UINT32_MAX;
    for (uint32_t i = 0; i < f->instruction_count; ++i) {
        if (ops[i].op == XR_XIR_SLOT_INIT) publish = i;
        if (ops[i].op == XR_XIR_SLOT_LOAD) read = i;
    }
    CHECK(publish != UINT32_MAX && read > publish && f->block_count == 1);
    literal_guard(&request, &source, true);
    // Values are examined in the actual Closed definition, including zero.
    uint32_t definition = ops[publish].args[0] - f->parameter_count;
    ops[definition].immediate = 0;
    literal_guard(&request, &source, true);
    ops[definition] = original_ops[definition];
    const XrXirOp definitions[] = {XR_XIR_COPY, XR_XIR_ADD_INT, XR_XIR_CONVERT_NUMBER, XR_XIR_PHI};
    for (size_t i = 0; i < sizeof(definitions) / sizeof(definitions[0]); ++i) {
        ops[definition].op = definitions[i];
        literal_guard(&request, &source, false);
    }
    ops[definition] = original_ops[definition];
    const XrXirOp roles[] = {XR_XIR_SLOT_INIT, XR_XIR_SLOT_STORE, XR_XIR_SLOT_PLACE,
        XR_XIR_SLOT_GROUP_INIT};
    for (size_t i = 0; i < sizeof(roles) / sizeof(roles[0]); ++i) {
        ops[read].op = roles[i];
        literal_guard(&request, &source, false);
    }
    ops[read] = original_ops[read];
    // Preserve the real CONST and move the sole INIT after an existing read.
    // This violates only read ordering, not the direct-definition condition.
    ops[publish] = original_ops[read];
    ops[read] = original_ops[publish];
    XrLirLowerFailure failure = {0};
    Lower early = {.request = &request, .source = &source, .failure = &failure,
        .instruction = UINT32_MAX};
    CHECK(lower_modules(&early));
    CHECK(!lower_literal(&early) && early.status == XR_LIR_UNSUPPORTED);
    CHECK(strstr(failure.diagnostic.message, "slot read outside its initialized block"));
    xr_lir_lower_failure_free(&failure);
    lower_modules_free(&early);
    ops[publish] = original_ops[publish];
    ops[read] = original_ops[read];
    slots[0].mutable = 1;
    literal_guard(&request, &source, false);
    slots[0].mutable = 0;
    slots[0].type = XR_XIR_F64;
    literal_guard(&request, &source, false);
    slots[0].type = XR_XIR_I64;
    declarations.slot_count = 2;
    literal_guard(&request, &source, false);
    declarations.slot_count = 1;
    // A unique static INIT inside a recurrent nonentry block is insufficient.
    XrXirBlock blocks[2] = {{.first = 0, .count = 1}, {.first = 1, .count = 4}};
    XrXirInstruction recurrent[] = {
        {.op = XR_XIR_JUMP, .targets = {1}},
        {.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 7},
        {.op = XR_XIR_SLOT_INIT, .args = {1}},
        {.op = XR_XIR_SLOT_LOAD, .type = XR_XIR_I64},
        {.op = XR_XIR_JUMP, .targets = {1}},
    };
    f->instructions = recurrent;
    f->instruction_count = 5;
    f->block_count = 2;
    f->blocks = blocks;
    literal_guard(&request, &source, false);
    XrXirInstruction cross_block[] = {
        {.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 7},
        {.op = XR_XIR_SLOT_INIT, .args = {0}},
        {.op = XR_XIR_JUMP, .targets = {1}},
        {.op = XR_XIR_SLOT_LOAD, .type = XR_XIR_I64},
        {.op = XR_XIR_RETURN},
    };
    blocks[0].count = 3;
    blocks[1] = (XrXirBlock) {.first = 3, .count = 2};
    f->instructions = cross_block;
    literal_guard(&request, &source, false);
    f->instructions = ops;
    f->instruction_count = original->functions[init].instruction_count;
    f->blocks = original->functions[init].blocks;
    f->block_count = 1;
    ops[read] = (XrXirInstruction) {.op = XR_XIR_CALL, .immediate = init};
    literal_guard(&request, &source, false); // Recursive original initializer call.
    ops[read] = original_ops[read];
    XrXirInstruction spare_ops[2] = {original_ops[read], {.op = XR_XIR_RETURN}};
    XrXirBlock spare_block = {.count = 2};
    functions[source.function_count] = (XrXirFunction) {.instructions = spare_ops,
        .instruction_count = 2, .blocks = &spare_block, .block_count = 1};
    ++source.function_count;
    literal_guard(&request, &source, true); // A same-module helper can read initialized constants.
    spare_ops[0] = (XrXirInstruction) {.op = XR_XIR_CALL, .immediate = init};
    literal_guard(&request, &source, false); // Unselected cross-function initializer call.
    for (size_t i = 0; i < sizeof(roles) / sizeof(roles[0]); ++i) {
        spare_ops[0] = (XrXirInstruction) {.op = roles[i]};
        literal_guard(&request, &source, false); // Every unselected slot role.
    }
    xr_free(ops);
    xr_free(functions);
    xr_free(identities);
    source_fixture_free(&fixture);
    puts("original literal slot roles and one-time entry restrictions passed");
}

int main(void) {
    SourceFixture fixture =
        source_fixture("fn add(n:i64)->i64{var i=0\nvar sum=0\nwhile(i<n){"
                       "sum=sum+(i<2 ? i : i+1)\ni=i+1}\nreturn sum}\nprint(add(5))\n");
    OK(run(&fixture));
    size_t total = attempts;
    CHECK(total > 0);
    const size_t points[] = {0, total / 2};
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); ++i) {
        attempts = 0;
        fail_at = points[i];
        CHECK(run(&fixture) == XR_LIR_OUT_OF_MEMORY);
        fail_at = SIZE_MAX;
        OK(run(&fixture));
    }
    source_fixture_free(&fixture);
    fixture = source_fixture("var n=1\nfn bump()->i64{n=n+1\nreturn n}\nprint(bump())\n");
    fail_at = SIZE_MAX;
    attempts = 0;
    OK(run(&fixture));
    CHECK(attempts > 2);
    for (size_t point=0; point<2; ++point) {
        attempts = 0; fail_at = point;
        CHECK(run(&fixture) == XR_LIR_OUT_OF_MEMORY);
        fail_at = SIZE_MAX;
        OK(run(&fixture));
    }
    source_fixture_free(&fixture);
    fixture = source_fixture("import time\nprint(1)\n");
    attempts = 0; fail_at = SIZE_MAX;
    OK(run(&fixture));
    size_t imported_attempts = attempts;
    CHECK(imported_attempts > 4);
    for (size_t point=0; point<imported_attempts; ++point) {
        attempts = 0; fail_at = point;
        CHECK(run(&fixture) == XR_LIR_OUT_OF_MEMORY);
        fail_at = SIZE_MAX;
        OK(run(&fixture));
    }
    source_fixture_free(&fixture);
    supported_length();
    diagnostic_copy_failure();
    literal_structure();
    CHECK(!lower_live && !lower_bytes);
    puts("representative lowering workspace and diagnostic-copy failures passed");
    return 0;
}
