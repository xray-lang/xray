/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_reference_clone.inc.c - Transactional reference-value graph cloning
 *
 * KEY CONCEPT:
 *   Clone candidates remain private to the arena until the complete graph is
 *   valid. A failed clone owns an exclusive suffix of every arena table, so it
 *   can reclaim that suffix without following graph edges or touching sources.
 */

typedef struct CloneMemoEntry {
    const XrReferenceClassValue *source;
    XrReferenceClassValue *target;
} CloneMemoEntry;

typedef struct CloneMemo {
    CloneMemoEntry *entries;
    uint32_t count;
    uint32_t capacity;
} CloneMemo;

typedef struct CloneArenaCheckpoint {
    uint32_t aggregate_count;
    uint32_t class_count;
    uint32_t existential_count;
    uint32_t callable_count;
    uint32_t string_count;
    uint64_t aggregate_cell_count;
} CloneArenaCheckpoint;

typedef struct CloneTransaction {
    EvalContext *context;
    CloneArenaCheckpoint checkpoint;
    CloneMemo memo;
} CloneTransaction;

typedef enum CloneStatus {
    CLONE_STATUS_OK = 0,
    CLONE_STATUS_RESOURCE_LIMIT,
    CLONE_STATUS_UNSUPPORTED,
    CLONE_STATUS_INVALID_VALUE,
} CloneStatus;

static void clone_transaction_begin(CloneTransaction *transaction, EvalContext *context) {
    memset(transaction, 0, sizeof(*transaction));
    transaction->context = context;
    transaction->checkpoint = (CloneArenaCheckpoint) {
        .aggregate_count = context->aggregate_count,
        .class_count = context->class_count,
        .existential_count = context->existential_count,
        .callable_count = context->callable_count,
        .string_count = context->string_count,
        .aggregate_cell_count = context->aggregate_cell_count,
    };
}

static void clone_transaction_rollback(CloneTransaction *transaction) {
    EvalContext *context = transaction->context;
    const CloneArenaCheckpoint *checkpoint = &transaction->checkpoint;
    for (uint32_t index = context->class_count; index > checkpoint->class_count; --index) {
        XrReferenceClassValue *value = context->classes[index - 1u];
        if (value) {
            emit_lifecycle(context, XR_REFERENCE_EVENT_CLASS_RECLAIM,
                           XR_REFERENCE_EVENT_ORIGIN_CLONE_ROLLBACK, value, UINT64_MAX,
                           UINT32_MAX);
            xr_free(value->fields);
            xr_free(value);
        }
        context->classes[index - 1u] = NULL;
    }
    for (uint32_t index = context->aggregate_count; index > checkpoint->aggregate_count; --index) {
        XrReferenceAggregateValue *value = context->aggregates[index - 1u];
        if (value) {
            xr_free(value->fields);
            xr_free(value);
        }
        context->aggregates[index - 1u] = NULL;
    }
    for (uint32_t index = context->existential_count; index > checkpoint->existential_count;
         --index) {
        xr_free(context->existentials[index - 1u]);
        context->existentials[index - 1u] = NULL;
    }
    for (uint32_t index = context->callable_count; index > checkpoint->callable_count; --index) {
        xr_free(context->callables[index - 1u]);
        context->callables[index - 1u] = NULL;
    }
    for (uint32_t index = context->string_count; index > checkpoint->string_count; --index) {
        XrReferenceStringValue *string = context->strings[index - 1u];
        if (string) {
            xr_free(string->bytes);
            xr_free(string);
        }
        context->strings[index - 1u] = NULL;
    }
    context->aggregate_count = checkpoint->aggregate_count;
    context->class_count = checkpoint->class_count;
    context->existential_count = checkpoint->existential_count;
    context->callable_count = checkpoint->callable_count;
    context->string_count = checkpoint->string_count;
    context->aggregate_cell_count = checkpoint->aggregate_cell_count;
}

