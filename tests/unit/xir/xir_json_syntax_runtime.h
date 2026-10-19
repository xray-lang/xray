/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_json_syntax_runtime.h - Independent complete JSON grammar expectations
 */
#ifndef XIR_JSON_SYNTAX_RUNTIME_H
#define XIR_JSON_SYNTAX_RUNTIME_H
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
static XrXirValue json_syntax_call(XrXirProgram *program,uint32_t entry,const char *bytes,size_t length) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    XrXirValue argument={0},result={0};CHECK(xr_xir_string_new(domain,bytes,length,&argument)==XR_XIR_VALUE_OK);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,&argument,1)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&argument);xr_xir_domain_drop(domain);
    XrXirCallStatus status;
    do {status=xr_xir_instance_poll_bounded(instance,4096).outcome.status;}while(status==XR_XIR_CALL_READY);
    CHECK(status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);return result;
}
static void json_syntax_expected(const XrXirValue *value,bool expected) {
    CHECK(value->type==XR_XIR_BOOL && !value->reserved && value->payload==(int64_t)expected);
}
static void json_syntax_case(XrXirProgram *program,uint32_t entry,const char *text,size_t size,bool expected) {
    XrXirValue value=json_syntax_call(program,entry,text,size);json_syntax_expected(&value,expected);xr_xir_value_drop(&value);
}
static void json_syntax_oracles(XrXirProgram *program,const uint32_t entries[4]) {
    const char *accepted[]={"null","true","false","0","-0","-12.5e+4","1e999999999999999999999999999",
        "\"\"","\"é汉😀\"","\"\\u6c49\\ud83d\\ude00\"","\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"",
        "[]","{}","[null,true,false,1,-1.5e-2,\"x\",{},[]]","{\"\":0}","{\"a\":1,\"b\":[2,{\"c\":null}]}",
        " \t\r\n{ \"a\" \t: [ 1 , 2 ] , \"b\" : true } \t\n","{\"a\":1,\"a\":2}","{\"\\u0000\":0}",
        "[[],[],[{},{}],{}]","[\"[{},:]\",\"\\\"\"]"};
    const char *rejected[]={""," ","nul","tru","False","TRUE","undefined","NaN","Infinity","+1","01","-01",
        "1.","1e",".1","0x10","truefalse","null null","1 2","[","{","[}","{]", "[,]","[1,]","[1,,2]",
        "[1 2]","[1:2]","{,}","{\"a\",1}","{\"a\":}","{\"a\":1,}","{\"a\":1 \"b\":2}","{a:1}",
        "{'a':1}","{\"a\"::1}","{\"a\":1,,\"b\":2}","[]x","{}[]","[true]false","/*x*/0","//x\n0",
        "\"","\"x","\"\\x\"","\"\\u123\"","\"\\ud800\"","\"\\udc00\"","\"\\ud800\\u0041\"",
        "{\"\\ud800\":0}","[\"\\udfff\"]","\"a\nb\"","\x0b" "0","\x0c" "0","\xc2\xa0" "0","\xef\xbb\xbf" "0"};
    size_t cases=0;
    for(unsigned i=0;i<sizeof(accepted)/sizeof(*accepted);++i) {
        json_syntax_case(program,entries[1],accepted[i],strlen(accepted[i]),true);++cases;
    }
    for(unsigned i=0;i<sizeof(rejected)/sizeof(*rejected);++i) {
        json_syntax_case(program,entries[1],rejected[i],strlen(rejected[i]),false);++cases;
    }
    const char *truncated[]={"[1,{\"a\":[true,false,null]}]","{\"汉😀\":\"\\ud83d\\ude00\"}","\"\\u0000\\n\\\"\""};
    for(unsigned i=0;i<sizeof(truncated)/sizeof(*truncated);++i)
        for(size_t n=0;n<strlen(truncated[i]);++n) {
            /* A String cannot contain a partial UTF-8 scalar. */
            if(n && ((unsigned char)truncated[i][n]&0xc0u)==0x80u)continue;
            json_syntax_case(program,entries[1],truncated[i],n,false);++cases;
        }
    for(unsigned c=0;c<128;++c) {
        char suffix[]={'1',(char)c},prefix[]={(char)c,'0'},key[]={'{','"',(char)c,'"',':','0','}'};
        bool whitespace=c==9 || c==10 || c==13 || c==32,digit=c>=48 && c<=57;
        json_syntax_case(program,entries[1],suffix,sizeof(suffix),whitespace || digit);++cases;
        json_syntax_case(program,entries[1],prefix,sizeof(prefix),whitespace || c==45 || (c>=49 && c<=57));++cases;
        json_syntax_case(program,entries[1],key,sizeof(key),c>=32 && c!=34 && c!=92);++cases;
    }
    const unsigned depths[]={1,2,32,128,257};
    for(unsigned n=0;n<sizeof(depths)/sizeof(*depths);++n) {
        char nested[520];unsigned depth=depths[n];memset(nested,'[',depth);nested[depth]='0';memset(nested+depth+1,']',depth);
        json_syntax_case(program,entries[1],nested,2*depth+1,true);++cases;
        json_syntax_case(program,entries[1],nested,2*depth,false);++cases;
        nested[depth+1]='}';json_syntax_case(program,entries[1],nested,2*depth+1,false);++cases;
    }
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entries[0],NULL,0)==XR_XIR_CALL_READY);
    XrXirCallStatus status;
    do {status=xr_xir_instance_poll_bounded(instance,4096).outcome.status;}while(status==XR_XIR_CALL_READY);
    CHECK(status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64 && !value.reserved && value.payload==42);xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    printf("Ordinary JSON syntax: %zu independent object/array/scalar cases, complete ASCII boundaries, truncations, depth257 and entry42\n",cases);
}
#endif // XIR_JSON_SYNTAX_RUNTIME_H
