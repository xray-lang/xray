/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_check.inc.c - Parameterized module effects from direct SSA input
 *
 * KEY CONCEPT:
 *   Instructions contribute equations once. Function and value equations own
 *   numeric dependencies, never instructions or a replayable body snapshot.
 *   Recursive components close before callers consume their summaries.
 */
#include "../base/xhash.h"
#include "xxir_stringable_internal.h"
#include "xxir_implementation_verify.h"

#ifndef XXIR_EFFECT_CHECK_IMPLEMENTATION
#define XXIR_EFFECT_CHECK_IMPLEMENTATION

typedef struct EffectCheckFunction {
    uint32_t first, values, contents, parameters, words, root, result, result_contents, last, module, generic_parameters;
    uint32_t declaration, type_first, type_count, pending_first, pending_count;
    uint32_t native_callable, capture_count, cleanup_first, cleanup_count;
    uint64_t *bits, *suspend_bits;
    uint8_t throws;
    bool default_callback;
} EffectCheckFunction;

typedef struct EffectCheckNode {
    uint32_t function, dependencies, consumers, aliases, calls, last_call, bindings, elements;
    uint32_t component, reason[2];
    XrXirType type;
    uint32_t block, mode, cleanup_call, cleanup_parent;
    bool cell, escaping, weaken, erased, callable_opaque, callable_input, storable_value;
    uint32_t suspend_reason[2], suspend_call[2], suspend_parameter[2], suspend_instruction[2], suspend_distance[2], suspend_order[2];
    uint8_t cell_use, origin, bound, closed, suspend_bound;
    bool task_creation, restricted;
    bool queued, deferred, worker_blocked, cell_parameter, spawn, tuple, capture_contents, access_denied, value_upper, contents_value;
} EffectCheckNode;

typedef enum EffectCheckPendingKind {
    EFFECT_CHECK_REQUIREMENT, EFFECT_CHECK_CLASS_DISPATCH, EFFECT_CHECK_HIGHER_ORDER, EFFECT_CHECK_TUPLE_FIELD
} EffectCheckPendingKind;

typedef struct EffectCheckPending {
    uint32_t function, instruction, node, selector, member, result;
    uint32_t receiver, argument_first, argument_count, type_first, type_count;
    XrXirType receiver_type, result_type;
    EffectCheckPendingKind kind;
    bool linked, root_linked;
    uint32_t remainder;
} EffectCheckPending;

typedef struct EffectCheckDispatch {
    XrXirType receiver;
    uint32_t slot, first, count;
} EffectCheckDispatch;

typedef struct EffectCheckDependency {
    uint32_t source, target, next_source, next_target;
} EffectCheckDependency;

typedef struct EffectCheckAlias {
    uint32_t source, next, instruction;
    bool suspension, root, callable_origin;
} EffectCheckAlias;

typedef struct EffectCheckBinding {
    uint32_t source, next;
} EffectCheckBinding;

typedef struct EffectCheckElement {
    uint32_t source, field, next;
} EffectCheckElement;

typedef struct EffectCheckCall {
    uint32_t target, first, count, next, instruction, output;
    uint32_t environment;
    bool has_environment;
    uint32_t type_first, type_count;
    uint8_t protocol;
    bool default_result, fixed_effects;
} EffectCheckCall;

typedef struct EffectCheckCleanupExit {
    uint32_t instruction, block, from, to;
    XirExecutionPort port;
} EffectCheckCleanupExit;

typedef struct EffectCheckCleanupUse {
    uint32_t function, instruction, block, order, registration;
    XirExecutionPort port;
} EffectCheckCleanupUse;

typedef struct EffectCheckOriginEdge {
    uint32_t function, value, application, next;
    bool parameter_contents;
} EffectCheckOriginEdge;

typedef struct EffectCheckOrigins {
    uint32_t *offsets, *heads;
    EffectCheckOriginEdge *edges;
    uint32_t nodes, count;
} EffectCheckOrigins;

typedef struct EffectCheckRestrictions {
    XirRestrictionView view;
    uint32_t *intrinsic, *owners;
    XrXirValueFlow *flows;
} EffectCheckRestrictions;

typedef struct EffectCheckFrame {
    uint32_t node, dependency, parent;
} EffectCheckFrame;

typedef struct EffectCheckCause {
    uint32_t function;
    XrXirRootEffectWitness witness;
} EffectCheckCause;

typedef struct EffectCheckedRoots {
    XrCompileResources *resources;
    uint32_t functions, nodes, dependencies, aliases, calls, arguments;
    uint32_t dependency_capacity, alias_capacity, call_capacity, argument_capacity;
    uint32_t modules, pending_count, pending_capacity;
    uint32_t type_arguments, type_argument_capacity;
    uint32_t dispatches, dispatch_capacity, dispatch_targets, dispatch_target_capacity;
    uint32_t *dispatch_index;
    uint32_t dispatch_index_capacity;
    uint32_t templates, function_capacity, node_capacity, terminal_capacity;
    uint32_t bindings, binding_capacity;
    uint32_t elements, element_capacity;
    uint32_t cleanup_exits, cleanup_exit_capacity, cleanup_uses, cleanup_use_capacity;
    uint32_t stringable_interface, stringable_integer, stringable_string;
    EffectCheckFunction *function;
    EffectCheckNode *node;
    EffectCheckDependency *dependency;
    EffectCheckAlias *alias;
    EffectCheckBinding *binding;
    EffectCheckElement *element;
    EffectCheckCall *call;
    EffectCheckCleanupExit *cleanup_exit;
    EffectCheckCleanupUse *cleanup_use;
    uint32_t *argument;
    uint32_t *module_offsets, *module_functions;
    EffectCheckPending *pending;
    EffectCheckDispatch *dispatch;
    XirClassMethodTarget *dispatch_target;
    XrXirType *type_argument;
    XrXirRootEffectWitness *terminals;
    EffectTerms *terms;
    const XrXirTypes *borrowed_types;
    XirStorageFlow *storage;
    EffectCheckOrigins storage_origins;
    EffectCheckRestrictions restrictions;
    bool liveness_checked;
} EffectCheckedRoots;

typedef struct EffectCheckBuild {
    const XrXirCompileContext *memory;
    const XrXirModule *module;
    const XrXirFunctionEffectContract *contracts;
    XrCompileRequestControl *control;
    EffectCheckedRoots *roots;
    const XrXirRootRefiner *refiner;
    XirReadScratch scratch;
    uint32_t *dispatch_index;
    uint32_t dispatch_index_capacity;
    uint32_t *instance_index;
    uint32_t instance_index_capacity;
    uint32_t progress;
    XrXirStatus status;
} EffectCheckBuild;

#include "xxir_effect_restrictions.inc.c"

static void effect_checked_roots_free(EffectCheckedRoots *roots) {
    if (!roots) return;
    effect_check_restrictions_clear(&roots->restrictions);
    xir_storage_flow_free(roots->storage);
    xr_compile_resources_free(roots->storage_origins.offsets);
    xr_compile_resources_free(roots->storage_origins.heads);
    xr_compile_resources_free(roots->storage_origins.edges);
    if (roots->function) for (uint32_t f = 0; f < roots->functions; ++f)
        {
            xr_compile_resources_free(roots->function[f].bits);
            xr_compile_resources_free(roots->function[f].suspend_bits);
        }
    xr_compile_resources_free(roots->function);
    xr_compile_resources_free(roots->node);
    xr_compile_resources_free(roots->dependency);
    xr_compile_resources_free(roots->alias);
    xr_compile_resources_free(roots->binding);
    xr_compile_resources_free(roots->element);
    xr_compile_resources_free(roots->call);
    xr_compile_resources_free(roots->cleanup_exit);
    xr_compile_resources_free(roots->cleanup_use);
    xr_compile_resources_free(roots->argument);
    xr_compile_resources_free(roots->module_offsets);
    xr_compile_resources_free(roots->module_functions);
    xr_compile_resources_free(roots->pending);
    xr_compile_resources_free(roots->dispatch);
    xr_compile_resources_free(roots->dispatch_target);
    xr_compile_resources_free(roots->dispatch_index);
    xr_compile_resources_free(roots->type_argument);
    xr_compile_resources_free(roots->terminals);
    if (roots->terms) effect_terms_free(roots->terms);
    xr_compile_resources_free(roots->terms);
    xr_compile_resources_free(roots);
}

static bool effect_check_poll(EffectCheckBuild *build) {
    if (build->status == XR_XIR_OK)
        build->status = xir_receive_step(build->control, &build->progress);
    return build->status == XR_XIR_OK;
}

static uint64_t *effect_check_bits(const EffectCheckedRoots *roots, uint32_t node) {
    const EffectCheckFunction *function = &roots->function[roots->node[node].function];
    return function->bits + (size_t)(node - function->first) * function->words;
}

static uint64_t *effect_check_suspend_bits(const EffectCheckedRoots *roots, uint32_t node) {
    const EffectCheckFunction *function = &roots->function[roots->node[node].function];
    return function->suspend_bits + (size_t)(node - function->first) * function->words;
}

static void effect_check_suspend_seed(EffectCheckedRoots *roots, uint32_t node,
    uint8_t mask, uint32_t instruction) {
    effect_check_suspend_bits(roots, node)[0] |= mask;
    for (uint32_t bit = 0; bit < 2; ++bit) {
        if (!(mask & (1u << bit)) || (roots->node[node].suspend_distance[bit] == 0 &&
            roots->node[node].suspend_order[bit] <= instruction)) continue;
        roots->node[node].suspend_reason[bit] = node;
        roots->node[node].suspend_order[bit] = instruction;
        roots->node[node].suspend_instruction[bit] = instruction;
        roots->node[node].suspend_distance[bit] = 0;
    }
}

static uint32_t effect_check_value(EffectCheckBuild *build, uint32_t function, uint32_t value) {
    if (function >= build->roots->functions || value >= build->roots->function[function].values) {
        build->status = XR_XIR_BAD_STRUCTURE;
        return UINT32_MAX;
    }
    return build->roots->function[function].first + value;
}

static bool effect_check_dependency(EffectCheckBuild *build, uint32_t source, uint32_t target) {
    EffectCheckedRoots *roots = build->roots;
    if (source >= roots->nodes || target >= roots->nodes) {
        build->status = XR_XIR_BAD_STRUCTURE;
        return false;
    }
    if (roots->dependencies == roots->dependency_capacity) {
        EffectCheckDependency *values = xir_compile_array_grow(build->memory, roots->dependency,
            roots->dependencies, sizeof(*values), &roots->dependency_capacity, &build->status);
        if (!values) return false;
        roots->dependency = values;
    }
    uint32_t index = roots->dependencies++;
    roots->dependency[index] = (EffectCheckDependency){source, target,
        roots->node[source].consumers, roots->node[target].dependencies};
    roots->node[source].consumers = roots->node[target].dependencies = index;
    return true;
}

static bool effect_check_alias(EffectCheckBuild *build, uint32_t target, uint32_t source) {
    EffectCheckedRoots *roots = build->roots;
    if (source >= roots->nodes || target >= roots->nodes ||
        roots->node[source].function != roots->node[target].function) {
        build->status = XR_XIR_BAD_STRUCTURE;
        return false;
    }
    if (roots->aliases == roots->alias_capacity) {
        EffectCheckAlias *values = xir_compile_array_grow(build->memory, roots->alias,
            roots->aliases, sizeof(*values), &roots->alias_capacity, &build->status);
        if (!values) return false;
        roots->alias = values;
    }
    uint32_t index = roots->aliases++;
    roots->alias[index] = (EffectCheckAlias){source, roots->node[target].aliases, UINT32_MAX,
        target != roots->function[roots->node[target].function].root, true, true};
    roots->node[target].aliases = index;
    return effect_check_dependency(build, source, target);
}

static bool effect_check_suspend_alias(EffectCheckBuild *build, uint32_t target, uint32_t source) {
    if (!effect_check_alias(build, target, source)) return false;
    EffectCheckAlias *alias = &build->roots->alias[build->roots->node[target].aliases];
    alias->root = false;
    alias->suspension = true;
    return true;
}

static const XrXirTypes *effect_check_types_read(const EffectCheckBuild *build);

static bool effect_check_suspend_arguments(EffectCheckBuild *build, uint32_t node,
    const XrXirTypeNode *signature, uint32_t first, uint32_t count) {
    if (!signature || count > signature->parameter_count) {
        build->status = XR_XIR_BAD_TYPE;
        return false;
    }
    for (uint32_t p = 0; p < count; ++p) {
        const XrXirCallableParameter *parameter = &signature->parameters[p];
        if (!parameter->escaping && xr_xir_type_is_callable(effect_check_types_read(build), parameter->type) &&
            !xr_xir_type_is_c_callable(effect_check_types_read(build), parameter->type) &&
            !effect_check_suspend_alias(build, node, build->roots->argument[first + p])) return false;
    }
    return true;
}

static bool effect_check_suspend_requirement(EffectCheckBuild *build,
    const EffectCheckPending *pending, const XrXirTypeNode *signature) {
    if (!signature || !pending->argument_count ||
        pending->argument_count - 1 != signature->parameter_count) {
        build->status = XR_XIR_BAD_TYPE;
        return false;
    }
    return effect_check_suspend_arguments(build, pending->node, signature,
        pending->argument_first + 1, pending->argument_count - 1);
}

static bool effect_check_element(EffectCheckBuild *build, uint32_t target, uint32_t field, uint32_t source) {
    EffectCheckedRoots *roots = build->roots;
    if (roots->elements == roots->element_capacity) {
        EffectCheckElement *values = xir_compile_array_grow(build->memory, roots->element,
            roots->elements, sizeof(*values), &roots->element_capacity, &build->status);
        if (!values) return false;
        roots->element = values;
    }
    uint32_t index = roots->elements++;
    roots->element[index] = (EffectCheckElement){source, field, roots->node[target].elements};
    roots->node[target].elements = index;
    return true;
}

static void effect_check_seed(EffectCheckedRoots *roots, uint32_t node,
    uint32_t mask, XrXirRootEffectWitness witness) {
    uint64_t *bits = effect_check_bits(roots, node);
    bits[0] |= mask;
    for (uint32_t bit = 0; bit < 2; ++bit) {
        if ((mask & (1u << bit)) && roots->node[node].reason[bit] == UINT32_MAX) {
            roots->node[node].reason[bit] = node;
            roots->terminals[(size_t)node * 2 + bit] = witness;
        }
    }
}

static uint32_t effect_check_type_mask(const XrXirTypes *types, XrXirType type) {
    const XrXirTypeNode *callable = xr_xir_callable_signature(types, type);
    if (!callable) return 0;
    return ((callable->flags & XR_XIR_CALLABLE_ROOT_REQUIRED) ? 1u : 0u) |
        ((callable->flags & XR_XIR_CALLABLE_ROOT_UNRESOLVED) ? 2u : 0u);
}

static EffectCheckNode effect_check_empty_node(uint32_t function) {
    return (EffectCheckNode){.function = function, .dependencies = UINT32_MAX,
        .consumers = UINT32_MAX, .aliases = UINT32_MAX, .calls = UINT32_MAX, .last_call = UINT32_MAX,
        .bindings = UINT32_MAX, .elements = UINT32_MAX, .component = UINT32_MAX,
        .reason = {UINT32_MAX, UINT32_MAX}, .block = UINT32_MAX, .cleanup_call = UINT32_MAX,
        .suspend_reason = {UINT32_MAX, UINT32_MAX}, .suspend_call = {UINT32_MAX, UINT32_MAX},
        .suspend_parameter = {UINT32_MAX, UINT32_MAX}, .suspend_instruction = {UINT32_MAX, UINT32_MAX},
        .suspend_distance = {UINT32_MAX, UINT32_MAX}, .suspend_order = {UINT32_MAX, UINT32_MAX}};
}

