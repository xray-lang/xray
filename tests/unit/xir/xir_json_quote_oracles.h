/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_json_quote_oracles.h - Independent bytes for ordinary JSON quoting
 */
#ifndef XIR_JSON_QUOTE_ORACLES_H
#define XIR_JSON_QUOTE_ORACLES_H
static void json_quote_case(XrXirProgram *program,uint32_t entry,const char *input,
    size_t size,const char *expected,size_t length) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    XrXirValue argument={0},result={0};
    CHECK(xr_xir_string_new(domain,input,size,&argument)==XR_XIR_VALUE_OK);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,&argument,1)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&argument);xr_xir_domain_drop(domain);
    CHECK(xr_xir_instance_poll_bounded(instance,4096).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    const char *bytes=NULL;size_t count=0;
    CHECK(result.type==XR_XIR_STRING && !result.reserved &&
        xr_xir_string_view(&result,&bytes,&count) && count==length && !memcmp(bytes,expected,length));
    xr_xir_value_drop(&result);
}
static void json_quote_oracles(XrXirProgram *program,uint32_t entry) {
    for(unsigned c=0;c<128;++c) {
        char input=(char)c,expected[16];size_t length;
        if(c<32) {
            int n=snprintf(expected,sizeof(expected),"\"\\u%04x\"",c);CHECK(n==8);length=(size_t)n;
        } else if(c==34 || c==92) {
            expected[0]='"';expected[1]='\\';expected[2]=(char)c;expected[3]='"';length=4;
        } else {expected[0]='"';expected[1]=(char)c;expected[2]='"';length=3;}
        json_quote_case(program,entry,&input,1,expected,length);
    }
    const char *unicode[]={"","\xc2\x80","\xdf\xbf","\xe0\xa0\x80","\xed\x9f\xbf",
        "\xee\x80\x80","\xef\xbf\xbf","\xf0\x90\x80\x80","\xf4\x8f\xbf\xbf","é汉😀",
        "\xe2\x80\xa8\xe2\x80\xa9"};
    for(unsigned i=0;i<sizeof(unicode)/sizeof(*unicode);++i) {
        char expected[32];size_t size=strlen(unicode[i]);CHECK(size+2<=sizeof(expected));
        expected[0]='"';memcpy(expected+1,unicode[i],size);expected[size+1]='"';
        json_quote_case(program,entry,unicode[i],size,expected,size+2);
    }
    puts("Ordinary JSON quote: all128 ASCII bytes and11 empty/UTF8 boundary cases; input Domain dies before poll");
}
#endif // XIR_JSON_QUOTE_ORACLES_H
