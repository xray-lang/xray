/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_coroutine_module_shape.h - Reachable frozen suspension proof
 *
 * KEY CONCEPT:
 *   Source calls enter exact immutable declarations in an explicitly supplied
 *   module graph. Only reachable body edges are read. Published coroutine
 *   states, live Xi children, and source names never prove a synchronous body.
 */
#ifndef XR_SEMANTIC_COROUTINE_MODULE_SHAPE_H
#define XR_SEMANTIC_COROUTINE_MODULE_SHAPE_H

#include "xr_semantic_coroutine_function_shape.h"
#include "xr_semantic_source_dependency_call_shape.h"
#include "xr_semantic_string_runes_shape.h"
#include "xr_semantic_iterator_rune_nth_shape.h"
#include "xr_semantic_rune_to_uint32_shape.h"
#include "xr_semantic_builtin_runtime_method_shape.h"

typedef struct XrSemanticCoroutineModule {
    const XrSemanticPlan *plan;
    const char *identity;
} XrSemanticCoroutineModule;

typedef struct XrSemanticCoroutineModuleGraph {
    const XrSemanticCoroutineModule *modules;
    uint32_t count;
    /* Scratch visited modules, owned by the invoking authority survey. */
    uint8_t *used;
} XrSemanticCoroutineModuleGraph;

typedef struct XrSemanticCoroutineFunctionNode {
    uint32_t module;
    uint32_t function;
} XrSemanticCoroutineFunctionNode;

/* The final length-framed module identity occupies the end of its owned key.
 * Parse the preceding name by length so names cannot inject the delimiter. */
static inline const char *xr_semantic_coroutine_module_identity(const XrSemanticPlan *plan) {
    const XrSemanticEntityRecord *module = xr_semantic_plan_unique_module_entity(plan);
    if (!module || !module->canonical_key)
        return NULL;
    const char *key = module->canonical_key;
    XrSemanticCallableKeyCursor cursor = {key, key + strlen(key)};
    XrSemanticCallableKeySlice name = {0}, identity = {0};
    unsigned schema = 0, kind = 0;
    if (!xr_semantic_callable_key_literal(&cursor, "entity-v1:schema=") ||
        !xr_semantic_callable_key_unsigned(&cursor, UINT32_MAX, &schema) ||
        schema != XR_SEMANTIC_SCHEMA_VERSION ||
        !xr_semantic_callable_key_literal(&cursor, ":kind=") ||
        !xr_semantic_callable_key_unsigned(&cursor, XR_SEM_ENTITY_KIND_COUNT - 1u, &kind) ||
        kind != XR_SEM_ENTITY_MODULE || !xr_semantic_callable_key_literal(&cursor, ":parent=") ||
        (size_t) (cursor.end - cursor.at) < XR_STABLE_ID_BYTES * 2u)
        return NULL;
    for (unsigned c = 0; c < XR_STABLE_ID_BYTES * 2u; c++) {
        char value = *cursor.at++;
        if ((value < '0' || value > '9') && (value < 'a' || value > 'f'))
            return NULL;
    }
    if (!xr_semantic_callable_key_literal(&cursor, ":name=") ||
        !xr_semantic_callable_key_component(&cursor, &name) ||
        !xr_semantic_callable_key_literal(&cursor, ":identity=") ||
        !xr_semantic_callable_key_component(&cursor, &identity) ||
        identity.size == 0 || cursor.at != cursor.end)
        return NULL;
    return identity.data;
}

/* A matching fingerprint alone does not bind a module path. All three
 * fields must resolve one graph entry, and duplicate identities refuse. */
