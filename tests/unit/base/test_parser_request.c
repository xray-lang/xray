/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_parser_request.c - Short parser controls and detached syntax ownership
 *
 * KEY CONCEPT:
 *   Failed controls do not poison the memory owner or survive parser sessions.
 */
#include "base/xmalloc.h"
#include "base/xcompile_request.h"
#include "toolchain/xcompiler_session.h"
#include "frontend/parser/xparse.h"
#include <stdio.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_COMPILE_RESOURCE_OK)

typedef struct Cancel { unsigned calls, after; } Cancel;
static bool cancel_after(const void *context) {
    Cancel *cancel = (Cancel *)context;
    return ++cancel->calls >= cancel->after;
}

static void cancelled_then_fresh(XrCompileResources *memory) {
    /* A long comment exercises actual scanner reads before AST publication. */
    char *text = xr_malloc(32768);
    CHECK(text);
    text[0] = '/'; text[1] = '/';
    memset(text + 2, 'x', 32000);
    memcpy(text + 32002, "\nconst answer = 42\n", 20);
    Cancel cancelled = {0, 2};
    XrCompileRequestOptions options = {cancel_after, &cancelled, XR_COMPILE_DEADLINE_NONE, 0};
    XrCompileRequestControl control = {0};
    OK(xr_compile_request_control_init(&options, &control));
    XrCompilerSession *session = NULL;
    OK(xr_compile_session_new(memory, &session));
    OK(xr_compile_session_bind_request(session, &control));
    AstNode *program = NULL;
    CHECK(xr_compile_parse(session, text, &program) == XR_PARSE_CANCELLED);
    CHECK(!program && cancelled.calls == 2);
    CHECK(xr_compile_session_resource_status(session) == XR_COMPILE_RESOURCE_CANCELLED);
    xr_compile_session_free(session);
    unsigned calls = cancelled.calls;
    xr_free(text);

    XrCompileRequestControl fresh = {0};
    options = (XrCompileRequestOptions){0};
    OK(xr_compile_request_control_init(&options, &fresh));
    session = NULL;
    OK(xr_compile_session_new(memory, &session));
    OK(xr_compile_session_bind_request(session, &fresh));
    CHECK(xr_compile_parse(session, "const answer = 42", &program) == XR_PARSE_OK);
    xr_compile_session_free(session);
    CHECK(program && program->as.program.owns_arena && program->as.program.count == 1);
    CHECK(cancelled.calls == calls);
    /* Neither AST reads nor destruction may borrow either dead request. */
    memset(&fresh, 0xa5, sizeof(fresh));
    memset(&control, 0xa5, sizeof(control));
    CHECK(program->as.program.statements[0]->type == AST_CONST_DECL);
    xr_program_destroy(program);
    CHECK(cancelled.calls == calls);
}

static void deadline(XrCompileResources *memory) {
    XrCompileRequestControl control = {0};
    XrCompileRequestOptions options = {NULL, NULL, XR_COMPILE_DEADLINE_AFTER, 0};
    OK(xr_compile_request_control_init(&options, &control));
    XrCompilerSession *session = NULL;
    OK(xr_compile_session_new(memory, &session));
    OK(xr_compile_session_bind_request(session, &control));
    AstNode *program = NULL;
    CHECK(xr_compile_parse(session, "const answer = 42", &program) == XR_PARSE_DEADLINE);
    CHECK(!program);
    xr_compile_session_free(session);
}

int main(void) {
    const XrCompileResourceLimits options = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrCompileResources *memory = NULL;
    OK(xr_compile_resources_new(&options, &memory));
    XrCompileResourceStats before = {0}, after = {0};
    OK(xr_compile_resources_stats(memory, &before));
    cancelled_then_fresh(memory);
    deadline(memory);
    OK(xr_compile_resources_stats(memory, &after));
    CHECK(after.held_bytes == before.held_bytes);
    xr_compile_resources_release(memory);
    puts("parser request cancellation, deadline and detached AST: PASS");
    return 0;
}
