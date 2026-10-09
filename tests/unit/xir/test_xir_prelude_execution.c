/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */
/* Same complete program through VM, native and both actual mixed call directions. */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if (!(x)) {fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}} while(0)
#if CONSUMER_KIND!=3
#include "base/xsha256.c"
#endif
#include "xir_library_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task_budget.c"
#include "xir/xxir_task.c"
#include "xir/xxir_specialize.c"
#include "xir/xxir_effects.c"
#include "xir/xxir_vm.c"
#if CONSUMER_KIND==0
#include "xir/xxir_emit_c.c"
#endif
#if CONSUMER_KIND==3
#include "xir/xxir_library_catalog.c"
#endif
#define XR_PRELUDE_RUNTIME_IMPLEMENTATION
#include "xir_prelude_runtime.h"
#if CONSUMER_KIND==1 || CONSUMER_KIND==2 || CONSUMER_KIND==4
extern const XrXirProgramSpec prelude_program;
extern const uint32_t prelude_consumer_answer;
#endif
#if CONSUMER_KIND==2 || CONSUMER_KIND==4
typedef struct PreludeMixed { XrXirVmBinding vm;XrXirCallEntry actual;bool native; } PreludeMixed;
static PreludeMixed prelude_mixed[128];static uint32_t prelude_mixed_count;
static uint64_t prelude_steps[2],prelude_crossings;
static XrXirAction prelude_mixed_resume(XrXirCallView *view) {
    PreludeMixed *binding=(PreludeMixed *)view->environment;
    CHECK(binding>=prelude_mixed && binding<prelude_mixed+prelude_mixed_count);
    ++prelude_steps[binding->native];XrXirAction action=binding->actual.resume(view);CHECK(view->environment==binding);
    if (action.kind==XR_XIR_ACTION_CALL) {
        CHECK(action.callee<prelude_mixed_count);
        if (binding->native!=prelude_mixed[action.callee].native) ++prelude_crossings;
    }
    return action;
}
static void prelude_mixed_release(XrXirCallView *view,XrXirCallStatus reason) {
    PreludeMixed *binding=(PreludeMixed *)view->environment;
    CHECK(binding>=prelude_mixed && binding<prelude_mixed+prelude_mixed_count);
    binding->actual.release(view,reason);CHECK(view->environment==binding);
}
#endif
#if CONSUMER_KIND==0 || CONSUMER_KIND==3
static uint32_t prelude_answer(const XrXirModule *module) {
    uint32_t answer=UINT32_MAX;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *fn=&module->functions[f];
        const XrXirFunctionIdentity *id=&module->declarations->functions[f];
        if (id->module==module->declarations->root_module && fn->name_length==14 && !memcmp(fn->name,"consumerAnswer",14)) {
            CHECK(id->exported && answer==UINT32_MAX);answer=f;
        }
    }
    CHECK(answer!=UINT32_MAX);return answer;
}
static XrXirArtifact *prelude_lower(XrXirArtifact *owned,uint32_t *answer) {
    const XrXirCompileContext *context=xr_xir_compile_artifact_context(owned);
    XrXirArtifact *read=NULL;
    for (unsigned round=0;round<2;++round) {
        XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(owned,&packet,NULL)==XR_XIR_OK);
        CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL)==XR_XIR_OK);
        xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(owned);owned=read;read=NULL;
        context=xr_xir_compile_artifact_context(owned);
    }
    CHECK(xr_xir_compile_artifact_verify(owned,NULL)==XR_XIR_OK);
    XrXirArtifact *special=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(owned,&special,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(owned);
    CHECK(xr_xir_compile_artifact_verify(special,NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(special,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(special);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);*answer=prelude_answer(xr_xir_compile_artifact_module(lowered));return lowered;
}
#endif
#if CONSUMER_KIND!=1
static XrXirProgramSpec prelude_vm_spec(XrXirArtifact *lowered,XrXirCallEntry *entries,XrXirVmBinding *bindings) {
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);CHECK(module->function_count<=128);
    XrXirProgramSpec spec={.abi_version=XR_XIR_PROGRAM_ABI_VERSION,
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.entry_count=module->function_count,
        .types=module->types,.declarations=module->declarations,.proof=xr_xir_compile_program_proof(lowered)};
    for (uint32_t f=0;f<module->function_count;++f) CHECK(xr_xir_compile_vm_bind(lowered,f,&bindings[f],&entries[f])==XR_XIR_OK);
    spec.entries=entries;return spec;
}
#endif
#if CONSUMER_KIND==3
XR_FUNC XrXirStatus xr_test_prelude_compile_pipeline(const XrXirCompileContext *context,const XrXirArtifact *checked) {
    if (!checked || xr_xir_compile_artifact_context(checked)->resources!=context->resources) return XR_XIR_BAD_STRUCTURE;
    XrXirArtifact *special=NULL,*lowered=NULL;XrXirProgram *program=NULL;
    XrXirStatus status=xr_xir_compile_specialize(checked,&special,NULL);
    if (status!=XR_XIR_OK) {CHECK(!special);goto done;}
    status=xr_xir_compile_artifact_verify(special,NULL);if (status!=XR_XIR_OK) goto done;
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    status=xr_xir_compile_lower(special,&target,&lowered,NULL);
    if (status!=XR_XIR_OK) {CHECK(!lowered);goto done;}
    status=xr_xir_compile_artifact_verify(lowered,NULL);if (status!=XR_XIR_OK) goto done;
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    if (module->function_count>128) {status=XR_XIR_BUDGET;goto done;}
    XrXirCallEntry entries[128];XrXirVmBinding bindings[128];
    for (uint32_t f=0;f<module->function_count;++f) {
        status=xr_xir_compile_vm_bind(lowered,f,&bindings[f],&entries[f]);if(status!=XR_XIR_OK)goto done;
    }
    XrXirProgramSpec spec={.abi_version=XR_XIR_PROGRAM_ABI_VERSION,.target=target,
        .entries=entries,.entry_count=module->function_count,.types=module->types,
        .declarations=module->declarations,.proof=xr_xir_compile_program_proof(lowered)};
    status=xr_xir_compile_program_seal(context,&spec,&program);CHECK(status==XR_XIR_OK ? program!=NULL : program==NULL);
done:
    xr_xir_compile_program_drop(program);xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(special);return status;
}
XR_FUNC void xr_test_prelude_run(const XrXirCompileContext *context,XrXirArtifact *owned) {
    uint32_t answer;XrXirArtifact *lowered=prelude_lower(owned,&answer);
    XrXirCallEntry entries[128];XrXirVmBinding bindings[128];XrXirProgramSpec spec=prelude_vm_spec(lowered,entries,bindings);
    prelude_finish(context,&spec,lowered,answer);
}
#else
int main(int argc,char **argv) {
    LibraryCompileOwner compiler={0};CHECK(library_compile_owner_new(&compiler,&library_compile_limits)==XR_XIR_OK);
    XrXirArtifact *lowered=NULL;uint32_t answer=UINT32_MAX;XrXirProgramSpec spec={0};
#if CONSUMER_KIND!=1
    XrXirArtifact *checked=NULL;
#endif
#if CONSUMER_KIND==0
    CHECK(argc==3);FILE *file=fopen(argv[1],"rb");CHECK(file && !fseek(file,0,SEEK_END));long length=ftell(file);
    CHECK(length>=64 && length<=262144 && !fseek(file,0,SEEK_SET));void *bytes=malloc((size_t)length);
    CHECK(bytes && fread(bytes,1,(size_t)length,file)==(size_t)length && !fclose(file));
    CHECK(xr_xir_compile_checked_read(&compiler.context,bytes,(size_t)length,&checked,NULL)==XR_XIR_OK);
    memset(bytes,0,(size_t)length);free(bytes);lowered=prelude_lower(checked,&answer);checked=NULL;
    XrXirCSource output={0};CHECK(xr_xir_compile_emit_c(lowered,"prelude",1048576,&output)==XR_XIR_OK);
    file=fopen(argv[2],"wb");CHECK(file && fwrite(output.text,1,output.length,file)==output.length);
    CHECK(fprintf(file,"\nconst uint32_t prelude_consumer_answer=%u;\n",answer)>0 && !fclose(file));xr_xir_compile_c_source_free(&output);
#else
    CHECK(argc==1);(void)argv;spec=prelude_program;answer=prelude_consumer_answer;
#if CONSUMER_KIND==2 || CONSUMER_KIND==4
    CHECK(xr_xir_compile_checked_read(&compiler.context,spec.proof.bytes,spec.proof.length,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_lower(checked,&spec.target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);checked=NULL;
#endif
#endif
#if CONSUMER_KIND!=1
    XrXirCallEntry entries[128];XrXirVmBinding bindings[128];spec=prelude_vm_spec(lowered,entries,bindings);
#if CONSUMER_KIND==2 || CONSUMER_KIND==4
    CHECK(spec.entry_count==prelude_program.entry_count);prelude_mixed_count=spec.entry_count;
    for (uint32_t f=0;f<spec.entry_count;++f) {
        PreludeMixed *binding=&prelude_mixed[f];
        binding->native=CONSUMER_KIND==2 ? f!=answer : f==answer;
        if (binding->native) {binding->actual=prelude_program.entries[f];CHECK(!binding->actual.environment);}
        else CHECK(xr_xir_compile_vm_bind(lowered,f,&binding->vm,&binding->actual)==XR_XIR_OK && binding->actual.environment==&binding->vm);
        entries[f]=binding->actual;entries[f].environment=binding;entries[f].resume=prelude_mixed_resume;entries[f].release=prelude_mixed_release;
    }
#endif
#endif
    prelude_finish(&compiler.context,&spec,lowered,answer);library_compile_owner_drop(&compiler);
#if CONSUMER_KIND==2 || CONSUMER_KIND==4
    CHECK(prelude_steps[0] && prelude_steps[1] && prelude_crossings);
    printf("Canonical prelude actual VMsteps=%llu nativeSteps=%llu crossings=%llu\n",
        (unsigned long long)prelude_steps[0],(unsigned long long)prelude_steps[1],(unsigned long long)prelude_crossings);
#endif
    library_compile_observer_free();printf("Canonical prelude consumer%u PASS\n",CONSUMER_KIND);return 0;
}
#endif
