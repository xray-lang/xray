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
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"move Source %d: %s\n",__LINE__,#c);exit(1);}}while(0)

int main(void) {
    static const char text[]=
        "fn consume(xs: move Array<i64>)->i64{xs.push(20);return xs[0]+xs[1]}\n"
        "fn moved()->i64{var xs=[20,22];return consume(move xs)}\n"
        "fn copied()->i64{var xs=[20,22];consume(xs);return len(xs)}\n"
        "print(moved(),copied())\n";
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
    CHECK(found==1);xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
    xr_compile_resources_release(context.resources);return 0;
}
