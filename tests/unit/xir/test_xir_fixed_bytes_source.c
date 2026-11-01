/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_fixed_bytes_source.c - Complete original literals through cold Checked
 *
 * KEY CONCEPT:
 *   The actual unchanged demo survives producer retirement before specialization.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_generic.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"fixed source %d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_source_fixture_owner.h"
#include "fixed_bytes_product.h"
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILE_RESOURCE_OK);
    const char *root=XR_FIXED_BYTES_ROOT;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request={session,XR_FIXED_BYTES_ROOT "/demos/01-basics/quoted_literals.xr",
        &authority,&owner.context,XR_FIXED_BYTES_ROOT "/stdlib",NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&source,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"complete original status%u %u:%d:%d %s\n",status,
        diagnostic.module,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked);
    xr_compile_session_free(session);session=NULL;
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    XrXirArtifact *cold=NULL,*special=NULL,*code=NULL;
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&cold,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xcc,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(cold,&special,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(cold);
    XrXirDiagnostic lowered={0};
    status=xr_xir_compile_lower(special,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},
        NULL,&code,&lowered);
    if(status!=XR_XIR_OK)fprintf(stderr,"fixed lower status%u f%u/b%u/i%u\n",status,
        lowered.function,lowered.block,lowered.instruction);
    CHECK(status==XR_XIR_OK);xr_xir_compile_artifact_free(special);
    const XrXirModule *module=xr_xir_compile_artifact_module(code);
    uint32_t entry=module->declarations->entry_function;
    XrXirCSource native={0};CHECK(xr_xir_compile_emit_c(code,"fixed_bytes",16777216,&native)==XR_XIR_OK);
    if(argc==2) {
        FILE *file=fopen(argv[1],"wb");CHECK(file);
        CHECK(fwrite(native.text,1,native.length,file)==native.length);
        CHECK(fprintf(file,"\nconst uint32_t fixed_bytes_entry=%uu;\n",entry)>0 && !fclose(file));
    }
    xr_xir_compile_c_source_free(&native);
    if(argc==1) {
        XrXirProgram *program=NULL;status=xr_xir_compile_vm_program_take(&code,&program);
        if(status!=XR_XIR_OK)fprintf(stderr,"fixed VM Program status%u\n",status);
        CHECK(status==XR_XIR_OK);
        fixed_bytes_run(program,entry);xr_xir_compile_program_drop(program);
    }
    xr_xir_compile_artifact_free(code);source_fixture_owner_free(&owner);return 0;
}
