/* Governed native metadata is not a guest Hashable candidate. */
#include "xir/xxir_file_stat_native.h"
static void native_file_stat_discovery(const XrXirCompileContext *compile) {
    static const char path_text[]="export class Path{raw:string\ninit(raw:string){this.raw=raw}}\n";
    static const char io_text[]="import {Path} from path\nexport fn stat(path:Path)->__FileStat?{return __stat(path)}\n";
    char root[1024],entry[1100],path[1100];
    CHECK(snprintf(root,sizeof(root),"%s/native_stdlib",HASHABLE_LIBRARY_ROOT)>0);
    CHECK(snprintf(entry,sizeof(entry),"%s/io/io.xr",root)>0);
    CHECK(snprintf(path,sizeof(path),"%s/path/path.xr",root)>0);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_STDLIB,"io",root};
    XrModuleOverlayInput overlays[]={
        {{XR_MODULE_IDENTITY_STDLIB,"io",root},"io/io.xr",entry,io_text,sizeof(io_text)-1},
        {{XR_MODULE_IDENTITY_STDLIB,"path",root},"path/path.xr",path,path_text,sizeof(path_text)-1}};
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(compile->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrXirSourceRequest request={session,entry,&authority,compile,root,NULL,XR_XIR_LIBRARY,NULL, XR_XIR_HOST_TARGET};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_overlay(&request,overlays,2,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"Hashable native FileStat source status%u %d:%d %s\n",
        (unsigned)status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);
    xr_compile_session_free(session);memset(overlays,0xa5,sizeof(overlays));
    const XrXirModule *module=xr_xir_compile_artifact_module(result.checked);
    CHECK(!module->declarations->hashable_witness_count);
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);bool found=false;
    for(uint32_t d=0;d<view->declaration_count;++d)if(view->declarations[d].native_identity==XR_NATIVE_DECLARATION_FILE_STAT) {
        CHECK(view->declarations[d].kind==XR_XIR_SOURCE_TYPE&&view->declarations[d].type.known);
        CHECK(xir_file_stat_type(module->types,view->declarations[d].type.type));found=true;
    }
    CHECK(found);xr_xir_compile_source_result_free(&result);
}
