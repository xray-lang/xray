/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_evaluate_data.inc.c - Exact observations across temporary Source owners
 */
#include "app/dap/xdap_expression.h"
#include "app/dap/xdap_value.h"
typedef struct EvaluateDataCase {
    const char *text;
    XrXirType type;
    int64_t payload;
    const char *bytes;
    size_t length;
    bool array;
    uint64_t children;
} EvaluateDataCase;
static EvaluateFixture evaluate_data_open(void) {
    EvaluateFixture fixture=evaluate_open(XR_DAP_EXPRESSION_FIXTURE);char message[1024];
    int length=snprintf(message,sizeof(message),"{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\","
        "\"arguments\":{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":13}]}}",XR_DAP_EXPRESSION_FIXTURE);
    CHECK(length>0 && (size_t)length<sizeof(message));request(fixture.adapter,message);
    request(fixture.adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(fixture.adapter,1000)==XR_XIR_OK && fixture.adapter->snapshot && !fixture.adapter->running);
    capture.count=0;return fixture;
}
static void evaluate_data_result(const EvaluateDataCase *test,XrXirValue *value,XrXirDomain *domain) {
    CHECK(xr_xir_value_valid(value));
    if (test->array) {
        XdapValueView view={value,domain,NULL,0};XdapValueShape shape={0};
        CHECK(xdap_value_shape(&view,&shape)==XR_XIR_VALUE_OK && shape.indexed && shape.children==test->children);
        if (shape.children) {
            XrXirValue child={0};XdapValueChildInfo info={0};const char *bytes=NULL;size_t length=0;
            CHECK(xdap_value_child(&view,0,&child,&info)==XR_XIR_VALUE_OK);
            CHECK(xr_xir_string_view(&child,&bytes,&length) && length==1 && bytes[0]=='b');xr_xir_value_drop(&child);
            CHECK(xdap_value_child(&view,1,&child,&info)==XR_XIR_VALUE_OK);
            CHECK(xr_xir_string_view(&child,&bytes,&length) && length==3 && !memcmp(bytes,"s\0x",3));xr_xir_value_drop(&child);
        }
    } else {
        CHECK(value->type==(uint32_t)test->type);
        if (test->bytes) {
            const char *bytes=NULL;size_t length=0;
            CHECK(xr_xir_string_view(value,&bytes,&length) && length==test->length && !memcmp(bytes,test->bytes,length));
        } else CHECK(value->payload==test->payload);
    }
}
static void evaluate_data(void) {
    const EvaluateDataCase cases[]={
        {"words[1]+\"!\"",XR_XIR_STRING,0,"b!",2,false,0},
        {"record.0+0.5",XR_XIR_F64,INT64_C(0x3ffc000000000000),NULL,0,false,0},
        {"record.1=='界'",XR_XIR_BOOL,1,NULL,0,false,0},
        {"record.1",XR_XIR_RUNE,INT64_C(0x754c),NULL,0,false,0},
        {"small+0.5",XR_XIR_F32,INT64_C(0x3fe00000),NULL,0,false,0},
        {"record.2+\"!\"",XR_XIR_STRING,0,"s\0x!",4,false,0},
        {"nested==null",XR_XIR_BOOL,0,NULL,0,false,0},
        {"nested! == null",XR_XIR_BOOL,1,NULL,0,false,0},
        {"filled ?? 99",XR_XIR_I64,7,NULL,0,false,0},
        {"(wide,record.2).0",XR_XIR_U64,-1,NULL,0,false,0},
        {"(negative,record.2).0",XR_XIR_F64,INT64_MIN,NULL,0,false,0},
        {"units.0",XR_XIR_UNIT,0,NULL,0,false,0},
        {"_dap_observed_0+words[1]",XR_XIR_STRING,0,"shadowb",7,false,0},
        {"[words[1],record.2]",XR_XIR_UNIT,0,NULL,0,true,2},
        {"record.2.contains(\"s\\0x\")",XR_XIR_BOOL,1,NULL,0,false,0},
        {"record.2.startsWith(\"s\")",XR_XIR_BOOL,1,NULL,0,false,0},
        {"record.2.endsWith(\"x\")",XR_XIR_BOOL,1,NULL,0,false,0},
        {"record.2.indexOf(\"x\")",XR_XIR_I64,2,NULL,0,false,0},
        {"record.2.lastIndexOf(\"\\0\")",XR_XIR_I64,1,NULL,0,false,0},
        {"words.contains(\"b\")",XR_XIR_BOOL,1,NULL,0,false,0},
        {"words.indexOf(\"z\")",XR_XIR_I64,-1,NULL,0,false,0},
        {"true ? empty : empty",XR_XIR_UNIT,0,NULL,0,true,0}
    };
    for (uint32_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        const EvaluateDataCase *test=&cases[i];EvaluateFixture fixture=evaluate_data_open();
        XdapAdapter *adapter=fixture.adapter;XdapLexicalView lexical={0};
        CHECK(xdap_lexical_open(adapter->launch,adapter->snapshot,0,&lexical)==XR_XIR_OK);
        XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;
        XdapExpressionRequest request={adapter->compiler,&lexical,test->text,strlen(test->text),16384};
        XrXirValue value={0};XrXirStatus status=xdap_expression_evaluate(&request,&value);
        if (status!=XR_XIR_OK) fprintf(stderr,"Data expression [%s] status=%d\n",test->text,(int)status);
        CHECK(status==XR_XIR_OK && adapter->snapshot==snapshot && adapter->stop_epoch==epoch && !adapter->running && !capture.count);
        evaluate_data_result(test,&value,fixture.domain);
        CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);fixture.adapter=NULL;
        evaluate_data_result(test,&value,fixture.domain);xr_xir_value_drop(&value);evaluate_finish(&fixture);
    }
}

