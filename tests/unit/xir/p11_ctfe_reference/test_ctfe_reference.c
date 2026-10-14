/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_ctfe_reference.c - Independent recursive compiler constant controls
 *
 * KEY CONCEPT:
 *   Fixed mathematical answers distinguish recursive frames and closure scope.
 *   Source, session and packet death precede independent receiving and execution.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#include "../xir_source_compile_owner.h"
#include "base/xsha256.h"
#include "xir/xxir_ctfe.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_scalar_core.h"

typedef struct CtfeReferenceCase {
    const char *name;
    XrXirType type;
    int64_t value;
    XrXirStatus status;
    const char *reason;
    int line;
} CtfeReferenceCase;

static const CtfeReferenceCase reference_cases[] = {
    {"cell_rune", XR_XIR_RUNE, 128512, XR_XIR_OK, NULL, 0},
    {"cell_loop", XR_XIR_I64, 42, XR_XIR_OK, NULL, 0},
    {"cell_recursion", XR_XIR_I64, 720, XR_XIR_OK, NULL, 0},
    {"cell_unit", XR_XIR_I64, 42, XR_XIR_OK, NULL, 0},
    {"cell_infinite", XR_XIR_UNIT, 0, XR_XIR_BUDGET, NULL, 2},
    {"cell_io", XR_XIR_UNIT, 0, XR_XIR_BAD_TYPE, "admitted deterministic scalar direct-call closure", 2},
    {"cell_leaf", XR_XIR_UNIT, 0, XR_XIR_OK, NULL, 0},
    {"rune_literal", XR_XIR_RUNE, 65, XR_XIR_OK, NULL, 0},
    {"rune_nested", XR_XIR_RUNE, 128512, XR_XIR_OK, NULL, 0},
    {"rune_equal", XR_XIR_BOOL, 1, XR_XIR_OK, NULL, 0},
    {"rune_direct", XR_XIR_UNIT, 0, XR_XIR_OK, NULL, 0},
    {"rune_surrogate", XR_XIR_UNIT, 0, XR_XIR_BAD_TYPE, NULL, 1},
    {"rune_overflow", XR_XIR_UNIT, 0, XR_XIR_BAD_TYPE, NULL, 1},
    {"factorial", XR_XIR_I64, 720, XR_XIR_OK, NULL, 0},
    {"unit_empty", XR_XIR_I64, 42, XR_XIR_OK, NULL, 0},
    {"unit_statements", XR_XIR_I64, 42, XR_XIR_OK, NULL, 0},
    {"unit_direct", XR_XIR_UNIT, 0, XR_XIR_OK, NULL, 0},
    {"unit_io", XR_XIR_UNIT, 0, XR_XIR_BAD_TYPE, "admitted deterministic scalar direct-call closure", 2},
    {"unit_infinite", XR_XIR_UNIT, 0, XR_XIR_BUDGET, NULL, 2},
    {"unit_closure", XR_XIR_I64, 42, XR_XIR_OK, NULL, 0},
    {"unit_recipe", XR_XIR_I64, 42, XR_XIR_OK, NULL, 0},
    {"unit_nested", XR_XIR_I64, 42, XR_XIR_OK, NULL, 0},
    {"short_circuit", XR_XIR_I64, 42, XR_XIR_OK, NULL, 0},
    {"logical_truth_table", XR_XIR_I64, 232, XR_XIR_OK, NULL, 0},
    {"logical_skip_division", XR_XIR_I64, 42, XR_XIR_OK, NULL, 0},
    {"logical_recursion", XR_XIR_I64, 42, XR_XIR_OK, NULL, 0},
    {"leaf_locals", XR_XIR_UNIT, 0, XR_XIR_OK, NULL, 0},
    {"mutual_parity", XR_XIR_BOOL, 0, XR_XIR_OK, NULL, 0},
    {"frame_state", XR_XIR_I64, 316, XR_XIR_OK, NULL, 0},
    {"unreachable_function_io", XR_XIR_I64, 42, XR_XIR_OK, NULL, 0},
    {"reachable_short_circuit_io", XR_XIR_UNIT, 0, XR_XIR_BAD_TYPE,
        "admitted deterministic scalar direct-call closure", 3},
    {"reachable_branch_io", XR_XIR_UNIT, 0, XR_XIR_BAD_TYPE,
        "admitted deterministic scalar direct-call closure", 3},
    {"mutual_depth", XR_XIR_UNIT, 0, XR_XIR_BUDGET, NULL, 3},
    {"mutual_depth_direct", XR_XIR_UNIT, 0, XR_XIR_OK, NULL, 0}
};
static const XrCompileResourceLimits reference_limits = {
    UINT64_C(64) * 1024 * 1024, UINT64_C(8) * 1024 * 1024, UINT64_C(128000000)
};

