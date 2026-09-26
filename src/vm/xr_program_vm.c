/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_vm.c - Generic CoreSpec dispatch over validated XrProgram
 */

#include "../runtime/core/xr_array_append_storage.h"
#include "../shared/xr_sync_core.h"
#include "xr_program_vm.h"
#include "../runtime/core/xr_text_kernel.h"
#include "../runtime/core/xr_array_allocation_plan.h"
#include "../runtime/core/xr_channel_storage.h"
#include "../runtime/core/xr_string_builder_storage.h"
#include "../shared/xr_integer_division_core.h"
#include "../shared/xr_byte_compare_core.h"
#include "../shared/xr_integer_bitwise_core.h"

#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include "../base/xsha256.h"
#include "../core/xr_core_spec_gen.h"
#include "../program/xr_validated_program_internal.h"

#include <float.h>
#include <limits.h>

_Static_assert(sizeof(double) == 8 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "binary64 required");
#include <string.h>

typedef struct XrVmExistentialValue XrVmExistentialValue;
typedef struct XrVmCallableValue XrVmCallableValue;
typedef struct XrVmClassValue XrVmClassValue;
typedef struct XrVmStringValue XrVmStringValue;

struct XrVmCode {
    atomic_uint_least32_t references;
    XrValidatedProgram *program;
    XrExecutionId execution_id;
    XrFingerprint private_digest;
    XrVmCodeOptions options;
    uint16_t pointer_width;
    uint16_t operating_system;
    uint16_t architecture;
    uint16_t native_abi;
    uint16_t endianness;
};

typedef struct XrVmInstructionView {
    uint16_t operation_id;
    uint16_t result_type_id;
    XrCoreIrValueCategory result_category;
    uint32_t result_id;
    const uint32_t *operands;
    uint32_t operand_count;
    XrCoreIrImmediateKind immediate_kind;
    union {
        int64_t i64;
        uint32_t u32;
        bool boolean;
        uint32_t constant_id;
        uint32_t function_id;
        struct {
            uint32_t module_index;
            uint32_t slot_index;
        } module_slot;
        uint32_t field_ordinal;
        uint32_t variant_ordinal;
        struct {
            uint32_t variant_ordinal;
            uint32_t field_ordinal;
        } variant_field;
        uint16_t type_id;
        struct {
            uint32_t requirement_index;
            uint32_t operation_index;
        } provider_operation;
        struct {
            uint32_t function_id;
            uint32_t safepoint_id;
        } coroutine_call;
        struct {
            uint32_t safepoint_id;
            uint16_t request_kind;
            uint16_t request_operand_count;
        } coroutine_suspend;
    } immediate;
    const uint32_t *successors;
    uint32_t successor_count;
} XrVmInstructionView;

/* All frames in one execution tree borrow this owner. Independent entries
 * have separate storage, and returned host values retain their graph owner. */
typedef struct XrVmValueCell XrVmValueCell;
typedef struct XrVmModuleState XrVmModuleState;

typedef struct XrVmValueStorage {
    uint64_t aggregate_cell_count;
    uint64_t next_class_identity;
    uint64_t steps;
    bool persistent;
    XrVmValueCell *cells;
    void *interrupt_context;
    bool (*interrupt_requested)(void *context);
} XrVmValueStorage;

struct XrVmValueCell {
    XrVmValueCell *previous;
    XrVmValueCell *next;
    XrVmValueStorage *storage;
    uint64_t cell_count;
    XrVmValueKind kind;
    bool marked;
};

typedef struct XrVmContext {
    const XrVmCode *code;
    const XrExecutionLease *lease;
    uint64_t steps;
    XrSHA256Context trace;
    XrVmValueStorage *storage;
    XrVmModuleState *modules;
} XrVmContext;

typedef struct XrVmOutcomeOwner {
    XrExecutionLease lease;
    XrVmCode *code;
    XrVmValueStorage storage;
    XrVmValue value;
} XrVmOutcomeOwner;

static void free_value_storage(XrVmValueStorage *storage);

/* One immutable string owner in the VM arena.  Releasing it is not a
 * semantic event, so drops leave the cell to arena teardown. */
struct XrVmStringValue {
    XrVmValueCell cell;
    uint32_t size;
    uint32_t scalar_count;
    uint8_t *bytes;
};

typedef struct XrVmAggregateValue {
    XrVmValueCell cell;
    uint16_t type_id;
    uint32_t variant_ordinal;
    uint32_t owner_count;
    XrVmValue *fields;
    uint32_t field_count;
    uint32_t field_capacity;
} XrVmAggregateValue;

typedef struct XrVmAtomicValue {
    XrVmValueCell cell;
    uint16_t type_id;
    XrAtomicStorageCore *shared;
} XrVmAtomicValue;

typedef struct XrVmStringBuilderValue {
    XrVmValueCell cell;
    XrStringBuilderStorage text;
} XrVmStringBuilderValue;

typedef struct XrVmChannelValue {
    XrVmValueCell cell;
    uint16_t type_id;
    XrChannelStorage *shared;
} XrVmChannelValue;

typedef struct XrVmResourceValue {
    XrVmValueCell cell;
    uint16_t type_id;
    XrExecutionResource *owner;
} XrVmResourceValue;

struct XrVmClassValue {
    XrVmValueCell cell;
    uint16_t type_id;
    uint16_t reserved16;
    uint32_t owner_count;
    XrVmValue *fields;
    uint32_t field_count;
    uint64_t identity;
    bool alive;
};

static void emit_lifecycle(XrVmContext *context, XrVmLifecycleEventKind kind,
                           XrVmLifecycleEventOrigin origin, const XrVmClassValue *value,
                           uint64_t related_identity, uint32_t field_ordinal);

typedef struct XrVmPlace {
    XrVmValue value;
    XrVmValue *alias;
    XrVmValueCell *container;
    bool initialized;
    XrVmModuleState *module_state;
    uint32_t module_slot;
} XrVmPlace;

struct XrVmModuleState {
    XrVmCode *code;
    XrVmValueStorage storage;
    XrVmValueStorage failure_storage;
    atomic_uint_least64_t next_class_identity;
    XrVmPlace *slots;
    uint32_t *publication_order;
    uint32_t publication_count;
    XrVmOutcomeKind failure_kind;
    XrVmTrap failure_trap;
    XrVmValue failure_error;
    XrVmValue failure_panic;
};

typedef struct XrVmRuntimeValue {
    XrCoreIrValueCategory category;
    union {
        XrVmValue value;
        XrVmPlace *place;
    } as;
} XrVmRuntimeValue;

struct XrVmExecution {
    XrVmContext context;
    XrVmValue terminal_owner;
    XrVmValueStorage storage;
    XrExecutionLease lease;
    XrVmCode *code;
    uint32_t function_id;
    uint32_t block_id;
    uint32_t instruction_id;
    uint32_t state_id;
    uint32_t cancel_block_id;
    uint32_t suspension_block_id;
    uint32_t suspension_instruction_id;
    XrVmRuntimeValue *values;
    XrVmRuntimeValue *edge_values;
    XrVmPlace *places;
    bool *initialized;
    struct XrVmExecution *child;
    uint32_t depth;
    bool owns_lease;
    bool initialization_complete;
    uint32_t initialization_module;
    XrVmExecution *initialization_child;
    bool suspended;
    bool finished;
    bool values_ready;
    bool owners_released;
};

struct XrVmExistentialValue {
    XrVmValueCell cell;
    uint16_t existential_type_id;
    uint16_t concrete_type_id;
    uint32_t conformance_id;
    XrVmRuntimeValue payload;
    XrVmPlace owned_storage;
};

struct XrVmCallableValue {
    XrVmValueCell cell;
    uint16_t callable_type_id;
    uint16_t capture_type_id;
    uint32_t function_id;
    bool has_capture;
    XrVmValue capture;
};

static XrVmValueCell *vm_value_cell(XrVmValue value) {
    switch (value.kind) {
        case XR_VM_VALUE_PANIC_INFO:
            return (XrVmValueCell *) (void *) value.as.panic_info.message;
        case XR_VM_VALUE_RESOURCE:
            return (XrVmValueCell *) (void *) value.as.resource;
        case XR_VM_VALUE_STRING_BUILDER:
            return (XrVmValueCell *) (void *) value.as.string_builder;
        case XR_VM_VALUE_CHANNEL:
            return (XrVmValueCell *) (void *) value.as.channel_storage;
        case XR_VM_VALUE_ATOMIC:
            return (XrVmValueCell *) (void *) value.as.atomic_storage;
        case XR_VM_VALUE_AGGREGATE:
            return (XrVmValueCell *) (void *) value.as.aggregate;
        case XR_VM_VALUE_CLASS_REFERENCE:
            return (XrVmValueCell *) (void *) value.as.class_reference;
        case XR_VM_VALUE_STRING:
            return (XrVmValueCell *) (void *) value.as.string;
        case XR_VM_VALUE_EXISTENTIAL:
            return (XrVmValueCell *) (void *) value.as.existential;
        case XR_VM_VALUE_CALLABLE:
            return (XrVmValueCell *) (void *) value.as.callable;
        default:
            return NULL;
    }
}

static void vm_value_cell_destroy(XrVmValueCell *cell);

static XrVmOutcome vm_outcome(XrVmOutcomeKind kind, const XrVmContext *context) {
    XrVmOutcome outcome = {.kind = kind, .steps = context ? context->steps : 0u};
    return outcome;
}

static XrVmOutcome vm_trap(XrVmTrap trap, const XrVmContext *context) {
    XrVmOutcome outcome = vm_outcome(XR_VM_OUTCOME_TRAP, context);
    outcome.trap = trap;
    return outcome;
}

static XrVmValue void_value(void) {
    XrVmValue value = {.kind = XR_VM_VALUE_VOID};
    return value;
}

static bool class_value_is_live(const XrVmClassValue *value, uint16_t type_id) {
    return value && value->alive && value->owner_count != 0u && value->type_id == type_id &&
           (value->field_count == 0u || value->fields);
}

static XrVmValue *vm_place_value(XrVmPlace *place) {
    return place ? (place->alias ? place->alias : &place->value) : NULL;
}

static const XrVmValue *vm_place_value_const(const XrVmPlace *place) {
    return place ? (place->alias ? place->alias : &place->value) : NULL;
}

static bool vm_place_is_persistent(const XrVmPlace *place) {
    return place &&
           (place->module_state || (place->container && place->container->storage->persistent));
}

static bool value_matches_type(const XrValidatedProgram *program, XrVmValue value,
                               uint16_t type_id) {
    switch (type_id) {
        case XR_CORE_TYPE_VOID:
            return value.kind == XR_VM_VALUE_VOID;
        case XR_CORE_TYPE_BOOL:
            return value.kind == XR_VM_VALUE_BOOL;
        case XR_CORE_TYPE_F64:
            return value.kind == XR_VM_VALUE_F64;
        case XR_CORE_TYPE_I64:
            return value.kind == XR_VM_VALUE_I64;
        case XR_CORE_TYPE_I8:
            return value.kind == XR_VM_VALUE_I8;
        case XR_CORE_TYPE_U8:
            return value.kind == XR_VM_VALUE_U8;
        case XR_CORE_TYPE_I16:
            return value.kind == XR_VM_VALUE_I16;
        case XR_CORE_TYPE_I32:
            return value.kind == XR_VM_VALUE_I32;
        case XR_CORE_TYPE_U64:
            return value.kind == XR_VM_VALUE_U64;
        case XR_CORE_TYPE_U32:
            return value.kind == XR_VM_VALUE_U32;
        case XR_CORE_TYPE_U16:
            return value.kind == XR_VM_VALUE_U16;
        case XR_CORE_TYPE_TARGET_OS:
            return value.kind == XR_VM_VALUE_TARGET_OS;
        case XR_CORE_TYPE_TARGET_ARCH:
            return value.kind == XR_VM_VALUE_TARGET_ARCH;
        case XR_CORE_TYPE_TARGET_ABI:
            return value.kind == XR_VM_VALUE_TARGET_ABI;
        case XR_CORE_TYPE_TARGET_ENDIAN:
            return value.kind == XR_VM_VALUE_TARGET_ENDIAN;
        case XR_CORE_TYPE_ERROR:
            return value.kind == XR_VM_VALUE_ERROR;
        case XR_CORE_TYPE_PANIC_INFO:
            return value.kind == XR_VM_VALUE_PANIC_INFO;
        case XR_CORE_TYPE_STRING_BUILDER:
            return value.kind == XR_VM_VALUE_STRING_BUILDER && value.as.string_builder;
        case XR_CORE_TYPE_STRING:
            return value.kind == XR_VM_VALUE_STRING && value.as.string;
        case XR_CORE_TYPE_RUNE:
            return value.kind == XR_VM_VALUE_RUNE && xr_text_rune_is_scalar(value.as.rune);
        default: {
            const XrValidatedType *type = xr_validated_program_type(program, type_id);
            if (!type)
                return false;
            if (type->kind == XR_CORE_IR_TYPE_PROVIDER_RESOURCE) {
                const XrVmResourceValue *resource = value.as.resource;
                return value.kind == XR_VM_VALUE_RESOURCE && resource && resource->owner &&
                       resource->type_id == type_id;
            }
            if (type->kind == XR_CORE_IR_TYPE_CHANNEL) {
                const XrVmChannelValue *channel = value.as.channel_storage;
                return value.kind == XR_VM_VALUE_CHANNEL && channel && channel->shared &&
                       channel->type_id == type_id;
            }
            if (type->kind == XR_CORE_IR_TYPE_ATOMIC) {
                const XrVmAtomicValue *atomic_value = value.as.atomic_storage;
                return value.kind == XR_VM_VALUE_ATOMIC && atomic_value && atomic_value->shared &&
                       atomic_value->type_id == type_id;
            }
            if (type->kind == XR_CORE_IR_TYPE_EXISTENTIAL)
                return value.kind == XR_VM_VALUE_EXISTENTIAL && value.as.existential &&
                       ((const XrVmExistentialValue *) value.as.existential)->existential_type_id ==
                           type_id;
            if (type->kind == XR_CORE_IR_TYPE_CALLABLE)
                return value.kind == XR_VM_VALUE_CALLABLE && value.as.callable &&
                       ((const XrVmCallableValue *) value.as.callable)->callable_type_id == type_id;
            if (xr_program_type_kind_is_reference_record(type->kind))
                return value.kind == XR_VM_VALUE_CLASS_REFERENCE &&
                       class_value_is_live(value.as.class_reference, type_id);
            return (type->kind == XR_CORE_IR_TYPE_AGGREGATE ||
                    type->kind == XR_CORE_IR_TYPE_VARIANT || type->kind == XR_CORE_IR_TYPE_ARRAY) &&
                   value.kind == XR_VM_VALUE_AGGREGATE && value.as.aggregate &&
                   ((const XrVmAggregateValue *) value.as.aggregate)->type_id == type_id;
        }
    }
}

static void vm_value_cell_link(XrVmValueStorage *storage, XrVmValueCell *cell, XrVmValueKind kind,
                               uint64_t cell_count) {
    XR_CHECK(storage && cell && !cell->storage, "value cell already has a storage owner");
    cell->storage = storage;
    cell->kind = kind;
    cell->cell_count = cell_count;
    cell->next = storage->cells;
    if (cell->next)
        cell->next->previous = cell;
    storage->cells = cell;
    storage->aggregate_cell_count += cell_count;
}

