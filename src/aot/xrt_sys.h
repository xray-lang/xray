/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_sys.h - AOT helpers for sys.* OS-domain primitives.
 */

#ifndef XRT_SYS_H
#define XRT_SYS_H

#include "xrt_arc.h"
#include "xrt_method_symbols.h"
#include "xrt_value.h"
#include "../shared/xr_array_abi.h"
#include "../shared/xr_elem_type.h"
#include "../shared/xr_typed_ops.h"
#include "../os/os_pipe.h"
#include "../os/os_thread.h"
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(XR_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#define XRT_SYS_PROCESS_KILLED_EXIT_CODE ((unsigned int) 0xE0000001u)
#else
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

static inline XrValue xrt_closure_call0(XrValue callback);

typedef struct xrt_sys_mutex_object {
    xr_mutex_t mutex;
} xrt_sys_mutex_object_t;

typedef struct xrt_sys_rwlock_object {
    xr_rwlock_t rwlock;
} xrt_sys_rwlock_object_t;

typedef struct xrt_sys_condvar_object {
    xr_cond_t cond;
} xrt_sys_condvar_object_t;

typedef struct xrt_sys_barrier_object {
    xr_mutex_t mutex;
    xr_cond_t cond;
    int64_t parties;
    int64_t arrived;
    int64_t generation;
} xrt_sys_barrier_object_t;

typedef struct xrt_sys_once_object {
    xr_once_t once;
} xrt_sys_once_object_t;

#ifdef XRT_ENABLE_SYS_THREAD
typedef enum xrt_thread_state {
    XRT_THREAD_CREATED = 0,
    XRT_THREAD_JOINING,
    XRT_THREAD_JOINED,
    XRT_THREAD_DETACHED,
} xrt_thread_state_t;

typedef struct xrt_thread_object {
    xr_thread_t handle;
    _Atomic(int) state;
    _Atomic(bool) finished;
    _Atomic(bool) failed;
    bool error_is_value;
    XrValue retval;
    XrValue error;
} xrt_thread_object_t;

#ifdef XRT_THREAD_USE_PENDING_ERROR
extern XR_THREAD_LOCAL XrValue xrt_pending_error;
#define XRT_THREAD_SET_PENDING_ERROR(value) (xrt_pending_error = (value))
#else
#define XRT_THREAD_SET_PENDING_ERROR(value) ((void) (value))
#endif
#endif

static XR_THREAD_LOCAL XrValue xrt_sys_once_callback = {.tag = XR_TAG_NULL};

typedef struct xrt_sys_array_view {
    XrObjHeader hdr;
    XR_ARRAY_ABI_FIELDS;
} xrt_sys_array_view_t;

static inline size_t xrt_sys_array_data_bytes_or_abort(int64_t cap, uint8_t elem_size,
                                                       const char *where) {
    if (cap < 0)
        cap = 0;
    if (elem_size == 0)
        elem_size = 1;
    if ((uint64_t) cap > (uint64_t) SIZE_MAX / (uint64_t) elem_size) {
        fprintf(stderr, "%s: capacity overflow\n", where);
        abort();
    }
    return (size_t) cap * (size_t) elem_size;
}

static inline XrValue xrt_sys_array_new_typed_uninit(int64_t cap, uint8_t elem_type,
                                                     const char *where) {
    if (cap < 4)
        cap = 4;
    if (elem_type >= XR_ELEM_COUNT)
        elem_type = XR_ELEM_ANY;
    uint8_t elem_size = XR_ELEM_SIZES[elem_type];
    size_t data_bytes = xrt_sys_array_data_bytes_or_abort(cap, elem_size, where);
    size_t pad = data_bytes ? (XRT_DATA_ALIGN - 1) : 0;
    if (data_bytes > SIZE_MAX - sizeof(xrt_sys_array_view_t) - pad) {
        fprintf(stderr, "%s: allocation size overflow\n", where);
        abort();
    }

    size_t total = sizeof(xrt_sys_array_view_t) + data_bytes + pad;
    xrt_sys_array_view_t *arr = (xrt_sys_array_view_t *) XRT_MALLOC(total);
    if (XR_UNLIKELY(!arr)) {
        fprintf(stderr, "%s: out of memory\n", where);
        abort();
    }
    memset(arr, 0, sizeof(*arr));
    xrt_bump_header_init(&arr->hdr, XR_TARRAY);
    arr->hdr.extra |= XR_OBJ_AOT_NATIVE;
    if (!xrt_bump_enabled) {
        arr->hdr.extra &= (uint16_t) ~(uint16_t) XR_OBJ_STORAGE_BUMP;
        atomic_store_explicit(&arr->hdr.refcount, 0, memory_order_relaxed);
    }
    arr->capacity = cap;
    arr->data_storage = XR_ARRAY_DATA_INLINE;
    arr->elem_type = elem_type;
    arr->elem_size = elem_size;
    arr->content_version = XR_ARRAY_CONTENT_VERSION_INIT;
    if (data_bytes) {
        arr->data = (void *) (((uintptr_t) ((char *) arr + sizeof(xrt_sys_array_view_t)) +
                               (XRT_DATA_ALIGN - 1)) &
                              ~(uintptr_t) (XRT_DATA_ALIGN - 1));
    }
    return xr_mkptr(arr, XR_TAG_ARRAY);
}

static inline void xrt_sys_once_trampoline(void) {
    (void) xrt_closure_call0(xrt_sys_once_callback);
}

static inline int64_t xrt_sys_int_arg(XrValue v) {
    return v.tag == XR_TAG_I64 ? v.i : 0;
}

static inline char *xrt_sys_cstr_dup_arg(const char *data, int64_t len) {
    if (!data || len < 0)
        return NULL;
    uint64_t n64 = (uint64_t) len;
    if (n64 > (uint64_t) SIZE_MAX - 1u)
        return NULL;
    size_t n = (size_t) n64;
    char *out = (char *) XRT_MALLOC(n + 1u);
    if (!out)
        return NULL;
    memcpy(out, data, n);
    out[n] = '\0';
    return out;
}

static XR_THREAD_LOCAL char xrt_sys_dylib_error[512];

static inline void xrt_sys_dylib_set_error(const char *message) {
    if (!message)
        message = "";
    snprintf(xrt_sys_dylib_error, sizeof(xrt_sys_dylib_error), "%s", message);
}

#if defined(XR_OS_WINDOWS)
static inline void xrt_sys_dylib_set_win_error(DWORD err) {
    if (err == 0) {
        xrt_sys_dylib_set_error("");
        return;
    }
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err,
                             MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), xrt_sys_dylib_error,
                             (DWORD) sizeof(xrt_sys_dylib_error), NULL);
    if (n == 0)
        snprintf(xrt_sys_dylib_error, sizeof(xrt_sys_dylib_error), "Windows error %lu",
                 (unsigned long) err);
}
#endif

static inline XrValue xrt_sys_dylib_open(const char *path_data, int64_t path_len) {
    char *path = xrt_sys_cstr_dup_arg(path_data, path_len);
    if (!path || path[0] == '\0') {
        XRT_FREE(path);
        xrt_sys_dylib_set_error("empty dynamic library path");
        return XR_FROM_INT(0);
    }

#if defined(XR_OS_WINDOWS)
    HMODULE lib = LoadLibraryA(path);
    DWORD err = lib ? 0 : GetLastError();
    XRT_FREE(path);
    if (!lib)
        xrt_sys_dylib_set_win_error(err);
    else
        xrt_sys_dylib_set_error("");
    return XR_FROM_INT((int64_t) (intptr_t) lib);
#else
    (void) dlerror();
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    const char *err = lib ? NULL : dlerror();
    XRT_FREE(path);
    xrt_sys_dylib_set_error(err ? err : "");
    return XR_FROM_INT((int64_t) (intptr_t) lib);
#endif
}

static inline XrValue xrt_sys_dylib_symbol(XrValue handle_value, const char *name_data,
                                           int64_t name_len) {
    void *handle = (void *) (intptr_t) xrt_sys_int_arg(handle_value);
    char *name = xrt_sys_cstr_dup_arg(name_data, name_len);
    if (!handle || !name || name[0] == '\0') {
        XRT_FREE(name);
        xrt_sys_dylib_set_error(!handle ? "invalid dynamic library handle"
                                        : "empty dynamic library symbol");
        return XR_NULL_VAL;
    }

#if defined(XR_OS_WINDOWS)
    FARPROC sym = GetProcAddress((HMODULE) handle, name);
    DWORD err = sym ? 0 : GetLastError();
    XRT_FREE(name);
    if (!sym) {
        xrt_sys_dylib_set_win_error(err);
        return XR_NULL_VAL;
    }
    xrt_sys_dylib_set_error("");
    return XR_FROM_INT((int64_t) (intptr_t) sym);
#else
    (void) dlerror();
    void *sym = dlsym(handle, name);
    const char *err = dlerror();
    XRT_FREE(name);
    if (err || !sym) {
        xrt_sys_dylib_set_error(err ? err : "dynamic library symbol resolved to null");
        return XR_NULL_VAL;
    }
    xrt_sys_dylib_set_error("");
    return XR_FROM_INT((int64_t) (intptr_t) sym);
#endif
}

