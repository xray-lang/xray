/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_execution.inc.c - Read-only projections of the shared execution owner
 */
static XirExecutionSite execution_call_site(const EffectCheckedRoots *roots, uint32_t index) {
    const EffectCheckCall *call = &roots->call[index];
    const EffectCheckFunction *caller = &roots->function[roots->node[call->output].function];
    const EffectCheckFunction *callee = &roots->function[roots->node[call->target].function];
    uint32_t value = call->output - caller->first;
    uint32_t block = UINT32_MAX;
    if (call->instruction < caller->values - caller->parameters)
        block = roots->node[caller->first + caller->parameters + call->instruction].block;
    return (XirExecutionSite){.kind = XIR_EXECUTION_CALL, .index = index,
        .function = caller->declaration, .instruction = call->instruction, .block = block,
        .caller_instance = roots->node[call->output].function,
        .target = roots->node[call->target].function, .declaration = callee->declaration,
        .argument_count = call->count, .capture_count = callee->capture_count,
        .output_function = caller->declaration,
        .output_value = call->instruction < caller->values - caller->parameters ?
            caller->parameters + call->instruction : value < caller->values ? value : UINT32_MAX,
        .receiver_function = UINT32_MAX, .receiver_value = UINT32_MAX, .registration = UINT32_MAX,
        .native_callable = callee->native_callable, .reference = UINT32_MAX, .capture_environment = UINT32_MAX,
        .environment = call->has_environment ? call->environment : UINT32_MAX,
        .port = XIR_EXECUTION_NORMAL, .signature = XR_XIR_UNIT, .protocol = call->protocol};
}

static XirExecutionSite execution_pending_site(const EffectCheckedRoots *roots, uint32_t index) {
    const EffectCheckPending *pending = &roots->pending[index];
    const EffectCheckFunction *caller = &roots->function[pending->function];
    const EffectCheckFunction *receiver = &roots->function[roots->node[pending->receiver].function];
    uint32_t value = pending->receiver - receiver->first;
    uint32_t block = pending->instruction < caller->values - caller->parameters ?
        roots->node[caller->first + caller->parameters + pending->instruction].block : UINT32_MAX;
    return (XirExecutionSite){.kind = XIR_EXECUTION_PENDING, .index = index,
        .function = caller->declaration, .instruction = pending->instruction, .block = block,
        .caller_instance = pending->function, .target = UINT32_MAX, .declaration = UINT32_MAX,
        .argument_count = pending->argument_count, .output_function = caller->declaration,
        .output_value = pending->instruction < caller->values - caller->parameters ?
            caller->parameters + pending->instruction : UINT32_MAX, .receiver_function = receiver->declaration,
        .receiver_value = value < receiver->values ? value : UINT32_MAX,
        .registration = UINT32_MAX, .environment = UINT32_MAX, .reference = UINT32_MAX, .capture_environment = UINT32_MAX,
        .remainder = pending->remainder ? pending->remainder : XIR_EXECUTION_REMAINDER_STORAGE,
        .port = XIR_EXECUTION_NORMAL, .signature = pending->receiver_type,
        .protocol = 2, .unresolved = true};
}

static XirExecutionSite execution_cleanup_site(const EffectCheckedRoots *roots, uint32_t index) {
    const EffectCheckCleanupUse *use = &roots->cleanup_use[index];
    XirExecutionSite site = execution_call_site(roots, use->registration);
    site.kind = XIR_EXECUTION_CLEANUP_USE;
    site.index = index;
    site.instruction = use->instruction;
    site.block = use->block;
    site.order = use->order;
    site.port = use->port;
    site.registration = use->registration;
    return site;
}

