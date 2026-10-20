/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_paths.c - One ledger for input lookup and standard-library selection
 */
#include "xsource_paths.h"
#include "../base/xfileio.h"
#include "../base/xio_policy.inc.h"
#include "../os/os_fs.h"
#include "../os/os_proc.h"

static XrSourceCompileStatus paths_status(XrOsIoStatus status) {
    switch (status) {
    case XR_OS_IO_OK: return XR_SOURCE_COMPILE_OK;
    case XR_OS_IO_NOT_FOUND: return XR_SOURCE_COMPILE_NOT_FOUND;
    case XR_OS_IO_BAD_ARGUMENT: return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    case XR_OS_IO_BUDGET: return XR_SOURCE_COMPILE_BUDGET;
    case XR_OS_IO_OUT_OF_MEMORY: return XR_SOURCE_COMPILE_OUT_OF_MEMORY;
    case XR_OS_IO_UNSUPPORTED: return XR_SOURCE_COMPILE_UNSUPPORTED;
    default: return XR_SOURCE_COMPILE_IO;
    }
}
static char *paths_copy(XrIoContext *io, const char *text) {
    size_t length = 0;
    if (!io_length(io, text, &length)) return NULL;
    char *copy = io_alloc(io, length + 1);
    if (copy && !io_copy(io, copy, text, length + 1)) { io_free(io, copy); copy = NULL; }
    return copy;
}
/* Probes one owned directory candidate and consumes it. Selection moves its
 * absolute spelling to OUTPUT. Rejection moves the most precise spelling,
 * absolute when the platform can form one, into the diagnostic record. */
static XrOsIoStatus paths_probe(const XrOsIoPolicy *policy, char *candidate, char **output,
    XrSourcePathsDiagnostic *detail, bool *wrong_kind) {
    char *absolute = NULL;
    XrOsIoStatus status = xr_realpath_owned(policy, candidate, &absolute);
    XrFsStat info = {0};
    if (status == XR_OS_IO_OK) status = xr_os_io_stat(policy, absolute, &info);
    if (status == XR_OS_IO_OK && info.kind != XR_FS_DIR) {
        *wrong_kind = true; status = XR_OS_IO_BAD_ARGUMENT;
    }
    if (absolute) { policy->free(policy->context, candidate); candidate = absolute; }
    if (status == XR_OS_IO_OK) *output = candidate;
    else if (detail->candidate_count < XR_SOURCE_STDLIB_CANDIDATE_LIMIT)
        detail->candidates[detail->candidate_count++] = candidate;
    else policy->free(policy->context, candidate);
    return status;
}
static void paths_release_candidates(XrSourcePathsDiagnostic *detail) {
    for (uint32_t i = 0; i < detail->candidate_count && i < XR_SOURCE_STDLIB_CANDIDATE_LIMIT; ++i) {
        xr_compile_resources_free(detail->candidates[i]); detail->candidates[i] = NULL;
    }
    detail->candidate_count = 0;
}
typedef struct PathsLayout {
    XrStdlibOrigin origin;
    bool executable_relative;
    const char *spelling;
} PathsLayout;
static const PathsLayout paths_layouts[] = {
    {XR_STDLIB_EXECUTABLE_DEVELOPMENT, true, "../stdlib"},
    {XR_STDLIB_EXECUTABLE_INSTALLED, true, "../lib/xray/stdlib"},
    {XR_STDLIB_WORKING_DIRECTORY, false, "stdlib"},
#ifdef XR_SOURCE_TREE_STDLIB
    /* Injected by CMake for uninstalled builds only; never set for a package. */
    {XR_STDLIB_SOURCE_TREE, false, XR_SOURCE_TREE_STDLIB},
#endif
};
/* File and text consumers share this exact real process/environment selector.
 * It neither accepts nor probes a source entry. Authority admission remains the
 * caller's explicit file/text operation. Only NOT_FOUND permits a fallback. */