static inline XrValue xrt_sys_dylib_close(XrValue handle_value) {
    void *handle = (void *) (intptr_t) xrt_sys_int_arg(handle_value);
    if (!handle)
        return XR_FROM_BOOL(true);
#if defined(XR_OS_WINDOWS)
    BOOL ok = FreeLibrary((HMODULE) handle);
    if (!ok)
        xrt_sys_dylib_set_win_error(GetLastError());
    else
        xrt_sys_dylib_set_error("");
    return XR_FROM_BOOL(ok != 0);
#else
    int rc = dlclose(handle);
    xrt_sys_dylib_set_error(rc == 0 ? "" : dlerror());
    return XR_FROM_BOOL(rc == 0);
#endif
}

static inline XrValue xrt_sys_dylib_last_error(void) {
    return xr_box_str(xrt_sys_dylib_error);
}

static inline void xrt_sys_process_argv_free(char **argv) {
    if (!argv)
        return;
    for (size_t i = 0; argv[i] != NULL; i++)
        XRT_FREE(argv[i]);
    XRT_FREE(argv);
}

static inline char **xrt_sys_process_argv_from_array(const char *program_data, int64_t program_len,
                                                     XrValue args_value) {
    if (!program_data || program_len <= 0 || !XR_IS_ARRAY(args_value) || !args_value.ptr)
        return NULL;

    xrt_sys_array_view_t *args = (xrt_sys_array_view_t *) args_value.ptr;
    if (args->length < 0 || args->length > INT32_MAX - 1 || (args->length > 0 && !args->data))
        return NULL;

    size_t extra = (size_t) args->length;
    if (extra > (SIZE_MAX / sizeof(char *)) - 2u)
        return NULL;

    char **argv = (char **) XRT_CALLOC(extra + 2u, sizeof(char *));
    if (!argv)
        return NULL;

    argv[0] = xrt_sys_cstr_dup_arg(program_data, program_len);
    if (!argv[0]) {
        XRT_FREE(argv);
        return NULL;
    }

    for (size_t i = 0; i < extra; i++) {
        XrValue elem = xr_typed_get(args->data, (int32_t) i, args->elem_type);
        if (!XR_IS_STR(elem)) {
            xrt_sys_process_argv_free(argv);
            return NULL;
        }
        argv[i + 1u] = xrt_sys_cstr_dup_arg(xr_str_data(elem), xr_str_len(elem));
        if (!argv[i + 1u]) {
            xrt_sys_process_argv_free(argv);
            return NULL;
        }
    }
    argv[extra + 1u] = NULL;
    return argv;
}

static inline int xrt_sys_process_env_key_valid(const char *key) {
    if (!key || key[0] == '\0')
        return 0;
    return strchr(key, '=') == NULL;
}

static inline void xrt_sys_process_env_free(char **keys, char **values, size_t count) {
    if (keys) {
        for (size_t i = 0; i < count; i++)
            XRT_FREE(keys[i]);
        XRT_FREE(keys);
    }
    if (values) {
        for (size_t i = 0; i < count; i++)
            XRT_FREE(values[i]);
        XRT_FREE(values);
    }
}

static inline int xrt_sys_process_env_from_arrays(XrValue keys_value, XrValue values_value,
                                                  char ***out_keys, char ***out_values,
                                                  size_t *out_count) {
    *out_keys = NULL;
    *out_values = NULL;
    *out_count = 0;

    if (XR_IS_NULL(keys_value) && XR_IS_NULL(values_value))
        return 1;
    if (!XR_IS_ARRAY(keys_value) || !XR_IS_ARRAY(values_value) || !keys_value.ptr ||
        !values_value.ptr)
        return 0;

    xrt_sys_array_view_t *keys_arr = (xrt_sys_array_view_t *) keys_value.ptr;
    xrt_sys_array_view_t *values_arr = (xrt_sys_array_view_t *) values_value.ptr;
    if (keys_arr->length < 0 || keys_arr->length > INT32_MAX ||
        keys_arr->length != values_arr->length)
        return 0;
    if (keys_arr->length == 0)
        return 1;
    if (!keys_arr->data || !values_arr->data)
        return 0;

    size_t count = (size_t) keys_arr->length;
    if (count > SIZE_MAX / sizeof(char *))
        return 0;
    char **keys = (char **) XRT_CALLOC(count, sizeof(char *));
    char **values = (char **) XRT_CALLOC(count, sizeof(char *));
    if (!keys || !values) {
        xrt_sys_process_env_free(keys, values, count);
        return 0;
    }

    for (size_t i = 0; i < count; i++) {
        XrValue key_value = xr_typed_get(keys_arr->data, (int32_t) i, keys_arr->elem_type);
        XrValue value_value = xr_typed_get(values_arr->data, (int32_t) i, values_arr->elem_type);
        if (!XR_IS_STR(key_value) || !XR_IS_STR(value_value)) {
            xrt_sys_process_env_free(keys, values, count);
            return 0;
        }
        keys[i] = xrt_sys_cstr_dup_arg(xr_str_data(key_value), xr_str_len(key_value));
        values[i] = xrt_sys_cstr_dup_arg(xr_str_data(value_value), xr_str_len(value_value));
        if (!xrt_sys_process_env_key_valid(keys[i]) || !values[i]) {
            xrt_sys_process_env_free(keys, values, count);
            return 0;
        }
    }

    *out_keys = keys;
    *out_values = values;
    *out_count = count;
    return 1;
}

static inline int xrt_sys_process_pipe_handle_from_optional(XrValue value, bool *out_has,
                                                            XrPipeHandle *out_handle) {
    *out_has = false;
    *out_handle = XR_PIPE_INVALID;
    if (XR_IS_NULL(value))
        return 1;
    if (!XR_IS_INT(value))
        return 0;
    *out_has = true;
    *out_handle = (XrPipeHandle) value.i;
    return 1;
}

#if defined(XR_OS_WINDOWS)
static inline void xrt_sys_process_append_escaped_arg(char *buf, size_t *pos, const char *arg) {
    size_t backslashes = 0;
    for (const char *p = arg; *p; p++) {
        if (*p == '\\') {
            backslashes++;
            buf[(*pos)++] = '\\';
        } else if (*p == '"') {
            for (size_t k = 0; k < backslashes; k++)
                buf[(*pos)++] = '\\';
            buf[(*pos)++] = '\\';
            buf[(*pos)++] = '"';
            backslashes = 0;
        } else {
            backslashes = 0;
            buf[(*pos)++] = *p;
        }
    }
    for (size_t k = 0; k < backslashes; k++)
        buf[(*pos)++] = '\\';
}

static inline char *xrt_sys_process_build_command_line(const char *prog, char *const argv[]) {
    size_t cap = strlen(prog) * 2u + 3u;
    for (size_t i = 0; argv[i] != NULL; i++)
        cap += strlen(argv[i]) * 2u + 3u;
    char *buf = (char *) XRT_MALLOC(cap + 1u);
    if (!buf)
        return NULL;
    size_t pos = 0;
    if (argv[0] == NULL || strcmp(argv[0], prog) != 0) {
        buf[pos++] = '"';
        xrt_sys_process_append_escaped_arg(buf, &pos, prog);
        buf[pos++] = '"';
    }
    for (size_t i = 0; argv[i] != NULL; i++) {
        if (pos > 0)
            buf[pos++] = ' ';
        buf[pos++] = '"';
        xrt_sys_process_append_escaped_arg(buf, &pos, argv[i]);
        buf[pos++] = '"';
    }
    buf[pos] = '\0';
    return buf;
}

static inline char *xrt_sys_process_strdup(const char *s) {
    size_t len = strlen(s);
    char *out = (char *) XRT_MALLOC(len + 1u);
    if (!out)
        return NULL;
    memcpy(out, s, len + 1u);
    return out;
}

static inline int xrt_sys_process_env_entry_matches_key(const char *entry, const char *key) {
    const char *eq = entry ? strchr(entry, '=') : NULL;
    if (!eq || eq == entry || !key)
        return 0;
    size_t name_len = (size_t) (eq - entry);
    return strlen(key) == name_len && _strnicmp(entry, key, name_len) == 0;
}

static inline int xrt_sys_process_env_entry_cmp(const void *a, const void *b) {
    const char *ea = *(const char *const *) a;
    const char *eb = *(const char *const *) b;
    return _stricmp(ea, eb);
}

typedef struct xrt_sys_process_env_entries {
    char **items;
    size_t count;
    size_t cap;
} xrt_sys_process_env_entries_t;

static inline void xrt_sys_process_env_entries_free(xrt_sys_process_env_entries_t *entries) {
    if (!entries)
        return;
    for (size_t i = 0; i < entries->count; i++)
        XRT_FREE(entries->items[i]);
    XRT_FREE(entries->items);
    entries->items = NULL;
    entries->count = 0;
    entries->cap = 0;
}

static inline int xrt_sys_process_env_entries_push(xrt_sys_process_env_entries_t *entries,
                                                   char *item) {
    if (entries->count == entries->cap) {
        size_t next_cap = entries->cap ? entries->cap * 2u : 32u;
        char **next = (char **) XRT_REALLOC(entries->items, next_cap * sizeof(char *));
        if (!next)
            return 0;
        entries->items = next;
        entries->cap = next_cap;
    }
    entries->items[entries->count++] = item;
    return 1;
}

