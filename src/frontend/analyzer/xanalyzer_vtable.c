/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_vtable.c - Stage complete inheritance tables before publication
 */
#include "xanalyzer_vtable.h"
#include "xanalyzer_symbol.h"
#include <limits.h>

typedef struct VTableStage {
    XaMethodSlot *slots;
    int count;
} VTableStage;
typedef struct VTableOverride {
    size_t owner;
    int index;
    int32_t symbol_id;
    size_t ancestors;
} VTableOverride;

static XrCompileResourceStatus invalid(XrCompileState *state) {
    return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
}

static void free_slots(XaMethodSlot *slots, int count) {
    for (int i = 0; i < count; ++i)
        xr_compile_state_free((void *) slots[i].name);
    xr_compile_state_free(slots);
}

XR_FUNC void xa_class_vtable_free_owned(XrClassInfo *info) {
    if (!info) return;
    XaMethodSlot *slots = info->vtable;int count = info->vtable_size;
    info->vtable = NULL;info->vtable_size = 0;
    free_slots(slots, slots ? count : 0);
}

static XrCompileResourceStatus validate_inputs(XrCompileState *state,
    const XaVTableClassInput *inputs, size_t count, size_t *methods) {
    if (!inputs || count > INT_MAX)
        return count > INT_MAX ? xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET) : invalid(state);
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        if (xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK) return xr_compile_state_status(state);
        const XrClassInfo *info = inputs[i].info;
        if (!info || info->vtable || info->vtable_size || info->method_count < 0 ||
            (info->method_count && (!info->methods || !inputs[i].method_ids)) ||
            (i && info->base != inputs[i - 1].info)) return invalid(state);
        if ((size_t) info->method_count > SIZE_MAX - total)
            return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET);
        total += (size_t) info->method_count;
    }
    XrClassInfo *base = inputs[0].info->base;
    if (base && (!base->vtable || base->vtable_size < 0)) return invalid(state);
    XrClassInfo *slow = base, *fast = base;
    while (fast && fast->base) {
        if (xr_compile_state_work(state, 2) != XR_COMPILE_RESOURCE_OK) return xr_compile_state_status(state);
        slow = slow->base;fast = fast->base->base;
        if (slow == fast) return invalid(state);
    }
    *methods = total;
    return XR_COMPILE_RESOURCE_OK;
}

static bool equal_name(XrCompileState *state, const char *left, const char *right) {
    if (!left || !right) { (void) invalid(state);return false; }
    for (;;) {
        if (xr_compile_state_work(state, 2) != XR_COMPILE_RESOURCE_OK) return false;
        unsigned char a = (unsigned char) *left++, b = (unsigned char) *right++;
        if (a != b) { (void) invalid(state);return false; }
        if (!a) return true;
    }
}

static XrCompileResourceStatus append_slot(XrCompileState *state, VTableStage *stage,
    XaMethodSlot source) {
    if (!source.name || !source.symbol || source.symbol_id <= 0) return invalid(state);
    char *name = NULL;
    XrCompileResourceStatus status = xr_compile_state_strdup(state, source.name, &name);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    source.name = name;source.vtable_index = stage->count;
    status = xr_compile_state_copy(state, &stage->slots[stage->count], &source, sizeof(source));
    if (status == XR_COMPILE_RESOURCE_OK) ++stage->count;
    else xr_compile_state_free(name);
    return status;
}

static XrCompileResourceStatus prepare_stage(XrCompileState *state,
    const XaVTableClassInput *input, const VTableStage *base, VTableStage *stage,
    VTableOverride *overrides, size_t *override_count) {
    const XrClassInfo *info = input->info;
    if (base->count < 0 || info->method_count > INT_MAX - base->count)
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET);
    int capacity = base->count + info->method_count;
    if (capacity) {
        void *memory = NULL;
        XrCompileResourceStatus status = xr_compile_state_calloc(state, (size_t) capacity, sizeof(XaMethodSlot), &memory);
        if (status != XR_COMPILE_RESOURCE_OK) return status;
        stage->slots = memory;
    }
    for (int i = 0; i < base->count; ++i) {
        if (xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK) return xr_compile_state_status(state);
        XaMethodSlot source = base->slots[i];source.is_final = true;
        if (source.vtable_index != i) return invalid(state);
        XrCompileResourceStatus status = append_slot(state, stage, source);
        if (status != XR_COMPILE_RESOURCE_OK) return status;
    }
    for (int i = 0; i < info->method_count; ++i) {
        if (xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK) return xr_compile_state_status(state);
        XaSymbol *method = info->methods[i];int32_t id = input->method_ids[i];
        if (!method || !method->name) { if (id) return invalid(state);continue; }
        if (id <= 0) return invalid(state);
        int index = 0;
        for (; index < stage->count; ++index) {
            if (xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK) return xr_compile_state_status(state);
            if (stage->slots[index].symbol_id == id) break;
        }
        if (index == stage->count) {
            XaMethodSlot slot = {method->name, id, method, false, true, index};
            XrCompileResourceStatus status = append_slot(state, stage, slot);
            if (status != XR_COMPILE_RESOURCE_OK) return status;
        } else {
            if (!equal_name(state, stage->slots[index].name, method->name)) return xr_compile_state_status(state);
            XaMethodSlot slot = stage->slots[index];slot.symbol = method;
            slot.is_overridden = false;slot.is_final = true;
            XrCompileResourceStatus status = xr_compile_state_copy(state, &stage->slots[index], &slot, sizeof(slot));
            if (status != XR_COMPILE_RESOURCE_OK) return status;
            if (index < base->count) {
                VTableOverride fact = {0, index, id, 0};
                status = xr_compile_state_copy(state, &overrides[*override_count], &fact, sizeof(fact));
                if (status != XR_COMPILE_RESOURCE_OK) return status;
                ++*override_count;
            }
        }
    }
    return XR_COMPILE_RESOURCE_OK;
}

