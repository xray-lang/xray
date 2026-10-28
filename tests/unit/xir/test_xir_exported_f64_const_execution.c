/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_exported_f64_const_execution.c - Real generated C and both mixed directions for public constants
 */
#include "base/xresource_limits.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_program_packet_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_exported_f64_const_runtime.h"
#if CI_CONSUMER_KIND!=0
XR_DATA const XrXirProgramSpec ci_value_program;
XR_DATA const uint32_t ci_value_selection[6];
#endif
#if CI_CONSUMER_KIND!=1
static XrXirArtifact *ci_packet(const XrXirCompileContext *context,const char *path) {
    FILE *file=fopen(path,"rb");CHECK(file && !fseek(file,0,SEEK_END));long length=ftell(file);
    CHECK(length>0 && !fseek(file,0,SEEK_SET));void *bytes=NULL;
    CHECK(xr_compile_resources_alloc(context->resources,(size_t)length,&bytes)==XR_COMPILE_RESOURCE_OK);
    CHECK(fread(bytes,1,(size_t)length,file)==(size_t)length && !fclose(file));
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,bytes,(size_t)length,&checked,NULL)==XR_XIR_OK);
    memset(bytes,0xa5,(size_t)length);xr_compile_resources_free(bytes);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);return lowered;
}
#endif
#if CI_CONSUMER_KIND==0
static void ci_packet_selection(const XrXirModule *module,uint32_t selection[6]) {
    const char *names[]={"bits","shadow","initializations","all","otherValue","callable"};
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
static void ci_emit(XrXirArtifact *artifact,const char *path,const uint32_t selection[6]) {
    XrXirCSource source={0};CHECK(xr_xir_compile_emit_c(artifact,"ci_value",SIZE_MAX,&source)==XR_XIR_OK);
    FILE *file=fopen(path,"wb");CHECK(file && fwrite(source.text,1,source.length,file)==source.length);
    CHECK(fputs("\nconst uint32_t ci_value_selection[6]={",file)>=0);
    for(unsigned s=0;s<6;++s)CHECK(fprintf(file,"%s%uu",s?",":"",selection[s])>0);
    CHECK(fputs("};\n",file)>=0 && !fclose(file));xr_xir_compile_c_source_free(&source);
}
#endif
#if CI_CONSUMER_KIND==2 || CI_CONSUMER_KIND==4
static bool ci_engine_seen[2],ci_crossing,ci_code_retired;
typedef struct CiMixedEntry {XrXirVmBinding vm;XrXirCallEntry actual;bool native;struct CiMixedEntry *family;uint32_t count;} CiMixedEntry;
typedef struct CiMixedOwner {XrXirArtifact *artifact;XrXirCallEntry *entries;XrXirVmBinding *bindings;CiMixedEntry *mixed;} CiMixedOwner;
static XrXirAction ci_resume(XrXirCallView *view) {
    const CiMixedEntry *entry=view->environment;
    /* The leading VM binding preserves the real environment identity required
     * by owned call guards, while native entries use the same original view. */
    XrXirAction action=entry->actual.resume(view);CHECK(view->environment==entry);
    ci_engine_seen[entry->native?1:0]=true;
    if(action.kind==XR_XIR_ACTION_CALL){CHECK(action.callee<entry->count);if(entry->native!=entry->family[action.callee].native)ci_crossing=true;}
    return action;
}
static void ci_release(XrXirCallView *view,XrXirCallStatus reason) {
    const CiMixedEntry *entry=view->environment;
    entry->actual.release(view,reason);CHECK(view->environment==entry);
}
static void ci_code_release(void *pointer) {
    CiMixedOwner *owner=pointer;xr_xir_compile_artifact_free(owner->artifact);
    xr_compile_resources_free(owner->entries);xr_compile_resources_free(owner->bindings);xr_compile_resources_free(owner->mixed);
    xr_compile_resources_free(owner);ci_code_retired=true;
}
static XrXirProgram *ci_mixed_program(const XrXirCompileContext *context,XrXirArtifact *artifact) {
    CiMixedOwner *owner=NULL;CHECK(xr_compile_resources_calloc(context->resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);
    owner->artifact=artifact;const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    CHECK(module->function_count==ci_value_program.entry_count);
    CHECK(xr_xir_compile_vm_bind_table(artifact,&owner->bindings,&owner->entries)==XR_XIR_OK);
    CHECK(xr_compile_resources_calloc(context->resources,module->function_count,sizeof(*owner->mixed),(void **)&owner->mixed)==XR_COMPILE_RESOURCE_OK);
    for(uint32_t f=0;f<module->function_count;++f) {
        bool root=module->declarations->functions[f].module==module->declarations->root_module;
        CiMixedEntry *entry=&owner->mixed[f];entry->native=CI_CONSUMER_KIND==2?root:!root;
        entry->vm=owner->bindings[f];entry->family=owner->mixed;entry->count=module->function_count;
        entry->actual=entry->native?ci_value_program.entries[f]:owner->entries[f];
        owner->entries[f]=entry->actual;owner->entries[f].environment=entry;owner->entries[f].resume=ci_resume;owner->entries[f].release=ci_release;
    }
    XrXirProgramPacket packet=xir_program_packet_borrow(artifact);
    CHECK(packet.length==ci_value_program.packet.length && !memcmp(packet.identity,ci_value_program.packet.identity,32) &&
        !memcmp(packet.bytes,ci_value_program.packet.bytes,packet.length));
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},owner->entries,
        module->function_count,module->declarations,{owner,ci_code_release},module->types,packet};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(context,&spec,&program)==XR_XIR_OK);return program;
}
#endif
int main(int argc,char **argv) {
    XrXirCompileContext context=ci_context();XrXirProgram *program=NULL;uint32_t selection[6];
#if CI_CONSUMER_KIND==0
    CHECK(argc==3);XrXirArtifact *artifact=ci_packet(&context,argv[1]);
    ci_packet_selection(xr_xir_compile_artifact_module(artifact),selection);ci_emit(artifact,argv[2],selection);
    CHECK(xr_xir_compile_vm_program_take(&artifact,&program)==XR_XIR_OK && !artifact);
#else
    CHECK(argc==1);(void)argv;memcpy(selection,ci_value_selection,sizeof(selection));
#if CI_CONSUMER_KIND==1
    CHECK(xr_xir_compile_program_seal(&context,&ci_value_program,&program)==XR_XIR_OK);
#else
    XrXirArtifact *artifact=ci_packet(&context,XR_EXPORTED_F64_CONST_PACKET);program=ci_mixed_program(&context,artifact);
#endif
#endif
    XrXirValue kept=ci_runtime_cases(program,selection);xr_compile_resources_release(context.resources);ci_retained(&kept);
#if CI_CONSUMER_KIND==2 || CI_CONSUMER_KIND==4
    CHECK(ci_code_retired && ci_engine_seen[0] && ci_engine_seen[1] && ci_crossing);
    fprintf(stderr,"Public const actual VM/native crossing and code retirement PASS\n");
#endif
    fprintf(stderr,"Public i64 consumer%u canonical slot/callable retirement PASS\n",(unsigned)CI_CONSUMER_KIND);return 0;
}
