/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_check.c - Source and single-file syntax checking with one compiler ledger
 */
#include "xcli.h"
#include "xcli_canonical_source.h"
#include "xcli_source_paths.h"
#include "../../base/xio_policy.inc.h"
#include "../../frontend/parser/xparse.h"
#include "../../os/os_dir.h"
#include "../../os/os_fs.h"
#include <stdio.h>
#if XR_OS_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif

typedef struct CheckWork {
    XrCompileResources *resources;
    XrOsIoPolicy policy;
    size_t total, passed, errors;
    int result;
    bool syntax_only, verbose, quiet, stopped;
} CheckWork;
typedef struct CheckDirectory {
    struct CheckDirectory *next;
    char *path;
} CheckDirectory;

static void check_failure(CheckWork *work, int result, bool stop) {
    ++work->errors;
    if (result > work->result) work->result = result;
    work->stopped |= stop;
}
static void check_io_failure(CheckWork *work, const char *path, XrOsIoStatus status) {
    fprintf(stderr, "xray check: %s: I/O status=%u\n", path, (unsigned)status);
    bool internal = status == XR_OS_IO_OUT_OF_MEMORY || status == XR_OS_IO_BAD_ARGUMENT;
    check_failure(work, internal ? XR_CLI_EXIT_INTERNAL : XR_CLI_EXIT_FAIL,
        internal || status == XR_OS_IO_BUDGET);
}
static bool check_path_length(XrIoContext *io, const char *path, size_t *length) {
    if (!path) return io_status(io, XR_OS_IO_BAD_ARGUMENT);
    for (size_t i = 0; i < XR_PATH_MAX; ++i) {
        if (!io_work(io, 1)) return false;
        if (!path[i]) { *length = i; return true; }
    }
    return io_status(io, XR_OS_IO_BUDGET);
}
static char *check_path_copy(CheckWork *work, const char *path) {
    XrIoContext io = {&work->policy, XR_OS_IO_OK};
    size_t length = 0;
    char *copy = NULL;
    if (check_path_length(&io, path, &length)) copy = io_alloc(&io, length + 1);
    if (copy) io_copy(&io, copy, path, length + 1);
    if (io.status != XR_OS_IO_OK) {
        io_free(&io, copy);
        check_io_failure(work, "input path", io.status);
        return NULL;
    }
    return copy;
}
static char *check_path_join(CheckWork *work, const char *directory, const char *name) {
    XrIoContext io = {&work->policy, XR_OS_IO_OK};
    size_t parent = 0, child = 0;
    char *path = NULL;
    if (check_path_length(&io, directory, &parent) && check_path_length(&io, name, &child)) {
        if (parent > XR_PATH_MAX - 2 || child > XR_PATH_MAX - parent - 2)
            io_status(&io, XR_OS_IO_BUDGET);
        else path = io_alloc(&io, parent + child + 2);
    }
    if (path && io_copy(&io, path, directory, parent) && io_work(&io, 1)) {
        path[parent] = '/';
        io_copy(&io, path + parent + 1, name, child + 1);
    }
    if (io.status != XR_OS_IO_OK) {
        io_free(&io, path);
        check_io_failure(work, directory, io.status);
        return NULL;
    }
    return path;
}
static bool check_source_name(CheckWork *work, const char *name) {
    XrIoContext io = {&work->policy, XR_OS_IO_OK};
    size_t length = 0;
    bool result = false;
    if (check_path_length(&io, name, &length) && length > 3 && io_work(&io, 3))
        result = memcmp(name + length - 3, ".xr", 3) == 0;
    if (io.status != XR_OS_IO_OK) check_io_failure(work, "directory entry", io.status);
    return result;
}
static void check_syntax(CheckWork *work, const char *path) {
    uint8_t *bytes = NULL;
    size_t length = 0;
    XrOsIoStatus status = xr_os_io_read_regular_file(&work->policy, path, SIZE_MAX - 1, &bytes, &length);
    XrIoContext io = {&work->policy, status};
    bool nul = false;
    for (size_t i = 0; i < length && io_work(&io, 1); ++i) {
        if (!bytes[i]) { nul = true; break; }
    }
    if (nul) {
        fprintf(stderr, "%s: error: source contains a NUL byte\n", path);
        check_failure(work, XR_CLI_EXIT_FAIL, false);
    } else if (io.status == XR_OS_IO_OK) {
        void *terminated = bytes;
        XrCompileResourceStatus resized = xr_compile_resources_resize(work->resources, &terminated, length + 1);
        bytes = terminated;
        if (resized != XR_COMPILE_RESOURCE_OK)
            io_status(&io, resized == XR_COMPILE_RESOURCE_BUDGET ? XR_OS_IO_BUDGET :
                resized == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_OS_IO_OUT_OF_MEMORY : XR_OS_IO_BAD_ARGUMENT);
        if (io_work(&io, 1)) bytes[length] = 0;
        if (io.status == XR_OS_IO_OK) {
            XrCompilerSession *session = NULL;
            XrCompilerSessionStatus opened = xr_compile_session_new(work->resources, &session);
            AstNode *ast = NULL;
            XrParseStatus parsed = opened == XR_COMPILER_SESSION_OK ?
                xr_compile_parse_with_source(session, (const char *)bytes, path, &ast) :
                opened == XR_COMPILER_SESSION_BUDGET ? XR_PARSE_BUDGET :
                opened == XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_PARSE_OUT_OF_MEMORY : XR_PARSE_BAD_ARGUMENT;
            /* The AST owns its arena independently of the producing session. */
            xr_compile_session_free(session);
            xr_program_destroy(ast);
            if (parsed != XR_PARSE_OK) {
                if (parsed != XR_PARSE_SYNTAX)
                    fprintf(stderr, "%s: error: parser status=%u\n", path, (unsigned)parsed);
                bool internal = parsed == XR_PARSE_OUT_OF_MEMORY || parsed == XR_PARSE_BAD_ARGUMENT;
                check_failure(work, internal ? XR_CLI_EXIT_INTERNAL : XR_CLI_EXIT_FAIL,
                    internal || parsed == XR_PARSE_BUDGET);
            }
        }
    }
    xr_compile_resources_free(bytes);
    if (io.status != XR_OS_IO_OK) check_io_failure(work, path, io.status);
}
static void check_semantic(CheckWork *work, const char *path) {
#if defined(XR_ARCH_X86_64)
    XrCliSourcePaths paths = {0};
    XrCliSourcePathsDiagnostic path_diagnostic = {0};
    XrCliCompileSourceDiagnostic diagnostic = {0};
    XrXirSourceProduct *product = NULL;
    XrXirCompileContext context = {work->resources, xr_xir_compile_default_limits()};
    XrCliCompileSourceStatus status = xr_cli_compile_source_paths(work->resources, path, &paths, &path_diagnostic);
    if (status == XR_CLI_COMPILE_SOURCE_OK) {
        XrCliCompileSourceRequest request = {&context, paths.entry, paths.stdlib, NULL,
            xr_cli_compile_default_manifest_limits(), {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
        status = xr_cli_compile_source_build(&request, &product, &diagnostic);
        if (status != XR_CLI_COMPILE_SOURCE_OK) {
            const XrXirSourceDiagnostic *source = &diagnostic.source.source;
            const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(diagnostic.source.snapshot);
            const char *failed_path = diagnostic.source.source_path;
            if (!failed_path && view && view->modules && source->module < view->module_count)
                failed_path = view->modules[source->module].path;
            if (failed_path && source->line > 0)
                fprintf(stderr, "%s:%d:%d: error: %s\n", failed_path, source->line, source->column,
                    source->message[0] ? source->message : xr_cli_compile_source_status_name(status));
            else
                fprintf(stderr, "xray check: %s: source build failed (stage=%u status=%s): %s\n",
                    path, (unsigned)diagnostic.stage, xr_cli_compile_source_status_name(status), source->message);
        }
    } else {
        fprintf(stderr, "xray check: %s: path lookup failed (stage=%u status=%s io=%u)\n", path,
            (unsigned)path_diagnostic.stage, xr_cli_compile_source_status_name(status), (unsigned)path_diagnostic.io_status);
    }
    xr_xir_compile_source_product_free(product);
    xr_cli_compile_source_diagnostic_free(&diagnostic);
    xr_cli_compile_source_paths_free(&paths);
    if (status != XR_CLI_COMPILE_SOURCE_OK) {
        bool internal = status == XR_CLI_COMPILE_SOURCE_OUT_OF_MEMORY || status == XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT;
        check_failure(work, internal ? XR_CLI_EXIT_INTERNAL : XR_CLI_EXIT_FAIL,
            internal || status == XR_CLI_COMPILE_SOURCE_BUDGET);
    }
#else
    fprintf(stderr, "xray check: %s: unsupported Source target architecture\n", path);
    check_failure(work, XR_CLI_EXIT_FAIL, true);
#endif
}
static void check_file(CheckWork *work, const char *path) {
    size_t errors = work->errors;
    ++work->total;
    if (work->syntax_only) check_syntax(work, path);
    else check_semantic(work, path);
    if (errors == work->errors) {
        ++work->passed;
        if (work->verbose && !work->quiet && printf("ok %s\n", path) < 0)
            check_io_failure(work, "stdout", XR_OS_IO_IO);
    }
}
/* Every queued path is owned. No recursive C frames or directory handles
 * accumulate with tree depth. Final reparse points are never descended into. */
static void check_path(CheckWork *work, char *path, CheckDirectory **pending) {
    XrFsStat stat = {0};
    XrOsIoStatus status = xr_os_io_stat(&work->policy, path, &stat);
    if (status != XR_OS_IO_OK) check_io_failure(work, path, status);
    else if (stat.kind == XR_FS_FILE) check_file(work, path);
    else if (stat.kind == XR_FS_DIR) {
        XrIoContext io = {&work->policy, XR_OS_IO_OK};
        CheckDirectory *item = io_alloc(&io, sizeof(*item));
        if (item) {
            item->path = path; item->next = *pending; *pending = item;
            return;
        }
        check_io_failure(work, path, io.status);
    } else check_io_failure(work, path, XR_OS_IO_UNSUPPORTED);
    xr_compile_resources_free(path);
}
static void check_directories(CheckWork *work, CheckDirectory **pending) {
    while (*pending && !work->stopped) {
        CheckDirectory *item = *pending;
        *pending = item->next;
        XrDirIter *iterator = NULL;
        XrOsIoStatus status = xr_os_io_dir_open(&work->policy, item->path, &iterator);
        if (status != XR_OS_IO_OK) check_io_failure(work, item->path, status);
        while (status == XR_OS_IO_OK && !work->stopped) {
            XrDirEntry entry;
            status = xr_os_io_dir_next(iterator, &entry);
            if (status == XR_OS_IO_END) break;
            if (status != XR_OS_IO_OK) { check_io_failure(work, item->path, status); break; }
            if (entry.is_dir || check_source_name(work, entry.name)) {
                char *path = check_path_join(work, item->path, entry.name);
                if (path) check_path(work, path, pending);
            }
        }
        xr_os_io_dir_close(iterator);
        xr_compile_resources_free(item->path);
        xr_compile_resources_free(item);
    }
    while (*pending) {
        CheckDirectory *item = *pending;
        *pending = item->next;
        xr_compile_resources_free(item->path);
        xr_compile_resources_free(item);
    }
}
XR_FUNC int cmd_check(const XrCliInvocation *inv) {
    if (!inv || inv->positional_count < 0 || (inv->positional_count && !inv->positionals))
        return XR_CLI_EXIT_INTERNAL;
#if XR_OS_WINDOWS
    if (_setmode(_fileno(stdout), _O_BINARY) == -1 || _setmode(_fileno(stderr), _O_BINARY) == -1)
        return XR_CLI_EXIT_INTERNAL;
#endif
    CheckWork work = {0};
    work.syntax_only = xr_cli_opt_bool(&inv->options, "syntax-only");
    work.verbose = xr_cli_opt_bool(&inv->options, "verbose");
    work.quiet = xr_cli_opt_bool(&inv->options, "quiet");
    XrCompileResourceLimits limits = xr_cli_compile_default_resource_limits();
    XrCompileResourceStatus status = xr_compile_resources_new(&limits, &work.resources);
    if (status != XR_COMPILE_RESOURCE_OK) {
        fprintf(stderr, "xray check: cannot initialize compiler resources\n");
        return status == XR_COMPILE_RESOURCE_BUDGET ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_INTERNAL;
    }
    work.policy = xr_compile_io_policy(work.resources);
    for (int i = 0; i < (inv->positional_count ? inv->positional_count : 1) && !work.stopped; ++i) {
        const char *spelling = inv->positional_count ? inv->positionals[i] : ".";
        char *path = check_path_copy(&work, spelling);
        CheckDirectory *pending = NULL;
        if (path) check_path(&work, path, &pending);
        check_directories(&work, &pending);
    }
    if (!work.quiet && work.total > 1) {
        int printed = work.errors ? printf("\nFAIL: %zu files checked, %zu errors\n", work.total, work.errors) :
            printf("\nOK: %zu files checked, no errors\n", work.total);
        if (printed < 0) check_io_failure(&work, "stdout", XR_OS_IO_IO);
    }
    if (fflush(stdout) || ferror(stderr)) check_failure(&work, XR_CLI_EXIT_FAIL, false);
    xr_compile_resources_release(work.resources);
    return work.result;
}
