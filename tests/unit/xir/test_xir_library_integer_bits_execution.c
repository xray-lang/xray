/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_library_integer_bits_execution.c - Real generated C and both mixed directions for integer operations
 */
#include "base/xresource_limits.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_program_packet_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_library_integer_bits_runtime.h"
#if BITS_CONSUMER_KIND!=0
XR_DATA const XrXirProgramSpec bits_value_program;
XR_DATA const uint32_t bits_value_selection[6];
#endif
#if BITS_CONSUMER_KIND!=1
static XrXirArtifact *bits_packet(const XrXirCompileContext *context,const char *path) {
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
#if BITS_CONSUMER_KIND==0
static void bits_packet_selection(const XrXirModule *module,uint32_t selection[6]) {
    const char *names[]={"andEntry","orEntry","xorEntry","notEntry","leftEntry","rightEntry"};
    for(unsigned s=0;s<6;++s) {
        selection[s]=UINT32_MAX;
        for(uint32_t f=0;f<module->function_count;++f) {
            const XrXirFunction *fn=&module->functions[f];const XrXirFunctionIdentity *id=&module->declarations->functions[f];
            if(id->module==module->declarations->root_module && !id->nominal_owner && fn->name_length==strlen(names[s]) &&
                !memcmp(fn->name,names[s],fn->name_length)){CHECK(selection[s]==UINT32_MAX && id->exported);selection[s]=f;}
        }
        CHECK(selection[s]!=UINT32_MAX);
    }
}
static void bits_emit(XrXirArtifact *artifact,const char *path,const uint32_t selection[6]) {
    XrXirCSource source={0};CHECK(xr_xir_compile_emit_c(artifact,"bits_value",SIZE_MAX,&source)==XR_XIR_OK);
    FILE *file=fopen(path,"wb");CHECK(file && fwrite(source.text,1,source.length,file)==source.length);
    CHECK(fputs("\nconst uint32_t bits_value_selection[6]={",file)>=0);
    for(unsigned s=0;s<6;++s)CHECK(fprintf(file,"%s%uu",s?",":"",selection[s])>0);
    CHECK(fputs("};\n",file)>=0 && !fclose(file));xr_xir_compile_c_source_free(&source);
}
#endif
#if BITS_CONSUMER_KIND==2 || BITS_CONSUMER_KIND==4
static uint64_t bits_engine_steps[2],bits_crossings;
static unsigned bits_lease_releases;
typedef struct BitsMixedEntry {XrXirVmBinding vm;XrXirCallEntry actual;bool native;struct BitsMixedEntry *family;uint32_t count;} BitsMixedEntry;
typedef struct BitsMixedOwner {XrXirArtifact *artifact;XrXirCallEntry *entries;XrXirVmBinding *bindings;BitsMixedEntry *mixed;} BitsMixedOwner;
static XrXirAction bits_resume(XrXirCallView *view) {
    const BitsMixedEntry *entry=view->environment;
    /* The leading VM binding preserves the real environment identity required
     * by owned call guards, while native entries use the same original view. */
    XrXirAction action=entry->actual.resume(view);CHECK(view->environment==entry);
    ++bits_engine_steps[entry->native?1:0];
    if(action.kind==XR_XIR_ACTION_CALL){CHECK(action.callee<entry->count);if(entry->native!=entry->family[action.callee].native)++bits_crossings;}
    return action;
}
static void bits_release(XrXirCallView *view,XrXirCallStatus reason) {
    const BitsMixedEntry *entry=view->environment;
    entry->actual.release(view,reason);CHECK(view->environment==entry);
}
static void bits_code_release(void *pointer) {
    BitsMixedOwner *owner=pointer;xr_xir_compile_artifact_free(owner->artifact);
    xr_compile_resources_free(owner->entries);xr_compile_resources_free(owner->bindings);xr_compile_resources_free(owner->mixed);
    xr_compile_resources_free(owner);++bits_lease_releases;
}
static XrXirProgram *bits_mixed_program(const XrXirCompileContext *context,XrXirArtifact *artifact) {
    BitsMixedOwner *owner=NULL;CHECK(xr_compile_resources_calloc(context->resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);
    owner->artifact=artifact;const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    CHECK(module->function_count==bits_value_program.entry_count);
    CHECK(xr_xir_compile_vm_bind_table(artifact,&owner->bindings,&owner->entries)==XR_XIR_OK);
    CHECK(xr_compile_resources_calloc(context->resources,module->function_count,sizeof(*owner->mixed),(void **)&owner->mixed)==XR_COMPILE_RESOURCE_OK);
    for(uint32_t f=0;f<module->function_count;++f) {
        bool root=module->declarations->functions[f].module==module->declarations->root_module;
        BitsMixedEntry *entry=&owner->mixed[f];entry->native=BITS_CONSUMER_KIND==2?root:!root;
        entry->vm=owner->bindings[f];entry->family=owner->mixed;entry->count=module->function_count;
        entry->actual=entry->native?bits_value_program.entries[f]:owner->entries[f];
        owner->entries[f]=entry->actual;owner->entries[f].environment=entry;owner->entries[f].resume=bits_resume;owner->entries[f].release=bits_release;
    }
    XrXirProgramPacket packet=xir_program_packet_borrow(artifact);
    CHECK(packet.length==bits_value_program.packet.length && !memcmp(packet.identity,bits_value_program.packet.identity,32) &&
        !memcmp(packet.bytes,bits_value_program.packet.bytes,packet.length));
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},owner->entries,
        module->function_count,module->declarations,{owner,bits_code_release},module->types,packet};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(context,&spec,&program)==XR_XIR_OK);return program;
}
#endif
int main(int argc,char **argv) {
    XrXirCompileContext context=bits_context();XrXirProgram *program=NULL;uint32_t selection[6];
#if BITS_CONSUMER_KIND==0
    CHECK(argc==3);XrXirArtifact *artifact=bits_packet(&context,argv[1]);
    bits_packet_selection(xr_xir_compile_artifact_module(artifact),selection);bits_emit(artifact,argv[2],selection);
    CHECK(xr_xir_compile_vm_program_take(&artifact,&program)==XR_XIR_OK && !artifact);
#else
    CHECK(argc==1);(void)argv;memcpy(selection,bits_value_selection,sizeof(selection));
#if BITS_CONSUMER_KIND==1
    CHECK(xr_xir_compile_program_seal(&context,&bits_value_program,&program)==XR_XIR_OK);
#else
    XrXirArtifact *artifact=bits_packet(&context,XR_LIBRARY_INTEGER_BITS_PACKET);program=bits_mixed_program(&context,artifact);
#endif
#endif
    XrXirValue kept=bits_runtime_cases(program,selection);xr_compile_resources_release(context.resources);bits_retained(&kept);
#if BITS_CONSUMER_KIND==2 || BITS_CONSUMER_KIND==4
    CHECK(bits_lease_releases==1 && bits_engine_steps[0] && bits_engine_steps[1] && bits_crossings);
    fprintf(stderr,"Integer bits mixed actual VM%llu C%llu calls%llu\n",(unsigned long long)bits_engine_steps[0],(unsigned long long)bits_engine_steps[1],(unsigned long long)bits_crossings);
#endif
    fprintf(stderr,"Integer bits consumer%u actual parameterized cold Library operations PASS\n",(unsigned)BITS_CONSUMER_KIND);return 0;
}