static XrXirStatus effect_check_allocate(EffectCheckBuild *build) {
    EffectCheckedRoots *roots = xir_compile_calloc(build->memory, 1, sizeof(*roots), &build->status);
    if (!roots) return build->status;
    build->roots = roots;
    roots->resources = build->memory->resources;
    roots->borrowed_types = build->module->types;
    roots->stringable_interface = roots->stringable_integer = roots->stringable_string = UINT32_MAX;
    roots->functions = build->module->function_count;
    roots->templates = roots->function_capacity = roots->functions;
    roots->modules = build->module->declarations ? build->module->declarations->module_count : 1;
    if (!roots->modules || (uint64_t)roots->modules + 1 > SIZE_MAX / sizeof(uint32_t))
        return XR_XIR_BAD_STRUCTURE;
    roots->function = xir_compile_calloc(build->memory, roots->functions,
        sizeof(*roots->function), &build->status);
    if (!roots->function) return build->status;
    uint64_t nodes = 0;
    for (uint32_t f = 0; f < roots->functions; ++f) {
        if (!effect_check_poll(build)) return build->status;
        const XrXirFunction *source = &build->module->functions[f];
        uint64_t values = (uint64_t)source->parameter_count + source->instruction_count;
        uint64_t words = ((uint64_t)source->parameter_count * 3 + 2 + 63) / 64;
        uint64_t expressions = values * 2 + 3;
        if (expressions > UINT32_MAX - nodes || words > UINT32_MAX ||
            expressions > SIZE_MAX / sizeof(uint64_t) / words) return XR_XIR_OVERFLOW;
        EffectCheckFunction *function = &roots->function[f];
        *function = (EffectCheckFunction){.first = (uint32_t)nodes, .values = (uint32_t)values,
            .contents = (uint32_t)(nodes + values),
            .parameters = source->parameter_count, .words = (uint32_t)words,
            .result_contents = (uint32_t)(nodes + values * 2),
            .root = (uint32_t)(nodes + values * 2 + 1), .result = (uint32_t)(nodes + values * 2 + 2),
            .last = (uint32_t)(nodes + expressions - 1),
            .declaration = f};
        if (build->module->declarations && build->module->declarations->functions) {
            const XrXirFunctionIdentity *identity = &build->module->declarations->functions[f];
            function->module = identity->module;
            function->native_callable = identity->native_callable;
            function->capture_count = identity->capture_count;
        }
        if (build->module->generics)
            function->generic_parameters = build->module->generics[f].parameter_count;
        function->bits = xir_compile_calloc(build->memory, (size_t)expressions * (size_t)words,
            sizeof(*function->bits), &build->status);
        function->suspend_bits = xir_compile_calloc(build->memory, (size_t)expressions * (size_t)words,
            sizeof(*function->suspend_bits), &build->status);
        if (!function->bits || !function->suspend_bits) return build->status;
        nodes += expressions;
    }
    roots->nodes = (uint32_t)nodes;
    roots->node_capacity = roots->terminal_capacity = roots->nodes;
    roots->node = xir_compile_calloc(build->memory, roots->nodes, sizeof(*roots->node), &build->status);
    roots->terminals = xir_compile_calloc(build->memory, roots->nodes,
        2 * sizeof(*roots->terminals), &build->status);
    if (!roots->node || !roots->terminals) return build->status;
    roots->module_offsets = xir_compile_calloc(build->memory, (size_t)roots->modules + 1,
        sizeof(*roots->module_offsets), &build->status);
    roots->module_functions = xir_compile_alloc(build->memory,
        (size_t)roots->functions * sizeof(*roots->module_functions), &build->status);
    if (!roots->module_offsets || !roots->module_functions) return build->status;
    for (uint32_t f = 0; f < roots->functions; ++f) {
        const EffectCheckFunction *function = &roots->function[f];
        if (function->module >= roots->modules) return XR_XIR_BAD_STRUCTURE;
        ++roots->module_offsets[function->module];
        for (uint32_t n = function->first; n <= function->last; ++n)
            roots->node[n] = effect_check_empty_node(f);
    }
    uint32_t total = 0;
    for (uint32_t m = 0; m < roots->modules; ++m) {
        uint32_t count = roots->module_offsets[m];
        roots->module_offsets[m] = total;
        total += count;
    }
    roots->module_offsets[roots->modules] = total;
    for (uint32_t f = 0; f < roots->functions; ++f) {
        uint32_t module = roots->function[f].module;
        roots->module_functions[roots->module_offsets[module]++] = f;
    }
    for (uint32_t m = roots->modules; m > 0; --m)
        roots->module_offsets[m] = roots->module_offsets[m - 1];
    roots->module_offsets[0] = 0;
    for (uint32_t d = 0; build->module->defaults && d < build->module->defaults->count; ++d) {
        const XrXirDefaultBinding *binding = &build->module->defaults->records[d];
        if (binding->owner_kind != XR_XIR_DEFAULT_PARAMETER || binding->function >= roots->functions ||
            binding->owner >= roots->functions) continue;
        const XrXirFunction *owner = &build->module->functions[binding->owner];
        if (binding->ordinal < owner->parameter_count &&
            xr_xir_type_is_callable(build->module->types, owner->parameters[binding->ordinal]) &&
            !xr_xir_type_is_c_callable(build->module->types, owner->parameters[binding->ordinal]) &&
            !xr_xir_function_parameter_escaping(owner, binding->ordinal))
            roots->function[binding->function].default_callback = true;
    }
    return XR_XIR_OK;
}

static bool effect_check_type_arguments(EffectCheckBuild *build, uint32_t function,
    const XrXirInstruction *op, uint32_t *first, uint32_t *count) {
    EffectCheckedRoots *roots = build->roots;
    const XrXirGeneric *generic = build->module->generics ? &build->module->generics[function] : NULL;
    uint32_t stored = generic ? generic->argument_count : 0;
    if (op->type_arguments[0] > stored || op->type_arguments[1] > stored - op->type_arguments[0]) {
        build->status = XR_XIR_BAD_STRUCTURE;
        return false;
    }
    *first = roots->type_arguments;
    *count = op->type_arguments[1];
    for (uint32_t a = 0; a < *count; ++a) {
        if (roots->type_arguments == roots->type_argument_capacity) {
            XrXirType *values = xir_compile_array_grow(build->memory, roots->type_argument,
                roots->type_arguments, sizeof(*values), &roots->type_argument_capacity, &build->status);
            if (!values) return false;
            roots->type_argument = values;
        }
        roots->type_argument[roots->type_arguments++] = generic->arguments[op->type_arguments[0] + a];
    }
    return true;
}

static void effect_check_array_bound(EffectCheckBuild *build, uint32_t function, uint32_t operand,
    XrXirType type, uint32_t instruction);

/* A naked binder can become callable after substitution. Keep its value
 * equations while the admitted body is available to the numeric producer. */
static bool effect_check_callable_result(const XrXirTypes *types, XrXirType type) {
    return xr_xir_type_is_callable(types, type) ||
        ((uint32_t)type >= XR_XIR_TYPE_PARAMETER_BASE && (uint32_t)type < XR_XIR_TYPE_PARAMETER_LIMIT);
}

static bool effect_check_array_result(const XrXirTypes *types, XrXirType type) {
    return xr_xir_type_is_array(types, type) ||
        ((uint32_t)type >= XR_XIR_TYPE_PARAMETER_BASE && (uint32_t)type < XR_XIR_TYPE_PARAMETER_LIMIT);
}

static bool effect_check_pending(EffectCheckBuild *build, uint32_t function,
    uint32_t instruction, uint32_t node, EffectCheckPendingKind kind) {
    EffectCheckedRoots *roots = build->roots;
    if (roots->pending_count == roots->pending_capacity) {
        EffectCheckPending *pending = xir_compile_array_grow(build->memory, roots->pending,
            roots->pending_count, sizeof(*pending), &roots->pending_capacity, &build->status);
        if (!pending) return false;
        roots->pending = pending;
    }
    const XrXirFunction *body = &build->module->functions[function];
    const XrXirInstruction *op = &body->instructions[instruction];
    EffectCheckPending pending = {.function = function, .instruction = instruction, .node = node,
        .selector = op->targets[0], .member = op->targets[1], .kind = kind,
        .argument_first = roots->arguments, .argument_count = op->args[1], .receiver = UINT32_MAX,
        .result = UINT32_MAX};
    if (kind != EFFECT_CHECK_CLASS_DISPATCH && effect_check_callable_result(build->module->types, op->type))
        pending.result = roots->function[function].first + body->parameter_count + instruction;
    uint32_t receiver = UINT32_MAX;
    if (kind == EFFECT_CHECK_CLASS_DISPATCH || kind == EFFECT_CHECK_TUPLE_FIELD) {
        pending.selector = (uint32_t)op->immediate;
        receiver = op->args[0];
    } else if (kind == EFFECT_CHECK_HIGHER_ORDER) receiver = (uint32_t)op->immediate;
    else if (op->args[1] && op->args[0] < body->operand_count) receiver = body->operands[op->args[0]];
    pending.receiver = effect_check_value(build, function, receiver);
    if (pending.receiver == UINT32_MAX) return false;
    pending.receiver_type = xr_xir_operand_type(body, receiver);
    pending.result_type = op->type;
    const uint32_t *arguments = &receiver;
    uint32_t argument_count = 1;
    if (kind == EFFECT_CHECK_TUPLE_FIELD) {
        pending.receiver = roots->function[function].contents + receiver;
        argument_count = 0;
    } else if (kind != EFFECT_CHECK_CLASS_DISPATCH) {
        if (op->args[0] > body->operand_count || op->args[1] > body->operand_count - op->args[0]) {
            build->status = XR_XIR_BAD_STRUCTURE;
            return false;
        }
        arguments = op->args[1] ? body->operands + op->args[0] : NULL;
        argument_count = op->args[1];
    }
    pending.argument_count = argument_count;
    if (argument_count && !arguments) {
        build->status = XR_XIR_BAD_STRUCTURE;
        return false;
    }
    for (uint32_t a = 0; a < argument_count; ++a) {
        uint32_t value = effect_check_value(build, function, arguments[a]);
        if (value == UINT32_MAX) return false;
        effect_check_array_bound(build, function, arguments[a], xr_xir_operand_type(body, arguments[a]), instruction);
        if (roots->arguments == roots->argument_capacity) {
            uint32_t *values = xir_compile_array_grow(build->memory, roots->argument,
                roots->arguments, sizeof(*values), &roots->argument_capacity, &build->status);
            if (!values) return false;
            roots->argument = values;
        }
        roots->argument[roots->arguments++] = value;
    }
    if (!effect_check_type_arguments(build, function, op, &pending.type_first, &pending.type_count)) return false;
    roots->pending[roots->pending_count++] = pending;
    roots->node[node].deferred = true;
    return true;
}

static uint32_t effect_check_actual_contents(const EffectCheckedRoots *roots, uint32_t actual) {
    const EffectCheckFunction *caller = &roots->function[roots->node[actual].function];
    XR_DCHECK(actual >= caller->first && actual - caller->first < caller->values,
        "Application actuals name admitted physical values");
    return caller->contents + actual - caller->first;
}

static bool effect_check_call_record(EffectCheckBuild *build, uint32_t output, EffectCheckCall call) {
    EffectCheckedRoots *roots = build->roots;
    if (output >= roots->nodes || call.target >= roots->nodes || call.first > roots->arguments ||
        call.count > roots->arguments - call.first) {
        build->status = XR_XIR_BAD_STRUCTURE;
        return false;
    }
    if (roots->calls == roots->call_capacity) {
        EffectCheckCall *values = xir_compile_array_grow(build->memory, roots->call,
            roots->calls, sizeof(*values), &roots->call_capacity, &build->status);
        if (!values) return false;
        roots->call = values;
    }
    uint32_t index = roots->calls++;
    /* Source order breaks equal first-cause choices without retaining bodies. */
    call.next = UINT32_MAX;
    call.output = output;
    roots->call[index] = call;
    if (roots->node[output].last_call == UINT32_MAX) roots->node[output].calls = index;
    else roots->call[roots->node[output].last_call].next = index;
    roots->node[output].last_call = index;
    EffectCheckFunction callee = roots->function[roots->node[call.target].function];
    uint8_t use = call.protocol == 8 ? 2 : call.protocol;
    /* Invoking a closure supplies its existing capture prefix. Those Cells
     * retain the capture role instead of acquiring a second ref-call role. */
    for (uint32_t p = 0; p < call.count; ++p)
        roots->node[callee.first + p].cell_use |=
            use == 2 && p < callee.capture_count ? 1u : use;
    if (call.protocol == 1) for (uint32_t p = call.count; p < callee.parameters; ++p)
        roots->node[callee.first + p].cell_use |= 2;
    if (!effect_check_dependency(build, call.target, output)) return false;
    for (uint32_t p = 0; p < call.count; ++p) {
        uint32_t actual = roots->argument[call.first + p];
        if (!effect_check_dependency(build, actual, output) ||
            !effect_check_dependency(build, effect_check_actual_contents(roots, actual), output)) return false;
    }
    return true;
}

#include "xxir_execution_collect.inc.c"

static bool effect_check_argument(EffectCheckBuild *build, uint32_t value) {
    EffectCheckedRoots *roots = build->roots;
    if (value >= roots->nodes) {
        build->status = XR_XIR_BAD_STRUCTURE;
        return false;
    }
    if (roots->arguments == roots->argument_capacity) {
        uint32_t *values = xir_compile_array_grow(build->memory, roots->argument,
            roots->arguments, sizeof(*values), &roots->argument_capacity, &build->status);
        if (!values) return false;
        roots->argument = values;
    }
    roots->argument[roots->arguments++] = value;
    return true;
}

typedef enum EffectCheckCallOutput {
    EFFECT_CHECK_OUTPUT_ROOT,
    EFFECT_CHECK_OUTPUT_VALUE,
    EFFECT_CHECK_OUTPUT_CONTENTS
} EffectCheckCallOutput;

static bool effect_check_call(EffectCheckBuild *build, uint32_t output,
    uint32_t target, const XrXirInstruction *op, uint32_t instruction, EffectCheckCallOutput result) {
    EffectCheckedRoots *roots = build->roots;
    uint32_t caller = roots->node[output].function;
    const XrXirFunction *function = &build->module->functions[caller];
    uint32_t operand_first = 0, argument_count = 0;
    if (!xr_xir_direct_argument_range(op, &operand_first, &argument_count) ||
        target >= roots->functions || operand_first > function->operand_count ||
        argument_count > function->operand_count - operand_first ||
        argument_count > roots->function[target].parameters) {
        build->status = XR_XIR_BAD_STRUCTURE;
        return false;
    }
    uint32_t first = roots->arguments;
    uint8_t protocol = op->op == XR_XIR_FUNCTION_REF || op->op == XR_XIR_GENERATOR_NEW ? 1u :
        op->op == XR_XIR_CLEANUP_REGISTER ? 4u : 2u;
    for (uint32_t p = 0; p < argument_count; ++p) {
        uint32_t operand = function->operands[operand_first + p];
        uint32_t value = effect_check_value(build, caller, operand);
        if (value == UINT32_MAX) return false;
        XrXirType formal = build->module->functions[target].parameters[p];
        /* A generic value or element transports its actual contents. Written
         * callable element types retain their advertised storage bound. */
        if (!xir_effect_parameter_identity(formal, XR_XIR_EFFECT_PARAMETER_FIXED) &&
            !xir_effect_parameter_identity(xr_xir_array_element(build->module->types, formal),
                XR_XIR_EFFECT_PARAMETER_FIXED))
            effect_check_array_bound(build, caller, operand, xr_xir_operand_type(function, operand), instruction);
        if (!effect_check_argument(build, value)) return false;
    }
    uint32_t callee = result == EFFECT_CHECK_OUTPUT_CONTENTS ? roots->function[target].result_contents :
        result == EFFECT_CHECK_OUTPUT_VALUE ? roots->function[target].result : roots->function[target].root;
    EffectCheckCall call = {.target = callee, .first = first, .count = argument_count,
        .instruction = instruction, .protocol = protocol,
        .default_result = result && (op->op == XR_XIR_CALL_DEFAULT || op->op == XR_XIR_INVOKE_DEFAULT)};
    if (!effect_check_type_arguments(build, caller, op, &call.type_first, &call.type_count)) return false;
    return effect_check_call_record(build, output, call);
}

static XrXirStatus effect_check_local(EffectCheckBuild *build, uint32_t function,
    uint32_t instruction, const XrXirInstruction *operation) {
    if (operation->op == XR_XIR_CALL_INDIRECT || operation->op == XR_XIR_INVOKE_INDIRECT ||
        operation->op == XR_XIR_CALL_REQUIREMENT) return XR_XIR_OK;
    XirMetadataDomain domain = xir_metadata_domain_from_module(build->module);
    EffectRootSeedRecord input = {&domain, operation, function, instruction, XR_XIR_UNIT};
    XrXirRootEffects facts = {0};
    XrXirRootEffectWitness root = {0}, unknown = {0};
    XrXirStatus status;
    for (;;) {
        status = effect_root_seed_output(&input, &facts, &root, &unknown, build->memory, &build->scratch);
        if (status != XR_XIR_BUDGET || !build->scratch.required) break;
        status = xir_read_scratch_grow(build->memory, &build->scratch);
        if (status != XR_XIR_OK) break;
        status = xir_read_scratch_begin(build->memory, &build->scratch);
        if (status != XR_XIR_OK) break;
    }
    if (status == XR_XIR_OK) {
        uint32_t node = build->roots->function[function].root;
        if (facts.requires_root) effect_check_seed(build->roots, node, 1, root);
        if (facts.unresolved) effect_check_seed(build->roots, node, 2, unknown);
    }
    return status;
}

static bool effect_check_copy(EffectCheckBuild *build, uint32_t function,
    uint32_t target, uint32_t source) {
    source = effect_check_value(build, function, source);
    if (source == UINT32_MAX || !effect_check_alias(build, target, source)) return false;
    EffectCheckFunction row = build->roots->function[function];
    if (target < row.first || target - row.first >= row.values) return true;
    uint32_t target_contents = row.contents + target - row.first;
    uint32_t source_contents = row.contents + source - row.first;
    if (!effect_check_alias(build, target_contents, source_contents)) return false;
    XrXirType type = xr_xir_operand_type(&build->module->functions[function], source - row.first);
    return !xr_xir_type_is_callable(build->module->types, xr_xir_array_element(build->module->types, type)) ||
        effect_check_alias(build, source_contents, target_contents);
}

static void effect_check_array_bound(EffectCheckBuild *build, uint32_t function, uint32_t operand,
    XrXirType type, uint32_t instruction) {
    XrXirType element = xr_xir_array_element(build->module->types, type);
    uint32_t mask = effect_check_type_mask(build->module->types, element);
    if (mask) effect_check_seed(build->roots, build->roots->function[function].contents + operand,
        mask, (XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_INDIRECT, instruction, UINT32_MAX, UINT32_MAX, 0});
}

