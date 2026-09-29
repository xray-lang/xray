/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_path_verify.inc.c - Typed logical projection admission
 */
static XrXirStatus path_field_type(const XrXirInstruction *op,
    XrXirType parent, VerifyContext *context) {
    const XrXirTypes *types = context->module->types;
    const XrXirTypeNode *node = xr_xir_type_node(types, parent);
    if (!node || !xr_xir_type_is_struct(types, parent) || op->immediate < 0 ||
        (uint64_t)op->immediate > UINT32_MAX) return XR_XIR_BAD_TYPE;
    uint32_t field = (uint32_t)op->immediate;
    XrXirStatus status = xr_xir_nominal_access(context->module, context->location.function,
        node->nominal.declaration, field, XR_XIR_NOMINAL_READ, &context->remaining.work);
    if (status != XR_XIR_OK) return status;
    if (types->nominals->declarations) {
        const XrXirNominalDeclaration *declaration = &types->nominals->declarations[node->nominal.declaration];
        if (field >= declaration->field_count) return XR_XIR_BAD_STRUCTURE;
        return xr_xir_type_substitution_matches(types, node->nominal.arguments, node->nominal.argument_count,
            declaration->fields[field].type, op->type, &context->remaining);
    }
    return field < node->nominal.field_count && node->nominal.fields[field] == op->type ? XR_XIR_OK : XR_XIR_BAD_TYPE;
}
static XrXirStatus path_uses(const Graph *graph, const XrXirFunction *function,
    VerifyContext *context, uint32_t index) {
    const XrXirInstruction *op = &function->instructions[index];
    if (xr_xir_place_kind(function, op->args[0]) == XR_XIR_PLACE_NONE) return XR_XIR_BAD_VALUE;
    XrXirType parent = xr_xir_operand_type(function, op->args[0]);
    XrXirStatus status = role_operand(function, graph, context, index, 0, parent);
    if (status != XR_XIR_OK) return status;
    if (op->op == XR_XIR_FIELD_PLACE) return path_field_type(op, parent, context);
    if (op->op == XR_XIR_INDEX_PLACE) {
        if (!xr_xir_type_is_array(context->module->types, parent) ||
            xr_xir_array_element(context->module->types, parent) != op->type) return XR_XIR_BAD_TYPE;
        return role_operand(function, graph, context, index, 1, XR_XIR_I64);
    }
    if (op->op == XR_XIR_PLACE_READ) return op->type == parent ? XR_XIR_OK : XR_XIR_BAD_TYPE;
    return role_operand(function, graph, context, index, 1, parent);
}
