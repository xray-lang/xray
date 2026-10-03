/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xproject.h - Project configuration (xray.toml parsing)
 *
 * KEY CONCEPT:
 *   Parses xray.toml to get project metadata (name, version, main entry).
 *   Manages dependencies and resolves local package paths.
 */

#ifndef XPROJECT_H
#define XPROJECT_H

#include "../base/xhashmap.h"
#include <stdbool.h>

#include "../base/xforward_decl.h"
#include "../base/xdefs.h"
#include "xmodule_identity.h"
#include "xnative_package.h"

/* ========== Dependency Declaration ========== */

typedef struct XrDependency {
    char *name;
    char *version;
    char *path;
    bool is_local;
} XrDependency;

/* ========== Native Target Configuration ========== */

typedef struct XrTargetConfig {
    char *name;
    char *profile;
    char *toolchain;
    char *cc;
    char *zig;
    char *sysroot;
    char *linker_script;
    char *objcopy;
    char *objcopy_output;
    char *runtime_provider;
    char **runtime_capabilities;
    int n_runtime_capabilities;
    char **runtime_hooks;
    int n_runtime_hooks;
    char **cc_flags;
    int n_cc_flags;
    char **ld_flags;
    int n_ld_flags;
    char **objcopy_flags;
    int n_objcopy_flags;
} XrTargetConfig;

/* ========== Project Configuration ========== */

// Parsed from xray.toml
typedef struct XrProject {
    char *root;
    char *name;
    char *main;
    char *version;
    char *description;
    char *license;
    bool is_package;
    XrHashMap *dependencies;
    XrHashMap *targets;
    XrNativePackagePlan *native_plan;
    bool initialized;
} XrProject;

/* ========== Project API ========== */

/* All input paths are absolute. Resources and structural limits are explicit.
 * Constructors require an initially NULL output. Failed operations preserve
 * outputs; query misses publish NULL on OK. */
XR_FUNC XrManifestStatus xr_project_load_owned(const XrOsIoPolicy *policy,
    const char *absolute_root, const XrTomlParseLimits *limits, XrProject **output,
    XrManifestDiagnostic *diagnostic);
XR_FUNC void xr_project_free_owned(XrProject *project);
XR_FUNC bool xr_project_uses_policy(const XrProject *, const XrOsIoPolicy *);
typedef struct XrProjectAuthority XrProjectAuthority;
XR_FUNC XrManifestStatus xr_project_authority_build_owned(const XrProject *,
    XrProjectAuthority **output, XrManifestDiagnostic *diagnostic);
XR_FUNC const XrModuleIdentityAuthority *xr_project_authority_view(const XrProjectAuthority *);
XR_FUNC void xr_project_authority_free_owned(XrProjectAuthority *);
XR_FUNC XrManifestStatus xr_resolve_local_dependency_owned(const XrProject *,
    const char *package_name, char **output);
/* Dependency paths use private policy ownership and this matching release. */
XR_FUNC void xr_project_path_free_owned(char *path);
XR_FUNC XrManifestStatus xr_project_find_target_config_owned(const XrProject *,
    const char *target_name, const XrTargetConfig **output);

#endif /* XPROJECT_H */
