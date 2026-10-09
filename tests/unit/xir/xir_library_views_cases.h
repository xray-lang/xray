/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_views_cases.h - Owned module views preserve real dependency closure
 */
#ifndef XIR_LIBRARY_VIEWS_CASES_H
#define XIR_LIBRARY_VIEWS_CASES_H
static const char *library_views_names[]={"views_api.xr","views_left.xr","views_right.xr","views_leaf.xr","views_extra.xr"};
static XrXirCheckedPacket library_views_packet(const char *entry,XrModuleIdentityAuthority authority,
    const char *const *names,size_t name_count,XrXirLibraryModuleInput *bindings,size_t *count) {
    XrCompilerSession *session=library_session_new(library_context);
    XrXirSourceRequest request={session,entry,&authority,library_context,NULL,NULL,XR_XIR_LIBRARY,NULL};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"views producer %u %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);
    const XrXirDeclarations *d=xr_xir_compile_artifact_module(result.checked)->declarations;
    CHECK(d->module_count&&d->module_count<=name_count&&d->root_module==UINT32_MAX&&d->entry_function==UINT32_MAX&&!d->slot_count);
    *count=d->module_count;
    if(d->module_count==4){
        XrXirModule malformed=*xr_xir_compile_artifact_module(result.checked);
        XrXirDeclarations declarations=*d;XrXirSourceModule modules[4];uint32_t self=0;
        memcpy(modules,d->modules,sizeof(modules));modules[0].dependencies=&self;modules[0].dependency_count=1;
        declarations.modules=modules;malformed.declarations=&declarations;XrXirArtifact *rejected=NULL;
        CHECK(xr_xir_compile_recheck_v2(library_context,&malformed,xr_xir_compile_artifact_construction(result.checked),&rejected,NULL)==XR_XIR_BAD_STRUCTURE&&!rejected);
    }
    for(uint32_t m=0;m<d->module_count;++m){
        bool found=false;
        for(size_t n=0;n<name_count;++n){char *canonical=NULL;
            CHECK(xr_compile_module_identity_from_logical(library_context->resources,&authority,names[n],&canonical)==XR_MODULE_OK);
            if(strlen(canonical)==d->modules[m].name_length&&!memcmp(canonical,d->modules[m].name,d->modules[m].name_length)){
                CHECK(!found);bindings[m]=(XrXirLibraryModuleInput){authority,names[n]};found=true;
            }
            xr_compile_resources_free(canonical);
        }
        CHECK(found);
    }
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);return packet;
}
/* A failed constructor preserves a real occupied output and restores both
 * physical ledgers while the original Catalog remains live. */
