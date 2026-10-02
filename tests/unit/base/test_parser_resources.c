/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_parser_resources.c - Production Session/parser resource ownership tests
 */
#include "base/xmalloc.h"
#include "base/xcompile_state.h"
#include "toolchain/xcompiler_session.h"
#include "toolchain/xcompiler_arena_backing.h"
#include "frontend/parser/xparse.h"
#include "frontend/parser/xattribute_registry.h"
#include <stdio.h>
#include <string.h>
#include <io.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_COMPILE_RESOURCE_OK)
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[8192];
static size_t physical_live, physical_peak, physical_total, allocation_count, attempt_count;
static size_t fail_at = SIZE_MAX;

static void *observed_malloc(size_t bytes) {
    if (attempt_count++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes);
    CHECK(memory);
    size_t slot = 0;
    while (slot < 8192 && allocations[slot].pointer) ++slot;
    CHECK(slot < 8192);
    allocations[slot] = (Allocation) {memory, bytes};
    physical_live += bytes;
    physical_total += bytes;
    if (physical_live > physical_peak) physical_peak = physical_live;
    ++allocation_count;
    return memory;
}

static void observed_free(void *memory) {
    if (!memory) return;
    size_t slot = 0;
    while (slot < 8192 && allocations[slot].pointer != memory) ++slot;
    CHECK(slot < 8192);
    physical_live -= allocations[slot].bytes;
    --allocation_count;
    allocations[slot] = (Allocation) {0};
    xr_free(memory);
}

#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) observed_malloc(bytes)
#define xr_free(memory) observed_free(memory)
#include "base/xcompile_resources.c"

static const XrCompileResourceLimits unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
static const char source[] = "// retained trivia\ntype Thing = { value: i64 }\nconst answer = 42\nconst text = \"value ${answer}\"\n";

static void reset(void) {
    CHECK(!physical_live && !allocation_count);
    physical_peak = physical_total = attempt_count = 0;
    fail_at = SIZE_MAX;
}

static XrCompileResourceStatus pipeline(const XrCompileResourceLimits *limits, XrCompileResourceStats *observed) {
    XrCompileResources *resources = NULL;
    XrCompilerSession *session = NULL;
    AstNode *program = NULL;
    XrCompileResourceStatus status = xr_compile_resources_new(limits, &resources);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    XrCompilerSessionStatus opened = xr_compile_session_new(resources, &session);
    if (opened != XR_COMPILER_SESSION_OK) {
        status = opened == XR_COMPILER_SESSION_BUDGET ? XR_COMPILE_RESOURCE_BUDGET :
                 opened == XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_COMPILE_RESOURCE_OUT_OF_MEMORY : XR_COMPILE_RESOURCE_BAD_ARGUMENT;
        goto done;
    }
    XrParseStatus parsed = xr_compile_parse_with_trivia(session, source, "resource-owner.xr", &program);
    status = xr_compile_session_resource_status(session);
    if (status == XR_COMPILE_RESOURCE_OK) {
        CHECK(parsed == XR_PARSE_OK && program && program->as.program.owns_arena);
        CHECK(program->leading_comments && program->as.program.count == 3);
    } else {
        CHECK(!program && (parsed == XR_PARSE_BUDGET || parsed == XR_PARSE_OUT_OF_MEMORY));
        size_t calls_before_retry = attempt_count;
        CHECK(xr_compile_parse(session, (const char *) (uintptr_t) 1, &program) == parsed);
        CHECK(attempt_count == calls_before_retry && !program);
    }
done:
    OK(xr_compile_resources_stats(resources, observed));
    CHECK(observed->live_bytes == physical_live && observed->peak_bytes == physical_peak &&
          observed->allocated_bytes == physical_total);
    xr_compile_session_free(session);
    xr_compile_resources_release(resources);
    if (program) {
        CHECK(program->leading_comments->length == 16);
        CHECK(!memcmp(program->leading_comments->start, " retained trivia", 16));
        CHECK(program->as.program.statements[1]->type == AST_CONST_DECL);
        xr_program_destroy(program);
    }
    CHECK(!physical_live && !allocation_count);
    return status;
}

