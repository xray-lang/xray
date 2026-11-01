/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_buffer_slice_call.c - Real memory source through cold borrowed calls
 *
 * KEY CONCEPT:
 *   The actual stdlib produces and independently checks receiver facts. No
 *   fixture invents a return row, native signature or caller scope authority.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_generic.h"
#include "toolchain/xcompiler_session.h"
#include "base/xresource_limits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"buffer call %d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "buffer_slice_call_cases.h"
static const char input[]=
    "import mem\n"
    "fn read(v:Slice<u8>)->i64{return v[0] as i64}\n"
    "export fn run()->i64{var buffer=mem.allocZeroed(4);"
    "unsafe{const pointer=buffer.borrowPtr();pointer[0]=42 as u8};"
    "const view:Slice<u8>=buffer.asBytes();const first=read(view);const second=view[0] as i64;"
    "if(first!=42 || second!=42 || len(view)!=4){return -1};return first}\n"
    "export fn resized()->i64{var buffer=mem.allocZeroed(4);"
    "unsafe{const pointer=buffer.borrowPtr();pointer[0]=42 as u8};"
    "const view:Slice<u8>=buffer.asBytes();const before=view[0] as i64;"
    "const changed=buffer.resize(8);const next:Slice<u8>=buffer.asBytes();"
    "if(!changed || len(next)!=8){return -2};return before}\n"
    "enum ScopedError{Expected}\n"
    "fn scopedEdit(view:MutSlice<u8>){view[0]=7;view[1]=9}\n"
    "fn scopedThrow(view:MutSlice<u8>) throws(ScopedError){view[0]=7;throw ScopedError.Expected}\n"
    "fn scopedPanicEdit(view:MutSlice<u8>){view[0]=7;assert(false)}\n"
    "export fn scoped()->i64{var buffer=mem.allocZeroed(4);"
    "unsafe{mem.withSliceMut<u8>(buffer.borrowPtr(),len(buffer),ref buffer,scopedEdit)};"
    "const view=buffer.asBytes();if(view[0]!=7 || view[1]!=9 || len(view)!=4){return -3};"
    "if(!buffer.resize(8)){return -4};return 42}\n"
    "export fn scopedError()->i64{var buffer=mem.allocZeroed(4);var caught=false;"
    "unsafe{try{try mem.withSliceMut<u8>(buffer.borrowPtr(),len(buffer),ref buffer,scopedThrow)}"
    "catch(error:ScopedError){caught=true}};"
    "const view=buffer.asBytes();if(!caught || view[0]!=7 || len(view)!=4){return -5};"
    "if(!buffer.resize(8)){return -6};return 42}\n"
    "export fn scopedPanic()->i64{var buffer=mem.allocZeroed(4);"
    "unsafe{mem.withSliceMut<u8>(buffer.borrowPtr(),len(buffer),ref buffer,scopedPanicEdit)};return -7}\n";
