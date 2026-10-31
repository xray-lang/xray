/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_type_description.inc.c - Canonical compiler descriptions and ordered fields
 */
XR_FUNC XrXirStatus xr_xir_compile_type_description(const XrXirCompileContext *context,
    const XrXirTypes *types, XrXirType type, XrXirTypeDescription *output) {
    if (!xir_compile_context_valid(context) || !output) return XR_XIR_BAD_STRUCTURE;

    XrXirTypeDescription description = {0}; description.type = type;
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    if (!node) {
        if (type != XR_XIR_UNIT && type != XR_XIR_NEVER && type != XR_XIR_BOOL && type != XR_XIR_RUNE &&
            !xr_xir_type_is_number(type) && type != XR_XIR_STRING &&
            type != XR_XIR_ERROR && type != XR_XIR_PANIC_INFO && type != XR_XIR_BUFFER_STORAGE) return XR_XIR_BAD_TYPE;
    } else {
        if (node->kind < XR_XIR_TYPE_CALLABLE || node->kind > XR_XIR_TYPE_UNION)
            return XR_XIR_BAD_TYPE;
        description.kind = node->kind; description.node = node;
        if (node->kind == XR_XIR_TYPE_TUPLE) {
            if (!node->parameter_count || !node->parameters) return XR_XIR_BAD_STRUCTURE;
            description.field_count = node->parameter_count;
        } else if (node->kind == XR_XIR_TYPE_NOMINAL) {
            const XrXirNominalTable *table = types->nominals;
            if (!table || node->nominal.declaration >= table->count ||
                (table->declarations != NULL) == (table->identities != NULL)) return XR_XIR_BAD_TYPE;
            uint32_t index = node->nominal.declaration;
            const XrXirNominalDeclaration *definition = table->declarations ? &table->declarations[index] : NULL;
            const XrXirNominalIdentity *identity = definition ? NULL : &table->identities[index];
            description.module = definition ? definition->module : identity->module;
            description.name = definition ? definition->name : identity->name;
            description.nominal_kind = definition ? definition->kind : identity->kind;
            description.field_count = definition ? definition->field_count : identity->field_count;
            description.variants = definition ? definition->variants : identity->variants;
            description.variant_count = definition ? definition->variant_count : identity->variant_count;
            description.declaration = index; description.instance = &node->nominal;
            description.definition = definition; description.identity = identity;
            if (!description.module.bytes || !description.module.length ||
                !description.name.bytes || !description.name.length ||
                description.nominal_kind > XR_XIR_NOMINAL_CLASS ||
                (description.field_count && !(definition ? definition->fields != NULL : identity->fields != NULL)) ||
                (description.variant_count != 0) != (description.variants != NULL) ||
                (description.nominal_kind == XR_XIR_NOMINAL_ENUM) != (description.variant_count != 0))
                return XR_XIR_BAD_STRUCTURE;
        }
    }
    *output = description; return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_description_field(const XrXirCompileContext *context,
    const XrXirTypeDescription *description, uint32_t field, XrXirFieldDescription *output) {
    if (!xir_compile_context_valid(context) || !description || !output) return XR_XIR_BAD_STRUCTURE;

    if (field >= description->field_count) return XR_XIR_BAD_STRUCTURE;
    XrXirFieldDescription result = {0};
    if (description->kind == XR_XIR_TYPE_TUPLE) {
        if (!description->node || description->node->kind != XR_XIR_TYPE_TUPLE ||
            description->node->parameter_count != description->field_count ||
            !description->node->parameters) return XR_XIR_BAD_STRUCTURE;
        result.type = result.template_type = description->node->parameters[field].type;
        result.resolved = true;
    } else if (description->kind == XR_XIR_TYPE_NOMINAL) {
        if (!description->node || description->node->kind != XR_XIR_TYPE_NOMINAL ||
            description->instance != &description->node->nominal ||
            (description->definition != NULL) == (description->identity != NULL)) return XR_XIR_BAD_STRUCTURE;
        bool resolved = description->instance->field_count == description->field_count &&
            description->instance->fields != NULL;
        if (description->instance->field_count && !resolved) return XR_XIR_BAD_STRUCTURE;
        if (description->definition) {
            if (!description->definition->fields || description->definition->field_count != description->field_count)
                return XR_XIR_BAD_STRUCTURE;
            const XrXirNominalField *member = &description->definition->fields[field];
            /* owned: synchronous canonical-pool/Source owner view; persistent receivers copy names. */
            result.name = member->name;
            result.flags = member->flags; result.template_type = member->type;
        } else {
            if (!description->identity->fields || description->identity->field_count != description->field_count ||
                !resolved) return XR_XIR_BAD_STRUCTURE;
            const XrXirNominalFieldIdentity *member = &description->identity->fields[field];
            /* owned: synchronous canonical identity-pool view; persistent receivers copy names. */
            result.name = member->name;
            result.flags = member->flags; result.template_type = description->instance->fields[field];
        }
        result.resolved = resolved;
        if (resolved) result.type = description->instance->fields[field];
    } else return XR_XIR_BAD_TYPE;
    *output = result; return XR_XIR_OK;
}
