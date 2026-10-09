/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_source_paths.c - One ledger for input lookup and standard-library selection
 */
#include "xcli_source_paths.h"
#include "../../base/xfileio.h"
#include "../../base/xio_policy.inc.h"
#include "../../os/os_fs.h"
#include "../../os/os_proc.h"

static XrCliCompileSourceStatus paths_status(XrOsIoStatus status) {
    switch (status) {
    case XR_OS_IO_OK: return XR_CLI_COMPILE_SOURCE_OK;
    case XR_OS_IO_NOT_FOUND: return XR_CLI_COMPILE_SOURCE_NOT_FOUND;
    case XR_OS_IO_BAD_ARGUMENT: return XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT;
    case XR_OS_IO_BUDGET: return XR_CLI_COMPILE_SOURCE_BUDGET;
    case XR_OS_IO_OUT_OF_MEMORY: return XR_CLI_COMPILE_SOURCE_OUT_OF_MEMORY;
    case XR_OS_IO_UNSUPPORTED: return XR_CLI_COMPILE_SOURCE_UNSUPPORTED;
    default: return XR_CLI_COMPILE_SOURCE_IO;
    }
}
static XrOsIoStatus paths_directory(const XrOsIoPolicy *policy, const char *candidate,
    char **output, bool *wrong_kind) {
    char *absolute = NULL;
    XrOsIoStatus status = xr_realpath_owned(policy, candidate, &absolute);
    XrFsStat info = {0};
    if (status == XR_OS_IO_OK) status = xr_os_io_stat(policy, absolute, &info);
    if (status == XR_OS_IO_OK && info.kind != XR_FS_DIR) {
        *wrong_kind = true; status = XR_OS_IO_BAD_ARGUMENT;
    }
    if (status == XR_OS_IO_OK) *output = absolute;
    else policy->free(policy->context, absolute);
    return status;
}
/* File and text consumers share this exact real process/environment selector.
 * It neither accepts nor probes a source entry. Authority admission remains the
 * caller's explicit file/text operation. Only NOT_FOUND permits a fallback. */
