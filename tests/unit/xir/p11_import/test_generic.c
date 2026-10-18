/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_generic.c - Closed constant thunks in real generic lexical scopes
 */
#define XR_IMPORT_MAIN import_original_main
#include "test_import.c"

static const char generic_scalar[]=
    "fn compute<T>()->i64{return comptime 21}\n"
    "export fn run()->i64{return compute<i8>()+compute<string>()}\n";
static const char generic_calls[]=
    "fn square<T>(value:T)->T{return value}\n"
    "fn compute<T>()->i64{return comptime {var result:i64=0;for(var i=0;i<3;i++){result+=square<i64>(7)};result}}\n"
    "export fn run()->i64{return compute<i8>()+compute<string>()}\n";
static const char generic_owned[]=
    "fn compute<T>()->i64{const value=comptime (\"ab\",[19,21]);return len(value.0)+value.1[0]+value.1[1]}\n"
    "export fn run()->i64{return compute<i8>()+compute<string>()-42}\n";
static const char generic_method[]=
    "struct Box<T>{private value:T;static compute()->i64{return comptime 21}}\n"
    "export fn run()->i64{return Box<i8>.compute()+Box<string>.compute()}\n";
static const char prior_arguments[]=
    "fn id<T>(value:T)->T{return value}\n"
    "export fn run()->i64{const value=id<i64>(21);return value+comptime 21}\n";
static const char type_dependent[]=
    "fn compute<T>()->i64{return comptime {const values:Array<T>=[];42}}\n"
    "export fn run()->i64{return compute<i8>()}\n";
static const char runtime_capture[]=
    "fn compute<T>(value:i64)->i64{return comptime value}\n"
    "export fn run()->i64{return compute<i8>(42)}\n";
static const char shadow_type[]=
    "struct T{value:i64}\n"
    "fn compute<T>()->i64{return comptime {const values:Array<T>=[];42}}\n"
    "export fn run()->i64{return compute<i8>()}\n";

static XrXirStatus generic_source(const XrXirCompileContext *context,const char *text,bool library,
    XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic) {
    XrCompilerSession *session=NULL;XrCompilerSessionStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILER_SESSION_OK) return created==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
        created==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"generic-constant",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,library?XR_XIR_LIBRARY:XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,text,strlen(text)};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,output,diagnostic,NULL);
    xr_compile_session_free(session);return status;
}
static void generic_positive(const char *text,bool library) {
    XrXirCompileContext producer;context_new(&producer);XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=generic_source(&producer,text,library,&source,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"Generic constant Source=%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked && source.snapshot);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    XrXirCompileContext receiver;context_new(&receiver);XrXirArtifact *checked=NULL;
    CHECK(xr_xir_compile_checked_read(&receiver,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    xr_compile_resources_release(producer.resources);
    if (!library) {
        XrXirArtifact *closed=NULL,*lowered=NULL;CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
        const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
        CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
        XrXirCtfeCallRequest request=import_request(lowered,"run",NULL);XrXirValue value={0};
        CHECK(xr_xir_compile_ctfe_calls(&request,&value)==XR_XIR_CTFE_OK && value.type==XR_XIR_I64 && value.payload==42);
        xr_xir_value_drop(&value);xr_xir_compile_artifact_free(lowered);
    }
    xr_xir_compile_artifact_free(checked);xr_compile_resources_release(receiver.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
static void generic_negative(const char *text) {
    XrXirCompileContext context;context_new(&context);XrCompileResourceStats baseline=stats(&context);
    for (unsigned attempt=0;attempt<2;++attempt) {
        XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=generic_source(&context,text,false,&output,&diagnostic);
        fprintf(stderr,"Generic refusal attempt=%u status=%u %d:%d %s\n",attempt,status,diagnostic.line,diagnostic.column,diagnostic.message);
        XrXirStatus expected=text==runtime_capture?XR_XIR_BAD_VALUE:XR_XIR_BAD_TYPE;
        CHECK(status==expected && diagnostic.line>0 && !output.checked && !output.snapshot);
        CHECK(stats(&context).live_bytes==baseline.live_bytes);
    }
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
#include "generic_resources.h"
int main(int argc,char **argv) {
    CHECK(argc==2 || argc==3);unsigned mode=(unsigned)strtoul(argv[1],NULL,10);CHECK(mode<13);
    const char *positive[]={generic_scalar,generic_calls,generic_owned,generic_method,prior_arguments};
    if (argc==3) {
        CHECK(mode<10 && (!strcmp(argv[2],"--prefix") || !strcmp(argv[2],"--faults")));
        generic_resources(positive[mode%5],mode>=5,!strcmp(argv[2],"--prefix"));return 0;
    }
    if (mode<10) generic_positive(positive[mode%5],mode>=5);
    else if (mode==11) generic_negative(runtime_capture);
    else generic_positive(mode==10?type_dependent:shadow_type,false);
    return 0;
}
