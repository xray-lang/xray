/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source.c - Source and source-free owned rest declarations
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "base/xsha256.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_types.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_library_catalog.h"
#include "toolchain/xcompiler_session.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}} while(0)
#include "../xir_source_fixture_owner.h"
#include "execution.h"
#include "interface_catalog.h"

static XrXirStatus rest_source(const XrXirCompileContext *context,const char *root,
    const char *name,XrXirLinkageKind kind,const XrXirLibraryCatalog *catalog,XrXirArtifact **output) {
    char path[4096];CHECK(snprintf(path,sizeof(path),"%s/%s",root,name)>0);
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,kind,catalog,NULL};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if (status==XR_XIR_OK) {
        const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);
        CHECK(view && view->complete);
        *output=result.checked;result.checked=NULL;
    } else {
        CHECK(!result.checked && !result.snapshot && !*output);
        if (!strcmp(name,"interface_call.xr") || !strcmp(name,"interface_value.xr"))
            CHECK(status==XR_XIR_BAD_TYPE && strstr(diagnostic.message,"rest interface call shape is not admitted"));
        fprintf(stderr,"expected or actual Source rejection %s status%u: %s\n",name,status,diagnostic.message);
    }
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
    return status;
}
static void rest_slots(const XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    unsigned rest=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        if (!module->declarations->functions[f].variadic) continue;
        const XrXirFunction *function=&module->functions[f];
        CHECK(function->parameter_count && xr_xir_type_is_array(module->types,function->parameters[function->parameter_count-1]));
        ++rest;
    }
    CHECK(rest==5);
}
int main(int argc,char **argv) {
    CHECK(argc==2 || argc==3);const char *root=argv[1];
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrXirArtifact *source=NULL,*producer=NULL,*received=NULL;
    CHECK(rest_source(&owner.context,root,"root.xr",XR_XIR_PROGRAM,NULL,&source)==XR_XIR_OK);
    rest_slots(source);rest_vm(&owner.context,source,NULL);xr_xir_compile_artifact_free(source);source=NULL;
    const char *negative[]={"wrong.xr","write.xr","ref.xr","default.xr","fixed_value.xr","missing.xr","array_argument.xr","not_last.xr"};
    for (unsigned n=0;n<sizeof(negative)/sizeof(*negative);++n)
        CHECK(rest_source(&owner.context,root,negative[n],XR_XIR_PROGRAM,NULL,&source)!=XR_XIR_OK);
    XrXirLibraryCatalog *interface_catalog=rest_interface_catalog(&owner.context,root);
    CHECK(rest_source(&owner.context,root,"interface_call.xr",XR_XIR_PROGRAM,interface_catalog,&source)==XR_XIR_BAD_TYPE);
    CHECK(rest_source(&owner.context,root,"interface_value.xr",XR_XIR_PROGRAM,interface_catalog,&source)==XR_XIR_BAD_TYPE);
    xr_xir_compile_library_catalog_free(interface_catalog);
    CHECK(rest_source(&owner.context,root,"rest.xr",XR_XIR_LIBRARY,NULL,&producer)==XR_XIR_OK);
    rest_slots(producer);
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(producer,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(producer);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    char logical[]="rest.xr";
    XrXirLibraryModuleInput binding={.logical_path=logical,.authority=authority};
    XrXirLibraryInput input={.packet=packet.bytes,.length=packet.length,.modules=&binding,.module_count=1};
    xr_sha256(input.packet,input.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(&owner.context,&input,1,&catalog)==XR_XIR_OK);
    memset(packet.bytes,0,packet.length);xr_xir_compile_checked_packet_free(&packet);
    memset(logical,0,sizeof(logical));memset(&binding,0,sizeof(binding));memset(&input,0,sizeof(input));
    char path[4096];CHECK(snprintf(path,sizeof(path),"%s/rest.xr",root)>0 && !remove(path));
    CHECK(rest_source(&owner.context,root,"root.xr",XR_XIR_PROGRAM,catalog,&received)==XR_XIR_OK);
    xr_xir_compile_library_catalog_free(catalog);
    if (argc==3) {
        CHECK(xr_xir_compile_checked_write(received,&packet,NULL)==XR_XIR_OK);
        CHECK(snprintf(path,sizeof(path),"%s.chk",argv[2])>0);
        FILE *file=fopen(path,"wb");CHECK(file);
        CHECK(fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));
        xr_xir_compile_checked_packet_free(&packet);
    }
    rest_slots(received);rest_vm(&owner.context,received,argc==3 ? argv[2] : NULL);
    xr_xir_compile_artifact_free(received);source_fixture_owner_free(&owner);
    puts("Source and source-free owned rest declarations VM PASS");return 0;
}
