/* Public Source lifetime check against complete file and output fixtures. */
#include "xray_source_ast.h"
#include "xray_source_value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
typedef struct Bytes {char *data;size_t count,capacity;} Bytes;
static Bytes read_file(const char *path) {
    FILE *file=fopen(path,"rb");CHECK(file && !fseek(file,0,SEEK_END));long extent=ftell(file);
    CHECK(extent>=0 && !fseek(file,0,SEEK_SET));Bytes result={(char *)malloc((size_t)extent+1),(size_t)extent,(size_t)extent+1};CHECK(result.data);
    CHECK(fread(result.data,1,result.count,file)==result.count && !ferror(file) && !fclose(file));result.data[result.count]=0;return result;
}
static XrSourceWriteStatus capture(void *opaque,XrSourceOutputStream stream,const char *text,size_t count) {
    Bytes *output=(Bytes *)opaque;CHECK(stream==XR_SOURCE_STDOUT && count<=output->capacity-output->count);
    memcpy(output->data+output->count,text,count);output->count+=count;return XR_SOURCE_WRITE_OK;
}
static void one(const char *stdlib,const char *path,const char *golden) {
    Bytes source=read_file(path),expected=read_file(golden),output={(char *)malloc(expected.count+1),0,expected.count+1};CHECK(output.data);
    XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=stdlib;
    XrSourceText text={NULL,"hir-body-original",source.data,source.count};XrSourceAst *ast=NULL;
    XrSourceArtifact *artifact=NULL;XrSourceFailure failure={0};XrSourceDiagnostic *diagnostic=NULL;
    CHECK(xray_source_parse(&text,&options,&ast,&failure,&diagnostic)==XR_SOURCE_OK && ast && !diagnostic);
    memset(source.data,'!',source.count);free(source.data);memset(&text,0xa5,sizeof(text));
    XrSourceStatus status=xray_source_build_ast(ast,&options,&artifact,&failure,&diagnostic);
    if(status!=XR_SOURCE_OK && diagnostic) {XrSourceDiagnosticRoot root={0};CHECK(xray_source_diagnostic_root(diagnostic,0,&root)==XR_SOURCE_OK);fprintf(stderr,"%d:%d %s\n",root.primary.line,root.primary.column,root.message);}
    CHECK(status==XR_SOURCE_OK && artifact && !diagnostic);xray_source_ast_free(ast);memset(&options,0xa5,sizeof(options));
    XrSourceExecutionOptions policy=xray_source_execution_options_default();XrSourceExecution *execution=NULL;
    CHECK(xray_source_execution_new(&policy,&execution)==XR_SOURCE_OK);XrSourceCallOptions runtime;
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);runtime.write=capture;runtime.context=&output;
    XrSourceCall *call=NULL;CHECK(xray_source_artifact_begin(execution,artifact,&runtime,&call)==XR_SOURCE_OK);
    xray_source_artifact_free(artifact);xray_source_execution_free(execution);XrSourceEvent event={0};
    for(size_t tick=0;;++tick) {CHECK(tick<10000 && xray_source_call_poll(call,1024,&event)==XR_SOURCE_OK);if(event.state!=XR_SOURCE_RUN_READY)break;}
    CHECK(event.state==XR_SOURCE_RUN_RETURNED && output.count==expected.count && !memcmp(output.data,expected.data,expected.count));
    XrSourceResult *result=NULL;CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK);CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK && !call);
    XrSourceValue *value=NULL,*copy=NULL;CHECK(xray_source_result_value(result,&value)==XR_SOURCE_OK);xray_source_result_free(result);
    CHECK(xray_source_value_copy(value,&copy)==XR_SOURCE_OK);xray_source_value_free(value);XrSourceScalar scalar={0};
    CHECK(xray_source_value_scalar(copy,&scalar)==XR_SOURCE_OK && scalar.kind==XR_SOURCE_VALUE_I64 && scalar.value.signed_value==0);
    xray_source_value_free(copy);free(expected.data);free(output.data);
}
static void rejection(const char *stdlib,const char *path) {
    Bytes source=read_file(path);XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=stdlib;
    XrSourceText text={NULL,"hir-body-refusal",source.data,source.count};XrSourceInput input={0};input.kind=XR_SOURCE_INPUT_TEXT;input.value.text=text;
    XrSourceArtifact *artifact=NULL;XrSourceDiagnostic *diagnostic=NULL;XrSourceFailure failure={0};
    XrSourceStatus status=xray_source_build(&input,&options,&artifact,&failure,&diagnostic);
    CHECK(status!=XR_SOURCE_OK && !artifact && diagnostic);free(source.data);memset(&options,0xa5,sizeof(options));
    XrSourceDiagnosticReportInfo report={0};XrSourceDiagnosticRoot item={0};
    CHECK(xray_source_diagnostic_report(diagnostic,&report)==XR_SOURCE_OK && report.roots==1);
    CHECK(xray_source_diagnostic_root(diagnostic,0,&item)==XR_SOURCE_OK);
    printf("%u %d %d %s\n",(unsigned)status,item.primary.line,item.primary.column,item.message);
    xray_source_diagnostic_free(diagnostic);
}
int main(int argc,char **argv) {CHECK(argc>=4 && !(argc%2));for(int i=2;i<argc;i+=2){if(!strcmp(argv[i+1],"-"))rejection(argv[1],argv[i]);else one(argv[1],argv[i],argv[i+1]);}puts("Complete body original Source and producer/result retirement PASS");return 0;}
