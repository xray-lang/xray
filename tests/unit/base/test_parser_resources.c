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
#include "parser_resources_observer.h"

static const XrCompileResourceLimits unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
static const char source[] = "// retained trivia\ntype Thing = { value: i64 }\nconst answer = 42\nconst text = \"value ${answer}\"\n";
static const char nul_source[] =
    "const escaped = \"a\\0b\"\n"
    "const unicode = \"a\\u{0}b\"\n"
    "const template = \"a\\u{0}b${1}c\\0d\"\n";

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
    XrParseStatus parsed = xr_compile_parse_with_trivia(session, source, "resource-owner.xr", NULL, &program);
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
    if (opened == XR_COMPILER_SESSION_OK) status = xr_compile_parse_with_trivia(session, text, "fixture.xr", NULL, &program);
    CHECK((status == XR_PARSE_OK) == (program != NULL));
    OK(xr_compile_resources_stats(resources, observed));
    if (program) xr_program_destroy(program);
    xr_compile_session_free(session);
    xr_compile_resources_release(resources);
    CHECK(!physical_live && !allocation_count);
    return status;
}

static void check_nul_literals(const AstNode *program) {
    CHECK(program && program->type == AST_PROGRAM && program->as.program.count == 3);
    for (unsigned i = 0; i < 2; ++i) {
        const AstNode *literal = program->as.program.statements[i]->as.var_decl.initializer;
        CHECK(literal && literal->type == AST_LITERAL_STRING);
        CHECK(literal->as.literal.string_length == 3);
        CHECK(!memcmp(literal->as.literal.raw_value.string_val, "a\0b", 3));
    }
    const AstNode *template = program->as.program.statements[2]->as.var_decl.initializer;
    CHECK(template && template->type == AST_TEMPLATE_STRING && template->as.template_str.part_count == 3);
    const AstNode *first = template->as.template_str.parts[0], *last = template->as.template_str.parts[2];
    CHECK(first->type == AST_LITERAL_STRING && last->type == AST_LITERAL_STRING);
    CHECK(first->as.literal.string_length == 3 && last->as.literal.string_length == 3);
    CHECK(!memcmp(first->as.literal.raw_value.string_val, "a\0b", 3));
    CHECK(!memcmp(last->as.literal.raw_value.string_val, "c\0d", 3));
    CHECK(template->as.template_str.parts[1]->type == AST_LITERAL_INT);
}

static void nul_literal_lifetime(void) {
    reset();
    XrCompileResources *resources = NULL;
    XrCompilerSession *session = NULL;
    AstNode *program = NULL;
    char input[sizeof(nul_source)];
    memcpy(input, nul_source, sizeof(input));
    OK(xr_compile_resources_new(&unlimited, &resources));
    CHECK(xr_compile_session_new(resources, &session) == XR_COMPILER_SESSION_OK);
    CHECK(xr_compile_parse(session, input, &program) == XR_PARSE_OK);
    check_nul_literals(program);
    memset(input, '?', sizeof(input));
    xr_compile_session_free(session);
    xr_compile_resources_release(resources);
    check_nul_literals(program);
    xr_program_destroy(program);
    CHECK(!physical_live && !allocation_count);

    static const char *const rejected[] = {
        "const invalid = \"a\\xFFb\"\n",
        "const invalid = \"a\\xFF${1}b\"\n",
        "const invalid = c\"a\\u{0}b\"\n",
        "const invalid = c\"a\\0b\"\n",
        "const invalid = {\"a\\u{0}b\": 1}\n",
        "import { value } from \"./a\\u{0}b\"\n",
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(*rejected); ++i) {
        reset(); XrCompileResourceStats observed = {0};
        CHECK(fixture(rejected[i], &unlimited, &observed) == XR_PARSE_SYNTAX);
    }
    reset(); XrCompileResourceStats observed = {0};
    CHECK(fixture("const raw_c = cr\"a\\0b\"\n", &unlimited, &observed) == XR_PARSE_OK);
    puts("NUL plain/template AST bytes survive source, Session and producer destruction; UTF8/C/field/import boundaries PASS");
}

