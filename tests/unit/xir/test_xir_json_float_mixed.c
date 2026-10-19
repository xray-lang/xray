/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_json_float_mixed.c - Both engines across the ordinary JSON floating call
 */
#include "xir/xxir_vm.h"
#include "xir/xxir_program_packet_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_json_float_runtime.h"
XR_DATA const XrXirProgramSpec json_float_program;
XR_DATA const uint32_t json_float_selection[3];
XR_DATA const uint32_t json_float_callee;
typedef struct JsonFloatMixedOwner {XrXirArtifact *artifact;XrXirCallEntry *entries;XrXirVmBinding *bindings;} JsonFloatMixedOwner;
static unsigned float_mixed_releases;
static void float_mixed_release(void *pointer) {
    JsonFloatMixedOwner *owner=pointer;xr_xir_compile_artifact_free(owner->artifact);
    xr_compile_resources_free(owner->entries);xr_compile_resources_free(owner->bindings);
    xr_compile_resources_free(owner);++float_mixed_releases;
}
static XrXirArtifact *float_mixed_lower(const XrXirCompileContext *context) {
    FILE *file=fopen(XR_JSON_FLOAT_PACKET,"rb");CHECK(file && !fseek(file,0,SEEK_END));
    long length=ftell(file);CHECK(length>0 && !fseek(file,0,SEEK_SET));void *bytes=NULL;
    CHECK(xr_compile_resources_alloc(context->resources,(size_t)length,&bytes)==XR_COMPILE_RESOURCE_OK);
    CHECK(fread(bytes,1,(size_t)length,file)==(size_t)length && !fclose(file));
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    XrXirStatus status=xr_xir_compile_checked_read(context,bytes,(size_t)length,&checked,NULL);
    memset(bytes,0xa5,(size_t)length);xr_compile_resources_free(bytes);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_specialize(checked,&closed,NULL);
    xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if(status==XR_XIR_OK)status=xr_xir_compile_lower(closed,&target,&lowered,NULL);
    xr_xir_compile_artifact_free(closed);
    if(status!=XR_XIR_OK) {
        CHECK(!lowered);xr_compile_resources_release(context->resources);
        CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
        fprintf(stderr,"Floating JSON mixed Checked receiving refusal%u, all owners released\n",(unsigned)status);
    }
    CHECK(status==XR_XIR_OK);return lowered;
}
static void float_mixed(unsigned direction) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();unsigned released=float_mixed_releases;
    XrXirArtifact *artifact=float_mixed_lower(&context);const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    CHECK(module->function_count==json_float_program.entry_count && json_float_callee<module->function_count);
    const char *names[]={"main","inspect","observed"};
    for(unsigned i=0;i<3;++i) {
        uint32_t f=json_float_selection[i];CHECK(f<module->function_count &&
            module->functions[f].name_length==strlen(names[i]) && !memcmp(module->functions[f].name,names[i],strlen(names[i])));
    }
    CHECK(module->functions[json_float_callee].name_length==9 && !memcmp(module->functions[json_float_callee].name,"jsonFloat",9));
    uint32_t caller=json_float_selection[1];bool calls=false;
    for(uint32_t i=0;i<module->functions[caller].instruction_count;++i) {
        const XrXirInstruction *op=&module->functions[caller].instructions[i];
        calls|=op->op==XR_XIR_CALL && op->immediate==json_float_callee;
    }
    CHECK(calls);JsonFloatMixedOwner *owner=NULL;
    CHECK(xr_compile_resources_calloc(context.resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);
    owner->artifact=artifact;CHECK(xr_xir_compile_vm_bind_table(artifact,&owner->bindings,&owner->entries)==XR_XIR_OK);
    if(direction<2) {
        uint32_t selected=direction?json_float_callee:caller,other=direction?caller:json_float_callee;
        owner->entries[selected]=json_float_program.entries[selected];
        CHECK(owner->entries[selected].resume==json_float_program.entries[selected].resume &&
            owner->entries[other].resume!=json_float_program.entries[other].resume);
    }
    XrXirProgramPacket packet=xir_program_packet_borrow(artifact);
    CHECK(packet.identity && packet.length==json_float_program.packet.length &&
        !memcmp(packet.identity,json_float_program.packet.identity,32) &&
        !memcmp(packet.bytes,json_float_program.packet.bytes,packet.length));
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},owner->entries,
        module->function_count,module->declarations,{owner,float_mixed_release},module->types,packet};
    XrXirProgram *program=NULL;XrXirStatus status=xr_xir_compile_program_seal(&context,&spec,&program);
    if(status!=XR_XIR_OK) {
        CHECK(!program);float_mixed_release(owner);xr_compile_resources_release(context.resources);
        CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
        fprintf(stderr,"Floating JSON mixed direction%u Program refusal%u, all owners released\n",direction,(unsigned)status);
    }
    CHECK(status==XR_XIR_OK);XrXirValue kept=json_float_cases(program,json_float_selection);
    xr_compile_resources_release(context.resources);CHECK(float_mixed_releases==released+1);json_float_retained(&kept);
    printf("Finite floating JSON direction%u: real inspect/jsonFloat call, complete token cases, final Nullable/physical0\n",direction);
}
int main(void) {
#ifdef XR_JSON_FLOAT_VM_ONLY
    float_mixed(2);
#else
    float_mixed(0);float_mixed(1);
#endif
    return 0;
}
