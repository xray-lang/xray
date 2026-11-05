/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_path_flavours_source.h - Public lexical types through cold checked input
 */
#ifndef XIR_PATH_FLAVOURS_SOURCE_H
#define XIR_PATH_FLAVOURS_SOURCE_H
#include "base/xsha256.h"
static void path_previous134(const XrXirCompileContext *context){
    _Static_assert(XR_XIR_CHECKED_CONTRACT>134,"the native Path reader excludes the projected-cwd contract");
    FILE *file=fopen(XR_PATH_FIXTURES "/previous134-native-path/old134-path.xrchk","rb");CHECK(file);
    unsigned char *packet=xr_malloc(69634);CHECK(packet);size_t length=fread(packet,1,69634,file);
    CHECK(length==69633&&feof(file)&&!ferror(file)&&!fclose(file));
    static const uint8_t expected[32]={0x41,0x64,0xf1,0x6d,0xb5,0x9c,0x37,0x36,0xa8,0xae,0x84,0x38,0x16,0x81,0xa6,0xfd,0x74,0xb7,0xd2,0x12,0x15,0x43,0x14,0x81,0x38,0xa1,0xad,0xba,0x04,0xf3,0x8e,0x93};uint8_t digest[32];
    xr_sha256(packet,length,digest);CHECK(!memcmp(digest,expected,sizeof(digest)));
    CHECK(packet[8]==42&&packet[12]==134);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *output=NULL;CHECK(xr_xir_compile_checked_read(context,packet,length,&output,NULL)==XR_XIR_BAD_STRUCTURE&&!output);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK&&before.held_bytes==after.held_bytes);
    uint32_t future=XR_XIR_CHECKED_CONTRACT+1;for(unsigned i=0;i<4;++i)packet[12+i]=(uint8_t)(future>>(i*8));
    CHECK(xr_xir_compile_checked_read(context,packet,length,&output,NULL)==XR_XIR_BAD_STRUCTURE&&!output);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK&&before.held_bytes==after.held_bytes);
    memset(packet,0xa5,length);xr_free(packet);
}
static XrXirStatus path_flavours_check(const XrXirCompileContext *context,const char *text,
    XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic){
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_PROJECT,"path-shadow",XR_PATH_FIXTURES};
    XrXirSourceRequest request={session,XR_PATH_FIXTURES "/client.xr",&authority,context,XR_PATH_ROOT "/stdlib",NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={"client.xr",text,strlen(text)};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,output,diagnostic,NULL);
    xr_compile_session_free(session);return status;
}
static void path_flavours_contracts(const XrXirCompileContext *context){
    static const char *const modules[]={"posix","windows","logical","logical"};
    for(unsigned n=0;n<4;++n){
        char text[1024];int count=snprintf(text,sizeof(text),
            "import io\nimport path\nimport \"std/path/%s\" as foreign\nfn good(){io.exists(path.from(\"x\"))}\n",modules[n]);
        CHECK(count>0&&(size_t)count<sizeof(text));
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        CHECK(path_flavours_check(context,text,&result,&diagnostic)==XR_XIR_OK&&result.checked);
        xr_xir_compile_source_result_free(&result);
        const char *bad=n==3?"fn bad(){foreign.LogicalPath(\"unchecked\")}\n":
            n==2?"fn bad(){io.exists(foreign.from(\"x\")!)}\n":"fn bad(){io.exists(foreign.from(\"x\"))}\n";
        size_t suffix=strlen(bad);CHECK((size_t)count+suffix<sizeof(text));memcpy(text+count,bad,suffix+1);
        XrXirStatus status=path_flavours_check(context,text,&result,&diagnostic);
        const char *reason=n==3?"constructor arity or authority mismatch":"expression cannot satisfy its declared type";
        if(status!=XR_XIR_BAD_TYPE||diagnostic.module||diagnostic.line!=5||strcmp(diagnostic.message,reason))
            fprintf(stderr,"Path flavour refusal%u status%u module%u %d:%d %s\n",n,status,diagnostic.module,diagnostic.line,diagnostic.column,diagnostic.message);
        CHECK(status==XR_XIR_BAD_TYPE&&!result.checked&&!result.snapshot);
        CHECK(!diagnostic.module&&diagnostic.line==5&&!strcmp(diagnostic.message,reason));
        xr_xir_compile_source_result_free(&result);
    }
}
static void path_flavours_product(const XrXirCompileContext *context,const char *emit_base){
    path_previous134(context);
    FILE *file=fopen(XR_PATH_FIXTURES "/flavours.xr","rb");CHECK(file&&!fseek(file,0,SEEK_END));
    long bytes=ftell(file);CHECK(bytes>0&&!fseek(file,0,SEEK_SET));
    char *text=xr_malloc((size_t)bytes+1);CHECK(text);
    CHECK(fread(text,1,(size_t)bytes,file)==(size_t)bytes&&!fclose(file));text[bytes]=0;
    XrXirSourceResult source={0};CHECK(path_checked(context,text,XR_PATH_ROOT "/stdlib",NULL,&source)==XR_XIR_OK);
    memset(text,0xa5,(size_t)bytes+1);xr_free(text);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    XrXirArtifact *cold=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&cold,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(cold,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(cold);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},NULL,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t ids[]={module->declarations->entry_function,path_complete_named(module,"check")};
    if(emit_base){
        char path[2048];int count=snprintf(path,sizeof(path),"%s.flavours.c",emit_base);CHECK(count>0&&(size_t)count<sizeof(path));
        XrXirCSource generated={0};CHECK(xr_xir_compile_emit_c(lowered,"path_flavours",16777216,&generated)==XR_XIR_OK);
        file=fopen(path,"wb");CHECK(file&&fwrite(generated.text,1,generated.length,file)==generated.length);
        CHECK(fprintf(file,"\nconst uint32_t path_flavours_ids[2]={%u,%u};\n",ids[0],ids[1])>0&&!fclose(file));
        xr_xir_compile_c_source_free(&generated);xr_xir_compile_artifact_free(lowered);
    }else{
        path_flavours_contracts(context);
        XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK&&!lowered);
        path_flavours_execute(program,ids);xr_xir_compile_program_drop(program);
    }
}
#endif // XIR_PATH_FLAVOURS_SOURCE_H
