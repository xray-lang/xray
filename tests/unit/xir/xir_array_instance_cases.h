/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_instance_cases.h - Array access authority and cumulative work
 *
 * KEY CONCEPT:
 *   Native callbacks obey root permissions and cannot replenish work by invoking
 *   another synchronous helper or suspending the current activation.
 */
#ifndef XIR_ARRAY_INSTANCE_CASES_H
#define XIR_ARRAY_INSTANCE_CASES_H
#include "xir_native_metadata_fixture.h"
#include "xir/xxir_instance_value.h"

typedef struct ArrayAccessWitness { uint32_t mode, reads, cleanups; } ArrayAccessWitness;
typedef struct ArrayAccessFrame { XrXirValue value, cell; uint32_t phase; } ArrayAccessFrame;
static XrXirValueReceiver array_slot_receiver(uint32_t slot) {
    return (XrXirValueReceiver) {XR_XIR_ROOT_SLOT, (XrXirType) 256, {0}, NULL, slot};
}
static void array_access_cleanup(XrXirCallView *view, XrXirCallStatus reason) {
    (void) reason;
    ArrayAccessWitness *witness = (ArrayAccessWitness *) view->environment;
    ArrayAccessFrame *frame = view->state;
    CHECK(xr_xir_call_admission(view) == NULL);
    xr_xir_value_drop(&frame->value); xr_xir_value_drop(&frame->cell);
    ++witness->cleanups;
}
static XrXirAction array_access_initializer(XrXirCallView *view) {
    ArrayAccessWitness *witness = (ArrayAccessWitness *) view->environment;
    if (witness->mode) return done();
    ArrayAccessFrame *frame = view->state;
    XrXirValueReceiver unpublished = array_slot_receiver(0);
    XrXirValue output = {0}; XrXirFaultDetail detail = {0};
    CHECK(xr_xir_instance_array_read(view,&unpublished,0,false,&output,&detail) == XR_XIR_CALL_BAD_STATE);
    XrXirValue element = {XR_XIR_I64,0,7};
    CHECK(xr_xir_instance_array_new(view,(XrXirType)256,&element,1,&frame->value) == XR_XIR_CALL_READY);
    if (xr_xir_call_current_entry(view->activation) == 2) {
        CHECK(xr_xir_instance_slot_write(view,2,&frame->value,true) == XR_XIR_CALL_READY);
    } else {
        CHECK(xr_xir_instance_slot_write(view,0,&frame->value,true) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_slot_write(view,1,&frame->value,true) == XR_XIR_CALL_READY);
    }
    return done();
}
static void array_access_read(XrXirCallView *view, XrXirValueReceiver receiver, int64_t expected) {
    XrXirValue output = {0}; XrXirFaultDetail detail = {0};
    CHECK(xr_xir_instance_array_read(view,&receiver,0,false,&output,&detail) == XR_XIR_CALL_READY);
    CHECK(output.type == XR_XIR_I64 && output.payload == expected && xr_xir_fault_empty(detail));
}
static void array_path_permissions(XrXirCallView *view) {
    ArrayAccessFrame *frame = view->state;
    XrXirValuePathStep steps[] = {{XR_XIR_PATH_INDEX,(XrXirType)258,0},
        {XR_XIR_PATH_INDEX,(XrXirType)256,0}};
    XrXirValuePath path = {steps + 1,1}, root_path = {NULL,0};
    XrXirValueReceiver root = array_slot_receiver(0), receiver = array_slot_receiver(1);
    XrXirValue output = {0}, element = {XR_XIR_I64,0,17}; XrXirFaultDetail detail = {0};
    XrXirCallView copied = *view;
    CHECK(xr_xir_instance_path_read(&copied,&root,&path,&output,&detail) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_path_write(&copied,&root,&path,&element,&detail) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_path_write(view,&receiver,&path,&element,&detail) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_path_push(view,&receiver,&root_path,&element,&detail) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_path_read(view,&receiver,&path,&output,&detail) == XR_XIR_CALL_READY);
    CHECK(output.type == XR_XIR_I64 && output.payload == 7); output = (XrXirValue){0};
    receiver.slot = 2;
    CHECK(xr_xir_instance_path_read(view,&receiver,&path,&output,&detail) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_path_write(view,&receiver,&path,&element,&detail) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_path_write(view,&root,&path,&element,&detail) == XR_XIR_CALL_READY);
    array_access_read(view,root,17); array_access_read(view,array_slot_receiver(1),7);
    receiver = (XrXirValueReceiver){XR_XIR_ROOT_VALUE,(XrXirType)256,frame->value,NULL,0};
    CHECK(xr_xir_instance_path_read(view,&receiver,&path,&output,&detail) == XR_XIR_CALL_BAD_STATE);
    receiver = (XrXirValueReceiver){XR_XIR_ROOT_LOCAL,(XrXirType)256,{0},
        (char *)view->state + sizeof(*frame) - 7,0};
    CHECK(xr_xir_instance_path_write(view,&receiver,&path,&element,&detail) == XR_XIR_CALL_BAD_STATE);
    receiver.local_payload = &frame->value.payload; element.payload = 31;
    CHECK(xr_xir_instance_path_write(view,&receiver,&path,&element,&detail) == XR_XIR_CALL_READY);
    array_access_read(view,receiver,31); array_access_read(view,root,17);
    receiver = (XrXirValueReceiver){XR_XIR_ROOT_CELL,(XrXirType)256,frame->cell,NULL,0};
    element.payload = 41;
    CHECK(xr_xir_instance_path_write(view,&receiver,&path,&element,&detail) == XR_XIR_CALL_READY);
    array_access_read(view,receiver,41); array_access_read(view,root,17);
    XrXirValue outer = {0};
    CHECK(xr_xir_instance_array_new(view,(XrXirType)258,&frame->value,1,&outer) == XR_XIR_CALL_READY);
    xr_xir_value_drop(&frame->value); frame->value = outer;
    receiver = (XrXirValueReceiver){XR_XIR_ROOT_LOCAL,(XrXirType)258,{0},&frame->value.payload,0};
    path = (XrXirValuePath){steps,2}; element.payload = 55;
    CHECK(xr_xir_instance_path_write(view,&receiver,&path,&element,&detail) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_path_read(view,&receiver,&path,&output,&detail) == XR_XIR_CALL_READY);
    CHECK(output.type == XR_XIR_I64 && output.payload == 55); output = (XrXirValue){0};
    path.count = 1; element.payload = 61;
    CHECK(xr_xir_instance_path_push(view,&receiver,&path,&element,&detail) == XR_XIR_CALL_READY);
    path.count = 2; steps[1].selector = 1;
    CHECK(xr_xir_instance_path_read(view,&receiver,&path,&output,&detail) == XR_XIR_CALL_READY);
    CHECK(output.type == XR_XIR_I64 && output.payload == 61); output = (XrXirValue){0};
    steps[0].selector = -1;
    CHECK(xr_xir_instance_path_write(view,&receiver,&path,&element,&detail) == XR_XIR_CALL_BOUNDS);
    CHECK(detail.code == 430 && detail.index == -1 && detail.length == 1);
}
static XrXirAction array_access_permissions(XrXirCallView *view) {
    ArrayAccessFrame *frame = view->state;
    if (!frame->phase++) return (XrXirAction) {XR_XIR_ACTION_CALL,3,NULL,0,{0},{0}, 0};
    XrXirFaultDetail detail = {0}; XrXirValue output = {0}, element = {XR_XIR_I64,0,9};
    XrXirValueReceiver root = array_slot_receiver(0), receiver = array_slot_receiver(1);
    CHECK(xr_xir_call_admission(view) != NULL);
    XrXirCallView copied = *view;
    CHECK(xr_xir_call_admission(&copied) == NULL);
    CHECK(xr_xir_instance_array_read(&copied,&root,0,false,&output,&detail) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_array_write(view,&receiver,0,&element,false,&detail) == XR_XIR_CALL_BAD_STATE);
    receiver.slot = 2;
    CHECK(xr_xir_instance_array_read(view,&receiver,0,false,&output,&detail) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_array_write(view,&receiver,0,&element,false,&detail) == XR_XIR_CALL_BAD_STATE);
    receiver.slot = 3;
    CHECK(xr_xir_instance_array_read(view,&receiver,0,false,&output,&detail) == XR_XIR_CALL_BAD_STATE);
    receiver = root; receiver.type = XR_XIR_I64;
    CHECK(xr_xir_instance_array_write(view,&receiver,0,&element,false,&detail) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_slot_read(view,0,&frame->value) == XR_XIR_CALL_READY);
    receiver = (XrXirValueReceiver) {XR_XIR_ROOT_VALUE,(XrXirType)256,frame->value,NULL,0};
    CHECK(xr_xir_instance_array_write(view,&receiver,0,&element,false,&detail) == XR_XIR_CALL_BAD_STATE);
    receiver = (XrXirValueReceiver) {XR_XIR_ROOT_LOCAL,(XrXirType)256,{0},
        (char *)view->state + sizeof(*frame) - 7,0};
    CHECK(xr_xir_instance_array_write(view,&receiver,0,&element,false,&detail) == XR_XIR_CALL_BAD_STATE);
    receiver.local_payload = &frame->value.payload; receiver.slot = 1;
    CHECK(xr_xir_instance_array_write(view,&receiver,0,&element,false,&detail) == XR_XIR_CALL_BAD_STATE);
    receiver.slot = 0;
    CHECK(xr_xir_instance_array_write(view,&receiver,0,&element,false,&detail) == XR_XIR_CALL_READY);
    array_access_read(view,receiver,9); array_access_read(view,root,7);
    CHECK(xr_xir_instance_cell(view,(XrXirType)257,&frame->value,&frame->cell) == XR_XIR_CALL_READY);
    receiver = (XrXirValueReceiver) {XR_XIR_ROOT_CELL,(XrXirType)256,frame->cell,NULL,0};
    element.payload = 11;
    CHECK(xr_xir_instance_array_write(view,&receiver,0,&element,false,&detail) == XR_XIR_CALL_READY);
    array_access_read(view,receiver,11); array_access_read(view,root,7);
    receiver.slot = 1;
    CHECK(xr_xir_instance_array_write(view,&receiver,0,&element,false,&detail) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_fault_empty(detail));
    array_path_permissions(view);
    return action(XR_XIR_ACTION_RETURN,(XrXirValue) {XR_XIR_I64,0,0});
}
static XrXirAction array_access_library(XrXirCallView *view) {
    XrXirValueReceiver receiver = array_slot_receiver(2);
    array_access_read(view,receiver,7);
    XrXirValue output = {0}, element = {XR_XIR_I64,0,9}; XrXirFaultDetail detail = {0};
    CHECK(xr_xir_instance_array_write(view,&receiver,0,&element,false,&detail) == XR_XIR_CALL_BAD_STATE);
    receiver.slot = 0;
    CHECK(xr_xir_instance_array_read(view,&receiver,0,false,&output,&detail) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_array_write(view,&receiver,0,&element,false,&detail) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_fault_empty(detail));
    return done();
}
static XrXirAction array_access_budget(XrXirCallView *view) {
    ArrayAccessWitness *witness = (ArrayAccessWitness *) view->environment;
    ArrayAccessFrame *frame = view->state;
    if (!frame->phase) {
        CHECK(xr_xir_instance_array_new(view,(XrXirType)256,NULL,0,&frame->value) == XR_XIR_CALL_READY);
        frame->phase = 1;
    }
    XrXirValueReceiver receiver = {XR_XIR_ROOT_VALUE,(XrXirType)256,frame->value,NULL,0};
    bool path_mode = witness->mode >= 3;
    XrXirValuePath root_path = {NULL,0};
    if (path_mode) receiver = (XrXirValueReceiver){XR_XIR_ROOT_LOCAL,(XrXirType)256,{0},&frame->value.payload,0};
    for (uint32_t attempt = 0; attempt < 65; ++attempt) {
        XrXirValue output = {0}; XrXirFaultDetail detail = {0};
        uint64_t before = xr_xir_call_admission(view)->work;
        XrXirCallStatus status = path_mode ? xr_xir_instance_path_read(view,&receiver,&root_path,&output,&detail) :
            xr_xir_instance_array_read(view,&receiver,0,true,&output,&detail);
        CHECK(xr_xir_fault_empty(detail));
        if (status == XR_XIR_CALL_LIMIT) {
            CHECK(output.type == XR_XIR_UNIT && !output.payload);
            CHECK(path_mode ? witness->reads > 8 && witness->reads < 32 : witness->reads > 32 && witness->reads < 64);
            CHECK(!xr_xir_call_admission(view)->work);
            return (XrXirAction) {XR_XIR_ACTION_FAULT,0,NULL,0,{XR_XIR_I64,0,XR_XIR_CALL_LIMIT},{0}, 0};
        }
        CHECK(status == XR_XIR_CALL_READY);
        CHECK(before - xr_xir_call_admission(view)->work == (path_mode ? 3u : 1u));
        CHECK(path_mode ? output.type == 256 : output.type == XR_XIR_I64 && !output.payload);
        xr_xir_value_drop(&output);
        ++witness->reads;
        if ((witness->mode == 2 || witness->mode == 4) && witness->reads == (path_mode ? 8u : 32u))
            return (XrXirAction) {XR_XIR_ACTION_SUSPEND,0,NULL,0,{0},{0}, 0};
    }
    CHECK(false);
    return done();
}
static void array_instance_cases(void) {
    uint32_t completed[4] = {0};
    for (uint32_t mode = 0; mode < 5; ++mode) {
        ArrayAccessWitness witness = {mode,0,0};
        XrXirTypeNode nodes[] = {
            {XR_XIR_TYPE_ARRAY,XR_XIR_I64,NULL,0,XR_XIR_UNIT,0,0, {0}},
            {XR_XIR_TYPE_CELL,(XrXirType)256,NULL,0,XR_XIR_UNIT,0,0, {0}},
            {XR_XIR_TYPE_ARRAY,(XrXirType)256,NULL,0,XR_XIR_UNIT,0,0, {0}}};
        XrXirTypes types = {nodes,3, NULL, NULL};
        uint32_t dependency = 1;
        XrXirSourceModule modules[] = {{"array",5,mode ? NULL : &dependency,mode ? 0 : 1,0},
            {"library",7,NULL,0,2}};
        XrXirFunctionIdentity identities[] = {{0,0, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0},{0,1, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0},{1,0, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0},{1,1, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0}};
        XrXirSlot slots[] = {{0,(XrXirType)256,1},{0,(XrXirType)256,0},{1,(XrXirType)256,0}};
        XrXirDeclarations declarations = {modules,mode ? 1 : 2,identities,slots,mode ? 0 : 3,NULL,0,0,1, NULL};
        XrXirCallEntry entries[] = {
            {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_UNIT,sizeof(ArrayAccessFrame),array_access_initializer,array_access_cleanup,&witness, 0, 0},
            {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_I64,sizeof(ArrayAccessFrame),
                mode ? array_access_budget : array_access_permissions,array_access_cleanup,&witness, 0, 0},
            {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_UNIT,sizeof(ArrayAccessFrame),array_access_initializer,array_access_cleanup,&witness, 0, 0},
            {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_UNIT,sizeof(ArrayAccessFrame),array_access_library,array_access_cleanup,&witness, 0, 0}};
        XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
            entries,mode ? 2 : 4,&declarations,{0},&types,{0}};
        XrXirArtifact *proof = NULL;
        CHECK(native_metadata_fixture(&spec, &proof) == XR_XIR_OK);
        spec.proof = xr_xir_program_proof(proof);
        XrXirProgram *program = NULL; XrXirInstance *instance = NULL;
        CHECK(xr_xir_program_seal(&spec,(XrXirProgramBudget) {2097152, 16000000},&program) == XR_XIR_OK);
        xr_xir_artifact_free(proof);
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        if (mode) config.poll_limit = 64;
        CHECK(xr_xir_instance_new(program,&config,&instance) == XR_XIR_CALL_READY);
        xr_xir_program_drop(program);
        CHECK(xr_xir_instance_start(instance,1,NULL,0) == XR_XIR_CALL_READY);
        XrXirInstanceResult result = xr_xir_instance_poll_bounded(instance, UINT64_MAX);
        if (mode == 2 || mode == 4) {
            CHECK(result.outcome.status == XR_XIR_CALL_SUSPENDED && witness.reads == (mode == 4 ? 8u : 32u));
            CHECK(xr_xir_instance_resume(instance,result.epoch,result.outcome.wake) == XR_XIR_CALL_READY);
            result = xr_xir_instance_poll_bounded(instance, UINT64_MAX);
        }
        CHECK(result.outcome.status == (mode ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_RETURNED));
        CHECK(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY && witness.cleanups == (mode ? 2u : 4u));
        if (mode) completed[mode-1] = witness.reads;
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    }
    CHECK(completed[0] == completed[1] && completed[2] == completed[3]);
}
#endif // XIR_ARRAY_INSTANCE_CASES_H
