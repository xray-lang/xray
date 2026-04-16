/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * path.c - Path manipulation module implementation
 *
 * KEY CONCEPT:
 *   Cross-platform path operations following POSIX/Windows conventions.
 *   Zero-copy normalize via offset array. Thread-safe (no strtok).
 */

#include "path.h"
#include "../../src/vm/xvm_internal.h"
#include "../../include/xray_platform.h"

#ifndef XR_PLATFORM_WINDOWS
#include <unistd.h>
#endif

/* ========== Platform Definitions ========== */

#ifdef XR_PLATFORM_WINDOWS
#define PATH_SEP '\\'
#define PATH_SEP_STR "\\"
#define PATH_DELIMITER ";"
#define IS_SEP(c) ((c) == '/' || (c) == '\\')
#else
#define PATH_SEP '/'
#define PATH_SEP_STR "/"
#define PATH_DELIMITER ":"
#define IS_SEP(c) ((c) == '/')
#endif

/* ========== Helper Functions ========== */

// Get string argument
static const char* get_string_arg(XrValue v) {
    if (!XR_IS_STRING(v)) return NULL;
    return XR_TO_STRING(v)->data;
}

// Create string value
static XrValue make_string(XrayIsolate *X, const char *s) {
    if (!s) return xr_null();
    XrString *str = xr_string_intern(X, s, strlen(s), 0);
    return xr_string_value(str);
}

// Create string from buffer with specified length
static XrValue make_string_n(XrayIsolate *X, const char *s, size_t len) {
    if (!s || len == 0) return make_string(X, "");
    XrString *str = xr_string_intern(X, s, len, 0);
    return xr_string_value(str);
}

/* ========== Path Operations ========== */

// join(...) - Join multiple path segments
static XrValue path_join(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1) return make_string(X, "");
    
    // Calculate max result length
    size_t total_len = 0;
    for (int i = 0; i < argc; i++) {
        const char *s = get_string_arg(args[i]);
        if (s) total_len += strlen(s) + 1;
    }
    
    if (total_len == 0) return make_string(X, "");
    
    char *result = (char*)xr_malloc(total_len + 1);
    if (!result) return xr_null();
    
    size_t pos = 0;
    
    for (int i = 0; i < argc; i++) {
        const char *part = get_string_arg(args[i]);
        if (!part || part[0] == '\0') continue;
        
        // Check absolute path FIRST, before adding separator
#ifdef XR_PLATFORM_WINDOWS
        bool is_abs = IS_SEP(part[0]) || 
            (((part[0] >= 'A' && part[0] <= 'Z') || (part[0] >= 'a' && part[0] <= 'z')) && part[1] == ':');
#else
        bool is_abs = (part[0] == '/');
#endif
        if (pos > 0 && is_abs) {
            pos = 0;
        }
        
        // Add separator if needed
        if (pos > 0 && !IS_SEP(result[pos - 1]) && !IS_SEP(part[0])) {
            result[pos++] = PATH_SEP;
        }
        
        // Skip leading separator if already have content
        if (pos > 0 && IS_SEP(part[0])) {
            part++;
        }
        
        size_t part_len = strlen(part);
        memcpy(result + pos, part, part_len);
        pos += part_len;
    }
    
    result[pos] = '\0';
    
    XrValue ret = make_string(X, result);
    xr_free(result);
    return ret;
}

// dirname(path) - Get directory part
static XrValue path_dirname(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1) return make_string(X, ".");
    
    const char *path = get_string_arg(args[0]);
    if (!path || path[0] == '\0') return make_string(X, ".");
    
    size_t len = strlen(path);
    
    // Skip trailing separators
    while (len > 0 && IS_SEP(path[len - 1])) {
        len--;
    }
    
    // Find last separator
    while (len > 0 && !IS_SEP(path[len - 1])) {
        len--;
    }
    
    // Skip separators
    while (len > 1 && IS_SEP(path[len - 1])) {
        len--;
    }
    
    if (len == 0) {
        // No directory part
        return IS_SEP(path[0]) ? make_string(X, PATH_SEP_STR) : make_string(X, ".");
    }
    
    return make_string_n(X, path, len);
}

// basename(path) - Get filename part
static XrValue path_basename(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1) return make_string(X, "");
    
    const char *path = get_string_arg(args[0]);
    if (!path || path[0] == '\0') return make_string(X, "");
    
    size_t len = strlen(path);
    
    // Skip trailing separators
    while (len > 0 && IS_SEP(path[len - 1])) {
        len--;
    }
    
    if (len == 0) return make_string(X, "");
    
    // Find last separator
    size_t start = len;
    while (start > 0 && !IS_SEP(path[start - 1])) {
        start--;
    }
    
    return make_string_n(X, path + start, len - start);
}

