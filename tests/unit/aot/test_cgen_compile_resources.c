/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cgen_compile_resources.c - Shared verifier work and physical ownership
 */
#include "base/xmalloc.h"
#include "aot/xi_cgen_verify_output.h"
#include <stdatomic.h>
#include <limits.h>
#ifdef XR_OS_WINDOWS
#include <windows.h>
#endif

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)

typedef XiCgenVerifyStatus (*VerifyPrototype)(XrCompileResources *, const char *, size_t, XiCgenVerifyResult *);
typedef XiCgenVerifyStatus (*IcePrototype)(XrCompileResources *, const char *, size_t, const char *);
_Static_assert(_Generic(&xr_compile_cgen_verify_output, VerifyPrototype: 1, default: 0), "Structural verifier takes caller resources");
_Static_assert(_Generic(&xr_compile_cgen_verify_c90_output, VerifyPrototype: 1, default: 0), "C90 verifier preserves typed failures");
_Static_assert(_Generic(&xr_compile_cgen_verify_output_or_ice, IcePrototype: 1, default: 0), "Fatal verifier takes caller resources");

typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[32];
static size_t calls, live, physical, fail_at = SIZE_MAX;
static void *counted_malloc(size_t bytes) {
    if (calls++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes); if (!memory) return NULL;
    for (size_t i = 0; i < 32; ++i) if (!allocations[i].pointer) {
        allocations[i] = (Allocation){memory, bytes}; ++live; physical += bytes; return memory;
    }
    CHECK(false); return NULL;
}
static void counted_free(void *memory) {
    if (!memory) return;
    for (size_t i = 0; i < 32; ++i) if (allocations[i].pointer == memory) {
        physical -= allocations[i].bytes; --live; allocations[i] = (Allocation){0}; xr_free(memory); return;
    }
    CHECK(false);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc counted_malloc
#define xr_free counted_free
#include "base/xcompile_resources.c"
#undef xr_malloc
#undef xr_free

/* Independent layout arithmetic for the two-pointer allocation prefix and
 * aligned ledger root; never derive expected work from verifier statistics. */
typedef union OracleAlignment {
    long double floating; uint64_t integer; void *pointer;
} OracleAlignment;
typedef union OracleHeader {
    struct { void *owner; size_t bytes; } fields;
#ifdef XR_COMPILER_MSVC
    OracleAlignment alignment;
#else
    max_align_t alignment;
#endif
} OracleHeader;
typedef struct OracleLedger {
    uint64_t limits[3], stats[5], references;
    atomic_bool locked;
} OracleLedger;
static const XrCompileResourceLimits unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
static XrCompileResources *ledger(XrCompileResourceLimits limits) {
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    return resources;
}
static void release(XrCompileResources *resources) {
    xr_compile_resources_release(resources); CHECK(!live && !physical);
}
static XiCgenVerifyResult poisoned(void) {
    XiCgenVerifyResult result; memset(&result, 0xA5, sizeof(result)); return result;
}

static void formula(bool c90, const char *source, size_t length) {
    const uint64_t diagnostic = sizeof(XiCgenVerifyCategory) + sizeof(int) + 256;
    const uint64_t scratch = sizeof(void *) + sizeof(size_t) + 64 * sizeof(long) + 4 * diagnostic + 160;
    CHECK(diagnostic == sizeof(XiCgenVerifyResult));
    uint64_t allocated = sizeof(OracleLedger) + sizeof(OracleHeader) + (c90 ? diagnostic : scratch);
    if (!c90) allocated += sizeof(OracleHeader) + length;
    /* Quoted C90: one allocation, n loop steps, 2n-1 byte reads, publication.
     * Newline-only W1-W4: scratch zero, two allocations, n copy bytes,
     * 3n-1 lexical operations, n line steps and n delimiter reads. */
    uint64_t work = 1 + (c90 ? 3 * length + diagnostic : scratch + diagnostic + 6 * length + 1);
    VerifyPrototype verify = c90 ? xr_compile_cgen_verify_c90_output : xr_compile_cgen_verify_output;
    for (unsigned dimension = 0; dimension < 3; ++dimension) for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits limits = unlimited;
        if (dimension == 0) limits.allocated_bytes = allocated - minus;
        if (dimension == 1) limits.live_bytes = allocated - minus;
        if (dimension == 2) limits.work = work - minus;
        XrCompileResources *resources = ledger(limits);
        XiCgenVerifyResult result = poisoned(), saved = result;
        CHECK(verify(resources, source, length, &result) == (minus ? XI_CGEN_VERIFY_BUDGET : XI_CGEN_VERIFY_PASSED));
        XrCompileResourceStats stats; CHECK(xr_compile_resources_stats(resources, &stats) == XR_COMPILE_RESOURCE_OK);
        if (minus) CHECK(!memcmp(&result, &saved, sizeof(result)));
        else {
            CHECK(result.category == XI_CGEN_VERIFY_OK);
            CHECK(stats.allocated_bytes == allocated && stats.peak_bytes == allocated);
            CHECK(stats.work == work && stats.allocation_count == (c90 ? 2u : 3u));
        }
        CHECK(stats.live_bytes == sizeof(OracleLedger)); release(resources);
    }
}

