/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xproject.c - Project configuration implementation
 */

#include "xproject.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../base/xfileio.h"
#include "../base/xhashmap.h"
#include "../base/xtoml.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../os/os_dir.h"

/* ========== Helper Functions ========== */

static void free_dependency(XrDependency *dep);
static void free_target_config(XrTargetConfig *cfg);

/* Get a strdup'd string from a TOML table by key, or NULL. */
static char *get_toml_str(XrTomlValue *tbl, const char *key) {
    const char *s = xtoml_get_string(tbl, key);
    return s ? xr_strdup(s) : NULL;
}

static bool get_toml_str_array(XrTomlValue *tbl, const char *key, char ***out_items,
                               int *out_count) {
    XrTomlValue *arr;
    char **items = NULL;
    int count;

    if (out_items)
        *out_items = NULL;
    if (out_count)
        *out_count = 0;
    if (!tbl || !key || !out_items || !out_count)
        return true;

    arr = xtoml_get_array(tbl, key);
    if (!arr)
        return true;

    count = xtoml_array_len(arr);
    if (count <= 0)
        return true;

    items = (char **) xr_calloc((size_t) count, sizeof(char *));
    if (!items)
        return false;

    for (int i = 0; i < count; i++) {
        XrTomlValue *item = xtoml_array_get(arr, i);
        if (!item || item->type != XR_TOML_STRING)
            continue;
        items[i] = xr_strdup(item->as.string);
        if (!items[i]) {
            for (int j = 0; j < i; j++)
                xr_free(items[j]);
            xr_free(items);
            return false;
        }
    }

    *out_items = items;
    *out_count = count;
    return true;
}

static XrTargetConfig *load_target_config(const char *name, XrTomlValue *tbl) {
    XrTargetConfig *cfg;

    if (!name || !tbl || tbl->type != XR_TOML_TABLE)
        return NULL;

    cfg = (XrTargetConfig *) xr_calloc(1, sizeof(XrTargetConfig));
    if (!cfg)
        return NULL;

    cfg->name = xr_strdup(name);
    cfg->profile = get_toml_str(tbl, "profile");
    cfg->toolchain = get_toml_str(tbl, "toolchain");
    cfg->cc = get_toml_str(tbl, "cc");
    cfg->zig = get_toml_str(tbl, "zig");
    cfg->sysroot = get_toml_str(tbl, "sysroot");
    cfg->linker_script = get_toml_str(tbl, "linker_script");
    cfg->objcopy = get_toml_str(tbl, "objcopy");
    cfg->objcopy_output = get_toml_str(tbl, "objcopy_output");
    cfg->runtime_provider = get_toml_str(tbl, "runtime_provider");
    if (!cfg->name || !get_toml_str_array(tbl, "cc_flags", &cfg->cc_flags, &cfg->n_cc_flags) ||
        !get_toml_str_array(tbl, "ld_flags", &cfg->ld_flags, &cfg->n_ld_flags) ||
        !get_toml_str_array(tbl, "runtime_capabilities", &cfg->runtime_capabilities,
                            &cfg->n_runtime_capabilities) ||
        !get_toml_str_array(tbl, "runtime_hooks", &cfg->runtime_hooks, &cfg->n_runtime_hooks) ||
        !get_toml_str_array(tbl, "objcopy_flags", &cfg->objcopy_flags, &cfg->n_objcopy_flags)) {
        free_target_config(cfg);
        return NULL;
    }

    return cfg;
}

/* ========== Project Loading ========== */

