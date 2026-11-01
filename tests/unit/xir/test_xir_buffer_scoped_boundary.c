/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_buffer_scoped_boundary.c - Actual scope transport and closed host inputs
 *
 * KEY CONCEPT:
 *   Physical mutable signatures cannot replace an owned, independently checked
 *   begin-to-callback relationship. Failed reception preserves its input owner.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_program_internal.h"
#include "xir/xxir_program_packet_internal.h"
#include "xir/xxir_view_types_internal.h"
#include "xir/xxir_native_callable.h"
#include "toolchain/xcompiler_session.h"
#include "base/xresource_limits.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"Buffer boundary %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static const char input[]=
    "import mem\n"
    "fn edit(view:MutSlice<u8>){view[0]=7}\n"
    "export fn exercise()->i64{var buffer=mem.allocZeroed(4);"
    "unsafe{mem.withSliceMut<u8>(buffer.borrowPtr(),len(buffer),ref buffer,edit)};"
    "const view=buffer.asBytes();assert(view[0]==7);return 42}\n"
    "export fn rawLifetime()->i64{var buffer=mem.allocZeroed(4);unsafe{"
    "const p=buffer.borrowPtr();const q=p;mem.withSliceMut<u8>(p,len(buffer),ref buffer,fn(v:MutSlice<u8>){v[0]=7});"
    "q[0]=9};const result=buffer.asBytes();assert(result[0]==9);return 42}\n"
    "export fn shadow()->i64{var buffer=mem.allocZeroed(4);unsafe{"
    "mem.withSliceMut<u8>(buffer.borrowPtr(),len(buffer),ref buffer,fn(view:MutSlice<u8>){view[0]=7})};"
    "var view=buffer.asBytes();assert(view[0]==7);return 42}\n";
