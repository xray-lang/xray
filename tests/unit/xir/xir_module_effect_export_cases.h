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
    /* The complete live-after-callback positive stays in its own fixture;
     * normalizing that indirect loan still needs contextual lifetime facts.
     * This receiver checks the admitted whole-call module WRITE contract. */
    artifact = root_source("slice_callback_after_read.xr", XR_XIR_OK); owned = NULL;
    CHECK(xir_effects_module_export_copy(artifact, &owned) == XR_XIR_OK);
    CHECK(xir_effects_module_export_view(owned, &view) == XR_XIR_OK && view.module_access_complete);
    uint32_t change = root_find(&artifact->module, "change");
    CHECK(view.functions[change].write.count);
    xr_xir_compile_artifact_free(artifact); xir_effects_module_export_free(owned); effect_mark_check(baseline);
    CHECK(!root_source_expect("rejected/slice_callback_alias.xr", XR_XIR_BAD_TYPE,
        "E0382: Slice of module binding `values` conflicts with write by `invoke` via `change`"));
    effect_mark_check(baseline);
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
static void module_export_instance_rows(const XrXirArtifact *closed, uint32_t source_function,
    XirModuleEffectExportView view) {
    const XrXirProvenance *provenance=closed->module.provenance;
    CHECK(provenance && provenance->kind==XR_XIR_EVIDENCE_INSTANCE && provenance->count==view.function_count &&
        view.module_access_complete && view.slot_count==1 && view.function_count==closed->module.function_count);
    unsigned families=0, instances=0;
    for (uint32_t f=0;f<view.function_count;++f) {
        const XirModuleEffectFunction *row=&view.functions[f];
        const XrXirFunction *actual=&closed->module.functions[f];
        CHECK(row->parameter_count==actual->parameter_count && row->value_count==
            (size_t)actual->parameter_count+actual->instruction_count &&
            row->module==closed->module.declarations->functions[f].module &&
            row->capture_count==closed->module.declarations->functions[f].capture_count);
        if (provenance->origins[f].function!=source_function) continue;
        const XrXirTypeNode *type=xr_xir_type_node(closed->module.types,actual->parameters[0]);
        CHECK(type && type->kind==XR_XIR_TYPE_SLICE && type->element==XR_XIR_I64 && row->parameter_count==2 &&
            module_export_contains(view,row->result_origin,XIR_MODULE_EFFECT_PARAMETER,0) &&
            module_export_contains(view,row->write,XIR_MODULE_EFFECT_SLOT,0));
        /* Distinct marker types instantiate the same source declaration;
         * the loan itself stays within the concrete Slice admission. */
        unsigned family=actual->parameters[1]==XR_XIR_I64 ? 1u : actual->parameters[1]==XR_XIR_U8 ? 2u : 0u;
        CHECK(family && !(families&family)); families|=family; ++instances;
    }
    CHECK(families==3 && instances==2);
}
static void module_export_instance_failures(XrXirArtifact *closed) {
    XirModuleEffectExport *owned=NULL; attempts=0;
    CHECK(xir_effects_module_export_copy(closed,&owned)==XR_XIR_OK && owned);
    size_t sites=attempts; CHECK(sites); xir_effects_module_export_free(owned); owned=NULL;
    EffectMark baseline=effect_mark(); XrCompileResourceStats memory=effect_stats(&closed->context);
    for (size_t point=0;point<sites;++point) {
        attempts=0; injected=false; fail_at=point;
        XrXirStatus status=xir_effects_module_export_copy(closed,&owned); fail_at=SIZE_MAX;
        if (status!=XR_XIR_OUT_OF_MEMORY || owned || !injected)
            fprintf(stderr,"Instance module export allocation%zu/%zu status%u injected%d\n",point,sites,(unsigned)status,injected);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && !owned && injected);
        effect_mark_check(baseline); CHECK(effect_stats(&closed->context).held_bytes==memory.held_bytes);
    }
    XrXirProvenance *provenance=(XrXirProvenance *)closed->module.provenance;
    XrXirOrigin *origins=(XrXirOrigin *)provenance->origins;
    uint32_t origin=origins[0].function; origins[0].function=UINT32_MAX;
    CHECK(xir_effects_module_export_copy(closed,&owned)==XR_XIR_BAD_STRUCTURE && !owned);
    origins[0].function=origin; effect_mark_check(baseline);
    XrXirSlot *slots=(XrXirSlot *)closed->module.declarations->slots;
    uint32_t module=slots[0].module; slots[0].module=UINT32_MAX;
    CHECK(xir_effects_module_export_copy(closed,&owned)==XR_XIR_BAD_STRUCTURE && !owned);
    slots[0].module=module; effect_mark_check(baseline);
    printf("Instance module export: %zu real allocation failures preserve empty output and refund\n",sites);
}
static void module_export_instances(void) {
    EffectMark baseline=effect_mark();
    XrXirArtifact *source=root_source("slice_module_instances.xr",XR_XIR_OK), *closed=NULL;
    uint32_t identity=root_find(&source->module,"identity");
    CHECK(identity<source->module.function_count && xr_xir_compile_specialize(source,&closed,NULL,NULL)==XR_XIR_OK && closed);
    XirModuleEffectExport *owned=NULL; XirModuleEffectExportView view;
    CHECK(xir_effects_module_export_copy(closed,&owned)==XR_XIR_OK && owned);
    CHECK(xir_effects_module_export_view(owned,&view)==XR_XIR_OK);
    module_export_instance_rows(closed,identity,view); xir_effects_module_export_free(owned); owned=NULL;
    module_export_instance_failures(closed);
    CHECK(xir_effects_module_export_copy(closed,&owned)==XR_XIR_OK && owned);
    CHECK(xir_effects_module_export_view(owned,&view)==XR_XIR_OK);
    xr_xir_compile_artifact_free(source); source=NULL;
    module_export_instance_rows(closed,identity,view);
    xr_xir_compile_artifact_free(closed); closed=NULL;
    CHECK(xir_effects_module_export_view(owned,&view)==XR_XIR_OK && view.module_access_complete && view.slot_count==1);
    bool returned=false;
    for (uint32_t f=0;f<view.function_count;++f)
        returned|=module_export_contains(view,view.functions[f].result_origin,XIR_MODULE_EFFECT_PARAMETER,0);
    CHECK(returned); xir_effects_module_export_free(owned); effect_mark_check(baseline);
}
static void module_export_callback_pair(XirModuleEffectExportView view,uint32_t function,
    uint32_t callback,uint32_t loan) {
    CHECK(view.callback_loans_complete && view.callback_loan_count && view.callback_loans &&
        function<view.function_count);
    XirModuleEffectSpan span=view.functions[function].callback_loans;
    CHECK(span.first<=view.callback_loan_count && span.count<=view.callback_loan_count-span.first);
    bool found=false;
    for (size_t p=0;p<span.count;++p) {
        XirModuleCallbackLoan row=view.callback_loans[span.first+p];
        found|=row.callback_parameter==callback && row.loan_kind==XIR_MODULE_EFFECT_PARAMETER &&
            row.loan_parameter==loan;
    }
    CHECK(found);
}
typedef struct CallbackLoanObserver {
    const XrXirModule *module;
    XrXirEffects *pending;
} CallbackLoanObserver;
static XrXirStatus module_callback_loan_observe(void *user,const XrXirCompileContext *context,
    const XrXirViewLiveEvent *event) {
    CallbackLoanObserver *observer=user;
    return xir_effects_callback_loan_observe(context,observer->module,observer->pending,event);
}
/* The immutable admitted body supplies real liveness events. A separate pending
 * ROOT owner receives those same events, so every replay reaches the new hash
 * and seed storage without faulting a parser or fabricating a live-bit set. */