static void clone_transaction_commit(CloneTransaction *transaction) {
    for (uint32_t index = 0u; index < transaction->memo.count; ++index) {
        const CloneMemoEntry *entry = &transaction->memo.entries[index];
        emit_lifecycle(transaction->context, XR_REFERENCE_EVENT_CLASS_COPY,
                       XR_REFERENCE_EVENT_ORIGIN_PROGRAM_OPERATION, entry->target,
                       entry->source->identity, UINT32_MAX);
    }
}

static void clone_transaction_dispose(CloneTransaction *transaction) {
    xr_free(transaction->memo.entries);
    memset(transaction, 0, sizeof(*transaction));
}

static bool clone_memo_add(CloneMemo *memo, const XrReferenceClassValue *source,
                           XrReferenceClassValue *target) {
    if (memo->count == memo->capacity) {
        uint32_t capacity = memo->capacity ? memo->capacity * 2u : 8u;
        if (capacity < memo->count || (size_t) capacity > SIZE_MAX / sizeof(*memo->entries))
            return false;
        CloneMemoEntry *grown =
            xr_realloc(memo->entries, (size_t) capacity * sizeof(*memo->entries));
        if (!grown)
            return false;
        memo->entries = grown;
        memo->capacity = capacity;
    }
    memo->entries[memo->count++] = (CloneMemoEntry) {.source = source, .target = target};
    return true;
}

static bool clone_type_is_pure_trivial_value_graph(const XrValidatedProgram *program,
                                                   uint16_t type_id, uint32_t depth) {
    if (type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
        return type_id == XR_CORE_TYPE_BOOL || type_id == XR_CORE_TYPE_I64 ||
               type_id == XR_CORE_TYPE_U32 || type_id == XR_CORE_TYPE_U16 ||
               type_id == XR_CORE_TYPE_TARGET_OS || type_id == XR_CORE_TYPE_TARGET_ARCH ||
               type_id == XR_CORE_TYPE_TARGET_ABI || type_id == XR_CORE_TYPE_TARGET_ENDIAN ||
               type_id == XR_CORE_TYPE_ERROR || type_id == XR_CORE_TYPE_PANIC_INFO ||
               type_id == XR_CORE_TYPE_RUNE;
    }
    if (!program || depth > program->type_count)
        return false;
    const XrValidatedType *type = xr_validated_program_type(program, type_id);
    if (!type || type->ownership != XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL ||
        type->copy_contract != XR_CORE_IR_COPY_TRIVIAL)
        return false;
    if (type->kind == XR_CORE_IR_TYPE_AGGREGATE) {
        for (uint32_t field = 0u; field < type->field_count; ++field)
            if (!clone_type_is_pure_trivial_value_graph(program, type->field_types[field],
                                                        depth + 1u))
                return false;
        return true;
    }
    if (type->kind == XR_CORE_IR_TYPE_VARIANT) {
        for (uint32_t variant = 0u; variant < type->variant_count; ++variant)
            for (uint32_t field = 0u; field < type->variants[variant].payload_count; ++field)
                if (!clone_type_is_pure_trivial_value_graph(
                        program, type->variants[variant].payload_types[field], depth + 1u))
                    return false;
        return true;
    }
    return false;
}

