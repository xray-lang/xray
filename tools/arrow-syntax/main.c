/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * main.c - Source-offset arrow observations from the canonical parser
 *
 * KEY CONCEPT:
 *   Report syntax roles without rewriting trivia, strings or unrelated tokens.
 */
#include "frontend/parser/xparse.h"
#include "base/xmalloc.h"
#include "base/xarena.h"
#include "toolchain/xcompiler_arena_backing.h"
#include <stdio.h>

static void arrow(void *data, const Token *token, bool value_arrow) {
    const char *source = data;
    printf("%s %zu\n", value_arrow ? "VALUE" : "TYPE", (size_t)(token->start - source));
}

static void recovery_arrow(void *data, const Token *token, bool value_arrow) {
    const char *source = data;
    printf("RECOVERY_%s %zu\n", value_arrow ? "VALUE" : "TYPE", (size_t)(token->start - source));
}

static void arm_end(void *data, const Token *token) {
    const char *source = data;
    printf("ARM_END %zu\n", (size_t)(token->start - source) + (size_t)token->length);
}

static void diagnostic(void *data, int line, int column, int end_line,
                       int end_column, const char *message) {
    (void)data; (void)end_line; (void)end_column;
    printf("ERROR %d %d %s\n", line, column, message);
}

static int inspect(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 2;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 2; }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return 2; }
    char *source = xr_malloc((size_t)size + 1);
    if (!source) { fclose(file); return 2; }
    size_t read = fread(source, 1, (size_t)size, file);
    int closed = fclose(file);
    if (read != (size_t)size || closed != 0) { xr_free(source); return 2; }
    source[size] = '\0';
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrCompileResources *resources = NULL;
    XrCompilerSession *session = NULL;
    XrArena arena = {0};
    XrArenaBacking backing;
    int result = 2;
    if (xr_compile_resources_new(&limits, &resources) != XR_COMPILE_RESOURCE_OK) goto done;
    if (xr_compile_session_new(resources, &session) != XR_COMPILE_RESOURCE_OK) goto done;
    XrCompileState *state = xr_compile_session_compile_state(session);
    if (xr_compiler_arena_state_backing(state, &backing) != XR_ARENA_OK ||
        xr_arena_open(&arena, 32768, &backing) != XR_ARENA_OK) goto done;
    Scanner scanner = {0};
    if (xr_compile_scanner_open_source(&scanner, state, &arena, source, false, NULL) !=
        XR_COMPILE_RESOURCE_OK) goto done;
    for (;;) {
        Token token = xr_scanner_scan(&scanner);
        if (token.type == TK_EOF) break;
        printf("LEX %zu %d\n", (size_t)(token.start - source), token.length);
        if (token.type == TK_ARROW || token.type == TK_FAT_ARROW)
            printf("TOKEN %zu\n", (size_t)(token.start - source));
        if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) goto done;
    }
    Parser parser = {0};
    if (xr_compile_parser_open(&parser, session, source, path, &arena) != XR_PARSE_OK) goto done;
    if (xr_parser_scan_arrow_syntax(&parser, recovery_arrow, source) != XR_PARSE_OK) {
        xr_compile_parser_close(&parser);
        goto done;
    }
    xr_parser_set_error_callback(&parser, diagnostic, NULL, 0);
    xr_parser_set_arrow_syntax_callback(&parser, arrow, source);
    xr_parser_set_match_arm_syntax_callback(&parser, arm_end, source);
    AstNode *program = NULL;
    XrParseStatus status = xr_compile_parse_recoverable(&parser, &program);
    printf("STATUS %d\n", status);
    result = status == XR_PARSE_OK ? 0 : status == XR_PARSE_RECOVERED || status == XR_PARSE_SYNTAX ? 1 : 2;
done:
    xr_arena_destroy(&arena);
    xr_compile_session_free(session);
    xr_compile_resources_release(resources);
    xr_free(source);
    return result;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    return inspect(argv[1]);
}
