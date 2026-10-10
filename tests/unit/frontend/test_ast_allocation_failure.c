/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_ast_allocation_failure.c - Exact typed AST allocation failure boundary
 *
 * Actual allocator failure and size overflow preserve NULL publication, the
 * first State failure, same-owner retry and complete physical release.
 */
#include "base/xarena.h"
#include "base/xcompile_state.h"
#include "toolchain/xcompiler_session.h"
#include "toolchain/xcompiler_arena_backing.h"
#include "frontend/parser/xparse.h"
#include "frontend/parser/xparse_internal.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_COMPILE_RESOURCE_OK)
#include "../base/parser_resources_observer.h"

static void check_stats(const XrCompileResources *resources, XrCompileResourceStats *stats) {
    OK(xr_compile_resources_stats(resources, stats));
    CHECK(stats->allocated_bytes == physical_total && stats->live_bytes == physical_live);
    CHECK(stats->peak_bytes == physical_peak);
}

static void check_sticky_retry(XrCompilerSession *session, XrArena *arena,
                                XrCompileResourceStatus expected) {
    XrCompileState *state = xr_compile_session_compile_state(session);
    XrCompileResources *resources = xr_compile_state_resources(state);
    XrCompileResourceStats before = {0}, after = {0};
    check_stats(resources, &before);
    size_t attempts = attempt_count;
    CHECK(xr_compile_state_status(state) == expected);
    CHECK(xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT) == expected);
    CHECK(xr_compile_state_fail(state, expected == XR_COMPILE_RESOURCE_BUDGET ?
        XR_COMPILE_RESOURCE_OUT_OF_MEMORY : XR_COMPILE_RESOURCE_BUDGET) == expected);
    CHECK(ast_alloc(session, 16) == NULL);
    CHECK(ast_alloc_array(session, 0, 1) == NULL);
    CHECK(xr_ast_literal_int(session, 42, 1) == NULL);
    CHECK(xr_ast_program(session) == NULL);
    XrParseStatus parsed = expected == XR_COMPILE_RESOURCE_BUDGET ? XR_PARSE_BUDGET : XR_PARSE_OUT_OF_MEMORY;
    AstNode *output = NULL;
    CHECK(xr_compile_parse(session, (const char *) (uintptr_t) 1, &output) == parsed);
    CHECK(!output);
    Parser parser = {0};
    CHECK(xr_compile_parser_open(&parser, session, (const char *) (uintptr_t) 1, NULL, arena) == parsed);
    CHECK(!parser.owner && !parser.state && !parser.compiler_session && !parser.arena);
    CHECK(xr_compile_state_status(state) == expected && attempt_count == attempts);
    check_stats(resources, &after);
    CHECK(after.allocation_count == before.allocation_count && after.allocated_bytes == before.allocated_bytes);
    CHECK(after.live_bytes == before.live_bytes && after.peak_bytes == before.peak_bytes && after.work == before.work);
}

static void ast_failure_case(bool oom) {
    /* This independent operation has one fixed finite ledger, including setup. */
    const XrCompileResourceLimits limits = {1024 * 1024, 512 * 1024, 1024 * 1024};
    reset();
    XrCompileResources *resources = NULL;
    XrCompilerSession *session = NULL;
    OK(xr_compile_resources_new(&limits, &resources));
    XrCompileResourceStats baseline = {0}, before = {0}, failed = {0}, released = {0};
    check_stats(resources, &baseline);
    CHECK(xr_compile_session_new(resources, &session) == XR_COMPILER_SESSION_OK);
    XrCompileState *state = xr_compile_session_compile_state(session);
    CHECK(xr_compile_state_resources(state) == resources);
    XrArena arena = {0};
    XrArenaBacking backing;
    CHECK(xr_compiler_arena_state_backing(state, &backing) == XR_ARENA_OK);
    CHECK(xr_arena_open(&arena, 4096, &backing) == XR_ARENA_OK);
    CHECK(xr_compiler_arena_matches_state(&arena, state));
    XrCompilerSessionScope scope = {0};
    OK(xr_compile_session_push_arena(session, &arena, &scope));
    CHECK(xr_compile_session_current_arena(session) == &arena && scope.active);
    unsigned char *normal = ast_alloc(session, 16);
    CHECK(normal);
    for (unsigned i = 0; i < 16; ++i) CHECK(normal[i] == 0);
    AstNode *literal = xr_ast_literal_int(session, 42, 1);
    CHECK(literal && literal->type == AST_LITERAL_INT && literal->as.literal.raw_value.int_val == 42);
    if (oom) {
        /* Consume actual remaining capacity; the original 16-byte request must
         * now grow storage and reach the shared real malloc fault injector. */
        size_t remaining = (size_t) (arena.limit - arena.position);
        CHECK(remaining && ast_alloc(session, remaining));
        CHECK(arena.position == arena.limit);
    }
    check_stats(resources, &before);
    size_t attempts = attempt_count;
    if (oom) fail_at = attempts;
    void *allocation = oom ? ast_alloc(session, 16) : ast_alloc_array(session, SIZE_MAX / 2 + 1, 2);
    XrCompileResourceStatus expected = oom ? XR_COMPILE_RESOURCE_OUT_OF_MEMORY : XR_COMPILE_RESOURCE_BUDGET;
    CHECK(!allocation && xr_compile_session_resource_status(session) == expected);
    CHECK(xr_compile_session_compile_state(session) == state && xr_compiler_arena_matches_state(&arena, state));
    CHECK(attempt_count == attempts + (oom ? 1 : 0));
    CHECK(xr_arena_status(&arena) == (oom ? XR_ARENA_OUT_OF_MEMORY : XR_ARENA_OK));
    check_stats(resources, &failed);
    CHECK(failed.allocation_count == before.allocation_count && failed.allocated_bytes == before.allocated_bytes);
    CHECK(failed.live_bytes == before.live_bytes && failed.peak_bytes == before.peak_bytes);
    CHECK(failed.work == before.work + (oom ? 1 : 0));
    check_sticky_retry(session, &arena, expected);
    CHECK(xr_compile_session_pop_arena(&scope) == expected);
    CHECK(!scope.active && !xr_compile_session_current_arena(session) && !xr_compile_session_string_pool(session));
    CHECK(xr_compile_state_status(state) == expected);
    xr_arena_destroy(&arena);
    CHECK(!arena.retained && !arena.head);
    xr_compile_session_free(session);
    check_stats(resources, &released);
    CHECK(released.live_bytes == baseline.live_bytes && allocation_count == 1);
    CHECK(released.allocated_bytes == failed.allocated_bytes && released.work == failed.work);
    printf("AST root: allocated=%llu peak=%llu work=%llu attempts=%zu\n",
        (unsigned long long) released.allocated_bytes, (unsigned long long) released.peak_bytes,
        (unsigned long long) released.work, attempt_count);
    xr_compile_resources_release(resources);
    CHECK(!physical_live && !allocation_count);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "oom")) {
        ast_failure_case(true);
        puts("AST oom: OUT_OF_MEMORY NULL sticky no-publication physical-zero");
    } else if (!strcmp(argv[1], "overflow")) {
        ast_failure_case(false);
        puts("AST overflow: BUDGET NULL sticky no-publication physical-zero");
    } else return 5;
    return 0;
}
