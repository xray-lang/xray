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
#include <dirent.h>

/* ========== Helper Functions ========== */

/* Get a strdup'd string from a TOML table by key, or NULL. */
static char *get_toml_str(XrTomlValue *tbl, const char *key) {
    const char *s = xtoml_get_string(tbl, key);
    return s ? xr_strdup(s) : NULL;
}

/* ========== Project Loading ========== */

XrProject* xr_project_load(XrayIsolate *isolate, const char *project_root) {
    (void)isolate; /* no longer needed — base xtoml parser is pure C */
    if (!project_root) return NULL;

    char *toml_path = xr_path_join(project_root, "xray.toml");
    if (!toml_path) return NULL;

    size_t content_size;
    char *content = xr_file_read_all(toml_path, "r", &content_size);
    xr_free(toml_path);
    if (!content) return NULL;

    XrTomlValue *root = xtoml_parse(content, content_size);
    xr_free(content);
    if (!root) return NULL;

    XrProject *project = (XrProject*)xr_calloc(1, sizeof(XrProject));
    if (!project) { xtoml_free(root); return NULL; }

    project->root = xr_strdup(project_root);
    project->dependencies = xr_hashmap_new();

    // Try [project], then [package]
    XrTomlValue *section = xtoml_get_table(root, "project");
    if (!section) {
        section = xtoml_get_table(root, "package");
        if (section) project->is_package = true;
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
    if (deps) {
        for (int i = 0; i < deps->as.table.count; i++) {
            XrTomlMember *m = &deps->as.table.members[i];
            XR_DCHECK(m->key != NULL, "TOML member key must not be NULL");

            XrDependency *dep = (XrDependency*)xr_calloc(1, sizeof(XrDependency));
            if (!dep) continue;
            dep->name = xr_strdup(m->key);

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

            xr_hashmap_set(project->dependencies, m->key, dep);
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
    if (!dep) return;
    xr_free(dep->name);
    xr_free(dep->version);
    xr_free(dep->path);
    xr_free(dep);
}

void xr_project_free(XrProject *project) {
    if (!project) return;

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
                free_dependency((XrDependency*)map->entries[i].value);
            }
        }
        xr_hashmap_free(project->dependencies);
    }

    xr_free(project);
}

/* ========== Local Dependency Resolution ========== */

char* xr_resolve_local_dependency(XrProject *project, const char *package_name) {
    if (!project || !package_name || !project->dependencies) {
        return NULL;
    }

    XrDependency *dep = (XrDependency*)xr_hashmap_get(project->dependencies, package_name);
    if (!dep || !dep->is_local || !dep->path) {
        return NULL;
    }

    if (dep->path[0] == '/') {
        return xr_strdup(dep->path);
    }

    return xr_path_join(project->root, dep->path);
}

/* ========== File Collection Utilities ========== */

/*
 * Internal recursive file collector.
 */
static bool collect_files_recursive(const char *dir_path, char ***files,
                                     int *count, int *capacity) {
    DIR *dir = opendir(dir_path);
    if (!dir) return false;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char *full_path = xr_path_join(dir_path, entry->d_name);
        if (!full_path) continue;

        struct stat st;
        if (stat(full_path, &st) != 0) {
            xr_free(full_path);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            // Recursively collect from subdirectory
            collect_files_recursive(full_path, files, count, capacity);
            xr_free(full_path);
        } else if (S_ISREG(st.st_mode)) {
            // Check if it's a .xr file
            size_t name_len = strlen(entry->d_name);
            if (name_len > 3 && strcmp(entry->d_name + name_len - 3, ".xr") == 0) {
                // Expand array if needed
                if (*count >= *capacity) {
                    int new_cap = *capacity * 2;
                    char **new_files = (char**)xr_realloc(*files, sizeof(char*) * new_cap);
                    if (!new_files) {
                        xr_free(full_path);
                        closedir(dir);
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
        } else {
            xr_free(full_path);
        }
    }

    closedir(dir);
    return true;
}

bool xr_project_collect_files(const char *dir_path, char ***files, int *count) {
    if (!dir_path || !files || !count) return false;

    *files = NULL;
    *count = 0;

    int capacity = 16;
    *files = (char**)xr_malloc(sizeof(char*) * capacity);
    if (!*files) return false;

    return collect_files_recursive(dir_path, files, count, &capacity);
}

void xr_project_free_files(char **files, int count) {
    if (!files) return;

    for (int i = 0; i < count; i++) {
        xr_free(files[i]);
    }
    xr_free(files);
}
