/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_nominal_description.inc.c - Canonical borrowed enum descriptions
 *
 * KEY CONCEPT:
 *   Source and instruction checking consume the same declaration or identity
 *   record. Describing it never replaces operation-specific access checks.
 */
XR_FUNC XrXirStatus xr_xir_compile_enum_description(const XrXirCompileContext *context,
    const XrXirTypes *types, XrXirType type, XrXirEnumDescription *output) {
    if (!xir_compile_context_valid(context) || !output) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
    if (!xr_xir_type_is_enum(types, type)) return XR_XIR_BAD_TYPE;
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    const XrXirNominalTable *table = types->nominals;
    uint32_t index = node->nominal.declaration;
    const XrXirNominalDeclaration *definition = table->declarations ? &table->declarations[index] : NULL;
    const XrXirNominalIdentity *identity = definition ? NULL : &table->identities[index];
    XrXirEnumDescription description = {0};
    description.module = definition ? definition->module : identity->module;
    description.name = definition ? definition->name : identity->name;
    description.variants = definition ? definition->variants : identity->variants;
    description.variant_count = definition ? definition->variant_count : identity->variant_count;
    description.declaration = index;
    description.instance = &node->nominal;
    description.definition = definition;
    if (!description.module.bytes || !description.module.length ||
        !description.name.bytes || !description.name.length ||
        !description.variants || !description.variant_count) return XR_XIR_BAD_STRUCTURE;
    *output = description;
    return XR_XIR_OK;
}
