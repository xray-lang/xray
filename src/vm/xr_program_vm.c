/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_vm.c - Generic CoreSpec dispatch over validated XrProgram
 */

#include "xr_program_vm.h"
#include "../runtime/core/xr_text_kernel.h"

#include "../base/xmalloc.h"
#include "../base/xsha256.h"
#include "../core/xr_core_spec_gen.h"
#include "../program/xr_validated_program_internal.h"

#include <limits.h>
#include <string.h>

typedef struct XrVmExistentialValue XrVmExistentialValue;
typedef struct XrVmCallableValue XrVmCallableValue;
typedef struct XrVmClassValue XrVmClassValue;
typedef struct XrVmStringValue XrVmStringValue;

typedef struct XrVmFixedInstruction {
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
} XrVmFixedInstruction;

typedef struct XrVmFixedBlock {
    XrVmFixedInstruction *instructions;
    uint32_t instruction_count;
} XrVmFixedBlock;

typedef struct XrVmFixedFunction {
    XrVmFixedBlock *blocks;
    uint32_t block_count;
} XrVmFixedFunction;

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
    XrVmFixedFunction *fixed_functions;
    size_t private_size;
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
 * have separate storage, and returned host values use explicit detachment. */
typedef struct XrVmValueStorage {
    uint64_t aggregate_cell_count;
    uint64_t next_class_identity;
    struct XrVmAggregateValue **aggregates;
    uint32_t aggregate_count;
    uint32_t aggregate_capacity;
    XrVmClassValue **classes;
    uint32_t class_count;
    uint32_t class_capacity;
    XrVmExistentialValue **existentials;
    uint32_t existential_count;
    uint32_t existential_capacity;
    XrVmCallableValue **callables;
    uint32_t callable_count;
    uint32_t callable_capacity;
    XrVmStringValue **strings;
    uint32_t string_count;
    uint32_t string_capacity;
} XrVmValueStorage;

typedef struct XrVmContext {
    const XrVmCode *code;
    const XrExecutionLease *lease;
    uint64_t steps;
    XrSHA256Context trace;
    XrVmValueStorage *storage;
} XrVmContext;

/* One immutable string owner in the VM arena.  Releasing it is not a
 * semantic event, so drops leave the cell to arena teardown. */
struct XrVmStringValue {
    uint32_t size;
    uint8_t *bytes;
};

typedef struct XrVmAggregateValue {
    uint16_t type_id;
    uint32_t variant_ordinal;
    XrVmValue *fields;
    uint32_t field_count;
} XrVmAggregateValue;

struct XrVmClassValue {
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
    bool initialized;
} XrVmPlace;

typedef struct XrVmRuntimeValue {
    XrCoreIrValueCategory category;
    union {
        XrVmValue value;
        XrVmPlace *place;
    } as;
} XrVmRuntimeValue;

struct XrVmExecution {
    XrVmContext context;
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
    bool suspended;
    bool finished;
};

struct XrVmExistentialValue {
    uint16_t existential_type_id;
    uint16_t concrete_type_id;
    uint32_t conformance_id;
    XrVmRuntimeValue payload;
    XrVmPlace owned_storage;
};

struct XrVmCallableValue {
    uint16_t callable_type_id;
    uint16_t capture_type_id;
    uint32_t function_id;
    bool has_capture;
    XrVmValue capture;
};

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

static bool value_matches_type(const XrValidatedProgram *program, XrVmValue value,
                               uint16_t type_id) {
    switch (type_id) {
        case XR_CORE_TYPE_VOID:
            return value.kind == XR_VM_VALUE_VOID;
        case XR_CORE_TYPE_BOOL:
            return value.kind == XR_VM_VALUE_BOOL;
        case XR_CORE_TYPE_I64:
            return value.kind == XR_VM_VALUE_I64;
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
        case XR_CORE_TYPE_STRING:
            return value.kind == XR_VM_VALUE_STRING && value.as.string;
        case XR_CORE_TYPE_RUNE:
            return value.kind == XR_VM_VALUE_RUNE && xr_text_rune_is_scalar(value.as.rune);
        default: {
            const XrValidatedType *type = xr_validated_program_type(program, type_id);
            if (!type)
                return false;
            if (type->kind == XR_CORE_IR_TYPE_EXISTENTIAL)
                return value.kind == XR_VM_VALUE_EXISTENTIAL && value.as.existential &&
                       ((const XrVmExistentialValue *) value.as.existential)->existential_type_id ==
                           type_id;
            if (type->kind == XR_CORE_IR_TYPE_CALLABLE)
                return value.kind == XR_VM_VALUE_CALLABLE && value.as.callable &&
                       ((const XrVmCallableValue *) value.as.callable)->callable_type_id == type_id;
            if (type->kind == XR_CORE_IR_TYPE_CLASS_REFERENCE)
                return value.kind == XR_VM_VALUE_CLASS_REFERENCE &&
                       class_value_is_live(value.as.class_reference, type_id);
            return (type->kind == XR_CORE_IR_TYPE_AGGREGATE ||
                    type->kind == XR_CORE_IR_TYPE_VARIANT) &&
                   value.kind == XR_VM_VALUE_AGGREGATE && value.as.aggregate &&
                   ((const XrVmAggregateValue *) value.as.aggregate)->type_id == type_id;
        }
    }
}

