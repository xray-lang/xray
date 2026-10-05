/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_atomic_value.inc.c - Owned atomic cells with precommit failure barriers
 */
static bool atomic_cell_capable(XirAtomic *cell) {
    return atomic_is_lock_free(&cell->object.references) && atomic_is_lock_free(&cell->bits);
}
XR_FUNC bool xr_xir_atomic_capability(void) {
    XirAtomic cell = {0};
    atomic_init(&cell.object.references, 1);
    atomic_init(&cell.bits, 0);
    return atomic_cell_capable(&cell);
}
static bool atomic_work(XrXirValueAdmission *admission, uint64_t units) {
    if (!admission || units > admission->work) return false;
    admission->work -= units;
    return true;
}
XR_FUNC XrXirValueStatus xr_xir_atomic_new(XrXirType type, const XrXirValue *initial,
    XrXirValueAdmission *admission, XrXirValue *output) {
    if (!admission || !admission->domain || !admission->arena || !unit_value(output))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirType element = xr_xir_atomic_element(xr_xir_compile_type_arena_types(admission->arena), type);
    if ((element != XR_XIR_I64 && element != XR_XIR_F64 && element != XR_XIR_BOOL) || !initial)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = xr_xir_value_admit(initial, element, admission);
    if (status != XR_XIR_VALUE_OK) return status;
    if (!atomic_work(admission, 1)) return XR_XIR_VALUE_LIMIT;
    if (!xr_xir_atomic_capability()) return XR_XIR_VALUE_UNSUPPORTED;
    XirAtomic *cell = (XirAtomic *)constructed_allocate(admission->domain,
        (XrXirTypeArena *)admission->arena, type, sizeof(*cell), &status);
    if (!cell) return status;
    cell->element = element;
    if (!atomic_cell_capable(cell)) {
        constructed_discard(&cell->object, sizeof(*cell));
        return XR_XIR_VALUE_UNSUPPORTED;
    }
    atomic_init(&cell->bits, (uint64_t)initial->payload);
    XrXirValue result = {(uint32_t)type, 0, 0};
    memcpy(&result.payload, &cell, sizeof(cell));
    *output = result;
    return XR_XIR_VALUE_OK;
}
static XrXirRunStatus atomic_value_status(XrXirValueStatus status) {
    switch (status) {
    case XR_XIR_VALUE_OK: return XR_XIR_RUN_OK;
    case XR_XIR_VALUE_OOM: return XR_XIR_RUN_OUT_OF_MEMORY;
    case XR_XIR_VALUE_LIMIT: case XR_XIR_VALUE_REFCOUNT_LIMIT: return XR_XIR_RUN_STEP_LIMIT;
    case XR_XIR_VALUE_UNSUPPORTED: return XR_XIR_RUN_UNSUPPORTED;
    default: return XR_XIR_RUN_BAD_ARGUMENT;
    }
}
static XrXirAtomicOutcome atomic_outcome(XrXirRunStatus status) {
    return (XrXirAtomicOutcome){status, XR_XIR_CALL_READY, false};
}
static XrXirAtomicOutcome atomic_permission(const XrXirCallView *view,
    XrXirValueAdmission *admission) {
    XrXirAtomicOutcome result = atomic_outcome(XR_XIR_RUN_OK);
    if (view) {
        result.permission = xr_xir_call_execution_status(view);
        if (result.permission == XR_XIR_CALL_READY && xr_xir_call_admission(view) != admission)
            result.permission = XR_XIR_CALL_BAD_STATE;
    }
    return result;
}
static XrXirRunStatus atomic_ordering(const XrXirValue *value, XrXirAtomicOperation operation,
    XrXirValueAdmission *admission, uint32_t *ordinal) {
    *ordinal = 4;
    if (value) {
        const XrXirTypes *types = xr_xir_compile_type_arena_types(admission->arena);
        XrXirType inner = xr_xir_nullable_element(types, (XrXirType)value->type);
        if (!inner || !xr_xir_nominal_native_ordering(types, inner)) return XR_XIR_RUN_ATOMIC_ARGUMENT;
        XrXirValueStatus status = xr_xir_value_admit(value, (XrXirType)value->type, admission);
        if (status != XR_XIR_VALUE_OK) return status == XR_XIR_VALUE_BAD_ARGUMENT ?
            XR_XIR_RUN_ATOMIC_ARGUMENT : atomic_value_status(status);
        bool some = false; const XrXirValue *payload = NULL;
        if (!xr_xir_nullable_view(value, &some, &payload)) return XR_XIR_RUN_ATOMIC_ARGUMENT;
        if (some && xr_xir_enum_variant(payload, ordinal) != XR_XIR_VALUE_OK)
            return XR_XIR_RUN_ATOMIC_ARGUMENT;
    }
    if (*ordinal > 4 || (operation == XR_XIR_ATOMIC_OPERATION_LOAD && (*ordinal == 2 || *ordinal == 3)) ||
        (operation == XR_XIR_ATOMIC_OPERATION_STORE && (*ordinal == 1 || *ordinal == 3)) ||
        (operation == XR_XIR_ATOMIC_OPERATION_TO_STRING && value)) return XR_XIR_RUN_ATOMIC_ARGUMENT;
    return XR_XIR_RUN_OK;
}
static memory_order atomic_success_order(uint32_t ordinal) {
    switch (ordinal) {
    case 0: return memory_order_relaxed;
    case 1: return memory_order_acquire;
    case 2: return memory_order_release;
    case 3: return memory_order_acq_rel;
    default: return memory_order_seq_cst;
    }
}
static memory_order atomic_failure_order(uint32_t ordinal) {
    return ordinal == 2 ? memory_order_relaxed : ordinal == 3 ? memory_order_acquire :
        atomic_success_order(ordinal);
}
static uint32_t atomic_operand_count(XrXirAtomicOperation operation) {
    return operation == XR_XIR_ATOMIC_OPERATION_COMPARE_EXCHANGE ? 2 :
        operation == XR_XIR_ATOMIC_OPERATION_LOAD || operation == XR_XIR_ATOMIC_OPERATION_TOGGLE ||
        operation == XR_XIR_ATOMIC_OPERATION_TO_STRING ? 0 : 1;
}
static bool atomic_operation_type(XrXirAtomicOperation operation, XrXirType element) {
    if (operation == XR_XIR_ATOMIC_OPERATION_TOGGLE) return element == XR_XIR_BOOL;
    if (operation == XR_XIR_ATOMIC_OPERATION_ADD || operation == XR_XIR_ATOMIC_OPERATION_SUB ||
        operation == XR_XIR_ATOMIC_OPERATION_FETCH_ADD || operation == XR_XIR_ATOMIC_OPERATION_FETCH_SUB)
        return element == XR_XIR_I64 || element == XR_XIR_F64;
    return element == XR_XIR_I64 || element == XR_XIR_F64 || element == XR_XIR_BOOL;
}
static bool atomic_result_type(const XrXirAtomicProgress *progress, XrXirType element,
    const XrXirTypes *types) {
    if (progress->operation == XR_XIR_ATOMIC_OPERATION_COMPARE_EXCHANGE) {
        const XrXirTypeNode *node = xr_xir_tuple_signature(types, progress->result_type);
        return node && !node->parameter_span && node->parameter_count == 2 && node->parameters &&
            node->parameters[0].type == element && node->parameters[1].type == XR_XIR_BOOL;
    }
    XrXirType expected = progress->operation == XR_XIR_ATOMIC_OPERATION_STORE ||
        progress->operation == XR_XIR_ATOMIC_OPERATION_ADD || progress->operation == XR_XIR_ATOMIC_OPERATION_SUB ?
        XR_XIR_UNIT : progress->operation == XR_XIR_ATOMIC_OPERATION_TO_STRING ? XR_XIR_STRING : element;
    return progress->result_type == expected;
}
static XrXirRunStatus atomic_tuple_prepare(XrXirAtomicProgress *progress, XirAtomic *cell,
    XrXirValueAdmission *admission, XrXirValue *prepared) {
    XrXirValue fields[2] = {{(uint32_t)cell->element, 0, 0}, {XR_XIR_BOOL, 0, 0}};
    return atomic_value_status(xr_xir_tuple_new(progress->result_type, fields, 2, admission, prepared));
}
/* Only the unaliased tuple prepared here is filled. Scalar payload publication
 * cannot allocate, retain, admit, invoke user code, or report failure. */
