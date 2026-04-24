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

/* ========== Dependency Declaration ========== */

typedef struct XrDependency {
    char *name;
    char *version;
    char *path;
    bool is_local;
} XrDependency;

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
    bool initialized;
} XrProject;

/* ========== Project API ========== */

XR_FUNC XrProject* xr_project_load(XrayIsolate *isolate, const char *project_root);
XR_FUNC void xr_project_free(XrProject *project);

// Returns local path (caller frees), or NULL for non-local dependencies
XR_FUNC char* xr_resolve_local_dependency(XrProject *project, const char *package_name);

/* ========== File Utilities ========== */

XR_FUNC bool xr_project_collect_files(const char *dir_path, char ***files, int *count);
XR_FUNC void xr_project_free_files(char **files, int count);

#endif // XPROJECT_H
