/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Cold library constants retain syntax eligibility and all String bytes. */
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_construction.h"
static XrXirStatus map_library_check(const XrXirCompileContext *compile,const char *logical,const char *source,
    XrXirLinkageKind kind,const XrXirLibraryCatalog *catalog,XrXirSourceResult *result,XrXirSourceDiagnostic *diagnostic) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(compile->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_MAP_LIBRARY_ROOT};
    char path[1024];int size=snprintf(path,sizeof(path),"%s/%s",XR_MAP_LIBRARY_ROOT,logical);
    CHECK(size>0 && (size_t)size<sizeof(path));
    XrXirSourceRequest request={session,path,&authority,compile,NULL,NULL,kind,catalog, XR_XIR_HOST_TARGET};
    XrXirSourceText text={logical,source,strlen(source)};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&text,result,diagnostic,NULL);
    xr_compile_session_free(session);return status;
}
static void map_library_constant_owners(void) {
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext compile={.resources=resources,.limits=xr_xir_compile_default_limits()};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(map_library_check(&compile,"key-library.xr","export const KEY=\"a\\0b\"\n",XR_XIR_LIBRARY,NULL,&source,&diagnostic)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(source.checked);CHECK(module->declarations->module_count==1);
    const XrXirSlotFacts *facts=xr_xir_compile_construction_slots(xr_xir_compile_artifact_construction(source.checked));
    CHECK(facts->count==1 && facts->rows[0].constant_key.kind==XR_XIR_KEY_STRING &&
        facts->rows[0].constant_key.string.length==3 && !memcmp(facts->rows[0].constant_key.string.bytes,"a\0b",3));
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_MAP_LIBRARY_ROOT};
    XrXirLibraryModuleInput binding={authority,"key-library.xr"};
    XrXirLibraryInput input={.packet=packet.bytes,.length=packet.length,.modules=&binding,.module_count=1};
    xr_sha256(packet.bytes,packet.length,input.sha256);XrXirLibraryCatalog *catalog=NULL;
    size_t first=physical_attempts;
    CHECK(xr_xir_compile_library_catalog_new_v2(&compile,&input,1,&catalog)==XR_XIR_OK);
    size_t sites=physical_attempts-first,live=physical_live,bytes=physical_bytes;CHECK(sites>0);
    for(size_t point=0;point<sites;++point) {
        XrXirLibraryCatalog *failed=NULL;physical_fail=physical_attempts+point;
        XrXirStatus status=xr_xir_compile_library_catalog_new_v2(&compile,&input,1,&failed);
        physical_fail=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY && !failed && physical_live==live && physical_bytes==bytes);
    }
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);memset(&binding,0xa5,sizeof(binding));
    const char positive[]="import {KEY} from \"./key-library\"\nconst ALIAS=KEY\nconst value={ALIAS:1,\"other\":2}\n";
    diagnostic=(XrXirSourceDiagnostic){0};
    XrXirStatus status=map_library_check(&compile,"key-consumer.xr",positive,XR_XIR_PROGRAM,catalog,&source,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"Map library consumer: %u %s\n",(unsigned)status,diagnostic.message);
    CHECK(status==XR_XIR_OK);xr_xir_compile_source_result_free(&source);
    const char duplicate[]="import {KEY} from \"./key-library\"\nconst ALIAS=KEY\nconst value={ALIAS:1,\"a\\0b\":2}\n";
    diagnostic=(XrXirSourceDiagnostic){0};
    status=map_library_check(&compile,"key-consumer.xr",duplicate,XR_XIR_PROGRAM,catalog,&source,&diagnostic);
    CHECK(status==XR_XIR_BAD_TYPE && !source.checked && !source.snapshot && diagnostic.line==3 && diagnostic.column==22 &&
        !strcmp(diagnostic.message,"E0325: duplicate key \"a\\u0000b\" in Map literal"));
    xr_xir_compile_library_catalog_free(catalog);xr_compile_resources_release(resources);CHECK(!physical_live && !physical_bytes);
}
