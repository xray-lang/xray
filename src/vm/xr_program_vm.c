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

#include "../base/xmalloc.h"
#include "../base/xsha256.h"
#include "../core/xr_core_spec_gen.h"
#include "../program/xr_validated_program_internal.h"

#include <limits.h>
#include <string.h>

typedef struct XrVmExistentialValue XrVmExistentialValue;
typedef struct XrVmCallableValue XrVmCallableValue;
typedef struct XrVmClassValue XrVmClassValue;

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
    XrExecutionCacheKey cache_key;
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

typedef struct XrVmContext {
    const XrVmCode *code;
    const XrExecutionLease *lease;
    uint64_t steps;
    uint64_t aggregate_cell_count;
    uint64_t next_class_identity;
    XrSHA256Context trace;
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
} XrVmContext;

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
    const XrValidatedType *type = xr_validated_program_type(context->code->program, type_id);
    if (!type || type->kind != XR_CORE_IR_TYPE_CLASS_REFERENCE || type->field_count != field_count ||
        (uint64_t) field_count >
            (uint64_t) context->code->options.max_value_cells - context->aggregate_cell_count)
        return NULL;
    if (context->class_count == context->class_capacity) {
        uint32_t capacity = context->class_capacity ? context->class_capacity * 2u : 8u;
        if (capacity < context->class_count ||
            (size_t) capacity > SIZE_MAX / sizeof(*context->classes))
            return NULL;
        XrVmClassValue **grown =
            xr_realloc(context->classes, (size_t) capacity * sizeof(*context->classes));
        if (!grown)
            return NULL;
        context->classes = grown;
        context->class_capacity = capacity;
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
    value->identity = ++context->next_class_identity;
    value->alive = true;
    context->classes[context->class_count++] = value;
    context->aggregate_cell_count += field_count;
    return value;
}

