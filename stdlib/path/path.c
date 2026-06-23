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
#ifdef XR_OS_WINDOWS
#define IS_SEP(c) ((c) == '/' || (c) == '\\')
#else
#define IS_SEP(c) ((c) == '/')
#endif

/* ========== Helper Functions ========== */

// Create string from buffer with specified length.
static inline XrValue make_string_n(XrVMRuntime *X, const char *s, size_t len) {
    if (!s || len == 0)
        return xrs_string_value_c(X, "");
    return xrs_string_value_n(X, s, len);
}

static inline XrValue make_string_slice(XrVMRuntime *X, XrPathCoreSlice slice) {
    return make_string_n(X, slice.data, slice.len);
}

/* ========== Path Operations ========== */

// join(...) - Join multiple path segments
static XrValue path_join(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1)
        return xrs_string_value_c(X, "");

    enum {
        PATH_JOIN_STACK_PARTS = 16
    };
    const char *stack_parts[PATH_JOIN_STACK_PARTS];
    size_t stack_lens[PATH_JOIN_STACK_PARTS];
    const char **parts = stack_parts;
    size_t *lens = stack_lens;
    void *heap_buf = NULL;
    if (argc > PATH_JOIN_STACK_PARTS) {
        size_t count = (size_t) argc;
        heap_buf = xr_malloc(sizeof(char *) * count + sizeof(size_t) * count);
        if (!heap_buf)
            return xr_null();
        parts = (const char **) heap_buf;
        lens = (size_t *) (parts + count);
    }
    for (int i = 0; i < argc; i++) {
        size_t len = 0;
        parts[i] = xrs_string_arg(args[i], &len);
        lens[i] = parts[i] ? len : 0;
    }

    size_t out_len = 0;
    if (!xr_path_core_join_len(parts, lens, (size_t) argc, &out_len)) {
        xr_free(heap_buf);
        return xr_null();
    }
    if (out_len == 0) {
        xr_free(heap_buf);
        return xrs_string_value_c(X, "");
    }

    char *result = (char *) xr_malloc(out_len + 1);
    if (!result) {
        xr_free(heap_buf);
        return xr_null();
    }
    xr_path_core_join_write(parts, lens, (size_t) argc, result);

    XrValue ret = xrs_string_value_n(X, result, out_len);
    xr_free(result);
    xr_free(heap_buf);
    return ret;
}

// dirname core - returns a slice of `path` (or a static literal). Borrows: the
// returned pointer aliases `path` or static storage; the caller copies it.
static const char *path_dirname_core(const char *path, size_t len, size_t *out_len) {
    return xr_path_core_dirname(path, len, out_len);
}

