/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_evaluate_member_consumers.inc.c - Fields share readonly DAP expression admission
 */
static uint32_t evaluate_member_reference(XdapAdapter *adapter,const char *text) {
    char message[256];int length=snprintf(message,sizeof(message),
        "{\"seq\":41,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"%s\"}}",text);
    CHECK(length>0 && (size_t)length<sizeof(message));request(adapter,message);
    XrJsonValue *reply=last_response("evaluate",true);uint32_t reference=(uint32_t)xjson_get_int(xjson_get_object(reply,"body"),"variablesReference");
    CHECK(reference);xjson_free(reply);capture.count=0;return reference;
}
static uint32_t evaluate_member_root(XdapAdapter *adapter) {
    char message[256];int length=snprintf(message,sizeof(message),
        "{\"seq\":41,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%u}}",adapter->frames[0].scope);
    CHECK(length>0 && (size_t)length<sizeof(message));request(adapter,message);
    XrJsonValue *reply=last_response("variables",true),*values=xjson_get_array(xjson_get_object(reply,"body"),"variables");uint32_t root=0;
    for (int i=0;i<xjson_array_len(values);++i) {
        XrJsonValue *value=xjson_array_get(values,i);
        if (!strcmp(xjson_get_string(value,"name"),"box")) root=(uint32_t)xjson_get_int(value,"variablesReference");
    }
    CHECK(root);xjson_free(reply);capture.count=0;return root;
}
static void evaluate_member_set(XdapAdapter *adapter,uint32_t reference,const char *text,bool success) {
    char message[512];int length=snprintf(message,sizeof(message),
        "{\"seq\":42,\"type\":\"request\",\"command\":\"setVariable\",\"arguments\":{\"variablesReference\":%u,\"name\":\"number\",\"value\":\"%s\"}}",reference,text);
    CHECK(length>0 && (size_t)length<sizeof(message));request(adapter,message);
    XrJsonValue *reply=last_response("setVariable",success);
    if (success) CHECK(!strcmp(xjson_get_string(xjson_get_object(reply,"body"),"value"),"36"));
    xjson_free(reply);capture.count=0;
}
static void evaluate_member_predicates(void) {
    for (uint32_t i=0;i<3;++i) {
        EvaluateFixture fixture=evaluate_member_prepare();XdapAdapter *adapter=fixture.adapter;char message[1024];uint32_t line=0;
        for (uint32_t f=0;f<adapter->launch->function_count;++f) if (!strcmp(adapter->launch->functions[f].name,"probe")) {
            for (uint32_t at=0;at<adapter->launch->functions[f].instructions && !line;++at) {
                XrXirSourceDebugPoint point={0};
                if (xdap_launch_source_point(adapter->launch,f,at,&point)==XR_XIR_OK && (point.flags & XR_XIR_SOURCE_DEBUG_ENTRY)) line=(uint32_t)point.range.line;
            }
        }
        CHECK(line);
        const char *command=i ? "setFunctionBreakpoints" : "setBreakpoints";
        int length=i ? snprintf(message,sizeof(message),
            "{\"seq\":43,\"type\":\"request\",\"command\":\"setFunctionBreakpoints\",\"arguments\":{\"breakpoints\":[{\"name\":\"probe\",\"condition\":\"box.number==%u\"}]}}",i==1 ? 12u : 11u) :
            snprintf(message,sizeof(message),
            "{\"seq\":43,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":%u,\"condition\":\"box.number==11 && record.flag\",\"logMessage\":\"n={box.number+record.number}\"}]}}",XR_DAP_MEMBER_FIXTURE,line);
        CHECK(length>0 && (size_t)length<sizeof(message));request(adapter,message);
        XrJsonValue *reply=last_response(command,true);XrJsonValue *list=xjson_get(xjson_get_object(reply,"body"),"breakpoints");
        CHECK(list && list->as.array.count==1 && xjson_get_bool(list->as.array.items[0],"verified"));xjson_free(reply);capture.count=0;
        request(adapter,"{\"seq\":44,\"type\":\"request\",\"command\":\"configurationDone\"}");capture.count=0;
        for (uint32_t step=0;step<64 && adapter->running;++step) CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK);
        CHECK(!adapter->running);
        if (i==2) {
            CHECK(adapter->snapshot && !adapter->terminal && event_count("stopped")==1 && !event_count("output"));capture.count=0;
            evaluate_request(adapter,"box.number+record.number",adapter->frames[0].handle,"24");evaluate_member_outputs(adapter,"29\n");
        } else {
            CHECK(adapter->terminal && !event_count("stopped") && event_count("output")==7u+(i==0 ? 1u : 0u));uint32_t outputs=0;
            const char *expected[]={"29\n","36\n","46\n","true\n","3\n","false\n","99\n"};
            for (uint32_t j=0;j<capture.count;++j) {
                reply=xjson_parse(capture.messages[j].bytes,capture.messages[j].size);CHECK(reply);const char *event=xjson_get_string(reply,"event");
                if (event && !strcmp(event,"output")) {
                    XrJsonValue *output=xjson_get_object(reply,"body");const char *text=xjson_get_string(output,"output");
                    if (!i && !outputs) CHECK(!strcmp(text,"n=24\n") && !strcmp(xjson_get_string(output,"category"),"console"));
                    else { uint32_t at=outputs-(i==0);CHECK(at<7 && !strcmp(text,expected[at])); }
                    ++outputs;
                }
                xjson_free(reply);
            }
            CHECK(outputs==7u+(i==0 ? 1u : 0u));capture.count=0;
        }
        evaluate_dispose(&fixture);
    }
}
static void evaluate_member_boundaries(EvaluateFixture *fixture) {
    XdapAdapter *adapter=fixture->adapter;XdapLexicalView lexical={0};
    CHECK(xdap_lexical_open(adapter->launch,adapter->snapshot,0,&lexical)==XR_XIR_OK);
    XdapExpressionRequest input={adapter->compiler,&lexical,"box",3,16384,NULL};XrXirValue parent={0},child={0};
    CHECK(xdap_expression_evaluate(&input,&parent)==XR_XIR_OK);
    XdapValueView view={&parent,fixture->domain,adapter->snapshot,0};
    CHECK(xdap_value_member(&view,"number",6,&child)==XR_XIR_VALUE_OK && child.type==XR_XIR_I64 && child.payload==11);xr_xir_value_drop(&child);
    CHECK(xdap_value_member(&view,"secret",6,&child)==XR_XIR_VALUE_UNSUPPORTED && !child.type && !child.payload);
    CHECK(xdap_value_member(&view,"absent",6,&child)==XR_XIR_VALUE_UNSUPPORTED && !child.type && !child.payload);
    XrXirValue marker={XR_XIR_I64,0,71};
    CHECK(xdap_value_member(&view,"number",6,&marker)==XR_XIR_VALUE_BAD_ARGUMENT && marker.type==XR_XIR_I64 && marker.payload==71);
    view.frame=UINT32_MAX;CHECK(xdap_value_member(&view,"number",6,&child)==XR_XIR_VALUE_BAD_ARGUMENT && !child.type && !child.payload);
    view.frame=0;view.snapshot=NULL;CHECK(xdap_value_member(&view,"number",6,&child)==XR_XIR_VALUE_BAD_ARGUMENT && !child.type && !child.payload);
    xr_xir_value_drop(&parent);
}
static void evaluate_member_escape(void) {
    EvaluateFixture fixture=evaluate_member_open();XdapAdapter *adapter=fixture.adapter;XdapLexicalView lexical={0};
    CHECK(xdap_lexical_open(adapter->launch,adapter->snapshot,0,&lexical)==XR_XIR_OK);
    XdapExpressionRequest input={adapter->compiler,&lexical,"box.leaf",8,16384,NULL};XrXirValue value={0};
    CHECK(xdap_expression_evaluate(&input,&value)==XR_XIR_OK && xr_xir_value_valid(&value));
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);fixture.adapter=NULL;
    CHECK(xr_xir_value_valid(&value));xr_xir_value_drop(&value);evaluate_finish(&fixture);
}
static void evaluate_member_consumers(void) {
    EvaluateFixture fixture=evaluate_member_open();XdapAdapter *adapter=fixture.adapter;
    uint32_t frame=adapter->frames[0].handle,id=0;XrXirValue value={0};evaluate_member_boundaries(&fixture);
    const char *watch="box.leaf.number+record.number";
    CHECK(xdap_watch_add(adapter,watch,strlen(watch),&id)==XR_XIR_OK && id);
    CHECK(xdap_watch_evaluate(adapter,id,frame,&value)==XR_XIR_OK && value.type==XR_XIR_I64 && value.payload==36);xr_xir_value_drop(&value);
    
    uint32_t projected=evaluate_member_reference(adapter,"box.leaf");
    evaluate_member_set(adapter,projected,"99",false);evaluate_request(adapter,"box.leaf.number",frame,"23");
    uint32_t pure=evaluate_member_reference(adapter,"box");evaluate_member_set(adapter,pure,"36",false);
    uint32_t root=evaluate_member_root(adapter);CHECK(root!=pure);
    XdapLexicalView lexical={0};CHECK(xdap_lexical_open(adapter->launch,adapter->snapshot,0,&lexical)==XR_XIR_OK);
    XdapExpressionRequest input={adapter->compiler,&lexical,"box.number",10,16384,NULL};
    CHECK(xdap_expression_evaluate(&input,&value)==XR_XIR_OK && value.payload==11);
    uint64_t epoch=adapter->stop_epoch;evaluate_member_set(adapter,root,"record.number+box.leaf.number",true);
    CHECK(adapter->stop_epoch==epoch+1 && value.type==XR_XIR_I64 && value.payload==11);xr_xir_value_drop(&value);
    evaluate_request(adapter,"box.number",frame,"36");evaluate_request(adapter,"box.leaf.number",frame,"23");
    evaluate_member_outputs(adapter,"54\n");
    CHECK(xdap_watch_evaluate(adapter,id,frame,&value)==XR_XIR_BAD_STRUCTURE && !value.type && !value.payload);
    evaluate_dispose(&fixture);
    puts("Field consumers: actual watch, condition/log, contextual setVariable RHS36/final54, derived Class reference stays readonly PASS");
}
