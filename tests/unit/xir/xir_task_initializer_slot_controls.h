/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_task_initializer_slot_controls.h - Current worker publication boundaries
 *
 * KEY CONCEPT:
 *   Faulted private views keep the genuine sealed Program and active worker.
 *   Each refusal restores the view before scheduling or ownership can change.
 */
#ifndef XIR_TASK_INITIALIZER_SLOT_CONTROLS_H
#define XIR_TASK_INITIALIZER_SLOT_CONTROLS_H
#define xr_xir_instance_slot_read initializer_slot_read_actual
#include "xir_runtime_allocations.h"
#undef xr_xir_instance_slot_read
static unsigned initializer_slot_controls;
static void initializer_slot_refused(XrXirCallView *view,uint32_t slot) {
    XrXirValue value={0};
    CHECK(initializer_slot_read_actual(view,slot,&value)==XR_XIR_CALL_BAD_STATE);
    CHECK(!value.type&&!value.payload);
    ++initializer_slot_controls;
}
XR_FUNC XrXirCallStatus xr_xir_instance_slot_read(XrXirCallView *view,uint32_t slot,XrXirValue *output) {
    XrXirInstance *instance=view_instance(view);
    if(!initializer_slot_controls&&instance&&view_is_child(instance,view)&&
        instance->state==XR_XIR_INSTANCE_INITIALIZING&&
        slot<instance->program->declarations->slot_count&&worker_slot_read(instance,slot)) {
        size_t blocks=runtime_live,bytes=runtime_bytes;
        XrXirProgram *program=instance->program;
        const XrXirDeclarations *original=program->declarations;
        uint32_t module=original->slots[slot].module,current=instance->current_module;
        CHECK(instance->published[slot]&&!instance->ready[module]&&module==current);
        instance->published[slot]=0;
        initializer_slot_refused(view,slot);
        instance->published[slot]=1;

        XrXirSlot *slots=xr_calloc(original->slot_count,sizeof(*slots));CHECK(slots);
        memcpy(slots,original->slots,(size_t)original->slot_count*sizeof(*slots));
        XrXirDeclarations changed=*original;changed.slots=slots;
        slots[slot].mutable=true;program->declarations=&changed;
        CHECK(instance_view_current(view));
        initializer_slot_refused(view,slot);
        program->declarations=original;xr_free(slots);

        instance->current_module=UINT32_MAX;
        initializer_slot_refused(view,slot);
        instance->current_module=current;

        uint8_t *ready=xr_calloc(original->module_count,sizeof(*ready));CHECK(ready);
        InstanceStorageRoute *routes=xr_calloc(original->slot_count,sizeof(*routes));CHECK(routes);
        XrXirInstance birth=*instance;birth.ready=ready;
        InstanceStorageRoute *original_routes=instance->slot_origins;
        if(original_routes)memcpy(routes,original_routes,(size_t)original->slot_count*sizeof(*routes));
        routes[slot]=(InstanceStorageRoute){&birth,slot};
        instance->slot_origins=routes;instance->ready[module]=1;
        initializer_slot_refused(view,slot);
        instance->ready[module]=0;instance->slot_origins=original_routes;
        xr_free(routes);xr_free(ready);
        CHECK(initializer_slot_controls==4&&worker_slot_read(instance,slot));
        CHECK(runtime_live==blocks&&runtime_bytes==bytes);
    }
    return initializer_slot_read_actual(view,slot,output);
}
#endif // XIR_TASK_INITIALIZER_SLOT_CONTROLS_H
