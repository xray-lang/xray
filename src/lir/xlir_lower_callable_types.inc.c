/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_lower_callable_types.inc.c - Receive existing exact callable contracts
 */
static bool lower_receive_callable(Lower *c, XrXirType type, const XrXirTypeNode *node) {
    if (!c->constructed_types) {
        c->constructed_types = lower_alloc(c, c->source->types->count, sizeof(*c->constructed_types));
        if (!c->constructed_types) return false;
    }
    uint32_t index = (uint32_t)type - XR_XIR_CONSTRUCTED_TYPE_BASE;
    if (c->constructed_types[index]) return true;
    if (node->element != XR_XIR_NEVER || (node->flags & XR_XIR_CALLABLE_REST))
        return lower_unsupported(c, "throwing or variadic callable ABI");
    const XrXirTypeNode *result = xr_xir_type_node(c->source->types, node->result);
    if (result && result->kind == XR_XIR_TYPE_CALLABLE)
        return lower_unsupported(c, "owned callable result transport");
    if (!lower_receive_type(c, node->result)) return false;
    XrLirCallableParameter *parameters = lower_alloc(c, node->parameter_count, sizeof(*parameters));
    if (node->parameter_count && !parameters) return false;
    bool good = true;
    for (uint32_t p = 0; p < node->parameter_count && good; ++p) {
        XrXirCallableParameter source = node->parameters[p];
        const XrXirTypeNode *parameter = xr_xir_type_node(c->source->types, source.type);
        if (source.mode == XR_PARAM_REF || (parameter && parameter->kind == XR_XIR_TYPE_CALLABLE)) {
            good = lower_unsupported(c, "nested or reference callable parameter transport");
            break;
        }
        good = lower_receive_type(c, source.type);
        parameters[p] = (XrLirCallableParameter){.type=lower_type(c, source.type),
            .array_element=lower_array_element(c, source.type), .mode=(XrParamMode)source.mode,
            .escaping=source.escaping != 0};
        if (!parameters[p].type) good = lower_unsupported(c, "callable parameter physical type");
    }
    XrLirCallableType shape = {.parameters=parameters, .parameter_count=node->parameter_count,
        .result=lower_type(c, node->result), .result_element=lower_array_element(c, node->result),
        .flags=(node->flags & XR_XIR_CALLABLE_NO_SUSPEND ? XR_LIR_CALLABLE_NO_SUSPEND : 0u) |
            (node->flags & XR_XIR_CALLABLE_C_ABI ? XR_LIR_CALLABLE_C_ABI : 0u)};
    if (good && !shape.result) good = lower_unsupported(c, "callable result physical type");
    if (good) good = lower_build(c, xr_lir_callable_type(c->module, &shape, &c->constructed_types[index]));
    xr_free(parameters);
    return good;
}
