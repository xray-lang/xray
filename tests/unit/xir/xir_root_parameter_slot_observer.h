/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_root_parameter_slot_observer.h - Physical published counter observation
 *
 * KEY CONCEPT:
 *   Read-only witnesses inspect a tracked Cell after execution without creating
 *   an admission, validator, allocation or execution authority.
 */
#ifndef XIR_ROOT_PARAMETER_SLOT_OBSERVER_H
#define XIR_ROOT_PARAMETER_SLOT_OBSERVER_H

static const XrXirValue *rps_slot_value(const XrXirInstance *instance) {
    CHECK(instance && instance->program->declarations->slot_count == 1 && instance->published[0]);
    const XrXirValue *handle = &instance->slots[0];
    const XrXirSlot *slot = &instance->program->declarations->slots[0];
    CHECK(slot->module == instance->program->declarations->root_module && slot->mutable);
    CHECK(xr_xir_type_is_cell(instance->program->types, slot->type) &&
        xr_xir_cell_element(instance->program->types, slot->type) == XR_XIR_I64);
    CHECK(handle->type == (uint32_t)slot->type && !handle->reserved);
    const XirCell *cell = (const XirCell *)object_pointer(handle);
    uintptr_t address = (uintptr_t)cell;
    bool tracked = false;
    for (size_t i = 0; i < runtime_owned_capacity; ++i) {
        const RuntimeAllocationRecord *record = &runtime_owned[i];
        uintptr_t base = (uintptr_t)record->pointer;
        if (record->pointer && address >= base && address - base <= record->bytes &&
            sizeof(*cell) <= record->bytes - (size_t)(address - base)) tracked = true;
    }
    CHECK(tracked);
    CHECK(cell->object.kind == XIR_OBJECT_CELL && cell->object.domain == instance->domain &&
        cell->object.arena == instance->program->arena && cell->object.type == slot->type);
    CHECK(cell->initialized && cell->module_storage && !cell->projection_count &&
        cell->module_owner == instance && !cell->module_slot && !cell->borrow_top && !cell->prepared_loan);
    return &cell->value;
}
#endif // XIR_ROOT_PARAMETER_SLOT_OBSERVER_H
