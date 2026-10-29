/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_sdk_regex_consumer.inc.c - Complete regex after request retirement
 *
 * KEY CONCEPT:
 *   The host's exact byte sink preserves output without stdio translation.
 *   Ready execution and owned results outlive every input and compiler request.
 */
typedef struct SdkRegexControl {size_t polls;int retired;} SdkRegexControl;
typedef struct SdkRegexOutput {FILE *file;size_t bytes;} SdkRegexOutput;
static int sdk_regex_cancel(const void *opaque) {
    SdkRegexControl *control=(SdkRegexControl *)opaque;
    CHECK(!control->retired);++control->polls;return 0;
}
static XrSourceWriteStatus sdk_regex_write(void *opaque,XrSourceOutputStream stream,
    const char *bytes,size_t count) {
    SdkRegexOutput *output=(SdkRegexOutput *)opaque;
    CHECK(stream==XR_SOURCE_STDOUT && count<=SIZE_MAX-output->bytes);
    if(fwrite(bytes,1,count,output->file)!=count)return XR_SOURCE_WRITE_IO;
    output->bytes+=count;return XR_SOURCE_WRITE_OK;
}
static void sdk_regex_case(const char *source_path,const char *stdlib,const char *output_path) {
    FILE *input=fopen(source_path,"rb");CHECK(input && fseek(input,0,SEEK_END)==0);
    long extent=ftell(input);CHECK(extent==891 && fseek(input,0,SEEK_SET)==0);
    size_t length=(size_t)extent;char *source=(char *)malloc(length);CHECK(source);
    CHECK(fread(source,1,length,input)==length && fclose(input)==0);
    SdkRegexControl control={0,0};XrSourceCompileOptions options=xray_source_compile_options_default();
    options.stdlib_path=stdlib;options.cancel_poll=sdk_regex_cancel;options.cancel_context=&control;
    XrSourceText text={source_path,NULL,source,length};XrSourceAst *ast=NULL;
    XrSourceFailure failure={XR_SOURCE_OK,XR_SOURCE_OK,XR_SOURCE_STAGE_OPTIONS};
    XrSourceDiagnostic *diagnostic=NULL;
    CHECK(xray_source_parse(&text,&options,&ast,&failure,&diagnostic)==XR_SOURCE_OK && ast && !diagnostic);
    memset(source,0xCE,length);free(source);source=NULL;memset(&text,0xCE,sizeof(text));
    XrSourceArtifact *artifact=NULL;
    XrSourceStatus status=xray_source_build_ast(ast,&options,&artifact,&failure,&diagnostic);
    if(status!=XR_SOURCE_OK)fprintf(stderr,"regex compile status%u stage%u cleanup%u\n",status,failure.stage,failure.cleanup_status);
    CHECK(status==XR_SOURCE_OK && artifact && !diagnostic && control.polls);
    xray_source_ast_free(ast);ast=NULL;control.retired=1;size_t polls=control.polls;
    memset(&options,0xCE,sizeof(options));
    XrSourceArtifactInfo info={XR_SOURCE_TARGET_C,0,0,0,0};
    CHECK(xray_source_artifact_info(artifact,&info)==XR_SOURCE_OK && info.target==XR_SOURCE_TARGET_VM);
    XrSourceExecution *execution=sdk_execution_new();
    SdkRegexOutput output={fopen(output_path,"wb"),0};CHECK(output.file);
    XrSourceCallOptions runtime;CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    runtime.write=sdk_regex_write;runtime.context=&output;
    XrSourceCall *call=NULL;CHECK(xray_source_artifact_begin(execution,artifact,&runtime,&call)==XR_SOURCE_OK && call);
    xray_source_artifact_free(artifact);artifact=NULL;xray_source_execution_free(execution);execution=NULL;
    XrSourceEvent event;
    for(size_t i=0;;++i) {
        CHECK(i<1000000 && xray_source_call_poll(call,1024,&event)==XR_SOURCE_OK);
        if(event.state!=XR_SOURCE_RUN_READY)break;
    }
    CHECK(event.state==XR_SOURCE_RUN_RETURNED && control.polls==polls && output.bytes==92);
    XrSourceResult *result=NULL,*copy=NULL;
    CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK && result);
    CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK && !call);
    CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_RETURNED);
    CHECK(xray_source_result_copy(result,&copy)==XR_SOURCE_OK && copy);xray_source_result_free(result);
    CHECK(xray_source_result_event(copy,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_RETURNED);
    xray_source_result_free(copy);CHECK(fclose(output.file)==0);
    fprintf(stderr,"Installed complete891B regex: retired input/AST/request, ready execution and owned result release PASS\n");
}
