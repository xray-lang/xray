/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_hash_verify.inc.c - Exact hash-container operands and writable roots
 */
static XrXirStatus hash_uses(const Graph *graph, const XrXirFunction *function,
    VerifyContext *context, uint32_t instruction) {
    const XrXirInstruction *op = &function->instructions[instruction];
    const XrXirTypes *types = context->module->types;
    bool constructing = op->op == XR_XIR_MAP_NEW || op->op == XR_XIR_SET_NEW;
    uint32_t receiver = xr_xir_op_uses_operand_table(op->op) && !constructing ?
        function->operands[op->args[0]] : op->args[0];
    XrXirType container = constructing ? op->type : xr_xir_operand_type(function, receiver);
    const XrXirTypeNode *node = xr_xir_type_node(types, container);
    if (!node || (node->kind != XR_XIR_TYPE_MAP && node->kind != XR_XIR_TYPE_SET)) return XR_XIR_BAD_TYPE;
    bool map = node->kind == XR_XIR_TYPE_MAP;
    bool positional = op->op == XR_XIR_HASH_KEY_AT || op->op == XR_XIR_MAP_VALUE_AT;
    if (positional && ((op->op == XR_XIR_MAP_VALUE_AT && !map) ||
        op->type != (op->op == XR_XIR_MAP_VALUE_AT ? node->result : node->element))) return XR_XIR_BAD_TYPE;
    bool binary_set = op->op >= XR_XIR_SET_UNION && op->op <= XR_XIR_SET_IS_SUPERSET;
    if (binary_set && (map || op->type != (op->op >= XR_XIR_SET_IS_SUBSET ? XR_XIR_BOOL : container)))
        return XR_XIR_BAD_TYPE;
    if (((op->op == XR_XIR_MAP_NEW || op->op == XR_XIR_MAP_GET || op->op == XR_XIR_MAP_INDEX ||
        op->op == XR_XIR_MAP_SET || op->op == XR_XIR_HASH_KEYS || op->op == XR_XIR_MAP_ENTRIES) && !map) ||
        ((op->op == XR_XIR_SET_NEW || op->op == XR_XIR_SET_ADD) && map)) return XR_XIR_BAD_TYPE;
    if (op->op == XR_XIR_MAP_CONTAINS_VALUE && (!map || op->type != XR_XIR_BOOL)) return XR_XIR_BAD_TYPE;
    XrXirConstraint key = {.markers = XR_XIR_CONSTRAINT_HASHABLE};
    XrXirStatus status = xir_effect_evidence_is_instance(context->module) ?
        xr_xir_compile_type_constraints(&context->remaining, context->module, context->location.function, node->element, key) :
        xr_xir_compile_type_satisfies(&context->remaining, context->module, context->location.function, node->element, key);
    if (status != XR_XIR_OK) return status;
    if (op->op == XR_XIR_MAP_CONTAINS_VALUE) {
        XrXirConstraint equal = {.markers = XR_XIR_CONSTRAINT_EQUAL};
        status = xir_effect_evidence_is_instance(context->module) ?
            xr_xir_compile_type_constraints(&context->remaining, context->module, context->location.function, node->result, equal) :
            xr_xir_compile_type_satisfies(&context->remaining, context->module, context->location.function, node->result, equal);
        if (status != XR_XIR_OK) return status;
    }
    if (op->op == XR_XIR_MAP_GET && (!xr_xir_type_is_nullable(types, op->type) ||
        xr_xir_nullable_element(types, op->type) != node->result)) return XR_XIR_BAD_TYPE;
    if (op->op == XR_XIR_MAP_INDEX && op->type != node->result) return XR_XIR_BAD_TYPE;
    if (op->op >= XR_XIR_HASH_KEYS && op->op <= XR_XIR_MAP_ENTRIES) {
        if (!xr_xir_type_is_array(types, op->type)) return XR_XIR_BAD_TYPE;
        XrXirType element = xr_xir_array_element(types, op->type);
        if (op->op == XR_XIR_MAP_ENTRIES) {
            const XrXirTypeNode *tuple = xr_xir_tuple_signature(types, element);
            if (!tuple || tuple->parameter_count != 2 ||
                tuple->parameters[0].mode || tuple->parameters[1].mode ||
                tuple->parameters[0].type != node->element || tuple->parameters[1].type != node->result)
                return XR_XIR_BAD_TYPE;
        } else if (element != (op->op == XR_XIR_HASH_KEYS || !map ? node->element : node->result))
            return XR_XIR_BAD_TYPE;
    }
    uint32_t count = operand_count(function, op, context->module);
    if (op->op == XR_XIR_MAP_NEW && ((count & 1u) || node->result == XR_XIR_UNIT)) return XR_XIR_BAD_TYPE;
    if (!xir_compile_work(&context->remaining, count)) return XR_XIR_BUDGET;
    for (uint32_t a = 0; a < count; ++a) {
        XrXirType expected;
        if (constructing) expected = map && (a & 1u) ? node->result : node->element;
        else expected = !a || binary_set ? container : positional ? XR_XIR_I64 : (op->op == XR_XIR_MAP_SET && a == 2) || op->op == XR_XIR_MAP_CONTAINS_VALUE ? node->result : node->element;
        status = role_operand(function, graph, context, instruction, a, expected);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
