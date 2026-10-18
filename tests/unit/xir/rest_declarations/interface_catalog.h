/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * interface_catalog.h - Authentic source-free rest requirement admission
 *
 * KEY CONCEPT:
 *   A valid low-level physical signature must not silently acquire fixed Source
 *   arity while its logical rest requirement surface remains closed.
 */
#ifndef XIR_REST_INTERFACE_CATALOG_H
#define XIR_REST_INTERFACE_CATALOG_H
#include "xir/xxir_interface.h"
#include "../xir_construction_fixture.h"
static XrXirLibraryCatalog *rest_interface_catalog(const XrXirCompileContext *context,const char *root) {
    static const char identity[]="module-id-v1:kind=6:script:namespace=0::path=8:iface.xr";
    XrXirInstruction instruction={.op=XR_XIR_RETURN};XrXirBlock block={.count=1};
    XrXirFunction function={"init",4,NULL,0,XR_XIR_UNIT,&block,1,&instruction,1,NULL,0};
    XrXirCallableParameter parameter={(XrXirType)256,XR_PARAM_READ};
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CALLABLE,.parameters=&parameter,.parameter_count=1,.result=XR_XIR_I64,.flags=24}};
    XrXirInterfaceMethod method={{"sum",3},(XrXirType)257,XR_PARAM_READ,0,NULL};
    XrXirInterfaceDeclaration declaration={{identity,sizeof(identity)-1},{"Rest",4},1,NULL,0,NULL,0,&method,1};
    XrXirInterfaceTable table={&declaration,1};XrXirTypes types={nodes,2,NULL,&table};
    XrXirSourceModule source={identity,sizeof(identity)-1,NULL,0,0,0};XrXirFunctionIdentity function_identity={0};
    XrXirDeclarations declarations={&source,1,&function_identity,NULL,0,NULL,0,UINT32_MAX,UINT32_MAX,NULL,NULL};
    XrXirModule built={XR_XIR_BUILT,&function,1,&declarations,NULL,&types,NULL,XR_XIR_LIBRARY,NULL};
    XrXirArtifact *checked=NULL;CHECK(xir_fixture_check(context,&built,&checked,NULL)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirLibraryModuleInput binding={.logical_path="iface.xr",.authority=authority};
    XrXirLibraryInput input={.packet=packet.bytes,.length=packet.length,.modules=&binding,.module_count=1};
    xr_sha256(input.packet,input.length,input.sha256);XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog)==XR_XIR_OK);
    memset(packet.bytes,0,packet.length);xr_xir_compile_checked_packet_free(&packet);
    return catalog;
}
#endif // XIR_REST_INTERFACE_CATALOG_H
