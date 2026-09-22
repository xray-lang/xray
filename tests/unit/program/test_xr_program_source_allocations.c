/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xr_program_source_allocations.c - Source product allocation failure ownership
 */

/* Reuse the existing source fixture without replacing its normal allocator. */
#define xr_program_source_build_default_budget xr_source_probe_default_budget
#define xr_program_source_build xr_source_probe_build
#define xr_program_source_product_free xr_source_probe_product_free
#define xr_program_source_build_status_name xr_source_probe_status_name
#define main xr_source_fixture_test_main
#include "test_xr_program_source_build.c"
#undef main
#include "xr_program_source_allocation_probe.h"

#define REQUIRE(condition)                                                                    \
    do {                                                                                      \
        if (!(condition)) {                                                                   \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                          \
        }                                                                                     \
    } while (0)

static size_t attempts, fail_at, live_count;
static void *live[4096];

static void *track(void *pointer) {
    REQUIRE(pointer != NULL);
    for (size_t index = 0u; index < XR_COUNTOF(live); ++index) {
        if (live[index])
            continue;
        live[index] = pointer;
        live_count++;
        return pointer;
    }
    abort();
}

void *xr_program_test_malloc(size_t size) {
    return ++attempts == fail_at ? NULL : track(xr_program_test_system_malloc(size));
}

void *xr_program_test_calloc(size_t count, size_t size) {
    return ++attempts == fail_at ? NULL : track(xr_program_test_system_calloc(count, size));
}

void *xr_program_test_realloc(void *pointer, size_t size) {
    if (++attempts == fail_at)
        return NULL;
    if (!pointer)
        return track(xr_program_test_system_realloc(NULL, size));
    for (size_t index = 0u; index < XR_COUNTOF(live); ++index) {
        if (live[index] != pointer)
            continue;
        void *grown = xr_program_test_system_realloc(pointer, size);
        REQUIRE(grown != NULL);
        live[index] = grown;
        return grown;
    }
    abort();
}

void xr_program_test_free(void *pointer) {
    if (!pointer)
        return;
    for (size_t index = 0u; index < XR_COUNTOF(live); ++index) {
        if (live[index] != pointer)
            continue;
        live[index] = NULL;
        live_count--;
        break;
    }
    /* Graph diagnostics are transferred from an uninstrumented dependency. */
    xr_program_test_system_free(pointer);
}

char *xr_program_test_strdup(const char *source) {
    size_t size = strlen(source) + 1u;
    char *copy = xr_program_test_malloc(size);
    if (copy)
        memcpy(copy, source, size);
    return copy;
}