static void effect_check_cell_seed(EffectCheckBuild *build, uint32_t node, const XrXirInstruction *op) {
    if (!xr_xir_type_is_cell(build->module->types, op->type)) return;
    uint8_t origin = XR_XIR_CELL_ORIGIN_UNKNOWN;
    switch (op->op) {
    case XR_XIR_CELL_NEW: case XR_XIR_CELL_NEW_TAKE:
        origin = XR_XIR_CELL_ORIGIN_OWNED;
        break;
    case XR_XIR_COPY: case XR_XIR_OWNED_RETAIN: case XR_XIR_LOCAL_NEW: case XR_XIR_LOCAL_READ:
    case XR_XIR_OWNED_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_READ: case XR_XIR_PHI:
    case XR_XIR_CELL_READ: case XR_XIR_CELL_TAKE: case XR_XIR_VALUE_TAKE: case XR_XIR_PLACE_READ:
        origin = 0;
        break;
    case XR_XIR_CELL_PROJECT: case XR_XIR_FIELD_PLACE: case XR_XIR_INDEX_PLACE: case XR_XIR_OBJECT_PLACE:
        origin = XR_XIR_CELL_ORIGIN_SCOPED;
        break;
    case XR_XIR_SLOT_PLACE:
        origin = XR_XIR_CELL_ORIGIN_MODULE;
        break;
    default: break;
    }
    build->roots->node[node].origin = origin;
}

static bool effect_check_instruction_metadata(EffectCheckBuild *build, uint32_t f, uint32_t i) {
    const XrXirFunction *function = &build->module->functions[f];
    const XrXirInstruction *op = &function->instructions[i];
    EffectCheckFunction row = build->roots->function[f];
    uint32_t value = row.first + function->parameter_count + i;
    build->roots->node[value].type = op->type;
    build->roots->node[value].cell = xr_xir_type_is_cell(build->module->types, op->type);
    build->roots->node[value].weaken = op->op == XR_XIR_FUNCTION_WEAKEN;
    build->roots->node[value].erased = op->op == XR_XIR_FUNCTION_WEAKEN;
    EffectCheckNode *contents = &build->roots->node[row.contents + function->parameter_count + i];
    contents->type = op->type;
    contents->contents_value = true;
    contents->value_upper = true;
    switch (op->op) {
    case XR_XIR_ARRAY_NEW: case XR_XIR_ARRAY_REPEAT: case XR_XIR_ARRAY_WITH_CAPACITY:
    case XR_XIR_ARRAY_RESERVE: case XR_XIR_CALL: case XR_XIR_INVOKE:
    case XR_XIR_CALL_DEFAULT: case XR_XIR_INVOKE_DEFAULT: case XR_XIR_INVOKE_RESULT:
    case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
    case XR_XIR_LOCAL_NEW: case XR_XIR_LOCAL_READ: case XR_XIR_SCALAR_LOCAL_NEW:
    case XR_XIR_SCALAR_LOCAL_READ: case XR_XIR_OWNED_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_READ:
    case XR_XIR_VALUE_TAKE: case XR_XIR_PHI:
        contents->value_upper = false; break;
    default: break;
    }
    switch (op->op) {
    case XR_XIR_FUNCTION_REF: case XR_XIR_CLASS_METHOD_REF:
    case XR_XIR_ARRAY_GET: case XR_XIR_TUPLE_FIELD:
    case XR_XIR_CALL: case XR_XIR_INVOKE: case XR_XIR_CALL_DEFAULT: case XR_XIR_INVOKE_DEFAULT:
    case XR_XIR_INVOKE_RESULT:
    case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
    case XR_XIR_LOCAL_NEW: case XR_XIR_LOCAL_READ: case XR_XIR_SCALAR_LOCAL_NEW:
    case XR_XIR_SCALAR_LOCAL_READ: case XR_XIR_OWNED_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_READ:
    case XR_XIR_VALUE_TAKE: case XR_XIR_PHI: break;
    default: build->roots->node[value].value_upper = true; break;
    }
    effect_check_cell_seed(build, value, op);
    XrXirFunctionEffects control = {0};
    if (effect_seed_opcode(op->op, &control) == EFFECT_SEED_INVALID) {
        build->status = XR_XIR_BAD_STRUCTURE;
        return false;
    }
    if (control.suspend == XR_XIR_EFFECT_MAY) effect_check_suspend_seed(build->roots, row.root, 2, i);
    if (xr_xir_op_is_spawn(op->op)) build->roots->node[row.root].task_creation = true;
    if (!effect_default_access_local(function,op) && !effect_default_access_scalar_slot(build->module,f,op))
        build->roots->node[row.root].access_denied=true;
    if (op->op == XR_XIR_SLOT_PLACE) build->roots->node[row.root].worker_blocked = true;
    const XrXirTypeNode *suspend_signature = xr_xir_callable_signature(build->module->types, op->type);
    bool callable = suspend_signature != NULL;
    switch (op->op) {
    case XR_XIR_CALL: case XR_XIR_CALL_INDIRECT: case XR_XIR_CALL_REQUIREMENT: case XR_XIR_CALL_DEFAULT:
    case XR_XIR_INVOKE_RESULT:
    case XR_XIR_SLOT_LOAD: case XR_XIR_ARRAY_GET: case XR_XIR_STRUCT_GET: case XR_XIR_CLASS_GET:
    case XR_XIR_ENUM_GET: case XR_XIR_MAP_INDEX: case XR_XIR_MAP_VALUE_AT:
    case XR_XIR_NULLABLE_UNWRAP:
        build->roots->node[value].storable_value = true;
        break;
    default: break;
    }
    if (callable) {
        switch (op->op) {
        case XR_XIR_FUNCTION_REF: case XR_XIR_CLASS_METHOD_REF:
        case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
        case XR_XIR_LOCAL_NEW: case XR_XIR_LOCAL_READ: case XR_XIR_SCALAR_LOCAL_NEW:
        case XR_XIR_SCALAR_LOCAL_READ: case XR_XIR_OWNED_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_READ:
        case XR_XIR_CELL_READ: case XR_XIR_CELL_TAKE: case XR_XIR_PLACE_READ:
        case XR_XIR_VALUE_TAKE: case XR_XIR_PHI: case XR_XIR_FUNCTION_WEAKEN:
        case XR_XIR_TUPLE_FIELD: case XR_XIR_CALL_DEFAULT: case XR_XIR_INVOKE_DEFAULT: break;
        default: build->roots->node[value].callable_opaque = true; break;
        }
    }
    if (callable && !(suspend_signature->flags & XR_XIR_CALLABLE_NO_SUSPEND)) {
        switch (op->op) {
        case XR_XIR_FUNCTION_REF: case XR_XIR_CLASS_METHOD_REF:
        case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
        case XR_XIR_LOCAL_NEW: case XR_XIR_LOCAL_READ: case XR_XIR_SCALAR_LOCAL_NEW:
        case XR_XIR_SCALAR_LOCAL_READ: case XR_XIR_OWNED_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_READ:
        case XR_XIR_CELL_READ: case XR_XIR_CELL_TAKE: case XR_XIR_VALUE_TAKE:
        case XR_XIR_PHI: case XR_XIR_FUNCTION_WEAKEN:
        case XR_XIR_CALL_DEFAULT: case XR_XIR_INVOKE_DEFAULT: break;
        default: effect_check_suspend_seed(build->roots, value, 1, i); break;
        }
    }
    if (op->op == XR_XIR_FUNCTION_WEAKEN) {
        uint32_t origin = effect_check_value(build, f, op->args[0]);
        if (origin == UINT32_MAX || !effect_check_alias(build, value, origin)) return false;
        build->roots->alias[build->roots->node[value].aliases].root = false;
        build->roots->alias[build->roots->node[value].aliases].suspension = false;
        bool restricted = op->args[0] < row.values && build->roots->node[row.first + op->args[0]].restricted;
        if (restricted || row.default_callback) {
            uint32_t source = effect_check_value(build, f, op->args[0]);
            if (source == UINT32_MAX || !effect_check_alias(build, value, source)) return false;
            build->roots->alias[build->roots->node[value].aliases].root = false;
        } else if (callable && !(suspend_signature->flags & XR_XIR_CALLABLE_NO_SUSPEND)) {
            effect_check_suspend_seed(build->roots, value, 1, i);
        }
    }
    switch (op->op) {
    case XR_XIR_ARRAY_NEW: case XR_XIR_ARRAY_REPEAT: case XR_XIR_ARRAY_WITH_CAPACITY:
    case XR_XIR_ARRAY_RESERVE: case XR_XIR_COPY: case XR_XIR_OWNED_RETAIN:
    case XR_XIR_LOCAL_NEW: case XR_XIR_LOCAL_READ: case XR_XIR_OWNED_LOCAL_NEW:
    case XR_XIR_OWNED_LOCAL_READ: case XR_XIR_PHI: break;
    default: effect_check_array_bound(build, f, function->parameter_count + i, op->type, i); break;
    }
    return true;
}

static bool effect_check_instruction(EffectCheckBuild *build, uint32_t f, uint32_t i) {
    const XrXirFunction *function = &build->module->functions[f];
    const XrXirInstruction *op = &function->instructions[i];
    EffectCheckFunction row = build->roots->function[f];
    uint32_t value = row.first + function->parameter_count + i;
    build->status = effect_check_local(build, f, i, op);
    if (build->status != XR_XIR_OK) return false;
    if (!effect_check_instruction_metadata(build, f, i)) return false;
    bool callable = effect_check_callable_result(build->module->types, op->type);
    switch (op->op) {
    case XR_XIR_GO: case XR_XIR_SCOPED_GO: {
        if (op->immediate < 0 || (uint64_t)op->immediate >= build->roots->templates) return false;
        build->roots->node[value].spawn = true;
        if (!effect_check_call(build, value, (uint32_t)op->immediate, op, i, EFFECT_CHECK_OUTPUT_ROOT)) return false;
        uint32_t call = build->roots->node[value].calls;
        build->roots->call[call].protocol = 8;
        return true;
    }
    case XR_XIR_CALL_REQUIREMENT:
        return effect_check_pending(build, f, i, row.root, EFFECT_CHECK_REQUIREMENT);
    case XR_XIR_CLASS_METHOD_REF:
        return effect_check_pending(build, f, i, value, EFFECT_CHECK_CLASS_DISPATCH);
    case XR_XIR_FUNCTION_REF:
        return op->immediate >= 0 && (uint64_t)op->immediate < build->roots->functions &&
            effect_check_call(build, value, (uint32_t)op->immediate, op, i, EFFECT_CHECK_OUTPUT_ROOT);
    case XR_XIR_CALL: case XR_XIR_INVOKE: case XR_XIR_CLEANUP_REGISTER:
        if (op->immediate < 0 || (uint64_t)op->immediate >= build->roots->functions) return false;
        if (!effect_check_call(build, row.root, (uint32_t)op->immediate, op, i, EFFECT_CHECK_OUTPUT_ROOT)) return false;
        if (op->op == XR_XIR_CLEANUP_REGISTER) {
            build->roots->node[value].cleanup_call = build->roots->node[row.root].last_call;
            uint32_t block = build->roots->node[value].block;
            if (block >= function->block_count) return false;
            build->roots->node[value].cleanup_parent = function->blocks[block].frontier;
        }
        if (callable && !effect_check_call(build, value, (uint32_t)op->immediate, op, i, EFFECT_CHECK_OUTPUT_VALUE)) return false;
        return !effect_check_array_result(build->module->types, op->type) ||
            effect_check_call(build, row.contents + function->parameter_count + i, (uint32_t)op->immediate, op, i, EFFECT_CHECK_OUTPUT_CONTENTS);
    case XR_XIR_CALL_DEFAULT: case XR_XIR_INVOKE_DEFAULT: {
        const uint32_t *identity = xr_xir_default_identity(op);
        const XrXirDefaultTable *defaults = build->module->defaults;
        uint32_t target = UINT32_MAX;
        for (uint32_t d = 0; defaults && d < defaults->count; ++d) {
            const XrXirDefaultBinding *binding = &defaults->records[d];
            if (binding->owner_kind == XR_XIR_DEFAULT_PARAMETER &&
                binding->owner == identity[0] && binding->ordinal == identity[1]) {
                target = binding->function;
                break;
            }
        }
        if (target == UINT32_MAX) return false;
        XrXirInstruction call = {.op = op->op, .type_arguments = {op->type_arguments[0], op->type_arguments[1]}};
        if (!effect_check_call(build, row.root, target, &call, i, EFFECT_CHECK_OUTPUT_ROOT)) return false;
        if (callable && !effect_check_call(build, value, target, &call, i, EFFECT_CHECK_OUTPUT_VALUE)) return false;
        return !effect_check_array_result(build->module->types, op->type) ||
            effect_check_call(build, row.contents + function->parameter_count + i, target, &call, i, EFFECT_CHECK_OUTPUT_CONTENTS);
    }
    case XR_XIR_GENERATOR_NEW:
        return op->immediate >= 0 && (uint64_t)op->immediate < build->roots->functions &&
            effect_check_call(build, value, (uint32_t)op->immediate, op, i, EFFECT_CHECK_OUTPUT_ROOT);
    case XR_XIR_ITERATOR_HAS_NEXT: case XR_XIR_ITERATOR_NEXT:
    case XR_XIR_ITERATOR_NTH: case XR_XIR_ITERATOR_CLOSE:
        if (!effect_check_copy(build, f, row.root, op->args[0])) return false;
        break;
    case XR_XIR_CALL_INDIRECT: case XR_XIR_INVOKE_INDIRECT:
        if (op->immediate < 0 || (uint64_t)op->immediate >= row.values) return false;
        return effect_check_pending(build, f, i, row.root, EFFECT_CHECK_HIGHER_ORDER);
    case XR_XIR_INVOKE_RESULT:
        return effect_check_copy(build, f, value, function->parameter_count + (uint32_t)op->immediate);
    case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
    case XR_XIR_LOCAL_NEW: case XR_XIR_LOCAL_READ: case XR_XIR_SCALAR_LOCAL_NEW:
    case XR_XIR_SCALAR_LOCAL_READ: case XR_XIR_OWNED_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_READ:
    case XR_XIR_CELL_PROJECT: case XR_XIR_CELL_PLACE: case XR_XIR_VALUE_TAKE:
        return effect_check_copy(build, f, value, op->args[0]);
    case XR_XIR_CELL_READ: case XR_XIR_CELL_TAKE: case XR_XIR_PLACE_READ: {
        uint32_t cell = effect_check_value(build, f, op->args[0]);
        if (cell == UINT32_MAX || !effect_check_alias(build, row.root, cell) ||
            !effect_check_alias(build, value, row.contents + op->args[0])) return false;
        build->roots->alias[build->roots->node[value].aliases].callable_origin = false;
        return true;
    }
    case XR_XIR_ARRAY_NEW: case XR_XIR_TUPLE_NEW: {
        uint32_t contents = row.contents + function->parameter_count + i;
        build->roots->node[contents].tuple = op->op == XR_XIR_TUPLE_NEW;
        if (op->args[0] > function->operand_count ||
            op->args[1] > function->operand_count - op->args[0]) return false;
        for (uint32_t p = 0; p < op->args[1]; ++p) {
            uint32_t operand = function->operands[op->args[0] + p];
            if (!effect_check_callable_result(build->module->types, xr_xir_operand_type(function, operand))) continue;
            uint32_t input = effect_check_value(build, f, operand);
            if (input == UINT32_MAX || !effect_check_element(build, contents, p, input) ||
                !effect_check_alias(build, contents, input)) return false;
        }
        return true;
    }
    case XR_XIR_ARRAY_GET:
        if (op->args[0] >= row.values) return false;
        return !callable || effect_check_alias(build, value, row.contents + op->args[0]);
    case XR_XIR_TUPLE_FIELD:
        return !callable || effect_check_pending(build, f, i, value, EFFECT_CHECK_TUPLE_FIELD);
    case XR_XIR_ARRAY_RESERVE:
        return effect_check_copy(build, f, value, op->args[0]);
    case XR_XIR_ARRAY_REPEAT:
        return effect_check_alias(build, row.contents + function->parameter_count + i,
            effect_check_value(build, f, op->args[1]));
    case XR_XIR_ARRAY_PUSH:
        if (op->args[0] >= row.values) return false;
        return effect_check_alias(build, row.contents + op->args[0], effect_check_value(build, f, op->args[1]));
    case XR_XIR_ARRAY_SET: {
        if (op->args[0] > function->operand_count || function->operand_count - op->args[0] < 3) return false;
        uint32_t array = function->operands[op->args[0]];
        if (array >= row.values) return false;
        return effect_check_alias(build, row.contents + array,
            effect_check_value(build, f, function->operands[op->args[0] + 2]));
    }
    case XR_XIR_LOCAL_WRITE: case XR_XIR_SCALAR_LOCAL_WRITE: case XR_XIR_OWNED_LOCAL_WRITE:
        return effect_check_copy(build, f, effect_check_value(build, f, op->args[0]), op->args[1]);
    case XR_XIR_PHI:
        if (op->args[0] > function->operand_count || op->args[1] > function->operand_count - op->args[0] ||
            op->args[1] % 2) return false;
        for (uint32_t p = 1; p < op->args[1]; p += 2)
            if (!effect_check_copy(build, f, value, function->operands[op->args[0] + p])) return false;
        return true;
    case XR_XIR_RETURN:
        if (effect_check_callable_result(build->module->types, function->result) &&
            !effect_check_copy(build, f, row.result, op->args[0])) return false;
        return !effect_check_array_result(build->module->types, function->result) ||
            effect_check_alias(build, row.result_contents, row.contents + op->args[0]);
    case XR_XIR_CELL_NEW: case XR_XIR_CELL_NEW_TAKE: {
        uint32_t input = effect_check_value(build, f, op->args[0]);
        return input != UINT32_MAX && effect_check_alias(build,
            row.contents + function->parameter_count + i, input);
    }
    case XR_XIR_CELL_WRITE: case XR_XIR_CELL_WRITE_TAKE: case XR_XIR_CELL_STRING_APPEND:
    case XR_XIR_CELL_LOCAL_WRITE: case XR_XIR_PLACE_WRITE:
    {
        uint32_t cell = effect_check_value(build, f, op->args[0]);
        uint32_t input = effect_check_value(build, f, op->args[1]);
        return cell != UINT32_MAX && input != UINT32_MAX &&
            effect_check_alias(build, row.root, cell) &&
            effect_check_alias(build, row.contents + op->args[0], input);
    }
    default: break;
    }
    if (callable) {
        uint32_t mask = effect_check_type_mask(build->module->types, op->type);
        effect_check_seed(build->roots, value, mask,
            (XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_INDIRECT, i, UINT32_MAX, UINT32_MAX, 0});
    }
    return true;
}