static inline int xrt_sys_process_env_key_overridden(char *const env_keys[], size_t env_count,
                                                     const char *entry) {
    for (size_t i = 0; i < env_count; i++) {
        if (xrt_sys_process_env_entry_matches_key(entry, env_keys[i]))
            return 1;
    }
    return 0;
}

static inline char *xrt_sys_process_env_pair_new(const char *key, const char *value) {
    size_t key_len = strlen(key);
    size_t value_len = strlen(value);
    if (key_len > SIZE_MAX - value_len - 2u)
        return NULL;
    char *out = (char *) XRT_MALLOC(key_len + value_len + 2u);
    if (!out)
        return NULL;
    memcpy(out, key, key_len);
    out[key_len] = '=';
    memcpy(out + key_len + 1u, value, value_len + 1u);
    return out;
}

static inline char *xrt_sys_process_env_block_build(char *const env_keys[],
                                                    char *const env_values[], size_t env_count) {
    if (env_count == 0)
        return NULL;

    xrt_sys_process_env_entries_t entries = {0};
    LPCH current = GetEnvironmentStringsA();
    if (current) {
        for (const char *p = current; *p; p += strlen(p) + 1u) {
            if (xrt_sys_process_env_key_overridden(env_keys, env_count, p))
                continue;
            char *copy = xrt_sys_process_strdup(p);
            if (!copy || !xrt_sys_process_env_entries_push(&entries, copy)) {
                XRT_FREE(copy);
                FreeEnvironmentStringsA(current);
                xrt_sys_process_env_entries_free(&entries);
                return NULL;
            }
        }
        FreeEnvironmentStringsA(current);
    }

    for (size_t i = 0; i < env_count; i++) {
        char *pair = xrt_sys_process_env_pair_new(env_keys[i], env_values[i]);
        if (!pair || !xrt_sys_process_env_entries_push(&entries, pair)) {
            XRT_FREE(pair);
            xrt_sys_process_env_entries_free(&entries);
            return NULL;
        }
    }

    qsort(entries.items, entries.count, sizeof(char *), xrt_sys_process_env_entry_cmp);

    size_t bytes = 1u;
    for (size_t i = 0; i < entries.count; i++) {
        size_t len = strlen(entries.items[i]);
        if (bytes > SIZE_MAX - len - 1u) {
            xrt_sys_process_env_entries_free(&entries);
            return NULL;
        }
        bytes += len + 1u;
    }

    char *block = (char *) XRT_MALLOC(bytes);
    if (!block) {
        xrt_sys_process_env_entries_free(&entries);
        return NULL;
    }
    char *dst = block;
    for (size_t i = 0; i < entries.count; i++) {
        size_t len = strlen(entries.items[i]) + 1u;
        memcpy(dst, entries.items[i], len);
        dst += len;
    }
    *dst = '\0';
    xrt_sys_process_env_entries_free(&entries);
    return block;
}

typedef struct xrt_sys_process_stdio_dup {
    HANDLE in;
    HANDLE out;
    HANDLE err;
} xrt_sys_process_stdio_dup_t;

static inline void xrt_sys_process_stdio_dup_close(xrt_sys_process_stdio_dup_t *dup) {
    if (!dup)
        return;
    if (dup->in)
        CloseHandle(dup->in);
    if (dup->out)
        CloseHandle(dup->out);
    if (dup->err)
        CloseHandle(dup->err);
    dup->in = NULL;
    dup->out = NULL;
    dup->err = NULL;
}

static inline int xrt_sys_process_duplicate_inheritable(HANDLE src, HANDLE *out) {
    if (!src || src == INVALID_HANDLE_VALUE || !out)
        return 0;
    HANDLE current = GetCurrentProcess();
    return DuplicateHandle(current, src, current, out, 0, TRUE, DUPLICATE_SAME_ACCESS) != 0;
}

static inline int xrt_sys_process_stdio_prepare(bool has_stdin, XrPipeHandle stdin_read,
                                                bool has_stdout, XrPipeHandle stdout_write,
                                                bool has_stderr, XrPipeHandle stderr_write,
                                                STARTUPINFOA *si,
                                                xrt_sys_process_stdio_dup_t *dup) {
    if (!has_stdin && !has_stdout && !has_stderr)
        return 1;

    memset(dup, 0, sizeof(*dup));
    si->dwFlags |= STARTF_USESTDHANDLES;
    si->hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si->hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si->hStdError = GetStdHandle(STD_ERROR_HANDLE);

    if (has_stdin) {
        if (!xrt_sys_process_duplicate_inheritable((HANDLE) (intptr_t) stdin_read, &dup->in))
            return 0;
        si->hStdInput = dup->in;
    }
    if (has_stdout) {
        if (!xrt_sys_process_duplicate_inheritable((HANDLE) (intptr_t) stdout_write, &dup->out))
            return 0;
        si->hStdOutput = dup->out;
    }
    if (has_stderr) {
        if (!xrt_sys_process_duplicate_inheritable((HANDLE) (intptr_t) stderr_write, &dup->err))
            return 0;
        si->hStdError = dup->err;
    }
    return 1;
}
#endif

#if defined(XR_OS_WINDOWS)
static BOOL CALLBACK xrt_sys_once_win_thunk(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void) once;
    (void) param;
    (void) ctx;
    xrt_sys_once_trampoline();
    return TRUE;
}
#endif

static inline void xrt_sys_mutex_init(xr_mutex_t *mutex) {
#if defined(XR_OS_WINDOWS)
    InitializeSRWLock(mutex);
#else
    pthread_mutex_init(mutex, NULL);
#endif
}

static inline void xrt_sys_mutex_destroy(xr_mutex_t *mutex) {
#if defined(XR_OS_WINDOWS)
    (void) mutex;
#else
    pthread_mutex_destroy(mutex);
#endif
}

static inline void xrt_sys_mutex_lock_native(xr_mutex_t *mutex) {
#if defined(XR_OS_WINDOWS)
    AcquireSRWLockExclusive(mutex);
#else
    pthread_mutex_lock(mutex);
#endif
}

static inline void xrt_sys_mutex_unlock_native(xr_mutex_t *mutex) {
#if defined(XR_OS_WINDOWS)
    ReleaseSRWLockExclusive(mutex);
#else
    pthread_mutex_unlock(mutex);
#endif
}

static inline bool xrt_sys_mutex_trylock_native(xr_mutex_t *mutex) {
#if defined(XR_OS_WINDOWS)
    return TryAcquireSRWLockExclusive(mutex) != 0;
#else
    return pthread_mutex_trylock(mutex) == 0;
#endif
}

static inline void xrt_sys_rwlock_init(xr_rwlock_t *rwlock) {
#if defined(XR_OS_WINDOWS)
    InitializeSRWLock(rwlock);
#else
    pthread_rwlock_init(rwlock, NULL);
#endif
}

static inline void xrt_sys_rwlock_destroy(xr_rwlock_t *rwlock) {
#if defined(XR_OS_WINDOWS)
    (void) rwlock;
#else
    pthread_rwlock_destroy(rwlock);
#endif
}

static inline void xrt_sys_rwlock_rdlock_native(xr_rwlock_t *rwlock) {
#if defined(XR_OS_WINDOWS)
    AcquireSRWLockShared(rwlock);
#else
    pthread_rwlock_rdlock(rwlock);
#endif
}

static inline void xrt_sys_rwlock_rdunlock_native(xr_rwlock_t *rwlock) {
#if defined(XR_OS_WINDOWS)
    ReleaseSRWLockShared(rwlock);
#else
    pthread_rwlock_unlock(rwlock);
#endif
}

static inline void xrt_sys_rwlock_wrlock_native(xr_rwlock_t *rwlock) {
#if defined(XR_OS_WINDOWS)
    AcquireSRWLockExclusive(rwlock);
#else
    pthread_rwlock_wrlock(rwlock);
#endif
}

static inline void xrt_sys_rwlock_wrunlock_native(xr_rwlock_t *rwlock) {
#if defined(XR_OS_WINDOWS)
    ReleaseSRWLockExclusive(rwlock);
#else
    pthread_rwlock_unlock(rwlock);
#endif
}

static inline void xrt_sys_condvar_init(xr_cond_t *cond) {
#if defined(XR_OS_WINDOWS)
    InitializeConditionVariable(cond);
#elif defined(XR_OS_MACOS)
    // macOS lacks pthread_condattr_setclock; the timed wait uses the
    // relative-timeout variant, which is inherently monotonic.
    pthread_cond_init(cond, NULL);
#else
    // Bind to CLOCK_MONOTONIC so relative timeouts survive wall-clock jumps.
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
#endif
}

static inline void xrt_sys_condvar_destroy(xr_cond_t *cond) {
#if defined(XR_OS_WINDOWS)
    (void) cond;
#else
    pthread_cond_destroy(cond);
#endif
}

static inline void xrt_sys_condvar_wait_native(xr_cond_t *cond, xr_mutex_t *mutex) {
#if defined(XR_OS_WINDOWS)
    SleepConditionVariableSRW(cond, mutex, INFINITE, 0);
#else
    pthread_cond_wait(cond, mutex);
#endif
}