static size_t run_allocation_case(unsigned scenario, size_t failure) {
    static const char *const sources[] = {
        "fn answer() -> i64 { return 0 }\n"
        "fn exported() -> i64 { return 42 }\n"
        "@test\nfn checkAnswer() { assert(exported() == 42) }\n",
        "fn answer() -> i64 {\n"
        " const counter = Atomic(40)\n const alias = counter\n"
        " var (old, matched) = alias.compareExchange(40, 42)\n"
        " counter.store(42, Ordering.Release)\n counter.add(1)\n counter.sub(1)\n return counter.load()\n}\n"
        "fn exported() -> i64 { return 42 }\n"
        "@test\nfn checkAnswer() { assert(exported() == 42) }\n",
        "fn answer() -> i64 {\n"
        " const flag = Atomic(false)\n const alias = flag\n"
        " var (old, matched) = alias.compareExchange(false, true)\n"
        " flag.store(true, Ordering.AcquireRelease)\n const before = flag.toggle(Ordering.Relaxed)\n if (matched && before) { return 42 }\n return 0\n}\n"
        "fn exported() -> i64 { return 42 }\n"
        "@test\nfn checkAnswer() { assert(exported() == 42) }\n",
        "fn identity(value: f64) -> f64 { return value }\n"
        "fn answer() -> i64 {\n"
        " const value = identity(1.5)\n const other = identity(2.25)\n"
        " const counter = Atomic(value)\n const old = counter.fetchAdd(other)\n"
        " var (before, matched) = counter.compareExchange(3.75, 1.5)\n"
        " counter.store(1.5, Ordering.Release)\n"
        " const text = counter.load().toString()\n"
        " if (old < other && len(text) == 3) { return 42 }\n return 0\n}\n"
        "fn exported() -> i64 { return 42 }\n"
        "@test\nfn checkAnswer() { assert(exported() == 42) }\n",
        "type State = { token:i64, enabled:bool, nested:{ text:string }? }\n"
        "var state:State?=null\n"
        "fn answer() -> i64 { return 42 }\n"
        "fn exported() -> i64 { return 42 }\n"
        "@test\nfn checkAnswer() { assert(exported() == 42) }\n",
        "type Child = { text:string }\n"
        "type State = { token:i64, child:Child }\n"
        "fn answer() -> i64 {\n"
        " var child:Child={text:\"before\"}\n var state:State={child:child,token:7}\n"
        " var alias=state\n alias.token=42\n alias.child.text=\"after\"\n"
        " if (state.child.text == \"after\") { return state.token }\n return 0\n}\n"
        "fn exported() -> i64 { return 42 }\n"
        "@test\nfn checkAnswer() { assert(exported() == 42) }\n",
        "import { NetConn } from net\n"
        "var connection:NetConn?=null\n"
        "fn answer() -> i64 { return 42 }\n"
        "fn exported() -> i64 { return 42 }\n"
        "@test\nfn checkAnswer() { assert(exported() == 42) }\n",
    };
    REQUIRE(scenario < XR_COUNTOF(sources));
    const char *source = sources[scenario];
    static const char manifest[] =
        "[[export.c]]\nxray = \"exported\"\nsymbol = \"external_answer\"\n"
        "abi = \"native\"\nvisibility = \"hidden\"\nheader = true\n"
        "[[export.c]]\nxray = \"answer\"\nsymbol = \"external_entry\"\n";
    SourceBuildFixture fixture;
    REQUIRE(source_build_fixture_init(&fixture, source, NULL));
    XrTomlValue *toml = xtoml_parse(manifest, sizeof(manifest) - 1u);
    REQUIRE(toml != NULL);
    XrNativePackagePlan *plan = xr_native_package_plan_parse(toml, fixture.directory);
    xtoml_free(toml);
    REQUIRE(plan && plan->valid);
    xr_compiler_session_set_native_package_plan(fixture.session, plan);
    fixture.input.discover_tests = 1u;
    fixture.input.discover_exports = 1u;
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    REQUIRE(live_count == 0u);
    attempts = 0u;
    fail_at = failure;
    XrProgramSourceBuildStatus status =
        xr_program_source_build(&fixture.input, &product, &diagnostic);
    size_t count = attempts;
    fail_at = 0u;
    if (failure) {
        if (status != XR_PROGRAM_SOURCE_BUILD_OUT_OF_MEMORY)
            fprintf(stderr, "scenario %u failure %zu status %u: %s\n", scenario, failure,
                    (unsigned) status, diagnostic.message);
        REQUIRE(status == XR_PROGRAM_SOURCE_BUILD_OUT_OF_MEMORY);
        REQUIRE(diagnostic.message[0] != '\0');
        REQUIRE(product.program == NULL && product.artifact.bytes == NULL);
        REQUIRE(product.exports == NULL && product.export_function_ids == NULL);
        REQUIRE(product.tests == NULL && product.test_entry_count == 0u);
        REQUIRE(product.export_count == 0u && product.retained_function_count == 0u);
        REQUIRE(live_count == 0u);
    } else {
        if (status != XR_PROGRAM_SOURCE_BUILD_OK)
            fprintf(stderr, "success probe scenario %u status %u: %s\n", scenario,
                    (unsigned) status, diagnostic.message);
        REQUIRE(status == XR_PROGRAM_SOURCE_BUILD_OK);
        REQUIRE(product.export_count == 2u && product.test_entry_count == 1u);
        REQUIRE(strcmp(product.exports[0].symbol, "external_answer") == 0);
        REQUIRE(product.export_function_ids[0] < xr_validated_program_function_count(product.program));
    }
    source_build_fixture_free(&fixture);
    xr_native_package_plan_free(plan);
    xr_program_source_product_free(&product);
    REQUIRE(live_count == 0u);
    return count;
}

int main(void) {
    for (unsigned scenario = 0u; scenario < 7u; ++scenario) {
        size_t count = run_allocation_case(scenario, 0u);
        REQUIRE(count > 0u);
        printf("source allocation scenario %u: %zu points\n", scenario, count);
        fflush(stdout);
        for (size_t failure = 1u; failure <= count; ++failure) {
            (void) run_allocation_case(scenario, failure);
        }
        REQUIRE(run_allocation_case(scenario, 0u) == count);
        printf("source product allocation failures: scenario %u, %zu points, no retained allocations\n", scenario, count);
    }
    return 0;
}
