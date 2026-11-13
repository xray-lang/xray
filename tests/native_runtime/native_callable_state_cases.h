/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static bool callable_state_retain(void *owner) {
    ++*(uint32_t *)owner;
    return true;
}
static void callable_state_release(void *owner) { CHECK(*(uint32_t *)owner); --*(uint32_t *)owner; }

static void callable_state_values(void) {
    XrtCallableTypeSpec callables[] = {
        {.kind=XRT_CALLABLE_SIGNATURE, .result={XRT_CALLABLE_SCALAR, XR_POD_I64}, .flags=1},
        {.kind=XRT_CALLABLE_SIGNATURE, .result={XRT_CALLABLE_SCALAR, XR_POD_I64}},
        {.kind=XRT_CALLABLE_SIGNATURE, .result={XRT_CALLABLE_SCALAR, XR_POD_BOOL}}
    };
    XrtModuleTypeSpec type = {XRT_MODULE_CALLABLE, 1};
    XrtModuleSpec module = {0};
    XrtModuleSlotSpec slot = {0, 0, XRT_MODULE_SLOT_MUTABLE, 0, sizeof(void *), _Alignof(void *)};
    XrtModuleLayoutSpec spec = {.types=&type, .type_count=1, .modules=&module, .module_count=1,
        .slots=&slot, .slot_count=1, .payload_size=sizeof(void *), .payload_alignment=_Alignof(void *),
        .byte_limit=UINT64_MAX, .callable_types=callables, .callable_type_count=3};
    XrtModuleLayout *layout = NULL;
    OK(xrt_module_layout_new(&spec, &layout));
    XrXirTypeArena *arena = xrt_module_callable_catalog(layout);
    CHECK(arena);
    XrtCallableEntry entries[] = {{(XrXirType)260, NULL, 0}, {(XrXirType)262, NULL, 0}};
    uint32_t references = 1;
    XrtCallableCode *code = NULL;
    OK(xrt_callable_code_new(arena, entries, 2,
        (XrtCallableLease){&references, callable_state_retain, callable_state_release}, &code));
    XrXirDomain *domain = NULL;
    OK(xr_xir_domain_new(UINT64_MAX, &domain));
    XrXirValue function = {0}, wrong = {0};
    OK(xrt_callable_new(code, domain, 0, NULL, 0, &function));
    OK(xrt_callable_new(code, domain, 1, NULL, 0, &wrong));
    XrtModuleState *state = NULL;
    OK(xrt_module_state_new(layout, UINT64_MAX, &state));
    XrtModuleRef *reference = NULL;
    OK(xrt_module_begin(state, 0));
    void *value = (void *)(intptr_t)function.payload;
    OK(xrt_module_construct(state, 0, &value, &reference));
    OK(xrt_module_publish(reference));
    OK(xrt_module_ready(state, 0));
    const void *borrowed = NULL;
    OK(xrt_module_borrow(reference, &borrowed));
    void *saved = NULL;
    memcpy(&saved, borrowed, sizeof(saved));
    CHECK(saved == value);
    value = (void *)(intptr_t)wrong.payload;
    CHECK(xrt_module_replace(reference, &value) == XR_XIR_VALUE_BAD_ARGUMENT);
    void *current = NULL;
    memcpy(&current, borrowed, sizeof(current));
    CHECK(current == saved);
    xr_xir_value_drop(&function); xr_xir_value_drop(&wrong);
    xrt_callable_code_drop(&code);
    callable_state_release(&references);
    xrt_module_layout_drop(&layout);
    CHECK(references == 1);
    xrt_module_state_clear(state);
    CHECK(references == 0);
    OK(xrt_module_begin(state, 0));
    xrt_module_state_drop(&state);
    OK(xr_xir_domain_close(domain));
    xr_xir_domain_drop(domain);
    CHECK(!live);
}
