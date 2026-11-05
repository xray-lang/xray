/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_string_bytes_native.c - Genuine C and both directions of borrowed calls
 */
#include "xir/xxir_program_packet_internal.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "base/xresource_limits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "string_bytes_cases.h"
XR_DATA const XrXirProgramSpec string_bytes_program;
XR_DATA const uint32_t string_bytes_run,string_bytes_probe;
XR_DATA const uint32_t string_bytes_host[5];
typedef struct BytesOwner { XrXirProgram *vm;XrXirCallEntry *entries; } BytesOwner;
static void bytes_owner_free(void *pointer) {
    BytesOwner *owner=pointer;
    xr_compile_resources_free(owner->entries);xr_xir_compile_program_drop(owner->vm);
    xr_compile_resources_free(owner);
}
static XrXirProgram *bytes_build(unsigned mode) {
    CHECK(mode>=1 && mode<=3);
    XrXirCompileContext context={0};const XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    const XrXirProgramSpec *native=&string_bytes_program;XrXirArtifact *checked=NULL,*code=NULL;
    CHECK(xr_xir_compile_checked_read(&context,native->packet.bytes,native->packet.length,&checked,NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(checked,&target,&code,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    const XrXirModule *module=xr_xir_compile_artifact_module(code);
    CHECK(module->function_count==native->entry_count && string_bytes_probe<module->function_count &&
        string_bytes_run<module->function_count && string_bytes_probe!=string_bytes_run);
    const XrXirFunction *probe=module->functions+string_bytes_probe;
    CHECK(probe->name_length==5 && !memcmp(probe->name,"probe",5));
    BytesOwner *owner=NULL;
    CHECK(xr_compile_resources_calloc(context.resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(context.resources,module->function_count,sizeof(*owner->entries),
        (void **)&owner->entries)==XR_COMPILE_RESOURCE_OK);
    XrXirProgramPacket proof=xir_program_packet_borrow(code);
    CHECK(xr_xir_compile_vm_program_take(&code,&owner->vm)==XR_XIR_OK && !code);
    bool has_native=false,has_vm=false;
    for (uint32_t f=0;f<module->function_count;++f) {
        bool native_entry=mode==1 || (mode==2?f==string_bytes_probe:f!=string_bytes_probe);
        owner->entries[f]=native_entry?native->entries[f]:owner->vm->entries[f];
        CHECK((owner->entries[f].resume==native->entries[f].resume)==native_entry);
        if (native_entry) has_native=true;else has_vm=true;
    }
    if (mode>=2) CHECK(has_native && has_vm);
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,target,owner->entries,module->function_count,
        module->declarations,{owner,bytes_owner_free},module->types,proof};
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_OK);
    xr_compile_resources_release(context.resources);return program;
}
int main(int argc,char **argv) {
    CHECK(argc==2 && strlen(argv[1])==1 && argv[1][0]>='1' && argv[1][0]<='3');
    unsigned mode=(unsigned)(argv[1][0]-'0');
    XrXirProgram *program=bytes_build(mode);
    BytesHostCases host={string_bytes_host[0],string_bytes_host[1],string_bytes_host[2],string_bytes_host[3],
        (XrXirType)string_bytes_host[4]};
    bytes_host_rejections(program,&host);bytes_program_cases(program,string_bytes_run);
    puts("String byte views: native and selected mixed backends passed the same oracle");return 0;
}