static inline bool xrt_sys_condvar_wait_for_ns_native(xr_cond_t *cond, xr_mutex_t *mutex,
                                                      uint64_t timeout_ns) {
#if defined(XR_OS_WINDOWS)
    DWORD ms;
    if (timeout_ns >= (uint64_t) INFINITE * 1000000ULL)
        ms = INFINITE - 1;
    else
        ms = (DWORD) ((timeout_ns + 999999ULL) / 1000000ULL);
    return SleepConditionVariableSRW(cond, mutex, ms, 0) != 0;
#elif defined(XR_OS_MACOS)
    // Relative timeout is naturally monotonic and immune to wall-clock jumps.
    struct timespec rel;
    rel.tv_sec = (time_t) (timeout_ns / 1000000000ULL);
    rel.tv_nsec = (long) (timeout_ns % 1000000000ULL);
    return pthread_cond_timedwait_relative_np(cond, mutex, &rel) == 0;
#else
    // Deadline interpreted against CLOCK_MONOTONIC (matches condvar_init).
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    uint64_t total_ns = (uint64_t) deadline.tv_nsec + timeout_ns;
    deadline.tv_sec += (time_t) (total_ns / 1000000000ULL);
    deadline.tv_nsec = (long) (total_ns % 1000000000ULL);
    return pthread_cond_timedwait(cond, mutex, &deadline) == 0;
#endif
}

static inline void xrt_sys_condvar_signal_native(xr_cond_t *cond) {
#if defined(XR_OS_WINDOWS)
    WakeConditionVariable(cond);
#else
    pthread_cond_signal(cond);
#endif
}

static inline void xrt_sys_condvar_broadcast_native(xr_cond_t *cond) {
#if defined(XR_OS_WINDOWS)
    WakeAllConditionVariable(cond);
#else
    pthread_cond_broadcast(cond);
#endif
}

static inline void xrt_sys_once_init(xr_once_t *once) {
#if defined(XR_OS_WINDOWS)
    InitOnceInitialize(once);
#else
    xr_once_t init = XR_ONCE_INITIALIZER;
    *once = init;
#endif
}

static inline void xrt_sys_once_call_native(xr_once_t *once) {
#if defined(XR_OS_WINDOWS)
    InitOnceExecuteOnce(once, xrt_sys_once_win_thunk, NULL, NULL);
#else
    pthread_once(once, xrt_sys_once_trampoline);
#endif
}

static inline int xrt_sys_mutex_is(XrValue value) {
    return value.tag == XR_TAG_SYS_MUTEX && value.ptr != NULL;
}

static inline int xrt_sys_rwlock_is(XrValue value) {
    return value.tag == XR_TAG_SYS_RWLOCK && value.ptr != NULL;
}

static inline int xrt_sys_condvar_is(XrValue value) {
    return value.tag == XR_TAG_SYS_CONDVAR && value.ptr != NULL;
}

static inline int xrt_sys_barrier_is(XrValue value) {
    return value.tag == XR_TAG_SYS_BARRIER && value.ptr != NULL;
}

static inline int xrt_sys_once_is(XrValue value) {
    return value.tag == XR_TAG_SYS_ONCE && value.ptr != NULL;
}

static inline int xrt_thread_is(XrValue value) {
    return value.tag == XR_TAG_THREAD && value.ptr != NULL;
}

static inline xrt_sys_mutex_object_t *xrt_sys_mutex_ptr(XrValue value) {
    return xrt_sys_mutex_is(value) ? (xrt_sys_mutex_object_t *) value.ptr : NULL;
}

static inline xrt_sys_rwlock_object_t *xrt_sys_rwlock_ptr(XrValue value) {
    return xrt_sys_rwlock_is(value) ? (xrt_sys_rwlock_object_t *) value.ptr : NULL;
}

static inline xrt_sys_condvar_object_t *xrt_sys_condvar_ptr(XrValue value) {
    return xrt_sys_condvar_is(value) ? (xrt_sys_condvar_object_t *) value.ptr : NULL;
}

static inline xrt_sys_barrier_object_t *xrt_sys_barrier_ptr(XrValue value) {
    return xrt_sys_barrier_is(value) ? (xrt_sys_barrier_object_t *) value.ptr : NULL;
}

static inline xrt_sys_once_object_t *xrt_sys_once_ptr(XrValue value) {
    return xrt_sys_once_is(value) ? (xrt_sys_once_object_t *) value.ptr : NULL;
}

static inline XrValue xrt_sys_mutex_box(xrt_sys_mutex_object_t *mutex) {
    return mutex ? xr_mkptr(mutex, XR_TAG_SYS_MUTEX) : XR_NULL_VAL;
}

static inline XrValue xrt_sys_rwlock_box(xrt_sys_rwlock_object_t *rwlock) {
    return rwlock ? xr_mkptr(rwlock, XR_TAG_SYS_RWLOCK) : XR_NULL_VAL;
}

static inline XrValue xrt_sys_condvar_box(xrt_sys_condvar_object_t *condvar) {
    return condvar ? xr_mkptr(condvar, XR_TAG_SYS_CONDVAR) : XR_NULL_VAL;
}

static inline XrValue xrt_sys_barrier_box(xrt_sys_barrier_object_t *barrier) {
    return barrier ? xr_mkptr(barrier, XR_TAG_SYS_BARRIER) : XR_NULL_VAL;
}

static inline XrValue xrt_sys_once_box(xrt_sys_once_object_t *once) {
    return once ? xr_mkptr(once, XR_TAG_SYS_ONCE) : XR_NULL_VAL;
}

static inline XrValue xrt_sys_mutex_new(void) {
    xrt_sys_mutex_object_t *mutex = (xrt_sys_mutex_object_t *) xrt_arc_alloc(sizeof(*mutex));
    xrt_sys_mutex_init(&mutex->mutex);
    xrt_arc_mark_builtin(mutex, XRT_ARC_KIND_SYS_MUTEX);
    return xrt_sys_mutex_box(mutex);
}

static inline XrValue xrt_sys_rwlock_new(void) {
    xrt_sys_rwlock_object_t *rwlock = (xrt_sys_rwlock_object_t *) xrt_arc_alloc(sizeof(*rwlock));
    xrt_sys_rwlock_init(&rwlock->rwlock);
    xrt_arc_mark_builtin(rwlock, XRT_ARC_KIND_SYS_RWLOCK);
    return xrt_sys_rwlock_box(rwlock);
}

static inline XrValue xrt_sys_condvar_new(void) {
    xrt_sys_condvar_object_t *condvar =
        (xrt_sys_condvar_object_t *) xrt_arc_alloc(sizeof(*condvar));
    xrt_sys_condvar_init(&condvar->cond);
    xrt_arc_mark_builtin(condvar, XRT_ARC_KIND_SYS_CONDVAR);
    return xrt_sys_condvar_box(condvar);
}

static inline XrValue xrt_sys_barrier_new(XrValue parties_value) {
    int64_t parties =
        (parties_value.tag == XR_TAG_I64 && parties_value.i > 0) ? parties_value.i : 0;
    if (parties <= 0) {
        fprintf(stderr, "sys.Barrier parties must be > 0\n");
        abort();
    }
    xrt_sys_barrier_object_t *barrier =
        (xrt_sys_barrier_object_t *) xrt_arc_alloc(sizeof(*barrier));
    xrt_sys_mutex_init(&barrier->mutex);
    xrt_sys_condvar_init(&barrier->cond);
    barrier->parties = parties;
    barrier->arrived = 0;
    barrier->generation = 0;
    xrt_arc_mark_builtin(barrier, XRT_ARC_KIND_SYS_BARRIER);
    return xrt_sys_barrier_box(barrier);
}

static inline XrValue xrt_sys_once_new(void) {
    xrt_sys_once_object_t *once = (xrt_sys_once_object_t *) xrt_arc_alloc(sizeof(*once));
    xrt_sys_once_init(&once->once);
    xrt_arc_mark_builtin(once, XRT_ARC_KIND_SYS_ONCE);
    return xrt_sys_once_box(once);
}

static inline XrValue xrt_sys_cpu_count(void) {
#if defined(XR_OS_WINDOWS)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return XR_FROM_INT(si.dwNumberOfProcessors > 0 ? (int64_t) si.dwNumberOfProcessors : 1);
#elif defined(_SC_NPROCESSORS_ONLN)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return XR_FROM_INT(n > 0 ? (int64_t) n : 1);
#else
    return XR_FROM_INT(1);
#endif
}

static inline void xrt_sys_signal_poll(void);

static inline XrValue xrt_sys_thread_yield(void) {
#if defined(XR_OS_WINDOWS)
    SwitchToThread();
#else
    sched_yield();
#endif
    xrt_sys_signal_poll();
    return XR_NULL_VAL;
}

static inline XrValue xrt_sys_sleep_ms(XrValue ms_value) {
    int64_t ms = xrt_sys_int_arg(ms_value);
    if (ms <= 0) {
        xrt_sys_signal_poll();
        return XR_NULL_VAL;
    }
#if defined(XR_OS_WINDOWS)
    Sleep((DWORD) ms);
#else
    struct timespec req;
    req.tv_sec = (time_t) (ms / 1000);
    req.tv_nsec = (long) (ms % 1000) * 1000000L;
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
    }
#endif
    xrt_sys_signal_poll();
    return XR_NULL_VAL;
}