static void syntax_boundaries(void) {
    static const char *const fixtures[] = {
        "@test(timeout: 0x20)\nfn test_value() { assert(true) }\n",
        "struct Word align(0x10) {\n  @deprecated(\"use rotateLeft\")\n  rotate(n: i64) -> u32 { return 0 }\n}\n",
        "fn id<T>(value: T) -> T { return value }\nconst n = id<i64>(42)\n",
        "const fraction = 0.125\nconst code = '\\u{41}'\nconst pattern = /a+/i\n",
        nul_source
    };
    for (size_t index = 0; index < sizeof(fixtures) / sizeof(*fixtures); ++index) {
        reset(); XrCompileResourceStats exact = {0}, observed = {0};
        CHECK(fixture(fixtures[index], &unlimited, &exact) == XR_PARSE_OK);
        printf("fixture %zu: allocations=%zu work=%llu\n", index, attempt_count, (unsigned long long) exact.work);
        size_t points = attempt_count;
        if (fixtures[index] == nul_source) {
            XrCompileResourceLimits limits = {exact.allocated_bytes, exact.peak_bytes, exact.work};
            reset(); CHECK(fixture(nul_source, &limits, &observed) == XR_PARSE_OK);
            for (unsigned axis = 0; axis < 3; ++axis) {
                XrCompileResourceLimits smaller = limits;
                if (!axis) --smaller.allocated_bytes;
                else if (axis == 1) --smaller.live_bytes;
                else --smaller.work;
                reset(); CHECK(fixture(nul_source, &smaller, &observed) == XR_PARSE_BUDGET);
            }
        }
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


typedef struct TriviaDiagnostics { unsigned count; char last[512]; } TriviaDiagnostics;
static void trivia_diagnostic(void *data,int line,int column,int end_line,int end_column,const char *message) {
    TriviaDiagnostics *capture=data;
    CHECK(line>0 && column>=0 && end_line>=line && end_column>=0 && message && *message);
    ++capture->count; snprintf(capture->last,sizeof(capture->last),"%s",message);
}
static void owning_trivia_diagnostics(void) {
    for(int maximum=0;maximum<3;++maximum) {
        reset(); XrCompileResources *resources=NULL; XrCompilerSession *session=NULL; AstNode *out=NULL;
        OK(xr_compile_resources_new(&unlimited,&resources));
        CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
        TriviaDiagnostics capture={0}; XrParseDiagnostics diagnostics={trivia_diagnostic,&capture,maximum};
        CHECK(xr_compile_parse_with_trivia(session,"const first =\nconst second =\nconst third =\n","bad.xr",&diagnostics,&out)==XR_PARSE_SYNTAX);
        CHECK(!out && capture.count>0 && capture.last[0]);
        if(maximum) CHECK(capture.count<=(unsigned)maximum);
        XrCompileResourceStats before,after; OK(xr_compile_resources_stats(resources,&before));
        diagnostics.max_errors=-1;
        CHECK(xr_compile_parse_with_trivia(session,"const good=1","good.xr",&diagnostics,&out)==XR_PARSE_BAD_ARGUMENT);
        OK(xr_compile_resources_stats(resources,&after)); CHECK(after.work==before.work && !out);
        out=(AstNode *)(uintptr_t)1; diagnostics.max_errors=1;
        CHECK(xr_compile_parse_with_trivia(session,"bad","bad.xr",&diagnostics,&out)==XR_PARSE_BAD_ARGUMENT);
        CHECK(out==(AstNode *)(uintptr_t)1); out=NULL;
        fail_at=attempt_count;
        CHECK(xr_compile_parse_with_trivia(session,"const good=1","good.xr",&diagnostics,&out)==XR_PARSE_OUT_OF_MEMORY);
        CHECK(!out);
        xr_compile_session_free(session); xr_compile_resources_release(resources); CHECK(!physical_live);
    }
    puts("owned trivia: bounded/unlimited callback, invalid count, failure output and physical zero PASS");
}


static XrParseStatus position_pipeline(const XrCompileResourceLimits *limits,
    XrCompileResourceStats *stats, unsigned recovered) {
    static const char *const texts[]={
        "fn recover()->i64 {\n if (true) {\n  return 7\n }\n return 0\n}\n",
        "var *** = ;\nfn recover()->i64 {\n if (true) {\n  return 7\n }\n return 0\n}\n"};
    XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;
    XrArena arena={0};Parser parser={0};AstNode *program=NULL;
    XrCompileResourceStatus made=xr_compile_resources_new(limits,&resources);
    if(made!=XR_COMPILE_RESOURCE_OK)
        return made==XR_COMPILE_RESOURCE_BUDGET?XR_PARSE_BUDGET:XR_PARSE_OUT_OF_MEMORY;
    XrCompilerSessionStatus opened=xr_compile_session_new(resources,&session);
    XrParseStatus status=opened==XR_COMPILER_SESSION_OK?XR_PARSE_OK:
        opened==XR_COMPILER_SESSION_BUDGET?XR_PARSE_BUDGET:XR_PARSE_OUT_OF_MEMORY;
    TriviaDiagnostics diagnostics={0};
    if(status==XR_PARSE_OK) {
        XrArenaBacking backing;
        CHECK(xr_compiler_arena_state_backing(xr_compile_session_compile_state(session),&backing)==XR_ARENA_OK);
        XrArenaStatus arena_status=xr_arena_open(&arena,128,&backing);
        status=arena_status==XR_ARENA_OK?XR_PARSE_OK:
            arena_status==XR_ARENA_BUDGET?XR_PARSE_BUDGET:XR_PARSE_OUT_OF_MEMORY;
    }
    if(status==XR_PARSE_OK) status=xr_compile_parser_open(&parser,session,texts[recovered],"positions.xr",&arena);
    if(status==XR_PARSE_OK) {
        xr_parser_set_error_callback(&parser,trivia_diagnostic,&diagnostics,100);
        status=xr_compile_parse_recoverable(&parser,&program);
    }
    if(status==XR_PARSE_OK||status==XR_PARSE_RECOVERED) {
        CHECK(status==(recovered?XR_PARSE_RECOVERED:XR_PARSE_OK));
        CHECK(!!diagnostics.count==!!recovered && !!parser.had_error==!!recovered);
        CHECK(program&&program->type==AST_PROGRAM);
        const AstNode *function=NULL;
        for(int i=0;i<program->as.program.count;++i) {
            const AstNode *node=program->as.program.statements[i];
            if(node&&node->type==AST_FUNCTION_DECL&&!strcmp(node->as.function_decl.name,"recover")) {
                CHECK(!function);function=node;
            }
        }
        CHECK(function&&function->line==(int)recovered+1&&function->end_line==(int)recovered+6);
        const AstNode *body=function->as.function_decl.body;
        CHECK(body&&body->type==AST_BLOCK&&body->as.block.count==2);
        const AstNode *branch=body->as.block.statements[0];
        CHECK(branch&&branch->type==AST_IF_STMT&&branch->line==(int)recovered+2);
        const AstNode *nested=branch->as.if_stmt.then_branch;
        CHECK(nested&&nested->type==AST_BLOCK&&nested->as.block.count==1);
        CHECK(nested->end_line==(int)recovered+4&&nested->end_column==3);
        CHECK(nested->as.block.statements[0]->type==AST_RETURN_STMT);
        CHECK(body->as.block.statements[1]->type==AST_RETURN_STMT);
    } else CHECK((status==XR_PARSE_BUDGET||status==XR_PARSE_OUT_OF_MEMORY)&&!program);
    xr_compile_parser_close(&parser);
    OK(xr_compile_resources_stats(resources,stats));
    CHECK(stats->allocated_bytes==physical_total&&stats->peak_bytes==physical_peak&&stats->live_bytes==physical_live);
    xr_compile_session_free(session);xr_compile_resources_release(resources);xr_arena_destroy(&arena);
    CHECK(!physical_live&&!allocation_count);return status;
}
static void recovered_positions(void) {
    const XrCompileResourceLimits finite={UINT64_C(1048576),UINT64_C(1048576),UINT64_C(1048576)};
    for(unsigned recovered=0;recovered<2;++recovered) {
        reset();XrCompileResourceStats exact={0},stats={0};
        XrParseStatus expected=recovered?XR_PARSE_RECOVERED:XR_PARSE_OK;
        CHECK(position_pipeline(&finite,&exact,recovered)==expected);
        size_t sites=attempt_count;
        CHECK(sites&&exact.allocated_bytes&&exact.peak_bytes&&exact.work);
        for(size_t point=0;point<sites;++point) {
            reset();fail_at=point;
            CHECK(position_pipeline(&finite,&stats,recovered)==XR_PARSE_OUT_OF_MEMORY);
            CHECK(attempt_count==point+1);
        }
        XrCompileResourceLimits limits={exact.allocated_bytes,exact.peak_bytes,exact.work};
        reset();CHECK(position_pipeline(&limits,&stats,recovered)==expected);
        for(unsigned axis=0;axis<3;++axis) {
            XrCompileResourceLimits smaller=limits;
            if(!axis)--smaller.allocated_bytes;else if(axis==1)--smaller.live_bytes;else --smaller.work;
            reset();CHECK(position_pipeline(&smaller,&stats,recovered)==XR_PARSE_BUDGET);
        }
        printf("parser positions: recovered=%u allocations=%zu full FI/three axes/physical zero passed\n",recovered,sites);
    }
}

/* Independent byte-column oracles from the literal, including both mutation
 * and value access. The parser must own these facts after every producer dies. */
static const char this_position_source[] =
    "struct S {\n"
    " value:i64\n"
    " ref bump()->i64 {\n"
    "  this.value += 1\n"
    "  return this.value\n"
    " }\n"
    "}\n";
static void this_position_facts(const AstNode *program) {
    CHECK(program && program->type==AST_PROGRAM && program->as.program.count==1);
    const AstNode *owner=program->as.program.statements[0];
    CHECK(owner && owner->type==AST_STRUCT_DECL && owner->as.struct_decl.method_count==1);
    const AstNode *method=owner->as.struct_decl.methods[0];
    CHECK(method && method->type==AST_METHOD_DECL && !strcmp(method->as.method_decl.name,"bump") &&
        method->as.method_decl.receiver_mode==XR_PARAM_REF);
    const AstNode *body=method->as.method_decl.body;
    CHECK(body && body->type==AST_BLOCK && body->as.block.count==2);
    const AstNode *statement=body->as.block.statements[0],*returned=body->as.block.statements[1];
    CHECK(statement && statement->type==AST_EXPR_STMT && statement->as.expr_stmt);
    const AstNode *update=statement->as.expr_stmt;
    CHECK(update->type==AST_COMPOUND_ASSIGNMENT && update->as.compound_assignment.object);
    CHECK(returned && returned->type==AST_RETURN_STMT && returned->as.return_stmt.value_count==1);
    const AstNode *member=returned->as.return_stmt.values[0];
    CHECK(member && member->type==AST_MEMBER_ACCESS && member->as.member_access.object);
    const AstNode *receivers[]={update->as.compound_assignment.object,member->as.member_access.object};
    const int lines[]={4,5},columns[]={3,10},ends[]={7,14};
    for(unsigned i=0;i<2;++i) {
        const AstNode *receiver=receivers[i];
        CHECK(receiver && receiver->type==AST_THIS_EXPR && receiver->line==lines[i]);
        CHECK(receiver->column==columns[i] && receiver->end_line==lines[i] && receiver->end_column==ends[i]);
    }
}
static XrParseStatus this_position_pipeline(const XrCompileResourceLimits *limits,XrCompileResourceStats *stats) {
    XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;AstNode *program=NULL;
    char input[sizeof(this_position_source)];memcpy(input,this_position_source,sizeof(input));
    XrCompileResourceStatus made=xr_compile_resources_new(limits,&resources);
    if(made!=XR_COMPILE_RESOURCE_OK)
        return made==XR_COMPILE_RESOURCE_BUDGET?XR_PARSE_BUDGET:XR_PARSE_OUT_OF_MEMORY;
    XrCompilerSessionStatus opened=xr_compile_session_new(resources,&session);
    XrParseStatus status=opened==XR_COMPILER_SESSION_BUDGET?XR_PARSE_BUDGET:XR_PARSE_OUT_OF_MEMORY;
    if(opened==XR_COMPILER_SESSION_OK) {
        status=xr_compile_parse(session,input,&program);
        if(status==XR_PARSE_OK) {
            CHECK(program && program->as.program.owns_arena);this_position_facts(program);
        } else {
            CHECK((status==XR_PARSE_BUDGET||status==XR_PARSE_OUT_OF_MEMORY)&&!program);
            size_t attempts=attempt_count;
            CHECK(xr_compile_parse(session,(const char *)(uintptr_t)1,&program)==status);
            CHECK(attempt_count==attempts&&!program);
        }
    }
    OK(xr_compile_resources_stats(resources,stats));
    CHECK(stats->allocated_bytes==physical_total&&stats->peak_bytes==physical_peak&&stats->live_bytes==physical_live);
    memset(input,'?',sizeof(input));xr_compile_session_free(session);xr_compile_resources_release(resources);
    if(program){this_position_facts(program);xr_program_destroy(program);}
    CHECK(!physical_live&&!allocation_count);return status;
}
static void this_positions(void) {
    const XrCompileResourceLimits finite={UINT64_C(1048576),UINT64_C(1048576),UINT64_C(1048576)};
    reset();XrCompileResourceStats exact={0},stats={0};
    CHECK(this_position_pipeline(&finite,&exact)==XR_PARSE_OK);
    size_t sites=attempt_count;CHECK(sites&&exact.allocated_bytes&&exact.peak_bytes&&exact.work);
    for(size_t point=0;point<sites;++point) {
        reset();fail_at=point;
        CHECK(this_position_pipeline(&finite,&stats)==XR_PARSE_OUT_OF_MEMORY);
        CHECK(attempt_count==point+1);
    }
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits limits={exact.allocated_bytes,exact.peak_bytes,exact.work};
        uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        CHECK(*bound&&*bound<UINT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
        reset();CHECK(this_position_pipeline(&limits,&stats)==(delta<0?XR_PARSE_BUDGET:XR_PARSE_OK));
    }
    printf("parser this spans: allocations=%zu full FI/three axes/producer death/physical zero passed\n",sites);
}

static const char this_ref_source[] =
    "final class Holder {\n"
    " value:i64\n"
    " constructor(){this.value=0}\n"
    " update()->i64 {\n"
    "  touch(ref this.value)\n"
    "  return this.value\n"
    " }\n"
    "}\n";
static void this_ref_facts(const AstNode *program) {
    CHECK(program && program->type==AST_PROGRAM && program->as.program.count==1);
    const AstNode *owner=program->as.program.statements[0];
    CHECK(owner && owner->type==AST_CLASS_DECL && owner->as.class_decl.method_count==2);
    const AstNode *method=owner->as.class_decl.methods[1];
    CHECK(method && method->type==AST_METHOD_DECL && !strcmp(method->as.method_decl.name,"update") &&
        method->as.method_decl.receiver_mode==XR_PARAM_READ);
    const AstNode *body=method->as.method_decl.body;
    CHECK(body && body->type==AST_BLOCK && body->as.block.count==2);
    const AstNode *statement=body->as.block.statements[0],*returned=body->as.block.statements[1];
    CHECK(statement && statement->type==AST_EXPR_STMT && statement->as.expr_stmt);
    const AstNode *call=statement->as.expr_stmt;
    CHECK(call->type==AST_CALL_EXPR && call->as.call_expr.arg_count==1 &&
        call->as.call_expr.arg_accesses[0]==XR_CALL_ARG_REF);
    const AstNode *field=call->as.call_expr.arguments[0];
    CHECK(field && field->type==AST_MEMBER_ACCESS && !strcmp(field->as.member_access.name,"value"));
    const AstNode *receiver=field->as.member_access.object;
    CHECK(receiver && receiver->type==AST_THIS_EXPR && receiver->line==5 && receiver->column==13 &&
        receiver->end_line==5 && receiver->end_column==17);
    CHECK(receiver->as.this_expr.access_marker_span.line==5 &&
        receiver->as.this_expr.access_marker_span.column==9);
    CHECK(returned && returned->type==AST_RETURN_STMT && returned->as.return_stmt.value_count==1);
    field=returned->as.return_stmt.values[0];
    CHECK(field && field->type==AST_MEMBER_ACCESS && !strcmp(field->as.member_access.name,"value"));
    receiver=field->as.member_access.object;
    CHECK(receiver && receiver->type==AST_THIS_EXPR && receiver->line==6 && receiver->column==10 &&
        receiver->end_line==6 && receiver->end_column==14);
    CHECK(!receiver->as.this_expr.access_marker_span.line && !receiver->as.this_expr.access_marker_span.column);
}
static XrParseStatus this_ref_pipeline(const XrCompileResourceLimits *limits,XrCompileResourceStats *stats) {
    XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;AstNode *program=NULL;
    char input[sizeof(this_ref_source)];memcpy(input,this_ref_source,sizeof(input));
    XrCompileResourceStatus made=xr_compile_resources_new(limits,&resources);
    if(made!=XR_COMPILE_RESOURCE_OK)
        return made==XR_COMPILE_RESOURCE_BUDGET?XR_PARSE_BUDGET:XR_PARSE_OUT_OF_MEMORY;
    XrCompilerSessionStatus opened=xr_compile_session_new(resources,&session);
    XrParseStatus status=opened==XR_COMPILER_SESSION_BUDGET?XR_PARSE_BUDGET:XR_PARSE_OUT_OF_MEMORY;
    if(opened==XR_COMPILER_SESSION_OK) {
        status=xr_compile_parse(session,input,&program);
        if(status==XR_PARSE_OK) {
            CHECK(program && program->as.program.owns_arena);this_ref_facts(program);
        } else {
            CHECK((status==XR_PARSE_BUDGET||status==XR_PARSE_OUT_OF_MEMORY)&&!program);
            size_t attempts=attempt_count;
            CHECK(xr_compile_parse(session,(const char *)(uintptr_t)1,&program)==status);
            CHECK(attempt_count==attempts&&!program);
        }
    }
    OK(xr_compile_resources_stats(resources,stats));
    CHECK(stats->allocated_bytes==physical_total&&stats->peak_bytes==physical_peak&&stats->live_bytes==physical_live);
    memset(input,'?',sizeof(input));xr_compile_session_free(session);xr_compile_resources_release(resources);
    if(program){this_ref_facts(program);xr_program_destroy(program);}
    CHECK(!physical_live&&!allocation_count);return status;
}
static void this_ref_positions(void) {
    const XrCompileResourceLimits finite={UINT64_C(1048576),UINT64_C(1048576),UINT64_C(1048576)};
    reset();XrCompileResourceStats exact={0},stats={0};
    CHECK(this_ref_pipeline(&finite,&exact)==XR_PARSE_OK);
    size_t sites=attempt_count;CHECK(sites&&exact.allocated_bytes&&exact.peak_bytes&&exact.work);
    for(size_t point=0;point<sites;++point) {
        reset();fail_at=point;
        CHECK(this_ref_pipeline(&finite,&stats)==XR_PARSE_OUT_OF_MEMORY);
        CHECK(attempt_count==point+1);
    }
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits limits={exact.allocated_bytes,exact.peak_bytes,exact.work};
        uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        CHECK(*bound&&*bound<UINT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
        reset();CHECK(this_ref_pipeline(&limits,&stats)==(delta<0?XR_PARSE_BUDGET:XR_PARSE_OK));
    }
    printf("parser real ref THIS marker spans: allocations=%zu full FI/three axes/producer death/physical zero passed\n",sites);
}

int main(void) {
    recovered_positions();
    this_positions();
    this_ref_positions();
    owning_trivia_diagnostics();
    faults();
    recoverable_and_rollback();
    scopes();
    parser_scope_lifetime();
    diagnostic_io();
    nul_literal_lifetime();
    syntax_boundaries();
    growth_and_arguments();
    puts("production parser: typed failures, rollback, actual arena lifetime and physical zero passed");
    return 0;
}
