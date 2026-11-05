/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_stdlib_checkedgen.c - Checked-only standard library generation
 *
 * KEY CONCEPT: The generator verifies and encodes Library input without CGen or VM.
 */
#include "base/xresource_limits.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_prelude_source_internal.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(test) do { if (!(test)) { fprintf(stderr,"Checked producer FAIL %d %s\n",__LINE__,#test); exit(1); } } while (0)
static void write_bytes(const char *path, const void *bytes, size_t length) {
    FILE *file = fopen(path,"wb"); CHECK(file);
    CHECK(fwrite(bytes,1,length,file) == length); CHECK(!fclose(file));
}
static void write_json_text(FILE *file,const char *text,uint32_t length) {
    CHECK(fputc('"',file)!=EOF);
    for(uint32_t i=0;i<length;++i){
        unsigned char c=(unsigned char)text[i];
        if(c=='"'||c=='\\'){CHECK(fputc('\\',file)!=EOF&&fputc(c,file)!=EOF);}
        else if(c<32){CHECK(fprintf(file,"\\u%04x",(unsigned)c)>0);}
        else CHECK(fputc(c,file)!=EOF);
    }
    CHECK(fputc('"',file)!=EOF);
}
static void write_modules(const XrXirCompileContext *context,const char *packet,const XrXirModule *module) {
    static const char suffix[]=".modules.json";size_t length=strlen(packet);char *path=NULL;
    CHECK(length<=SIZE_MAX-sizeof(suffix));
    CHECK(xr_compile_resources_alloc(context->resources,length+sizeof(suffix),(void **)&path)==XR_COMPILE_RESOURCE_OK);
    memcpy(path,packet,length);memcpy(path+length,suffix,sizeof(suffix));
    FILE *file=fopen(path,"wb");CHECK(file);
    CHECK(fprintf(file,"{\"schema\":1,\"stage\":%u,\"root_module\":%u,\"modules\":[",
        (unsigned)module->stage,(unsigned)module->declarations->root_module)>0);
    bool prelude=false;
    for(uint32_t m=0;m<module->declarations->module_count;++m){
        const XrXirSourceModule *row=module->declarations->modules+m;
        if(row->name_length==sizeof(XIR_PRELUDE_CANONICAL)-1&&
            !memcmp(row->name,XIR_PRELUDE_CANONICAL,sizeof(XIR_PRELUDE_CANONICAL)-1))prelude=true;
        CHECK(fprintf(file,"%s{\"ordinal\":%u,\"canonical\":",m?",":"",(unsigned)m)>0);
        write_json_text(file,row->name,row->name_length);
        CHECK(fputs(",\"dependencies\":[",file)!=EOF);
        for(uint32_t e=0;e<row->dependency_count;++e)
            CHECK(fprintf(file,"%s%u",e?",":"",(unsigned)row->dependencies[e])>0);
        CHECK(fputs("]}",file)!=EOF);
    }
    CHECK(fputc(']',file)!=EOF);
    if(prelude){
        CHECK(fputs(",\"generated_prelude\":{\"canonical\":\"" XIR_PRELUDE_CANONICAL "\",\"sourceKind\":\"generated-prelude\",\"text\":",file)!=EOF);
        size_t text_length=strlen(xir_prelude_source.text);CHECK(text_length<=UINT32_MAX);
        write_json_text(file,xir_prelude_source.text,(uint32_t)text_length);
        CHECK(fputs(",\"normalizedInputSha256\":\"",file)!=EOF);
        for(unsigned i=0;i<32;++i)CHECK(fprintf(file,"%02x",(unsigned)xir_prelude_source.input_sha256[i])==2);
        CHECK(fputs("\",\"normalizedRangeSha256\":\"",file)!=EOF);
        for(unsigned i=0;i<32;++i)CHECK(fprintf(file,"%02x",(unsigned)xir_prelude_source.range_input_sha256[i])==2);
        CHECK(fputs("\"}",file)!=EOF);
    }
    CHECK(fputs("}\n",file)!=EOF&&!fclose(file));xr_compile_resources_free(path);
}
int main(int argc, char **argv) {
    CHECK(argc == 5);
    XrCompileResourceLimits caps = XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&caps,&context.resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats initial = {0}; CHECK(xr_compile_resources_stats(context.resources,&initial) == XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context.resources,&session) == XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_STDLIB,argv[3],argv[1]};
    XrXirSourceRequest request = {session,argv[2],&authority,&context,argv[1],NULL,XR_XIR_LIBRARY,NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if (status != XR_XIR_OK) fprintf(stderr,"Checked library status=%u %s\n",status,diagnostic.message);
    CHECK(status == XR_XIR_OK);
    XrXirCheckedPacket library = {0}; CHECK(xr_xir_compile_checked_write(result.checked,&library,NULL) == XR_XIR_OK);
    XrXirArtifact *decoded = NULL;
    XrXirDiagnostic wire_diagnostic = {0};
    status = xr_xir_compile_checked_read(&context,library.bytes,library.length,&decoded,&wire_diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr,"Checked wire status=%u function=%u block=%u instruction=%u reason=%u\n",
        status,wire_diagnostic.function,wire_diagnostic.block,wire_diagnostic.instruction,wire_diagnostic.reason);
    CHECK(status == XR_XIR_OK);
    write_modules(&context,argv[4],xr_xir_compile_artifact_module(decoded));
    xr_xir_compile_artifact_free(decoded);
    write_bytes(argv[4],library.bytes,library.length);
    xr_xir_compile_checked_packet_free(&library);
    xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(session);
    XrCompileResourceStats final = {0};
    CHECK(xr_compile_resources_stats(context.resources,&final) == XR_COMPILE_RESOURCE_OK);
    CHECK(final.held_bytes == initial.held_bytes);
    xr_compile_resources_release(context.resources);
    return 0;
}