static inline XrValue xrt_sys_pin_to_cpu(XrValue cpu_value) {
    int64_t cpu = xrt_sys_int_arg(cpu_value);
    if (cpu < 0)
        return XR_FROM_BOOL(false);
#if defined(XR_OS_WINDOWS)
    XrValue count_value = xrt_sys_cpu_count();
    int64_t count = count_value.tag == XR_TAG_I64 && count_value.i > 0 ? count_value.i : 1;
    DWORD_PTR mask = ((DWORD_PTR) 1) << ((unsigned int) cpu % (unsigned int) count);
    return XR_FROM_BOOL(SetThreadAffinityMask(GetCurrentThread(), mask) != 0);
#elif defined(XR_OS_LINUX) && defined(CPU_ZERO) && defined(CPU_SET)
    XrValue count_value = xrt_sys_cpu_count();
    int64_t count = count_value.tag == XR_TAG_I64 && count_value.i > 0 ? count_value.i : 1;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET((int) ((unsigned int) cpu % (unsigned int) count), &set);
    return XR_FROM_BOOL(pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0);
#else
    (void) cpu;
    return XR_FROM_BOOL(false);
#endif
}

static atomic_flag xrt_threadlocal_live_lock = ATOMIC_FLAG_INIT;
static _Atomic bool xrt_threadlocal_live_fail_open = false;
static _Atomic uint64_t xrt_threadlocal_next_id = 1;
static XR_THREAD_LOCAL uint64_t xrt_threadlocal_tls_id = 0;
static uint64_t *xrt_threadlocal_live_ids = NULL;
static size_t xrt_threadlocal_live_count = 0;
static size_t xrt_threadlocal_live_cap = 0;
static uint64_t *xrt_threadlocal_exited_ids = NULL;
static size_t xrt_threadlocal_exited_count = 0;
static size_t xrt_threadlocal_exited_cap = 0;

static inline void xrt_threadlocal_lock(void) {
    while (atomic_flag_test_and_set_explicit(&xrt_threadlocal_live_lock, memory_order_acquire)) {
    }
}

static inline void xrt_threadlocal_unlock(void) {
    atomic_flag_clear_explicit(&xrt_threadlocal_live_lock, memory_order_release);
}

static inline uint64_t xrt_threadlocal_alloc_id(void) {
    uint64_t id = atomic_fetch_add_explicit(&xrt_threadlocal_next_id, 1, memory_order_relaxed);
    if (id == 0)
        id = atomic_fetch_add_explicit(&xrt_threadlocal_next_id, 1, memory_order_relaxed);
    return id;
}

static inline size_t xrt_threadlocal_find_unlocked(const uint64_t *ids, size_t count, uint64_t id) {
    for (size_t i = 0; i < count; i++) {
        if (ids[i] == id)
            return i;
    }
    return SIZE_MAX;
}

static inline bool xrt_threadlocal_add_unique_unlocked(uint64_t **ids, size_t *count, size_t *cap,
                                                       uint64_t id) {
    if (!ids || !count || !cap || id == 0)
        return true;
    if (xrt_threadlocal_find_unlocked(*ids, *count, id) != SIZE_MAX)
        return true;
    if (*count == *cap) {
        size_t next_cap = *cap ? *cap * 2u : 8u;
        uint64_t *next = (uint64_t *) XRT_REALLOC(*ids, next_cap * sizeof(uint64_t));
        if (!next)
            return false;
        *ids = next;
        *cap = next_cap;
    }
    (*ids)[(*count)++] = id;
    return true;
}

static inline void xrt_threadlocal_remove_unlocked(uint64_t *ids, size_t *count, uint64_t id) {
    if (!ids || !count || *count == 0 || id == 0)
        return;
    size_t i = xrt_threadlocal_find_unlocked(ids, *count, id);
    if (i != SIZE_MAX) {
        ids[i] = ids[*count - 1u];
        (*count)--;
    }
}

static inline void xrt_threadlocal_enter_current(void) {
    uint64_t id = xrt_threadlocal_tls_id;
    if (id == 0) {
        id = xrt_threadlocal_alloc_id();
        xrt_threadlocal_tls_id = id;
    }
    if (id == 0)
        return;

    xrt_threadlocal_lock();
    xrt_threadlocal_remove_unlocked(xrt_threadlocal_exited_ids, &xrt_threadlocal_exited_count, id);
    if (!xrt_threadlocal_add_unique_unlocked(&xrt_threadlocal_live_ids, &xrt_threadlocal_live_count,
                                             &xrt_threadlocal_live_cap, id)) {
        atomic_store_explicit(&xrt_threadlocal_live_fail_open, true, memory_order_release);
    }
    xrt_threadlocal_unlock();
}

static inline void xrt_threadlocal_leave_current(void) {
    uint64_t id = xrt_threadlocal_tls_id;
    if (id == 0)
        return;

    xrt_threadlocal_lock();
    xrt_threadlocal_remove_unlocked(xrt_threadlocal_live_ids, &xrt_threadlocal_live_count, id);
    if (!xrt_threadlocal_add_unique_unlocked(&xrt_threadlocal_exited_ids,
                                             &xrt_threadlocal_exited_count,
                                             &xrt_threadlocal_exited_cap, id)) {
        atomic_store_explicit(&xrt_threadlocal_live_fail_open, true, memory_order_release);
    }
    xrt_threadlocal_unlock();
    xrt_threadlocal_tls_id = 0;
}

static inline uint64_t xrt_threadlocal_current_id(void) {
    if (xrt_threadlocal_tls_id == 0)
        xrt_threadlocal_enter_current();
    return xrt_threadlocal_tls_id;
}

static inline XrValue xrt_sys_thread_local_id(void) {
    return XR_FROM_INT((int64_t) xrt_threadlocal_current_id());
}

static inline XrValue xrt_sys_thread_local_alive(XrValue id_value) {
    uint64_t id = (uint64_t) xrt_sys_int_arg(id_value);
    if (id == 0)
        return XR_FROM_BOOL(false);
    if (id == xrt_threadlocal_tls_id)
        return XR_FROM_BOOL(true);
    if (atomic_load_explicit(&xrt_threadlocal_live_fail_open, memory_order_acquire))
        return XR_FROM_BOOL(true);

    xrt_threadlocal_lock();
    bool alive = xrt_threadlocal_find_unlocked(xrt_threadlocal_live_ids, xrt_threadlocal_live_count,
                                               id) != SIZE_MAX;
    xrt_threadlocal_unlock();
    return XR_FROM_BOOL(alive);
}

typedef struct xrt_sys_signal_slot {
    _Atomic int pending;
    XrValue handler;
} xrt_sys_signal_slot_t;

static xrt_sys_signal_slot_t xrt_sys_signal_term = {
    .pending = ATOMIC_VAR_INIT(0),
    .handler = {.tag = XR_TAG_NULL},
};
static xrt_sys_signal_slot_t xrt_sys_signal_int = {
    .pending = ATOMIC_VAR_INIT(0),
    .handler = {.tag = XR_TAG_NULL},
};
static atomic_flag xrt_sys_signal_lock = ATOMIC_FLAG_INIT;

static inline xrt_sys_signal_slot_t *xrt_sys_signal_slot_for(int sig) {
    if (sig == SIGTERM)
        return &xrt_sys_signal_term;
    if (sig == SIGINT)
        return &xrt_sys_signal_int;
    return NULL;
}

static inline void xrt_sys_signal_mutex_lock(void) {
    while (atomic_flag_test_and_set_explicit(&xrt_sys_signal_lock, memory_order_acquire)) {
#if defined(XR_OS_WINDOWS)
        Sleep(0);
#else
        sched_yield();
#endif
    }
}

static inline void xrt_sys_signal_mutex_unlock(void) {
    atomic_flag_clear_explicit(&xrt_sys_signal_lock, memory_order_release);
}

static void xrt_sys_signal_handler(int sig) {
    xrt_sys_signal_slot_t *slot = xrt_sys_signal_slot_for(sig);
    if (slot)
        atomic_store_explicit(&slot->pending, 1, memory_order_relaxed);
}

static inline bool xrt_sys_signal_install_native(int sig) {
    if (!xrt_sys_signal_slot_for(sig))
        return false;
#if defined(XR_OS_WINDOWS)
    return signal(sig, xrt_sys_signal_handler) != SIG_ERR;
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = xrt_sys_signal_handler;
    sigemptyset(&sa.sa_mask);
    return sigaction(sig, &sa, NULL) == 0;
#endif
}

static inline bool xrt_sys_signal_take(xrt_sys_signal_slot_t *slot) {
    return slot && atomic_exchange_explicit(&slot->pending, 0, memory_order_acq_rel) != 0;
}

static inline void xrt_sys_signal_dispatch_slot(xrt_sys_signal_slot_t *slot) {
    if (!slot)
        return;
    XrValue handler = XR_NULL_VAL;
    xrt_sys_signal_mutex_lock();
    if (slot->handler.tag == XR_TAG_CLOSURE && slot->handler.ptr) {
        handler = slot->handler;
        xrt_retain(handler);
    }
    xrt_sys_signal_mutex_unlock();

    if (handler.tag == XR_TAG_CLOSURE && handler.ptr) {
        (void) xrt_closure_call0(handler);
        xrt_release(handler);
    }
}

static inline void xrt_sys_signal_poll(void) {
    if (xrt_sys_signal_take(&xrt_sys_signal_term))
        xrt_sys_signal_dispatch_slot(&xrt_sys_signal_term);
    if (xrt_sys_signal_take(&xrt_sys_signal_int))
        xrt_sys_signal_dispatch_slot(&xrt_sys_signal_int);
}