static XrVmClassValue *allocate_class(XrVmContext *context, uint16_t type_id,
                                      uint32_t field_count) {
    XrVmValueStorage *storage = context->storage;
    const XrValidatedType *type = xr_validated_program_type(context->code->program, type_id);
    if (!type || !xr_program_type_kind_is_reference_record(type->kind) ||
        type->field_count != field_count ||
        (uint64_t) field_count >
            (uint64_t) context->code->options.max_value_cells - storage->aggregate_cell_count)
        return NULL;
    XrVmClassValue *value = xr_calloc(1u, sizeof(*value));
    if (!value)
        return NULL;
    if (field_count != 0u) {
        value->fields = xr_calloc(field_count, sizeof(*value->fields));
        if (!value->fields) {
            xr_free(value);
            return NULL;
        }
    }
    value->type_id = type_id;
    value->field_count = field_count;
    value->owner_count = 1u;
    value->identity = context->modules
                          ? atomic_fetch_add_explicit(&context->modules->next_class_identity, 1u,
                                                      memory_order_relaxed) +
                                1u
                          : ++storage->next_class_identity;
    value->alive = true;
    vm_value_cell_link(storage, &value->cell, XR_VM_VALUE_CLASS_REFERENCE, field_count);
    return value;
}

static void *vm_array_backing_allocate(void *context, size_t size) {
    (void) context;
    return xr_malloc(size);
}

static XrVmAggregateValue *allocate_aggregate(XrVmContext *context, uint16_t type_id,
                                              uint32_t variant_ordinal, uint32_t field_count) {
    XrVmValueStorage *storage = context->storage;
    uint32_t limit = context->code->options.max_value_cells;
    if (storage->aggregate_cell_count > limit)
        return NULL;
    XrArrayAllocationPlan plan = xr_array_allocation_plan(
        field_count, sizeof(XrVmValue), limit - (uint32_t) storage->aggregate_cell_count, SIZE_MAX);
    if (plan.status != XR_ARRAY_ALLOCATION_OK)
        return NULL;
    XrVmAggregateValue *aggregate = xr_calloc(1u, sizeof(*aggregate));
    if (!aggregate)
        return NULL;
    if (field_count != 0u) {
        aggregate->fields = xr_calloc(1u, plan.bytes);
        if (!aggregate->fields) {
            xr_free(aggregate);
            return NULL;
        }
    }
    aggregate->type_id = type_id;
    aggregate->owner_count = 1u;
    aggregate->variant_ordinal = variant_ordinal;
    aggregate->field_count = field_count;
    aggregate->field_capacity = field_count;
    vm_value_cell_link(storage, &aggregate->cell, XR_VM_VALUE_AGGREGATE, field_count);
    return aggregate;
}

/* Each string counts one cell plus its payload bytes against the VM budget. */
static void *vm_builder_allocate(void *opaque, size_t size) {
    XrVmContext *context = opaque;
    if (context->storage->aggregate_cell_count > context->code->options.max_value_cells ||
        size > context->code->options.max_value_cells - context->storage->aggregate_cell_count)
        return NULL;
    return xr_malloc(size);
}

static void vm_builder_release(void *opaque, void *pointer) {
    (void) opaque;
    xr_free(pointer);
}

static void release_string_builder_storage(XrVmStringBuilderValue *value) {
    if (!value) return;
    size_t capacity = value->text.capacity;
    XrStringBuilderAllocator allocator = {NULL, NULL, vm_builder_release};
    xr_string_builder_dispose(&value->text, &allocator);
    if (value->cell.storage) {
        value->cell.storage->aggregate_cell_count -= capacity;
        value->cell.cell_count -= capacity;
    }
}

static XrVmStringBuilderValue *allocate_string_builder(XrVmContext *context) {
    if (context->storage->aggregate_cell_count >= context->code->options.max_value_cells)
        return NULL;
    XrVmStringBuilderValue *value = xr_calloc(1u, sizeof(*value));
    if (value) vm_value_cell_link(context->storage, &value->cell, XR_VM_VALUE_STRING_BUILDER, 1u);
    return value;
}

static XrVmStringValue *allocate_string(XrVmContext *context, size_t size) {
    XrVmValueStorage *storage = context->storage;
    if (size > XR_PROGRAM_CONSTANT_STRING_MAX_BYTES ||
        (uint64_t) size + 1u >
            (uint64_t) context->code->options.max_value_cells - storage->aggregate_cell_count)
        return NULL;
    XrVmStringValue *string = xr_calloc(1u, sizeof(*string));
    if (!string)
        return NULL;
    string->bytes = xr_malloc(size != 0u ? size : 1u);
    if (!string->bytes) {
        xr_free(string);
        return NULL;
    }
    string->size = (uint32_t) size;
    vm_value_cell_link(storage, &string->cell, XR_VM_VALUE_STRING, (uint64_t) size + 1u);
    return string;
}

static XrVmValue vm_string_value(const XrVmStringValue *string) {
    XrVmValue value = {.kind = XR_VM_VALUE_STRING};
    value.as.string = string;
    return value;
}

static bool vm_string_view(XrVmValue value, const uint8_t **bytes_out, size_t *size_out) {
    const XrVmStringValue *string = value.as.string;
    if (value.kind != XR_VM_VALUE_STRING || !string)
        return false;
    *bytes_out = string->bytes;
    *size_out = string->size;
    return true;
}

static XrVmExistentialValue *allocate_existential(XrVmContext *context) {
    XrVmValueStorage *storage = context->storage;
    if (storage->aggregate_cell_count == context->code->options.max_value_cells)
        return NULL;
    XrVmExistentialValue *value = xr_calloc(1u, sizeof(*value));
    if (!value)
        return NULL;
    vm_value_cell_link(storage, &value->cell, XR_VM_VALUE_EXISTENTIAL, 1u);
    return value;
}

static XrVmAtomicValue *allocate_atomic(XrVmContext *context, uint16_t type_id,
                                          XrAtomicStorageCore *shared, int64_t initial) {
    XrVmValueStorage *storage = context->storage;
    if (storage->aggregate_cell_count >= context->code->options.max_value_cells)
        return NULL;
    XrVmAtomicValue *value = xr_calloc(1u, sizeof(*value));
    if (!value)
        return NULL;
    if (shared) {
        if (!xr_atomic_storage_retain_core(shared)) {
            xr_free(value);
            return NULL;
        }
    } else {
        shared = xr_malloc(sizeof(*shared));
        if (!shared) {
            xr_free(value);
            return NULL;
        }
        xr_atomic_storage_init_core(shared, initial);
    }
    value->type_id = type_id;
    value->shared = shared;
    vm_value_cell_link(storage, &value->cell, XR_VM_VALUE_ATOMIC, 1u);
    return value;
}

static void release_atomic_storage(XrVmAtomicValue *value) {
    if (value->shared && xr_atomic_storage_release_core(value->shared) == XR_ATOMIC_STORAGE_RELEASE_LAST)
        xr_free(value->shared);
    value->shared = NULL;
}

static XrVmChannelValue *allocate_channel(XrVmContext *context, uint16_t type_id, int64_t capacity) {
    if (capacity < 0 || (uint64_t) capacity > UINT32_MAX ||
        (uint64_t) capacity > context->code->options.max_value_cells ||
        context->storage->aggregate_cell_count >= context->code->options.max_value_cells)
        return NULL;
    XrVmChannelValue *value = xr_calloc(1u, sizeof(*value));
    XrChannelStorage *shared = value ? xr_malloc(sizeof(*shared)) : NULL;
    XrChannelMessageOwner *slots = shared && capacity ? xr_calloc((size_t) capacity, sizeof(*slots)) : NULL;
    if (!shared || (capacity && !slots) || !xr_channel_storage_init(shared, slots, (uint32_t) capacity)) {
        xr_free(slots);
        xr_free(shared);
        xr_free(value);
        return NULL;
    }
    value->type_id = type_id;
    value->shared = shared;
    vm_value_cell_link(context->storage, &value->cell, XR_VM_VALUE_CHANNEL, 1u);
    return value;
}

static XrVmChannelValue *retain_channel(XrVmContext *context, uint16_t type_id,
                                       XrChannelStorage *shared) {
    if (!shared || context->storage->aggregate_cell_count >= context->code->options.max_value_cells)
        return NULL;
    XrVmChannelValue *value = xr_calloc(1u, sizeof(*value));
    if (!value)
        return NULL;
    if (!xr_channel_storage_retain(shared)) {
        xr_free(value);
        return NULL;
    }
    value->type_id = type_id;
    value->shared = shared;
    vm_value_cell_link(context->storage, &value->cell, XR_VM_VALUE_CHANNEL, 1u);
    return value;
}

static void release_channel_storage(XrVmChannelValue *value) {
    XrChannelStorage *shared = value->shared;
    value->shared = NULL;
    if (shared && xr_channel_storage_release(shared) == XR_CHANNEL_STORAGE_LAST_OWNER) {
        xr_free(shared->slots);
        xr_free(shared);
    }
}

static XrVmCallableValue *allocate_callable(XrVmContext *context) {
    XrVmValueStorage *storage = context->storage;
    if (storage->aggregate_cell_count == context->code->options.max_value_cells)
        return NULL;
    XrVmCallableValue *value = xr_calloc(1u, sizeof(*value));
    if (!value)
        return NULL;
    vm_value_cell_link(storage, &value->cell, XR_VM_VALUE_CALLABLE, 1u);
    return value;
}

static uint32_t callable_function_id(const XrValidatedProgram *program,
                                     const XrVmCallableValue *carrier) {
    if (!program || !carrier || carrier->function_id >= program->function_count)
        return XR_PROGRAM_LOCATION_NONE;
    const XrValidatedType *callable = xr_validated_program_type(program, carrier->callable_type_id);
    const XrValidatedFunction *target = &program->functions[carrier->function_id];
    if (!callable || callable->kind != XR_CORE_IR_TYPE_CALLABLE ||
        callable->signature_id >= program->signature_count ||
        target->has_receiver != carrier->has_capture ||
        (carrier->has_capture &&
         (target->receiver_mode != XR_PARAM_READ || target->parameter_count == 0u ||
          target->parameter_types[0] != carrier->capture_type_id)))
        return XR_PROGRAM_LOCATION_NONE;
    return carrier->function_id;
}

static uint32_t conformance_id(const XrValidatedProgram *program, uint16_t concrete_type_id,
                               uint32_t interface_id) {
    for (uint32_t index = 0; index < program->conformance_count; ++index)
        if (program->conformances[index].implementor_type_id == concrete_type_id &&
            program->conformances[index].interface_id == interface_id)
            return index;
    return XR_PROGRAM_LOCATION_NONE;
}

static uint32_t witness_function_id(const XrValidatedProgram *program,
                                    const XrVmExistentialValue *carrier, uint32_t slot_ordinal) {
    if (!program || !carrier || carrier->conformance_id >= program->conformance_count)
        return XR_PROGRAM_LOCATION_NONE;
    const XrValidatedConformance *conformance = &program->conformances[carrier->conformance_id];
    const XrValidatedType *existential =
        xr_validated_program_type(program, carrier->existential_type_id);
    if (!existential || existential->kind != XR_CORE_IR_TYPE_EXISTENTIAL ||
        conformance->interface_id != existential->interface_id ||
        conformance->implementor_type_id != carrier->concrete_type_id ||
        slot_ordinal >= conformance->slot_count)
        return XR_PROGRAM_LOCATION_NONE;
    uint32_t function_id = conformance->slot_function_ids[slot_ordinal];
    return function_id < program->function_count ? function_id : XR_PROGRAM_LOCATION_NONE;
}

static bool function_parameter_is_class_receiver(const XrValidatedProgram *program,
                                                 const XrValidatedFunction *function,
                                                 uint32_t parameter) {
    if (!program || !function || !function->has_receiver || parameter != 0u ||
        function->parameter_count == 0u)
        return false;
    const XrValidatedType *type = xr_validated_program_type(program, function->parameter_types[0]);
    return type && type->kind == XR_CORE_IR_TYPE_CLASS_REFERENCE;
}

static XrCoreIrValueCategory function_parameter_category(const XrValidatedProgram *program,
                                                         const XrValidatedFunction *function,
                                                         uint32_t parameter) {
    return function->parameter_modes[parameter] == XR_PARAM_REF &&
                   !function_parameter_is_class_receiver(program, function, parameter)
               ? XR_CORE_IR_PLACE
               : XR_CORE_IR_VALUE;
}

static bool witness_receiver_argument(const XrVmExistentialValue *carrier,
                                      XrParamMode receiver_mode, XrVmRuntimeValue *argument) {
    if (!carrier || !argument)
        return false;
    if (receiver_mode == XR_PARAM_REF) {
        if (carrier->payload.category != XR_CORE_IR_PLACE || !carrier->payload.as.place ||
            !carrier->payload.as.place->initialized)
            return false;
        *argument = carrier->payload;
        return true;
    }
    if (carrier->payload.category == XR_CORE_IR_PLACE) {
        if (!carrier->payload.as.place || !carrier->payload.as.place->initialized)
            return false;
        argument->category = XR_CORE_IR_VALUE;
        argument->as.value = *vm_place_value_const(carrier->payload.as.place);
        return true;
    }
    *argument = carrier->payload;
    return argument->category == XR_CORE_IR_VALUE;
}

