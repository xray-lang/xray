/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_functions.c - Verified function origins and shared specialization counts
 */
#define XDAP_HITS_MAIN reference_hits_main
#include "test_hits.c"
static void functions_origins(XdapAdapter *adapter) {
    uint32_t origin=UINT32_MAX,instances=0;bool renamed=false;
    for (uint32_t f=0;f<adapter->launch->function_count;++f) {
        const XdapCodeFunction *function=&adapter->launch->functions[f];
        if (strcmp(function->source_name,"record")) continue;
        if (origin==UINT32_MAX) origin=function->origin;
        CHECK(function->origin==origin);++instances;
        if (strcmp(function->name,function->source_name)) renamed=true;
    }
    CHECK(instances>=3 && renamed);
    XdapFunctionSelection selection={.name="record",.length=6,.rule={.hit_kind=XR_XIR_VM_DEBUG_HIT_EQUAL,.hit_value=3,.group=1}};
    XrXirVmDebugRule rules[32]={{0}};uint32_t count=0;
    CHECK(xdap_launch_function_rules(adapter->launch,&selection,rules,32,&count)==XR_XIR_OK && count==instances);
    for (uint32_t i=0;i<count;++i) CHECK(rules[i].group==1 && rules[i].hit_value==3);
    uint32_t saved_count=77;XrXirVmDebugRule occupied={.location={9,10},.hit_value=55};
    CHECK(xdap_launch_function_rules(adapter->launch,&selection,&occupied,1,&saved_count)==XR_XIR_BUDGET && saved_count==77);
    CHECK(occupied.location.function==9 && occupied.location.instruction==10 && occupied.hit_value==55);
    selection.name="record\0junk";selection.length=11;
    CHECK(xdap_launch_function_rules(adapter->launch,&selection,rules,32,&count)==XR_XIR_OK && !count);
    selection.name="ambiguous";selection.length=9;
    rules[0]=occupied;
    CHECK(xdap_launch_function_rules(adapter->launch,&selection,rules,32,&saved_count)==XR_XIR_UNRESOLVED && saved_count==77);
    CHECK(rules[0].location.function==9 && rules[0].location.instruction==10 && rules[0].hit_value==55);
}
static void functions_group_rejection(void) {
    XrCompileResources *ledger=NULL;XrXirDomain *domain=NULL;XrCompileResourceStats baseline={0};
    XdapAdapter *adapter=hits_setup(&ledger,&domain,&baseline);functions_origins(adapter);hits_set(adapter,">=2");
    XdapFunctionSelection selection={"record",6,adapter->function_rules[0]};XrXirVmDebugRule rules[32]={{0}};uint32_t count=0;
    CHECK(xdap_launch_function_rules(adapter->launch,&selection,rules,32,&count)==XR_XIR_OK && count>1);
    rules[1].hit_value=9;CHECK(xdap_current_rules_replace(adapter->launch->control,rules,count)==XR_XIR_VALUE_BAD_ARGUMENT);
    rules[1].hit_value=rules[0].hit_value;rules[1].group=33;
    CHECK(xdap_current_rules_replace(adapter->launch->control,rules,count)==XR_XIR_VALUE_BAD_ARGUMENT);
    request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    const int64_t expected[]={22,33,44};hits_drive(adapter,expected,3);hits_dispose(adapter,ledger,domain,baseline);
}
int main(int argc,char **argv) {
    bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);xr_test_suppress_dialogs();
    const int64_t all[]={11,22,33,44},equal[]={33},multiples[]={22,44};
    hits_case("0",all,4,false);hits_case("==3",equal,1,true);hits_case("%2",multiples,2,true);
    functions_group_rejection();hits_duplicates();hits_replacement();hits_rejected_rules();
    size_t sites=hits_failure(SIZE_MAX);CHECK(sites);if (resources) for (size_t i=0;i<sites;++i) hits_failure(i);
    printf("Source function specialization origins, shared counters/reset/restart, rejection and setter FI: %zu sites, injection=%s; physical0/0\n",
        sites,resources ? "ALL" : "NOT_RUN");return 0;
}
