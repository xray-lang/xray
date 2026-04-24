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
#include "../runtime/xisolate_api.h"
#include "../base/xmalloc.h"
#include "../base/xfileio.h"
#include "../base/xhashmap.h"
#if defined(XR_HAS_DATA_FORMATS) || !defined(XR_STDLIB_MODULAR)
#include "../../stdlib/toml/toml.h"
#endif
#include "../runtime/object/xmap.h"
#include "../runtime/object/xarray.h"
#include "../runtime/object/xstring.h"
#include "../runtime/object/xiterator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

/* ========== Helper Functions ========== */
static char* join_path(const char *dir, const char *file) {
    XR_DCHECK(dir != NULL && file != NULL, "join_path: NULL argument");
    size_t dir_len = strlen(dir);
    size_t file_len = strlen(file);

    while (dir_len > 0 && dir[dir_len - 1] == '/') {
        dir_len--;
    }

    char *result = (char*)xr_malloc(dir_len + 1 + file_len + 1);
    if (!result) return NULL;
    memcpy(result, dir, dir_len);
    result[dir_len] = '/';
    memcpy(result + dir_len + 1, file, file_len);
    result[dir_len + 1 + file_len] = '\0';

    return result;
}

static char* get_string_from_map(XrayIsolate *isolate, XrMap *map, const char *key) {
    XrString *key_str = xr_string_intern(isolate, key, strlen(key), 0);
    bool found = false;
    XrValue val = xr_map_get(map, xr_string_value(key_str), &found);

    if (found && XR_IS_STRING(val)) {
        XrString *str = (XrString*)XR_TO_PTR(val);
        return xr_strdup(str->data);
    }

    return NULL;
}

/* ========== Project Loading ========== */

XrProject* xr_project_load(XrayIsolate *isolate, const char *project_root) {
    if (!isolate || !project_root) return NULL;

    char *toml_path = join_path(project_root, "xray.toml");
    if (!toml_path) return NULL;

    size_t content_size;
    char *content = xr_file_read_all(toml_path, "r", &content_size);
    xr_free(toml_path);
    if (!content) return NULL;
    long size = (long)content_size;

#if !defined(XR_HAS_DATA_FORMATS) && defined(XR_STDLIB_MODULAR)
    // TOML parsing not available in minimal build
    xr_free(content);
    return NULL;
#else
    XrValue parsed = xr_toml_parse(isolate, content, size);
    xr_free(content);

    if (XR_IS_NULL(parsed) || !XR_IS_MAP(parsed)) {
        return NULL;
    }

    XrMap *root_map = (XrMap*)XR_TO_PTR(parsed);

    XrProject *project = (XrProject*)xr_malloc(sizeof(XrProject));
    memset(project, 0, sizeof(XrProject));

    project->root = xr_strdup(project_root);
    project->dependencies = xr_hashmap_new();

    XrString *project_key = xr_string_intern(isolate, "project", 7, 0);
    XrString *package_key = xr_string_intern(isolate, "package", 7, 0);

    bool found = false;
    XrValue section = xr_map_get(root_map, xr_string_value(project_key), &found);
    if (!found) {
        section = xr_map_get(root_map, xr_string_value(package_key), &found);
        if (found) {
            project->is_package = true;
        }
    }

    if (found && XR_IS_MAP(section)) {
        XrMap *section_map = (XrMap*)XR_TO_PTR(section);
        project->name = get_string_from_map(isolate, section_map, "name");
        project->main = get_string_from_map(isolate, section_map, "main");
        if (project->is_package) {
            project->version = get_string_from_map(isolate, section_map, "version");
            project->description = get_string_from_map(isolate, section_map, "description");
            project->license = get_string_from_map(isolate, section_map, "license");
        }
    }

    // Parse [dependencies] section
    XrString *deps_key = xr_string_intern(isolate, "dependencies", 12, 0);
    XrValue deps_section = xr_map_get(root_map, xr_string_value(deps_key), &found);

    if (found && XR_IS_MAP(deps_section)) {
        XrMap *deps_map = (XrMap*)XR_TO_PTR(deps_section);

        // Iterate over all dependencies using entries iterator
        XrIterator *iter = xr_map_entries_iterator(isolate, deps_map);
        if (iter) {
            while (xr_iterator_has_next(iter)) {
                XrValue entry = xr_iterator_next(iter);
                if (!XR_IS_ARRAY(entry)) continue;

                XrArray *pair = (XrArray*)XR_TO_PTR(entry);
                if (pair->length < 2) continue;

                XrValue key_val = ((XrValue*)pair->data)[0];
                XrValue val = ((XrValue*)pair->data)[1];

                if (!XR_IS_STRING(key_val)) continue;
                XrString *key_str = (XrString*)XR_TO_PTR(key_val);
                const char *dep_name = key_str->data;

                XrDependency *dep = (XrDependency*)xr_malloc(sizeof(XrDependency));
                if (!dep) continue;
                memset(dep, 0, sizeof(XrDependency));

                dep->name = xr_strdup(dep_name);

                if (XR_IS_STRING(val)) {
                    // Simple version string: "^1.0.0"
                    XrString *ver_str = (XrString*)XR_TO_PTR(val);
                    dep->version = xr_strdup(ver_str->data);
                    dep->is_local = false;
                } else if (XR_IS_MAP(val)) {
                    // Complex dependency: { version = "^1.0.0", path = "./local" }
                    XrMap *dep_map = (XrMap*)XR_TO_PTR(val);
                    dep->version = get_string_from_map(isolate, dep_map, "version");
                    dep->path = get_string_from_map(isolate, dep_map, "path");
                    dep->is_local = (dep->path != NULL);
                }

                xr_hashmap_set(project->dependencies, dep_name, dep);
            }
        }
    }

    project->initialized = true;

    return project;
#endif
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

    return join_path(project->root, dep->path);
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

        char *full_path = join_path(dir_path, entry->d_name);
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