static void paths_select_stdlib(XrIoContext *io,char **output,XrCliStdlibOrigin *origin,
    XrCliSourcePathsDiagnostic *detail,bool *wrong_kind) {
    char *override=NULL,*executable=NULL,*directory=NULL,*candidate=NULL;
    detail->stage = XR_CLI_SOURCE_PATH_ENVIRONMENT;
    XrOsIoStatus queried = xr_os_io_environment_get(io->policy, "XRAY_STDLIB_PATH", &override);
    if (queried != XR_OS_IO_NOT_FOUND) io_status(io, queried);
    if (io->status != XR_OS_IO_OK) goto cleanup;
    if (override && !io_work(io, 1)) goto cleanup;
    detail->stage = XR_CLI_SOURCE_PATH_STDLIB;
    if (override && override[0]) {
        io_status(io, paths_directory(io->policy, override, output, wrong_kind));
        goto cleanup;
    }
    detail->stage = XR_CLI_SOURCE_PATH_EXECUTABLE;
    io_status(io, xr_os_io_self_exe_path(io->policy, &executable));
    if (io->status == XR_OS_IO_OK) io_status(io, xr_path_dirname_owned(io->policy, executable, &directory));
    if (io->status != XR_OS_IO_OK) goto cleanup;
    detail->stage = XR_CLI_SOURCE_PATH_STDLIB;
    static const char *const suffixes[] = {"../stdlib", "../lib/xray/stdlib"};
    for (unsigned i = 0; i < 2 && io->status == XR_OS_IO_OK; ++i) {
        io_status(io, xr_path_join_owned(io->policy, directory, suffixes[i], &candidate));
        if (io->status != XR_OS_IO_OK) break;
        queried = paths_directory(io->policy, candidate, output, wrong_kind);
        io_free(io, candidate); candidate = NULL;
        if (queried == XR_OS_IO_NOT_FOUND) continue;
        io_status(io, queried);
        *origin = i ? XR_CLI_STDLIB_EXECUTABLE_INSTALLED : XR_CLI_STDLIB_EXECUTABLE_DEVELOPMENT;
        goto cleanup;
    }
    if (io->status == XR_OS_IO_OK) {
        *origin = XR_CLI_STDLIB_WORKING_DIRECTORY;
        io_status(io, paths_directory(io->policy, "stdlib", output, wrong_kind));
    }
cleanup:
    io_free(io,candidate);io_free(io,directory);io_free(io,executable);io_free(io,override);
}
XR_FUNC void xr_cli_compile_stdlib_path_free(XrCliStdlibPath *path) {
    if(!path)return;
    xr_compile_resources_free(path->path);memset(path,0,sizeof(*path));
}
XR_FUNC XrCliCompileSourceStatus xr_cli_compile_stdlib_path(XrCompileResources *resources,
    XrCliStdlibPath *output,XrCliSourcePathsDiagnostic *diagnostic) {
    XrCliSourcePathsDiagnostic detail={XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT,
        XR_CLI_SOURCE_PATH_INPUT,XR_OS_IO_BAD_ARGUMENT};
    if(!resources||!output||output->path||output->origin)goto done;
    XrOsIoPolicy policy=xr_compile_io_policy(resources);XrIoContext io={&policy,XR_OS_IO_OK};
    XrCliStdlibPath owned={0};bool wrong_kind=false;
    paths_select_stdlib(&io,&owned.path,&owned.origin,&detail,&wrong_kind);
    detail.io_status=io.status;
    detail.status=wrong_kind?XR_CLI_COMPILE_SOURCE_INVALID:paths_status(io.status);
    if(detail.status==XR_CLI_COMPILE_SOURCE_OK)*output=owned;
    else xr_cli_compile_stdlib_path_free(&owned);
done:
    if(diagnostic)*diagnostic=detail;
    return detail.status;
}
XR_FUNC void xr_cli_compile_source_paths_free(XrCliSourcePaths *paths) {
    if (!paths) return;
    xr_compile_resources_free(paths->entry); xr_compile_resources_free(paths->stdlib);
    memset(paths, 0, sizeof(*paths));
}
XR_FUNC XrCliCompileSourceStatus xr_cli_compile_source_paths(XrCompileResources *resources,
    const char *entry_spelling, XrCliSourcePaths *output, XrCliSourcePathsDiagnostic *diagnostic) {
    XrCliSourcePathsDiagnostic detail = {XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT,
        XR_CLI_SOURCE_PATH_INPUT, XR_OS_IO_BAD_ARGUMENT};
    if (!resources || !entry_spelling || !output || output->entry || output->stdlib || output->stdlib_origin)
        goto done;
    XrOsIoPolicy policy = xr_compile_io_policy(resources);
    XrIoContext io = {&policy, XR_OS_IO_OK};
    XrCliSourcePaths owned = {0};
    bool wrong_kind = false;
    detail.stage = XR_CLI_SOURCE_PATH_ENTRY;
    if (!io_work(&io, 1)) goto cleanup;
    if (!entry_spelling[0]) { io_status(&io, XR_OS_IO_BAD_ARGUMENT); goto cleanup; }
    io_status(&io, xr_realpath_owned(&policy, entry_spelling, &owned.entry));
    if (io.status == XR_OS_IO_OK) {
        XrFsStat info = {0};
        io_status(&io, xr_os_io_stat(&policy, owned.entry, &info));
        if (io.status == XR_OS_IO_OK && info.kind != XR_FS_FILE) {
            wrong_kind = true; io_status(&io, XR_OS_IO_BAD_ARGUMENT);
        }
    }
    if (io.status != XR_OS_IO_OK) goto cleanup;
    paths_select_stdlib(&io,&owned.stdlib,&owned.stdlib_origin,&detail,&wrong_kind);
cleanup:
    detail.io_status = io.status;
    detail.status = wrong_kind ? XR_CLI_COMPILE_SOURCE_INVALID : paths_status(io.status);
    if (detail.status == XR_CLI_COMPILE_SOURCE_OK) *output = owned;
    else xr_cli_compile_source_paths_free(&owned);
done:
    if (diagnostic) *diagnostic = detail;
    return detail.status;
}