static void library_views_reject_occupied(const XrXirLibraryInput *inputs,size_t count,XrXirLibraryCatalog *occupied) {
    size_t view_count=0;const XrModuleResourceBinding *views=xr_xir_compile_library_catalog_resources_v2(occupied,&view_count);
    CHECK(views&&view_count&&view_count<=17);XrModuleResourceBinding saved[17];
    memcpy(saved,views,view_count*sizeof(*views));
    size_t compiler_blocks=source_live,compiler_bytes=source_bytes,blocks=runtime_live,bytes=runtime_bytes;
    uint64_t live=library_compile_stats(library_context).live_bytes;
    XrXirLibraryCatalog *output=occupied;
    CHECK(xr_xir_compile_library_catalog_new_v2(library_context,inputs,count,&output)==XR_XIR_BAD_STRUCTURE);
    CHECK(output==occupied);
    size_t after_count=0;const XrModuleResourceBinding *after=xr_xir_compile_library_catalog_resources_v2(output,&after_count);
    CHECK(after==views&&after_count==view_count&&!memcmp(after,saved,view_count*sizeof(*views)));
    CHECK(source_live==compiler_blocks&&source_bytes==compiler_bytes&&runtime_live==blocks&&runtime_bytes==bytes);
    CHECK(library_compile_stats(library_context).live_bytes==live);
}
static void library_views_case(unsigned mode) {
    CHECK(mode<4);
    conflict_text("views_leaf.xr","export fn answer()->i64{return 20;}\n");
    conflict_text("views_left.xr","import \"./views_leaf\" as leaf;export fn answer()->i64{return leaf.answer();}\n");
    conflict_text("views_right.xr","import \"./views_leaf\" as leaf;export fn answer()->i64{return leaf.answer()+1;}\n");
    conflict_text("views_api.xr","import \"./views_left\" as left;import \"./views_right\" as right;export fn answer()->i64{return left.answer()+right.answer();}\n");
    conflict_text("views_extra.xr","export fn zero()->i64{return 0;}\n");
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    XrXirLibraryModuleInput bindings[2][5]={0};size_t counts[2]={0};
    XrXirCheckedPacket packets[2]={library_views_packet(XR_SOURCE_FIXTURES "/views_api.xr",authority,library_views_names,5,bindings[0],&counts[0]),
        library_views_packet(XR_SOURCE_FIXTURES "/views_extra.xr",authority,library_views_names,5,bindings[1],&counts[1])};
    CHECK(counts[0]==4&&counts[1]==1);
    XrXirLibraryInput inputs[2]={{packets[0].bytes,packets[0].length,{0},bindings[0],counts[0]},
        {packets[1].bytes,packets[1].length,{0},bindings[1],counts[1]}};
    for(unsigned i=0;i<2;++i)xr_sha256(inputs[i].packet,inputs[i].length,inputs[i].sha256);
    if(mode==3){XrXirLibraryInput swap=inputs[0];inputs[0]=inputs[1];inputs[1]=swap;}
    if(mode==1)conflict_text("views_root.xr","import \"./views_leaf\" as leaf;export fn result()->i64{return leaf.answer()+21;}\n");
    else if(mode>=2)conflict_text("views_root.xr","import \"./views_api\" as api;import \"./views_extra\" as extra;export fn result()->i64{return api.answer()+extra.zero();}\n");
    else conflict_text("views_root.xr","import \"./views_api\" as api;export fn result()->i64{return api.answer();}\n");
    /* No producer file can satisfy these imports after publication. */
    for(unsigned i=0;i<5;++i){char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%s",XR_SOURCE_FIXTURES,library_views_names[i])>0);CHECK(!remove(path));}
    size_t input_count=mode>=2?2:1;
    LibrarySourceFixture fixture={inputs,input_count,XR_SOURCE_FIXTURES "/views_root.xr",authority,XR_XIR_PROGRAM};
    const char *labels[]={"Views diamond closure","Views leaf closure","Views disjoint units","Views disjoint units reverse"};
    library_compile_operation_cases(labels[mode],library_source_operation,&fixture);
    XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(library_context,inputs,input_count,&catalog)==XR_XIR_OK);
    size_t resource_count=0;const XrModuleResourceBinding *resources=xr_xir_compile_library_catalog_resources_v2(catalog,&resource_count);
    CHECK(resource_count==(mode>=2?5u:4u));
    unsigned edges=0,leaves=0;
    for(size_t m=0;m<resource_count;++m){
        const XrXirModule *unit=xr_xir_compile_artifact_module(resources[m].checked);
        CHECK(resources[m].checked_module<unit->declarations->module_count);
        const XrXirSourceModule *module=&unit->declarations->modules[resources[m].checked_module];
        CHECK(resources[m].dependency_count==module->dependency_count);edges+=module->dependency_count;
        if(!strcmp(resources[m].logical_path,"views_leaf.xr"))++leaves;
        for(uint32_t e=0;e<resources[m].dependency_count;++e){const XrModuleResourceBinding *dependency=resources[m].dependencies[e];
            CHECK(dependency->checked==resources[m].checked&&dependency->checked_module==module->dependencies[e]);}
    }
    CHECK(edges==4&&leaves==1);
    if(mode==0){XrXirLibraryInput duplicate[]={inputs[0],inputs[0]};
        library_views_reject_occupied(duplicate,2,catalog);
        XrXirLibraryInput missing=inputs[0];--missing.module_count;
        library_views_reject_occupied(&missing,1,catalog);
        XrXirLibraryModuleInput swap=bindings[0][0];bindings[0][0]=bindings[0][1];bindings[0][1]=swap;
        library_views_reject_occupied(inputs,1,catalog);
        swap=bindings[0][0];bindings[0][0]=bindings[0][1];bindings[0][1]=swap;
    }
    for(unsigned i=0;i<2;++i)xr_xir_compile_checked_packet_free(&packets[i]);
    memset(bindings,0,sizeof(bindings));memset(inputs,0,sizeof(inputs));
    XrCompilerSession *consumer=library_session_new(library_context);
    XrXirSourceRequest request={consumer,XR_SOURCE_FIXTURES "/views_root.xr",&authority,library_context,NULL,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"views consumer %u %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);
    const XrXirModule *checked=xr_xir_compile_artifact_module(result.checked);
    CHECK(checked->declarations->module_count==(mode==1?2u:mode>=2?6u:5u));
    CHECK(checked->function_count==(mode==1?5u:mode>=2?13u:11u));
    XrXirArtifact *owned=result.checked;result.checked=NULL;
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(consumer);xr_xir_compile_library_catalog_free(catalog);
    xr_test_library_source_run(owned);CHECK(!runtime_live&&!runtime_bytes);
    printf("views mode%u independent41 twoInstances producerdeath realDependencies PASS\n",mode);
}
#include "xir_library_views_growth_cases.h"
#endif // XIR_LIBRARY_VIEWS_CASES_H
