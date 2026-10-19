/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_library_c_abi.c - Compiler catalog layout and module-view boundary
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_library_catalog.h"
#include "base/xsha256.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"library C ABI FAIL %d %s\n",__LINE__,#x); exit(1); } } while (0)
#define OFFSET(type,field,value) _Static_assert(offsetof(type,field)==value,"compiler field offset")
_Static_assert(sizeof(void *)==8 && sizeof(size_t)==8,"x64 ABI experiment");
_Static_assert(XR_XIR_LIBRARY_C_INTERFACE_VERSION==2,"compiler interface revision");
_Static_assert(sizeof(XrXirLibraryInput)==64 && _Alignof(XrXirLibraryInput)==8,"input layout");
_Static_assert(sizeof(XrXirLibraryInput[2])==128,"input stride");
OFFSET(XrXirLibraryInput,packet,0); OFFSET(XrXirLibraryInput,length,8);
OFFSET(XrXirLibraryInput,sha256,16); OFFSET(XrXirLibraryInput,modules,48);
OFFSET(XrXirLibraryInput,module_count,56);
_Static_assert(sizeof(XrXirLibraryModuleInput)==32 && _Alignof(XrXirLibraryModuleInput)==8,"module input layout");
OFFSET(XrXirLibraryModuleInput,authority,0); OFFSET(XrXirLibraryModuleInput,logical_path,24);
_Static_assert(sizeof(XrModuleResourceBinding)==80 && _Alignof(XrModuleResourceBinding)==8,"view layout");
_Static_assert(sizeof(XrModuleResourceBinding[2])==160,"view stride");
OFFSET(XrModuleResourceBinding,canonical,0); OFFSET(XrModuleResourceBinding,logical_path,8);
OFFSET(XrModuleResourceBinding,source_locator,16); OFFSET(XrModuleResourceBinding,authority,24);
OFFSET(XrModuleResourceBinding,checked,48); OFFSET(XrModuleResourceBinding,checked_module,56);
OFFSET(XrModuleResourceBinding,dependencies,64); OFFSET(XrModuleResourceBinding,dependency_count,72);

int main(void) {
    const XrCompileResourceLimits caps={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&caps,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrCompileResourceStats initial={0},final={0};
    CHECK(xr_compile_resources_stats(resources,&initial)==XR_COMPILE_RESOURCE_OK);
    const char *names[]={"module-id-v1:kind=6:script:namespace=0::path=4:a.xr",
                         "module-id-v1:kind=6:script:namespace=0::path=4:b.xr"};
    uint32_t dependency=1;
    XrXirSourceModule modules[]={
        {names[0],(uint32_t)strlen(names[0]),&dependency,1,0},
        {names[1],(uint32_t)strlen(names[1]),NULL,0,1}};
    const XrXirInstruction ops[]={{XR_XIR_RETURN,XR_XIR_UNIT,{0,0},{0,0},0,{0,0}}};
    const XrXirBlock block={0,1,0,0};
    const XrXirFunction functions[]={
        {"init_a",6,NULL,0,XR_XIR_UNIT,&block,1,ops,1,NULL,0},
        {"init_b",6,NULL,0,XR_XIR_UNIT,&block,1,ops,1,NULL,0}};
    const XrXirFunctionIdentity identities[]={{0,0,0,0,0,0,0,0,0},{1,0,0,0,0,0,0,0,0}};
    XrXirDeclarations declarations={modules,2,identities,NULL,0,NULL,0,UINT32_MAX,UINT32_MAX,NULL};
    XrXirModule module={XR_XIR_BUILT,functions,2,&declarations,NULL,NULL,NULL,XR_XIR_LIBRARY,NULL};
    XrXirArtifact *checked=NULL;XrXirCheckedPacket packet={0};
    CHECK(xir_fixture_check(&context, &module, &checked, NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    XrXirLibraryCatalog *catalog=NULL;
    {
        char root[]="C:/library-c-abi";char a[]="a.xr",b[]="b.xr";
        XrXirLibraryModuleInput bindings[]={{{XR_MODULE_IDENTITY_SCRIPT,NULL,root},a},
                                           {{XR_MODULE_IDENTITY_SCRIPT,NULL,root},b}};
        XrXirLibraryInput input={packet.bytes,packet.length,{0},bindings,2};
        xr_sha256(packet.bytes,packet.length,input.sha256);
        CHECK(xr_xir_compile_library_catalog_new_v2(&context,&input,1,&catalog)==XR_XIR_OK);
        memset(bindings,0,sizeof(bindings));memset(root,0,sizeof(root));
        memset(a,0,sizeof(a));memset(b,0,sizeof(b));
    }
    xr_xir_compile_checked_packet_free(&packet);
    size_t count=0;
    const XrModuleResourceBinding *views=xr_xir_compile_library_catalog_resources_v2(catalog,&count);
    CHECK(views && count==2 && views[0].checked && views[0].checked==views[1].checked);
    CHECK(views[0].checked_module==0 && views[1].checked_module==1);
    CHECK(views[0].dependency_count==1 && views[0].dependencies[0]==&views[1]);
    CHECK(!views[1].dependency_count && !views[1].dependencies);
    CHECK(!strcmp(views[0].logical_path,"a.xr") && !strcmp(views[1].logical_path,"b.xr"));
    CHECK(!strcmp(views[0].authority.physical_root,"C:/library-c-abi"));
    CHECK(xr_xir_compile_library_catalog_context(catalog)->resources==resources);
    xr_xir_compile_library_catalog_free(catalog);
    CHECK(xr_compile_resources_stats(resources,&final)==XR_COMPILE_RESOURCE_OK);
    CHECK(final.live_bytes==initial.live_bytes);
    xr_compile_resources_release(resources);
    puts("compiler C ABI v2 independent x64 layout; two real owned module views; input death; ledger live baseline PASS");
    return 0;
}