static XrXirStatus effect_check_collect(EffectCheckBuild *build) {
    for (uint32_t f = 0; f < build->roots->functions; ++f) {
        const XrXirFunction *body = &build->module->functions[f];
        EffectCheckFunction row = build->roots->function[f];
        build->roots->node[row.result].type = body->result;
        build->roots->node[row.result_contents].type = body->result;
        build->roots->node[row.result_contents].contents_value = true;
        build->roots->function[f].pending_first = build->roots->pending_count;
        XrXirType error = body->error_type;
        const XrXirDeclarations *identity = build->module->declarations;
        if (identity && identity->functions &&
            (identity->functions[f].promises & XR_XIR_FUNCTION_GENERATOR)) {
            const XrXirTypeNode *iterator = xr_xir_type_node(build->module->types, body->result);
            if (!iterator || iterator->kind != XR_XIR_TYPE_ITERATOR) return XR_XIR_BAD_TYPE;
            error = iterator->result;
        }
        if (error == XR_XIR_UNIT) return XR_XIR_BAD_TYPE;
        build->roots->function[f].throws = error == XR_XIR_NEVER ? XR_XIR_EFFECT_NONE : XR_XIR_EFFECT_MAY;
        row.default_callback = build->roots->function[f].default_callback;
        if (identity && identity->functions && xir_sys_pipe_can_suspend(identity->functions[f].native_callable))
            effect_check_suspend_seed(build->roots, row.root, 2, UINT32_MAX);
        for (uint32_t p = 0; p < body->parameter_count; ++p) {
            EffectCheckNode *parameter_node = &build->roots->node[row.first + p];
            parameter_node->type = body->parameters[p];
            parameter_node->cell = xr_xir_type_is_cell(build->module->types, body->parameters[p]);
            parameter_node->escaping = xr_xir_function_parameter_escaping(body, p);
            parameter_node->callable_input = xr_xir_type_is_callable(build->module->types, body->parameters[p]);
            parameter_node->mode = xr_xir_declared_parameter_mode(build->module->types, body->parameters[p],
                body->consume_parameters, body->consume_parameter_count, p);

            build->roots->node[row.contents + p].type = body->parameters[p];
            build->roots->node[row.contents + p].contents_value = true;
            build->roots->node[row.contents + p].bound = (uint8_t)effect_check_type_mask(build->module->types,
                xr_xir_array_element(build->module->types, body->parameters[p]));
            if (effect_check_array_result(build->module->types, body->parameters[p])) {
                uint32_t bit = body->parameter_count * 2u + p + 2u;
                effect_check_bits(build->roots, row.contents + p)[bit / 64] |= UINT64_C(1) << (bit % 64);
            }
            uint32_t parameter_type = (uint32_t)body->parameters[p];
            if (parameter_type >= XR_XIR_TYPE_PARAMETER_BASE && parameter_type < XR_XIR_TYPE_PARAMETER_LIMIT) {
                uint32_t bit = p + 2;
                effect_check_bits(build->roots, row.first + p)[bit / 64] |= UINT64_C(1) << (bit % 64);
            }
            effect_check_array_bound(build, f, p, body->parameters[p], 0);
            const XrXirTypeNode *callable = xr_xir_callable_signature(build->module->types, body->parameters[p]);
            if (callable) {
                uint8_t bound = callable->flags & XR_XIR_CALLABLE_NO_SUSPEND ? 0 : 1;
                build->roots->node[row.first + p].suspend_bound = bound;
                if (!xr_xir_function_parameter_escaping(body, p) &&
                    !xr_xir_type_is_c_callable(build->module->types, body->parameters[p])) {
                    uint32_t bit = p + 2;
                    effect_check_suspend_bits(build->roots, row.first + p)[bit / 64] |= UINT64_C(1) << (bit % 64);
                    /* The declaration states every non-escaping callback dependency,
                     * including a callback unused by this particular body. */
                    effect_check_suspend_bits(build->roots, row.root)[bit / 64] |= UINT64_C(1) << (bit % 64);
                } else effect_check_suspend_seed(build->roots, row.first + p, bound, UINT32_MAX);
            } else if (xr_xir_type_is_cell(build->module->types, body->parameters[p])) {
                const XrXirTypeNode *content = xr_xir_callable_signature(build->module->types,
                    xr_xir_cell_element(build->module->types, body->parameters[p]));
                if (content && !xr_xir_type_is_c_callable(build->module->types,
                    xr_xir_cell_element(build->module->types, body->parameters[p]))) {
                    uint32_t bit = body->parameter_count + p + 2;
                    build->roots->node[row.contents + p].suspend_bound =
                        content->flags & XR_XIR_CALLABLE_NO_SUSPEND ? 0 : 1;
                    effect_check_suspend_bits(build->roots, row.contents + p)[bit / 64] |= UINT64_C(1) << (bit % 64);
                }
            }
            uint32_t mask = effect_check_type_mask(build->module->types, body->parameters[p]);
            build->roots->node[row.first + p].bound = (uint8_t)mask;
            uint32_t kind = build->contracts ? build->contracts[f].parameters[p].kind : XR_XIR_EFFECT_PARAMETER_FIXED;
            if (callable && kind != XR_XIR_EFFECT_PARAMETER_FIXED) {
                uint32_t bit = p + 2;
                effect_check_bits(build->roots, row.first + p)[bit / 64] |= UINT64_C(1) << (bit % 64);
            } else if (callable) {
                effect_check_seed(build->roots, row.first + p,
                    effect_check_type_mask(build->module->types, body->parameters[p]),
                    (XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_INDIRECT, 0, UINT32_MAX, UINT32_MAX, 0});
            } else if (xr_xir_type_is_cell(build->module->types, body->parameters[p])) {
                build->roots->node[row.first + p].cell_parameter = true;
                build->roots->node[row.first + p].bound = 1;
                uint32_t bit = body->parameter_count + p + 2;
                effect_check_bits(build->roots, row.first + p)[bit / 64] |= UINT64_C(1) << (bit % 64);
                build->roots->node[row.first + p].origin = XR_XIR_CELL_ORIGIN_SCOPED;
                XrXirType element = xr_xir_cell_element(build->module->types, body->parameters[p]);
                effect_check_seed(build->roots, row.contents + p,
                    effect_check_type_mask(build->module->types, element),
                    (XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_INDIRECT, UINT32_MAX, UINT32_MAX, p, 0});
            }
        }
        const XrXirDeclarations *declarations = build->module->declarations;
        if (declarations && declarations->functions && declarations->modules &&
            row.module < declarations->module_count && declarations->modules[row.module].initializer == f)
            effect_check_seed(build->roots, row.root, 1,
                (XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_INITIALIZER, UINT32_MAX, UINT32_MAX, UINT32_MAX, 0});
        if (declarations && declarations->functions &&
            declarations->functions[f].native_callable == XR_NATIVE_CALLABLE_SYS_ON_SIGNAL)
            effect_check_seed(build->roots, row.root, 1,
                (XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_NATIVE, UINT32_MAX, UINT32_MAX, UINT32_MAX, 0});
        uint32_t block = 0;
        for (uint32_t i = 0; i < body->instruction_count; ++i) {
            if (!effect_check_poll(build)) return build->status;
            while (block + 1 < body->block_count && body->blocks[block + 1].first <= i) ++block;
            if (body->block_count) build->roots->node[row.first + row.parameters + i].block = block;
            if (build->roots->storage) {
                build->status = xir_storage_flow_observe(build->roots->storage, f, i, &body->instructions[i]);
                if (build->status != XR_XIR_OK) return build->status;
            }
            if (build->refiner && build->refiner->observe) {
                build->status = build->refiner->observe(build->refiner->context, build->module, f, i, &body->instructions[i]);
                if (build->status != XR_XIR_OK) return build->status;
            }
            if (!effect_check_instruction(build, f, i))
                return build->status == XR_XIR_OK ? XR_XIR_BAD_STRUCTURE : build->status;
        }
        if (build->roots->node[row.result_contents].aliases == UINT32_MAX) {
            uint32_t bound = effect_check_type_mask(build->module->types,
                xr_xir_array_element(build->module->types, body->result));
            effect_check_seed(build->roots, row.result_contents, bound,
                (XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_INDIRECT, UINT32_MAX, UINT32_MAX, UINT32_MAX, 0});
        }
        if (!effect_check_cleanup_exits(build, f)) return build->status;
        build->roots->function[f].pending_count = build->roots->pending_count -
            build->roots->function[f].pending_first;
    }
    return XR_XIR_OK;
}

/* Prelude scalar adapters are admitted with their module while bodies are
 * available. Linkers consume only the exported witness ordinals. */
static XrXirStatus effect_check_builtin_interfaces(EffectCheckBuild *build) {
    EffectCheckedRoots *roots = build->roots;
    XirStringableRead read = xir_stringable_read(build->control);
    if (read.status != XR_XIR_OK) return read.status;
    for (uint32_t p = 0; p < roots->pending_count; ++p) {
        EffectCheckPending pending = roots->pending[p];
        if (pending.kind != EFFECT_CHECK_REQUIREMENT) continue;
        XrXirInterfaceApplication application = {pending.selector, NULL, 0};
        uint32_t module = UINT32_MAX;
        if (!xir_stringable_schema(build->module->types, build->module->declarations,
                application, &module, &read)) {
            if (read.status != XR_XIR_OK) return read.status;
            continue;
        }
        XrXirStatus status = xir_stringable_witness(build->module, application, 0,
            XR_XIR_I64, &roots->stringable_integer, &read);
        if (status != XR_XIR_OK) return status;
        roots->stringable_interface = pending.selector;
        for (uint32_t f = 0; f < build->module->function_count; ++f) {
            if (!effect_check_poll(build)) return build->status;
            const XrXirFunctionIdentity *identity = &build->module->declarations->functions[f];
            const XrXirFunction *function = &build->module->functions[f];
            if (identity->module == module && !identity->nominal_owner &&
                f != build->module->declarations->modules[module].initializer &&
                function->parameter_count == 1 && function->parameters[0] == XR_XIR_STRING)
                roots->stringable_string = f;
        }
        return roots->stringable_string == UINT32_MAX ? XR_XIR_BAD_TYPE : XR_XIR_OK;
    }
    return XR_XIR_OK;
}

static bool effect_check_type_argument(EffectCheckBuild *build, XrXirType type) {
    EffectCheckedRoots *roots = build->roots;
    if (roots->type_arguments == roots->type_argument_capacity) {
        XrXirType *values = xir_compile_array_grow(build->memory, roots->type_argument,
            roots->type_arguments, sizeof(*values), &roots->type_argument_capacity, &build->status);
        if (!values) return false;
        roots->type_argument = values;
    }
    roots->type_argument[roots->type_arguments++] = type;
    return true;
}

static bool effect_check_types(EffectCheckBuild *build) {
    if (build->roots->terms) return true;
    EffectTerms *terms = xir_compile_calloc(build->memory, 1, sizeof(*terms), &build->status);
    if (!terms) return false;
    build->roots->terms = terms;
    terms->remaining = build->memory;
    build->status = effect_terms_seed(terms, build->module->types ? build->module->types : &effect_empty_types);
    return build->status == XR_XIR_OK;
}

static const XrXirTypes *effect_check_types_read(const EffectCheckBuild *build) {
    return build->roots->terms ? &build->roots->terms->types : build->module->types;
}

static bool effect_check_substitute(EffectCheckBuild *build, uint32_t function,
    XrXirType type, XrXirType *output) {
    EffectCheckFunction row = build->roots->function[function];
    if (!row.type_count) {
        *output = type;
        return true;
    }
    if (!effect_check_types(build)) return false;
    XrXirGeneric arguments = {.arguments = build->roots->type_argument + row.type_first,
        .argument_count = row.type_count};
    build->status = effect_terms_substitute(build->roots->terms, type, &arguments, output);
    return build->status == XR_XIR_OK;
}

static bool effect_check_substitute_arguments(EffectCheckBuild *build,
    uint32_t function, uint32_t first, uint32_t count, uint32_t *output) {
    *output = build->roots->type_arguments;
    for (uint32_t a = 0; a < count; ++a) {
        XrXirType type = build->roots->type_argument[first + a];
        if (!effect_check_substitute(build, function, type, &type) ||
            !effect_check_type_argument(build, type)) return false;
    }
    return true;
}

static uint32_t effect_check_instance_hash(const EffectCheckedRoots *roots,
    uint32_t declaration, uint32_t first, uint32_t count) {
    return xr_hash_int(declaration) ^ xr_hash_bytes(roots->type_argument + first,
        (size_t)count * sizeof(*roots->type_argument));
}

static bool effect_check_instance_index(EffectCheckBuild *build) {
    EffectCheckedRoots *roots = build->roots;
    if (roots->functions - roots->templates < build->instance_index_capacity / 2) return true;
    uint64_t capacity = build->instance_index_capacity ? (uint64_t)build->instance_index_capacity * 2 : 32;
    if (capacity > UINT32_MAX || capacity > SIZE_MAX / sizeof(uint32_t)) {
        build->status = XR_XIR_OVERFLOW;
        return false;
    }
    uint32_t *index = xir_compile_calloc(build->memory, (size_t)capacity, sizeof(*index), &build->status);
    if (!index) return false;
    for (uint32_t f = roots->templates; f < roots->functions; ++f) {
        EffectCheckFunction row = roots->function[f];
        uint32_t at = effect_check_instance_hash(roots, row.declaration, row.type_first, row.type_count) &
            ((uint32_t)capacity - 1);
        while (index[at]) at = (at + 1) & ((uint32_t)capacity - 1);
        index[at] = f + 1;
    }
    xr_compile_resources_free(build->instance_index);
    build->instance_index = index;
    build->instance_index_capacity = (uint32_t)capacity;
    return true;
}

static bool effect_check_instance_storage(EffectCheckBuild *build, uint32_t count) {
    EffectCheckedRoots *roots = build->roots;
    if (roots->functions == roots->function_capacity) {
        EffectCheckFunction *values = xir_compile_array_grow(build->memory, roots->function,
            roots->functions, sizeof(*values), &roots->function_capacity, &build->status);
        if (!values) return false;
        roots->function = values;
    }
    while (count > roots->node_capacity) {
        EffectCheckNode *values = xir_compile_array_grow(build->memory, roots->node,
            roots->nodes, sizeof(*values), &roots->node_capacity, &build->status);
        if (!values) return false;
        roots->node = values;
    }
    while (count > roots->terminal_capacity) {
        XrXirRootEffectWitness *values = xir_compile_array_grow(build->memory, roots->terminals,
            roots->nodes, 2 * sizeof(*values), &roots->terminal_capacity, &build->status);
        if (!values) return false;
        roots->terminals = values;
    }
    return true;
}

/* A generic instance binds numeric equations. It owns no executable body and
 * never fetches an instruction while resolving an interface requirement. */
static bool effect_check_instance_equations(EffectCheckBuild *build, uint32_t function) {
    EffectCheckedRoots *roots = build->roots;
    EffectCheckFunction row = roots->function[function];
    EffectCheckFunction source = roots->function[row.declaration];
    for (uint32_t n = source.first; n <= source.last; ++n) {
        uint32_t output = row.first + n - source.first;
        EffectCheckNode original = roots->node[n];
        for (uint32_t a = original.aliases; a != UINT32_MAX; a = roots->alias[a].next) {
            uint32_t input = row.first + roots->alias[a].source - source.first;
            if (!effect_check_alias(build, output, input)) return false;
            roots->alias[roots->node[output].aliases].suspension = roots->alias[a].suspension;
            roots->alias[roots->node[output].aliases].root = roots->alias[a].root;
            roots->alias[roots->node[output].aliases].instruction = roots->alias[a].instruction;
            roots->alias[roots->node[output].aliases].callable_origin = roots->alias[a].callable_origin;
        }
        for (uint32_t e = original.elements; e != UINT32_MAX; e = roots->element[e].next) {
            EffectCheckElement element = roots->element[e];
            uint32_t input = row.first + element.source - source.first;
            if (!effect_check_element(build, output, element.field, input)) return false;
        }
        for (uint32_t c = original.calls; c != UINT32_MAX; c = roots->call[c].next) {
            EffectCheckCall call = roots->call[c];
            uint32_t first = roots->arguments;
            for (uint32_t p = 0; p < call.count; ++p) {
                uint32_t input = row.first + roots->argument[call.first + p] - source.first;
                if (!effect_check_argument(build, input)) return false;
            }
            call.first = first;
            if (!effect_check_substitute_arguments(build, function, call.type_first,
                    call.type_count, &call.type_first) || !effect_check_call_record(build, output, call)) return false;
            if (call.protocol == 4 && call.instruction < row.values - row.parameters)
                roots->node[row.first + row.parameters + call.instruction].cleanup_call = roots->node[output].last_call;
        }
    }
    roots->function[function].pending_first = roots->pending_count;
    for (uint32_t p = 0; p < source.pending_count; ++p) {
        EffectCheckPending pending = roots->pending[source.pending_first + p];
        pending.function = function;
        pending.node = row.first + pending.node - source.first;
        pending.receiver = row.first + pending.receiver - source.first;
        if (pending.result != UINT32_MAX) pending.result = row.first + pending.result - source.first;
        pending.linked = pending.root_linked = false;
        uint32_t first = roots->arguments;
        for (uint32_t a = 0; a < pending.argument_count; ++a) {
            uint32_t input = row.first + roots->argument[pending.argument_first + a] - source.first;
            if (!effect_check_argument(build, input)) return false;
        }
        pending.argument_first = first;
        if (!effect_check_substitute(build, function, pending.receiver_type, &pending.receiver_type) ||
            !effect_check_substitute(build, function, pending.result_type, &pending.result_type) ||
            !effect_check_substitute_arguments(build, function, pending.type_first,
                pending.type_count, &pending.type_first)) return false;
        if (roots->pending_count == roots->pending_capacity) {
            EffectCheckPending *values = xir_compile_array_grow(build->memory, roots->pending,
                roots->pending_count, sizeof(*values), &roots->pending_capacity, &build->status);
            if (!values) return false;
            roots->pending = values;
        }
        roots->pending[roots->pending_count++] = pending;
    }
    roots->function[function].pending_count = source.pending_count;
    return true;
}

