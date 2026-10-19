/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_library_inventory.c - Multiple module views of one owned Checked library
 */
#include "xir/xxir_source.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_vm.h"
#include "base/xsha256.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while(0)
#include "../xir_source_fixture_owner.h"

static void source_file(const char *name,const char *text) {
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%s",XR_LIBRARY_INVENTORY_WORK,name)>0);
    if(!text) { CHECK(!remove(path));return; }
    FILE *file=fopen(path,"wb");CHECK(file);
    size_t length=strlen(text);CHECK(fwrite(text,1,length,file)==length && !fclose(file));
}
static XrXirSourceResult compile_source(SourceFixtureOwner *owner,const XrXirLibraryCatalog *catalog,
    const char *name,XrXirLinkageKind kind,const char *text) {
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(owner->context.resources,&session)==XR_COMPILER_SESSION_OK);
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%s",XR_LIBRARY_INVENTORY_WORK,name)>0);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_LIBRARY_INVENTORY_WORK};
    XrXirSourceRequest request={session,path,&authority,&owner->context,NULL,NULL,kind,catalog};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=text?xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){name,text,strlen(text)},&result,&diagnostic,NULL):
        xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK) fprintf(stderr,"inventory source=%s status=%u %s\n",name,status,diagnostic.message);
    xr_compile_session_free(session);CHECK(status==XR_XIR_OK && result.checked && result.snapshot);return result;
}
static XrXirLibraryCatalog *catalog(SourceFixtureOwner *owner) {
    source_file("base.xr","export struct Seed {value:i64}\nvar stored:i64=20\nexport fn base()->i64{return stored}\n");
    source_file("bridge.xr","import {base} from \"./base\"\nexport struct Offset {value:i64}\n"
        "var extra:i64=2\nexport fn bridge()->i64{return base()+extra}\n");
    XrXirSourceResult source=compile_source(owner,NULL,"bridge.xr",XR_XIR_LIBRARY,NULL);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(source.checked);
    CHECK(module->declarations && module->declarations->module_count==2);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_LIBRARY_INVENTORY_WORK};
    const char *names[2]={"base.xr","bridge.xr"};XrXirLibraryModuleInput views[2]={{0}};
    for(unsigned n=0;n<2;++n) {
        char *identity=NULL;
        CHECK(xr_compile_module_identity_from_logical(owner->context.resources,&authority,names[n],&identity)==XR_MODULE_OK);
        unsigned matched=0;
        for(unsigned m=0;m<2;++m) {
            const XrXirSourceModule *view=&module->declarations->modules[m];
            if(view->name_length==strlen(identity) && !memcmp(view->name,identity,view->name_length)) {
                CHECK(!views[m].logical_path);views[m]=(XrXirLibraryModuleInput){authority,names[n]};++matched;
            }
        }
        CHECK(matched==1);xr_compile_resources_free(identity);
    }
    XrXirLibraryInput input={packet.bytes,packet.length,{0},views,2};xr_sha256(packet.bytes,packet.length,input.sha256);
    XrXirLibraryCatalog *result=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(&owner->context,&input,1,&result)==XR_XIR_OK && result);
    xr_xir_compile_source_result_free(&source);xr_xir_compile_checked_packet_free(&packet);
    source_file("base.xr",NULL);source_file("bridge.xr",NULL);return result;
}
static void run_order(unsigned reverse) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrXirLibraryCatalog *library=catalog(&owner);
    const char *imports[2]={
        "import {base,Seed} from \"./base\"\nimport {bridge,Offset} from \"./bridge\"\n",
        "import {bridge,Offset} from \"./bridge\"\nimport {base,Seed} from \"./base\"\n"};
    char text[512];CHECK(snprintf(text,sizeof(text),"%sexport fn referenceAnswer()->i64{"
        "const left=Seed{value:base()};const right=Offset{value:bridge()};return left.value+right.value}\n",imports[reverse])>0);
    XrXirSourceResult source=compile_source(&owner,library,"consumer.xr",XR_XIR_PROGRAM,text);
    const XrXirModule *module=xr_xir_compile_artifact_module(source.checked);
    CHECK(module->declarations->module_count==3 && module->declarations->slot_count==2);
    CHECK(module->types && module->types->nominals && module->types->nominals->count==2);
    XrXirArtifact *closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(source.checked,&closed,NULL)==XR_XIR_OK);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_source_result_free(&source);
    xr_xir_compile_library_catalog_free(library);
    module=xr_xir_compile_artifact_module(lowered);uint32_t entry=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==15 && !memcmp(module->functions[f].name,"referenceAnswer",15)) {
            CHECK(entry==UINT32_MAX);entry=f;
        }
    CHECK(entry!=UINT32_MAX);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    XrXirInstanceConfig config;XrXirInstance *instance=NULL;XrXirValue value={0};
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    CHECK(value.type==XR_XIR_I64 && value.payload==42);xr_xir_value_drop(&value);
    source_fixture_owner_free(&owner);
    printf("two Checked module views retain both slots and nominal declarations; order=%u result=42\n",reverse);
}
int main(void) { run_order(0);run_order(1);return 0; }
