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
static void mem_direct_built_rejections(const XrXirCompileContext *context,const XrXirModule *original,
    const XrXirConstruction *construction) {
    uint64_t baseline=held(context->resources);uint32_t targets[3]={UINT32_MAX,UINT32_MAX,UINT32_MAX},resize=UINT32_MAX;
    for (uint32_t f=0;f<original->function_count;++f) {
        if (original->declarations->functions[f].native_callable==XR_NATIVE_CALLABLE_BUFFER_RESIZE) resize=f;
        XrXirBorrowReturn row={0};bool present=false;
        CHECK(xir_borrow_return_describe(context,original,f,&row,&present,NULL)==XR_XIR_OK);
        if (row.native_callable==XR_NATIVE_CALLABLE_BUFFER_LENGTH) targets[1]=f;
        if (present && row.native_callable==XR_NATIVE_CALLABLE_BUFFER_AS_BYTES) targets[0]=f;
        if (present && row.native_callable==XR_NATIVE_CALLABLE_BUFFER_BORROW_PTR) targets[2]=f;
    }
    CHECK(resize!=UINT32_MAX);XrXirStatus allocation=XR_XIR_OK;
    XrXirFunction *functions=xir_compile_copy(context,original->functions,original->function_count*sizeof(*functions),&allocation);
    CHECK(allocation==XR_XIR_OK);
    for (uint32_t k=0;k<3;++k) {
        CHECK(targets[k]!=UINT32_MAX);const XrXirFunction *body=&original->functions[targets[k]];
        uint32_t leaf=UINT32_MAX;
        for (uint32_t i=0;i<body->instruction_count;++i) if (body->instructions[i].op==XR_XIR_CALL) leaf=(uint32_t)body->instructions[i].immediate;
        CHECK(leaf!=UINT32_MAX);
        /* New, canonical BUILT instructions borrow only the unchanged type,
         * declaration and Construction owners until complete check_v2 clones. */
        XrXirInstruction direct[]={
            {.op=XR_XIR_CLASS_GET,.type=XR_XIR_BUFFER_STORAGE,.args={0}},
            {.op=XR_XIR_CALL,.type=body->result,.args={0,1},.immediate=leaf},
            {.op=XR_XIR_RETURN,.args={2}}};
        uint32_t direct_operands[]={1};XrXirBlock block={0,3,0,0};
        functions[targets[k]]=*body;functions[targets[k]].instructions=direct;functions[targets[k]].instruction_count=3;
        functions[targets[k]].operands=direct_operands;functions[targets[k]].operand_count=1;functions[targets[k]].blocks=&block;
        XrXirModule built=*original;built.stage=XR_XIR_BUILT;built.provenance=NULL;built.functions=functions;
        XrXirArtifact *output=NULL;CHECK(xr_xir_compile_check_v2(context,&built,construction,&output,NULL)==XR_XIR_OK&&output);
        xr_xir_compile_artifact_free(output);output=NULL;
        XrXirInstruction effects[]={
            {.op=XR_XIR_CLASS_GET,.type=XR_XIR_BUFFER_STORAGE,.args={0}},
            {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=8},
            {.op=XR_XIR_CALL,.type=XR_XIR_BOOL,.args={0,2},.immediate=resize},
            {.op=XR_XIR_CALL,.type=body->result,.args={2,1},.immediate=leaf},
            {.op=XR_XIR_RETURN,.args={4}}};
        uint32_t effect_operands[]={1,2,1};block.count=5;
        functions[targets[k]].instructions=effects;functions[targets[k]].instruction_count=5;
        functions[targets[k]].operands=effect_operands;functions[targets[k]].operand_count=3;
        CHECK(xr_xir_compile_check_v2(context,&built,construction,&output,NULL)!=XR_XIR_OK&&!output);
        if (k==2) {
            XrXirInstruction stores[]={direct[0],direct[1],
                {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=0},
                {.op=XR_XIR_CONST_INT,.type=XR_XIR_U8,.immediate=1},
                {.op=XR_XIR_PTR_STORE,.type=XR_XIR_UNIT,.args={1,3}},
                {.op=XR_XIR_CALL,.type=body->result,.args={4,1},.immediate=leaf},
                {.op=XR_XIR_RETURN,.args={6}}};
            uint32_t store_operands[]={1,2,3,4,1};block.count=7;
            functions[targets[k]].instructions=stores;functions[targets[k]].instruction_count=7;
            functions[targets[k]].operands=store_operands;functions[targets[k]].operand_count=5;
            CHECK(xr_xir_compile_check_v2(context,&built,construction,&output,NULL)!=XR_XIR_OK&&!output);
        }
        functions[targets[k]]=*body;
    }
    xr_compile_resources_free(functions);CHECK(held(context->resources)==baseline);
    puts("Direct BUILT: three exact leaves accepted; three resize bodies and one raw store rejected");
}
static void mem_direct_source_rejections(const XrXirCompileContext *context,const char *stdlib) {
    uint64_t baseline=held(context->resources);XrOsIoPolicy policy=xr_compile_io_policy(context->resources);
    char *root=NULL,*stdroot=NULL;CHECK(xr_realpath_owned(&policy,XR_MEM_COLD_ROOT,&root)==XR_OS_IO_OK);
    CHECK(xr_realpath_owned(&policy,stdlib,&stdroot)==XR_OS_IO_OK);char path[2048],entry[2048];
    CHECK(snprintf(path,sizeof(path),"%s/mem/mem.xr",stdlib)>0);CHECK(snprintf(entry,sizeof(entry),"%s/direct-body.xr",root)>0);
    FILE *file=fopen(path,"rb");CHECK(file&&!fseek(file,0,SEEK_END));long bytes=ftell(file);CHECK(bytes>0&&!fseek(file,0,SEEK_SET));
    char *original=NULL;CHECK(xr_compile_resources_alloc(context->resources,(size_t)bytes+1,(void **)&original)==XR_COMPILE_RESOURCE_OK);
    CHECK(fread(original,1,(size_t)bytes,file)==(size_t)bytes&&!fclose(file));original[bytes]=0;
    const char *old[]={"asBytes() -> Slice<u8> from this { return __bufferAsBytes(this._storage) }",
        "operator len() -> i64 { return __bufferLength(this._storage) }","borrowPtr() -> MutPtr<u8> { return __bufferBorrowPtr(this._storage) }"};
    const char *replacement[]={"asBytes() -> Slice<u8> from this { __bufferResize(this._storage,8); return __bufferAsBytes(this._storage) }",
        "operator len() -> i64 { __bufferResize(this._storage,8); return __bufferLength(this._storage) }",
        "borrowPtr() -> MutPtr<u8> {const p=__bufferBorrowPtr(this._storage);unsafe{p[0]=1 as u8};return __bufferBorrowPtr(this._storage)}"};
    const char *main_text[]={"import mem\nfn use(b:mem.Buffer)->i64{const old=b.asBytes();const next=b.asBytes();return len(old)+len(next)}\n",
        "import mem\nfn use(b:mem.Buffer)->i64{const old=b.asBytes();const n=len(b);return len(old)+n}\n",
        "import mem\nfn use(b:mem.Buffer)->i64{const old=b.asBytes();const p=b.borrowPtr();return len(old)}\n"};
    for (uint32_t k=0;k<3;++k) {
        const char *match=strstr(original,old[k]);CHECK(match);size_t first=(size_t)(match-original);
        size_t tail=(size_t)bytes-first-strlen(old[k]),length=first+strlen(replacement[k])+tail;char *candidate=NULL;
        CHECK(xr_compile_resources_alloc(context->resources,length+1,(void **)&candidate)==XR_COMPILE_RESOURCE_OK);
        memcpy(candidate,original,first);memcpy(candidate+first,replacement[k],strlen(replacement[k]));
        memcpy(candidate+first+strlen(replacement[k]),match+strlen(old[k]),tail+1);
        XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
        XrXirSourceRequest request={session,entry,&authority,context,stdroot,NULL,XR_XIR_PROGRAM,NULL};
        XrModuleOverlayInput overlays[]={{authority,"direct-body.xr",entry,main_text[k],strlen(main_text[k])},
            {{XR_MODULE_IDENTITY_STDLIB,"mem",stdroot},"mem/mem.xr",path,candidate,length}};
        XrXirSourceResult result={0};CHECK(xr_xir_compile_source_check_overlay(&request,overlays,2,&result,NULL,NULL)!=XR_XIR_OK&&!result.checked&&!result.snapshot);
        xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);xr_compile_resources_free(candidate);
    }
    xr_compile_resources_free(original);xr_compile_resources_free(root);xr_compile_resources_free(stdroot);
    CHECK(held(context->resources)==baseline);
    puts("Direct Source: complete asBytes/len resize and borrowPtr store overlays rejected");
}