static inline uint32_t xr_semantic_coroutine_dependency_module(
    const XrSemanticCoroutineModuleGraph *graph, const XrSemanticDependencyRecord *required) {
    if (!graph || !required || !required->module_path || !graph->modules)
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t match = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t m = 0; m < graph->count; m++) {
        const XrSemanticCoroutineModule *entry = &graph->modules[m];
        const XrSemanticEntityRecord *module =
            xr_semantic_plan_unique_module_entity(entry->plan);
        if (!entry->plan || !entry->identity || !module ||
            !xr_stable_id_equal(module->id, required->module))
            continue;
        const char *owned_identity = xr_semantic_coroutine_module_identity(entry->plan);
        if (match != XR_SEMANTIC_INDEX_NONE || !owned_identity ||
            strcmp(entry->identity, owned_identity) != 0 ||
            strcmp(entry->identity, required->module_path) != 0 ||
            !xr_fingerprint_equal(required->semantic_fingerprint,
                                  xr_semantic_plan_fingerprint(entry->plan)))
            return XR_SEMANTIC_INDEX_NONE;
        match = m;
    }
    return match;
}

/* Reconstruct the lexical shared import instead of trusting a target row's
 * selected export. The shared-store helper independently proves uniqueness,
 * owner ancestry, and initialization before activation. */
static inline bool xr_semantic_coroutine_source_export_import(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *call,
    const char **out_module, const char **out_member) {
    uint32_t operand_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!call || !operands || !metadata || call->operand_count == 0 ||
        call->operand_begin >= operand_count)
        return false;
    bool namespace_call = call->opcode == XI_CALL_METHOD;
    if ((!namespace_call && call->opcode != XI_CALL) ||
        (namespace_call && ((call->semantic_immediate & 1) != 0 || call->metadata_count != 1 ||
                           call->metadata_begin >= metadata_count)) ||
        (!namespace_call && call->metadata_count != 0) ||
        operands[call->operand_begin].role !=
            (namespace_call ? XR_SEM_OPERAND_RECEIVER : XR_SEM_OPERAND_CALLEE))
        return false;
    uint32_t value = operands[call->operand_begin].value;
    const XrSemanticOperationRecord *load = NULL;
    for (uint32_t depth = 0; depth < xr_semantic_plan_operation_count(plan); depth++) {
        load = xr_semantic_class_value_definition(plan, value);
        if (!load || load->function != call->function)
            return false;
        if (load->opcode != XI_COPY || load->semantic_immediate != XI_COPY_KIND_IDENTITY ||
            load->operand_count != 1 || load->result_alias_operand != 0)
            break;
        if (load->operand_begin >= operand_count)
            return false;
        value = operands[load->operand_begin].value;
    }
    if (!load || !xr_semantic_class_shared_read_shape_is_exact(load))
        return false;
    const XrSemanticOperationRecord *store = xr_semantic_class_shared_read_store(plan, load);
    if (!store || store->function != 0 || store->operand_count != 1 ||
        store->operand_begin >= operand_count)
        return false;
    value = operands[store->operand_begin].value;
    const XrSemanticOperationRecord *import = NULL;
    for (uint32_t depth = 0; depth < xr_semantic_plan_operation_count(plan); depth++) {
        import = xr_semantic_class_value_definition(plan, value);
        if (!import || import->function != 0)
            return false;
        if (import->opcode != XI_COPY || import->semantic_immediate != XI_COPY_KIND_IDENTITY ||
            import->operand_count != 1 || import->result_alias_operand != 0)
            break;
        if (import->operand_begin >= operand_count)
            return false;
        value = operands[import->operand_begin].value;
    }
    if (!import || import->opcode != XI_IMPORT_REF ||
        import->import_resolution != XR_SEM_IMPORT_RESOLUTION_SOURCE_MODULE ||
        import->operand_count != 0 || import->metadata_count != 2 ||
        import->metadata_begin >= metadata_count ||
        import->metadata_count > metadata_count - import->metadata_begin)
        return false;
    const char *module = metadata[import->metadata_begin];
    const char *member = metadata[import->metadata_begin + 1u];
    if (!module || !module[0] || !member || (member[0] == '\0') != namespace_call)
        return false;
    if (namespace_call)
        member = metadata[call->metadata_begin];
    if (!member || !member[0])
        return false;
    *out_module = module;
    *out_member = member;
    return true;
}

/* This fixed-arity export judgement is intentionally narrower than general
 * execution admission. Unsupported variadic/default or place forms leave the
 * effect unknown; no alternate type or ownership rule is invented here. */