static XrVmClassValue *allocate_class(XrVmContext *context, uint16_t type_id,
                                      uint32_t field_count) {
    XrVmValueStorage *storage = context->storage;
    const XrValidatedType *type = xr_validated_program_type(context->code->program, type_id);
    if (!type || type->kind != XR_CORE_IR_TYPE_CLASS_REFERENCE || type->field_count != field_count ||
        (uint64_t) field_count >
            (uint64_t) context->code->options.max_value_cells - storage->aggregate_cell_count)
        return NULL;
    if (storage->class_count == storage->class_capacity) {
        uint32_t capacity = storage->class_capacity ? storage->class_capacity * 2u : 8u;
        if (capacity < storage->class_count ||
            (size_t) capacity > SIZE_MAX / sizeof(*storage->classes))
            return NULL;
        XrVmClassValue **grown =
            xr_realloc(storage->classes, (size_t) capacity * sizeof(*storage->classes));
        if (!grown)
            return NULL;
        storage->classes = grown;
        storage->class_capacity = capacity;
    }
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
    value->identity = ++storage->next_class_identity;
    value->alive = true;
    storage->classes[storage->class_count++] = value;
    storage->aggregate_cell_count += field_count;
    return value;
}

static XrVmAggregateValue *allocate_aggregate(XrVmContext *context, uint16_t type_id,
                                              uint32_t variant_ordinal, uint32_t field_count) {
    XrVmValueStorage *storage = context->storage;
    if ((uint64_t) field_count >
        (uint64_t) context->code->options.max_value_cells - storage->aggregate_cell_count)
        return NULL;
    if (storage->aggregate_count == storage->aggregate_capacity) {
        uint32_t capacity = storage->aggregate_capacity ? storage->aggregate_capacity * 2u : 8u;
        if (capacity < storage->aggregate_count)
            return NULL;
#if SIZE_MAX < UINT64_MAX
        if ((size_t) capacity > SIZE_MAX / sizeof(*storage->aggregates))
            return NULL;
#endif
        XrVmAggregateValue **grown =
            xr_realloc(storage->aggregates, (size_t) capacity * sizeof(*storage->aggregates));
        if (!grown)
            return NULL;
        storage->aggregates = grown;
        storage->aggregate_capacity = capacity;
    }
    XrVmAggregateValue *aggregate = xr_calloc(1u, sizeof(*aggregate));
    if (!aggregate)
        return NULL;
    if (field_count != 0u) {
        aggregate->fields = xr_calloc(field_count, sizeof(XrVmValue));
        if (!aggregate->fields) {
            xr_free(aggregate);
            return NULL;
        }
    }
    aggregate->type_id = type_id;
    aggregate->variant_ordinal = variant_ordinal;
    aggregate->field_count = field_count;
    storage->aggregates[storage->aggregate_count++] = aggregate;
    storage->aggregate_cell_count += field_count;
    return aggregate;
}

/* Each string counts one cell plus its payload bytes against the VM budget. */
static XrVmStringValue *allocate_string(XrVmContext *context, size_t size) {
    XrVmValueStorage *storage = context->storage;
    if (size > XR_PROGRAM_CONSTANT_STRING_MAX_BYTES ||
        (uint64_t) size + 1u >
            (uint64_t) context->code->options.max_value_cells - storage->aggregate_cell_count)
        return NULL;
    if (storage->string_count == storage->string_capacity) {
        uint32_t capacity = storage->string_capacity ? storage->string_capacity * 2u : 8u;
        if (capacity < storage->string_count)
            return NULL;
#if SIZE_MAX < UINT64_MAX
        if ((size_t) capacity > SIZE_MAX / sizeof(*storage->strings))
            return NULL;
#endif
        XrVmStringValue **grown =
            xr_realloc(storage->strings, (size_t) capacity * sizeof(*storage->strings));
        if (!grown)
            return NULL;
        storage->strings = grown;
        storage->string_capacity = capacity;
    }
    XrVmStringValue *string = xr_calloc(1u, sizeof(*string));
    if (!string)
        return NULL;
    string->bytes = xr_malloc(size != 0u ? size : 1u);
    if (!string->bytes) {
        xr_free(string);
        return NULL;
    }
    string->size = (uint32_t) size;
    storage->strings[storage->string_count++] = string;
    storage->aggregate_cell_count += (uint64_t) size + 1u;
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
    if (storage->existential_count == storage->existential_capacity) {
        uint32_t capacity = storage->existential_capacity ? storage->existential_capacity * 2u : 8u;
        if (capacity < storage->existential_count)
            return NULL;
#if SIZE_MAX < UINT64_MAX
        if ((size_t) capacity > SIZE_MAX / sizeof(*storage->existentials))
            return NULL;
#endif
        XrVmExistentialValue **grown =
            xr_realloc(storage->existentials, (size_t) capacity * sizeof(*storage->existentials));
        if (!grown)
            return NULL;
        storage->existentials = grown;
        storage->existential_capacity = capacity;
    }
    XrVmExistentialValue *value = xr_calloc(1u, sizeof(*value));
    if (!value)
        return NULL;
    storage->existentials[storage->existential_count++] = value;
    ++storage->aggregate_cell_count;
    return value;
}

