/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * library_foreign.h - Real module and private witness closure without ASTs
 */
static XrXirStatus library_comptime_foreign_source(XrXirCompileContext *context,XrXirSourceResult *output) {
    static const char *names[]={"ctfe-library.xr","ctfe-protocol.xr","ctfe-owner.xr"};
    static const char *texts[]={
        "import {Getter} from \"./ctfe-protocol\"\nimport {Holder} from \"./ctfe-owner\"\n"
        "fn apply<U:Getter>(value:U)->i64{return value.get()}\n"
        "export fn make<T>()->i64{return comptime apply<Holder<T>>(Holder<T>.make())}\n",
        "export interface Getter{get()->i64}\n",
        ("import {Getter} from \"./ctfe-protocol\"\nexport struct Holder<T> implements Getter{private value:Array<string>;private optional:T?;"
        "static make()->Holder<T>{return Holder<T>{value:[\"ab\",\"cd\"],optional:null}};get()->i64{const x=this.optional;return len(this.value[0])+len(this.value[1])+38}}\n")};
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,MATERIALIZATION_ROOT};
    char paths[3][1024];XrModuleOverlayInput overlays[3];
    for (unsigned i=0;i<3;++i) {
        int n=snprintf(paths[i],sizeof(paths[i]),"%s/%s",MATERIALIZATION_ROOT,names[i]);
        CHECK(n>0 && (size_t)n<sizeof(paths[i]));
        overlays[i]=(XrModuleOverlayInput){authority,names[i],paths[i],texts[i],strlen(texts[i])};
    }
    XrCompilerSession *session=NULL;XrCompilerSessionStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILER_SESSION_OK) return created==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:
        created==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
    XrXirSourceRequest request={session,paths[0],&authority,context,NULL,NULL,XR_XIR_LIBRARY,NULL};
    XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_overlay(&request,overlays,3,output,&diagnostic,NULL);
    xr_compile_session_free(session);
    if (status!=XR_XIR_OK && status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY)
        fprintf(stderr,"Three-module Library status%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    return status;
}
static XrXirStatus library_comptime_foreign_operation(XrXirCompileContext *context,XrXirArtifact **output) {
    static const char *names[]={"ctfe-library.xr","ctfe-protocol.xr","ctfe-owner.xr"};
    static const char consumer[]="import {make} from \"./ctfe-library\"\nstruct Later{private value:i64}\n"
        "export fn run()->i64{return make<Later>()+make<Array<Later>>()-42}\n";
    XrXirSourceResult source={0};XrXirCheckedPacket packet={0};XrXirLibraryCatalog *catalog=NULL;
    XrXirLibraryModuleInput bindings[3]={0};XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,MATERIALIZATION_ROOT};
    XrXirStatus status=library_comptime_foreign_source(context,&source);
    if (status!=XR_XIR_OK) goto done;
    const XrXirDeclarations *declarations=source.checked->module.declarations;
    CHECK(declarations->module_count==3 && source.checked->construction->comptime.site_count==1 &&
        !source.checked->construction->comptime.binding_count);
    for (uint32_t m=0;m<3;++m) {
        bool found=false;
        for (uint32_t n=0;n<3;++n) {
            char *canonical=NULL;
            XrModuleStatus identity=xr_compile_module_identity_from_logical(context->resources,&authority,names[n],&canonical);
            if (identity!=XR_MODULE_OK) {
                status=identity==XR_MODULE_BUDGET?XR_XIR_BUDGET:identity==XR_MODULE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
                xr_compile_resources_free(canonical);goto done;
            }
            if (declarations->modules[m].name_length==strlen(canonical) &&
                !memcmp(declarations->modules[m].name,canonical,strlen(canonical))) {
                CHECK(!found);bindings[m]=(XrXirLibraryModuleInput){authority,names[n]};found=true;
            }
            xr_compile_resources_free(canonical);
        }
        CHECK(found);
    }
    status=xr_xir_compile_checked_write(source.checked,&packet,NULL);
    xr_xir_compile_source_result_free(&source);
    if (status!=XR_XIR_OK) goto done;
    XrXirLibraryInput input={packet.bytes,packet.length,{0},bindings,3};xr_sha256(input.packet,input.length,input.sha256);
    status=xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);memset(bindings,0xa5,sizeof(bindings));
    if (status==XR_XIR_OK) status=library_comptime_source(context,consumer,false,catalog,&source);
    if (status==XR_XIR_OK) {*output=source.checked;source.checked=NULL;}
done:
    xr_xir_compile_library_catalog_free(catalog);xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_source_result_free(&source);return status;
}
