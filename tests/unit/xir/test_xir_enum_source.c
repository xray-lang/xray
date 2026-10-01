/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_enum_source.c - Dedicated enum identity source and ownership execution
 *
 * KEY CONCEPT:
 *   The same owned Checked program supplies independent VM and native expectations.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)
#include "xir_runtime_allocations.h"
#include "xir_source_enum_identity_execution.h"
#include "xir_source_enum_identity_selection.h"
#include "toolchain/xcompiler_session.h"
static XrXirArtifact *enum_checked(void) {
    XrCompilerSession *session=xr_compiler_session_new(NULL); CHECK(session);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/root.xr",&authority,NULL,XR_SOURCE_STDLIB,NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_source_check(&request,&result,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"source %u: %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK && result.checked && result.snapshot);
    XrXirArtifact *checked=result.checked;result.checked=NULL;
    xr_xir_source_result_free(&result);xr_compiler_session_delete(session);return checked;
}
static XrXirArtifact *enum_lower(XrXirArtifact *checked) {
    XrXirArtifact *specialized=NULL,*lowered=NULL;
    CHECK(xr_xir_specialize(checked,NULL,&specialized,NULL)==XR_XIR_OK);xr_xir_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(specialized,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_artifact_free(specialized);return lowered;
}
int main(int argc,char **argv){
    CHECK(argc==1 || argc==2);XrXirArtifact *checked=enum_checked();
    XrXirCheckedPacket packet={0};CHECK(xr_xir_checked_write(checked,NULL,&packet,NULL)==XR_XIR_OK);
    CHECK(packet.length<262144);
    if(argc==2){FILE *file=fopen(argv[1],"wb");CHECK(file);CHECK(fwrite(packet.bytes,1,packet.length,file)==packet.length);CHECK(!fclose(file));}
    xr_xir_checked_packet_free(&packet);XrXirArtifact *lowered=enum_lower(checked);
    SourceEnumIdentityEntries entries=source_enum_identity_select(xr_xir_artifact_module(lowered));
    XrXirProgram *program=NULL;CHECK(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_OK);
    XrXirValue retained[2][4]={{{0}}};
    source_enum_identity_pair(program,entries,retained);
    source_enum_identity_runtime_failures(program,entries);
    xr_xir_program_drop(program);
    source_enum_identity_retained_drop(retained);
    CHECK(!runtime_live && !runtime_bytes);
    puts("enum source VM: independent 41, Box.Full, 703/401-byte strings");return 0;
}
