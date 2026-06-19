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
#include "../common.h"
#include "../ctxbuf.h"
#include "../../src/runtime/object/xmap.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/base/xplatform.h"
#include "../../src/base/xchecks.h"
#include "../../src/shared/xr_path_core.h"
#include "../../src/coro/xcoroutine.h"  // xr_current_coro
#include <limits.h>
#include "../../src/os/os_fs.h"

/* ========== Platform Definitions ========== */

/* Output always uses '/' for portability; input parsing accepts both on Windows. */
#define PATH_SEP '/'
#define PATH_SEP_STR "/"
#ifdef XR_OS_WINDOWS
#define PATH_DELIMITER ";"
#define IS_SEP(c) ((c) == '/' || (c) == '\\')
#else
#define PATH_DELIMITER ":"
#define IS_SEP(c) ((c) == '/')
#endif

/* ========== Helper Functions ========== */

// Create string from buffer with specified length.
static inline XrValue make_string_n(XrayIsolate *X, const char *s, size_t len) {
    if (!s || len == 0)
        return xrs_string_value_c(X, "");
    return xrs_string_value_n(X, s, len);
}

/* ========== Path Operations ========== */

// join(...) - Join multiple path segments
static XrValue path_join(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xrs_string_value_c(X, "");

    // Calculate max result length
    size_t total_len = 0;
    for (int i = 0; i < argc; i++) {
        const char *s = xrs_string_arg(args[i], NULL);
        if (s)
            total_len += strlen(s) + 1;
    }

    if (total_len == 0)
        return xrs_string_value_c(X, "");

    char *result = (char *) xr_malloc(total_len + 1);
    if (!result)
        return xr_null();

    size_t pos = 0;
    XR_DCHECK(total_len > 0, "path_join: total_len already validated > 0");

    for (int i = 0; i < argc; i++) {
        const char *part = xrs_string_arg(args[i], NULL);
        if (!part || part[0] == '\0')
            continue;

        // Check absolute path FIRST, before adding separator
#ifdef XR_OS_WINDOWS
        bool is_abs = IS_SEP(part[0]) ||
                      (((part[0] >= 'A' && part[0] <= 'Z') || (part[0] >= 'a' && part[0] <= 'z')) &&
                       part[1] == ':');
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

    XrValue ret = xrs_string_value_c(X, result);
    xr_free(result);
    return ret;
}

// dirname core - returns a slice of `path` (or a static literal). Shared by the
// VM binding and the AOT shim (115 single-definition). Borrows: the returned
// pointer aliases `path` or static storage; the caller copies it.
static const char *path_dirname_core(const char *path, size_t len, size_t *out_len) {
    return xr_path_core_dirname(path, len, out_len);
}

// dirname(path) - Get directory part
static XrValue path_dirname(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xrs_string_value_c(X, ".");
    size_t plen = 0;
    const char *path = xrs_string_arg(args[0], &plen);
    if (!path)
        return xrs_string_value_c(X, ".");
    size_t rl = 0;
    const char *r = path_dirname_core(path, plen, &rl);
    return make_string_n(X, r, rl);
}

// AOT direct-call shim: returns the dirname slice (borrowed) as (data, length);
// generated C copies it into an AOT string.
XR_FUNC const char *xr_aot_path_dirname(const char *path, int64_t len, int64_t *out_len) {
    size_t rl = 0;
    const char *r = path_dirname_core(path, len < 0 ? 0 : (size_t) len, &rl);
    *out_len = (int64_t) rl;
    return r;
}

// basename core - returns a slice of `path`. Shared by VM binding + AOT shim.
static const char *path_basename_core(const char *path, size_t len, size_t *out_len) {
    return xr_path_core_basename(path, len, out_len);
}

// basename(path) - Get filename part
static XrValue path_basename(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xrs_string_value_c(X, "");
    size_t plen = 0;
    const char *path = xrs_string_arg(args[0], &plen);
    if (!path)
        return xrs_string_value_c(X, "");
    size_t rl = 0;
    const char *r = path_basename_core(path, plen, &rl);
    return make_string_n(X, r, rl);
}

// AOT direct-call shim for path.basename (borrowed slice).
XR_FUNC const char *xr_aot_path_basename(const char *path, int64_t len, int64_t *out_len) {
    size_t rl = 0;
    const char *r = path_basename_core(path, len < 0 ? 0 : (size_t) len, &rl);
    *out_len = (int64_t) rl;
    return r;
}

// extname core - returns a slice of `path` from the last dot to the end, or "".
// The slice runs to the input end (matches the historical NUL-terminated
// return), so `plen` must be the full string length. Shared by VM + AOT.
static const char *path_extname_core(const char *path, size_t plen, size_t *out_len) {
    return xr_path_core_extname(path, plen, out_len);
}

// extname(path) - Get file extension
static XrValue path_extname(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xrs_string_value_c(X, "");
    size_t plen = 0;
    const char *path = xrs_string_arg(args[0], &plen);
    if (!path)
        return xrs_string_value_c(X, "");
    size_t rl = 0;
    const char *r = path_extname_core(path, plen, &rl);
    return make_string_n(X, r, rl);
}

// AOT direct-call shim for path.extname (borrowed slice).
XR_FUNC const char *xr_aot_path_extname(const char *path, int64_t len, int64_t *out_len) {
    size_t rl = 0;
    const char *r = path_extname_core(path, len < 0 ? 0 : (size_t) len, &rl);
    *out_len = (int64_t) rl;
    return r;
}

// Core absolute-path test, shared by the tagged stdlib binding and the AOT
// direct-call shim so the two signatures can never drift (115 single-definition
// principle). Pure: no isolate, no allocation.
static bool path_is_absolute_raw(const char *path, size_t len) {
    return xr_path_core_is_absolute(path, len);
}

// isAbsolute(path) - Check if path is absolute
static XrValue path_isAbsolute(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    size_t len = 0;
    const char *path = xrs_string_arg(args[0], &len);
    return xr_bool(path_is_absolute_raw(path, len));
}

// AOT direct-call shim for `path.isAbsolute(s)`. Specialized signature: takes
// the raw string data/length (no tagged XrString marshalling, ABI-neutral
// across VM/AOT string representations) and returns a tagged bool. Generated
// AOT C calls this symbol directly (resolved from xray_core via dead-strip)
// instead of dispatching through the runtime module table.
XR_FUNC XrValue xr_aot_path_isAbsolute(const char *path, int64_t len) {
    return xr_bool(path_is_absolute_raw(path, len < 0 ? 0 : (size_t) len));
}

// normalize(path) - Normalize path (resolve . and ..)
// Thread-safe: no strtok. Zero-copy: uses offset array into original string.
static XrValue path_normalize(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xrs_string_value_c(X, ".");

    const char *path = xrs_string_arg(args[0], NULL);
    if (!path || path[0] == '\0')
        return xrs_string_value_c(X, ".");

    size_t len = strlen(path);
    int is_absolute = IS_SEP(path[0]);

    // Offset array: each entry is (start, length) pair into original path.
    // Max segments = len/2 + 1 (e.g. "a/b/c").
    size_t max_segs = len / 2 + 2;
    size_t *seg_buf = (size_t *) xr_malloc(sizeof(size_t) * max_segs * 2);
    if (!seg_buf)
        return xr_null();
    size_t *seg_starts = seg_buf;
    size_t *seg_lens = seg_buf + max_segs;
    int seg_count = 0;

    // Manual tokenization (thread-safe, no strtok)
    size_t i = 0;
    while (i < len) {
        // Skip separators
        while (i < len && IS_SEP(path[i]))
            i++;
        if (i >= len)
            break;

        // Find segment end
        size_t seg_start = i;
        while (i < len && !IS_SEP(path[i]))
            i++;
        size_t seg_len = i - seg_start;

        if (seg_len == 1 && path[seg_start] == '.') {
            // Skip "."
        } else if (seg_len == 2 && path[seg_start] == '.' && path[seg_start + 1] == '.') {
            if (seg_count > 0 &&
                !(seg_lens[seg_count - 1] == 2 && path[seg_starts[seg_count - 1]] == '.' &&
                  path[seg_starts[seg_count - 1] + 1] == '.')) {
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
    char *result = (char *) xr_malloc(len + 2);
    if (!result) {
        xr_free(seg_buf);
        return xr_null();
    }

    size_t pos = 0;
    if (is_absolute) {
        result[pos++] = '/';
    }

    for (int s = 0; s < seg_count; s++) {
        if (s > 0)
            result[pos++] = '/';
        memcpy(result + pos, path + seg_starts[s], seg_lens[s]);
        pos += seg_lens[s];
    }

    if (pos == 0) {
        result[0] = '.';
        pos = 1;
    }
    result[pos] = '\0';

    XrValue ret = xrs_string_value_c(X, result);
    xr_free(result);
    xr_free(seg_buf);
    return ret;
}

// resolve(...) - Resolve to absolute path.
//
// Unlike the previous implementation this grows dynamically and accepts any
// path length, and it recognises the platform-specific separator set
// (backslash on Windows) through IS_SEP().
static XrValue path_resolve(XrayIsolate *X, XrValue *args, int argc) {
    char cwd[XR_PATH_MAX];
    if (xr_fs_getcwd(cwd, sizeof(cwd)) == NULL) {
        cwd[0] = PATH_SEP;
        cwd[1] = '\0';
    }

    XrCtxBuf result;
    xr_ctxbuf_init(&result, 256);
    XR_DCHECK(result.data != NULL, "path_resolve: ctxbuf init must succeed");
    xr_ctxbuf_append_cstr(&result, cwd);

    for (int i = 0; i < argc; i++) {
        const char *part = xrs_string_arg(args[i], NULL);
        if (!part || part[0] == '\0')
            continue;

        if (IS_SEP(part[0])) {
            // Absolute path resets the accumulated buffer.
            result.len = 0;
            if (result.data)
                result.data[0] = '\0';
            xr_ctxbuf_append_cstr(&result, part);
        } else {
            // Ensure a single separator between segments.
            if (result.len > 0 && !IS_SEP(result.data[result.len - 1])) {
                xr_ctxbuf_putc(&result, PATH_SEP);
            }
            xr_ctxbuf_append_cstr(&result, part);
        }
    }

    // Normalize result using the existing path_normalize binding.
    XrValue path_val = xrs_string_value_n(X, result.data ? result.data : "", result.len);
    xr_ctxbuf_free(&result);
    return path_normalize(X, &path_val, 1);
}

// relative(from, to) - Compute relative path
// Fixed: segment-boundary-aware common prefix (avoids /foo vs /foobar mismatch)
static XrValue path_relative(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 2)
        return xrs_string_value_c(X, "");

    const char *from = xrs_string_arg(args[0], NULL);
    const char *to = xrs_string_arg(args[1], NULL);

    if (!from || !to)
        return xrs_string_value_c(X, "");

    // Normalize both paths first
    XrValue from_norm = path_normalize(X, args, 1);
    XrValue to_norm = path_normalize(X, args + 1, 1);

    from = xrs_string_arg(from_norm, NULL);
    to = xrs_string_arg(to_norm, NULL);

    if (!from || !to)
        return xrs_string_value_c(X, "");

    // Find common prefix at segment boundary
    size_t common = 0;
    size_t last_sep = 0;
    while (from[common] && to[common] && from[common] == to[common]) {
        if (IS_SEP(from[common]))
            last_sep = common;
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
    while (*fp && IS_SEP(*fp))
        fp++;
    if (*fp) {
        up_count = 1;
        for (; *fp; fp++) {
            if (IS_SEP(*fp))
                up_count++;
        }
    }

    // Get 'to' remainder, skip leading separators
    const char *to_rest = to + common;
    while (*to_rest && IS_SEP(*to_rest))
        to_rest++;
    size_t to_rest_len = strlen(to_rest);

    // Calculate result size: up_count * 3 ("../" per entry) + to_rest_len
    size_t result_size = (up_count > 0 ? (size_t) up_count * 3 : 0) + to_rest_len + 2;
    char *result = (char *) xr_malloc(result_size);
    if (!result)
        return xr_null();

    size_t pos = 0;
    for (int i = 0; i < up_count; i++) {
        if (pos > 0)
            result[pos++] = '/';
        result[pos++] = '.';
        result[pos++] = '.';
    }

    if (to_rest_len > 0) {
        if (pos > 0)
            result[pos++] = '/';
        memcpy(result + pos, to_rest, to_rest_len);
        pos += to_rest_len;
    }

    result[pos] = '\0';

    XrValue ret;
    if (pos == 0) {
        ret = xrs_string_value_c(X, ".");
    } else {
        ret = xrs_string_value_c(X, result);
    }
    xr_free(result);
    return ret;
}

// parse(path) - Parse path into a PathInfo handle (Json with fixed shape:
// root, dir, base, name, ext). Returns Json so user code uses .field access
// (e.g. `let p = path.parse(s); p.dir`) instead of `.get("dir")`.
static XrValue path_parse(XrayIsolate *X, XrValue *args, int argc) {
    // Short-circuit on bad input — return an empty Json with the same five
    // fields populated as empty strings, so downstream `.field` access on
    // the result never NPEs.
    XrJson *json = xr_json_new(xr_current_coro(X));
    if (!json)
        return XR_NULL_VAL;

    if (argc < 1 || !XR_IS_STRING(args[0])) {
        XrValue empty = xrs_string_value_c(X, "");
        xr_json_set_by_key(X, json, "root", empty);
        xr_json_set_by_key(X, json, "dir", empty);
        xr_json_set_by_key(X, json, "base", empty);
        xr_json_set_by_key(X, json, "name", empty);
        xr_json_set_by_key(X, json, "ext", empty);
        return xr_json_value(json);
    }

    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        path = "";

    // Get each part
    XrValue dir = path_dirname(X, args, 1);
    XrValue base = path_basename(X, args, 1);
    XrValue ext = path_extname(X, args, 1);

    // name = basename without ext
    const char *base_str = xrs_string_arg(base, NULL);
    const char *ext_str = xrs_string_arg(ext, NULL);
    size_t base_len = base_str ? strlen(base_str) : 0;
    size_t ext_len = ext_str ? strlen(ext_str) : 0;
    XrValue name = make_string_n(X, base_str, base_len - ext_len);

    // root
    XrValue root = xrs_string_value_c(X, "");
    if (path[0] == '/') {
        root = xrs_string_value_c(X, "/");
    }

    xr_json_set_by_key(X, json, "root", root);
    xr_json_set_by_key(X, json, "dir", dir);
    xr_json_set_by_key(X, json, "base", base);
    xr_json_set_by_key(X, json, "name", name);
    xr_json_set_by_key(X, json, "ext", ext);

    return xr_json_value(json);
}

// format(obj) - Build path from a PathInfo Json.
//
// Uses a dynamic buffer so arbitrarily long paths are preserved, and honours
// PATH_SEP rather than hard-coding '/'. That avoids mixed-separator output
// on Windows, e.g. `C:\foo/bar`.
static XrValue path_format(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xrs_string_value_c(X, "");
    if (!xr_value_is_json(args[0]))
        return xrs_string_value_c(X, "");

    XrJson *json = xr_value_to_json(args[0]);

    XrValue dir = xr_json_get_by_key(X, json, "dir");
    XrValue base = xr_json_get_by_key(X, json, "base");
    XrValue name = xr_json_get_by_key(X, json, "name");
    XrValue ext = xr_json_get_by_key(X, json, "ext");

    // Derive `base` from name+ext when only those are present.
    XrCtxBuf base_buf;
    xr_ctxbuf_init(&base_buf, 64);
    const char *base_str = xrs_string_arg(base, NULL);
    if (!base_str || base_str[0] == '\0') {
        const char *name_str = xrs_string_arg(name, NULL);
        const char *ext_str = xrs_string_arg(ext, NULL);
        if (name_str && name_str[0] != '\0') {
            xr_ctxbuf_append_cstr(&base_buf, name_str);
            if (ext_str)
                xr_ctxbuf_append_cstr(&base_buf, ext_str);
            base_str = base_buf.data;
        }
    }

    const char *dir_str = xrs_string_arg(dir, NULL);
    if (dir_str && dir_str[0] != '\0') {
        XrCtxBuf out;
        xr_ctxbuf_init(&out, 128);
        xr_ctxbuf_append_cstr(&out, dir_str);
        // Avoid duplicating the separator if `dir` already ends with one.
        if (out.len > 0 && !IS_SEP(out.data[out.len - 1])) {
            xr_ctxbuf_putc(&out, PATH_SEP);
        }
        if (base_str)
            xr_ctxbuf_append_cstr(&out, base_str);
        xr_ctxbuf_free(&base_buf);
        XrValue v = xrs_string_value_n(X, out.data ? out.data : "", out.len);
        xr_ctxbuf_free(&out);
        return v;
    }

    XrValue v =
        (base_str && base_str[0]) ? xrs_string_value_c(X, base_str) : xrs_string_value_c(X, "");
    xr_ctxbuf_free(&base_buf);
    return v;
}

/* ========== Module Loading ========== */

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module path
// @handle PathInfo { const root: string, const dir: string, const base: string, const name: string,
// const ext: string }

XR_DEFINE_BUILTIN(path_join, "join", "(...parts: string): string", "Join path segments")
XR_DEFINE_BUILTIN(path_dirname, "dirname", "(path: string): string", "Get directory name")
XR_DEFINE_BUILTIN(path_basename, "basename", "(path: string): string", "Get base name")
XR_DEFINE_BUILTIN(path_extname, "extname", "(path: string): string", "Get file extension")
XR_DEFINE_BUILTIN(path_normalize, "normalize", "(path: string): string",
                  "Normalize path separators")
XR_DEFINE_BUILTIN(path_isAbsolute, "isAbsolute", "(path: string): bool",
                  "Check if path is absolute")
XR_DEFINE_BUILTIN(path_resolve, "resolve", "(...parts: string): string", "Resolve to absolute path")
XR_DEFINE_BUILTIN(path_relative, "relative", "(from: string, to: string): string",
                  "Get relative path")
XR_DEFINE_BUILTIN(path_parse, "parse", "(path: string): PathInfo",
                  "Parse path into components (root, dir, base, name, ext)")
XR_DEFINE_BUILTIN(path_format, "format", "(obj: PathInfo): string", "Format path from components")

XR_FUNC XrModule *xr_load_module_path(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_path: NULL isolate");

    XrModule *mod = xr_module_create_native(isolate, "path");
    if (!mod)
        return NULL;

    XRS_EXPORT(mod, isolate, "join", path_join);
    XRS_EXPORT(mod, isolate, "dirname", path_dirname);
    XRS_EXPORT(mod, isolate, "basename", path_basename);
    XRS_EXPORT(mod, isolate, "extname", path_extname);
    XRS_EXPORT(mod, isolate, "normalize", path_normalize);
    XRS_EXPORT(mod, isolate, "isAbsolute", path_isAbsolute);
    XRS_EXPORT(mod, isolate, "resolve", path_resolve);
    XRS_EXPORT(mod, isolate, "relative", path_relative);
    XRS_EXPORT(mod, isolate, "parse", path_parse);
    XRS_EXPORT(mod, isolate, "format", path_format);

    // Add constants
    xr_module_add_export(isolate, mod, "sep", xrs_string_value_c(isolate, PATH_SEP_STR));
    xr_module_add_export(isolate, mod, "delimiter", xrs_string_value_c(isolate, PATH_DELIMITER));

    // Mark as loaded
    mod->loaded = true;
    return mod;
}
