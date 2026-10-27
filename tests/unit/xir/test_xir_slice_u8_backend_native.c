/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_slice_u8_backend_native.c - Genuine C and both directions of borrowed calls
 */
#include "xir/xxir_program_packet_internal.h"
#include "xir/xxir_program_internal.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "base/xresource_limits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "slice_u8_backend_cases.h"
XR_DATA const XrXirProgramSpec slice_u8_program;
XR_DATA const uint32_t slice_u8_run;
XR_DATA const uint32_t slice_u8_host[3];
typedef struct SliceOwner { XrXirProgram *vm;XrXirCallEntry *entries; } SliceOwner;
static void slice_owner_free(void *pointer) {
    SliceOwner *owner=pointer;
    xr_compile_resources_free(owner->entries);xr_xir_compile_program_drop(owner->vm);
    xr_compile_resources_free(owner);
}
static XrXirProgram *slice_build(unsigned mode) {
    CHECK(mode>=1 && mode<=3);
    XrXirCompileContext context={0};const XrCompileResourceLimits limits={.memory_mode=XR_COMPILE_MEMORY_ORDINARY};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    const XrXirProgramSpec *native=&slice_u8_program;XrXirArtifact *checked=NULL,*code=NULL;
    CHECK(xr_xir_compile_checked_read(&context,native->packet.bytes,native->packet.length,&checked,NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(checked,&target,&code,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    const XrXirModule *module=xr_xir_compile_artifact_module(code);
    uint32_t read=slice_u8_host[0],factory=slice_u8_host[1];
    CHECK(module->function_count==native->entry_count && read<module->function_count &&
        factory<module->function_count && slice_u8_run<module->function_count && read!=factory);
    CHECK(module->functions[read].name_length==4 && !memcmp(module->functions[read].name,"read",4));
    CHECK(module->functions[factory].name_length==12 && !memcmp(module->functions[factory].name,"readFunction",12));
    SliceOwner *owner=NULL;
    CHECK(xr_compile_resources_calloc(context.resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(context.resources,module->function_count,sizeof(*owner->entries),
        (void **)&owner->entries)==XR_COMPILE_RESOURCE_OK);
    XrXirProgramPacket proof=xir_program_packet_borrow(code);
    CHECK(xr_xir_compile_vm_program_take(&code,&owner->vm)==XR_XIR_OK && !code);
    bool has_native=false,has_vm=false;
    for (uint32_t f=0;f<module->function_count;++f) {
        bool leaf=f==read || f==factory;
        bool native_entry=mode==1 || (mode==2?leaf:!leaf);
        owner->entries[f]=native_entry?native->entries[f]:owner->vm->entries[f];
        CHECK((owner->entries[f].resume==native->entries[f].resume)==native_entry);
        if (native_entry) has_native=true;else has_vm=true;
    }
    if (mode>=2) {
        CHECK(has_native && has_vm);
        CHECK((owner->entries[read].resume==native->entries[read].resume)==(mode==2));
        CHECK((owner->entries[factory].resume==native->entries[factory].resume)==(mode==2));
        CHECK((owner->entries[slice_u8_run].resume==native->entries[slice_u8_run].resume)==(mode==3));
    }
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,target,owner->entries,module->function_count,
        module->declarations,{owner,slice_owner_free},module->types,proof};
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_OK);
    xr_compile_resources_release(context.resources);return program;
}
int main(int argc,char **argv) {
    CHECK(argc==2 && strlen(argv[1])==1 && argv[1][0]>='1' && argv[1][0]<='3');
    unsigned mode=(unsigned)(argv[1][0]-'0');
    XrXirProgram *program=slice_build(mode);
    SliceHostCases host={slice_u8_host[0],slice_u8_host[1],(XrXirType)slice_u8_host[2]};
    slice_host_rejections(program,&host);slice_program_case(program,slice_u8_run);
    puts("Readonly byte views: native and both function-level mixed directions passed");return 0;
}
