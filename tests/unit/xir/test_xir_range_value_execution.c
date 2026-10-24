/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_range_value_execution.c - Real generated C and both mixed directions for retained range values
 */
#include "base/xresource_limits.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_program_packet_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_range_value_runtime.h"
#if RANGE_CONSUMER_KIND!=0
XR_DATA const XrXirProgramSpec range_value_program;
XR_DATA const uint32_t range_value_selection[9];
#endif
#if RANGE_CONSUMER_KIND!=1
static XrXirArtifact *range_packet(const XrXirCompileContext *context,const char *path) {
    FILE *file=fopen(path,"rb");CHECK(file && !fseek(file,0,SEEK_END));long length=ftell(file);
    CHECK(length>0 && !fseek(file,0,SEEK_SET));void *bytes=NULL;
    CHECK(xr_compile_resources_alloc(context->resources,(size_t)length,&bytes)==XR_COMPILE_RESOURCE_OK);
    CHECK(fread(bytes,1,(size_t)length,file)==(size_t)length && !fclose(file));
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,bytes,(size_t)length,&checked,NULL)==XR_XIR_OK);
    memset(bytes,0xa5,(size_t)length);xr_compile_resources_free(bytes);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);return lowered;
}
#endif
#if RANGE_CONSUMER_KIND==0
static void range_packet_selection(const XrXirModule *module,uint32_t selection[9]) {
    const char *names[]={"literal","inclusive","constructed","rangeCount","endpoints","once","accept","rangeLength","lenOnce"};
    for(unsigned s=0;s<9;++s) {
        selection[s]=UINT32_MAX;
        for(uint32_t f=0;f<module->function_count;++f) {
            const XrXirFunction *fn=&module->functions[f];const XrXirFunctionIdentity *id=&module->declarations->functions[f];
            if(id->module==module->declarations->root_module && !id->nominal_owner && fn->name_length==strlen(names[s]) &&
                !memcmp(fn->name,names[s],fn->name_length)){CHECK(selection[s]==UINT32_MAX && id->exported);selection[s]=f;}
        }
        CHECK(selection[s]!=UINT32_MAX);
    }
}
static void range_emit(XrXirArtifact *artifact,const char *path,const uint32_t selection[9]) {
    XrXirCSource source={0};CHECK(xr_xir_compile_emit_c(artifact,"range_value",SIZE_MAX,&source)==XR_XIR_OK);
    FILE *file=fopen(path,"wb");CHECK(file && fwrite(source.text,1,source.length,file)==source.length);
    CHECK(fputs("\nconst uint32_t range_value_selection[9]={",file)>=0);
    for(unsigned s=0;s<9;++s)CHECK(fprintf(file,"%s%uu",s?",":"",selection[s])>0);
    CHECK(fputs("};\n",file)>=0 && !fclose(file));xr_xir_compile_c_source_free(&source);
}
#endif
#if RANGE_CONSUMER_KIND==2 || RANGE_CONSUMER_KIND==4
static uint64_t range_engine_steps[2],range_crossings;
static unsigned range_lease_releases;
typedef struct RangeMixedEntry {XrXirVmBinding vm;XrXirCallEntry actual;bool native;struct RangeMixedEntry *family;uint32_t count;} RangeMixedEntry;
typedef struct RangeMixedOwner {XrXirArtifact *artifact;XrXirCallEntry *entries;XrXirVmBinding *bindings;RangeMixedEntry *mixed;} RangeMixedOwner;
static XrXirAction range_resume(XrXirCallView *view) {
    const RangeMixedEntry *entry=view->environment;
    /* The leading VM binding preserves the real environment identity required
     * by owned call guards, while native entries use the same original view. */
    XrXirAction action=entry->actual.resume(view);CHECK(view->environment==entry);
    ++range_engine_steps[entry->native?1:0];
    if(action.kind==XR_XIR_ACTION_CALL){CHECK(action.callee<entry->count);if(entry->native!=entry->family[action.callee].native)++range_crossings;}
    return action;
}
static void range_release(XrXirCallView *view,XrXirCallStatus reason) {
    const RangeMixedEntry *entry=view->environment;
    entry->actual.release(view,reason);CHECK(view->environment==entry);
}
static void range_code_release(void *pointer) {
    RangeMixedOwner *owner=pointer;xr_xir_compile_artifact_free(owner->artifact);
    xr_compile_resources_free(owner->entries);xr_compile_resources_free(owner->bindings);xr_compile_resources_free(owner->mixed);
    xr_compile_resources_free(owner);++range_lease_releases;
}
static XrXirProgram *range_mixed_program(const XrXirCompileContext *context,XrXirArtifact *artifact) {
    RangeMixedOwner *owner=NULL;CHECK(xr_compile_resources_calloc(context->resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);
    owner->artifact=artifact;const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    CHECK(module->function_count==range_value_program.entry_count);
    CHECK(xr_xir_compile_vm_bind_table(artifact,&owner->bindings,&owner->entries)==XR_XIR_OK);
    CHECK(xr_compile_resources_calloc(context->resources,module->function_count,sizeof(*owner->mixed),(void **)&owner->mixed)==XR_COMPILE_RESOURCE_OK);
    for(uint32_t f=0;f<module->function_count;++f) {
        bool root=module->declarations->functions[f].module==module->declarations->root_module;
        RangeMixedEntry *entry=&owner->mixed[f];entry->native=RANGE_CONSUMER_KIND==2?root:!root;
        entry->vm=owner->bindings[f];entry->family=owner->mixed;entry->count=module->function_count;
        entry->actual=entry->native?range_value_program.entries[f]:owner->entries[f];
        owner->entries[f]=entry->actual;owner->entries[f].environment=entry;owner->entries[f].resume=range_resume;owner->entries[f].release=range_release;
    }
    XrXirProgramPacket packet=xir_program_packet_borrow(artifact);
    CHECK(packet.length==range_value_program.packet.length && !memcmp(packet.identity,range_value_program.packet.identity,32) &&
        !memcmp(packet.bytes,range_value_program.packet.bytes,packet.length));
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},owner->entries,
        module->function_count,module->declarations,{owner,range_code_release},module->types,packet};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(context,&spec,&program)==XR_XIR_OK);return program;
}
#endif
int main(int argc,char **argv) {
    XrXirCompileContext context=range_context();XrXirProgram *program=NULL;uint32_t selection[9];
#if RANGE_CONSUMER_KIND==0
    CHECK(argc==3);XrXirArtifact *artifact=range_packet(&context,argv[1]);
    range_packet_selection(xr_xir_compile_artifact_module(artifact),selection);range_emit(artifact,argv[2],selection);
    CHECK(xr_xir_compile_vm_program_take(&artifact,&program)==XR_XIR_OK && !artifact);
#else
    CHECK(argc==1);(void)argv;memcpy(selection,range_value_selection,sizeof(selection));
#if RANGE_CONSUMER_KIND==1
    CHECK(xr_xir_compile_program_seal(&context,&range_value_program,&program)==XR_XIR_OK);
#else
    XrXirArtifact *artifact=range_packet(&context,XR_RANGE_VALUE_PACKET);program=range_mixed_program(&context,artifact);
#endif
#endif
    XrXirValue kept=range_runtime_cases(program,selection);xr_compile_resources_release(context.resources);range_retained(&kept);
#if RANGE_CONSUMER_KIND==2 || RANGE_CONSUMER_KIND==4
    CHECK(range_lease_releases==1 && range_engine_steps[0] && range_engine_steps[1] && range_crossings);
    fprintf(stderr,"Range mixed actual VM%llu C%llu calls%llu\n",(unsigned long long)range_engine_steps[0],(unsigned long long)range_engine_steps[1],(unsigned long long)range_crossings);
#endif
    fprintf(stderr,"Range consumer%u actual retained values/count/len PASS\n",(unsigned)RANGE_CONSUMER_KIND);return 0;
}
