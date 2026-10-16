/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_wait_resources.c - Real factory and consumer resource ownership
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_source_fixture_owner.h"
#include "xir_go_source_native_allocations.h"
static XrXirInstance *resource_observed_instance;
#include "xir/xxir_task.c"
static void source_native_event_role(SourceNativeAllocationEvent *event) {
    XrXirInstance *instance=resource_observed_instance;
    if(!instance || !instance->executor) return;
    XrXirTaskExecutor *executor=instance->executor;
    XrXirCall *call=executor->current ? executor->current->call : instance->call;
    if(!call) return;
    event->role=executor->current && executor->current->task ? 2u : 1u;
    if(generator_core_activation(executor,executor->current)) event->role=3;
    if(call->top) event->entry=(unsigned)(call->top->entry-call->config.entries);
}
#include "xir_generator_wait_resource_fixture.h"
#include "xir_generator_wait_resource_operation.h"
#include "xir_generator_wait_resource_gates.h"
int main(int argc,char **argv) {
    CHECK(argc==3 || argc==5);setvbuf(stdout,NULL,_IONBF,0);
    unsigned selected=UINT32_MAX;
    if(argc==5) {
        char *end=NULL;unsigned long mode=strtoul(argv[3],&end,10);
        CHECK(*argv[3] && !*end && mode<3 && !strcmp(argv[2],"probe-fault"));selected=(unsigned)mode;
    }
    WaitResourceFixture fixture={0};resource_fixture_new(&fixture,argv[1]);
    for(unsigned mode=0;mode<3;++mode) {
        if(argc==5 && mode!=selected) continue;
        WaitResourceRequest request=resource_request(mode);
        WaitResourceCost base=resource_operation(&fixture,&request);
        CHECK(base.failure==XR_XIR_CALL_READY && base.sites && base.primary[0]==XR_XIR_CALL_RETURNED && base.primary[1]==XR_XIR_CALL_RETURNED);
        resource_print_base(mode,&base);memcpy(resource_normal,source_native_events,base.sites*sizeof(*resource_normal));
        if(!strcmp(argv[2],"measure")) CHECK(argc==3);
        else if(!strcmp(argv[2],"axes")) resource_axes(&fixture,mode,&base);
        else if(!strcmp(argv[2],"faults")) resource_faults(&fixture,mode,&base);
        else if(!strcmp(argv[2],"cancel")) resource_cancel(&fixture,mode,&base);
        else if(!strcmp(argv[2],"probe-fault")) {
            CHECK(argc==5);char *end=NULL;request.fault=(size_t)strtoull(argv[4],&end,10);
            CHECK(*argv[4] && !*end && request.fault<base.sites);
            WaitResourceCost failed=resource_operation(&fixture,&request);
            resource_print_events(mode,UINT32_MAX,UINT32_MAX);
            CHECK(failed.hit && failed.failure==XR_XIR_CALL_OOM && failed.sites==request.fault+1);
        } else CHECK(false);
    }
    WaitResourceRequest retirement=resource_request(1);retirement.retire=true;
    CHECK(resource_operation(&fixture,&retirement).failure==XR_XIR_CALL_READY);
    resource_fixture_free(&fixture);return 0;
}
