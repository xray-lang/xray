/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mem_borrow_library_cases.h - Cold receiver facts through actual imports
 *
 * KEY CONCEPT:
 *   Internal modules are exact dependencies of owned Checked code. They have
 *   no disk locator or independent import name and carry no extra permission.
 */
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_generic.h"
#include "base/xsha256.h"
#include "base/xio_policy.h"
#include "module/xmodule_graph.h"
#include "os/os_fs.h"
static void mem_library_graph_rejections(const XrXirCompileContext *context,
    XrCompilerSession *session,const char *root,const XrXirLibraryCatalog *catalog,const XrXirArtifact *foreign) {
    size_t count=0;const XrModuleResourceBinding *resources=xr_xir_compile_library_catalog_resources_v2(catalog,&count);
    CHECK(count==2 && resources[0].dependency_count==1);
    XrModuleResolverConfig config={root,NULL,catalog};XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"graph-receiver",NULL};
    for (uint32_t kind=0;kind<6;++kind) {
        uint64_t baseline=held(context->resources);XrModuleResolver *resolver=NULL;XrModuleGraph *graph=NULL;
        CHECK(xr_compile_module_resolver_new(context->resources,&config,&resolver)==XR_MODULE_OK);
        CHECK(xr_compile_module_graph_new(context->resources,session,resolver,&graph)==XR_MODULE_OK);
        CHECK(xr_compile_module_graph_build_source(graph,&authority,"import mem\n",NULL)==XR_MODULE_OK);
        int selected=-1;for (int m=0;m<graph->spec_count;++m) if (graph->specs[m].resource==resources) selected=m;
        CHECK(selected>=0);XrModuleResourceBinding owner=resources[0],edge=*owner.dependencies[0];
        const XrModuleResourceBinding *pointer=&edge;owner.dependencies=&pointer;
        if (!kind) edge.checked=foreign;
        if (kind==1) edge.checked_module=owner.checked_module;
        if (kind==2) owner.dependency_count=0;
        if (kind==3) edge.canonical="memory-module-v1:id=6:forged";
        if (kind==4) edge.authority.namespace_id="forged";
        if (kind==5) edge.source_locator="forged.xr";
        /* Only the new graph's unsealed resource view changes. Both catalog
         * artifacts, their declarations and Construction remain immutable. */
        graph->specs[selected].resource=&owner;graph->specs[selected].status=XR_MODSPEC_PARSED;
        XrModuleIdentityAuthority trigger={XR_MODULE_IDENTITY_MEMORY,"graph-trigger",NULL};
        CHECK(xr_compile_module_graph_include_logical_source(graph,&trigger,NULL,NULL,"",NULL)!=XR_MODULE_OK);
        xr_compile_module_graph_free(graph);xr_compile_module_resolver_free(resolver);CHECK(held(context->resources)==baseline);
    }
    XrModuleResolver *resolver=NULL;CHECK(xr_compile_module_resolver_new(context->resources,&config,&resolver)==XR_MODULE_OK);
    XrModuleId output={0};CHECK(xr_compile_module_resolver_resolve(resolver,resources[1].authority.namespace_id,
        NULL,&authority,&output,NULL)!=XR_MODULE_OK && !output.resource && !output.canonical);
    xr_compile_module_resolver_free(resolver);
}
static void mem_library_case(const XrXirCompileContext *context,const char *stdlib,bool assertions) {
    uint64_t baseline=held(context->resources);XrOsIoPolicy policy=xr_compile_io_policy(context->resources);char *root=NULL;
    CHECK(xr_realpath_owned(&policy,XR_MEM_COLD_ROOT,&root)==XR_OS_IO_OK);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    char path[2048];CHECK(snprintf(path,sizeof(path),"%s/mem/mem.xr",stdlib)>0);
    XrModuleIdentityAuthority authority=assertions?(XrModuleIdentityAuthority){XR_MODULE_IDENTITY_SCRIPT,NULL,root}:
        (XrModuleIdentityAuthority){XR_MODULE_IDENTITY_STDLIB,"mem",stdlib};
    char entry[2048];CHECK(snprintf(entry,sizeof(entry),"%s/assertions.xr",root)>0);
    XrXirSourceRequest request={session,assertions?entry:path,&authority,context,stdlib,NULL,XR_XIR_LIBRARY,NULL};
    XrXirSourceResult producer={0};XrXirSourceDiagnostic diagnostic={0};
    static const char assertion[]="export fn run()->i64{assert(true);return 42}\n";
    XrXirStatus status=assertions?xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){"assertions.xr",assertion,sizeof(assertion)-1},&producer,&diagnostic,NULL):
        xr_xir_compile_source_check(&request,&producer,&diagnostic,NULL);
    CHECK(status==XR_XIR_OK);const XrXirModule *module=xr_xir_compile_artifact_module(producer.checked);
    CHECK(module->declarations->module_count==2);
    XrXirLibraryModuleInput bindings[2]={{{assertions?XR_MODULE_IDENTITY_SCRIPT:XR_MODULE_IDENTITY_STDLIB,
        assertions?NULL:"mem",root},assertions?"assertions.xr":"mem/mem.xr"},{0}};
    const XrXirSourceModule *internal=&module->declarations->modules[1];
    static const char prefix[]="memory-module-v1:id=";size_t at=sizeof(prefix)-1,length=0;
    CHECK(internal->name_length>at&&!memcmp(internal->name,prefix,at));
    while (at<internal->name_length && internal->name[at]>='0' && internal->name[at]<='9')
        length=length*10u+(unsigned)(internal->name[at++]-'0');
    CHECK(at<internal->name_length && internal->name[at++]==':' && length==internal->name_length-at);
    char *name=NULL;CHECK(xr_compile_resources_alloc(context->resources,length+1,(void **)&name)==XR_COMPILE_RESOURCE_OK);
    memcpy(name,internal->name+at,length);name[length]=0;
    bindings[1]=(XrXirLibraryModuleInput){{XR_MODULE_IDENTITY_MEMORY,name,NULL},NULL};
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(producer.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&producer);xr_compile_session_free(session);session=NULL;
    XrXirLibraryInput input={.packet=packet.bytes,.length=packet.length,.modules=bindings,.module_count=2};
    xr_sha256(packet.bytes,packet.length,input.sha256);XrXirLibraryCatalog *catalog=NULL,*bad=NULL;
    XrXirLibraryModuleInput wrong[2]={bindings[0],bindings[1]};XrXirLibraryInput rejected=input;rejected.modules=wrong;
    wrong[1].authority.namespace_id="wrong-identity";
    CHECK(xr_xir_compile_library_catalog_new_v2(context,&rejected,1,&bad)!=XR_XIR_OK&&!bad);
    wrong[0]=bindings[1];wrong[1]=bindings[0];
    CHECK(xr_xir_compile_library_catalog_new_v2(context,&rejected,1,&bad)!=XR_XIR_OK&&!bad);
    wrong[0]=bindings[0];wrong[1]=bindings[1];wrong[1].logical_path="forged.xr";
    CHECK(xr_xir_compile_library_catalog_new_v2(context,&rejected,1,&bad)!=XR_XIR_OK&&!bad);
    rejected=input;rejected.module_count=1;
    CHECK(xr_xir_compile_library_catalog_new_v2(context,&rejected,1,&bad)!=XR_XIR_OK&&!bad);
    CHECK(xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog)==XR_XIR_OK&&catalog);
    XrXirArtifact *foreign=NULL;CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&foreign,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    memset(name,0xa5,length);xr_compile_resources_free(name);
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    if (!assertions) mem_library_graph_rejections(context,session,root,catalog,foreign);
    xr_xir_compile_artifact_free(foreign);
    XrModuleIdentityAuthority consumer={XR_MODULE_IDENTITY_MEMORY,"library-consumer",NULL};
    request=(XrXirSourceRequest){session,NULL,&consumer,context,root,NULL,XR_XIR_PROGRAM,catalog};
    static const char text[]="import mem\nexport fn run()->i64{var b=mem.allocZeroed(4);unsafe{const p=b.borrowPtr();p[0]=42 as u8};const v=b.asBytes();return v[0] as i64}\n";
    XrXirSourceResult result={0};
    if (!assertions) {
        CHECK(xr_xir_compile_source_check_text(&request,&(XrXirSourceText){NULL,text,sizeof(text)-1},&result,&diagnostic,NULL)==XR_XIR_OK);
        CHECK(xr_xir_compile_construction_borrow_returns(xr_xir_compile_artifact_construction(result.checked))->count==2);
    }
    xr_xir_compile_library_catalog_free(catalog);xr_compile_session_free(session);
    if (!assertions) {
        XrXirArtifact *closed=NULL,*lowered=NULL;CHECK(xr_xir_compile_artifact_verify(result.checked,NULL)==XR_XIR_OK);
        CHECK(xr_xir_compile_specialize(result.checked,&closed,NULL,NULL)==XR_XIR_OK);
        XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
        CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);
        xr_xir_compile_source_result_free(&result);xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(lowered);
    }
    xr_compile_resources_free(root);CHECK(held(context->resources)==baseline);
}
static void mem_library_root_rejection(const XrXirCompileContext *context) {
    uint64_t baseline=held(context->resources);XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"standalone-library",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_LIBRARY,NULL};
    static const char text[]="export fn run()->i64{return 42}\n";XrXirSourceResult result={0};
    CHECK(xr_xir_compile_source_check_text(&request,&(XrXirSourceText){NULL,text,sizeof(text)-1},&result,NULL,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_module(result.checked)->declarations->module_count==1);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
    XrXirLibraryModuleInput binding={authority,NULL};XrXirLibraryInput input={.packet=packet.bytes,
        .length=packet.length,.modules=&binding,.module_count=1};xr_sha256(packet.bytes,packet.length,input.sha256);
    XrXirLibraryCatalog *output=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(context,&input,1,&output)!=XR_XIR_OK&&!output);
    xr_xir_compile_checked_packet_free(&packet);CHECK(held(context->resources)==baseline);
}
