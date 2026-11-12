/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_array_signed_rc_cases.h - Actual root consumers keep local object counts
 */
#ifndef NATIVE_ARRAY_SIGNED_RC_CASES_H
#define NATIVE_ARRAY_SIGNED_RC_CASES_H
typedef struct NativeSignedRc {
    XrtString *escaped[4];
    bool completed;
} NativeSignedRc;
static void native_signed_rc_root(void *opaque) {
    NativeSignedRc *probe = opaque;
    for (unsigned family = 0; family < 4; ++family) {
        XrtArray *array = display_array_new(family, false);
        CHECK(xir_object_references((const XirObject *)array) == 1);
        XrtArray *copy = xrt_array_retain(array, 1);
        CHECK(copy == array && xir_object_references((const XirObject *)array) == 2);
        xrt_array_drop(&copy);
        CHECK(xir_object_references((const XirObject *)array) == 1);
        XrtString *text = array_display_functions[family](array, 1);
        CHECK(xir_object_references(&text->object) == 1);
        probe->escaped[family] = xrt_string_root_take(text);
        CHECK(xir_object_references(&probe->escaped[family]->object) == 1);
        xrt_array_drop(&array);
    }
    probe->completed = true;
}
static void native_signed_rc_cases(XrtLocationOwner *locations) {
    NativeSignedRc probe = {0}; XrtExecutionFailure failure = {0};
    CHECK(xrt_run_embedded(locations, &failure, native_signed_rc_root, &probe) == XRT_EXECUTION_COMPLETED);
    CHECK(probe.completed);
    for (unsigned family = 0; family < 4; ++family) {
        XrtString *text = probe.escaped[family];
        string_equals(text, array_display_expected[family], strlen(array_display_expected[family]));
        CHECK(xir_object_references(&text->object) == 1);
        XrXirValue value = {XR_XIR_STRING, 0, (int64_t)(intptr_t)text};
        OK(xr_xir_value_share(&value));
        CHECK(xir_object_references(&text->object) == -1);
        xr_xir_string_release(text);
    }
    puts("Native String and four Array families: positive local ownership, escaped domain pins, explicit shared publication and retirement");
}
#endif // NATIVE_ARRAY_SIGNED_RC_CASES_H
