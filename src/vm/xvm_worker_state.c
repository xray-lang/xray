/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_worker_state.c - VM private worker scratch state implementation
 */

#include "xvm_worker_state.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../coro/xmachine.h"
#include <string.h>

#define XR_VM_MACHINE_STACK_SIZE 1024
#define XR_VM_MACHINE_FRAME_SIZE 64

typedef struct XrVmMachineStorage {
    XrValue stack[XR_VM_MACHINE_STACK_SIZE];
    XrBcCallFrame frames[XR_VM_MACHINE_FRAME_SIZE];
    XrVMContext ctx;
} XrVmMachineStorage;

static void vm_machine_ctx_init(XrVmMachineStorage *storage, XrayIsolate *isolate) {
    XR_DCHECK(storage != NULL, "vm_machine_ctx_init: NULL storage");

    XrVMContext *ctx = &storage->ctx;
    memset(ctx, 0, sizeof(*ctx));
    ctx->stack = storage->stack;
    ctx->stack_top = storage->stack;
    ctx->stack_capacity = XR_VM_MACHINE_STACK_SIZE;
    ctx->frames = storage->frames;
    ctx->frame_count = 0;
    ctx->frame_capacity = XR_VM_MACHINE_FRAME_SIZE;
    ctx->handlers = ctx->handler_inline;
    ctx->handler_count = 0;
    ctx->handler_capacity = XR_HANDLER_INLINE_CAP;
    ctx->module_base_frame = 0;
    ctx->current_coro = NULL;
    ctx->trace_execution = false;
    ctx->isolate = isolate;
}

static void vm_machine_ctx_free(XrVMContext *ctx) {
    if (!ctx)
        return;

    if (ctx->handlers && ctx->handlers != ctx->handler_inline) {
        xr_free(ctx->handlers);
    }
    ctx->handlers = ctx->handler_inline;
    ctx->handler_count = 0;
    ctx->handler_capacity = XR_HANDLER_INLINE_CAP;

    if (ctx->struct_areas) {
        for (int i = 0; i < ctx->struct_areas_cap; i++) {
            if (ctx->struct_areas[i])
                xr_free(ctx->struct_areas[i]);
        }
        xr_free(ctx->struct_areas);
        xr_free(ctx->struct_area_caps);
        ctx->struct_areas = NULL;
        ctx->struct_area_caps = NULL;
        ctx->struct_areas_cap = 0;
    }
    if (ctx->struct_ret_arena) {
        xr_free(ctx->struct_ret_arena);
        ctx->struct_ret_arena = NULL;
        ctx->struct_ret_arena_used = 0;
        ctx->struct_ret_arena_cap = 0;
    }
    xr_vm_ctx_free_ic_tables(ctx);
    if (ctx->defer_stack) {
        xr_free(ctx->defer_stack);
        ctx->defer_stack = NULL;
    }
    if (ctx->defer_frame_marks) {
        xr_free(ctx->defer_frame_marks);
        ctx->defer_frame_marks = NULL;
    }
    ctx->defer_count = 0;
    ctx->defer_capacity = 0;
}

static void vm_machine_storage_destroy(void *ptr) {
    XrVmMachineStorage *storage = (XrVmMachineStorage *) ptr;
    if (!storage)
        return;
    vm_machine_ctx_free(&storage->ctx);
    xr_free(storage);
}

XrVMContext *xr_vm_machine_ctx(struct XrMachine *machine, XrayIsolate *isolate) {
    if (!machine)
        return NULL;

    if (machine->backend_storage) {
        XR_DCHECK(machine->backend_storage_destroy == vm_machine_storage_destroy,
                  "vm_machine_ctx: backend storage is not VM-owned");
        XrVmMachineStorage *storage = (XrVmMachineStorage *) machine->backend_storage;
        storage->ctx.isolate = isolate;
        return &storage->ctx;
    }

    XrVmMachineStorage *storage = (XrVmMachineStorage *) xr_calloc(1, sizeof(XrVmMachineStorage));
    if (!storage)
        return NULL;
    vm_machine_ctx_init(storage, isolate);
    machine->backend_storage = storage;
    machine->backend_storage_destroy = vm_machine_storage_destroy;
    return &storage->ctx;
}

void xr_vm_machine_ctx_set_isolate(struct XrMachine *machine, XrayIsolate *isolate) {
    XrVMContext *ctx = xr_vm_machine_ctx(machine, isolate);
    if (ctx)
        ctx->isolate = isolate;
}
