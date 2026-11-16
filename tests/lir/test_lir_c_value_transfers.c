/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_c_value_transfers.c - Final IR events become executable C transfers
 */
#include "lir/xlir_builder.h"
#include "lir/xlir_c.h"
#include "lir/xlir_value_homes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
#include "../unit/xir/xir_sdk_resource_test.h"
#define OK(c) CHECK((c) == XR_LIR_OK)
#define MANAGED(c) CHECK((c) == XRT_MANAGED_OK)
#define LAYOUT(c) CHECK((c) == XR_LIR_VALUE_LAYOUT_OK)
typedef struct Fixture {
    XrLirValueLayouts *layouts;
    XrLirValueHomes *homes;
    uint32_t copies[2], loop[2], exit[2];
    uint32_t pod_type;
} Fixture;
static Fixture fixture(bool pod) {
    Fixture f = {0}; XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType optional = 0, record = 0;
    if (!pod) OK(xr_lir_nullable_type(m, XR_LIR_STRING, &optional));
    XrLirRecordField fields[] = {{"first", 5, optional}, {"score", 5, XR_LIR_I64}, {"second", 6, optional}};
    if (pod) fields[0] = (XrLirRecordField){"score", 5, XR_LIR_I64};
    OK(xr_lir_record_type(m, fields, pod ? 1 : 3, &record));
    XrLirType plain = record;
    if (!pod) OK(xr_lir_record_type(m, &(XrLirRecordField){"score", 5, XR_LIR_I64}, 1, &plain));
    XrLirType params[] = {record, record, XR_LIR_I1, XR_LIR_I64, XR_LIR_I64, plain};
    XrLirFunction fn = {0};
    XrLirFunctionSpec spec = {.name="transport", .name_length=9, .parameters=params, .parameter_count=pod ? 5 : 6, .result=record};
    OK(xr_lir_declare_function(m, &spec, &fn)); OK(xr_lir_define_function(m, fn));
    XrLirBlock blocks[3] = {{0}}; XrLirValue inputs[6], copies[2], loop[5] = {{0}}, exit[5] = {{0}}, plain_copy = {0};
    OK(xr_lir_add_block(m, fn, NULL, 0, &blocks[0]));
    XrLirType edge_types[] = {record, record, XR_LIR_I64, XR_LIR_I64, plain};
    for (unsigned b = 1; b < 3; ++b) OK(xr_lir_add_block(m, fn, edge_types, pod ? 4 : 5, &blocks[b]));
    OK(xr_lir_set_entry(m, fn, blocks[0]));
    for (uint32_t p = 0; p < spec.parameter_count; ++p) OK(xr_lir_function_parameter(m, fn, p, &inputs[p]));
    for (unsigned p = 0; p < 2; ++p) {
        XrLirInstruction retain = {.op=XR_LIR_VALUE_RETAIN, .type=record, .operands=&inputs[p], .operand_count=1};
        OK(xr_lir_emit_value(m, blocks[0], &retain, &copies[p]));
        OK(xr_lir_block_parameter(m, blocks[1], p, &loop[p]));
        OK(xr_lir_block_parameter(m, blocks[2], p, &exit[p]));
        f.copies[p] = copies[p].id; f.loop[p] = loop[p].id; f.exit[p] = exit[p].id;
    }
    for (unsigned p = 2; p < 4; ++p) {
        OK(xr_lir_block_parameter(m, blocks[1], p, &loop[p]));
        OK(xr_lir_block_parameter(m, blocks[2], p, &exit[p]));
    }
    if (!pod) {
        XrLirInstruction retain = {.op=XR_LIR_VALUE_RETAIN, .type=plain, .operands=&inputs[5], .operand_count=1};
        OK(xr_lir_emit_value(m, blocks[0], &retain, &plain_copy));
        OK(xr_lir_block_parameter(m, blocks[1], 4, &loop[4]));
        OK(xr_lir_block_parameter(m, blocks[2], 4, &exit[4]));
    }
    XrLirValue initial[] = {copies[0], copies[1], inputs[3], inputs[4], plain_copy};
    XrLirInstruction branch = {.op=XR_LIR_BR, .type=XR_LIR_UNIT, .edge_count=1,
        .edges={{blocks[1], initial, pod ? 4 : 5}}};
    OK(xr_lir_set_terminator(m, blocks[0], &branch));
    XrLirValue swapped[] = {loop[1], loop[0], loop[3], loop[2], loop[4]};
    branch = (XrLirInstruction){.op=XR_LIR_CONDBR, .type=XR_LIR_UNIT, .operands=&inputs[2], .operand_count=1,
        .edge_count=2, .edges={{blocks[1], swapped, pod ? 4 : 5}, {blocks[2], loop, pod ? 4 : 5}}};
    OK(xr_lir_set_terminator(m, blocks[1], &branch));
    XrLirInstruction drop = {.op=XR_LIR_VALUE_DROP, .type=XR_LIR_UNIT, .operands=&exit[1], .operand_count=1};
    OK(xr_lir_emit_effect(m, blocks[2], &drop));
    if (!pod) { drop.operands = &exit[4]; OK(xr_lir_emit_effect(m, blocks[2], &drop)); }
    XrLirInstruction ret = {.op=XR_LIR_RET, .type=XR_LIR_UNIT, .operands=&exit[0], .operand_count=1};
    OK(xr_lir_set_terminator(m, blocks[2], &ret));
    XrLirType callable = 0;
    OK(xr_lir_callable_type(m, &(XrLirCallableType){.result=XR_LIR_I64}, &callable));
    OK(xr_lir_module_finish(m, NULL));
    XrtBillingOwner *billing = NULL; MANAGED(xrt_billing_owner_new(NULL, SIZE_MAX, &billing));
    /* A module image can include a Fn even when this function has no Fn value.
     * Its declaration still needs the canonical header and must not add RC. */
    XrLirValueLayoutKey roots[] = {{record, 0, 0}, {XR_LIR_I1, 0, 0}, {plain, 0, 0}, {callable, 0, 0}};
    LAYOUT(xr_lir_value_layouts_create(m, roots, 4, billing, &f.layouts));
    for (size_t i = 0; i < xr_lir_value_layouts_count(f.layouts); ++i) {
        XrLirValueLayoutKey key; const XrtValuePlan *plan;
        LAYOUT(xr_lir_value_layouts_at(f.layouts, i, &key, &plan));
        if (key.type == plain) f.pod_type = (uint32_t)i + 1;
    }
    const XrLirOwnershipTypeFact *facts = NULL; uint32_t count = 0;
    LAYOUT(xr_lir_value_layouts_ownership(f.layouts, &facts, &count));
    XrLirOwnershipPlan *ownership = NULL;
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){m, facts, count}, &ownership, NULL));
    LAYOUT(xr_lir_value_homes_create(&(XrLirValueHomesRequest){m, fn, f.layouts, ownership}, &f.homes));
    CHECK(f.homes && f.homes->owned_frame_needed == !pod && f.homes->edge_count == 3);
    for (uint32_t p = 0; p < 2; ++p) {
        const XrLirValueHome *home = &f.homes->values[inputs[p].id];
        CHECK(home->parameter == p + 1 && home->offset == SIZE_MAX && home->first_owner == SIZE_MAX &&
            home->temporary_offset == SIZE_MAX && home->temporary_first_owner == SIZE_MAX);
    }
    xr_lir_ownership_plan_destroy(ownership); xr_lir_module_destroy(m); xrt_billing_owner_drop(billing);
    return f;
}
static void failures(const XrLirCValueTransfersRequest *request) {
    XrLirCOutput output = {0}; XrLirCDiagnostic diagnostic;
    size_t before = runtime_attempts;
    CHECK(xr_lir_emit_c_value_transfers(request, &output, &diagnostic) == XR_LIR_C_OK);
    size_t attempts = runtime_attempts - before; CHECK(attempts > 3);
    xr_lir_c_output_free(&output);
    uint64_t held = sdk_stats(request->resources).held_bytes;
    for (size_t i = 0; i < attempts; ++i) {
        runtime_fail_at = runtime_attempts + i;
        CHECK(xr_lir_emit_c_value_transfers(request, &output, &diagnostic) == XR_LIR_C_OUT_OF_MEMORY);
        CHECK(!output.bytes && !output.length && sdk_stats(request->resources).held_bytes == held);
        runtime_fail_at = SIZE_MAX;
    }
}
static void write_home(FILE *stream, const XrLirValueHome *home) {
    CHECK(fprintf(stream, "{\"offset\":%zu,\"size\":%zu,\"first\":%zu,\"temporary\":%zu,\"temporary_first\":%zu}",
        home->offset, home->size, home->first_owner, home->temporary_offset, home->temporary_first_owner) > 0);
}
static void write_fixture(const char *path, const Fixture *f, const XrLirCOutput *output) {
    FILE *stream = fopen(path, "wb"); CHECK(stream);
    CHECK(fwrite(output->bytes, 1, output->length, stream) == output->length); CHECK(fclose(stream) == 0);
    char manifest[1024]; int length = snprintf(manifest, sizeof(manifest), "%s.json", path);
    CHECK(length > 0 && (size_t)length < sizeof(manifest));
    stream = fopen(manifest, "wb"); CHECK(stream);
    CHECK(fprintf(stream, "{\"type\":%u,\"pod_type\":%u,\"copies\":[", f->homes->result_type, f->pod_type) > 0);
    for (unsigned i = 0; i < 2; ++i) {
        if (i) CHECK(fputc(',', stream) != EOF);
        write_home(stream, &f->homes->values[f->copies[i]]);
    }
    CHECK(fputs("],\"loop\":[", stream) >= 0);
    for (unsigned i = 0; i < 2; ++i) {
        if (i) CHECK(fputc(',', stream) != EOF);
        write_home(stream, &f->homes->values[f->loop[i]]);
    }
    CHECK(fputs("],\"exit\":[", stream) >= 0);
    for (unsigned i = 0; i < 2; ++i) {
        if (i) CHECK(fputc(',', stream) != EOF);
        write_home(stream, &f->homes->values[f->exit[i]]);
    }
    CHECK(fputs("]}\n", stream) >= 0); CHECK(fclose(stream) == 0);
}
static void run(bool pod, const char *path) {
    Fixture f = fixture(pod);
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited);
    XrLirCValueTransfersRequest request = {f.layouts, f.homes, resources, "value-transfers", NULL};
    failures(&request);
    XrLirCOutput output = {0}; XrLirCDiagnostic diagnostic;
    CHECK(xr_lir_emit_c_value_transfers(&request, &output, &diagnostic) == XR_LIR_C_OK);
    CHECK(strstr(output.bytes, "xrl_edge_1_2_1") && !strstr(output.bytes, "Xir") && !strstr(output.bytes, "../src"));
    CHECK(strstr(output.bytes, "#include \"xray_native_function.h\"") && strstr(output.bytes, "typedef XrtFunction"));
    if (pod) CHECK(!strstr(output.bytes, "XrtManagedRoot") && !strstr(output.bytes, "xrt_place_") && !strstr(output.bytes, "XrlLanding_"));
    else CHECK(strstr(output.bytes, "xrt_place_edge_transfer") && strstr(output.bytes, "XRT_PLACE_TRANSFER_MOVE") &&
        strstr(output.bytes, "xrl_return_1_3_3"));
    if (path) write_fixture(path, &f, &output);
    xr_lir_value_homes_destroy(f.homes); xr_lir_value_layouts_destroy(f.layouts);
    xr_compile_resources_release(resources);
    CHECK(output.bytes[output.length] == '\0'); xr_lir_c_output_free(&output);
    CHECK(!runtime_live && !runtime_bytes);
}
int main(int argc, char **argv) {
    (void)sdk_fixture_malloc; (void)sdk_fixture_free;
    CHECK(argc == 1 || (argc == 3 && (!strcmp(argv[1], "managed") || !strcmp(argv[1], "pod"))));
    if (argc == 1) { run(false, NULL); run(true, NULL); }
    else run(!strcmp(argv[1], "pod"), argv[2]);
    puts("final Record/Nullable events emit typed frame, selected-edge and caller-landing C helpers");
    puts("borrowed parameters reserve no owners; POD transport creates no Root; emitter OOM refunds exactly");
    return 0;
}