static bool clone_vm_value(XrVmContext *context, XrVmValue source, uint16_t type_id,
                           XrVmValue *output) {
    const XrValidatedType *type = xr_validated_program_type(context->code->program, type_id);
    if (type_id == XR_CORE_TYPE_STRING) {
        const XrVmStringValue *original = source.as.string;
        if (source.kind != XR_VM_VALUE_STRING || !original)
            return false;
        XrVmStringValue *copy = allocate_string(context, original->size);
        if (!copy)
            return false;
        if (original->size != 0u)
            memcpy(copy->bytes, original->bytes, original->size);
        copy->scalar_count = original->scalar_count;
        *output = vm_string_value(copy);
        return true;
    }
    if (!type) {
        *output = source;
        return true;
    }
    if (type->kind == XR_CORE_IR_TYPE_CHANNEL) {
        if (!value_matches_type(context->code->program, source, type_id))
            return false;
        const XrVmChannelValue *original = source.as.channel_storage;
        XrVmChannelValue *copy = retain_channel(context, type_id, original->shared);
        if (!copy)
            return false;
        output->kind = XR_VM_VALUE_CHANNEL;
        output->as.channel_storage = copy;
        return true;
    }

    if (type->kind == XR_CORE_IR_TYPE_ATOMIC) {
        if (!value_matches_type(context->code->program, source, type_id))
            return false;
        const XrVmAtomicValue *original = source.as.atomic_storage;
        XrVmAtomicValue *copy = allocate_atomic(context, type_id, original->shared, 0);
        if (!copy)
            return false;
        output->kind = XR_VM_VALUE_ATOMIC;
        output->as.atomic_storage = copy;
        return true;
    }
    if (type->kind == XR_CORE_IR_TYPE_EXISTENTIAL) {
        if (source.kind != XR_VM_VALUE_EXISTENTIAL || !source.as.existential)
            return false;
        *output = source;
        return true;
    }
    if (type->kind == XR_CORE_IR_TYPE_CALLABLE) {
        if (source.kind != XR_VM_VALUE_CALLABLE || !source.as.callable)
            return false;
        const XrVmCallableValue *source_callable = source.as.callable;
        XrVmCallableValue *copy = allocate_callable(context);
        if (!copy)
            return false;
        XrVmValueCell cell = copy->cell;
        *copy = *source_callable;
        copy->cell = cell;
        if (copy->has_capture && !clone_vm_value(context, source_callable->capture,
                                                 source_callable->capture_type_id, &copy->capture))
            return false;
        output->kind = XR_VM_VALUE_CALLABLE;
        output->as.callable = copy;
        return true;
    }
    if (type->kind != XR_CORE_IR_TYPE_AGGREGATE && type->kind != XR_CORE_IR_TYPE_VARIANT &&
        type->kind != XR_CORE_IR_TYPE_ARRAY)
        return false;
    const XrVmAggregateValue *source_aggregate = source.as.aggregate;
    if (!source_aggregate || source_aggregate->type_id != type_id)
        return false;
    XrVmAggregateValue *copy = allocate_aggregate(
        context, type_id, source_aggregate->variant_ordinal, source_aggregate->field_count);
    if (!copy)
        return false;
    for (uint32_t field = 0; field < source_aggregate->field_count; ++field) {
        uint16_t field_type = XR_CORE_TYPE_VOID;
        if (type->kind == XR_CORE_IR_TYPE_ARRAY) {
            field_type = type->array_element_type;
        } else if (type->kind == XR_CORE_IR_TYPE_AGGREGATE) {
            if (field >= type->field_count)
                return false;
            field_type = type->field_types[field];
        } else {
            if (source_aggregate->variant_ordinal >= type->variant_count ||
                field >= type->variants[source_aggregate->variant_ordinal].payload_count)
                return false;
            field_type = type->variants[source_aggregate->variant_ordinal].payload_types[field];
        }
        if (!clone_vm_value(context, source_aggregate->fields[field], field_type,
                            &copy->fields[field]))
            return false;
    }
    output->kind = XR_VM_VALUE_AGGREGATE;
    output->as.aggregate = copy;
    return true;
}

static bool class_field_load_value(XrVmContext *context, const XrVmClassValue *instance,
                                   uint32_t field_ordinal, uint16_t field_type_id,
                                   XrVmValue *output) {
    const XrValidatedType *type =
        context && instance ? xr_validated_program_type(context->code->program, instance->type_id)
                            : NULL;
    if (!context || !output || !type || !xr_program_type_kind_is_reference_record(type->kind) ||
        !class_value_is_live(instance, instance->type_id) || field_ordinal >= type->field_count ||
        field_ordinal >= instance->field_count || type->field_types[field_ordinal] != field_type_id)
        return false;
    if (xr_validated_program_type_ownership(context->code->program, field_type_id) ==
        XR_CORE_IR_TYPE_OWNERSHIP_AFFINE) {
        *output = instance->fields[field_ordinal];
        return true;
    }
    return clone_vm_value(context, instance->fields[field_ordinal], field_type_id, output);
}

static void drop_vm_value(XrVmContext *context, XrVmValue *value, XrVmLifecycleEventOrigin origin) {
    if (!value)
        return;
    XrVmValueCell *cell = vm_value_cell(*value);
    bool release_cell = cell && cell->storage && cell->storage->persistent;
    if (value->kind == XR_VM_VALUE_RESOURCE) {
        XrVmResourceValue *resource = (XrVmResourceValue *) (void *) value->as.resource;
        if (resource)
            xr_execution_resource_free(&resource->owner);
    } else if (value->kind == XR_VM_VALUE_STRING_BUILDER) {
        release_string_builder_storage((XrVmStringBuilderValue *) (void *) value->as.string_builder);
    } else if (value->kind == XR_VM_VALUE_CHANNEL) {
        release_channel_storage((XrVmChannelValue *) (void *) value->as.channel_storage);
    } else if (value->kind == XR_VM_VALUE_ATOMIC) {
        release_atomic_storage((XrVmAtomicValue *) (void *) value->as.atomic_storage);
    } else if (value->kind == XR_VM_VALUE_CLASS_REFERENCE) {
        XrVmClassValue *instance = (XrVmClassValue *) (void *) value->as.class_reference;
        if (instance && instance->alive && instance->owner_count != 0u) {
            emit_lifecycle(context, XR_VM_EVENT_OWNER_DROP, origin, instance, UINT64_MAX,
                           UINT32_MAX);
            if (--instance->owner_count != 0u) {
                release_cell = false;
                goto consumed;
            }
            emit_lifecycle(context, XR_VM_EVENT_CLASS_FINALIZE, origin, instance, UINT64_MAX,
                           UINT32_MAX);
            for (uint32_t field = instance->field_count; field != 0u; --field)
                drop_vm_value(context, &instance->fields[field - 1u],
                              XR_VM_EVENT_ORIGIN_FIELD_FINALIZATION);
            xr_free(instance->fields);
            instance->fields = NULL;
            instance->field_count = 0u;
            instance->alive = false;
            emit_lifecycle(context, XR_VM_EVENT_CLASS_RECLAIM, origin, instance, UINT64_MAX,
                           UINT32_MAX);
        }
    } else if (value->kind == XR_VM_VALUE_AGGREGATE && value->as.aggregate) {
        XrVmAggregateValue *aggregate = (XrVmAggregateValue *) (void *) value->as.aggregate;
        const XrValidatedType *type = xr_validated_program_type(context->code->program, aggregate->type_id);
        if (type && type->kind == XR_CORE_IR_TYPE_ARRAY && aggregate->owner_count &&
            --aggregate->owner_count != 0u) {
            release_cell = false;
            goto consumed;
        }
        for (uint32_t field = aggregate->field_count; field != 0u; --field)
            drop_vm_value(context, &aggregate->fields[field - 1u], origin);
    } else if (value->kind == XR_VM_VALUE_EXISTENTIAL && value->as.existential) {
        XrVmExistentialValue *existential = (XrVmExistentialValue *) (void *) value->as.existential;
        if (existential->owned_storage.initialized) {
            drop_vm_value(context, &existential->owned_storage.value, origin);
            existential->owned_storage.initialized = false;
        }
    } else if (value->kind == XR_VM_VALUE_CALLABLE && value->as.callable) {
        XrVmCallableValue *callable = (XrVmCallableValue *) (void *) value->as.callable;
        if (callable->has_capture) {
            drop_vm_value(context, &callable->capture, origin);
            callable->has_capture = false;
        }
    }
consumed:
    if (release_cell)
        vm_value_cell_destroy(cell);
    *value = void_value();
}

static void vm_cleanup_consume(XrVmExecution *execution, const XrValidatedFunction *function,
                               const XrValidatedInstruction *instruction, uint32_t block_id,
                               bool calls_only) {
    for (uint32_t operand = 0u; operand < instruction->operand_count; ++operand) {
        uint32_t owner = xr_validated_instruction_consumed_owner(
            execution->code->program, function, instruction, operand, block_id, calls_only);
        if (owner != XR_PROGRAM_LOCATION_NONE)
            execution->initialized[owner] = false;
    }
}

/* Reconstruct the current block's committed ownership prefix only at frame
 * exit. Reuse admission
 * scratch after execution stops; no live-owner bitmap
 * or executor-specific ownership graph
 * persists during execution. */
static void vm_execution_release_owners(XrVmExecution *execution, uint32_t block_id,
                                        uint32_t committed,
                                        const XrValidatedInstruction *transferred,
                                        bool calls_only) {
    if (!execution || !execution->values_ready || execution->owners_released)
        return;
    execution->owners_released = true;
    const XrValidatedFunction *function =
        &execution->code->program->functions[execution->function_id];
    const XrValidatedBlock *block = &function->blocks[block_id];
    memset(execution->initialized, 0, function->value_count * sizeof(*execution->initialized));
    for (uint32_t argument = 0u; argument < block->argument_count; ++argument)
        if (block->argument_ownerships[argument] == XR_CORE_IR_OWNER)
            execution->initialized[block->argument_ids[argument]] = true;
    for (uint32_t index = 0u; index < committed; ++index) {
        const XrValidatedInstruction *instruction = &block->instructions[index];
        vm_cleanup_consume(execution, function, instruction, block_id, false);
        if (instruction->result_id != XR_PROGRAM_LOCATION_NONE &&
            instruction->result_ownership == XR_CORE_IR_OWNER)
            execution->initialized[instruction->result_id] = true;
    }
    if (transferred)
        vm_cleanup_consume(execution, function, transferred, block_id, calls_only);
    for (uint32_t index = committed; index != 0u; --index) {
        uint32_t value = block->instructions[index - 1u].result_id;
        if (value != XR_PROGRAM_LOCATION_NONE && execution->initialized[value])
            drop_vm_value(&execution->context, &execution->values[value].as.value,
                          XR_VM_EVENT_ORIGIN_DOMAIN_TEARDOWN);
    }
    for (uint32_t argument = block->argument_count; argument != 0u; --argument) {
        uint32_t value = block->argument_ids[argument - 1u];
        if (execution->initialized[value])
            drop_vm_value(&execution->context, &execution->values[value].as.value,
                          XR_VM_EVENT_ORIGIN_DOMAIN_TEARDOWN);
    }
}

static void vm_execution_release_current_owners(XrVmExecution *execution) {
    if (!execution)
        return;
    bool suspended = execution->suspension_block_id != XR_PROGRAM_LOCATION_NONE;
    vm_execution_release_owners(
        execution, suspended ? execution->suspension_block_id : execution->block_id,
        suspended ? execution->suspension_instruction_id : execution->instruction_id, NULL, false);
}

bool xr_vm_value_string_view(const XrVmValue *value, XrVmStringView *view_out) {
    if (view_out)
        memset(view_out, 0, sizeof(*view_out));
    if (!value || !view_out || value->kind != XR_VM_VALUE_STRING || !value->as.string)
        return false;
    const XrVmStringValue *string = value->as.string;
    view_out->bytes = string->bytes;
    view_out->size = string->size;
    return true;
}

bool xr_vm_value_aggregate_view(const XrVmValue *value, XrVmAggregateView *view_out) {
    if (view_out)
        memset(view_out, 0, sizeof(*view_out));
    if (!value || !view_out || value->kind != XR_VM_VALUE_AGGREGATE || !value->as.aggregate)
        return false;
    const XrVmAggregateValue *aggregate = value->as.aggregate;
    *view_out = (XrVmAggregateView) {
        .type_id = aggregate->type_id,
        .variant_ordinal = aggregate->variant_ordinal,
        .fields = aggregate->fields,
        .field_count = aggregate->field_count,
    };
    return true;
}

#include "xr_program_vm_format.inc.c"

bool xr_vm_panic_message_view(const XrVmPanicInfo *panic, XrVmStringView *view_out) {
    XrVmValue value = {.kind = XR_VM_VALUE_STRING, .as.string = panic ? panic->message : NULL};
    return xr_vm_value_string_view(&value, view_out);
}

void xr_vm_outcome_dispose(XrVmOutcome *outcome) {
    if (!outcome)
        return;
    if (outcome->private_owner) {
        XrVmOutcomeOwner *owner = outcome->private_owner;
        if (owner->code) {
            XrVmContext context = {.code = owner->code, .storage = &owner->storage};
            drop_vm_value(&context, &owner->value, XR_VM_EVENT_ORIGIN_DOMAIN_TEARDOWN);
            free_value_storage(&owner->storage);
            xr_vm_code_free(owner->code);
        }
        (void) xr_execution_lease_release(&owner->lease);
        xr_free(owner);
    }
    memset(outcome, 0, sizeof(*outcome));
}

static void vm_value_cell_unlink(XrVmValueCell *cell) {
    XrVmValueStorage *storage = cell->storage;
    XR_CHECK(storage && storage->aggregate_cell_count >= cell->cell_count,
             "value cell lost its physical storage owner");
    if (cell->previous)
        cell->previous->next = cell->next;
    else
        storage->cells = cell->next;
    if (cell->next)
        cell->next->previous = cell->previous;
    storage->aggregate_cell_count -= cell->cell_count;
    cell->storage = NULL;
    cell->previous = NULL;
    cell->next = NULL;
}

static void vm_value_cell_destroy(XrVmValueCell *cell) {
    vm_value_cell_unlink(cell);
    switch (cell->kind) {
        case XR_VM_VALUE_STRING_BUILDER:
            release_string_builder_storage((XrVmStringBuilderValue *) cell);
            break;
        case XR_VM_VALUE_RESOURCE:
            xr_execution_resource_free(&((XrVmResourceValue *) cell)->owner);
            break;
        case XR_VM_VALUE_CHANNEL:
            release_channel_storage((XrVmChannelValue *) cell);
            break;
        case XR_VM_VALUE_ATOMIC:
            release_atomic_storage((XrVmAtomicValue *) cell);
            break;
        case XR_VM_VALUE_AGGREGATE:
            xr_free(((XrVmAggregateValue *) cell)->fields);
            break;
        case XR_VM_VALUE_CLASS_REFERENCE:
            xr_free(((XrVmClassValue *) cell)->fields);
            break;
        case XR_VM_VALUE_STRING:
            xr_free(((XrVmStringValue *) cell)->bytes);
            break;
        case XR_VM_VALUE_EXISTENTIAL:
        case XR_VM_VALUE_CALLABLE:
            break;
        default:
            XR_CHECK(false, "unsupported physical value cell");
    }
    xr_free(cell);
}

static void free_value_storage(XrVmValueStorage *storage) {
    while (storage->cells)
        vm_value_cell_destroy(storage->cells);
}

typedef struct XrVmValueTransfer {
    XrVmValueStorage *source;
    XrVmValueStorage *published_source;
    XrVmValueStorage *destination;
    XrVmValueCell **cells;
    size_t count;
    size_t capacity;
    uint64_t cost;
    uint64_t budget;
    bool retain_published;
} XrVmValueTransfer;

static bool vm_value_transfer_visit(XrVmValueTransfer *transfer, XrVmValue value) {
    XrVmValueCell *cell = vm_value_cell(value);
    if (!cell || cell->storage == transfer->destination || cell->marked ||
        (transfer->retain_published && cell->storage == transfer->published_source))
        return true;
    if ((cell->storage != transfer->source && cell->storage != transfer->published_source) ||
        cell->cell_count > transfer->budget - transfer->cost)
        return false;
    if (transfer->count == transfer->capacity) {
        size_t capacity = transfer->capacity ? transfer->capacity * 2u : 8u;
        if (capacity < transfer->capacity || capacity > SIZE_MAX / sizeof(*transfer->cells))
            return false;
        XrVmValueCell **cells = xr_realloc(transfer->cells, capacity * sizeof(*cells));
        if (!cells)
            return false;
        transfer->cells = cells;
        transfer->capacity = capacity;
    }
    transfer->cells[transfer->count++] = cell;
    transfer->cost += cell->cell_count;
    cell->marked = true;
    return true;
}

/* Collect before transferring: allocation failure leaves both owners unchanged.
 * Only the published graph survives an entry, never its unrelated temporaries. */