XrProject *xr_project_load(XrVMRuntime *isolate, const char *project_root) {
    (void) isolate; /* no longer needed — base xtoml parser is pure C */
    if (!project_root)
        return NULL;

    char *toml_path = xr_path_join(project_root, "xray.toml");
    if (!toml_path)
        return NULL;

    size_t content_size;
    char *content = xr_file_read_all(toml_path, "r", &content_size);
    xr_free(toml_path);
    if (!content)
        return NULL;

    XrTomlValue *root = xtoml_parse(content, content_size);
    xr_free(content);
    if (!root)
        return NULL;

    XrProject *project = (XrProject *) xr_calloc(1, sizeof(XrProject));
    if (!project) {
        xtoml_free(root);
        return NULL;
    }

    project->root = xr_strdup(project_root);
    project->dependencies = xr_hashmap_new();
    project->targets = xr_hashmap_new();

    // Try [project], then [package]
    XrTomlValue *section = xtoml_get_table(root, "project");
    if (!section) {
        section = xtoml_get_table(root, "package");
        if (section)
            project->is_package = true;
    }

    if (section) {
        project->name = get_toml_str(section, "name");
        project->main = get_toml_str(section, "main");
        if (project->is_package) {
            project->version = get_toml_str(section, "version");
            project->description = get_toml_str(section, "description");
            project->license = get_toml_str(section, "license");
        }
    }

    // Parse [dependencies] section
    XrTomlValue *deps = xtoml_get_table(root, "dependencies");
    if (deps && project->dependencies) {
        for (int i = 0; i < deps->as.table.count; i++) {
            XrTomlMember *m = &deps->as.table.members[i];
            XR_DCHECK(m->key != NULL, "TOML member key must not be NULL");

            XrDependency *dep = (XrDependency *) xr_calloc(1, sizeof(XrDependency));
            if (!dep)
                continue;
            dep->name = xr_strdup(m->key);
            if (!dep->name) {
                xr_free(dep);
                continue;
            }

            if (m->value->type == XR_TOML_STRING) {
                // Simple version string: "^1.0.0"
                dep->version = xr_strdup(m->value->as.string);
                dep->is_local = false;
            } else if (m->value->type == XR_TOML_TABLE) {
                // Complex dependency: { version = "^1.0.0", path = "./local" }
                dep->version = get_toml_str(m->value, "version");
                dep->path = get_toml_str(m->value, "path");
                dep->is_local = (dep->path != NULL);
            }

            // Key must be dep->name (owned by dep): the TOML tree and its
            // m->key strings are freed right after parsing, and lookups
            // happen long after that.
            if (!xr_hashmap_set(project->dependencies, dep->name, dep)) {
                free_dependency(dep);
            }
        }
    }

    // Parse [target.<triple>] sections for native/freestanding build defaults.
    XrTomlValue *targets = xtoml_get_table(root, "target");
    if (targets && project->targets) {
        for (int i = 0; i < targets->as.table.count; i++) {
            XrTomlMember *m = &targets->as.table.members[i];
            XrTargetConfig *cfg;
            XR_DCHECK(m->key != NULL, "TOML target key must not be NULL");
            if (!m->value || m->value->type != XR_TOML_TABLE)
                continue;
            cfg = load_target_config(m->key, m->value);
            if (!cfg)
                continue;
            if (!xr_hashmap_set(project->targets, cfg->name, cfg))
                free_target_config(cfg);
        }
    }

    project->initialized = true;
    xtoml_free(root);
    return project;
}

/*
 * Free a dependency structure.
 */
static void free_dependency(XrDependency *dep) {
    if (!dep)
        return;
    xr_free(dep->name);
    xr_free(dep->version);
    xr_free(dep->path);
    xr_free(dep);
}

static void free_string_list(char **items, int count) {
    if (!items)
        return;
    for (int i = 0; i < count; i++)
        xr_free(items[i]);
    xr_free(items);
}

static void free_target_config(XrTargetConfig *cfg) {
    if (!cfg)
        return;
    xr_free(cfg->name);
    xr_free(cfg->profile);
    xr_free(cfg->toolchain);
    xr_free(cfg->cc);
    xr_free(cfg->zig);
    xr_free(cfg->sysroot);
    xr_free(cfg->linker_script);
    xr_free(cfg->objcopy);
    xr_free(cfg->objcopy_output);
    xr_free(cfg->runtime_provider);
    free_string_list(cfg->runtime_capabilities, cfg->n_runtime_capabilities);
    free_string_list(cfg->runtime_hooks, cfg->n_runtime_hooks);
    free_string_list(cfg->cc_flags, cfg->n_cc_flags);
    free_string_list(cfg->ld_flags, cfg->n_ld_flags);
    free_string_list(cfg->objcopy_flags, cfg->n_objcopy_flags);
    xr_free(cfg);
}