static void faults(void) {
    reset();
    XrCompileResourceStats exact = {0}, observed = {0};
    OK(pipeline(&unlimited, &exact));
    printf("parser baseline: allocations=%zu bytes=%llu peak=%llu work=%llu\n", attempt_count,
           (unsigned long long) exact.allocated_bytes, (unsigned long long) exact.peak_bytes,
           (unsigned long long) exact.work);
    size_t points = attempt_count;
    for (size_t i = 0; i < points; ++i) {
        reset(); fail_at = i;
        CHECK(pipeline(&unlimited, &observed) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
        CHECK(attempt_count == i + 1);
    }
    XrCompileResourceLimits limits = {exact.allocated_bytes, exact.peak_bytes, exact.work};
    reset(); OK(pipeline(&limits, &observed));
    for (unsigned i = 0; i < 3; ++i) {
        reset(); XrCompileResourceLimits smaller = limits;
        if (!i) --smaller.allocated_bytes;
        else if (i == 1) --smaller.live_bytes;
        else --smaller.work;
        CHECK(pipeline(&smaller, &observed) == XR_COMPILE_RESOURCE_BUDGET);
    }
    for (uint64_t work = 0; work < exact.work; ++work) {
        reset();
        observed = (XrCompileResourceStats) {0};
        XrCompileResourceLimits boundary = unlimited;
        boundary.work = work;
        CHECK(pipeline(&boundary, &observed) == XR_COMPILE_RESOURCE_BUDGET);
        CHECK(observed.work <= work);
    }
}

static void recoverable_and_rollback(void) {
    reset();
    XrCompileResources *resources = NULL;
    XrCompilerSession *session = NULL;
    OK(xr_compile_resources_new(&unlimited, &resources));
    CHECK(xr_compile_session_new(resources, &session) == XR_COMPILER_SESSION_OK);
    XrCompileState *state = xr_compile_session_compile_state(session);
    XrArena arena = {0};
    XrArenaBacking backing;
    CHECK(xr_compiler_arena_state_backing(state, &backing) == XR_ARENA_OK);
    CHECK(xr_arena_open(&arena, 128, &backing) == XR_ARENA_OK);
    Parser parser = {0};
    CHECK(xr_compile_parser_open(&parser, session, "const value =", NULL, &arena) == XR_PARSE_OK);
    AstNode *partial = NULL;
    CHECK(xr_compile_parse_recoverable(&parser, &partial) == XR_PARSE_RECOVERED);
    CHECK(partial && !partial->as.program.owns_arena && partial->as.program.arena == &arena);
    xr_compile_parser_close(&parser);
    parser = (Parser) {0};
    CHECK(xr_compile_parser_open(&parser, session, "const value = 1", NULL, &arena) == XR_PARSE_OK);
    Parser checkpoint = parser;
    CHECK(xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET) == XR_COMPILE_RESOURCE_BUDGET);
    parser = checkpoint;
    AstNode *output = NULL;
    CHECK(xr_compile_parse_recoverable(&parser, &output) == XR_PARSE_BUDGET && !output);
    CHECK(xr_compile_session_resource_status(session) == XR_COMPILE_RESOURCE_BUDGET);
    xr_compile_parser_close(&parser);
    xr_compile_session_free(session);
    xr_compile_resources_release(resources);
    xr_arena_destroy(&arena);
    CHECK(!physical_live && !allocation_count);
}