static inline XrValue xrt_sys_on_signal(XrValue sig_value, XrValue handler_value) {
    int64_t sig64 = xrt_sys_int_arg(sig_value);
    if (sig64 < 0 || sig64 > INT_MAX)
        return XR_FROM_BOOL(false);
    int sig = (int) sig64;
    xrt_sys_signal_slot_t *slot = xrt_sys_signal_slot_for(sig);
    if (!slot || handler_value.tag != XR_TAG_CLOSURE || !handler_value.ptr)
        return XR_FROM_BOOL(false);
    if (!xrt_sys_signal_install_native(sig))
        return XR_FROM_BOOL(false);

    xrt_retain(handler_value);
    xrt_sys_signal_mutex_lock();
    XrValue old = slot->handler;
    slot->handler = handler_value;
    atomic_store_explicit(&slot->pending, 0, memory_order_release);
    xrt_sys_signal_mutex_unlock();
    xrt_release(old);
    return XR_FROM_BOOL(true);
}

#if !defined(XR_OS_WINDOWS)
static inline bool xrt_sys_process_write_i64(int fd, int64_t value) {
    const char *p = (const char *) &value;
    size_t left = sizeof(value);
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        p += n;
        left -= (size_t) n;
    }
    return true;
}

static inline bool xrt_sys_process_read_i64(int fd, int64_t *out) {
    if (!out)
        return false;
    char *p = (char *) out;
    size_t left = sizeof(*out);
    while (left > 0) {
        ssize_t n = read(fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        p += n;
        left -= (size_t) n;
    }
    return true;
}
#endif

static inline XrValue xrt_sys_process_spawn(const char *program_data, int64_t program_len,
                                            XrValue args_value, XrValue cwd_value,
                                            XrValue env_keys_value, XrValue env_values_value,
                                            XrValue stdin_read_value, XrValue stdout_write_value,
                                            XrValue stderr_write_value, XrValue detached_value) {
    if (!XR_IS_BOOL(detached_value))
        return XR_FROM_INT(-1);
    bool detached = XR_TO_BOOL(detached_value);
    char **argv = xrt_sys_process_argv_from_array(program_data, program_len, args_value);
    if (!argv)
        return XR_FROM_INT(-1);
    char *cwd = NULL;
    if (XR_IS_STR(cwd_value)) {
        cwd = xrt_sys_cstr_dup_arg(xr_str_data(cwd_value), xr_str_len(cwd_value));
        if (!cwd) {
            xrt_sys_process_argv_free(argv);
            return XR_FROM_INT(-1);
        }
    } else if (!XR_IS_NULL(cwd_value)) {
        xrt_sys_process_argv_free(argv);
        return XR_FROM_INT(-1);
    }

    char **env_keys = NULL;
    char **env_values = NULL;
    size_t env_count = 0;
    if (!xrt_sys_process_env_from_arrays(env_keys_value, env_values_value, &env_keys, &env_values,
                                         &env_count)) {
        xrt_sys_process_argv_free(argv);
        XRT_FREE(cwd);
        return XR_FROM_INT(-1);
    }

    bool has_stdin = false;
    bool has_stdout = false;
    bool has_stderr = false;
    XrPipeHandle stdin_read = XR_PIPE_INVALID;
    XrPipeHandle stdout_write = XR_PIPE_INVALID;
    XrPipeHandle stderr_write = XR_PIPE_INVALID;
    if (!xrt_sys_process_pipe_handle_from_optional(stdin_read_value, &has_stdin, &stdin_read) ||
        !xrt_sys_process_pipe_handle_from_optional(stdout_write_value, &has_stdout,
                                                   &stdout_write) ||
        !xrt_sys_process_pipe_handle_from_optional(stderr_write_value, &has_stderr,
                                                   &stderr_write)) {
        xrt_sys_process_argv_free(argv);
        xrt_sys_process_env_free(env_keys, env_values, env_count);
        XRT_FREE(cwd);
        return XR_FROM_INT(-1);
    }

#if defined(XR_OS_WINDOWS)
    char *cmdline = xrt_sys_process_build_command_line(argv[0], argv);
    if (!cmdline) {
        xrt_sys_process_argv_free(argv);
        xrt_sys_process_env_free(env_keys, env_values, env_count);
        XRT_FREE(cwd);
        return XR_FROM_INT(-1);
    }
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    xrt_sys_process_stdio_dup_t stdio_dup = {0};
    if (!xrt_sys_process_stdio_prepare(has_stdin, stdin_read, has_stdout, stdout_write, has_stderr,
                                       stderr_write, &si, &stdio_dup)) {
        xrt_sys_process_stdio_dup_close(&stdio_dup);
        XRT_FREE(cmdline);
        xrt_sys_process_argv_free(argv);
        xrt_sys_process_env_free(env_keys, env_values, env_count);
        XRT_FREE(cwd);
        return XR_FROM_INT(-1);
    }
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    char *env_block = xrt_sys_process_env_block_build(env_keys, env_values, env_count);
    if (env_count > 0 && !env_block) {
        xrt_sys_process_stdio_dup_close(&stdio_dup);
        XRT_FREE(cmdline);
        xrt_sys_process_argv_free(argv);
        xrt_sys_process_env_free(env_keys, env_values, env_count);
        XRT_FREE(cwd);
        return XR_FROM_INT(-1);
    }
    DWORD create_flags = detached ? CREATE_NEW_PROCESS_GROUP : 0;
    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, create_flags, env_block,
                             (cwd && cwd[0] != '\0') ? cwd : NULL, &si, &pi);
    xrt_sys_process_stdio_dup_close(&stdio_dup);
    XRT_FREE(env_block);
    XRT_FREE(cmdline);
    xrt_sys_process_argv_free(argv);
    xrt_sys_process_env_free(env_keys, env_values, env_count);
    XRT_FREE(cwd);
    if (!ok)
        return XR_FROM_INT(-1);
    CloseHandle(pi.hThread);
    if (detached) {
        CloseHandle(pi.hProcess);
        return XR_FROM_INT((int64_t) pi.dwProcessId);
    }
    return XR_FROM_INT((int64_t) (intptr_t) pi.hProcess);
#else
    int detached_pipe[2] = {-1, -1};
    if (detached && pipe(detached_pipe) != 0) {
        xrt_sys_process_argv_free(argv);
        xrt_sys_process_env_free(env_keys, env_values, env_count);
        XRT_FREE(cwd);
        return XR_FROM_INT(-1);
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (detached) {
            close(detached_pipe[0]);
            close(detached_pipe[1]);
        }
        xrt_sys_process_argv_free(argv);
        xrt_sys_process_env_free(env_keys, env_values, env_count);
        XRT_FREE(cwd);
        return XR_FROM_INT(-1);
    }
    if (pid == 0) {
        if (detached) {
            close(detached_pipe[0]);
            if (setsid() < 0) {
                (void) xrt_sys_process_write_i64(detached_pipe[1], -1);
                close(detached_pipe[1]);
                _exit(127);
            }
            pid_t grandchild = fork();
            if (grandchild < 0) {
                (void) xrt_sys_process_write_i64(detached_pipe[1], -1);
                close(detached_pipe[1]);
                _exit(127);
            }
            if (grandchild > 0) {
                (void) xrt_sys_process_write_i64(detached_pipe[1], (int64_t) grandchild);
                close(detached_pipe[1]);
                _exit(0);
            }
            close(detached_pipe[1]);
        }
        if ((has_stdin && dup2((int) stdin_read, STDIN_FILENO) < 0) ||
            (has_stdout && dup2((int) stdout_write, STDOUT_FILENO) < 0) ||
            (has_stderr && dup2((int) stderr_write, STDERR_FILENO) < 0)) {
            _exit(127);
        }
        if (cwd && cwd[0] != '\0' && chdir(cwd) != 0)
            _exit(127);
        for (size_t i = 0; i < env_count; i++) {
            if (setenv(env_keys[i], env_values[i], 1) != 0)
                _exit(127);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    xrt_sys_process_argv_free(argv);
    xrt_sys_process_env_free(env_keys, env_values, env_count);
    XRT_FREE(cwd);
    if (detached) {
        close(detached_pipe[1]);
        int64_t detached_pid = -1;
        bool ok = xrt_sys_process_read_i64(detached_pipe[0], &detached_pid);
        close(detached_pipe[0]);
        int status = 0;
        pid_t r;
        do {
            r = waitpid(pid, &status, 0);
        } while (r < 0 && errno == EINTR);
        if (!ok || r < 0 || detached_pid <= 0)
            return XR_FROM_INT(-1);
        return XR_FROM_INT(detached_pid);
    }
    return XR_FROM_INT((int64_t) pid);
#endif
}

static inline XrValue xrt_sys_process_wait(XrValue id_value) {
    int64_t id = xrt_sys_int_arg(id_value);
    if (id <= 0)
        return XR_FROM_INT(-1);

#if defined(XR_OS_WINDOWS)
    int status = -1;
    if (_cwait(&status, (intptr_t) id, _WAIT_CHILD) == -1)
        return XR_FROM_INT(-1);
    if ((unsigned int) status == XRT_SYS_PROCESS_KILLED_EXIT_CODE)
        return XR_FROM_INT(-1);
    return XR_FROM_INT((int64_t) status);
#else
    int status = 0;
    pid_t r;
    do {
        r = waitpid((pid_t) id, &status, 0);
    } while (r < 0 && errno == EINTR);
    if (r < 0)
        return XR_FROM_INT(-1);
    if (WIFEXITED(status))
        return XR_FROM_INT((int64_t) WEXITSTATUS(status));
    return XR_FROM_INT(-1);
#endif
}

static inline XrValue xrt_sys_process_try_wait(XrValue id_value) {
    int64_t id = xrt_sys_int_arg(id_value);
    if (id <= 0)
        return XR_FROM_INT(-1);

#if defined(XR_OS_WINDOWS)
    HANDLE h = (HANDLE) (intptr_t) id;
    DWORD wait_result = WaitForSingleObject(h, 0);
    if (wait_result == WAIT_TIMEOUT)
        return XR_NULL_VAL;
    if (wait_result != WAIT_OBJECT_0)
        return XR_FROM_INT(-1);

    DWORD code = 0;
    BOOL ok = GetExitCodeProcess(h, &code);
    CloseHandle(h);
    if (!ok)
        return XR_FROM_INT(-1);
    if ((unsigned int) code == XRT_SYS_PROCESS_KILLED_EXIT_CODE)
        return XR_FROM_INT(-1);
    return XR_FROM_INT((int64_t) code);
#else
    int status = 0;
    pid_t r;
    do {
        r = waitpid((pid_t) id, &status, WNOHANG);
    } while (r < 0 && errno == EINTR);
    if (r == 0)
        return XR_NULL_VAL;
    if (r < 0)
        return XR_FROM_INT(-1);
    if (WIFEXITED(status))
        return XR_FROM_INT((int64_t) WEXITSTATUS(status));
    return XR_FROM_INT(-1);
#endif
}

static inline XrValue xrt_sys_process_kill(XrValue id_value, XrValue signal_value) {
    int64_t id = xrt_sys_int_arg(id_value);
    int64_t sig = xrt_sys_int_arg(signal_value);
    if (id <= 0 || sig <= 0)
        return XR_FROM_BOOL(false);

#if defined(XR_OS_WINDOWS)
    (void) sig;
    return XR_FROM_BOOL(
        TerminateProcess((HANDLE) (intptr_t) id, XRT_SYS_PROCESS_KILLED_EXIT_CODE) != 0);
#else
    return XR_FROM_BOOL(kill((pid_t) id, (int) sig) == 0);
#endif
}

static inline int xrt_sys_pipe_set_inheritable(XrPipeHandle handle, bool inheritable) {
#if defined(XR_OS_WINDOWS)
    DWORD flags = inheritable ? HANDLE_FLAG_INHERIT : 0;
    return SetHandleInformation((HANDLE) (intptr_t) handle, HANDLE_FLAG_INHERIT, flags) ? 0 : -1;
#else
    int flags = fcntl((int) handle, F_GETFD);
    if (flags < 0)
        return -1;
    if (inheritable)
        flags &= ~FD_CLOEXEC;
    else
        flags |= FD_CLOEXEC;
    return fcntl((int) handle, F_SETFD, flags);
#endif
}

static inline int xrt_sys_pipe_create(XrPipe *out) {
    if (!out)
        return -1;
    out->read = XR_PIPE_INVALID;
    out->write = XR_PIPE_INVALID;

#if defined(XR_OS_WINDOWS)
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = FALSE;
    HANDLE read_handle = NULL;
    HANDLE write_handle = NULL;
    if (!CreatePipe(&read_handle, &write_handle, &sa, 0))
        return -1;
    out->read = (XrPipeHandle) (intptr_t) read_handle;
    out->write = (XrPipeHandle) (intptr_t) write_handle;
    return 0;
#else
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    if (xrt_sys_pipe_set_inheritable((XrPipeHandle) fds[0], false) != 0 ||
        xrt_sys_pipe_set_inheritable((XrPipeHandle) fds[1], false) != 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    out->read = (XrPipeHandle) fds[0];
    out->write = (XrPipeHandle) fds[1];
    return 0;
#endif
}

static inline int xrt_sys_pipe_close_handle(XrPipeHandle handle) {
    if (handle == XR_PIPE_INVALID)
        return 0;
#if defined(XR_OS_WINDOWS)
    return CloseHandle((HANDLE) (intptr_t) handle) ? 0 : -1;
#else
    return close((int) handle) == 0 ? 0 : -1;
#endif
}

static inline int64_t xrt_sys_pipe_read_chunk(XrPipeHandle handle, void *buf, size_t len) {
    if (handle == XR_PIPE_INVALID || (!buf && len > 0))
        return -1;
#if defined(XR_OS_WINDOWS)
    DWORD chunk = len > (size_t) UINT_MAX ? (DWORD) UINT_MAX : (DWORD) len;
    DWORD read_bytes = 0;
    if (!ReadFile((HANDLE) (intptr_t) handle, buf, chunk, &read_bytes, NULL))
        return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
    return (int64_t) read_bytes;
#else
    ssize_t n;
    do {
        n = read((int) handle, buf, len);
    } while (n < 0 && errno == EINTR);
    return n < 0 ? -1 : (int64_t) n;
#endif
}

static inline int64_t xrt_sys_pipe_write_chunk(XrPipeHandle handle, const void *buf, size_t len) {
    if (handle == XR_PIPE_INVALID || (!buf && len > 0))
        return -1;
#if defined(XR_OS_WINDOWS)
    DWORD chunk = len > (size_t) UINT_MAX ? (DWORD) UINT_MAX : (DWORD) len;
    DWORD written = 0;
    if (!WriteFile((HANDLE) (intptr_t) handle, buf, chunk, &written, NULL))
        return -1;
    return (int64_t) written;
#else
    ssize_t n;
    do {
        n = write((int) handle, buf, len);
    } while (n < 0 && errno == EINTR);
    return n < 0 ? -1 : (int64_t) n;
#endif
}

static inline XrValue xrt_sys_pipe_open(void) {
    XrPipe pipe;
    if (xrt_sys_pipe_create(&pipe) != 0)
        return XR_NULL_VAL;

    XrValue ends_value = xrt_sys_array_new_typed_uninit(2, XR_ELEM_I64, "xrt_sys_pipe_open");
    xrt_sys_array_view_t *ends = (xrt_sys_array_view_t *) ends_value.ptr;
    if (!ends) {
        xrt_sys_pipe_close_handle(pipe.read);
        xrt_sys_pipe_close_handle(pipe.write);
        return XR_NULL_VAL;
    }
    ((int64_t *) ends->data)[0] = (int64_t) pipe.read;
    ((int64_t *) ends->data)[1] = (int64_t) pipe.write;
    ends->length = 2;
    return ends_value;
}

static inline XrValue xrt_sys_pipe_read(XrValue handle_value, XrValue max_bytes_value) {
    int64_t max_bytes = xrt_sys_int_arg(max_bytes_value);
    if (max_bytes < 0 || max_bytes > INT32_MAX)
        return XR_NULL_VAL;

    XrValue bytes_value =
        xrt_sys_array_new_typed_uninit(max_bytes, XR_ELEM_U8, "xrt_sys_pipe_read");
    xrt_sys_array_view_t *bytes = (xrt_sys_array_view_t *) bytes_value.ptr;
    if (!bytes)
        return XR_NULL_VAL;

    int64_t n = xrt_sys_pipe_read_chunk((XrPipeHandle) xrt_sys_int_arg(handle_value), bytes->data,
                                        (size_t) max_bytes);
    if (n < 0) {
        xrt_release(bytes_value);
        return XR_NULL_VAL;
    }
    bytes->length = n;
    return bytes_value;
}

static inline XrValue xrt_sys_pipe_write(XrValue handle_value, XrValue data_value) {
    if (!XR_IS_ARRAY(data_value) || !data_value.ptr)
        return XR_FROM_INT(-1);

    xrt_sys_array_view_t *bytes = (xrt_sys_array_view_t *) data_value.ptr;
    if (bytes->elem_type != XR_ELEM_U8)
        return XR_FROM_INT(-1);

    int64_t n = xrt_sys_pipe_write_chunk((XrPipeHandle) xrt_sys_int_arg(handle_value), bytes->data,
                                         (size_t) bytes->length);
    return XR_FROM_INT(n);
}

static inline XrValue xrt_sys_pipe_close(XrValue handle_value) {
    return XR_FROM_BOOL(xrt_sys_pipe_close_handle((XrPipeHandle) xrt_sys_int_arg(handle_value)) ==
                        0);
}

static inline void xrt_sys_mutex_destroy_builtin(void *obj) {
    if (!obj)
        return;
    xrt_sys_mutex_object_t *mutex = (xrt_sys_mutex_object_t *) obj;
    xrt_sys_mutex_destroy(&mutex->mutex);
}

static inline void xrt_sys_rwlock_destroy_builtin(void *obj) {
    if (!obj)
        return;
    xrt_sys_rwlock_object_t *rwlock = (xrt_sys_rwlock_object_t *) obj;
    xrt_sys_rwlock_destroy(&rwlock->rwlock);
}

static inline void xrt_sys_condvar_destroy_builtin(void *obj) {
    if (!obj)
        return;
    xrt_sys_condvar_object_t *condvar = (xrt_sys_condvar_object_t *) obj;
    xrt_sys_condvar_destroy(&condvar->cond);
}

static inline void xrt_sys_barrier_destroy_builtin(void *obj) {
    if (!obj)
        return;
    xrt_sys_barrier_object_t *barrier = (xrt_sys_barrier_object_t *) obj;
    xrt_sys_condvar_destroy(&barrier->cond);
    xrt_sys_mutex_destroy(&barrier->mutex);
}

static inline void xrt_sys_once_destroy_builtin(void *obj) {
    (void) obj;
}

#ifdef XRT_ENABLE_SYS_THREAD
static inline xrt_thread_object_t *xrt_thread_ptr(XrValue value) {
    return xrt_thread_is(value) ? (xrt_thread_object_t *) value.ptr : NULL;
}

static inline XrValue xrt_thread_box(xrt_thread_object_t *thread) {
    return thread ? xr_mkptr(thread, XR_TAG_THREAD) : XR_NULL_VAL;
}

static inline void xrt_thread_destroy_builtin(void *obj) {
    xrt_thread_object_t *thread = (xrt_thread_object_t *) obj;
    if (!thread)
        return;
    int expected = XRT_THREAD_CREATED;
    if (atomic_compare_exchange_strong_explicit(&thread->state, &expected, XRT_THREAD_DETACHED,
                                                memory_order_acq_rel, memory_order_acquire) &&
        xr_thread_is_valid(thread->handle)) {
        fputs("xray: warning: Thread handle dropped without join() or detach(); detaching OS "
              "thread\n",
              stderr);
        xr_thread_detach(thread->handle);
    }
}

static inline XrValue xrt_thread_done_value(XrValue recv) {
    xrt_thread_object_t *thread = xrt_thread_ptr(recv);
    return XR_FROM_BOOL(thread && atomic_load_explicit(&thread->finished, memory_order_acquire));
}

static inline XrValue xrt_thread_join_result_value(xrt_thread_object_t *thread) {
    if (!thread)
        return XR_NULL_VAL;
    if (atomic_load_explicit(&thread->failed, memory_order_acquire)) {
        if (!XR_IS_NULL(thread->error)) {
            XRT_THREAD_SET_PENDING_ERROR(thread->error);
        }
        return XR_NULL_VAL;
    }
    return thread->retval;
}

static inline XrValue xrt_thread_method_0(XrValue recv, int sym) {
    xrt_thread_object_t *thread = xrt_thread_ptr(recv);
    if (!thread)
        return XR_NULL_VAL;
    if (sym == XRT_SYM_JOIN) {
        for (;;) {
            int state = atomic_load_explicit(&thread->state, memory_order_acquire);
            switch ((xrt_thread_state_t) state) {
                case XRT_THREAD_JOINED:
                    return xrt_thread_join_result_value(thread);
                case XRT_THREAD_DETACHED:
                    return XR_NULL_VAL;
                case XRT_THREAD_JOINING:
                    xr_thread_yield();
                    break;
                case XRT_THREAD_CREATED: {
                    int expected = XRT_THREAD_CREATED;
                    if (!atomic_compare_exchange_strong_explicit(
                            &thread->state, &expected, XRT_THREAD_JOINING, memory_order_acq_rel,
                            memory_order_acquire)) {
                        break;
                    }
                    if (xr_thread_is_valid(thread->handle))
                        (void) xr_thread_join(thread->handle, NULL);
                    atomic_store_explicit(&thread->finished, true, memory_order_release);
                    atomic_store_explicit(&thread->state, XRT_THREAD_JOINED, memory_order_release);
                    return xrt_thread_join_result_value(thread);
                }
            }
        }
    }
    if (sym == XRT_SYM_DETACH) {
        int expected = XRT_THREAD_CREATED;
        if (atomic_compare_exchange_strong_explicit(&thread->state, &expected, XRT_THREAD_DETACHED,
                                                    memory_order_acq_rel, memory_order_acquire) &&
            xr_thread_is_valid(thread->handle)) {
            xr_thread_detach(thread->handle);
        }
        return XR_NULL_VAL;
    }
    return XR_NULL_VAL;
}
#endif

static inline XrValue xrt_sys_mutex_method_0(XrValue recv, int sym) {
    xrt_sys_mutex_object_t *mutex = xrt_sys_mutex_ptr(recv);
    if (!mutex)
        return XR_NULL_VAL;
    if (sym == XRT_SYM_LOCK) {
        xrt_sys_mutex_lock_native(&mutex->mutex);
        return XR_NULL_VAL;
    }
    if (sym == XRT_SYM_UNLOCK) {
        xrt_sys_mutex_unlock_native(&mutex->mutex);
        return XR_NULL_VAL;
    }
    if (sym == XRT_SYM_TRYLOCK)
        return XR_FROM_BOOL(xrt_sys_mutex_trylock_native(&mutex->mutex));
    return XR_NULL_VAL;
}

static inline XrValue xrt_sys_rwlock_method_0(XrValue recv, int sym) {
    xrt_sys_rwlock_object_t *rwlock = xrt_sys_rwlock_ptr(recv);
    if (!rwlock)
        return XR_NULL_VAL;
    if (sym == XRT_SYM_RDLOCK) {
        xrt_sys_rwlock_rdlock_native(&rwlock->rwlock);
        return XR_NULL_VAL;
    }
    if (sym == XRT_SYM_RDUNLOCK) {
        xrt_sys_rwlock_rdunlock_native(&rwlock->rwlock);
        return XR_NULL_VAL;
    }
    if (sym == XRT_SYM_WRLOCK) {
        xrt_sys_rwlock_wrlock_native(&rwlock->rwlock);
        return XR_NULL_VAL;
    }
    if (sym == XRT_SYM_WRUNLOCK) {
        xrt_sys_rwlock_wrunlock_native(&rwlock->rwlock);
        return XR_NULL_VAL;
    }
    return XR_NULL_VAL;
}

static inline XrValue xrt_sys_barrier_method_0(XrValue recv, int sym) {
    xrt_sys_barrier_object_t *barrier = xrt_sys_barrier_ptr(recv);
    if (!barrier || sym != XRT_SYM_WAIT)
        return XR_NULL_VAL;

    xrt_sys_mutex_lock_native(&barrier->mutex);
    int64_t generation = barrier->generation;
    barrier->arrived++;
    if (barrier->arrived >= barrier->parties) {
        barrier->arrived = 0;
        barrier->generation++;
        xrt_sys_condvar_broadcast_native(&barrier->cond);
        xrt_sys_mutex_unlock_native(&barrier->mutex);
        return XR_TRUE_VAL;
    }
    while (generation == barrier->generation)
        xrt_sys_condvar_wait_native(&barrier->cond, &barrier->mutex);
    xrt_sys_mutex_unlock_native(&barrier->mutex);
    return XR_TRUE_VAL;
}

static inline XrValue xrt_sys_condvar_method_0(XrValue recv, int sym) {
    xrt_sys_condvar_object_t *condvar = xrt_sys_condvar_ptr(recv);
    if (!condvar)
        return XR_NULL_VAL;
    if (sym == XRT_SYM_SIGNAL) {
        xrt_sys_condvar_signal_native(&condvar->cond);
        return XR_NULL_VAL;
    }
    if (sym == XRT_SYM_BROADCAST) {
        xrt_sys_condvar_broadcast_native(&condvar->cond);
        return XR_NULL_VAL;
    }
    return XR_NULL_VAL;
}

static inline XrValue xrt_sys_condvar_method_1(XrValue recv, int sym, XrValue arg0) {
    xrt_sys_condvar_object_t *condvar = xrt_sys_condvar_ptr(recv);
    xrt_sys_mutex_object_t *mutex = xrt_sys_mutex_ptr(arg0);
    if (!condvar || !mutex)
        return XR_NULL_VAL;
    if (sym == XRT_SYM_WAIT) {
        xrt_sys_condvar_wait_native(&condvar->cond, &mutex->mutex);
        return XR_NULL_VAL;
    }
    return XR_NULL_VAL;
}

static inline XrValue xrt_sys_condvar_method_2(XrValue recv, int sym, XrValue arg0, XrValue arg1) {
    xrt_sys_condvar_object_t *condvar = xrt_sys_condvar_ptr(recv);
    xrt_sys_mutex_object_t *mutex = xrt_sys_mutex_ptr(arg0);
    if (!condvar || !mutex || sym != XRT_SYM_WAITFOR)
        return XR_NULL_VAL;
    uint64_t timeout_ns = (arg1.tag == XR_TAG_I64 && arg1.i > 0) ? (uint64_t) arg1.i : 0u;
    return XR_FROM_BOOL(
        xrt_sys_condvar_wait_for_ns_native(&condvar->cond, &mutex->mutex, timeout_ns));
}

static inline XrValue xrt_sys_once_method_1(XrValue recv, int sym, XrValue arg0) {
    xrt_sys_once_object_t *once = xrt_sys_once_ptr(recv);
    if (!once || sym != XRT_SYM_CALL || arg0.tag != XR_TAG_CLOSURE)
        return XR_NULL_VAL;

    XrValue previous = xrt_sys_once_callback;
    xrt_sys_once_callback = arg0;
    xrt_sys_once_call_native(&once->once);
    xrt_sys_once_callback = previous;
    return XR_NULL_VAL;
}

#endif /* XRT_SYS_H */
