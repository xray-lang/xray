/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_string_place.c - Owned String mutation and finite admission
 *
 * KEY CONCEPT:
 *   Unique growth, COW and receiving-domain transfer preserve the root on failure.
 */
#define XR_COMPILE_DOMAIN_ENTRY original_compile_domain_main
#include "test_xir_compile_domain.c"
#undef XR_COMPILE_DOMAIN_ENTRY
#include "xir/xxir_cell_owner_internal.h"

typedef struct StringPlaceFixture {
    XrXirDomain *producer, *receiver;
    XrXirValue root, suffix, alias;
    XrXirValueAdmission admission;
    XrXirValuePlace place;
} StringPlaceFixture;
static void string_place_expect(const XrXirValue *value, const char *expected, size_t length) {
    const char *bytes = NULL; size_t count = 0;
    CHECK(xr_xir_string_view(value, &bytes, &count) && count == length && !memcmp(bytes, expected, count));
}
static void string_place_open(StringPlaceFixture *f, unsigned mode, XrCompileResources *resources) {
    memset(f, 0, sizeof(*f));
    CHECK((resources ? xr_xir_compile_domain_new(resources, 65536, &f->producer) :
        xr_xir_domain_new(65536, &f->producer)) == XR_XIR_VALUE_OK);
    f->receiver = f->producer;
    if (mode == 2) f->receiver = NULL;
    if (mode == 2) CHECK((resources ? xr_xir_compile_domain_new(resources, 65536, &f->receiver) :
        xr_xir_domain_new(65536, &f->receiver)) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(f->producer, "a", 1, &f->root) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(f->receiver, "bcdefghijklmnopqrstuvwxyz", 25, &f->suffix) == XR_XIR_VALUE_OK);
    if (mode == 1) CHECK(xr_xir_value_copy(&f->root, &f->alias) == XR_XIR_VALUE_OK);
    f->admission = (XrXirValueAdmission){NULL, f->receiver, NULL, NULL, 100000, 65536};
    f->place = (XrXirValuePlace){XR_XIR_STRING, &f->root.payload};
}
static void string_place_close(StringPlaceFixture *f) {
    xr_xir_value_drop(&f->alias); xr_xir_value_drop(&f->suffix); xr_xir_value_drop(&f->root);
    if (f->receiver != f->producer) xr_xir_domain_drop(f->receiver);
    xr_xir_domain_drop(f->producer);
}
static XrXirValueStatus string_place_apply(StringPlaceFixture *f, bool utf8) {
    return utf8 ? xr_xir_string_place_append_utf8(&f->place, "bcdefghijklmnopqrstuvwxyz", 25, &f->admission) :
        xr_xir_string_place_append(&f->place, &f->suffix, &f->admission);
}
static void string_place_missing_admission(bool utf8) {
    for (unsigned mode = 0; mode < 3; ++mode) {
        StringPlaceFixture f; string_place_open(&f, mode, NULL);
        size_t before = calls; int64_t root = f.root.payload;
        uint64_t work = f.admission.work;
        XrXirDomain retired = {0};
        for (unsigned invalid = 0; invalid < 3; ++invalid) {
            XrXirValueAdmission *admission = invalid ? &f.admission : NULL;
            f.admission.domain = invalid == 1 ? NULL : invalid == 2 ? &retired : f.receiver;
            XrXirValueStatus status = utf8 ?
                xr_xir_string_place_append_utf8(&f.place, "b", 1, admission) :
                xr_xir_string_place_append(&f.place, &f.suffix, admission);
            CHECK(status == XR_XIR_VALUE_BAD_ARGUMENT);
            CHECK(f.root.payload == root && calls == before && f.admission.work == work);
            string_place_expect(&f.root, "a", 1);
            if (mode == 1) string_place_expect(&f.alias, "a", 1);
        }
        f.admission.domain = f.receiver;
        string_place_close(&f); CHECK(!live);
    }
    printf("String place utf8=%u missing admission/Domain and retired identity preserve roots/work with no allocation PASS\n", utf8);
}
static void string_place_utf8_normal(void) {
    for (unsigned mode = 0; mode < 3; ++mode) {
        StringPlaceFixture f; string_place_open(&f, mode, NULL);
        size_t before = calls;
        CHECK(xr_xir_string_place_append_utf8(&f.place, "\xff", 1, &f.admission) == XR_XIR_VALUE_BAD_UTF8);
        CHECK(calls == before); string_place_expect(&f.root, "a", 1);
        CHECK(xr_xir_string_place_append_utf8(&f.place, "\0\xce\xa9", 3, &f.admission) == XR_XIR_VALUE_OK);
        string_place_expect(&f.root, "a\0\xce\xa9", 4);
        if (mode == 1) string_place_expect(&f.alias, "a", 1);
        const char *bytes = NULL; size_t length = 0;
        CHECK(xr_xir_string_view(&f.root, &bytes, &length) && length == 4);
        CHECK(xr_xir_string_place_append_utf8(&f.place, bytes, length, &f.admission) == XR_XIR_VALUE_OK);
        string_place_expect(&f.root, "a\0\xce\xa9" "a\0\xce\xa9", 8);
        size_t runes = 0; CHECK(xr_xir_string_runes(&f.root, &runes) && runes == 6);
        string_place_close(&f); CHECK(!live);
    }
    puts("String UTF-8 span no temporary suffix, invalid bytes, NUL/Rune/self/COW/receiving Domain PASS");
}
static void string_place_normal(void) {
    for (unsigned mode = 0; mode < 3; ++mode) {
        StringPlaceFixture f; string_place_open(&f, mode, NULL);
        CHECK(xr_xir_string_place_append(&f.place, &f.suffix, &f.admission) == XR_XIR_VALUE_OK);
        string_place_expect(&f.root, "abcdefghijklmnopqrstuvwxyz", 26);
        if (mode == 1) string_place_expect(&f.alias, "a", 1);
        CHECK(object_pointer(&f.root)->domain == f.receiver);
        XrXirValue empty = {0}; CHECK(xr_xir_string_new(f.receiver, "", 0, &empty) == XR_XIR_VALUE_OK);
        int64_t payload = f.root.payload; size_t before = calls;
        CHECK(xr_xir_string_place_append(&f.place, &empty, &f.admission) == XR_XIR_VALUE_OK);
        CHECK(f.root.payload == payload && calls == before); xr_xir_value_drop(&empty);
        string_place_close(&f); CHECK(!live);
    }
    StringPlaceFixture f; string_place_open(&f, 0, NULL);
    xr_xir_value_drop(&f.suffix);
    CHECK(xr_xir_string_new(f.receiver, "\0\xce\xa9", 3, &f.suffix) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_place_append(&f.place, &f.suffix, &f.admission) == XR_XIR_VALUE_OK);
    string_place_expect(&f.root, "a\0\xce\xa9", 4);
    size_t runes = 0; CHECK(xr_xir_string_runes(&f.root, &runes) && runes == 3);
    CHECK(xr_xir_string_place_append(&f.place, &f.root, &f.admission) == XR_XIR_VALUE_OK);
    string_place_expect(&f.root, "a\0\xce\xa9" "a\0\xce\xa9", 8);
    CHECK(xr_xir_string_runes(&f.root, &runes) && runes == 6);
    string_place_close(&f); CHECK(!live);
    puts("String place unique/shared/foreign/empty/self/Unicode/NUL normal physical=0");
}
static void string_place_cell_authority(void) {
    XrCompileResources *resources = NULL; XrCompileResourceLimits limits = compile_domain_limits();
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrXirTypeNode node = {.kind = XR_XIR_TYPE_CELL, .element = XR_XIR_STRING};
    XrXirTypes types = {&node, 1, NULL, NULL};
    XrXirCompileContext context = {.resources = resources, .limits = xr_xir_compile_default_limits()};
    XrXirTypeArena *arena = NULL; XrXirDomain *domain = NULL;
    CHECK(xr_xir_compile_type_arena_new(&context, &types, &arena) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 100000, 65536};
    XrXirValue text = {0}, suffix = {0}, cell = {0}, read = {0};
    CHECK(xr_xir_string_new(domain, "a", 1, &text) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "b", 1, &suffix) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain, arena, (XrXirType)256, &text, &admission, &cell) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&text);
    XrXirValuePlace place = {0};
    CHECK(xr_xir_cell_value_place(&cell, &admission, &place) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_place_append(&place, &suffix, &admission) == XR_XIR_VALUE_OK);
    XrXirCellLoan loan = {0};
    CHECK(xr_xir_cell_loan_prepare(&cell, NULL, &loan) == XR_XIR_VALUE_OK);
    place = (XrXirValuePlace){0};
    CHECK(xr_xir_cell_value_place(&cell, &admission, &place) == XR_XIR_VALUE_BAD_ARGUMENT && !place.payload);
    xr_xir_cell_loan_abort(&loan);
    unsigned activation = 0, frame = 0;
    XrXirCellAuthority authority = {&activation, &frame, 1}, wrong = authority; wrong.epoch = 2;
    CHECK(xr_xir_cell_loan_prepare(&cell, NULL, &loan) == XR_XIR_VALUE_OK);
    xr_xir_cell_loan_commit(&cell, &loan, &authority);
    CHECK(xr_xir_cell_value_place(&cell, &admission, &place) == XR_XIR_VALUE_BAD_ARGUMENT && !place.payload);
    CHECK(xr_xir_cell_authorized_place(&cell, &wrong, &admission, &place) == XR_XIR_VALUE_BAD_ARGUMENT && !place.payload);
    CHECK(xr_xir_cell_authorized_place(&cell, &authority, &admission, &place) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_place_append(&place, &suffix, &admission) == XR_XIR_VALUE_OK);
    xr_xir_cell_loan_release(&loan);
    CHECK(xr_xir_cell_read(&cell, &read) == XR_XIR_VALUE_OK); string_place_expect(&read, "abb", 3);
    xr_xir_value_drop(&read); xr_xir_value_drop(&cell); xr_xir_value_drop(&suffix);
    xr_xir_domain_drop(domain); xr_xir_compile_type_arena_drop(arena); xr_compile_resources_release(resources);
    CHECK(!live && !value_compile_live && !value_compile_bytes);
    puts("String Cell prepared/active loan and wrong epoch denial; authenticated primitive PASS");
}
static void string_place_runtime_faults(bool utf8) {
    for (unsigned mode = 0; mode < 3; ++mode) {
        StringPlaceFixture f; string_place_open(&f, mode, NULL);
        calls = 0;
        CHECK(string_place_apply(&f, utf8) == XR_XIR_VALUE_OK);
        const size_t sites = calls; CHECK(sites > 0); string_place_close(&f); CHECK(!live);
        for (size_t failure = 0; failure < sites; ++failure) {
            string_place_open(&f, mode, NULL);
            int64_t payload = f.root.payload;
            uint64_t bytes = xr_xir_domain_stats(f.receiver).live_bytes;
            calls = 0; fail_at = failure;
            CHECK(string_place_apply(&f, utf8) == XR_XIR_VALUE_OOM);
            fail_at = SIZE_MAX;
            CHECK(calls > failure && f.root.payload == payload && xr_xir_domain_stats(f.receiver).live_bytes == bytes);
            string_place_expect(&f.root, "a", 1);
            if (mode == 1) string_place_expect(&f.alias, "a", 1);
            CHECK(string_place_apply(&f, utf8) == XR_XIR_VALUE_OK);
            string_place_expect(&f.root, "abcdefghijklmnopqrstuvwxyz", 26);
            string_place_close(&f); CHECK(!live);
        }
        printf("String place utf8=%u mode=%u actual runtime OOM sites=%zu, preserved root and same-owner retry physical=0\n", utf8, mode, sites);
    }
}
static void string_place_local_work(bool utf8) {
    for (unsigned mode = 0; mode < 3; ++mode) for (unsigned shortfall = 0; shortfall < 2; ++shortfall) {
        StringPlaceFixture f; string_place_open(&f, mode, NULL);
        uint64_t exact = 26 + (utf8 ? 26 : 0) + (mode ? 1 : string_pointer(&f.root)->capacity);
        f.admission.work = exact - shortfall; size_t before = calls;
        CHECK(string_place_apply(&f, utf8) ==
            (shortfall ? XR_XIR_VALUE_LIMIT : XR_XIR_VALUE_OK));
        if (shortfall) { CHECK(calls == before); string_place_expect(&f.root, "a", 1); }
        else { CHECK(!f.admission.work); string_place_expect(&f.root, "abcdefghijklmnopqrstuvwxyz", 26); }
        string_place_close(&f); CHECK(!live);
    }
    puts("String place local work exact/minus1 unique/shared/foreign PASS");
}
static XrCompileResourceStats string_place_compile_sequence(XrCompileResources *resources, bool utf8) {
    StringPlaceFixture f; string_place_open(&f, 1, resources);
    CHECK(string_place_apply(&f, utf8) == XR_XIR_VALUE_OK);
    string_place_expect(&f.root, "abcdefghijklmnopqrstuvwxyz", 26); string_place_expect(&f.alias, "a", 1);
    XrCompileResourceStats stats = compile_domain_stats(resources);
    string_place_close(&f); return stats;
}
static void string_place_compile_faults(bool utf8) {
    for (unsigned mode = 0; mode < 3; ++mode) {
        XrCompileResources *resources = NULL; XrCompileResourceLimits limits = compile_domain_limits();
        CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
        StringPlaceFixture f; string_place_open(&f, mode, resources);
        size_t begin = value_compile_calls;
        CHECK(string_place_apply(&f, utf8) == XR_XIR_VALUE_OK);
        size_t sites = value_compile_calls - begin; CHECK(sites > 0);
        string_place_close(&f); xr_compile_resources_release(resources);
        for (size_t failure = 0; failure < sites; ++failure) {
            resources = NULL; CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
            string_place_open(&f, mode, resources); int64_t payload = f.root.payload;
            XrCompileResourceStats before = compile_domain_stats(resources);
            value_compile_injected = false; value_compile_fail_at = value_compile_calls + failure;
            CHECK(string_place_apply(&f, utf8) == XR_XIR_VALUE_OOM);
            value_compile_fail_at = SIZE_MAX; CHECK(value_compile_injected && f.root.payload == payload);
            XrCompileResourceStats after = compile_domain_stats(resources);
            CHECK(after.live_bytes == before.live_bytes && after.allocated_bytes >= before.allocated_bytes && after.work > before.work);
            string_place_expect(&f.root, "a", 1);
            CHECK(string_place_apply(&f, utf8) == XR_XIR_VALUE_OK);
            string_place_close(&f); xr_compile_resources_release(resources);
            CHECK(!live && !value_compile_live && !value_compile_bytes);
        }
        printf("String place utf8=%u mode=%u actual compile-owner OOM sites=%zu, cumulative work and retry physical=0/0\n", utf8, mode, sites);
    }
    XrCompileResources *resources = NULL; XrCompileResourceLimits limits = compile_domain_limits();
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline = string_place_compile_sequence(resources, utf8);
    xr_compile_resources_release(resources); CHECK(!value_compile_live && !value_compile_bytes);
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned shortfall = 0; shortfall < 2; ++shortfall) {
        limits = compile_domain_limits();
        uint64_t exact = axis == 0 ? baseline.allocated_bytes : axis == 1 ? baseline.peak_bytes : baseline.work;
        if (axis == 0) limits.allocated_bytes = exact - shortfall;
        else if (axis == 1) limits.live_bytes = exact - shortfall;
        else limits.work = exact - shortfall;
        resources = NULL; CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
        StringPlaceFixture f; string_place_open(&f, 1, resources);
        CHECK(string_place_apply(&f, utf8) ==
            (shortfall ? XR_XIR_VALUE_LIMIT : XR_XIR_VALUE_OK));
        if (shortfall) string_place_expect(&f.root, "a", 1);
        string_place_close(&f); xr_compile_resources_release(resources); CHECK(!value_compile_live && !value_compile_bytes);
    }
    puts("String place same compile ledger allocated/live/work exact/minus1 PASS");
}
int main(int argc, char **argv) {
    if (argc == 2 && (!strcmp(argv[1], "--invalid-admission-string") ||
        !strcmp(argv[1], "--invalid-admission-utf8"))) {
        string_place_missing_admission(!strcmp(argv[1], "--invalid-admission-utf8"));
        CHECK(!live && !value_compile_live && !value_compile_bytes); return 0;
    }
    string_place_missing_admission(false); string_place_missing_admission(true);
    string_place_normal(); string_place_utf8_normal();
    string_place_local_work(false); string_place_local_work(true); string_place_cell_authority();
    if (argc != 2 || strcmp(argv[1], "--normal")) {
        string_place_runtime_faults(false); string_place_compile_faults(false);
        string_place_runtime_faults(true); string_place_compile_faults(true);
    }
    CHECK(!live && !value_compile_live && !value_compile_bytes); return 0;
}
