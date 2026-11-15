/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Signature owners preserve declaration names and written argument permutations. */
typedef struct BindingNames {
    char text[3][8];
    XrXirLiteral names[3];
    unsigned calls;
} BindingNames;

static XrXirStatus binding_names(void *data, const XrHirSignatureInput *signature,
                               XrHirCallDeclarationNames *output) {
    BindingNames *names = data;
    CHECK(signature->family == XR_HIR_CALL_FUNCTION && signature->function < 2);
    const char *const ordinary[] = {"a", "b", "c"};
    for (unsigned i = 0; i < 3; ++i) {
        const char *name = signature->function == 1 && i == 2 ? "rest" : ordinary[i];
        memcpy(names->text[i], name, strlen(name) + 1);
        names->names[i] = (XrXirLiteral){names->text[i], (uint32_t)strlen(name)};
    }
    ++names->calls;
    *output = (XrHirCallDeclarationNames){names->names, 3};
    return XR_XIR_OK;
}

static XrHirBodySignature binding_signature(XrHirBody *body, XrHirSignatureInput input) {
    XrHirBodySignature signature = {0};
    CHECK(xr_hir_body_signature(body, &input, NULL, &signature, NULL) == XR_XIR_OK);
    return signature;
}

static void call_binding_receivers(void) {
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrXirCallableParameter callable_parameters[] = {
        {XR_XIR_I64, XR_PARAM_READ, 0}, {XR_XIR_I64, XR_PARAM_READ, 0}, {XR_XIR_I64, XR_PARAM_READ, 0}};
    XrXirTypeNode nodes[] = {
        {.kind=XR_XIR_TYPE_CALLABLE, .element=XR_XIR_NEVER, .parameters=callable_parameters,
         .parameter_count=3, .result=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_ARRAY, .element=XR_XIR_I64}};
    XrXirTypes types = {.nodes=nodes, .count=2};
    XrXirType formals[] = {XR_XIR_I64, XR_XIR_I64, XR_XIR_I64};
    XrXirType rest_formals[] = {XR_XIR_I64, XR_XIR_I64, (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + 1)};
    XrXirFunction functions[] = {
        {.parameters=formals, .parameter_count=3, .result=XR_XIR_I64},
        {.parameters=rest_formals, .parameter_count=3, .result=XR_XIR_I64}, {.result=XR_XIR_I64}};
    XrXirFunction *function_view = functions;
    XrXirFunctionIdentity identities[3] = {{0}, {.promises=XR_XIR_FUNCTION_REST}, {0}};
    XrXirFunctionIdentity *identity_view = identities;
    uint32_t function_count = 3, module_count = 1;
    XrXirDefaultBinding default_record = {.owner_kind=XR_XIR_DEFAULT_PARAMETER, .owner=0, .ordinal=1, .function=2};
    XrXirDefaultTable defaults = {.records=&default_record, .count=1};
    BindingNames names = {0};
    XrHirBodyDeclarations declarations = {.functions=&function_view, .identities=&identity_view,
        .function_count=&function_count, .module_count=&module_count, .defaults=&defaults,
        .call_names=binding_names, .call_names_data=&names};
    XrHirBody *body = NULL;
    CHECK(xr_hir_body_new(resources, &types, &body) == XR_XIR_OK);
    CHECK(xr_hir_body_declarations(body, &declarations) == XR_XIR_OK);
    XrHirBodyScope scope = {0};
    CHECK(xr_hir_body_scope(body, scope, (XrHirBodyOrigin){0}, NULL, &scope) == XR_XIR_OK);
    XrHirSignatureInput input = {.types=&types, .scope=scope, .family=XR_HIR_CALL_FUNCTION, .count=3};
    XrHirBodySignature signature = binding_signature(body, input);
    XrHirCallBindingError error = {0};
    size_t bytes = physical_bytes, live = physical_live;
    for (size_t point = 0; point < 2; ++point) {
        physical_fail = physical_attempts + point;
        CHECK(xr_hir_body_call_bind_open(body, signature, NULL, &error) == XR_XIR_OUT_OF_MEMORY);
        physical_fail = SIZE_MAX;
        CHECK(physical_bytes == bytes && physical_live == live);
    }
    CHECK(xr_hir_body_call_bind_open(body, signature, NULL, &error) == XR_XIR_OK);
    memset(names.text, '?', sizeof(names.text));
    uint32_t formal = UINT32_MAX;
    CHECK(xr_hir_body_call_bind_argument(body, signature, "missing", 7, 1, false, NULL, &formal, &error) == XR_XIR_BAD_TYPE);
    CHECK(error.kind == XR_HIR_CALL_BIND_UNKNOWN && formal == UINT32_MAX);
    CHECK(xr_hir_body_call_bind_argument(body, signature, "c", 1, 1, false, NULL, &formal, &error) == XR_XIR_OK && formal == 2);
    CHECK(xr_hir_body_call_bind_argument(body, signature, "c", 1, 1, false, NULL, &formal, &error) == XR_XIR_BAD_TYPE);
    CHECK(error.kind == XR_HIR_CALL_BIND_DUPLICATE && error.formal == 2 && error.previous_argument == 0);
    CHECK(xr_hir_body_call_bind_argument(body, signature, NULL, 0, 0, true, NULL, &formal, &error) == XR_XIR_BAD_TYPE);
    CHECK(error.kind == XR_HIR_CALL_BIND_POSITIONAL_AFTER_NAMED);
    XrHirCallBinding binding = {0};
    CHECK(xr_hir_body_call_bind_seal(body, signature, NULL, &binding, &error) == XR_XIR_BAD_TYPE);
    CHECK(error.kind == XR_HIR_CALL_BIND_MISSING && error.formal == 0 && !binding.argument_formals);
    CHECK(xr_hir_body_call_bind_argument(body, signature, "a", 1, 1, false, NULL, &formal, &error) == XR_XIR_OK && formal == 0);
    CHECK(xr_hir_body_call_bind_seal(body, signature, NULL, &binding, &error) == XR_XIR_OK);
    CHECK(binding.argument_count == 2 && binding.parameter_count == 3);
    CHECK(binding.argument_formals[0] == 2 && binding.argument_formals[1] == 0);
    CHECK(binding.formal_arguments[0] == 1 && binding.formal_arguments[1] == UINT32_MAX && binding.formal_arguments[2] == 0);
    XrHirBodyUse use = {0};
    XrHirBodyAction action = {0};
    XrHirCallUseInput access = {.types=&types, .scope=scope, .kind=XR_HIR_CALL_ACCESS,
        .formal=0, .argument=0, .access=XR_CALL_ARG_PLAIN};
    CHECK(xr_hir_body_call_argument(body, signature, &access, NULL, &use, &action) == XR_XIR_BAD_STRUCTURE);
    access.formal=2;
    CHECK(xr_hir_body_call_argument(body, signature, &access, NULL, &use, &action) == XR_XIR_OK);
    XrHirCallBinding observed={0};
    CHECK(xr_hir_body_call_binding(body, signature, &observed) && observed.argument_formals[0]==2);
    for (unsigned i = 0; i < 64; ++i) (void)binding_signature(body, input);
    CHECK(binding.argument_formals[0] == 2 && binding.formal_arguments[0] == 1);
    signature = binding_signature(body, input);
    CHECK(xr_hir_body_call_bind_open(body, signature, NULL, &error) == XR_XIR_OK);
    physical_fail = physical_attempts;
    CHECK(xr_hir_body_call_bind_argument(body, signature, NULL, 0, 1, false, NULL, &formal, &error) == XR_XIR_OUT_OF_MEMORY);
    physical_fail = SIZE_MAX;
    CHECK(xr_hir_body_call_bind_argument(body, signature, NULL, 0, 1, false, NULL, &formal, &error) == XR_XIR_OK && formal == 0);
    CHECK(xr_hir_body_call_bind_argument(body, signature, "a", 1, 1, false, NULL, &formal, &error) == XR_XIR_BAD_TYPE);
    CHECK(error.kind == XR_HIR_CALL_BIND_DUPLICATE && error.previous_argument == 0);
    input.function = 1;
    signature = binding_signature(body, input);
    CHECK(xr_hir_body_call_bind_open(body, signature, NULL, &error) == XR_XIR_OK);
    CHECK(xr_hir_body_call_bind_argument(body, signature, "rest", 4, 1, false, NULL, &formal, &error) == XR_XIR_BAD_TYPE);
    CHECK(error.kind == XR_HIR_CALL_BIND_NAMED_REST);
    CHECK(xr_hir_body_call_bind_argument(body, signature, NULL, 0, 5, true, NULL, &formal, &error) == XR_XIR_OK);
    CHECK(xr_hir_body_call_bind_seal(body, signature, NULL, &binding, &error) == XR_XIR_OK);
    CHECK(binding.argument_count == 5 && binding.argument_formals[0] == 0 && binding.argument_formals[4] == 2);
    input.family = XR_HIR_CALL_INDIRECT; input.callable = (XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    signature = binding_signature(body, input);
    unsigned calls = names.calls;
    CHECK(xr_hir_body_call_bind_open(body, signature, NULL, &error) == XR_XIR_BAD_TYPE);
    CHECK(error.kind == XR_HIR_CALL_BIND_INDIRECT && names.calls == calls);
    xr_hir_body_end(body);
    CHECK(xr_hir_body_call_bind_seal(body, signature, NULL, &binding, &error) == XR_XIR_BAD_STRUCTURE);
    xr_hir_body_free(body);
    xr_compile_resources_release(resources);
    CHECK(!physical_live && !physical_bytes);
    puts("Call bindings: authentic names, owned permutation, noncontiguous defaults, spread/rest and allocation rollback PASS");
}
