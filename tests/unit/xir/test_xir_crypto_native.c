/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"Crypto %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "crypto_native_runtime.h"
static void crypto_product(const XrXirCompileContext *context,const char *root,const char *entry,
    const char *expected,const char *output,const char *prefix) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_PROJECT,"crypto-native",root};
    char stdlib[1024];CHECK(snprintf(stdlib,sizeof(stdlib),"%s/stdlib",root)>0);
    XrXirSourceRequest request={.session=session,.entry_path=entry,.authority=&authority,.context=context,.stdlib_path=stdlib,.linkage_kind=XR_XIR_PROGRAM};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};XrXirSourceFailure failure={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&source,&diagnostic,&failure);
    fprintf(stderr,"SOURCE %u %s:%d:%d %s\n",status,failure.path?failure.path:entry,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK);xr_xir_compile_source_failure_free(&failure);xr_compile_session_free(session);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;XrXirDiagnostic issue={0};
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    status=xr_xir_compile_specialize(checked,&closed,&issue,NULL);fprintf(stderr,"SPECIALIZE %u f%u b%u i%u\n",status,issue.function,issue.block,issue.instruction);CHECK(status==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,&issue)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    uint32_t selected=xr_xir_compile_artifact_module(lowered)->declarations->entry_function;
    if(output) {
        XrXirCSource code={0};CHECK(xr_xir_compile_emit_c(lowered,prefix,8388608,&code)==XR_XIR_OK);
        FILE *file=fopen(output,"wb");CHECK(file&&fwrite(code.text,1,code.length,file)==code.length);
        CHECK(fprintf(file,"\nconst uint32_t %s_entry=%uu;\n",prefix,selected)>0&&!fclose(file));xr_xir_compile_c_source_free(&code);
    }
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK);crypto_run(program,selected,expected);
}
int main(int argc,char **argv) {
    CHECK(argc==2 || argc==4);XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrCompileResourceStats before,after;CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/demos/07-advanced/crypto_entropy.xr",argv[1])>0);
    crypto_product(&context,argv[1],path,"6\n",argc==4?argv[2]:NULL,"crypto_demo");
    CHECK(snprintf(path,sizeof(path),"%s/tests/unit/xir/fixtures/crypto_import.xr",argv[1])>0);
    crypto_product(&context,argv[1],path,"1\n",argc==4?argv[3]:NULL,"crypto_import");
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && before.held_bytes==after.held_bytes);
    xr_compile_resources_release(context.resources);puts("Complete entropy demo and crypto import VM PASS");return 0;
}