static void module_callback_producer_failures(const XrXirArtifact *source) {
    EffectMark baseline=effect_mark();
    XrCompileResourceStats memory=effect_stats(&source->context);
    XirEffectInput input={.module=&source->module,.construction=source->construction};
    size_t sites=0;
    for (size_t point=SIZE_MAX;;point=point==SIZE_MAX?0:point+1) {
        XrXirEffects *pending=NULL;
        CHECK(xir_effects_prepare_verified_input(&source->context,input,NULL,&pending,NULL)==XR_XIR_OK && pending);
        CHECK(!pending->checked_roots->callback_loans && !pending->checked_roots->callback_loans_closed);
        CallbackLoanObserver observer={&source->module,pending};
        XirViewLiveRequest request={.visitor=module_callback_loan_observe,.user=&observer};
        attempts=0;injected=false;fail_at=point;
        XrXirStatus status=xir_compile_view_liveness_loans_verified(&source->context,
            &source->module,source->checked_effects,&request);
        fail_at=SIZE_MAX;
        if (point==SIZE_MAX) {
            CHECK(status==XR_XIR_OK && pending->checked_roots->callback_loans &&
                pending->checked_roots->callback_loan_capacity && pending->checked_roots->callback_loan_index_capacity);
            sites=attempts;CHECK(sites);
        } else {
            if(status!=XR_XIR_OUT_OF_MEMORY || !injected)
                fprintf(stderr,"callback loan producer allocation%zu/%zu status%u injected%d\n",
                    point,sites,(unsigned)status,injected);
            CHECK(status==XR_XIR_OUT_OF_MEMORY && injected);
        }
        CHECK(!pending->checked_roots->callback_loans_closed);
        xr_xir_compile_effects_free(pending);effect_mark_check(baseline);
        CHECK(effect_stats(&source->context).held_bytes==memory.held_bytes);
        if(point!=SIZE_MAX && point+1==sites)break;
    }
    printf("callback loan producer: %zu real allocation failures refund unpublished relations\n",sites);
}
static void module_export_callback_loans(void) {
    EffectMark baseline=effect_mark();
    XrXirArtifact *source=root_source("slice_callback_loans.xr",XR_XIR_OK),*closed=NULL;
    uint32_t invoke=root_find(&source->module,"invoke"),relay=root_find(&source->module,"relay");
    XirModuleEffectExport *owned=NULL;XirModuleEffectExportView view;
    CHECK(xir_effects_module_export_copy(source,&owned)==XR_XIR_OK && owned);
    CHECK(xir_effects_module_export_view(owned,&view)==XR_XIR_OK);
    module_export_callback_pair(view,invoke,1,0);module_export_callback_pair(view,relay,0,1);
    xir_effects_module_export_free(owned);owned=NULL;
    module_callback_producer_failures(source);
    CHECK(xr_xir_compile_specialize(source,&closed,NULL,NULL)==XR_XIR_OK && closed);
    module_export_instance_failures(closed);
    CHECK(xir_effects_module_export_copy(closed,&owned)==XR_XIR_OK && owned);
    CHECK(xir_effects_module_export_view(owned,&view)==XR_XIR_OK);
    CHECK(closed->module.provenance && closed->module.provenance->kind==XR_XIR_EVIDENCE_INSTANCE);
    unsigned families=0,instances=0;
    for(uint32_t f=0;f<closed->module.function_count;++f) {
        if(closed->module.provenance->origins[f].function!=relay)continue;
        const XrXirFunction *actual=closed->module.functions+f;
        CHECK(actual->parameter_count==3);
        unsigned family=actual->parameters[2]==XR_XIR_I64?1u:actual->parameters[2]==XR_XIR_U8?2u:0u;
        CHECK(family && !(families&family));families|=family;++instances;
        module_export_callback_pair(view,f,0,1);
    }
    CHECK(families==3 && instances==2);
    size_t count=view.callback_loan_count;
    XirModuleCallbackLoan first;memcpy(&first,view.callback_loans,sizeof(first));
    xr_xir_compile_artifact_free(source);xr_xir_compile_artifact_free(closed);
    CHECK(xir_effects_module_export_view(owned,&view)==XR_XIR_OK && view.callback_loans_complete &&
        view.callback_loan_count==count && !memcmp(&first,view.callback_loans,sizeof(first)));
    xir_effects_module_export_free(owned);effect_mark_check(baseline);
}
static void module_effect_export_cases(void) {
    module_export_retirement(); module_export_slice_sources(); module_export_unused_slice(); module_export_instances();
    module_export_callback_loans();
}
#endif