static void categories(void) {
    static const struct { const char *source, *message; XiCgenVerifyCategory category; int line; } cases[] = {
        {"void f(void) {\n", "unbalanced braces at end of unit (depth 1)", XI_CGEN_VERIFY_W1_BALANCE, 1},
        {"\"unfinished", "unterminated string literal", XI_CGEN_VERIFY_W1_BALANCE, 1},
        {"static int x = pkg/../oops;\n", "path fragment '../' in emitted code", XI_CGEN_VERIFY_W2_IDENTIFIER, 1},
        {"return 0;\n", "statement-shaped line at file scope (brace depth 0)", XI_CGEN_VERIFY_W3_SCOPE, 1},
        {"void f(void) {\n return v31;\n}\n", "temporary v31 used before it is defined", XI_CGEN_VERIFY_W4_FORWARD_REF, 2}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        XrCompileResources *resources = ledger(unlimited); size_t length = strlen(cases[i].source);
        void *producer = NULL; CHECK(xr_compile_resources_alloc(resources, length, &producer) == XR_COMPILE_RESOURCE_OK);
        memcpy(producer, cases[i].source, length); XiCgenVerifyResult result = poisoned();
        CHECK(xr_compile_cgen_verify_output(resources, producer, length, &result) == XI_CGEN_VERIFY_MALFORMED);
        xr_compile_resources_release(resources); xr_compile_resources_free(producer);
        CHECK(!live && !physical); CHECK(result.category == cases[i].category && result.line == cases[i].line);
        CHECK(!strcmp(result.message, cases[i].message));
    }
    XrCompileResources *resources = ledger(unlimited); XiCgenVerifyResult result = poisoned();
    const char input[] = "int f(void) {\n // prohibited\n}\n";
    CHECK(xr_compile_cgen_verify_c90_output(resources, input, sizeof(input) - 1, &result) == XI_CGEN_VERIFY_MALFORMED);
    CHECK(result.category == XI_CGEN_VERIFY_C90_RESTRICTED && result.line == 2 && !strcmp(result.message, "C++ line-comment residue"));
    release(resources);
}

