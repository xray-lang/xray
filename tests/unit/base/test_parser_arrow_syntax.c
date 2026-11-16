/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_parser_arrow_syntax.c - Original-byte syntax roles and resource failure ownership
 *
 * KEY CONCEPT:
 *   Tooling copies offsets synchronously; rejected allocations publish no AST.
 */
#include "base/xmalloc.h"
#include "toolchain/xcompiler_session.h"
#include "toolchain/xcompiler_arena_backing.h"
#include "frontend/parser/xparse.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_COMPILE_RESOURCE_OK)
#include "parser_resources_observer.h"

static const XrCompileResourceLimits unlimited = {XR_COMPILE_MEMORY_ORDINARY, 0, true};
static const char source[] =
    "fn wrap(action: fn(i64) -> i64) -> fn(i64) -> i64 { return x => action(x) }\n"
    "struct Box { apply(action: fn(i64) -> i64) -> i64 { return action(1) } }\n"
    "const text = r\"prefix ${((n: i64) => match (n) { 0 => 1, _ => n })(1)}\"\n"
    "const multi = \"\"\"\r\n"
    "    margin ${((n: i64) => n + 1)(1)}\r\n"
    "    \"\"\"\n"
    "const nested = \"${r\"inside ${((n: i64) => n)(1)}\"}\"\n"
    "const ignored = r\"ordinary text -> and =>\"\n";
static const char rejected[] =
    "const text = \"\"\"\r\n"
    "    prefix ${r\"nested ${((n: i64) -> n)(1)}\"}\r\n"
    "    \"\"\"\n";

typedef struct Observations {
    unsigned char roles[sizeof(source)];
    size_t types, values, arm_end;
    unsigned errors;
} Observations;

static void original_error(void *data, int line, int column, int end_line,
                           int end_column, const char *message) {
    Observations *facts = data;
    CHECK(line == 2 && column == 35 && end_line == 2 && end_column == 37);
    CHECK(strstr(message, "use `=>` for lambdas, match arms and select arms"));
    CHECK(strstr(message, "replace `->` with `=>`"));
    CHECK(++facts->errors == 1);
}

static void arrow(void *data, const Token *token, bool value) {
    Observations *facts = data;
    CHECK((uintptr_t)token->start >= (uintptr_t)source && token->length > 0 &&
          (uintptr_t)token->start <= (uintptr_t)(source + sizeof(source)) - (size_t)token->length);
    CHECK(token->length == 2 && !memcmp(token->start, value ? "=>" : "->", 2));
    size_t offset = (size_t)(token->start - source);
    unsigned char role = value ? 1 : 2;
    CHECK(!facts->roles[offset] || facts->roles[offset] == role);
    if (!facts->roles[offset]) {
        facts->roles[offset] = role;
        if (value) ++facts->values;
        else ++facts->types;
    }
}

static void arm_end(void *data, const Token *token) {
    Observations *facts = data;
    const char *expected = strstr(source, "_ => n }") + 5;
    CHECK(token->start == expected && token->length == 1);
    facts->arm_end = (size_t)(expected - source) + 1;
}

static XrParseStatus resource_status(XrCompileResourceStatus status) {
    CHECK(status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY || status == XR_COMPILE_RESOURCE_MEMORY_LIMIT);
    return status == XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_PARSE_MEMORY_LIMIT : XR_PARSE_OUT_OF_MEMORY;
}

static XrParseStatus pipeline(const XrCompileResourceLimits *limits, XrCompileResourceStats *stats,
                             unsigned malformed) {
    XrCompileResources *resources = NULL;
    XrCompilerSession *session = NULL;
    XrArena arena = {0};
    Parser parser = {0};
    AstNode *program = NULL;
    Observations facts = {0};
    XrCompileResourceStatus made = xr_compile_resources_new(limits, &resources);
    if (made != XR_COMPILE_RESOURCE_OK) return resource_status(made);
    made = xr_compile_session_new(resources, &session);
    XrParseStatus status = XR_PARSE_OK;
    if (made != XR_COMPILE_RESOURCE_OK) { status = resource_status(made); goto done; }
    XrArenaBacking backing;
    CHECK(xr_compiler_arena_state_backing(xr_compile_session_compile_state(session), &backing) == XR_ARENA_OK);
    if (xr_arena_open(&arena, 128, &backing) != XR_ARENA_OK) {
        status = resource_status(xr_compile_session_resource_status(session));
        goto done;
    }
    status = xr_compile_parser_open(&parser, session, malformed ? rejected : source, "arrow-offsets.xr", &arena);
    if (status != XR_PARSE_OK) goto done;
    if (malformed) xr_parser_set_error_callback(&parser, original_error, &facts, 0);
    else {
        status = xr_parser_scan_arrow_syntax(&parser, arrow, &facts);
        if (status != XR_PARSE_OK) goto done;
        CHECK(facts.types == 5 && facts.values == 1);
        facts = (Observations){0};
        xr_parser_set_arrow_syntax_callback(&parser, arrow, &facts);
        xr_parser_set_match_arm_syntax_callback(&parser, arm_end, &facts);
    }
    status = xr_compile_parse_recoverable(&parser, &program);
    if (status == XR_PARSE_OK) {
        CHECK(program && facts.types == 5 && facts.values == 6 && facts.arm_end);
        const char *ignored = strstr(source, "ordinary text -> and =>");
        CHECK(!facts.roles[(size_t)(strstr(ignored, "->") - source)]);
        CHECK(!facts.roles[(size_t)(strstr(ignored, "=>") - source)]);
    } else if (status == XR_PARSE_RECOVERED) CHECK(malformed && program && facts.errors == 1);
    else CHECK(!program);
done:
    xr_compile_parser_close(&parser);
    OK(xr_compile_resources_stats(resources, stats));
    CHECK(stats->allocated_bytes == physical_total && stats->peak_bytes == physical_peak &&
          stats->held_bytes == physical_live);
    xr_compile_session_free(session);
    xr_compile_resources_release(resources);
    xr_arena_destroy(&arena);
    CHECK(!physical_live && !allocation_count);
    if (status == XR_PARSE_OK) CHECK(facts.types == 5 && facts.values == 6 && facts.arm_end);
    return status;
}

int main(void) {
    for (unsigned malformed = 0; malformed < 2; ++malformed) {
        reset();
        XrCompileResourceStats exact = {0}, observed = {0};
        XrParseStatus expected = malformed ? XR_PARSE_RECOVERED : XR_PARSE_OK;
        CHECK(pipeline(&unlimited, &exact, malformed) == expected);
        size_t sites = attempt_count;
        CHECK(sites > 0 && exact.peak_bytes > 0);
        for (size_t point = 0; point < sites; ++point) {
            reset(); fail_at = point;
            CHECK(pipeline(&unlimited, &observed, malformed) == XR_PARSE_OUT_OF_MEMORY);
            CHECK(attempt_count == point + 1);
        }
        XrCompileResourceLimits finite = {XR_COMPILE_MEMORY_FINITE, exact.peak_bytes, true};
        reset(); CHECK(pipeline(&finite, &observed, malformed) == expected);
        --finite.memory_limit;
        reset(); CHECK(pipeline(&finite, &observed, malformed) == XR_PARSE_MEMORY_LIMIT);
    }
    puts("arrow source offsets, nested/raw/block interpolation and every allocation failure passed");
    return 0;
}
