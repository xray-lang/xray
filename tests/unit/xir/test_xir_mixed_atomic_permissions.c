/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_atomic_permissions.c - Shared Atomic handles preserve binding permissions
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)){fprintf(stderr,"Atomic permission %d: %s\n",__LINE__,#c);exit(1);} } while(0)
#include "xir_effect_execution_owner.h"
typedef struct AtomicPermissionCase { const char *name; bool admitted; } AtomicPermissionCase;
static const AtomicPermissionCase atomic_permission_cases[]={
    {"const-i64",true},{"const-alias-i64",true},{"root-param-i64",true},
    {"root-local-alias-i64",true},{"worker-local-i64",true},{"mixed-library-pure",true},
    {"var-direct-i64",false},{"var-transitive-i64",false},{"var-in-worker-arg-i64",false},
    {"var-rebind-i64",false},{"var-local-module-alias-i64",false},{"nested-worker-i64",false},
    {"const-f64",true},{"root-param-f64",true},{"var-direct-f64",false},
    {"const-bool",true},{"root-param-bool",true},{"var-direct-bool",false}
};
static void atomic_permission_packet(const XrXirCompileContext *context,XrXirArtifact *checked) {
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    CHECK(checked);memset(packet.bytes,0xCC,packet.length);xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(checked);
}
static void atomic_permission_check(const AtomicPermissionCase *oracle) {
    const XrXirCompileContext *context=effects_source_owner(UINT64_C(64)*1024*1024,UINT64_C(128000000));
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    char path[1024];int length=snprintf(path,sizeof(path),"%s/%s.xr",XR_P20_ATOMIC_PERMISSION_FIXTURES,oracle->name);
    CHECK(length>0 && (size_t)length<sizeof(path));
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_P20_ATOMIC_PERMISSION_FIXTURES};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result={0},empty={0};XrXirSourceDiagnostic diagnostic={0};char *failure=NULL;
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,&failure);
    printf("Atomic permission source case=%s admitted=%u status=%u checked=%u at=%d:%d message=%s\n",
        oracle->name,(unsigned)oracle->admitted,(unsigned)status,(unsigned)(result.checked!=NULL),diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==(oracle->admitted?XR_XIR_OK:XR_XIR_BAD_TYPE) && diagnostic.status==status);
    if(oracle->admitted)CHECK(result.checked && result.snapshot && !failure);
    else CHECK(!memcmp(&result,&empty,sizeof(result)) && failure &&
        !strcmp(diagnostic.message,"GO target requires the current instance root execution"));
    XrXirArtifact *checked=result.checked;result.checked=NULL;
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);xr_compile_resources_free(failure);
    if(checked)atomic_permission_packet(context,checked);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    CHECK(effects_compile_attempts && effects_compile_attempts<20000);
    size_t sites=effects_compile_attempts;effects_source_owners_free();
    printf("Atomic permission complete case=%s admitted=%u sites=%zu allocated=%llu peak=%llu work=%llu checkedPackets=%u poisonedPackets=%u sourceSessionDead originalCompilerOwner physical0/0\n",
        oracle->name,(unsigned)oracle->admitted,sites,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work,(unsigned)oracle->admitted,(unsigned)oracle->admitted);
}
int main(int argc,char **argv) {
    CHECK(argc==2);const AtomicPermissionCase *selected=NULL;
    for(size_t i=0;i<sizeof(atomic_permission_cases)/sizeof(*atomic_permission_cases);++i)
        if(!strcmp(argv[1],atomic_permission_cases[i].name)){CHECK(!selected);selected=&atomic_permission_cases[i];}
    CHECK(selected);atomic_permission_check(selected);return 0;
}