static inline bool xr_semantic_coroutine_source_export_shape(
    const XrSemanticPlan *caller, const XrSemanticCallTargetRecord *target,
    const XrSemanticPlan *dependency, uint32_t function) {
    const XrSemanticFunctionRecord *callee = xr_semantic_plan_function(dependency, function);
    const XrSemanticOperationRecord *call = xr_semantic_plan_operation(caller, target->operation);
    const XrSemanticSourceExportRecord *exported =
        xr_semantic_plan_source_export(dependency, target->source_export);
    const XrSemanticDependencyRecord *required =
        xr_semantic_plan_dependency(caller, target->dependency);
    const char *module = NULL, *member = NULL;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(caller, &operand_count);
    const XrSemanticTypeRecord *result = call ?
        xr_semantic_plan_type(caller, call->result_type) : NULL;
    const XrSemanticTypeRecord *declared = callee ?
        xr_semantic_plan_type(dependency, callee->return_type) : NULL;
    if (!callee || !call || !exported || !required || !operands || !result || !declared ||
        call->operand_count != (uint32_t) callee->parameter_count + 1u ||
        call->operand_begin > operand_count || call->operand_count > operand_count - call->operand_begin ||
        !xr_stable_id_equal(result->id, declared->id) ||
        !xr_semantic_coroutine_source_export_import(caller, call, &module, &member) ||
        strcmp(module, required->module_path) != 0 || strcmp(member, exported->name) != 0)
        return false;
    for (uint16_t p = 0; p < callee->parameter_count; p++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(dependency, callee->parameter_begin + p);
        const XrSemanticOperandRecord *argument = &operands[call->operand_begin + 1u + p];
        const XrSemanticTypeRecord *parameter_type = parameter ?
            xr_semantic_plan_type(dependency, parameter->type) : NULL;
        const XrSemanticTypeRecord *argument_type = xr_semantic_plan_type(caller, argument->type);
        if (!parameter || !parameter_type || !argument_type || parameter->function != function ||
            parameter->ordinal != p || parameter->mode != XR_PARAM_READ ||
            (parameter->flags & XR_SEM_PARAMETER_VARIADIC) != 0 ||
            !xr_semantic_parameter_type_admits_argument(dependency, parameter_type, argument_type,
                                                         parameter->mode) ||
            argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != (int16_t) p ||
            argument->parameter_mode != parameter->mode || argument->access != XR_CALL_ARG_PLAIN ||
            argument->transfer_mode != parameter->transfer_mode || argument->origin != 0 ||
            argument->lifetime != 0 || argument->escape != 0 ||
            argument->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
            argument->ownership_action != (parameter->ownership == XI_OWN_BORROWED
                ? XR_SEM_OPERAND_BORROW : XR_SEM_OPERAND_CONSUME))
            return false;
    }
    return true;
}

static inline bool xr_semantic_coroutine_sync_intrinsic(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation) {
    uint32_t receiver = 0, index = 0;
    const XaBuiltinReceiverMethodSpec *spec = NULL;
    return xr_semantic_string_runes_is_exact(plan, operation, &receiver) ||
        xr_semantic_iterator_rune_nth_is_exact(plan, operation, &receiver, &index) ||
        xr_semantic_rune_to_uint32_is_exact(plan, operation, &receiver) ||
        (xr_semantic_builtin_runtime_method_is_exact(plan, operation, &spec, &receiver) &&
         spec->receiver == XA_BUILTIN_RECEIVER_STRING);
}

/* An edge resolves a body, proves a synchronous native leaf, or carries an
 * open-domain suspension obligation. Unknown authority remains distinct. */