static uint32_t effect_check_instance(EffectCheckBuild *build,
    uint32_t declaration, uint32_t first, uint32_t count) {
    EffectCheckedRoots *roots = build->roots;
    if (!count) return declaration;
    const XrXirTypes *types = effect_check_types_read(build);
    for (uint32_t a = 0; a < count; ++a)
        if (xr_xir_type_span(types, roots->type_argument[first + a])) return declaration;
    if (count != roots->function[declaration].generic_parameters) {
        build->status = XR_XIR_BAD_STRUCTURE;
        return UINT32_MAX;
    }
    if (!effect_check_instance_index(build)) return UINT32_MAX;
    uint32_t mask = build->instance_index_capacity - 1;
    uint32_t at = effect_check_instance_hash(roots, declaration, first, count) & mask;
    while (build->instance_index[at]) {
        uint32_t index = build->instance_index[at] - 1;
        EffectCheckFunction row = roots->function[index];
        if (row.declaration == declaration && row.type_count == count &&
            !memcmp(roots->type_argument + row.type_first, roots->type_argument + first,
                (size_t)count * sizeof(*roots->type_argument))) return index;
        at = (at + 1) & mask;
    }
    EffectCheckFunction source = roots->function[declaration];
    uint32_t expressions = source.last - source.first + 1;
    if (expressions > UINT32_MAX - roots->nodes) {
        build->status = XR_XIR_OVERFLOW;
        return UINT32_MAX;
    }
    if (!effect_check_instance_storage(build, roots->nodes + expressions)) return UINT32_MAX;
    uint32_t index = roots->functions++;
    EffectCheckFunction row = source;
    row.first = roots->nodes;
    row.contents = row.first + source.contents - source.first;
    row.result_contents = row.first + source.result_contents - source.first;
    row.root = row.first + source.root - source.first;
    row.result = row.first + source.result - source.first;
    row.last = row.first + source.last - source.first;
    row.type_first = first;
    row.type_count = count;
    row.generic_parameters = 0;
    row.bits = NULL;
    row.suspend_bits = NULL;
    roots->function[index] = row;
    roots->function[index].bits = xir_compile_copy(build->memory, source.bits,
        (size_t)expressions * source.words * sizeof(*source.bits), &build->status);
    roots->function[index].suspend_bits = xir_compile_copy(build->memory, source.suspend_bits,
        (size_t)expressions * source.words * sizeof(*source.suspend_bits), &build->status);
    if (!roots->function[index].bits || !roots->function[index].suspend_bits) return UINT32_MAX;
    for (uint32_t n = 0; n < expressions; ++n) {
        EffectCheckNode node = roots->node[source.first + n];
        bool callable = xr_xir_type_is_callable(effect_check_types_read(build), node.type);
        if ((n < source.values || node.contents_value || n == source.result - source.first) &&
            !effect_check_substitute(build, index, node.type, &node.type)) return UINT32_MAX;
        if (node.contents_value) node.bound = (uint8_t)effect_check_type_mask(effect_check_types_read(build),
            xr_xir_array_element(effect_check_types_read(build), node.type));
        bool substituted_callable = (n < source.values || n == source.result - source.first) && !callable &&
            xr_xir_type_is_callable(effect_check_types_read(build), node.type);
        if (substituted_callable) node.bound = (uint8_t)effect_check_type_mask(effect_check_types_read(build), node.type);
        node.function = index;
        node.cell = xr_xir_type_is_cell(effect_check_types_read(build), node.type);
        if (n < source.parameters) node.callable_input = xr_xir_type_is_callable(effect_check_types_read(build), node.type);
        bool stored_callable = n < source.values && !callable &&
            xr_xir_type_is_callable(effect_check_types_read(build), node.type);
        if (stored_callable) {
            node.erased = true;
            node.bound = (uint8_t)effect_check_type_mask(effect_check_types_read(build), node.type);
            if (n < source.parameters) node.escaping = true;
        }
        node.dependencies = node.consumers = node.aliases = node.calls = node.last_call = node.bindings = node.elements = UINT32_MAX;
        for (uint32_t bit = 0; bit < 2; ++bit)
            if (node.reason[bit] != UINT32_MAX) node.reason[bit] = row.first + n;
        for (uint32_t bit = 0; bit < 2; ++bit)
            if (node.suspend_reason[bit] != UINT32_MAX) node.suspend_reason[bit] = row.first + n;
        roots->node[row.first + n] = node;
        memcpy(roots->terminals + (size_t)(row.first + n) * 2,
            roots->terminals + (size_t)(source.first + n) * 2, 2 * sizeof(*roots->terminals));
        bool opaque_result = n == source.result - source.first &&
            roots->node[source.first + n].aliases == UINT32_MAX && roots->node[source.first + n].calls == UINT32_MAX;
        bool opaque_contents = n == source.result_contents - source.first &&
            roots->node[source.first + n].aliases == UINT32_MAX;
        if (node.contents_value && (node.value_upper || opaque_contents))
            effect_check_seed(roots, row.first + n, node.bound,
                (XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_INDIRECT, UINT32_MAX, UINT32_MAX, n, 0});
        if (substituted_callable && ((n >= source.parameters && n < source.values && node.value_upper) || opaque_result))
            effect_check_seed(roots, row.first + n, node.bound,
                (XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_INDIRECT, UINT32_MAX, UINT32_MAX, n, 0});
        if (stored_callable) {
            const XrXirTypeNode *signature = xr_xir_callable_signature(effect_check_types_read(build), node.type);
            uint8_t bound = signature->flags & XR_XIR_CALLABLE_NO_SUSPEND ? 0 : 1;
            roots->node[row.first + n].suspend_bound = bound;
            effect_check_suspend_seed(roots, row.first + n, bound, UINT32_MAX);
        }
    }
    for (uint32_t p = 0; p < row.parameters; ++p) {
        XrXirType type = roots->node[row.first + p].type;
        if (!xr_xir_type_is_cell(effect_check_types_read(build), type)) continue;
        XrXirType element = xr_xir_cell_element(effect_check_types_read(build), type);
        const XrXirTypeNode *signature = xr_xir_callable_signature(effect_check_types_read(build), element);
        XrXirType original = roots->node[source.first + p].type;
        if (!signature || (xr_xir_type_is_cell(effect_check_types_read(build), original) &&
            xr_xir_type_is_callable(effect_check_types_read(build),
                xr_xir_cell_element(effect_check_types_read(build), original)))) continue;
        uint8_t bound = signature->flags & XR_XIR_CALLABLE_NO_SUSPEND ? 0 : 1;
        roots->node[row.contents + p].suspend_bound = bound;
        effect_check_suspend_seed(roots, row.contents + p, bound, UINT32_MAX);
        effect_check_seed(roots, row.contents + p, effect_check_type_mask(effect_check_types_read(build), element),
            (XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_INDIRECT, UINT32_MAX, UINT32_MAX, p, 0});
    }
    roots->nodes += expressions;
    build->instance_index[at] = index + 1;
    if (!effect_check_instance_equations(build, index)) return UINT32_MAX;
    return index;
}

static bool effect_check_bind_calls(EffectCheckBuild *build, uint32_t function) {
    EffectCheckedRoots *roots = build->roots;
    EffectCheckFunction row = roots->function[function];
    for (uint32_t n = row.first; n <= row.last; ++n) {
        for (uint32_t c = roots->node[n].calls; c != UINT32_MAX; c = roots->call[c].next) {
            EffectCheckCall call = roots->call[c];
            uint32_t callee = roots->node[call.target].function;
            uint32_t target = effect_check_instance(build, roots->function[callee].declaration,
                call.type_first, call.type_count);
            if (target == UINT32_MAX) return false;
            if (target == callee) continue;
            roots->call[c].target = call.target == roots->function[callee].root ? roots->function[target].root :
                call.target == roots->function[callee].result_contents ? roots->function[target].result_contents :
                roots->function[target].result;
            if (!effect_check_dependency(build, roots->call[c].target, n)) return false;
        }
    }
    return true;
}

static XrXirStatus effect_check_dispatch_target(void *owner, const XirClassMethodTarget *target) {
    EffectCheckBuild *build = owner;
    EffectCheckedRoots *roots = build->roots;
    if (target->function >= roots->functions) return XR_XIR_BAD_STRUCTURE;
    if (roots->dispatch_targets == roots->dispatch_target_capacity) {
        XirClassMethodTarget *values = xir_compile_array_grow(build->memory, roots->dispatch_target,
            roots->dispatch_targets, sizeof(*values), &roots->dispatch_target_capacity, &build->status);
        if (!values) return build->status;
        roots->dispatch_target = values;
    }
    roots->dispatch_target[roots->dispatch_targets++] = *target;
    return XR_XIR_OK;
}

static uint32_t effect_check_dispatch_hash(XrXirType receiver, uint32_t slot) {
    return xr_hash_int((int64_t)(((uint64_t)(uint32_t)receiver << 32) | slot));
}

static bool effect_check_dispatch_index(EffectCheckBuild *build) {
    EffectCheckedRoots *roots = build->roots;
    if (roots->dispatches < build->dispatch_index_capacity / 2) return true;
    uint64_t capacity = build->dispatch_index_capacity ? (uint64_t)build->dispatch_index_capacity * 2 : 32;
    if (capacity > UINT32_MAX || capacity > SIZE_MAX / sizeof(uint32_t)) {
        build->status = XR_XIR_OVERFLOW;
        return false;
    }
    uint32_t *index = xir_compile_calloc(build->memory, (size_t)capacity, sizeof(*index), &build->status);
    if (!index) return false;
    for (uint32_t d = 0; d < roots->dispatches; ++d) {
        EffectCheckDispatch row = roots->dispatch[d];
        uint32_t at = effect_check_dispatch_hash(row.receiver, row.slot) & ((uint32_t)capacity - 1);
        while (index[at]) at = (at + 1) & ((uint32_t)capacity - 1);
        index[at] = d + 1;
    }
    xr_compile_resources_free(build->dispatch_index);
    build->dispatch_index = index;
    build->dispatch_index_capacity = (uint32_t)capacity;
    return true;
}

/* Candidate identity is shared by every use of the same receiver and slot.
 * Linking consumes declaration headers and the recorded site arguments only. */
static uint32_t effect_check_dispatch(EffectCheckBuild *build, const EffectCheckPending *pending) {
    EffectCheckedRoots *roots = build->roots;
    if (!effect_check_dispatch_index(build)) return UINT32_MAX;
    uint32_t mask = build->dispatch_index_capacity - 1;
    uint32_t at = effect_check_dispatch_hash(pending->receiver_type, pending->selector) & mask;
    while (build->dispatch_index[at]) {
        uint32_t index = build->dispatch_index[at] - 1;
        EffectCheckDispatch row = roots->dispatch[index];
        if (row.receiver == pending->receiver_type && row.slot == pending->selector) return index;
        at = (at + 1) & mask;
    }
    if (roots->dispatches == roots->dispatch_capacity) {
        EffectCheckDispatch *values = xir_compile_array_grow(build->memory, roots->dispatch,
            roots->dispatches, sizeof(*values), &roots->dispatch_capacity, &build->status);
        if (!values) return UINT32_MAX;
        roots->dispatch = values;
    }
    uint32_t first = roots->dispatch_targets;
    XrXirModule module = *build->module;
    module.types = effect_check_types_read(build);
    build->status = xir_compile_class_method_candidates(&module, pending->receiver_type,
        pending->selector, effect_check_dispatch_target, build, build->control);
    if (build->status != XR_XIR_OK) return UINT32_MAX;
    uint32_t index = roots->dispatches++;
    roots->dispatch[index] = (EffectCheckDispatch){pending->receiver_type, pending->selector,
        first, roots->dispatch_targets - first};
    build->dispatch_index[at] = index + 1;
    return index;
}

static bool effect_check_link_target(EffectCheckBuild *build,
    const EffectCheckPending *pending, uint32_t target, uint8_t protocol) {
    EffectCheckedRoots *roots = build->roots;
    if (target >= roots->functions || pending->argument_count > roots->function[target].parameters) {
        build->status = XR_XIR_BAD_STRUCTURE;
        return false;
    }
    EffectCheckCall call = {.target = roots->function[target].root,
        .first = pending->argument_first, .count = pending->argument_count,
        .instruction = pending->instruction, .protocol = protocol,
        .type_first = pending->type_first, .type_count = pending->type_count};
    if (!effect_check_call_record(build, pending->node, call)) return false;
    if (pending->result != UINT32_MAX) {
        call.target = roots->function[target].result;
        if (!effect_check_call_record(build, pending->result, call)) return false;
    }
    return true;
}

static bool effect_check_link_requirement(EffectCheckBuild *build, EffectCheckPending *pending) {
    XrXirModule module = *build->module;
    module.types = effect_check_types_read(build);
    const XrXirInterfaceTable *table = module.types ? module.types->interfaces : NULL;
    if (!table || pending->selector >= table->count ||
        pending->member >= table->declarations[pending->selector].method_count) {
        build->status = XR_XIR_BAD_STRUCTURE;
        return false;
    }
    const XrXirInterfaceDeclaration *interface = &table->declarations[pending->selector];
    const XrXirInterfaceMethod *method = &interface->methods[pending->member];
    const XrXirTypeNode *signature = xr_xir_callable_signature(module.types, method->signature);
    if (!signature) {
        build->status = XR_XIR_BAD_TYPE;
        return false;
    }
    if (signature->flags & XR_XIR_CALLABLE_ROOT_REQUIRED)
        effect_check_seed(build->roots, pending->node, 1,
            (XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_REQUIREMENT,
                pending->instruction, UINT32_MAX, UINT32_MAX, 0});
    if (!(signature->flags & XR_XIR_CALLABLE_ROOT_UNRESOLVED)) {
        if (!(signature->flags & XR_XIR_CALLABLE_NO_SUSPEND))
            effect_check_suspend_seed(build->roots, pending->node, 1, pending->instruction);
        if (!effect_check_suspend_requirement(build, pending, signature)) return false;
        effect_check_seed(build->roots, pending->node,
            effect_check_type_mask(module.types, method->signature),
            (XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_REQUIREMENT,
                pending->instruction, UINT32_MAX, UINT32_MAX, 0});
        if (pending->result != UINT32_MAX)
            effect_check_seed(build->roots, pending->result,
                effect_check_type_mask(module.types, pending->result_type),
                (XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_INDIRECT,
                    pending->instruction, UINT32_MAX, UINT32_MAX, 0});
        pending->root_linked = true;
    }
    if (xr_xir_type_span(module.types, pending->receiver_type)) return true;
    uint32_t count = interface->parameter_count;
    if (pending->type_count < count) {
        build->status = XR_XIR_BAD_STRUCTURE;
        return false;
    }
    const XrXirType *types = count ? build->roots->type_argument + pending->type_first : NULL;
    for (uint32_t a = 0; a < pending->type_count; ++a)
        if (xr_xir_type_span(module.types, build->roots->type_argument[pending->type_first + a])) return true;
    XrXirProofContext context = {&module, {XR_XIR_CONTEXT_CLOSED, 0, 0}};
    XrXirWitnessRequest request = {build->module, pending->receiver_type,
        {pending->selector, types, count}, pending->member};
    XrXirWitness witness = {0};
    if (pending->receiver_type == XR_XIR_I64 || pending->receiver_type == XR_XIR_STRING) {
        const EffectCheckedRoots *roots = build->roots;
        if (pending->selector != roots->stringable_interface || pending->member || pending->type_count) {
            build->status = XR_XIR_BAD_TYPE;
            return false;
        }
        witness.function = pending->receiver_type == XR_XIR_I64 ?
            roots->stringable_integer : roots->stringable_string;
    } else build->status = xir_compile_witness_resolve_controlled(build->memory, &context,
        &request, &witness, build->control);
    if (build->status != XR_XIR_OK) return false;
    if (witness.function >= build->roots->templates) {
        build->status = XR_XIR_BAD_STRUCTURE;
        return false;
    }
    EffectCheckPending applied = *pending;
    applied.type_first = build->roots->type_arguments;
    uint32_t own = method->own_parameter_count;
    if (own > UINT32_MAX - witness.argument_count || own != pending->type_count - count) {
        build->status = XR_XIR_BAD_STRUCTURE;
        return false;
    }
    applied.type_count = witness.argument_count + own;
    for (uint32_t a = 0; a < witness.argument_count; ++a)
        if (!effect_check_type_argument(build, witness.arguments[a])) return false;
    for (uint32_t a = 0; a < own; ++a) {
        XrXirType type = build->roots->type_argument[pending->type_first + count + a];
        if (!effect_check_type_argument(build, type)) return false;
    }
    pending->linked = effect_check_link_target(build, &applied, witness.function, 2);
    return pending->linked;
}

