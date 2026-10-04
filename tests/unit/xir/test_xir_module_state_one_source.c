/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_module_state_one_source.c - A source-owned String module-state program
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
#include "xir_module_state_one_cases.h"
static uint32_t state_one_select(const XrXirModule *module,const char *name,XrXirType result) {
    CHECK(module && module->declarations);
    uint32_t entry=UINT32_MAX;size_t length=strlen(name);
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];
        if(function->name_length!=length || memcmp(function->name,name,length) ||
            module->declarations->functions[f].module!=module->declarations->root_module) continue;
        CHECK(entry==UINT32_MAX && function->parameter_count==0 && function->result==result);
        entry=f;
    }
    CHECK(entry!=UINT32_MAX); return entry;
}
static void state_one_visibility(void) {
    const XrXirCompileContext *context=source_program_owner(UINT64_C(64)*1024*1024,UINT64_C(128000000));
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_STATE_ONE_FIXTURES};
    XrXirSourceProductRequest request={{session,XR_STATE_ONE_FIXTURES "/reject-private.xr",&authority,context,
        XR_STATE_ONE_STDLIB,NULL,XR_XIR_PROGRAM,NULL},{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_product_build(&request,&product,&diagnostic);
    CHECK(status==XR_XIR_BAD_STRUCTURE && !product);
    CHECK(strstr(diagnostic.source.message,"import requires an exported declaration"));
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);xr_compile_session_free(session);
    CHECK(!runtime_live && !runtime_bytes);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);
    const XrXirCompileContext *context=source_program_owner(UINT64_C(64)*1024*1024,UINT64_C(128000000));
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_STATE_ONE_FIXTURES};
    XrXirSourceProductRequest request={{session,XR_STATE_ONE_FIXTURES "/root.xr",&authority,context,
        XR_STATE_ONE_STDLIB,NULL,XR_XIR_PROGRAM,NULL},{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
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
    CHECK(view && view->complete && view->module_count==4);
    /* Function selection reads a real, checked closed packet. It grants no
     * authority beyond the immutable declaration's exported root identity. */
    XrXirSourceProductPacketView packet={0};
    CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
    XrXirArtifact *checked=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(checked);
    uint32_t advance=state_one_select(module,"advance",XR_XIR_UNIT);
    uint32_t snapshot=state_one_select(module,"snapshot",XR_XIR_STRING);
    uint32_t read_left=state_one_select(module,"libraryLeft",XR_XIR_STRING);
    uint32_t read_right=state_one_select(module,"libraryRight",XR_XIR_STRING);
    StateOneShape shape=state_one_shape(module->declarations,advance,snapshot,read_left,read_right);
    xr_xir_compile_artifact_free(checked); checked=NULL;
    XrXirCSource source={0};
    CHECK(xr_xir_compile_source_product_emit(product,"module_state_one",UINT64_C(1)*1024*1024,&source)==XR_XIR_OK);
    if(argc==2) {
        FILE *file=fopen(argv[1],"wb"); CHECK(file);
        CHECK(fwrite(source.text,1,source.length,file)==source.length);
        CHECK(fprintf(file,"\nconst uint32_t module_state_one_advance=%uu;\n",advance)>0);
        CHECK(fprintf(file,"const uint32_t module_state_one_snapshot=%uu;\n",snapshot)>0);
        CHECK(fprintf(file,"const uint32_t module_state_one_left=%uu;\n",read_left)>0);
        CHECK(fprintf(file,"const uint32_t module_state_one_right=%uu;\n",read_right)>0);
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
    state_one_pair(program,shape);
    state_one_visibility();
    source_program_owners_free(); return 0;
}