static void comparison_formula(void) {
    const uint64_t diagnostic = sizeof(XiCgenVerifyCategory) + sizeof(int) + 256;
    const uint64_t detail_length = sizeof("C11 _Atomic residue") - 1;
    const uint64_t bytes = sizeof(OracleLedger) + sizeof(OracleHeader) + diagnostic;
    /* Ledger, allocation, scan step, two lookahead bytes, first mismatching
     * inline byte, table step, keyword including NUL, seven equal bytes.
     * The local diagnostic clears D bytes and stores its two scalar fields;
     * %s scans three format bytes and L+1 text bytes, writes L+1 bytes, then
     * publishes the complete D-byte result. */
    const uint64_t work = 1 + 1 + 1 + 2 + 1 + 1 + 8 + 7 + diagnostic +
        sizeof(XiCgenVerifyCategory) + sizeof(int) + 2 * detail_length + 5 + diagnostic;
    for (unsigned dimension = 0; dimension < 3; ++dimension) for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits limits = unlimited;
        if (dimension == 0) limits.allocated_bytes = bytes - minus;
        if (dimension == 1) limits.live_bytes = bytes - minus;
        if (dimension == 2) limits.work = work - minus;
        XrCompileResources *resources = ledger(limits);
        XiCgenVerifyResult result = poisoned(), saved = result;
        CHECK(xr_compile_cgen_verify_c90_output(resources, "_Atomic", 7, &result) ==
            (minus ? XI_CGEN_VERIFY_BUDGET : XI_CGEN_VERIFY_MALFORMED));
        if (minus) CHECK(!memcmp(&result, &saved, sizeof(result)));
        else {
            XrCompileResourceStats stats; CHECK(xr_compile_resources_stats(resources, &stats) == XR_COMPILE_RESOURCE_OK);
            CHECK(stats.work == work && stats.allocated_bytes == bytes && stats.peak_bytes == bytes);
            CHECK(result.category == XI_CGEN_VERIFY_C90_RESTRICTED && result.line == 1);
            CHECK(!strcmp(result.message, "C11 _Atomic residue"));
        }
        release(resources);
    }
}

static void scan_semantics(void) {
    static const char *valid[] = {
        "/* } )( */\nstatic const char *s = \"../ {\\\"\";\nvoid f(void) {\n // } (\n int v7=2;\n return v7;\n}\n",
        "void f(void) {\n#define v3 (f->v3)\n#if defined(DEBUG)\n }}}\n#endif\n return v3;\n}\n",
        "void f(void) {\n int v0=1;\n return v0;\n}\nvoid g(void) {\n int v0=2;\n return v0;\n}\n",
        "#define" /* Empty final token must never read past the input. */
    };
    for (unsigned i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        XrCompileResources *resources = ledger(unlimited);
        CHECK(xr_compile_cgen_verify_output(resources, valid[i], strlen(valid[i]), NULL) == XI_CGEN_VERIFY_PASSED);
        release(resources);
    }
    static const struct { const char *source; XiCgenVerifyCategory category; } priority[] = {
        {"return pkg/../x;\nvoid f(void) {\n return v31;\n", XI_CGEN_VERIFY_W1_BALANCE},
        {"return pkg/../x;\nvoid f(void) {\n return v31;\n}\n", XI_CGEN_VERIFY_W2_IDENTIFIER},
        {"return 1;\nvoid f(void) {\n return v31;\n}\n", XI_CGEN_VERIFY_W3_SCOPE},
        {"void f(void) {\n int v0=1;\n}\nvoid g(void) {\n return v0;\n}\n", XI_CGEN_VERIFY_W4_FORWARD_REF}
    };
    for (unsigned i = 0; i < sizeof(priority) / sizeof(priority[0]); ++i) {
        XrCompileResources *resources = ledger(unlimited); XiCgenVerifyResult result;
        CHECK(xr_compile_cgen_verify_output(resources, priority[i].source, strlen(priority[i].source), &result) == XI_CGEN_VERIFY_MALFORMED);
        CHECK(result.category == priority[i].category); release(resources);
    }
    static const char *forbidden[] = {"_Atomic", "_Thread_local", "_Alignof", "long long", "({", "){", "{ .", "...",
        "union {", "[];", "for (int ", "for (size_t ", "xrt_shared_", "xrt_builtins", "xrt_global_ctx", "xrt_map_",
        "xrt_set_", "xrt_thread", "xrt_coro", "xrt_task", "xrt_channel", "inline", "//"};
    for (unsigned i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
        XrCompileResources *resources = ledger(unlimited); XiCgenVerifyResult result;
        CHECK(xr_compile_cgen_verify_c90_output(resources, forbidden[i], strlen(forbidden[i]), &result) == XI_CGEN_VERIFY_MALFORMED);
        CHECK(result.category == XI_CGEN_VERIFY_C90_RESTRICTED && result.line == 1); release(resources);
    }
    const char masked[] = "/* _Atomic // ({ */ const char *s = \"_Atomic // \\\"\"; int inlined = 1; char c = '(';";
    XrCompileResources *resources = ledger(unlimited);
    CHECK(xr_compile_cgen_verify_c90_output(resources, masked, sizeof(masked) - 1, NULL) == XI_CGEN_VERIFY_PASSED);
    release(resources);
}