static XrCompileResourceStats reference_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(context->resources, &stats) == XR_COMPILE_RESOURCE_OK);
    return stats;
}

static XrXirStatus reference_source(const XrXirCompileContext *context, const char *name,
    XrXirSourceResult *result, XrXirSourceDiagnostic *diagnostic) {
    char path[1024], text[4096], identity[64];
    int length = snprintf(path, sizeof(path), "%s/%s.xr", XR_CTFE_REFERENCE_FIXTURES, name);
    CHECK(length > 0 && (size_t)length < sizeof(path));
    FILE *file = fopen(path, "rb");
    CHECK(file);
    size_t bytes = fread(text, 1, sizeof(text) - 1, file);
    CHECK(!ferror(file) && feof(file) && fclose(file) == 0 && bytes);
    text[bytes] = 0;
    length = snprintf(identity, sizeof(identity), "ctfe-reference-%s", name);
    CHECK(length > 0 && (size_t)length < sizeof(identity));
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY, identity, NULL};
    XrXirSourceRequest request = {session, NULL, &authority, context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceText input = {NULL, text, bytes};
    XrXirStatus status = xr_xir_compile_source_check_text(&request, &input, result, diagnostic, NULL);
    xr_compile_session_free(session);
    memset(text, 0xa5, sizeof(text));
    memset(identity, 0xa5, sizeof(identity));
    return status;
}

static uint32_t reference_run_function(const XrXirModule *module) {
    uint32_t found = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *body = &module->functions[f];
        if (body->name_length == 3 && !memcmp(body->name, "run", 3)) {
            CHECK(found == UINT32_MAX);
            found = f;
        }
    }
    CHECK(found != UINT32_MAX);
    return found;
}

