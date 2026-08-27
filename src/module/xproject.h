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

#include "../runtime/value/xvalue.h"
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

XR_FUNC XrProject *xr_project_load(XrVMRuntime *isolate, const char *project_root);
XR_FUNC void xr_project_free(XrProject *project);

/* Build the exact typed entry authority declared by the manifest. Both
 * returned strings are xr_malloc-owned and back the authority fields.
 * On failure `err` receives the manifest field that made the authority
 * inexact, so a caller can name the required spelling instead of reporting
 * that some authority could not be established. */
XR_FUNC bool xr_project_module_identity_authority(const XrProject *project,
                                                  XrModuleIdentityAuthority *authority,
                                                  char **namespace_out, char **physical_root_out,
                                                  char *err, size_t err_size);

// Returns local path (caller frees), or NULL for non-local dependencies
XR_FUNC char *xr_resolve_local_dependency(XrProject *project, const char *package_name);
XR_FUNC const XrTargetConfig *xr_project_find_target_config(const XrProject *project,
                                                            const char *target_name);

/* ========== File Utilities ========== */

XR_FUNC bool xr_project_collect_files(const char *dir_path, char ***files, int *count);
XR_FUNC void xr_project_free_files(char **files, int count);

#endif  // XPROJECT_H
