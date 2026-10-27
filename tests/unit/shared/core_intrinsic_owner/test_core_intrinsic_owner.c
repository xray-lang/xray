/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_core_intrinsic_owner.c - Exact registry names, lengths and borrowed descriptor boundaries
 */
#include "base/xchecks.h"
#include "shared/xr_core_intrinsic.h"

#include <stdio.h>
#include <string.h>
#if defined(XR_OS_WINDOWS)
#include <windows.h>
#endif

#define CHECK(condition) XR_CHECK(condition, #condition)

static void check_case(const char *name, size_t length, XrCoreBuiltinId expected) {
    CHECK(xr_core_intrinsic_by_source_name(name, length) == xr_core_intrinsic_by_id(expected));
}

static void bounded_name_cases(void) {
    check_case("assert", 6, XR_CORE_BUILTIN_ASSERT);
    check_case("assertEqual", 11, XR_CORE_BUILTIN_ASSERT_EQUAL);
    check_case("assertThrows", 12, XR_CORE_BUILTIN_ASSERT_THROWS);
    check_case("assertPanics", 12, XR_CORE_BUILTIN_ASSERT_PANICS);
    check_case("print", 5, XR_CORE_BUILTIN_PRINT);
    check_case("len", 3, XR_CORE_BUILTIN_LEN);
    check_case("", 0, XR_CORE_BUILTIN_NONE);
    check_case("x", 1, XR_CORE_BUILTIN_NONE);
    check_case("asser", 5, XR_CORE_BUILTIN_NONE);
    check_case("assertP", 7, XR_CORE_BUILTIN_NONE);
    check_case("assert\0", 7, XR_CORE_BUILTIN_NONE);
    check_case("assertPanicsX", 13, XR_CORE_BUILTIN_NONE);
    char long_name[4096];
    memset(long_name, 'x', sizeof(long_name));
    memcpy(long_name, "assertPanics", 12);
    check_case(long_name, sizeof(long_name), XR_CORE_BUILTIN_NONE);
}

static void generated_registry(void) {
    char error[256];
    CHECK(xr_core_intrinsic_registry_validate(error, sizeof(error)));
    CHECK(xr_core_intrinsic_count() == XR_CORE_BUILTIN_COUNT);
    for (size_t i = 0; i < xr_core_intrinsic_count(); i++) {
        const XrCoreIntrinsicDesc *row = xr_core_intrinsic_at(i);
        CHECK(row && xr_core_intrinsic_descriptor_validate(row));
        CHECK(xr_core_intrinsic_by_source_name(row->source_name,strlen(row->source_name)) == row);
        CHECK(xr_core_intrinsic_by_id(row->id) == row);
    }
    CHECK(xr_core_intrinsic_at(xr_core_intrinsic_count()) == NULL);
    CHECK(xr_core_intrinsic_by_source_name(NULL, SIZE_MAX) == NULL);
}

static void bad_arguments(void) {
    CHECK(xr_core_intrinsic_by_source_name(NULL,0)==NULL);
    CHECK(xr_core_intrinsic_by_source_name(NULL,SIZE_MAX)==NULL);
}

#if defined(XR_OS_WINDOWS)
static void guarded_input(void) {
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    size_t page_size = system.dwPageSize;
    unsigned char *pages = VirtualAlloc(NULL, 2 * page_size, MEM_RESERVE | MEM_COMMIT,
                                        PAGE_READWRITE);
    CHECK(pages != NULL);
    DWORD old_protect;
    CHECK(VirtualProtect(pages + page_size, page_size, PAGE_NOACCESS, &old_protect));
    char *name = (char *)pages + page_size - 12;
    memcpy(name, "assertPanics", 12);
    check_case(name, 12, XR_CORE_BUILTIN_ASSERT_PANICS);
    /* No candidate is longer than twelve bytes, so a huge input length does
     * not authorize reading an input terminator or scanning the unused tail. */
    check_case(name, SIZE_MAX, XR_CORE_BUILTIN_NONE);
    const char *inaccessible = (const char *)pages + page_size;
    check_case(inaccessible, 0, XR_CORE_BUILTIN_NONE);
    CHECK(VirtualFree(pages, 0, MEM_RELEASE));
}
#endif

int main(void) {
    generated_registry();
    bounded_name_cases();
    bad_arguments();
#if defined(XR_OS_WINDOWS)
    guarded_input();
#endif
    puts("core intrinsic owner: canonical descriptors, exact names, lengths and guarded reads pass");
    return 0;
}