static XrXirArtifact *reference_lower(const XrXirCompileContext *context,
    XrXirSourceResult *source, uint8_t *identity) {
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(source->checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_source_result_free(source);
    XrXirArtifact *received = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &received, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xa5, packet.length);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_verify(received, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(received, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(received);
    CHECK(xr_xir_compile_artifact_verify(closed, NULL) == XR_XIR_OK);
    if (identity) {
        CHECK(xr_xir_compile_checked_write(closed, &packet, NULL) == XR_XIR_OK);
        CHECK(xr_compile_resources_work(context->resources, packet.length) == XR_COMPILE_RESOURCE_OK);
        xr_sha256(packet.bytes, packet.length, identity);
        xr_xir_compile_checked_packet_free(&packet);
    }
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_context(lowered)->resources == context->resources);
    return lowered;
}

static void reference_accept(const XrXirCompileContext *context, XrXirSourceResult *source,
    const CtfeReferenceCase *selected) {
    CHECK(source->checked && source->snapshot);
    CHECK(xr_xir_compile_artifact_verify(source->checked, NULL) == XR_XIR_OK);
    const XrXirModule *module = xr_xir_compile_artifact_module(source->checked);
    const XrXirFunction *body = &module->functions[reference_run_function(module)];
    bool constant = false;
    for (uint32_t i = 0; i < body->instruction_count; ++i) {
        const XrXirInstruction *op = &body->instructions[i];
        CHECK(op->op != XR_XIR_CALL && op->op != XR_XIR_CALL_INDIRECT &&
            op->op != XR_XIR_FUNCTION_REF);
        if (op->op == (selected->type == XR_XIR_BOOL ? XR_XIR_CONST_BOOL : selected->type == XR_XIR_RUNE ? XR_XIR_CONST_RUNE : XR_XIR_CONST_INT) &&
            op->type == selected->type && op->immediate == selected->value) constant = true;
    }
    CHECK(constant);
    XrXirArtifact *lowered = reference_lower(context, source, NULL);
    XrXirRunContext run = {XR_XIR_CTFE_MAX_STEPS, XR_XIR_CTFE_MAX_FRAME_BYTES, 0, 0, 0, 0};
    XrXirValue value = {0};
    CHECK(xr_xir_compile_vm_run(lowered,
        reference_run_function(xr_xir_compile_artifact_module(lowered)),
        &run, NULL, 0, &value) == XR_XIR_RUN_OK);
    CHECK(!run.live_bytes && run.allocations == run.frees);
    xr_xir_compile_artifact_free(lowered);
    CHECK(value.type == (uint32_t)selected->type && !value.reserved && value.payload == selected->value);
}

static void reference_direct_depth(const XrXirCompileContext *context, XrXirSourceResult *source) {
    uint8_t identity[32] = {0};
    XrXirArtifact *lowered = reference_lower(context, source, identity);
    XrXirCtfeCallRequest request = {
        .evaluation = {.artifact = lowered,
            .function = reference_run_function(xr_xir_compile_artifact_module(lowered)),
            .target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION},
            .limits = {XR_XIR_CTFE_MAX_STEPS, XR_XIR_CTFE_MAX_FRAME_BYTES, XR_XIR_CTFE_MAX_OUTPUT_BYTES}},
        .call_depth = 3
    };
    memcpy(request.evaluation.checked_identity, identity, sizeof(identity));
    XrCompileResourceStats before = reference_stats(context);
    size_t blocks = source_fixture_compile_live, bytes = source_fixture_compile_bytes;
    XrXirValue output = {0};
    CHECK(xr_xir_compile_ctfe_calls(&request, &output) == XR_XIR_CTFE_DEPTH_LIMIT);
    CHECK(!output.type && !output.reserved && !output.payload);
    XrCompileResourceStats after = reference_stats(context);
    CHECK(after.live_bytes == before.live_bytes && after.work >= before.work &&
        after.allocated_bytes >= before.allocated_bytes);
    CHECK(source_fixture_compile_live == blocks && source_fixture_compile_bytes == bytes);
    xr_xir_compile_artifact_free(lowered);
    fprintf(stderr, "CTFE mutual recursion explicit depth=3 rejected; Unit preserved; same finite ledger\n");
}

static void reference_leaf_locals(const XrXirCompileContext *context, XrXirSourceResult *source) {
    uint8_t identity[32] = {0};
    XrXirArtifact *lowered = reference_lower(context, source, identity);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    uint32_t function = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        if (module->functions[f].name_length == 4 && !memcmp(module->functions[f].name, "both", 4)) {
            CHECK(function == UINT32_MAX);
            function = f;
        }
    }
    CHECK(function != UINT32_MAX);
    unsigned locals = 0;
    const XrXirFunction *body = &module->functions[function];
    for (uint32_t i = 0; i < body->instruction_count; ++i) {
        XrXirOp op = body->instructions[i].op;
        if (op == XR_XIR_SCALAR_LOCAL_NEW) locals |= 1;
        if (op == XR_XIR_SCALAR_LOCAL_READ) locals |= 2;
        if (op == XR_XIR_SCALAR_LOCAL_WRITE) locals |= 4;
    }
    CHECK(locals == 7);
    XrXirValue arguments[2] = {{XR_XIR_BOOL, 0, 0}, {XR_XIR_BOOL, 0, 0}};
    XrXirCtfeRequest request = {.artifact = lowered, .function = function,
        .target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION},
        .arguments = arguments, .argument_count = 2,
        .limits = {XR_XIR_CTFE_MAX_STEPS, XR_XIR_CTFE_MAX_FRAME_BYTES, XR_XIR_CTFE_MAX_OUTPUT_BYTES}};
    memcpy(request.checked_identity, identity, sizeof(identity));
    for (int a = 0; a < 2; ++a) for (int b = 0; b < 2; ++b) {
        arguments[0].payload = a;
        arguments[1].payload = b;
        XrXirValue output = {0};
        CHECK(xr_xir_compile_ctfe_leaf(&request, &output) == XR_XIR_CTFE_OK);
        CHECK(output.type == XR_XIR_BOOL && !output.reserved && output.payload == (a && b));
    }
    xr_xir_compile_artifact_free(lowered);
    fprintf(stderr, "CTFE actual scalar local NEW/READ/WRITE leaf truth table passed\n");
}

