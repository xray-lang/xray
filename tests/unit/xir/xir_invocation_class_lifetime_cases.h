/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_class_lifetime_cases.h - Actual Class discovery retirement
 *
 * KEY CONCEPT:
 *   The real private seal releases Class discovery and Workspace allocations.
 *   Queries then use Final after the raw fixture, Artifact and graph have died.
 */
#ifndef XIR_INVOCATION_CLASS_LIFETIME_CASES_H
#define XIR_INVOCATION_CLASS_LIFETIME_CASES_H
static XrXirStatus conditional_class_discovery_lifetime(const XrXirCompileContext *work,
    uint32_t mode) {
    CHECK(mode<4);InvocationClassRowsFixture rows;InvocationClassCellFixture cells;
    const XrXirModule *raw=NULL;uint32_t subject=0,expected=1;
    if(mode<2) {
        invocation_class_rows_fixture(&rows,mode?2:0);raw=&rows.module;subject=mode?7:2;
    } else {
        invocation_class_cell_fixture(&cells,mode==2?0:6);raw=&cells.source.module;
        subject=cells.source.functions[3].instructions[cells.place].args[0];expected=mode==2?1:5;
    }
    XrXirArtifact *checked=NULL;XrXirStatus status=xir_fixture_check(work,raw,&checked,NULL);
    const XrXirModule *module=checked?xr_xir_compile_artifact_module(checked):NULL;
    XrXirEffects effects={0};EffectGraph graph={0};EffectInvocationDeclaredBounds *bounds=NULL;
    EffectInvocationCertificate *certificate=NULL;EffectFinalFunctions *receiving=NULL;
    if(status==XR_XIR_OK) status=invocation_core_storage(work,module,&effects);
    if(status==XR_XIR_OK) status=effect_graph_build(module,&effects,&graph,(XrXirCompileContext *)work);
    if(status==XR_XIR_OK) status=effect_parameters_derive(module,&effects,&graph,work);
    if(status==XR_XIR_OK) status=effect_invocation_bounds_new(work,&bounds);
    for(uint32_t f=0;status==XR_XIR_OK && f<module->function_count;++f)
        status=effect_invocation_bounds_capture(work,bounds,module->types,&module->functions[f],f,NULL);
    if(status==XR_XIR_OK) {
        EffectBodyRequest body={0};status=effect_invocation_collect(work,module,&effects,&graph,bounds,&body);
        EffectInvocationDriverInput input={work,&body,&effects,&graph,graph.material?graph.material->declared:NULL,NULL};
        if(status==XR_XIR_OK) status=invocation_retired_seal(&input,&certificate,&receiving);
        if(status==XR_XIR_OK) CHECK(invocation_discovery_class_count>0);
    }
    xr_xir_compile_artifact_free(checked);memset(&rows,0xCE,sizeof(rows));memset(&cells,0xCE,sizeof(cells));
    effect_graph_free(&graph);invocation_core_storage_free(&effects);effect_invocation_bounds_free(bounds);
    if(status==XR_XIR_OK) {
        EffectInvocationRead read={0};status=effect_invocation_read_open(work,certificate,certificate->final,&read);
        if(status==XR_XIR_OK) {
            uint32_t selected=certificate->final->roots[3],mask=0;
            CHECK(!read.discovery && !effect_invocation_class_discovery(&read,selected));
            size_t query_attempts=rp_attempts;
            status=effect_final_class_mask(work,certificate->final,selected,subject,&mask);
            if(status==XR_XIR_OK) CHECK(mask==expected);
            InvocationReturnedLedger ledger={work};XirEffectRuntimeRead runtime={.owner=&ledger,.work=invocation_returned_work};
            mask=0;if(status==XR_XIR_OK) status=effect_runtime_class_mask(certificate,&runtime,selected,subject,&mask);
            if(status==XR_XIR_OK) CHECK(mask==expected);
            CHECK(rp_attempts==query_attempts);
            effect_invocation_read_close(&read);
        }
        if(status==XR_XIR_OK) status=effect_invocation_publication_match(work,certificate,&certificate->final->summary);
    }
    effect_invocation_result_free(receiving);effect_invocation_certificate_free(certificate);return status;
}
#endif // XIR_INVOCATION_CLASS_LIFETIME_CASES_H
