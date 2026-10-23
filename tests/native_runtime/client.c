/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * client.c - Real standalone runtime consumers and failure side effects
 */
#include "xray_native_runtime.h"
#include "base/xplatform.h"
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(XR_OS_WINDOWS)
#include <windows.h>
#else
#include <unistd.h>
#endif

#if MAP_KIND == 0 || MAP_KIND == 4 || MAP_KIND == 5
// A non-NUL-terminated slice excludes the trailing sentinel from the path.
static const char path[] = {'s','o','u','r','c','e','\n','.','x','r','c','!'};
static const XrtSourceLocation locations[] = {{path, sizeof path - 1, 3, 5, 7, 9}};
#endif
#if MAP_KIND == 0
const XrtSourceMap xrt_standalone_source_map = {locations, 1};
#elif MAP_KIND == 1
const XrtSourceMap xrt_standalone_source_map = {NULL, 0};
#elif MAP_KIND == 2
const XrtSourceMap xrt_standalone_source_map = {NULL, 1};
#elif MAP_KIND == 3
static const XrtSourceLocation bad_location[] = {{NULL, 0, 0, 0, 0, 0}};
const XrtSourceMap xrt_standalone_source_map = {bad_location, 1};
#elif MAP_KIND == 4
const XrtSourceMap xrt_standalone_source_map = {locations, 0};
#elif MAP_KIND == 5
#if SIZE_MAX > UINT32_MAX
const XrtSourceMap xrt_standalone_source_map = {locations, (size_t)UINT32_MAX + 1};
#else
const XrtSourceMap xrt_standalone_source_map = {NULL, 1};
#endif
#elif MAP_KIND == 6
static const char invalid_path[] = "invalid.xrc";
static const XrtSourceLocation backwards[] = {{invalid_path, sizeof invalid_path - 1, 8, 4, 3, 2}};
const XrtSourceMap xrt_standalone_source_map = {backwards, 1};
#else
static const char start_path[] = "start.xrc";
#if MAP_KIND == 7
static const XrtSourceLocation start[] = {{start_path, sizeof start_path - 1, 3, 5, 0, 0}};
#elif MAP_KIND == 8
static const XrtSourceLocation start[] = {{start_path, sizeof start_path - 1, 3, 5, 0, 9}};
#else
static const XrtSourceLocation start[] = {{start_path, sizeof start_path - 1, 3, 5, 7, 0}};
#endif
const XrtSourceMap xrt_standalone_source_map = {start, 1};
#endif

static void exit_callback(void) { fputs("EXIT_CALLBACK\n", stderr); }

static int close_output(void) {
#if defined(XR_OS_WINDOWS)
    return CloseHandle(GetStdHandle(STD_OUTPUT_HANDLE)) ? 0 : 1;
#else
    return close(STDOUT_FILENO);
#endif
}

static void empty_root(void *context) { (void)context; }

static int run_client(int argc, char **argv) {
    if (argc < 2) return 90;
    bool string_output = strncmp(argv[1], "string-", 7) == 0;
    const char *mode = string_output ? argv[1] + 7 : argv[1];
    if (string_output && strcmp(mode, "values") == 0) {
        const char bytes[] = "A\0\xe4\xb8\xad\r\nZ!";
        xrt_print_string(bytes, sizeof bytes - 2, 1);
        xrt_print_string(NULL, 0, 0);
        return 0;
    }
    if (string_output && strcmp(mode, "bad-span") == 0) {
        xrt_print_string(NULL, 1, 1);
        return 97;
    }
    if (strncmp(argv[1], "panic", 5) == 0) {
        if (atexit(exit_callback)) return 92;
        if (strcmp(argv[1], "panic-nested") == 0) {
            const XrtSourceMap empty = {NULL, 0};
            xrt_run_standalone(&empty, empty_root, NULL);
        }
        if (strcmp(argv[1], "panic-sink-error") == 0) {
#if defined(XR_OS_WINDOWS)
            if (!CloseHandle(GetStdHandle(STD_ERROR_HANDLE))) return 94;
#else
            if (close(STDERR_FILENO)) return 94;
#endif
        }
        uint32_t id = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 0;
        uint32_t code = strcmp(argv[1], "panic-rem") == 0 ? 421 :
            strcmp(argv[1], "panic-bad-code") == 0 ? 430 : 420;
        xrt_arithmetic_panic(code, id);
    }
    if (strcmp(argv[1], "values") == 0) {
        xrt_print_i64(INT64_MIN, UINT32_MAX);
        xrt_print_i64(0, 0);
        xrt_print_i64(INT64_MAX, 1);
        return 0;
    }
    static char buffer[128];
    if (strcmp(mode, "prefix") == 0) {
        if (fputs("prefix:", stdout) < 0) return 96;
        if (string_output) xrt_print_string("text", 4, 0);
        else xrt_print_i64(12, 0);
        return 0;
    }
    if (setvbuf(stdout, buffer, _IOFBF, sizeof buffer)) return 91;
    if (atexit(exit_callback)) return 92;
    bool stale = strcmp(mode, "stale-flush-error") == 0;
    bool buffered = stale || strcmp(mode, "flush-error") == 0 || strcmp(mode, "pipe-flush") == 0;
    bool pipe = strcmp(mode, "pipe") == 0 || strcmp(mode, "pipe-flush") == 0 || strcmp(mode, "empty-pipe") == 0;
    if (buffered && fputs("buffered prefix", stdout) < 0) return 93;
    if (!pipe && close_output()) return 94;
    if (stale) {
        errno = EPIPE;
#if defined(XR_OS_WINDOWS)
        (void)_set_doserrno(ERROR_BROKEN_PIPE);
#endif
    }
    uint32_t id = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 1;
    if (string_output) {
        bool empty = strcmp(mode, "empty-error") == 0 || strcmp(mode, "empty-pipe") == 0;
        xrt_print_string(empty ? NULL : "text", empty ? 0 : 4, id);
    } else xrt_print_i64(42, id);
    fputs("AFTER_CALL\n", stderr);
    return 95;
}

typedef struct ClientInput { int argc, result; char **argv; } ClientInput;
static void invoke_client(void *context) {
    ClientInput *input = context;
    input->result = run_client(input->argc, input->argv);
}
int main(int argc, char **argv) {
#if defined(XR_OS_WINDOWS)
    (void)_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    if (argc > 1 && strcmp(argv[1], "unbound-panic") == 0)
        xrt_arithmetic_panic(420, 0);
    ClientInput input = {argc, 90, argv};
    xrt_run_standalone(&xrt_standalone_source_map, invoke_client, &input);
    return input.result;
}
