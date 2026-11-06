/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_semver_compile_resources.c - Mandatory shared resources for package metadata
 */
#include "base/xmalloc.h"
#include "base/xcompile_resources.h"
#include "base/xio_policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[256];
static size_t calls, live, physical, fail_at = SIZE_MAX;
static void *counted_malloc(size_t bytes) {
    if (calls++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes); if (!memory) return NULL;
    for (size_t i = 0; i < 256; ++i) if (!allocations[i].pointer) {
        allocations[i] = (Allocation){memory, bytes}; ++live; physical += bytes; return memory;
    }
    CHECK(false); return NULL;
}
static void counted_free(void *memory) {
    if (!memory) return;
    for (size_t i = 0; i < 256; ++i) if (allocations[i].pointer == memory) {
        physical -= allocations[i].bytes; --live; allocations[i] = (Allocation){0}; xr_free(memory); return;
    }
    CHECK(false);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc counted_malloc
#define xr_free counted_free
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")

static const XrCompileResourceLimits unlimited = {XR_COMPILE_MEMORY_ORDINARY, 0, true};
static XrCompileResources *ledger(XrCompileResourceLimits limits) {
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    return resources;
}
static void release(XrCompileResources *resources) {
    xr_compile_resources_release(resources); CHECK(!live && !physical);
}
#include "module/xsemver.h"

static size_t memory_boundaries, oom_points;
static XrSemVer poison_version(void) { XrSemVer value; memset(&value, 0xA5, sizeof(value)); return value; }
static XrCompileResourceStats statistics(XrCompileResources *resources) {
    XrCompileResourceStats result; CHECK(xr_compile_resources_stats(resources, &result) == XR_COMPILE_RESOURCE_OK); return result;
}
static void parse_memory(bool suffix) {
    const char *source = suffix ? "1.2.3-a+b" : "1";
    XrCompileResources *resources = ledger(unlimited);
    XrOsIoPolicy policy = xr_compile_io_policy(resources);
    XrSemVer out = poison_version();
    CHECK(xr_semver_parse_owned(&policy, source, &out) == XR_OS_IO_OK);
    XrCompileResourceStats exact = statistics(resources);
    CHECK(exact.held_bytes == physical && exact.peak_bytes == physical);
    CHECK(out.major == 1 && out.minor == (suffix ? 2 : 0));
    xr_semver_free_owned(&out); release(resources);
    for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_FINITE, exact.peak_bytes - minus, true};
        resources = NULL; out = poison_version(); XrSemVer saved = out;
        XrCompileResourceStatus created = xr_compile_resources_new(&limits, &resources);
        if (created != XR_COMPILE_RESOURCE_OK) {
            CHECK(minus && created == XR_COMPILE_RESOURCE_MEMORY_LIMIT && !resources);
            CHECK(!memcmp(&out, &saved, sizeof(out)) && !live && !physical);
        } else {
            policy = xr_compile_io_policy(resources);
            CHECK(xr_semver_parse_owned(&policy, source, &out) == (minus ? XR_OS_IO_MEMORY_LIMIT : XR_OS_IO_OK));
            if (minus) CHECK(!memcmp(&out, &saved, sizeof(out)));
            else {
                CHECK(out.major == 1 && out.minor == (suffix ? 2 : 0));
                xr_semver_free_owned(&out);
                CHECK(xr_semver_parse_owned(&policy, source, &out) == XR_OS_IO_OK);
                xr_semver_free_owned(&out);
            }
            release(resources);
        }
        ++memory_boundaries;
    }
}
static void grammar(void) {
    static const struct { const char *text; bool valid; int major, minor, patch; const char *pre, *build; } cases[] = {
        {"0", true, 0, 0, 0, NULL, NULL}, {"v1.2", true, 1, 2, 0, NULL, NULL},
        {" V12.34.56-alpha.2+tag ", true, 12, 34, 56, "alpha.2", "tag"},
        {"2147483647.0.0", true, INT_MAX, 0, 0, NULL, NULL},
        {"01.2.3", false, 0, 0, 0, NULL, NULL}, {"1.02.3", false, 0, 0, 0, NULL, NULL},
        {"1.2.03", false, 0, 0, 0, NULL, NULL}, {"2147483648.1.1", false, 0, 0, 0, NULL, NULL},
        {"x", false, 0, 0, 0, NULL, NULL}, {"1.2.3?", false, 0, 0, 0, NULL, NULL},
        {"1.2.3-", true, 1, 2, 3, NULL, NULL}, {"1ignored", true, 1, 0, 0, NULL, NULL}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        XrCompileResources *resources = ledger(unlimited); XrOsIoPolicy policy = xr_compile_io_policy(resources);
        XrSemVer version = poison_version(), saved = version; bool valid = !cases[i].valid;
        CHECK(xr_semver_is_valid_owned(&policy, cases[i].text, &valid) == XR_OS_IO_OK && valid == cases[i].valid);
        CHECK(xr_semver_parse_owned(&policy, cases[i].text, &version) == (valid ? XR_OS_IO_OK : XR_OS_IO_BAD_ARGUMENT));
        if (valid) {
            CHECK(version.major == cases[i].major && version.minor == cases[i].minor && version.patch == cases[i].patch);
            CHECK(cases[i].pre ? version.prerelease && !strcmp(cases[i].pre, version.prerelease) : !version.prerelease);
            CHECK(cases[i].build ? version.build && !strcmp(cases[i].build, version.build) : !version.build);
            xr_semver_free_owned(&version);
        } else CHECK(!memcmp(&version, &saved, sizeof(saved)));
        release(resources);
    }
}
static void numeric_prerelease(void) {
    static const struct { const char *a, *b; int comparison; } cases[] = {
        {"1.2.3-2147483648", "1.2.3-2147483647", 1},
        {"1.2.3-99999999999999999999999999999999", "1.2.3-100000000000000000000000000000000", -1},
        {"1.2.3-000000000000000000000000000000001", "1.2.3-1", 0},
        {"1.2.3-100000000000000000000000000000001", "1.2.3-100000000000000000000000000000002", -1},
        {"1.2.3-alpha.1", "1.2.3-alpha.2", -1}, {"1.2.3-alpha", "1.2.3", -1}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        XrCompileResources *resources = ledger(unlimited); XrOsIoPolicy policy = xr_compile_io_policy(resources);
        XrSemVer a = {0}, b = {0}; int compared = 99;
        CHECK(xr_semver_parse_owned(&policy, cases[i].a, &a) == XR_OS_IO_OK);
        CHECK(xr_semver_parse_owned(&policy, cases[i].b, &b) == XR_OS_IO_OK);
        CHECK(xr_semver_compare_owned(&policy, &a, &b, &compared) == XR_OS_IO_OK && compared == cases[i].comparison);
        xr_semver_free_owned(&a); xr_semver_free_owned(&b); release(resources);
    }
}
static void constraints(void) {
    static const struct { const char *constraint, *version; bool matches; } cases[] = {
        {"^1.2.3", "1.9.0", true}, {"^1.2.3", "2.0.0", false}, {"^0.2.3", "0.2.9", true},
        {"^0.2.3", "0.3.0", false}, {"^0.0.3", "0.0.4", false}, {"~1.2.3", "1.2.8", true},
        {"~1.2.3", "1.3.0", false}, {">=1.2.3", "1.2.3", true}, {"<=1.2.3", "1.2.4", false},
        {"*", "0.0.1-alpha", true}, {"=1.2.3", "1.2.3+metadata", true}
    };
    XrCompileResources *resources = ledger(unlimited); XrOsIoPolicy policy = xr_compile_io_policy(resources);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        XrSemVer version = {0}; XrVersionConstraint constraint = {0}; bool matches = !cases[i].matches;
        CHECK(xr_semver_parse_owned(&policy, cases[i].version, &version) == XR_OS_IO_OK);
        CHECK(xr_constraint_parse_owned(&policy, cases[i].constraint, &constraint) == XR_OS_IO_OK);
        CHECK(xr_constraint_matches_owned(&policy, &version, &constraint, &matches) == XR_OS_IO_OK && matches == cases[i].matches);
        xr_semver_free_owned(&version); xr_constraint_free_owned(&constraint);
    }
    XrSemVer versions[3] = {{0}}; XrVersionConstraint constraint = {0}; int index = 99;
    for (int i = 0; i < 3; ++i) { char text[] = "1.2.3"; text[4] = (char)('3' + i); CHECK(xr_semver_parse_owned(&policy, text, &versions[i]) == XR_OS_IO_OK); }
    CHECK(xr_constraint_parse_owned(&policy, "~1.2.3", &constraint) == XR_OS_IO_OK);
    CHECK(xr_semver_select_best_owned(&policy, versions, 3, &constraint, &index) == XR_OS_IO_OK && index == 2);
    for (int i = 0; i < 3; ++i) xr_semver_free_owned(&versions[i]); xr_constraint_free_owned(&constraint); release(resources);
}
static XrOsIoStatus operation(unsigned op, const XrOsIoPolicy *policy) {
    if (op == 0) {
        XrSemVer output = poison_version(), saved = output;
        XrOsIoStatus status = xr_semver_parse_owned(policy, "1.2.3-alpha.1+long-build", &output);
        if (status == XR_OS_IO_OK) xr_semver_free_owned(&output); else CHECK(!memcmp(&output, &saved, sizeof(output)));
        return status;
    }
    if (op == 1) {
        bool output = false; XrOsIoStatus status = xr_semver_is_valid_owned(policy, "1.2.3-alpha.1+long-build", &output);
        CHECK(output == (status == XR_OS_IO_OK)); return status;
    }
    if (op == 2) {
        XrVersionConstraint output, saved; memset(&output, 0xA5, sizeof(output)); saved = output;
        XrOsIoStatus status = xr_constraint_parse_owned(policy, "^1.2.3-alpha+build", &output);
        if (status == XR_OS_IO_OK) xr_constraint_free_owned(&output); else CHECK(!memcmp(&output, &saved, sizeof(output)));
        return status;
    }
    XrSemVer a = {0}, b = {0}; a.major = b.major = 1; a.prerelease = "999999999999999999999999"; b.prerelease = "1000000000000000000000000";
    if (op == 3) {
        int output = 99; XrOsIoStatus status = xr_semver_compare_owned(policy, &a, &b, &output);
        CHECK(output == (status == XR_OS_IO_OK ? -1 : 99)); return status;
    }
    char buffer[128], saved[128]; memset(buffer, 0xA5, sizeof(buffer)); memcpy(saved, buffer, sizeof(buffer)); size_t written = 999;
    XrOsIoStatus status = xr_semver_to_string_owned(policy, &a, buffer, sizeof(buffer), &written);
    if (status == XR_OS_IO_OK) CHECK(!strcmp(buffer, "1.0.0-999999999999999999999999") && written == 30);
    else CHECK(!memcmp(buffer, saved, sizeof(buffer)) && written == 999);
    return status;
}
static void faults(void) {
    XrCompileResources *rejected = NULL;
    fail_at = calls;
    CHECK(xr_compile_resources_new(&unlimited, &rejected) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !rejected);
    fail_at = SIZE_MAX; CHECK(!live && !physical); ++oom_points;
    for (unsigned op = 0; op < 5; ++op) {
        XrCompileResources *resources = ledger(unlimited);
        XrCompileResourceStats baseline = statistics(resources);
        XrOsIoPolicy policy = xr_compile_io_policy(resources); size_t start = calls;
        CHECK(operation(op, &policy) == XR_OS_IO_OK);
        size_t points = calls - start;
        XrCompileResourceStats exact = statistics(resources);
        CHECK(exact.held_bytes == baseline.held_bytes && exact.held_bytes == physical);
        release(resources);
        for (size_t i = 0; i < points; ++i) {
            resources = ledger(unlimited); policy = xr_compile_io_policy(resources); fail_at = calls + i;
            CHECK(operation(op, &policy) == XR_OS_IO_OUT_OF_MEMORY);
            fail_at = SIZE_MAX; release(resources); ++oom_points;
        }
        for (unsigned minus = 0; minus < 2; ++minus) {
            XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_FINITE, exact.peak_bytes - minus, true};
            resources = NULL;
            XrCompileResourceStatus created = xr_compile_resources_new(&limits, &resources);
            if (created != XR_COMPILE_RESOURCE_OK) {
                CHECK(minus && created == XR_COMPILE_RESOURCE_MEMORY_LIMIT && !resources && !live && !physical);
            } else {
                policy = xr_compile_io_policy(resources);
                CHECK(operation(op, &policy) == (minus ? XR_OS_IO_MEMORY_LIMIT : XR_OS_IO_OK));
                release(resources);
            }
            ++memory_boundaries;
        }
    }
}
static void lifetime(void) {
    XrCompileResources *resources = ledger(unlimited); XrOsIoPolicy policy = xr_compile_io_policy(resources); XrSemVer version = {0};
    CHECK(xr_semver_parse_owned(&policy, "1.2.3-alpha+build", &version) == XR_OS_IO_OK); xr_compile_resources_release(resources);
    CHECK(live == 3 && !strcmp(version.prerelease, "alpha") && !strcmp(version.build, "build")); xr_semver_free_owned(&version); CHECK(!live && !physical);
}
int main(void) {
    parse_memory(false); parse_memory(true); grammar(); numeric_prerelease(); constraints(); faults(); lifetime();
    printf("semver resources passed: %zu held memory boundaries, %zu malloc OOM points; physical zero\n", memory_boundaries, oom_points); return 0;
}
