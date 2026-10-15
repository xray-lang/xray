/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "xir/xxir_program.h"
#include "xir/xxir_instance_value.h"
#include "xir/xxir_instance_debug_internal.h"
#include <stdio.h>
#include <string.h>
typedef XrXirCallStatus (*CellReadAt)(XrXirCallView *,uint32_t,const XrXirValue *,XrXirValue *,XrXirFaultDetail *);
typedef XrXirCallStatus (*CellWriteAt)(XrXirCallView *,uint32_t,const XrXirValue *,const XrXirValue *,XrXirFaultDetail *);
typedef XrXirCallStatus (*ProjectAt)(XrXirCallView *,uint32_t,XrXirType,const XrXirValueReceiver *,const XrXirValuePath *,XrXirValue *);
typedef XrXirCallStatus (*PathReadAt)(XrXirCallView *,uint32_t,const XrXirValueReceiver *,const XrXirValuePath *,XrXirValue *,XrXirFaultDetail *);
typedef XrXirCallStatus (*PathWriteAt)(XrXirCallView *,uint32_t,const XrXirValueReceiver *,const XrXirValuePath *,const XrXirValue *,XrXirFaultDetail *);
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#condition);return false; \
} } while (0)
typedef struct HostAccessWitness {
    const void *opaque;
    XrXirDomain *domain;
    XrXirCallAccounting *accounting;
    uint32_t callbacks;
    bool passed;
} HostAccessWitness;
/* A legitimate host validator grants no Function binding admission. */
static XrXirValueStatus host_binding(void *context,const XrXirFunctionBinding *binding,
    XrXirType type,uint64_t *work) {
    (void)context;(void)binding;(void)type;(void)work;
    return XR_XIR_VALUE_BAD_ARGUMENT;
}
static bool host_rejections(XrXirCallView *view,HostAccessWitness *witness) {
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    CHECK(admission && view->instance==witness->opaque && admission->context==witness->opaque);
    CHECK(admission->domain==witness->domain &&
        admission->function==(witness->domain?host_binding:NULL));
    const XrXirValueAdmission before=*admission;
    const XrXirDomainStats stats=xr_xir_domain_stats(witness->domain);
    const XrXirCallAccounting accounting=*witness->accounting;
    const XrXirValue value={XR_XIR_I64,0,42};
    const XrXirValueReceiver receiver={.kind=XR_XIR_ROOT_VALUE,.type=XR_XIR_I64,.value=value};
    const XrXirValuePath path={0};
    const XrXirFaultDetail observed={XR_XIR_PANIC_BOUNDS,0,9,7};
    XrXirCallClassAccess access={0};
    CHECK(xir_instance_debug_class_access(view,&value,&access)==XR_XIR_CALL_BAD_STATE);
    CHECK(!access.value && !access.loan.cell && !access.authority.activation);
    for (uint32_t occupied=0;occupied<2;++occupied) {
        XrXirValue output=occupied?value:(XrXirValue){0};
        const XrXirValue original=output;
        XrXirFaultDetail fault=observed;
        CHECK(xr_xir_instance_cell_read_at(view,0,&value,&output,&fault)==XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_cell_write_at(view,0,&value,&value,&fault)==XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_cell_project_at(view,0,XR_XIR_UNIT,&receiver,&path,&output)==XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_path_read_at(view,0,&receiver,&path,&output,&fault)==XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_path_write_at(view,0,&receiver,&path,&value,&fault)==XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_path_push_at(view,0,&receiver,&path,&value,&fault)==XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_path_length_at(view,0,&receiver,&path,&output,&fault)==XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_path_capacity_at(view,0,&receiver,&path,&output,&fault)==XR_XIR_CALL_BAD_STATE);
        CHECK(memcmp(&output,&original,sizeof(output))==0 && memcmp(&fault,&observed,sizeof(fault))==0);
    }
    const XrXirDomainStats after=xr_xir_domain_stats(witness->domain);
    CHECK(admission->arena==before.arena && admission->domain==before.domain &&
        admission->function==before.function && admission->context==before.context &&
        admission->work==before.work && admission->scratch_bytes==before.scratch_bytes);
    CHECK(after.live_bytes==stats.live_bytes && after.peak_bytes==stats.peak_bytes &&
        after.allocations==stats.allocations && after.frees==stats.frees && after.reallocations==stats.reallocations);
    CHECK(witness->accounting->live_bytes==accounting.live_bytes &&
        witness->accounting->peak_bytes==accounting.peak_bytes &&
        witness->accounting->allocations==accounting.allocations && witness->accounting->frees==accounting.frees &&
        witness->accounting->polls==accounting.polls && witness->accounting->depth==accounting.depth &&
        witness->accounting->peak_depth==accounting.peak_depth);
    return true;
}
static XrXirAction host_resume(XrXirCallView *view) {
    HostAccessWitness *witness=(HostAccessWitness *)view->environment;
    ++witness->callbacks;
    witness->passed=host_rejections(view,witness);
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN};
}
static bool host_call_control(bool current_domain) {
    XrXirDomain *domain=NULL;
    if (current_domain) CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    /* This complete host context is smaller than an Instance. A premature
     * Instance field read crosses its ASan object boundary. */
    uint64_t opaque=UINT64_C(0x619ce5372d48a0bf);
    XrXirCallAccounting accounting={0};
    HostAccessWitness witness={&opaque,domain,&accounting,0,false};
    const XrXirCallEntry entry={.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_UNIT,
        .resume=host_resume,.environment=&witness};
    XrXirCallConfig config;
    bool passed=xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY;
    if (passed) {
        config.entries=&entry;config.entry_count=1;config.instance=&opaque;
        config.byte_limit=65536;config.poll_limit=10;config.depth_limit=2;config.accounting=&accounting;
        config.admission=(XrXirValueAdmission){.domain=domain,.function=current_domain?host_binding:NULL,
            .context=&opaque,.work=1024,.scratch_bytes=4096};
    }
    XrXirCall *call=NULL;
    if (passed) passed=xr_xir_call_new(&config,0,NULL,0,&call)==XR_XIR_CALL_READY;
    if (passed) {
        const XrXirCallResult result=xr_xir_call_poll_bounded(call,UINT64_MAX);
        passed=result.status==XR_XIR_CALL_RETURNED && !result.value.type && !result.value.reserved &&
            !result.value.payload && !result.wake && witness.callbacks==1 && witness.passed;
        XrXirValue output={0};
        if (passed) passed=xr_xir_call_take_result(call,&output)==XR_XIR_CALL_RETURNED &&
            !output.type && !output.reserved && !output.payload;
    }
    if (call && xr_xir_call_free(call)!=XR_XIR_CALL_READY) passed=false;
    passed=passed && accounting.live_bytes==0 && accounting.depth==0 &&
        accounting.allocations==accounting.frees && opaque==UINT64_C(0x619ce5372d48a0bf);
    xr_xir_domain_drop(domain);
    CHECK(passed);
    printf("CLASS_HOST_OWNER domain=%u context_bytes=%zu callbacks=%u gates=16 live=0\n",
        current_domain?1u:0u,sizeof(opaque),witness.callbacks);
    return true;
}
int main(void) {
    CellReadAt read=xr_xir_instance_cell_read_at;CellWriteAt write=xr_xir_instance_cell_write_at;
    ProjectAt project=xr_xir_instance_cell_project_at;
    PathReadAt path=xr_xir_instance_path_read_at,length=xr_xir_instance_path_length_at,capacity=xr_xir_instance_path_capacity_at;
    PathWriteAt replace=xr_xir_instance_path_write_at,push=xr_xir_instance_path_push_at;
    XrXirValue output={0},value={XR_XIR_I64,0,42};XrXirValueReceiver receiver={0};
    XrXirValuePath empty={0};XrXirFaultDetail fault={0};
    if (read(NULL,0,&value,&output,&fault)!=XR_XIR_CALL_BAD_STATE ||
        write(NULL,0,&value,&value,&fault)!=XR_XIR_CALL_BAD_STATE ||
        project(NULL,0,XR_XIR_UNIT,&receiver,&empty,&output)!=XR_XIR_CALL_BAD_STATE ||
        path(NULL,0,&receiver,&empty,&output,&fault)!=XR_XIR_CALL_BAD_STATE ||
        length(NULL,0,&receiver,&empty,&output,&fault)!=XR_XIR_CALL_BAD_STATE ||
        capacity(NULL,0,&receiver,&empty,&output,&fault)!=XR_XIR_CALL_BAD_STATE ||
        replace(NULL,0,&receiver,&empty,&value,&fault)!=XR_XIR_CALL_BAD_STATE ||
        push(NULL,0,&receiver,&empty,&value,&fault)!=XR_XIR_CALL_BAD_STATE) return 1;
    if (output.type || output.reserved || output.payload || !xr_xir_fault_empty(fault)) return 1;
    return !host_call_control(false) || !host_call_control(true);
}
