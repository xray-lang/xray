/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_domain_instance_prefix.c - Domain-only rejection before Instance effects
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"INSTANCE_PREFIX %d: %s\n",__LINE__,#c); exit(1); } } while (0)
/* This is the real current representation, not a copied or fabricated old layout. */
#include "xir/xxir_instance.c"

static void instance_prefix_case(XrXirDomain *old, void *unreadable) {
    XrXirInstance instance = {0};
    instance.domain=old; instance.program=unreadable;
    instance.call=unreadable; instance.executor=unreadable;
    instance.slots=unreadable; instance.function_gate=unreadable;
    instance.state=XR_XIR_INSTANCE_READY;
    instance.driving=true; instance.observing=true; instance.epoch=41;
    unsigned char saved[sizeof(instance)]; memcpy(saved,&instance,sizeof(instance));
    XrXirValue output={XR_XIR_I64,0,42}, original=output;
    XrXirCallResult failure={.status=XR_XIR_CALL_RETURNED,.value={XR_XIR_I64,0,43}};
    unsigned char original_failure[sizeof(failure)]; memcpy(original_failure,&failure,sizeof(failure));
    XrXirClassObservation *observation=(XrXirClassObservation *)(uintptr_t)1;
    XrXirWaitRequest wait={0}, original_wait=wait;
    CHECK(xr_xir_instance_state(&instance)==XR_XIR_INSTANCE_DRAINING);
    CHECK(xr_xir_instance_class_observation_open(&instance,1,&observation)==XR_XIR_CALL_BAD_ARGUMENT &&
        observation==(XrXirClassObservation *)(uintptr_t)1);
    CHECK(xr_xir_instance_start(&instance,0,NULL,0)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_start_test(&instance,0)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_start_function(&instance,&output,NULL,0)==XR_XIR_CALL_BAD_ARGUMENT);
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(&instance,1);
    CHECK(result.outcome.status==XR_XIR_CALL_BAD_ARGUMENT && !result.epoch &&
        !result.outcome.value.type && !result.outcome.value.reserved && !result.outcome.value.payload);
    CHECK(xr_xir_instance_resume(&instance,41,1)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_wait_request(&instance,41,1,&wait)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!memcmp(&wait,&original_wait,sizeof(wait)));
    CHECK(xr_xir_instance_take_result(&instance,&output)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_copy_failure(&instance,&failure)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!memcmp(&failure,original_failure,sizeof(failure)));
    CHECK(xr_xir_instance_cancel_current(&instance)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_stop(&instance)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_free(&instance)==XR_XIR_CALL_BAD_ARGUMENT);
    XrXirCallView view={.instance=&instance,.activation=unreadable};
    XrXirFaultDetail fault={430,0,3,4}, original_fault=fault;
    CHECK(xr_xir_instance_path_read(&view,NULL,NULL,&output,&fault)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!memcmp(&fault,&original_fault,sizeof(fault)));
    CHECK(xr_xir_instance_slot_write(&view,0,&output,false)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_function_at(&view,0,NULL,0,&output)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!memcmp(&output,&original,sizeof(output)) && !memcmp(&instance,saved,sizeof(instance)));
}
static void instance_prefix_current(void) {
    XrXirDomain *domain=NULL;
    const XrXirDomainBudgetControls controls={65536,65536,65536,65536,65536};
    CHECK(xr_xir_domain_new_budgeted(65536,&controls,&domain)==XR_XIR_VALUE_OK);
    XrXirInstance instance={.domain=domain,.state=XR_XIR_INSTANCE_NEW};
    XrXirClassObservation *observation=NULL;
    CHECK(xr_xir_instance_class_observation_open(&instance,1,&observation)==XR_XIR_CALL_READY && observation);
    CHECK(xr_xir_instance_state(&instance)==XR_XIR_INSTANCE_NEW);
    xr_xir_class_observation_drop(&observation);
    xr_xir_domain_drop(domain);
    CHECK(xr_xir_instance_free(NULL)==XR_XIR_CALL_READY &&
        xr_xir_instance_state(NULL)==XR_XIR_INSTANCE_DRAINING);
}
int main(void) {
    SYSTEM_INFO info; GetSystemInfo(&info); SIZE_T page=info.dwPageSize;
    unsigned char *memory=VirtualAlloc(NULL,page*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    CHECK(memory); DWORD previous=0;
    CHECK(VirtualProtect(memory+page,page,PAGE_NOACCESS,&previous));
    const unsigned char old_locked[2][8]={{0x58,0x52,0x44,0x4d,0,0,0,1},{0x58,0x52,0x44,0x4d,1,0,0,1}};
    for (unsigned mode=0;mode<2;++mode) {
        unsigned char *prefix=memory+page-8; memcpy(prefix,old_locked[mode],8);
        instance_prefix_case((XrXirDomain *)prefix,memory+page);
        CHECK(!memcmp(prefix,old_locked[mode],8));
    }
    CHECK(VirtualFree(memory,0,MEM_RELEASE)); instance_prefix_current();
    return 0;
}
