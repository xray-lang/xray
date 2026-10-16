/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_json_aggregate_encode_negative.h - Aggregate field policy and type refusal
 */
#ifndef XIR_JSON_AGGREGATE_ENCODE_NEGATIVE_H
#define XIR_JSON_AGGREGATE_ENCODE_NEGATIVE_H
static void aggregate_reject(const XrXirCheckedPacket *packet,unsigned variant) {
    CHECK(variant<4);size_t live=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    XrXirCompileContext context=aggregate_context();
    XrXirLibraryModuleInput binding={{XR_MODULE_IDENTITY_SCRIPT,NULL,XR_JSON_AGGREGATE_WORK},"json_aggregates.xr"};
    XrXirLibraryInput input={packet->bytes,packet->length,{0},&binding,1};xr_sha256(packet->bytes,packet->length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(&context,&input,1,&catalog)==XR_XIR_OK);
    char text[sizeof(aggregate_user_source)];int n;
    if(variant<2) {
        const char *needle="read:[\"values\",\"optional\"]",*at=strstr(aggregate_user_source,needle);CHECK(at && !strstr(at+1,needle));
        n=snprintf(text,sizeof(text),"%.*sread:[\"%s\"]%s",(int)(at-aggregate_user_source),aggregate_user_source,
            variant?"optional":"values",at+strlen(needle));
    }else {
        const char *array=variant==2?"Array<bool>":"Array<i64>",*optional=variant==3?"bool?":"i64?";
        n=snprintf(text,sizeof(text),
            "import {generate,jsonIntegerArray,jsonOptionalInteger} from \"./json_aggregates\"\n"
            "interface Encode {encodeJSON()->string}\n"
            "@derive(Encode,generate,describe:[\"values\",\"optional\"],read:[\"values\",\"optional\"])\n"
            "struct Future {private const hidden:i64;private const values:%s;private const optional:%s\n"
            " init(values:%s,optional:%s){this.hidden=99;this.values=values;this.optional=optional} }\n"
            "export fn main()->i64{return 0}\n",array,optional,array,optional);
    }
    CHECK(n>0 && n<(int)sizeof(text));char path[2048];
    n=snprintf(path,sizeof(path),"%s/future_json_aggregates.xr",XR_JSON_AGGREGATE_WORK);CHECK(n>0 && n<(int)sizeof(path));
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_JSON_AGGREGATE_WORK};
    XrXirSourceRequest request={session,path,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,catalog,NULL};
    XrXirSourceText source={"future_json_aggregates.xr",text,strlen(text)};XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&source,&output,&diagnostic,NULL);
    XrCompileResourceStats spent={0};CHECK(xr_compile_resources_stats(context.resources,&spent)==XR_COMPILE_RESOURCE_OK);
    fprintf(stderr,"Aggregate refusal%u status%u %d:%d %s allocated=%llu peak=%llu work=%llu\n",variant,(unsigned)status,
        diagnostic.line,diagnostic.column,diagnostic.message,(unsigned long long)spent.allocated_bytes,
        (unsigned long long)spent.peak_bytes,(unsigned long long)spent.work);
    CHECK(!output.checked && !output.snapshot && diagnostic.line>0 && diagnostic.column>0);
    if(variant<2)CHECK(status==XR_XIR_BAD_TYPE && diagnostic.line==4 &&
        strstr(diagnostic.message,variant?"Future.values":"Future.optional"));
    else CHECK(status==XR_XIR_BAD_STRUCTURE && strstr(diagnostic.message,"invalid logical body data"));
    memset(text,0xa5,sizeof(text));xr_xir_compile_source_result_free(&output);xr_compile_session_free(session);
    xr_xir_compile_library_catalog_free(catalog);xr_compile_resources_release(context.resources);
    CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes && !runtime_live && !runtime_bytes);
}
#endif // XIR_JSON_AGGREGATE_ENCODE_NEGATIVE_H
