/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_reference_evaluator.c - Simple host-independent CoreSpec evaluator
 */

#include "xr_reference_evaluator.h"

#include "../base/xmalloc.h"
#include "../core/xr_core_spec_gen.h"
#include "../runtime/abi/xr_target_machine_facts.h"
#include "../runtime/core/xr_text_kernel.h"
#include "xr_validated_program_internal.h"

#include <limits.h>
#include <string.h>

typedef struct XrReferenceExistentialValue XrReferenceExistentialValue;
typedef struct XrReferenceCallableValue XrReferenceCallableValue;
typedef struct XrReferenceClassValue XrReferenceClassValue;
typedef struct XrReferenceStringValue XrReferenceStringValue;
typedef struct DetachedDisposeState DetachedDisposeState;

static bool class_value_is_live(const XrReferenceClassValue *value, uint16_t type_id);

typedef struct EvalContext {
    const XrValidatedProgram *program;
    const XrReferenceProviderBinding *providers;
    XrReferenceProfile profile;
    XrReferenceBudget budget;
    uint64_t steps;
    uint64_t aggregate_cell_count;
    struct XrReferenceAggregateValue **aggregates;
    uint32_t aggregate_count;
    uint32_t aggregate_capacity;
    XrReferenceClassValue **classes;
    uint32_t class_count;
    uint32_t class_capacity;
    uint64_t next_class_identity;
    XrReferenceExistentialValue **existentials;
    uint32_t existential_count;
    uint32_t existential_capacity;
    XrReferenceCallableValue **callables;
    uint32_t callable_count;
    uint32_t callable_capacity;
    XrReferenceStringValue **strings;
    uint32_t string_count;
    uint32_t string_capacity;
} EvalContext;

static void drop_class_value(EvalContext *context, XrReferenceClassValue *value,
                             XrReferenceLifecycleEventOrigin origin,
                             DetachedDisposeState *dispose_state);
static void drop_reference_value(EvalContext *context, XrReferenceValue *value,
                                 XrReferenceLifecycleEventOrigin origin,
                                 DetachedDisposeState *dispose_state);

typedef struct XrReferenceAggregateValue {
    uint16_t type_id;
    uint32_t variant_ordinal;
    XrReferenceValue *fields;
    uint32_t field_count;
    bool detached;
    bool dispose_queued;
    struct XrReferenceAggregateValue *dispose_next;
} XrReferenceAggregateValue;

struct XrReferenceClassValue {
    uint16_t type_id;
    uint16_t reserved16;
    uint32_t owner_count;
    uint32_t field_count;
    uint64_t identity;
    XrReferenceValue *fields;
    bool alive;
    bool detached;
};

typedef struct EvalPlace {
    XrReferenceValue value;
    XrReferenceValue *alias;
    bool initialized;
} EvalPlace;

typedef struct EvalRuntimeValue {
    XrCoreIrValueCategory category;
    union {
        XrReferenceValue value;
        EvalPlace *place;
    } as;
} EvalRuntimeValue;

struct XrReferenceExistentialValue {
    uint16_t existential_type_id;
    uint16_t concrete_type_id;
    uint32_t conformance_id;
    EvalRuntimeValue payload;
    EvalPlace owned_storage;
    bool detached;
    bool dispose_queued;
    XrReferenceExistentialValue *dispose_next;
};

struct XrReferenceCallableValue {
    uint16_t callable_type_id;
    uint16_t capture_type_id;
    uint32_t function_id;
    bool has_capture;
    XrReferenceValue capture;
    bool detached;
    bool dispose_queued;
    XrReferenceCallableValue *dispose_next;
};

/* One immutable string owner.  Bytes are private to the cell; every logical
 * copy is a distinct cell so ownership stays exactly-once observable. */
struct XrReferenceStringValue {
    uint32_t size;
    bool detached;
    bool dispose_queued;
    XrReferenceStringValue *dispose_next;
    uint8_t *bytes;
};

struct DetachedDisposeState {
    XrReferenceAggregateValue *aggregates;
    XrReferenceExistentialValue *existentials;
    XrReferenceCallableValue *callables;
    XrReferenceStringValue *strings;
};

struct XrReferenceExecution {
    XrExecutionLease lease;
    EvalContext *context;
    XrReferenceProviderBinding providers;
    EvalRuntimeValue *values;
    EvalRuntimeValue *edge_scratch;
    EvalPlace *places;
    bool *initialized;
    uint32_t edge_scratch_count;
    uint32_t function_id;
    uint32_t block_id;
    uint32_t instruction_id;
    uint32_t state_id;
    uint32_t cancel_block_id;
    uint32_t suspension_block_id;
    uint32_t suspension_instruction_id;
    struct XrReferenceExecution *child;
    uint32_t depth;
    bool owns_context;
    bool owns_lease;
    bool suspended;
    bool finished;
};

static XrReferenceOutcome outcome(XrReferenceOutcomeKind kind, EvalContext *context) {
    XrReferenceOutcome result = {.kind = kind, .steps = context->steps};
    return result;
}

static XrReferenceOutcome trap_outcome(EvalContext *context, XrReferenceTrap trap) {
    XrReferenceOutcome result = outcome(XR_REFERENCE_OUTCOME_TRAP, context);
    result.trap = trap;
    return result;
}

static XrReferenceValue void_value(void) {
    XrReferenceValue value = {.kind = XR_REFERENCE_VALUE_VOID};
    return value;
}

static XrReferenceValue *eval_place_value(EvalPlace *place) {
    return place ? (place->alias ? place->alias : &place->value) : NULL;
}

static const XrReferenceValue *eval_place_value_const(const EvalPlace *place) {
    return place ? (place->alias ? place->alias : &place->value) : NULL;
}

static bool reference_value_matches_type(const XrValidatedProgram *program, XrReferenceValue value,
                                         uint16_t type_id) {
    switch (type_id) {
        case XR_CORE_TYPE_VOID:
            return value.kind == XR_REFERENCE_VALUE_VOID;
        case XR_CORE_TYPE_BOOL:
            return value.kind == XR_REFERENCE_VALUE_BOOL;
        case XR_CORE_TYPE_I64:
            return value.kind == XR_REFERENCE_VALUE_I64;
        case XR_CORE_TYPE_U32:
            return value.kind == XR_REFERENCE_VALUE_U32;
        case XR_CORE_TYPE_U16:
            return value.kind == XR_REFERENCE_VALUE_U16;
        case XR_CORE_TYPE_TARGET_OS:
            return value.kind == XR_REFERENCE_VALUE_TARGET_OS;
        case XR_CORE_TYPE_TARGET_ARCH:
            return value.kind == XR_REFERENCE_VALUE_TARGET_ARCH;
        case XR_CORE_TYPE_TARGET_ABI:
            return value.kind == XR_REFERENCE_VALUE_TARGET_ABI;
        case XR_CORE_TYPE_TARGET_ENDIAN:
            return value.kind == XR_REFERENCE_VALUE_TARGET_ENDIAN;
        case XR_CORE_TYPE_ERROR:
            return value.kind == XR_REFERENCE_VALUE_ERROR;
        case XR_CORE_TYPE_PANIC_INFO:
            return value.kind == XR_REFERENCE_VALUE_PANIC_INFO;
        case XR_CORE_TYPE_STRING:
            return value.kind == XR_REFERENCE_VALUE_STRING && value.as.string;
        case XR_CORE_TYPE_RUNE:
            return value.kind == XR_REFERENCE_VALUE_RUNE && xr_text_rune_is_scalar(value.as.rune);
        default: {
            const XrValidatedType *type = xr_validated_program_type(program, type_id);
            if (!type)
                return false;
            if (type->kind == XR_CORE_IR_TYPE_EXISTENTIAL)
                return value.kind == XR_REFERENCE_VALUE_EXISTENTIAL && value.as.existential &&
                       ((const XrReferenceExistentialValue *) value.as.existential)
                               ->existential_type_id == type_id;
            if (type->kind == XR_CORE_IR_TYPE_CALLABLE)
                return value.kind == XR_REFERENCE_VALUE_CALLABLE && value.as.callable &&
                       ((const XrReferenceCallableValue *) value.as.callable)->callable_type_id ==
                           type_id;
            if (type->kind == XR_CORE_IR_TYPE_CLASS_REFERENCE)
                return value.kind == XR_REFERENCE_VALUE_CLASS_REFERENCE &&
                       class_value_is_live(value.as.class_reference, type_id);
            return (type->kind == XR_CORE_IR_TYPE_AGGREGATE ||
                    type->kind == XR_CORE_IR_TYPE_VARIANT) &&
                   value.kind == XR_REFERENCE_VALUE_AGGREGATE && value.as.aggregate &&
                   ((const XrReferenceAggregateValue *) value.as.aggregate)->type_id == type_id;
        }
    }
}

