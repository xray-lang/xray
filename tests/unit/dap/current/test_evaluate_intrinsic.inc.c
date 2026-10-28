/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_evaluate_intrinsic.inc.c - Core calls never replace original Source bindings
 */
static EvaluateFixture evaluate_intrinsic_open(const char *text) {
    EvaluateFixture fixture=evaluate_open(XR_DAP_CALL_FIXTURE);XdapAdapter *adapter=fixture.adapter;
    CHECK(xdap_launch_free(adapter->launch)==XR_XIR_CALL_READY);adapter->launch=NULL;
    XrSourceCompilerText input={NULL,"dap-intrinsic-scope",text,strlen(text)};
    XdapLaunchRequest launch_request={.compiler=adapter->compiler,.text=&input,.observer_metadata=fixture.domain,.instance=adapter->config.instance};
    XrSourceCompileDiagnostic diagnostic={0};XrFrontendRoot diagnostic_root={.message=""};XrXirStatus status=xdap_launch_build(&launch_request,&adapter->launch,&diagnostic);
    if(diagnostic.source.failure.report) {
        XrFrontendReportInfo info={0};CHECK(xr_frontend_report_info(diagnostic.source.failure.report,&info)&&info.roots==1);
        CHECK(xr_frontend_report_root(diagnostic.source.failure.report,0,&diagnostic_root));
    }
    if (status!=XR_XIR_OK) fprintf(stderr,"intrinsic Source=%u stage=%u: %s\n",status,diagnostic.source.stage,diagnostic_root.message);
    CHECK(status==XR_XIR_OK && adapter->launch);xr_source_compile_diagnostic_free(&diagnostic);
    request(adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"setFunctionBreakpoints\",\"arguments\":{\"breakpoints\":[{\"name\":\"probe\"}]}}");
    XrJsonValue *reply=last_response("setFunctionBreakpoints",true);xjson_free(reply);
    request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && !adapter->running);
    capture.count=0;return fixture;
}
static void evaluate_intrinsic_scopes(void) {
    const struct { const char *text,*length,*plain,*output; } cases[]={
        {"fn len(value:Array<i64>)->i64 {print(\"BAD\");return 99}\nfn probe()->i64 {return 7}\nprint(probe())\n",
         NULL,NULL,"7\n"},
        {"fn probe(len:i64)->i64 {return len}\nprint(probe(21))\n",
         NULL,"21","21\n"},
        {"const len:i64=9\nfn probe()->i64 {return 7}\nprint(probe())\n",
         NULL,"9","7\n"},
        {"fn other()->i64 {const len:i64=9;return len}\nfn probe()->i64 {return 7}\nprint(probe())\n",
         "2",NULL,"7\n"},
        {"fn probe()->i64 {const value:i64=7;if (value==99) {const len:i64=9};return value}\nprint(probe())\n",
         "2",NULL,"7\n"},
        {"fn probe()->i64 {const value:i64=7;if (value==99) {fn len(value:Array<i64>)->i64 {print(\"BAD\");return 99}};return value}\nprint(probe())\n",
         "2",NULL,"7\n"}
    };
    for (uint32_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        EvaluateFixture fixture=evaluate_intrinsic_open(cases[i].text);XdapAdapter *adapter=fixture.adapter;
        uint32_t frame=adapter->frames[0].handle;uint64_t epoch=adapter->stop_epoch;XrXirVmDebugSnapshot *snapshot=adapter->snapshot;
        evaluate_request(adapter,"len([1,2])",frame,cases[i].length);
        if (cases[i].plain) evaluate_request(adapter,"len",frame,cases[i].plain);
        CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch && !adapter->running && !capture.count);
        request(adapter,"{\"seq\":6,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");capture.count=0;
        CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal && event_count("output")==1);
        bool output=false;
        for (uint32_t j=0;j<capture.count;++j) {
            XrJsonValue *json=xjson_parse(capture.messages[j].bytes,capture.messages[j].size);CHECK(json);
            const char *event=xjson_get_string(json,"event");
            if (event && !strcmp(event,"output")) {
                CHECK(!strcmp(xjson_get_string(xjson_get_object(json,"body"),"output"),cases[i].output));output=true;
            }
            xjson_free(json);
        }
        CHECK(output);evaluate_dispose(&fixture);
    }
    puts("Core length call respects original module function, local parameter, global scalar and unrelated nested binding; no user call/output PASS");
}