static bool vm_transfer_value(XrVmContext *context, XrVmValue value,
                                XrVmValueStorage *destination, bool retain_published) {
    if (destination->aggregate_cell_count > context->code->options.max_value_cells)
        return false;
    XrVmValueTransfer transfer = {
        .source = context->storage,
        .published_source = context->modules ? &context->modules->storage : NULL,
        .destination = destination,
        .retain_published = retain_published,
        .budget = context->code->options.max_value_cells - destination->aggregate_cell_count,
    };
    bool success = vm_value_transfer_visit(&transfer, value);
    for (size_t index = 0u; success && index < transfer.count; ++index) {
        XrVmValueCell *cell = transfer.cells[index];
        const XrVmValue *fields = NULL;
        uint32_t count = 0u;
        switch (cell->kind) {
            case XR_VM_VALUE_AGGREGATE: {
                XrVmAggregateValue *aggregate = (XrVmAggregateValue *) cell;
                fields = aggregate->fields;
                count = aggregate->field_count;
                break;
            }
            case XR_VM_VALUE_CLASS_REFERENCE: {
                XrVmClassValue *instance = (XrVmClassValue *) cell;
                fields = instance->fields;
                count = instance->field_count;
                break;
            }
            case XR_VM_VALUE_CALLABLE: {
                XrVmCallableValue *callable = (XrVmCallableValue *) cell;
                fields = &callable->capture;
                count = callable->has_capture ? 1u : 0u;
                break;
            }
            case XR_VM_VALUE_EXISTENTIAL: {
                XrVmExistentialValue *existential = (XrVmExistentialValue *) cell;
                fields = &existential->owned_storage.value;
                count = existential->owned_storage.initialized ? 1u : 0u;
                break;
            }
            default:
                break;
        }
        for (uint32_t field = 0u; success && field < count; ++field)
            success = vm_value_transfer_visit(&transfer, fields[field]);
    }
    for (size_t index = 0u; index < transfer.count; ++index) {
        XrVmValueCell *cell = transfer.cells[index];
        cell->marked = false;
        if (success) {
            vm_value_cell_unlink(cell);
            vm_value_cell_link(destination, cell, cell->kind, cell->cell_count);
        }
    }
    xr_free(transfer.cells);
    return success;
}

static bool vm_publish_value(XrVmContext *context, XrVmValue value) {
    return vm_transfer_value(context, value, &context->modules->storage, false);
}

static void vm_module_state_clear(XrVmModuleState *state) {
    XrVmContext context = {.code = state->code, .modules = state, .storage = &state->storage};
    while (state->publication_count) {
        XrVmPlace *slot = &state->slots[state->publication_order[--state->publication_count]];
        if (slot->initialized) {
            drop_vm_value(&context, &slot->value, XR_VM_EVENT_ORIGIN_DOMAIN_TEARDOWN);
            slot->initialized = false;
        }
    }
    free_value_storage(&state->storage);
}

static void vm_module_state_destroy(void *opaque) {
    XrVmModuleState *state = opaque;
    vm_module_state_clear(state);
    XrVmContext context = {.code = state->code, .modules = state,
                          .storage = &state->failure_storage};
    drop_vm_value(&context, &state->failure_error, XR_VM_EVENT_ORIGIN_DOMAIN_TEARDOWN);
    drop_vm_value(&context, &state->failure_panic, XR_VM_EVENT_ORIGIN_DOMAIN_TEARDOWN);
    free_value_storage(&state->failure_storage);
    xr_free(state->publication_order);
    xr_free(state->slots);
    xr_vm_code_free(state->code);
    xr_free(state);
}

static bool vm_bind_module_state(XrVmContext *context) {
    static const uint8_t layout_key = 0u;
    if (!context->code->program->module_count)
        return true;
    void *bound = NULL;
    XrExecutionStateBindingResult result =
        xr_execution_lease_bind_state(context->lease, &layout_key, NULL, NULL, &bound);
    if (result == XR_EXECUTION_STATE_EMPTY) {
        uint32_t count = context->code->program->module_slot_count;
        if (count > context->code->options.max_value_cells)
            return false;
        XrVmModuleState *candidate = xr_calloc(1u, sizeof(*candidate));
        if (!candidate)
            return false;
        candidate->code = xr_vm_code_retain(context->code);
        candidate->storage.persistent = true;
        candidate->failure_storage.persistent = true;
        atomic_init(&candidate->next_class_identity, 0u);
        candidate->failure_kind = XR_VM_OUTCOME_TRAP;
        candidate->failure_trap = XR_VM_TRAP_EXPLICIT;
        candidate->slots = xr_calloc(count ? count : 1u, sizeof(*candidate->slots));
        candidate->publication_order =
            xr_calloc(count ? count : 1u, sizeof(*candidate->publication_order));
        if (!candidate->slots || !candidate->publication_order) {
            vm_module_state_destroy(candidate);
            return false;
        }
        for (uint32_t index = 0u; index < count; ++index) {
            candidate->slots[index].module_state = candidate;
            candidate->slots[index].module_slot = index;
        }
        result = xr_execution_lease_bind_state(context->lease, &layout_key, candidate,
                                               vm_module_state_destroy, &bound);
        if (result != XR_EXECUTION_STATE_ADOPTED)
            vm_module_state_destroy(candidate);
    }
    if (result != XR_EXECUTION_STATE_PRESENT && result != XR_EXECUTION_STATE_ADOPTED)
        return false;
    context->modules = bound;
    return context->modules->storage.aggregate_cell_count <= context->code->options.max_value_cells;
}

static XrVmPlace *vm_module_slot(XrVmContext *context, uint32_t module, uint32_t slot) {
    if (!context->modules || module >= context->code->program->module_count ||
        slot >= context->code->program->modules[module].slot_count)
        return NULL;
    uint32_t index = slot;
    for (uint32_t previous = 0u; previous < module; ++previous)
        index += context->code->program->modules[previous].slot_count;
    return &context->modules->slots[index];
}

/* Outcome of a text operation in the common dispatcher. */
typedef enum VmTextStatus {
    VM_TEXT_OK = 0,
    VM_TEXT_RESOURCE_LIMIT,
    VM_TEXT_PROVIDER_FAILED,
    VM_TEXT_INVALID,
} VmTextStatus;

/* Executes the string/rune/output family through the shared text kernel. */
static bool vm_display_operand(const XrVmValue *value, XrTextDisplayOperand *operand) {
    memset(operand, 0, sizeof(*operand));
    switch (value->kind) {
        case XR_VM_VALUE_I8:
            operand->kind = XR_TEXT_DISPLAY_I64;
            operand->i64 = value->as.i8;
            break;
        case XR_VM_VALUE_I16:
            operand->kind = XR_TEXT_DISPLAY_I64;
            operand->i64 = value->as.i16;
            break;
        case XR_VM_VALUE_I32:
            operand->kind = XR_TEXT_DISPLAY_I64;
            operand->i64 = value->as.i32;
            break;
        case XR_VM_VALUE_U8:
            operand->kind = XR_TEXT_DISPLAY_U64;
            operand->u64 = value->as.u8;
            break;
        case XR_VM_VALUE_U16:
            operand->kind = XR_TEXT_DISPLAY_U64;
            operand->u64 = value->as.u16;
            break;
        case XR_VM_VALUE_U32:
            operand->kind = XR_TEXT_DISPLAY_U64;
            operand->u64 = value->as.u32;
            break;
        case XR_VM_VALUE_U64:
            operand->kind = XR_TEXT_DISPLAY_U64;
            operand->u64 = value->as.u64;
            break;
        case XR_VM_VALUE_I64:
            operand->kind = XR_TEXT_DISPLAY_I64;
            operand->i64 = value->as.i64;
            break;
        case XR_VM_VALUE_F64:
            operand->kind = XR_TEXT_DISPLAY_F64;
            memcpy(&operand->f64, &value->as.f64_bits, sizeof(operand->f64));
            break;
        case XR_VM_VALUE_BOOL:
            operand->kind = XR_TEXT_DISPLAY_BOOL;
            operand->boolean = value->as.boolean ? 1 : 0;
            break;
        case XR_VM_VALUE_RUNE:
            operand->kind = XR_TEXT_DISPLAY_RUNE;
            operand->rune = value->as.rune;
            break;
        case XR_VM_VALUE_STRING: {
            const uint8_t *bytes = NULL;
            size_t size = 0u;
            if (!vm_string_view(*value, &bytes, &size)) {
                return false;
            }
            operand->kind = XR_TEXT_DISPLAY_STRING;
            operand->bytes = bytes;
            operand->size = size;
            break;
        }
        default:
            return false;
    }
    return true;
}

static VmTextStatus vm_execute_text_operation(XrVmContext *context, uint16_t operation_id,
                                              const uint32_t *operands, uint32_t operand_count,
                                              uint32_t constant_id, uint32_t predicate,
                                              uint32_t requirement_index, uint32_t operation_index,
                                              const XrVmRuntimeValue *values, XrVmValue *produced) {
    switch (operation_id) {
        case XR_CORE_OP_CORE_SEQUENCE_LENGTH: {
            XrVmValue sequence = values[operands[0]].as.value;
            produced->kind = XR_VM_VALUE_I64;
            if (sequence.kind == XR_VM_VALUE_STRING && sequence.as.string) {
                produced->as.i64 = (int64_t)((const XrVmStringValue *)sequence.as.string)->scalar_count;
            } else if (sequence.kind == XR_VM_VALUE_AGGREGATE && sequence.as.aggregate) {
                produced->as.i64 = (int64_t)((const XrVmAggregateValue *)sequence.as.aggregate)->field_count;
            } else {
                return VM_TEXT_INVALID;
            }
            return VM_TEXT_OK;
        }
        case XR_CORE_OP_CORE_CONSTANT_STRING: {
            const XrValidatedConstant *constant = &context->code->program->constants[constant_id];
            XrVmStringValue *string = allocate_string(context, constant->value.string.size);
            if (!string)
                return VM_TEXT_RESOURCE_LIMIT;
            if (constant->value.string.size != 0u)
                memcpy(string->bytes, constant->value.string.bytes, constant->value.string.size);
            string->scalar_count = (uint32_t) xr_text_scalar_count(string->bytes, string->size);
            *produced = vm_string_value(string);
            return VM_TEXT_OK;
        }
        case XR_CORE_OP_CORE_CONSTANT_RUNE: {
            const XrValidatedConstant *constant = &context->code->program->constants[constant_id];
            produced->kind = XR_VM_VALUE_RUNE;
            produced->as.rune = constant->value.rune;
            return VM_TEXT_OK;
        }
        case XR_CORE_OP_CORE_STRING_FROM_SCALAR: {
            XrTextDisplayOperand source;
            if (!vm_display_operand(&values[operands[0]].as.value, &source)) return VM_TEXT_INVALID;
            size_t size = xr_text_display_operand(&source, NULL);
            XrVmStringValue *string = allocate_string(context, size);
            if (!string)
                return VM_TEXT_RESOURCE_LIMIT;
            (void) xr_text_display_operand(&source, string->bytes);
            string->scalar_count = (uint32_t) xr_text_scalar_count(string->bytes, size);
            *produced = vm_string_value(string);
            return VM_TEXT_OK;
        }
        case XR_CORE_OP_CORE_STRING_CONCAT: {
            const uint8_t *left = NULL;
            const uint8_t *right = NULL;
            size_t left_size = 0u;
            size_t right_size = 0u;
            int ok = 0;
            if (!vm_string_view(values[operands[0]].as.value, &left, &left_size) ||
                !vm_string_view(values[operands[1]].as.value, &right, &right_size))
                return VM_TEXT_INVALID;
            size_t size = xr_text_concat_size(left_size, right_size, &ok);
            if (!ok)
                return VM_TEXT_RESOURCE_LIMIT;
            XrVmStringValue *string = allocate_string(context, size);
            if (!string)
                return VM_TEXT_RESOURCE_LIMIT;
            xr_text_concat(left, left_size, right, right_size, string->bytes);
            const XrVmStringValue *left_value = values[operands[0]].as.value.as.string;
            const XrVmStringValue *right_value = values[operands[1]].as.value.as.string;
            string->scalar_count = left_value->scalar_count + right_value->scalar_count;
            *produced = vm_string_value(string);
            return VM_TEXT_OK;
        }
        case XR_CORE_OP_CORE_COMPARE_STRING: {
            const uint8_t *left = NULL;
            const uint8_t *right = NULL;
            size_t left_size = 0u;
            size_t right_size = 0u;
            if (!vm_string_view(values[operands[0]].as.value, &left, &left_size) ||
                !vm_string_view(values[operands[1]].as.value, &right, &right_size))
                return VM_TEXT_INVALID;
            produced->kind = XR_VM_VALUE_BOOL;
            produced->as.boolean =
                xr_text_predicate(xr_text_compare(left, left_size, right, right_size), predicate) !=
                0;
            return VM_TEXT_OK;
        }
        case XR_CORE_OP_CORE_COMPARE_RUNE: {
            uint32_t left = values[operands[0]].as.value.as.rune;
            uint32_t right = values[operands[1]].as.value.as.rune;
            int order = left == right ? 0 : (left < right ? -1 : 1);
            produced->kind = XR_VM_VALUE_BOOL;
            produced->as.boolean = xr_text_predicate(order, predicate) != 0;
            return VM_TEXT_OK;
        }
        case XR_CORE_OP_CORE_OUTPUT_GROUP: {
            XrTextDisplayOperand stack_operands[8];
            XrTextDisplayOperand *display = stack_operands;
            int ok = 0;
            if (operand_count > sizeof(stack_operands) / sizeof(stack_operands[0])) {
                display = xr_calloc(operand_count, sizeof(*display));
                if (!display)
                    return VM_TEXT_RESOURCE_LIMIT;
            }
            for (uint32_t index = 0u; index < operand_count; ++index) {
                const XrVmValue *value = &values[operands[index]].as.value;
                XrTextDisplayOperand *operand = &display[index];
                if (!vm_display_operand(value, operand)) {
                    if (display != stack_operands) xr_free(display);
                    return VM_TEXT_INVALID;
                }
            }
            size_t size = xr_text_group_size(display, operand_count, &ok);
            uint8_t *line = ok ? xr_malloc(size) : NULL;
            if (!line) {
                if (display != stack_operands)
                    xr_free(display);
                return VM_TEXT_RESOURCE_LIMIT;
            }
            (void) xr_text_group_render(display, operand_count, line);
            XrExecutionProviderCallResult call = xr_execution_lease_provider_output_write(
                context->lease, requirement_index, operation_index, line, size);
            xr_free(line);
            if (display != stack_operands)
                xr_free(display);
            return call == XR_EXECUTION_PROVIDER_CALL_OK ? VM_TEXT_OK : VM_TEXT_PROVIDER_FAILED;
        }
        default:
            return VM_TEXT_INVALID;
    }
}

static XrVmOutcome vm_text_outcome(const XrVmContext *context, VmTextStatus status) {
    switch (status) {
        case VM_TEXT_RESOURCE_LIMIT:
            return vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
        case VM_TEXT_PROVIDER_FAILED:
            return vm_trap(XR_VM_TRAP_PROVIDER_CALL_FAILED, context);
        default:
            return vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, context);
    }
}

static int64_t i64_from_bits(uint64_t bits) {
    if (bits <= (uint64_t) INT64_MAX)
        return (int64_t) bits;
    return -(int64_t) (~bits) - 1;
}

