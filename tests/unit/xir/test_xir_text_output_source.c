/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_text_output_source.c - A source-owned typed Rune and text program
 *
 * KEY CONCEPT:
 *   The same finite typed source product supplies native C and the VM Program.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)
#include "xir_source_program_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "xir_text_output_cases.h"
static uint32_t text_output_select(const XrXirModule *module,const char *name) {
    CHECK(module && module->declarations);
    uint32_t entry=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];
        if(function->name_length!=strlen(name) || memcmp(function->name,name,strlen(name)) ||
            module->declarations->functions[f].module!=module->declarations->root_module) continue;
        CHECK(entry==UINT32_MAX && module->declarations->functions[f].exported);
        entry=f;
    }
    CHECK(entry!=UINT32_MAX); return entry;
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);
    const XrXirCompileContext *context=source_program_owner(UINT64_C(64)*1024*1024,UINT64_C(128000000));
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_TEXT_OUTPUT_FIXTURES};
    XrXirSourceProductRequest request={{session,XR_TEXT_OUTPUT_FIXTURES "/root.xr",&authority,context,
        XR_TEXT_OUTPUT_STDLIB,NULL,XR_XIR_PROGRAM,NULL},{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product=NULL;
    XrXirSourceProductDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_product_build(&request,&product,&diagnostic);
    if(status!=XR_XIR_OK) fprintf(stderr,"source status=%u stage=%u line=%d: %s\n",status,
        diagnostic.stage,diagnostic.source.line,diagnostic.source.message);
    CHECK(status==XR_XIR_OK && product);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_compile_session_free(session); session=NULL;
    memset(&request,0xcc,sizeof(request)); memset(&authority,0xcc,sizeof(authority));
    CHECK(xr_xir_compile_source_product_context(product)->resources==context->resources);
    const XrXirSourceView *view=xr_xir_compile_source_product_view(product);
    CHECK(view && view->complete && view->module_count>=1);
    /* Function selection reads a real, checked closed packet. It grants no
     * authority beyond the immutable declaration's exported root identity. */
    XrXirSourceProductPacketView packet={0};
    CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
    XrXirArtifact *checked=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(checked);
    const char *names[]={"text","scalar","point","nul","operations","caught","ordinary"};
    uint32_t ids[7];
    for(unsigned i=0;i<7;++i) {
        ids[i]=text_output_select(module,names[i]);
        const XrXirFunction *fn=&module->functions[ids[i]];
        CHECK(fn->result==(i==1?XR_XIR_RUNE:i==3?XR_XIR_STRING:XR_XIR_I64));
        CHECK(fn->parameter_count==(i==1 || i==2?1u:0u));
        if(i==1 || i==2)CHECK(fn->parameters[0]==(i==1?XR_XIR_I64:XR_XIR_RUNE));
    }
    uint32_t advance=ids[0];
    xr_xir_compile_artifact_free(checked); checked=NULL;
    XrXirCSource source={0};
    CHECK(xr_xir_compile_source_product_emit(product,"text_output",UINT64_C(8)*1024*1024,&source)==XR_XIR_OK);
    if(argc==2) {
        FILE *file=fopen(argv[1],"wb"); CHECK(file);
        CHECK(fwrite(source.text,1,source.length,file)==source.length);
        CHECK(fprintf(file,"\nconst uint32_t text_output_entry=%uu;\n",advance)>0);
        CHECK(fprintf(file,"const uint32_t text_output_ids[7]={%uu,%uu,%uu,%uu,%uu,%uu,%uu};\n",
            ids[0],ids[1],ids[2],ids[3],ids[4],ids[5],ids[6])>0);
        CHECK(!fclose(file));
    }
    xr_xir_compile_c_source_free(&source);
    if(argc==2) {
        xr_xir_compile_source_product_free(product);
        CHECK(!runtime_live && !runtime_bytes); source_program_owners_free(); return 0;
    }
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK && program);
    XrXirProgram *second=NULL;
    CHECK(xr_xir_compile_source_product_vm_take(product,&second)==XR_XIR_BAD_STAGE && !second);
    xr_xir_compile_source_product_free(product); product=NULL;
    text_output_pair(program,ids,false);
    source_program_owners_free(); return 0;
}
