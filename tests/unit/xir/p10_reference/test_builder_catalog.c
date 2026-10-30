/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_builder_catalog.c - Ordinary canonical Class from owned Checked input
 */
#include "xir/xxir_source.h"
#include "xir/xxir_prelude_source.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_vm.h"
#include "base/xsha256.h"
#include "base/xio_policy.h"
#include "os/os_fs.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "../xir_source_fixture_owner.h"
static const char builder_source[]=
    "fn build()->string {var b=StringBuilder();b.append(\"é\\0🙂\");"
    "var old=b.toString();b.clear();b.append(42);"
    "if(b.toString()!=\"42\"){return \"bad\"};return old}\n"
    "export fn referenceAnswer()->i64 {if(build()==\"é\\0🙂\"){return 42};return 0}\n";

static XrXirLibraryCatalog *owned_catalog(SourceFixtureOwner *owner) {
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_prelude_library(&owner->context,&source,&diagnostic);
    if(status!=XR_XIR_OK) fprintf(stderr,"canonical provider=%u %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK);
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    XrOsIoPolicy policy=xr_compile_io_policy(owner->context.resources);char *physical=NULL;
    CHECK(xr_realpath_owned(&policy,XR_C08_STDLIB,&physical)==XR_OS_IO_OK);
    XrXirLibraryModuleInput module={{XR_MODULE_IDENTITY_STDLIB,"prelude",physical},"prelude/builtin_symbols.def"};
    XrXirLibraryInput input={.packet=packet.bytes,.length=packet.length,.modules=&module,.module_count=1};
    CHECK(xr_compile_resources_work(owner->context.resources,packet.length)==XR_COMPILE_RESOURCE_OK);
    xr_sha256(packet.bytes,packet.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;
    status=xr_xir_compile_library_catalog_new_v2(&owner->context,&input,1,&catalog);
    xr_compile_resources_free(physical);
    if(status!=XR_XIR_OK) fprintf(stderr,"canonical Catalog=%u\n",status);
    CHECK(status==XR_XIR_OK && catalog);
    xr_xir_compile_source_result_free(&source);
    memset(packet.bytes,0,packet.length);xr_xir_compile_checked_packet_free(&packet);
    return catalog;
}
static XrXirArtifact *owned_consumer(SourceFixtureOwner *owner,const XrXirLibraryCatalog *catalog) {
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(owner->context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"builder-catalog",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&owner->context,XR_C08_STDLIB,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){NULL,builder_source,sizeof(builder_source)-1},&source,&diagnostic,NULL);
    if(status!=XR_XIR_OK) fprintf(stderr,"Catalog consumer=%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK);
    XrXirArtifact *closed=NULL,*decoded=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_specialize(source.checked,&closed,NULL, NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(closed,&packet,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&owner->context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(decoded,&target, NULL,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);xr_xir_compile_artifact_free(closed);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_source_result_free(&source);
    xr_compile_session_free(session);return lowered;
}
int main(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrXirLibraryCatalog *catalog=owned_catalog(&owner);
    XrXirArtifact *lowered=owned_consumer(&owner,catalog);
    xr_xir_compile_library_catalog_free(catalog);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t entry=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==15 && !memcmp(module->functions[f].name,"referenceAnswer",15)) entry=f;
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
    puts("Canonical Class Catalog owns provider inputs; copied consumer executes after Catalog drop: 42");return 0;
}