static void allocation_failures(void) {
    const char source[] = "void f(void) {\n int v0=1;\n int v2048=2;\n int v4096=3;\n return;\n}\n";
    XrCompileResources *resources = ledger(unlimited); size_t before = calls; XiCgenVerifyResult result;
    CHECK(xr_compile_cgen_verify_output(resources, source, sizeof(source) - 1, &result) == XI_CGEN_VERIFY_PASSED);
    size_t points = calls - before; CHECK(points == 5); release(resources);
    for (size_t point = 0; point < points; ++point) for (unsigned wrapper = 0; wrapper < 2; ++wrapper) {
        resources = ledger(unlimited); result = poisoned(); XiCgenVerifyResult saved = result;
        fail_at = calls + point;
        XiCgenVerifyStatus status = wrapper ? xr_compile_cgen_verify_output_or_ice(resources, source, sizeof(source) - 1, "allocation") :
            xr_compile_cgen_verify_output(resources, source, sizeof(source) - 1, &result);
        CHECK(status == XI_CGEN_VERIFY_OUT_OF_MEMORY); CHECK(!memcmp(&result, &saved, sizeof(result)));
        fail_at = SIZE_MAX; release(resources);
    }
    resources = ledger(unlimited); result = poisoned(); XiCgenVerifyResult saved = result;
    fail_at = calls; CHECK(xr_compile_cgen_verify_c90_output(resources, "\"x\"", 3, &result) == XI_CGEN_VERIFY_OUT_OF_MEMORY);
    CHECK(!memcmp(&result, &saved, sizeof(result))); fail_at = SIZE_MAX; release(resources);
    puts("verifier allocator failures: five real W1-W4 points and one C90 point, physical zero");
}

static void growth_formula(void) {
    const char source[] = "void f(void) {\n int v0=1;\n int v2048=2;\n int v4096=3;\n return;\n}\n";
    const uint64_t diagnostic = sizeof(XiCgenVerifyCategory) + sizeof(int) + 256;
    const uint64_t scratch = sizeof(void *) + sizeof(size_t) + 64 * sizeof(long) + 4 * diagnostic + 160;
    const uint64_t fixed = sizeof(OracleLedger) + 2 * sizeof(OracleHeader) + scratch + sizeof(source) - 1;
    /* Definitions grow the seen bytes from 1024 to 4096 to 8192. All three
     * allocations are cumulative; the two largest coexist during resize. */
    const uint64_t cumulative = fixed + 3 * sizeof(OracleHeader) + 1024 + 4096 + 8192;
    const uint64_t peak = fixed + 2 * sizeof(OracleHeader) + 4096 + 8192;
    for (unsigned dimension = 0; dimension < 2; ++dimension) for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits limits = unlimited;
        if (dimension == 0) limits.allocated_bytes = cumulative - minus;
        else limits.live_bytes = peak - minus;
        XrCompileResources *resources = ledger(limits); XiCgenVerifyResult result = poisoned(), saved = result;
        CHECK(xr_compile_cgen_verify_output(resources, source, sizeof(source) - 1, &result) ==
            (minus ? XI_CGEN_VERIFY_BUDGET : XI_CGEN_VERIFY_PASSED));
        if (minus) CHECK(!memcmp(&result, &saved, sizeof(result)));
        else {
            XrCompileResourceStats stats; CHECK(xr_compile_resources_stats(resources, &stats) == XR_COMPILE_RESOURCE_OK);
            CHECK(stats.allocated_bytes == cumulative && stats.peak_bytes == peak && stats.allocation_count == 6);
            CHECK(stats.live_bytes == sizeof(OracleLedger));
        }
        release(resources);
    }
}