static inline int xr_semantic_coroutine_graph_call(
    const XrSemanticCoroutineModuleGraph *graph, uint32_t owner,
    const XrSemanticCallTargetRecord *target, XrSemanticCoroutineFunctionNode *out) {
    const XrSemanticPlan *plan = graph->modules[owner].plan;
    out->module = owner;
    out->function = XR_SEMANTIC_INDEX_NONE;
    if (!target)
        return -1;
    switch (target->kind) {
        case XR_SEM_CALL_TARGET_DIRECT_LOCAL:
        case XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_LOCAL:
        case XR_SEM_CALL_TARGET_SOURCE_STATIC_METHOD_LOCAL:
        case XR_SEM_CALL_TARGET_SOURCE_TEMPLATE_METHOD_LOCAL:
            out->function = target->function;
            return xr_semantic_plan_function(plan, out->function) ? 0 : -1;
        case XR_SEM_CALL_TARGET_INDIRECT_CALLABLE:
        case XR_SEM_CALL_TARGET_NATIVE_YIELDABLE:
        case XR_SEM_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE:
        case XR_SEM_CALL_TARGET_BUILTIN_INSTANCE_YIELDABLE:
        case XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_SEALED_CANDIDATE:
            return 1;
        case XR_SEM_CALL_TARGET_NATIVE_DIRECT:
            return 0;
        case XR_SEM_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR:
            if (target->dependency == XR_SEMANTIC_INDEX_NONE)
                return xr_semantic_constructor_local_function(plan, target, &out->function) ? 0 : -1;
            break;
        case XR_SEM_CALL_TARGET_SOURCE_EXPORT:
        case XR_SEM_CALL_TARGET_SOURCE_METHOD_DEPENDENCY:
        case XR_SEM_CALL_TARGET_SOURCE_STATIC_METHOD_DEPENDENCY:
            break;
        default:
            return -1;
    }
    const XrSemanticDependencyRecord *required = xr_semantic_plan_dependency(plan, target->dependency);
    uint32_t dependency = xr_semantic_coroutine_dependency_module(graph, required);
    if (dependency == XR_SEMANTIC_INDEX_NONE)
        return -1;
    const XrSemanticPlan *match = graph->modules[dependency].plan;
    uint32_t function = XR_SEMANTIC_INDEX_NONE;
    if (target->kind == XR_SEM_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR) {
        const XrSemanticSourceExportRecord *exported = xr_semantic_plan_source_export(match, target->source_export);
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(plan, target->operation);
        uint32_t klass = xr_semantic_imported_class_construction_source_class(
            plan, match, required, exported, operation, &function);
        const XrSemanticFunctionRecord *callee = xr_semantic_plan_function(match, function);
        XrStableId zero = {{0}};
        if (klass == XR_SEMANTIC_INDEX_NONE || !exported ||
            !xr_stable_id_equal(exported->id, target->export_identity) ||
            !xr_stable_id_equal(target->callee_function, callee ? callee->id : zero))
            return -1;
    } else {
        function = xr_semantic_source_dependency_call_function(plan, target, match);
        if (function == XR_SEMANTIC_INDEX_NONE)
            return -1;
        if (target->kind == XR_SEM_CALL_TARGET_SOURCE_EXPORT &&
            !xr_semantic_coroutine_source_export_shape(plan, target, match, function))
            return -1;
        if (target->kind == XR_SEM_CALL_TARGET_SOURCE_METHOD_DEPENDENCY) {
            const XrSemanticFunctionRecord *callee = xr_semantic_plan_function(match, function);
            const XrSemanticSourceClassRecord *klass = callee ?
                xr_semantic_plan_source_class(match, callee->source_class) : NULL;
            if (!klass)
                return -1;
            if ((klass->flags & XR_SEM_SOURCE_CLASS_EXPLICIT_FINAL) == 0)
                return 1;
        }
    }
    if (graph->used)
        graph->used[dependency] = 1;
    *out = (XrSemanticCoroutineFunctionNode) {dependency, function};
    return 0;
}

static inline bool xr_semantic_coroutine_graph_enqueue(
    const XrSemanticCoroutineModuleGraph *graph, const uint32_t *offsets, uint8_t *visited,
    XrSemanticCoroutineFunctionNode node, XrSemanticCoroutineFunctionNode *queue, uint32_t *end) {
    if (node.module >= graph->count || node.function >=
        xr_semantic_plan_function_count(graph->modules[node.module].plan))
        return false;
    uint32_t index = offsets[node.module] + node.function;
    if (!visited[index]) {
        visited[index] = 1;
        queue[(*end)++] = node;
        if (graph->used)
            graph->used[node.module] = 1;
    }
    return true;
}

