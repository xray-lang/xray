/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_assert_panics_resource.inc.c - Result facts and resource failures retain their owners
 */
static void panics_snapshot_owned(XrXirSourceResult *source) {
    XrXirBudget budget=xr_xir_default_budget();XrXirSourceSnapshot *copy=NULL;
    const XrXirSourceView *view=xr_xir_source_snapshot_view(source->snapshot);
    CHECK(xr_xir_source_snapshot_copy(view,&budget,&copy)==XR_XIR_OK);
    XrXirArtifact *artifact=source->checked;source->checked=NULL;xr_xir_source_result_free(source);
    view=xr_xir_source_snapshot_view(copy);uint32_t result_roles=0;
    for (uint32_t d=0;d<view->declaration_count;++d) {
        const XrXirSourceDeclaration *decl=&view->declarations[d];
        if (decl->native_identity==XR_CORE_BUILTIN_ASSERT_PANICS) {
            CHECK(!strcmp(decl->name,"assertPanics") && decl->generic_parameter_count==1 &&
                decl->type_parameter_kinds && decl->type_parameter_kinds[0]==XR_XIR_BINDER_RESULT_VARIABLE);
            ++result_roles;
        }
    }
    CHECK(result_roles==1);xr_xir_source_snapshot_free(copy);source->checked=artifact;
}
static void panics_resource(const char *directory,const char *path) {
    XrXirSourceResult source=panics_source(directory,path,
        "export fn limit(){assertPanics(fn()->string{return \"owned\"})}\n"
        "export fn cancelled(){assertPanics(fn()->string{defer{const owned=\"cleanup\"};Coro.yield();return \"unreached\"})}\n",true);
    panics_snapshot_owned(&source);XrXirArtifact *lowered=panics_lower(&source);
    uint32_t limited=panics_find(xr_xir_artifact_module(lowered),"limit");
    uint32_t cancelled=panics_find(xr_xir_artifact_module(lowered),"cancelled");
    XrXirProgram *program=NULL;CHECK(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){16777216,64000000},&program)==XR_XIR_OK);
    size_t live=runtime_live,bytes=runtime_bytes;XrXirInstance *instance=NULL;
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);config.depth_limit=3;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,limited,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(instance).outcome.status==XR_XIR_CALL_LIMIT);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(runtime_live==live && runtime_bytes==bytes);
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,cancelled,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(instance).outcome.status==XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(instance).outcome.status==XR_XIR_CALL_CANCELLED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(runtime_live==live && runtime_bytes==bytes);xr_xir_program_drop(program);
    CHECK(!source_live && !source_bytes && !runtime_live && !runtime_bytes);
    puts("Owned RESULT snapshot survives producer destruction; callback LIMIT and cancellation never become assertion success; physical refunds PASS");
}