static void failed_work_preserves_diagnostic(VerifyPrototype verify, const char *source) {
    XrCompileResources *resources = ledger(unlimited); XiCgenVerifyResult result;
    size_t length = strlen(source);
    CHECK(verify(resources, source, length, &result) == XI_CGEN_VERIFY_MALFORMED);
    XrCompileResourceStats stats; CHECK(xr_compile_resources_stats(resources, &stats) == XR_COMPILE_RESOURCE_OK);
    release(resources);
    for (uint64_t work = 1; work < stats.work; ++work) {
        XrCompileResourceLimits limits = unlimited; limits.work = work; resources = ledger(limits);
        result = poisoned(); XiCgenVerifyResult saved = result;
        CHECK(verify(resources, source, length, &result) == XI_CGEN_VERIFY_BUDGET);
        CHECK(!memcmp(&result, &saved, sizeof(result))); release(resources);
    }
    printf("all %llu incomplete work limits preserve diagnostic bytes\n", (unsigned long long)(stats.work - 1));
}

static void arguments_and_cumulative(void) {
    XrCompileResources *resources = ledger(unlimited); XiCgenVerifyResult result = poisoned(), saved = result;
    CHECK(xr_compile_cgen_verify_output(NULL, NULL, 0, &result) == XI_CGEN_VERIFY_BAD_ARGUMENT);
    CHECK(xr_compile_cgen_verify_output(resources, NULL, 1, &result) == XI_CGEN_VERIFY_BAD_ARGUMENT);
    CHECK(xr_compile_cgen_verify_c90_output(resources, NULL, 1, &result) == XI_CGEN_VERIFY_BAD_ARGUMENT);
    CHECK(xr_compile_cgen_verify_output(resources, "", SIZE_MAX, &result) == XI_CGEN_VERIFY_BUDGET);
    CHECK(!memcmp(&result, &saved, sizeof(result)));
    CHECK(xr_compile_cgen_verify_output(resources, NULL, 0, &result) == XI_CGEN_VERIFY_PASSED);
    CHECK(result.category == XI_CGEN_VERIFY_OK && !result.line && !result.message[0]); release(resources);
    XrCompileResourceLimits limits = unlimited; limits.work = 1 + 3 * 3 + sizeof(XiCgenVerifyResult);
    resources = ledger(limits);
    CHECK(xr_compile_cgen_verify_c90_output(resources, "\"x\"", 3, &result) == XI_CGEN_VERIFY_PASSED);
    result = poisoned(); saved = result; size_t before = calls;
    CHECK(xr_compile_cgen_verify_output_or_ice(resources, "\n", 1, "budget") == XI_CGEN_VERIFY_BUDGET);
    CHECK(calls == before); CHECK(xr_compile_cgen_verify_c90_output(resources, "\"x\"", 3, &result) == XI_CGEN_VERIFY_BUDGET);
    CHECK(!memcmp(&result, &saved, sizeof(result))); release(resources);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--ice")) {
#ifdef XR_OS_WINDOWS
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
        XrCompileResources *resources = ledger(unlimited);
        static const char malformed[] = "void f(void) {\n";
        (void)xr_compile_cgen_verify_output_or_ice(resources, malformed, sizeof(malformed) - 1, "resource-fatal");
        return 86;
    }
    CHECK(argc == 1);
    formula(true, "\"abc\"", 5); formula(true, "\"abcdefgh\"", 10);
    formula(false, "\n", 1); formula(false, "\n\n\n\n\n\n\n", 7);
    comparison_formula(); categories(); scan_semantics(); allocation_failures(); growth_formula();
    failed_work_preserves_diagnostic(xr_compile_cgen_verify_output, "void f(void) {\n return v31;\n}\n");
    failed_work_preserves_diagnostic(xr_compile_cgen_verify_c90_output, "_Atomic");
    arguments_and_cumulative();
    puts("neutral compiler verifier resources and independent formulas passed"); return 0;
}