static CloneStatus clone_reference_value_inner(CloneTransaction *transaction,
                                               XrReferenceValue source, uint16_t type_id,
                                               XrReferenceValue *output) {
    EvalContext *context = transaction->context;
    const XrValidatedType *type = xr_validated_program_type(context->program, type_id);
    if (type_id == XR_CORE_TYPE_STRING) {
        /* An explicit string copy is an independent owner with its own bytes. */
        const XrReferenceStringValue *original = source.as.string;
        if (source.kind != XR_REFERENCE_VALUE_STRING || !original)
            return CLONE_STATUS_INVALID_VALUE;
        XrReferenceStringValue *copy = allocate_string(context, original->size);
        if (!copy)
            return CLONE_STATUS_RESOURCE_LIMIT;
        if (original->size != 0u)
            memcpy(copy->bytes, original->bytes, original->size);
        *output = string_value(copy);
        return CLONE_STATUS_OK;
    }
    if (!type) {
        *output = source;
        return CLONE_STATUS_OK;
    }
    if (type->kind == XR_CORE_IR_TYPE_EXISTENTIAL) {
        if (source.kind != XR_REFERENCE_VALUE_EXISTENTIAL || !source.as.existential)
            return CLONE_STATUS_INVALID_VALUE;
        const XrReferenceExistentialValue *source_existential = source.as.existential;
        /* Trivial read existentials are cycle-free value envelopes. Affine or
         * place-backed existentials need graph-aware carrier memoization and
         * remain fail-closed until that contract is implemented. */
        if (source_existential->existential_type_id != type_id ||
            type->ownership != XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL ||
            type->copy_contract != XR_CORE_IR_COPY_TRIVIAL ||
            source_existential->payload.category != XR_CORE_IR_VALUE)
            return CLONE_STATUS_UNSUPPORTED;
        if (!clone_type_is_pure_trivial_value_graph(
                context->program, source_existential->concrete_type_id, 0u))
            return CLONE_STATUS_UNSUPPORTED;
        XrReferenceExistentialValue *copy = allocate_existential(context);
        if (!copy)
            return CLONE_STATUS_RESOURCE_LIMIT;
        copy->existential_type_id = source_existential->existential_type_id;
        copy->concrete_type_id = source_existential->concrete_type_id;
        copy->conformance_id = source_existential->conformance_id;
        copy->payload.category = XR_CORE_IR_VALUE;
        CloneStatus payload_status = clone_reference_value_inner(
            transaction, source_existential->payload.as.value,
            source_existential->concrete_type_id, &copy->payload.as.value);
        if (payload_status != CLONE_STATUS_OK)
            return payload_status;
        output->kind = XR_REFERENCE_VALUE_EXISTENTIAL;
        output->as.existential = copy;
        return CLONE_STATUS_OK;
    }
    if (type->kind == XR_CORE_IR_TYPE_CALLABLE) {
        if (source.kind != XR_REFERENCE_VALUE_CALLABLE || !source.as.callable)
            return CLONE_STATUS_INVALID_VALUE;
        const XrReferenceCallableValue *source_callable = source.as.callable;
        XrReferenceCallableValue *copy = allocate_callable(context);
        if (!copy)
            return CLONE_STATUS_RESOURCE_LIMIT;
        *copy = *source_callable;
        copy->detached = false;
        copy->dispose_queued = false;
        copy->dispose_next = NULL;
        if (copy->has_capture) {
            CloneStatus capture_status = clone_reference_value_inner(
                transaction, source_callable->capture, source_callable->capture_type_id,
                &copy->capture);
            if (capture_status != CLONE_STATUS_OK)
                return capture_status;
        }
        output->kind = XR_REFERENCE_VALUE_CALLABLE;
        output->as.callable = copy;
        return CLONE_STATUS_OK;
    }
    if (type->kind == XR_CORE_IR_TYPE_CLASS_REFERENCE) {
        if (source.kind != XR_REFERENCE_VALUE_CLASS_REFERENCE || !source.as.class_reference)
            return CLONE_STATUS_INVALID_VALUE;
        const XrReferenceClassValue *source_class = source.as.class_reference;
        if (!class_value_is_live(source_class, type_id) ||
            source_class->field_count != type->field_count)
            return CLONE_STATUS_INVALID_VALUE;
        for (uint32_t index = 0u; index < transaction->memo.count; ++index) {
            if (transaction->memo.entries[index].source != source_class)
                continue;
            if (transaction->memo.entries[index].target->owner_count == UINT32_MAX)
                return CLONE_STATUS_RESOURCE_LIMIT;
            ++transaction->memo.entries[index].target->owner_count;
            output->kind = XR_REFERENCE_VALUE_CLASS_REFERENCE;
            output->as.class_reference = transaction->memo.entries[index].target;
            return CLONE_STATUS_OK;
        }
        XrReferenceClassValue *copy = allocate_class(context, type_id, type->field_count);
        if (!copy || !clone_memo_add(&transaction->memo, source_class, copy))
            return CLONE_STATUS_RESOURCE_LIMIT;
        output->kind = XR_REFERENCE_VALUE_CLASS_REFERENCE;
        output->as.class_reference = copy;
        for (uint32_t field = 0u; field < type->field_count; ++field) {
            CloneStatus field_status = clone_reference_value_inner(
                transaction, source_class->fields[field], type->field_types[field],
                &copy->fields[field]);
            if (field_status != CLONE_STATUS_OK)
                return field_status;
        }
        return CLONE_STATUS_OK;
    }
    if (type->kind != XR_CORE_IR_TYPE_AGGREGATE && type->kind != XR_CORE_IR_TYPE_VARIANT)
        return CLONE_STATUS_UNSUPPORTED;
    if (source.kind != XR_REFERENCE_VALUE_AGGREGATE || !source.as.aggregate)
        return CLONE_STATUS_INVALID_VALUE;
    const XrReferenceAggregateValue *source_aggregate = source.as.aggregate;
    if (source_aggregate->type_id != type_id)
        return CLONE_STATUS_INVALID_VALUE;
    uint32_t expected_field_count = type->field_count;
    if (type->kind == XR_CORE_IR_TYPE_VARIANT) {
        if (source_aggregate->variant_ordinal >= type->variant_count)
            return CLONE_STATUS_INVALID_VALUE;
        expected_field_count =
            type->variants[source_aggregate->variant_ordinal].payload_count;
    }
    if (source_aggregate->field_count != expected_field_count)
        return CLONE_STATUS_INVALID_VALUE;
    XrReferenceAggregateValue *copy = allocate_aggregate(
        context, type_id, source_aggregate->variant_ordinal, source_aggregate->field_count);
    if (!copy)
        return CLONE_STATUS_RESOURCE_LIMIT;
    for (uint32_t field = 0u; field < source_aggregate->field_count; ++field) {
        uint16_t field_type = XR_CORE_TYPE_VOID;
        if (type->kind == XR_CORE_IR_TYPE_AGGREGATE) {
            field_type = type->field_types[field];
        } else {
            field_type = type->variants[source_aggregate->variant_ordinal].payload_types[field];
        }
        CloneStatus field_status = clone_reference_value_inner(
            transaction, source_aggregate->fields[field], field_type, &copy->fields[field]);
        if (field_status != CLONE_STATUS_OK)
            return field_status;
    }
    output->kind = XR_REFERENCE_VALUE_AGGREGATE;
    output->as.aggregate = copy;
    return CLONE_STATUS_OK;
}