static uint64_t held(XrCompileResources *resources) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(resources,&stats)==XR_COMPILE_RESOURCE_OK);
    return stats.held_bytes;
}
static XrXirStatus check_source(const XrXirCompileContext *context,const char *stdlib,const char *text,
    XrXirSourceResult *output) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"buffer-slice-call",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,stdlib,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){NULL,text,strlen(text)},output,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"Source status%u module%u %d:%d %s\n",status,
        diagnostic.module,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);return status;
}
static uint32_t named(const XrXirModule *module,const char *name) {
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==strlen(name) && !memcmp(module->functions[f].name,name,strlen(name))) return f;
    CHECK(false);return UINT32_MAX;
}
static void source_refusals(const XrXirCompileContext *context,const char *stdlib) {
    const char *cases[]={
        "import mem\nfn bad(){var buffer=mem.allocZeroed(4);const view=buffer.asBytes();buffer.resize(8);print(view[0])}\n",
        "import mem\nfn bad()->Slice<u8>{var buffer=mem.allocZeroed(4);return buffer.asBytes()}\n",
        "import mem\nclass Other{value:i64=0;asBytes()->Slice<u8> from this{const buffer=mem.allocZeroed(4);return buffer.asBytes()}}\n",
        "import mem\nfn edit(view:MutSlice<u8>){view[0]=7}\nfn bad(){var buffer=mem.allocZeroed(4);mem.withSliceMut<u8>(buffer.borrowPtr(),len(buffer),ref buffer,edit)}\n",
        "import mem\nfn edit(view:Slice<u8>){print(len(view))}\nfn bad(){var buffer=mem.allocZeroed(4);unsafe{mem.withSliceMut<u8>(buffer.borrowPtr(),len(buffer),ref buffer,edit)}}\n",
        "import mem\nfn edit(view:MutSlice<u8>){view[0]=7}\nfn bad(){var buffer=mem.allocZeroed(4);var other=mem.allocZeroed(4);unsafe{mem.withSliceMut<u8>(other.borrowPtr(),len(buffer),ref buffer,edit)}}\n",
        "import mem\nfn bad(){var buffer=mem.allocZeroed(4);unsafe{mem.withSliceMut<u8>(buffer.borrowPtr(),len(buffer),ref buffer,fn(view:MutSlice<u8>){buffer.resize(8)})}}\n",
        "import mem\nfn edit(view:MutSlice<u8>){view[0]=7}\nfn bad(){var mem=0;var owner=1;unsafe{mem.withSliceMut<u8>(0,4,ref owner,edit)}}\n"};
    for (size_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        uint64_t frame=held(context->resources);XrXirSourceResult rejected={0};
        XrXirStatus status=check_source(context,stdlib,cases[i],&rejected);
        CHECK(status!=XR_XIR_OK && !rejected.checked);xr_xir_compile_source_result_free(&rejected);
        CHECK(held(context->resources)==frame);
    }
}
int main(int argc,char **argv) {
    bool emit_only=argc==4 && !strcmp(argv[1],"--emit");CHECK(emit_only || argc==2 || argc==3);
    const char *root=argv[emit_only?2:1],*path=emit_only?argv[3]:argc==3?argv[2]:NULL;
    char stdlib[2048];int length=snprintf(stdlib,sizeof(stdlib),"%s/stdlib",root);
    CHECK(length>0 && (size_t)length<sizeof(stdlib));
    XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);uint64_t frame=held(context.resources);
    XrXirSourceResult source={0};CHECK(check_source(&context,stdlib,input,&source)==XR_XIR_OK && source.checked);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    XrXirArtifact *cold=NULL,*closed=NULL,*code=NULL;
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&cold,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xcc,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(cold,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(cold);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},NULL,&code,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);const XrXirModule *module=xr_xir_compile_artifact_module(code);
    uint32_t ids[]={module->declarations->entry_function,named(module,"run"),named(module,"resized"),
        named(module,"scoped"),named(module,"scopedError"),named(module,"scopedPanic"),named(module,"scopedEdit")};
    if (path) {
        XrXirCSource generated={0};CHECK(xr_xir_compile_emit_c(code,"buffer_slice_call",16777216,&generated)==XR_XIR_OK);
        FILE *file=fopen(path,emit_only?"wb":"rb");CHECK(file);
        if (emit_only) {
            CHECK(fwrite(generated.text,1,generated.length,file)==generated.length);
            CHECK(fprintf(file,"\nconst uint32_t buffer_slice_call_ids[7]={%u,%u,%u,%u,%u,%u,%u};\n",
                ids[0],ids[1],ids[2],ids[3],ids[4],ids[5],ids[6])>0);
        } else {
            for (size_t i=0;i<generated.length;++i) CHECK(fgetc(file)==(unsigned char)generated.text[i]);
            char tail[160];int count=snprintf(tail,sizeof(tail),"\nconst uint32_t buffer_slice_call_ids[7]={%u,%u,%u,%u,%u,%u,%u};\n",
                ids[0],ids[1],ids[2],ids[3],ids[4],ids[5],ids[6]);
            CHECK(count>0 && (size_t)count<sizeof(tail));
            for (int i=0;i<count;++i) CHECK(fgetc(file)==(unsigned char)tail[i]);
            CHECK(fgetc(file)==EOF);
        }
        CHECK(!fclose(file));xr_xir_compile_c_source_free(&generated);
    }
    if (emit_only) xr_xir_compile_artifact_free(code);
    else {
        XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&code,&program)==XR_XIR_OK && !code);
        buffer_call_cases(program,ids);xr_xir_compile_program_drop(program);source_refusals(&context,stdlib);
    }
    CHECK(held(context.resources)==frame);xr_compile_resources_release(context.resources);
    puts(emit_only?"Buffer call C emitted; execution NOT_RUN":"Buffer caller: genuine native/wrapper scopes and metadata retirement PASS");return 0;
}
