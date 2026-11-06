/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lexer_fixture_owner.h - Keep tokens and trivia within their compiler owner
 */
#ifndef XR_LEXER_FIXTURE_OWNER_H
#define XR_LEXER_FIXTURE_OWNER_H
#include "frontend/lexer/xlex.h"
#include "base/xarena.h"
#include "toolchain/xcompiler_arena_backing.h"
#include <stdio.h>
#include <stdlib.h>

static XrCompileResources *lexer_resources;
static XrCompileState *lexer_state;
static XrArena lexer_arena;
static uint64_t lexer_baseline;

static void lexer_owner_check(bool condition) {
    if (!condition) {
        fprintf(stderr, "Lexer fixture ownership failed\n");
        abort();
    }
}

static void lexer_fixture_open(void) {
    const XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, true};
    XrCompileResourceStats stats = {0};
    XrArenaBacking backing = {0};
    lexer_owner_check(!lexer_resources && !lexer_state);
    lexer_owner_check(xr_compile_resources_new(&limits, &lexer_resources) == XR_COMPILE_RESOURCE_OK);
    lexer_owner_check(xr_compile_resources_stats(lexer_resources, &stats) == XR_COMPILE_RESOURCE_OK);
    lexer_baseline = stats.held_bytes;
    lexer_owner_check(xr_compile_state_new(lexer_resources, &lexer_state) == XR_COMPILE_RESOURCE_OK);
    lexer_owner_check(xr_compiler_arena_state_backing(lexer_state, &backing) == XR_ARENA_OK);
    lexer_owner_check(xr_arena_open(&lexer_arena, XR_ARENA_SEGMENT_SIZE, &backing) == XR_ARENA_OK);
}

/* Every returned token and trivia chain stays readable until fixture close. */
static void lexer_scanner_open(Scanner *scanner, const char *source, bool trivia) {
    lexer_owner_check(xr_compile_scanner_open_source(scanner, lexer_state,
        &lexer_arena, source, trivia, NULL) == XR_COMPILE_RESOURCE_OK);
}

static void lexer_fixture_close(void) {
    XrCompileResourceStats stats = {0};
    lexer_owner_check(xr_compile_state_status(lexer_state) == XR_COMPILE_RESOURCE_OK);
    xr_arena_destroy(&lexer_arena);
    xr_compile_state_release(lexer_state);
    lexer_state = NULL;
    lexer_owner_check(xr_compile_resources_stats(lexer_resources, &stats) == XR_COMPILE_RESOURCE_OK);
    lexer_owner_check(stats.held_bytes == lexer_baseline);
    xr_compile_resources_release(lexer_resources);
    lexer_resources = NULL;
}
#endif