static XrReferenceAggregateValue *allocate_aggregate(EvalContext *context, uint16_t type_id,
                                                     uint32_t variant_ordinal,
                                                     uint32_t field_count) {
    if ((uint64_t) field_count > context->budget.max_value_cells - context->aggregate_cell_count)
        return NULL;
    if (context->aggregate_count == context->aggregate_capacity) {
        uint32_t capacity = context->aggregate_capacity ? context->aggregate_capacity * 2u : 8u;
        if (capacity < context->aggregate_count)
            return NULL;
#if SIZE_MAX < UINT64_MAX
        if ((size_t) capacity > SIZE_MAX / sizeof(*context->aggregates))
            return NULL;
#endif
        XrReferenceAggregateValue **grown =
            xr_realloc(context->aggregates, (size_t) capacity * sizeof(*context->aggregates));
        if (!grown)
            return NULL;
        context->aggregates = grown;
        context->aggregate_capacity = capacity;
    }
    XrReferenceAggregateValue *aggregate = xr_calloc(1u, sizeof(*aggregate));
    if (!aggregate)
        return NULL;
    if (field_count != 0u) {
        aggregate->fields = xr_calloc(field_count, sizeof(XrReferenceValue));
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

/* Every string cell counts one value cell plus its payload bytes against the
 * evaluator budget, so runaway concatenation surfaces as a resource limit
 * instead of unbounded host memory. */
static XrReferenceStringValue *allocate_string(EvalContext *context, size_t size) {
    if (size > XR_PROGRAM_CONSTANT_STRING_MAX_BYTES ||
        (uint64_t) size + 1u > context->budget.max_value_cells - context->aggregate_cell_count)
        return NULL;
    if (context->string_count == context->string_capacity) {
        uint32_t capacity = context->string_capacity ? context->string_capacity * 2u : 8u;
        if (capacity < context->string_count)
            return NULL;
#if SIZE_MAX < UINT64_MAX
        if ((size_t) capacity > SIZE_MAX / sizeof(*context->strings))
            return NULL;
#endif
        XrReferenceStringValue **grown =
            xr_realloc(context->strings, (size_t) capacity * sizeof(*context->strings));
        if (!grown)
            return NULL;
        context->strings = grown;
        context->string_capacity = capacity;
    }
    XrReferenceStringValue *string = xr_calloc(1u, sizeof(*string));
    if (!string)
        return NULL;
    string->bytes = xr_malloc(size != 0u ? size : 1u);
    if (!string->bytes) {
        xr_free(string);
        return NULL;
    }
    string->size = (uint32_t) size;
    context->strings[context->string_count++] = string;
    context->aggregate_cell_count += (uint64_t) size + 1u;
    return string;
}

static XrReferenceValue string_value(const XrReferenceStringValue *string) {
    XrReferenceValue value = {.kind = XR_REFERENCE_VALUE_STRING};
    value.as.string = string;
    return value;
}

static bool string_view(XrReferenceValue value, const uint8_t **bytes_out, size_t *size_out) {
    const XrReferenceStringValue *string = value.as.string;
    if (value.kind != XR_REFERENCE_VALUE_STRING || !string)
        return false;
    *bytes_out = string->bytes;
    *size_out = string->size;
    return true;
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

static void emit_lifecycle_event(EvalContext *context, XrReferenceLifecycleEvent event) {
    if (!context || !context->providers || !context->providers->lifecycle_event)
        return;
    context->providers->lifecycle_event(context->providers->lifecycle_context, &event);
}

static void emit_lifecycle(EvalContext *context, XrReferenceLifecycleEventKind kind,
                           XrReferenceLifecycleEventOrigin origin,
                           const XrReferenceClassValue *value, uint64_t related_identity,
                           uint32_t field_ordinal) {
    XrReferenceLifecycleEvent event = {
        .kind = kind,
        .origin = origin,
        .type_id = value ? value->type_id : XR_CORE_TYPE_VOID,
        .field_ordinal = field_ordinal,
        .identity = value ? value->identity : UINT64_MAX,
        .related_identity = related_identity,
    };
    emit_lifecycle_event(context, event);
}

static void emit_place_exchange(EvalContext *context, uint16_t type_id, XrReferenceValue previous,
                                XrReferenceValue replacement) {
    const XrReferenceClassValue *previous_class =
        previous.kind == XR_REFERENCE_VALUE_CLASS_REFERENCE ? previous.as.class_reference : NULL;
    const XrReferenceClassValue *replacement_class =
        replacement.kind == XR_REFERENCE_VALUE_CLASS_REFERENCE ? replacement.as.class_reference
                                                               : NULL;
    emit_lifecycle_event(context, (XrReferenceLifecycleEvent) {
                                      .kind = XR_REFERENCE_EVENT_PLACE_EXCHANGE,
                                      .origin = XR_REFERENCE_EVENT_ORIGIN_PROGRAM_OPERATION,
                                      .type_id = type_id,
                                      .field_ordinal = UINT32_MAX,
                                      .identity = previous_class ? previous_class->identity
                                                                 : UINT64_MAX,
                                      .related_identity = replacement_class
                                                              ? replacement_class->identity
                                                              : UINT64_MAX,
                                  });
}

static XrReferenceClassValue *allocate_class(EvalContext *context, uint16_t type_id,
                                             uint32_t field_count) {
    if (!context || (uint64_t) field_count >
                        context->budget.max_value_cells - context->aggregate_cell_count)
        return NULL;
    if (context->class_count == context->class_capacity) {
        uint32_t capacity = context->class_capacity ? context->class_capacity * 2u : 8u;
        if (capacity < context->class_count ||
            (size_t) capacity > SIZE_MAX / sizeof(*context->classes))
            return NULL;
        XrReferenceClassValue **grown =
            xr_realloc(context->classes, (size_t) capacity * sizeof(*context->classes));
        if (!grown)
            return NULL;
        context->classes = grown;
        context->class_capacity = capacity;
    }
    XrReferenceClassValue *value = xr_calloc(1u, sizeof(*value));
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

static bool class_value_is_live(const XrReferenceClassValue *value, uint16_t type_id) {
    return value && value->alive && value->owner_count != 0u && value->type_id == type_id &&
           (value->field_count == 0u || value->fields);
}

static void drop_class_value(EvalContext *context, XrReferenceClassValue *value,
                             XrReferenceLifecycleEventOrigin origin,
                             DetachedDisposeState *dispose_state) {
    if (!value || !value->alive || value->owner_count == 0u)
        return;
    emit_lifecycle(context, XR_REFERENCE_EVENT_OWNER_DROP, origin, value, UINT64_MAX, UINT32_MAX);
    if (--value->owner_count != 0u)
        return;
    emit_lifecycle(context, XR_REFERENCE_EVENT_CLASS_FINALIZE, origin, value, UINT64_MAX,
                   UINT32_MAX);
    for (uint32_t field = value->field_count; field != 0u; --field) {
        XrReferenceValue *nested = &value->fields[field - 1u];
        drop_reference_value(context, nested, XR_REFERENCE_EVENT_ORIGIN_FIELD_FINALIZATION,
                             dispose_state);
    }
    xr_free(value->fields);
    value->fields = NULL;
    value->field_count = 0u;
    value->alive = false;
    emit_lifecycle(context, XR_REFERENCE_EVENT_CLASS_RECLAIM, origin, value, UINT64_MAX,
                   UINT32_MAX);
    if (!context)
        xr_free(value);
}

static void drop_reference_value(EvalContext *context, XrReferenceValue *value,
                                 XrReferenceLifecycleEventOrigin origin,
                                 DetachedDisposeState *dispose_state) {
    if (!value)
        return;
    switch (value->kind) {
        case XR_REFERENCE_VALUE_CLASS_REFERENCE:
            drop_class_value(context,
                             (XrReferenceClassValue *) (void *) value->as.class_reference, origin,
                             dispose_state);
            break;
        case XR_REFERENCE_VALUE_AGGREGATE:
            if (value->as.aggregate) {
                XrReferenceAggregateValue *aggregate =
                    (XrReferenceAggregateValue *) (void *) value->as.aggregate;
                if (dispose_state && aggregate->dispose_queued)
                    break;
                if (dispose_state) {
                    aggregate->dispose_queued = true;
                    aggregate->dispose_next = dispose_state->aggregates;
                    dispose_state->aggregates = aggregate;
                }
                for (uint32_t field = aggregate->field_count; field != 0u; --field)
                    drop_reference_value(context, &aggregate->fields[field - 1u], origin,
                                         dispose_state);
                if (!context && !dispose_state) {
                    xr_free(aggregate->fields);
                    xr_free(aggregate);
                }
            }
            break;
        case XR_REFERENCE_VALUE_EXISTENTIAL:
            if (value->as.existential) {
                XrReferenceExistentialValue *existential =
                    (XrReferenceExistentialValue *) (void *) value->as.existential;
                if (dispose_state && existential->dispose_queued)
                    break;
                if (dispose_state) {
                    existential->dispose_queued = true;
                    existential->dispose_next = dispose_state->existentials;
                    dispose_state->existentials = existential;
                }
                if (existential->owned_storage.initialized)
                    drop_reference_value(context, &existential->owned_storage.value, origin,
                                         dispose_state);
                if (!context && !dispose_state)
                    xr_free(existential);
            }
            break;
        case XR_REFERENCE_VALUE_CALLABLE:
            if (value->as.callable) {
                XrReferenceCallableValue *callable =
                    (XrReferenceCallableValue *) (void *) value->as.callable;
                if (dispose_state && callable->dispose_queued)
                    break;
                if (dispose_state) {
                    callable->dispose_queued = true;
                    callable->dispose_next = dispose_state->callables;
                    dispose_state->callables = callable;
                }
                if (callable->has_capture)
                    drop_reference_value(context, &callable->capture, origin, dispose_state);
                if (!context && !dispose_state)
                    xr_free(callable);
            }
            break;
        case XR_REFERENCE_VALUE_STRING:
            /* Releasing text is not a semantic event: no finalizer, no
             * resource, nothing an observer can distinguish. */
            if (value->as.string) {
                XrReferenceStringValue *string =
                    (XrReferenceStringValue *) (void *) value->as.string;
                if (dispose_state && string->dispose_queued)
                    break;
                if (dispose_state) {
                    string->dispose_queued = true;
                    string->dispose_next = dispose_state->strings;
                    dispose_state->strings = string;
                }
                if (!context && !dispose_state) {
                    xr_free(string->bytes);
                    xr_free(string);
                }
            }
            break;
        default:
            break;
    }
    *value = void_value();
}

static XrReferenceExistentialValue *allocate_existential(EvalContext *context) {
    if (context->aggregate_cell_count == context->budget.max_value_cells)
        return NULL;
    if (context->existential_count == context->existential_capacity) {
        uint32_t capacity = context->existential_capacity ? context->existential_capacity * 2u : 8u;
        if (capacity < context->existential_count)
            return NULL;
#if SIZE_MAX < UINT64_MAX
        if ((size_t) capacity > SIZE_MAX / sizeof(*context->existentials))
            return NULL;
#endif
        XrReferenceExistentialValue **grown =
            xr_realloc(context->existentials, (size_t) capacity * sizeof(*context->existentials));
        if (!grown)
            return NULL;
        context->existentials = grown;
        context->existential_capacity = capacity;
    }
    XrReferenceExistentialValue *value = xr_calloc(1u, sizeof(*value));
    if (!value)
        return NULL;
    context->existentials[context->existential_count++] = value;
    ++context->aggregate_cell_count;
    return value;
}

static XrReferenceCallableValue *allocate_callable(EvalContext *context) {
    if (context->aggregate_cell_count == context->budget.max_value_cells)
        return NULL;
    if (context->callable_count == context->callable_capacity) {
        uint32_t capacity = context->callable_capacity ? context->callable_capacity * 2u : 8u;
        if (capacity < context->callable_count)
            return NULL;
#if SIZE_MAX < UINT64_MAX
        if ((size_t) capacity > SIZE_MAX / sizeof(*context->callables))
            return NULL;
#endif
        XrReferenceCallableValue **grown =
            xr_realloc(context->callables, (size_t) capacity * sizeof(*context->callables));
        if (!grown)
            return NULL;
        context->callables = grown;
        context->callable_capacity = capacity;
    }
    XrReferenceCallableValue *value = xr_calloc(1u, sizeof(*value));
    if (!value)
        return NULL;
    context->callables[context->callable_count++] = value;
    ++context->aggregate_cell_count;
    return value;
}

static uint32_t callable_function_id(const XrValidatedProgram *program,
                                     const XrReferenceCallableValue *carrier) {
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
                                    const XrReferenceExistentialValue *carrier,
                                    uint32_t slot_ordinal) {
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

static bool witness_receiver_argument(const XrReferenceExistentialValue *carrier,
                                      XrParamMode receiver_mode, EvalRuntimeValue *argument) {
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
        argument->as.value = *eval_place_value_const(carrier->payload.as.place);
        return true;
    }
    *argument = carrier->payload;
    return argument->category == XR_CORE_IR_VALUE;
}

#include "xr_reference_clone.inc.c"

bool xr_reference_value_aggregate_view(const XrReferenceValue *value,
                                       XrReferenceAggregateView *view_out) {
    if (view_out)
        memset(view_out, 0, sizeof(*view_out));
    if (!value || !view_out || value->kind != XR_REFERENCE_VALUE_AGGREGATE || !value->as.aggregate)
        return false;
    const XrReferenceAggregateValue *aggregate = value->as.aggregate;
    *view_out = (XrReferenceAggregateView) {
        .type_id = aggregate->type_id,
        .variant_ordinal = aggregate->variant_ordinal,
        .fields = aggregate->fields,
        .field_count = aggregate->field_count,
    };
    return true;
}

bool xr_reference_value_string_view(const XrReferenceValue *value,
                                    XrReferenceStringView *view_out) {
    if (view_out)
        memset(view_out, 0, sizeof(*view_out));
    if (!value || !view_out || value->kind != XR_REFERENCE_VALUE_STRING || !value->as.string)
        return false;
    const XrReferenceStringValue *string = value->as.string;
    view_out->bytes = string->bytes;
    view_out->size = string->size;
    return true;
}

void xr_reference_outcome_dispose(XrReferenceOutcome *outcome) {
    if (!outcome)
        return;
    if (outcome->owns_dynamic_values) {
        DetachedDisposeState state = {0};
        dispose_detached_reference_value(&state, &outcome->value);
        dispose_detached_reference_value(&state, &outcome->error_value);
        dispose_detached_reference_value(&state, &outcome->panic_value);
        free_detached_dispose_state(&state);
    }
    memset(outcome, 0, sizeof(*outcome));
}

static void free_eval_arena(EvalContext *context) {
    for (uint32_t index = 0; index < context->class_count; ++index) {
        XrReferenceClassValue *value = context->classes[index];
        if (!value || value->detached)
            continue;
        if (value->alive)
            emit_lifecycle(context, XR_REFERENCE_EVENT_CLASS_RECLAIM,
                           XR_REFERENCE_EVENT_ORIGIN_DOMAIN_TEARDOWN, value, UINT64_MAX,
                           UINT32_MAX);
        xr_free(value->fields);
        xr_free(value);
    }
    for (uint32_t index = 0; index < context->aggregate_count; ++index) {
        if (context->aggregates[index]->detached)
            continue;
        xr_free(context->aggregates[index]->fields);
        xr_free(context->aggregates[index]);
    }
    xr_free(context->aggregates);
    xr_free(context->classes);
    for (uint32_t index = 0; index < context->existential_count; ++index) {
        if (!context->existentials[index]->detached)
            xr_free(context->existentials[index]);
    }
    xr_free(context->existentials);
    for (uint32_t index = 0; index < context->callable_count; ++index) {
        if (!context->callables[index]->detached)
            xr_free(context->callables[index]);
    }
    xr_free(context->callables);
    for (uint32_t index = 0; index < context->string_count; ++index) {
        if (context->strings[index]->detached)
            continue;
        xr_free(context->strings[index]->bytes);
        xr_free(context->strings[index]);
    }
    xr_free(context->strings);
}

/* Outcome of one text operation shared by both evaluation loops. */
typedef enum TextOperationStatus {
    TEXT_OPERATION_OK = 0,
    TEXT_OPERATION_RESOURCE_LIMIT,
    TEXT_OPERATION_PROVIDER_FAILED,
    TEXT_OPERATION_INVALID,
} TextOperationStatus;

static bool operation_is_text(uint16_t operation_id) {
    return operation_id == XR_CORE_OP_CORE_CONSTANT_STRING ||
           operation_id == XR_CORE_OP_CORE_CONSTANT_RUNE ||
           operation_id == XR_CORE_OP_CORE_STRING_FROM_SCALAR ||
           operation_id == XR_CORE_OP_CORE_STRING_CONCAT ||
           operation_id == XR_CORE_OP_CORE_COMPARE_STRING ||
           operation_id == XR_CORE_OP_CORE_COMPARE_RUNE ||
           operation_id == XR_CORE_OP_CORE_OUTPUT_GROUP;
}

/* Evaluates the string/rune/output family against the shared text kernel.
 * Values are read through the operand table; the produced value, when the
 * operation has one, is written to *produced. */
static TextOperationStatus evaluate_text_operation(EvalContext *context,
                                                   const XrValidatedInstruction *instruction,
                                                   const EvalRuntimeValue *values,
                                                   XrReferenceValue *produced) {
    switch (instruction->operation_id) {
        case XR_CORE_OP_CORE_CONSTANT_STRING: {
            const XrValidatedConstant *constant =
                &context->program->constants[instruction->immediate.constant_id];
            XrReferenceStringValue *string = allocate_string(context, constant->value.string.size);
            if (!string)
                return TEXT_OPERATION_RESOURCE_LIMIT;
            if (constant->value.string.size != 0u)
                memcpy(string->bytes, constant->value.string.bytes, constant->value.string.size);
            *produced = string_value(string);
            return TEXT_OPERATION_OK;
        }
        case XR_CORE_OP_CORE_CONSTANT_RUNE: {
            const XrValidatedConstant *constant =
                &context->program->constants[instruction->immediate.constant_id];
            produced->kind = XR_REFERENCE_VALUE_RUNE;
            produced->as.rune = constant->value.rune;
            return TEXT_OPERATION_OK;
        }
        case XR_CORE_OP_CORE_STRING_FROM_SCALAR: {
            if (values[instruction->operands[0]].as.value.kind != XR_REFERENCE_VALUE_I64)
                return TEXT_OPERATION_INVALID;
            int64_t source = values[instruction->operands[0]].as.value.as.i64;
            size_t size = xr_text_display_i64(source, NULL);
            XrReferenceStringValue *string = allocate_string(context, size);
            if (!string)
                return TEXT_OPERATION_RESOURCE_LIMIT;
            (void) xr_text_display_i64(source, string->bytes);
            *produced = string_value(string);
            return TEXT_OPERATION_OK;
        }
        case XR_CORE_OP_CORE_STRING_CONCAT: {
            const uint8_t *left = NULL;
            const uint8_t *right = NULL;
            size_t left_size = 0u;
            size_t right_size = 0u;
            int ok = 0;
            if (!string_view(values[instruction->operands[0]].as.value, &left, &left_size) ||
                !string_view(values[instruction->operands[1]].as.value, &right, &right_size))
                return TEXT_OPERATION_INVALID;
            size_t size = xr_text_concat_size(left_size, right_size, &ok);
            if (!ok)
                return TEXT_OPERATION_RESOURCE_LIMIT;
            XrReferenceStringValue *string = allocate_string(context, size);
            if (!string)
                return TEXT_OPERATION_RESOURCE_LIMIT;
            xr_text_concat(left, left_size, right, right_size, string->bytes);
            *produced = string_value(string);
            return TEXT_OPERATION_OK;
        }
        case XR_CORE_OP_CORE_COMPARE_STRING: {
            const uint8_t *left = NULL;
            const uint8_t *right = NULL;
            size_t left_size = 0u;
            size_t right_size = 0u;
            if (!string_view(values[instruction->operands[0]].as.value, &left, &left_size) ||
                !string_view(values[instruction->operands[1]].as.value, &right, &right_size))
                return TEXT_OPERATION_INVALID;
            produced->kind = XR_REFERENCE_VALUE_BOOL;
            produced->as.boolean =
                xr_text_predicate(xr_text_compare(left, left_size, right, right_size),
                                  instruction->immediate.u32) != 0;
            return TEXT_OPERATION_OK;
        }
        case XR_CORE_OP_CORE_COMPARE_RUNE: {
            uint32_t left = values[instruction->operands[0]].as.value.as.rune;
            uint32_t right = values[instruction->operands[1]].as.value.as.rune;
            int order = left == right ? 0 : (left < right ? -1 : 1);
            produced->kind = XR_REFERENCE_VALUE_BOOL;
            produced->as.boolean = xr_text_predicate(order, instruction->immediate.u32) != 0;
            return TEXT_OPERATION_OK;
        }
        case XR_CORE_OP_CORE_OUTPUT_GROUP: {
            XrTextDisplayOperand stack_operands[8];
            XrTextDisplayOperand *operands = stack_operands;
            size_t count = instruction->operand_count;
            int ok = 0;
            if (count > sizeof(stack_operands) / sizeof(stack_operands[0])) {
                operands = xr_calloc(count, sizeof(*operands));
                if (!operands)
                    return TEXT_OPERATION_RESOURCE_LIMIT;
            }
            for (size_t index = 0u; index < count; ++index) {
                const XrReferenceValue *value = &values[instruction->operands[index]].as.value;
                XrTextDisplayOperand *operand = &operands[index];
                memset(operand, 0, sizeof(*operand));
                switch (value->kind) {
                    case XR_REFERENCE_VALUE_I64:
                        operand->kind = XR_TEXT_DISPLAY_I64;
                        operand->i64 = value->as.i64;
                        break;
                    case XR_REFERENCE_VALUE_BOOL:
                        operand->kind = XR_TEXT_DISPLAY_BOOL;
                        operand->boolean = value->as.boolean ? 1 : 0;
                        break;
                    case XR_REFERENCE_VALUE_RUNE:
                        operand->kind = XR_TEXT_DISPLAY_RUNE;
                        operand->rune = value->as.rune;
                        break;
                    case XR_REFERENCE_VALUE_STRING: {
                        const uint8_t *bytes = NULL;
                        size_t size = 0u;
                        if (!string_view(*value, &bytes, &size)) {
                            if (operands != stack_operands)
                                xr_free(operands);
                            return TEXT_OPERATION_INVALID;
                        }
                        operand->kind = XR_TEXT_DISPLAY_STRING;
                        operand->bytes = bytes;
                        operand->size = size;
                        break;
                    }
                    default:
                        if (operands != stack_operands)
                            xr_free(operands);
                        return TEXT_OPERATION_INVALID;
                }
            }
            size_t size = xr_text_group_size(operands, count, &ok);
            uint8_t *line = ok ? xr_malloc(size) : NULL;
            if (!line) {
                if (operands != stack_operands)
                    xr_free(operands);
                return TEXT_OPERATION_RESOURCE_LIMIT;
            }
            (void) xr_text_group_render(operands, count, line);
            bool written =
                context->providers && context->providers->output_write &&
                context->providers->output_write(
                    context->providers->context,
                    instruction->immediate.provider_operation.requirement_index,
                    instruction->immediate.provider_operation.operation_index, line, size);
            xr_free(line);
            if (operands != stack_operands)
                xr_free(operands);
            return written ? TEXT_OPERATION_OK : TEXT_OPERATION_PROVIDER_FAILED;
        }
        default:
            return TEXT_OPERATION_INVALID;
    }
}

static XrReferenceOutcome text_operation_outcome(EvalContext *context, TextOperationStatus status) {
    switch (status) {
        case TEXT_OPERATION_RESOURCE_LIMIT:
            return outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
        case TEXT_OPERATION_PROVIDER_FAILED:
            return trap_outcome(context, XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED);
        default:
            return outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
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
    } else {
        if ((right > 0 && left < INT64_MIN / right) || (right < 0 && left < INT64_MAX / right))
            return false;
    }
    *result = left * right;
    return true;
}

static XrReferenceOutcome reference_provider_call(EvalContext *context,
                                                  const XrValidatedFunction *function,
                                                  const XrValidatedInstruction *instruction,
                                                  const EvalRuntimeValue *values,
                                                  uint32_t operand_count) {
    uint16_t operand_type =
        operand_count == 1u ? function->value_types[instruction->operands[0]] : XR_CORE_TYPE_VOID;
    XrProviderLogicalCallKind call_kind = xr_validated_program_provider_call_kind(
        context->program, instruction->result_type_id, operand_count == 1u ? &operand_type : NULL,
        operand_count);
    XrReferenceOutcome result = outcome(XR_REFERENCE_OUTCOME_RETURN, context);
    bool call_ok = false;
    if (call_kind == XR_PROVIDER_LOGICAL_CALL_I64_UNARY ||
        call_kind == XR_PROVIDER_LOGICAL_CALL_I64_NULLARY) {
        int64_t provider_result = 0;
        call_ok = context->providers &&
                  (call_kind == XR_PROVIDER_LOGICAL_CALL_I64_UNARY
                       ? context->providers->call_i64_unary &&
                             context->providers->call_i64_unary(
                                 context->providers->context,
                                 instruction->immediate.provider_operation.requirement_index,
                                 instruction->immediate.provider_operation.operation_index,
                                 values[instruction->operands[0]].as.value.as.i64, &provider_result)
                       : context->providers->call_i64_nullary &&
                             context->providers->call_i64_nullary(
                                 context->providers->context,
                                 instruction->immediate.provider_operation.requirement_index,
                                 instruction->immediate.provider_operation.operation_index,
                                 &provider_result));
        if (call_ok) {
            result.value.kind = XR_REFERENCE_VALUE_I64;
            result.value.as.i64 = provider_result;
        }
    } else if (call_kind == XR_PROVIDER_LOGICAL_CALL_BOOL_I64_UNARY) {
        bool provider_result = false;
        call_ok = context->providers && context->providers->call_bool_i64_unary &&
                  context->providers->call_bool_i64_unary(
                      context->providers->context,
                      instruction->immediate.provider_operation.requirement_index,
                      instruction->immediate.provider_operation.operation_index,
                      values[instruction->operands[0]].as.value.as.i64, &provider_result);
        if (call_ok) {
            result.value.kind = XR_REFERENCE_VALUE_BOOL;
            result.value.as.boolean = provider_result;
        }
    } else if (call_kind == XR_PROVIDER_LOGICAL_CALL_OPTIONAL_I64_PAIR_NULLARY) {
        uint16_t pair_type_id = XR_CORE_TYPE_VOID;
        (void) xr_validated_program_type_is_optional_i64_pair(
            context->program, instruction->result_type_id, &pair_type_id);
        XrReferenceAggregateValue *pair = allocate_aggregate(context, pair_type_id, UINT32_MAX, 2u);
        XrReferenceAggregateValue *optional =
            allocate_aggregate(context, instruction->result_type_id, 1u, 1u);
        if (!pair || !optional)
            return outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
        bool present = false;
        int64_t first = 0;
        int64_t second = 0;
        call_ok = context->providers && context->providers->call_optional_i64_pair_nullary &&
                  context->providers->call_optional_i64_pair_nullary(
                      context->providers->context,
                      instruction->immediate.provider_operation.requirement_index,
                      instruction->immediate.provider_operation.operation_index, &present, &first,
                      &second);
        if (call_ok) {
            if (present) {
                pair->fields[0] =
                    (XrReferenceValue) {.kind = XR_REFERENCE_VALUE_I64, .as.i64 = first};
                pair->fields[1] =
                    (XrReferenceValue) {.kind = XR_REFERENCE_VALUE_I64, .as.i64 = second};
                optional->fields[0] =
                    (XrReferenceValue) {.kind = XR_REFERENCE_VALUE_AGGREGATE, .as.aggregate = pair};
            } else {
                optional->variant_ordinal = 0u;
                optional->field_count = 0u;
            }
            result.value.kind = XR_REFERENCE_VALUE_AGGREGATE;
            result.value.as.aggregate = optional;
        }
    }
    return call_ok ? result : trap_outcome(context, XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED);
}

static XrReferenceOutcome evaluate_function(EvalContext *context, uint32_t function_id,
                                            const EvalRuntimeValue *arguments,
                                            uint32_t argument_count, uint32_t depth) {
    if (depth > context->budget.max_call_depth)
        return outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
    const XrValidatedFunction *function = &context->program->functions[function_id];
    if (argument_count != function->parameter_count)
        return outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
    for (uint32_t index = 0; index < argument_count; ++index) {
        XrCoreIrValueCategory expected =
            function_parameter_category(context->program, function, index);
        if (arguments[index].category != expected)
            return outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
        if (expected == XR_CORE_IR_PLACE &&
            (!arguments[index].as.place || !arguments[index].as.place->initialized))
            return outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
        XrReferenceValue value = expected == XR_CORE_IR_PLACE && arguments[index].as.place
                                     ? *eval_place_value_const(arguments[index].as.place)
                                     : arguments[index].as.value;
        if (!reference_value_matches_type(context->program, value,
                                          function->parameter_types[index]))
            return outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
    }

    EvalRuntimeValue *values =
        xr_calloc(function->value_count ? function->value_count : 1u, sizeof(EvalRuntimeValue));
    EvalPlace *places =
        xr_calloc(function->value_count ? function->value_count : 1u, sizeof(EvalPlace));
    bool *initialized = xr_calloc(function->value_count ? function->value_count : 1u, sizeof(bool));
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
    EvalRuntimeValue *scratch =
        xr_calloc(scratch_count ? scratch_count : 1u, sizeof(EvalRuntimeValue));
    if (!values || !places || !initialized || !scratch) {
        xr_free(scratch);
        xr_free(initialized);
        xr_free(places);
        xr_free(values);
        return outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
    }

    uint32_t block_id = function->entry_block;
    uint32_t incoming_count = argument_count;
    if (incoming_count != 0)
        memcpy(scratch, arguments, (size_t) incoming_count * sizeof(EvalRuntimeValue));
    XrReferenceOutcome result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
    for (;;) {
        const XrValidatedBlock *block = &function->blocks[block_id];
        if (incoming_count != block->argument_count) {
            result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
            break;
        }
        for (uint32_t argument = 0; argument < block->argument_count; ++argument) {
            uint32_t value_id = block->argument_ids[argument];
            values[value_id] = scratch[argument];
            initialized[value_id] = true;
        }
        bool transferred = false;
        for (uint32_t instruction_id = 0; instruction_id < block->instruction_count;
             ++instruction_id) {
            const XrValidatedInstruction *instruction = &block->instructions[instruction_id];
            if (context->steps == context->budget.max_steps) {
                result = outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
                goto done;
            }
            ++context->steps;
            for (uint32_t operand = 0; operand < instruction->operand_count; ++operand) {
                if (!initialized[instruction->operands[operand]]) {
                    result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
                    goto done;
                }
            }
            EvalRuntimeValue produced = {
                .category = XR_CORE_IR_VALUE,
                .as.value = void_value(),
            };
            bool has_result = instruction->result_id != XR_PROGRAM_LOCATION_NONE;
            switch (instruction->operation_id) {
                case XR_CORE_OP_CORE_CONSTANT_I64: {
                    const XrValidatedConstant *constant =
                        &context->program->constants[instruction->immediate.constant_id];
                    produced.as.value.kind = XR_REFERENCE_VALUE_I64;
                    produced.as.value.as.i64 = constant->value.i64;
                    break;
                }
                case XR_CORE_OP_CORE_CONSTANT_BOOL: {
                    const XrValidatedConstant *constant =
                        &context->program->constants[instruction->immediate.constant_id];
                    produced.as.value.kind = XR_REFERENCE_VALUE_BOOL;
                    produced.as.value.as.boolean = constant->value.boolean;
                    break;
                }
                case XR_CORE_OP_CORE_CONSTANT_TARGET_ENUM:
                    produced.as.value.kind =
                        instruction->result_type_id == XR_CORE_TYPE_TARGET_OS
                            ? XR_REFERENCE_VALUE_TARGET_OS
                        : instruction->result_type_id == XR_CORE_TYPE_TARGET_ARCH
                            ? XR_REFERENCE_VALUE_TARGET_ARCH
                        : instruction->result_type_id == XR_CORE_TYPE_TARGET_ABI
                            ? XR_REFERENCE_VALUE_TARGET_ABI
                            : XR_REFERENCE_VALUE_TARGET_ENDIAN;
                    produced.as.value.as.target_enum = (uint16_t) instruction->immediate.u32;
                    break;
                case XR_CORE_OP_CORE_ADD_I64:
                case XR_CORE_OP_CORE_SUB_I64:
                case XR_CORE_OP_CORE_MUL_I64: {
                    int64_t left = values[instruction->operands[0]].as.value.as.i64;
                    int64_t right = values[instruction->operands[1]].as.value.as.i64;
                    int64_t exact = 0;
                    bool valid = instruction->operation_id == XR_CORE_OP_CORE_ADD_I64
                                     ? checked_add(left, right, &exact)
                                 : instruction->operation_id == XR_CORE_OP_CORE_SUB_I64
                                     ? checked_sub(left, right, &exact)
                                     : checked_mul(left, right, &exact);
                    if (instruction->immediate.u32 == 0u && !valid) {
                        result = trap_outcome(context, XR_REFERENCE_TRAP_INTEGER_OVERFLOW);
                        goto done;
                    }
                    if (instruction->immediate.u32 != 0u) {
                        uint64_t bits = instruction->operation_id == XR_CORE_OP_CORE_ADD_I64
                                            ? (uint64_t) left + (uint64_t) right
                                        : instruction->operation_id == XR_CORE_OP_CORE_SUB_I64
                                            ? (uint64_t) left - (uint64_t) right
                                            : (uint64_t) left * (uint64_t) right;
                        exact = i64_from_bits(bits);
                    }
                    produced.as.value.kind = XR_REFERENCE_VALUE_I64;
                    produced.as.value.as.i64 = exact;
                    break;
                }
                case XR_CORE_OP_CORE_DIV_I64: {
                    int64_t left = values[instruction->operands[0]].as.value.as.i64;
                    int64_t right = values[instruction->operands[1]].as.value.as.i64;
                    if (right == 0) {
                        result = trap_outcome(context, XR_REFERENCE_TRAP_INTEGER_DIVISION_BY_ZERO);
                        goto done;
                    }
                    if (left == INT64_MIN && right == -1) {
                        result = trap_outcome(context, XR_REFERENCE_TRAP_INTEGER_DIVISION_OVERFLOW);
                        goto done;
                    }
                    produced.as.value.kind = XR_REFERENCE_VALUE_I64;
                    produced.as.value.as.i64 = left / right;
                    break;
                }
                case XR_CORE_OP_CORE_LOGICAL_NOT:
                    produced.as.value.kind = XR_REFERENCE_VALUE_BOOL;
                    produced.as.value.as.boolean =
                        !values[instruction->operands[0]].as.value.as.boolean;
                    break;
                case XR_CORE_OP_CORE_LOGICAL_AND:
                case XR_CORE_OP_CORE_LOGICAL_OR: {
                    bool left = values[instruction->operands[0]].as.value.as.boolean;
                    bool right = values[instruction->operands[1]].as.value.as.boolean;
                    produced.as.value.kind = XR_REFERENCE_VALUE_BOOL;
                    produced.as.value.as.boolean =
                        instruction->operation_id == XR_CORE_OP_CORE_LOGICAL_AND ? left && right
                                                                                 : left || right;
                    break;
                }
                case XR_CORE_OP_CORE_COMPARE_I64: {
                    int64_t left = values[instruction->operands[0]].as.value.as.i64;
                    int64_t right = values[instruction->operands[1]].as.value.as.i64;
                    bool comparison = false;
                    switch (instruction->immediate.u32) {
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
                            result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
                            goto done;
                    }
                    produced.as.value.kind = XR_REFERENCE_VALUE_BOOL;
                    produced.as.value.as.boolean = comparison;
                    break;
                }
                case XR_CORE_OP_CORE_COMPARE_TARGET_ENUM: {
                    uint16_t left = values[instruction->operands[0]].as.value.as.target_enum;
                    uint16_t right = values[instruction->operands[1]].as.value.as.target_enum;
                    produced.as.value.kind = XR_REFERENCE_VALUE_BOOL;
                    produced.as.value.as.boolean =
                        instruction->immediate.u32 == 0u ? left == right : left != right;
                    break;
                }
                case XR_CORE_OP_CORE_BLOCK_ARGUMENT:
                    break;
                case XR_CORE_OP_CORE_BRANCH: {
                    const XrValidatedBlock *target = &function->blocks[instruction->successors[0]];
                    for (uint32_t index = 0; index < target->argument_count; ++index)
                        scratch[index] = values[instruction->operands[index]];
                    incoming_count = target->argument_count;
                    block_id = instruction->successors[0];
                    transferred = true;
                    break;
                }
                case XR_CORE_OP_CORE_CONDITIONAL_BRANCH: {
                    bool condition = values[instruction->operands[0]].as.value.as.boolean;
                    uint32_t successor = condition ? 0u : 1u;
                    uint32_t operand =
                        condition
                            ? 1u
                            : 1u + function->blocks[instruction->successors[0]].argument_count;
                    const XrValidatedBlock *target =
                        &function->blocks[instruction->successors[successor]];
                    for (uint32_t index = 0; index < target->argument_count; ++index)
                        scratch[index] = values[instruction->operands[operand + index]];
                    incoming_count = target->argument_count;
                    block_id = instruction->successors[successor];
                    transferred = true;
                    break;
                }
                case XR_CORE_OP_CORE_ASSERT_CONDITION:
                    /* The historical reference subset has scalar panic tokens only. */
                    if (instruction->immediate.u32 & XR_CORE_ASSERT_MESSAGE_PRESENT) {
                        result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
                        goto done;
                    }
                    if (!values[instruction->operands[0]].as.value.as.boolean) {
                        XrReferenceValue panic = {
                            .kind = XR_REFERENCE_VALUE_PANIC_INFO,
                            .as.panic_info = instruction->immediate.u32,
                        };
                        if (instruction->successor_count == 0u) {
                            result = outcome(XR_REFERENCE_OUTCOME_PANIC, context);
                            result.panic_value = panic;
                            goto done;
                        }
                        const XrValidatedBlock *target =
                            &function->blocks[instruction->successors[0]];
                        scratch[0] = (EvalRuntimeValue) {
                            .category = XR_CORE_IR_VALUE,
                            .as.value = panic,
                        };
                        for (uint32_t index = 1u; index < target->argument_count; ++index)
                            scratch[index] = values[instruction->operands[index]];
                        incoming_count = target->argument_count;
                        block_id = instruction->successors[0];
                        transferred = true;
                    }
                    break;
                case XR_CORE_OP_CORE_RETURN:
                    result = outcome(XR_REFERENCE_OUTCOME_RETURN, context);
                    result.value = instruction->operand_count == 0
                                       ? void_value()
                                       : values[instruction->operands[0]].as.value;
                    goto done;
                case XR_CORE_OP_CORE_CALL_SEALED_DIRECT:
                case XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT:
                case XR_CORE_OP_CORE_CALL_WITNESS_DIRECT: {
                    uint32_t target_function =
                        instruction->operation_id == XR_CORE_OP_CORE_CALL_SEALED_DIRECT
                            ? instruction->immediate.function_id
                            : XR_PROGRAM_LOCATION_NONE;
                    uint32_t source_argument = 0u;
                    uint32_t target_argument = 0u;
                    if (instruction->operation_id == XR_CORE_OP_CORE_CALL_WITNESS_DIRECT) {
                        const XrReferenceExistentialValue *carrier =
                            values[instruction->operands[0]].as.value.as.existential;
                        target_function = witness_function_id(context->program, carrier,
                                                              instruction->immediate.u32);
                        if (target_function == XR_PROGRAM_LOCATION_NONE) {
                            result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
                            goto done;
                        }
                        if (!witness_receiver_argument(
                                carrier, context->program->functions[target_function].receiver_mode,
                                &scratch[0])) {
                            result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
                            goto done;
                        }
                        source_argument = 1u;
                        target_argument = 1u;
                    } else if (instruction->operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT) {
                        const XrReferenceCallableValue *carrier =
                            values[instruction->operands[0]].as.value.as.callable;
                        target_function = callable_function_id(context->program, carrier);
                        if (target_function == XR_PROGRAM_LOCATION_NONE) {
                            result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
                            goto done;
                        }
                        source_argument = 1u;
                        if (carrier->has_capture) {
                            scratch[0] = (EvalRuntimeValue) {
                                .category = XR_CORE_IR_VALUE,
                                .as.value = carrier->capture,
                            };
                            target_argument = 1u;
                        }
                    }
                    uint32_t call_operand_count = instruction->operand_count;
                    if (instruction->successor_count == 1u)
                        call_operand_count -=
                            function->blocks[instruction->successors[0]].argument_count;
                    for (; source_argument < call_operand_count;
                         ++source_argument, ++target_argument)
                        scratch[target_argument] = values[instruction->operands[source_argument]];
                    const XrValidatedFunction *callee =
                        &context->program->functions[target_function];
                    XrReferenceOutcome nested = evaluate_function(
                        context, target_function, scratch, callee->parameter_count, depth + 1u);
                    if (nested.kind != XR_REFERENCE_OUTCOME_RETURN) {
                        if (nested.kind == XR_REFERENCE_OUTCOME_TRAP &&
                            nested.trap == XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED &&
                            instruction->successor_count == 1u) {
                            const XrValidatedBlock *target =
                                &function->blocks[instruction->successors[0]];
                            for (uint32_t index = 0u; index < target->argument_count; ++index)
                                scratch[index] =
                                    values[instruction->operands[call_operand_count + index]];
                            incoming_count = target->argument_count;
                            block_id = instruction->successors[0];
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
                        instruction->operation_id == XR_CORE_OP_CORE_CALL_SEALED_INVOKE
                            ? instruction->immediate.function_id
                            : XR_PROGRAM_LOCATION_NONE;
                    uint32_t source_argument = 0u;
                    uint32_t target_argument = 0u;
                    if (instruction->operation_id == XR_CORE_OP_CORE_CALL_WITNESS_INVOKE) {
                        const XrReferenceExistentialValue *carrier =
                            values[instruction->operands[0]].as.value.as.existential;
                        target_function = witness_function_id(context->program, carrier,
                                                              instruction->immediate.u32);
                        if (target_function == XR_PROGRAM_LOCATION_NONE) {
                            result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
                            goto done;
                        }
                        if (!witness_receiver_argument(
                                carrier, context->program->functions[target_function].receiver_mode,
                                &scratch[0])) {
                            result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
                            goto done;
                        }
                        source_argument = 1u;
                        target_argument = 1u;
                    } else if (instruction->operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE) {
                        const XrReferenceCallableValue *carrier =
                            values[instruction->operands[0]].as.value.as.callable;
                        target_function = callable_function_id(context->program, carrier);
                        if (target_function == XR_PROGRAM_LOCATION_NONE) {
                            result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
                            goto done;
                        }
                        source_argument = 1u;
                        if (carrier->has_capture) {
                            scratch[0] = (EvalRuntimeValue) {
                                .category = XR_CORE_IR_VALUE,
                                .as.value = carrier->capture,
                            };
                            target_argument = 1u;
                        }
                    }
                    const XrValidatedFunction *callee =
                        &context->program->functions[target_function];
                    for (; target_argument < callee->parameter_count;
                         ++source_argument, ++target_argument)
                        scratch[target_argument] = values[instruction->operands[source_argument]];
                    XrReferenceOutcome nested = evaluate_function(
                        context, target_function, scratch, callee->parameter_count, depth + 1u);
                    uint32_t successor = 0u;
                    uint32_t implicit = 0u;
                    uint32_t operand = callee->parameter_count;
                    uint32_t typed_successors =
                        1u + (callee->error_type_id == XR_CORE_TYPE_VOID ? 0u : 1u) +
                        (callee->panic_type_id == XR_CORE_TYPE_VOID ? 0u : 1u);
                    if (instruction->operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE &&
                        !callee->has_receiver)
                        ++operand;
                    if (nested.kind == XR_REFERENCE_OUTCOME_RETURN) {
                        if (callee->result_type_id != XR_CORE_TYPE_VOID) {
                            scratch[0] = (EvalRuntimeValue) {
                                .category = XR_CORE_IR_VALUE,
                                .as.value = nested.value,
                            };
                            implicit = 1u;
                        }
                    } else if (nested.kind == XR_REFERENCE_OUTCOME_ERROR) {
                        successor = 1u;
                        operand += function->blocks[instruction->successors[0]].argument_count -
                                   (callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u);
                        scratch[0] = (EvalRuntimeValue) {
                            .category = XR_CORE_IR_VALUE,
                            .as.value = nested.error_value,
                        };
                        implicit = 1u;
                    } else if (nested.kind == XR_REFERENCE_OUTCOME_PANIC) {
                        successor = 1u + (callee->error_type_id == XR_CORE_TYPE_VOID ? 0u : 1u);
                        operand += function->blocks[instruction->successors[0]].argument_count -
                                   (callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u);
                        if (callee->error_type_id != XR_CORE_TYPE_VOID)
                            operand +=
                                function->blocks[instruction->successors[1]].argument_count - 1u;
                        scratch[0] = (EvalRuntimeValue) {
                            .category = XR_CORE_IR_VALUE,
                            .as.value = nested.panic_value,
                        };
                        implicit = 1u;
                    } else if ((instruction->operation_id == XR_CORE_OP_CORE_CALL_WITNESS_INVOKE ||
                                instruction->operation_id == XR_CORE_OP_CORE_CALL_SEALED_INVOKE ||
                                instruction->operation_id ==
                                    XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE) &&
                               nested.kind == XR_REFERENCE_OUTCOME_TRAP &&
                               nested.trap == XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED &&
                               instruction->successor_count == typed_successors + 1u) {
                        successor = typed_successors;
                        for (uint32_t prior = 0u; prior < typed_successors; ++prior) {
                            uint32_t prior_implicit =
                                prior == 0u
                                    ? (callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u)
                                    : 1u;
                            const XrValidatedBlock *prior_target =
                                &function->blocks[instruction->successors[prior]];
                            operand += prior_target->argument_count - prior_implicit;
                        }
                    } else {
                        result = nested;
                        goto done;
                    }
                    const XrValidatedBlock *target =
                        &function->blocks[instruction->successors[successor]];
                    for (uint32_t index = implicit; index < target->argument_count; ++index)
                        scratch[index] = values[instruction->operands[operand + index - implicit]];
                    incoming_count = target->argument_count;
                    block_id = instruction->successors[successor];
                    transferred = true;
                    break;
                }
                case XR_CORE_OP_CORE_TRAP:
                    result = trap_outcome(context, instruction->immediate.u32 == 7u
                                                       ? XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED
                                                       : XR_REFERENCE_TRAP_EXPLICIT);
                    goto done;
                case XR_CORE_OP_CORE_ERROR_PUBLISH:
                    result = outcome(XR_REFERENCE_OUTCOME_ERROR, context);
                    result.error_value = values[instruction->operands[0]].as.value;
                    goto done;
                case XR_CORE_OP_CORE_PANIC_PUBLISH:
                    result = outcome(XR_REFERENCE_OUTCOME_PANIC, context);
                    result.panic_value = values[instruction->operands[0]].as.value;
                    goto done;
                case XR_CORE_OP_CORE_TARGET_POINTER_WIDTH:
                    if (context->profile.pointer_width != 32u &&
                        context->profile.pointer_width != 64u) {
                        result = trap_outcome(context, XR_REFERENCE_TRAP_PROFILE_UNAVAILABLE);
                        goto done;
                    }
                    produced.as.value.kind = XR_REFERENCE_VALUE_U16;
                    produced.as.value.as.u16 = context->profile.pointer_width;
                    break;
                case XR_CORE_OP_CORE_TARGET_OPERATING_SYSTEM:
                    if (context->profile.operating_system <= XR_TARGET_OS_NONE ||
                        context->profile.operating_system >= XR_TARGET_OS_COUNT) {
                        result = trap_outcome(context, XR_REFERENCE_TRAP_PROFILE_UNAVAILABLE);
                        goto done;
                    }
                    produced.as.value.kind = XR_REFERENCE_VALUE_TARGET_OS;
                    produced.as.value.as.target_enum = context->profile.operating_system;
                    break;
                case XR_CORE_OP_CORE_TARGET_ARCHITECTURE:
                    if (context->profile.architecture <= XR_TARGET_ARCH_NONE ||
                        context->profile.architecture >= XR_TARGET_ARCH_COUNT) {
                        result = trap_outcome(context, XR_REFERENCE_TRAP_PROFILE_UNAVAILABLE);
                        goto done;
                    }
                    produced.as.value.kind = XR_REFERENCE_VALUE_TARGET_ARCH;
                    produced.as.value.as.target_enum = context->profile.architecture;
                    break;
                case XR_CORE_OP_CORE_TARGET_NATIVE_ABI:
                    if (context->profile.native_abi <= XR_TARGET_ABI_NONE ||
                        context->profile.native_abi >= XR_TARGET_ABI_COUNT) {
                        result = trap_outcome(context, XR_REFERENCE_TRAP_PROFILE_UNAVAILABLE);
                        goto done;
                    }
                    produced.as.value.kind = XR_REFERENCE_VALUE_TARGET_ABI;
                    produced.as.value.as.target_enum = context->profile.native_abi;
                    break;
                case XR_CORE_OP_CORE_TARGET_ENDIANNESS:
                    if (context->profile.endianness != XR_TARGET_ENDIAN_LITTLE &&
                        context->profile.endianness != XR_TARGET_ENDIAN_BIG) {
                        result = trap_outcome(context, XR_REFERENCE_TRAP_PROFILE_UNAVAILABLE);
                        goto done;
                    }
                    produced.as.value.kind = XR_REFERENCE_VALUE_TARGET_ENDIAN;
                    produced.as.value.as.target_enum = context->profile.endianness;
                    break;
                case XR_CORE_OP_CORE_PROVIDER_CALL: {
                    bool has_trap_edge = instruction->successor_count != 0u;
                    const XrValidatedBlock *trap_target =
                        has_trap_edge ? &function->blocks[instruction->successors[0]] : NULL;
                    uint32_t provider_operand_count =
                        instruction->operand_count -
                        (trap_target ? trap_target->argument_count : 0u);
                    XrReferenceOutcome provider = reference_provider_call(
                        context, function, instruction, values, provider_operand_count);
                    if (provider.kind != XR_REFERENCE_OUTCOME_RETURN) {
                        if (provider.kind != XR_REFERENCE_OUTCOME_TRAP ||
                            provider.trap != XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED ||
                            !has_trap_edge) {
                            result = provider;
                            goto done;
                        }
                        for (uint32_t index = 0u; index < trap_target->argument_count; ++index)
                            scratch[index] =
                                values[instruction->operands[provider_operand_count + index]];
                        incoming_count = trap_target->argument_count;
                        block_id = instruction->successors[0];
                        transferred = true;
                    } else {
                        produced.as.value = provider.value;
                    }
                    break;
                }
                case XR_CORE_OP_CORE_CONSTANT_STRING:
                case XR_CORE_OP_CORE_CONSTANT_RUNE:
                case XR_CORE_OP_CORE_STRING_FROM_SCALAR:
                case XR_CORE_OP_CORE_STRING_CONCAT:
                case XR_CORE_OP_CORE_COMPARE_STRING:
                case XR_CORE_OP_CORE_COMPARE_RUNE:
                case XR_CORE_OP_CORE_OUTPUT_GROUP: {
                    TextOperationStatus text_status =
                        evaluate_text_operation(context, instruction, values, &produced.as.value);
                    if (text_status != TEXT_OPERATION_OK) {
                        result = text_operation_outcome(context, text_status);
                        goto done;
                    }
                    break;
                }
                case XR_CORE_OP_CORE_CALLABLE_PACK: {
                    XrReferenceCallableValue *carrier = allocate_callable(context);
                    if (!carrier) {
                        result = outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    carrier->callable_type_id = instruction->result_type_id;
                    carrier->function_id = instruction->immediate.function_id;
                    carrier->has_capture = instruction->operand_count != 0u;
                    if (carrier->has_capture) {
                        uint32_t capture_value = instruction->operands[0];
                        carrier->capture_type_id = function->value_types[capture_value];
                        carrier->capture = values[capture_value].as.value;
                    }
                    produced.as.value.kind = XR_REFERENCE_VALUE_CALLABLE;
                    produced.as.value.as.callable = carrier;
                    break;
                }
                case XR_CORE_OP_CORE_OWNER_COPY: {
                    CloneStatus clone_status = clone_reference_value(
                        context, values[instruction->operands[0]].as.value,
                        instruction->result_type_id, &produced.as.value);
                    if (clone_status != CLONE_STATUS_OK) {
                        result = outcome(clone_failure_outcome_kind(clone_status), context);
                        goto done;
                    }
                    break;
                }
                case XR_CORE_OP_CORE_OWNER_MOVE:
                    produced.as.value = values[instruction->operands[0]].as.value;
                    break;
                case XR_CORE_OP_CORE_OWNER_DROP: {
                    drop_reference_value(context,
                                         &values[instruction->operands[0]].as.value,
                                         XR_REFERENCE_EVENT_ORIGIN_PROGRAM_OPERATION, NULL);
                    break;
                }
                case XR_CORE_OP_CORE_CLASS_CONSTRUCT: {
                    XrReferenceClassValue *value =
                        allocate_class(context, instruction->result_type_id,
                                       instruction->operand_count);
                    if (!value) {
                        result = outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    for (uint32_t field = 0; field < instruction->operand_count; ++field)
                        value->fields[field] = values[instruction->operands[field]].as.value;
                    produced.as.value.kind = XR_REFERENCE_VALUE_CLASS_REFERENCE;
                    produced.as.value.as.class_reference = value;
                    emit_lifecycle(context, XR_REFERENCE_EVENT_CLASS_CONSTRUCT,
                                   XR_REFERENCE_EVENT_ORIGIN_PROGRAM_OPERATION, value, UINT64_MAX,
                                   UINT32_MAX);
                    break;
                }
                case XR_CORE_OP_CORE_OWNER_ALIAS: {
                    XrReferenceClassValue *value =
                        (XrReferenceClassValue *) (void *)
                            values[instruction->operands[0]].as.value.as.class_reference;
                    if (!value || !value->alive || value->owner_count == UINT32_MAX) {
                        result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
                        goto done;
                    }
                    ++value->owner_count;
                    produced.as.value.kind = XR_REFERENCE_VALUE_CLASS_REFERENCE;
                    produced.as.value.as.class_reference = value;
                    emit_lifecycle(context, XR_REFERENCE_EVENT_CLASS_SHARE,
                                   XR_REFERENCE_EVENT_ORIGIN_PROGRAM_OPERATION, value, value->identity,
                                   UINT32_MAX);
                    break;
                }
                case XR_CORE_OP_CORE_CLASS_FIELD_LOAD: {
                    XrReferenceClassValue *value =
                        (XrReferenceClassValue *) (void *)
                            values[instruction->operands[0]].as.value.as.class_reference;
                    if (!value || !value->alive ||
                        instruction->immediate.field_ordinal >= value->field_count) {
                        result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
                        goto done;
                    }
                    CloneStatus load_status = class_field_load_value(
                        context, value, instruction->immediate.field_ordinal,
                        instruction->result_type_id, &produced.as.value);
                    if (load_status != CLONE_STATUS_OK) {
                        result = outcome(clone_failure_outcome_kind(load_status), context);
                        goto done;
                    }
                    emit_lifecycle(context, XR_REFERENCE_EVENT_CLASS_FIELD_LOAD,
                                   XR_REFERENCE_EVENT_ORIGIN_PROGRAM_OPERATION, value, UINT64_MAX,
                                   instruction->immediate.field_ordinal);
                    break;
                }
                case XR_CORE_OP_CORE_CLASS_FIELD_PLACE: {
                    XrReferenceClassValue *value =
                        (XrReferenceClassValue *) (void *)
                            values[instruction->operands[0]].as.value.as.class_reference;
                    if (!value || !value->alive ||
                        instruction->immediate.field_ordinal >= value->field_count) {
                        result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
                        goto done;
                    }
                    places[instruction->result_id].alias =
                        &value->fields[instruction->immediate.field_ordinal];
                    places[instruction->result_id].initialized = true;
                    produced.category = XR_CORE_IR_PLACE;
                    produced.as.place = &places[instruction->result_id];
                    emit_lifecycle(context, XR_REFERENCE_EVENT_CLASS_FIELD_PLACE,
                                   XR_REFERENCE_EVENT_ORIGIN_PROGRAM_OPERATION, value, UINT64_MAX,
                                   instruction->immediate.field_ordinal);
                    break;
                }
                case XR_CORE_OP_CORE_PLACE_LOCAL:
                    places[instruction->result_id].alias =
                        &values[instruction->operands[0]].as.value;
                    places[instruction->result_id].initialized = true;
                    produced.category = XR_CORE_IR_PLACE;
                    produced.as.place = &places[instruction->result_id];
                    break;
                case XR_CORE_OP_CORE_PLACE_LOAD:
                    produced.as.value =
                        *eval_place_value(values[instruction->operands[0]].as.place);
                    break;
                case XR_CORE_OP_CORE_PLACE_STORE:
                    *eval_place_value(values[instruction->operands[0]].as.place) =
                        values[instruction->operands[1]].as.value;
                    break;
                case XR_CORE_OP_CORE_PLACE_PROJECT: {
                    XrReferenceValue *source =
                        eval_place_value(values[instruction->operands[0]].as.place);
                    XrReferenceAggregateValue *aggregate =
                        (XrReferenceAggregateValue *) (void *) source->as.aggregate;
                    places[instruction->result_id].alias =
                        &aggregate->fields[instruction->immediate.field_ordinal];
                    places[instruction->result_id].initialized = true;
                    produced.category = XR_CORE_IR_PLACE;
                    produced.as.place = &places[instruction->result_id];
                    break;
                }
                case XR_CORE_OP_CORE_PLACE_TAKE:
                    produced.as.value =
                        *eval_place_value(values[instruction->operands[0]].as.place);
                    values[instruction->operands[0]].as.place->initialized = false;
                    break;
                case XR_CORE_OP_CORE_PLACE_EXCHANGE: {
                    XrReferenceValue *place =
                        eval_place_value(values[instruction->operands[0]].as.place);
                    produced.as.value = *place;
                    XrReferenceValue replacement = values[instruction->operands[1]].as.value;
                    *place = replacement;
                    emit_place_exchange(context, instruction->result_type_id, produced.as.value,
                                        replacement);
                    break;
                }
                case XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT: {
                    XrReferenceAggregateValue *aggregate =
                        allocate_aggregate(context, instruction->result_type_id, UINT32_MAX,
                                           instruction->operand_count);
                    if (!aggregate) {
                        result = outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    for (uint32_t field = 0; field < instruction->operand_count; ++field)
                        aggregate->fields[field] = values[instruction->operands[field]].as.value;
                    produced.as.value.kind = XR_REFERENCE_VALUE_AGGREGATE;
                    produced.as.value.as.aggregate = aggregate;
                    break;
                }
                case XR_CORE_OP_CORE_AGGREGATE_PROJECT: {
                    const XrReferenceAggregateValue *aggregate =
                        values[instruction->operands[0]].as.value.as.aggregate;
                    produced.as.value = aggregate->fields[instruction->immediate.field_ordinal];
                    break;
                }
                case XR_CORE_OP_CORE_AGGREGATE_UPDATE: {
                    const XrReferenceAggregateValue *source =
                        values[instruction->operands[0]].as.value.as.aggregate;
                    XrReferenceAggregateValue *aggregate = allocate_aggregate(
                        context, instruction->result_type_id, UINT32_MAX, source->field_count);
                    if (!aggregate) {
                        result = outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    memcpy(aggregate->fields, source->fields,
                           (size_t) source->field_count * sizeof(XrReferenceValue));
                    aggregate->fields[instruction->immediate.field_ordinal] =
                        values[instruction->operands[1]].as.value;
                    produced.as.value.kind = XR_REFERENCE_VALUE_AGGREGATE;
                    produced.as.value.as.aggregate = aggregate;
                    break;
                }
                case XR_CORE_OP_CORE_VARIANT_CONSTRUCT: {
                    XrReferenceAggregateValue *aggregate = allocate_aggregate(
                        context, instruction->result_type_id,
                        instruction->immediate.variant_ordinal, instruction->operand_count);
                    if (!aggregate) {
                        result = outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    for (uint32_t field = 0; field < instruction->operand_count; ++field)
                        aggregate->fields[field] = values[instruction->operands[field]].as.value;
                    produced.as.value.kind = XR_REFERENCE_VALUE_AGGREGATE;
                    produced.as.value.as.aggregate = aggregate;
                    break;
                }
                case XR_CORE_OP_CORE_VARIANT_TEST: {
                    const XrReferenceAggregateValue *aggregate =
                        values[instruction->operands[0]].as.value.as.aggregate;
                    produced.as.value.kind = XR_REFERENCE_VALUE_BOOL;
                    produced.as.value.as.boolean =
                        aggregate->variant_ordinal == instruction->immediate.variant_ordinal;
                    break;
                }
                case XR_CORE_OP_CORE_VARIANT_PROJECT: {
                    const XrReferenceAggregateValue *aggregate =
                        values[instruction->operands[0]].as.value.as.aggregate;
                    if (aggregate->variant_ordinal !=
                        instruction->immediate.variant_field.variant_ordinal) {
                        result = trap_outcome(context, XR_REFERENCE_TRAP_VARIANT_TAG_MISMATCH);
                        goto done;
                    }
                    produced.as.value =
                        aggregate->fields[instruction->immediate.variant_field.field_ordinal];
                    break;
                }
                case XR_CORE_OP_CORE_EXISTENTIAL_PACK: {
                    const XrValidatedType *existential =
                        xr_validated_program_type(context->program, instruction->result_type_id);
                    uint16_t concrete_type = function->value_types[instruction->operands[0]];
                    XrReferenceExistentialValue *carrier = allocate_existential(context);
                    uint32_t conformance = existential
                                               ? conformance_id(context->program, concrete_type,
                                                                existential->interface_id)
                                               : XR_PROGRAM_LOCATION_NONE;
                    if (!carrier || conformance == XR_PROGRAM_LOCATION_NONE) {
                        result = outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    carrier->existential_type_id = instruction->result_type_id;
                    carrier->concrete_type_id = concrete_type;
                    carrier->conformance_id = conformance;
                    if (existential->interface_use_kind ==
                        XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE) {
                        carrier->owned_storage.value = values[instruction->operands[0]].as.value;
                        carrier->owned_storage.initialized = true;
                        carrier->payload.category = XR_CORE_IR_PLACE;
                        carrier->payload.as.place = &carrier->owned_storage;
                    } else {
                        carrier->payload = values[instruction->operands[0]];
                    }
                    produced.as.value.kind = XR_REFERENCE_VALUE_EXISTENTIAL;
                    produced.as.value.as.existential = carrier;
                    break;
                }
                case XR_CORE_OP_CORE_EXISTENTIAL_REBORROW_READ: {
                    const XrReferenceExistentialValue *source =
                        values[instruction->operands[0]].as.value.as.existential;
                    XrReferenceExistentialValue *carrier = allocate_existential(context);
                    if (!source || !carrier) {
                        result = outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    carrier->existential_type_id = instruction->result_type_id;
                    carrier->concrete_type_id = source->concrete_type_id;
                    carrier->conformance_id = source->conformance_id;
                    carrier->payload.category = XR_CORE_IR_VALUE;
                    carrier->payload.as.value =
                        source->payload.category == XR_CORE_IR_PLACE
                            ? *eval_place_value_const(source->payload.as.place)
                            : source->payload.as.value;
                    produced.as.value.kind = XR_REFERENCE_VALUE_EXISTENTIAL;
                    produced.as.value.as.existential = carrier;
                    break;
                }
                case XR_CORE_OP_CORE_EXISTENTIAL_TEST: {
                    const XrReferenceExistentialValue *carrier =
                        values[instruction->operands[0]].as.value.as.existential;
                    produced.as.value.kind = XR_REFERENCE_VALUE_BOOL;
                    produced.as.value.as.boolean =
                        carrier->concrete_type_id == instruction->immediate.type_id;
                    break;
                }
                case XR_CORE_OP_CORE_EXISTENTIAL_PROJECT: {
                    const XrReferenceExistentialValue *carrier =
                        values[instruction->operands[0]].as.value.as.existential;
                    const XrValidatedType *existential =
                        xr_validated_program_type(context->program, carrier->existential_type_id);
                    if (existential && existential->interface_use_kind ==
                                           XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE) {
                        produced.category = XR_CORE_IR_VALUE;
                        produced.as.value = *eval_place_value_const(&carrier->owned_storage);
                    } else {
                        produced = carrier->payload;
                    }
                    break;
                }
                default:
                    result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
                    goto done;
            }
            if (has_result && !transferred) {
                values[instruction->result_id] = produced;
                initialized[instruction->result_id] = true;
            }
            if (transferred)
                break;
        }
        if (!transferred) {
            result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
            break;
        }
    }

done:
    result.steps = context->steps;
    xr_free(scratch);
    xr_free(initialized);
    xr_free(places);
    xr_free(values);
    return result;
}

static bool reference_execution_provider_i64_unary(void *context, uint32_t requirement_index,
                                                   uint32_t operation_index, int64_t argument,
                                                   int64_t *result_out) {
    return xr_execution_lease_provider_call_i64_unary(context, requirement_index, operation_index,
                                                      argument,
                                                      result_out) == XR_EXECUTION_PROVIDER_CALL_OK;
}

static bool reference_execution_provider_i64_nullary(void *context, uint32_t requirement_index,
                                                     uint32_t operation_index,
                                                     int64_t *result_out) {
    return xr_execution_lease_provider_call_i64_nullary(context, requirement_index, operation_index,
                                                        result_out) ==
           XR_EXECUTION_PROVIDER_CALL_OK;
}

static bool reference_execution_provider_bool_i64_unary(void *context, uint32_t requirement_index,
                                                        uint32_t operation_index, int64_t argument,
                                                        bool *result_out) {
    return xr_execution_lease_provider_call_bool_i64_unary(context, requirement_index,
                                                           operation_index, argument, result_out) ==
           XR_EXECUTION_PROVIDER_CALL_OK;
}

static bool
reference_execution_provider_optional_i64_pair_nullary(void *context, uint32_t requirement_index,
                                                       uint32_t operation_index, bool *present_out,
                                                       int64_t *first_out, int64_t *second_out) {
    return xr_execution_lease_provider_call_optional_i64_pair_nullary(
               context, requirement_index, operation_index, present_out, first_out, second_out) ==
           XR_EXECUTION_PROVIDER_CALL_OK;
}

static bool reference_execution_provider_output_write(void *context, uint32_t requirement_index,
                                                      uint32_t operation_index,
                                                      const uint8_t *bytes, size_t size) {
    return xr_execution_lease_provider_output_write(context, requirement_index, operation_index,
                                                    bytes, size) == XR_EXECUTION_PROVIDER_CALL_OK;
}

static XrReferenceOutcome execution_outcome(XrReferenceExecution *execution,
                                            XrReferenceOutcomeKind kind) {
    XrReferenceOutcome result = {.kind = kind};
    if (execution) {
        result.steps = execution->context ? execution->context->steps : 0u;
        result.state_id = execution->state_id;
    }
    return result;
}

static bool reference_coroutine_operation_supported(uint16_t operation_id) {
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
           operation_id == XR_CORE_OP_CORE_OWNER_COPY ||
           operation_id == XR_CORE_OP_CORE_OWNER_MOVE ||
           operation_id == XR_CORE_OP_CORE_CLASS_CONSTRUCT ||
           operation_id == XR_CORE_OP_CORE_OWNER_ALIAS ||
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
           operation_id == XR_CORE_OP_CORE_RETURN || operation_is_text(operation_id);
}

static void reference_execution_release_lease(XrReferenceExecution *execution) {
    if (execution && execution->owns_lease && xr_execution_lease_is_valid(&execution->lease))
        (void) xr_execution_lease_release(&execution->lease);
}

static bool reference_execution_allocate_values(XrReferenceExecution *execution,
                                                const XrValidatedFunction *function) {
    uint32_t scratch_count = 0u;
    for (uint32_t block = 0u; block < function->block_count; ++block)
        if (function->blocks[block].argument_count > scratch_count)
            scratch_count = function->blocks[block].argument_count;
    if (scratch_count > function->value_count)
        return false;
    size_t value_count = function->value_count ? function->value_count : 1u;
    size_t scratch_capacity = scratch_count ? scratch_count : 1u;
    if (value_count > SIZE_MAX / sizeof(*execution->values) ||
        value_count > SIZE_MAX / sizeof(*execution->places) ||
        scratch_capacity > SIZE_MAX / sizeof(*execution->edge_scratch))
        return false;
    execution->values = xr_calloc(value_count, sizeof(*execution->values));
    execution->places = xr_calloc(value_count, sizeof(*execution->places));
    execution->initialized = xr_calloc(value_count, sizeof(*execution->initialized));
    execution->edge_scratch = xr_calloc(scratch_capacity, sizeof(*execution->edge_scratch));
    execution->edge_scratch_count = scratch_count;
    return execution->values && execution->places && execution->initialized &&
           execution->edge_scratch;
}

/* A successor transfer is a simultaneous assignment. Snapshot every selected
 * source before
 * publishing any target, including a child's implicit result:
 * loop block arguments may permute
 * the same SSA slots they read. Scratch is
 * bounded and owned by this execution, independently of
 * its parent or child. */
static bool reference_execution_transfer_edge(XrReferenceExecution *execution,
                                              const XrValidatedInstruction *instruction,
                                              uint32_t successor_index, uint32_t operand_start,
                                              const XrReferenceValue *implicit_result) {
    const XrValidatedFunction *function =
        &execution->context->program->functions[execution->function_id];
    if (successor_index >= instruction->successor_count ||
        instruction->successors[successor_index] >= function->block_count)
        return false;
    const XrValidatedBlock *target = &function->blocks[instruction->successors[successor_index]];
    uint32_t implicit_count = implicit_result ? 1u : 0u;
    if (target->argument_count < implicit_count ||
        target->argument_count > execution->edge_scratch_count ||
        operand_start > instruction->operand_count ||
        target->argument_count - implicit_count > instruction->operand_count - operand_start)
        return false;
    if (implicit_result)
        execution->edge_scratch[0] = (EvalRuntimeValue) {
            .category = XR_CORE_IR_VALUE,
            .as.value = *implicit_result,
        };
    for (uint32_t argument = 0u; argument < target->argument_count; ++argument) {
        if (target->argument_ids[argument] >= function->value_count)
            return false;
        if (argument < implicit_count)
            continue;
        uint32_t source = instruction->operands[operand_start + argument - implicit_count];
        if (source >= function->value_count || !execution->initialized[source])
            return false;
        execution->edge_scratch[argument] = execution->values[source];
    }
    for (uint32_t argument = 0u; argument < target->argument_count; ++argument) {
        uint32_t value = target->argument_ids[argument];
        execution->values[value] = execution->edge_scratch[argument];
        execution->initialized[value] = true;
    }
    execution->block_id = instruction->successors[successor_index];
    execution->instruction_id = 0u;
    return true;
}

static bool reference_child_execution_create(XrReferenceExecution *parent,
                                             const XrValidatedInstruction *instruction,
                                             XrReferenceExecution **child_out) {
    if (child_out)
        *child_out = NULL;
    if (!parent || !parent->context || !instruction || !child_out)
        return false;
    bool indirect = instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT;
    const XrReferenceCallableValue *carrier = NULL;
    uint32_t source_argument = 0u;
    uint32_t target_argument = 0u;
    uint32_t function_id = instruction->immediate.coroutine_call.function_id;
    if (indirect) {
        if (instruction->operand_count == 0u || !parent->initialized[instruction->operands[0]] ||
            parent->values[instruction->operands[0]].category != XR_CORE_IR_VALUE ||
            parent->values[instruction->operands[0]].as.value.kind != XR_REFERENCE_VALUE_CALLABLE)
            return false;
        carrier = parent->values[instruction->operands[0]].as.value.as.callable;
        function_id = callable_function_id(parent->context->program, carrier);
        source_argument = 1u;
    }
    if (function_id >= parent->context->program->function_count)
        return false;
    const XrValidatedFunction *function = &parent->context->program->functions[function_id];
    uint32_t visible_parameters =
        function->parameter_count - (carrier && carrier->has_capture ? 1u : 0u);
    if (function->parameter_count < (carrier && carrier->has_capture ? 1u : 0u) ||
        instruction->operand_count < source_argument + visible_parameters ||
        function->value_count > parent->context->budget.max_value_cells ||
        parent->depth == parent->context->budget.max_call_depth)
        return false;
    XrReferenceExecution *child = xr_calloc(1u, sizeof(*child));
    if (!child)
        return false;
    child->lease = parent->lease;
    child->context = parent->context;
    child->depth = parent->depth + 1u;
    child->function_id = function_id;
    child->block_id = function->entry_block;
    child->cancel_block_id = XR_PROGRAM_LOCATION_NONE;
    child->suspension_block_id = XR_PROGRAM_LOCATION_NONE;
    child->suspension_instruction_id = XR_PROGRAM_LOCATION_NONE;
    if (!reference_execution_allocate_values(child, function)) {
        xr_reference_execution_free(child);
        return false;
    }
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
            parent->context->program, function, target_argument);
        XrReferenceValue value = expected == XR_CORE_IR_PLACE && parent->values[source].as.place
                                     ? *eval_place_value_const(parent->values[source].as.place)
                                     : parent->values[source].as.value;
        if (!parent->initialized[source] || parent->values[source].category != expected ||
            !reference_value_matches_type(parent->context->program, value,
                                          function->parameter_types[target_argument])) {
            xr_reference_execution_free(child);
            return false;
        }
        child->values[target] = parent->values[source];
        child->initialized[target] = true;
    }
    *child_out = child;
    return true;
}

bool xr_reference_execution_create(XrInstance *instance, uint32_t function_id,
                                   const XrReferenceValue *arguments, uint32_t argument_count,
                                   const XrReferenceBudget *budget,
                                   XrReferenceExecution **execution_out) {
    if (execution_out)
        *execution_out = NULL;
    XrReferenceBudget selected = budget ? *budget : xr_reference_default_budget();
    if (!instance || !execution_out || (argument_count != 0u && !arguments) ||
        selected.max_steps == 0u || selected.max_value_cells == 0u || selected.max_call_depth == 0u)
        return false;
    XrExecutionLease lease = {0};
    if (xr_execution_instance_acquire(instance, &lease) != XR_EXECUTION_OK)
        return false;
    XrValidatedProgram *program = xr_execution_lease_retain_program(&lease);
    if (!program || program->module_count != 0u || function_id >= program->function_count) {
        xr_validated_program_free(program);
        (void) xr_execution_lease_release(&lease);
        return false;
    }
    const XrValidatedFunction *function = &program->functions[function_id];
    if (argument_count != function->parameter_count || function->coroutine_safepoint_count == 0u ||
        function->coroutine_state_count != function->coroutine_safepoint_count + 1u ||
        function->value_count > selected.max_value_cells)
        goto reject;
    for (uint32_t block = 0; block < function->block_count; ++block)
        for (uint32_t instruction = 0; instruction < function->blocks[block].instruction_count;
             ++instruction)
            if (!reference_coroutine_operation_supported(
                    function->blocks[block].instructions[instruction].operation_id))
                goto reject;
    XrReferenceExecution *execution = xr_calloc(1u, sizeof(*execution));
    if (!execution)
        goto reject;
    execution->lease = lease;
    execution->owns_lease = true;
    execution->context = xr_calloc(1u, sizeof(*execution->context));
    execution->owns_context = true;
    if (!execution->context || !reference_execution_allocate_values(execution, function)) {
        if (execution->context) {
            execution->context->program = program;
            xr_reference_execution_free(execution);
        } else {
            xr_free(execution->edge_scratch);
            xr_free(execution->initialized);
            xr_free(execution->places);
            xr_free(execution->values);
            reference_execution_release_lease(execution);
            xr_validated_program_free(program);
            xr_free(execution);
        }
        return false;
    }
    execution->providers = (XrReferenceProviderBinding) {
        .context = &execution->lease,
        .call_i64_unary = reference_execution_provider_i64_unary,
        .call_i64_nullary = reference_execution_provider_i64_nullary,
        .call_bool_i64_unary = reference_execution_provider_bool_i64_unary,
        .call_optional_i64_pair_nullary = reference_execution_provider_optional_i64_pair_nullary,
        .output_write = reference_execution_provider_output_write,
    };
    execution->context->program = program;
    execution->context->providers = &execution->providers;
    execution->context->budget = selected;
    execution->function_id = function_id;
    execution->depth = 1u;
    execution->block_id = function->entry_block;
    execution->cancel_block_id = XR_PROGRAM_LOCATION_NONE;
    execution->suspension_block_id = XR_PROGRAM_LOCATION_NONE;
    execution->suspension_instruction_id = XR_PROGRAM_LOCATION_NONE;
    for (uint32_t argument = 0; argument < argument_count; ++argument) {
        uint32_t value_id = function->blocks[function->entry_block].argument_ids[argument];
        if ((function->parameter_modes[argument] == XR_PARAM_REF &&
             !function_parameter_is_class_receiver(program, function, argument)) ||
            !reference_value_matches_type(program, arguments[argument],
                                          function->parameter_types[argument])) {
            xr_reference_execution_free(execution);
            return false;
        }
        execution->values[value_id] = (EvalRuntimeValue) {
            .category = XR_CORE_IR_VALUE,
            .as.value = arguments[argument],
        };
        execution->initialized[value_id] = true;
    }
    *execution_out = execution;
    return true;

reject:
    xr_validated_program_free(program);
    (void) xr_execution_lease_release(&lease);
    return false;
}

static const XrValidatedInstruction *
reference_suspension_instruction(const XrReferenceExecution *execution) {
    if (!execution || !execution->context ||
        execution->suspension_block_id == XR_PROGRAM_LOCATION_NONE ||
        execution->suspension_instruction_id == XR_PROGRAM_LOCATION_NONE)
        return NULL;
    const XrValidatedFunction *function =
        &execution->context->program->functions[execution->function_id];
    if (execution->suspension_block_id >= function->block_count)
        return NULL;
    const XrValidatedBlock *source = &function->blocks[execution->suspension_block_id];
    if (execution->suspension_instruction_id >= source->instruction_count)
        return NULL;
    return &source->instructions[execution->suspension_instruction_id];
}

static bool reference_materialize_suspension_edge(XrReferenceExecution *execution,
                                                  uint32_t successor_index) {
    const XrValidatedInstruction *instruction = reference_suspension_instruction(execution);
    if (!instruction)
        return false;
    const XrValidatedFunction *function =
        &execution->context->program->functions[execution->function_id];
    if (successor_index >= instruction->successor_count ||
        instruction->successors[successor_index] >= function->block_count)
        return false;
    uint32_t operand_start = 0u;
    if (instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_YIELD ||
        instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_SUSPEND) {
        if (instruction->successor_count != 2u)
            return false;
        if (instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_SUSPEND)
            operand_start = instruction->immediate.coroutine_suspend.request_operand_count;
        if (successor_index == 1u)
            operand_start += function->blocks[instruction->successors[0]].argument_count;
    } else if ((instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED ||
                instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT) &&
               successor_index != 0u && instruction->successor_count <= 3u) {
        bool indirect = instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT;
        uint32_t callee_id = instruction->immediate.coroutine_call.function_id;
        uint32_t parameter_prefix = 0u;
        uint16_t result_type_id = XR_CORE_TYPE_VOID;
        if (indirect) {
            const XrReferenceCallableValue *carrier =
                execution->values[instruction->operands[0]].as.value.as.callable;
            callee_id = callable_function_id(execution->context->program, carrier);
            const XrValidatedType *callable =
                carrier ? xr_validated_program_type(execution->context->program,
                                                    carrier->callable_type_id)
                        : NULL;
            const XrValidatedSignature *signature =
                callable && callable->signature_id < execution->context->program->signature_count
                    ? &execution->context->program->signatures[callable->signature_id]
                    : NULL;
            if (!signature)
                return false;
            parameter_prefix = signature->parameter_count + 1u;
            result_type_id = signature->result_type_id;
        }
        if (callee_id >= execution->context->program->function_count)
            return false;
        const XrValidatedFunction *callee = &execution->context->program->functions[callee_id];
        if (!indirect) {
            parameter_prefix = callee->parameter_count;
            result_type_id = callee->result_type_id;
        }
        const XrValidatedBlock *normal = &function->blocks[instruction->successors[0]];
        uint32_t implicit_result = result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u;
        if (normal->argument_count < implicit_result)
            return false;
        operand_start = parameter_prefix + normal->argument_count - implicit_result;
        if (successor_index == 2u)
            operand_start += function->blocks[instruction->successors[1]].argument_count;
    } else {
        return false;
    }
    return reference_execution_transfer_edge(execution, instruction, successor_index, operand_start,
                                             NULL);
}

XrReferenceOutcome xr_reference_execution_step(XrReferenceExecution *execution) {
    if (!execution || !execution->context || execution->finished ||
        !xr_execution_lease_is_valid(&execution->lease))
        return execution_outcome(execution, XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
    if (execution->suspended && !execution->child &&
        !reference_materialize_suspension_edge(execution, 0u)) {
        execution->finished = true;
        reference_execution_release_lease(execution);
        return execution_outcome(execution, XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
    }
    execution->suspended = false;
    execution->cancel_block_id = XR_PROGRAM_LOCATION_NONE;
    execution->suspension_block_id = XR_PROGRAM_LOCATION_NONE;
    execution->suspension_instruction_id = XR_PROGRAM_LOCATION_NONE;
    const XrValidatedFunction *function =
        &execution->context->program->functions[execution->function_id];
    for (;;) {
        const XrValidatedBlock *block = &function->blocks[execution->block_id];
        if (execution->instruction_id >= block->instruction_count) {
            execution->finished = true;
            reference_execution_release_lease(execution);
            return execution_outcome(execution, XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
        }
        const XrValidatedInstruction *instruction =
            &block->instructions[execution->instruction_id++];
        if (++execution->context->steps > execution->context->budget.max_steps) {
            execution->finished = true;
            reference_execution_release_lease(execution);
            return execution_outcome(execution, XR_REFERENCE_OUTCOME_RESOURCE_LIMIT);
        }
        for (uint32_t operand = 0; operand < instruction->operand_count; ++operand) {
            if (!execution->initialized[instruction->operands[operand]]) {
                execution->finished = true;
                reference_execution_release_lease(execution);
                return execution_outcome(execution, XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
            }
        }
        switch (instruction->operation_id) {
            case XR_CORE_OP_CORE_BLOCK_ARGUMENT:
                break;
            case XR_CORE_OP_CORE_CONSTANT_I64: {
                const XrValidatedConstant *constant =
                    &execution->context->program->constants[instruction->immediate.constant_id];
                execution->values[instruction->result_id] = (EvalRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value =
                        {
                            .kind = XR_REFERENCE_VALUE_I64,
                            .as.i64 = constant->value.i64,
                        },
                };
                execution->initialized[instruction->result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_CONSTANT_BOOL: {
                const XrValidatedConstant *constant =
                    &execution->context->program->constants[instruction->immediate.constant_id];
                execution->values[instruction->result_id] = (EvalRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value =
                        {
                            .kind = XR_REFERENCE_VALUE_BOOL,
                            .as.boolean = constant->value.boolean,
                        },
                };
                execution->initialized[instruction->result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_ADD_I64:
            case XR_CORE_OP_CORE_SUB_I64:
            case XR_CORE_OP_CORE_MUL_I64: {
                int64_t left = execution->values[instruction->operands[0]].as.value.as.i64;
                int64_t right = execution->values[instruction->operands[1]].as.value.as.i64;
                int64_t value = 0;
                if (instruction->immediate.u32 == 0u) {
                    bool valid = instruction->operation_id == XR_CORE_OP_CORE_ADD_I64
                                     ? checked_add(left, right, &value)
                                 : instruction->operation_id == XR_CORE_OP_CORE_SUB_I64
                                     ? checked_sub(left, right, &value)
                                     : checked_mul(left, right, &value);
                    if (!valid) {
                        execution->finished = true;
                        reference_execution_release_lease(execution);
                        XrReferenceOutcome result =
                            execution_outcome(execution, XR_REFERENCE_OUTCOME_TRAP);
                        result.trap = XR_REFERENCE_TRAP_INTEGER_OVERFLOW;
                        return result;
                    }
                } else {
                    uint64_t bits = instruction->operation_id == XR_CORE_OP_CORE_ADD_I64
                                        ? (uint64_t) left + (uint64_t) right
                                    : instruction->operation_id == XR_CORE_OP_CORE_SUB_I64
                                        ? (uint64_t) left - (uint64_t) right
                                        : (uint64_t) left * (uint64_t) right;
                    value = i64_from_bits(bits);
                }
                execution->values[instruction->result_id] = (EvalRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value = {.kind = XR_REFERENCE_VALUE_I64, .as.i64 = value},
                };
                execution->initialized[instruction->result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_COMPARE_I64: {
                int64_t left = execution->values[instruction->operands[0]].as.value.as.i64;
                int64_t right = execution->values[instruction->operands[1]].as.value.as.i64;
                bool comparison = false;
                switch (instruction->immediate.u32) {
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
                        reference_execution_release_lease(execution);
                        return execution_outcome(execution,
                                                 XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
                }
                execution->values[instruction->result_id] = (EvalRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value = {.kind = XR_REFERENCE_VALUE_BOOL, .as.boolean = comparison},
                };
                execution->initialized[instruction->result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT: {
                XrReferenceAggregateValue *aggregate =
                    allocate_aggregate(execution->context, instruction->result_type_id, UINT32_MAX,
                                       instruction->operand_count);
                if (!aggregate) {
                    execution->finished = true;
                    reference_execution_release_lease(execution);
                    return execution_outcome(execution, XR_REFERENCE_OUTCOME_RESOURCE_LIMIT);
                }
                for (uint32_t field = 0u; field < instruction->operand_count; ++field)
                    aggregate->fields[field] =
                        execution->values[instruction->operands[field]].as.value;
                execution->values[instruction->result_id] = (EvalRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value =
                        {
                            .kind = XR_REFERENCE_VALUE_AGGREGATE,
                            .as.aggregate = aggregate,
                        },
                };
                execution->initialized[instruction->result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_AGGREGATE_PROJECT: {
                const XrReferenceAggregateValue *aggregate =
                    execution->values[instruction->operands[0]].as.value.as.aggregate;
                execution->values[instruction->result_id] = (EvalRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value = aggregate->fields[instruction->immediate.field_ordinal],
                };
                execution->initialized[instruction->result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_CALLABLE_PACK: {
                XrReferenceCallableValue *carrier = allocate_callable(execution->context);
                if (!carrier) {
                    execution->finished = true;
                    reference_execution_release_lease(execution);
                    return execution_outcome(execution, XR_REFERENCE_OUTCOME_RESOURCE_LIMIT);
                }
                carrier->callable_type_id = instruction->result_type_id;
                carrier->function_id = instruction->immediate.function_id;
                carrier->has_capture = instruction->operand_count != 0u;
                if (carrier->has_capture) {
                    uint32_t capture_value = instruction->operands[0];
                    carrier->capture_type_id = function->value_types[capture_value];
                    carrier->capture = execution->values[capture_value].as.value;
                }
                execution->values[instruction->result_id] = (EvalRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value =
                        {
                            .kind = XR_REFERENCE_VALUE_CALLABLE,
                            .as.callable = carrier,
                        },
                };
                execution->initialized[instruction->result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_EXISTENTIAL_PACK: {
                const XrValidatedType *existential = xr_validated_program_type(
                    execution->context->program, instruction->result_type_id);
                uint16_t concrete_type = function->value_types[instruction->operands[0]];
                XrReferenceExistentialValue *carrier = allocate_existential(execution->context);
                uint32_t conformance =
                    existential ? conformance_id(execution->context->program, concrete_type,
                                                 existential->interface_id)
                                : XR_PROGRAM_LOCATION_NONE;
                if (!carrier || conformance == XR_PROGRAM_LOCATION_NONE) {
                    execution->finished = true;
                    reference_execution_release_lease(execution);
                    return execution_outcome(execution, XR_REFERENCE_OUTCOME_RESOURCE_LIMIT);
                }
                carrier->existential_type_id = instruction->result_type_id;
                carrier->concrete_type_id = concrete_type;
                carrier->conformance_id = conformance;
                if (existential->interface_use_kind ==
                    XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE) {
                    carrier->owned_storage.value =
                        execution->values[instruction->operands[0]].as.value;
                    carrier->owned_storage.initialized = true;
                    carrier->payload.category = XR_CORE_IR_PLACE;
                    carrier->payload.as.place = &carrier->owned_storage;
                } else {
                    carrier->payload = execution->values[instruction->operands[0]];
                }
                execution->values[instruction->result_id] = (EvalRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value =
                        {
                            .kind = XR_REFERENCE_VALUE_EXISTENTIAL,
                            .as.existential = carrier,
                        },
                };
                execution->initialized[instruction->result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_EXISTENTIAL_REBORROW_READ: {
                const XrReferenceExistentialValue *source =
                    execution->values[instruction->operands[0]].as.value.as.existential;
                XrReferenceExistentialValue *carrier = allocate_existential(execution->context);
                if (!source || !carrier) {
                    execution->finished = true;
                    reference_execution_release_lease(execution);
                    return execution_outcome(execution, XR_REFERENCE_OUTCOME_RESOURCE_LIMIT);
                }
                carrier->existential_type_id = instruction->result_type_id;
                carrier->concrete_type_id = source->concrete_type_id;
                carrier->conformance_id = source->conformance_id;
                carrier->payload.category = XR_CORE_IR_VALUE;
                carrier->payload.as.value = source->payload.category == XR_CORE_IR_PLACE
                                                ? *eval_place_value_const(source->payload.as.place)
                                                : source->payload.as.value;
                execution->values[instruction->result_id] = (EvalRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value =
                        {
                            .kind = XR_REFERENCE_VALUE_EXISTENTIAL,
                            .as.existential = carrier,
                        },
                };
                execution->initialized[instruction->result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_OWNER_COPY: {
                XrReferenceValue copy = void_value();
                CloneStatus clone_status = clone_reference_value(
                    execution->context,
                    execution->values[instruction->operands[0]].as.value,
                    instruction->result_type_id, &copy);
                if (clone_status != CLONE_STATUS_OK) {
                    execution->finished = true;
                    reference_execution_release_lease(execution);
                    return execution_outcome(execution,
                                             clone_failure_outcome_kind(clone_status));
                }
                execution->values[instruction->result_id] = (EvalRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value = copy,
                };
                execution->initialized[instruction->result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_OWNER_MOVE:
                execution->values[instruction->result_id] =
                    execution->values[instruction->operands[0]];
                execution->initialized[instruction->result_id] = true;
                break;
            case XR_CORE_OP_CORE_CONSTANT_STRING:
            case XR_CORE_OP_CORE_CONSTANT_RUNE:
            case XR_CORE_OP_CORE_STRING_FROM_SCALAR:
            case XR_CORE_OP_CORE_STRING_CONCAT:
            case XR_CORE_OP_CORE_COMPARE_STRING:
            case XR_CORE_OP_CORE_COMPARE_RUNE:
            case XR_CORE_OP_CORE_OUTPUT_GROUP: {
                XrReferenceValue produced = void_value();
                TextOperationStatus text_status = evaluate_text_operation(
                    execution->context, instruction, execution->values, &produced);
                if (text_status != TEXT_OPERATION_OK) {
                    XrReferenceOutcome failure =
                        text_operation_outcome(execution->context, text_status);
                    execution->finished = true;
                    reference_execution_release_lease(execution);
                    XrReferenceOutcome result = execution_outcome(execution, failure.kind);
                    result.trap = failure.trap;
                    return result;
                }
                if (instruction->result_id != XR_PROGRAM_LOCATION_NONE) {
                    execution->values[instruction->result_id] = (EvalRuntimeValue) {
                        .category = XR_CORE_IR_VALUE,
                        .as.value = produced,
                    };
                    execution->initialized[instruction->result_id] = true;
                }
                break;
            }
            case XR_CORE_OP_CORE_CLASS_CONSTRUCT: {
                XrReferenceClassValue *value = allocate_class(
                    execution->context, instruction->result_type_id, instruction->operand_count);
                if (!value) {
                    execution->finished = true;
                    reference_execution_release_lease(execution);
                    return execution_outcome(execution, XR_REFERENCE_OUTCOME_RESOURCE_LIMIT);
                }
                for (uint32_t field = 0u; field < instruction->operand_count; ++field)
                    value->fields[field] =
                        execution->values[instruction->operands[field]].as.value;
                execution->values[instruction->result_id] = (EvalRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value =
                        {
                            .kind = XR_REFERENCE_VALUE_CLASS_REFERENCE,
                            .as.class_reference = value,
                        },
                };
                execution->initialized[instruction->result_id] = true;
                emit_lifecycle(execution->context, XR_REFERENCE_EVENT_CLASS_CONSTRUCT,
                               XR_REFERENCE_EVENT_ORIGIN_PROGRAM_OPERATION, value, UINT64_MAX,
                               UINT32_MAX);
                break;
            }
            case XR_CORE_OP_CORE_OWNER_ALIAS: {
                XrReferenceClassValue *value =
                    (XrReferenceClassValue *) (void *)
                        execution->values[instruction->operands[0]].as.value.as.class_reference;
                if (!value || !value->alive || value->owner_count == UINT32_MAX) {
                    execution->finished = true;
                    reference_execution_release_lease(execution);
                    return execution_outcome(execution,
                                             XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
                }
                ++value->owner_count;
                execution->values[instruction->result_id] = (EvalRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value =
                        {
                            .kind = XR_REFERENCE_VALUE_CLASS_REFERENCE,
                            .as.class_reference = value,
                        },
                };
                execution->initialized[instruction->result_id] = true;
                emit_lifecycle(execution->context, XR_REFERENCE_EVENT_CLASS_SHARE,
                               XR_REFERENCE_EVENT_ORIGIN_PROGRAM_OPERATION, value, value->identity,
                               UINT32_MAX);
                break;
            }
            case XR_CORE_OP_CORE_CLASS_FIELD_LOAD: {
                XrReferenceClassValue *value =
                    (XrReferenceClassValue *) (void *)
                        execution->values[instruction->operands[0]].as.value.as.class_reference;
                uint32_t field = instruction->immediate.field_ordinal;
                if (!value || !value->alive || field >= value->field_count) {
                    execution->finished = true;
                    reference_execution_release_lease(execution);
                    return execution_outcome(execution,
                                             XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
                }
                XrReferenceValue loaded = void_value();
                CloneStatus load_status = class_field_load_value(
                    execution->context, value, field, instruction->result_type_id, &loaded);
                if (load_status != CLONE_STATUS_OK) {
                    execution->finished = true;
                    reference_execution_release_lease(execution);
                    return execution_outcome(execution,
                                             clone_failure_outcome_kind(load_status));
                }
                execution->values[instruction->result_id] = (EvalRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value = loaded,
                };
                execution->initialized[instruction->result_id] = true;
                emit_lifecycle(execution->context, XR_REFERENCE_EVENT_CLASS_FIELD_LOAD,
                               XR_REFERENCE_EVENT_ORIGIN_PROGRAM_OPERATION, value, UINT64_MAX,
                               field);
                break;
            }
            case XR_CORE_OP_CORE_CLASS_FIELD_PLACE: {
                XrReferenceClassValue *value =
                    (XrReferenceClassValue *) (void *)
                        execution->values[instruction->operands[0]].as.value.as.class_reference;
                uint32_t field = instruction->immediate.field_ordinal;
                if (!value || !value->alive || field >= value->field_count) {
                    execution->finished = true;
                    reference_execution_release_lease(execution);
                    return execution_outcome(execution,
                                             XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
                }
                execution->places[instruction->result_id].alias = &value->fields[field];
                execution->places[instruction->result_id].initialized = true;
                execution->values[instruction->result_id] = (EvalRuntimeValue) {
                    .category = XR_CORE_IR_PLACE,
                    .as.place = &execution->places[instruction->result_id],
                };
                execution->initialized[instruction->result_id] = true;
                emit_lifecycle(execution->context, XR_REFERENCE_EVENT_CLASS_FIELD_PLACE,
                               XR_REFERENCE_EVENT_ORIGIN_PROGRAM_OPERATION, value, UINT64_MAX,
                               field);
                break;
            }
            case XR_CORE_OP_CORE_PLACE_LOCAL:
                execution->places[instruction->result_id].alias =
                    &execution->values[instruction->operands[0]].as.value;
                execution->places[instruction->result_id].initialized = true;
                execution->values[instruction->result_id] = (EvalRuntimeValue) {
                    .category = XR_CORE_IR_PLACE,
                    .as.place = &execution->places[instruction->result_id],
                };
                execution->initialized[instruction->result_id] = true;
                break;
            case XR_CORE_OP_CORE_PLACE_LOAD:
                execution->values[instruction->result_id] = (EvalRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value =
                        *eval_place_value(execution->values[instruction->operands[0]].as.place),
                };
                execution->initialized[instruction->result_id] = true;
                break;
            case XR_CORE_OP_CORE_PLACE_STORE:
                *eval_place_value(execution->values[instruction->operands[0]].as.place) =
                    execution->values[instruction->operands[1]].as.value;
                break;
            case XR_CORE_OP_CORE_PLACE_PROJECT: {
                XrReferenceValue *source =
                    eval_place_value(execution->values[instruction->operands[0]].as.place);
                XrReferenceAggregateValue *aggregate =
                    (XrReferenceAggregateValue *) (void *) source->as.aggregate;
                execution->places[instruction->result_id].alias =
                    &aggregate->fields[instruction->immediate.field_ordinal];
                execution->places[instruction->result_id].initialized = true;
                execution->values[instruction->result_id] = (EvalRuntimeValue) {
                    .category = XR_CORE_IR_PLACE,
                    .as.place = &execution->places[instruction->result_id],
                };
                execution->initialized[instruction->result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_PLACE_TAKE:
                execution->values[instruction->result_id] = (EvalRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value =
                        *eval_place_value(execution->values[instruction->operands[0]].as.place),
                };
                execution->values[instruction->operands[0]].as.place->initialized = false;
                execution->initialized[instruction->result_id] = true;
                break;
            case XR_CORE_OP_CORE_PLACE_EXCHANGE: {
                XrReferenceValue *place =
                    eval_place_value(execution->values[instruction->operands[0]].as.place);
                XrReferenceValue previous = *place;
                XrReferenceValue replacement =
                    execution->values[instruction->operands[1]].as.value;
                *place = replacement;
                execution->values[instruction->result_id] = (EvalRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value = previous,
                };
                execution->initialized[instruction->result_id] = true;
                emit_place_exchange(execution->context, instruction->result_type_id, previous,
                                    replacement);
                break;
            }
            case XR_CORE_OP_CORE_PROVIDER_CALL: {
                const XrValidatedBlock *trap_target =
                    instruction->successor_count == 1u
                        ? &function->blocks[instruction->successors[0]]
                        : NULL;
                uint32_t operand_count =
                    instruction->operand_count - (trap_target ? trap_target->argument_count : 0u);
                XrReferenceOutcome provider = reference_provider_call(
                    execution->context, function, instruction, execution->values, operand_count);
                if (provider.kind != XR_REFERENCE_OUTCOME_RETURN) {
                    if (provider.kind == XR_REFERENCE_OUTCOME_TRAP &&
                        provider.trap == XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED && trap_target) {
                        if (!reference_execution_transfer_edge(execution, instruction, 0u,
                                                               operand_count, NULL)) {
                            execution->finished = true;
                            reference_execution_release_lease(execution);
                            return execution_outcome(execution,
                                                     XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
                        }
                        break;
                    }
                    execution->finished = true;
                    reference_execution_release_lease(execution);
                    provider.state_id = execution->state_id;
                    return provider;
                }
                execution->values[instruction->result_id] = (EvalRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value = provider.value,
                };
                execution->initialized[instruction->result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_CALL_SEALED_DIRECT:
            case XR_CORE_OP_CORE_CALL_WITNESS_DIRECT: {
                uint32_t target_function =
                    instruction->operation_id == XR_CORE_OP_CORE_CALL_SEALED_DIRECT
                        ? instruction->immediate.function_id
                        : XR_PROGRAM_LOCATION_NONE;
                const XrValidatedFunction *callee =
                    target_function == XR_PROGRAM_LOCATION_NONE
                        ? NULL
                        : &execution->context->program->functions[target_function];
                uint32_t call_operand_count = instruction->operand_count;
                if (instruction->successor_count == 1u)
                    call_operand_count -=
                        function->blocks[instruction->successors[0]].argument_count;
                if (instruction->operation_id == XR_CORE_OP_CORE_CALL_WITNESS_DIRECT) {
                    const XrReferenceExistentialValue *carrier =
                        execution->values[instruction->operands[0]].as.value.as.existential;
                    target_function = witness_function_id(execution->context->program, carrier,
                                                          instruction->immediate.u32);
                    if (target_function != XR_PROGRAM_LOCATION_NONE)
                        callee = &execution->context->program->functions[target_function];
                }
                if (!callee || call_operand_count != callee->parameter_count) {
                    execution->finished = true;
                    reference_execution_release_lease(execution);
                    return execution_outcome(execution, XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
                }
                EvalRuntimeValue *arguments = xr_calloc(
                    callee->parameter_count ? callee->parameter_count : 1u, sizeof(*arguments));
                if (!arguments) {
                    execution->finished = true;
                    reference_execution_release_lease(execution);
                    return execution_outcome(execution, XR_REFERENCE_OUTCOME_RESOURCE_LIMIT);
                }
                uint32_t argument = 0u;
                if (instruction->operation_id == XR_CORE_OP_CORE_CALL_WITNESS_DIRECT) {
                    const XrReferenceExistentialValue *carrier =
                        execution->values[instruction->operands[0]].as.value.as.existential;
                    if (!witness_receiver_argument(carrier, callee->receiver_mode, &arguments[0])) {
                        xr_free(arguments);
                        execution->finished = true;
                        reference_execution_release_lease(execution);
                        return execution_outcome(execution,
                                                 XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
                    }
                    argument = 1u;
                }
                for (; argument < callee->parameter_count; ++argument)
                    arguments[argument] = execution->values[instruction->operands[argument]];
                XrReferenceOutcome nested =
                    evaluate_function(execution->context, target_function, arguments,
                                      callee->parameter_count, execution->depth + 1u);
                xr_free(arguments);
                if (nested.kind != XR_REFERENCE_OUTCOME_RETURN) {
                    if (nested.kind == XR_REFERENCE_OUTCOME_TRAP &&
                        nested.trap == XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED &&
                        instruction->successor_count == 1u) {
                        if (!reference_execution_transfer_edge(execution, instruction, 0u,
                                                               call_operand_count, NULL)) {
                            execution->finished = true;
                            reference_execution_release_lease(execution);
                            return execution_outcome(execution,
                                                     XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
                        }
                        break;
                    }
                    execution->finished = true;
                    reference_execution_release_lease(execution);
                    nested.state_id = execution->state_id;
                    return nested;
                }
                if (instruction->result_id != XR_PROGRAM_LOCATION_NONE) {
                    execution->values[instruction->result_id] = (EvalRuntimeValue) {
                        .category = XR_CORE_IR_VALUE,
                        .as.value = nested.value,
                    };
                    execution->initialized[instruction->result_id] = true;
                }
                break;
            }
            case XR_CORE_OP_CORE_BRANCH:
            case XR_CORE_OP_CORE_CONDITIONAL_BRANCH: {
                uint32_t successor = 0u;
                uint32_t operand_start = 0u;
                if (instruction->operation_id == XR_CORE_OP_CORE_CONDITIONAL_BRANCH) {
                    bool condition =
                        execution->values[instruction->operands[0]].as.value.as.boolean;
                    successor = condition ? 0u : 1u;
                    operand_start =
                        condition
                            ? 1u
                            : 1u + function->blocks[instruction->successors[0]].argument_count;
                }
                if (!reference_execution_transfer_edge(execution, instruction, successor,
                                                       operand_start, NULL)) {
                    execution->finished = true;
                    reference_execution_release_lease(execution);
                    return execution_outcome(execution, XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
                }
                break;
            }
            case XR_CORE_OP_CORE_COROUTINE_YIELD: {
                const XrValidatedCoroutineSafepoint *safepoint =
                    &function->coroutine_safepoints[instruction->immediate.u32];
                execution->suspension_block_id = execution->block_id;
                execution->suspension_instruction_id = execution->instruction_id - 1u;
                execution->state_id = safepoint->resume_state_id;
                execution->block_id = instruction->successors[0];
                execution->instruction_id = 0u;
                execution->suspended = true;
                execution->cancel_block_id = instruction->successors[1];
                XrReferenceOutcome result =
                    execution_outcome(execution, XR_REFERENCE_OUTCOME_SUSPENDED);
                result.safepoint_id = instruction->immediate.u32;
                result.suspension.kind = XR_SUSPENSION_REQUEST_COOPERATIVE_YIELD;
                return result;
            }
            case XR_CORE_OP_CORE_COROUTINE_SUSPEND: {
                uint32_t safepoint_id = instruction->immediate.coroutine_suspend.safepoint_id;
                uint32_t request_value = instruction->operands[0];
                const XrValidatedCoroutineSafepoint *safepoint =
                    &function->coroutine_safepoints[safepoint_id];
                execution->suspension_block_id = execution->block_id;
                execution->suspension_instruction_id = execution->instruction_id - 1u;
                execution->state_id = safepoint->resume_state_id;
                execution->block_id = instruction->successors[0];
                execution->instruction_id = 0u;
                execution->suspended = true;
                execution->cancel_block_id = instruction->successors[1];
                XrReferenceOutcome result =
                    execution_outcome(execution, XR_REFERENCE_OUTCOME_SUSPENDED);
                result.safepoint_id = safepoint_id;
                result.suspension.kind = XR_SUSPENSION_REQUEST_TIMER_AFTER_MS;
                result.suspension.operand_count = 1u;
                result.suspension.payload.timer_after_ms = xr_suspension_timer_normalize_ms(
                    execution->values[request_value].as.value.as.i64);
                return result;
            }
            case XR_CORE_OP_CORE_COROUTINE_CALL_SEALED:
            case XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT: {
                bool indirect =
                    instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT;
                uint32_t callee_id = instruction->immediate.coroutine_call.function_id;
                uint32_t safepoint_id = indirect
                                            ? instruction->immediate.u32
                                            : instruction->immediate.coroutine_call.safepoint_id;
                const XrValidatedCoroutineSafepoint *safepoint =
                    &function->coroutine_safepoints[safepoint_id];
                if (!execution->child &&
                    !reference_child_execution_create(execution, instruction, &execution->child)) {
                    execution->finished = true;
                    reference_execution_release_lease(execution);
                    return execution_outcome(execution, XR_REFERENCE_OUTCOME_RESOURCE_LIMIT);
                }
                callee_id = execution->child->function_id;
                const XrValidatedFunction *callee =
                    &execution->context->program->functions[callee_id];
                XrReferenceOutcome child = xr_reference_execution_step(execution->child);
                if (child.kind == XR_REFERENCE_OUTCOME_SUSPENDED) {
                    execution->suspension_block_id = execution->block_id;
                    execution->suspension_instruction_id = execution->instruction_id - 1u;
                    --execution->instruction_id;
                    execution->state_id = safepoint->resume_state_id;
                    execution->suspended = true;
                    execution->cancel_block_id = instruction->successors[1];
                    XrReferenceOutcome suspended =
                        execution_outcome(execution, XR_REFERENCE_OUTCOME_SUSPENDED);
                    suspended.safepoint_id = safepoint_id;
                    suspended.suspension = child.suspension;
                    return suspended;
                }
                if (child.kind != XR_REFERENCE_OUTCOME_RETURN) {
                    if (child.kind == XR_REFERENCE_OUTCOME_TRAP &&
                        child.trap == XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED &&
                        instruction->successor_count == 3u) {
                        execution->suspension_block_id = execution->block_id;
                        execution->suspension_instruction_id = execution->instruction_id - 1u;
                        bool transferred = reference_materialize_suspension_edge(execution, 2u);
                        xr_reference_execution_free(execution->child);
                        execution->child = NULL;
                        execution->suspension_block_id = XR_PROGRAM_LOCATION_NONE;
                        execution->suspension_instruction_id = XR_PROGRAM_LOCATION_NONE;
                        if (!transferred) {
                            execution->finished = true;
                            reference_execution_release_lease(execution);
                            return execution_outcome(execution,
                                                     XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
                        }
                        execution->instruction_id = 0u;
                        break;
                    }
                    execution->finished = true;
                    xr_reference_execution_free(execution->child);
                    execution->child = NULL;
                    reference_execution_release_lease(execution);
                    child.steps = execution->context->steps;
                    child.state_id = execution->state_id;
                    return child;
                }
                const XrReferenceValue *implicit_result =
                    callee->result_type_id == XR_CORE_TYPE_VOID ? NULL : &child.value;
                uint32_t parameter_prefix = callee->parameter_count;
                if (indirect) {
                    const XrReferenceCallableValue *carrier =
                        execution->values[instruction->operands[0]].as.value.as.callable;
                    const XrValidatedType *callable =
                        carrier ? xr_validated_program_type(execution->context->program,
                                                            carrier->callable_type_id)
                                : NULL;
                    if (!callable ||
                        callable->signature_id >= execution->context->program->signature_count) {
                        execution->finished = true;
                        xr_reference_execution_free(execution->child);
                        execution->child = NULL;
                        reference_execution_release_lease(execution);
                        return execution_outcome(execution,
                                                 XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
                    }
                    parameter_prefix =
                        execution->context->program->signatures[callable->signature_id]
                            .parameter_count +
                        1u;
                }
                bool transferred = reference_execution_transfer_edge(
                    execution, instruction, 0u, parameter_prefix, implicit_result);
                xr_reference_execution_free(execution->child);
                execution->child = NULL;
                if (!transferred) {
                    execution->finished = true;
                    reference_execution_release_lease(execution);
                    return execution_outcome(execution, XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
                }
                break;
            }
            case XR_CORE_OP_CORE_OWNER_DROP: {
                drop_reference_value(execution->context,
                                     &execution->values[instruction->operands[0]].as.value,
                                     XR_REFERENCE_EVENT_ORIGIN_PROGRAM_OPERATION, NULL);
                break;
            }
            case XR_CORE_OP_CORE_CANCEL_PUBLISH:
                execution->finished = true;
                reference_execution_release_lease(execution);
                return execution_outcome(execution, XR_REFERENCE_OUTCOME_CANCELLED);
            case XR_CORE_OP_CORE_TRAP: {
                execution->finished = true;
                XrReferenceOutcome result = execution_outcome(execution, XR_REFERENCE_OUTCOME_TRAP);
                result.trap = instruction->immediate.u32 == 7u
                                  ? XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED
                                  : XR_REFERENCE_TRAP_EXPLICIT;
                reference_execution_release_lease(execution);
                return result;
            }
            case XR_CORE_OP_CORE_RETURN: {
                execution->finished = true;
                XrReferenceOutcome result =
                    execution_outcome(execution, XR_REFERENCE_OUTCOME_RETURN);
                result.value = instruction->operand_count == 0u
                                   ? void_value()
                                   : execution->values[instruction->operands[0]].as.value;
                reference_execution_release_lease(execution);
                return result;
            }
            default:
                execution->finished = true;
                reference_execution_release_lease(execution);
                return execution_outcome(execution, XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
        }
    }
}

XrReferenceOutcome xr_reference_execution_cancel(XrReferenceExecution *execution) {
    if (!execution || execution->finished || !execution->suspended ||
        execution->cancel_block_id == XR_PROGRAM_LOCATION_NONE ||
        !xr_execution_lease_is_valid(&execution->lease))
        return execution_outcome(execution, XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
    uint32_t successor_index = 1u;
    if (execution->child) {
        XrReferenceOutcome child = xr_reference_execution_cancel(execution->child);
        const XrValidatedInstruction *instruction = reference_suspension_instruction(execution);
        if (child.kind == XR_REFERENCE_OUTCOME_TRAP &&
            child.trap == XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED && instruction &&
            (instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED ||
             instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT) &&
            instruction->successor_count == 3u) {
            successor_index = 2u;
        } else if (child.kind != XR_REFERENCE_OUTCOME_CANCELLED) {
            execution->finished = true;
            xr_reference_execution_free(execution->child);
            execution->child = NULL;
            reference_execution_release_lease(execution);
            child.steps = execution->context->steps;
            child.state_id = execution->state_id;
            return child;
        }
        xr_reference_execution_free(execution->child);
        execution->child = NULL;
    }
    if (!reference_materialize_suspension_edge(execution, successor_index)) {
        execution->finished = true;
        reference_execution_release_lease(execution);
        return execution_outcome(execution, XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
    }
    execution->instruction_id = 0u;
    execution->suspended = false;
    execution->cancel_block_id = XR_PROGRAM_LOCATION_NONE;
    execution->suspension_block_id = XR_PROGRAM_LOCATION_NONE;
    execution->suspension_instruction_id = XR_PROGRAM_LOCATION_NONE;
    return xr_reference_execution_step(execution);
}

void xr_reference_execution_free(XrReferenceExecution *execution) {
    if (!execution)
        return;
    xr_reference_execution_free(execution->child);
    xr_free(execution->edge_scratch);
    xr_free(execution->initialized);
    xr_free(execution->places);
    xr_free(execution->values);
    reference_execution_release_lease(execution);
    if (execution->owns_context && execution->context) {
        XrValidatedProgram *program = (XrValidatedProgram *) execution->context->program;
        free_eval_arena(execution->context);
        xr_free(execution->context);
        xr_validated_program_free(program);
    }
    xr_free(execution);
}

XrReferenceBudget xr_reference_default_budget(void) {
    XrReferenceBudget budget = {
        .max_steps = UINT64_C(1000000),
        .max_value_cells = UINT64_C(1048576),
        .max_call_depth = 1024u,
    };
    return budget;
}

XrReferenceOutcome
xr_reference_evaluate_bound(const XrValidatedProgram *program, uint32_t function_id,
                            const XrReferenceValue *arguments, uint32_t argument_count,
                            const XrReferenceProfile *profile, const XrReferenceBudget *budget,
                            const XrReferenceProviderBinding *providers) {
    XrReferenceBudget selected = budget ? *budget : xr_reference_default_budget();
    EvalContext context = {
        .program = program,
        .providers = providers,
        .profile = profile ? *profile : (XrReferenceProfile) {0},
        .budget = selected,
    };
    if (program && function_id < program->function_count &&
        program->functions[function_id].coroutine_safepoint_count != 0u)
        return outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, &context);
    if (!program || program->module_count != 0u || function_id >= program->function_count || (argument_count != 0 && !arguments) ||
        selected.max_steps == 0 || selected.max_value_cells == 0 || selected.max_call_depth == 0)
        return outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, &context);
    const XrValidatedFunction *function = &program->functions[function_id];
    EvalRuntimeValue *runtime_arguments =
        xr_calloc(argument_count ? argument_count : 1u, sizeof(EvalRuntimeValue));
    if (!runtime_arguments)
        return outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, &context);
    for (uint32_t index = 0; index < argument_count; ++index) {
        if (index >= function->parameter_count ||
            (function->parameter_modes[index] == XR_PARAM_REF &&
             !function_parameter_is_class_receiver(program, function, index))) {
            xr_free(runtime_arguments);
            return outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, &context);
        }
        runtime_arguments[index].category = XR_CORE_IR_VALUE;
        runtime_arguments[index].as.value = arguments[index];
    }
    XrReferenceOutcome result =
        evaluate_function(&context, function_id, runtime_arguments, argument_count, 1u);
    xr_free(runtime_arguments);
    if (result.kind == XR_REFERENCE_OUTCOME_RETURN &&
        (result.value.kind == XR_REFERENCE_VALUE_AGGREGATE ||
         result.value.kind == XR_REFERENCE_VALUE_CLASS_REFERENCE ||
         result.value.kind == XR_REFERENCE_VALUE_STRING)) {
        XrReferenceValue detached = void_value();
        if (detach_reference_value(result.value, &detached)) {
            result.value = detached;
            result.owns_dynamic_values = true;
        } else {
            result = outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, &context);
        }
    }
    if (result.kind == XR_REFERENCE_OUTCOME_RETURN &&
        (result.value.kind == XR_REFERENCE_VALUE_EXISTENTIAL ||
         result.value.kind == XR_REFERENCE_VALUE_CALLABLE))
        result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, &context);
    if (result.kind == XR_REFERENCE_OUTCOME_ERROR &&
        (result.error_value.kind == XR_REFERENCE_VALUE_AGGREGATE ||
         result.error_value.kind == XR_REFERENCE_VALUE_CLASS_REFERENCE)) {
        XrReferenceValue detached = void_value();
        if (detach_reference_value(result.error_value, &detached)) {
            result.error_value = detached;
            result.owns_dynamic_values = true;
        } else {
            result = outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, &context);
        }
    }
    if (result.kind == XR_REFERENCE_OUTCOME_ERROR &&
        (result.error_value.kind == XR_REFERENCE_VALUE_EXISTENTIAL ||
         result.error_value.kind == XR_REFERENCE_VALUE_CALLABLE))
        result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, &context);
    if (result.kind == XR_REFERENCE_OUTCOME_PANIC &&
        result.panic_value.kind != XR_REFERENCE_VALUE_PANIC_INFO)
        result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, &context);
    free_eval_arena(&context);
    return result;
}

XrReferenceOutcome xr_reference_evaluate(const XrValidatedProgram *program, uint32_t function_id,
                                         const XrReferenceValue *arguments, uint32_t argument_count,
                                         const XrReferenceProfile *profile,
                                         const XrReferenceBudget *budget) {
    return xr_reference_evaluate_bound(program, function_id, arguments, argument_count, profile,
                                       budget, NULL);
}