void xr_project_free(XrProject *project) {
    if (!project)
        return;

    xr_free(project->root);
    xr_free(project->name);
    xr_free(project->main);
    xr_free(project->version);
    xr_free(project->description);
    xr_free(project->license);

    if (project->dependencies) {
        // Free all dependency entries by iterating over hashmap entries
        XrHashMap *map = project->dependencies;
        for (uint32_t i = 0; i < map->capacity; i++) {
            if (map->entries[i].key != NULL) {
                free_dependency((XrDependency *) map->entries[i].value);
            }
        }
        xr_hashmap_free(project->dependencies);
    }

    if (project->targets) {
        XrHashMap *map = project->targets;
        for (uint32_t i = 0; i < map->capacity; i++) {
            if (map->entries[i].key != NULL) {
                free_target_config((XrTargetConfig *) map->entries[i].value);
            }
        }
        xr_hashmap_free(project->targets);
    }

    xr_free(project);
}

/* ========== Local Dependency Resolution ========== */

char *xr_resolve_local_dependency(XrProject *project, const char *package_name) {
    if (!project || !package_name || !project->dependencies) {
        return NULL;
    }

    XrDependency *dep = (XrDependency *) xr_hashmap_get(project->dependencies, package_name);
    if (!dep || !dep->is_local || !dep->path) {
        return NULL;
    }

    if (dep->path[0] == '/') {
        return xr_strdup(dep->path);
    }

    return xr_path_join(project->root, dep->path);
}

const XrTargetConfig *xr_project_find_target_config(const XrProject *project,
                                                    const char *target_name) {
    if (!project || !target_name || !project->targets)
        return NULL;
    return (const XrTargetConfig *) xr_hashmap_get(project->targets, target_name);
}

/* ========== File Collection Utilities ========== */

/*
 * Internal recursive file collector.
 */
static bool collect_files_recursive(const char *dir_path, char ***files, int *count,
                                    int *capacity) {
    XrDirIter *it = xr_dir_open(dir_path);
    if (!it)
        return false;

    XrDirEntry e;
    while (xr_dir_next(it, &e)) {
        char *full_path = xr_path_join(dir_path, e.name);
        if (!full_path)
            continue;

        if (e.is_dir) {
            // Recursively collect from subdirectory
            collect_files_recursive(full_path, files, count, capacity);
            xr_free(full_path);
        } else {
            // Check if it's a .xr file
            size_t name_len = strlen(e.name);
            if (name_len > 3 && strcmp(e.name + name_len - 3, ".xr") == 0) {
                // Expand array if needed
                if (*count >= *capacity) {
                    int new_cap = *capacity * 2;
                    char **new_files = (char **) xr_realloc(*files, sizeof(char *) * new_cap);
                    if (!new_files) {
                        xr_free(full_path);
                        xr_dir_close(it);
                        return false;
                    }
                    *files = new_files;
                    *capacity = new_cap;
                }
                (*files)[*count] = full_path;
                (*count)++;
            } else {
                xr_free(full_path);
            }
        }
    }

    xr_dir_close(it);
    return true;
}

bool xr_project_collect_files(const char *dir_path, char ***files, int *count) {
    if (!dir_path || !files || !count)
        return false;

    *files = NULL;
    *count = 0;

    int capacity = 16;
    *files = (char **) xr_malloc(sizeof(char *) * capacity);
    if (!*files)
        return false;

    return collect_files_recursive(dir_path, files, count, &capacity);
}

void xr_project_free_files(char **files, int count) {
    if (!files)
        return;

    for (int i = 0; i < count; i++) {
        xr_free(files[i]);
    }
    xr_free(files);
}
