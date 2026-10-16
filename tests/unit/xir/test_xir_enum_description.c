/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_enum_description.c - Description identity, fees and separate authority
 */
#include "xir/xxir_types.h"
#include "base/xcompile_resources.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_enum_metadata_fixture.h"
static XrXirCompileContext description_context(uint64_t work) {
    XrXirCompileContext context = {0};
    const XrCompileResourceLimits limits = {67108864,8388608,work};
    CHECK(xr_compile_resources_new(&limits,&context.resources) == XR_COMPILE_RESOURCE_OK);
    context.limits = xr_xir_compile_default_limits(); return context;
}
static XrCompileResourceStats description_stats(XrXirCompileContext *context) {
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(context->resources,&stats) == XR_COMPILE_RESOURCE_OK); return stats;
}
static void description_expect(const XrXirTypeDescription *view, const XrXirNominalType *instance) {
    CHECK(view->kind == XR_XIR_TYPE_NOMINAL && view->nominal_kind == XR_XIR_NOMINAL_ENUM);
    CHECK(view->declaration == 0 && view->instance == instance && view->variant_count == 3);
    CHECK(instance->argument_count == 0 && !instance->arguments &&
        instance->field_count == 2 && instance->fields[0] == XR_XIR_I64 && instance->fields[1] == XR_XIR_STRING);
    CHECK(view->module.length == 5 && !memcmp(view->module.bytes,"alpha",5));
    CHECK(view->name.length == 6 && !memcmp(view->name.bytes,"Choice",6));
    CHECK(view->variants[0].name.length == 4 && !memcmp(view->variants[0].name.bytes,"None",4));
    CHECK(view->variants[1].name.length == 4 && !memcmp(view->variants[1].name.bytes,"Left",4));
    CHECK(view->variants[2].name.length == 5 && !memcmp(view->variants[2].name.bytes,"Right",5));
    CHECK(view->variants[0].field_count == 0 && view->variants[1].field_begin == 0 &&
        view->variants[1].field_count == 1 && view->variants[2].field_begin == 1 && view->variants[2].field_count == 1);
}
static void description_budget(const XrXirTypes *types) {
    for (unsigned limit = 0; limit < 2; ++limit) {
        XrXirCompileContext context = description_context(1 + limit);
        XrXirTypeDescription view = {0}; view.variant_count = 99;
        unsigned char saved[sizeof(view)]; memcpy(saved,&view,sizeof(view));
        XrCompileResourceStats before = description_stats(&context);
        XrXirStatus status = xr_xir_compile_type_description(&context,types,(XrXirType)256,&view);
        CHECK(status == (limit ? XR_XIR_OK : XR_XIR_BUDGET));
        if (!limit) CHECK(!memcmp(saved,&view,sizeof(view)));
        XrCompileResourceStats after = description_stats(&context);
        CHECK(after.work == before.work + limit && after.allocated_bytes == before.allocated_bytes &&
            after.live_bytes == before.live_bytes && after.allocation_count == before.allocation_count);
        memcpy(saved,&view,sizeof(view));
        CHECK(xr_xir_compile_type_description(&context,types,(XrXirType)256,&view) == XR_XIR_BUDGET);
        CHECK(!memcmp(saved,&view,sizeof(view)) && description_stats(&context).work == after.work);
        xr_compile_resources_release(context.resources);
    }
}
static void description_authority(XrXirCompileContext *context, XrXirTypes *types,
    EnumMetadataFixture *fixture) {
    uint32_t dependency = 0;
    XrXirSourceModule modules[] = {{"alpha",5,NULL,0,0, 0,XR_XIR_MODULE_FRESH_STORAGE},{"beta",4,&dependency,1,1, 0,XR_XIR_MODULE_FRESH_STORAGE}};
    XrXirFunctionIdentity callers[] = {{.module=0},{.module=1}};
    XrXirDeclarations declarations = {modules,2,callers,NULL,0,NULL,0,1,1,NULL};
    XrXirModule module = {.function_count=2,.declarations=&declarations,.types=types};
    XrXirTypeDescription view = {0}; fixture->declaration.exported = 0;
    CHECK(xr_xir_compile_type_description(context,types,(XrXirType)256,&view) == XR_XIR_OK);
    CHECK(xr_xir_compile_nominal_access(context,&module,1,view.declaration,0,XR_XIR_NOMINAL_TYPE) == XR_XIR_BAD_TYPE);
    fixture->declaration.exported = 1;
    CHECK(xr_xir_compile_nominal_access(context,&module,1,view.declaration,0,XR_XIR_NOMINAL_TYPE) == XR_XIR_OK);
    modules[1].dependencies = NULL; modules[1].dependency_count = 0;
    CHECK(xr_xir_compile_nominal_access(context,&module,1,view.declaration,0,XR_XIR_NOMINAL_TYPE) == XR_XIR_BAD_TYPE);
}
int main(void) {
    EnumMetadataFixture fixture; enum_metadata_fixture(&fixture);
    XrXirType fields[] = {XR_XIR_I64,XR_XIR_STRING};
    XrXirTypeNode node = {.kind=XR_XIR_TYPE_NOMINAL,
        .nominal={.declaration=0,.fields=fields,.field_count=2}};
    XrXirTypes types = {&node,1,&fixture.table,NULL};
    XrXirCompileContext context = description_context(128000000);
    uint64_t baseline_live = description_stats(&context).live_bytes;
    CHECK(xr_xir_compile_types_structure_verify(&context,&types) == XR_XIR_OK);
    XrXirTypeDescription view = {0};
    CHECK(xr_xir_compile_type_description(&context,&types,(XrXirType)256,&view) == XR_XIR_OK);
    description_expect(&view,&node.nominal); CHECK(view.definition == &fixture.declaration);
    XrXirTypeDescription original = view;
    CHECK(xr_xir_compile_type_description(&context,&types,(XrXirType)255,&view) == XR_XIR_BAD_TYPE);
    CHECK(view.variant_count == original.variant_count && view.variants == original.variants);
    uint32_t saved_index = node.nominal.declaration; node.nominal.declaration = 1;
    CHECK(xr_xir_compile_type_description(&context,&types,(XrXirType)256,&view) == XR_XIR_BAD_TYPE);
    node.nominal.declaration = saved_index;
    const XrXirNominalVariant *saved_variants = fixture.declaration.variants;
    fixture.declaration.variants = NULL;
    CHECK(xr_xir_compile_type_description(&context,&types,(XrXirType)256,&view) == XR_XIR_BAD_STRUCTURE);
    fixture.declaration.variants = saved_variants;
    description_budget(&types); description_authority(&context,&types,&fixture);
    XrXirNominalTable *projected = NULL;
    CHECK(xr_xir_compile_nominal_project(&context,&fixture.table,&projected) == XR_XIR_OK);
    memset(&fixture,0xcc,sizeof(fixture)); types.nominals = projected;
    CHECK(xr_xir_compile_types_structure_verify(&context,&types) == XR_XIR_OK);
    CHECK(xr_xir_compile_type_description(&context,&types,(XrXirType)256,&view) == XR_XIR_OK);
    description_expect(&view,&node.nominal); CHECK(!view.definition);
    xr_xir_compile_nominal_free(projected);
    CHECK(description_stats(&context).live_bytes == baseline_live);
    xr_compile_resources_release(context.resources);
    puts("canonical enum descriptions: declaration/identity, producer death, exact budget and separate authority");
    return 0;
}
