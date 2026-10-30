/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_decimal_parse_mixed.c - Both execution directions across the decimal helper call
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_program_packet_internal.h"
#include "xir/xxir_nullable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_decimal_parse_runtime.h"
XR_DATA const XrXirProgramSpec decimal_source_program;
XR_DATA const uint32_t decimal_source_functions[3];
typedef struct DecimalMixedOwner {XrXirArtifact *artifact;XrXirCallEntry *entries;XrXirVmBinding *bindings;} DecimalMixedOwner;
static unsigned decimal_mixed_releases;
static void decimal_mixed_free(void *pointer) {
    DecimalMixedOwner *owner=pointer;xr_xir_compile_artifact_free(owner->artifact);
    xr_compile_resources_free(owner->entries);xr_compile_resources_free(owner->bindings);xr_compile_resources_free(owner);++decimal_mixed_releases;
}
static void decimal_mixed(bool native_caller) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    FILE *file=fopen(XR_DECIMAL_CHECKED_FIXTURE,"rb");CHECK(file && fseek(file,0,SEEK_END)==0);
    long size=ftell(file);CHECK(size>0 && size<1048576 && fseek(file,0,SEEK_SET)==0);
    uint8_t *bytes=xr_malloc((size_t)size);CHECK(bytes && fread(bytes,1,(size_t)size,file)==(size_t)size && fclose(file)==0);
    XrXirArtifact *checked=NULL,*closed=NULL;CHECK(xr_xir_compile_checked_read(&context,bytes,(size_t)size,&checked,NULL)==XR_XIR_OK);
    memset(bytes,0xa5,(size_t)size);xr_free(bytes);
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL, NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    DecimalMixedOwner *owner=NULL;CHECK(xr_compile_resources_calloc(context.resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target, NULL,&owner->artifact,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(owner->artifact);CHECK(module->function_count==decimal_source_program.entry_count);
    CHECK(xr_xir_compile_vm_bind_table(owner->artifact,&owner->bindings,&owner->entries)==XR_XIR_OK);
    unsigned native=0,vm=0;
    for(uint32_t i=0;i<module->function_count;++i) {
        bool helper=i==decimal_source_functions[2];
        if(helper!=native_caller){owner->entries[i]=decimal_source_program.entries[i];++native;}else ++vm;
    }
    CHECK(native && vm);
    XrXirProgramPacket packet=xir_program_packet_borrow(owner->artifact);
    CHECK(packet.identity && packet.length==decimal_source_program.packet.length &&
        !memcmp(packet.identity,decimal_source_program.packet.identity,32) &&
        !memcmp(packet.bytes,decimal_source_program.packet.bytes,packet.length));
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},owner->entries,
        module->function_count,module->declarations,{owner,decimal_mixed_free},module->types,packet};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_OK);
    XrXirValue kept=decimal_program_cases(program,decimal_source_functions);xr_compile_resources_release(context.resources);decimal_retained(&kept);
    fprintf(stderr,"Decimal mixed native_caller=%u actual main/inspect boundary passed\n",native_caller);
}
int main(void) {
    decimal_mixed(false);CHECK(decimal_mixed_releases==1);decimal_mixed(true);CHECK(decimal_mixed_releases==2);return 0;
}
