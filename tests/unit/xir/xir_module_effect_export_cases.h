/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_module_effect_export_cases.h - Detached numeric facts and failure ownership
 */
#ifndef XIR_MODULE_EFFECT_EXPORT_CASES_H
#define XIR_MODULE_EFFECT_EXPORT_CASES_H
static bool module_export_contains(XirModuleEffectExportView view, XirModuleEffectSpan span,
    XirModuleEffectTermKind kind, uint32_t identity) {
    CHECK(span.first <= view.term_count && span.count <= view.term_count - span.first);
    for (size_t n = 0; n < span.count; ++n)
        if (view.terms[span.first + n].kind == kind && view.terms[span.first + n].identity == identity) return true;
    return false;
}
static void module_export_retirement(void) {
    EffectMark baseline = effect_mark(); XrXirCompileContext context = effect_owner_new(effect_caps());
    ModuleBorrowFixture fixture; module_borrow_fixture(&fixture, false);
    XrXirConstruction *construction = NULL; XrXirArtifact *artifact = NULL;
    CHECK(xir_fixture_module_construction(&context, &fixture.module, &construction) == XR_XIR_OK);
    CHECK(xr_xir_compile_check_v2(&context, &fixture.module, construction, &artifact, NULL) == XR_XIR_OK);
    XirModuleEffectExport *owned = NULL; attempts = 0;
    CHECK(xir_effects_module_export_copy(artifact, &owned) == XR_XIR_OK && owned); size_t sites = attempts;
    XirModuleEffectExportView view;
    CHECK(xir_effects_module_export_view(owned, &view) == XR_XIR_OK && view.module_access_complete && view.slot_count == 2);
    CHECK(module_export_contains(view, view.functions[1].access, XIR_MODULE_EFFECT_SLOT, 1));
    XirModuleEffectExport *preserved = owned;
    CHECK(xir_effects_module_export_copy(artifact, &owned) == XR_XIR_BAD_STRUCTURE && owned == preserved);
    xir_effects_module_export_free(owned); owned = NULL; EffectMark source = effect_mark();
    XirModuleEffectExportView untouched = {.term_count=42};
    CHECK(xir_effects_module_export_view(NULL, &untouched) == XR_XIR_BAD_STRUCTURE && untouched.term_count == 42);
    EffectCheckedRoots *roots = artifact->checked_effects->checked_roots;
    uint32_t member = roots->node[roots->function[1].root].module_sets[EFFECT_MODULE_ACCESS];
    CHECK(member < roots->module_members); uint8_t kind = roots->module_member[member].kind;
    roots->module_member[member].kind = UINT8_MAX;
    CHECK(xir_effects_module_export_copy(artifact, &owned) == XR_XIR_BAD_STRUCTURE && !owned);
    roots->module_member[member].kind = kind; effect_mark_check(source);
    for (size_t point = 0; point < sites; ++point) {
        attempts = 0; injected = false; fail_at = point;
        CHECK(xir_effects_module_export_copy(artifact, &owned) == XR_XIR_OUT_OF_MEMORY && !owned && injected);
        fail_at = SIZE_MAX; effect_mark_check(source);
    }
    CHECK(xir_effects_module_export_copy(NULL, &owned) == XR_XIR_BAD_STRUCTURE && !owned);
    CHECK(xir_effects_module_export_copy(artifact, &owned) == XR_XIR_OK);
    xr_xir_compile_artifact_free(artifact); xr_xir_compile_construction_free(construction);
    memset(&fixture, 0xCE, sizeof(fixture));
    xr_compile_resources_release(context.resources); context = (XrXirCompileContext){0};
    CHECK(xir_effects_module_export_view(owned, &view) == XR_XIR_OK);
    CHECK(module_export_contains(view, view.functions[1].access, XIR_MODULE_EFFECT_SLOT, 1));
    xir_effects_module_export_free(owned); effect_mark_check(baseline);
}
static void module_export_slice_sources(void) {
    EffectMark baseline = effect_mark();
    XrXirArtifact *artifact = root_source("slice_module_export.xr", XR_XIR_OK);
    XirModuleEffectExport *owned = NULL; XirModuleEffectExportView view;
    CHECK(xir_effects_module_export_copy(artifact, &owned) == XR_XIR_OK);
    CHECK(xir_effects_module_export_view(owned, &view) == XR_XIR_OK && view.module_access_complete && !view.slot_count);
    const char *names[] = {"tail","relay","carry"};
    for (size_t n = 0; n < sizeof(names)/sizeof(*names); ++n) {
        uint32_t f = root_find(&artifact->module, names[n]);
        CHECK(f < view.function_count && module_export_contains(view, view.functions[f].result_origin, XIR_MODULE_EFFECT_PARAMETER, 0));
    }
    uint32_t invoke = root_find(&artifact->module, "invoke");
    CHECK(module_export_contains(view, view.functions[invoke].write, XIR_MODULE_EFFECT_CALLBACK, 1) ||
        module_export_contains(view, view.functions[invoke].write, XIR_MODULE_EFFECT_UNKNOWN, 0));
    xr_xir_compile_artifact_free(artifact);
    CHECK(xir_effects_module_export_view(owned, &view) == XR_XIR_OK && view.module_access_complete);
    xir_effects_module_export_free(owned); effect_mark_check(baseline);
    artifact = root_source("root.xr", XR_XIR_OK); owned = NULL;
    CHECK(xir_effects_module_export_copy(artifact, &owned) == XR_XIR_OK);
    CHECK(xir_effects_module_export_view(owned, &view) == XR_XIR_OK);
    uint32_t indirect = root_find(&artifact->module, "indirect");
    CHECK(module_export_contains(view, view.functions[indirect].write, XIR_MODULE_EFFECT_CALLBACK, 0) ||
        module_export_contains(view, view.functions[indirect].write, XIR_MODULE_EFFECT_UNKNOWN, 0));
    xr_xir_compile_artifact_free(artifact); xir_effects_module_export_free(owned); effect_mark_check(baseline);
    CHECK(!root_source("rejected/slice_callback_alias.xr", XR_XIR_BAD_TYPE)); effect_mark_check(baseline);
}
static void module_export_unused_slice(void) {
    EffectMark baseline = effect_mark();
    XrXirTypeNode slice = {.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64}; XrXirTypes types = {.nodes=&slice,.count=1};
    XrXirInstruction initialize = {.op=XR_XIR_RETURN};
    XrXirInstruction body[] = {{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42},{.op=XR_XIR_RETURN,.args={0}}};
    XrXirBlock blocks[] = {{.count=1},{.count=2}};
    XrXirFunction functions[] = {{.name="init",.name_length=4,.error_type=XR_XIR_NEVER,
        .instructions=&initialize,.instruction_count=1,.blocks=blocks,.block_count=1},
        {.name="main",.name_length=4,.result=XR_XIR_I64,.error_type=XR_XIR_NEVER,
        .instructions=body,.instruction_count=2,.blocks=blocks+1,.block_count=1}};
    XrXirSourceModule source = {.name="unused",.name_length=6}; XrXirFunctionIdentity identities[2] = {{0}};
    XrXirDeclarations declarations = {.modules=&source,.module_count=1,.functions=identities,.entry_function=1};
    XrXirModule module = {.stage=XR_XIR_BUILT,.linkage_kind=XR_XIR_PROGRAM,.types=&types,
        .functions=functions,.function_count=2,.declarations=&declarations};
    XrXirConstruction *construction = NULL; XrXirArtifact *artifact = NULL;
    CHECK(xir_fixture_module_construction(&effect_context, &module, &construction) == XR_XIR_OK);
    CHECK(xr_xir_compile_check_v2(&effect_context, &module, construction, &artifact, NULL) == XR_XIR_OK);
    XirModuleEffectExport *owned = NULL; XirModuleEffectExportView view;
    CHECK(xir_effects_module_export_copy(artifact, &owned) == XR_XIR_OK);
    CHECK(xir_effects_module_export_view(owned, &view) == XR_XIR_OK && !view.module_access_complete && !view.term_count);
    xr_xir_compile_artifact_free(artifact); xr_xir_compile_construction_free(construction);
    xir_effects_module_export_free(owned); effect_mark_check(baseline);
}
static void module_effect_export_cases(void) {
    module_export_retirement(); module_export_slice_sources(); module_export_unused_slice();
}
#endif
