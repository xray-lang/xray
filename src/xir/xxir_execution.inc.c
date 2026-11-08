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

XR_FUNC XrXirStatus xir_execution_parameter(const XirExecutionOwner *owner,
    const XirExecutionSite *site, uint32_t parameter, XirExecutionParameter *output) {
    const EffectCheckedRoots *roots = owner;
    if (!roots || !site || !output) return XR_XIR_BAD_STRUCTURE;
    bool input = site->kind == XIR_EXECUTION_FORMAL || site->kind == XIR_EXECUTION_CAPTURE_INPUT;
    if (!input && parameter >= site->argument_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t target = UINT32_MAX;
    if (input) {
        if (parameter || site->caller_instance >= roots->functions) return XR_XIR_BAD_STRUCTURE;
        target = site->caller_instance;
        const EffectCheckFunction *function = &roots->function[target];
        if (site->receiver_function != function->declaration || site->receiver_value >= function->parameters ||
            (site->receiver_value < function->capture_count) != (site->kind == XIR_EXECUTION_CAPTURE_INPUT) ||
            !roots->node[function->first + site->receiver_value].callable_input) return XR_XIR_BAD_STRUCTURE;
        parameter = site->receiver_value;
    } else if (site->kind == XIR_EXECUTION_CALL || site->kind == XIR_EXECUTION_CLEANUP_USE) {
        uint32_t call = site->index;
        if (site->kind == XIR_EXECUTION_CLEANUP_USE) {
            if (call >= roots->cleanup_uses) return XR_XIR_BAD_STRUCTURE;
            call = roots->cleanup_use[call].registration;
        }
        if (call >= roots->calls) return XR_XIR_BAD_STRUCTURE;
        target = roots->node[roots->call[call].target].function;
    } else if (site->kind == XIR_EXECUTION_PENDING) {
        if (site->index >= roots->pending_count) return XR_XIR_BAD_STRUCTURE;
        if (site->reference != UINT32_MAX) {
            if (site->reference >= roots->calls || roots->call[site->reference].protocol != 1)
                return XR_XIR_BAD_STRUCTURE;
            target = roots->node[roots->call[site->reference].target].function;
        }
    } else return XR_XIR_BAD_STRUCTURE;
    if (target != UINT32_MAX) {
        const EffectCheckFunction *function = &roots->function[target];
        if (parameter >= function->parameters) return XR_XIR_BAD_STRUCTURE;
        EffectCheckNode node = roots->node[function->first + parameter];
        XrXirType declared = roots->node[roots->function[function->declaration].first + parameter].type;
        *output = (XirExecutionParameter){node.type, node.mode, node.escaping,
            parameter < function->capture_count, node.cell,
            (uint32_t)declared >= XR_XIR_TYPE_PARAMETER_BASE && (uint32_t)declared < XR_XIR_TYPE_PARAMETER_LIMIT};
        return XR_XIR_OK;
    }
    const EffectCheckPending *pending = &roots->pending[site->index];
    const XrXirTypes *types = xir_execution_types(roots);
    const XrXirTypeNode *signature = xr_xir_callable_signature(types, pending->receiver_type);
    if (pending->kind == EFFECT_CHECK_REQUIREMENT) {
        if (!parameter) {
            EffectCheckNode node = roots->node[pending->receiver];
            *output = (XirExecutionParameter){node.type, XR_PARAM_READ, false, false, node.cell, false};
            return XR_XIR_OK;
        }
        --parameter;
        const XrXirInterfaceTable *table = types ? types->interfaces : NULL;
        if (!table || pending->selector >= table->count ||
            pending->member >= table->declarations[pending->selector].method_count) return XR_XIR_BAD_STRUCTURE;
        signature = xr_xir_callable_signature(types,
            table->declarations[pending->selector].methods[pending->member].signature);
    }
    if (!signature || parameter >= signature->parameter_count) return XR_XIR_BAD_TYPE;
    XrXirCallableParameter formal = signature->parameters[parameter];
    *output = (XirExecutionParameter){formal.type, formal.mode, formal.escaping != 0, false,
        xr_xir_type_is_cell(types, formal.type),
        (uint32_t)formal.type >= XR_XIR_TYPE_PARAMETER_BASE && (uint32_t)formal.type < XR_XIR_TYPE_PARAMETER_LIMIT};
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_execution_argument(const XirExecutionOwner *owner,
    const XirExecutionSite *site, uint32_t parameter, XirExecutionArgument *output) {
    const EffectCheckedRoots *roots = owner;
    if (!roots || !site || !output) return XR_XIR_BAD_STRUCTURE;
    if (site->kind == XIR_EXECUTION_FORMAL || site->kind == XIR_EXECUTION_CAPTURE_INPUT) {
        if (parameter || site->caller_instance >= roots->functions) return XR_XIR_BAD_STRUCTURE;
        const EffectCheckFunction *function = &roots->function[site->caller_instance];
        if (site->receiver_function != function->declaration || site->receiver_value >= function->parameters ||
            (site->receiver_value < function->capture_count) != (site->kind == XIR_EXECUTION_CAPTURE_INPUT))
            return XR_XIR_BAD_STRUCTURE;
        const EffectCheckNode *node = &roots->node[function->first + site->receiver_value];
        if (!node->callable_input) return XR_XIR_BAD_STRUCTURE;
        *output = (XirExecutionArgument){.function = function->declaration, .value = site->receiver_value,
            .caller_handle = site->receiver_value, .environment = site->environment,
            .mode = node->mode, .type = node->type, .cell = node->cell, .escaping = node->escaping};
        return XR_XIR_OK;
    }
    uint32_t first, count, caller;
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
    } else if (site->kind == XIR_EXECUTION_PENDING) {
        if (site->index >= roots->pending_count) return XR_XIR_BAD_STRUCTURE;
        const EffectCheckPending *pending = &roots->pending[site->index];
        first = pending->argument_first; count = pending->argument_count; caller = pending->function;
        if (site->reference != UINT32_MAX) {
            if (site->reference >= roots->calls) return XR_XIR_BAD_STRUCTURE;
            EffectCheckCall capture = roots->call[site->reference];
            if (capture.protocol != 1 || capture.count > UINT32_MAX - count ||
                site->argument_count != capture.count + count) return XR_XIR_BAD_STRUCTURE;
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
    XirExecutionParameter formal = {0};
    XrXirStatus status = xir_execution_parameter(owner, site, formal_ordinal, &formal);
    if (status != XR_XIR_OK) return status;
    argument.mode = formal.mode;
    argument.escaping = formal.escaping;

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