static uint64_t held(XrCompileResources *resources) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(resources,&stats)==XR_COMPILE_RESOURCE_OK);
    return stats.held_bytes;
}
static XrXirStatus source_check(const XrXirCompileContext *context,const char *stdlib,
    const char *text,XrXirSourceResult *output) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"buffer-scoped-boundary",NULL};
    XrXirSourceRequest request={.session=session,.authority=&authority,.context=context,
        .stdlib_path=stdlib,.linkage_kind=XR_XIR_PROGRAM};
    XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){.text=text,.length=strlen(text)},output,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"Boundary Source%u %d:%d %s\n",status,
        diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);return status;
}
static void source_refusals(const XrXirCompileContext *context,const char *stdlib) {
    const char *const cases[]={
        "fn bad(v:MutSlice<u8>){v[0]=1}\n",
        "import mem\nexport fn edit(v:MutSlice<u8>){v[0]=1}\nfn bad(){var b=mem.allocZeroed(4);unsafe{mem.withSliceMut<u8>(b.borrowPtr(),len(b),ref b,edit)}}\n",
        "import mem\nfn bad(callback:fn(MutSlice<u8>)){var b=mem.allocZeroed(4);unsafe{mem.withSliceMut<u8>(b.borrowPtr(),len(b),ref b,callback)}}\n",
        "import mem\nfn edit(v:MutSlice<u8>){v[0]=1}\nfn bad(){var b=mem.allocZeroed(4);var a:Array<u8>=[1];var v:MutSlice<u8>=a[:];edit(v);unsafe{mem.withSliceMut<u8>(b.borrowPtr(),len(b),ref b,edit)}}\n",
        "import mem\nfn edit(v:MutSlice<u8>){v[0]=1}\nfn bad(){var b=mem.allocZeroed(4);const saved=[edit];unsafe{mem.withSliceMut<u8>(b.borrowPtr(),len(b),ref b,edit)}}\n",
        "import mem\nfn bad(){var b=mem.allocZeroed(4);var other=mem.allocZeroed(4);unsafe{mem.withSliceMut<u8>(other.borrowPtr(),len(b),ref b,fn(v:MutSlice<u8>){v[0]=7})}}\n",
        "import mem\nfn bad(p:MutPtr<u8>,b:ref mem.Buffer){unsafe{mem.withSliceMut<u8>(p,len(b),ref b,fn(v:MutSlice<u8>){v[0]=7})}}\n",
        "import mem\nfn bad(){var b=mem.allocZeroed(4);unsafe{const p=b.borrowPtr();const q=p;mem.withSliceMut<u8>(p,len(b),ref b,fn(v:MutSlice<u8>){unsafe{q[0]=9};v[0]=7})}}\n",
        "import mem\nfn bad(){var b=mem.allocZeroed(4);unsafe{const p=b.borrowPtr();const q=p;mem.withSliceMut<u8>(p,len(b),ref b,fn(v:MutSlice<u8>){unsafe{assert(q[0]==0)};v[0]=7})}}\n",
        "import mem\nfn bad(){var b=mem.allocZeroed(4);unsafe{mem.withSliceMut<u8>(b.borrowPtr(),len(b),ref b,fn(v:MutSlice<u8>){b.resize(8);v[0]=7})}}\n",
        "import mem\nfn bad(){var b=mem.allocZeroed(4);unsafe{mem.withSliceMut<u8>(b.borrowPtr(),len(b),ref b,fn(v:MutSlice<u8>){b=mem.allocZeroed(8);v[0]=7})}}\n"
    };
    uint64_t frame=held(context->resources);
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        XrXirSourceResult out={0};XrXirStatus status=source_check(context,stdlib,cases[i],&out);
        fprintf(stderr,"Boundary refusal%zu status%u\n",i,status);CHECK(status!=XR_XIR_OK);
        CHECK(!out.checked&&!out.snapshot);xr_xir_compile_source_result_free(&out);
        CHECK(held(context->resources)==frame);
    }
}
static void same_module_refusal(const XrXirCompileContext *context,const char *stdlib) {
    char path[4096];int count=snprintf(path,sizeof(path),"%s/mem/mem.xr",stdlib);
    CHECK(count>0&&(size_t)count<sizeof(path));FILE *file=fopen(path,"rb");CHECK(file);
    CHECK(!fseek(file,0,SEEK_END));long length=ftell(file);CHECK(length>0&&!fseek(file,0,SEEK_SET));
    static const char suffix[]="\nfn nakedScope(p:MutPtr<u8>,bytes:i64,owner:ref Buffer){unsafe{const view=__bufferSliceMutBegin(p,bytes,ref owner)}}\n";
    CHECK((uint64_t)length<SIZE_MAX-sizeof(suffix));size_t size=(size_t)length;
    char *text=xr_malloc(size+sizeof(suffix));CHECK(text&&fread(text,1,size,file)==size&&!fclose(file));
    memcpy(text+size,suffix,sizeof(suffix));uint32_t line=1;
    for(size_t i=0;i<size;++i)line+=text[i]=='\n';
    uint64_t frame=held(context->resources);XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_STDLIB,"mem",stdlib};
    XrXirSourceRequest request={.session=session,.entry_path=path,.authority=&authority,.context=context,
        .stdlib_path=stdlib,.linkage_kind=XR_XIR_LIBRARY};
    XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){"mem/mem.xr",text,size+sizeof(suffix)-1},&output,&diagnostic,NULL);
    fprintf(stderr,"Same-module naked begin status%u line%d: %s\n",status,diagnostic.line,diagnostic.message);
    CHECK(status==XR_XIR_BAD_TYPE && diagnostic.line>(int)line&&!output.checked&&!output.snapshot);
    xr_xir_compile_source_result_free(&output);xr_compile_session_free(session);xr_free(text);
    CHECK(held(context->resources)==frame);
}
static bool cancelled(const void *owner){++*(unsigned *)owner;return true;}
static void control_cases(const XrXirCompileContext *context,const XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    const XrXirConstruction *construction=xr_xir_compile_artifact_construction(artifact);
    uint32_t function=UINT32_MAX,instruction=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)for(uint32_t i=0;i<module->functions[f].instruction_count;++i) {
        const XrXirInstruction *op=module->functions[f].instructions+i;
        if(op->op==XR_XIR_CALL&&op->immediate>=0&&(uint64_t)op->immediate<module->function_count&&
            module->declarations->functions[op->immediate].native_callable==XR_NATIVE_CALLABLE_BUFFER_SLICE_MUT_BEGIN)
            {function=f;instruction=i;}
    }
    CHECK(function!=UINT32_MAX);uint64_t frame=held(context->resources);
    for(unsigned mode=0;mode<2;++mode) {
        unsigned calls=0;XrCompileRequestOptions options={.cancel_poll=cancelled,.cancel_context=&calls};
        if(mode)options=(XrCompileRequestOptions){.deadline_mode=XR_COMPILE_DEADLINE_AFTER};
        XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        bool accepted=false;
        CHECK(xir_compile_buffer_scoped_site(context,(XirEffectInput){module,construction,&control},
            function,instruction,&accepted)==(mode?XR_XIR_DEADLINE:XR_XIR_CANCELLED)&&!accepted);
        CHECK(held(context->resources)==frame&&calls==(mode?0u:1u));
    }
    bool accepted=false;
    CHECK(xir_compile_buffer_scoped_site(context,(XirEffectInput){module,NULL,NULL},function,instruction,&accepted)==XR_XIR_BAD_STRUCTURE&&!accepted);
    CHECK(xir_compile_buffer_scoped_site(context,(XirEffectInput){module,construction,NULL},function,instruction,&accepted)==XR_XIR_OK&&accepted);
}
static void program_cases(const XrXirCompileContext *context,XrXirArtifact *code) {
    const XrXirModule *module=xr_xir_compile_artifact_module(code);
    XrXirVmBinding *bindings=NULL;XrXirCallEntry *entries=NULL;
    CHECK(xr_xir_compile_vm_bind_table(code,&bindings,&entries)==XR_XIR_OK);
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,*xr_xir_compile_artifact_target(code),
        entries,module->function_count,module->declarations,{0},module->types,xir_program_packet_borrow(code)};
    uint64_t frame=held(context->resources);XrXirProgram *program=NULL;XrXirProgramSpec missing=spec;missing.packet=(XrXirProgramPacket){0};
    CHECK(xr_xir_compile_program_seal(context,&missing,&program)!=XR_XIR_OK&&!program&&held(context->resources)==frame);
    XrXirProgramInput fake={.context=*context,.artifact=code},*fake_slot=&fake;
    CHECK(xir_program_receive(context,&spec,&fake_slot,NULL,&program)==XR_XIR_BAD_STRUCTURE&&fake_slot==&fake&&!program);
    XrXirProgramInput *owned=NULL;CHECK(xir_program_input_read(context,&spec.packet,&spec.target,&owned)==XR_XIR_OK);
    XrXirProgramInput *original=owned;uint64_t owned_frame=held(context->resources);
    uint32_t callback=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==4&&!memcmp(module->functions[f].name,"edit",4))callback=f;
    CHECK(callback!=UINT32_MAX&&entries[callback].parameter_count==1);
    XrXirCallEntry saved=entries[callback];XrXirType wrong=XR_XIR_I64;
    entries[callback].parameters=&wrong;
    CHECK(xir_program_receive(context,&spec,&owned,NULL,&program)!=XR_XIR_OK&&!program&&owned==original);
    CHECK(held(context->resources)==owned_frame);entries[callback]=saved;
    CHECK(xir_program_receive(context,&spec,&owned,NULL,&program)==XR_XIR_OK&&!owned&&program);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    /* A real scoped callback ordinal still grants no host entry or argument. */
    CHECK(xr_xir_instance_start(instance,callback,NULL,0)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    CHECK(held(context->resources)==frame);
    xr_compile_resources_free(entries);xr_compile_resources_free(bindings);
}
int main(int argc,char **argv) {
    CHECK(argc==2);XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    XrXirCompileContext context={.limits=xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);uint64_t frame=held(context.resources);
    source_refusals(&context,argv[1]);same_module_refusal(&context,argv[1]);
    XrXirSourceResult source={0};CHECK(source_check(&context,argv[1],input,&source)==XR_XIR_OK);
    control_cases(&context,source.checked);XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);XrXirArtifact *cold=NULL,*closed=NULL,*code=NULL;
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&cold,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(cold,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(cold);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},NULL,&code,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);program_cases(&context,code);xr_xir_compile_artifact_free(code);
    CHECK(held(context.resources)==frame);xr_compile_resources_release(context.resources);
    puts("Buffer boundary: actual scope, cold ownership, Program refusal and recovery PASS");return 0;
}
