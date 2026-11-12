/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_execution_callbacks.inc.c - Possible callback entries of opaque calls
 *
 * KEY CONCEPT:
 *   The execution owner projects callback targets from its existing origins.
 *   These entries carry invocation guards without adding physical CFG writes.
 */
typedef struct ExecutionCallbackVisit {
    const XirExecutionOwner *owner;
    XirExecutionVisitor visit;
    void *context;
    XirExecutionSite entry;
    uint32_t provided_arguments;
    ExecutionEntryIndex entries;
} ExecutionCallbackVisit;

static bool execution_callback_scalar(XrXirType type) {
    return xr_xir_type_is_integer(type) || type == XR_XIR_UNIT || type == XR_XIR_BOOL || type == XR_XIR_RUNE ||
        type == XR_XIR_F32 || type == XR_XIR_F64 || type == XR_XIR_STRING;
}

static XrXirStatus execution_callback_capture(void *context, const XirExecutionSite *capture) {
    ExecutionCallbackVisit *run = context;
    if (capture->kind == XIR_EXECUTION_FORMAL) {
        XirExecutionParameter formal;
        XrXirStatus status = xir_execution_parameter(run->owner, capture, 0, &formal);
        if (status != XR_XIR_OK) return status;
        XirExecutionSite entry = run->entry;
        entry.receiver_function = capture->receiver_function;
        entry.receiver_value = capture->receiver_value;
        entry.capture_environment = capture->environment;
        entry.invocation_contract = (formal.escaping || formal.generic_storage) &&
            capture->receiver_function == entry.function && capture->environment == XIR_EXECUTION_CURRENT_ENVIRONMENT;
        entry.remainder = entry.invocation_contract ? 0 : XIR_EXECUTION_REMAINDER_PARAMETER;
        return run->visit(run->context, &entry);
    }
    if (capture->kind == XIR_EXECUTION_STORABLE) {
        XirExecutionSite entry = run->entry;
        entry.remainder = XIR_EXECUTION_REMAINDER_OPAQUE;
        return run->visit(run->context, &entry);
    }
    if (capture->kind != XIR_EXECUTION_CALL || capture->protocol != 1 ||
        capture->target >= run->owner->functions) return XR_XIR_BAD_STRUCTURE;
    const EffectCheckFunction *callee = &run->owner->function[capture->target];
    XirExecutionSite entry = run->entry;
    entry.target = capture->target;
    entry.declaration = capture->declaration;
    entry.reference = capture->index;
    entry.capture_environment = capture->environment;
    entry.capture_count = callee->capture_count;
    entry.argument_count = callee->parameters;
    entry.native_callable = callee->native_callable;
    uint64_t provided = (uint64_t)capture->argument_count + run->provided_arguments;
    entry.remainder = callee->parameters > provided ? XIR_EXECUTION_REMAINDER_PARAMETER : 0;
    const EffectCheckCall *binding = &run->owner->call[capture->index];
    for (uint32_t p = 0; p < binding->count; ++p) {
        uint32_t node = run->owner->argument[binding->first + p];
        if (!run->owner->node[node].cell) continue;
        /* Primitive contents keep fixed callable and lifetime properties.
         * Their captured Cell identity still carries its invocation loan. */
        XrXirType payload = xr_xir_cell_element(xir_execution_types(run->owner), run->owner->node[node].type);
        if (entry.entry_context != UINT32_MAX && execution_callback_scalar(payload)) continue;
        entry.remainder |= XIR_EXECUTION_REMAINDER_OPAQUE;
    }
    entry.unresolved = false;
    return run->visit(run->context, &entry);
}