// extname(path) - Get file extension
static XrValue path_extname(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1) return make_string(X, "");
    
    const char *path = get_string_arg(args[0]);
    if (!path || path[0] == '\0') return make_string(X, "");
    
    // Get basename first
    size_t len = strlen(path);
    while (len > 0 && IS_SEP(path[len - 1])) len--;
    
    size_t start = len;
    while (start > 0 && !IS_SEP(path[start - 1])) start--;
    
    const char *base = path + start;
    size_t base_len = len - start;
    
    // Find last dot in basename
    const char *dot = NULL;
    for (size_t i = base_len; i > 0; i--) {
        if (base[i - 1] == '.') {
            // Skip leading dot (e.g., .gitignore)
            if (i > 1) {
                dot = base + i - 1;
            }
            break;
        }
    }
    
    if (!dot) return make_string(X, "");
    
    return make_string(X, dot);
}

// isAbsolute(path) - Check if path is absolute
static XrValue path_isAbsolute(XrayIsolate *X, XrValue *args, int argc) {
    (void)X;
    if (argc < 1) return xr_bool(false);
    
    const char *path = get_string_arg(args[0]);
    if (!path || path[0] == '\0') return xr_bool(false);
    
#ifdef XR_PLATFORM_WINDOWS
    // Windows: drive letter or UNC path
    if ((path[0] >= 'A' && path[0] <= 'Z' && path[1] == ':') ||
        (path[0] >= 'a' && path[0] <= 'z' && path[1] == ':')) {
        return xr_bool(true);
    }
    if (path[0] == '\\' && path[1] == '\\') {
        return xr_bool(true);
    }
#endif
    
    return xr_bool(path[0] == '/');
}

// normalize(path) - Normalize path (resolve . and ..)
// Thread-safe: no strtok. Zero-copy: uses offset array into original string.
static XrValue path_normalize(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1) return make_string(X, ".");
    
    const char *path = get_string_arg(args[0]);
    if (!path || path[0] == '\0') return make_string(X, ".");
    
    size_t len = strlen(path);
    int is_absolute = IS_SEP(path[0]);
    
    // Offset array: each entry is (start, length) pair into original path.
    // Max segments = len/2 + 1 (e.g. "a/b/c").
    size_t max_segs = len / 2 + 2;
    size_t *seg_buf = (size_t*)xr_malloc(sizeof(size_t) * max_segs * 2);
    if (!seg_buf) return xr_null();
    size_t *seg_starts = seg_buf;
    size_t *seg_lens   = seg_buf + max_segs;
    int seg_count = 0;
    
    // Manual tokenization (thread-safe, no strtok)
    size_t i = 0;
    while (i < len) {
        // Skip separators
        while (i < len && IS_SEP(path[i])) i++;
        if (i >= len) break;
        
        // Find segment end
        size_t seg_start = i;
        while (i < len && !IS_SEP(path[i])) i++;
        size_t seg_len = i - seg_start;
        
        if (seg_len == 1 && path[seg_start] == '.') {
            // Skip "."
        } else if (seg_len == 2 && path[seg_start] == '.' && path[seg_start + 1] == '.') {
            if (seg_count > 0 && !(seg_lens[seg_count - 1] == 2 &&
                path[seg_starts[seg_count - 1]] == '.' && path[seg_starts[seg_count - 1] + 1] == '.')) {
                seg_count--;
            } else if (!is_absolute) {
                seg_starts[seg_count] = seg_start;
                seg_lens[seg_count] = seg_len;
                seg_count++;
            }
        } else {
            seg_starts[seg_count] = seg_start;
            seg_lens[seg_count] = seg_len;
            seg_count++;
        }
    }
    
    // Build result
    char *result = (char*)xr_malloc(len + 2);
    if (!result) {
        xr_free(seg_buf);
        return xr_null();
    }
    
    size_t pos = 0;
    if (is_absolute) {
        result[pos++] = '/';
    }
    
    for (int s = 0; s < seg_count; s++) {
        if (s > 0) result[pos++] = '/';
        memcpy(result + pos, path + seg_starts[s], seg_lens[s]);
        pos += seg_lens[s];
    }
    
    if (pos == 0) {
        result[0] = '.';
        pos = 1;
    }
    result[pos] = '\0';
    
    XrValue ret = make_string(X, result);
    xr_free(result);
    xr_free(seg_buf);
    return ret;
}

