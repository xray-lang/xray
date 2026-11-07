/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cgen_compile_resources.c - Controlled verifier memory and physical ownership
 */
#include "base/xmalloc.h"
#include "lir/xi_cgen_verify_output.h"
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
static size_t calls, live, physical, physical_peak, fail_at = SIZE_MAX;
static void *counted_malloc(size_t bytes) {
    if (calls++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes); if (!memory) return NULL;
    for (size_t i = 0; i < 32; ++i) if (!allocations[i].pointer) {
        allocations[i] = (Allocation){memory, bytes}; ++live; physical += bytes;
        if (physical > physical_peak) physical_peak = physical;
        return memory;
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

static const XrCompileResourceLimits unlimited = {XR_COMPILE_MEMORY_ORDINARY, 0, true};
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

/* The physical allocator is an independent oracle for held memory. */
static XiCgenVerifyStatus memory_edges(VerifyPrototype verify, const char *source, size_t length) {
    XrCompileResources *resources = ledger(unlimited);
    physical_peak = physical;
    XiCgenVerifyResult expected = poisoned();
    XiCgenVerifyStatus status = verify(resources, source, length, &expected);
    CHECK(status == XI_CGEN_VERIFY_PASSED || status == XI_CGEN_VERIFY_MALFORMED);
    XrCompileResourceStats stats;
    CHECK(xr_compile_resources_stats(resources, &stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.peak_bytes == physical_peak && stats.held_bytes == physical && live == 1);
    uint64_t peak = stats.peak_bytes;
    CHECK(peak > physical);
    release(resources);
    for (int delta = -1; delta <= 1; ++delta) {
        XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_FINITE, (uint64_t)((int64_t)peak + delta), true};
        resources = ledger(limits);
        XiCgenVerifyResult result = poisoned(), saved = result;
        CHECK(verify(resources, source, length, &result) == (delta < 0 ? XI_CGEN_VERIFY_MEMORY_LIMIT : status));
        if (delta < 0) CHECK(!memcmp(&result, &saved, sizeof(result)));
        else CHECK(result.category == expected.category && result.line == expected.line && !strcmp(result.message, expected.message));
        CHECK(xr_compile_resources_stats(resources, &stats) == XR_COMPILE_RESOURCE_OK);
        CHECK(stats.held_bytes == physical && stats.peak_bytes <= limits.memory_limit && live == 1);
        release(resources);
    }
    return status;
}
static void formula(bool c90, const char *source, size_t length) {
    CHECK(memory_edges(c90 ? xr_compile_cgen_verify_c90_output : xr_compile_cgen_verify_output,
        source, length) == XI_CGEN_VERIFY_PASSED);
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
    CHECK(memory_edges(xr_compile_cgen_verify_c90_output, "_Atomic", 7) == XI_CGEN_VERIFY_MALFORMED);
    XrCompileResources *resources = ledger(unlimited); XiCgenVerifyResult result;
    CHECK(xr_compile_cgen_verify_c90_output(resources, "_Atomic", 7, &result) == XI_CGEN_VERIFY_MALFORMED);
    CHECK(result.category == XI_CGEN_VERIFY_C90_RESTRICTED && result.line == 1);
    CHECK(!strcmp(result.message, "C11 _Atomic residue")); release(resources);
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
    size_t points = calls - before; CHECK(points == 7); release(resources);
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
    puts("verifier allocator failures: seven real W1-W4 points and one C90 point, physical zero");
}

static void growth_formula(void) {
    const char source[] = "void f(void) {\n int v0=1;\n int v2048=2;\n int v4096=3;\n return;\n}\n";
    CHECK(memory_edges(xr_compile_cgen_verify_output, source, sizeof(source)-1) == XI_CGEN_VERIFY_PASSED);
}

static void failed_memory_preserves_diagnostic(VerifyPrototype verify, const char *source) {
    (void)memory_edges(verify, source, strlen(source));
}

static void line_scratch_boundaries(void) {
    static const struct {
        const char *source, *message;
        XiCgenVerifyCategory category;
        int line;
    } cases[] = {
        {"/* first\n } ) ../\n */\nvoid f(void) {\n return;\n}\n", "", XI_CGEN_VERIFY_OK, 0},
        {"static const char *s = \"{\n../ ) }\";\n", "", XI_CGEN_VERIFY_OK, 0},
        {"static const char *s = \"{\\\n../ ) }\";\n", "", XI_CGEN_VERIFY_OK, 0},
        {"static int c = '\\\n(';\n", "", XI_CGEN_VERIFY_OK, 0},
        {"/*\r\n } ) ../\r\n */\r\nstatic int x;", "", XI_CGEN_VERIFY_OK, 0},
        {"return pkg/../x;\n}\n/* unfinished", "unterminated block comment", XI_CGEN_VERIFY_W1_BALANCE, 3},
        {"return pkg/../x;\n}\n\"unfinished", "unterminated string literal", XI_CGEN_VERIFY_W1_BALANCE, 3},
        {"static const char *s = \"ok\"; // tail", "", XI_CGEN_VERIFY_OK, 0},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        size_t length = strlen(cases[i].source), before = calls;
        XrCompileResources *resources = ledger(unlimited);
        XiCgenVerifyResult result = poisoned();
        CHECK(xr_compile_cgen_verify_output(resources, cases[i].source, length, &result) ==
            (cases[i].category == XI_CGEN_VERIFY_OK ? XI_CGEN_VERIFY_PASSED : XI_CGEN_VERIFY_MALFORMED));
        CHECK(result.category == cases[i].category && result.line == cases[i].line);
        CHECK(!strcmp(result.message, cases[i].message));
        XrCompileResourceStats stats; CHECK(xr_compile_resources_stats(resources, &stats) == XR_COMPILE_RESOURCE_OK);
        CHECK(stats.held_bytes == physical && live == 1);
        size_t points = calls - before - 1; CHECK(points); release(resources);
        for (size_t point = 0; point < points; ++point) {
            resources = ledger(unlimited); result = poisoned(); XiCgenVerifyResult saved = result;
            fail_at = calls + point;
            CHECK(xr_compile_cgen_verify_output(resources, cases[i].source, length, &result) == XI_CGEN_VERIFY_OUT_OF_MEMORY);
            CHECK(!memcmp(&result, &saved, sizeof(result)));
            fail_at = SIZE_MAX; release(resources);
        }
        (void)memory_edges(xr_compile_cgen_verify_output, cases[i].source, length);
    }
    static char many_lines[4096]; memset(many_lines, '\n', sizeof(many_lines));
    formula(false, many_lines, sizeof(many_lines));
    puts("cross-line lexical priority, held-memory boundaries, OOM and physical scratch passed");
}

static void w4_token_boundaries(void) {
    static const struct {
        const char *source;
        XiCgenVerifyCategory category;
        int line;
        const char *message;
    } cases[] = {
        {"void f(void) {\n return foo_v3 + av3 + v3abc + v3_ + v3v4 + 1v3;\n}\n", XI_CGEN_VERIFY_OK, 0, ""},
        {"void f(void) {\n return state->v3 + object.v4;\n}\n", XI_CGEN_VERIFY_OK, 0, ""},
        {"void f(void) {\n int v0=1;\n int v1=2;\n return v0+v1;\n}\n", XI_CGEN_VERIFY_OK, 0, ""},
        {"void f(void) {\n return v3; int v3=1;\n}\n", XI_CGEN_VERIFY_OK, 0, ""},
        {"void f(void) {\n return v3abc + v4;\n}\n", XI_CGEN_VERIFY_W4_FORWARD_REF, 2, "temporary v4 used before it is defined"},
        {"void f(void) {\n return v3_ + v5;\n}\n", XI_CGEN_VERIFY_W4_FORWARD_REF, 2, "temporary v5 used before it is defined"},
        {"void f(void) {\n return state->v3 + v6;\n}\n", XI_CGEN_VERIFY_W4_FORWARD_REF, 2, "temporary v6 used before it is defined"},
        {"void f(void) {\n int v0=0, v1=1, v2=2, v3=3, v4=4, v5=5, v6=6, v7=7, v8=8, v9=9, v10=10, v11=11, v12=12, v13=13, v14=14, v15=15, v16=16, v17=17, v18=18, v19=19, v20=20, v21=21, v22=22, v23=23, v24=24, v25=25, v26=26, v27=27, v28=28, v29=29, v30=30, v31=31, v32=32, v33=33, v34=34, v35=35, v36=36, v37=37, v38=38, v39=39, v40=40, v41=41, v42=42, v43=43, v44=44, v45=45, v46=46, v47=47, v48=48, v49=49, v50=50, v51=51, v52=52, v53=53, v54=54, v55=55, v56=56, v57=57, v58=58, v59=59, v60=60, v61=61, v62=62, v63=63;\n return v63;\n}\n", XI_CGEN_VERIFY_OK, 0, ""},
        {"void f(void) {\n int v0=0, v1=1, v2=2, v3=3, v4=4, v5=5, v6=6, v7=7, v8=8, v9=9, v10=10, v11=11, v12=12, v13=13, v14=14, v15=15, v16=16, v17=17, v18=18, v19=19, v20=20, v21=21, v22=22, v23=23, v24=24, v25=25, v26=26, v27=27, v28=28, v29=29, v30=30, v31=31, v32=32, v33=33, v34=34, v35=35, v36=36, v37=37, v38=38, v39=39, v40=40, v41=41, v42=42, v43=43, v44=44, v45=45, v46=46, v47=47, v48=48, v49=49, v50=50, v51=51, v52=52, v53=53, v54=54, v55=55, v56=56, v57=57, v58=58, v59=59, v60=60, v61=61, v62=62, v63=63, v64=64;\n return v64;\n}\n", XI_CGEN_VERIFY_W4_FORWARD_REF, 3, "temporary v64 used before it is defined"},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        size_t length = strlen(cases[i].source);
        XrCompileResources *resources = ledger(unlimited); size_t before = calls;
        XiCgenVerifyResult result = poisoned();
        CHECK(xr_compile_cgen_verify_output(resources, cases[i].source, length, &result) ==
            (cases[i].category == XI_CGEN_VERIFY_OK ? XI_CGEN_VERIFY_PASSED : XI_CGEN_VERIFY_MALFORMED));
        CHECK(result.category == cases[i].category && result.line == cases[i].line);
        CHECK(!strcmp(result.message, cases[i].message));
        size_t points = calls - before; CHECK(points); release(resources);
        for (size_t point = 0; point < points; ++point) {
            resources = ledger(unlimited); result = poisoned(); XiCgenVerifyResult saved = result;
            fail_at = calls + point;
            CHECK(xr_compile_cgen_verify_output(resources, cases[i].source, length, &result) == XI_CGEN_VERIFY_OUT_OF_MEMORY);
            CHECK(!memcmp(&result, &saved, sizeof(result)));
            fail_at = SIZE_MAX; release(resources);
        }
    }
    failed_memory_preserves_diagnostic(xr_compile_cgen_verify_output,
        "void f(void) {\n return v3abc + v4;\n}\n");
}

static void uses_growth_formula(void) {
    const char source[] = "void f(void) {\n int v0=7;\n return v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0 + v0;\n}\n";
    CHECK(memory_edges(xr_compile_cgen_verify_output, source, sizeof(source)-1) == XI_CGEN_VERIFY_PASSED);
}

static void fused_line_vectors(void) {
    static const struct {
        const char *source;
        XiCgenVerifyCategory category;
        int line;
        const char *message;
    } cases[] = {
        {"void f(void) {\n return v7 + v3; int v3=1; int v7=2;\n}\n", XI_CGEN_VERIFY_OK, 0, ""},
        {"void f(void) {\n return v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3; int v3=7;\n}\n", XI_CGEN_VERIFY_OK, 0, ""},
        {"void f(void) {\n int v3=7;\n return v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v9 + v8;\n}\n", XI_CGEN_VERIFY_W4_FORWARD_REF, 3, "temporary v9 used before it is defined"},
        {"void f(void) {\n return v7 + v3; int v7=1;\n}\n", XI_CGEN_VERIFY_W4_FORWARD_REF, 2, "temporary v3 used before it is defined"},
        {"void f(void) {\n const char *s=\"v8 ../ } (\"; /* v9 */\n int v3=7;\n return v3;\n}\n", XI_CGEN_VERIFY_OK, 0, ""},
        {"void f(void) {\n return v8 + pkg/../oops;\n}\n", XI_CGEN_VERIFY_W2_IDENTIFIER, 2, "path fragment '../' in emitted code"},
        {"void f(void) {\n return v8 + pkg/../oops; )\n}\n", XI_CGEN_VERIFY_W1_BALANCE, 2, "unbalanced ')' (closes with no matching '(')"},
        {"void f(void) {\n return v8 + pkg/../oops;\n}\n/* late", XI_CGEN_VERIFY_W1_BALANCE, 4, "unterminated block comment"},
        {"void f(void) {\n#if FEATURE\n return v9; )\n#endif\n#define v3 frame_slot\n return v3;\n}\n", XI_CGEN_VERIFY_OK, 0, ""},
        {"void f(void) {\n int v3=7;\n return v3;\n}\nvoid g(void) {\n return v3;\n}\n", XI_CGEN_VERIFY_W4_FORWARD_REF, 6, "temporary v3 used before it is defined"},
        {"void f(void) {\n return v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3; int v3=7;\n return v3;\n}\n", XI_CGEN_VERIFY_OK, 0, ""},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        size_t length = strlen(cases[i].source);
        XrCompileResources *resources = ledger(unlimited); size_t before = calls;
        XiCgenVerifyResult result = poisoned();
        CHECK(xr_compile_cgen_verify_output(resources, cases[i].source, length, &result) ==
            (cases[i].category == XI_CGEN_VERIFY_OK ? XI_CGEN_VERIFY_PASSED : XI_CGEN_VERIFY_MALFORMED));
        CHECK(result.category == cases[i].category && result.line == cases[i].line);
        CHECK(!strcmp(result.message, cases[i].message));
        size_t points = calls - before; CHECK(points); release(resources);
        for (size_t point = 0; point < points; ++point) for (unsigned wrapper = 0; wrapper < 2; ++wrapper) {
            resources = ledger(unlimited); result = poisoned(); XiCgenVerifyResult saved = result;
            fail_at = calls + point;
            XiCgenVerifyStatus status = wrapper ? xr_compile_cgen_verify_output_or_ice(resources, cases[i].source, length, "fused-oom") :
                xr_compile_cgen_verify_output(resources, cases[i].source, length, &result);
            CHECK(status == XI_CGEN_VERIFY_OUT_OF_MEMORY); CHECK(!memcmp(&result, &saved, sizeof(result)));
            fail_at = SIZE_MAX; release(resources);
        }
    }
    failed_memory_preserves_diagnostic(xr_compile_cgen_verify_output,
        "void f(void) {\n return v7 + v3; int v7=1;\n}\n");
    failed_memory_preserves_diagnostic(xr_compile_cgen_verify_output,
        "void f(void) {\n int v3=7;\n return v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v3 + v9 + v8;\n}\n");
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
    resources = ledger(unlimited);
    XrCompileResourceStats baseline;
    CHECK(xr_compile_resources_stats(resources, &baseline) == XR_COMPILE_RESOURCE_OK);
    release(resources);
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_FINITE, baseline.held_bytes, true};
    resources = ledger(limits); result = poisoned(); saved = result;
    size_t before = calls;
    CHECK(xr_compile_cgen_verify_output_or_ice(resources, "\n", 1, "memory") == XI_CGEN_VERIFY_MEMORY_LIMIT);
    CHECK(xr_compile_cgen_verify_c90_output(resources, "\"x\"", 3, &result) == XI_CGEN_VERIFY_MEMORY_LIMIT);
    CHECK(calls == before && !memcmp(&result, &saved, sizeof(result))); release(resources);
    resources = ledger(unlimited);
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        CHECK(xr_compile_cgen_verify_c90_output(resources, "\"x\"", 3, &result) == XI_CGEN_VERIFY_PASSED);
        CHECK(xr_compile_cgen_verify_output(resources, "\n", 1, &result) == XI_CGEN_VERIFY_PASSED);
        XrCompileResourceStats current;
        CHECK(xr_compile_resources_stats(resources, &current) == XR_COMPILE_RESOURCE_OK);
        CHECK(current.held_bytes == baseline.held_bytes && current.held_bytes == physical && live == 1);
    }
    release(resources);
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
    failed_memory_preserves_diagnostic(xr_compile_cgen_verify_output, "void f(void) {\n return v31;\n}\n");
    failed_memory_preserves_diagnostic(xr_compile_cgen_verify_c90_output, "_Atomic");
    line_scratch_boundaries(); w4_token_boundaries(); uses_growth_formula(); fused_line_vectors(); arguments_and_cumulative();
    puts("compiler verifier held memory and physical failure ownership passed"); return 0;
}
