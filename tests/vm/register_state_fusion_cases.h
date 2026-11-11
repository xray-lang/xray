/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * register_state_fusion_cases.h - A fused retain preserves its own failure site
 */
#ifndef REGISTER_STATE_FUSION_CASES_H
#define REGISTER_STATE_FUSION_CASES_H

static void module_state_fused_retain_failure(void) {
    XrtModuleTypeSpec type = {XRT_MODULE_STRING};
    XrtModuleSpec owner = {0};
    XrtModuleSlotSpec slot = {0, 0, XRT_MODULE_SLOT_MUTABLE, 0,
        sizeof(void *), _Alignof(void *)};
    XrtModuleLayoutSpec layout = {.types=&type, .type_count=1, .modules=&owner,
        .module_count=1, .slots=&slot, .slot_count=1, .payload_size=sizeof(void *),
        .payload_alignment=_Alignof(void *), .byte_limit=UINT64_MAX};
    XrtModuleLayout *storage = NULL;
    XR_CHECK(xrt_module_layout_new(&layout, &storage) == 0, "String state layout");
    uint32_t code[3], ids[] = {1, 2, 2};
    immediate(code, 0, XR_RBC_STATE_REFERENCE, 2, 0);
    emit(code, 1, XR_RBC_STATE_READ, 1, 2, XRT_MODULE_STRING);
    emit(code, 2, XR_RBC_RET, 1, 0, 0);
    XrRbcStateAccess access = {0};
    XrRbcRegisterKind kinds[] = {XR_RBC_STATE, XR_RBC_STRING, XR_RBC_STATE_REF};
    XrRbcFunction function = {.code=code, .code_count=3, .register_count=3,
        .parameter_count=1, .register_kinds=kinds, .register_kind_count=3,
        .result_kind=XR_RBC_STRING, .state_accesses=&access, .state_access_count=1,
        .source_ids=ids, .source_id_count=3};
    XrRbcModule module = {.functions=&function, .function_count=1, .state_layout=storage};
    const XrtSourceLocation points[] = {
        {.path="reference.xr", .path_length=12, .line=7, .column=2},
        {.path="retain.xr", .path_length=9, .line=19, .column=4}
    };
    XrtLocationOwner *locations = NULL;
    XR_CHECK(xrt_locations_create(points, 2, &locations) == XRT_LOCATION_OK, "separate operation sites");
    static const char bytes[] = "published String remains owned by its module across a failed guest retain";
    for (unsigned limited = 0; limited < 2; ++limited) {
        FrameObservation o = {0};
        XR_CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0, locations, &o.execution) == XR_RBC_OK,
            "owned execution prepared before the active String root");
        XrXirValue value = {0};
        XrXirDomain *domain = NULL;
        XR_CHECK(xr_xir_domain_new(UINT64_MAX, &domain) == XR_XIR_VALUE_OK, "source domain");
        XR_CHECK(xr_xir_string_new(domain, bytes, sizeof(bytes) - 1, &value) == XR_XIR_VALUE_OK,
            "independent canonical String");
        XrtString *string = (XrtString *)(intptr_t)value.payload;
        XrtModuleRef *initializer = NULL;
        XR_CHECK(xrt_module_begin(o.execution->state, 0) == 0 &&
            xrt_module_construct(o.execution->state, 0, &string, &initializer) == 0 &&
            xrt_module_publish(initializer) == 0 && xrt_module_ready(o.execution->state, 0) == 0,
            "publish the module's real String owner before guest entry");
        xr_xir_value_drop(&value);
        xr_xir_domain_drop(domain);
        /* A one-byte domain cannot allocate the real guest ownership table.
         * State and its String remain independently owned outside that budget. */
        if (limited) o.strings.byte_limit = 1;
        XrtExecutionStatus status = xrt_run_embedded_owned(locations, &o.failure,
            observe_frame_root, &o, &o.strings);
        if (limited) {
            XR_CHECK(status == XRT_EXECUTION_FAILURE && o.failure.kind == XRT_FAILURE_VALUE &&
                o.failure.detail.value.status == XR_XIR_VALUE_LIMIT &&
                o.failure.detail.value.source_id == 2 && !o.before_drain,
                "the fused READ retain reports its own actual allocation failure site");
            XR_CHECK(o.execution->stack.slots[2].state_ref == &o.execution->stack.module_accesses[1],
                "the adjacent REFERENCE still produced its observable capability");
        } else {
            XR_CHECK(status == XRT_EXECUTION_COMPLETED && o.status == XR_RBC_OK && o.before_drain,
                "the same fused String READ succeeds with ordinary resources");
            xrt_string_owned_drop(&o.result.string);
        }
        XR_CHECK(xrt_value_root_empty(&o.strings), "the active root drained on either exit");
        const void *payload = NULL;
        XR_CHECK(xrt_module_borrow(initializer, &payload) == 0, "the module owner survives guest failure");
        memcpy(&string, payload, sizeof(string));
        size_t length = 0;
        const char *actual = xrt_string_bytes(string, &length);
        XR_CHECK(length == sizeof(bytes) - 1 && !memcmp(actual, bytes, length), "state bytes remain intact");
        xr_rbc_execution_free(&o.execution);
    }
    xrt_locations_release(locations);
    xrt_module_layout_drop(&storage);
    for (size_t i = 0; i < 4; ++i)
        XR_CHECK(!blocks[i].pointer && !blocks[i].size, "all moving buffers reclaimed after fused retain");
}

#endif // REGISTER_STATE_FUSION_CASES_H
