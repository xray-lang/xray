/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * pending_cases.h - Real unresolved keys remain owned, non-executable metadata
 */
#include "xir/xxir_comptime_pending_internal.h"
#include "xir/xxir_construction_internal.h"
#include "../xir_comptime_construction_fixture.h"
static XrXirStatus pending_generated_source(XrXirCompileContext *context,XrXirArtifact **output) {
    XirSourceComptimeInstances stage={0};
    XrXirStatus status=comptime_instances_operation(context,comptime_instances_text("nested"),&stage,
        comptime_instances_limits(),XR_XIR_CTFE_MAX_CALL_DEPTH,false);
    XirCtfeInstanceView views[2]={0};XirSourceComptimeBinding actual[2]={0};
    if (status==XR_XIR_OK) status=literal_ctfe_status(xr_xir_compile_source_comptime_instances_view(&stage,views,actual,2));
    XrXirConstruction *construction=NULL;
    if (status==XR_XIR_OK) {
        const XrXirArtifact *source=views[0].definition;
        XrXirComptimeSite site={actual[0].source_function,actual[0].source_instruction,actual[0].helper};
        XrXirComptimeTable table={&site,1,NULL,0};
        status=xir_fixture_comptime_construction(context,source,&table,&construction);
        if (status==XR_XIR_OK) {
            XrXirModule built=source->module;built.stage=XR_XIR_BUILT;
            status=xr_xir_compile_check_v2(context,&built,construction,output,NULL);
        }
    }
    xr_xir_compile_construction_free(construction);xr_xir_compile_source_comptime_instances_free(&stage);return status;
}
static XrXirStatus pending_source(XrXirCompileContext *context,const char *kind,unsigned mode,XrXirArtifact **output) {
    if (!strcmp(kind,"generated")) return pending_generated_source(context,output);
    if (!strcmp(kind,"nested-unbound")) mode=2;
    const char *text=!strcmp(kind,"foreign")?NULL:!strcmp(kind,"none")?"export fn run()->i64{return 42}\n":
        !strcmp(kind,"builtin")?"fn zero<T>()->i64{return 42}\nfn make<T>()->i64{return comptime zero<T>()}\n"
            "export fn run()->i64{return make<i8>()+make<string>()-42}\n":
        program_text(!strcmp(kind,"nested-unbound")?"nested-open":kind);
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=!strcmp(kind,"foreign")?program_foreign_source(context,&source,&diagnostic):
        program_source(context,text,&source,&diagnostic);
    XrXirConstruction *construction=NULL;
    if (status==XR_XIR_OK) {
        XrXirComptimeTable table=source.checked->construction->comptime;
        if (mode!=1 && table.binding_count && (mode || strcmp(kind,"nested-open")))
            table.binding_count=mode==2?0:table.binding_count-1;
        if (!table.binding_count) table.bindings=NULL;
        XrXirConstructionSpec spec={source.checked->construction->rows,source.checked->construction->count,
            source.checked->construction->materializations,source.checked->construction->materialization_count,&table};
        status=xr_xir_compile_construction_new_v4(context,source.checked->module.types,&spec,xr_xir_compile_construction_slots(source.checked->construction),&construction);
        if (status==XR_XIR_OK) {
            XrXirModule built=source.checked->module;built.stage=XR_XIR_BUILT;
            status=xr_xir_compile_check_v2(context,&built,construction,output,NULL);
        }
    }
    xr_xir_compile_construction_free(construction);xr_xir_compile_source_result_free(&source);return status;
}
static bool pending_empty(const XirComptimePendingView *view) {
    return !view->definition && !view->types && !view->arguments && !view->argument_count &&
        !view->site.caller && !view->site.instruction && !view->site.helper;
}
static void pending_normal(const char *kind) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *source=NULL,*closed=NULL;
    XrXirStatus status=pending_source(&context,kind,0,&source);
    if (status!=XR_XIR_OK) fprintf(stderr,"Pending Source %s status%u\n",kind,status);
    CHECK(status==XR_XIR_OK && source && xr_xir_compile_artifact_verify(source,NULL)==XR_XIR_OK);
    XirComptimePending *owner=NULL;XrXirDiagnostic diagnostic={0};
    status=xr_xir_compile_comptime_pending(source,&owner,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"Pending %s status%u f%u b%u i%u\n",kind,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK);
    bool absent=!strcmp(kind,"unused") || !strcmp(kind,"none") || !strcmp(kind,"nested-open");CHECK((owner==NULL)==absent);
    if (owner) {
        CHECK(xr_xir_compile_specialize(source,&closed,NULL)==XR_XIR_BAD_TYPE && !closed);
        XirComptimePendingView view={0};CHECK(xr_xir_compile_comptime_pending_view(owner,&view,NULL)==XR_XIR_OK);
        CHECK(view.definition!=source && view.definition->context.resources==context.resources && view.types==view.definition->module.types);
        CHECK(view.definition->module.provenance && view.definition->module.provenance->kind==XR_XIR_EVIDENCE_TEMPLATE);
        CHECK(view.argument_count==(!strcmp(kind,"multi")?2u:1u));
        const XrXirComptimeTable *table=&view.definition->construction->comptime;
        const XrXirComptimeSite *site=xir_comptime_site(view.definition->construction,view.site.caller,view.site.instruction);
        CHECK(site && site->helper==view.site.helper && table->binding_count==source->construction->comptime.binding_count);
        XirComptimeSelection selection={&view.definition->module,view.types,view.site.caller,view.site.instruction,view.arguments,view.argument_count};
        const XrXirComptimeBinding *binding=NULL;
        CHECK(xir_comptime_select(&context,view.definition->construction,&selection,&binding)==XR_XIR_BAD_TYPE && !binding);
        if (!strcmp(kind,"generated")) {
            CHECK(view.types->count>source->module.types->count);
            CHECK(xr_xir_type_is_array(view.types,xr_xir_array_element(view.types,view.arguments[0])));
            bool found=false;
            for (uint32_t i=0;i<source->module.types->count;++i) if (!source->module.types->nodes[i].parameter_span) {
                XrXirStatus same=xr_xir_compile_type_substitution_matches_between(&context,source->module.types,view.types,
                    NULL,0,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i),view.arguments[0]);
                if (same==XR_XIR_OK) found=true;else CHECK(same==XR_XIR_BAD_TYPE);
            }
            CHECK(!found);
        }
        if (!strcmp(kind,"builtin")) CHECK(!view.types);
    }
    xr_xir_compile_artifact_free(source);xr_compile_resources_release(context.resources);
    if (owner) {
        XirComptimePendingView view={0};CHECK(xr_xir_compile_comptime_pending_view(owner,&view,NULL)==XR_XIR_OK);
        CHECK(xr_xir_compile_artifact_verify(view.definition,NULL)==XR_XIR_OK);
    }
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;
    xr_xir_compile_comptime_pending_free(owner);CHECK(source_fixture_compile_attempts==attempts);
    source_fixture_compile_fail_at=SIZE_MAX;CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Pending %s actual missing key, original TEMPLATE/bindings, producer-dead metadata only physical0\n",kind);
}