static CloneStatus clone_reference_value(EvalContext *context, XrReferenceValue source,
                                         uint16_t type_id, XrReferenceValue *output) {
    if (!context || !output)
        return CLONE_STATUS_INVALID_VALUE;
    *output = void_value();
    CloneTransaction transaction;
    clone_transaction_begin(&transaction, context);
    XrReferenceValue candidate = void_value();
    CloneStatus status = clone_reference_value_inner(&transaction, source, type_id, &candidate);
    if (status != CLONE_STATUS_OK) {
        clone_transaction_rollback(&transaction);
        clone_transaction_dispose(&transaction);
        return status;
    }
    clone_transaction_commit(&transaction);
    *output = candidate;
    clone_transaction_dispose(&transaction);
    return CLONE_STATUS_OK;
}

static XrReferenceOutcomeKind clone_failure_outcome_kind(CloneStatus status) {
    return status == CLONE_STATUS_RESOURCE_LIMIT ? XR_REFERENCE_OUTCOME_RESOURCE_LIMIT
                                                 : XR_REFERENCE_OUTCOME_INVALID_INVOCATION;
}

static CloneStatus class_field_load_value(EvalContext *context,
                                          const XrReferenceClassValue *instance,
                                          uint32_t field_ordinal, uint16_t field_type_id,
                                          XrReferenceValue *output) {
    if (!context || !instance || !output || !instance->alive ||
        field_ordinal >= instance->field_count)
        return CLONE_STATUS_INVALID_VALUE;
    if (xr_validated_program_type_ownership(context->program, field_type_id) ==
        XR_CORE_IR_TYPE_OWNERSHIP_AFFINE) {
        *output = instance->fields[field_ordinal];
        return CLONE_STATUS_OK;
    }
    return clone_reference_value(context, instance->fields[field_ordinal], field_type_id, output);
}

