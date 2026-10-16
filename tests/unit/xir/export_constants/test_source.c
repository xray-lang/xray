/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source.c - Source and source-free owned constant imports
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "base/xsha256.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_library_catalog.h"
#include "toolchain/xcompiler_session.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}} while(0)
#include "../xir_source_fixture_owner.h"
#include "execution.h"

static XrXirStatus constant_source(const XrXirCompileContext *context,const char *root,
    const char *name,XrXirLinkageKind kind,const XrXirLibraryCatalog *catalog,XrXirArtifact **output) {
    char path[4096];CHECK(snprintf(path,sizeof(path),"%s/%s",root,name)>0);
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,kind,catalog};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if (status==XR_XIR_OK) {
        const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);
        CHECK(view && view->complete);
        unsigned public_bindings=0;
        for (uint32_t i=0;i<view->declaration_count;++i) {
            const XrXirSourceDeclaration *declaration=&view->declarations[i];
            if (declaration->kind==XR_XIR_SOURCE_BINDING && declaration->exported) {
                CHECK(!declaration->mutable && declaration->type.known);++public_bindings;
            }
        }
        CHECK(public_bindings==5);
        *output=result.checked;result.checked=NULL;
    } else {
        CHECK(!result.checked && !result.snapshot && !*output);
        fprintf(stderr,"expected or actual Source rejection %s status%u: %s\n",name,status,diagnostic.message);
    }
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
    return status;
}
static void constant_slots(const XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    const XrXirDeclarations *d=module->declarations;uint32_t public_slots=0,private_slots=0;
    for (uint32_t s=0;s<d->slot_count;++s) {
        const XrXirSlot *slot=&d->slots[s];CHECK(!slot->mutable);
        if (!slot->exported) {CHECK(!slot->name);++private_slots;continue;}
        CHECK(slot->exported==1 && slot->name<d->literal_count);
        const XrXirLiteral *name=&d->literals[slot->name];
        CHECK(name->length && name->bytes && !memchr(name->bytes,0,name->length));++public_slots;
    }
    CHECK(public_slots==5 && private_slots==1);
    uint32_t loads=0,unit_loads=0;
    for (uint32_t f=0;f<module->function_count;++f) for(uint32_t i=0;i<module->functions[f].instruction_count;++i) {
        const XrXirInstruction *op=&module->functions[f].instructions[i];
        if (op->op==XR_XIR_SLOT_LOAD && d->slots[op->immediate].module!=d->functions[f].module) {
            ++loads;unit_loads+=op->type==XR_XIR_UNIT;
            CHECK(xr_xir_slot_readable(d,d->functions[f].module,(uint32_t)op->immediate));
        }
    }
    if (module->linkage_kind==XR_XIR_PROGRAM) CHECK(loads>=9 && unit_loads>=3);
}
int main(int argc,char **argv) {
    CHECK(argc==2 || argc==3);const char *root=argv[1];
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrXirArtifact *source=NULL,*producer=NULL,*received=NULL;
    CHECK(constant_source(&owner.context,root,"root.xr",XR_XIR_PROGRAM,NULL,&source)==XR_XIR_OK);
    constant_slots(source);constant_vm(&owner.context,source,NULL);xr_xir_compile_artifact_free(source);source=NULL;
    CHECK(constant_source(&owner.context,root,"private.xr",XR_XIR_PROGRAM,NULL,&source)!=XR_XIR_OK);
    CHECK(constant_source(&owner.context,root,"write.xr",XR_XIR_PROGRAM,NULL,&source)!=XR_XIR_OK);
    CHECK(constant_source(&owner.context,root,"mutable.xr",XR_XIR_LIBRARY,NULL,&source)!=XR_XIR_OK);
    CHECK(constant_source(&owner.context,root,"constants.xr",XR_XIR_LIBRARY,NULL,&producer)==XR_XIR_OK);
    constant_slots(producer);
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(producer,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(producer);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    char logical[]="constants.xr";
    XrXirLibraryModuleInput binding={.logical_path=logical,.authority=authority};
    XrXirLibraryInput input={.packet=packet.bytes,.length=packet.length,.modules=&binding,.module_count=1};
    xr_sha256(input.packet,input.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(&owner.context,&input,1,&catalog)==XR_XIR_OK);
    memset(packet.bytes,0,packet.length);xr_xir_compile_checked_packet_free(&packet);
    memset(logical,0,sizeof(logical));memset(&binding,0,sizeof(binding));memset(&input,0,sizeof(input));
    char path[4096];CHECK(snprintf(path,sizeof(path),"%s/constants.xr",root)>0 && !remove(path));
    CHECK(constant_source(&owner.context,root,"root.xr",XR_XIR_PROGRAM,catalog,&received)==XR_XIR_OK);
    xr_xir_compile_library_catalog_free(catalog);
    if (argc==3) {
        CHECK(xr_xir_compile_checked_write(received,&packet,NULL)==XR_XIR_OK);
        CHECK(snprintf(path,sizeof(path),"%s.chk",argv[2])>0);
        FILE *file=fopen(path,"wb");CHECK(file);
        CHECK(fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));
        xr_xir_compile_checked_packet_free(&packet);
    }
    constant_slots(received);constant_vm(&owner.context,received,argc==3 ? argv[2] : NULL);
    xr_xir_compile_artifact_free(received);source_fixture_owner_free(&owner);
    puts("Source and source-free owned exported constants VM PASS");return 0;
}
