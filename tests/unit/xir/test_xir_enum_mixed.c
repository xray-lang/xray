/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_enum_mixed.c - Dedicated enum identity source and ownership execution
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
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_source_enum_identity_execution.h"
XR_DATA const XrXirProgramSpec fixture_enum_program;
XR_DATA const uint32_t fixture_enum_values[4],fixture_enum_count;
#include "toolchain/xcompiler_session.h"
static XrXirArtifact *enum_checked(const XrXirCompileContext *context) {
    XrCompilerSession *session=NULL; CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK && session);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/root.xr",&authority,context,XR_SOURCE_STDLIB,NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"source %u: %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK && result.checked && result.snapshot);
    XrXirArtifact *checked=result.checked;result.checked=NULL;
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);return checked;
}
static XrXirArtifact *enum_lower(XrXirArtifact *checked) {
    XrXirArtifact *specialized=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(checked,&specialized,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(specialized,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(specialized);return lowered;
}
typedef struct EnumMixed {XrXirArtifact *artifact;XrXirCallEntry *entries;XrXirVmBinding *bindings;} EnumMixed;
static unsigned released;
static void enum_release(void *pointer){EnumMixed *owner=pointer;xr_xir_compile_artifact_free(owner->artifact);xr_compile_resources_free(owner->entries);xr_compile_resources_free(owner->bindings);xr_compile_resources_free(owner);++released;}
int main(void){
    SourceEnumIdentityEntries entries={{fixture_enum_values[0],fixture_enum_values[1],fixture_enum_values[2],fixture_enum_values[3]},fixture_enum_count};
    const XrXirCompileContext context=*effects_source_owner(UINT64_C(64)*1024*1024,UINT64_C(128000000));
    EnumMixed *owner=NULL;CHECK(xr_compile_resources_calloc(context.resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);
    owner->artifact=enum_lower(enum_checked(&context));
    const XrXirModule *module=xr_xir_compile_artifact_module(owner->artifact);CHECK(module->function_count==fixture_enum_program.entry_count);
    CHECK(xr_compile_resources_calloc(context.resources,module->function_count,sizeof(*owner->entries),(void **)&owner->entries)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(context.resources,module->function_count,sizeof(*owner->bindings),(void **)&owner->bindings)==XR_COMPILE_RESOURCE_OK);CHECK(owner->entries && owner->bindings);
    unsigned native=0,vm=0;
    for(uint32_t i=0;i<module->function_count;++i){
        CHECK(xr_xir_compile_vm_bind(owner->artifact,i,&owner->bindings[i],&owner->entries[i])==XR_XIR_OK);
        const XrXirFunction *function=&module->functions[i];
        bool choose=i==entries.values[0] || i==entries.values[2] ||
            (function->name_length==18 && !memcmp(function->name,"identityTextResult",18));
        if(choose){owner->entries[i]=fixture_enum_program.entries[i];++native;}else ++vm;
    }
    CHECK(native==3 && vm>0);
    CHECK(owner->entries[entries.values[0]].resume==fixture_enum_program.entries[entries.values[0]].resume);
    CHECK(owner->entries[entries.values[1]].resume!=fixture_enum_program.entries[entries.values[1]].resume);
    CHECK(owner->entries[entries.values[2]].resume==fixture_enum_program.entries[entries.values[2]].resume);
    CHECK(owner->entries[entries.values[3]].resume!=fixture_enum_program.entries[entries.values[3]].resume);
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},owner->entries,module->function_count,module->declarations,{owner,enum_release},module->types,xr_xir_compile_program_proof(owner->artifact)};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_OK);
    XrXirValue retained[2][4]={{{0}}};
    source_enum_identity_pair(program,entries,retained);
    source_enum_identity_runtime_failures(program,entries);
    xr_xir_compile_program_drop(program);
    source_enum_identity_retained_drop(retained);
    CHECK(!runtime_live && !runtime_bytes);effects_source_owners_free();
    CHECK(released==1);puts("enum mixed: native-to-VM and VM-to-native, owned strings and physical zero");return 0;
}
