/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_atomic_verify.inc.c - Definition-context proof for constructed Atomic operations
 *
 * KEY CONCEPT:
 *   Runtime capability never substitutes for constraints, visibility or SSA proof.
 */
static XrXirStatus atomic_result(const XrXirTypes *types, const XrXirInstruction *op,
    XrXirType element) {
    if (op->op == XR_XIR_ATOMIC_NEW) return XR_XIR_OK;
    if (op->op == XR_XIR_ATOMIC_LOAD || op->op == XR_XIR_ATOMIC_FETCH_ADD ||
        op->op == XR_XIR_ATOMIC_FETCH_SUB || op->op == XR_XIR_ATOMIC_SWAP)
        return op->type == element ? XR_XIR_OK : XR_XIR_BAD_TYPE;
    if (op->op == XR_XIR_ATOMIC_COMPARE_EXCHANGE) {
        const XrXirTypeNode *tuple = xr_xir_type_node(types, op->type);
        if (!tuple || tuple->kind != XR_XIR_TYPE_TUPLE || tuple->parameter_count != 2 ||
            !tuple->parameters || tuple->parameters[0].mode || tuple->parameters[1].mode ||
            tuple->parameters[0].type != element || tuple->parameters[1].type != XR_XIR_BOOL)
            return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}
/* Only already-proved SSA constructors expose a known ordinal. Parameters and
 * arbitrary arithmetic/immediates remain dynamic; the runtime checks them. */
static XrXirStatus atomic_known_ordering(const XrXirFunction *function,
    const XrXirTypes *types, XrXirType nullable, uint32_t value,
    const XrXirCompileContext *context, int *ordinal) {
    XrXirType expected = nullable;
    *ordinal = -1;
    for (uint32_t steps = 0; steps < function->instruction_count; ++steps) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        if (value < function->parameter_count) return XR_XIR_OK;
        uint32_t index = value - function->parameter_count;
        if (index >= function->instruction_count) return XR_XIR_BAD_VALUE;
        const XrXirInstruction *in = &function->instructions[index];
        if (in->type != expected) return XR_XIR_OK;
        if (in->op == XR_XIR_COPY || in->op == XR_XIR_SCALAR_COPY || in->op == XR_XIR_OWNED_RETAIN) {
            value = in->args[0];continue;
        }
        if (expected == nullable && in->op == XR_XIR_NULLABLE_NONE) {
            *ordinal = 4;return XR_XIR_OK;
        }
        if (expected == nullable && in->op == XR_XIR_NULLABLE_SOME) {
            expected = xr_xir_nullable_element(types, nullable);value = in->args[0];continue;
        }
        if (expected != nullable && in->op == XR_XIR_ENUM_NEW && !in->args[1] &&
            in->immediate >= 0 && in->immediate < 5) *ordinal = (int)in->immediate;
        return XR_XIR_OK;
    }
    return XR_XIR_OK;
}
static XrXirStatus atomic_uses(const Graph *graph, const XrXirFunction *function,
    VerifyContext *context, uint32_t instruction) {
    const XrXirInstruction *op = &function->instructions[instruction];
    const XrXirTypes *types = context->module->types;
    bool create = op->op == XR_XIR_ATOMIC_NEW;
    uint32_t required = create ? 1 : xr_xir_atomic_required_operands(op->op);
    uint32_t count = create ? 1 : op->args[1];
    uint32_t first = create ? op->args[0] : function->operands[op->args[0]];
    if (!xir_compile_work(&context->remaining, 1)) return XR_XIR_BUDGET;
    if (first >= (uint64_t)function->parameter_count + function->instruction_count) return XR_XIR_BAD_VALUE;
    XrXirType receiver = create ? op->type : xr_xir_operand_type(function, first);
    if (!xr_xir_type_is_atomic(types, receiver)) return XR_XIR_BAD_TYPE;
    XrXirType element = xr_xir_atomic_element(types, receiver);
    uint32_t marker = XR_XIR_CONSTRAINT_ATOMIC_VALUE;
    if (op->op >= XR_XIR_ATOMIC_ADD && op->op <= XR_XIR_ATOMIC_FETCH_SUB)
        marker = XR_XIR_CONSTRAINT_ATOMIC_NUMBER;
    if (op->op == XR_XIR_ATOMIC_TOGGLE) marker = XR_XIR_CONSTRAINT_ATOMIC_BOOLEAN;
    XrXirStatus status = xr_xir_compile_type_constraints(&context->remaining, context->module,
        context->location.function, element, (XrXirConstraint){.markers=marker});
    if (status != XR_XIR_OK) return status;
    status = atomic_result(types, op, element);
    if (status != XR_XIR_OK) return status;
    XrXirType ordering = XR_XIR_UNIT;
    if (!create && count > required) {
        if (!xir_compile_work(&context->remaining, 1)) return XR_XIR_BUDGET;
        uint32_t value = function->operands[op->args[0] + required];
        if (value >= (uint64_t)function->parameter_count + function->instruction_count) return XR_XIR_BAD_VALUE;
        ordering = xr_xir_operand_type(function, value);
        if (!xr_xir_type_is_nullable(types, ordering)) return XR_XIR_BAD_TYPE;
        if (!xir_compile_work(&context->remaining, 36)) return XR_XIR_BUDGET;
        if (!xr_xir_nominal_native_ordering(types, xr_xir_nullable_element(types, ordering))) return XR_XIR_BAD_TYPE;
        status = type_use_context(context, context->location.function, ordering);
        if (status != XR_XIR_OK) return status;
    }
    if (!xir_compile_work(&context->remaining, count)) return XR_XIR_BUDGET;
    for (uint32_t a = 0; a < count; ++a) {
        XrXirType expected = create || a ? element : receiver;
        if (!create && a == required) expected = ordering;
        status = role_operand(function, graph, context, instruction, a, expected);
        if (status != XR_XIR_OK) return status;
    }
    if (count > required && (op->op == XR_XIR_ATOMIC_LOAD || op->op == XR_XIR_ATOMIC_STORE)) {
        int ordinal = -1;
        status = atomic_known_ordering(function, types, ordering,
            function->operands[op->args[0] + required], &context->remaining, &ordinal);
        if (status != XR_XIR_OK) return status;
        if ((op->op == XR_XIR_ATOMIC_LOAD && (ordinal == 2 || ordinal == 3)) ||
            (op->op == XR_XIR_ATOMIC_STORE && (ordinal == 1 || ordinal == 3))) return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}