static bool checked_add(int64_t left, int64_t right, int64_t *result) {
    if ((right > 0 && left > INT64_MAX - right) || (right < 0 && left < INT64_MIN - right))
        return false;
    *result = left + right;
    return true;
}

static bool checked_sub(int64_t left, int64_t right, int64_t *result) {
    if ((right < 0 && left > INT64_MAX + right) || (right > 0 && left < INT64_MIN + right))
        return false;
    *result = left - right;
    return true;
}

static bool checked_mul(int64_t left, int64_t right, int64_t *result) {
    if (left == 0 || right == 0) {
        *result = 0;
        return true;
    }
    if ((left == -1 && right == INT64_MIN) || (right == -1 && left == INT64_MIN))
        return false;
    if (left > 0) {
        if ((right > 0 && left > INT64_MAX / right) || (right < 0 && right < INT64_MIN / left))
            return false;
    } else if ((right > 0 && left < INT64_MIN / right) || (right < 0 && left < INT64_MAX / right)) {
        return false;
    }
    *result = left * right;
    return true;
}

static void hash_u32(XrSHA256Context *context, uint32_t value) {
    uint8_t bytes[4];
    for (size_t index = 0; index < sizeof(bytes); ++index)
        bytes[index] = (uint8_t) (value >> (index * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void emit_lifecycle(XrVmContext *context, XrVmLifecycleEventKind kind,
                           XrVmLifecycleEventOrigin origin, const XrVmClassValue *value,
                           uint64_t related_identity, uint32_t field_ordinal) {
    if (!context || !context->code || !context->code->options.lifecycle_event)
        return;
    XrVmLifecycleEvent event = {
        .kind = kind,
        .origin = origin,
        .type_id = value ? value->type_id : XR_CORE_TYPE_VOID,
        .field_ordinal = field_ordinal,
        .identity = value ? value->identity : UINT64_MAX,
        .related_identity = related_identity,
    };
    context->code->options.lifecycle_event(context->code->options.lifecycle_context, &event);
}

static void emit_place_exchange(XrVmContext *context, uint16_t type_id, XrVmValue previous,
                                XrVmValue replacement) {
    if (!context || !context->code || !context->code->options.lifecycle_event)
        return;
    const XrVmClassValue *previous_class =
        previous.kind == XR_VM_VALUE_CLASS_REFERENCE ? previous.as.class_reference : NULL;
    const XrVmClassValue *replacement_class =
        replacement.kind == XR_VM_VALUE_CLASS_REFERENCE ? replacement.as.class_reference : NULL;
    XrVmLifecycleEvent event = {
        .kind = XR_VM_EVENT_PLACE_EXCHANGE,
        .origin = XR_VM_EVENT_ORIGIN_PROGRAM_OPERATION,
        .type_id = type_id,
        .field_ordinal = UINT32_MAX,
        .identity = previous_class ? previous_class->identity : UINT64_MAX,
        .related_identity = replacement_class ? replacement_class->identity : UINT64_MAX,
        .previous_value_kind = (uint32_t) previous.kind,
        .replacement_value_kind = (uint32_t) replacement.kind,
        .previous_i64 = previous.kind == XR_VM_VALUE_I64 ? previous.as.i64 : 0,
        .replacement_i64 = replacement.kind == XR_VM_VALUE_I64 ? replacement.as.i64 : 0,
    };
    context->code->options.lifecycle_event(context->code->options.lifecycle_context, &event);
}

static void trace_instruction(XrVmContext *context, uint32_t function_id, uint32_t block_id,
                              uint32_t instruction_id, uint16_t operation_id) {
    hash_u32(&context->trace, function_id);
    hash_u32(&context->trace, block_id);
    hash_u32(&context->trace, instruction_id);
    hash_u32(&context->trace, operation_id);
}

static XrVmInstructionView instruction_view(const XrVmCode *code, uint32_t function_id,
                                            uint32_t block_id, uint32_t instruction_id) {
    XrVmInstructionView view = {0};
    const XrValidatedInstruction *source =
        &code->program->functions[function_id].blocks[block_id].instructions[instruction_id];
    view.operation_id = source->operation_id;
    view.result_type_id = source->result_type_id;
    view.result_category = source->result_category;
    view.result_id = source->result_id;
    view.operands = source->operands;
    view.operand_count = source->operand_count;
    view.immediate_kind = source->immediate_kind;
    memcpy(&view.immediate, &source->immediate, sizeof(view.immediate));
    view.successors = source->successors;
    view.successor_count = source->successor_count;
    return view;
}

#include "xr_program_vm_provider.inc.c"

static XrVmOutcome vm_execution_step(XrVmExecution *execution);
static bool vm_execution_allocate_values(XrVmExecution *execution,
                                         const XrValidatedFunction *function);
static void vm_execution_free_values(XrVmExecution *execution);

static XrVmOutcome execute_function(XrVmContext *context, uint32_t function_id,
                                    const XrVmRuntimeValue *arguments, uint32_t argument_count,
                                    uint32_t depth, bool *entered) {
    *entered = false;
    if (depth > context->code->options.max_call_depth)
        return vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
    const XrValidatedFunction *function = &context->code->program->functions[function_id];
    if (argument_count != function->parameter_count)
        return vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, context);
    for (uint32_t index = 0; index < argument_count; ++index) {
        XrCoreIrValueCategory expected =
            function_parameter_category(context->code->program, function, index);
        if (arguments[index].category != expected ||
            (expected == XR_CORE_IR_PLACE &&
             (!arguments[index].as.place || !arguments[index].as.place->initialized)))
            return vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, context);
        XrVmValue value = expected == XR_CORE_IR_PLACE
                              ? *vm_place_value_const(arguments[index].as.place)
                              : arguments[index].as.value;
        if (!value_matches_type(context->code->program, value, function->parameter_types[index]))
            return vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, context);
    }

    if (function->coroutine_safepoint_count != 0u)
        return vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, context);
    XrVmExecution execution = {
        .context = *context,
        .lease = *context->lease,
        .code = xr_vm_code_retain(context->code),
        .function_id = function_id,
        .block_id = function->entry_block,
        .depth = depth,
        .cancel_block_id = XR_PROGRAM_LOCATION_NONE,
        .suspension_block_id = XR_PROGRAM_LOCATION_NONE,
        .suspension_instruction_id = XR_PROGRAM_LOCATION_NONE,
    };
    if (function->value_count > context->code->options.max_value_cells ||
        !vm_execution_allocate_values(&execution, function)) {
        vm_execution_free_values(&execution);
        xr_vm_code_free(execution.code);
        return vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
    }
    for (uint32_t argument = 0u; argument < argument_count; ++argument) {
        uint32_t value_id = function->blocks[function->entry_block].argument_ids[argument];
        execution.values[value_id] = arguments[argument];
        execution.initialized[value_id] = true;
    }
    execution.values_ready = true;
    *entered = true;
    XrVmOutcome result = vm_execution_step(&execution);
    *context = execution.context;
    vm_execution_free_values(&execution);
    xr_vm_code_free(execution.code);
    return result;
}

static void compute_private_digest(XrVmCode *code) {
    static const uint8_t domain[] = "xray-private-vm-code-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    xr_sha256_update(&context, code->execution_id.bytes, sizeof(code->execution_id.bytes));
    xr_sha256_update(&context, (const uint8_t *) XR_VM_BUILD_ID, sizeof(XR_VM_BUILD_ID));
    xr_sha256_update(&context, &code->options.quickening_policy,
                     sizeof(code->options.quickening_policy));
    hash_u32(&context, code->pointer_width);
    hash_u32(&context, code->operating_system);
    hash_u32(&context, code->architecture);
    hash_u32(&context, code->native_abi);
    hash_u32(&context, code->endianness);
    xr_sha256_final(&context, code->private_digest.bytes);
}

XrVmCodeOptions xr_vm_code_default_options(void) {
    XrVmCodeOptions options = {
        .schema_version = XR_VM_CODE_OPTIONS_SCHEMA_VERSION,
        .quickening_policy = XR_VM_QUICKENING_NONE,
        .max_steps = UINT64_C(1000000),
        .max_value_cells = UINT32_C(1048576),
        .max_call_depth = 1024u,
    };
    return options;
}

static bool vm_program_operations_active(const XrValidatedProgram *program,
                                         XrVmCodeDiagnostic *diagnostic_out) {
    for (uint32_t function = 0u; function < program->function_count; ++function) {
        const XrValidatedFunction *function_row = &program->functions[function];
        for (uint32_t block = 0u; block < function_row->block_count; ++block) {
            const XrValidatedBlock *block_row = &function_row->blocks[block];
            for (uint32_t instruction = 0u; instruction < block_row->instruction_count;
                 ++instruction) {
                const XrValidatedInstruction *instruction_row =
                    &block_row->instructions[instruction];
                uint16_t operation_id = instruction_row->operation_id;
                const XrCoreOperationSpec *spec = xr_core_spec_operation_by_id(operation_id);
                bool implemented_operation = operation_id == XR_CORE_OP_CORE_CLASS_CONSTRUCT ||
                                             operation_id == XR_CORE_OP_CORE_OWNER_ALIAS ||
                                             operation_id == XR_CORE_OP_CORE_CLASS_FIELD_LOAD ||
                                             operation_id == XR_CORE_OP_CORE_CLASS_FIELD_PLACE ||
                                             operation_id == XR_CORE_OP_CORE_PLACE_EXCHANGE;
                const XrValidatedType *result_type =
                    xr_validated_program_type(program, instruction_row->result_type_id);
                bool deferred_class_copy = operation_id == XR_CORE_OP_CORE_OWNER_COPY &&
                                           result_type &&
                                           xr_program_type_kind_is_reference_record(result_type->kind);
                if (!deferred_class_copy &&
                    ((spec && spec->vm_status == XR_CORE_COVERAGE_COMPLETE) ||
                     implemented_operation))
                    continue;
                if (diagnostic_out) {
                    diagnostic_out->status = XR_VM_CODE_UNSUPPORTED_OPERATION;
                    diagnostic_out->operation_id = operation_id;
                    diagnostic_out->function_id = function;
                    diagnostic_out->block_id = block;
                    diagnostic_out->instruction_id = instruction;
                }
                return false;
            }
        }
    }
    return true;
}

XrVmCodeStatus xr_vm_code_build(const XrValidatedProgram *program, const XrTargetProfile *profile,
                                const XrVmCodeOptions *options, XrVmCode **code_out,
                                XrVmCodeDiagnostic *diagnostic_out) {
    if (code_out)
        *code_out = NULL;
    if (diagnostic_out)
        memset(diagnostic_out, 0, sizeof(*diagnostic_out));
    XrVmCodeOptions selected = options ? *options : xr_vm_code_default_options();
    if (!program || !profile || !code_out ||
        selected.schema_version != XR_VM_CODE_OPTIONS_SCHEMA_VERSION || selected.reserved16 != 0u || selected.reserved8 != 0u ||
        selected.max_steps == 0u || selected.max_value_cells == 0u ||
        selected.max_call_depth == 0u) {
        if (diagnostic_out)
            diagnostic_out->status = XR_VM_CODE_INVALID_INPUT;
        return XR_VM_CODE_INVALID_INPUT;
    }
    if (selected.quickening_policy != XR_VM_QUICKENING_NONE) {
        if (diagnostic_out)
            diagnostic_out->status = XR_VM_CODE_POLICY_REJECTED;
        return XR_VM_CODE_POLICY_REJECTED;
    }
    XrExecutionId execution_id;
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(profile);
    if (!machine || !xr_execution_id_compute(program, profile, &execution_id)) {
        if (diagnostic_out)
            diagnostic_out->status = XR_VM_CODE_INVALID_INPUT;
        return XR_VM_CODE_INVALID_INPUT;
    }
    if (!vm_program_operations_active(program, diagnostic_out))
        return XR_VM_CODE_UNSUPPORTED_OPERATION;
    for (uint32_t index = 0u; index < program->type_count; ++index) {
        if (program->types[index].parent_type_id != XR_CORE_TYPE_VOID) {
            if (diagnostic_out)
                diagnostic_out->status = XR_VM_CODE_UNSUPPORTED_OPERATION;
            return XR_VM_CODE_UNSUPPORTED_OPERATION;
        }
    }
    XrVmCode *code = xr_calloc(1u, sizeof(XrVmCode));
    if (!code) {
        if (diagnostic_out)
            diagnostic_out->status = XR_VM_CODE_OUT_OF_MEMORY;
        return XR_VM_CODE_OUT_OF_MEMORY;
    }
    atomic_init(&code->references, 1u);
    code->program = xr_validated_program_retain(program);
    code->execution_id = execution_id;
    code->options = selected;
    code->pointer_width = (uint16_t) (machine->data_layout.pointer.size * UINT16_C(8));
    code->operating_system = machine->operating_system;
    code->architecture = machine->architecture;
    code->native_abi = machine->native_abi;
    code->endianness = (uint16_t) machine->data_layout.endian;
    compute_private_digest(code);
    *code_out = code;
    return XR_VM_CODE_OK;
}

void xr_vm_code_free(XrVmCode *code) {
    if (!code)
        return;
    if (atomic_fetch_sub_explicit(&code->references, 1u, memory_order_acq_rel) != 1u)
        return;
    xr_validated_program_free(code->program);
    xr_free(code);
}

XrVmCode *xr_vm_code_retain(const XrVmCode *code) {
    if (!code)
        return NULL;
    atomic_fetch_add_explicit((atomic_uint_least32_t *) &code->references, 1u,
                              memory_order_relaxed);
    return (XrVmCode *) code;
}

bool xr_vm_code_matches_instance(const XrVmCode *code, const XrInstance *instance) {
    if (!code || !instance)
        return false;
    return xr_fingerprint_equal(xr_execution_instance_id(instance), code->execution_id);
}

XrExecutionId xr_vm_code_execution_id(const XrVmCode *code) {
    XrExecutionId id = {{0}};
    return code ? code->execution_id : id;
}

XrFingerprint xr_vm_code_private_digest(const XrVmCode *code) {
    XrFingerprint digest = {{0}};
    return code ? code->private_digest : digest;
}

static void vm_execution_release_lease(XrVmExecution *execution) {
    if (execution && execution->owns_lease && xr_execution_lease_is_valid(&execution->lease))
        (void) xr_execution_lease_release(&execution->lease);
}

static bool vm_execution_allocate_values(XrVmExecution *execution,
                                         const XrValidatedFunction *function) {
    size_t count = function->value_count ? function->value_count : 1u;
    size_t edge_count = 1u;
    for (uint32_t block = 0u; block < function->block_count; ++block) {
        const XrValidatedBlock *row = &function->blocks[block];
        if (row->argument_count > edge_count)
            edge_count = row->argument_count;
        for (uint32_t instruction = 0u; instruction < row->instruction_count; ++instruction)
            if (row->instructions[instruction].operand_count > edge_count)
                edge_count = row->instructions[instruction].operand_count;
    }
    if (edge_count > execution->context.code->options.max_value_cells ||
        count > SIZE_MAX / sizeof(*execution->values) ||
        count > SIZE_MAX / sizeof(*execution->places) ||
        count > SIZE_MAX / sizeof(*execution->initialized) ||
        edge_count > SIZE_MAX / sizeof(*execution->edge_values))
        return false;
    execution->values = xr_calloc(count, sizeof(*execution->values));
    execution->places = xr_calloc(count, sizeof(*execution->places));
    execution->initialized = xr_calloc(count, sizeof(*execution->initialized));
    execution->edge_values = xr_calloc(edge_count, sizeof(*execution->edge_values));
    return execution->values && execution->places && execution->initialized &&
           execution->edge_values;
}

