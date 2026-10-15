/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_class_write.c - Genuine Class identity writes preserve owned value copies
 */
#define XR_DAP_CHILD_ENTRY reference_class_child_main
#include "test_child_write.c"
#undef XR_DAP_CHILD_ENTRY
#include "xir/xxir_class.h"
#include "xir/xxir_struct.h"
#include "xir/xxir_array.h"
#include "xir/xxir_cell_owner_internal.h"
#include "xir/xxir_value_observe_internal.h"
static uint32_t class_binding_at(XdapAdapter *adapter,uint32_t frame,const char *name,XrXirValue *output) {
    XdapLexicalView lexical={0};CHECK(xdap_lexical_open(adapter->launch,adapter->snapshot,frame,&lexical)==XR_XIR_OK);
    for (uint32_t b=lexical.environment;b;b=lexical.function->bindings[b-1].parent) {
        const XrXirSourceDebugBinding *binding=&lexical.function->bindings[b-1];
        if (strcmp(lexical.source->declarations[binding->declaration-1].name,name)) continue;
        const char *text=NULL;size_t size=0;CHECK(xdap_lexical_read(&lexical,b,output,&text,&size)==XR_XIR_VALUE_OK);return binding->value;
    }
    CHECK(false);return UINT32_MAX;
}
static uint32_t class_binding(XdapAdapter *adapter,const char *name,XrXirValue *output) { return class_binding_at(adapter,0,name,output); }
static XrXirValue class_read(XrXirValue *root,uint32_t index,uint32_t kind,XrXirDomain *domain) {
    XrXirValue value={0};XrXirValueAdmission admission={.arena=xr_xir_value_arena(root),.domain=domain,.work=65536,.scratch_bytes=65536};XrXirFaultDetail fault={0};
    XrXirValuePathStep step={kind==0 ? XR_XIR_PATH_INDEX : XR_XIR_PATH_FIELD,(XrXirType)root->type,index};
    XrXirValueStatus status=kind==2 ? xr_xir_class_get(root,index,&admission,&value) : xir_value_observe_child(root,&step,&admission,&value,&fault);
    if (status!=XR_XIR_VALUE_OK) fprintf(stderr,"Class read failed: kind=%u type=%u index=%u status=%u\n",kind,root->type,index,(unsigned)status);
    CHECK(status==XR_XIR_VALUE_OK);return value;
}
static void class_number(XrXirValue *box,XrXirDomain *domain,int64_t expected) {
    XrXirValue array=class_read(box,0,2,domain),piece=class_read(&array,0,0,domain),number=class_read(&piece,0,1,domain);
    CHECK(number.type==XR_XIR_I64 && number.payload==expected);xr_xir_value_drop(&number);xr_xir_value_drop(&piece);xr_xir_value_drop(&array);
}
static WriteFixture class_open_at(const char *file,uint32_t line) {
    WriteFixture fixture=write_open(file);char message[2048];
    snprintf(message,sizeof(message),"{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":%u}]}}",file,line);
    request(fixture.adapter,message);request(fixture.adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(fixture.adapter,1000)==XR_XIR_OK && fixture.adapter->snapshot && !fixture.adapter->running);capture.count=0;return fixture;
}
static size_t class_case(uint32_t kind,size_t fail,uint32_t invalid) {
    WriteFixture fixture=class_open_at(XR_DAP_CLASS_WRITE_FIXTURE,21);XdapAdapter *adapter=fixture.adapter;XrXirValue root={0},box={0};
    bool readonly=invalid==1;
    const char *names[]={"box","roots","cells","envelopes"};uint32_t operand=class_binding(adapter,names[kind],&root);
    class_binding(adapter,"box",&box);XrXirValuePathStep steps[5];uint32_t count=0;
    if (kind) {
        XrXirValue holder=class_read(&root,0,0,fixture.domain);
        steps[count++]=(XrXirValuePathStep){XR_XIR_PATH_INDEX,(XrXirType)root.type,0};
        steps[count++]=(XrXirValuePathStep){XR_XIR_PATH_FIELD,(XrXirType)holder.type,0};xr_xir_value_drop(&holder);
    }
    XrXirValue array=class_read(&box,0,2,fixture.domain),piece=class_read(&array,0,0,fixture.domain);
    steps[count++]=(XrXirValuePathStep){XR_XIR_PATH_FIELD,(XrXirType)box.type,readonly ? 1 : 0};
    if (!readonly) {
        steps[count++]=(XrXirValuePathStep){XR_XIR_PATH_INDEX,(XrXirType)array.type,0};
        steps[count++]=(XrXirValuePathStep){XR_XIR_PATH_FIELD,(XrXirType)piece.type,0};
    }
    xr_xir_value_drop(&piece);xr_xir_value_drop(&array);
    XrXirValuePath path={steps,count};XrXirValue replacement={XR_XIR_I64,0,99};
    if (invalid==2) steps[count-1].selector=1;
    if (invalid==3) steps[count-2].selector=-1;
    if (invalid==4) replacement=(XrXirValue){XR_XIR_BOOL,0,1};
    if (invalid==5) steps[count-3].container=XR_XIR_I64;
    uint64_t epoch=adapter->stop_epoch;XrXirVmDebugFrameInfo frame=write_frame(adapter->snapshot);
    if (!kind) CHECK(xdap_current_write_begin(adapter->launch->control,epoch,frame.id,operand,&write_whole_path,&root)==XR_XIR_VALUE_BAD_ARGUMENT);
    size_t blocks=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirValueStatus status=xdap_current_write_begin(adapter->launch->control,epoch,frame.id,operand,&path,&replacement);
    memset(steps,0xcc,sizeof(steps));XdapCurrentResult result={0};XrXirVmDebugSnapshot *fresh=NULL;bool visited=false;
    if (status==XR_XIR_VALUE_OK) {
        CHECK(xdap_current_poll(adapter->launch->control,1,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_STOPPED);
        status=xdap_current_write_finish(adapter->launch->control,&visited,&fresh);CHECK(visited);
    }
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;bool changed=fail==SIZE_MAX && !invalid;
    if (changed) {
        CHECK(status==XR_XIR_VALUE_OK && fresh && result.debug_epoch==epoch+1);class_number(&box,fixture.domain,99);
        XrXirVmDebugFrameInfo same=write_frame(fresh);CHECK(same.id==frame.id && same.instruction==frame.instruction && same.thread==frame.thread);
    }
    else {
        CHECK(!fresh && status==(fail!=SIZE_MAX ? XR_XIR_VALUE_OOM : invalid==3 ? XR_XIR_VALUE_BOUNDS : XR_XIR_VALUE_BAD_ARGUMENT));
        CHECK(blocks==runtime_live && bytes==runtime_bytes);class_number(&box,fixture.domain,11);
    }
    CHECK(!capture.count);CHECK(xdap_current_continue(adapter->launch->control,changed ? epoch+1 : epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
    xdap_current_result_drop(&result);CHECK(xdap_current_poll(adapter->launch->control,1000,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_ENDED);
    const char *number=changed ? "99\n" : "11\n";const char *expected[]={number,number,"11\n","37\n","13\n",number,number};uint32_t found=0;
    for (uint32_t i=0;i<capture.count;++i) {
        XrJsonValue *reply=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);const char *event=xjson_get_string(reply,"event");
        if (event && !strcmp(event,"output")) { CHECK(found<7 && !strcmp(xjson_get_string(xjson_get_object(reply,"body"),"output"),expected[found++])); }xjson_free(reply);
    }
    CHECK(found==7);xdap_current_result_drop(&result);xr_xir_vm_debug_snapshot_drop(fresh);
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);class_number(&box,fixture.domain,changed ? 99 : 11);
    xr_xir_value_drop(&box);xr_xir_value_drop(&root);write_finish(&fixture);return sites;
}
static size_t class_child_case(bool module,size_t fail) {
    WriteFixture fixture=class_open_at(module ? XR_DAP_CLASS_MODULE_FIXTURE : XR_DAP_CLASS_CHILD_FIXTURE,module ? 13 : 9);
    XdapAdapter *adapter=fixture.adapter;XrXirValue box={0};
    if (module) {
        XrXirValue outside={0};class_binding(adapter,"outside",&outside);CHECK(xr_xir_class_module_storage(&outside));
        char message[2048];snprintf(message,sizeof(message),"{\"seq\":5,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":9}]}}",XR_DAP_CLASS_MODULE_FIXTURE);request(adapter,message);
        request(adapter,"{\"seq\":6,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");capture.count=0;
        CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && !adapter->running);
        uint32_t target=class_binding(adapter,"box",&box);XrXirVmDebugFrameInfo active=write_frame(adapter->snapshot);
        CHECK(active.thread!=1 && !xr_xir_class_module_storage(&box));xr_xir_value_drop(&box);
        CHECK(xdap_current_write_begin(adapter->launch->control,adapter->stop_epoch,active.id,target,&write_whole_path,&outside)==XR_XIR_VALUE_OK);
        XdapCurrentResult injected={0};CHECK(xdap_current_poll(adapter->launch->control,1,&injected)==XR_XIR_VALUE_OK && injected.state==XDAP_CURRENT_STOPPED);
        XrXirVmDebugSnapshot *fresh=NULL;bool visited=false;CHECK(xdap_current_write_finish(adapter->launch->control,&visited,&fresh)==XR_XIR_VALUE_OK && visited && fresh);
        xr_xir_vm_debug_snapshot_drop(adapter->snapshot);adapter->snapshot=fresh;adapter->stop_epoch=injected.debug_epoch;
        xdap_current_result_drop(&injected);xr_xir_value_drop(&outside);capture.count=0;
    }
    uint32_t operand=class_binding(adapter,"box",&box);CHECK(xr_xir_class_module_storage(&box)==module);
    XrXirVmDebugFrameInfo frame=write_frame(adapter->snapshot);CHECK(frame.thread!=1);uint64_t epoch=adapter->stop_epoch;
    XrXirValuePathStep step={XR_XIR_PATH_FIELD,(XrXirType)box.type,0};XrXirValuePath path={&step,1};XrXirValue replacement={XR_XIR_I64,0,99};
    size_t blocks=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirValueStatus status=xdap_current_write_begin(adapter->launch->control,epoch,frame.id,operand,&path,&replacement);
    XdapCurrentResult result={0};XrXirVmDebugSnapshot *fresh=NULL;bool visited=false;
    if (status==XR_XIR_VALUE_OK) {
        CHECK(xdap_current_poll(adapter->launch->control,1,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_STOPPED);
        status=xdap_current_write_finish(adapter->launch->control,&visited,&fresh);CHECK(visited);
    }
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;bool changed=!module && fail==SIZE_MAX;
    if (changed) {
        CHECK(status==XR_XIR_VALUE_OK && fresh && result.debug_epoch==epoch+1);
        XrXirVmDebugFrameInfo same=write_frame(fresh);CHECK(same.id==frame.id && same.instruction==frame.instruction && same.thread==frame.thread);
    }
    else { CHECK(!fresh && status==(module ? XR_XIR_VALUE_BAD_ARGUMENT : XR_XIR_VALUE_OOM));CHECK(blocks==runtime_live && bytes==runtime_bytes); }
    XrXirValue observed=class_read(&box,0,2,fixture.domain);CHECK(observed.payload==(changed ? 99 : 11));xr_xir_value_drop(&observed);
    CHECK(xdap_current_continue(adapter->launch->control,changed ? epoch+1 : epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
    xdap_current_result_drop(&result);CHECK(xdap_current_poll(adapter->launch->control,1000,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_ENDED);
    uint32_t found=0;
    for (uint32_t i=0;i<capture.count;++i) {
        XrJsonValue *reply=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);const char *event=xjson_get_string(reply,"event");
        if (event && !strcmp(event,"output")) { CHECK(!found++ && !strcmp(xjson_get_string(xjson_get_object(reply,"body"),"output"),module ? "11\n" : changed ? "209\n" : "33\n")); }xjson_free(reply);
    }
    CHECK(found==1);xdap_current_result_drop(&result);xr_xir_vm_debug_snapshot_drop(fresh);xr_xir_value_drop(&box);write_dispose(&fixture);return sites;
}
static size_t class_rpc(size_t fail,bool io) {
    WriteFixture fixture=class_open_at(XR_DAP_CLASS_WRITE_FIXTURE,21);XdapAdapter *adapter=fixture.adapter;uint32_t scope=adapter->frames[0].scope;
    uint32_t root=origin_child(adapter,scope,"envelopes"),holder=origin_child(adapter,root,"[0]"),box=origin_child(adapter,holder,"box");
    XrXirValue identity={0},incoming={0};class_binding(adapter,"box",&identity);class_binding(adapter,"incoming",&incoming);
    uint64_t epoch=adapter->stop_epoch;XrXirVmDebugFrameInfo frame=write_frame(adapter->snapshot);XrXirVmDebugSnapshot *old=adapter->snapshot;
    child_set(adapter,scope,"box","box",false);child_set(adapter,root,"[0]","holder",false);
    child_set(adapter,holder,"box","box",false);child_set(adapter,box,"sibling","99",false);
    CHECK(adapter->snapshot==old && adapter->stop_epoch==epoch);
    request(adapter,"{\"seq\":6,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"box\"}}");
    XrJsonValue *reply=last_response("evaluate",true);uint32_t pure=(uint32_t)xjson_get_int(xjson_get_object(reply,"body"),"variablesReference");
    xjson_free(reply);capture.count=0;CHECK(pure && pure!=box && !origin_ref(adapter,pure)->origin.epoch);child_set(adapter,pure,"values","incoming",false);
    char message[512];size_t length=child_message(message,sizeof(message),box,"values","incoming");uint32_t refs=adapter->value_count,next=adapter->next_handle;
    size_t blocks=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    if (io) adapter->config.write=child_failed_output;
    XrXirStatus status=xdap_adapter_request(adapter,message,length);size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    bool changed=fail==SIZE_MAX;
    if (changed) {
        CHECK(status==(io ? XR_XIR_IO : XR_XIR_OK) && adapter->stop_epoch==epoch+1 && adapter->value_count==1);
        CHECK(adapter->values[0].origin.path.count==3 && adapter->values[0].origin.epoch==epoch+1);class_number(&identity,fixture.domain,23);
        uint32_t array=adapter->values[0].handle;if (!io) { reply=last_response("setVariable",true);xjson_free(reply);capture.count=0; }
        else { CHECK(!capture.count);adapter->config.write=write_write; }
        uint32_t piece=origin_child(adapter,array,"[0]");CHECK(!child_set(adapter,piece,"count","99",true));class_number(&identity,fixture.domain,99);
        XrXirVmDebugFrameInfo same=write_frame(adapter->snapshot);CHECK(same.id==frame.id && same.instruction==frame.instruction && same.thread==frame.thread);
    } else {
        CHECK(status==XR_XIR_OUT_OF_MEMORY && sites>fail && adapter->snapshot==old && adapter->stop_epoch==epoch);
        CHECK(runtime_live==blocks && runtime_bytes==bytes && adapter->value_count==refs && adapter->next_handle==next);class_number(&identity,fixture.domain,11);
    }
    XrXirValue piece=class_read(&incoming,0,0,fixture.domain),number=class_read(&piece,0,1,fixture.domain);CHECK(number.payload==23);
    xr_xir_value_drop(&number);xr_xir_value_drop(&piece);request(adapter,"{\"seq\":8,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");capture.count=0;
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal);
    const char *text=changed ? "99\n" : "11\n";const char *expected[]={text,text,"11\n","37\n","13\n",text,text};uint32_t found=0;
    for (uint32_t i=0;i<capture.count;++i) {
        reply=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);const char *event=xjson_get_string(reply,"event");
        if (event && !strcmp(event,"output")) { CHECK(found<7 && !strcmp(xjson_get_string(xjson_get_object(reply,"body"),"output"),expected[found++])); }xjson_free(reply);
    }
    CHECK(found==7);CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);class_number(&identity,fixture.domain,changed ? 99 : 11);
    xr_xir_value_drop(&identity);xr_xir_value_drop(&incoming);write_finish(&fixture);return sites;
}
static void class_projected_loan(void) {
    WriteFixture fixture=class_open_at(XR_DAP_CLASS_LOAN_FIXTURE,10);XdapAdapter *adapter=fixture.adapter;
    XrXirValue box={0},incoming={0};uint32_t operand=class_binding_at(adapter,1,"box",&box);class_binding(adapter,"incoming",&incoming);
    XrXirVmDebugFrameInfo frame={0};XrXirValue name={0};CHECK(xr_xir_vm_debug_snapshot_frame(adapter->snapshot,1,&frame,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);
    XrXirValuePathStep step={XR_XIR_PATH_FIELD,(XrXirType)box.type,0};XrXirValuePath path={&step,1};uint64_t epoch=adapter->stop_epoch;
    size_t blocks=runtime_live,bytes=runtime_bytes;CHECK(xdap_current_write_begin(adapter->launch->control,epoch,frame.id,operand,&path,&incoming)==XR_XIR_VALUE_OK);
    XdapCurrentResult result={0};CHECK(xdap_current_poll(adapter->launch->control,1,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_STOPPED && result.debug_epoch==epoch);
    XrXirVmDebugSnapshot *fresh=NULL;bool visited=false;CHECK(xdap_current_write_finish(adapter->launch->control,&visited,&fresh)==XR_XIR_VALUE_BAD_ARGUMENT && visited && !fresh);
    CHECK(blocks==runtime_live && bytes==runtime_bytes);xdap_current_result_drop(&result);
    CHECK(xdap_current_continue(adapter->launch->control,epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
    CHECK(xdap_current_poll(adapter->launch->control,1000,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_ENDED);
    const char *expected[]={"11\n","11\n","13\n","24\n","11\n","37\n","11\n","11\n"};uint32_t found=0;
    for (uint32_t i=0;i<capture.count;++i) {
        XrJsonValue *reply=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);const char *event=xjson_get_string(reply,"event");
        if (event && !strcmp(event,"output")) { CHECK(found<8 && !strcmp(xjson_get_string(xjson_get_object(reply,"body"),"output"),expected[found++])); }xjson_free(reply);
    }
    CHECK(found==8);xdap_current_result_drop(&result);xr_xir_value_drop(&box);xr_xir_value_drop(&incoming);write_dispose(&fixture);
}
static size_t class_observer_fault(size_t fail) {
    WriteFixture fixture=class_open_at(XR_DAP_CLASS_WRITE_FIXTURE,21);XdapAdapter *adapter=fixture.adapter;
    uint32_t parent=origin_child(adapter,adapter->frames[0].scope,"envelopes");char message[256];size_t length=origin_message(message,sizeof(message),parent);
    XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;uint32_t refs=adapter->value_count,next=adapter->next_handle;
    XdapValueRef existing[128];memcpy(existing,adapter->values,(size_t)refs*sizeof(*existing));
    XrCompileResourceStats before={0};CHECK(xr_compile_resources_stats(fixture.ledger,&before)==XR_COMPILE_RESOURCE_OK);
    size_t blocks=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirStatus status=xdap_adapter_request(adapter,message,length);size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail==SIZE_MAX) {
        CHECK(status==XR_XIR_OK && adapter->value_count==refs+1);
        CHECK(adapter->values[refs].origin.path.count==1);XrJsonValue *reply=last_response("variables",true);xjson_free(reply);
    } else {
        CHECK(status==XR_XIR_OUT_OF_MEMORY && sites>fail && adapter->value_count==refs);
        CHECK(adapter->next_handle>=next && adapter->next_handle-next<=1 && !capture.count);
        CHECK(!memcmp(existing,adapter->values,(size_t)refs*sizeof(*existing)));
        CHECK(runtime_live==blocks && runtime_bytes==bytes);XrCompileResourceStats after={0};
        CHECK(xr_compile_resources_stats(fixture.ledger,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==before.live_bytes && after.work>=before.work);
        uint32_t spent=adapter->next_handle;
        if (spent>next) {
            char retired[256];origin_message(retired,sizeof(retired),next);request(adapter,retired);
            XrJsonValue *reply=last_response("variables",false);xjson_free(reply);capture.count=0;
        }
        request(adapter,message);CHECK(adapter->value_count==refs+1 && adapter->values[refs].handle>=spent);
        XrJsonValue *reply=last_response("variables",true);xjson_free(reply);
    }
    CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch);write_dispose(&fixture);return sites;
}
static void class_projection_aliases(void) {
    WriteFixture fixture=class_open_at(XR_DAP_CLASS_ALIAS_FIXTURE,13);XdapAdapter *adapter=fixture.adapter;XrXirValue box={0};class_binding(adapter,"box",&box);
    uint32_t slots[64],count=0;XrXirVmDebugFrameInfo frame=write_frame(adapter->snapshot);
    for (uint32_t v=0;v<frame.value_count;++v) {
        XrXirValue value={0};if (xr_xir_vm_debug_snapshot_value(adapter->snapshot,0,v,&value)!=XR_XIR_VALUE_OK) continue;
        const XrXirValue *root=xr_xir_cell_canonical_root(&value);
        if (root && root->type==box.type && root->payload==box.payload) {
            XrXirValuePath path={0};CHECK(xir_cell_path_view(&value,&path)==XR_XIR_VALUE_OK);
            if (path.count==2 && path.steps[0].kind==XR_XIR_PATH_FIELD && !path.steps[0].selector && path.steps[1].kind==XR_XIR_PATH_INDEX && !path.steps[1].selector) {
                XrXirValue number={0};CHECK(xr_xir_vm_debug_snapshot_cell_value(adapter->snapshot,0,v,&number)==XR_XIR_VALUE_OK && number.payload==11);
                xr_xir_value_drop(&number);CHECK(count<64);slots[count++]=v;
            }
        }
        xr_xir_value_drop(&value);
    }
    CHECK(count);XrXirVmDebugSnapshot *old=NULL;CHECK(xdap_current_snapshot(adapter->launch->control,&old)==XR_XIR_VALUE_OK);
    uint32_t box_ref=origin_child(adapter,adapter->frames[0].scope,"box");uint64_t epoch=adapter->stop_epoch;
    CHECK(!child_set(adapter,box_ref,"values","empty",true) && adapter->stop_epoch==epoch+1);
    for (uint32_t i=0;i<count;++i) { XrXirValue value={0};CHECK(xr_xir_vm_debug_snapshot_cell_value(adapter->snapshot,0,slots[i],&value)==XR_XIR_VALUE_UNSUPPORTED); }
    box_ref=origin_child(adapter,adapter->frames[0].scope,"box");uint32_t array=child_set(adapter,box_ref,"values","incoming",true);CHECK(array && adapter->stop_epoch==epoch+2);
    for (uint32_t i=0;i<count;++i) { XrXirValue value={0};CHECK(xr_xir_vm_debug_snapshot_cell_value(adapter->snapshot,0,slots[i],&value)==XR_XIR_VALUE_OK && value.payload==100);xr_xir_value_drop(&value); }
    CHECK(!child_set(adapter,array,"[1]","right",true));request(adapter,"{\"seq\":8,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");capture.count=0;
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal);const char *expected[]={"100\n","11\n","13\n","17\n"};uint32_t found=0;
    for (uint32_t i=0;i<capture.count;++i) {
        XrJsonValue *reply=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);const char *event=xjson_get_string(reply,"event");
        if (event && !strcmp(event,"output")) { CHECK(found<4 && !strcmp(xjson_get_string(xjson_get_object(reply,"body"),"output"),expected[found++])); }xjson_free(reply);
    }
    CHECK(found==4 && xdap_adapter_free(adapter)==XR_XIR_CALL_READY);
    for (uint32_t i=0;i<count;++i) { XrXirValue value={0};CHECK(xr_xir_vm_debug_snapshot_cell_value(old,0,slots[i],&value)==XR_XIR_VALUE_OK && value.payload==11);xr_xir_value_drop(&value); }
    xr_xir_vm_debug_snapshot_drop(old);xr_xir_value_drop(&box);write_finish(&fixture);
    printf("Class field: actual projected aliases=%u, bounds unavailable/restored100, escaped old11/stdout100-11-13-17/physical0 PASS\n",count);
}
#include "test_class_wrapper_write.h"
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    for (uint32_t kind=0;kind<4;++kind) {
        size_t sites=class_case(kind,SIZE_MAX,0);CHECK(sites);
        for (uint32_t invalid=1;invalid<=5;++invalid) class_case(kind,SIZE_MAX,invalid);
        for (size_t i=0;i<sites;++i) if (resources || !i || i==sites-1) class_case(kind,i,0);
        printf("Actual Class identity: root=%u sites=%zu injection=%s old11/new99/aliases99/readonly/type/bounds/physical0 PASS\n",kind,sites,resources ? "ALL" : "FIRST_LAST_ONLY");
    }
    size_t sites=class_child_case(false,SIZE_MAX);CHECK(sites);class_child_case(true,SIZE_MAX);
    for (size_t i=0;i<sites;++i) if (resources || !i || i==sites-1) class_child_case(false,i);
    printf("Pure child Class: local209/module-denied11 sites=%zu injection=%s physical0 PASS\n",sites,resources ? "ALL" : "FIRST_LAST_ONLY");
    sites=class_rpc(SIZE_MAX,false);CHECK(sites);class_rpc(SIZE_MAX,true);
    for (size_t i=0;i<sites;++i) if (resources || !i || i==sites-1) class_rpc(i,false);
    printf("Class child setVariable: const selection/readonly/pure rejection, owned continuation/IO/PC/incoming23 sites=%zu injection=%s physical0 PASS\n",sites,resources ? "ALL" : "FIRST_LAST_ONLY");
    class_projected_loan();puts("Class scope denied during an actual projected ref loan; parent restored/stdout24/physical0 PASS");
    sites=class_observer_fault(SIZE_MAX);CHECK(sites);
    for (size_t i=0;i<sites;++i) if (resources || !i || i==sites-1) class_observer_fault(i);
    printf("Observer Array/Struct foreign Class identity materialization sites=%zu injection=%s preserves stop/owners/physical0 PASS\n",sites,resources ? "ALL" : "FIRST_LAST_ONLY");
    class_projection_aliases();
    for (uint32_t kind=0;kind<12;++kind) {
        size_t count=class_wrapper_case(kind,SIZE_MAX);CHECK(count);
        for (size_t i=0;i<count;++i) if (resources || !i || i==count-1) class_wrapper_case(kind,i);
        printf("Immutable wrapper Class: kind=%u sites=%zu injection=%s Source123/old11/pure-denied/wrapper-denied/physical0 PASS\n",kind,count,resources ? "ALL" : "FIRST_LAST_ONLY");
    }
    return 0;
}
