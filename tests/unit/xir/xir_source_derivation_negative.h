/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_derivation_negative.h - Real Source policy and body refusal
 */
#ifndef XIR_SOURCE_DERIVATION_NEGATIVE_H
#define XIR_SOURCE_DERIVATION_NEGATIVE_H
static void derive_negative(const XrXirCheckedPacket *packet,const char *path,
    const char *intent,XrXirStatus expected,const char *message,const XrXirSourceComptimeLimits *limits) {
    const XrXirCompileContext *context=source_fixture_source_owner(67108864,128000000);
    XrXirLibraryModuleInput binding={{XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_DERIVATION_WORK},"generator.xr"};
    XrXirLibraryInput input={packet->bytes,packet->length,{0},&binding,1};xr_sha256(packet->bytes,packet->length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog)==XR_XIR_OK);
    char text[4096];int n=snprintf(text,sizeof(text),
        "import {generate} from \"./generator\"\ninterface Measure { measure()->i64 }\n%s\n"
        "struct Future { private const hidden:string;private const secret:i64\n"
        " init(hidden:string,secret:i64){this.hidden=hidden;this.secret=secret} }\n"
        "export fn main()->i64 { return 0 }\n",intent);
    CHECK(n>0 && n<(int)sizeof(text));derive_write(path,text);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_DERIVATION_WORK};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,XR_XIR_PROGRAM,catalog,limits};
    XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&output,&diagnostic,NULL);
    if (status!=expected || !strstr(diagnostic.message,message))
        fprintf(stderr,"derive refusal got=%u expected=%u %d:%d %s\n",(unsigned)status,(unsigned)expected,
            diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==expected && !output.checked && !output.snapshot && diagnostic.line>0 && diagnostic.column>0);
    CHECK(strstr(diagnostic.message,message));
    if (expected==XR_XIR_BUDGET) CHECK(!diagnostic.message[0]);
    if (strstr(message,"Future.secret")) CHECK(diagnostic.line==4 && diagnostic.column>30);
    xr_xir_compile_source_result_free(&output);xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);
    CHECK(!remove(path));source_fixture_source_owner_close(context);
}
static void derive_negative_cases(const XrXirCheckedPacket *packet,const char *path) {
    derive_negative(packet,path,"@derive(Measure, generate, describe: [\"secret\"])",XR_XIR_BAD_TYPE,"Future.secret",NULL);
    derive_negative(packet,path,"@derive(Measure, generate, read: [\"secret\"])",XR_XIR_BAD_STRUCTURE,"invalid logical body data",NULL);
    derive_negative(packet,path,"@derive(Measure, generate, describe: [\"missing\"])",XR_XIR_BAD_TYPE,"names no field",NULL);
    derive_negative(packet,path,"@derive(Measure, generate, describe: [\"secret\",\"secret\"])",XR_XIR_BAD_TYPE,"duplicate derive field operation",NULL);
    derive_negative(packet,path,"@derive(Measure, generate, write: [\"secret\"])",XR_XIR_BAD_TYPE,"definition policy",NULL);
    derive_negative(packet,path,"@derive(Measure, generate) @derive(Measure, generate)",XR_XIR_BAD_TYPE,"duplicate derive protocol",NULL);
    const XrXirSourceComptimeLimits limit={8},frozen=limit;
    derive_negative(packet,path,"@derive(Measure, generate, describe: [\"secret\"], read: [\"secret\"])",XR_XIR_BUDGET,"",&limit);
    CHECK(limit.output_bytes==frozen.output_bytes);
}
#endif // XIR_SOURCE_DERIVATION_NEGATIVE_H
