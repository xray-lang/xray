/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_projection_write.c - Actual paused nested value replacement transactions
 */
#define XR_DAP_WRITE_ENTRY reference_projection_write_main
#include "test_write.c"
#undef XR_DAP_WRITE_ENTRY
#include "xir/xxir_array.h"
#include "xir/xxir_struct.h"
static uint32_t projection_binding(XdapAdapter *adapter,const XrXirVmDebugSnapshot *snapshot,const char *name,XrXirValue *output) {
    XdapLexicalView lexical={0};CHECK(xdap_lexical_open(adapter->launch,snapshot,0,&lexical)==XR_XIR_OK);
    for (uint32_t b=lexical.environment;b;b=lexical.function->bindings[b-1].parent) {
        const XrXirSourceDebugBinding *binding=&lexical.function->bindings[b-1];
        const char *text=lexical.source->declarations[binding->declaration-1].name;
        if (strcmp(text,name)) continue;
        const char *read_name=NULL;size_t length=0;
        CHECK(xdap_lexical_read(&lexical,b,output,&read_name,&length)==XR_XIR_VALUE_OK);return binding->value;
    }
    CHECK(false);return UINT32_MAX;
}
static XrXirValue projection_read(XrXirValue *root,uint32_t selector,XrXirDomain *domain,bool array) {
    XrXirValue output={0};XrXirValueAdmission admission={.arena=xr_xir_value_arena(root),.domain=domain,.work=65536,.scratch_bytes=65536};
    XrXirFaultDetail fault={0};
    CHECK((array ? xr_xir_array_get(root,selector,&admission,&output,&fault) : xr_xir_struct_get(root,selector,&admission,&output))==XR_XIR_VALUE_OK);
    return output;
}
static void projection_pause(WriteFixture *fixture) {
    char request_text[2048];snprintf(request_text,sizeof(request_text),
        "{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":6}]}}",XR_DAP_PROJECTION_FIXTURE);
    request(fixture->adapter,request_text);request(fixture->adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(fixture->adapter,1000)==XR_XIR_OK && fixture->adapter->snapshot && !fixture->adapter->running);capture.count=0;
}
static void projection_steps(XrXirValue *root,XrXirDomain *domain,XrXirValuePathStep *steps) {
    XrXirValue parcel=projection_read(root,0,domain,true),array=projection_read(&parcel,0,domain,false),piece=projection_read(&array,0,domain,true);
    steps[0]=(XrXirValuePathStep){XR_XIR_PATH_INDEX,(XrXirType)root->type,0};
    steps[1]=(XrXirValuePathStep){XR_XIR_PATH_FIELD,(XrXirType)parcel.type,0};
    steps[2]=(XrXirValuePathStep){XR_XIR_PATH_INDEX,(XrXirType)array.type,0};
    steps[3]=(XrXirValuePathStep){XR_XIR_PATH_FIELD,(XrXirType)piece.type,0};
    xr_xir_value_drop(&piece);xr_xir_value_drop(&array);xr_xir_value_drop(&parcel);
}
static void projection_number(XrXirValue *root,XrXirDomain *domain,int64_t expected) {
    XrXirValue parcel=projection_read(root,0,domain,true),array=projection_read(&parcel,0,domain,false),piece=projection_read(&array,0,domain,true);
    XrXirValue number=projection_read(&piece,0,domain,false);CHECK(number.type==XR_XIR_I64 && number.payload==expected);
    XrXirValue fixed=projection_read(&piece,1,domain,false);CHECK(fixed.payload==13);
    XrXirValue sibling=projection_read(&parcel,1,domain,false);CHECK(sibling.payload==37);
    xr_xir_value_drop(&number);xr_xir_value_drop(&fixed);xr_xir_value_drop(&sibling);
    xr_xir_value_drop(&piece);xr_xir_value_drop(&array);xr_xir_value_drop(&parcel);
}
static void projection_return(WriteFixture *fixture,uint64_t epoch,bool changed) {
    CHECK(xdap_current_continue(fixture->adapter->launch->control,epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
    XdapCurrentResult result={0};CHECK(xdap_current_poll(fixture->adapter->launch->control,1000,&result)==XR_XIR_VALUE_OK);
    CHECK(result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_RETURNED);xdap_current_result_drop(&result);
    const char *expected[]={changed ? "99\n" : "11\n","11\n","37\n"};uint32_t found=0;
    for (uint32_t i=0;i<capture.count;++i) {
        XrJsonValue *message=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);const char *event=xjson_get_string(message,"event");
        if (event && !strcmp(event,"output")) { CHECK(found<3);CHECK(!strcmp(xjson_get_string(xjson_get_object(message,"body"),"output"),expected[found++])); }
        xjson_free(message);
    }
    CHECK(found==3);
}
static size_t projection_attempt(size_t fail,uint32_t invalid) {
    WriteFixture fixture=write_open(XR_DAP_PROJECTION_FIXTURE);projection_pause(&fixture);XdapAdapter *adapter=fixture.adapter;
    XrXirValue root={0},replacement={XR_XIR_I64,0,99};uint32_t operand=projection_binding(adapter,adapter->snapshot,"root",&root);
    XrXirVmDebugFrameInfo frame=write_frame(adapter->snapshot);uint64_t epoch=adapter->stop_epoch;
    XrXirValuePathStep steps[4];projection_steps(&root,fixture.domain,steps);XrXirValuePath path={steps,4};
    if (invalid==1) steps[3].selector=1;
    if (invalid==2) steps[2].selector=-1;
    if (invalid==3) steps[3].container=XR_XIR_I64;
    if (invalid==4) replacement=(XrXirValue){XR_XIR_BOOL,0,1};
    size_t live_before=runtime_live,bytes_before=runtime_bytes,begin=runtime_attempts;
    runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirValueStatus status=xdap_current_write_begin(adapter->launch->control,epoch,frame.id,operand,&path,&replacement);
    memset(steps,0xcc,sizeof(steps));XdapCurrentResult result={0};bool visited=false;XrXirVmDebugSnapshot *fresh=NULL;
    if (status==XR_XIR_VALUE_OK) {
        CHECK(xdap_current_poll(adapter->launch->control,1,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_STOPPED);
        status=xdap_current_write_finish(adapter->launch->control,&visited,&fresh);CHECK(visited);
    }
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;bool changed=!invalid && fail==SIZE_MAX;
    if (changed) {
        CHECK(status==XR_XIR_VALUE_OK && fresh && result.debug_epoch==epoch+1);
        XrXirValue updated={0};CHECK(projection_binding(adapter,fresh,"root",&updated)==operand);projection_number(&updated,fixture.domain,99);xr_xir_value_drop(&updated);
        XrXirVmDebugFrameInfo same=write_frame(fresh);CHECK(same.id==frame.id && same.instruction==frame.instruction && same.thread==frame.thread);
    } else {
        CHECK(!fresh && status==(fail!=SIZE_MAX ? XR_XIR_VALUE_OOM : invalid==2 ? XR_XIR_VALUE_BOUNDS : XR_XIR_VALUE_BAD_ARGUMENT));
        CHECK(runtime_live==live_before && runtime_bytes==bytes_before);
    }
    CHECK(!capture.count);projection_number(&root,fixture.domain,11);xdap_current_result_drop(&result);
    projection_return(&fixture,changed ? epoch+1 : epoch,changed);xr_xir_vm_debug_snapshot_drop(fresh);
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);projection_number(&root,fixture.domain,11);xr_xir_value_drop(&root);write_finish(&fixture);return sites;
}
static void projection_revoked(void) {
    for (uint32_t mode=0;mode<3;++mode) {
        WriteFixture fixture=write_open(XR_DAP_PROJECTION_FIXTURE);projection_pause(&fixture);XdapAdapter *adapter=fixture.adapter;
        XrXirValue root={0},replacement={XR_XIR_I64,0,99};uint32_t operand=projection_binding(adapter,adapter->snapshot,"root",&root);
        XrXirVmDebugFrameInfo frame=write_frame(adapter->snapshot);XrXirValuePathStep steps[4];projection_steps(&root,fixture.domain,steps);
        XrXirValuePath path={steps,4};size_t live_before=runtime_live,bytes_before=runtime_bytes;
        CHECK(xdap_current_write_begin(adapter->launch->control,adapter->stop_epoch,frame.id,operand,NULL,&replacement)==XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(xdap_current_write_begin(adapter->launch->control,adapter->stop_epoch,frame.id,operand,&path,&replacement)==XR_XIR_VALUE_OK);
        memset(steps,0xcc,sizeof(steps));
        if (mode==1) CHECK(xdap_current_cancel(adapter->launch->control)==XR_XIR_CALL_CANCEL_REQUESTED);
        if (mode!=2) {
            bool visited=true;XrXirVmDebugSnapshot *fresh=NULL;
            CHECK(xdap_current_write_finish(adapter->launch->control,&visited,&fresh)==XR_XIR_VALUE_BAD_ARGUMENT && !visited && !fresh);
            CHECK(runtime_live==live_before && runtime_bytes==bytes_before);
            if (!mode) projection_return(&fixture,adapter->stop_epoch,false);
        }
        CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);projection_number(&root,fixture.domain,11);
        xr_xir_value_drop(&root);write_finish(&fixture);
    }
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    projection_revoked();size_t sites=projection_attempt(SIZE_MAX,0);CHECK(sites);
    for (uint32_t i=1;i<5;++i) projection_attempt(SIZE_MAX,i);
    for (size_t i=0;i<sites;++i) if (resources || !i || i==sites-1) projection_attempt(i,0);
    printf("Actual nested paused99/old11/sibling37, owned selectors, readonly/type/bounds rollback; sites=%zu injection=%s physical=0/0 PASS\n",sites,resources ? "ALL" : "FIRST_LAST_ONLY");return 0;
}