static XrCompileResourceStatus reserve_overrides(XrCompileState *state,
    const XaVTableClassInput *inputs, VTableStage *stages, VTableOverride *facts, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK) return xr_compile_state_status(state);
        VTableOverride *fact = &facts[i];size_t index = fact->owner;
        XrClassInfo *ancestor = inputs[index].info->base;
        while (ancestor) {
            /* This admits both the inspection and the later infallible flag
             * writes. Arrays and class links cannot change until commit. */
            if (xr_compile_state_work(state, 4) != XR_COMPILE_RESOURCE_OK) return xr_compile_state_status(state);
            VTableStage table = index ? stages[--index] : (VTableStage){ancestor->vtable, ancestor->vtable_size};
            if (table.count <= fact->index) break;
            if (!table.slots || table.slots[fact->index].symbol_id != fact->symbol_id) return invalid(state);
            ++fact->ancestors;ancestor = ancestor->base;
        }
    }
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XrCompileResourceStatus xa_class_vtable_build_owned(XrCompileState *state,
    const XaVTableClassInput *inputs, size_t count) {
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK || !count) return status;
    size_t methods = 0;
    status = validate_inputs(state, inputs, count, &methods);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    void *memory = NULL;VTableStage *stages = NULL;VTableOverride *facts = NULL;size_t used = 0;
    status = xr_compile_state_calloc(state, count, sizeof(VTableStage), &memory);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    stages = memory;
    if (methods) {
        memory = NULL;status = xr_compile_state_calloc(state, methods, sizeof(VTableOverride), &memory);
        if (status != XR_COMPILE_RESOURCE_OK) goto done;
        facts = memory;
    }
    for (size_t i = 0; i < count; ++i) {
        VTableStage base = i ? stages[i - 1] : inputs[0].info->base
            ? (VTableStage){inputs[0].info->base->vtable, inputs[0].info->base->vtable_size} : (VTableStage){0};
        size_t first = used;
        status = prepare_stage(state, &inputs[i], &base, &stages[i], facts, &used);
        if (status != XR_COMPILE_RESOURCE_OK) goto done;
        for (size_t j = first; j < used; ++j) {
            if (xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK) { status = xr_compile_state_status(state);goto done; }
            facts[j].owner = i;
        }
    }
    status = reserve_overrides(state, inputs, stages, facts, used);
    if (status != XR_COMPILE_RESOURCE_OK) goto done;
    status = xr_compile_state_work(state, (uint64_t) count * (sizeof(void *) + sizeof(int) + 1));
    if (status != XR_COMPILE_RESOURCE_OK) goto done;
    for (size_t i = 0; i < used; ++i) {
        size_t index = facts[i].owner;XrClassInfo *ancestor = inputs[index].info->base;
        for (size_t j = 0; j < facts[i].ancestors; ++j) {
            XaMethodSlot *slots = index ? stages[--index].slots : ancestor->vtable;
            slots[facts[i].index].is_overridden = true;
            slots[facts[i].index].is_final = false;
            ancestor = ancestor->base;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        inputs[i].info->vtable = stages[i].slots;inputs[i].info->vtable_size = stages[i].count;
        stages[i] = (VTableStage){0};
    }
done:
    for (size_t i = 0; i < count; ++i) free_slots(stages[i].slots, stages[i].count);
    xr_compile_state_free(facts);xr_compile_state_free(stages);
    return status;
}