static void vm_execution_free_values(XrVmExecution *execution) {
    xr_free(execution->initialized);
    xr_free(execution->places);
    xr_free(execution->edge_values);
    xr_free(execution->values);
}

static XrVmExecution *vm_execution_create_child_frame(XrVmExecution *parent, uint32_t function_id) {
    if (!parent || function_id >= parent->context.code->program->function_count ||
        parent->depth == parent->context.code->options.max_call_depth)
        return NULL;
    const XrValidatedFunction *function = &parent->context.code->program->functions[function_id];
    if (function->value_count > parent->context.code->options.max_value_cells)
        return NULL;
    XrVmExecution *child = xr_calloc(1u, sizeof(*child));
    if (!child)
        return NULL;
    child->lease = parent->lease;
    child->context.code = parent->context.code;
    child->context.lease = &child->lease;
    child->context.storage = parent->context.storage;
    child->context.modules = parent->context.modules;
    child->code = xr_vm_code_retain(parent->code);
    child->depth = parent->depth + 1u;
    child->function_id = function_id;
    child->block_id = function->entry_block;
    child->cancel_block_id = XR_PROGRAM_LOCATION_NONE;
    child->suspension_block_id = XR_PROGRAM_LOCATION_NONE;
    child->suspension_instruction_id = XR_PROGRAM_LOCATION_NONE;
    if (!child->code || !vm_execution_allocate_values(child, function)) {
        xr_vm_execution_free(child);
        return NULL;
    }
    static const uint8_t trace_domain[] = "xray-vm-coroutine-child-logical-trace-v1\0";
    xr_sha256_init(&child->context.trace);
    xr_sha256_update(&child->context.trace, trace_domain, sizeof(trace_domain) - 1u);
    xr_sha256_update(&child->context.trace, parent->code->execution_id.bytes,
                     sizeof(parent->code->execution_id.bytes));
    hash_u32(&child->context.trace, function_id);
    child->values_ready = function->parameter_count == 0u;
    return child;
}

static bool vm_child_execution_create(XrVmExecution *parent, const XrVmInstructionView *instruction,
                                      XrVmExecution **child_out) {
    if (child_out)
        *child_out = NULL;
    if (!parent || !instruction || !child_out)
        return false;
    bool indirect = instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT;
    const XrVmCallableValue *carrier = NULL;
    uint32_t source_argument = 0u;
    uint32_t target_argument = 0u;
    uint32_t function_id = instruction->immediate.coroutine_call.function_id;
    if (indirect) {
        if (instruction->operand_count == 0u || !parent->initialized[instruction->operands[0]] ||
            parent->values[instruction->operands[0]].category != XR_CORE_IR_VALUE ||
            parent->values[instruction->operands[0]].as.value.kind != XR_VM_VALUE_CALLABLE)
            return false;
        carrier = parent->values[instruction->operands[0]].as.value.as.callable;
        function_id = callable_function_id(parent->context.code->program, carrier);
        source_argument = 1u;
    }
    if (function_id >= parent->context.code->program->function_count)
        return false;
    const XrValidatedFunction *function = &parent->context.code->program->functions[function_id];
    uint32_t visible_parameters =
        function->parameter_count - (carrier && carrier->has_capture ? 1u : 0u);
    if (instruction->operand_count < source_argument + visible_parameters ||
        function->value_count > parent->context.code->options.max_value_cells ||
        parent->depth == parent->context.code->options.max_call_depth)
        return false;
    XrVmExecution *child = vm_execution_create_child_frame(parent, function_id);
    if (!child)
        return false;
    if (carrier && carrier->has_capture) {
        uint32_t target = function->blocks[function->entry_block].argument_ids[0];
        child->values[target].category = XR_CORE_IR_VALUE;
        child->values[target].as.value = carrier->capture;
        child->initialized[target] = true;
        target_argument = 1u;
    }
    for (; target_argument < function->parameter_count; ++target_argument, ++source_argument) {
        uint32_t source = instruction->operands[source_argument];
        uint32_t target = function->blocks[function->entry_block].argument_ids[target_argument];
        XrCoreIrValueCategory expected =
            function_parameter_category(parent->context.code->program, function, target_argument);
        XrVmValue value = expected == XR_CORE_IR_PLACE && parent->values[source].as.place
                              ? *vm_place_value_const(parent->values[source].as.place)
                              : parent->values[source].as.value;
        if (!parent->initialized[source] ||
            (function->parameter_modes[target_argument] != XR_PARAM_READ &&
             function->parameter_modes[target_argument] != XR_PARAM_REF) ||
            parent->values[source].category != expected ||
            !value_matches_type(parent->context.code->program, value,
                                function->parameter_types[target_argument])) {
            xr_vm_execution_free(child);
            return false;
        }
        child->values[target] = parent->values[source];
        child->initialized[target] = true;
    }
    child->values_ready = true;
    *child_out = child;
    return true;
}

static XrVmOutcome vm_execution_outcome(XrVmExecution *execution, XrVmOutcomeKind kind) {
    XrVmOutcome result = vm_outcome(kind, execution ? &execution->context : NULL);
    if (!execution)
        return result;
    result.state_id = execution->state_id;
    hash_u32(&execution->context.trace, (uint32_t) kind);
    hash_u32(&execution->context.trace, result.state_id);
    XrSHA256Context snapshot = execution->context.trace;
    xr_sha256_final(&snapshot, result.logical_trace.bytes);
    return result;
}

bool xr_vm_execution_create(const XrVmCode *code, XrInstance *instance, uint32_t function_id,
                            const XrVmValue *arguments, uint32_t argument_count,
                            XrVmExecution **execution_out) {
    if (execution_out)
        *execution_out = NULL;
    if (!code || !instance || !execution_out || function_id >= code->program->function_count ||
        (argument_count != 0u && !arguments) || !xr_vm_code_matches_instance(code, instance))
        return false;
    const XrValidatedFunction *function = &code->program->functions[function_id];
    if (argument_count != function->parameter_count ||
        (function->coroutine_safepoint_count != 0u &&
         function->coroutine_state_count != function->coroutine_safepoint_count + 1u) ||
        function->value_count > code->options.max_value_cells)
        return false;
    XrVmExecution *execution = xr_calloc(1u, sizeof(*execution));
    if (!execution)
        return false;
    if (xr_execution_instance_acquire(instance, &execution->lease) != XR_EXECUTION_OK) {
        xr_free(execution);
        return false;
    }
    execution->owns_lease = true;
    execution->context.code = code;
    execution->context.lease = &execution->lease;
    execution->context.storage = &execution->storage;
    execution->code = xr_vm_code_retain(code);
    execution->function_id = function_id;
    execution->depth = 1u;
    execution->block_id = function->entry_block;
    execution->cancel_block_id = XR_PROGRAM_LOCATION_NONE;
    execution->suspension_block_id = XR_PROGRAM_LOCATION_NONE;
    execution->suspension_instruction_id = XR_PROGRAM_LOCATION_NONE;
    if (!vm_bind_module_state(&execution->context) ||
        !vm_execution_allocate_values(execution, function)) {
        xr_vm_execution_free(execution);
        return false;
    }
    static const uint8_t trace_domain[] = "xray-vm-coroutine-logical-trace-v1\0";
    xr_sha256_init(&execution->context.trace);
    xr_sha256_update(&execution->context.trace, trace_domain, sizeof(trace_domain) - 1u);
    xr_sha256_update(&execution->context.trace, code->execution_id.bytes,
                     sizeof(code->execution_id.bytes));
    hash_u32(&execution->context.trace, function_id);
    for (uint32_t argument = 0; argument < argument_count; ++argument) {
        uint32_t value_id = function->blocks[function->entry_block].argument_ids[argument];
        if ((function->parameter_modes[argument] == XR_PARAM_REF &&
             !function_parameter_is_class_receiver(code->program, function, argument)) ||
            !value_matches_type(code->program, arguments[argument],
                                function->parameter_types[argument])) {
            xr_vm_execution_free(execution);
            return false;
        }
        execution->values[value_id] = (XrVmRuntimeValue) {
            .category = XR_CORE_IR_VALUE,
            .as.value = arguments[argument],
        };
        execution->initialized[value_id] = true;
    }
    execution->values_ready = true;
    *execution_out = execution;
    return true;
}

static bool vm_suspension_instruction(const XrVmExecution *execution,
                                      XrVmInstructionView *instruction) {
    if (!execution || execution->suspension_block_id == XR_PROGRAM_LOCATION_NONE ||
        execution->suspension_instruction_id == XR_PROGRAM_LOCATION_NONE)
        return false;
    const XrValidatedFunction *function =
        &execution->context.code->program->functions[execution->function_id];
    if (execution->suspension_block_id >= function->block_count ||
        execution->suspension_instruction_id >=
            function->blocks[execution->suspension_block_id].instruction_count)
        return false;
    *instruction =
        instruction_view(execution->context.code, execution->function_id,
                         execution->suspension_block_id, execution->suspension_instruction_id);
    return true;
}

static void vm_execution_assign_edge(XrVmExecution *execution, const XrValidatedBlock *target,
                                     const uint32_t *operands, uint32_t start,
                                     const XrVmValue *implicit) {
    uint32_t prefix = implicit ? 1u : 0u;
    if (implicit)
        execution->edge_values[0] =
            (XrVmRuntimeValue) {.category = XR_CORE_IR_VALUE, .as.value = *implicit};
    // A back-edge may permute its own arguments; all reads precede every write.
    for (uint32_t argument = prefix; argument < target->argument_count; ++argument)
        execution->edge_values[argument] = execution->values[operands[start + argument - prefix]];
    for (uint32_t argument = 0u; argument < target->argument_count; ++argument) {
        uint32_t value = target->argument_ids[argument];
        execution->values[value] = execution->edge_values[argument];
        execution->initialized[value] = true;
    }
}

static const XrValidatedSignature *
vm_coroutine_call_signature(const XrVmExecution *execution,
                            const XrVmInstructionView *instruction) {
    const XrValidatedProgram *program = execution->context.code->program;
    if (instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED) {
        uint32_t callee = instruction->immediate.coroutine_call.function_id;
        return callee < program->function_count
                   ? &program->signatures[program->functions[callee].signature_id]
                   : NULL;
    }
    if (instruction->operation_id != XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT ||
        instruction->operand_count == 0u)
        return NULL;
    const XrValidatedFunction *function = &program->functions[execution->function_id];
    const XrValidatedType *callable =
        xr_validated_program_type(program, function->value_types[instruction->operands[0]]);
    return callable && callable->signature_id < program->signature_count
               ? &program->signatures[callable->signature_id]
               : NULL;
}

static uint32_t vm_coroutine_trap_successor(const XrVmExecution *execution,
                                            const XrVmInstructionView *instruction) {
    const XrValidatedSignature *signature = vm_coroutine_call_signature(execution, instruction);
    if (!signature)
        return XR_PROGRAM_LOCATION_NONE;
    uint32_t typed = 2u + (signature->error_type_id != XR_CORE_TYPE_VOID ? 1u : 0u) +
                     (signature->panic_type_id != XR_CORE_TYPE_VOID ? 1u : 0u);
    return instruction->successor_count == typed + 1u ? typed : XR_PROGRAM_LOCATION_NONE;
}

static bool vm_materialize_suspension_edge(XrVmExecution *execution, uint32_t successor_index) {
    XrVmInstructionView instruction;
    if (!vm_suspension_instruction(execution, &instruction))
        return false;
    const XrValidatedFunction *function =
        &execution->context.code->program->functions[execution->function_id];
    if (successor_index >= instruction.successor_count ||
        instruction.successors[successor_index] >= function->block_count)
        return false;
    const XrValidatedBlock *target = &function->blocks[instruction.successors[successor_index]];
    uint32_t operand_start = 0u;
    if (instruction.operation_id == XR_CORE_OP_CORE_COROUTINE_YIELD ||
        instruction.operation_id == XR_CORE_OP_CORE_COROUTINE_SUSPEND) {
        if (instruction.successor_count != 2u)
            return false;
        if (instruction.operation_id == XR_CORE_OP_CORE_COROUTINE_SUSPEND)
            operand_start = instruction.immediate.coroutine_suspend.request_operand_count;
        if (successor_index == 1u)
            operand_start += function->blocks[instruction.successors[0]].argument_count;
    } else if ((instruction.operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED ||
                instruction.operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT) &&
               (successor_index == 1u ||
                successor_index == vm_coroutine_trap_successor(execution, &instruction))) {
        const XrValidatedSignature *signature =
            vm_coroutine_call_signature(execution, &instruction);
        if (!signature)
            return false;
        operand_start =
            signature->parameter_count +
            (instruction.operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT ? 1u : 0u);
        for (uint32_t prior = 0u; prior < successor_index; ++prior) {
            const XrValidatedBlock *row = &function->blocks[instruction.successors[prior]];
            uint32_t implicit = prior == 0u
                                    ? (signature->result_type_id != XR_CORE_TYPE_VOID ? 1u : 0u)
                                    : (prior >= 2u ? 1u : 0u);
            if (row->argument_count < implicit)
                return false;
            operand_start += row->argument_count - implicit;
        }
    } else {
        return false;
    }
    if (operand_start > instruction.operand_count ||
        target->argument_count > instruction.operand_count - operand_start)
        return false;
    for (uint32_t argument = 0u; argument < target->argument_count; ++argument) {
        uint32_t source_value = instruction.operands[operand_start + argument];
        if (!execution->initialized[source_value])
            return false;
    }
    vm_execution_assign_edge(execution, target, instruction.operands, operand_start, NULL);
    execution->block_id = instruction.successors[successor_index];
    return true;
}

#include "xr_program_vm_dispatch.inc.c"