static void paths_select_stdlib(XrIoContext *io,char **output,XrStdlibOrigin *origin,
    XrSourcePathsDiagnostic *detail,bool *wrong_kind) {
    char *override=NULL,*executable=NULL,*directory=NULL;
    detail->stage = XR_SOURCE_PATH_ENVIRONMENT;
    XrOsIoStatus queried = xr_os_io_environment_get(io->policy, "XRAY_STDLIB_PATH", &override);
    if (queried != XR_OS_IO_NOT_FOUND) io_status(io, queried);
    if (io->status != XR_OS_IO_OK) goto cleanup;
    if (override && !io_work(io, 1)) goto cleanup;
    detail->stage = XR_SOURCE_PATH_STDLIB;
    if (override && override[0]) {
        char *candidate = override; override = NULL;
        io_status(io, paths_probe(io->policy, candidate, output, detail, wrong_kind));
        goto cleanup;
    }
    detail->stage = XR_SOURCE_PATH_EXECUTABLE;
    io_status(io, xr_os_io_self_exe_path(io->policy, &executable));
    if (io->status == XR_OS_IO_OK) io_status(io, xr_path_dirname_owned(io->policy, executable, &directory));
    if (io->status != XR_OS_IO_OK) goto cleanup;
    detail->stage = XR_SOURCE_PATH_STDLIB;
    for (size_t i = 0; i < sizeof(paths_layouts) / sizeof(paths_layouts[0]); ++i) {
        const PathsLayout *layout = &paths_layouts[i];
        char *candidate = NULL;
        if (layout->executable_relative)
            io_status(io, xr_path_join_owned(io->policy, directory, layout->spelling, &candidate));
        else candidate = paths_copy(io, layout->spelling);
        if (io->status != XR_OS_IO_OK) goto cleanup;
        XrOsIoStatus probed = paths_probe(io->policy, candidate, output, detail, wrong_kind);
        if (probed == XR_OS_IO_NOT_FOUND) continue;
        io_status(io, probed);
        *origin = layout->origin;
        goto cleanup;
    }
    io_status(io, XR_OS_IO_NOT_FOUND);
cleanup:
    io_free(io,directory);io_free(io,executable);io_free(io,override);
}
/* Only a failure reported to a caller keeps its probe record. */
static void paths_publish(XrSourcePathsDiagnostic *detail, XrSourcePathsDiagnostic *diagnostic) {
    if (detail->status == XR_SOURCE_COMPILE_OK || !diagnostic) paths_release_candidates(detail);
    if (diagnostic) *diagnostic = *detail;
}
XR_FUNC void xr_source_stdlib_path_free(XrStdlibPath *path) {
    if(!path)return;
    xr_compile_resources_free(path->path);memset(path,0,sizeof(*path));
}
XR_FUNC XrSourceCompileStatus xr_source_stdlib_path(XrCompileResources *resources,
    XrStdlibPath *output,XrSourcePathsDiagnostic *diagnostic) {
    XrSourcePathsDiagnostic detail={.status=XR_SOURCE_COMPILE_BAD_ARGUMENT,
        .stage=XR_SOURCE_PATH_INPUT,.io_status=XR_OS_IO_BAD_ARGUMENT};
    if(diagnostic&&diagnostic->candidate_count)return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    if(!resources||!output||output->path||output->origin)goto done;
    XrOsIoPolicy policy=xr_compile_io_policy(resources);XrIoContext io={&policy,XR_OS_IO_OK};
    XrStdlibPath owned={0};bool wrong_kind=false;
    paths_select_stdlib(&io,&owned.path,&owned.origin,&detail,&wrong_kind);
    detail.io_status=io.status;
    detail.status=wrong_kind?XR_SOURCE_COMPILE_INVALID:paths_status(io.status);
    if(detail.status==XR_SOURCE_COMPILE_OK)*output=owned;
    else xr_source_stdlib_path_free(&owned);
done:
    paths_publish(&detail,diagnostic);
    return detail.status;
}
XR_FUNC void xr_source_paths_free(XrSourcePaths *paths) {
    if (!paths) return;
    xr_compile_resources_free(paths->entry); xr_compile_resources_free(paths->stdlib);
    memset(paths, 0, sizeof(*paths));
}
XR_FUNC XrSourceCompileStatus xr_source_paths(XrCompileResources *resources,
    const char *entry_spelling, XrSourcePaths *output, XrSourcePathsDiagnostic *diagnostic) {
    XrSourcePathsDiagnostic detail = {.status = XR_SOURCE_COMPILE_BAD_ARGUMENT,
        .stage = XR_SOURCE_PATH_INPUT, .io_status = XR_OS_IO_BAD_ARGUMENT};
    if (diagnostic && diagnostic->candidate_count) return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    if (!resources || !entry_spelling || !output || output->entry || output->stdlib || output->stdlib_origin)
        goto done;
    XrOsIoPolicy policy = xr_compile_io_policy(resources);
    XrIoContext io = {&policy, XR_OS_IO_OK};
    XrSourcePaths owned = {0};
    bool wrong_kind = false;
    detail.stage = XR_SOURCE_PATH_ENTRY;
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
    detail.status = wrong_kind ? XR_SOURCE_COMPILE_INVALID : paths_status(io.status);
    if (detail.status == XR_SOURCE_COMPILE_OK) *output = owned;
    else xr_source_paths_free(&owned);
done:
    paths_publish(&detail, diagnostic);
    return detail.status;
}
XR_FUNC void xr_source_paths_diagnostic_free(XrSourcePathsDiagnostic *diagnostic) {
    if (!diagnostic) return;
    paths_release_candidates(diagnostic);
    memset(diagnostic, 0, sizeof(*diagnostic));
}