static XrXirStatus execution_callback_arguments(void *context, const XirExecutionSite *parent) {
    ExecutionCallbackVisit *run = context;
    if (!parent->unresolved && !parent->native_callable) return XR_XIR_OK;
    if (parent->unresolved && !(parent->remainder &
        (XIR_EXECUTION_REMAINDER_PARAMETER | XIR_EXECUTION_REMAINDER_OPAQUE))) return XR_XIR_OK;
    const XrXirTypes *types = xir_execution_types(run->owner);
    for (uint32_t p = 0; p < parent->argument_count; ++p) {
        XirExecutionParameter formal;
        XrXirStatus status = xir_execution_parameter(run->owner, parent, p, &formal);
        if (status != XR_XIR_OK) return status;
        if (!xr_xir_type_is_callable(types, formal.type) || xr_xir_type_is_c_callable(types, formal.type)) continue;
        XirExecutionSite entry = *parent;
        entry.kind = XIR_EXECUTION_CALLBACK_ENTRY;
        entry.parent_kind = parent->kind;
        entry.callback_parameter = p;
        entry.entry_context = UINT32_MAX;
        entry.target = entry.declaration = UINT32_MAX;
        entry.argument_count = entry.capture_count = 0;
        entry.output_function = entry.output_value = UINT32_MAX;
        entry.receiver_function = entry.receiver_value = UINT32_MAX;
        entry.reference = UINT32_MAX;
        entry.native_callable = 0;
        entry.signature = formal.type;
        entry.unresolved = true;
        run->entry = entry;
        XirExecutionArgument actual;
        status = xir_execution_argument(run->owner, parent, p, &actual);
        bool complete = false;
        uint32_t remainder = XIR_EXECUTION_REMAINDER_OPAQUE;
        if (status == XR_XIR_OK) {
            XirExecutionValue reference = {actual.function, actual.value, actual.environment};
            run->entry.receiver_function = actual.function;
            run->entry.receiver_value = actual.value;
            status = xir_execution_visit_value_environment_in(run->owner, &reference,
                execution_callback_capture, run, &complete, &remainder);
        }
        if (status != XR_XIR_OK && status != XR_XIR_UNRESOLVED) return status;
        if (complete) continue;
        entry = run->entry;
        entry.remainder = remainder ? remainder : XIR_EXECUTION_REMAINDER_STORAGE;
        status = run->visit(run->context, &entry);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus execution_callback_receiver(void *context, const XirExecutionSite *resolved) {
    ExecutionCallbackVisit *run = context;
    if (resolved->entry_context == UINT32_MAX) return XR_XIR_OK;
    XirStorageFlowView view = {0};
    XrXirStatus status = xir_storage_flow_view(run->owner->storage, &view);
    if (status != XR_XIR_OK) return status;
    if (resolved->entry_context >= view.application_count) return XR_XIR_BAD_STRUCTURE;
    XirStorageApplication app = view.applications[resolved->entry_context];
    XirExecutionSite parent = execution_pending_site(run->owner, run->entry.index);
    run->entry = parent;
    run->entry.kind = XIR_EXECUTION_CALLBACK_ENTRY;
    run->entry.function = app.function; run->entry.instruction = app.instruction;
    run->entry.block = app.block; run->entry.caller_instance = app.caller_instance;
    run->entry.environment = app.environment; run->entry.order = app.order; run->entry.port = app.port;
    run->entry.after_panic = app.after_panic;
    run->entry.cleanup_handler = app.cleanup_handler;
    run->entry.parent_kind = app.kind; run->entry.callback_parameter = UINT32_MAX;
    run->entry.entry_context = resolved->entry_context;
    run->entry.output_function = run->entry.output_value = UINT32_MAX;
    return execution_callback_capture(run, resolved);
}

static XrXirStatus execution_callback_receivers(ExecutionCallbackVisit *run) {
    const EffectCheckedRoots *roots = run->owner;
    if (!roots->storage) return XR_XIR_OK;
    XrXirStatus status = XR_XIR_OK;
    for (uint32_t p = 0; p < roots->pending_count && status == XR_XIR_OK; ++p) {
        EffectCheckPending pending = roots->pending[p];
        if (pending.linked || pending.kind != EFFECT_CHECK_HIGHER_ORDER) continue;
        run->entry = execution_pending_site(roots, p);
        run->provided_arguments = pending.argument_count;
        const EffectCheckFunction *receiver = &roots->function[roots->node[pending.receiver].function];
        bool complete = false;
        uint32_t remainder = 0;
        status = execution_visit_origins(roots, roots->node[pending.receiver].function,
            pending.receiver - receiver->first, XIR_EXECUTION_CURRENT_ENVIRONMENT,
            execution_callback_receiver, run, &complete, &remainder,
            (ExecutionOriginRequest){.kind=EXECUTION_ORIGINS_CALLBACK_ENTRY, .entries=&run->entries});
    }
    return status;
}

XR_FUNC XrXirStatus xir_execution_visit_callback_entries(const XirExecutionOwner *owner,
    XirExecutionVisitor visit, void *context) {
    if (!owner || !visit) return XR_XIR_BAD_STRUCTURE;
    ExecutionCallbackVisit run = {.owner = owner, .visit = visit, .context = context};
    XrXirStatus status = xir_execution_visit_calls(owner, execution_callback_arguments, &run);
    if (status == XR_XIR_OK) status = execution_callback_receivers(&run);
    xr_compile_resources_free(run.entries.incoming); xr_compile_resources_free(run.entries.next_application);
    return status;
}