static void vm_dispatch_instruction(XrVmDispatch *dispatch) {
    switch (dispatch->instruction.operation_id) {
        case XR_CORE_OP_CORE_CONSTANT_F64:
        case XR_CORE_OP_CORE_CONSTANT_I64:
        case XR_CORE_OP_CORE_CONSTANT_BOOL:
        case XR_CORE_OP_CORE_CONSTANT_TARGET_ENUM:
        case XR_CORE_OP_CORE_TARGET_POINTER_WIDTH:
        case XR_CORE_OP_CORE_TARGET_OPERATING_SYSTEM:
        case XR_CORE_OP_CORE_TARGET_ARCHITECTURE:
        case XR_CORE_OP_CORE_TARGET_NATIVE_ABI:
        case XR_CORE_OP_CORE_TARGET_ENDIANNESS:
            vm_dispatch_constants(dispatch);
            return;
        case XR_CORE_OP_CORE_ADD_I64:
        case XR_CORE_OP_CORE_SUB_I64:
        case XR_CORE_OP_CORE_MUL_I64:
        case XR_CORE_OP_CORE_DIV_I64:
        case XR_CORE_OP_CORE_SCALAR_BITCAST64:
        case XR_CORE_OP_CORE_INTEGER_CONVERT:
        case XR_CORE_OP_CORE_STRING_SLICE:
        case XR_CORE_OP_CORE_INTEGER_BITWISE:
        case XR_CORE_OP_CORE_INTEGER_DIVMOD:
            vm_dispatch_arithmetic(dispatch);
            return;
        case XR_CORE_OP_CORE_LOGICAL_NOT:
        case XR_CORE_OP_CORE_LOGICAL_AND:
        case XR_CORE_OP_CORE_LOGICAL_OR:
        case XR_CORE_OP_CORE_COMPARE_F64:
        case XR_CORE_OP_CORE_COMPARE_I64:
        case XR_CORE_OP_CORE_COMPARE_TARGET_ENUM:
            vm_dispatch_logic(dispatch);
            return;
        case XR_CORE_OP_CORE_BLOCK_ARGUMENT:
        case XR_CORE_OP_CORE_BRANCH:
        case XR_CORE_OP_CORE_CONDITIONAL_BRANCH:
        case XR_CORE_OP_CORE_ASSERT_CONDITION:
        case XR_CORE_OP_CORE_RETURN:
        case XR_CORE_OP_CORE_TRAP:
        case XR_CORE_OP_CORE_ERROR_PUBLISH:
        case XR_CORE_OP_CORE_PANIC_PUBLISH:
        case XR_CORE_OP_CORE_CANCEL_PUBLISH:
            vm_dispatch_control(dispatch);
            return;
        case XR_CORE_OP_CORE_CALL_SEALED_DIRECT:
        case XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT:
        case XR_CORE_OP_CORE_CALL_WITNESS_DIRECT:
            vm_dispatch_direct_call(dispatch);
            return;
        case XR_CORE_OP_CORE_CALL_SEALED_INVOKE:
        case XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE:
        case XR_CORE_OP_CORE_CALL_WITNESS_INVOKE:
            vm_dispatch_invoke(dispatch);
            return;
        case XR_CORE_OP_CORE_PROVIDER_CALL:
        case XR_CORE_OP_CORE_CONSTANT_STRING:
        case XR_CORE_OP_CORE_CONSTANT_RUNE:
        case XR_CORE_OP_CORE_STRING_FROM_SCALAR:
        case XR_CORE_OP_CORE_STRING_CONCAT:
        case XR_CORE_OP_CORE_SEQUENCE_LENGTH:
        case XR_CORE_OP_CORE_COMPARE_STRING:
        case XR_CORE_OP_CORE_COMPARE_RUNE:
        case XR_CORE_OP_CORE_OUTPUT_GROUP:
            vm_dispatch_provider_text(dispatch);
            return;
        case XR_CORE_OP_CORE_CALLABLE_PACK:
        case XR_CORE_OP_CORE_OWNER_COPY:
        case XR_CORE_OP_CORE_OWNER_MOVE:
        case XR_CORE_OP_CORE_OWNER_DROP:
            vm_dispatch_owner_callable(dispatch);
            return;
        case XR_CORE_OP_CORE_CLASS_CONSTRUCT:
        case XR_CORE_OP_CORE_OWNER_ALIAS:
            vm_dispatch_class_owner(dispatch);
            return;
        case XR_CORE_OP_CORE_CLASS_FIELD_LOAD:
        case XR_CORE_OP_CORE_CLASS_FIELD_PLACE:
            vm_dispatch_class_field(dispatch);
            return;
        case XR_CORE_OP_CORE_PLACE_LOCAL:
        case XR_CORE_OP_CORE_PLACE_MODULE:
        case XR_CORE_OP_CORE_SEQUENCE_ELEMENT_PLACE:
        case XR_CORE_OP_CORE_PLACE_INITIALIZE:
        case XR_CORE_OP_CORE_PLACE_LOAD:
        case XR_CORE_OP_CORE_PLACE_STORE:
        case XR_CORE_OP_CORE_PLACE_PROJECT:
        case XR_CORE_OP_CORE_PLACE_TAKE:
        case XR_CORE_OP_CORE_PLACE_EXCHANGE:
            vm_dispatch_place(dispatch);
            return;
        case XR_CORE_OP_CORE_STRING_BUILDER_CONSTRUCT:
        case XR_CORE_OP_CORE_STRING_BUILDER_APPEND:
        case XR_CORE_OP_CORE_STRING_BUILDER_CLEAR:
        case XR_CORE_OP_CORE_STRING_BUILDER_LENGTH:
        case XR_CORE_OP_CORE_STRING_BUILDER_SNAPSHOT:
            vm_dispatch_string_builder(dispatch);
            return;
        case XR_CORE_OP_CORE_BYTES_TIMING_SAFE_EQUAL:
        case XR_CORE_OP_CORE_CHANNEL_CONSTRUCT:
        case XR_CORE_OP_CORE_CHANNEL_IS_CLOSED:
        case XR_CORE_OP_CORE_ATOMIC_CONSTRUCT:
        case XR_CORE_OP_CORE_ATOMIC_LOAD:
        case XR_CORE_OP_CORE_ATOMIC_EXCHANGE:
        case XR_CORE_OP_CORE_ATOMIC_COMPARE_EXCHANGE:
        case XR_CORE_OP_CORE_ATOMIC_UPDATE:
        case XR_CORE_OP_CORE_ARRAY_APPEND:
        case XR_CORE_OP_CORE_ARRAY_ALLOCATE_DEFAULT:
        case XR_CORE_OP_CORE_ARRAY_CONSTRUCT:
        case XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT:
        case XR_CORE_OP_CORE_AGGREGATE_PROJECT:
        case XR_CORE_OP_CORE_AGGREGATE_UPDATE:
        case XR_CORE_OP_CORE_VARIANT_CONSTRUCT:
        case XR_CORE_OP_CORE_VARIANT_TEST:
        case XR_CORE_OP_CORE_VARIANT_PROJECT:
            vm_dispatch_aggregate_variant(dispatch);
            return;
        case XR_CORE_OP_CORE_EXISTENTIAL_PACK:
        case XR_CORE_OP_CORE_EXISTENTIAL_REBORROW_READ:
        case XR_CORE_OP_CORE_EXISTENTIAL_TEST:
        case XR_CORE_OP_CORE_EXISTENTIAL_PROJECT:
            vm_dispatch_existential(dispatch);
            return;
        case XR_CORE_OP_CORE_COROUTINE_YIELD:
        case XR_CORE_OP_CORE_COROUTINE_SUSPEND:
            vm_dispatch_suspension(dispatch);
            return;
        case XR_CORE_OP_CORE_COROUTINE_CALL_SEALED:
        case XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT:
            vm_dispatch_coroutine_call(dispatch);
            return;
        default:
            dispatch->terminal = true;
            return;
    }
}

bool xr_vm_execution_set_interrupt(XrVmExecution *execution, void *context,
                                   bool (*requested)(void *context)) {
    if (!execution || execution->finished || execution->context.storage != &execution->storage ||
        execution->storage.steps != 0u || execution->initialization_complete ||
        execution->initialization_child || execution->child)
        return false;
    execution->storage.interrupt_context = context;
    execution->storage.interrupt_requested = requested;
    return true;
}

static XrVmOutcome vm_execution_step(XrVmExecution *execution) {
    if (!execution || execution->finished || !xr_execution_lease_is_valid(&execution->lease))
        return vm_execution_outcome(execution, XR_VM_OUTCOME_INVALID_INVOCATION);
    if (execution->suspended && !execution->child &&
        !vm_materialize_suspension_edge(execution, 0u)) {
        execution->finished = true;
        vm_execution_release_current_owners(execution);
        vm_execution_release_lease(execution);
        return vm_execution_outcome(execution, XR_VM_OUTCOME_INVALID_INVOCATION);
    }
    execution->suspended = false;
    execution->cancel_block_id = XR_PROGRAM_LOCATION_NONE;
    execution->suspension_block_id = XR_PROGRAM_LOCATION_NONE;
    execution->suspension_instruction_id = XR_PROGRAM_LOCATION_NONE;
    XrVmContext *context = &execution->context;
    const XrValidatedFunction *function =
        &context->code->program->functions[execution->function_id];
    XrVmOutcome result = vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, context);
    uint32_t cleanup_block = execution->block_id;
    uint32_t committed = execution->instruction_id;
    const XrValidatedInstruction *transferred = NULL;
    bool calls_only = false;
    for (;;) {
        cleanup_block = execution->block_id;
        committed = execution->instruction_id;
        const XrValidatedBlock *block = &function->blocks[execution->block_id];
        if (execution->instruction_id >= block->instruction_count)
            goto done;
        uint32_t instruction_id = execution->instruction_id++;
        XrVmDispatch dispatch = {
            .execution = execution,
            .instruction = instruction_view(context->code, execution->function_id,
                                            execution->block_id, instruction_id),
            .produced = {.category = XR_CORE_IR_VALUE, .as.value = void_value()},
            .outcome = vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, context),
            .instruction_id = instruction_id,
            .block_id = execution->block_id,
        };
        if (context->storage->steps == context->code->options.max_steps ||
            (context->storage->interrupt_requested &&
             context->storage->interrupt_requested(context->storage->interrupt_context))) {
            result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
            goto done;
        }
        ++context->steps;
        ++context->storage->steps;
        trace_instruction(context, execution->function_id, execution->block_id, instruction_id,
                          dispatch.instruction.operation_id);
        for (uint32_t operand = 0u; operand < dispatch.instruction.operand_count; ++operand)
            if (!execution->initialized[dispatch.instruction.operands[operand]])
                goto done;
        vm_dispatch_instruction(&dispatch);
        if (dispatch.returned && dispatch.outcome.kind == XR_VM_OUTCOME_SUSPENDED)
            return dispatch.outcome;
        if (dispatch.call_entered) {
            transferred = &block->instructions[instruction_id];
            calls_only = true;
        }
        if (dispatch.terminal || dispatch.returned) {
            result = dispatch.outcome;
            if (dispatch.instruction.operation_id == XR_CORE_OP_CORE_RETURN ||
                dispatch.instruction.operation_id == XR_CORE_OP_CORE_ERROR_PUBLISH ||
                dispatch.instruction.operation_id == XR_CORE_OP_CORE_PANIC_PUBLISH) {
                transferred = &block->instructions[instruction_id];
                calls_only = false;
            }
            goto done;
        }
        if (dispatch.transferred) {
            const XrValidatedBlock *target = &function->blocks[dispatch.block_id];
            if (dispatch.incoming_count != target->argument_count)
                goto done;
            for (uint32_t argument = 0u; argument < dispatch.incoming_count; ++argument) {
                uint32_t value_id = target->argument_ids[argument];
                execution->values[value_id] = execution->edge_values[argument];
                execution->initialized[value_id] = true;
            }
            execution->block_id = dispatch.block_id;
            execution->instruction_id = 0u;
        } else if (dispatch.instruction.result_id != XR_PROGRAM_LOCATION_NONE) {
            execution->values[dispatch.instruction.result_id] = dispatch.produced;
            execution->initialized[dispatch.instruction.result_id] = true;
        }
        transferred = NULL;
        calls_only = false;
    }

done:
    execution->finished = true;
    vm_execution_release_owners(execution, cleanup_block, committed, transferred, calls_only);
    result.logical_trace = vm_execution_outcome(execution, result.kind).logical_trace;
    result.steps = context->steps;
    result.state_id = execution->state_id;
    XrVmValue payload = result.kind == XR_VM_OUTCOME_RETURN ? result.value
                       : result.kind == XR_VM_OUTCOME_ERROR ? result.error_value
                       : result.kind == XR_VM_OUTCOME_PANIC ? result.panic_value : void_value();
    if (execution->owns_lease && vm_value_cell(payload))
        execution->terminal_owner = payload;
    else
        vm_execution_release_lease(execution);
    return result;
}

static XrVmOutcome vm_execution_cancel(XrVmExecution *execution) {
    if (!execution || execution->finished || !execution->suspended ||
        execution->cancel_block_id == XR_PROGRAM_LOCATION_NONE ||
        !xr_execution_lease_is_valid(&execution->lease))
        return vm_execution_outcome(execution, XR_VM_OUTCOME_INVALID_INVOCATION);
    uint32_t successor_index = 1u;
    if (execution->child) {
        uint64_t child_steps = execution->child->context.steps;
        XrVmOutcome child = vm_execution_cancel(execution->child);
        uint64_t child_delta = child.steps - child_steps;
        if (child_delta > execution->context.code->options.max_steps - execution->context.steps) {
            execution->finished = true;
            xr_vm_execution_free(execution->child);
            execution->child = NULL;
            vm_execution_release_current_owners(execution);
            vm_execution_release_lease(execution);
            return vm_execution_outcome(execution, XR_VM_OUTCOME_RESOURCE_LIMIT);
        }
        execution->context.steps += child_delta;

        XrVmInstructionView instruction;
        if (child.kind == XR_VM_OUTCOME_TRAP && child.trap == XR_VM_TRAP_PROVIDER_CALL_FAILED &&
            vm_suspension_instruction(execution, &instruction) &&
            (instruction.operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED ||
             instruction.operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT) &&
            vm_coroutine_trap_successor(execution, &instruction) != XR_PROGRAM_LOCATION_NONE) {
            successor_index = vm_coroutine_trap_successor(execution, &instruction);
        } else if (child.kind != XR_VM_OUTCOME_CANCELLED) {
            execution->finished = true;
            xr_vm_execution_free(execution->child);
            execution->child = NULL;
            vm_execution_release_current_owners(execution);
            vm_execution_release_lease(execution);
            child.steps = execution->context.steps;
            child.state_id = execution->state_id;
            return child;
        }
        xr_vm_execution_free(execution->child);
        execution->child = NULL;
    }
    if (!vm_materialize_suspension_edge(execution, successor_index)) {
        execution->finished = true;
        vm_execution_release_current_owners(execution);
        vm_execution_release_lease(execution);
        return vm_execution_outcome(execution, XR_VM_OUTCOME_INVALID_INVOCATION);
    }
    execution->instruction_id = 0u;
    execution->suspended = false;
    execution->cancel_block_id = XR_PROGRAM_LOCATION_NONE;
    execution->suspension_block_id = XR_PROGRAM_LOCATION_NONE;
    execution->suspension_instruction_id = XR_PROGRAM_LOCATION_NONE;
    return vm_execution_step(execution);
}

/* Publish the failure owner before clearing module slots. Other entrants
 * acquire the same immutable result through the initialization state barrier. */