static XrVmCallableValue *allocate_callable(XrVmContext *context) {
    XrVmValueStorage *storage = context->storage;
    if (storage->aggregate_cell_count == context->code->options.max_value_cells)
        return NULL;
    if (storage->callable_count == storage->callable_capacity) {
        uint32_t capacity = storage->callable_capacity ? storage->callable_capacity * 2u : 8u;
        if (capacity < storage->callable_count)
            return NULL;
#if SIZE_MAX < UINT64_MAX
        if ((size_t) capacity > SIZE_MAX / sizeof(*storage->callables))
            return NULL;
#endif
        XrVmCallableValue **grown =
            xr_realloc(storage->callables, (size_t) capacity * sizeof(*storage->callables));
        if (!grown)
            return NULL;
        storage->callables = grown;
        storage->callable_capacity = capacity;
    }
    XrVmCallableValue *value = xr_calloc(1u, sizeof(*value));
    if (!value)
        return NULL;
    storage->callables[storage->callable_count++] = value;
    ++storage->aggregate_cell_count;
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
    const XrValidatedType *type =
        xr_validated_program_type(program, function->parameter_types[0]);
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
        *output = vm_string_value(copy);
        return true;
    }
    if (!type) {
        *output = source;
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
        *copy = *source_callable;
        if (copy->has_capture && !clone_vm_value(context, source_callable->capture,
                                                 source_callable->capture_type_id, &copy->capture))
            return false;
        output->kind = XR_VM_VALUE_CALLABLE;
        output->as.callable = copy;
        return true;
    }
    if (type->kind != XR_CORE_IR_TYPE_AGGREGATE && type->kind != XR_CORE_IR_TYPE_VARIANT)
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
        if (type->kind == XR_CORE_IR_TYPE_AGGREGATE) {
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
    const XrValidatedType *type = context && instance
                                      ? xr_validated_program_type(context->code->program,
                                                                  instance->type_id)
                                      : NULL;
    if (!context || !output || !type || type->kind != XR_CORE_IR_TYPE_CLASS_REFERENCE ||
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

static void drop_vm_value(XrVmContext *context, XrVmValue *value,
                          XrVmLifecycleEventOrigin origin) {
    if (!value)
        return;
    if (value->kind == XR_VM_VALUE_CLASS_REFERENCE) {
        XrVmClassValue *instance =
            (XrVmClassValue *) (void *) value->as.class_reference;
        if (instance && instance->alive && instance->owner_count != 0u) {
            emit_lifecycle(context, XR_VM_EVENT_OWNER_DROP, origin, instance, UINT64_MAX,
                           UINT32_MAX);
            if (--instance->owner_count != 0u)
                goto consumed;
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
        for (uint32_t field = aggregate->field_count; field != 0u; --field)
            drop_vm_value(context, &aggregate->fields[field - 1u], origin);
    } else if (value->kind == XR_VM_VALUE_EXISTENTIAL && value->as.existential) {
        XrVmExistentialValue *existential =
            (XrVmExistentialValue *) (void *) value->as.existential;
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
    *value = void_value();
}

static void dispose_detached_vm_value(XrVmValue *value) {
    if (value && value->kind == XR_VM_VALUE_STRING && value->as.string) {
        XrVmStringValue *string = (XrVmStringValue *) (void *) value->as.string;
        xr_free(string->bytes);
        xr_free(string);
        *value = void_value();
        return;
    }
    if (!value || value->kind != XR_VM_VALUE_AGGREGATE || !value->as.aggregate)
        return;
    XrVmAggregateValue *aggregate = (XrVmAggregateValue *) (void *) value->as.aggregate;
    for (uint32_t field = 0u; field < aggregate->field_count; ++field)
        dispose_detached_vm_value(&aggregate->fields[field]);
    xr_free(aggregate->fields);
    xr_free(aggregate);
    *value = void_value();
}

static bool detach_vm_value(XrVmValue source, XrVmValue *output) {
    if (!output)
        return false;
    *output = void_value();
    if (source.kind == XR_VM_VALUE_EXISTENTIAL || source.kind == XR_VM_VALUE_CALLABLE)
        return false;
    if (source.kind == XR_VM_VALUE_STRING) {
        /* A detached string is a private heap copy the outcome owner frees. */
        const XrVmStringValue *original = source.as.string;
        if (!original)
            return false;
        XrVmStringValue *copy = xr_calloc(1u, sizeof(*copy));
        if (!copy)
            return false;
        copy->bytes = xr_malloc(original->size != 0u ? original->size : 1u);
        if (!copy->bytes) {
            xr_free(copy);
            return false;
        }
        if (original->size != 0u)
            memcpy(copy->bytes, original->bytes, original->size);
        copy->size = original->size;
        *output = vm_string_value(copy);
        return true;
    }
    if (source.kind != XR_VM_VALUE_AGGREGATE) {
        *output = source;
        return true;
    }
    const XrVmAggregateValue *source_aggregate = source.as.aggregate;
    if (!source_aggregate || (source_aggregate->field_count != 0u && !source_aggregate->fields))
        return false;
#if SIZE_MAX < UINT64_MAX
    if ((size_t) source_aggregate->field_count > SIZE_MAX / sizeof(*source_aggregate->fields))
        return false;
#endif
    XrVmAggregateValue *aggregate = xr_calloc(1u, sizeof(*aggregate));
    if (!aggregate)
        return false;
    if (source_aggregate->field_count != 0u) {
        aggregate->fields = xr_calloc(source_aggregate->field_count, sizeof(*aggregate->fields));
        if (!aggregate->fields) {
            xr_free(aggregate);
            return false;
        }
    }
    aggregate->type_id = source_aggregate->type_id;
    aggregate->variant_ordinal = source_aggregate->variant_ordinal;
    aggregate->field_count = source_aggregate->field_count;
    XrVmValue detached = {
        .kind = XR_VM_VALUE_AGGREGATE,
        .as.aggregate = aggregate,
    };
    for (uint32_t field = 0u; field < aggregate->field_count; ++field) {
        if (!detach_vm_value(source_aggregate->fields[field], &aggregate->fields[field])) {
            dispose_detached_vm_value(&detached);
            return false;
        }
    }
    *output = detached;
    return true;
}

static bool vm_value_contains_class(XrVmValue value) {
    if (value.kind == XR_VM_VALUE_CLASS_REFERENCE)
        return true;
    if (value.kind != XR_VM_VALUE_AGGREGATE || !value.as.aggregate)
        return false;
    const XrVmAggregateValue *aggregate = value.as.aggregate;
    for (uint32_t field = 0u; field < aggregate->field_count; ++field)
        if (vm_value_contains_class(aggregate->fields[field]))
            return true;
    return false;
}

static bool vm_outcome_contains_class(XrVmOutcome outcome) {
    if (outcome.kind == XR_VM_OUTCOME_RETURN)
        return vm_value_contains_class(outcome.value);
    if (outcome.kind == XR_VM_OUTCOME_ERROR)
        return vm_value_contains_class(outcome.error_value);
    return false;
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

void xr_vm_outcome_dispose(XrVmOutcome *outcome) {
    if (!outcome)
        return;
    if (outcome->owns_dynamic_values) {
        dispose_detached_vm_value(&outcome->value);
        dispose_detached_vm_value(&outcome->error_value);
        dispose_detached_vm_value(&outcome->panic_value);
    }
    memset(outcome, 0, sizeof(*outcome));
}

static void free_value_storage(XrVmValueStorage *storage) {
    for (uint32_t index = 0; index < storage->aggregate_count; ++index) {
        xr_free(storage->aggregates[index]->fields);
        xr_free(storage->aggregates[index]);
    }
    xr_free(storage->aggregates);
    for (uint32_t index = 0; index < storage->class_count; ++index) {
        xr_free(storage->classes[index]->fields);
        xr_free(storage->classes[index]);
    }
    xr_free(storage->classes);
    for (uint32_t index = 0; index < storage->existential_count; ++index)
        xr_free(storage->existentials[index]);
    xr_free(storage->existentials);
    for (uint32_t index = 0; index < storage->callable_count; ++index)
        xr_free(storage->callables[index]);
    xr_free(storage->callables);
    for (uint32_t index = 0; index < storage->string_count; ++index) {
        xr_free(storage->strings[index]->bytes);
        xr_free(storage->strings[index]);
    }
    xr_free(storage->strings);
}

/* Outcome of a text operation in the common dispatcher. */
typedef enum VmTextStatus {
    VM_TEXT_OK = 0,
    VM_TEXT_RESOURCE_LIMIT,
    VM_TEXT_PROVIDER_FAILED,
    VM_TEXT_INVALID,
} VmTextStatus;

/* Executes the string/rune/output family through the shared text kernel;
 * both the baseline view and the fixed-row view route here. */
static VmTextStatus vm_execute_text_operation(XrVmContext *context, uint16_t operation_id,
                                              const uint32_t *operands, uint32_t operand_count,
                                              uint32_t constant_id, uint32_t predicate,
                                              uint32_t requirement_index, uint32_t operation_index,
                                              const XrVmRuntimeValue *values, XrVmValue *produced) {
    switch (operation_id) {
        case XR_CORE_OP_CORE_CONSTANT_STRING: {
            const XrValidatedConstant *constant = &context->code->program->constants[constant_id];
            XrVmStringValue *string = allocate_string(context, constant->value.string.size);
            if (!string)
                return VM_TEXT_RESOURCE_LIMIT;
            if (constant->value.string.size != 0u)
                memcpy(string->bytes, constant->value.string.bytes, constant->value.string.size);
            *produced = vm_string_value(string);
            return VM_TEXT_OK;
        }
        case XR_CORE_OP_CORE_CONSTANT_RUNE: {
            const XrValidatedConstant *constant = &context->code->program->constants[constant_id];
            produced->kind = XR_VM_VALUE_RUNE;
            produced->as.rune = constant->value.rune;
            return VM_TEXT_OK;
        }
        case XR_CORE_OP_CORE_STRING_FROM_I64: {
            int64_t source = values[operands[0]].as.value.as.i64;
            size_t size = xr_text_display_i64(source, NULL);
            XrVmStringValue *string = allocate_string(context, size);
            if (!string)
                return VM_TEXT_RESOURCE_LIMIT;
            (void) xr_text_display_i64(source, string->bytes);
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
                memset(operand, 0, sizeof(*operand));
                switch (value->kind) {
                    case XR_VM_VALUE_I64:
                        operand->kind = XR_TEXT_DISPLAY_I64;
                        operand->i64 = value->as.i64;
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
                            if (display != stack_operands)
                                xr_free(display);
                            return VM_TEXT_INVALID;
                        }
                        operand->kind = XR_TEXT_DISPLAY_STRING;
                        operand->bytes = bytes;
                        operand->size = size;
                        break;
                    }
                    default:
                        if (display != stack_operands)
                            xr_free(display);
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

static void hash_u64(XrSHA256Context *context, uint64_t value) {
    uint8_t bytes[8];
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
    if (code->options.decode_policy == XR_VM_DECODE_FIXED_ROWS) {
        const XrVmFixedInstruction *source =
            &code->fixed_functions[function_id].blocks[block_id].instructions[instruction_id];
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
    } else {
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
    }
    return view;
}

static XrVmOutcome vm_provider_call(XrVmContext *context, const XrValidatedFunction *function,
                                    const XrVmInstructionView *instruction,
                                    const XrVmRuntimeValue *values, uint32_t operand_count) {
    uint16_t operand_type =
        operand_count == 1u ? function->value_types[instruction->operands[0]] : XR_CORE_TYPE_VOID;
    XrProviderLogicalCallKind call_kind = xr_validated_program_provider_call_kind(
        context->code->program, instruction->result_type_id,
        operand_count == 1u ? &operand_type : NULL, operand_count);
    XrVmOutcome result = vm_outcome(XR_VM_OUTCOME_RETURN, context);
    XrExecutionProviderCallResult call = XR_EXECUTION_PROVIDER_CALL_FAILED;
    if (call_kind == XR_PROVIDER_LOGICAL_CALL_I64_UNARY ||
        call_kind == XR_PROVIDER_LOGICAL_CALL_I64_NULLARY) {
        int64_t provider_result = 0;
        call =
            call_kind == XR_PROVIDER_LOGICAL_CALL_I64_UNARY
                ? xr_execution_lease_provider_call_i64_unary(
                      context->lease, instruction->immediate.provider_operation.requirement_index,
                      instruction->immediate.provider_operation.operation_index,
                      values[instruction->operands[0]].as.value.as.i64, &provider_result)
                : xr_execution_lease_provider_call_i64_nullary(
                      context->lease, instruction->immediate.provider_operation.requirement_index,
                      instruction->immediate.provider_operation.operation_index, &provider_result);
        if (call == XR_EXECUTION_PROVIDER_CALL_OK) {
            result.value.kind = XR_VM_VALUE_I64;
            result.value.as.i64 = provider_result;
        }
    } else if (call_kind == XR_PROVIDER_LOGICAL_CALL_BOOL_I64_UNARY) {
        bool provider_result = false;
        call = xr_execution_lease_provider_call_bool_i64_unary(
            context->lease, instruction->immediate.provider_operation.requirement_index,
            instruction->immediate.provider_operation.operation_index,
            values[instruction->operands[0]].as.value.as.i64, &provider_result);
        if (call == XR_EXECUTION_PROVIDER_CALL_OK) {
            result.value.kind = XR_VM_VALUE_BOOL;
            result.value.as.boolean = provider_result;
        }
    } else if (call_kind == XR_PROVIDER_LOGICAL_CALL_OPTIONAL_I64_PAIR_NULLARY) {
        uint16_t pair_type_id = XR_CORE_TYPE_VOID;
        (void) xr_validated_program_type_is_optional_i64_pair(
            context->code->program, instruction->result_type_id, &pair_type_id);
        XrVmAggregateValue *pair = allocate_aggregate(context, pair_type_id, UINT32_MAX, 2u);
        XrVmAggregateValue *optional =
            allocate_aggregate(context, instruction->result_type_id, 1u, 1u);
        if (!pair || !optional)
            return vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
        bool present = false;
        int64_t first = 0;
        int64_t second = 0;
        call = xr_execution_lease_provider_call_optional_i64_pair_nullary(
            context->lease, instruction->immediate.provider_operation.requirement_index,
            instruction->immediate.provider_operation.operation_index, &present, &first, &second);
        if (call == XR_EXECUTION_PROVIDER_CALL_OK) {
            if (present) {
                pair->fields[0] = (XrVmValue) {.kind = XR_VM_VALUE_I64, .as.i64 = first};
                pair->fields[1] = (XrVmValue) {.kind = XR_VM_VALUE_I64, .as.i64 = second};
                optional->fields[0] =
                    (XrVmValue) {.kind = XR_VM_VALUE_AGGREGATE, .as.aggregate = pair};
            } else {
                optional->variant_ordinal = 0u;
                optional->field_count = 0u;
            }
            result.value.kind = XR_VM_VALUE_AGGREGATE;
            result.value.as.aggregate = optional;
        }
    }
    return call == XR_EXECUTION_PROVIDER_CALL_OK
               ? result
               : vm_trap(XR_VM_TRAP_PROVIDER_CALL_FAILED, context);
}

static XrVmOutcome vm_execution_step(XrVmExecution *execution);
static bool vm_execution_allocate_values(XrVmExecution *execution,
                                         const XrValidatedFunction *function);
static void vm_execution_free_values(XrVmExecution *execution);

static XrVmOutcome execute_function(XrVmContext *context, uint32_t function_id,
                                    const XrVmRuntimeValue *arguments, uint32_t argument_count,
                                    uint32_t depth) {
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
    XrVmOutcome result = vm_execution_step(&execution);
    *context = execution.context;
    vm_execution_free_values(&execution);
    xr_vm_code_free(execution.code);
    return result;
}

static void fixed_view_free(XrVmCode *code) {
    if (!code || !code->fixed_functions)
        return;
    for (uint32_t function = 0; function < code->program->function_count; ++function) {
        XrVmFixedFunction *fixed = &code->fixed_functions[function];
        for (uint32_t block = 0; block < fixed->block_count; ++block)
            xr_free(fixed->blocks[block].instructions);
        xr_free(fixed->blocks);
    }
    xr_free(code->fixed_functions);
    code->fixed_functions = NULL;
}

static bool fixed_view_build(XrVmCode *code) {
    code->fixed_functions = xr_calloc(code->program->function_count, sizeof(XrVmFixedFunction));
    if (!code->fixed_functions)
        return false;
    code->private_size = (size_t) code->program->function_count * sizeof(XrVmFixedFunction);
    for (uint32_t function = 0; function < code->program->function_count; ++function) {
        const XrValidatedFunction *source_function = &code->program->functions[function];
        XrVmFixedFunction *destination_function = &code->fixed_functions[function];
        destination_function->block_count = source_function->block_count;
        destination_function->blocks =
            xr_calloc(source_function->block_count, sizeof(XrVmFixedBlock));
        if (!destination_function->blocks)
            return false;
        code->private_size += (size_t) source_function->block_count * sizeof(XrVmFixedBlock);
        for (uint32_t block = 0; block < source_function->block_count; ++block) {
            const XrValidatedBlock *source_block = &source_function->blocks[block];
            XrVmFixedBlock *destination_block = &destination_function->blocks[block];
            destination_block->instruction_count = source_block->instruction_count;
            destination_block->instructions =
                xr_calloc(source_block->instruction_count, sizeof(XrVmFixedInstruction));
            if (!destination_block->instructions)
                return false;
            code->private_size +=
                (size_t) source_block->instruction_count * sizeof(XrVmFixedInstruction);
            for (uint32_t instruction = 0; instruction < source_block->instruction_count;
                 ++instruction) {
                const XrValidatedInstruction *source = &source_block->instructions[instruction];
                XrVmFixedInstruction *destination = &destination_block->instructions[instruction];
                destination->operation_id = source->operation_id;
                destination->result_type_id = source->result_type_id;
                destination->result_category = source->result_category;
                destination->result_id = source->result_id;
                destination->operands = source->operands;
                destination->operand_count = source->operand_count;
                destination->immediate_kind = source->immediate_kind;
                memcpy(&destination->immediate, &source->immediate, sizeof(destination->immediate));
                destination->successors = source->successors;
                destination->successor_count = source->successor_count;
            }
        }
    }
    return true;
}

static void compute_private_digest(XrVmCode *code) {
    static const uint8_t domain[] = "xray-private-vm-code-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    xr_sha256_update(&context, code->execution_id.bytes, sizeof(code->execution_id.bytes));
    xr_sha256_update(&context, (const uint8_t *) XR_VM_BUILD_ID, sizeof(XR_VM_BUILD_ID));
    xr_sha256_update(&context, &code->options.decode_policy, sizeof(code->options.decode_policy));
    xr_sha256_update(&context, &code->options.quickening_policy,
                     sizeof(code->options.quickening_policy));
    hash_u32(&context, code->pointer_width);
    hash_u32(&context, code->operating_system);
    hash_u32(&context, code->architecture);
    hash_u32(&context, code->native_abi);
    hash_u32(&context, code->endianness);
    hash_u64(&context, (uint64_t) code->private_size);
    xr_sha256_final(&context, code->private_digest.bytes);
}

XrVmCodeOptions xr_vm_code_default_options(void) {
    XrVmCodeOptions options = {
        .schema_version = XR_VM_CODE_OPTIONS_SCHEMA_VERSION,
        .decode_policy = XR_VM_DECODE_BASELINE_VIEW,
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
                bool h2_class_operation = operation_id == XR_CORE_OP_CORE_CLASS_CONSTRUCT ||
                                          operation_id == XR_CORE_OP_CORE_CLASS_SHARE ||
                                          operation_id == XR_CORE_OP_CORE_CLASS_FIELD_LOAD ||
                                          operation_id == XR_CORE_OP_CORE_CLASS_FIELD_PLACE ||
                                          operation_id == XR_CORE_OP_CORE_PLACE_EXCHANGE;
                const XrValidatedType *result_type = xr_validated_program_type(
                    program, instruction_row->result_type_id);
                bool deferred_class_copy = operation_id == XR_CORE_OP_CORE_OWNER_COPY &&
                                           result_type &&
                                           result_type->kind == XR_CORE_IR_TYPE_CLASS_REFERENCE;
                if (!deferred_class_copy &&
                    ((spec && spec->vm_status == XR_CORE_COVERAGE_COMPLETE) ||
                     h2_class_operation))
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
        selected.schema_version != XR_VM_CODE_OPTIONS_SCHEMA_VERSION || selected.reserved16 != 0u ||
        selected.max_steps == 0u || selected.max_value_cells == 0u ||
        selected.max_call_depth == 0u) {
        if (diagnostic_out)
            diagnostic_out->status = XR_VM_CODE_INVALID_INPUT;
        return XR_VM_CODE_INVALID_INPUT;
    }
    if ((selected.decode_policy != XR_VM_DECODE_BASELINE_VIEW &&
         selected.decode_policy != XR_VM_DECODE_FIXED_ROWS) ||
        selected.quickening_policy != XR_VM_QUICKENING_NONE) {
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
    if (program->module_count != 0u) {
        if (diagnostic_out)
            diagnostic_out->status = XR_VM_CODE_UNSUPPORTED_OPERATION;
        return XR_VM_CODE_UNSUPPORTED_OPERATION;
    }
    if (!vm_program_operations_active(program, diagnostic_out))
        return XR_VM_CODE_UNSUPPORTED_OPERATION;
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
    if (selected.decode_policy == XR_VM_DECODE_FIXED_ROWS && !fixed_view_build(code)) {
        xr_vm_code_free(code);
        if (diagnostic_out)
            diagnostic_out->status = XR_VM_CODE_OUT_OF_MEMORY;
        return XR_VM_CODE_OUT_OF_MEMORY;
    }
    compute_private_digest(code);
    *code_out = code;
    return XR_VM_CODE_OK;
}

void xr_vm_code_free(XrVmCode *code) {
    if (!code)
        return;
    if (atomic_fetch_sub_explicit(&code->references, 1u, memory_order_acq_rel) != 1u)
        return;
    fixed_view_free(code);
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

size_t xr_vm_code_private_size(const XrVmCode *code) {
    return code ? code->private_size : 0u;
}

XrVmDecodePolicy xr_vm_code_decode_policy(const XrVmCode *code) {
    return code ? (XrVmDecodePolicy) code->options.decode_policy : XR_VM_DECODE_INVALID;
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
    XrVmExecution *child = xr_calloc(1u, sizeof(*child));
    if (!child)
        return false;
    child->lease = parent->lease;
    child->context.code = parent->context.code;
    child->context.lease = &child->lease;
    child->context.storage = parent->context.storage;
    child->code = xr_vm_code_retain(parent->code);
    child->depth = parent->depth + 1u;
    child->function_id = function_id;
    child->block_id = function->entry_block;
    child->cancel_block_id = XR_PROGRAM_LOCATION_NONE;
    child->suspension_block_id = XR_PROGRAM_LOCATION_NONE;
    child->suspension_instruction_id = XR_PROGRAM_LOCATION_NONE;
    if (!child->code || !vm_execution_allocate_values(child, function)) {
        xr_vm_execution_free(child);
        return false;
    }
    static const uint8_t trace_domain[] = "xray-vm-coroutine-child-logical-trace-v1\0";
    xr_sha256_init(&child->context.trace);
    xr_sha256_update(&child->context.trace, trace_domain, sizeof(trace_domain) - 1u);
    xr_sha256_update(&child->context.trace, parent->code->execution_id.bytes,
                     sizeof(parent->code->execution_id.bytes));
    hash_u32(&child->context.trace, function_id);
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
        XrCoreIrValueCategory expected = function_parameter_category(
            parent->context.code->program, function, target_argument);
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
    if (!xr_execution_instance_acquire(instance, &execution->lease)) {
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
    if (!vm_execution_allocate_values(execution, function)) {
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
               successor_index != 0u && instruction.successor_count <= 3u) {
        bool indirect = instruction.operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT;
        uint32_t callee_id = instruction.immediate.coroutine_call.function_id;
        uint32_t parameter_prefix = 0u;
        uint16_t result_type_id = XR_CORE_TYPE_VOID;
        if (indirect) {
            const XrVmCallableValue *carrier =
                execution->values[instruction.operands[0]].as.value.as.callable;
            callee_id = callable_function_id(execution->context.code->program, carrier);
            const XrValidatedType *callable =
                carrier ? xr_validated_program_type(execution->context.code->program,
                                                    carrier->callable_type_id)
                        : NULL;
            const XrValidatedSignature *signature =
                callable &&
                        callable->signature_id < execution->context.code->program->signature_count
                    ? &execution->context.code->program->signatures[callable->signature_id]
                    : NULL;
            if (!signature)
                return false;
            parameter_prefix = signature->parameter_count + 1u;
            result_type_id = signature->result_type_id;
        }
        if (callee_id >= execution->context.code->program->function_count)
            return false;
        const XrValidatedFunction *callee = &execution->context.code->program->functions[callee_id];
        if (!indirect) {
            parameter_prefix = callee->parameter_count;
            result_type_id = callee->result_type_id;
        }
        const XrValidatedBlock *normal = &function->blocks[instruction.successors[0]];
        uint32_t implicit_result = result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u;
        if (normal->argument_count < implicit_result)
            return false;
        operand_start = parameter_prefix + normal->argument_count - implicit_result;
        if (successor_index == 2u)
            operand_start += function->blocks[instruction.successors[1]].argument_count;
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
            vm_dispatch_arithmetic(dispatch);
            return;
        case XR_CORE_OP_CORE_LOGICAL_NOT:
        case XR_CORE_OP_CORE_LOGICAL_AND:
        case XR_CORE_OP_CORE_LOGICAL_OR:
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
        case XR_CORE_OP_CORE_STRING_FROM_I64:
        case XR_CORE_OP_CORE_STRING_CONCAT:
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
        case XR_CORE_OP_CORE_CLASS_SHARE:
            vm_dispatch_class_owner(dispatch);
            return;
        case XR_CORE_OP_CORE_CLASS_FIELD_LOAD:
        case XR_CORE_OP_CORE_CLASS_FIELD_PLACE:
            vm_dispatch_class_field(dispatch);
            return;
        case XR_CORE_OP_CORE_PLACE_LOCAL:
        case XR_CORE_OP_CORE_PLACE_LOAD:
        case XR_CORE_OP_CORE_PLACE_STORE:
        case XR_CORE_OP_CORE_PLACE_PROJECT:
        case XR_CORE_OP_CORE_PLACE_TAKE:
        case XR_CORE_OP_CORE_PLACE_EXCHANGE:
            vm_dispatch_place(dispatch);
            return;
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

static XrVmOutcome vm_execution_step(XrVmExecution *execution) {
    if (!execution || execution->finished || !xr_execution_lease_is_valid(&execution->lease))
        return vm_execution_outcome(execution, XR_VM_OUTCOME_INVALID_INVOCATION);
    if (execution->suspended && !execution->child &&
        !vm_materialize_suspension_edge(execution, 0u)) {
        execution->finished = true;
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
    for (;;) {
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
        if (context->steps == context->code->options.max_steps) {
            result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
            goto done;
        }
        ++context->steps;
        trace_instruction(context, execution->function_id, execution->block_id, instruction_id,
                          dispatch.instruction.operation_id);
        for (uint32_t operand = 0u; operand < dispatch.instruction.operand_count; ++operand)
            if (!execution->initialized[dispatch.instruction.operands[operand]])
                goto done;
        vm_dispatch_instruction(&dispatch);
        if (dispatch.returned)
            return dispatch.outcome;
        if (dispatch.terminal) {
            result = dispatch.outcome;
            goto done;
        }
        if (dispatch.transferred) {
            if (!dispatch.edge_materialized) {
                const XrValidatedBlock *target = &function->blocks[dispatch.block_id];
                if (dispatch.incoming_count != target->argument_count)
                    goto done;
                for (uint32_t argument = 0u; argument < dispatch.incoming_count; ++argument) {
                    uint32_t value_id = target->argument_ids[argument];
                    execution->values[value_id] = execution->edge_values[argument];
                    execution->initialized[value_id] = true;
                }
            }
            execution->block_id = dispatch.block_id;
            execution->instruction_id = 0u;
        } else if (dispatch.instruction.result_id != XR_PROGRAM_LOCATION_NONE) {
            execution->values[dispatch.instruction.result_id] = dispatch.produced;
            execution->initialized[dispatch.instruction.result_id] = true;
        }
    }

done:
    execution->finished = true;
    result.logical_trace = vm_execution_outcome(execution, result.kind).logical_trace;
    result.steps = context->steps;
    result.state_id = execution->state_id;
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
            vm_execution_release_lease(execution);
            return vm_execution_outcome(execution, XR_VM_OUTCOME_RESOURCE_LIMIT);
        }
        execution->context.steps += child_delta;

        XrVmInstructionView instruction;
        if (child.kind == XR_VM_OUTCOME_TRAP && child.trap == XR_VM_TRAP_PROVIDER_CALL_FAILED &&
            vm_suspension_instruction(execution, &instruction) &&
            (instruction.operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED ||
             instruction.operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT) &&
            instruction.successor_count == 3u) {
            successor_index = 2u;
        } else if (child.kind != XR_VM_OUTCOME_CANCELLED) {
            execution->finished = true;
            xr_vm_execution_free(execution->child);
            execution->child = NULL;
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

/* Child results remain in the shared execution-tree storage.
 * Only a host-facing outcome crosses the unsupported class-export boundary. */
XrVmOutcome xr_vm_execution_step(XrVmExecution *execution) {
    XrVmOutcome outcome = vm_execution_step(execution);
    return vm_outcome_contains_class(outcome)
               ? vm_execution_outcome(execution, XR_VM_OUTCOME_INVALID_INVOCATION)
               : outcome;
}

XrVmOutcome xr_vm_execution_cancel(XrVmExecution *execution) {
    XrVmOutcome outcome = vm_execution_cancel(execution);
    return vm_outcome_contains_class(outcome)
               ? vm_execution_outcome(execution, XR_VM_OUTCOME_INVALID_INVOCATION)
               : outcome;
}

void xr_vm_execution_free(XrVmExecution *execution) {
    if (!execution)
        return;
    xr_vm_execution_free(execution->child);
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
    if (!xr_vm_code_matches_instance(code, instance) ||
        !xr_execution_instance_acquire(instance, &lease))
        return vm_outcome(XR_VM_OUTCOME_STALE_CODE, &context);
    context.lease = &lease;
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
    XrVmOutcome outcome =
        execute_function(&context, function_id, runtime_arguments, argument_count, 1u);
    xr_free(runtime_arguments);
    if (vm_outcome_contains_class(outcome))
        outcome = vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, &context);
    if (outcome.kind == XR_VM_OUTCOME_RETURN &&
        (outcome.value.kind == XR_VM_VALUE_AGGREGATE || outcome.value.kind == XR_VM_VALUE_STRING)) {
        XrVmValue detached = void_value();
        if (detach_vm_value(outcome.value, &detached)) {
            outcome.value = detached;
            outcome.owns_dynamic_values = true;
        } else {
            outcome = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, &context);
        }
    }
    if (outcome.kind == XR_VM_OUTCOME_RETURN && (outcome.value.kind == XR_VM_VALUE_EXISTENTIAL ||
                                                 outcome.value.kind == XR_VM_VALUE_CALLABLE))
        outcome = vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, &context);
    if (outcome.kind == XR_VM_OUTCOME_ERROR && outcome.error_value.kind == XR_VM_VALUE_AGGREGATE) {
        XrVmValue detached = void_value();
        if (detach_vm_value(outcome.error_value, &detached)) {
            outcome.error_value = detached;
            outcome.owns_dynamic_values = true;
        } else {
            outcome = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, &context);
        }
    }
    if (outcome.kind == XR_VM_OUTCOME_ERROR &&
        (outcome.error_value.kind == XR_VM_VALUE_EXISTENTIAL ||
         outcome.error_value.kind == XR_VM_VALUE_CALLABLE))
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
        hash_u32(&context.trace, outcome.panic_value.as.panic_info);
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
