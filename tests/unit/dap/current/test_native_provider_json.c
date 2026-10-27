/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_native_provider_json.c - Canonical native receiving through real DAP JSON
 */
#include "app/dap/xdap_dispatch.h"
#include "xir/xxir_call_native_source_internal.h"
#include "base/xjson.h"
#include "../../test_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"native provider JSON %d: %s\n",__LINE__,#c);exit(1); } } while(0)
#include "../../xir/xir_go_source_native_compile_owner.h"
#include "../../xir/xir_runtime_allocations.h"
#include "native_provider.h"

typedef struct Message { char bytes[16384];size_t size; } Message;
typedef struct ProviderFixture {
    Message messages[128];uint32_t count,seq;
    XdapAdapter *adapter;XdapDetached *detached;XrXirDomain *domain;
    unsigned loads,releases,mode;bool reentry;
    size_t compiler_begin,runtime_begin;
    XirCallNativeSourceCode registration;
    XrXirCallEntry entries[NP_FUNCTIONS];unsigned char mask[NP_FUNCTIONS];uint8_t identity[32];
    int64_t output[8];unsigned outputs;
} ProviderFixture;
static ProviderFixture fixture;
static unsigned capture_kind;
static size_t capture_fault=SIZE_MAX;
static XrXirStatus message_write(void *owner,const char *bytes,size_t length) {
    ProviderFixture *f=owner;CHECK(f->count<128 && length<sizeof(f->messages[0].bytes));
    Message *message=&f->messages[f->count++];memcpy(message->bytes,bytes,length);message->bytes[length]=0;message->size=length;
    XrJsonValue *json=xjson_parse(bytes,length);
    if(!json || xjson_get_int(json,"seq")!=f->seq+1) fprintf(stderr,"expected seq=%u, actual message=%s\n",f->seq+1,message->bytes);
    CHECK(json && xjson_get_int(json,"seq")==++f->seq);xjson_free(json);
    return XR_XIR_OK;
}
static XrXirOutputStatus original_output(void *owner,const XrXirOutputGroup *group) {
    ProviderFixture *f=owner;CHECK(group && group->stream==XR_XIR_STDOUT && group->line && group->count==1);
    CHECK(f->outputs<8 && group->values[0].type==XR_XIR_I64);f->output[f->outputs++]=group->values[0].payload;return XR_XIR_OUTPUT_OK;
}
static void code_release(void *owner) { ++((ProviderFixture *)owner)->releases; }
static XrXirStatus native_load(void *owner,const XrXirSourceProduct *source,const XrXirArtifact *lowered,XdapNativeCode *output) {
    ProviderFixture *f=owner;++f->loads;CHECK(!output->program.entries && !output->mask && !output->source && !output->count);
    const XrXirSourceProductFacts *facts=xr_xir_compile_source_product_facts(source);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    CHECK(facts && facts->function_count==NP_FUNCTIONS && facts->entry==NP_ENTRY && module->stage==XR_XIR_LOWERED);
    CHECK(module->function_count==facts->function_count && module->declarations->entry_function==facts->entry);
    if(f->reentry) {
        CHECK(xdap_adapter_free(f->adapter)==XR_XIR_CALL_BUSY);
        CHECK(xdap_adapter_request(f->adapter,"{}",2)==XR_XIR_BAD_STRUCTURE);
    }
    if(f->mode==1) return XR_XIR_UNSUPPORTED;
    XdapNativeCode code={.program=native_provider_program,.source=&f->registration,.mask=f->mask,.count=NP_FUNCTIONS};
    f->registration=native_provider_native_source;memset(f->mask,1,sizeof(f->mask));
    code.program.code=(XrXirCodeLease){f,code_release};
    switch(f->mode) {
    case 2:f->registration.version=0;break;
    case 3:f->mask[NP_LEAF]=2;break;
    case 4:memcpy(f->identity,code.program.packet.identity,32);f->identity[0]^=1;code.program.packet.identity=f->identity;break;
    case 5:memcpy(f->entries,code.program.entries,sizeof(f->entries));f->entries[NP_LEAF].resume=f->entries[NP_CALLER].resume;code.program.entries=f->entries;break;
    case 6:--code.count;break;
    case 7:++code.program.target.abi_version;break;
    case 8:memset(f->mask,0,sizeof(f->mask));break;
    case 9:f->mask[NP_ENTRY]=0;
        for(uint32_t m=0;m<code.program.declarations->module_count;++m) f->mask[code.program.declarations->modules[m].initializer]=0;break;
    case 10:f->mask[NP_LEAF]=0;break;
    case 11:f->mask[NP_ENTRY]=0;break;
    default:break;
    }
    fixture.compiler_begin=effects_compile_attempts;fixture.runtime_begin=runtime_attempts;
    if(capture_kind==1 && capture_fault!=SIZE_MAX) effects_compile_fail_at=effects_compile_attempts+capture_fault;
    if(capture_kind==2 && capture_fault!=SIZE_MAX) runtime_fail_at=runtime_attempts+capture_fault;
    *output=code;return XR_XIR_OK;
}
static XrJsonValue *response(const char *command,bool success) {
    for(uint32_t i=fixture.count;i;--i) {
        Message *m=&fixture.messages[i-1];XrJsonValue *j=xjson_parse(m->bytes,m->size);CHECK(j);
        const char *type=xjson_get_string(j,"type"),*actual=xjson_get_string(j,"command");
        if(type && actual && !strcmp(type,"response") && !strcmp(actual,command)) {
            if(xjson_get_bool(j,"success")!=success) fprintf(stderr,"unexpected response: %s\n",m->bytes);
            CHECK(xjson_get_bool(j,"success")==success);return j;
        }
        xjson_free(j);
    }
    CHECK(false);return NULL;
}
static unsigned events(const char *event) {
    unsigned count=0;for(uint32_t i=0;i<fixture.count;++i) {
        XrJsonValue *j=xjson_parse(fixture.messages[i].bytes,fixture.messages[i].size);CHECK(j);
        const char *actual=xjson_get_string(j,"event");if(actual && !strcmp(actual,event)) ++count;xjson_free(j);
    }
    return count;
}
static void request(const char *text,const char *command,bool success) {
    CHECK(xdap_adapter_request(fixture.adapter,text,strlen(text))==XR_XIR_OK);
    XrJsonValue *j=response(command,success);xjson_free(j);
}
static void setup(unsigned mode) {
    memset(&fixture,0,sizeof(fixture));fixture.mode=mode;
    const XrXirCompileContext *context=effects_source_owner(67108864,128000000);
    XrSourceCompilerConfig compiler_config={*context,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_SOURCE_STDLIB,
        xr_source_compile_default_manifest_limits()};XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&compiler_config,NULL,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);
    xr_source_compile_diagnostic_free(&diagnostic);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&fixture.domain)==XR_XIR_VALUE_OK);
    XdapAdapterConfig config={.observer_metadata=fixture.domain,.write=message_write,.context=&fixture,.message_limit=16384,
        .detached=&fixture.detached,.retain_output=true,.native={&fixture,native_load}};
    CHECK(xr_xir_instance_config_init(&config.instance,sizeof(config.instance))==XR_XIR_CALL_READY);
    config.instance.metadata_limit=config.instance.value_limit=config.instance.call_limit=1048576;
    config.instance.requested_value_limit=config.instance.requested_call_limit=1048576;
    config.instance.work_limit=128000000;config.instance.poll_limit=10000;config.instance.depth_limit=64;
    config.instance.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,original_output,&fixture};
    XdapAdapterConfig malformed=config;malformed.native.load=NULL;
    CHECK(xdap_adapter_new(&compiler,&malformed,&fixture.adapter)==XR_XIR_BAD_STRUCTURE && compiler && !fixture.adapter);
    CHECK(xdap_adapter_new(&compiler,&config,&fixture.adapter)==XR_XIR_OK && !compiler);fixture.reentry=true;
    request("{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}","initialize",true);
}
static void launch(const char *command,bool stop,bool success) {
    char text[4096];CHECK(snprintf(text,sizeof(text),"{\"seq\":2,\"type\":\"request\",\"command\":\"%s\",\"arguments\":{\"program\":\"%s\",\"stopOnEntry\":%s}}",command,XR_NATIVE_PROVIDER_FIXTURE,stop ? "true":"false")>0);
    request(text,command,success);
}
static void filter(const char *name) {
    char text[512];CHECK(snprintf(text,sizeof(text),"{\"seq\":3,\"type\":\"request\",\"command\":\"setExceptionBreakpoints\",\"arguments\":{\"filters\":[\"%s\"]}}",name)>0);
    request(text,"setExceptionBreakpoints",true);
}
static void drive(void) {
    unsigned polls=0;do { CHECK(++polls<1000 && xdap_adapter_step(fixture.adapter,64)==XR_XIR_OK); } while(xdap_adapter_running(fixture.adapter));
}
static void teardown(void) {
    fixture.reentry=false;CHECK(xdap_adapter_free(fixture.adapter)==XR_XIR_CALL_READY);fixture.adapter=NULL;
    CHECK(!fixture.detached && !xr_xir_domain_budget_stats(fixture.domain).metadata_live);xr_xir_domain_drop(fixture.domain);
    CHECK(!runtime_live && !runtime_bytes);effects_source_owners_free();
}
static void stop_inspect(void) {
    CHECK(events("stopped")==1 && !fixture.outputs);
    request("{\"seq\":5,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}","stackTrace",true);
    XrJsonValue *j=response("stackTrace",true);XrJsonValue *frames=xjson_get_array(xjson_get_object(j,"body"),"stackFrames");
    CHECK(xjson_array_len(frames)==3);XrJsonValue *top=xjson_array_get(frames,0);CHECK(!strcmp(xjson_get_string(top,"name"),"native_leaf"));
    CHECK(xjson_get_int(top,"line")==5);CHECK(!strcmp(xjson_get_string(xjson_array_get(frames,1),"name"),"native_caller"));
    const char *actual=xjson_get_string(xjson_get_object(top,"source"),"path"),*expected=XR_NATIVE_PROVIDER_FIXTURE;
    CHECK(actual && strlen(actual)==strlen(expected));
    for(size_t at=0;expected[at];++at) CHECK((actual[at]=='\\' ? '/':actual[at])==expected[at]);
    int64_t handle=xjson_get_int(top,"id");xjson_free(j);
    request("{\"seq\":6,\"type\":\"request\",\"command\":\"exceptionInfo\",\"arguments\":{\"threadId\":1}}","exceptionInfo",true);
    j=response("exceptionInfo",true);XrJsonValue *body=xjson_get_object(j,"body");
    const char *identity=xjson_get_string(body,"exceptionId"),*mode=xjson_get_string(body,"breakMode");
    CHECK(identity && !strcmp(identity,"NativeError.Bad") && mode && !strcmp(mode,"always"));xjson_free(j);
    char text[512];CHECK(snprintf(text,sizeof(text),"{\"seq\":7,\"type\":\"request\",\"command\":\"scopes\",\"arguments\":{\"frameId\":%lld}}",(long long)handle)>0);
    request(text,"scopes",true);
    request("{\"seq\":8,\"type\":\"request\",\"command\":\"next\",\"arguments\":{\"threadId\":1}}","next",false);
    CHECK(!xdap_adapter_running(fixture.adapter) && events("stopped")==1);
}
static void lifecycle(unsigned action) {
    setup(0);launch(action==5 ? "attach":"launch",false,true);CHECK(fixture.loads==1 && !fixture.releases);
    memset(fixture.mask,0,sizeof(fixture.mask));fixture.registration.version=0;
    request("{\"seq\":3,\"type\":\"request\",\"command\":\"setFunctionBreakpoints\",\"arguments\":{\"breakpoints\":[{\"name\":\"native_leaf\"}]}}","setFunctionBreakpoints",false);
    char bp[4096];CHECK(snprintf(bp,sizeof(bp),"{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":5}]}}",XR_NATIVE_PROVIDER_FIXTURE)>0);
    request(bp,"setBreakpoints",false);
    filter("caught");request("{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}","configurationDone",true);
    request("{\"seq\":4,\"type\":\"request\",\"command\":\"pause\",\"arguments\":{\"threadId\":1}}","pause",false);drive();stop_inspect();
    if(action==1) {
        fixture.mode=2;request("{\"seq\":9,\"type\":\"request\",\"command\":\"restart\"}","restart",false);
        CHECK(fixture.loads==2 && fixture.releases==1);stop_inspect();fixture.mode=0;
        request("{\"seq\":10,\"type\":\"request\",\"command\":\"restart\"}","restart",true);
        CHECK(fixture.loads==3 && fixture.releases==2);drive();CHECK(events("stopped")==2 && fixture.outputs==1 && fixture.output[0]==91);
    }
    if(action==6) {
        request("{\"seq\":11,\"type\":\"request\",\"command\":\"disconnect\",\"arguments\":{\"terminateDebuggee\":true}}","disconnect",true);
        CHECK(xdap_adapter_closed(fixture.adapter) && events("terminated")==1 && fixture.outputs==1 && fixture.output[0]==91);
    } else if(action==2) {
        request("{\"seq\":11,\"type\":\"request\",\"command\":\"terminate\"}","terminate",true);drive();
        CHECK(events("terminated")==1 && fixture.outputs==1 && fixture.output[0]==91);
    } else if(action==3 || action==5) {
        request(action==5 ? "{\"seq\":11,\"type\":\"request\",\"command\":\"disconnect\"}":
            "{\"seq\":11,\"type\":\"request\",\"command\":\"disconnect\",\"arguments\":{\"terminateDebuggee\":false}}","disconnect",true);
        CHECK(fixture.detached && xdap_adapter_closed(fixture.adapter));fixture.reentry=false;
        CHECK(xdap_adapter_free(fixture.adapter)==XR_XIR_CALL_READY);fixture.adapter=NULL;unsigned before=fixture.releases;
        bool ended=false;unsigned polls=0;while(!ended) { CHECK(++polls<1000 && xdap_detached_poll(fixture.detached,64,&ended)==XR_XIR_OK); }
        XrXirCallResult result={0};CHECK(xdap_detached_result(fixture.detached,&result)==XR_XIR_VALUE_OK && result.status==XR_XIR_CALL_RETURNED);xr_xir_call_result_drop(&result);
        CHECK(fixture.releases==before && fixture.outputs==2 && fixture.output[0]==91 && fixture.output[1]==73);
        CHECK(xdap_detached_free(fixture.detached)==XR_XIR_CALL_READY);fixture.detached=NULL;CHECK(fixture.releases==before+1);
        CHECK(!xr_xir_domain_budget_stats(fixture.domain).metadata_live);xr_xir_domain_drop(fixture.domain);
        CHECK(!runtime_live && !runtime_bytes);effects_source_owners_free();return;
    } else if(action==4) {
        unsigned before=fixture.releases;teardown();CHECK(fixture.releases==before+1 && fixture.outputs==1 && fixture.output[0]==91);return;
    } else {
        request("{\"seq\":11,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}","continue",true);drive();
        CHECK(events("terminated")==1 && events("exited")==1 && fixture.output[fixture.outputs-2]==91 && fixture.output[fixture.outputs-1]==73);
    }
    unsigned loads=fixture.loads;teardown();CHECK(fixture.releases==loads);
}
static void mixed_provider(unsigned mode) {
    setup(mode);launch("launch",mode==9,true);filter("caught");
    request("{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}","configurationDone",true);drive();
    if(mode==9) {
        CHECK(events("stopped")==1 && !fixture.outputs);fixture.mode=0;
        request("{\"seq\":9,\"type\":\"request\",\"command\":\"restart\"}","restart",false);
        CHECK(fixture.loads==2 && fixture.releases==1 && !xdap_adapter_running(fixture.adapter));
        request("{\"seq\":10,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}","continue",true);drive();
        CHECK(events("stopped")==2);
    } else CHECK(events("stopped")==1);
    request("{\"seq\":11,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}","stackTrace",true);
    XrJsonValue *j=response("stackTrace",true),*frames=xjson_get_array(xjson_get_object(j,"body"),"stackFrames");
    CHECK(xjson_array_len(frames)==3 && xjson_get_int(xjson_array_get(frames,0),"line")==5);xjson_free(j);
    request("{\"seq\":12,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}","continue",true);drive();
    CHECK(events("terminated")==1 && fixture.outputs==2 && fixture.output[0]==91 && fixture.output[1]==73);
    unsigned loads=fixture.loads;teardown();CHECK(fixture.releases==loads);
}
static size_t receiving_fault(unsigned kind,size_t fault) {
    setup(0);capture_kind=kind;capture_fault=fault;effects_compile_injected=false;
    char text[4096];CHECK(snprintf(text,sizeof(text),"{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",XR_NATIVE_PROVIDER_FIXTURE)>0);
    XrXirStatus status=xdap_adapter_request(fixture.adapter,text,strlen(text));
    size_t sites=kind==1 ? effects_compile_attempts-fixture.compiler_begin:runtime_attempts-fixture.runtime_begin;
    CHECK(fixture.loads==1 && !fixture.outputs && fixture.releases<=1);
    if(fault!=SIZE_MAX) CHECK(kind==1 ? effects_compile_injected : runtime_attempts>runtime_fail_at);
    CHECK(status==XR_XIR_OK || status==XR_XIR_BUDGET || status==XR_XIR_OUT_OF_MEMORY);
    effects_compile_fail_at=runtime_fail_at=SIZE_MAX;capture_kind=0;capture_fault=SIZE_MAX;
    if(fault==SIZE_MAX) { XrJsonValue *j=response("launch",true);xjson_free(j);CHECK(!fixture.releases); }
    teardown();CHECK(fixture.releases==1);return sites;
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();
    if(argc==2) {
        bool census=!strcmp(argv[1],"--census"),boundary=!strcmp(argv[1],"--boundary"),resources=!strcmp(argv[1],"--resources");
        CHECK(census || boundary || resources);
        for(unsigned kind=1;kind<=2;++kind) {
            size_t sites=receiving_fault(kind,SIZE_MAX);CHECK(sites);
            if(boundary) { receiving_fault(kind,0);receiving_fault(kind,sites-1); }
            if(resources) for(size_t i=0;i<sites;++i) receiving_fault(kind,i);
            printf("NATIVE_PROVIDER_RECEIVING kind=%u sites=%zu injected=%zu physical=0/0\n",kind,sites,resources ? sites : boundary ? (size_t)2 : (size_t)0);
        }
        return 0;
    }
    CHECK(argc==1);
    for(unsigned mode=1;mode<=8;++mode) { setup(mode);launch("launch",false,false);CHECK(fixture.loads==1 && fixture.releases==(mode==1 ? 0u:1u) && !fixture.outputs && !events("stopped"));
        fixture.mode=0;launch("launch",false,true);CHECK(fixture.loads==2);teardown();CHECK(fixture.releases==(mode==1 ? 1u:2u)); }
    setup(0);launch("launch",true,false);CHECK(fixture.loads==1 && fixture.releases==1);teardown();
    setup(11);launch("launch",true,false);CHECK(fixture.loads==1 && fixture.releases==1);teardown();
    for(unsigned action=0;action<7;++action) lifecycle(action);
    mixed_provider(9);mixed_provider(10);
    puts("Canonical native provider JSON rejection/retry/filters/stack/restart/terminate/free/detach/lease: PASS");return 0;
}
