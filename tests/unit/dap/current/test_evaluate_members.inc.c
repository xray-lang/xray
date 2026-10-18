/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_evaluate_members.inc.c - Source arithmetic over authorized nominal fields
 */
static EvaluateFixture evaluate_member_prepare(void) {
    EvaluateFixture fixture=evaluate_open(XR_DAP_CALL_FIXTURE);XdapAdapter *adapter=fixture.adapter;
    CHECK(xdap_launch_free(adapter->launch)==XR_XIR_CALL_READY);adapter->launch=NULL;
    XdapLaunchRequest launch={.compiler=adapter->compiler,.absolute_file=XR_DAP_MEMBER_FIXTURE,.observer_metadata=fixture.domain,.instance=adapter->config.instance};
    XrSourceCompileDiagnostic diagnostic={0};CHECK(xdap_launch_build(&launch,&adapter->launch,&diagnostic)==XR_XIR_OK && adapter->launch);
    xr_source_compile_diagnostic_free(&diagnostic);return fixture;
}
static EvaluateFixture evaluate_member_open(void) {
    EvaluateFixture fixture=evaluate_member_prepare();XdapAdapter *adapter=fixture.adapter;
    request(adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"setFunctionBreakpoints\",\"arguments\":{\"breakpoints\":[{\"name\":\"probe\"}]}}");
    XrJsonValue *reply=last_response("setFunctionBreakpoints",true);xjson_free(reply);
    request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && !adapter->running);capture.count=0;return fixture;
}
static const EvaluateDataCase evaluate_member_cases[]={
    {"box.number+1",XR_XIR_I64,12,NULL,0,false,0},
    {"record.number+box.number",XR_XIR_I64,24,NULL,0,false,0},
    {"box.leaf.number+record.number",XR_XIR_I64,36,NULL,0,false,0},
    {"(box.leaf).number*2",XR_XIR_I64,46,NULL,0,false,0},
    {"((box).leaf).number+1",XR_XIR_I64,24,NULL,0,false,0},
    {"box.label+\"!\"",XR_XIR_STRING,0,"s\0x!",4,false,0},
    {"len(box.label)",XR_XIR_I64,3,NULL,0,false,0},
    {"box.label.contains(\"\\0\")",XR_XIR_BOOL,1,NULL,0,false,0},
    {"(box.label).endsWith(\"x\")",XR_XIR_BOOL,1,NULL,0,false,0},
    {"box.numbers[1]+1",XR_XIR_I64,13,NULL,0,false,0},
    {"box.maybe ?? 99",XR_XIR_I64,99,NULL,0,false,0},
    {"record.flag ? box.number : record.number",XR_XIR_I64,11,NULL,0,false,0},
    {"false && box.numbers[99]==0",XR_XIR_BOOL,0,NULL,0,false,0},
    {"true || box.numbers[99]==0",XR_XIR_BOOL,1,NULL,0,false,0},
    {"true ? box.number : box.numbers[99]",XR_XIR_I64,11,NULL,0,false,0},
    {"_dap_member_0+box.number",XR_XIR_I64,16,NULL,0,false,0},
    {"box.number+box.number",XR_XIR_I64,22,NULL,0,false,0},
    {"( /* 界 */ (box).leaf\n).number+1",XR_XIR_I64,24,NULL,0,false,0},
    {"box.number /* .leaf */ + (record).number",XR_XIR_I64,24,NULL,0,false,0},
    {"box.number",XR_XIR_I64,11,NULL,0,false,0},
    {"box.label",XR_XIR_STRING,0,"s\0x",3,false,0},
    {"\"界\"+box.label",XR_XIR_STRING,0,"界s\0x",6,false,0},
    {"\"box.number\"+box.label",XR_XIR_STRING,0,"box.numbers\0x",13,false,0},
    {"box.number+_dap_member_0",XR_XIR_I64,16,NULL,0,false,0},
    {"box.number+record.number+_dap_member_0",XR_XIR_I64,29,NULL,0,false,0}
};
static void evaluate_member_outputs(XdapAdapter *adapter,const char *first) {
    request(adapter,"{\"seq\":40,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");capture.count=0;
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal);
    const char *expected[]={first,"36\n","46\n","true\n","3\n","false\n","99\n"};uint32_t outputs=0;
    for (uint32_t i=0;i<capture.count;++i) {
        XrJsonValue *json=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);CHECK(json);
        const char *event=xjson_get_string(json,"event");
        if (event && !strcmp(event,"output")) {
            CHECK(outputs<sizeof(expected)/sizeof(*expected));
            CHECK(!strcmp(xjson_get_string(xjson_get_object(json,"body"),"output"),expected[outputs++]));
        }
        xjson_free(json);
    }
    CHECK(outputs==sizeof(expected)/sizeof(*expected));capture.count=0;
}
static void evaluate_members(void) {
    for (uint32_t i=0;i<sizeof(evaluate_member_cases)/sizeof(*evaluate_member_cases);++i) {
        const EvaluateDataCase *test=&evaluate_member_cases[i];EvaluateFixture fixture=evaluate_member_open();
        XdapAdapter *adapter=fixture.adapter;XdapLexicalView lexical={0};CHECK(adapter->depth>=2);
        CHECK(xdap_lexical_open(adapter->launch,adapter->snapshot,0,&lexical)==XR_XIR_OK);
        XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;
        XdapExpressionRequest input={adapter->compiler,&lexical,test->text,strlen(test->text),16384,NULL};XrXirValue value={0};
        XrXirStatus status=xdap_expression_evaluate(&input,&value);
        if (status!=XR_XIR_OK) fprintf(stderr,"Member expression [%s] status=%u\n",test->text,status);
        CHECK(status==XR_XIR_OK);evaluate_data_result(test,&value,fixture.domain);
        CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch && !adapter->running && !capture.count);
        if (!i) evaluate_member_outputs(adapter,"29\n");
        CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);fixture.adapter=NULL;
        evaluate_data_result(test,&value,fixture.domain);xr_xir_value_drop(&value);evaluate_finish(&fixture);
    }
    EvaluateFixture fixture=evaluate_member_open();XdapAdapter *adapter=fixture.adapter;
    uint32_t frame=adapter->frames[0].handle;
    CHECK(adapter->depth>=3);evaluate_request(adapter,"box.secret+box.seal",adapter->frames[1].handle,"36");
    evaluate_request(adapter,"box.number+1",frame,"12");
    evaluate_context(adapter,"box.leaf.number+record.number",frame,"36","\"hover\"");
    evaluate_context(adapter,"box.number+box.number",frame,"22","\"watch\"");
    const char *denied[]={"box.secret+1","box.seal+1","box.absent+1","box.leaf.absent+1","box.contains(\"x\")",
        "box.number=99","box==box","box.numbers[99]","box.maybe!+1","absent.number+1"};
    for (uint32_t i=0;i<sizeof(denied)/sizeof(*denied);++i) {
        XdapLexicalView lexical={0};CHECK(xdap_lexical_open(adapter->launch,adapter->snapshot,0,&lexical)==XR_XIR_OK);
        XdapExpressionRequest input={adapter->compiler,&lexical,denied[i],strlen(denied[i]),16384,NULL};XrXirValue value={0};
        CHECK(xdap_expression_evaluate(&input,&value)!=XR_XIR_OK && !value.type && !value.reserved && !value.payload);
    }
    evaluate_request(adapter,"box.number",frame,"11");evaluate_member_outputs(adapter,"29\n");
    evaluate_request(adapter,"box.number+1",frame,NULL);evaluate_dispose(&fixture);
    puts("Authorized nominal fields: 25 Source results/7 ordinary target oracles, hidden/missing/method/identity rejects, hover/watch, ownership and short-circuit PASS");
}