// resolve(...) - Resolve to absolute path
static XrValue path_resolve(XrayIsolate *X, XrValue *args, int argc) {
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        cwd[0] = '/';
        cwd[1] = '\0';
    }
    
    // Process arguments from left to right
    char result[4096];
    strncpy(result, cwd, sizeof(result) - 1);
    result[sizeof(result) - 1] = '\0';
    
    for (int i = 0; i < argc; i++) {
        const char *part = get_string_arg(args[i]);
        if (!part) continue;
        
        if (part[0] == '/') {
            // Absolute path, reset
            strncpy(result, part, sizeof(result) - 1);
        } else if (part[0] != '\0') {
            // Relative path, append
            size_t len = strlen(result);
            if (len > 0 && result[len - 1] != '/') {
                strncat(result, "/", sizeof(result) - len - 1);
            }
            strncat(result, part, sizeof(result) - strlen(result) - 1);
        }
    }
    
    // Normalize result
    XrValue path_val = make_string(X, result);
    return path_normalize(X, &path_val, 1);
}

// relative(from, to) - Compute relative path
// Fixed: segment-boundary-aware common prefix (avoids /foo vs /foobar mismatch)
static XrValue path_relative(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 2) return make_string(X, "");
    
    const char *from = get_string_arg(args[0]);
    const char *to = get_string_arg(args[1]);
    
    if (!from || !to) return make_string(X, "");
    
    // Normalize both paths first
    XrValue from_norm = path_normalize(X, args, 1);
    XrValue to_norm = path_normalize(X, args + 1, 1);
    
    from = get_string_arg(from_norm);
    to = get_string_arg(to_norm);
    
    if (!from || !to) return make_string(X, "");
    
    // Find common prefix at segment boundary
    size_t common = 0;
    size_t last_sep = 0;
    while (from[common] && to[common] && from[common] == to[common]) {
        if (IS_SEP(from[common])) last_sep = common;
        common++;
    }
    
    // Adjust to last complete segment boundary.
    // Common prefix is valid only if both sides end at a segment boundary:
    //   - both strings exhausted at same point, OR
    //   - the diverging char is a separator on at least one side
    if (from[common] == '\0' && to[common] == '\0') {
        // Identical paths
    } else if (from[common] == '\0' && IS_SEP(to[common])) {
        // from is prefix of to, at segment boundary
    } else if (to[common] == '\0' && IS_SEP(from[common])) {
        // to is prefix of from, at segment boundary
    } else {
        // Mid-segment divergence: roll back to last separator
        common = last_sep;
    }
    
    // Count ".." segments needed from 'from' remainder
    int up_count = 0;
    const char *fp = from + common;
    while (*fp && IS_SEP(*fp)) fp++;
    if (*fp) {
        up_count = 1;
        for (; *fp; fp++) {
            if (IS_SEP(*fp)) up_count++;
        }
    }
    
    // Get 'to' remainder, skip leading separators
    const char *to_rest = to + common;
    while (*to_rest && IS_SEP(*to_rest)) to_rest++;
    size_t to_rest_len = strlen(to_rest);
    
    // Calculate result size: up_count * 3 ("../" per entry) + to_rest_len
    size_t result_size = (up_count > 0 ? (size_t)up_count * 3 : 0) + to_rest_len + 2;
    char *result = (char*)xr_malloc(result_size);
    if (!result) return xr_null();
    
    size_t pos = 0;
    for (int i = 0; i < up_count; i++) {
        if (pos > 0) result[pos++] = '/';
        result[pos++] = '.';
        result[pos++] = '.';
    }
    
    if (to_rest_len > 0) {
        if (pos > 0) result[pos++] = '/';
        memcpy(result + pos, to_rest, to_rest_len);
        pos += to_rest_len;
    }
    
    result[pos] = '\0';
    
    XrValue ret;
    if (pos == 0) {
        ret = make_string(X, ".");
    } else {
        ret = make_string(X, result);
    }
    xr_free(result);
    return ret;
}

// parse(path) - Parse path into components
static XrValue path_parse(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1) {
        XrMap *map = xr_map_new(xr_current_coro(X));
        return xr_value_from_map(map);
    }
    
    const char *path = get_string_arg(args[0]);
    if (!path) path = "";
    
    XrMap *map = xr_map_new(xr_current_coro(X));
    
    // Get each part
    XrValue dir = path_dirname(X, args, 1);
    XrValue base = path_basename(X, args, 1);
    XrValue ext = path_extname(X, args, 1);
    
    // name = basename without ext
    const char *base_str = get_string_arg(base);
    const char *ext_str = get_string_arg(ext);
    size_t base_len = base_str ? strlen(base_str) : 0;
    size_t ext_len = ext_str ? strlen(ext_str) : 0;
    XrValue name = make_string_n(X, base_str, base_len - ext_len);
    
    // root
    XrValue root = make_string(X, "");
    if (path[0] == '/') {
        root = make_string(X, "/");
    }
    
    xr_map_set(map, make_string(X, "root"), root);
    xr_map_set(map, make_string(X, "dir"), dir);
    xr_map_set(map, make_string(X, "base"), base);
    xr_map_set(map, make_string(X, "name"), name);
    xr_map_set(map, make_string(X, "ext"), ext);
    
    return xr_value_from_map(map);
}