static XrVmAggregateValue *allocate_aggregate(XrVmContext *context, uint16_t type_id,
                                              uint32_t variant_ordinal, uint32_t field_count) {
    if ((uint64_t) field_count >
        (uint64_t) context->code->options.max_value_cells - context->aggregate_cell_count)
        return NULL;
    if (context->aggregate_count == context->aggregate_capacity) {
        uint32_t capacity = context->aggregate_capacity ? context->aggregate_capacity * 2u : 8u;
        if (capacity < context->aggregate_count)
            return NULL;
#if SIZE_MAX < UINT64_MAX
        if ((size_t) capacity > SIZE_MAX / sizeof(*context->aggregates))
            return NULL;
#endif
        XrVmAggregateValue **grown =
            xr_realloc(context->aggregates, (size_t) capacity * sizeof(*context->aggregates));
        if (!grown)
            return NULL;
        context->aggregates = grown;
        context->aggregate_capacity = capacity;
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
    context->aggregates[context->aggregate_count++] = aggregate;
    context->aggregate_cell_count += field_count;
    return aggregate;
}

static XrVmExistentialValue *allocate_existential(XrVmContext *context) {
    if (context->aggregate_cell_count == context->code->options.max_value_cells)
        return NULL;
    if (context->existential_count == context->existential_capacity) {
        uint32_t capacity = context->existential_capacity ? context->existential_capacity * 2u : 8u;
        if (capacity < context->existential_count)
            return NULL;
#if SIZE_MAX < UINT64_MAX
        if ((size_t) capacity > SIZE_MAX / sizeof(*context->existentials))
            return NULL;
#endif
        XrVmExistentialValue **grown =
            xr_realloc(context->existentials, (size_t) capacity * sizeof(*context->existentials));
        if (!grown)
            return NULL;
        context->existentials = grown;
        context->existential_capacity = capacity;
    }
    XrVmExistentialValue *value = xr_calloc(1u, sizeof(*value));
    if (!value)
        return NULL;
    context->existentials[context->existential_count++] = value;
    ++context->aggregate_cell_count;
    return value;
}

static XrVmCallableValue *allocate_callable(XrVmContext *context) {
    if (context->aggregate_cell_count == context->code->options.max_value_cells)
        return NULL;
    if (context->callable_count == context->callable_capacity) {
        uint32_t capacity = context->callable_capacity ? context->callable_capacity * 2u : 8u;
        if (capacity < context->callable_count)
            return NULL;
#if SIZE_MAX < UINT64_MAX
        if ((size_t) capacity > SIZE_MAX / sizeof(*context->callables))
            return NULL;
#endif
        XrVmCallableValue **grown =
            xr_realloc(context->callables, (size_t) capacity * sizeof(*context->callables));
        if (!grown)
            return NULL;
        context->callables = grown;
        context->callable_capacity = capacity;
    }
    XrVmCallableValue *value = xr_calloc(1u, sizeof(*value));
    if (!value)
        return NULL;
    context->callables[context->callable_count++] = value;
    ++context->aggregate_cell_count;
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

static void free_aggregates(XrVmContext *context) {
    for (uint32_t index = 0; index < context->aggregate_count; ++index) {
        xr_free(context->aggregates[index]->fields);
        xr_free(context->aggregates[index]);
    }
    xr_free(context->aggregates);
    for (uint32_t index = 0; index < context->class_count; ++index) {
        xr_free(context->classes[index]->fields);
        xr_free(context->classes[index]);
    }
    xr_free(context->classes);
    for (uint32_t index = 0; index < context->existential_count; ++index)
        xr_free(context->existentials[index]);
    xr_free(context->existentials);
    for (uint32_t index = 0; index < context->callable_count; ++index)
        xr_free(context->callables[index]);
    xr_free(context->callables);
}

static int64_t i64_from_bits(uint64_t bits) {
    if (bits <= (uint64_t) INT64_MAX)
        return (int64_t) bits;
    return -(int64_t) (~bits) - 1;
}

static size_t format_i64_line(int64_t value, uint8_t output[22]) {
    uint8_t reverse[20];
    size_t count = 0u;
    uint64_t magnitude = value < 0 ? UINT64_C(0) - (uint64_t) value : (uint64_t) value;
    do {
        reverse[count++] = (uint8_t) ('0' + magnitude % UINT64_C(10));
        magnitude /= UINT64_C(10);
    } while (magnitude != 0u);
    size_t cursor = 0u;
    if (value < 0)
        output[cursor++] = (uint8_t) '-';
    while (count != 0u)
        output[cursor++] = reverse[--count];
    output[cursor++] = (uint8_t) '\n';
    return cursor;
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
            function->parameter_modes[index] == XR_PARAM_REF ? XR_CORE_IR_PLACE : XR_CORE_IR_VALUE;
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

    size_t value_count = function->value_count ? function->value_count : 1u;
    XrVmRuntimeValue *values = xr_calloc(value_count, sizeof(XrVmRuntimeValue));
    XrVmPlace *places = xr_calloc(value_count, sizeof(XrVmPlace));
    bool *initialized = xr_calloc(value_count, sizeof(bool));
    uint32_t scratch_count = function->parameter_count;
    for (uint32_t block = 0; block < function->block_count; ++block) {
        const XrValidatedBlock *row = &function->blocks[block];
        if (row->argument_count > scratch_count)
            scratch_count = row->argument_count;
        for (uint32_t instruction = 0; instruction < row->instruction_count; ++instruction) {
            if (row->instructions[instruction].operand_count > scratch_count)
                scratch_count = row->instructions[instruction].operand_count;
        }
    }
    XrVmRuntimeValue *scratch =
        xr_calloc(scratch_count ? scratch_count : 1u, sizeof(XrVmRuntimeValue));
    if (!values || !places || !initialized || !scratch) {
        xr_free(scratch);
        xr_free(initialized);
        xr_free(places);
        xr_free(values);
        return vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
    }

    uint32_t block_id = function->entry_block;
    uint32_t incoming_count = argument_count;
    if (incoming_count != 0u)
        memcpy(scratch, arguments, (size_t) incoming_count * sizeof(XrVmRuntimeValue));
    XrVmOutcome result = vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, context);
    for (;;) {
        const XrValidatedBlock *block = &function->blocks[block_id];
        if (incoming_count != block->argument_count)
            break;
        for (uint32_t argument = 0; argument < block->argument_count; ++argument) {
            uint32_t value_id = block->argument_ids[argument];
            values[value_id] = scratch[argument];
            initialized[value_id] = true;
        }
        bool transferred = false;
        for (uint32_t instruction_id = 0; instruction_id < block->instruction_count;
             ++instruction_id) {
            XrVmInstructionView instruction =
                instruction_view(context->code, function_id, block_id, instruction_id);
            if (context->steps == context->code->options.max_steps) {
                result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                goto done;
            }
            ++context->steps;
            trace_instruction(context, function_id, block_id, instruction_id,
                              instruction.operation_id);
            for (uint32_t operand = 0; operand < instruction.operand_count; ++operand) {
                if (!initialized[instruction.operands[operand]])
                    goto done;
            }
            XrVmRuntimeValue produced = {
                .category = XR_CORE_IR_VALUE,
                .as.value = void_value(),
            };
            bool has_result = instruction.result_id != XR_PROGRAM_LOCATION_NONE;
            switch (instruction.operation_id) {
                case XR_CORE_OP_CORE_CONSTANT_I64: {
                    const XrValidatedConstant *constant =
                        &context->code->program->constants[instruction.immediate.constant_id];
                    produced.as.value.kind = XR_VM_VALUE_I64;
                    produced.as.value.as.i64 = constant->value.i64;
                    break;
                }
                case XR_CORE_OP_CORE_CONSTANT_BOOL: {
                    const XrValidatedConstant *constant =
                        &context->code->program->constants[instruction.immediate.constant_id];
                    produced.as.value.kind = XR_VM_VALUE_BOOL;
                    produced.as.value.as.boolean = constant->value.boolean;
                    break;
                }
                case XR_CORE_OP_CORE_CONSTANT_TARGET_ENUM:
                    produced.as.value.kind =
                        instruction.result_type_id == XR_CORE_TYPE_TARGET_OS ? XR_VM_VALUE_TARGET_OS
                        : instruction.result_type_id == XR_CORE_TYPE_TARGET_ARCH
                            ? XR_VM_VALUE_TARGET_ARCH
                        : instruction.result_type_id == XR_CORE_TYPE_TARGET_ABI
                            ? XR_VM_VALUE_TARGET_ABI
                            : XR_VM_VALUE_TARGET_ENDIAN;
                    produced.as.value.as.target_enum = (uint16_t) instruction.immediate.u32;
                    break;
                case XR_CORE_OP_CORE_ADD_I64:
                case XR_CORE_OP_CORE_SUB_I64:
                case XR_CORE_OP_CORE_MUL_I64: {
                    int64_t left = values[instruction.operands[0]].as.value.as.i64;
                    int64_t right = values[instruction.operands[1]].as.value.as.i64;
                    int64_t exact = 0;
                    bool valid = instruction.operation_id == XR_CORE_OP_CORE_ADD_I64
                                     ? checked_add(left, right, &exact)
                                 : instruction.operation_id == XR_CORE_OP_CORE_SUB_I64
                                     ? checked_sub(left, right, &exact)
                                     : checked_mul(left, right, &exact);
                    if (instruction.immediate.u32 == 0u && !valid) {
                        result = vm_trap(XR_VM_TRAP_INTEGER_OVERFLOW, context);
                        goto done;
                    }
                    if (instruction.immediate.u32 != 0u) {
                        uint64_t bits = instruction.operation_id == XR_CORE_OP_CORE_ADD_I64
                                            ? (uint64_t) left + (uint64_t) right
                                        : instruction.operation_id == XR_CORE_OP_CORE_SUB_I64
                                            ? (uint64_t) left - (uint64_t) right
                                            : (uint64_t) left * (uint64_t) right;
                        exact = i64_from_bits(bits);
                    }
                    produced.as.value.kind = XR_VM_VALUE_I64;
                    produced.as.value.as.i64 = exact;
                    break;
                }
                case XR_CORE_OP_CORE_DIV_I64: {
                    int64_t left = values[instruction.operands[0]].as.value.as.i64;
                    int64_t right = values[instruction.operands[1]].as.value.as.i64;
                    if (right == 0) {
                        result = vm_trap(XR_VM_TRAP_INTEGER_DIVISION_BY_ZERO, context);
                        goto done;
                    }
                    if (left == INT64_MIN && right == -1) {
                        result = vm_trap(XR_VM_TRAP_INTEGER_DIVISION_OVERFLOW, context);
                        goto done;
                    }
                    produced.as.value.kind = XR_VM_VALUE_I64;
                    produced.as.value.as.i64 = left / right;
                    break;
                }
                case XR_CORE_OP_CORE_LOGICAL_NOT:
                    produced.as.value.kind = XR_VM_VALUE_BOOL;
                    produced.as.value.as.boolean =
                        !values[instruction.operands[0]].as.value.as.boolean;
                    break;
                case XR_CORE_OP_CORE_LOGICAL_AND:
                case XR_CORE_OP_CORE_LOGICAL_OR: {
                    bool left = values[instruction.operands[0]].as.value.as.boolean;
                    bool right = values[instruction.operands[1]].as.value.as.boolean;
                    produced.as.value.kind = XR_VM_VALUE_BOOL;
                    produced.as.value.as.boolean =
                        instruction.operation_id == XR_CORE_OP_CORE_LOGICAL_AND ? left && right
                                                                                : left || right;
                    break;
                }
                case XR_CORE_OP_CORE_COMPARE_I64: {
                    int64_t left = values[instruction.operands[0]].as.value.as.i64;
                    int64_t right = values[instruction.operands[1]].as.value.as.i64;
                    bool comparison = false;
                    switch (instruction.immediate.u32) {
                        case 0:
                            comparison = left == right;
                            break;
                        case 1:
                            comparison = left != right;
                            break;
                        case 2:
                            comparison = left < right;
                            break;
                        case 3:
                            comparison = left <= right;
                            break;
                        case 4:
                            comparison = left > right;
                            break;
                        case 5:
                            comparison = left >= right;
                            break;
                        default:
                            goto done;
                    }
                    produced.as.value.kind = XR_VM_VALUE_BOOL;
                    produced.as.value.as.boolean = comparison;
                    break;
                }
                case XR_CORE_OP_CORE_COMPARE_TARGET_ENUM: {
                    uint16_t left = values[instruction.operands[0]].as.value.as.target_enum;
                    uint16_t right = values[instruction.operands[1]].as.value.as.target_enum;
                    produced.as.value.kind = XR_VM_VALUE_BOOL;
                    produced.as.value.as.boolean =
                        instruction.immediate.u32 == 0u ? left == right : left != right;
                    break;
                }
                case XR_CORE_OP_CORE_BLOCK_ARGUMENT:
                    break;
                case XR_CORE_OP_CORE_BRANCH: {
                    const XrValidatedBlock *target = &function->blocks[instruction.successors[0]];
                    for (uint32_t index = 0; index < target->argument_count; ++index)
                        scratch[index] = values[instruction.operands[index]];
                    incoming_count = target->argument_count;
                    block_id = instruction.successors[0];
                    transferred = true;
                    break;
                }
                case XR_CORE_OP_CORE_CONDITIONAL_BRANCH: {
                    bool condition = values[instruction.operands[0]].as.value.as.boolean;
                    uint32_t successor = condition ? 0u : 1u;
                    uint32_t operand =
                        condition ? 1u
                                  : 1u + function->blocks[instruction.successors[0]].argument_count;
                    const XrValidatedBlock *target =
                        &function->blocks[instruction.successors[successor]];
                    for (uint32_t index = 0; index < target->argument_count; ++index)
                        scratch[index] = values[instruction.operands[operand + index]];
                    incoming_count = target->argument_count;
                    block_id = instruction.successors[successor];
                    transferred = true;
                    break;
                }
                case XR_CORE_OP_CORE_ASSERT_CONDITION:
                    if (!values[instruction.operands[0]].as.value.as.boolean) {
                        XrVmValue panic = {
                            .kind = XR_VM_VALUE_PANIC_INFO,
                            .as.panic_info = instruction.immediate.u32,
                        };
                        if (instruction.successor_count == 0u) {
                            result = vm_outcome(XR_VM_OUTCOME_PANIC, context);
                            result.panic_value = panic;
                            goto done;
                        }
                        const XrValidatedBlock *target =
                            &function->blocks[instruction.successors[0]];
                        scratch[0] = (XrVmRuntimeValue) {
                            .category = XR_CORE_IR_VALUE,
                            .as.value = panic,
                        };
                        for (uint32_t index = 1u; index < target->argument_count; ++index)
                            scratch[index] = values[instruction.operands[index]];
                        incoming_count = target->argument_count;
                        block_id = instruction.successors[0];
                        transferred = true;
                    }
                    break;
                case XR_CORE_OP_CORE_RETURN:
                    result = vm_outcome(XR_VM_OUTCOME_RETURN, context);
                    result.value = instruction.operand_count == 0u
                                       ? void_value()
                                       : values[instruction.operands[0]].as.value;
                    goto done;
                case XR_CORE_OP_CORE_CALL_SEALED_DIRECT:
                case XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT:
                case XR_CORE_OP_CORE_CALL_WITNESS_DIRECT: {
                    uint32_t target_function =
                        instruction.operation_id == XR_CORE_OP_CORE_CALL_SEALED_DIRECT
                            ? instruction.immediate.function_id
                            : XR_PROGRAM_LOCATION_NONE;
                    uint32_t source_argument = 0u;
                    uint32_t target_argument = 0u;
                    if (instruction.operation_id == XR_CORE_OP_CORE_CALL_WITNESS_DIRECT) {
                        const XrVmExistentialValue *carrier =
                            values[instruction.operands[0]].as.value.as.existential;
                        target_function = witness_function_id(context->code->program, carrier,
                                                              instruction.immediate.u32);
                        if (target_function == XR_PROGRAM_LOCATION_NONE)
                            goto done;
                        if (!witness_receiver_argument(
                                carrier,
                                context->code->program->functions[target_function].receiver_mode,
                                &scratch[0]))
                            goto done;
                        source_argument = 1u;
                        target_argument = 1u;
                    } else if (instruction.operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT) {
                        const XrVmCallableValue *carrier =
                            values[instruction.operands[0]].as.value.as.callable;
                        target_function = callable_function_id(context->code->program, carrier);
                        if (target_function == XR_PROGRAM_LOCATION_NONE)
                            goto done;
                        source_argument = 1u;
                        if (carrier->has_capture) {
                            scratch[0] = (XrVmRuntimeValue) {
                                .category = XR_CORE_IR_VALUE,
                                .as.value = carrier->capture,
                            };
                            target_argument = 1u;
                        }
                    }
                    uint32_t call_operand_count = instruction.operand_count;
                    if (instruction.successor_count == 1u)
                        call_operand_count -=
                            function->blocks[instruction.successors[0]].argument_count;
                    for (; source_argument < call_operand_count;
                         ++source_argument, ++target_argument)
                        scratch[target_argument] = values[instruction.operands[source_argument]];
                    const XrValidatedFunction *callee =
                        &context->code->program->functions[target_function];
                    XrVmOutcome nested = execute_function(context, target_function, scratch,
                                                          callee->parameter_count, depth + 1u);
                    if (nested.kind != XR_VM_OUTCOME_RETURN) {
                        if (nested.kind == XR_VM_OUTCOME_TRAP &&
                            nested.trap == XR_VM_TRAP_PROVIDER_CALL_FAILED &&
                            instruction.successor_count == 1u) {
                            const XrValidatedBlock *target =
                                &function->blocks[instruction.successors[0]];
                            for (uint32_t index = 0u; index < target->argument_count; ++index)
                                scratch[index] =
                                    values[instruction.operands[call_operand_count + index]];
                            incoming_count = target->argument_count;
                            block_id = instruction.successors[0];
                            transferred = true;
                            break;
                        }
                        result = nested;
                        goto done;
                    }
                    produced.as.value = nested.value;
                    break;
                }
                case XR_CORE_OP_CORE_CALL_SEALED_INVOKE:
                case XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE:
                case XR_CORE_OP_CORE_CALL_WITNESS_INVOKE: {
                    uint32_t target_function =
                        instruction.operation_id == XR_CORE_OP_CORE_CALL_SEALED_INVOKE
                            ? instruction.immediate.function_id
                            : XR_PROGRAM_LOCATION_NONE;
                    uint32_t source_argument = 0u;
                    uint32_t target_argument = 0u;
                    if (instruction.operation_id == XR_CORE_OP_CORE_CALL_WITNESS_INVOKE) {
                        const XrVmExistentialValue *carrier =
                            values[instruction.operands[0]].as.value.as.existential;
                        target_function = witness_function_id(context->code->program, carrier,
                                                              instruction.immediate.u32);
                        if (target_function == XR_PROGRAM_LOCATION_NONE)
                            goto done;
                        if (!witness_receiver_argument(
                                carrier,
                                context->code->program->functions[target_function].receiver_mode,
                                &scratch[0]))
                            goto done;
                        source_argument = 1u;
                        target_argument = 1u;
                    } else if (instruction.operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE) {
                        const XrVmCallableValue *carrier =
                            values[instruction.operands[0]].as.value.as.callable;
                        target_function = callable_function_id(context->code->program, carrier);
                        if (target_function == XR_PROGRAM_LOCATION_NONE)
                            goto done;
                        source_argument = 1u;
                        if (carrier->has_capture) {
                            scratch[0] = (XrVmRuntimeValue) {
                                .category = XR_CORE_IR_VALUE,
                                .as.value = carrier->capture,
                            };
                            target_argument = 1u;
                        }
                    }
                    const XrValidatedFunction *callee =
                        &context->code->program->functions[target_function];
                    for (; target_argument < callee->parameter_count;
                         ++source_argument, ++target_argument)
                        scratch[target_argument] = values[instruction.operands[source_argument]];
                    XrVmOutcome nested = execute_function(context, target_function, scratch,
                                                          callee->parameter_count, depth + 1u);
                    uint32_t successor = 0u;
                    uint32_t implicit = 0u;
                    uint32_t operand = callee->parameter_count;
                    uint32_t typed_successors =
                        1u + (callee->error_type_id == XR_CORE_TYPE_VOID ? 0u : 1u) +
                        (callee->panic_type_id == XR_CORE_TYPE_VOID ? 0u : 1u);
                    if (instruction.operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE &&
                        !callee->has_receiver)
                        ++operand;
                    if (nested.kind == XR_VM_OUTCOME_RETURN) {
                        if (callee->result_type_id != XR_CORE_TYPE_VOID) {
                            scratch[0] = (XrVmRuntimeValue) {
                                .category = XR_CORE_IR_VALUE,
                                .as.value = nested.value,
                            };
                            implicit = 1u;
                        }
                    } else if (nested.kind == XR_VM_OUTCOME_ERROR) {
                        successor = 1u;
                        operand += function->blocks[instruction.successors[0]].argument_count -
                                   (callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u);
                        scratch[0] = (XrVmRuntimeValue) {
                            .category = XR_CORE_IR_VALUE,
                            .as.value = nested.error_value,
                        };
                        implicit = 1u;
                    } else if (nested.kind == XR_VM_OUTCOME_PANIC) {
                        successor = 1u + (callee->error_type_id == XR_CORE_TYPE_VOID ? 0u : 1u);
                        operand += function->blocks[instruction.successors[0]].argument_count -
                                   (callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u);
                        if (callee->error_type_id != XR_CORE_TYPE_VOID)
                            operand +=
                                function->blocks[instruction.successors[1]].argument_count - 1u;
                        scratch[0] = (XrVmRuntimeValue) {
                            .category = XR_CORE_IR_VALUE,
                            .as.value = nested.panic_value,
                        };
                        implicit = 1u;
                    } else if ((instruction.operation_id == XR_CORE_OP_CORE_CALL_WITNESS_INVOKE ||
                                instruction.operation_id == XR_CORE_OP_CORE_CALL_SEALED_INVOKE ||
                                instruction.operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE) &&
                               nested.kind == XR_VM_OUTCOME_TRAP &&
                               nested.trap == XR_VM_TRAP_PROVIDER_CALL_FAILED &&
                               instruction.successor_count == typed_successors + 1u) {
                        successor = typed_successors;
                        for (uint32_t prior = 0u; prior < typed_successors; ++prior) {
                            uint32_t prior_implicit =
                                prior == 0u
                                    ? (callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u)
                                    : 1u;
                            const XrValidatedBlock *prior_target =
                                &function->blocks[instruction.successors[prior]];
                            operand += prior_target->argument_count - prior_implicit;
                        }
                    } else {
                        result = nested;
                        goto done;
                    }
                    const XrValidatedBlock *target =
                        &function->blocks[instruction.successors[successor]];
                    for (uint32_t index = implicit; index < target->argument_count; ++index)
                        scratch[index] = values[instruction.operands[operand + index - implicit]];
                    incoming_count = target->argument_count;
                    block_id = instruction.successors[successor];
                    transferred = true;
                    break;
                }
                case XR_CORE_OP_CORE_TRAP:
                    result =
                        vm_trap(instruction.immediate.u32 == 7u ? XR_VM_TRAP_PROVIDER_CALL_FAILED
                                                                : XR_VM_TRAP_EXPLICIT,
                                context);
                    goto done;
                case XR_CORE_OP_CORE_ERROR_PUBLISH:
                    result = vm_outcome(XR_VM_OUTCOME_ERROR, context);
                    result.error_value = values[instruction.operands[0]].as.value;
                    goto done;
                case XR_CORE_OP_CORE_PANIC_PUBLISH:
                    result = vm_outcome(XR_VM_OUTCOME_PANIC, context);
                    result.panic_value = values[instruction.operands[0]].as.value;
                    goto done;
                case XR_CORE_OP_CORE_TARGET_POINTER_WIDTH:
                    if (context->code->pointer_width != 32u &&
                        context->code->pointer_width != 64u) {
                        result = vm_trap(XR_VM_TRAP_PROFILE_UNAVAILABLE, context);
                        goto done;
                    }
                    produced.as.value.kind = XR_VM_VALUE_U16;
                    produced.as.value.as.u16 = context->code->pointer_width;
                    break;
                case XR_CORE_OP_CORE_TARGET_OPERATING_SYSTEM:
                    if (context->code->operating_system <= XR_TARGET_OS_NONE ||
                        context->code->operating_system >= XR_TARGET_OS_COUNT) {
                        result = vm_trap(XR_VM_TRAP_PROFILE_UNAVAILABLE, context);
                        goto done;
                    }
                    produced.as.value.kind = XR_VM_VALUE_TARGET_OS;
                    produced.as.value.as.target_enum = context->code->operating_system;
                    break;
                case XR_CORE_OP_CORE_TARGET_ARCHITECTURE:
                    if (context->code->architecture <= XR_TARGET_ARCH_NONE ||
                        context->code->architecture >= XR_TARGET_ARCH_COUNT) {
                        result = vm_trap(XR_VM_TRAP_PROFILE_UNAVAILABLE, context);
                        goto done;
                    }
                    produced.as.value.kind = XR_VM_VALUE_TARGET_ARCH;
                    produced.as.value.as.target_enum = context->code->architecture;
                    break;
                case XR_CORE_OP_CORE_TARGET_NATIVE_ABI:
                    if (context->code->native_abi <= XR_TARGET_ABI_NONE ||
                        context->code->native_abi >= XR_TARGET_ABI_COUNT) {
                        result = vm_trap(XR_VM_TRAP_PROFILE_UNAVAILABLE, context);
                        goto done;
                    }
                    produced.as.value.kind = XR_VM_VALUE_TARGET_ABI;
                    produced.as.value.as.target_enum = context->code->native_abi;
                    break;
                case XR_CORE_OP_CORE_TARGET_ENDIANNESS:
                    if (context->code->endianness != XR_TARGET_ENDIAN_LITTLE &&
                        context->code->endianness != XR_TARGET_ENDIAN_BIG) {
                        result = vm_trap(XR_VM_TRAP_PROFILE_UNAVAILABLE, context);
                        goto done;
                    }
                    produced.as.value.kind = XR_VM_VALUE_TARGET_ENDIAN;
                    produced.as.value.as.target_enum = context->code->endianness;
                    break;
                case XR_CORE_OP_CORE_PROVIDER_CALL: {
                    bool has_trap_edge = instruction.successor_count != 0u;
                    const XrValidatedBlock *trap_target =
                        has_trap_edge ? &function->blocks[instruction.successors[0]] : NULL;
                    uint32_t provider_operand_count =
                        instruction.operand_count -
                        (trap_target ? trap_target->argument_count : 0u);
                    XrVmOutcome provider = vm_provider_call(context, function, &instruction, values,
                                                            provider_operand_count);
                    if (provider.kind != XR_VM_OUTCOME_RETURN) {
                        if (provider.kind != XR_VM_OUTCOME_TRAP ||
                            provider.trap != XR_VM_TRAP_PROVIDER_CALL_FAILED || !has_trap_edge) {
                            result = provider;
                            goto done;
                        }
                        for (uint32_t index = 0u; index < trap_target->argument_count; ++index)
                            scratch[index] =
                                values[instruction.operands[provider_operand_count + index]];
                        incoming_count = trap_target->argument_count;
                        block_id = instruction.successors[0];
                        transferred = true;
                    } else {
                        produced.as.value = provider.value;
                    }
                    break;
                }
                case XR_CORE_OP_CORE_OUTPUT_GROUP_I64: {
                    uint8_t bytes[22];
                    size_t size =
                        format_i64_line(values[instruction.operands[0]].as.value.as.i64, bytes);
                    XrExecutionProviderCallResult call = xr_execution_lease_provider_output_write(
                        context->lease, instruction.immediate.provider_operation.requirement_index,
                        instruction.immediate.provider_operation.operation_index, bytes, size);
                    if (call != XR_EXECUTION_PROVIDER_CALL_OK) {
                        result = vm_trap(XR_VM_TRAP_PROVIDER_CALL_FAILED, context);
                        goto done;
                    }
                    break;
                }
                case XR_CORE_OP_CORE_CALLABLE_PACK: {
                    XrVmCallableValue *carrier = allocate_callable(context);
                    if (!carrier) {
                        result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    carrier->callable_type_id = instruction.result_type_id;
                    carrier->function_id = instruction.immediate.function_id;
                    carrier->has_capture = instruction.operand_count != 0u;
                    if (carrier->has_capture) {
                        uint32_t capture_value = instruction.operands[0];
                        carrier->capture_type_id = function->value_types[capture_value];
                        carrier->capture = values[capture_value].as.value;
                    }
                    produced.as.value.kind = XR_VM_VALUE_CALLABLE;
                    produced.as.value.as.callable = carrier;
                    break;
                }
                case XR_CORE_OP_CORE_OWNER_COPY:
                    if (!clone_vm_value(context, values[instruction.operands[0]].as.value,
                                        instruction.result_type_id, &produced.as.value)) {
                        result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    break;
                case XR_CORE_OP_CORE_OWNER_MOVE:
                    produced.as.value = values[instruction.operands[0]].as.value;
                    break;
                case XR_CORE_OP_CORE_OWNER_DROP:
                    drop_vm_value(context, &values[instruction.operands[0]].as.value,
                                  XR_VM_EVENT_ORIGIN_PROGRAM_OPERATION);
                    break;
                case XR_CORE_OP_CORE_CLASS_CONSTRUCT: {
                    XrVmClassValue *instance = allocate_class(
                        context, instruction.result_type_id, instruction.operand_count);
                    if (!instance) {
                        result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    for (uint32_t field = 0u; field < instruction.operand_count; ++field)
                        instance->fields[field] = values[instruction.operands[field]].as.value;
                    produced.as.value.kind = XR_VM_VALUE_CLASS_REFERENCE;
                    produced.as.value.as.class_reference = instance;
                    emit_lifecycle(context, XR_VM_EVENT_CLASS_CONSTRUCT,
                                   XR_VM_EVENT_ORIGIN_PROGRAM_OPERATION, instance, UINT64_MAX,
                                   UINT32_MAX);
                    break;
                }
                case XR_CORE_OP_CORE_CLASS_SHARE: {
                    XrVmValue source = values[instruction.operands[0]].as.value;
                    XrVmClassValue *instance =
                        source.kind == XR_VM_VALUE_CLASS_REFERENCE
                            ? (XrVmClassValue *) (void *) source.as.class_reference
                            : NULL;
                    if (!class_value_is_live(instance, instruction.result_type_id) ||
                        instance->owner_count == UINT32_MAX) {
                        result = vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, context);
                        goto done;
                    }
                    ++instance->owner_count;
                    produced.as.value.kind = XR_VM_VALUE_CLASS_REFERENCE;
                    produced.as.value.as.class_reference = instance;
                    emit_lifecycle(context, XR_VM_EVENT_CLASS_SHARE,
                                   XR_VM_EVENT_ORIGIN_PROGRAM_OPERATION, instance,
                                   instance->identity, UINT32_MAX);
                    break;
                }
                case XR_CORE_OP_CORE_CLASS_FIELD_LOAD: {
                    XrVmValue source = values[instruction.operands[0]].as.value;
                    XrVmClassValue *instance =
                        source.kind == XR_VM_VALUE_CLASS_REFERENCE
                            ? (XrVmClassValue *) (void *) source.as.class_reference
                            : NULL;
                    if (!class_field_load_value(context, instance,
                                                instruction.immediate.field_ordinal,
                                                instruction.result_type_id, &produced.as.value)) {
                        result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    emit_lifecycle(context, XR_VM_EVENT_CLASS_FIELD_LOAD,
                                   XR_VM_EVENT_ORIGIN_PROGRAM_OPERATION, instance, UINT64_MAX,
                                   instruction.immediate.field_ordinal);
                    break;
                }
                case XR_CORE_OP_CORE_CLASS_FIELD_PLACE: {
                    XrVmValue source = values[instruction.operands[0]].as.value;
                    XrVmClassValue *instance =
                        source.kind == XR_VM_VALUE_CLASS_REFERENCE
                            ? (XrVmClassValue *) (void *) source.as.class_reference
                            : NULL;
                    const XrValidatedType *type =
                        instance ? xr_validated_program_type(context->code->program,
                                                             instance->type_id)
                                 : NULL;
                    uint32_t field = instruction.immediate.field_ordinal;
                    if (!type || type->kind != XR_CORE_IR_TYPE_CLASS_REFERENCE ||
                        !class_value_is_live(instance, instance->type_id) ||
                        field >= type->field_count || field >= instance->field_count ||
                        type->field_types[field] != instruction.result_type_id) {
                        result = vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, context);
                        goto done;
                    }
                    places[instruction.result_id].alias = &instance->fields[field];
                    places[instruction.result_id].initialized = true;
                    produced.category = XR_CORE_IR_PLACE;
                    produced.as.place = &places[instruction.result_id];
                    emit_lifecycle(context, XR_VM_EVENT_CLASS_FIELD_PLACE,
                                   XR_VM_EVENT_ORIGIN_PROGRAM_OPERATION, instance, UINT64_MAX,
                                   field);
                    break;
                }
                case XR_CORE_OP_CORE_PLACE_LOCAL:
                    places[instruction.result_id].alias = &values[instruction.operands[0]].as.value;
                    places[instruction.result_id].initialized = true;
                    produced.category = XR_CORE_IR_PLACE;
                    produced.as.place = &places[instruction.result_id];
                    break;
                case XR_CORE_OP_CORE_PLACE_LOAD:
                    produced.as.value = *vm_place_value(values[instruction.operands[0]].as.place);
                    break;
                case XR_CORE_OP_CORE_PLACE_STORE:
                    *vm_place_value(values[instruction.operands[0]].as.place) =
                        values[instruction.operands[1]].as.value;
                    break;
                case XR_CORE_OP_CORE_PLACE_PROJECT: {
                    XrVmValue *source = vm_place_value(values[instruction.operands[0]].as.place);
                    XrVmAggregateValue *aggregate =
                        (XrVmAggregateValue *) (void *) source->as.aggregate;
                    places[instruction.result_id].alias =
                        &aggregate->fields[instruction.immediate.field_ordinal];
                    places[instruction.result_id].initialized = true;
                    produced.category = XR_CORE_IR_PLACE;
                    produced.as.place = &places[instruction.result_id];
                    break;
                }
                case XR_CORE_OP_CORE_PLACE_TAKE:
                    produced.as.value = *vm_place_value(values[instruction.operands[0]].as.place);
                    values[instruction.operands[0]].as.place->initialized = false;
                    break;
                case XR_CORE_OP_CORE_PLACE_EXCHANGE: {
                    XrVmValue *place =
                        vm_place_value(values[instruction.operands[0]].as.place);
                    if (!place) {
                        result = vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, context);
                        goto done;
                    }
                    produced.as.value = *place;
                    XrVmValue replacement = values[instruction.operands[1]].as.value;
                    *place = replacement;
                    emit_place_exchange(context, instruction.result_type_id, produced.as.value,
                                        replacement);
                    break;
                }
                case XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT: {
                    XrVmAggregateValue *aggregate = allocate_aggregate(
                        context, instruction.result_type_id, UINT32_MAX, instruction.operand_count);
                    if (!aggregate) {
                        result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    for (uint32_t field = 0; field < instruction.operand_count; ++field)
                        aggregate->fields[field] = values[instruction.operands[field]].as.value;
                    produced.as.value.kind = XR_VM_VALUE_AGGREGATE;
                    produced.as.value.as.aggregate = aggregate;
                    break;
                }
                case XR_CORE_OP_CORE_AGGREGATE_PROJECT: {
                    const XrVmAggregateValue *aggregate =
                        values[instruction.operands[0]].as.value.as.aggregate;
                    produced.as.value = aggregate->fields[instruction.immediate.field_ordinal];
                    break;
                }
                case XR_CORE_OP_CORE_AGGREGATE_UPDATE: {
                    const XrVmAggregateValue *source =
                        values[instruction.operands[0]].as.value.as.aggregate;
                    XrVmAggregateValue *aggregate = allocate_aggregate(
                        context, instruction.result_type_id, UINT32_MAX, source->field_count);
                    if (!aggregate) {
                        result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    memcpy(aggregate->fields, source->fields,
                           (size_t) source->field_count * sizeof(XrVmValue));
                    aggregate->fields[instruction.immediate.field_ordinal] =
                        values[instruction.operands[1]].as.value;
                    produced.as.value.kind = XR_VM_VALUE_AGGREGATE;
                    produced.as.value.as.aggregate = aggregate;
                    break;
                }
                case XR_CORE_OP_CORE_VARIANT_CONSTRUCT: {
                    XrVmAggregateValue *aggregate = allocate_aggregate(
                        context, instruction.result_type_id, instruction.immediate.variant_ordinal,
                        instruction.operand_count);
                    if (!aggregate) {
                        result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    for (uint32_t field = 0; field < instruction.operand_count; ++field)
                        aggregate->fields[field] = values[instruction.operands[field]].as.value;
                    produced.as.value.kind = XR_VM_VALUE_AGGREGATE;
                    produced.as.value.as.aggregate = aggregate;
                    break;
                }
                case XR_CORE_OP_CORE_VARIANT_TEST: {
                    const XrVmAggregateValue *aggregate =
                        values[instruction.operands[0]].as.value.as.aggregate;
                    produced.as.value.kind = XR_VM_VALUE_BOOL;
                    produced.as.value.as.boolean =
                        aggregate->variant_ordinal == instruction.immediate.variant_ordinal;
                    break;
                }
                case XR_CORE_OP_CORE_VARIANT_PROJECT: {
                    const XrVmAggregateValue *aggregate =
                        values[instruction.operands[0]].as.value.as.aggregate;
                    if (aggregate->variant_ordinal !=
                        instruction.immediate.variant_field.variant_ordinal) {
                        result = vm_trap(XR_VM_TRAP_VARIANT_TAG_MISMATCH, context);
                        goto done;
                    }
                    produced.as.value =
                        aggregate->fields[instruction.immediate.variant_field.field_ordinal];
                    break;
                }
                case XR_CORE_OP_CORE_EXISTENTIAL_PACK: {
                    const XrValidatedType *existential = xr_validated_program_type(
                        context->code->program, instruction.result_type_id);
                    uint16_t concrete_type = function->value_types[instruction.operands[0]];
                    XrVmExistentialValue *carrier = allocate_existential(context);
                    uint32_t conformance =
                        existential ? conformance_id(context->code->program, concrete_type,
                                                     existential->interface_id)
                                    : XR_PROGRAM_LOCATION_NONE;
                    if (!carrier || conformance == XR_PROGRAM_LOCATION_NONE) {
                        result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    carrier->existential_type_id = instruction.result_type_id;
                    carrier->concrete_type_id = concrete_type;
                    carrier->conformance_id = conformance;
                    if (existential->interface_use_kind ==
                        XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE) {
                        carrier->owned_storage.value = values[instruction.operands[0]].as.value;
                        carrier->owned_storage.initialized = true;
                        carrier->payload.category = XR_CORE_IR_PLACE;
                        carrier->payload.as.place = &carrier->owned_storage;
                    } else {
                        carrier->payload = values[instruction.operands[0]];
                    }
                    produced.as.value.kind = XR_VM_VALUE_EXISTENTIAL;
                    produced.as.value.as.existential = carrier;
                    break;
                }
                case XR_CORE_OP_CORE_EXISTENTIAL_REBORROW_READ: {
                    const XrVmExistentialValue *source =
                        values[instruction.operands[0]].as.value.as.existential;
                    XrVmExistentialValue *carrier = allocate_existential(context);
                    if (!source || !carrier) {
                        result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    carrier->existential_type_id = instruction.result_type_id;
                    carrier->concrete_type_id = source->concrete_type_id;
                    carrier->conformance_id = source->conformance_id;
                    carrier->payload.category = XR_CORE_IR_VALUE;
                    carrier->payload.as.value =
                        source->payload.category == XR_CORE_IR_PLACE
                            ? *vm_place_value_const(source->payload.as.place)
                            : source->payload.as.value;
                    produced.as.value.kind = XR_VM_VALUE_EXISTENTIAL;
                    produced.as.value.as.existential = carrier;
                    break;
                }
                case XR_CORE_OP_CORE_EXISTENTIAL_TEST: {
                    const XrVmExistentialValue *carrier =
                        values[instruction.operands[0]].as.value.as.existential;
                    produced.as.value.kind = XR_VM_VALUE_BOOL;
                    produced.as.value.as.boolean =
                        carrier->concrete_type_id == instruction.immediate.type_id;
                    break;
                }
                case XR_CORE_OP_CORE_EXISTENTIAL_PROJECT: {
                    const XrVmExistentialValue *carrier =
                        values[instruction.operands[0]].as.value.as.existential;
                    const XrValidatedType *existential = xr_validated_program_type(
                        context->code->program, carrier->existential_type_id);
                    if (existential && existential->interface_use_kind ==
                                           XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE) {
                        produced.category = XR_CORE_IR_VALUE;
                        produced.as.value = *vm_place_value_const(&carrier->owned_storage);
                    } else {
                        produced = carrier->payload;
                    }
                    break;
                }
                default:
                    goto done;
            }
            if (has_result && !transferred) {
                values[instruction.result_id] = produced;
                initialized[instruction.result_id] = true;
            }
            if (transferred)
                break;
        }
        if (!transferred)
            break;
    }

done:
    result.steps = context->steps;
    xr_free(scratch);
    xr_free(initialized);
    xr_free(places);
    xr_free(values);
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
    xr_sha256_update(&context, code->cache_key.execution_id.bytes,
                     sizeof(code->cache_key.execution_id.bytes));
    hash_u64(&context, code->cache_key.generation);
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

XrVmCodeStatus xr_vm_code_build(XrInstance *instance, const XrVmCodeOptions *options,
                                XrVmCode **code_out, XrVmCodeDiagnostic *diagnostic_out) {
    if (code_out)
        *code_out = NULL;
    if (diagnostic_out)
        memset(diagnostic_out, 0, sizeof(*diagnostic_out));
    XrVmCodeOptions selected = options ? *options : xr_vm_code_default_options();
    if (!instance || !code_out || selected.schema_version != XR_VM_CODE_OPTIONS_SCHEMA_VERSION ||
        selected.reserved16 != 0u || selected.max_steps == 0u || selected.max_value_cells == 0u ||
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
    XrExecutionLease lease = {0};
    if (!xr_execution_instance_acquire(instance, &lease)) {
        if (diagnostic_out)
            diagnostic_out->status = XR_VM_CODE_INSTANCE_UNAVAILABLE;
        return XR_VM_CODE_INSTANCE_UNAVAILABLE;
    }
    XrValidatedProgram *program = xr_execution_lease_retain_program(&lease);
    XrTargetProfile *profile = xr_execution_lease_retain_profile(&lease);
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(profile);
    if (!program || !machine) {
        xr_target_profile_free(profile);
        xr_validated_program_free(program);
        (void) xr_execution_lease_release(&lease);
        if (diagnostic_out)
            diagnostic_out->status = XR_VM_CODE_INSTANCE_UNAVAILABLE;
        return XR_VM_CODE_INSTANCE_UNAVAILABLE;
    }
    if (!vm_program_operations_active(program, diagnostic_out)) {
        xr_target_profile_free(profile);
        xr_validated_program_free(program);
        (void) xr_execution_lease_release(&lease);
        return XR_VM_CODE_UNSUPPORTED_OPERATION;
    }
    XrVmCode *code = xr_calloc(1u, sizeof(XrVmCode));
    if (!code) {
        xr_target_profile_free(profile);
        xr_validated_program_free(program);
        (void) xr_execution_lease_release(&lease);
        if (diagnostic_out)
            diagnostic_out->status = XR_VM_CODE_OUT_OF_MEMORY;
        return XR_VM_CODE_OUT_OF_MEMORY;
    }
    atomic_init(&code->references, 1u);
    code->program = program;
    code->cache_key = xr_execution_instance_cache_key(instance);
    code->options = selected;
    code->pointer_width = (uint16_t) (machine->data_layout.pointer.size * UINT16_C(8));
    code->operating_system = machine->operating_system;
    code->architecture = machine->architecture;
    code->native_abi = machine->native_abi;
    code->endianness = (uint16_t) machine->data_layout.endian;
    if (selected.decode_policy == XR_VM_DECODE_FIXED_ROWS && !fixed_view_build(code)) {
        xr_target_profile_free(profile);
        xr_vm_code_free(code);
        (void) xr_execution_lease_release(&lease);
        if (diagnostic_out)
            diagnostic_out->status = XR_VM_CODE_OUT_OF_MEMORY;
        return XR_VM_CODE_OUT_OF_MEMORY;
    }
    compute_private_digest(code);
    xr_target_profile_free(profile);
    (void) xr_execution_lease_release(&lease);
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
    XrExecutionCacheKey key = xr_execution_instance_cache_key(instance);
    return key.generation == code->cache_key.generation &&
           xr_fingerprint_equal(key.execution_id, code->cache_key.execution_id);
}

XrExecutionCacheKey xr_vm_code_cache_key(const XrVmCode *code) {
    XrExecutionCacheKey key = {0};
    return code ? code->cache_key : key;
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

static bool vm_coroutine_operation_supported(uint16_t operation_id) {
    return operation_id == XR_CORE_OP_CORE_BLOCK_ARGUMENT ||
           operation_id == XR_CORE_OP_CORE_CONSTANT_I64 ||
           operation_id == XR_CORE_OP_CORE_CONSTANT_BOOL ||
           operation_id == XR_CORE_OP_CORE_ADD_I64 || operation_id == XR_CORE_OP_CORE_SUB_I64 ||
           operation_id == XR_CORE_OP_CORE_MUL_I64 || operation_id == XR_CORE_OP_CORE_COMPARE_I64 ||
           operation_id == XR_CORE_OP_CORE_BRANCH ||
           operation_id == XR_CORE_OP_CORE_CONDITIONAL_BRANCH ||
           operation_id == XR_CORE_OP_CORE_PROVIDER_CALL ||
           operation_id == XR_CORE_OP_CORE_CALL_SEALED_DIRECT ||
           operation_id == XR_CORE_OP_CORE_CALL_WITNESS_DIRECT ||
           operation_id == XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT ||
           operation_id == XR_CORE_OP_CORE_AGGREGATE_PROJECT ||
           operation_id == XR_CORE_OP_CORE_EXISTENTIAL_PACK ||
           operation_id == XR_CORE_OP_CORE_EXISTENTIAL_REBORROW_READ ||
           operation_id == XR_CORE_OP_CORE_CALLABLE_PACK ||
           operation_id == XR_CORE_OP_CORE_OWNER_MOVE ||
           operation_id == XR_CORE_OP_CORE_CLASS_CONSTRUCT ||
           operation_id == XR_CORE_OP_CORE_CLASS_SHARE ||
           operation_id == XR_CORE_OP_CORE_CLASS_FIELD_LOAD ||
           operation_id == XR_CORE_OP_CORE_CLASS_FIELD_PLACE ||
           operation_id == XR_CORE_OP_CORE_PLACE_LOCAL ||
           operation_id == XR_CORE_OP_CORE_PLACE_LOAD ||
           operation_id == XR_CORE_OP_CORE_PLACE_STORE ||
           operation_id == XR_CORE_OP_CORE_PLACE_PROJECT ||
           operation_id == XR_CORE_OP_CORE_PLACE_TAKE ||
           operation_id == XR_CORE_OP_CORE_PLACE_EXCHANGE ||
           operation_id == XR_CORE_OP_CORE_COROUTINE_YIELD ||
           operation_id == XR_CORE_OP_CORE_COROUTINE_SUSPEND ||
           operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED ||
           operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT ||
           operation_id == XR_CORE_OP_CORE_OWNER_DROP ||
           operation_id == XR_CORE_OP_CORE_CANCEL_PUBLISH || operation_id == XR_CORE_OP_CORE_TRAP ||
           operation_id == XR_CORE_OP_CORE_RETURN;
}

static void vm_execution_release_lease(XrVmExecution *execution) {
    if (execution && execution->owns_lease && xr_execution_lease_is_valid(&execution->lease))
        (void) xr_execution_lease_release(&execution->lease);
}

static bool vm_execution_allocate_values(XrVmExecution *execution,
                                         const XrValidatedFunction *function) {
    size_t count = function->value_count ? function->value_count : 1u;
    size_t edge_count = 1u;
    for (uint32_t block = 0u; block < function->block_count; ++block)
        if (function->blocks[block].argument_count > edge_count)
            edge_count = function->blocks[block].argument_count;
    if (edge_count > count || count > SIZE_MAX / sizeof(*execution->values) ||
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
    child->context.next_class_identity = parent->context.next_class_identity;
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
    xr_sha256_update(&child->context.trace, parent->code->cache_key.execution_id.bytes,
                     sizeof(parent->code->cache_key.execution_id.bytes));
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
        XrCoreIrValueCategory expected = function->parameter_modes[target_argument] == XR_PARAM_REF
                                             ? XR_CORE_IR_PLACE
                                             : XR_CORE_IR_VALUE;
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

static void *vm_reserve_child_carriers(void *storage, uint32_t count, uint32_t added,
                                       uint32_t *capacity, size_t item_size) {
    if (added > UINT32_MAX - count || (size_t) (count + added) > SIZE_MAX / item_size)
        return NULL;
    uint32_t required = count + added;
    if (required <= *capacity)
        return storage;
    void *grown = xr_realloc(storage, (size_t) required * item_size);
    if (grown)
        *capacity = required;
    return grown;
}

static bool vm_adopt_child_storage(XrVmExecution *execution) {
    XrVmContext *parent = &execution->context;
    XrVmContext *child = &execution->child->context;
    if (parent->aggregate_cell_count > parent->code->options.max_value_cells ||
        child->aggregate_cell_count >
            parent->code->options.max_value_cells - parent->aggregate_cell_count)
        return false;
    if (child->aggregate_count != 0u) {
        XrVmAggregateValue **grown = vm_reserve_child_carriers(
            parent->aggregates, parent->aggregate_count, child->aggregate_count,
            &parent->aggregate_capacity, sizeof(*parent->aggregates));
        if (!grown)
            return false;
        parent->aggregates = grown;
    }
    if (child->class_count != 0u) {
        XrVmClassValue **grown = vm_reserve_child_carriers(
            parent->classes, parent->class_count, child->class_count,
            &parent->class_capacity, sizeof(*parent->classes));
        if (!grown)
            return false;
        parent->classes = grown;
    }
    if (child->existential_count != 0u) {
        XrVmExistentialValue **grown = vm_reserve_child_carriers(
            parent->existentials, parent->existential_count, child->existential_count,
            &parent->existential_capacity, sizeof(*parent->existentials));
        if (!grown)
            return false;
        parent->existentials = grown;
    }
    if (child->callable_count != 0u) {
        XrVmCallableValue **grown = vm_reserve_child_carriers(
            parent->callables, parent->callable_count, child->callable_count,
            &parent->callable_capacity, sizeof(*parent->callables));
        if (!grown)
            return false;
        parent->callables = grown;
    }
    // REF writes and uncaught outcomes can retain child-created carriers. Moving
    // their allocation ownership preserves aliases when the child frame ends.
    if (child->aggregate_count != 0u)
        memcpy(parent->aggregates + parent->aggregate_count, child->aggregates,
               (size_t) child->aggregate_count * sizeof(*child->aggregates));
    if (child->class_count != 0u)
        memcpy(parent->classes + parent->class_count, child->classes,
               (size_t) child->class_count * sizeof(*child->classes));
    if (child->existential_count != 0u)
        memcpy(parent->existentials + parent->existential_count, child->existentials,
               (size_t) child->existential_count * sizeof(*child->existentials));
    if (child->callable_count != 0u)
        memcpy(parent->callables + parent->callable_count, child->callables,
               (size_t) child->callable_count * sizeof(*child->callables));
    parent->aggregate_count += child->aggregate_count;
    parent->class_count += child->class_count;
    parent->existential_count += child->existential_count;
    parent->callable_count += child->callable_count;
    parent->aggregate_cell_count += child->aggregate_cell_count;
    if (child->next_class_identity > parent->next_class_identity)
        parent->next_class_identity = child->next_class_identity;
    child->aggregate_count = 0u;
    child->class_count = 0u;
    child->existential_count = 0u;
    child->callable_count = 0u;
    child->aggregate_cell_count = 0u;
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
    if (argument_count != function->parameter_count || function->coroutine_safepoint_count == 0u ||
        function->coroutine_state_count != function->coroutine_safepoint_count + 1u ||
        function->value_count > code->options.max_value_cells)
        return false;
    for (uint32_t block = 0; block < function->block_count; ++block)
        for (uint32_t instruction = 0; instruction < function->blocks[block].instruction_count;
             ++instruction)
            if (!vm_coroutine_operation_supported(
                    function->blocks[block].instructions[instruction].operation_id))
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
    xr_sha256_update(&execution->context.trace, code->cache_key.execution_id.bytes,
                     sizeof(code->cache_key.execution_id.bytes));
    hash_u32(&execution->context.trace, function_id);
    for (uint32_t argument = 0; argument < argument_count; ++argument) {
        uint32_t value_id = function->blocks[function->entry_block].argument_ids[argument];
        if (function->parameter_modes[argument] == XR_PARAM_REF ||
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

XrVmOutcome xr_vm_execution_step(XrVmExecution *execution) {
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
    const XrValidatedFunction *function =
        &execution->context.code->program->functions[execution->function_id];
    for (;;) {
        const XrValidatedBlock *block = &function->blocks[execution->block_id];
        if (execution->instruction_id >= block->instruction_count) {
            execution->finished = true;
            vm_execution_release_lease(execution);
            return vm_execution_outcome(execution, XR_VM_OUTCOME_INVALID_INVOCATION);
        }
        uint32_t instruction_id = execution->instruction_id++;
        XrVmInstructionView instruction = instruction_view(
            execution->context.code, execution->function_id, execution->block_id, instruction_id);
        if (execution->context.steps == execution->context.code->options.max_steps) {
            execution->finished = true;
            vm_execution_release_lease(execution);
            return vm_execution_outcome(execution, XR_VM_OUTCOME_RESOURCE_LIMIT);
        }
        ++execution->context.steps;
        trace_instruction(&execution->context, execution->function_id, execution->block_id,
                          instruction_id, instruction.operation_id);
        for (uint32_t operand = 0; operand < instruction.operand_count; ++operand) {
            if (!execution->initialized[instruction.operands[operand]]) {
                execution->finished = true;
                vm_execution_release_lease(execution);
                return vm_execution_outcome(execution, XR_VM_OUTCOME_INVALID_INVOCATION);
            }
        }
        switch (instruction.operation_id) {
            case XR_CORE_OP_CORE_BLOCK_ARGUMENT:
                break;
            case XR_CORE_OP_CORE_CONSTANT_BOOL: {
                const XrValidatedConstant *constant =
                    &execution->context.code->program->constants[instruction.immediate.constant_id];
                execution->values[instruction.result_id] = (XrVmRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value = {.kind = XR_VM_VALUE_BOOL, .as.boolean = constant->value.boolean},
                };
                execution->initialized[instruction.result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_CONSTANT_I64: {
                const XrValidatedConstant *constant =
                    &execution->context.code->program->constants[instruction.immediate.constant_id];
                execution->values[instruction.result_id] = (XrVmRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value = {.kind = XR_VM_VALUE_I64, .as.i64 = constant->value.i64},
                };
                execution->initialized[instruction.result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_ADD_I64:
            case XR_CORE_OP_CORE_SUB_I64:
            case XR_CORE_OP_CORE_MUL_I64: {
                int64_t left = execution->values[instruction.operands[0]].as.value.as.i64;
                int64_t right = execution->values[instruction.operands[1]].as.value.as.i64;
                int64_t value = 0;
                if (instruction.immediate.u32 == 0u) {
                    bool valid = instruction.operation_id == XR_CORE_OP_CORE_ADD_I64
                                     ? checked_add(left, right, &value)
                                 : instruction.operation_id == XR_CORE_OP_CORE_SUB_I64
                                     ? checked_sub(left, right, &value)
                                     : checked_mul(left, right, &value);
                    if (!valid) {
                        execution->finished = true;
                        vm_execution_release_lease(execution);
                        return vm_trap(XR_VM_TRAP_INTEGER_OVERFLOW, &execution->context);
                    }
                } else {
                    uint64_t bits = instruction.operation_id == XR_CORE_OP_CORE_ADD_I64
                                        ? (uint64_t) left + (uint64_t) right
                                    : instruction.operation_id == XR_CORE_OP_CORE_SUB_I64
                                        ? (uint64_t) left - (uint64_t) right
                                        : (uint64_t) left * (uint64_t) right;
                    value = i64_from_bits(bits);
                }
                execution->values[instruction.result_id] = (XrVmRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value = {.kind = XR_VM_VALUE_I64, .as.i64 = value},
                };
                execution->initialized[instruction.result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_COMPARE_I64: {
                int64_t left = execution->values[instruction.operands[0]].as.value.as.i64;
                int64_t right = execution->values[instruction.operands[1]].as.value.as.i64;
                bool comparison = false;
                switch (instruction.immediate.u32) {
                    case 0u:
                        comparison = left == right;
                        break;
                    case 1u:
                        comparison = left != right;
                        break;
                    case 2u:
                        comparison = left < right;
                        break;
                    case 3u:
                        comparison = left <= right;
                        break;
                    case 4u:
                        comparison = left > right;
                        break;
                    case 5u:
                        comparison = left >= right;
                        break;
                    default:
                        execution->finished = true;
                        vm_execution_release_lease(execution);
                        return vm_execution_outcome(execution, XR_VM_OUTCOME_INVALID_INVOCATION);
                }
                execution->values[instruction.result_id] = (XrVmRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value = {.kind = XR_VM_VALUE_BOOL, .as.boolean = comparison},
                };
                execution->initialized[instruction.result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT: {
                XrVmAggregateValue *aggregate =
                    allocate_aggregate(&execution->context, instruction.result_type_id, UINT32_MAX,
                                       instruction.operand_count);
                if (!aggregate) {
                    execution->finished = true;
                    vm_execution_release_lease(execution);
                    return vm_execution_outcome(execution, XR_VM_OUTCOME_RESOURCE_LIMIT);
                }
                for (uint32_t field = 0u; field < instruction.operand_count; ++field)
                    aggregate->fields[field] =
                        execution->values[instruction.operands[field]].as.value;
                execution->values[instruction.result_id] = (XrVmRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value =
                        {
                            .kind = XR_VM_VALUE_AGGREGATE,
                            .as.aggregate = aggregate,
                        },
                };
                execution->initialized[instruction.result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_AGGREGATE_PROJECT: {
                const XrVmAggregateValue *aggregate =
                    execution->values[instruction.operands[0]].as.value.as.aggregate;
                execution->values[instruction.result_id] = (XrVmRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value = aggregate->fields[instruction.immediate.field_ordinal],
                };
                execution->initialized[instruction.result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_CALLABLE_PACK: {
                XrVmCallableValue *carrier = allocate_callable(&execution->context);
                if (!carrier) {
                    execution->finished = true;
                    vm_execution_release_lease(execution);
                    return vm_execution_outcome(execution, XR_VM_OUTCOME_RESOURCE_LIMIT);
                }
                carrier->callable_type_id = instruction.result_type_id;
                carrier->function_id = instruction.immediate.function_id;
                carrier->has_capture = instruction.operand_count != 0u;
                if (carrier->has_capture) {
                    uint32_t capture_value = instruction.operands[0];
                    carrier->capture_type_id = function->value_types[capture_value];
                    carrier->capture = execution->values[capture_value].as.value;
                }
                execution->values[instruction.result_id] = (XrVmRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value =
                        {
                            .kind = XR_VM_VALUE_CALLABLE,
                            .as.callable = carrier,
                        },
                };
                execution->initialized[instruction.result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_EXISTENTIAL_PACK: {
                const XrValidatedType *existential = xr_validated_program_type(
                    execution->context.code->program, instruction.result_type_id);
                uint16_t concrete_type = function->value_types[instruction.operands[0]];
                XrVmExistentialValue *carrier = allocate_existential(&execution->context);
                uint32_t conformance =
                    existential ? conformance_id(execution->context.code->program, concrete_type,
                                                 existential->interface_id)
                                : XR_PROGRAM_LOCATION_NONE;
                if (!carrier || conformance == XR_PROGRAM_LOCATION_NONE) {
                    execution->finished = true;
                    vm_execution_release_lease(execution);
                    return vm_execution_outcome(execution, XR_VM_OUTCOME_RESOURCE_LIMIT);
                }
                carrier->existential_type_id = instruction.result_type_id;
                carrier->concrete_type_id = concrete_type;
                carrier->conformance_id = conformance;
                if (existential->interface_use_kind ==
                    XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE) {
                    carrier->owned_storage.value =
                        execution->values[instruction.operands[0]].as.value;
                    carrier->owned_storage.initialized = true;
                    carrier->payload.category = XR_CORE_IR_PLACE;
                    carrier->payload.as.place = &carrier->owned_storage;
                } else {
                    carrier->payload = execution->values[instruction.operands[0]];
                }
                execution->values[instruction.result_id] = (XrVmRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value =
                        {
                            .kind = XR_VM_VALUE_EXISTENTIAL,
                            .as.existential = carrier,
                        },
                };
                execution->initialized[instruction.result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_EXISTENTIAL_REBORROW_READ: {
                const XrVmExistentialValue *source =
                    execution->values[instruction.operands[0]].as.value.as.existential;
                XrVmExistentialValue *carrier = allocate_existential(&execution->context);
                if (!source || !carrier) {
                    execution->finished = true;
                    vm_execution_release_lease(execution);
                    return vm_execution_outcome(execution, XR_VM_OUTCOME_RESOURCE_LIMIT);
                }
                carrier->existential_type_id = instruction.result_type_id;
                carrier->concrete_type_id = source->concrete_type_id;
                carrier->conformance_id = source->conformance_id;
                carrier->payload.category = XR_CORE_IR_VALUE;
                carrier->payload.as.value = source->payload.category == XR_CORE_IR_PLACE
                                                ? *vm_place_value_const(source->payload.as.place)
                                                : source->payload.as.value;
                execution->values[instruction.result_id] = (XrVmRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value =
                        {
                            .kind = XR_VM_VALUE_EXISTENTIAL,
                            .as.existential = carrier,
                        },
                };
                execution->initialized[instruction.result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_OWNER_MOVE:
                execution->values[instruction.result_id] =
                    execution->values[instruction.operands[0]];
                execution->initialized[instruction.result_id] = true;
                break;
            case XR_CORE_OP_CORE_CLASS_CONSTRUCT: {
                XrVmClassValue *instance = allocate_class(
                    &execution->context, instruction.result_type_id, instruction.operand_count);
                if (!instance) {
                    execution->finished = true;
                    vm_execution_release_lease(execution);
                    return vm_execution_outcome(execution, XR_VM_OUTCOME_RESOURCE_LIMIT);
                }
                for (uint32_t field = 0u; field < instruction.operand_count; ++field)
                    instance->fields[field] =
                        execution->values[instruction.operands[field]].as.value;
                execution->values[instruction.result_id] = (XrVmRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value =
                        {
                            .kind = XR_VM_VALUE_CLASS_REFERENCE,
                            .as.class_reference = instance,
                        },
                };
                execution->initialized[instruction.result_id] = true;
                emit_lifecycle(&execution->context, XR_VM_EVENT_CLASS_CONSTRUCT,
                               XR_VM_EVENT_ORIGIN_PROGRAM_OPERATION, instance, UINT64_MAX,
                               UINT32_MAX);
                break;
            }
            case XR_CORE_OP_CORE_CLASS_SHARE: {
                XrVmValue source = execution->values[instruction.operands[0]].as.value;
                XrVmClassValue *instance =
                    source.kind == XR_VM_VALUE_CLASS_REFERENCE
                        ? (XrVmClassValue *) (void *) source.as.class_reference
                        : NULL;
                if (!class_value_is_live(instance, instruction.result_type_id) ||
                    instance->owner_count == UINT32_MAX) {
                    execution->finished = true;
                    vm_execution_release_lease(execution);
                    return vm_execution_outcome(execution, XR_VM_OUTCOME_INVALID_INVOCATION);
                }
                ++instance->owner_count;
                execution->values[instruction.result_id] = (XrVmRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value =
                        {
                            .kind = XR_VM_VALUE_CLASS_REFERENCE,
                            .as.class_reference = instance,
                        },
                };
                execution->initialized[instruction.result_id] = true;
                emit_lifecycle(&execution->context, XR_VM_EVENT_CLASS_SHARE,
                               XR_VM_EVENT_ORIGIN_PROGRAM_OPERATION, instance,
                               instance->identity, UINT32_MAX);
                break;
            }
            case XR_CORE_OP_CORE_CLASS_FIELD_LOAD: {
                XrVmValue source = execution->values[instruction.operands[0]].as.value;
                XrVmClassValue *instance =
                    source.kind == XR_VM_VALUE_CLASS_REFERENCE
                        ? (XrVmClassValue *) (void *) source.as.class_reference
                        : NULL;
                XrVmValue loaded = void_value();
                if (!class_field_load_value(&execution->context, instance,
                                            instruction.immediate.field_ordinal,
                                            instruction.result_type_id, &loaded)) {
                    execution->finished = true;
                    vm_execution_release_lease(execution);
                    return vm_execution_outcome(execution, XR_VM_OUTCOME_RESOURCE_LIMIT);
                }
                execution->values[instruction.result_id] = (XrVmRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value = loaded,
                };
                execution->initialized[instruction.result_id] = true;
                emit_lifecycle(&execution->context, XR_VM_EVENT_CLASS_FIELD_LOAD,
                               XR_VM_EVENT_ORIGIN_PROGRAM_OPERATION, instance, UINT64_MAX,
                               instruction.immediate.field_ordinal);
                break;
            }
            case XR_CORE_OP_CORE_CLASS_FIELD_PLACE: {
                XrVmValue source = execution->values[instruction.operands[0]].as.value;
                XrVmClassValue *instance =
                    source.kind == XR_VM_VALUE_CLASS_REFERENCE
                        ? (XrVmClassValue *) (void *) source.as.class_reference
                        : NULL;
                const XrValidatedType *type =
                    instance ? xr_validated_program_type(execution->context.code->program,
                                                         instance->type_id)
                             : NULL;
                uint32_t field = instruction.immediate.field_ordinal;
                if (!type || type->kind != XR_CORE_IR_TYPE_CLASS_REFERENCE ||
                    !class_value_is_live(instance, instance->type_id) ||
                    field >= type->field_count || field >= instance->field_count ||
                    type->field_types[field] != instruction.result_type_id) {
                    execution->finished = true;
                    vm_execution_release_lease(execution);
                    return vm_execution_outcome(execution, XR_VM_OUTCOME_INVALID_INVOCATION);
                }
                execution->places[instruction.result_id].alias = &instance->fields[field];
                execution->places[instruction.result_id].initialized = true;
                execution->values[instruction.result_id] = (XrVmRuntimeValue) {
                    .category = XR_CORE_IR_PLACE,
                    .as.place = &execution->places[instruction.result_id],
                };
                execution->initialized[instruction.result_id] = true;
                emit_lifecycle(&execution->context, XR_VM_EVENT_CLASS_FIELD_PLACE,
                               XR_VM_EVENT_ORIGIN_PROGRAM_OPERATION, instance, UINT64_MAX, field);
                break;
            }
            case XR_CORE_OP_CORE_PLACE_LOCAL:
                execution->places[instruction.result_id].alias =
                    &execution->values[instruction.operands[0]].as.value;
                execution->places[instruction.result_id].initialized = true;
                execution->values[instruction.result_id] = (XrVmRuntimeValue) {
                    .category = XR_CORE_IR_PLACE,
                    .as.place = &execution->places[instruction.result_id],
                };
                execution->initialized[instruction.result_id] = true;
                break;
            case XR_CORE_OP_CORE_PLACE_LOAD:
                execution->values[instruction.result_id] = (XrVmRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value =
                        *vm_place_value(execution->values[instruction.operands[0]].as.place),
                };
                execution->initialized[instruction.result_id] = true;
                break;
            case XR_CORE_OP_CORE_PLACE_STORE:
                *vm_place_value(execution->values[instruction.operands[0]].as.place) =
                    execution->values[instruction.operands[1]].as.value;
                break;
            case XR_CORE_OP_CORE_PLACE_PROJECT: {
                XrVmValue *source =
                    vm_place_value(execution->values[instruction.operands[0]].as.place);
                XrVmAggregateValue *aggregate =
                    (XrVmAggregateValue *) (void *) source->as.aggregate;
                execution->places[instruction.result_id].alias =
                    &aggregate->fields[instruction.immediate.field_ordinal];
                execution->places[instruction.result_id].initialized = true;
                execution->values[instruction.result_id] = (XrVmRuntimeValue) {
                    .category = XR_CORE_IR_PLACE,
                    .as.place = &execution->places[instruction.result_id],
                };
                execution->initialized[instruction.result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_PLACE_TAKE:
                execution->values[instruction.result_id] = (XrVmRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value =
                        *vm_place_value(execution->values[instruction.operands[0]].as.place),
                };
                execution->values[instruction.operands[0]].as.place->initialized = false;
                execution->initialized[instruction.result_id] = true;
                break;
            case XR_CORE_OP_CORE_PLACE_EXCHANGE: {
                XrVmValue *place =
                    vm_place_value(execution->values[instruction.operands[0]].as.place);
                if (!place) {
                    execution->finished = true;
                    vm_execution_release_lease(execution);
                    return vm_execution_outcome(execution, XR_VM_OUTCOME_INVALID_INVOCATION);
                }
                XrVmValue previous = *place;
                XrVmValue replacement =
                    execution->values[instruction.operands[1]].as.value;
                *place = replacement;
                execution->values[instruction.result_id] = (XrVmRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value = previous,
                };
                execution->initialized[instruction.result_id] = true;
                emit_place_exchange(&execution->context, instruction.result_type_id, previous,
                                    replacement);
                break;
            }
            case XR_CORE_OP_CORE_PROVIDER_CALL: {
                const XrValidatedBlock *trap_target =
                    instruction.successor_count == 1u ? &function->blocks[instruction.successors[0]]
                                                      : NULL;
                uint32_t operand_count =
                    instruction.operand_count - (trap_target ? trap_target->argument_count : 0u);
                XrVmOutcome provider = vm_provider_call(&execution->context, function, &instruction,
                                                        execution->values, operand_count);
                if (provider.kind != XR_VM_OUTCOME_RETURN) {
                    if (provider.kind == XR_VM_OUTCOME_TRAP &&
                        provider.trap == XR_VM_TRAP_PROVIDER_CALL_FAILED && trap_target) {
                        vm_execution_assign_edge(execution, trap_target, instruction.operands,
                                                 operand_count, NULL);
                        execution->block_id = instruction.successors[0];
                        execution->instruction_id = 0u;
                        break;
                    }
                    execution->finished = true;
                    XrVmOutcome failed = vm_execution_outcome(execution, provider.kind);
                    failed.trap = provider.trap;
                    vm_execution_release_lease(execution);
                    return failed;
                }
                execution->values[instruction.result_id] = (XrVmRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value = provider.value,
                };
                execution->initialized[instruction.result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_CALL_SEALED_DIRECT:
            case XR_CORE_OP_CORE_CALL_WITNESS_DIRECT: {
                uint32_t target_function =
                    instruction.operation_id == XR_CORE_OP_CORE_CALL_SEALED_DIRECT
                        ? instruction.immediate.function_id
                        : XR_PROGRAM_LOCATION_NONE;
                const XrValidatedFunction *callee =
                    target_function == XR_PROGRAM_LOCATION_NONE
                        ? NULL
                        : &execution->context.code->program->functions[target_function];
                uint32_t call_operand_count = instruction.operand_count;
                if (instruction.successor_count == 1u)
                    call_operand_count -=
                        function->blocks[instruction.successors[0]].argument_count;
                if (instruction.operation_id == XR_CORE_OP_CORE_CALL_WITNESS_DIRECT) {
                    const XrVmExistentialValue *carrier =
                        execution->values[instruction.operands[0]].as.value.as.existential;
                    target_function = witness_function_id(execution->context.code->program, carrier,
                                                          instruction.immediate.u32);
                    if (target_function != XR_PROGRAM_LOCATION_NONE)
                        callee = &execution->context.code->program->functions[target_function];
                }
                if (!callee || call_operand_count != callee->parameter_count) {
                    execution->finished = true;
                    vm_execution_release_lease(execution);
                    return vm_execution_outcome(execution, XR_VM_OUTCOME_INVALID_INVOCATION);
                }
                XrVmRuntimeValue *arguments = xr_calloc(
                    callee->parameter_count ? callee->parameter_count : 1u, sizeof(*arguments));
                if (!arguments) {
                    execution->finished = true;
                    vm_execution_release_lease(execution);
                    return vm_execution_outcome(execution, XR_VM_OUTCOME_RESOURCE_LIMIT);
                }
                uint32_t argument = 0u;
                if (instruction.operation_id == XR_CORE_OP_CORE_CALL_WITNESS_DIRECT) {
                    const XrVmExistentialValue *carrier =
                        execution->values[instruction.operands[0]].as.value.as.existential;
                    if (!witness_receiver_argument(carrier, callee->receiver_mode, &arguments[0])) {
                        xr_free(arguments);
                        execution->finished = true;
                        vm_execution_release_lease(execution);
                        return vm_execution_outcome(execution, XR_VM_OUTCOME_INVALID_INVOCATION);
                    }
                    argument = 1u;
                }
                for (; argument < callee->parameter_count; ++argument)
                    arguments[argument] = execution->values[instruction.operands[argument]];
                XrVmOutcome nested =
                    execute_function(&execution->context, target_function, arguments,
                                     callee->parameter_count, execution->depth + 1u);
                xr_free(arguments);
                if (nested.kind != XR_VM_OUTCOME_RETURN) {
                    if (nested.kind == XR_VM_OUTCOME_TRAP &&
                        nested.trap == XR_VM_TRAP_PROVIDER_CALL_FAILED &&
                        instruction.successor_count == 1u) {
                        const XrValidatedBlock *target =
                            &function->blocks[instruction.successors[0]];
                        vm_execution_assign_edge(execution, target, instruction.operands,
                                                 call_operand_count, NULL);
                        execution->block_id = instruction.successors[0];
                        execution->instruction_id = 0u;
                        break;
                    }
                    execution->finished = true;
                    vm_execution_release_lease(execution);
                    nested.state_id = execution->state_id;
                    return nested;
                }
                if (instruction.result_id != XR_PROGRAM_LOCATION_NONE) {
                    execution->values[instruction.result_id] = (XrVmRuntimeValue) {
                        .category = XR_CORE_IR_VALUE,
                        .as.value = nested.value,
                    };
                    execution->initialized[instruction.result_id] = true;
                }
                break;
            }
            case XR_CORE_OP_CORE_BRANCH:
            case XR_CORE_OP_CORE_CONDITIONAL_BRANCH: {
                uint32_t edge = 0u;
                uint32_t start = 0u;
                if (instruction.operation_id == XR_CORE_OP_CORE_CONDITIONAL_BRANCH) {
                    edge = execution->values[instruction.operands[0]].as.value.as.boolean ? 0u : 1u;
                    start = 1u + (edge ? function->blocks[instruction.successors[0]].argument_count
                                       : 0u);
                }
                const XrValidatedBlock *target = &function->blocks[instruction.successors[edge]];
                vm_execution_assign_edge(execution, target, instruction.operands, start, NULL);
                execution->block_id = instruction.successors[edge];
                execution->instruction_id = 0u;
                break;
            }
            case XR_CORE_OP_CORE_COROUTINE_YIELD: {
                uint32_t safepoint_id = instruction.immediate.u32;
                const XrValidatedCoroutineSafepoint *safepoint =
                    &function->coroutine_safepoints[safepoint_id];
                execution->suspension_block_id = execution->block_id;
                execution->suspension_instruction_id = instruction_id;
                execution->state_id = safepoint->resume_state_id;
                execution->block_id = instruction.successors[0];
                execution->instruction_id = 0u;
                execution->suspended = true;
                execution->cancel_block_id = instruction.successors[1];
                XrVmOutcome result = vm_execution_outcome(execution, XR_VM_OUTCOME_SUSPENDED);
                result.safepoint_id = safepoint_id;
                result.suspension.kind = XR_SUSPENSION_REQUEST_COOPERATIVE_YIELD;
                return result;
            }
            case XR_CORE_OP_CORE_COROUTINE_SUSPEND: {
                uint32_t safepoint_id = instruction.immediate.coroutine_suspend.safepoint_id;
                uint32_t request_value = instruction.operands[0];
                const XrValidatedCoroutineSafepoint *safepoint =
                    &function->coroutine_safepoints[safepoint_id];
                execution->suspension_block_id = execution->block_id;
                execution->suspension_instruction_id = instruction_id;
                execution->state_id = safepoint->resume_state_id;
                execution->block_id = instruction.successors[0];
                execution->instruction_id = 0u;
                execution->suspended = true;
                execution->cancel_block_id = instruction.successors[1];
                XrVmOutcome result = vm_execution_outcome(execution, XR_VM_OUTCOME_SUSPENDED);
                result.safepoint_id = safepoint_id;
                result.suspension.kind = XR_SUSPENSION_REQUEST_TIMER_AFTER_MS;
                result.suspension.operand_count = 1u;
                result.suspension.payload.timer_after_ms = xr_suspension_timer_normalize_ms(
                    execution->values[request_value].as.value.as.i64);
                return result;
            }
            case XR_CORE_OP_CORE_COROUTINE_CALL_SEALED:
            case XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT: {
                bool indirect = instruction.operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT;
                uint32_t callee_id = instruction.immediate.coroutine_call.function_id;
                uint32_t safepoint_id = indirect
                                            ? instruction.immediate.u32
                                            : instruction.immediate.coroutine_call.safepoint_id;
                const XrValidatedCoroutineSafepoint *safepoint =
                    &function->coroutine_safepoints[safepoint_id];
                if (!execution->child &&
                    !vm_child_execution_create(execution, &instruction, &execution->child)) {
                    execution->finished = true;
                    vm_execution_release_lease(execution);
                    return vm_execution_outcome(execution, XR_VM_OUTCOME_RESOURCE_LIMIT);
                }
                callee_id = execution->child->function_id;
                const XrValidatedFunction *callee =
                    &execution->context.code->program->functions[callee_id];
                uint64_t child_steps = execution->child->context.steps;
                XrVmOutcome child = xr_vm_execution_step(execution->child);
                uint64_t child_delta = execution->child->context.steps - child_steps;
                if (child_delta >
                    execution->context.code->options.max_steps - execution->context.steps) {
                    execution->finished = true;
                    xr_vm_execution_free(execution->child);
                    execution->child = NULL;
                    vm_execution_release_lease(execution);
                    return vm_execution_outcome(execution, XR_VM_OUTCOME_RESOURCE_LIMIT);
                }
                execution->context.steps += child_delta;
                if (child.kind == XR_VM_OUTCOME_SUSPENDED) {
                    execution->suspension_block_id = execution->block_id;
                    execution->suspension_instruction_id = instruction_id;
                    --execution->instruction_id;
                    execution->state_id = safepoint->resume_state_id;
                    execution->suspended = true;
                    execution->cancel_block_id = instruction.successors[1];
                    XrVmOutcome suspended =
                        vm_execution_outcome(execution, XR_VM_OUTCOME_SUSPENDED);
                    suspended.safepoint_id = safepoint_id;
                    suspended.suspension = child.suspension;
                    return suspended;
                }
                if (!vm_adopt_child_storage(execution)) {
                    execution->finished = true;
                    xr_vm_execution_free(execution->child);
                    execution->child = NULL;
                    vm_execution_release_lease(execution);
                    return vm_execution_outcome(execution, XR_VM_OUTCOME_RESOURCE_LIMIT);
                }
                if (child.kind != XR_VM_OUTCOME_RETURN) {
                    if (child.kind == XR_VM_OUTCOME_TRAP &&
                        child.trap == XR_VM_TRAP_PROVIDER_CALL_FAILED &&
                        instruction.successor_count == 3u) {
                        execution->suspension_block_id = execution->block_id;
                        execution->suspension_instruction_id = instruction_id;
                        bool transferred = vm_materialize_suspension_edge(execution, 2u);
                        xr_vm_execution_free(execution->child);
                        execution->child = NULL;
                        execution->suspension_block_id = XR_PROGRAM_LOCATION_NONE;
                        execution->suspension_instruction_id = XR_PROGRAM_LOCATION_NONE;
                        if (!transferred) {
                            execution->finished = true;
                            vm_execution_release_lease(execution);
                            return vm_execution_outcome(execution,
                                                        XR_VM_OUTCOME_INVALID_INVOCATION);
                        }
                        execution->instruction_id = 0u;
                        break;
                    }
                    execution->finished = true;
                    xr_vm_execution_free(execution->child);
                    execution->child = NULL;
                    vm_execution_release_lease(execution);
                    child.steps = execution->context.steps;
                    child.state_id = execution->state_id;
                    return child;
                }
                const XrValidatedBlock *normal = &function->blocks[instruction.successors[0]];
                uint32_t implicit_result = callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u;
                uint32_t parameter_prefix = callee->parameter_count;
                if (indirect) {
                    const XrVmCallableValue *carrier =
                        execution->values[instruction.operands[0]].as.value.as.callable;
                    const XrValidatedType *callable =
                        carrier ? xr_validated_program_type(execution->context.code->program,
                                                            carrier->callable_type_id)
                                : NULL;
                    if (!callable || callable->signature_id >=
                                         execution->context.code->program->signature_count) {
                        execution->finished = true;
                        xr_vm_execution_free(execution->child);
                        execution->child = NULL;
                        vm_execution_release_lease(execution);
                        return vm_execution_outcome(execution, XR_VM_OUTCOME_INVALID_INVOCATION);
                    }
                    parameter_prefix =
                        execution->context.code->program->signatures[callable->signature_id]
                            .parameter_count +
                        1u;
                }
                vm_execution_assign_edge(execution, normal, instruction.operands, parameter_prefix,
                                         implicit_result ? &child.value : NULL);
                xr_vm_execution_free(execution->child);
                execution->child = NULL;
                execution->block_id = instruction.successors[0];
                execution->instruction_id = 0u;
                break;
            }
            case XR_CORE_OP_CORE_OWNER_DROP:
                drop_vm_value(&execution->context,
                              &execution->values[instruction.operands[0]].as.value,
                              XR_VM_EVENT_ORIGIN_PROGRAM_OPERATION);
                break;
            case XR_CORE_OP_CORE_CANCEL_PUBLISH:
                execution->finished = true;
                vm_execution_release_lease(execution);
                return vm_execution_outcome(execution, XR_VM_OUTCOME_CANCELLED);
            case XR_CORE_OP_CORE_TRAP: {
                execution->finished = true;
                XrVmOutcome result = vm_execution_outcome(execution, XR_VM_OUTCOME_TRAP);
                result.trap = instruction.immediate.u32 == 7u ? XR_VM_TRAP_PROVIDER_CALL_FAILED
                                                              : XR_VM_TRAP_EXPLICIT;
                vm_execution_release_lease(execution);
                return result;
            }
            case XR_CORE_OP_CORE_RETURN: {
                execution->finished = true;
                XrVmOutcome result = vm_execution_outcome(execution, XR_VM_OUTCOME_RETURN);
                result.value = instruction.operand_count == 0u
                                   ? void_value()
                                   : execution->values[instruction.operands[0]].as.value;
                if (vm_value_contains_class(result.value))
                    result = vm_execution_outcome(execution,
                                                  XR_VM_OUTCOME_INVALID_INVOCATION);
                vm_execution_release_lease(execution);
                return result;
            }
            default:
                execution->finished = true;
                vm_execution_release_lease(execution);
                return vm_execution_outcome(execution, XR_VM_OUTCOME_INVALID_INVOCATION);
        }
    }
}

XrVmOutcome xr_vm_execution_cancel(XrVmExecution *execution) {
    if (!execution || execution->finished || !execution->suspended ||
        execution->cancel_block_id == XR_PROGRAM_LOCATION_NONE ||
        !xr_execution_lease_is_valid(&execution->lease))
        return vm_execution_outcome(execution, XR_VM_OUTCOME_INVALID_INVOCATION);
    uint32_t successor_index = 1u;
    if (execution->child) {
        uint64_t child_steps = execution->child->context.steps;
        XrVmOutcome child = xr_vm_execution_cancel(execution->child);
        uint64_t child_delta = child.steps - child_steps;
        if (child_delta > execution->context.code->options.max_steps - execution->context.steps) {
            execution->finished = true;
            xr_vm_execution_free(execution->child);
            execution->child = NULL;
            vm_execution_release_lease(execution);
            return vm_execution_outcome(execution, XR_VM_OUTCOME_RESOURCE_LIMIT);
        }
        execution->context.steps += child_delta;
        if (!vm_adopt_child_storage(execution)) {
            execution->finished = true;
            xr_vm_execution_free(execution->child);
            execution->child = NULL;
            vm_execution_release_lease(execution);
            return vm_execution_outcome(execution, XR_VM_OUTCOME_RESOURCE_LIMIT);
        }
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
    return xr_vm_execution_step(execution);
}

void xr_vm_execution_free(XrVmExecution *execution) {
    if (!execution)
        return;
    xr_vm_execution_free(execution->child);
    vm_execution_release_lease(execution);
    free_aggregates(&execution->context);
    xr_free(execution->initialized);
    xr_free(execution->places);
    xr_free(execution->edge_values);
    xr_free(execution->values);
    xr_vm_code_free(execution->code);
    xr_free(execution);
}

XrVmOutcome xr_vm_code_execute(const XrVmCode *code, XrInstance *instance, uint32_t function_id,
                               const XrVmValue *arguments, uint32_t argument_count) {
    XrVmContext context = {.code = code};
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
    xr_sha256_update(&context.trace, code->cache_key.execution_id.bytes,
                     sizeof(code->cache_key.execution_id.bytes));
    hash_u32(&context.trace, function_id);
    const XrValidatedFunction *function = &code->program->functions[function_id];
    XrVmRuntimeValue *runtime_arguments =
        xr_calloc(argument_count ? argument_count : 1u, sizeof(XrVmRuntimeValue));
    if (!runtime_arguments) {
        free_aggregates(&context);
        (void) xr_execution_lease_release(&lease);
        return vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, &context);
    }
    for (uint32_t index = 0; index < argument_count; ++index) {
        if (index >= function->parameter_count ||
            function->parameter_modes[index] == XR_PARAM_REF) {
            xr_free(runtime_arguments);
            free_aggregates(&context);
            (void) xr_execution_lease_release(&lease);
            return vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, &context);
        }
        runtime_arguments[index].category = XR_CORE_IR_VALUE;
        runtime_arguments[index].as.value = arguments[index];
    }
    XrVmOutcome outcome =
        execute_function(&context, function_id, runtime_arguments, argument_count, 1u);
    xr_free(runtime_arguments);
    if (outcome.kind == XR_VM_OUTCOME_RETURN && vm_value_contains_class(outcome.value))
        outcome = vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, &context);
    if (outcome.kind == XR_VM_OUTCOME_RETURN && outcome.value.kind == XR_VM_VALUE_AGGREGATE) {
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
    free_aggregates(&context);
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
        case XR_VM_CODE_INSTANCE_UNAVAILABLE:
            return "instance-unavailable";
        case XR_VM_CODE_POLICY_REJECTED:
            return "policy-rejected";
        case XR_VM_CODE_OUT_OF_MEMORY:
            return "out-of-memory";
        default:
            return "unknown";
    }
}