// dirname(path) - Get directory part
static XrValue path_dirname(XrVMRuntime *X, XrValue *args, int argc) {
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

// basename core - returns a slice of `path`.
static const char *path_basename_core(const char *path, size_t len, size_t *out_len) {
    return xr_path_core_basename(path, len, out_len);
}

// basename(path) - Get filename part
static XrValue path_basename(XrVMRuntime *X, XrValue *args, int argc) {
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

// extname core - returns a slice of `path` from the last dot to the end, or "".
// The slice runs to the input end (matches the historical NUL-terminated
// return), so `plen` must be the full string length.
static const char *path_extname_core(const char *path, size_t plen, size_t *out_len) {
    return xr_path_core_extname(path, plen, out_len);
}

// extname(path) - Get file extension
static XrValue path_extname(XrVMRuntime *X, XrValue *args, int argc) {
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

// Core absolute-path test. Pure: no isolate, no allocation.
static bool path_is_absolute_raw(const char *path, size_t len) {
    return xr_path_core_is_absolute(path, len);
}

// isAbsolute(path) - Check if path is absolute
static XrValue path_isAbsolute(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    size_t len = 0;
    const char *path = xrs_string_arg(args[0], &len);
    return xr_bool(path_is_absolute_raw(path, len));
}

static bool path_normalize_alloc(const char *path, size_t len, char **out, size_t *out_len) {
    if (!out || !out_len)
        return false;
    *out = NULL;
    *out_len = 0;
    size_t max_segs = xr_path_core_normalize_segment_cap(len);
    size_t *seg_buf = (size_t *) xr_malloc(sizeof(size_t) * max_segs * 2);
    if (!seg_buf)
        return false;
    size_t *seg_starts = seg_buf;
    size_t *seg_lens = seg_buf + max_segs;
    size_t seg_count = 0;
    bool is_absolute = false;
    if (!xr_path_core_normalize_plan(path, len, seg_starts, seg_lens, max_segs, &seg_count,
                                     &is_absolute, out_len)) {
        xr_free(seg_buf);
        return false;
    }

    char *result = (char *) xr_malloc(*out_len + 1);
    if (!result) {
        xr_free(seg_buf);
        return false;
    }
    xr_path_core_normalize_write(path, seg_starts, seg_lens, seg_count, is_absolute, result);
    xr_free(seg_buf);
    *out = result;
    return true;
}

// normalize(path) - Normalize path (resolve . and ..)
static XrValue path_normalize(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1)
        return xrs_string_value_c(X, ".");

    size_t len = 0;
    const char *path = xrs_string_arg(args[0], &len);
    if (!path)
        return xrs_string_value_c(X, ".");

    char *result = NULL;
    size_t out_len = 0;
    if (!path_normalize_alloc(path, len, &result, &out_len))
        return xr_null();

    XrValue ret = xrs_string_value_n(X, result, out_len);
    xr_free(result);
    return ret;
}

static XrValue path_resolve_join_normalized(XrVMRuntime *X, const char **parts, const size_t *lens,
                                            size_t count) {
    size_t joined_len = 0;
    if (!xr_path_core_join_len(parts, lens, count, &joined_len))
        return xr_null();
    char *joined = (char *) xr_malloc(joined_len + 1);
    if (!joined)
        return xr_null();
    xr_path_core_join_write(parts, lens, count, joined);

    char *normalized = NULL;
    size_t normalized_len = 0;
    if (!path_normalize_alloc(joined, joined_len, &normalized, &normalized_len)) {
        xr_free(joined);
        return xr_null();
    }
    XrValue ret = xrs_string_value_n(X, normalized, normalized_len);
    xr_free(normalized);
    xr_free(joined);
    return ret;
}

static XrValue path_resolve(XrVMRuntime *X, XrValue *args, int argc) {
    enum {
        PATH_RESOLVE_STACK_PARTS = 17
    };
    const char *stack_parts[PATH_RESOLVE_STACK_PARTS];
    size_t stack_lens[PATH_RESOLVE_STACK_PARTS];
    const char **parts = stack_parts;
    size_t *lens = stack_lens;
    void *heap_buf = NULL;
    size_t total = (size_t) argc + 1;
    if (total > PATH_RESOLVE_STACK_PARTS) {
        heap_buf = xr_malloc(sizeof(char *) * total + sizeof(size_t) * total);
        if (!heap_buf)
            return xr_null();
        parts = (const char **) heap_buf;
        lens = (size_t *) (parts + total);
    }

    for (int i = 0; i < argc; i++) {
        size_t len = 0;
        parts[i + 1] = xrs_string_arg(args[i], &len);
        lens[i + 1] = parts[i + 1] ? len : 0;
    }

    if (xr_path_core_join_has_absolute(parts + 1, lens + 1, (size_t) argc)) {
        XrValue ret = path_resolve_join_normalized(X, parts + 1, lens + 1, (size_t) argc);
        xr_free(heap_buf);
        return ret;
    }

    char cwd[XR_PATH_MAX];
    if (xr_fs_getcwd(cwd, sizeof(cwd)) == NULL) {
        cwd[0] = PATH_SEP;
        cwd[1] = '\0';
    }

    parts[0] = cwd;
    lens[0] = strlen(cwd);
    XrValue ret = path_resolve_join_normalized(X, parts, lens, total);
    xr_free(heap_buf);
    return ret;
}

// relative(from, to) - Compute relative path
// Fixed: segment-boundary-aware common prefix (avoids /foo vs /foobar mismatch)
static XrValue path_relative(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 2)
        return xrs_string_value_c(X, "");

    size_t from_len = 0;
    size_t to_len = 0;
    const char *from = xrs_string_arg(args[0], &from_len);
    const char *to = xrs_string_arg(args[1], &to_len);
    if (!from || !to)
        return xrs_string_value_c(X, "");

    char *from_norm = NULL;
    char *to_norm = NULL;
    size_t from_norm_len = 0;
    size_t to_norm_len = 0;
    if (!path_normalize_alloc(from, from_len, &from_norm, &from_norm_len))
        return xr_null();
    if (!path_normalize_alloc(to, to_len, &to_norm, &to_norm_len)) {
        xr_free(from_norm);
        return xr_null();
    }

    XrPathCoreRelativePlan plan;
    if (!xr_path_core_relative_plan(from_norm, from_norm_len, to_norm, to_norm_len, &plan)) {
        xr_free(from_norm);
        xr_free(to_norm);
        return xr_null();
    }

    char *result = (char *) xr_malloc(plan.out_len + 1);
    if (!result) {
        xr_free(from_norm);
        xr_free(to_norm);
        return xr_null();
    }
    xr_path_core_relative_write(to_norm, &plan, result);
    XrValue ret = xrs_string_value_n(X, result, plan.out_len);
    xr_free(from_norm);
    xr_free(to_norm);
    xr_free(result);
    return ret;
}

// parse(path) - Parse path into a PathInfo handle (Json with fixed shape:
// root, dir, base, name, ext). Returns Json so user code uses .field access
// (e.g. `let p = path.parse(s); p.dir`) instead of `.get("dir")`.
static XrValue path_parse(XrVMRuntime *X, XrValue *args, int argc) {
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

    size_t plen = 0;
    const char *path = xrs_string_arg(args[0], &plen);
    if (!path)
        path = "";

    XrPathCoreParsePlan plan;
    if (!xr_path_core_parse_plan(path, plen, &plan))
        return xr_null();

    xr_json_set_by_key(X, json, "root", make_string_slice(X, plan.root));
    xr_json_set_by_key(X, json, "dir", make_string_slice(X, plan.dir));
    xr_json_set_by_key(X, json, "base", make_string_slice(X, plan.base));
    xr_json_set_by_key(X, json, "name", make_string_slice(X, plan.name));
    xr_json_set_by_key(X, json, "ext", make_string_slice(X, plan.ext));

    return xr_json_value(json);
}

// format(obj) - Build path from a PathInfo Json.
//
// Uses a dynamic buffer so arbitrarily long paths are preserved, and honours
// PATH_SEP rather than hard-coding '/'. That avoids mixed-separator output
// on Windows, e.g. `C:\foo/bar`.
static XrValue path_format(XrVMRuntime *X, XrValue *args, int argc) {
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

#define XR_STDLIB_VM_BIND_MODULE_PATH 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_PATH

XR_FUNC XrModule *xr_load_module_path(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_path: NULL isolate");

    XrModule *mod = xr_module_create_native(isolate, "path");
    if (!mod)
        return NULL;

    xr_stdlib_vm_bind_path_generated(isolate, mod);

    mod->loaded = true;
    return mod;
}
