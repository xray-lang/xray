/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_derivation_enum_negative.h - Ordinary enum generation refusals
 */
#ifndef XIR_SOURCE_DERIVATION_ENUM_NEGATIVE_H
#define XIR_SOURCE_DERIVATION_ENUM_NEGATIVE_H
static const char enum_invalid_generator_format[]=
    "%s"
    "type MethodInfo=(string,i64,i64,i64,i64)\n"
    "type CalleeInfo=(string,string,i64,i64,i64,i64)\n"
    "type DeriveContext=(Array<MethodInfo>,Array<CalleeInfo>,Array<i64>,i64,Array<string>)\n"
    "export fn generate(info:TypeInfo,context:DeriveContext)->Generated {\n"
    " const functions:Array<FunctionRow>=[(context.0[0].0,0,0,context.0[0].1,0,1,0,1,0,2,0,0,0,0)]\n"
    " const parameters:Array<i64>=[info.0];const blocks:Array<BlockRow>=[(0,2,0,0)]\n"
    " const code:Array<InstructionRow>=[(%u,-%u,0,0,0,0,1,0,0,%d),(%u,-1,1,0,0,0,0,0,0,-1)]\n"
    " const operands:Array<i64>=[];const literals:Array<string>=[];const arguments:Array<i64>=[]\n"
    " return (1,functions,parameters,blocks,code,operands,literals,arguments)\n}\n";
static const char enum_ambiguous_user_source[]=
    "import {generate} from \"./generator_enum\"\n"
    "interface CaseName { caseName()->string }\n"
    "@derive(CaseName,generate,read:[\"value\"])\n"
    "enum Future { Alpha { value:i64 }, Payload { value:i64 }, Omega }\n"
    "export fn main()->i64 {return 0}\n";
static void enum_negative(unsigned which) {
    CHECK(which<4);const XrXirCompileContext *context=source_fixture_source_owner(67108864,128000000);
    char text[18000];int n;
    if(which>=2) n=snprintf(text,sizeof(text),enum_generator_format,generated_schema_source,
        XR_XIR_NOMINAL_ENUM,XR_XIR_ENUM_TAG,XR_XIR_CONST_INT,XR_XIR_EQ_INT,XR_XIR_BRANCH,
        XR_XIR_CONST_STRING,XR_XIR_RETURN,XR_XIR_I64+1,XR_XIR_BOOL+1);
    else n=snprintf(text,sizeof(text),enum_invalid_generator_format,generated_schema_source,
        which?XR_XIR_ENUM_GET:XR_XIR_CONST_INT,XR_XIR_I64+1,which?0:-1,XR_XIR_RETURN);
    CHECK(n>0 && n<(int)sizeof(text));XrXirSourceResult producer={0};
    enum_source(context,text,XR_XIR_LIBRARY,NULL,&producer);memset(text,0xa5,sizeof(text));
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(producer.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&producer);
    XrXirLibraryModuleInput binding={{XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_DERIVATION_ENUM_WORK},"generator_enum.xr"};
    XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};xr_sha256(packet.bytes,packet.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    char path[2048];n=snprintf(path,sizeof(path),"%s/future_enum.xr",XR_SOURCE_DERIVATION_ENUM_WORK);
    CHECK(n>0 && n<(int)sizeof(path));XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_DERIVATION_ENUM_WORK};
    const XrXirSourceComptimeLimits limits={8},saved=limits;
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,XR_XIR_PROGRAM,catalog,which==2?&limits:NULL};
    const char *user=which==3?enum_ambiguous_user_source:enum_user_source;
    XrXirSourceText source={"future_enum.xr",user,strlen(user)};
    XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&source,&output,&diagnostic,NULL);
    if(status!=(which==2?XR_XIR_BUDGET:XR_XIR_BAD_TYPE))
        fprintf(stderr,"enum refusal%u status%u %d:%d %s\n",which,(unsigned)status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==(which==2?XR_XIR_BUDGET:XR_XIR_BAD_TYPE) && !output.checked && !output.snapshot);
    if(which==2) {
        if(diagnostic.message[0] || diagnostic.line!=4 || diagnostic.column!=6)
            fprintf(stderr,"enum output refusal %d:%d %s\n",diagnostic.line,diagnostic.column,diagnostic.message);
        CHECK(!diagnostic.message[0] && diagnostic.line==4 && diagnostic.column==6 && limits.output_bytes==saved.output_bytes);
    }
    else if(which==3) {
        CHECK(strstr(diagnostic.message,"ambiguous derive field name") && diagnostic.line==4 && diagnostic.column==6);
    } else {
        const char *field=which?"Future.Payload.value":"Future";
        if(!strstr(diagnostic.message,field)) fprintf(stderr,"enum refusal%u %d:%d %s; expected field %s\n",
            which,diagnostic.line,diagnostic.column,diagnostic.message,field);
        CHECK(strstr(diagnostic.message,field) && diagnostic.column>0 && diagnostic.line==(which?4:3));
        if(which) {
            const char *line=strstr(enum_user_source,"enum Future"),*payload=strstr(enum_user_source,"value:i64");
            CHECK(line && payload && diagnostic.column==(int)(payload-line)+1);
        }
    }
    xr_xir_compile_source_result_free(&output);xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);
    source_fixture_source_owner_close(context);
}
static void enum_negative_cases(void) {for(unsigned which=0;which<4;++which)enum_negative(which);}
#endif // XIR_SOURCE_DERIVATION_ENUM_NEGATIVE_H