static void atomic_tuple_publish(XrXirValue *prepared, uint64_t observed, bool exchanged,
    XrXirValue *output) {
    XirTuple *tuple = (XirTuple *)object_pointer(prepared);
    memcpy(&tuple->fields[0].payload, &observed, sizeof(observed));
    tuple->fields[1].payload = exchanged;
    *output = *prepared;
    *prepared = (XrXirValue){0};
}
static XrXirRunStatus atomic_text(XirAtomic *cell, uint64_t bits,
    XrXirValueAdmission *admission, XrXirValue *output) {
    char text[32]; size_t length = 0;
    if (cell->element == XR_XIR_F64) {
        if (!xr_xir_float_format(64, bits, text, sizeof(text), &length)) return XR_XIR_RUN_BAD_ARGUMENT;
    } else if (cell->element == XR_XIR_BOOL) {
        const char *word = bits ? "true" : "false";
        length = bits ? 4 : 5; memcpy(text, word, length);
    } else {
        int64_t number = 0; memcpy(&number, &bits, sizeof(number));
        int used = snprintf(text, sizeof(text), "%" PRId64, number);
        if (used < 0 || (size_t)used >= sizeof(text)) return XR_XIR_RUN_BAD_ARGUMENT;
        length = (size_t)used;
    }
    if (!atomic_work(admission, length)) return XR_XIR_RUN_STEP_LIMIT;
    return atomic_value_status(xr_xir_string_new(admission->domain, text, length, output));
}
static XrXirAtomicOutcome atomic_attempt(XrXirAtomicProgress *progress,
    XrXirValueAdmission *admission, const XrXirCallView *view, XrXirValue *output) {
    XirAtomic *cell = (XirAtomic *)object_pointer(&progress->receiver);
    XrXirAtomicOutcome result = atomic_permission(view, admission);
    if (result.permission != XR_XIR_CALL_READY) return result;
    XrXirValue prepared = {0};
    if (progress->operation == XR_XIR_ATOMIC_OPERATION_COMPARE_EXCHANGE) {
        result.status = atomic_tuple_prepare(progress, cell, admission, &prepared);
        if (result.status != XR_XIR_RUN_OK) return result;
    }
    if (!atomic_work(admission, 1)) result.status = XR_XIR_RUN_STEP_LIMIT;
    else result = atomic_permission(view, admission);
    if (result.status != XR_XIR_RUN_OK || result.permission != XR_XIR_CALL_READY) {
        xr_xir_value_drop(&prepared); return result;
    }
    memory_order order = atomic_success_order(progress->ordering);
    uint64_t previous = 0, operand = progress->operands[0];
    XrXirAtomicOperation operation = progress->operation;
    if (operation == XR_XIR_ATOMIC_OPERATION_LOAD || operation == XR_XIR_ATOMIC_OPERATION_TO_STRING)
        previous = atomic_load_explicit(&cell->bits, order);
    else if (operation == XR_XIR_ATOMIC_OPERATION_STORE) atomic_store_explicit(&cell->bits, operand, order);
    else if (operation == XR_XIR_ATOMIC_OPERATION_SWAP) previous = atomic_exchange_explicit(&cell->bits, operand, order);
    else if (operation == XR_XIR_ATOMIC_OPERATION_TOGGLE) previous = atomic_fetch_xor_explicit(&cell->bits, 1, order);
    else if (operation == XR_XIR_ATOMIC_OPERATION_COMPARE_EXCHANGE) {
        previous = operand;
        bool exchanged = atomic_compare_exchange_strong_explicit(&cell->bits, &previous,
            progress->operands[1], order, atomic_failure_order(progress->ordering));
        atomic_tuple_publish(&prepared, previous, exchanged, output);
        progress->phase = 2; return result;
    } else if (cell->element == XR_XIR_F64) {
        if (!progress->observed_ready) {
            if (!atomic_work(admission, 1)) return atomic_outcome(XR_XIR_RUN_STEP_LIMIT);
            progress->observed = atomic_load_explicit(&cell->bits, memory_order_relaxed);
            progress->observed_ready = true;
        }
        uint64_t desired = 0;
        bool subtract = operation == XR_XIR_ATOMIC_OPERATION_SUB || operation == XR_XIR_ATOMIC_OPERATION_FETCH_SUB;
        XrXirNumericStatus status = xr_xir_float_arithmetic(64,
            subtract ? XR_XIR_FLOAT_SUBTRACT : XR_XIR_FLOAT_ADD, progress->observed, operand, &desired);
        XR_CHECK(status == XR_XIR_NUMERIC_OK, "binary64 atomic arithmetic accepts every payload");
        previous = progress->observed;
        if (!atomic_work(admission, 1)) return atomic_outcome(XR_XIR_RUN_STEP_LIMIT);
        result = atomic_permission(view, admission);
        if (result.permission != XR_XIR_CALL_READY) return result;
        if (!atomic_compare_exchange_strong_explicit(&cell->bits, &progress->observed, desired,
            order, atomic_failure_order(progress->ordering))) {
            result.continuing = true; return result;
        }
    } else {
        bool subtract = operation == XR_XIR_ATOMIC_OPERATION_SUB || operation == XR_XIR_ATOMIC_OPERATION_FETCH_SUB;
        previous = subtract ? atomic_fetch_sub_explicit(&cell->bits, operand, order) :
            atomic_fetch_add_explicit(&cell->bits, operand, order);
    }
    progress->phase = 2;
    if (operation == XR_XIR_ATOMIC_OPERATION_TO_STRING) result.status = atomic_text(cell, previous, admission, output);
    else if (progress->result_type != XR_XIR_UNIT) {
        XrXirValue value = {(uint32_t)progress->result_type, 0, 0};
        memcpy(&value.payload, &previous, sizeof(previous)); *output = value;
    }
    return result;
}
XR_FUNC XrXirAtomicOutcome xr_xir_atomic_resume(XrXirAtomicProgress *progress,
    XrXirValueAdmission *admission, const XrXirCallView *view, XrXirValue *output) {
    if (!progress || progress->phase != 1 || !admission || !unit_value(output) ||
        !xr_xir_value_argument(&progress->receiver, admission->arena, (XrXirType)progress->receiver.type))
        return atomic_outcome(XR_XIR_RUN_BAD_ARGUMENT);
    XirAtomic *cell = (XirAtomic *)object_pointer(&progress->receiver);
    if (cell->object.kind != XR_XIR_TYPE_ATOMIC ||
        progress->operation < XR_XIR_ATOMIC_OPERATION_LOAD || progress->operation > XR_XIR_ATOMIC_OPERATION_TO_STRING ||
        !atomic_operation_type(progress->operation, cell->element) ||
        (cell->element == XR_XIR_BOOL &&
         ((atomic_operand_count(progress->operation) && progress->operands[0] > 1) ||
          (atomic_operand_count(progress->operation) == 2 && progress->operands[1] > 1))) ||
        progress->ordering > 4 ||
        (progress->operation == XR_XIR_ATOMIC_OPERATION_LOAD && (progress->ordering == 2 || progress->ordering == 3)) ||
        (progress->operation == XR_XIR_ATOMIC_OPERATION_STORE && (progress->ordering == 1 || progress->ordering == 3)) ||
        (progress->operation == XR_XIR_ATOMIC_OPERATION_TO_STRING && progress->ordering != 4) ||
        !atomic_result_type(progress, cell->element,
            xr_xir_compile_type_arena_types(admission->arena))) return atomic_outcome(XR_XIR_RUN_BAD_ARGUMENT);
    return atomic_attempt(progress, admission, view, output);
}
XR_FUNC XrXirAtomicOutcome xr_xir_atomic_start(const XrXirAtomicRequest *request,
    XrXirValueAdmission *admission, const XrXirCallView *view,
    XrXirAtomicProgress *progress, XrXirValue *output) {
    if (!request) return atomic_outcome(XR_XIR_RUN_BAD_ARGUMENT);
    const XrXirValue *receiver = request->receiver, *operands = request->operands;
    const XrXirValue *ordering = request->ordering;
    XrXirAtomicOperation operation = request->operation;
    XrXirType result_type = request->result_type;
    uint32_t count = request->operand_count;
    if (!progress || !unit_value(&progress->receiver) || progress->phase || !admission ||
        !admission->arena || !admission->domain || !receiver || !unit_value(output) ||
        operation < XR_XIR_ATOMIC_OPERATION_LOAD || operation > XR_XIR_ATOMIC_OPERATION_TO_STRING ||
        count != atomic_operand_count(operation) || (count && !operands))
        return atomic_outcome(XR_XIR_RUN_BAD_ARGUMENT);
    XrXirAtomicOutcome result = atomic_permission(view, admission);
    if (result.permission != XR_XIR_CALL_READY) return result;
    XrXirType type = (XrXirType)receiver->type;
    XrXirType element = xr_xir_atomic_element(xr_xir_compile_type_arena_types(admission->arena), type);
    if (!element) return atomic_outcome(XR_XIR_RUN_BAD_ARGUMENT);
    result.status = atomic_value_status(xr_xir_value_admit(receiver, type, admission));
    if (result.status != XR_XIR_RUN_OK) return result;
    if (!atomic_operation_type(operation, element)) return atomic_outcome(XR_XIR_RUN_BAD_ARGUMENT);
    for (uint32_t i = 0; i < count; ++i) {
        result.status = atomic_value_status(xr_xir_value_admit(&operands[i], element, admission));
        if (result.status != XR_XIR_RUN_OK) return result;
    }
    uint32_t ordinal = 4;
    result.status = atomic_ordering(ordering, operation, admission, &ordinal);
    if (result.status != XR_XIR_RUN_OK) return result;
    XrXirAtomicProgress prepared = {0};
    prepared.result_type = result_type; prepared.operation = operation; prepared.ordering = ordinal;
    if (!atomic_result_type(&prepared, element, xr_xir_compile_type_arena_types(admission->arena)))
        return atomic_outcome(XR_XIR_RUN_BAD_ARGUMENT);
    result.status = atomic_value_status(xr_xir_value_copy(receiver, &prepared.receiver));
    if (result.status != XR_XIR_RUN_OK) return result;
    for (uint32_t i = 0; i < count; ++i) prepared.operands[i] = (uint64_t)operands[i].payload;
    prepared.phase = 1; *progress = prepared;
    return xr_xir_atomic_resume(progress, admission, view, output);
}
XR_FUNC void xr_xir_atomic_progress_clear(XrXirAtomicProgress *progress) {
    if (!progress) return;
    xr_xir_value_drop(&progress->receiver);
    memset(progress, 0, sizeof(*progress));
}
