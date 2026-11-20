/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_c_value_types.c - Receiving layouts survive producers and compile as typed C
 */
#include "lir/xlir_builder.h"
#include "lir/xlir_c.h"
#include "lir/xlir_value_layout.h"
#include "lir/xlir_callable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
#include "../unit/xir/xir_sdk_resource_test.h"
#define LIR(c) CHECK((c) == XR_LIR_OK)
#define MANAGED(c) CHECK((c) == XRT_MANAGED_OK)
#define LAYOUT(c) CHECK((c) == XR_LIR_VALUE_LAYOUT_OK)

typedef struct Fixture {
    XrLirValueLayouts *layouts;
    XrtBillingOwner *billing;
    XrLirType record, nullable, pod, empty, unit, nested, callable;
    size_t record_row, nullable_row, pod_row, empty_row, unit_row, callable_row;
    XrtBillingStats initial;
} Fixture;
static size_t row_of(const XrLirValueLayouts *set, XrLirType type) {
    for (size_t i = 0; i < xr_lir_value_layouts_count(set); ++i) {
        XrLirValueLayoutKey key; const XrtValuePlan *plan;
        LAYOUT(xr_lir_value_layouts_at(set, i, &key, &plan));
        if (key.type == type && !key.array_element) return i + 1;
    }
    CHECK(false); return SIZE_MAX;
}
static Fixture fixture(void) {
    Fixture f = {0}; XrLirModule *m = NULL;
    LIR(xr_lir_module_create(NULL, &m));
    char key[] = "name";
    XrLirRecordField fields[] = {{"flag", 4, XR_LIR_I1, 0, 0}, {key, 4, XR_LIR_STRING, 0, 0}, {"value", 5, XR_LIR_I64, 0, 0}};
    LIR(xr_lir_record_type(m,
        &(XrLirRecordIdentity){.module="tests.lir.c_value_types", .module_length=sizeof("tests.lir.c_value_types")-1,
            .name="Record", .name_length=sizeof("Record")-1},
        fields, 3, &f.record));
    memset(key, 'x', 4);
    LIR(xr_lir_nullable_type(m, f.record, &f.nullable));
    LIR(xr_lir_nullable_type(m, f.nullable, &f.nested));
    XrLirType optional_string;
    LIR(xr_lir_nullable_type(m, XR_LIR_STRING, &optional_string));
    XrLirRecordField pod[] = {{"a", 1, XR_LIR_I1, 0, 0}, {"b", 1, XR_LIR_F64, 0, 0}, {"c", 1, XR_LIR_I64, 0, 0}};
    LIR(xr_lir_record_type(m,
        &(XrLirRecordIdentity){.module="tests.lir.c_value_types", .module_length=sizeof("tests.lir.c_value_types")-1,
            .name="Pod", .name_length=sizeof("Pod")-1},
        pod, 3, &f.pod));
    LIR(xr_lir_record_type(m,
        &(XrLirRecordIdentity){.module="tests.lir.c_value_types", .module_length=sizeof("tests.lir.c_value_types")-1,
            .name="Empty", .name_length=sizeof("Empty")-1},
        NULL, 0, &f.empty));
    LIR(xr_lir_nullable_type(m, XR_LIR_UNIT, &f.unit));
    XrLirType fixed, tuple, optional_f32, optional_empty;
    LIR(xr_lir_fixed_array_type(m, XR_LIR_U8, 17, &fixed));
    LIR(xr_lir_tuple_i64_type(m, 3, &tuple));
    LIR(xr_lir_nullable_type(m, XR_LIR_F32, &optional_f32));
    LIR(xr_lir_nullable_type(m, f.empty, &optional_empty));
    XrLirCallableParameter argument = {.type=XR_LIR_I64};
    XrLirCallableType signature = {.parameters=&argument, .parameter_count=1, .result=XR_LIR_I64};
    LIR(xr_lir_callable_type(m, &signature, &f.callable));
    LIR(xr_lir_module_finish(m, NULL));
    XrLirValueLayoutKey roots[] = {{f.nested, 0, 0}, {optional_string, 0, 0}, {f.pod, 0, 0}, {optional_empty, 0, 0},
        {f.unit, 0, 0}, {fixed, 0, 0}, {tuple, 0, 0}, {optional_f32, 0, 0}, {XR_LIR_ARRAY, XR_LIR_I64, 0},
        {XR_LIR_ARRAY, XR_LIR_STRING, 0}, {XR_LIR_ARRAY, XR_LIR_U8, 0}, {XR_LIR_ARRAY, XR_LIR_U64, 0},
        {XR_LIR_MAP_STRING_I64, 0, 0}, {XR_LIR_I8, 0, 0}, {XR_LIR_I16, 0, 0}, {XR_LIR_I32, 0, 0},
        {XR_LIR_U8, 0, 0}, {XR_LIR_U16, 0, 0}, {XR_LIR_U32, 0, 0}, {XR_LIR_U64, 0, 0}, {f.callable, 0, 0}};
    MANAGED(xrt_billing_owner_new(NULL, SIZE_MAX, &f.billing));
    f.initial = xrt_billing_owner_stats(f.billing);
    LAYOUT(xr_lir_value_layouts_create(m, roots, sizeof(roots) / sizeof(roots[0]), f.billing, &f.layouts));
    f.record_row = row_of(f.layouts, f.record); f.nullable_row = row_of(f.layouts, f.nullable);
    f.pod_row = row_of(f.layouts, f.pod); f.empty_row = row_of(f.layouts, f.empty);
    f.unit_row = row_of(f.layouts, f.unit);
    f.callable_row = row_of(f.layouts, f.callable);
    xr_lir_module_destroy(m);
    return f;
}
static bool cancelled(const void *context) { return *(const bool *)context; }
static void empty_inventory(void) {
    XrLirModule *m = NULL; XrLirValueLayouts *set = NULL; XrtBillingOwner *billing = NULL;
    LIR(xr_lir_module_create(NULL, &m)); LIR(xr_lir_module_finish(m, NULL));
    MANAGED(xrt_billing_owner_new(NULL, SIZE_MAX, &billing));
    LAYOUT(xr_lir_value_layouts_create(m, NULL, 0, billing, &set));
    xr_lir_module_destroy(m);
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited);
    XrLirCOutput output = {0};
    CHECK(xr_lir_emit_c_value_types(&(XrLirCValueTypesRequest){set, resources, "empty-values", NULL},
        &output, NULL) == XR_LIR_C_OK);
    CHECK(output.length && !strstr(output.bytes, "XrlValue_"));
    xr_lir_value_layouts_destroy(set); xrt_billing_owner_drop(billing);
    xr_compile_resources_release(resources); xr_lir_c_output_free(&output);
    CHECK(!runtime_live && !runtime_bytes);
    puts("empty receiving inventory emits a valid C declaration unit without payload homes");
}
static void failures(const XrLirCValueTypesRequest *request) {
    XrLirCOutput output = {0}; XrLirCDiagnostic diagnostic;
    XrCompileResourceStats before = sdk_stats(request->resources);
    CHECK(xr_lir_emit_c_value_types(NULL, &output, &diagnostic) == XR_LIR_C_BAD_ARGUMENT);
    CHECK(!output.bytes && !output.length && !output.entry_result);
    size_t start = runtime_attempts;
    CHECK(xr_lir_emit_c_value_types(request, &output, &diagnostic) == XR_LIR_C_OK);
    size_t allocations = runtime_attempts - start;
    xr_lir_c_output_free(&output);
    CHECK(allocations > 3);
    for (size_t i = 0; i < allocations; ++i) {
        runtime_fail_at = runtime_attempts + i;
        CHECK(xr_lir_emit_c_value_types(request, &output, &diagnostic) == XR_LIR_C_OUT_OF_MEMORY);
        CHECK(diagnostic.status == XR_LIR_C_OUT_OF_MEMORY && !output.bytes && !output.length);
        CHECK(sdk_stats(request->resources).held_bytes == before.held_bytes);
        runtime_fail_at = SIZE_MAX;
    }
    XrLirCValueTypesRequest altered = *request;
    bool stop = true;
    XrCompileRequestOptions options = {cancelled, &stop, XR_COMPILE_DEADLINE_NONE, 0};
    XrCompileRequestControl control = {0};
    CHECK(xr_compile_request_control_init(&options, &control) == XR_COMPILE_RESOURCE_OK);
    altered.control = &control;
    CHECK(xr_lir_emit_c_value_types(&altered, &output, &diagnostic) == XR_LIR_C_CANCELLED);
    CHECK(!output.bytes && sdk_stats(request->resources).held_bytes == before.held_bytes);
    output.entry_layout.size = 1;
    CHECK(xr_lir_emit_c_value_types(request, &output, &diagnostic) == XR_LIR_C_BAD_ARGUMENT);
    CHECK(output.entry_layout.size == 1 && !output.bytes);
    puts("typed C allocation failures and cancellation preserve empty output and exact resources");
}
static void write_output(const char *path, const XrLirCOutput *output, const Fixture *f) {
    FILE *stream = fopen(path, "wb"); CHECK(stream);
    CHECK(fwrite(output->bytes, 1, output->length, stream) == output->length);
    CHECK(fprintf(stream,
        "static XrlValue_%zu identity(XrlValue_%zu value) { return value; }\n"
        "int main(void) {\n"
        "    XrlValue_%zu input = {1, NULL, INT64_C(42)};\n"
        "    XrlValue_%zu result = identity(input);\n"
        "    XrlValue_%zu some = {1, result};\n"
        "    XrlValue_%zu none = {0};\n"
        "    XrlValue_%zu pod = {1, 1.5, INT64_C(-7)};\n"
        "    XrlValue_%zu empty = {0};\n"
        "    XrlValue_%zu unit = {1};\n"
        "    const int code_entry = 7;\n"
        "    XrlValue_%zu callable = {&code_entry, UINTPTR_MAX, NULL};\n"
        "    XrtFunction copied = callable;\n"
        "    return !(some.tag && some.payload.f0 && !some.payload.f1 && some.payload.f2 == INT64_C(42)\n"
        "        && !none.tag && !none.payload.f1 && pod.f1 == 1.5 && pod.f2 == INT64_C(-7)\n"
        "        && !empty.padding && unit.tag && copied.entry == &code_entry\n"
        "        && copied.environment == UINTPTR_MAX && !copied.owner);\n}\n",
        f->record_row, f->record_row, f->record_row, f->record_row, f->nullable_row,
        f->nullable_row, f->pod_row, f->empty_row, f->unit_row, f->callable_row) > 0);
    CHECK(fclose(stream) == 0);
}
int main(int argc, char **argv) {
    (void)sdk_fixture_malloc; (void)sdk_fixture_free;
    CHECK(argc == 1 || argc == 2);
    empty_inventory();
    Fixture f = fixture();
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited);
    XrLirCValueTypesRequest request = {f.layouts, resources, "receiving-value-types", NULL};
    failures(&request);
    XrLirCOutput output = {0}; XrLirCDiagnostic diagnostic;
    CHECK(xr_lir_emit_c_value_types(&request, &output, &diagnostic) == XR_LIR_C_OK);
    CHECK(output.bytes[output.length] == '\0' && !output.entry_result && !output.entry_layout.size);
    CHECK(strstr(output.bytes, "typedef XrtManagedHeader *") && strstr(output.bytes, "nullable payload"));
    CHECK(strstr(output.bytes, "typedef XrtFunction ") && strstr(output.bytes, "function owner"));
    CHECK(!strstr(output.bytes, "XrtBoxed") && !strstr(output.bytes, "Xir") && !strstr(output.bytes, "../src") &&
        !strstr(output.bytes, "xray_native_runtime.h") && !strstr(output.bytes, "({"));
    xr_lir_value_layouts_destroy(f.layouts);
    CHECK(xrt_billing_owner_stats(f.billing).bytes == f.initial.bytes &&
        xrt_billing_owner_stats(f.billing).blocks == f.initial.blocks);
    xrt_billing_owner_drop(f.billing);
    xr_compile_resources_release(resources);
    if (argc == 2) write_output(argv[1], &output, &f);
    xr_lir_c_output_free(&output);
    CHECK(!runtime_live && !runtime_bytes);
    puts("typed inline Record/Nullable declarations survive module, layouts, billing and caller resources");
    return 0;
}
