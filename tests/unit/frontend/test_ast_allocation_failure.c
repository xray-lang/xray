/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_ast_allocation_failure.c - Observe the real fatal AST allocation boundary
 *
 * KEY CONCEPT:
 *   An isolated process must terminate before allocation failure can publish syntax.
 */
#include "base/xarena.h"
#include "toolchain/xcompiler_session.h"
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static bool ast_allocation_failure;
static void *ast_failure_allocate(XrArena *arena, size_t size) {
    return ast_allocation_failure ? NULL : xr_arena_alloc(arena,size);
}
#define xr_arena_alloc ast_failure_allocate
#include "frontend/parser/xast.c"
#undef xr_arena_alloc
static void ast_expected_abort(int signal_number) {
    (void)signal_number;
    _Exit(73);
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    XrCompilerSession *session = xr_compiler_session_new(NULL);
    if (!session) return 3;
    XrArena arena; xr_arena_init(&arena,4096);
    xr_compiler_session_set_current_arena(session,&arena);
    if (signal(SIGABRT,ast_expected_abort) == SIG_ERR) return 4;
    if (!strcmp(argv[1],"oom")) {
        ast_allocation_failure = true;
        (void)ast_alloc(session,16);
    } else if (!strcmp(argv[1],"overflow")) {
        (void)ast_alloc_array(session,SIZE_MAX/2+1,2);
    } else return 5;
    return 6;
}