static uint32_t effect_check_application_index(const EffectCheckedRoots *roots,
    const EffectCheckPending *pending, EffectCheckCall reference) {
    uint64_t count = (uint64_t)reference.count + pending->argument_count;
    for (uint32_t c = roots->node[pending->node].calls; c != UINT32_MAX; c = roots->call[c].next) {
        EffectCheckCall call = roots->call[c];
        if (call.protocol != 2 || call.instruction != pending->instruction || call.target != reference.target ||
            call.count != count || call.type_count != reference.type_count) continue;
        bool same = true;
        for (uint32_t p = 0; same && p < call.type_count; ++p)
            same = roots->type_argument[call.type_first + p] == roots->type_argument[reference.type_first + p];
        for (uint32_t p = 0; same && p < call.count; ++p) {
            uint32_t source = p < reference.count ? roots->argument[reference.first + p] :
                roots->argument[pending->argument_first + p - reference.count];
            same = roots->argument[call.first + p] == source;
        }
        if (same) return c;
    }
    return UINT32_MAX;
}

static bool effect_check_link_application(EffectCheckBuild *build,
    const EffectCheckPending *pending, EffectCheckCall reference) {
    EffectCheckedRoots *roots = build->roots;
    uint32_t target = roots->node[reference.target].function;
    uint64_t count = (uint64_t)reference.count + pending->argument_count;
    if (count != roots->function[target].parameters) {
        build->status = XR_XIR_BAD_STRUCTURE;
        return false;
    }
    if (effect_check_application_index(roots, pending, reference) != UINT32_MAX) return true;
    uint32_t first = roots->arguments;
    for (uint32_t p = 0; p < reference.count; ++p) {
        uint32_t argument = roots->argument[reference.first + p];
        if (!effect_check_argument(build, argument)) return false;
    }
    for (uint32_t p = 0; p < pending->argument_count; ++p) {
        uint32_t argument = roots->argument[pending->argument_first + p];
        if (!effect_check_argument(build, argument)) return false;
    }
    EffectCheckPending applied = *pending;
    applied.argument_first = first;
    applied.argument_count = (uint32_t)count;
    applied.type_first = reference.type_first;
    applied.type_count = reference.type_count;
    return effect_check_link_target(build, &applied, target, 2);
}

static bool effect_check_binding(EffectCheckBuild *build, uint32_t target, uint32_t source) {
    EffectCheckedRoots *roots = build->roots;
    if (roots->bindings == roots->binding_capacity) {
        EffectCheckBinding *values = xir_compile_array_grow(build->memory, roots->binding,
            roots->bindings, sizeof(*values), &roots->binding_capacity, &build->status);
        if (!values) return false;
        roots->binding = values;
    }
    uint32_t index = roots->bindings++;
    roots->binding[index] = (EffectCheckBinding){source, roots->node[target].bindings};
    roots->node[target].bindings = index;
    return effect_check_dependency(build, source, target);
}

static bool effect_check_capture_bindings(EffectCheckBuild *build) {
    EffectCheckedRoots *roots = build->roots;
    for (uint32_t c = 0; c < roots->calls; ++c) {
        EffectCheckCall call = roots->call[c];
        if (call.protocol != 1) continue;
        EffectCheckFunction callee = roots->function[roots->node[call.target].function];
        for (uint32_t p = 0; p < call.count; ++p) {
            uint32_t source = roots->argument[call.first + p], target = callee.first + p;
            if (!effect_check_binding(build, target, source)) return false;
            if (!roots->node[target].cell_parameter) continue;
            EffectCheckFunction caller = roots->function[roots->node[source].function];
            uint32_t contents = callee.contents + p;
            if (!effect_check_binding(build, contents, caller.contents + source - caller.first)) return false;
            roots->node[contents].capture_contents = true;
            effect_check_bits(roots, contents)[0] &= ~UINT64_C(3);
            roots->node[contents].reason[0] = roots->node[contents].reason[1] = UINT32_MAX;
        }
    }
    return true;
}

typedef struct EffectCheckResolve {
    EffectCheckBuild *build;
    EffectCheckPending *pending;
    bool foreign;
} EffectCheckResolve;

static XrXirStatus effect_check_resolved_capture(void *context, const XirExecutionSite *site) {
    EffectCheckResolve *resolve = context;
    EffectCheckedRoots *roots = resolve->build->roots;
    if (site->reference >= roots->calls) return XR_XIR_BAD_STRUCTURE;
    EffectCheckCall reference = roots->call[site->reference];
    if (reference.count && roots->node[reference.output].function != resolve->pending->function) {
        resolve->foreign = true;
        return XR_XIR_OK;
    }
    return effect_check_link_application(resolve->build, resolve->pending, reference) ?
        XR_XIR_OK : resolve->build->status;
}

static bool effect_check_link_higher_order(EffectCheckBuild *build, EffectCheckPending *pending) {
    XirExecutionSite site = {0};
    uint32_t index = (uint32_t)(pending - build->roots->pending);
    build->status = xir_execution_site(build->roots, XIR_EXECUTION_PENDING, index, &site);
    if (build->status != XR_XIR_OK) return false;
    EffectCheckResolve resolve = {build, pending, false};
    bool complete = false;
    uint32_t remainder = 0;
    build->status = xir_execution_resolve_site(build->roots, &site, XIR_EXECUTION_CURRENT_ENVIRONMENT,
        effect_check_resolved_capture, &resolve, &complete, &remainder);
    if (build->status != XR_XIR_OK) return false;
    pending->remainder = remainder | (resolve.foreign ? XIR_EXECUTION_REMAINDER_PARAMETER : 0);
    pending->linked = complete && !resolve.foreign;
    return true;
}

static XrXirStatus effect_check_precise_capture(void *context, const XirExecutionSite *site) {
    EffectCheckResolve *resolve = context;
    EffectCheckedRoots *roots = resolve->build->roots;
    if (site->reference >= roots->calls) return XR_XIR_BAD_STRUCTURE;
    EffectCheckCall reference = roots->call[site->reference];
    if (reference.count && roots->node[reference.output].function != resolve->pending->function) {
        resolve->foreign = true;
        return XR_XIR_OK;
    }
    uint32_t call = effect_check_application_index(roots, resolve->pending, reference);
    if (call == UINT32_MAX) return XR_XIR_UNRESOLVED;
    roots->call[call].fixed_effects = false;
    if (resolve->pending->result != UINT32_MAX && call + 1 < roots->calls &&
        roots->call[call + 1].output == resolve->pending->result) roots->call[call + 1].fixed_effects = false;
    return XR_XIR_OK;
}

static void effect_check_call_precision(EffectCheckedRoots *roots, uint32_t node, uint32_t instruction) {
    if (node == UINT32_MAX) return;
    for (uint32_t c = roots->node[node].calls; c != UINT32_MAX; c = roots->call[c].next)
        if (roots->call[c].protocol == 2 && roots->call[c].instruction == instruction)
            roots->call[c].fixed_effects = true;
}

/* Physical candidates were linked before HIR consumed the graph. Precision is
 * selected only afterward, without changing the actual execution relation. */
static XrXirStatus effect_check_effect_projection(EffectCheckBuild *build) {
    EffectCheckedRoots *roots = build->roots;
    for (uint32_t p = 0; p < roots->pending_count; ++p) {
        if (!effect_check_poll(build)) return build->status;
        EffectCheckPending *pending = &roots->pending[p];
        if (pending->kind != EFFECT_CHECK_HIGHER_ORDER) continue;
        effect_check_call_precision(roots, pending->node, pending->instruction);
        effect_check_call_precision(roots, pending->result, pending->instruction);
        XirExecutionSite site = {0};
        build->status = xir_execution_site(roots, XIR_EXECUTION_PENDING, p, &site);
        if (build->status != XR_XIR_OK) return build->status;
        EffectCheckResolve resolve = {build, pending, false};
        bool complete = false;
        uint32_t remainder = 0;
        build->status = xir_execution_resolve_effect_site(roots, &site,
            effect_check_precise_capture, &resolve, &complete, &remainder);
        if (build->status != XR_XIR_OK) return build->status;
        if ((!complete || resolve.foreign) && pending->result != UINT32_MAX)
            effect_check_seed(roots, pending->result,
                effect_check_type_mask(effect_check_types_read(build), pending->result_type),
                (XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_INDIRECT, pending->instruction,
                    UINT32_MAX, UINT32_MAX, 0});
        if (complete || !(remainder & XIR_EXECUTION_REMAINDER_OPAQUE)) continue;
        if (!pending->root_linked) {
            if (!effect_check_alias(build, pending->node, pending->receiver)) return build->status;
            EffectCheckAlias *alias = &roots->alias[roots->node[pending->node].aliases];
            alias->suspension = false;
            alias->instruction = pending->instruction;
            pending->root_linked = true;
        }
        const XrXirTypeNode *signature = xr_xir_callable_signature(effect_check_types_read(build), pending->receiver_type);
        if (!signature) return XR_XIR_BAD_TYPE;
        if (!effect_check_suspend_arguments(build, pending->node, signature,
            pending->argument_first, pending->argument_count)) return build->status;
        if (!(signature->flags & XR_XIR_CALLABLE_NO_SUSPEND))
            effect_check_suspend_seed(roots, pending->node, 1, pending->instruction);
    }
    return XR_XIR_OK;
}

static bool effect_check_link_tuple(EffectCheckBuild *build, EffectCheckPending *pending,
    uint32_t *visited, uint32_t *stack, uint32_t generation) {
    EffectCheckedRoots *roots = build->roots;
    uint32_t depth = 0;
    bool complete = true, found = false;
    stack[depth++] = pending->receiver;
    visited[pending->receiver] = generation;
    while (depth) {
        if (!effect_check_poll(build)) return false;
        uint32_t node = stack[--depth];
        const EffectCheckNode *row = &roots->node[node];
        if (row->tuple) {
            bool field = false;
            for (uint32_t e = row->elements; e != UINT32_MAX; e = roots->element[e].next) {
                EffectCheckElement element = roots->element[e];
                if (element.field != pending->selector) continue;
                if (!effect_check_alias(build, pending->node, element.source)) return false;
                field = found = true;
            }
            if (!field) { build->status = XR_XIR_BAD_STRUCTURE; return false; }
            continue;
        }
        if (row->aliases == UINT32_MAX) complete = false;
        for (uint32_t a = row->aliases; a != UINT32_MAX; a = roots->alias[a].next) {
            if (!roots->alias[a].root) continue;
            uint32_t source = roots->alias[a].source;
            if (visited[source] == generation) continue;
            visited[source] = generation;
            XR_DCHECK(depth < roots->nodes, "Each tuple origin is queued once");
            stack[depth++] = source;
        }
    }
    if (!complete || !found)
        effect_check_seed(roots, pending->node, effect_check_type_mask(effect_check_types_read(build), pending->result_type),
            (XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_INDIRECT, pending->instruction, UINT32_MAX, UINT32_MAX, 0});
    pending->linked = true;
    return true;
}

static XrXirStatus effect_check_link(EffectCheckBuild *build) {
    EffectCheckedRoots *roots = build->roots;
    bool higher_order = false;
    for (uint32_t f = 0; f < roots->functions; ++f) {
        if (!effect_check_bind_calls(build, f)) return build->status;
        EffectCheckFunction function = roots->function[f];
        for (uint32_t p = 0; p < function.pending_count; ++p) {
            if (!effect_check_poll(build)) return build->status;
            EffectCheckPending *pending = &roots->pending[function.pending_first + p];
            if (pending->kind == EFFECT_CHECK_HIGHER_ORDER || pending->kind == EFFECT_CHECK_TUPLE_FIELD) {
                higher_order = true;
                continue;
            }
            if (pending->kind == EFFECT_CHECK_REQUIREMENT) {
                if (!effect_check_link_requirement(build, pending)) return build->status;
                continue;
            }
            if (build->module->linkage_kind != XR_XIR_PROGRAM) continue;
            uint32_t index = effect_check_dispatch(build, pending);
            if (index == UINT32_MAX) return build->status;
            EffectCheckDispatch row = roots->dispatch[index];
            for (uint32_t t = 0; t < row.count; ++t)
                if (!effect_check_link_target(build, pending, roots->dispatch_target[row.first + t].function, 1))
                    return build->status;
            pending->linked = true;
        }
        if (!effect_check_bind_calls(build, f)) return build->status;
    }
    uint32_t *visited = NULL, *stack = NULL;
    if (!effect_check_capture_bindings(build)) return build->status;
    if (higher_order) {
        visited = xir_compile_calloc(build->memory, roots->nodes, sizeof(*visited), &build->status);
        stack = xir_compile_alloc(build->memory, (size_t)roots->nodes * sizeof(*stack), &build->status);
        if (!visited || !stack) goto done;
        for (uint32_t p = 0; p < roots->pending_count; ++p)
            if (roots->pending[p].kind == EFFECT_CHECK_TUPLE_FIELD &&
                !effect_check_link_tuple(build, &roots->pending[p], visited, stack, p + 1)) goto done;
        for (uint32_t p = 0; p < roots->pending_count; ++p)
            if (roots->pending[p].kind == EFFECT_CHECK_HIGHER_ORDER &&
                !effect_check_link_higher_order(build, &roots->pending[p])) goto done;
    }
    for (uint32_t n = 0; n < roots->nodes; ++n) roots->node[n].deferred = false;
    for (uint32_t p = 0; p < roots->pending_count; ++p) {
        EffectCheckPending *pending = &roots->pending[p];
        if (pending->linked) continue;
        if (pending->kind == EFFECT_CHECK_HIGHER_ORDER && !pending->root_linked) {
            const XrXirTypeNode *type = xr_xir_callable_signature(effect_check_types_read(build), pending->receiver_type);
            bool higher = false;
            for (uint32_t p = 0; type && p < type->parameter_count; ++p)
                higher |= xr_xir_type_is_callable(effect_check_types_read(build), type->parameters[p].type);
            if (type && !higher) {
                if (!effect_check_alias(build, pending->node, pending->receiver)) goto done;
                roots->alias[roots->node[pending->node].aliases].instruction = pending->instruction;
                roots->alias[roots->node[pending->node].aliases].suspension = true;
                pending->root_linked = true;
            }
        }
        const XrXirTypeNode *signature = NULL;
        if (pending->kind == EFFECT_CHECK_HIGHER_ORDER) {
            signature = xr_xir_callable_signature(effect_check_types_read(build), pending->receiver_type);
            if (!effect_check_suspend_alias(build, pending->node, pending->receiver) ||
                !effect_check_suspend_arguments(build, pending->node, signature,
                    pending->argument_first, pending->argument_count)) goto done;
        } else if (pending->kind == EFFECT_CHECK_REQUIREMENT) {
            const XrXirInterfaceTable *table = effect_check_types_read(build)->interfaces;
            signature = xr_xir_callable_signature(effect_check_types_read(build),
                table->declarations[pending->selector].methods[pending->member].signature);
            if (!effect_check_suspend_requirement(build, pending, signature)) goto done;
            if (!(signature->flags & XR_XIR_CALLABLE_NO_SUSPEND))
                effect_check_suspend_seed(roots, pending->node, 1, pending->instruction);
        } else {
            signature = xr_xir_callable_signature(effect_check_types_read(build), pending->result_type);
            if (signature && !(signature->flags & XR_XIR_CALLABLE_NO_SUSPEND))
                effect_check_suspend_seed(roots, pending->node, 1, pending->instruction);
        }
        if (!pending->root_linked) roots->node[pending->node].deferred = true;
        if (!pending->root_linked && pending->result != UINT32_MAX) roots->node[pending->result].deferred = true;
    }
done:
    xr_compile_resources_free(visited);
    xr_compile_resources_free(stack);
    return build->status;
}

static XrXirStatus effect_check_cell_roles(EffectCheckBuild *build) {
    EffectCheckedRoots *roots = build->roots;
    for (uint32_t f = 0; f < roots->functions; ++f) {
        const EffectCheckFunction *function = &roots->function[f];
        for (uint32_t p = 0; p < function->parameters; ++p) {
            const EffectCheckNode *node = &roots->node[function->first + p];
            uint32_t bit = function->parameters + p + 2;
            bool cell = (effect_check_bits(roots, function->first + p)[bit / 64] &
                (UINT64_C(1) << (bit % 64))) != 0;
            if (cell && (node->cell_use & 1u) && (node->cell_use & 6u)) return XR_XIR_BAD_TYPE;
            if (cell && (node->cell_use & 4u) && (node->cell_use & 3u)) return XR_XIR_BAD_TYPE;
            if (cell && (node->cell_use & 1u)) {
                effect_check_bits(roots, function->first + p)[bit / 64] &= ~(UINT64_C(1) << (bit % 64));
                roots->node[function->first + p].origin = XR_XIR_CELL_ORIGIN_OWNED;
                roots->node[function->first + p].bound = 0;
            }
            if (cell && !(node->cell_use & 1u)) {
                uint64_t *contents = effect_check_suspend_bits(roots, function->contents + p);
                contents[bit / 64] &= ~(UINT64_C(1) << (bit % 64));
                effect_check_suspend_seed(roots, function->contents + p,
                    roots->node[function->contents + p].suspend_bound, UINT32_MAX);
            }
            roots->node[function->first + p].closed = node->cell_use == 1 ? 0 : node->bound;
        }
    }
    return XR_XIR_OK;
}

