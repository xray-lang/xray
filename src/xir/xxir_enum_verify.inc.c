/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_enum_verify.inc.c - Definition-bound enum variant and payload authority
 */
static XrXirStatus enum_uses(const Graph *graph, const XrXirFunction *function,
    VerifyContext *context, uint32_t instruction) {
    const XrXirInstruction *op = &function->instructions[instruction];
    bool construct = op->op == XR_XIR_ENUM_NEW, tag = op->op == XR_XIR_ENUM_TAG;
    XrXirType type = construct ? op->type : xr_xir_operand_type(function, op->args[0]);
    const XrXirTypes *types = context->module->types;
    if (!xr_xir_type_is_enum(types, type)) return XR_XIR_BAD_TYPE;
    const XrXirNominalType *instance = &xr_xir_type_node(types, type)->nominal;
    const XrXirNominalTable *table = types->nominals;
    const XrXirNominalDeclaration *d = table->declarations ? &table->declarations[instance->declaration] : NULL;
    const XrXirNominalIdentity *identity = d ? NULL : &table->identities[instance->declaration];
    const XrXirNominalVariant *variants = d ? d->variants : identity->variants;
    uint32_t variant_count = d ? d->variant_count : identity->variant_count;
    if (!tag && (op->immediate < 0 || (uint64_t) op->immediate >= variant_count)) return XR_XIR_BAD_STRUCTURE;
    const XrXirNominalVariant *variant = tag ? NULL : &variants[(uint32_t) op->immediate];
    if (!tag && (construct ? op->args[1] != variant->field_count : op->args[1] >= variant->field_count))
        return XR_XIR_BAD_STRUCTURE;
    uint32_t first = tag || construct ? 0 : variant->field_begin + op->args[1];
    XrXirStatus status = xr_xir_nominal_access(context->module, context->location.function,
        instance->declaration, first, construct ? XR_XIR_NOMINAL_CONSTRUCT : tag ? XR_XIR_NOMINAL_TYPE : XR_XIR_NOMINAL_READ,
        &context->remaining.work);
    if (status != XR_XIR_OK) return status;
    if (!construct) {
        status = local_operand(types, function, op, op->args[0], 0);
        if (status == XR_XIR_OK) status = value_use(function, graph, instruction, op->args[0], type);
        if (status != XR_XIR_OK || tag) return status;
    }
    uint32_t count = construct ? variant->field_count : 1;
    if (!spend(&context->remaining.work, count)) return XR_XIR_BUDGET;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t field = construct ? variant->field_begin + i : first;
        uint32_t value = construct ? function->operands[op->args[0] + i] : op->args[0];
        XrXirType actual = construct ? xr_xir_operand_type(function, value) : op->type;
        if (d) status = xr_xir_type_substitution_matches(types, instance->arguments,
            instance->argument_count, d->fields[field].type, actual, &context->remaining);
        else status = field < instance->field_count && instance->fields[field] == actual ? XR_XIR_OK : XR_XIR_BAD_TYPE;
        if (status == XR_XIR_OK && construct) status = local_operand(types, function, op, value, i);
        if (status == XR_XIR_OK && construct) status = value_use(function, graph, instruction, value, actual);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