static inline int xr_semantic_function_graph_suspendability(
    const XrSemanticCoroutineModuleGraph *graph, uint32_t owner, uint32_t function) {
    if (!graph || !graph->modules || owner >= graph->count || graph->count == 0 ||
        !xr_semantic_plan_function(graph->modules[owner].plan, function))
        return -1;
    uint32_t *offsets = (uint32_t *) xr_malloc((size_t) graph->count * sizeof(*offsets));
    if (!offsets)
        return -1;
    uint32_t count = 0;
    for (uint32_t m = 0; m < graph->count; m++) {
        size_t functions = xr_semantic_plan_function_count(graph->modules[m].plan);
        if (!graph->modules[m].plan || functions > UINT32_MAX - count) {
            xr_free(offsets);
            return -1;
        }
        offsets[m] = count;
        count += (uint32_t) functions;
    }
    uint8_t *visited = (uint8_t *) xr_calloc(count, 1);
    XrSemanticCoroutineFunctionNode *queue =
        (XrSemanticCoroutineFunctionNode *) xr_malloc((size_t) count * sizeof(*queue));
    if (!visited || !queue) {
        xr_free(offsets);
        xr_free(visited);
        xr_free(queue);
        return -1;
    }
    uint32_t begin = 0, end = 0;
    bool valid = xr_semantic_coroutine_graph_enqueue(graph, offsets, visited,
        (XrSemanticCoroutineFunctionNode) {owner, function}, queue, &end);
    bool suspended = false, unknown = false;
    while (valid && begin < end) {
        XrSemanticCoroutineFunctionNode node = queue[begin++];
        const XrSemanticPlan *plan = graph->modules[node.module].plan;
        const XrSemanticFunctionRecord *body = xr_semantic_plan_function(plan, node.function);
        for (uint32_t b = 0; valid && b < body->block_count; b++) {
            const XrSemanticBlockRecord *block = xr_semantic_plan_block(plan, body->block_begin + b);
            if (!block || block->function != node.function) {
                valid = false;
                break;
            }
            for (uint32_t o = 0; valid && o < block->operation_count; o++) {
                uint32_t operation_index = block->operation_begin + o;
                const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(plan, operation_index);
                if (!operation || operation->function != node.function) {
                    valid = false;
                    break;
                }
                suspended |= (operation->effects & XI_EFFECT_MAY_SUSPEND) != 0 || operation->opcode == XI_GO;
                if (operation->opcode != XI_CALL && operation->opcode != XI_TAIL_CALL &&
                    operation->opcode != XI_CALL_METHOD)
                    continue;
                const XrSemanticCallTargetRecord *target = NULL;
                for (uint32_t t = 0; t < xr_semantic_plan_call_target_count(plan); t++) {
                    const XrSemanticCallTargetRecord *candidate = xr_semantic_plan_call_target(plan, t);
                    if (candidate->operation != operation_index)
                        continue;
                    if (target) {
                        valid = false;
                        break;
                    }
                    target = candidate;
                }
                XrSemanticCoroutineFunctionNode edge = {0, XR_SEMANTIC_INDEX_NONE};
                int effect = target ? xr_semantic_coroutine_graph_call(graph, node.module, target, &edge) :
                    xr_semantic_coroutine_sync_intrinsic(plan, operation) ? 0 : -1;
                suspended |= effect == 1;
                unknown |= effect < 0;
                if (effect == 0 && edge.function != XR_SEMANTIC_INDEX_NONE)
                    valid = xr_semantic_coroutine_graph_enqueue(graph, offsets, visited, edge, queue, &end);
            }
        }
    }
    xr_free(offsets);
    xr_free(visited);
    xr_free(queue);
    return !valid ? -1 : suspended ? 1 : unknown ? -1 : 0;
}

#endif  // XR_SEMANTIC_COROUTINE_MODULE_SHAPE_H