XR_FUNC XrXirStatus xir_execution_site(const XirExecutionOwner *owner,
    XirExecutionSiteKind kind, uint32_t index, XirExecutionSite *output) {
    const EffectCheckedRoots *roots = owner;
    if (!roots || !output) return XR_XIR_BAD_STRUCTURE;
    if (kind == XIR_EXECUTION_CALL) {
        if (index >= roots->calls) return XR_XIR_BAD_STRUCTURE;
        *output = execution_call_site(roots, index);
    } else if (kind == XIR_EXECUTION_PENDING) {
        if (index >= roots->pending_count) return XR_XIR_BAD_STRUCTURE;
        *output = execution_pending_site(roots, index);
    } else if (kind == XIR_EXECUTION_CLEANUP_USE) {
        if (index >= roots->cleanup_uses) return XR_XIR_BAD_STRUCTURE;
        *output = execution_cleanup_site(roots, index);
    } else return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_execution_visit_calls(const XirExecutionOwner *owner,
    XirExecutionVisitor visit, void *context) {
    const EffectCheckedRoots *roots = owner;
    if (!roots || !visit) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t c = 0; c < roots->calls; ++c) {
        const EffectCheckCall *call = &roots->call[c];
        const EffectCheckFunction *caller = &roots->function[roots->node[call->output].function];
        const EffectCheckFunction *callee = &roots->function[roots->node[call->target].function];
        if (call->protocol != 2 || call->output != caller->root || call->target != callee->root) continue;
        XirExecutionSite site = execution_call_site(roots, c);
        XrXirStatus status = visit(context, &site);
        if (status != XR_XIR_OK) return status;
    }
    for (uint32_t c = 0; c < roots->cleanup_uses; ++c) {
        XirExecutionSite site = execution_cleanup_site(roots, c);
        XrXirStatus status = visit(context, &site);
        if (status != XR_XIR_OK) return status;
    }
    for (uint32_t p = 0; p < roots->pending_count; ++p) {
        const EffectCheckPending *pending = &roots->pending[p];
        if (pending->linked || (pending->kind != EFFECT_CHECK_HIGHER_ORDER &&
            pending->kind != EFFECT_CHECK_REQUIREMENT)) continue;
        XirExecutionSite site = execution_pending_site(roots, p);
        XrXirStatus status = visit(context, &site);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_execution_visit_captures(const XirExecutionOwner *owner,
    XirExecutionVisitor visit, void *context) {
    const EffectCheckedRoots *roots = owner;
    if (!roots || !visit) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t c = 0; c < roots->calls; ++c) {
        const EffectCheckCall *call = &roots->call[c];
        const EffectCheckFunction *callee = &roots->function[roots->node[call->target].function];
        if ((call->protocol != 1 && call->protocol != 4) || call->target != callee->root) continue;
        XirExecutionSite site = execution_call_site(roots, c);
        if (call->protocol == 4) site.registration = c;
        XrXirStatus status = visit(context, &site);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_execution_argument(const XirExecutionOwner *owner,
    const XirExecutionSite *site, uint32_t parameter, XirExecutionArgument *output) {
    const EffectCheckedRoots *roots = owner;
    if (!roots || !site || !output) return XR_XIR_BAD_STRUCTURE;
    uint32_t first, count, caller, target = UINT32_MAX;
    uint32_t formal_ordinal = parameter;
    uint32_t environment = site->environment;
    if (site->kind == XIR_EXECUTION_CALL || site->kind == XIR_EXECUTION_CLEANUP_USE) {
        uint32_t index = site->index;
        if (site->kind == XIR_EXECUTION_CLEANUP_USE) {
            if (index >= roots->cleanup_uses) return XR_XIR_BAD_STRUCTURE;
            index = roots->cleanup_use[index].registration;
        }
        if (index >= roots->calls) return XR_XIR_BAD_STRUCTURE;
        const EffectCheckCall *call = &roots->call[index];
        first = call->first; count = call->count;
        caller = roots->node[call->output].function;
        target = roots->node[call->target].function;
    } else if (site->kind == XIR_EXECUTION_PENDING) {
        if (site->index >= roots->pending_count) return XR_XIR_BAD_STRUCTURE;
        const EffectCheckPending *pending = &roots->pending[site->index];
        first = pending->argument_first; count = pending->argument_count; caller = pending->function;
        if (site->reference != UINT32_MAX) {
            if (site->reference >= roots->calls) return XR_XIR_BAD_STRUCTURE;
            EffectCheckCall capture = roots->call[site->reference];
            if (capture.protocol != 1 || capture.count > UINT32_MAX - count ||
                site->argument_count != capture.count + count) return XR_XIR_BAD_STRUCTURE;
            target = roots->node[capture.target].function;
            if (parameter < capture.count) {
                first = capture.first; count = capture.count;
                environment = site->capture_environment;
            } else parameter -= capture.count;
        }
    } else return XR_XIR_UNRESOLVED;
    if (parameter >= count || first > roots->arguments || count > roots->arguments - first)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t node = roots->argument[first + parameter];
    if (node >= roots->nodes) return XR_XIR_BAD_STRUCTURE;
    const EffectCheckFunction *source = &roots->function[roots->node[node].function];
    uint32_t value = node - source->first;
    if (value >= source->values) return XR_XIR_UNRESOLVED;
    XirExecutionArgument argument = {.function = source->declaration, .value = value,
        .caller_handle = roots->node[node].function == caller && environment == site->environment ? value : UINT32_MAX,
        .environment = roots->node[node].function == caller || environment != XIR_EXECUTION_CURRENT_ENVIRONMENT ?
            environment : XIR_EXECUTION_UNKNOWN_ENVIRONMENT,
        .mode = XR_PARAM_READ, .type = roots->node[node].type,
        .cell = roots->node[node].cell};
    if (target != UINT32_MAX) {
        if (formal_ordinal >= roots->function[target].parameters) return XR_XIR_BAD_STRUCTURE;
        const EffectCheckNode *formal = &roots->node[roots->function[target].first + formal_ordinal];
        argument.mode = formal->mode;
        argument.escaping = formal->escaping;
    } else {
        const EffectCheckPending *pending = &roots->pending[site->index];
        const XrXirTypes *types = xir_execution_types(roots);
        const XrXirTypeNode *signature = xr_xir_callable_signature(types, pending->receiver_type);
        uint32_t ordinal = parameter;
        if (pending->kind == EFFECT_CHECK_REQUIREMENT) {
            if (!ordinal) { *output = argument; return XR_XIR_OK; }
            --ordinal;
            const XrXirInterfaceTable *table = types ? types->interfaces : NULL;
            if (!table || pending->selector >= table->count ||
                pending->member >= table->declarations[pending->selector].method_count) return XR_XIR_BAD_STRUCTURE;
            signature = xr_xir_callable_signature(types,
                table->declarations[pending->selector].methods[pending->member].signature);
        }
        if (!signature || ordinal >= signature->parameter_count) return XR_XIR_BAD_TYPE;
        argument.mode = signature->parameters[ordinal].mode;
        argument.escaping = signature->parameters[ordinal].escaping != 0;
    }
    *output = argument;
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_execution_visit_value_captures(const XirExecutionOwner *owner,
    uint32_t function, uint32_t value, XirExecutionVisitor visit, void *context, bool *complete) {
    XirExecutionValue reference = {function, value, XIR_EXECUTION_CURRENT_ENVIRONMENT};
    return xir_execution_visit_value_captures_in(owner, &reference, visit, context, complete, NULL);
}

#include "xxir_execution_origins.inc.c"

XR_FUNC const XrXirTypes *xir_execution_types(const XirExecutionOwner *owner) {
    const EffectCheckedRoots *roots = owner;
    return roots ? (roots->terms ? &roots->terms->types : roots->borrowed_types) : NULL;
}
