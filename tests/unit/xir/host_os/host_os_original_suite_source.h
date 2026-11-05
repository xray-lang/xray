/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_original_suite_source.h - Receive and emit complete original catalogs
 */
#ifndef HOST_OS_ORIGINAL_SUITE_SOURCE_H
#define HOST_OS_ORIGINAL_SUITE_SOURCE_H
#define HOST_ORIGINAL_SUITE_SOURCE 1
#include "host_os_original_suite_runtime.h"
#undef HOST_ORIGINAL_SUITE_SOURCE
#include "api/xsource_compile.h"

typedef struct HostOriginalCWriter {
    FILE *file;
    bool compare, okay;
    XrSHA256Context sha;
} HostOriginalCWriter;
static void host_original_c_put(HostOriginalCWriter *writer,const void *bytes,size_t length){
    if(!writer->okay)return;
    xr_sha256_update(&writer->sha,bytes,length);
    if(!writer->compare){writer->okay=fwrite(bytes,1u,length,writer->file)==length;return;}
    const unsigned char *expected=bytes;unsigned char buffer[4096];
    for(size_t at=0;at<length;){size_t size=length-at;if(size>sizeof(buffer))size=sizeof(buffer);
        if(fread(buffer,1u,size,writer->file)!=size||memcmp(buffer,expected+at,size)){writer->okay=false;return;}at+=size;}
}
static void host_original_c_text(HostOriginalCWriter *writer,const char *text){
    host_original_c_put(writer,text,strlen(text));
}
static void host_original_c_format(HostOriginalCWriter *writer,const char *format,...){
    char buffer[2048];va_list args;va_start(args,format);int size=vsnprintf(buffer,sizeof(buffer),format,args);va_end(args);
    if(size<0||(size_t)size>=sizeof(buffer)){writer->okay=false;return;}
    host_original_c_put(writer,buffer,(size_t)size);
}
static void host_original_c_string(HostOriginalCWriter *writer,const char *text){
    host_original_c_text(writer,"\"");
    for(const unsigned char *p=(const unsigned char *)text;*p;++p){
        if((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='/'||*p=='.')
            host_original_c_put(writer,p,1u);
        else host_original_c_format(writer,"\\%03o",(unsigned)*p);
    }
    host_original_c_text(writer,"\"");
}
static bool host_original_c_project(XrXirSourceProduct *product,unsigned index,
    HostOriginalCatalog *catalog,const char *directory,bool emit){
    const char *prefix=host_original_inputs[index].prefix;char path[4096];
    int size=snprintf(path,sizeof(path),"%s/%s.c",directory,prefix);
    if(size<1||(size_t)size>=sizeof(path))return false;
    XrXirCSource code={0};
    if(xr_xir_compile_source_product_emit(product,prefix,16777216u,&code)!=XR_XIR_OK)return false;
    FILE *file=fopen(path,emit?"wb":"rb");
    if(!file){xr_xir_compile_c_source_free(&code);return false;}
    HostOriginalCWriter writer={file,!emit,true,{0}};xr_sha256_init(&writer.sha);
    host_original_c_put(&writer,code.text,code.length);
    host_original_c_text(&writer,"\n/* The rows retain the actual SourceProduct test identities. */\n");
    host_original_c_text(&writer,"typedef struct HostOriginalWireRow {uint32_t function,role,timeout_seconds;const char *name;} HostOriginalWireRow;\n");
    host_original_c_format(&writer,"const HostOriginalWireRow %s_rows[%u]={\n",prefix,catalog->count);
    for(uint32_t i=0;i<catalog->count;++i){const HostOriginalWireRow *row=catalog->rows+i;
        host_original_c_format(&writer,"%s{%uu,%uu,%uu,",i?",":"",row->function,row->role,row->timeout_seconds);
        host_original_c_string(&writer,row->name);host_original_c_text(&writer,"}\n");}
    host_original_c_format(&writer,"};\nconst uint32_t %s_count=(uint32_t)(sizeof(%s_rows)/sizeof(%s_rows[0]));\n",prefix,prefix,prefix);
    host_original_c_format(&writer,"const char %s_source_sha256[65]=\"%s\";\n",prefix,catalog->source_sha256);
    if(!emit)writer.okay=writer.okay&&fgetc(file)==EOF&&!ferror(file);
    if(fclose(file))writer.okay=false;
    uint8_t digest[32];xr_sha256_final(&writer.sha,digest);host_original_hex(digest,catalog->generated_sha256);
    xr_xir_compile_c_source_free(&code);return writer.okay;
}
static bool host_original_copy_tests(XrXirSourceProduct *product,unsigned index,HostOriginalCatalog *catalog){
    const XrXirSourceTests *tests=xr_xir_compile_source_product_tests(product);
    const XrXirModule *module=xr_xir_compile_source_product_module(product);
    if(!tests||!module||!host_original_catalog_allocate(catalog,tests->count))return false;
    for(uint32_t i=0;i<tests->count;++i){const XrXirSourceTestEntry *row=tests->entries+i;
        if(row->function>=module->function_count)return false;
        const XrXirFunctionIdentity *identity=module->declarations->functions+row->function;
        if(identity->module!=module->declarations->root_module||identity->test_role!=row->role||identity->test_timeout_seconds!=row->timeout_seconds||
            !host_original_row_copy(catalog->rows+i,row->function,row->role,row->timeout_seconds,row->name,row->name_length))return false;
    }
    return host_original_catalog_valid(index,catalog);
}
static bool host_original_build(const XrXirCompileContext *context,unsigned index,
    HostOriginalResult *result,XrXirSourceProduct **product){
    char source[4096];
    if(!host_original_check_source(index,&result->catalog)||!host_original_path(source,sizeof(source),index)){
        snprintf(result->message,sizeof(result->message),"original source bytes differ or cannot be read");return false;}
    XrSourceCompileRequest request={context,source,XR_HOST_OS_ROOT "/stdlib",NULL,
        xr_source_compile_default_manifest_limits(),xr_xir_host_target(),NULL};
    XrSourceCompileDiagnostic diagnostic={0};
    XrSourceCompileStatus status=xr_source_compile_build(&request,product,&diagnostic);
    result->source_status=(uint32_t)status;result->source_stage=(uint32_t)diagnostic.stage;
    if(status!=XR_SOURCE_COMPILE_OK){
        char *rendered=NULL;
        if(xr_source_compile_diagnostic_render(&diagnostic,context->resources,&rendered)==XR_SOURCE_COMPILE_OK&&rendered){
            fputs(rendered,stderr);fputc('\n',stderr);
            snprintf(result->message,sizeof(result->message),"%s",rendered);xr_compile_resources_free(rendered);
        }else snprintf(result->message,sizeof(result->message),"Source compile status=%u stage=%u",(unsigned)status,(unsigned)diagnostic.stage);
    }
    xr_source_compile_diagnostic_free(&diagnostic);
    return status==XR_SOURCE_COMPILE_OK&&host_original_copy_tests(*product,index,&result->catalog);
}
static int host_original_source(unsigned index,const char *report_path){
    if(index>=HOST_ORIGINAL_FILE_COUNT)return 2;
#if defined(XR_OS_WINDOWS)
    if(_setmode(_fileno(stdout),_O_BINARY)==-1||_setmode(_fileno(stderr),_O_BINARY)==-1)return 1;
#endif
    FILE *file=fopen(report_path,"wbx");if(!file)return 2;
    HostOriginalResult r={0};r.index=index;r.engine="VM";r.phase="Source";r.cleanup=XR_XIR_CALL_READY;r.source_status=UINT32_MAX;
    r.physical_before=runtime_live;r.bytes_before=runtime_bytes;
    XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};XrXirSourceProduct *product=NULL;XrXirProgram *program=NULL;
    bool okay=xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK;
    XrCompileResourceStats before={0},after={0};
    if(okay)okay=xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK;
    r.held_before=before.held_bytes;
    if(okay)okay=host_original_build(&context,index,&r,&product);
    if(okay){r.phase="same_C";okay=host_original_c_project(product,index,&r.catalog,XR_HOST_OS_GENERATED,false);}
    if(okay){r.phase="vm_take";okay=xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK;}
    xr_xir_compile_source_product_free(product);
    if(okay){r.phase="executed";(void)host_original_run(program,&r,file);}else r.extra_error=true;
    xr_xir_compile_program_drop(program);r.program_retired=true;
    if(context.resources&&xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK)r.held_after=after.held_bytes;
    else r.extra_error=true;
    bool passed=host_original_report(file,&r,true);if(fclose(file))passed=false;
    if(context.resources)xr_compile_resources_release(context.resources);return passed?0:1;
}
/* Every original file is attempted, even after an earlier Source rejection. */
static bool host_original_emit(const char *directory){
    bool all=true;
    for(unsigned index=0;index<HOST_ORIGINAL_FILE_COUNT;++index){
        XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
        XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};XrXirSourceProduct *product=NULL;
        HostOriginalResult result={0};result.index=index;result.engine="emit";
        bool okay=xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK;
        if(okay)okay=host_original_build(&context,index,&result,&product);
        if(okay)okay=host_original_c_project(product,index,&result.catalog,directory,true);
        if(!okay){fprintf(stderr,"original suite emit rejected %s status=%u stage=%u %s\n",host_original_inputs[index].path,result.source_status,result.source_stage,result.message);all=false;}
        host_original_catalog_free(&result.catalog);xr_xir_compile_source_product_free(product);
        if(context.resources)xr_compile_resources_release(context.resources);
    }
    return all;
}
#endif // HOST_OS_ORIGINAL_SUITE_SOURCE_H