static XrVmOutcome vm_initialization_failed(XrVmExecution *execution, const XrVmOutcome *failure) {
    XrVmModuleState *state = execution->context.modules;
    if (failure && state) {
        state->failure_kind = failure->kind;
        state->failure_trap = failure->trap;
        state->failure_panic = failure->kind == XR_VM_OUTCOME_PANIC &&
                                       failure->panic_value.kind == XR_VM_VALUE_PANIC_INFO
                                   ? failure->panic_value
                                   : void_value();
        if (failure->kind == XR_VM_OUTCOME_ERROR || failure->kind == XR_VM_OUTCOME_PANIC) {
            XrVmValue payload =
                failure->kind == XR_VM_OUTCOME_ERROR ? failure->error_value : failure->panic_value;
            if (vm_transfer_value(&execution->context, payload, &state->failure_storage, false)) {
                if (failure->kind == XR_VM_OUTCOME_ERROR)
                    state->failure_error = payload;
            } else {
                drop_vm_value(&execution->context, &payload, XR_VM_EVENT_ORIGIN_DOMAIN_TEARDOWN);
                state->failure_kind = XR_VM_OUTCOME_RESOURCE_LIMIT;
                state->failure_trap = XR_VM_TRAP_NONE;
                state->failure_panic = void_value();
            }
        }
        vm_module_state_clear(state);
    }
    XrVmOutcome result =
        vm_outcome(state ? state->failure_kind : XR_VM_OUTCOME_TRAP, &execution->context);
    result.trap = state ? state->failure_trap : XR_VM_TRAP_EXPLICIT;
    result.error_value = state ? state->failure_error : void_value();
    result.panic_value = state ? state->failure_panic : void_value();
    return result;
}

static void vm_execution_abandon_initialization(XrVmExecution *execution) {
    if (!execution->initialization_child || !xr_execution_lease_is_valid(&execution->lease))
        return;
    /* Publish failure only after clearing the acquired slots. Other entry
     * leases can outlive
     * the abandoned initializer and observe its failure. */
    XrVmOutcome failure = vm_trap(XR_VM_TRAP_EXPLICIT, &execution->context);
    (void) vm_initialization_failed(execution, &failure);
    (void) xr_execution_lease_initialization_finish(&execution->lease,
                                                    execution->initialization_module, false);
}

static XrVmOutcome vm_execution_initialize_modules(XrVmExecution *execution, bool cancel) {
    if (!execution || execution->finished || !xr_execution_lease_is_valid(&execution->lease))
        return vm_execution_outcome(execution, XR_VM_OUTCOME_INVALID_INVOCATION);
    while (!execution->initialization_complete) {
        if (execution->initialization_child) {
            XrVmExecution *child = execution->initialization_child;
            uint64_t before = child->context.steps;
            XrVmOutcome outcome = cancel ? vm_execution_cancel(child) : vm_execution_step(child);
            execution->context.steps += child->context.steps - before;
            if (child->function_id == execution->function_id)
                execution->state_id = outcome.state_id;
            if (outcome.kind == XR_VM_OUTCOME_SUSPENDED)
                return outcome;
            bool succeeded = outcome.kind == XR_VM_OUTCOME_RETURN;
            if (!succeeded)
                outcome = vm_initialization_failed(execution, &outcome);
            bool recorded = xr_execution_lease_initialization_finish(
                &execution->lease, execution->initialization_module, succeeded);
            hash_u32(&execution->context.trace, child->function_id);
            xr_sha256_update(&execution->context.trace, outcome.logical_trace.bytes,
                             sizeof(outcome.logical_trace.bytes));
            xr_vm_execution_free(child);
            execution->initialization_child = NULL;
            if (!succeeded || !recorded) {
                execution->finished = true;
                /* Typed failure views borrow the instance's immutable failure owner.
                 * Keep its lease until the host releases this frame. */
                if (outcome.kind != XR_VM_OUTCOME_ERROR &&
                    !(outcome.kind == XR_VM_OUTCOME_PANIC && vm_value_cell(outcome.panic_value)))
                    vm_execution_release_lease(execution);
                return succeeded ? vm_trap(XR_VM_TRAP_EXPLICIT, &execution->context) : outcome;
            }
        }
        XrExecutionInitializationStep next = {0};
        XrExecutionInitializationStatus status =
            xr_execution_lease_initialization_next(&execution->lease, &next);
        if (status == XR_EXECUTION_INITIALIZATION_WAIT)
            return vm_execution_outcome(execution, XR_VM_OUTCOME_INITIALIZING);
        if (status == XR_EXECUTION_INITIALIZATION_COMPLETE) {
            execution->initialization_complete = true;
            break;
        }
        if (status != XR_EXECUTION_INITIALIZATION_RUN) {
            XrVmOutcome failure = vm_initialization_failed(execution, NULL);
            execution->finished = true;
            if (failure.kind != XR_VM_OUTCOME_ERROR &&
                !(failure.kind == XR_VM_OUTCOME_PANIC && vm_value_cell(failure.panic_value)))
                vm_execution_release_lease(execution);
            return failure;
        }
        execution->initialization_module = next.module_index;
        execution->initialization_child =
            vm_execution_create_child_frame(execution, next.function_id);
        if (!execution->initialization_child) {
            XrVmOutcome failure = vm_execution_outcome(execution, XR_VM_OUTCOME_RESOURCE_LIMIT);
            failure = vm_initialization_failed(execution, &failure);
            (void) xr_execution_lease_initialization_finish(&execution->lease, next.module_index,
                                                            false);
            execution->finished = true;
            vm_execution_release_lease(execution);
            return failure;
        }
    }
    return vm_execution_outcome(execution, XR_VM_OUTCOME_RETURN);
}

static bool vm_function_is_initializer(const XrVmCode *code, uint32_t function_id) {
    const XrValidatedProgram *program = code->program;
    for (uint32_t module = 0u; module < program->module_count; ++module) {
        if (program->modules[module].initializer == function_id)
            return true;
    }
    return false;
}

static XrVmOutcome vm_execution_initialize_entry(XrVmExecution *execution, bool cancel) {
    if (cancel && execution && !execution->initialization_complete &&
        !execution->initialization_child)
        return vm_execution_outcome(execution, XR_VM_OUTCOME_INVALID_INVOCATION);
    XrVmOutcome initialized = vm_execution_initialize_modules(execution, cancel);
    if (initialized.kind != XR_VM_OUTCOME_RETURN)
        return initialized;
    if (vm_function_is_initializer(execution->code, execution->function_id)) {
        execution->finished = true;
        XrVmOutcome outcome = vm_execution_outcome(execution, XR_VM_OUTCOME_RETURN);
        vm_execution_release_lease(execution);
        return outcome;
    }
    return cancel ? vm_execution_cancel(execution) : vm_execution_step(execution);
}

XrVmOutcome xr_vm_execution_step(XrVmExecution *execution) {
    return vm_execution_initialize_entry(execution, false);
}

XrVmOutcome xr_vm_execution_cancel(XrVmExecution *execution) {
    return vm_execution_initialize_entry(execution, true);
}

void xr_vm_execution_free(XrVmExecution *execution) {
    if (!execution)
        return;
    xr_vm_execution_free(execution->initialization_child);
    xr_vm_execution_free(execution->child);
    vm_execution_release_current_owners(execution);
    drop_vm_value(&execution->context, &execution->terminal_owner,
                  XR_VM_EVENT_ORIGIN_DOMAIN_TEARDOWN);
    vm_execution_abandon_initialization(execution);
    vm_execution_release_lease(execution);
    if (execution->context.storage == &execution->storage)
        free_value_storage(&execution->storage);
    vm_execution_free_values(execution);
    xr_vm_code_free(execution->code);
    xr_free(execution);
}

XrVmOutcome xr_vm_code_execute(const XrVmCode *code, XrInstance *instance, uint32_t function_id,
                               const XrVmValue *arguments, uint32_t argument_count) {
    XrVmValueStorage storage = {0};
    XrVmContext context = {.code = code, .storage = &storage};
    if (!code || !instance || function_id >= code->program->function_count ||
        (argument_count != 0u && !arguments))
        return vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, &context);
    if (code->program->functions[function_id].coroutine_safepoint_count != 0u) {
        // A one-shot call cannot own a resumable session.
        // Callers must use the explicit XrVmExecution API.
        return vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, &context);
    }
    XrExecutionLease lease = {0};
    if (!xr_vm_code_matches_instance(code, instance))
        return vm_outcome(XR_VM_OUTCOME_STALE_CODE, &context);
    XrExecutionStatus acquired = xr_execution_instance_acquire(instance, &lease);
    if (acquired != XR_EXECUTION_OK)
        return vm_outcome(acquired == XR_EXECUTION_OUT_OF_MEMORY
                              ? XR_VM_OUTCOME_RESOURCE_LIMIT : XR_VM_OUTCOME_STALE_CODE, &context);
    context.lease = &lease;
    if (!vm_bind_module_state(&context)) {
        (void) xr_execution_lease_release(&lease);
        return vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, &context);
    }
    static const uint8_t trace_domain[] = "xray-vm-logical-trace-v1\0";
    xr_sha256_init(&context.trace);
    xr_sha256_update(&context.trace, trace_domain, sizeof(trace_domain) - 1u);
    xr_sha256_update(&context.trace, code->execution_id.bytes, sizeof(code->execution_id.bytes));
    hash_u32(&context.trace, function_id);
    const XrValidatedFunction *function = &code->program->functions[function_id];
    XrVmRuntimeValue *runtime_arguments =
        xr_calloc(argument_count ? argument_count : 1u, sizeof(XrVmRuntimeValue));
    if (!runtime_arguments) {
        free_value_storage(&storage);
        (void) xr_execution_lease_release(&lease);
        return vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, &context);
    }
    for (uint32_t index = 0; index < argument_count; ++index) {
        if (index >= function->parameter_count ||
            (function->parameter_modes[index] == XR_PARAM_REF &&
             !function_parameter_is_class_receiver(code->program, function, index))) {
            xr_free(runtime_arguments);
            free_value_storage(&storage);
            (void) xr_execution_lease_release(&lease);
            return vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, &context);
        }
        runtime_arguments[index].category = XR_CORE_IR_VALUE;
        runtime_arguments[index].as.value = arguments[index];
    }
    XrVmExecution initialization = {
        .context = context,
        .lease = lease,
        .code = xr_vm_code_retain(code),
        .depth = 1u,
    };
    XrVmOutcome outcome = vm_execution_initialize_modules(&initialization, false);
    context = initialization.context;
    bool entered = false;
    if (outcome.kind == XR_VM_OUTCOME_RETURN && !vm_function_is_initializer(code, function_id))
        outcome = execute_function(&context, function_id, runtime_arguments, argument_count, 1u,
                                   &entered);
    if (outcome.kind == XR_VM_OUTCOME_SUSPENDED)
        outcome = vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, &context);
    xr_vm_execution_free(initialization.initialization_child);
    vm_execution_abandon_initialization(&initialization);
    xr_vm_code_free(initialization.code);
    xr_free(runtime_arguments);
    if (context.modules &&
        ((outcome.kind == XR_VM_OUTCOME_ERROR && context.modules->failure_kind == XR_VM_OUTCOME_ERROR) ||
         (outcome.kind == XR_VM_OUTCOME_PANIC && context.modules->failure_kind == XR_VM_OUTCOME_PANIC &&
          outcome.panic_value.as.panic_info.message))) {
        /* A sticky failure is one immutable instance-owned result. Every host
         * observation retains that owner; affine payloads are never copied. */
        XrVmOutcomeOwner *owner = xr_calloc(1u, sizeof(*owner));
        if (owner) {
            owner->lease = lease;
            memset(&lease, 0, sizeof(lease));
            outcome.private_owner = owner;
            outcome.owns_dynamic_values = true;
        } else {
            outcome = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, &context);
        }
    }
    XrVmValue *published = outcome.kind == XR_VM_OUTCOME_RETURN ? &outcome.value
                         : outcome.kind == XR_VM_OUTCOME_ERROR ? &outcome.error_value
                         : outcome.kind == XR_VM_OUTCOME_PANIC ? &outcome.panic_value : NULL;
    bool owned_existential = published && published->kind == XR_VM_VALUE_EXISTENTIAL &&
        published->as.existential &&
        ((const XrVmExistentialValue *) published->as.existential)->owned_storage.initialized;
    if (published && !outcome.private_owner &&
        (owned_existential || published->kind == XR_VM_VALUE_AGGREGATE || published->kind == XR_VM_VALUE_STRING ||
         published->kind == XR_VM_VALUE_CLASS_REFERENCE || published->kind == XR_VM_VALUE_CALLABLE ||
         published->kind == XR_VM_VALUE_RESOURCE ||
         (published->kind == XR_VM_VALUE_PANIC_INFO && published->as.panic_info.message))) {
        XrVmOutcomeOwner *owner = xr_calloc(1u, sizeof(*owner));
        if (owner && vm_transfer_value(&context, *published, &owner->storage, true)) {
            owner->storage.persistent = true;
            owner->value = *published;
            owner->code = xr_vm_code_retain(code);
            owner->lease = lease;
            memset(&lease, 0, sizeof(lease));
            outcome.private_owner = owner;
            outcome.owns_dynamic_values = true;
        } else {
            xr_free(owner);
            drop_vm_value(&context, published, XR_VM_EVENT_ORIGIN_DOMAIN_TEARDOWN);
            outcome = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, &context);
        }
    }
    if (published && !outcome.private_owner && published->kind == XR_VM_VALUE_EXISTENTIAL)
        outcome = vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, &context);
    if (outcome.kind == XR_VM_OUTCOME_PANIC && outcome.panic_value.kind != XR_VM_VALUE_PANIC_INFO)
        outcome = vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, &context);
    hash_u32(&context.trace, (uint32_t) outcome.kind);
    hash_u32(&context.trace, (uint32_t) outcome.trap);
    hash_u32(&context.trace, (uint32_t) outcome.error_value.kind);
    if (outcome.error_value.kind == XR_VM_VALUE_ERROR)
        hash_u32(&context.trace, outcome.error_value.as.error);
    hash_u32(&context.trace, (uint32_t) outcome.panic_value.kind);
    if (outcome.panic_value.kind == XR_VM_VALUE_PANIC_INFO)
        hash_u32(&context.trace, outcome.panic_value.as.panic_info.code);
    if (outcome.panic_value.kind == XR_VM_VALUE_PANIC_INFO &&
        outcome.panic_value.as.panic_info.message) {
        const XrVmStringValue *message = outcome.panic_value.as.panic_info.message;
        hash_u32(&context.trace, UINT32_C(0x6d7367));
        hash_u32(&context.trace, message->size);
        xr_sha256_update(&context.trace, message->bytes, message->size);
    }
    if (outcome.panic_value.kind == XR_VM_VALUE_PANIC_INFO &&
        outcome.panic_value.as.panic_info.has_bounds) {
        const XrVmPanicInfo *panic = &outcome.panic_value.as.panic_info;
        hash_u32(&context.trace, 1u);
        hash_u32(&context.trace, (uint32_t)(uint64_t)panic->index);
        hash_u32(&context.trace, (uint32_t)((uint64_t)panic->index >> 32u));
        hash_u32(&context.trace, (uint32_t)panic->length);
        hash_u32(&context.trace, (uint32_t)(panic->length >> 32u));
    }
    xr_sha256_final(&context.trace, outcome.logical_trace.bytes);
    outcome.steps = context.steps;
    free_value_storage(&storage);
    (void) xr_execution_lease_release(&lease);
    return outcome;
}

const char *xr_vm_code_status_name(XrVmCodeStatus status) {
    switch (status) {
        case XR_VM_CODE_OK:
            return "ok";
        case XR_VM_CODE_INVALID_INPUT:
            return "invalid-input";
        case XR_VM_CODE_UNSUPPORTED_OPERATION:
            return "unsupported-operation";
        case XR_VM_CODE_POLICY_REJECTED:
            return "policy-rejected";
        case XR_VM_CODE_OUT_OF_MEMORY:
            return "out-of-memory";
        default:
            return "unknown";
    }
}