static void dispose_detached_reference_value(DetachedDisposeState *state,
                                             XrReferenceValue *value) {
    /* Host disposal releases detached storage but is not a Program lifecycle event. */
    drop_reference_value(NULL, value, XR_REFERENCE_EVENT_ORIGIN_PROGRAM_OPERATION, state);
}

static void free_detached_dispose_state(DetachedDisposeState *state) {
    while (state->aggregates) {
        XrReferenceAggregateValue *value = state->aggregates;
        state->aggregates = value->dispose_next;
        xr_free(value->fields);
        xr_free(value);
    }
    while (state->existentials) {
        XrReferenceExistentialValue *value = state->existentials;
        state->existentials = value->dispose_next;
        xr_free(value);
    }
    while (state->callables) {
        XrReferenceCallableValue *value = state->callables;
        state->callables = value->dispose_next;
        xr_free(value);
    }
    while (state->strings) {
        XrReferenceStringValue *value = state->strings;
        state->strings = value->dispose_next;
        xr_free(value->bytes);
        xr_free(value);
    }
}

static bool detach_reference_value(XrReferenceValue source, XrReferenceValue *output) {
    if (!output)
        return false;
    *output = void_value();
    if (source.kind == XR_REFERENCE_VALUE_CLASS_REFERENCE) {
        XrReferenceClassValue *value =
            (XrReferenceClassValue *) (void *) source.as.class_reference;
        if (!value || !value->alive)
            return false;
        if (!value->detached) {
            value->detached = true;
            for (uint32_t field = 0u; field < value->field_count; ++field) {
                XrReferenceValue detached = void_value();
                if (!detach_reference_value(value->fields[field], &detached))
                    return false;
                value->fields[field] = detached;
            }
        }
        *output = source;
        return true;
    }
    if (source.kind == XR_REFERENCE_VALUE_AGGREGATE) {
        XrReferenceAggregateValue *aggregate =
            (XrReferenceAggregateValue *) (void *) source.as.aggregate;
        if (!aggregate || (aggregate->field_count != 0u && !aggregate->fields))
            return false;
        if (!aggregate->detached) {
            aggregate->detached = true;
            for (uint32_t field = 0u; field < aggregate->field_count; ++field) {
                XrReferenceValue detached = void_value();
                if (!detach_reference_value(aggregate->fields[field], &detached))
                    return false;
                aggregate->fields[field] = detached;
            }
        }
        *output = source;
        return true;
    }
    if (source.kind == XR_REFERENCE_VALUE_EXISTENTIAL) {
        XrReferenceExistentialValue *existential =
            (XrReferenceExistentialValue *) (void *) source.as.existential;
        if (!existential)
            return false;
        if (!existential->detached) {
            existential->detached = true;
            if (existential->owned_storage.initialized) {
                XrReferenceValue detached = void_value();
                if (!detach_reference_value(existential->owned_storage.value, &detached))
                    return false;
                existential->owned_storage.value = detached;
            }
        }
        *output = source;
        return true;
    }
    if (source.kind == XR_REFERENCE_VALUE_STRING) {
        XrReferenceStringValue *string = (XrReferenceStringValue *) (void *) source.as.string;
        if (!string)
            return false;
        string->detached = true;
        *output = source;
        return true;
    }
    if (source.kind == XR_REFERENCE_VALUE_CALLABLE) {
        XrReferenceCallableValue *callable =
            (XrReferenceCallableValue *) (void *) source.as.callable;
        if (!callable)
            return false;
        if (!callable->detached) {
            callable->detached = true;
            if (callable->has_capture) {
                XrReferenceValue detached = void_value();
                if (!detach_reference_value(callable->capture, &detached))
                    return false;
                callable->capture = detached;
            }
        }
        *output = source;
        return true;
    }
    *output = source;
    return true;
}