static bool effect_check_merge(EffectCheckedRoots *roots, uint32_t target, uint32_t source) {
    const EffectCheckFunction *function = &roots->function[roots->node[target].function];
    uint64_t *to = effect_check_bits(roots, target), *from = effect_check_bits(roots, source);
    bool changed = false;
    uint8_t origin = roots->node[target].origin | roots->node[source].origin;
    if (origin != roots->node[target].origin) {
        roots->node[target].origin = origin;
        changed = true;
    }
    if (roots->node[source].deferred && !roots->node[target].deferred) {
        roots->node[target].deferred = true;
        changed = true;
    }
    for (uint32_t w = 0; w < function->words; ++w) {
        uint64_t combined = to[w] | from[w];
        changed |= combined != to[w];
        to[w] = combined;
    }
    for (uint32_t bit = 0; bit < 2; ++bit)
        if ((from[0] & (UINT64_C(1) << bit)) && roots->node[target].reason[bit] == UINT32_MAX)
            roots->node[target].reason[bit] = source;
    return changed;
}

static bool effect_check_apply(EffectCheckedRoots *roots, uint32_t output, const EffectCheckCall *call) {
    if (call->fixed_effects) return false;
    const EffectCheckFunction *callee = &roots->function[roots->node[call->target].function];
    uint64_t *to = effect_check_bits(roots, output), *from = effect_check_bits(roots, call->target);
    uint64_t constant = from[0] & 3u;
    bool changed = (to[0] | constant) != to[0];
    if (roots->node[call->target].deferred && !roots->node[output].deferred) {
        roots->node[output].deferred = true;
        changed = true;
    }
    to[0] |= constant;
    for (uint32_t bit = 0; bit < 2; ++bit)
        if ((constant & (UINT64_C(1) << bit)) && roots->node[output].reason[bit] == UINT32_MAX)
            roots->node[output].reason[bit] = call->target;
    for (uint32_t w = 0; w < callee->words; ++w) {
        uint64_t terms = from[w];
        if (!w) terms &= ~UINT64_C(3);
        while (terms) {
            uint32_t low = (uint32_t)__builtin_ctzll(terms);
            terms &= terms - 1;
            uint64_t bit = (uint64_t)w * 64 + low - 2;
            if (bit >= (uint64_t)callee->parameters * 3) continue;
            uint32_t parameter = (uint32_t)(bit % callee->parameters);
            if (parameter >= call->count) {
                if (call->protocol == 1) {
                    uint64_t mask = bit < callee->parameters ? 2u : bit < (uint64_t)callee->parameters * 2 ? 1u :
                        roots->node[callee->contents + parameter].bound;
                    if (!mask) continue;
                    changed |= (to[0] | mask) != to[0];
                    effect_check_seed(roots, output, (uint32_t)mask, (XrXirRootEffectWitness){
                        bit < callee->parameters ? XR_XIR_ROOT_CAUSE_PARAMETER :
                            bit < (uint64_t)callee->parameters * 2 ? XR_XIR_ROOT_CAUSE_CELL_PARAMETER : XR_XIR_ROOT_CAUSE_INDIRECT,
                        call->instruction, callee->declaration, parameter, 0});
                }
                continue;
            }
            uint32_t actual = roots->argument[call->first + parameter];
            if (bit >= (uint64_t)callee->parameters * 2) actual = effect_check_actual_contents(roots, actual);
            changed |= effect_check_merge(roots, output, actual);
        }
    }
    return changed;
}

/* Suspension equations share the ROOT graph and its component schedule. Only
 * execution aliases and actual call substitutions transfer this property. */
static bool effect_check_suspend_reason(EffectCheckedRoots *roots, uint32_t target,
    uint32_t source, uint32_t call, uint32_t parameter, uint32_t instruction, uint32_t bit) {
    uint32_t distance = roots->node[source].suspend_distance[bit];
    if (distance == UINT32_MAX || (call != UINT32_MAX && distance == UINT32_MAX - 1)) return false;
    distance += call != UINT32_MAX;
    uint32_t order = call != UINT32_MAX ? roots->call[call].instruction :
        instruction != UINT32_MAX ? instruction : roots->node[source].suspend_order[bit];
    if (distance > roots->node[target].suspend_distance[bit] ||
        (distance == roots->node[target].suspend_distance[bit] &&
            order >= roots->node[target].suspend_order[bit])) return false;
    roots->node[target].suspend_order[bit] = order;
    roots->node[target].suspend_distance[bit] = distance;
    roots->node[target].suspend_reason[bit] = source;
    roots->node[target].suspend_call[bit] = call;
    roots->node[target].suspend_parameter[bit] = parameter;
    return true;
}

static bool effect_check_suspend_merge(EffectCheckedRoots *roots, uint32_t target,
    uint32_t source, uint32_t call, uint32_t parameter, uint32_t instruction, bool formula) {
    const EffectCheckFunction *function = &roots->function[roots->node[target].function];
    uint64_t *to = effect_check_suspend_bits(roots, target);
    const uint64_t *from = effect_check_suspend_bits(roots, source);
    bool changed = false;
    uint32_t words = formula ? function->words : 1;
    for (uint32_t w = 0; w < words; ++w) {
        uint64_t value = from[w];
        if (!formula) value &= 3u;
        changed |= (to[w] | value) != to[w];
        to[w] |= value;
    }
    for (uint32_t bit = 0; bit < 2; ++bit)
        if (from[0] & (UINT64_C(1) << bit))
            changed |= effect_check_suspend_reason(roots, target, source, call, parameter, instruction, bit);
    if (roots->node[source].task_creation && !roots->node[target].task_creation) {
        roots->node[target].task_creation = true;
        changed = true;
    }
    return changed;
}

static bool effect_check_suspend_apply(EffectCheckedRoots *roots, uint32_t output, uint32_t index) {
    EffectCheckCall call = roots->call[index];
    const EffectCheckFunction *callee = &roots->function[roots->node[call.target].function];
    if (call.protocol == 8 || (call.target == callee->result && !call.default_result)) return false;
    if (call.fixed_effects) {
        bool changed = roots->node[call.target].task_creation && !roots->node[output].task_creation;
        roots->node[output].task_creation |= roots->node[call.target].task_creation;
        return changed;
    }
    bool changed = effect_check_suspend_merge(roots, output, call.target, index, UINT32_MAX, UINT32_MAX, false);
    const uint64_t *from = effect_check_suspend_bits(roots, call.target);
    for (uint32_t w = 0; w < callee->words; ++w) {
        uint64_t terms = from[w];
        if (!w) terms &= ~UINT64_C(3);
        while (terms) {
            uint32_t low = (uint32_t)__builtin_ctzll(terms);
            terms &= terms - 1;
            uint64_t term = (uint64_t)w * 64 + low - 2;
            if (term >= (uint64_t)callee->parameters * 2) continue;
            uint32_t parameter = (uint32_t)(term % callee->parameters);
            bool contents = term >= callee->parameters;
            if (parameter < call.count) {
                uint32_t source = roots->argument[call.first + parameter];
                if (contents) {
                    const EffectCheckFunction *caller = &roots->function[roots->node[source].function];
                    source = caller->contents + source - caller->first;
                }
                changed |= effect_check_suspend_merge(roots, output, source, index, parameter, UINT32_MAX, true);
            } else if (call.protocol == 1) {
                uint32_t source = contents ? callee->contents + parameter : callee->first + parameter;
                uint8_t bound = roots->node[source].suspend_bound;
                for (uint32_t bit = 0; bit < 2; ++bit) {
                    if (!(bound & (1u << bit)) || roots->node[output].suspend_distance[bit] == 0) continue;
                    effect_check_suspend_seed(roots, output, (uint8_t)(1u << bit), call.instruction);
                    roots->node[output].suspend_call[bit] = index;
                    roots->node[output].suspend_parameter[bit] = parameter;
                    changed = true;
                }
            }
        }
    }
    return changed;
}

static bool effect_check_evaluate(EffectCheckedRoots *roots, uint32_t node) {
    bool changed = false;
    for (uint32_t a = roots->node[node].aliases; a != UINT32_MAX; a = roots->alias[a].next)
        if (roots->alias[a].suspension)
            changed |= effect_check_suspend_merge(roots, node, roots->alias[a].source, UINT32_MAX, UINT32_MAX, roots->alias[a].instruction, true);
    for (uint32_t c = roots->node[node].calls; c != UINT32_MAX; c = roots->call[c].next)
        changed |= effect_check_suspend_apply(roots, node, c);
    for (uint32_t e = roots->node[node].aliases; e != UINT32_MAX; e = roots->alias[e].next)
        if (roots->alias[e].root) changed |= effect_check_merge(roots, node, roots->alias[e].source);
    for (uint32_t c = roots->node[node].calls; c != UINT32_MAX; c = roots->call[c].next)
        changed |= effect_check_apply(roots, node, &roots->call[c]);
    uint8_t mask = roots->node[node].closed | (uint8_t)(effect_check_bits(roots, node)[0] & 3u);
    bool blocked = roots->node[node].worker_blocked;
    bool access_denied = roots->node[node].access_denied;
    uint32_t root = roots->function[roots->node[node].function].root;
    if (roots->node[node].deferred) mask |= 2;
    for (uint32_t a = roots->node[node].aliases; a != UINT32_MAX; a = roots->alias[a].next) {
        if (!roots->alias[a].root) continue;
        mask |= roots->node[roots->alias[a].source].closed;
        blocked |= roots->node[roots->alias[a].source].worker_blocked;
    }
    for (uint32_t b = roots->node[node].bindings; b != UINT32_MAX; b = roots->binding[b].next) {
        uint32_t source = roots->binding[b].source;
        uint8_t actual = roots->node[source].closed;
        mask |= actual;
        if (roots->node[node].capture_contents) {
            uint64_t before = effect_check_bits(roots, node)[0];
            effect_check_seed(roots, node, actual,
                (XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_INDIRECT, 0, UINT32_MAX, UINT32_MAX, 0});
            changed |= before != effect_check_bits(roots, node)[0];
        }
        blocked |= roots->node[roots->binding[b].source].worker_blocked;
    }
    for (uint32_t c = roots->node[node].calls; c != UINT32_MAX; c = roots->call[c].next) {
        EffectCheckCall call = roots->call[c];
        EffectCheckFunction callee = roots->function[roots->node[call.target].function];
        if (node==root && call.target==callee.root && (call.protocol==2 || call.protocol==4))
            access_denied |= roots->node[call.target].access_denied;
        blocked |= roots->node[call.target].worker_blocked;
        if (call.fixed_effects) continue;
        const uint64_t *bits = effect_check_bits(roots, call.target);
        mask |= (uint8_t)(bits[0] & 3u);
        if (roots->node[call.target].deferred) mask |= 2;
        for (uint64_t p = 0; p < (uint64_t)callee.parameters * 3; ++p) {
            if (!(bits[(p + 2) / 64] & (UINT64_C(1) << ((p + 2) % 64)))) continue;
            uint32_t parameter = (uint32_t)(p % callee.parameters);
            if (parameter < call.count) {
                uint32_t source = roots->argument[call.first + parameter];
                if (p >= (uint64_t)callee.parameters * 2) source = effect_check_actual_contents(roots, source);
                mask |= roots->node[source].closed;
                blocked |= roots->node[source].worker_blocked;
            } else if (call.protocol == 1) mask |= roots->node[p >= (uint64_t)callee.parameters * 2 ?
                callee.contents + parameter : callee.first + parameter].bound;
        }
    }
    changed |= mask != roots->node[node].closed || blocked != roots->node[node].worker_blocked ||
        access_denied != roots->node[node].access_denied;
    roots->node[node].access_denied = access_denied;
    roots->node[node].closed = mask;
    roots->node[node].worker_blocked = blocked;
    return changed;
}

