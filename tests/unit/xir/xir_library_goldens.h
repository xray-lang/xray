/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_goldens.h - Current callable and Library input boundaries
 */
#ifndef XIR_LIBRARY_GOLDENS_H
#define XIR_LIBRARY_GOLDENS_H
static void library_independent_goldens(void) {
    const XrXirInstruction init={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    const XrXirInstruction body[]={{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}}};
    const XrXirBlock one={0,1,0,0},two={0,2,0,0};
    XrXirType parameter=XR_XIR_CONSTRUCTED_TYPE_BASE;
    const XrXirFunction functions[]={
        {"init",4,NULL,0,XR_XIR_UNIT,&one,1,&init,1,NULL,0,.error_type=XR_XIR_NEVER},
        {"answer",6,&parameter,1,XR_XIR_I64,&two,1,body,2,NULL,0,.error_type=XR_XIR_NEVER}};
    XrXirTypeNode node={.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,
        .result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    XrXirTypes types={&node,1,NULL,NULL};
    const XrXirSourceModule source={"alpha",5,NULL,0,0,0,XR_XIR_MODULE_FRESH_STORAGE};
    const XrXirFunctionIdentity identities[2]={{0},{.exported=1}};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    XrXirModule built={.stage=XR_XIR_BUILT,.functions=functions,.function_count=2,
        .declarations=&declarations,.types=&types,.linkage_kind=XR_XIR_LIBRARY};
    XrXirArtifact *owned=NULL;
    CHECK(xir_fixture_check(library_context,&built,&owned,NULL)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(owned,&packet,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(owned);
    for(unsigned mode=0;mode<3;++mode){
        XrXirModule bad=*module;XrXirDeclarations decl=*module->declarations;bad.declarations=&decl;
        if(!mode)decl.root_module=0;
        if(mode==1)decl.entry_function=1;
        if(mode==2)bad.linkage_kind=(XrXirLinkageKind)UINT32_MAX;
        XrXirArtifact *output=NULL;
        CHECK(xr_xir_compile_recheck_v2(library_context,&bad,xr_xir_compile_artifact_construction(owned),&output,NULL,NULL)==XR_XIR_BAD_STRUCTURE&&!output);
    }
    xr_xir_compile_artifact_free(owned);
    uint8_t signature[24]={0};
    string_fixture_u32(signature,XR_XIR_TYPE_CALLABLE);
    string_fixture_u32(signature+12,XR_XIR_I64);string_fixture_u32(signature+16,XR_XIR_NEVER);
    string_fixture_u32(signature+20,XR_XIR_CALLABLE_ROOT_UNRESOLVED);
    size_t flags=string_fixture_find(&packet,signature,sizeof(signature))+20;
    size_t blocks=source_live,bytes=source_bytes;
    for(unsigned mode=0;mode<2;++mode){
        string_fixture_u32(packet.bytes+flags,mode?XR_XIR_CALLABLE_NO_SUSPEND:0);
        string_catalog_rehash(packet.bytes,packet.length);
        XrXirArtifact *output=NULL;
        CHECK(xr_xir_compile_checked_read(library_context,packet.bytes,packet.length,&output,NULL)==XR_XIR_BAD_TYPE&&!output);
        output=(XrXirArtifact *)(uintptr_t)1;
        CHECK(xr_xir_compile_checked_read(library_context,packet.bytes,packet.length,&output,NULL)==XR_XIR_BAD_STRUCTURE);
        CHECK(output==(XrXirArtifact *)(uintptr_t)1&&source_live==blocks&&source_bytes==bytes);
    }
    xr_xir_compile_checked_packet_free(&packet);
    puts("Current callable root flags, invalid Library entry/root/kind and output ownership PASS");
}
#endif // XIR_LIBRARY_GOLDENS_H