static size_t evaluate_data_fault(const EvaluateDataCase *test,size_t fail) {
    EvaluateFixture fixture=evaluate_data_open();XdapAdapter *adapter=fixture.adapter;XdapLexicalView lexical={0};
    CHECK(xdap_lexical_open(adapter->launch,adapter->snapshot,0,&lexical)==XR_XIR_OK);
    XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(fixture.ledger,&before)==XR_COMPILE_RESOURCE_OK);
    size_t allocations=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;
    XdapExpressionRequest request={adapter->compiler,&lexical,test->text,strlen(test->text),16384};XrXirValue value={0};
    runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirStatus status=xdap_expression_evaluate(&request,&value);size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail==SIZE_MAX) {
        CHECK(status==XR_XIR_OK);evaluate_data_result(test,&value,fixture.domain);
    } else {
        if (status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"Data fault [%s] site=%zu/%zu status=%d\n",test->text,fail,sites,(int)status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && sites>fail && !value.type && !value.reserved && !value.payload);
    }
    xr_xir_value_drop(&value);
    CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch && !adapter->running && !adapter->value_count && !capture.count);
    CHECK(runtime_live==allocations && runtime_bytes==bytes);
    CHECK(xr_compile_resources_stats(fixture.ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==before.live_bytes && after.work>before.work && after.allocated_bytes>=before.allocated_bytes);
    evaluate_dispose(&fixture);return sites;
}
static void evaluate_data_failures(bool resources) {
    const EvaluateDataCase cases[]={
        {"words[1]+record.2",XR_XIR_STRING,0,"bs\0x",4,false,0},
        {"[words[1],record.2]",XR_XIR_UNIT,0,NULL,0,true,2},
        {"record.2.indexOf(\"\\0\")",XR_XIR_I64,1,NULL,0,false,0},
        {"nested! == null",XR_XIR_BOOL,1,NULL,0,false,0}
    };
    for (uint32_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        size_t sites=evaluate_data_fault(&cases[i],SIZE_MAX);CHECK(sites);
        if (resources) for (size_t site=0;site<sites;++site) evaluate_data_fault(&cases[i],site);
        else { evaluate_data_fault(&cases[i],0);if (sites>1) evaluate_data_fault(&cases[i],sites-1); }
        printf("Data expression [%s] sites=%zu injection=%s original owners unchanged physical=0/0 PASS\n",
            cases[i].text,sites,resources ? "ALL" : "FIRST_LAST_ONLY");
    }
}
