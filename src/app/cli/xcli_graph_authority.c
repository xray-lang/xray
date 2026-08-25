/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_graph_authority.c - Exact source graph authority for CLI commands
 */

#include "xcli_graph_authority.h"
#include "xcli_fs.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include "../../os/os_fs.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static bool graph_authority_error(char *error, size_t error_size, const char *format, ...) {
    if (error && error_size > 0) {
        va_list args;
        va_start(args, format);
        vsnprintf(error, error_size, format, args);
        va_end(args);
    }
    return false;
}

XR_FUNC void xr_cli_graph_authority_close(XrCliGraphAuthority *context) {
    if (!context)
        return;
    xr_module_resolver_free(context->resolver);
    xr_lockfile_free(context->lockfile);
    xr_project_free(context->project);
    xr_free(context->authority_namespace);
    xr_free(context->authority_root);
    memset(context, 0, sizeof(*context));
}

XR_FUNC bool xr_cli_graph_authority_open(XrCliGraphAuthority *context, XrModuleRegistry *registry,
                                         const char *entry_path, char *error, size_t error_size) {
    XR_DCHECK(context != NULL, "context is NULL");
    XR_DCHECK(registry != NULL, "registry is NULL");
    XR_DCHECK(entry_path != NULL, "entry_path is NULL");
    if (!context || !registry || !entry_path)
        return graph_authority_error(error, error_size, "invalid graph authority request");

    memset(context, 0, sizeof(*context));
    if (error && error_size > 0)
        error[0] = '\0';

    XrModuleResolver *registry_resolver = xr_module_registry_get_resolver(registry);
    if (!registry_resolver)
        return graph_authority_error(error, error_size, "cannot access module resolver config");

    char project_root[XR_CLI_PATH_MAX];
    if (xr_cli_find_project_root(entry_path, project_root, sizeof(project_root))) {
        context->project = xr_project_load(NULL, project_root);
        if (!context->project || !context->project->initialized) {
            const char *detail = context->project && context->project->native_plan &&
                                         context->project->native_plan->error
                                     ? context->project->native_plan->error
                                     : "invalid xray.toml project configuration";
            graph_authority_error(error, error_size, "%s", detail);
            xr_cli_graph_authority_close(context);
            return false;
        }
        if (!xr_project_module_identity_authority(context->project, &context->entry_authority,
                                                  &context->authority_namespace,
                                                  &context->authority_root)) {
            graph_authority_error(error, error_size,
                                  "cannot establish exact project/package module authority");
            xr_cli_graph_authority_close(context);
            return false;
        }

        char lockfile_path[XR_CLI_PATH_MAX];
        int written =
            snprintf(lockfile_path, sizeof(lockfile_path), "%s/xray.lock", context->project->root);
        if (written < 0 || (size_t) written >= sizeof(lockfile_path)) {
            graph_authority_error(error, error_size, "project lockfile path is too long");
            xr_cli_graph_authority_close(context);
            return false;
        }
        if (xr_fs_is_file(lockfile_path)) {
            context->lockfile = xr_lockfile_load(lockfile_path);
            if (!context->lockfile) {
                graph_authority_error(error, error_size, "cannot load project xray.lock");
                xr_cli_graph_authority_close(context);
                return false;
            }
        }
    } else if (!xr_module_identity_script_authority_from_source(
                   entry_path, &context->entry_authority, &context->authority_root)) {
        graph_authority_error(error, error_size, "cannot establish exact script module authority");
        xr_cli_graph_authority_close(context);
        return false;
    }

    if (!xr_module_identity_authority_valid(&context->entry_authority)) {
        graph_authority_error(error, error_size, "invalid entry module authority");
        xr_cli_graph_authority_close(context);
        return false;
    }

    XrModuleResolverConfig resolver_config = registry_resolver->config;
    resolver_config.lockfile = context->lockfile;
    context->resolver = xr_module_resolver_new(&resolver_config);
    if (!context->resolver) {
        graph_authority_error(error, error_size, "cannot create graph-local module resolver");
        xr_cli_graph_authority_close(context);
        return false;
    }
    return true;
}