static void scopes(void) {
    for (unsigned mode = 0; mode < 5; ++mode) {
        reset();
        XrCompileResources *resources = NULL;
        XrCompilerSession *session = NULL;
        OK(xr_compile_resources_new(&unlimited, &resources));
        CHECK(xr_compile_session_new(resources, &session) == XR_COMPILER_SESSION_OK);
        XrCompileState *state = xr_compile_session_compile_state(session);
        XrArena arenas[2] = {{0}, {0}};
        XrArenaBacking backing;
        CHECK(xr_compiler_arena_state_backing(state, &backing) == XR_ARENA_OK);
        for (unsigned i = 0; i < 2; ++i) CHECK(xr_arena_open(&arenas[i], 128, &backing) == XR_ARENA_OK);
        XrCompilerSessionScope outer = {0}, inner = {0};
        OK(xr_compile_session_push_arena(session, &arenas[0], &outer));
        struct XrCompileStringPool *pool = xr_compile_session_string_pool(session);
        OK(xr_compile_session_push_arena(session, &arenas[1], &inner));
        if (mode == 0) OK(xr_compile_session_pop_arena(&inner));
        else if (mode == 1) {
            CHECK(xr_compile_session_pop_arena(&outer) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
            CHECK(outer.active && inner.active && xr_compile_session_current_arena(session) == &arenas[1]);
            CHECK(xr_compile_session_pop_arena(&inner) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        } else if (mode == 2) {
            XrCompilerSessionScope copy = inner;
            CHECK(xr_compile_session_pop_arena(&copy) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
            CHECK(copy.active && inner.active && xr_compile_session_current_arena(session) == &arenas[1]);
            CHECK(xr_compile_session_pop_arena(&inner) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        } else if (mode == 3) {
            CHECK(xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET) == XR_COMPILE_RESOURCE_BUDGET);
            XrCompilerSessionScope untouched = {0}, expected = untouched;
            CHECK(xr_compile_session_push_arena(session, (XrArena *) (uintptr_t) 1, &untouched) == XR_COMPILE_RESOURCE_BUDGET);
            CHECK(!memcmp(&untouched, &expected, sizeof(expected)));
            CHECK(xr_compile_session_pop_arena(&inner) == XR_COMPILE_RESOURCE_BUDGET);
        } else {
            xr_compile_session_free(session);
            session = NULL;
            CHECK(!inner.active && !outer.active && !inner.session && !outer.session);
            CHECK(xr_compile_state_status(state) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        }
        if (session) {
            CHECK(!inner.active && xr_compile_session_current_arena(session) == &arenas[0]);
            CHECK(xr_compile_session_string_pool(session) == pool);
            CHECK(xr_compile_session_pop_arena(&outer) == xr_compile_state_status(state));
            CHECK(!outer.active && !xr_compile_session_current_arena(session) && !xr_compile_session_string_pool(session));
            CHECK(xr_compile_session_pop_arena(&outer) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
            xr_compile_session_free(session);
        }
        xr_compile_resources_release(resources);
        for (unsigned i = 0; i < 2; ++i) xr_arena_destroy(&arenas[i]);
        CHECK(!physical_live && !allocation_count);
    }
}

static void parser_scope_lifetime(void) {
    for (unsigned mode = 0; mode < 2; ++mode) {
        reset();
        XrCompileResources *resources = NULL;
        XrCompilerSession *session = NULL;
        OK(xr_compile_resources_new(&unlimited, &resources));
        CHECK(xr_compile_session_new(resources, &session) == XR_COMPILER_SESSION_OK);
        XrArena arenas[2] = {{0}, {0}};
        XrArenaBacking backing;
        CHECK(xr_compiler_arena_state_backing(xr_compile_session_compile_state(session), &backing) == XR_ARENA_OK);
        Parser parsers[2] = {{0}, {0}};
        for (unsigned i = 0; i < 2; ++i) {
            CHECK(xr_arena_open(&arenas[i], 128, &backing) == XR_ARENA_OK);
            CHECK(xr_compile_parser_open(&parsers[i], session, "const x = 1", NULL, &arenas[i]) == XR_PARSE_OK);
        }
        if (!mode) {
            xr_compile_parser_close(&parsers[0]);
            CHECK(xr_compile_parser_status(&parsers[0]) == XR_PARSE_BAD_ARGUMENT);
            CHECK(xr_compile_session_current_arena(session) == &arenas[1]);
        } else {
            xr_compile_session_free(session);
            session = NULL;
            AstNode *output = NULL;
            CHECK(xr_compile_parse_recoverable(&parsers[1], &output) == XR_PARSE_BAD_ARGUMENT && !output);
        }
        xr_compile_parser_close(&parsers[1]);
        xr_compile_parser_close(&parsers[0]);
        if (session) CHECK(!xr_compile_session_current_arena(session));
        xr_compile_session_free(session);
        xr_compile_resources_release(resources);
        for (unsigned i = 0; i < 2; ++i) xr_arena_destroy(&arenas[i]);
        CHECK(!physical_live && !allocation_count);
    }
}

static void diagnostic_io(void) {
    reset();
    XrCompileResources *resources = NULL;
    XrCompilerSession *session = NULL;
    OK(xr_compile_resources_new(&unlimited, &resources));
    CHECK(xr_compile_session_new(resources, &session) == XR_COMPILER_SESSION_OK);
    int original = _dup(_fileno(stderr));
    CHECK(original >= 0);
    FILE *readonly = fopen("NUL", "r");
    CHECK(readonly && fflush(stderr) == 0 && _dup2(_fileno(readonly), _fileno(stderr)) == 0);
    AstNode *output = NULL;
    XrParseStatus status = xr_compile_parse(session, "const value =", &output);
    CHECK(_dup2(original, _fileno(stderr)) == 0);
    clearerr(stderr);
    CHECK(_close(original) == 0 && fclose(readonly) == 0);
    CHECK(status == XR_PARSE_IO && !output);
    OK(xr_compile_session_resource_status(session));
    CHECK(xr_compile_parse(session, "const valid = 1", &output) == XR_PARSE_OK);
    xr_program_destroy(output);
    xr_compile_session_free(session);
    xr_compile_resources_release(resources);
    CHECK(!physical_live && !allocation_count);
}

static XrParseStatus fixture(const char *text, const XrCompileResourceLimits *limits,
                           XrCompileResourceStats *observed) {
    XrCompileResources *resources = NULL;
    XrCompilerSession *session = NULL;
    AstNode *program = NULL;
    XrCompileResourceStatus created = xr_compile_resources_new(limits, &resources);
    if (created != XR_COMPILE_RESOURCE_OK)
        return created == XR_COMPILE_RESOURCE_BUDGET ? XR_PARSE_BUDGET : XR_PARSE_OUT_OF_MEMORY;
    XrCompilerSessionStatus opened = xr_compile_session_new(resources, &session);
    XrParseStatus status = opened == XR_COMPILER_SESSION_BUDGET ? XR_PARSE_BUDGET : XR_PARSE_OUT_OF_MEMORY;
    if (opened == XR_COMPILER_SESSION_OK) status = xr_compile_parse_with_trivia(session, text, "fixture.xr", &program);
    CHECK((status == XR_PARSE_OK) == (program != NULL));
    OK(xr_compile_resources_stats(resources, observed));
    if (program) xr_program_destroy(program);
    xr_compile_session_free(session);
    xr_compile_resources_release(resources);
    CHECK(!physical_live && !allocation_count);
    return status;
}

static void syntax_boundaries(void) {
    static const char *const fixtures[] = {
        "@test(timeout: 0x20)\nfn test_value() { assert(true) }\n",
        "struct Word align(0x10) {\n  @deprecated(\"use rotateLeft\")\n  rotate(n: i64) -> u32 { return 0 }\n}\n",
        "fn id<T>(value: T) -> T { return value }\nconst n = id<i64>(42)\n",
        "const fraction = 0.125\nconst code = '\\u{41}'\nconst pattern = /a+/i\n"
    };
    for (size_t index = 0; index < sizeof(fixtures) / sizeof(*fixtures); ++index) {
        reset(); XrCompileResourceStats exact = {0}, observed = {0};
        CHECK(fixture(fixtures[index], &unlimited, &exact) == XR_PARSE_OK);
        printf("fixture %zu: allocations=%zu work=%llu\n", index, attempt_count, (unsigned long long) exact.work);
        size_t points = attempt_count;
        for (size_t point = 0; point < points; ++point) {
            reset(); fail_at = point;
            CHECK(fixture(fixtures[index], &unlimited, &observed) == XR_PARSE_OUT_OF_MEMORY);
            CHECK(attempt_count == point + 1);
        }
        for (uint64_t work = 0; work < exact.work; ++work) {
            reset(); observed = (XrCompileResourceStats) {0};
            XrCompileResourceLimits limited = unlimited;
            limited.work = work;
            CHECK(fixture(fixtures[index], &limited, &observed) == XR_PARSE_BUDGET);
            CHECK(observed.work <= work);
        }
    }
}

static void growth_and_arguments(void) {
    char text[32768];
    size_t used = 0;
    for (unsigned i = 0; i < 1000; ++i) {
        int written = snprintf(text + used, sizeof(text) - used, "const value_%u = %u\n", i, i);
        CHECK(written > 0 && (size_t) written < sizeof(text) - used);
        used += (size_t) written;
    }
    reset(); XrCompileResourceStats exact = {0}, observed = {0};
    CHECK(fixture(text, &unlimited, &exact) == XR_PARSE_OK);
    CHECK(exact.peak_bytes > XR_ARENA_SEGMENT_SIZE * 3);
    size_t points = attempt_count;
    printf("growing AST: allocations=%zu bytes=%llu work=%llu\n", points,
        (unsigned long long) exact.allocated_bytes, (unsigned long long) exact.work);
    for (size_t i = 0; i < points; ++i) {
        reset(); fail_at = i;
        CHECK(fixture(text, &unlimited, &observed) == XR_PARSE_OUT_OF_MEMORY);
        CHECK(attempt_count == i + 1);
    }
    reset();
    XrCompileResources *resources = NULL;
    XrCompilerSession *session = NULL;
    OK(xr_compile_resources_new(&unlimited, &resources));
    XrCompilerSession *sentinel = (XrCompilerSession *) (uintptr_t) 1;
    CHECK(xr_compile_session_new(resources, &sentinel) == XR_COMPILER_SESSION_BAD_ARGUMENT);
    CHECK(sentinel == (XrCompilerSession *) (uintptr_t) 1 && attempt_count == 1);
    CHECK(xr_compile_session_new(resources, &session) == XR_COMPILER_SESSION_OK);
    AstNode *output = (AstNode *) (uintptr_t) 1;
    size_t attempts = attempt_count;
    CHECK(xr_compile_parse(session, "const value = 1", &output) == XR_PARSE_BAD_ARGUMENT);
    CHECK(output == (AstNode *) (uintptr_t) 1 && attempt_count == attempts);
    XrCompileState *state = xr_compile_session_compile_state(session);
    XrCompileResourceStats before;
    OK(xr_compile_resources_stats(resources, &before));
    CHECK(xr_compile_public_attribute_by_name(state, "test", 4) == xr_public_attribute_by_kind(ATTR_TEST));
    OK(xr_compile_resources_stats(resources, &observed));
    CHECK(observed.work == before.work + 10); /* one entry, five spelling and four input reads */
    output = NULL;
    CHECK(xr_compile_parse(session, "@test(timeout: 0x20)\nfn check() {}", &output) == XR_PARSE_OK);
    CHECK(output->as.program.statements[0]->as.function_decl.attributes[0]->timeout == 32);
    xr_program_destroy(output);
    xr_compile_session_free(session);
    xr_compile_resources_release(resources);
    CHECK(!physical_live && !allocation_count);
}

int main(void) {
    faults();
    recoverable_and_rollback();
    scopes();
    parser_scope_lifetime();
    diagnostic_io();
    syntax_boundaries();
    growth_and_arguments();
    puts("production parser: typed failures, rollback, actual arena lifetime and physical zero passed");
    return 0;
}