static void reference_unit_direct(const XrXirCompileContext *context, XrXirSourceResult *source) {
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t noop=UINT32_MAX,leaf=UINT32_MAX;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];
        if (body->name_length==4 && !memcmp(body->name,"noop",4)) { CHECK(noop==UINT32_MAX);noop=f; }
        if (body->name_length==4 && !memcmp(body->name,"leaf",4)) { CHECK(leaf==UINT32_MAX);leaf=f; }
    }
    CHECK(noop!=UINT32_MAX && leaf!=UINT32_MAX);
    CHECK(module->functions[noop].result==XR_XIR_UNIT && module->functions[noop].instruction_count==1);
    XrXirCtfeRequest request={.artifact=lowered,.function=noop,
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={1,XR_XIR_CTFE_MAX_FRAME_BYTES,0}};
    memcpy(request.checked_identity,identity,sizeof(identity));
    XrXirValue output={0};
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK);
    CHECK(!output.type && !output.reserved && !output.payload);
    request.limits.steps=0;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_STEP_LIMIT);
    CHECK(!output.type && !output.reserved && !output.payload);
    request.limits.steps=1;
    XrXirCtfeCallRequest calls={request,1};
    CHECK(xr_xir_compile_ctfe_calls(&calls,&output)==XR_XIR_CTFE_OK);
    CHECK(!output.type && !output.reserved && !output.payload);
    calls.call_depth=0;
    CHECK(xr_xir_compile_ctfe_calls(&calls,&output)==XR_XIR_CTFE_DEPTH_LIMIT);
    CHECK(!output.type && !output.reserved && !output.payload);
    XrXirValue argument={XR_XIR_I64,0,41};calls.evaluation.function=leaf;
    calls.evaluation.arguments=&argument;calls.evaluation.argument_count=1;
    calls.evaluation.limits=(XrXirCtfeLimits){XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,8};calls.call_depth=2;
    CHECK(xr_xir_compile_ctfe_calls(&calls,&output)==XR_XIR_CTFE_OK);
    CHECK(output.type==XR_XIR_I64 && !output.reserved && output.payload==42 && argument.payload==41);
    XrXirValue occupied={XR_XIR_UNIT,77,99},saved=occupied;
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_xir_compile_ctfe_leaf(&request,&occupied)==XR_XIR_CTFE_BAD_ARGUMENT);
    CHECK(!memcmp(&occupied,&saved,sizeof(saved)));
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(!memcmp(&before,&after,sizeof(before)));
    xr_xir_compile_artifact_free(lowered);
    fprintf(stderr,"CTFE canonical Unit leaf/call output bytes=0; exact1/minus0 and depth1/minus0; child Unit keeps parent argument41/result42; occupied fees unchanged\n");
}

