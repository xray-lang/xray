/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_global_core.c - Actual caller module observations and prepared slot writes
 */
#define main reference_global_dispatch_main
#include "test_dispatch.c"
#undef main
#include "xir/xxir_vm_debug_internal.h"
#include "app/dap/xdap_dispatch_internal.h"
#include "test_global_core_helpers.inc.c"
static const XrXirValuePath global_whole={0};
typedef struct GlobalWriteTarget { uint64_t epoch;uint32_t frame,slot; } GlobalWriteTarget;
static XrXirValueStatus global_write(GlobalFixture *fixture,const GlobalWriteTarget *target,
    const XrXirValuePath *path,const XrXirValue *value,XrXirVmDebugSnapshot **fresh) {
    XdapCurrent *control=fixture->adapter->launch->control;
    XrXirVmDebugFrameInfo info=global_frame(fixture->adapter->snapshot,target->frame);
    CHECK(xdap_current_write_slot_begin(control,target->epoch,info.id,target->slot,path,value)==XR_XIR_VALUE_OK);
    XdapCurrentResult result={0};CHECK(xdap_current_poll(control,1,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_STOPPED);
    bool visited=false;XrXirValueStatus status=xdap_current_write_finish(control,&visited,fresh);CHECK(visited);xdap_current_result_drop(&result);return status;
}
static void global_text(const XrXirVmDebugSnapshot *snapshot,uint32_t frame,uint32_t slot,const char *expected,size_t length) {
    XrXirValue value={0};const char *text=NULL;size_t bytes=0;
    CHECK(xir_vm_debug_snapshot_slot(snapshot,frame,slot,&value)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_view(&value,&text,&bytes) && bytes==length && !memcmp(text,expected,length));xr_xir_value_drop(&value);
}
static XrXirValue global_array(GlobalFixture *fixture,const XrXirVmDebugSnapshot *snapshot,uint32_t frame,uint32_t slot,int64_t expected) {
    XrXirValue value={0},element={0};CHECK(xir_vm_debug_snapshot_slot(snapshot,frame,slot,&value)==XR_XIR_VALUE_OK);
    XdapValueView view={&value,fixture->domain,snapshot,frame};XdapValueChildInfo child={0};
    CHECK(xdap_value_child(&view,1,&element,&child)==XR_XIR_VALUE_OK && element.type==XR_XIR_I64 && (int64_t)element.payload==expected);
    xr_xir_value_drop(&element);return value;
}
static void global_root(void) {
    GlobalFixture fixture=global_open(XR_DAP_GLOBAL_ROOT_FIXTURE);XdapAdapter *adapter=fixture.adapter;global_pause(&fixture);
    XdapCurrent *control=adapter->launch->control;XrXirVmDebugSnapshot *old=adapter->snapshot,*first=NULL,*fresh=NULL;
    XrXirVmDebugFrameInfo leaf=global_frame(old,0),caller=global_frame(old,1);CHECK(leaf.thread==1 && caller.thread==1);
    CHECK(!strcmp(adapter->launch->functions[leaf.function].name,"leaf") && !strcmp(adapter->launch->functions[caller.function].name,"inspect"));
    uint32_t library=global_slot(adapter->launch,old,0,"shared"),shared=global_slot(adapter->launch,old,1,"shared");CHECK(library!=shared);
    uint32_t changing=global_slot(adapter->launch,old,1,"changing"),values=global_slot(adapter->launch,old,1,"values");
    uint32_t saved=global_slot(adapter->launch,old,1,"snapshot"),late=global_slot(adapter->launch,old,1,"late");
    global_number(old,0,library,19);global_number(old,1,shared,73);global_number(old,1,changing,11);
    uint32_t title=global_slot(adapter->launch,old,1,"title"),label=global_slot(adapter->launch,old,0,"label");
    global_text(old,1,title,"global\0value",12);global_text(old,0,label,"library\0value",13);
    XrXirValue empty={0};CHECK(xir_vm_debug_snapshot_slot(old,0,changing,&empty)==XR_XIR_VALUE_BAD_ARGUMENT && !empty.type && !empty.payload);
    CHECK(xir_vm_debug_snapshot_slot(old,1,late,&empty)==XR_XIR_VALUE_UNSUPPORTED && !empty.type && !empty.payload);
    XrXirValue replacement={XR_XIR_I64,0,99};uint64_t epoch=leaf.epoch;
    CHECK(xdap_current_write_slot_begin(control,epoch,leaf.id,changing,&global_whole,&replacement)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xdap_current_write_slot_begin(control,epoch+1,caller.id,changing,&global_whole,&replacement)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xdap_current_write_slot_begin(control,epoch,caller.id,shared,&global_whole,&replacement)==XR_XIR_VALUE_BAD_ARGUMENT);
    GlobalWriteTarget target={epoch,1,changing};XrXirValue bad={XR_XIR_BOOL,0,1};
    CHECK(global_write(&fixture,&target,&global_whole,&bad,&fresh)==XR_XIR_VALUE_BAD_ARGUMENT && !fresh);
    CHECK(xdap_current_stopped_epoch(control)==epoch);global_number(old,1,changing,11);
    CHECK(global_write(&fixture,&target,&global_whole,&replacement,&first)==XR_XIR_VALUE_OK && first);
    global_number(first,1,changing,99);global_number(old,1,changing,11);++epoch;
    XrXirValue array=global_array(&fixture,first,1,values,20);XrXirValuePathStep index={XR_XIR_PATH_INDEX,(XrXirType)array.type,1};
    XrXirValuePath path={&index,1};xr_xir_value_drop(&array);target=(GlobalWriteTarget){epoch,1,values};replacement.payload=77;
    CHECK(global_write(&fixture,&target,&path,&replacement,&fresh)==XR_XIR_VALUE_OK && fresh);++epoch;
    array=global_array(&fixture,fresh,1,values,77);xr_xir_value_drop(&array);array=global_array(&fixture,fresh,1,saved,20);xr_xir_value_drop(&array);
    array=global_array(&fixture,old,1,values,20);xr_xir_value_drop(&array);global_number(fresh,1,changing,99);
    XrXirVmDebugFrameInfo after=global_frame(fresh,0),after_caller=global_frame(fresh,1);
    CHECK(after.id==leaf.id && after.instruction==leaf.instruction && after.thread==leaf.thread && after.epoch==epoch);
    CHECK(after_caller.id==caller.id && after_caller.instruction==caller.instruction && after_caller.epoch==epoch);
    XdapCodeFunction *function=&adapter->launch->functions[caller.function];unsigned aliases=0;
    for (uint32_t i=0;i<function->instructions;++i) {
        XrXirInstruction *op=&adapter->launch->instructions[function->first+i];if (op->op!=XR_XIR_SLOT_LOAD || (uint32_t)op->immediate!=values) continue;
        XrXirValue value={0};if (xr_xir_vm_debug_snapshot_cell_value(fresh,1,function->parameters+i,&value)!=XR_XIR_VALUE_OK) continue;
        XrXirValue element={0};XdapValueView view={&value,fixture.domain,fresh,1};XdapValueChildInfo child={0};
        CHECK(xdap_value_child(&view,1,&element,&child)==XR_XIR_VALUE_OK && element.payload==77);++aliases;
        xr_xir_value_drop(&element);xr_xir_value_drop(&value);
    }
    CHECK(aliases);uint32_t nothing=global_slot(adapter->launch,old,1,"nothing");
    target=(GlobalWriteTarget){epoch,1,nothing};XrXirVmDebugSnapshot *unit=NULL;
    CHECK(global_write(&fixture,&target,&global_whole,&empty,&unit)==XR_XIR_VALUE_OK && unit);++epoch;
    CHECK(xir_vm_debug_snapshot_slot(unit,1,nothing,&empty)==XR_XIR_VALUE_OK && !empty.type && !empty.reserved && !empty.payload);
    global_return(&fixture,epoch,"296\n");
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);global_number(first,1,changing,99);global_number(fresh,1,changing,99);
    global_text(fresh,1,title,"global\0value",12);global_text(fresh,0,label,"library\0value",13);
    CHECK(xir_vm_debug_snapshot_slot(unit,1,nothing,&empty)==XR_XIR_VALUE_OK && !empty.type && !empty.payload);
    xr_xir_vm_debug_snapshot_drop(first);xr_xir_vm_debug_snapshot_drop(fresh);xr_xir_vm_debug_snapshot_drop(unit);global_finish(&fixture);
}
static void global_child(void) {
    GlobalFixture fixture=global_open(XR_DAP_GLOBAL_CHILD_FIXTURE);XdapAdapter *adapter=fixture.adapter;global_pause(&fixture);
    XrXirVmDebugSnapshot *snapshot=adapter->snapshot;XrXirVmDebugFrameInfo leaf=global_frame(snapshot,0),caller=global_frame(snapshot,1);
    CHECK(leaf.thread>1 && caller.thread==leaf.thread);uint32_t library=global_slot(adapter->launch,snapshot,0,"shared");
    global_number(snapshot,0,library,19);uint32_t changing=global_slot(adapter->launch,snapshot,1,"changing");
    global_text(snapshot,0,global_slot(adapter->launch,snapshot,0,"label"),"library\0value",13);
    XrXirValue empty={0};CHECK(xir_vm_debug_snapshot_slot(snapshot,1,changing,&empty)==XR_XIR_VALUE_UNSUPPORTED && !empty.type && !empty.payload);
    XrXirValue value={XR_XIR_I64,0,99};
    CHECK(xdap_current_write_slot_begin(adapter->launch->control,leaf.epoch,caller.id,changing,&global_whole,&value)==XR_XIR_VALUE_BAD_ARGUMENT);
    global_return(&fixture,leaf.epoch,"25\n");CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);global_finish(&fixture);
}
static void global_borrow(void) {
    GlobalFixture fixture=global_open(XR_DAP_GLOBAL_BORROW_FIXTURE);XdapAdapter *adapter=fixture.adapter;global_pause_at(&fixture,8);
    XrXirVmDebugSnapshot *snapshot=adapter->snapshot;XrXirVmDebugFrameInfo leaf=global_frame(snapshot,0),caller=global_frame(snapshot,1);
    CHECK(leaf.thread==1 && caller.thread==1);uint32_t slot=global_slot(adapter->launch,snapshot,1,"changing");XrXirValue value={0};
    CHECK(xir_vm_debug_snapshot_slot(snapshot,1,slot,&value)==XR_XIR_VALUE_UNSUPPORTED && !value.type && !value.payload);
    CHECK(xr_xir_vm_debug_snapshot_cell_value(snapshot,0,0,&value)==XR_XIR_VALUE_OK && value.type==XR_XIR_I64 && value.payload==11);
    xr_xir_value_drop(&value);value=(XrXirValue){XR_XIR_I64,0,99};XdapCurrent *control=adapter->launch->control;
    CHECK(xdap_current_write_slot_begin(control,leaf.epoch,caller.id,slot,&global_whole,&value)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xdap_current_write_begin(control,leaf.epoch,leaf.id,0,&global_whole,&value)==XR_XIR_VALUE_OK);
    XdapCurrentResult result={0};CHECK(xdap_current_poll(control,1,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_STOPPED);
    XrXirVmDebugSnapshot *fresh=NULL;bool visited=false;
    CHECK(xdap_current_write_finish(control,&visited,&fresh)==XR_XIR_VALUE_OK && visited && fresh);xdap_current_result_drop(&result);
    value=(XrXirValue){0};CHECK(xir_vm_debug_snapshot_slot(fresh,1,slot,&value)==XR_XIR_VALUE_UNSUPPORTED && !value.type && !value.payload);
    CHECK(xr_xir_vm_debug_snapshot_cell_value(fresh,0,0,&value)==XR_XIR_VALUE_OK && value.payload==99);xr_xir_value_drop(&value);
    global_return(&fixture,leaf.epoch+1,"99\n");CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);
    value=(XrXirValue){0};CHECK(xir_vm_debug_snapshot_slot(fresh,1,slot,&value)==XR_XIR_VALUE_UNSUPPORTED && !value.type && !value.payload);
    xr_xir_vm_debug_snapshot_drop(fresh);global_finish(&fixture);
}
#include "test_global_core_faults.inc.c"
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();CHECK(argc==2);
    if (!strcmp(argv[1],"--root") || !strcmp(argv[1],"--root-resources")) {
        global_root();bool all=!strcmp(argv[1],"--root-resources");global_faults(all);
    } else if (!strcmp(argv[1],"--child")) global_child();else { CHECK(!strcmp(argv[1],"--borrow"));global_borrow(); }
    puts("Genuine caller module slots, scoped writes, SSA/global aliases, old CoW snapshots and worker denial: PASS");return 0;
}
