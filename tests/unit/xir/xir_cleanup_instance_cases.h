/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cleanup_instance_cases.h - Revoked external gates and scoped draining access
 */
#ifndef XIR_CLEANUP_INSTANCE_CASES_H
#define XIR_CLEANUP_INSTANCE_CASES_H
#include "xir/xxir_program.h"
#include "xir/xxir_instance_value.h"
typedef struct DrainState { uint32_t pc; XrXirValue values[2]; int64_t panic; } DrainState;
typedef struct DrainWitness {
    XrXirInstance *instance; XrXirCall *activation; XrXirValue escaped, generated;
    uint32_t bodies, outputs, leases; bool late_gate, malformed;
} DrainWitness;
static XrXirAction drain_initializer(XrXirCallView *view) {
    XrXirValue value = {XR_XIR_I64,0,1};
    CHECK(xr_xir_instance_slot_write(view,0,&value,true) == XR_XIR_CALL_READY);
    return exit_action(XR_XIR_ACTION_RETURN);
}
static XrXirAction drain_owner(XrXirCallView *view) {
    DrainWitness *w = (DrainWitness *)view->environment; DrainState *s = view->state;
    if (view->phase == XR_XIR_CALL_EXIT) {
        CHECK(view->exit.status == XR_XIR_CALL_CANCELLED);
        if (s->pc == 1) { ++s->pc; return (XrXirAction){XR_XIR_ACTION_CALL,2,s->values,w->late_gate ? 1u : 2u,{0},{0},XR_XIR_ACTION_CLEANUP}; }
        CHECK(s->pc == 2 && view->inbox.status == XR_XIR_CALL_RETURNED);
        return exit_action(XR_XIR_ACTION_EXIT_DONE);
    }
    CHECK(!s->pc++);
    XrXirValue value = {XR_XIR_I64,0,16};
    CHECK(xr_xir_instance_cell(view,(XrXirType)257,&value,&s->values[0]) == XR_XIR_CALL_READY);
    if (!w->late_gate) {
        CHECK(xr_xir_instance_function(view,(XrXirType)256,3,s->values,1,&s->values[1]) == XR_XIR_CALL_READY);
        CHECK(xr_xir_value_copy(&s->values[1],&w->escaped) == XR_XIR_VALUE_OK);
    }
    return exit_action(XR_XIR_ACTION_SUSPEND);
}
static XrXirAction drain_body(XrXirCallView *view) {
    DrainWitness *w = (DrainWitness *)view->environment; uint32_t *pc = view->state;
    w->activation = view->activation;
    CHECK(xr_xir_instance_state(w->instance) == XR_XIR_INSTANCE_DRAINING);
    CHECK(xr_xir_instance_free(w->instance) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_stop(w->instance) == XR_XIR_CALL_READY);
    if (!(*pc)++) {
        ++w->bodies;
        XrXirValue value = {0}; XrXirCallView copied = *view;
        CHECK(xr_xir_instance_cell_read(&copied,&view->arguments[0],&value) == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_cell_read(view,&view->arguments[0],&value) == XR_XIR_CALL_READY && value.payload == 16);
        value.payload = 17;
        CHECK(xr_xir_instance_cell_write(view,&view->arguments[0],&value) == XR_XIR_CALL_READY);
        XrXirValue cell = {0};
        CHECK(xr_xir_instance_cell(view,(XrXirType)257,&value,&cell) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_function(view,(XrXirType)256,3,&cell,1,&w->generated) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start_function(w->instance,&w->generated,NULL,0) == XR_XIR_CALL_BAD_STATE);
        xr_xir_value_drop(&cell);
        const XrXirValue *function = w->late_gate ? &w->generated : &view->arguments[1];
        uint32_t entry = UINT32_MAX;
        CHECK(xr_xir_instance_resolve_function(view,function,&entry) == XR_XIR_CALL_READY && entry == 3);
        return (XrXirAction){XR_XIR_ACTION_CALL,entry,NULL,0,*function,{0},0};
    }
    if (*pc == 2) {
        CHECK(view->inbox.status == XR_XIR_CALL_RETURNED && view->inbox.value.payload == 21);
        XrXirValue value = {XR_XIR_I64,0,42};
        CHECK(xr_xir_instance_slot_write(view,0,&value,false) == XR_XIR_CALL_READY);
        value = (XrXirValue){0};
        CHECK(xr_xir_instance_slot_read(view,0,&value) == XR_XIR_CALL_READY && value.payload == 42);
        return (XrXirAction){XR_XIR_ACTION_OUTPUT,XR_XIR_STDOUT,&view->inbox.value,1,{0},{0},0};
    }
    if (w->malformed) return (XrXirAction){XR_XIR_ACTION_CONTINUE,0,NULL,0,{XR_XIR_I64,0,1},{0},0};
    return exit_action(XR_XIR_ACTION_RETURN);
}
static XrXirAction drain_callee(XrXirCallView *view) {
    DrainState *s = view->state;
    CHECK(xr_xir_call_cleanup_active(view->activation));
    if (!s->pc) {
        XrXirValue value = {0};
        CHECK(xr_xir_instance_cell_read(view,&view->arguments[0],&value) == XR_XIR_CALL_READY && value.payload == 17);
        value.payload = 21;
        CHECK(xr_xir_instance_cell_write(view,&view->arguments[0],&value) == XR_XIR_CALL_READY);
        return xr_xir_instance_panic_land(view,s,xr_xir_call_numeric_fault(XR_XIR_RUN_DIVIDE_BY_ZERO,false),
            (uint32_t)offsetof(DrainState,panic),1,&s->pc);
    }
    XrXirValue info = {XR_XIR_PANIC_INFO,0,s->panic};
    CHECK(xr_xir_value_argument(&info,NULL,XR_XIR_PANIC_INFO));
    return (XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,{XR_XIR_I64,0,21},{0},0};
}
static void drain_release(XrXirCallView *view, XrXirCallStatus reason) {
    (void)reason; DrainState *s = view->state;
    CHECK(!xr_xir_call_cleanup_active(view->activation));
    xr_xir_value_drop(&s->values[0]); xr_xir_value_drop(&s->values[1]);
    xr_xir_owned_slot_clear(s,(uint32_t)offsetof(DrainState,panic));
}
static bool drain_output(void *context, const XrXirOutputGroup *group) {
    DrainWitness *w = context;
    CHECK(!xr_xir_call_cleanup_active(w->activation));
    CHECK(xr_xir_instance_start_function(w->instance,&w->generated,NULL,0) == XR_XIR_CALL_BAD_STATE);
    CHECK(group->count == 1 && group->values[0].type == XR_XIR_I64 && group->values[0].payload == 21);
    CHECK(xr_xir_instance_stop(w->instance) == XR_XIR_CALL_READY);
    ++w->outputs; return true;
}
static void drain_lease(void *context) { ++((DrainWitness *)context)->leases; }
static XrXirArtifact *drain_proof(const XrXirDeclarations *declarations, const XrXirTypes *types,
    const XrXirType *parameters, bool late_gate) {
    XrXirInstruction unit = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirInstruction value[] = {{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirBlock unit_block = {0,1,0, 0}, value_block = {0,2,0, 0};
    XrXirInstruction callee_value[] = {value[0],value[1]}; callee_value[1].args[0] = 1;
    XrXirFunction functions[] = {
        {"init",4,NULL,0,XR_XIR_UNIT,&unit_block,1,&unit,1,NULL,0},
        {"main",4,NULL,0,XR_XIR_I64,&value_block,1,value,2,NULL,0},
        {"cleanup",7,parameters,late_gate ? 1u : 2u,XR_XIR_UNIT,&unit_block,1,&unit,1,NULL,0},
        {"callee",6,parameters,1,XR_XIR_I64,&value_block,1,callee_value,2,NULL,0}};
    XrXirModule built = {XR_XIR_BUILT,functions,4,declarations,NULL,types,NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_check(&built,NULL,&checked,NULL) == XR_XIR_OK);
    XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(checked,&target,NULL,&lowered,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked); return lowered;
}
static void cleanup_instance_case(bool late_gate, bool malformed) {
    DrainWitness w = {0}; w.late_gate = late_gate; w.malformed = malformed;
    XrXirTypeNode nodes[2] = {0}; nodes[0].kind = XR_XIR_TYPE_CALLABLE; nodes[0].result = XR_XIR_I64;
    nodes[1].kind = XR_XIR_TYPE_CELL; nodes[1].element = XR_XIR_I64;
    XrXirTypes types = {nodes,2,NULL, NULL};
    XrXirType parameters[] = {(XrXirType)257,(XrXirType)256};
    XrXirCallEntry entries[] = {
        {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_UNIT,0,drain_initializer,NULL,&w,0,0},
        {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_I64,sizeof(DrainState),drain_owner,drain_release,&w,XR_XIR_ENTRY_EXIT,0},
        {XR_XIR_CALL_ABI_VERSION,parameters,late_gate ? 1u : 2u,XR_XIR_UNIT,sizeof(uint32_t),drain_body,NULL,&w,0,2},
        {XR_XIR_CALL_ABI_VERSION,parameters,1,XR_XIR_I64,sizeof(DrainState),drain_callee,drain_release,&w,0,0}};
    XrXirFunctionIdentity ids[] = {{0},{0,1,0,0,0, 0, XR_XIR_NON_MEMBER},{0,0,0,0,2, 0, XR_XIR_NON_MEMBER},{0}};
    XrXirSourceModule module = {"main",4,NULL,0,0}; XrXirSlot slot = {0,XR_XIR_I64,1};
    XrXirDeclarations declarations = {&module,1,ids,&slot,1,NULL,0,0,1, NULL};
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        entries,4,&declarations,{&w,drain_lease},&types,{0}};
    XrXirArtifact *proof = drain_proof(&declarations,&types,parameters,late_gate);
    spec.proof = xr_xir_program_proof(proof);
    XrXirProgram *program = NULL;
    spec.abi_version = 18;
    CHECK(xr_xir_program_seal(&spec,(XrXirProgramBudget){16u<<20,1000000},&program) == XR_XIR_BAD_LAYOUT && !program);
    spec.abi_version = XR_XIR_PROGRAM_ABI_VERSION;
    entries[1].abi_version = 18;
    CHECK(xr_xir_program_seal(&spec,(XrXirProgramBudget){16u<<20,1000000},&program) == XR_XIR_BAD_LAYOUT && !program);
    entries[1].abi_version = XR_XIR_CALL_ABI_VERSION;
    entries[3].flags = XR_XIR_ENTRY_EXIT;
    CHECK(xr_xir_program_seal(&spec,(XrXirProgramBudget){16u<<20,1000000},&program) == XR_XIR_BAD_STRUCTURE && !program);
    entries[3].flags = 0; ids[2].cleanup_owner = entries[2].cleanup_owner = 1; entries[0].flags = XR_XIR_ENTRY_EXIT;
    CHECK(xr_xir_program_seal(&spec,(XrXirProgramBudget){16u<<20,1000000},&program) == XR_XIR_BAD_STRUCTURE && !program);
    ids[2].cleanup_owner = entries[2].cleanup_owner = 2; entries[0].flags = 0;
    CHECK(!w.leases);
    CHECK(xr_xir_program_seal(&spec,(XrXirProgramBudget){16u<<20,1000000},&program) == XR_XIR_OK);
    xr_xir_artifact_free(proof);
    XrXirInstanceConfig config = xr_xir_instance_defaults(); config.output = (XrXirOutputProvider){drain_output,&w};
    CHECK(xr_xir_instance_new(program,&config,&w.instance) == XR_XIR_CALL_READY);
    xr_xir_program_drop(program);
    CHECK(xr_xir_instance_start(w.instance,1,NULL,0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(w.instance).outcome.status == XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_instance_stop(w.instance) == (malformed ? XR_XIR_CALL_BAD_STATE : XR_XIR_CALL_READY));
    CHECK(w.bodies == 1 && w.outputs == 1);
    CHECK(xr_xir_instance_poll(w.instance).outcome.status == (malformed ? XR_XIR_CALL_BAD_STATE : XR_XIR_CALL_CANCELLED));
    CHECK(xr_xir_instance_start_function(w.instance,late_gate ? &w.generated : &w.escaped,NULL,0) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_stop(w.instance) == XR_XIR_CALL_READY && w.bodies == 1);
    CHECK(xr_xir_instance_free(w.instance) == XR_XIR_CALL_READY && !w.leases);
    xr_xir_value_drop(&w.escaped); CHECK(!w.leases); xr_xir_value_drop(&w.generated); CHECK(w.leases == 1 && !live);
    puts("Draining cell, function admission, PanicInfo, output and revoked gate passed");
}
static void cleanup_instance_cases(void) {
    cleanup_instance_case(false,false); cleanup_instance_case(true,false); cleanup_instance_case(false,true);
}
#endif // XIR_CLEANUP_INSTANCE_CASES_H