static XrXirStatus effect_check_component(EffectCheckBuild *build, const uint32_t *members,
    uint32_t count, uint32_t component, uint32_t *queue) {
    EffectCheckedRoots *roots = build->roots;
    uint32_t front = 0, back = 0, pending = count;
    for (uint32_t m = 0; m < count; ++m) {
        uint32_t node = members[m];
        roots->node[node].component = component;
        roots->node[node].queued = true;
        queue[m] = node;
    }
    while (pending) {
        if (!effect_check_poll(build)) return build->status;
        uint32_t node = queue[front];
        front = front + 1 == count ? 0 : front + 1;
        --pending;
        roots->node[node].queued = false;
        if (!effect_check_evaluate(roots, node)) continue;
        for (uint32_t e = roots->node[node].consumers; e != UINT32_MAX; e = roots->dependency[e].next_source) {
            uint32_t target = roots->dependency[e].target;
            if (roots->node[target].component != component || roots->node[target].queued) continue;
            XR_DCHECK(pending < count, "Each member has at most one pending equation");
            queue[back] = target;
            back = back + 1 == count ? 0 : back + 1;
            ++pending;
            roots->node[target].queued = true;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_check_close(EffectCheckBuild *build) {
    EffectCheckedRoots *roots = build->roots;
    uint32_t count = roots->nodes;
    uint64_t frames_bytes = (uint64_t)count * sizeof(EffectCheckFrame);
    if (frames_bytes > SIZE_MAX) return XR_XIR_OVERFLOW;
    uint32_t *index = xir_compile_calloc(build->memory, count, sizeof(*index), &build->status);
    uint32_t *low = xir_compile_calloc(build->memory, count, sizeof(*low), &build->status);
    uint32_t *stack = xir_compile_alloc(build->memory, (size_t)count * sizeof(*stack), &build->status);
    uint32_t *queue = xir_compile_alloc(build->memory, (size_t)count * sizeof(*queue), &build->status);
    EffectCheckFrame *frames = xir_compile_alloc(build->memory, (size_t)frames_bytes, &build->status);
    if (!index || !low || !stack || !queue || !frames) goto done;
    uint32_t next = 0, components = 0, stacked = 0;
    for (uint32_t start = 0; start < count && build->status == XR_XIR_OK; ++start) {
        if (index[start]) continue;
        uint32_t depth = 1;
        frames[0] = (EffectCheckFrame){start, roots->node[start].dependencies, UINT32_MAX};
        index[start] = low[start] = ++next;
        stack[stacked++] = start;
        while (depth && effect_check_poll(build)) {
            EffectCheckFrame *frame = &frames[depth - 1];
            uint32_t node = frame->node;
            if (frame->dependency != UINT32_MAX) {
                const EffectCheckDependency *edge = &roots->dependency[frame->dependency];
                frame->dependency = edge->next_target;
                uint32_t target = edge->source;
                if (!index[target]) {
                    index[target] = low[target] = ++next;
                    stack[stacked++] = target;
                    frames[depth++] = (EffectCheckFrame){target, roots->node[target].dependencies, node};
                } else if (roots->node[target].component == UINT32_MAX && index[target] < low[node]) {
                    low[node] = index[target];
                }
                continue;
            }
            uint32_t parent = frame->parent;
            --depth;
            if (index[node] == low[node]) {
                uint32_t end = stacked;
                do {
                    XR_DCHECK(stacked, "A live component contains its root");
                    --stacked;
                } while (stack[stacked] != node);
                build->status = effect_check_component(build, stack + stacked, end - stacked, components++, queue);
            }
            if (parent != UINT32_MAX && low[node] < low[parent]) low[parent] = low[node];
        }
    }
done:
    xr_compile_resources_free(index);
    xr_compile_resources_free(low);
    xr_compile_resources_free(stack);
    xr_compile_resources_free(queue);
    xr_compile_resources_free(frames);
    return build->status;
}

/* Scanning exports numeric module equations. Typed dispatch applications own
 * their pool so an imported interface never borrows the scanning arena. */
static XrXirStatus effect_checked_roots_collect(const XrXirCompileContext *memory,
    const XrXirModule *module, const XrXirFunctionEffectContract *contracts,
    XrCompileRequestControl *control, const XrXirRootRefiner *refiner, EffectCheckedRoots **output,
    bool prepare_storage) {
    if (!xir_compile_context_valid(memory) || !module || !module->functions || !module->function_count ||
        !output || *output) return XR_XIR_BAD_STRUCTURE;
    EffectCheckBuild build = {.memory = memory, .module = module, .contracts = contracts, .control = control, .refiner = refiner,
        .scratch = {.resources = memory->resources}};
    build.status = effect_check_allocate(&build);
    if (build.status == XR_XIR_OK && prepare_storage)
        build.status = xir_storage_flow_new(memory, module, build.roots, control, &build.roots->storage);
    if (build.status == XR_XIR_OK) build.status = effect_check_collect(&build);
    if (build.status == XR_XIR_OK && build.roots->storage)
        build.status = xir_storage_flow_finish_local(build.roots->storage);
    if (build.status == XR_XIR_OK && build.roots->storage)
        build.status = xir_execution_refresh_storage(build.roots);
    if (build.status == XR_XIR_OK) build.status = effect_check_builtin_interfaces(&build);
    if (build.status == XR_XIR_OK && (build.roots->pending_count || build.roots->type_arguments))
        effect_check_types(&build);
    if (build.status == XR_XIR_OK && build.roots->terms)
        build.status = effect_terms_retire(memory, build.roots->terms, true);
    xir_read_scratch_free(&build.scratch);
    if (build.status != XR_XIR_OK) {
        effect_checked_roots_free(build.roots);
        return build.status;
    }
    *output = build.roots;
    return XR_XIR_OK;
}

/* Linking consumes the exported owner even on failure. Only declaration
 * headers are visible here; recursive closure never rescans instruction arrays. */
static XrXirStatus effect_checked_roots_link_take(const XrXirCompileContext *memory,
    const XrXirModule *program, XrCompileRequestControl *control,
    EffectCheckedRoots **interfaces, EffectCheckedRoots **output, bool close,
    const XrXirRootRefiner *refiner) {
    if (!xir_compile_context_valid(memory) || !program || !program->functions || !interfaces ||
        !*interfaces || !output || *output || interfaces == output ||
        (*interfaces)->resources != memory->resources ||
        (*interfaces)->templates != program->function_count) return XR_XIR_BAD_STRUCTURE;
    EffectCheckedRoots *roots = *interfaces;
    *interfaces = NULL;
    if (roots->terms) roots->terms->remaining = memory;
    EffectCheckBuild build = {.memory = memory, .roots = roots, .control = control, .refiner = refiner,
        .scratch = {.resources = memory->resources}};
    XrXirFunction *headers = xir_compile_copy(memory, program->functions,
        (size_t)program->function_count * sizeof(*headers), &build.status);
    XrXirModule declarations = *program;
    declarations.functions = headers;
    build.module = &declarations;
    if (headers) for (uint32_t f = 0; f < program->function_count; ++f) {
        headers[f].instructions = NULL;
        headers[f].instruction_count = 0;
        headers[f].blocks = NULL;
        headers[f].block_count = 0;
        headers[f].operands = NULL;
        headers[f].operand_count = 0;
    }
    if (build.status == XR_XIR_OK) build.status = effect_check_link(&build);
    if (build.status == XR_XIR_OK) build.status = effect_check_cleanup_uses(&build);
    if (build.status == XR_XIR_OK && roots->storage) {
        bool changed = true, final = false;
        while (changed && build.status == XR_XIR_OK) {
            uint32_t nodes = roots->storage_origins.nodes, edges = roots->storage_origins.count;
            build.status = xir_storage_flow_link(roots->storage, roots, &changed);
            bool ready = true;
            if (build.status == XR_XIR_OK && refiner && refiner->points)
                build.status = refiner->points(refiner->context, roots, roots->storage, final, &ready);
            if (build.status == XR_XIR_OK) build.status = xir_execution_refresh_storage(roots);
            if (build.status != XR_XIR_OK) break;
            uint32_t calls = roots->calls;
            for (uint32_t p = 0; p < roots->pending_count && build.status == XR_XIR_OK; ++p)
                if (roots->pending[p].kind == EFFECT_CHECK_HIGHER_ORDER)
                    effect_check_link_higher_order(&build, &roots->pending[p]);
            if (build.status != XR_XIR_OK) break;
            bool content_changed = calls != roots->calls || edges != roots->storage_origins.count;
            bool origins_changed = content_changed || nodes != roots->storage_origins.nodes;
            if (final && content_changed) build.status = XR_XIR_UNRESOLVED;
            if (!ready && !changed && !origins_changed) build.status = XR_XIR_UNRESOLVED;
            changed |= (!final && origins_changed) || !ready;
            if (!changed && !final && refiner && refiner->points) {final = true; changed = true;}
        }
        if (build.status == XR_XIR_OK && final) {
            XirStorageFlowView storage = {0};
            build.status = xir_storage_flow_seal(roots->storage, &storage);
            if (build.status == XR_XIR_OK) build.status = xir_execution_refresh_storage(roots);
        }
        for (uint32_t n = 0; n < roots->nodes; ++n) roots->node[n].deferred = false;
        for (uint32_t p = 0; p < roots->pending_count; ++p) {
            EffectCheckPending pending = roots->pending[p];
            if (pending.linked || pending.root_linked) continue;
            roots->node[pending.node].deferred = true;
            if (pending.result != UINT32_MAX) roots->node[pending.result].deferred = true;
        }
    }
    if (build.status == XR_XIR_OK && close) build.status = effect_check_effect_projection(&build);
    if (build.status == XR_XIR_OK && close) build.status = effect_check_cell_roles(&build);
    if (build.status == XR_XIR_OK && close) build.status = effect_check_close(&build);
    if (build.status == XR_XIR_OK && roots->terms)
        build.status = effect_terms_retire(memory, roots->terms, true);
    xir_read_scratch_free(&build.scratch);
    xr_compile_resources_free(headers);
    if (build.status == XR_XIR_OK) {
        roots->dispatch_index = build.dispatch_index;
        roots->dispatch_index_capacity = build.dispatch_index_capacity;
    } else xr_compile_resources_free(build.dispatch_index);
    xr_compile_resources_free(build.instance_index);
    if (build.status != XR_XIR_OK) {
        effect_checked_roots_free(roots);
        return build.status;
    }
    *output = roots;
    return XR_XIR_OK;
}

static XrXirStatus effect_checked_roots_build(const XrXirCompileContext *memory,
    const XrXirModule *module, const XrXirFunctionEffectContract *contracts,
    XrCompileRequestControl *control, const XrXirRootRefiner *refiner, EffectCheckedRoots **output, bool close) {
    if (!output || *output) return XR_XIR_BAD_STRUCTURE;
    EffectCheckedRoots *interfaces = NULL;
    XrXirStatus status = effect_checked_roots_collect(memory, module, contracts, control, refiner, &interfaces, !close);
    if (status == XR_XIR_OK)
        status = effect_checked_roots_link_take(memory, module, control, &interfaces, output, close, refiner);
    effect_checked_roots_free(interfaces);
    return status;
}

/* The interface owns canonical parameter terms. A deferred function cannot be
 * consumed as a closed Program effect before its symbolic dependencies link. */
static XrXirStatus effect_checked_formula_contexts(const XrXirCompileContext *memory,
    const EffectCheckedRoots *roots, uint32_t function, uint32_t node, uint8_t **output, bool *unknown) {
    EffectCheckFunction body = roots->function[function];
    uint32_t expressions = body.last - body.first + 1, instructions = body.values - body.parameters;
    if ((uint64_t)expressions * sizeof(uint32_t) > SIZE_MAX) return XR_XIR_BUDGET;
    XrXirStatus status = XR_XIR_OK;
    uint8_t *contexts = NULL;
    uint32_t *stack = NULL;
    uint8_t *seen = xir_compile_calloc(memory, expressions, 1, &status);
    if (!seen) goto done;
    contexts = instructions ? xir_compile_calloc(memory, instructions, 1, &status) : NULL;
    if (instructions && !contexts) goto done;
    stack = xir_compile_alloc(memory, (size_t)expressions * sizeof(*stack), &status);
    if (!stack) goto done;
    uint32_t depth = 0;
    stack[depth++] = node;
    seen[node - body.first] = 1;
    while (depth) {
        uint32_t current = stack[--depth];
        const EffectCheckNode *row = &roots->node[current];
        for (uint32_t c = row->calls; c != UINT32_MAX; c = roots->call[c].next) {
            EffectCheckCall call = roots->call[c];
            if (!roots->node[call.target].deferred) continue;
            if ((call.protocol == 2 || call.protocol == 4) && call.instruction < instructions)
                contexts[call.instruction] = 1;
            else *unknown = true;
        }
        for (uint32_t a = row->aliases; a != UINT32_MAX; a = roots->alias[a].next) {
            if (!roots->alias[a].root) continue;
            uint32_t source = roots->alias[a].source;
            if (!roots->node[source].deferred || seen[source - body.first]) continue;
            seen[source - body.first] = 1;
            XR_DCHECK(depth < expressions, "Each local formula dependency is queued once");
            stack[depth++] = source;
        }
    }
    for (uint32_t p = 0; p < body.pending_count; ++p) {
        EffectCheckPending pending = roots->pending[body.pending_first + p];
        if (pending.linked || pending.root_linked || (!seen[pending.node - body.first] &&
            (pending.result == UINT32_MAX || !seen[pending.result - body.first]))) continue;
        if (pending.kind == EFFECT_CHECK_REQUIREMENT && pending.instruction < instructions)
            contexts[pending.instruction] = 1;
        else *unknown = true;
    }
    *output = contexts;
    contexts = NULL;
done:
    xr_compile_resources_free(seen);
    xr_compile_resources_free(stack);
    xr_compile_resources_free(contexts);
    return status;
}

static XrXirStatus effect_checked_root_formula(const XrXirCompileContext *memory,
    const EffectCheckedRoots *roots, uint32_t function, bool result, XrXirRootFormula *output) {
    if (!xir_compile_context_valid(memory) || !roots || roots->resources != memory->resources ||
        function >= roots->functions || !output || output->constant_mask || output->term_count || output->terms)
        return XR_XIR_BAD_STRUCTURE;
    const EffectCheckFunction *body = &roots->function[function];
    uint32_t node = result ? body->result : body->root;
    uint8_t *contexts = NULL;
    bool unknown = false;
    XrXirStatus status = roots->node[node].deferred ?
        effect_checked_formula_contexts(memory, roots, function, node, &contexts, &unknown) : XR_XIR_OK;
    if (status != XR_XIR_OK) return status;
    const uint64_t *bits = effect_check_bits(roots, node);
    for (uint64_t p = (uint64_t)body->parameters * 2; p < (uint64_t)body->parameters * 3; ++p)
        if (bits[(p + 2) / 64] & (UINT64_C(1) << ((p + 2) % 64))) {
            xr_compile_resources_free(contexts); return XR_XIR_UNRESOLVED;
        }
    uint32_t count = 0;
    for (uint64_t p = 0; p < (uint64_t)body->parameters * 2; ++p)
        count += !!(bits[(p + 2) / 64] & (UINT64_C(1) << ((p + 2) % 64)));
    uint32_t instructions = body->values - body->parameters, context_count = 0;
    for (uint32_t i = 0; contexts && i < instructions; ++i) context_count += !!contexts[i];
    if (context_count > UINT32_MAX - count) { xr_compile_resources_free(contexts); return XR_XIR_OVERFLOW; }
    count += context_count;
    XrXirRootTerm *terms = count ? xir_compile_calloc(memory, count, sizeof(*terms), &status) : NULL;
    if (count && !terms) { xr_compile_resources_free(contexts); return status; }
    uint32_t at = 0;
    for (uint64_t p = 0; p < body->parameters; ++p) {
        if (!(bits[(p + 2) / 64] & (UINT64_C(1) << ((p + 2) % 64)))) continue;
        terms[at++] = (XrXirRootTerm){XR_XIR_ROOT_TERM_PARAMETER, (uint32_t)p};
    }
    for (uint32_t i = 0; contexts && i < instructions; ++i)
        if (contexts[i]) terms[at++] = (XrXirRootTerm){XR_XIR_ROOT_TERM_CONTEXT_CALL, i};
    for (uint64_t p = body->parameters; p < (uint64_t)body->parameters * 2; ++p)
        if (bits[(p + 2) / 64] & (UINT64_C(1) << ((p + 2) % 64)))
            terms[at++] = (XrXirRootTerm){XR_XIR_ROOT_TERM_CELL_PARAMETER, (uint32_t)(p - body->parameters)};
    xr_compile_resources_free(contexts);
    uint32_t mask = (bits[0] & 1 ? XR_XIR_CALLABLE_ROOT_REQUIRED : 0) |
        (bits[0] & 2 ? XR_XIR_CALLABLE_ROOT_UNRESOLVED : 0);
    if (unknown || (roots->node[node].deferred && !context_count)) mask |= XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    *output = (XrXirRootFormula){mask, count, terms};
    return XR_XIR_OK;
}

/* Construction and specialization read the same closed row. Parameter masks
 * are substituted at the receiving boundary without creating another solver. */
static XrXirStatus effect_checked_root_mask(const EffectCheckedRoots *roots, uint32_t node,
    const uint32_t *arguments, uint32_t count, uint32_t *output) {
    if (!roots || node >= roots->nodes || !output) return XR_XIR_BAD_STRUCTURE;
    const EffectCheckFunction *function = &roots->function[roots->node[node].function];
    if ((!arguments && count) || (arguments && count != function->parameters)) return XR_XIR_BAD_STRUCTURE;
    if (arguments) for (uint32_t p = 0; p < count; ++p)
        if (arguments[p] & ~(XR_XIR_CALLABLE_ROOT_REQUIRED | XR_XIR_CALLABLE_ROOT_UNRESOLVED))
            return XR_XIR_BAD_STRUCTURE;
    if (roots->node[node].deferred) return XR_XIR_UNRESOLVED;
    if (!arguments) {
        uint32_t mask = roots->node[node].closed;
        *output = ((mask & 1) ? XR_XIR_CALLABLE_ROOT_REQUIRED : 0) |
            ((mask & 2) ? XR_XIR_CALLABLE_ROOT_UNRESOLVED : 0);
        return XR_XIR_OK;
    }
    const uint64_t *bits = effect_check_bits(roots, node);
    uint32_t mask = (bits[0] & 1 ? XR_XIR_CALLABLE_ROOT_REQUIRED : 0) |
        (bits[0] & 2 ? XR_XIR_CALLABLE_ROOT_UNRESOLVED : 0);
    for (uint32_t w = 0; w < function->words; ++w) {
        uint64_t terms = bits[w];
        if (!w) terms &= ~UINT64_C(3);
        while (terms) {
            uint32_t low = (uint32_t)__builtin_ctzll(terms);
            terms &= terms - 1;
            uint64_t parameter = (uint64_t)w * 64 + low - 2;
            if (parameter >= (uint64_t)function->parameters * 3) return XR_XIR_BAD_STRUCTURE;
            if (parameter >= (uint64_t)function->parameters * 2) return XR_XIR_UNRESOLVED;
            uint32_t p = (uint32_t)(parameter % function->parameters);
            mask |= arguments ? arguments[p] : parameter < function->parameters ?
                XR_XIR_CALLABLE_ROOT_UNRESOLVED : XR_XIR_CALLABLE_ROOT_REQUIRED;
        }
    }
    *output = mask;
    return XR_XIR_OK;
}

static XrXirStatus effect_checked_bound_cause(const XrXirCompileContext *memory,
    const EffectCheckedRoots *roots, uint32_t node, bool unresolved, EffectCheckCause *output);

static XrXirStatus effect_checked_first_cause(const XrXirCompileContext *memory,
    const EffectCheckedRoots *roots, uint32_t node,
    const uint32_t *arguments, uint32_t count, bool unresolved, EffectCheckCause *output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    uint32_t mask = 0;
    XrXirStatus status = effect_checked_root_mask(roots, node, arguments, count, &mask);
    if (status != XR_XIR_OK && status != XR_XIR_UNRESOLVED) return status;
    const uint64_t *bits = effect_check_bits(roots, node);
    if (status == XR_XIR_UNRESOLVED && (bits[0] & (UINT64_C(1) << unresolved))) {
        mask = unresolved ? XR_XIR_CALLABLE_ROOT_UNRESOLVED : XR_XIR_CALLABLE_ROOT_REQUIRED;
        status = XR_XIR_OK;
    }
    if (status != XR_XIR_OK) return status;
    uint32_t wanted = unresolved ? XR_XIR_CALLABLE_ROOT_UNRESOLVED : XR_XIR_CALLABLE_ROOT_REQUIRED;
    EffectCheckCause result = {.function = UINT32_MAX};
    if (!(mask & wanted)) {
        *output = result;
        return XR_XIR_OK;
    }
    const EffectCheckFunction *function = &roots->function[roots->node[node].function];
    if (bits[0] & (UINT64_C(1) << unresolved)) {
        uint32_t current = node, distance = 0;
        XrXirRootEffectWitness first = {0};
        for (uint32_t step = 0; step < roots->nodes; ++step) {
            uint32_t reason = roots->node[current].reason[unresolved];
            if (reason >= roots->nodes) return XR_XIR_BAD_STRUCTURE;
            if (reason == current) {
                result.function = roots->function[roots->node[node].function].declaration;
                result.witness = first.cause ? first : roots->terminals[(size_t)current * 2 + unresolved];
                if (!result.witness.cause) return XR_XIR_BAD_STRUCTURE;
                result.witness.distance += distance;
                *output = result;
                return XR_XIR_OK;
            }
            if (roots->node[current].function != roots->node[reason].function) {
                ++distance;
                if (!first.cause) {
                    for (uint32_t c = roots->node[current].calls; c != UINT32_MAX; c = roots->call[c].next) {
                        const EffectCheckCall *call = &roots->call[c];
                        if (call->target != reason) continue;
                        first = (XrXirRootEffectWitness){call->protocol == 4 ? XR_XIR_ROOT_CAUSE_CLEANUP :
                            XR_XIR_ROOT_CAUSE_CALL, call->instruction,
                            roots->function[roots->node[reason].function].declaration, UINT32_MAX, 0};
                        break;
                    }
                }
            }
            current = reason;
        }
        return XR_XIR_BAD_STRUCTURE;
    }
    for (uint64_t p = 0; p < (uint64_t)function->parameters * 3; ++p) {
        if (!(bits[(p + 2) / 64] & (UINT64_C(1) << ((p + 2) % 64)))) continue;
        uint32_t parameter = (uint32_t)(p % function->parameters);
        uint32_t source = p >= (uint64_t)function->parameters * 2 ? function->contents + parameter :
            function->first + parameter;
        uint32_t closed = roots->node[source].closed;
        uint32_t actual = arguments ? arguments[parameter] :
            ((closed & 1) ? XR_XIR_CALLABLE_ROOT_REQUIRED : 0) |
            ((closed & 2) ? XR_XIR_CALLABLE_ROOT_UNRESOLVED : 0);
        if (!(actual & wanted)) continue;
        result.function = function->declaration;
        result.witness = (XrXirRootEffectWitness){p < function->parameters ? XR_XIR_ROOT_CAUSE_PARAMETER :
            p < (uint64_t)function->parameters * 2 ? XR_XIR_ROOT_CAUSE_CELL_PARAMETER : XR_XIR_ROOT_CAUSE_INDIRECT,
            UINT32_MAX, UINT32_MAX, parameter, 0};
        *output = result;
        return XR_XIR_OK;
    }
    return arguments ? XR_XIR_BAD_STRUCTURE :
        effect_checked_bound_cause(memory, roots, node, unresolved, output);
}

#endif // XXIR_EFFECT_CHECK_IMPLEMENTATION