static uint32_t reference_named(const XrXirModule *module,const char *name) {
    uint32_t result=UINT32_MAX;size_t length=strlen(name);
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==length && !memcmp(module->functions[f].name,name,length)) {
            CHECK(result==UINT32_MAX);result=f;
        }
    CHECK(result!=UINT32_MAX);return result;
}
static void reference_rune_direct(const XrXirCompileContext *context,XrXirSourceResult *source) {
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t point=reference_named(module,"point"),convert=reference_named(module,"convert");
    uint32_t same=reference_named(module,"same"),literal=reference_named(module,"literal");
    XrXirValue args[2]={{XR_XIR_RUNE,0,0},{XR_XIR_RUNE,0,0}},output={0};
    XrXirCtfeRequest request={.artifact=lowered,.function=literal,
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,4}};
    memcpy(request.checked_identity,identity,sizeof(identity));
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK);
    CHECK(output.type==XR_XIR_RUNE && !output.reserved && output.payload==65);output=(XrXirValue){0};
    request.limits.output_bytes=3;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OUTPUT_LIMIT && !output.type && !output.payload);
    request.limits.output_bytes=8;request.arguments=args;request.argument_count=1;
    static const int64_t good[]={0,65,0xd7ff,0xe000,0x1f600,0x10ffff};
    static const int64_t bad[]={-1,0xd800,0xdfff,0x110000,INT64_MAX};
    for (size_t i=0;i<sizeof(good)/sizeof(good[0]);++i) for (unsigned c=0;c<2;++c) {
        request.function=c?convert:point;args[0]=(XrXirValue){c?XR_XIR_I64:XR_XIR_RUNE,0,good[i]};
        XrXirCtfeCallRequest calls={request,1};
        CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK);
        CHECK(output.type==(uint32_t)(c?XR_XIR_RUNE:XR_XIR_I64) && !output.reserved && output.payload==good[i]);output=(XrXirValue){0};
        CHECK(xr_xir_compile_ctfe_calls(&calls,&output)==XR_XIR_CTFE_OK);
        CHECK(output.type==(uint32_t)(c?XR_XIR_RUNE:XR_XIR_I64) && !output.reserved && output.payload==good[i]);output=(XrXirValue){0};
    }
    for (size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) for (unsigned c=0;c<2;++c) {
        request.function=c?convert:point;args[0]=(XrXirValue){c?XR_XIR_I64:XR_XIR_RUNE,0,bad[i]};
        XrXirCtfeStatus expected=c?XR_XIR_CTFE_NUMERIC_RANGE:XR_XIR_CTFE_BAD_ARGUMENT;
        XrXirCtfeCallRequest calls={request,1};
        CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==expected);
        CHECK(!output.type && !output.reserved && !output.payload);
        CHECK(xr_xir_compile_ctfe_calls(&calls,&output)==expected);
        CHECK(!output.type && !output.reserved && !output.payload);
        bool unchanged=true;
        CHECK(xir_scalar_core_equal(XR_XIR_RUNE,bad[i],65,&unchanged)==XR_XIR_RUN_BAD_ARGUMENT && unchanged);
    }
    request.function=same;request.argument_count=2;args[0]=(XrXirValue){XR_XIR_RUNE,0,65};
    for (unsigned different=0;different<2;++different) {
        args[1]=(XrXirValue){XR_XIR_RUNE,0,65+different};bool equal=false;
        CHECK(xir_scalar_core_equal(XR_XIR_RUNE,65,args[1].payload,&equal)==XR_XIR_RUN_OK && equal==!different);
        CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK);
        CHECK(output.type==XR_XIR_BOOL && !output.reserved && output.payload==!different);output=(XrXirValue){0};
    }
    request.function=literal;request.arguments=NULL;request.argument_count=0;
    size_t start=source_fixture_compile_attempts;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK);output=(XrXirValue){0};
    size_t sites=source_fixture_compile_attempts-start;CHECK(sites>0);
    XrCompileResourceStats baseline=reference_stats(context);
    for (size_t f=0;f<sites;++f) {
        source_fixture_compile_injected=false;source_fixture_compile_fail_at=source_fixture_compile_attempts+f;
        CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OUT_OF_MEMORY);
        CHECK(source_fixture_compile_injected && !output.type && !output.reserved && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
        source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK);
        CHECK(output.type==XR_XIR_RUNE && !output.reserved && output.payload==65);output=(XrXirValue){0};
    }
    xr_xir_compile_artifact_free(lowered);
    fprintf(stderr,"CTFE Rune canonical boundary/conversion/equality leaf and calls; output4/minus3; evaluator FI=%zu same-owner retry; physical0 after last owner\n",sites);
}