// format(obj) - Build path from components
static XrValue path_format(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1) return make_string(X, "");
    
    // Check if argument is a Map
    if (!XR_IS_MAP(args[0])) return make_string(X, "");
    
    XrMap *map = XR_TO_MAP(args[0]);
    bool found;
    
    // Get components
    XrValue dir = xr_map_get(map, make_string(X, "dir"), &found);
    XrValue base = xr_map_get(map, make_string(X, "base"), &found);
    XrValue name = xr_map_get(map, make_string(X, "name"), &found);
    XrValue ext = xr_map_get(map, make_string(X, "ext"), &found);
    
    // Use base if available, otherwise name + ext
    const char *base_str = get_string_arg(base);
    if (!base_str || base_str[0] == '\0') {
        const char *name_str = get_string_arg(name);
        const char *ext_str = get_string_arg(ext);
        
        if (name_str && name_str[0] != '\0') {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s%s", name_str, ext_str ? ext_str : "");
            base = make_string(X, buf);
            base_str = get_string_arg(base);
        }
    }
    
    // Join dir and base
    const char *dir_str = get_string_arg(dir);
    
    if (dir_str && dir_str[0] != '\0') {
        char buf[4096];
        snprintf(buf, sizeof(buf), "%s/%s", dir_str, base_str ? base_str : "");
        return make_string(X, buf);
    }
    
    return base_str ? make_string(X, base_str) : make_string(X, "");
}

/* ========== Module Loading ========== */

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module path

XR_DEFINE_BUILTIN(path_join, "join", "(...parts: string): string", "Join path segments")
XR_DEFINE_BUILTIN(path_dirname, "dirname", "(path: string): string", "Get directory name")
XR_DEFINE_BUILTIN(path_basename, "basename", "(path: string): string", "Get base name")
XR_DEFINE_BUILTIN(path_extname, "extname", "(path: string): string", "Get file extension")
XR_DEFINE_BUILTIN(path_normalize, "normalize", "(path: string): string", "Normalize path separators")
XR_DEFINE_BUILTIN(path_isAbsolute, "isAbsolute", "(path: string): bool", "Check if path is absolute")
XR_DEFINE_BUILTIN(path_resolve, "resolve", "(...parts: string): string", "Resolve to absolute path")
XR_DEFINE_BUILTIN(path_relative, "relative", "(from: string, to: string): string", "Get relative path")
XR_DEFINE_BUILTIN(path_parse, "parse", "(path: string): Json", "Parse path into components")
XR_DEFINE_BUILTIN(path_format, "format", "(obj: Json): string", "Format path from components")

XrModule* xr_load_module_path(XrayIsolate *isolate) {
    // Create native module
    XrModule *mod = xr_module_create_native(isolate, "path");
    if (!mod) return NULL;
    
    #define EXPORT_CFUNC(name_str, func_ptr) \
        do { \
            XrCFunction *cfunc = xr_vm_cfunction_new(isolate, func_ptr, name_str); \
            XrValue fn_val = xr_value_from_cfunction(cfunc); \
            xr_module_add_export(isolate, mod, name_str, fn_val); \
        } while(0)
    
    EXPORT_CFUNC("join", path_join);
    EXPORT_CFUNC("dirname", path_dirname);
    EXPORT_CFUNC("basename", path_basename);
    EXPORT_CFUNC("extname", path_extname);
    EXPORT_CFUNC("normalize", path_normalize);
    EXPORT_CFUNC("isAbsolute", path_isAbsolute);
    EXPORT_CFUNC("resolve", path_resolve);
    EXPORT_CFUNC("relative", path_relative);
    EXPORT_CFUNC("parse", path_parse);
    EXPORT_CFUNC("format", path_format);
    
    #undef EXPORT_CFUNC
    
    // Add constants
    xr_module_add_export(isolate, mod, "sep", make_string(isolate, PATH_SEP_STR));
    xr_module_add_export(isolate, mod, "delimiter", make_string(isolate, PATH_DELIMITER));
    
    // Mark as loaded
    mod->loaded = true;
    return mod;
}
