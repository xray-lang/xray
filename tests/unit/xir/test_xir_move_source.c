/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_move_source.c - Real consuming declarations and owned parameter bodies
 *
 * KEY CONCEPT:
 *   Source declares the consuming ordinal while its physical parameter stays
 *   the original value type. Rebinding uses a distinct owned local Cell.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_types.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"move Source %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "move_runtime.h"

int main(int argc,char **argv) {
    CHECK(argc==1||argc==2);
    static const char text[]=
        "fn consume(xs: move Array<i64>)->i64{xs.push(20);xs[0]=19;return xs[0]+xs[1]+1}\n"
        "fn moved()->i64{var xs=[20,22];return consume(move xs)}\n"
        "fn copied()->i64{var xs=[20,22];consume(xs);return len(xs)}\n"
        "fn constant()->i64{const xs=[20,22];return consume(move xs)}\n"
        "fn cow()->i64{var xs=[20,22];const saved=xs;consume(move xs);return saved[0]+saved[1]}\n"
        "fn indirect()->i64{var xs=[20,22];const action:fn(move Array<i64>)->i64=consume;return action(move xs)}\n"
        "fn restored()->i64{var xs=[20,22];fn read()->i64{return xs[0]+xs[1]};consume(move xs);xs=[40,2];return read()}\n"
        "fn snapshot()->i64{var xs=[20,22];const first=xs[0];consume(move xs);fn identity(x:i64)->i64{return x};return identity(first)}\n"
        "print(moved(),copied(),constant(),cow(),indirect(),restored(),snapshot())\n";
    const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"move-source",NULL};
    XrXirSourceRequest request={.session=session,.authority=&authority,.context=&context,.linkage_kind=XR_XIR_PROGRAM};
    XrXirSourceText input={.text=text,.length=sizeof(text)-1};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"Source status%u %d:%d %s\n",status,
        diagnostic.line,diagnostic.column,diagnostic.message?diagnostic.message:"");
    if(status!=XR_XIR_OK) {
        xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
        CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
        xr_compile_resources_release(context.resources);
        CHECK(status==XR_XIR_OK);return 1;
    }
    CHECK(status==XR_XIR_OK&&result.checked);
    const XrXirModule *module=xr_xir_compile_artifact_module(result.checked);uint32_t found=0;
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];
        if(function->name_length!=7||memcmp(function->name,"consume",7))continue;
        ++found;CHECK(function->parameter_count==1&&function->consume_parameter_count==1&&
            function->consume_parameters&&function->consume_parameters[0]==0&&
            xr_xir_type_is_array(module->types,function->parameters[0]));
    }
    CHECK(found==1);XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;XrXirDiagnostic issue={0};
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&checked,&issue)==XR_XIR_OK);
    memset(packet.bytes,0x5a,packet.length);xr_xir_compile_checked_packet_free(&packet);
    status=xr_xir_compile_specialize(checked,&closed,&issue,NULL);
    fprintf(stderr,"MOVE Spec%u f%u b%u i%u\n",status,issue.function,issue.block,issue.instruction);CHECK(status==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    status=xr_xir_compile_lower(closed,&target,NULL,&lowered,&issue);
    fprintf(stderr,"MOVE Lower%u f%u b%u i%u\n",status,issue.function,issue.block,issue.instruction);CHECK(status==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    uint32_t selected=xr_xir_compile_artifact_module(lowered)->declarations->entry_function;
    if(argc==2) {
        XrXirCSource code={0};CHECK(xr_xir_compile_emit_c(lowered,"move_values",8388608,&code)==XR_XIR_OK);
        FILE *file=fopen(argv[1],"wb");CHECK(file&&fwrite(code.text,1,code.length,file)==code.length);
        CHECK(fprintf(file,"\nconst uint32_t move_values_entry=%uu;\n",selected)>0&&!fclose(file));
        xr_xir_compile_c_source_free(&code);xr_xir_compile_artifact_free(lowered);
    } else {
        XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK);
        move_test_run(program,selected,&context);
    }
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
    xr_compile_resources_release(context.resources);CHECK(!runtime_live&&!runtime_bytes);return 0;
}