static void reference_cell_leaf(const XrXirCompileContext *context,XrXirSourceResult *source) {
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t flag=reference_named(module,"flag"),divide=reference_named(module,"divide");
    const XrXirFunction *body=&module->functions[flag];unsigned operations=0;
    for (uint32_t i=0;i<body->instruction_count;++i) {
        XrXirOp op=body->instructions[i].op;
        if (op==XR_XIR_CELL_NEW || op==XR_XIR_CELL_LOCAL_WRITE) operations|=1;
        if (op==XR_XIR_CELL_READ) operations|=2;
        if (op==XR_XIR_CELL_WRITE || op==XR_XIR_CELL_LOCAL_WRITE) operations|=4;
    }
    CHECK(operations==7);
    XrXirValue argument={XR_XIR_BOOL,0,1},output={0};
    XrXirCtfeRequest request={.artifact=lowered,.function=flag,
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.arguments=&argument,.argument_count=1,
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,1}};
    memcpy(request.checked_identity,identity,sizeof(identity));
    XrCompileResourceStats baseline=reference_stats(context);
    size_t start=source_fixture_compile_attempts;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK);
    CHECK(output.type==XR_XIR_BOOL && !output.reserved && !output.payload);output=(XrXirValue){0};
    size_t sites=source_fixture_compile_attempts-start;CHECK(sites>0);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    for (size_t f=0;f<sites;++f) {
        source_fixture_compile_injected=false;source_fixture_compile_fail_at=source_fixture_compile_attempts+f;
        CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OUT_OF_MEMORY);
        CHECK(source_fixture_compile_injected && !output.type && !output.reserved && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
        source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK);
        CHECK(output.type==XR_XIR_BOOL && !output.reserved && !output.payload);output=(XrXirValue){0};
    }
    request.limits.steps=1;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_STEP_LIMIT);
    CHECK(!output.type && !output.reserved && !output.payload && reference_stats(context).live_bytes==baseline.live_bytes);
    request.limits.steps=XR_XIR_CTFE_MAX_STEPS;request.function=divide;request.limits.output_bytes=8;
    argument=(XrXirValue){XR_XIR_I64,0,0};
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_DIVIDE_BY_ZERO);
    CHECK(!output.type && !output.reserved && !output.payload && reference_stats(context).live_bytes==baseline.live_bytes);
    argument.payload=2;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK);
    CHECK(output.type==XR_XIR_I64 && !output.reserved && !output.payload);output=(XrXirValue){0};
    xr_xir_compile_artifact_free(lowered);
    fprintf(stderr,"CTFE real Cell leaf NEW/READ/WRITE; evaluator FI=%zu same-owner retry; step/division failure clears owned frame; physical0 after last owner\n",sites);
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    const CtfeReferenceCase *selected = NULL;
    for (size_t i = 0; i < sizeof(reference_cases) / sizeof(reference_cases[0]); ++i) {
        if (!strcmp(argv[1], reference_cases[i].name)) selected = &reference_cases[i];
    }
    CHECK(selected && !source_fixture_compile_live && !source_fixture_compile_bytes);
    XrXirCompileContext context = {0};
    CHECK(xr_compile_resources_new(&reference_limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    context.limits = xr_xir_compile_default_limits();
    XrCompileResourceStats before = reference_stats(&context);
    XrXirSourceResult source = {0};
    XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = reference_source(&context, selected->name, &source, &diagnostic);
    if (status != selected->status) fprintf(stderr, "Source reference %s: %u at %d:%d %s\n",
        selected->name, (unsigned)status, diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == selected->status);
    if (status == XR_XIR_OK) {
        if (!strcmp(selected->name, "cell_leaf")) reference_cell_leaf(&context,&source);
        else if (!strcmp(selected->name, "rune_direct")) reference_rune_direct(&context,&source);
        else if (!strcmp(selected->name, "unit_direct")) reference_unit_direct(&context,&source);
        else if (!strcmp(selected->name, "leaf_locals")) reference_leaf_locals(&context, &source);
        else if (!strcmp(selected->name, "mutual_depth_direct")) reference_direct_depth(&context, &source);
        else reference_accept(&context, &source, selected);
    }
    else {
        fprintf(stderr, "Source reference refusal %s: %u at %d:%d %s\n",
            selected->name, (unsigned)diagnostic.status, diagnostic.line, diagnostic.column, diagnostic.message);
        CHECK(diagnostic.status == selected->status && diagnostic.line == selected->line &&
            diagnostic.column > 0 && (!selected->reason || strstr(diagnostic.message, selected->reason)));
        CHECK(!source.checked && !source.snapshot);
    }
    xr_xir_compile_source_result_free(&source);
    XrCompileResourceStats spent = reference_stats(&context);
    CHECK(spent.live_bytes == before.live_bytes && spent.work >= before.work &&
        spent.allocated_bytes >= before.allocated_bytes);
    CHECK(spent.allocated_bytes <= reference_limits.allocated_bytes &&
        spent.peak_bytes <= reference_limits.live_bytes && spent.work <= reference_limits.work);
    xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr, "CTFE reference %s: status=%u expected=%lld allocated=%llu peak=%llu work=%llu physical=0/0\n",
        selected->name, (unsigned)status, (long long)selected->value,
        (unsigned long long)spent.allocated_bytes, (unsigned long long)spent.peak_bytes,
        (unsigned long long)spent.work);
    return 0;
}
