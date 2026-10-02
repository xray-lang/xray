/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_process.c - Bounded, argv-only process execution for toolchain probes
 */

#include "xtc_process.h"

#include "../../os/os_pipe.h"
#include "../../os/os_proc.h"
#if defined(XR_OS_WINDOWS)
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "../../os/os_time.h"
#include "../../base/xutf8.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XR_OS_WINDOWS
#define xtc_environ _environ
#else
extern char **environ;
#define xtc_environ environ
#endif

typedef struct XtcCapture {
    uint8_t *data;
    size_t len;
    size_t cap;
    size_t limit;
    bool truncated;
    bool eof;
} XtcCapture;


static bool xtc_env_key_contains(const char *key, size_t key_size, const char *needle) {
    size_t needle_size = strlen(needle);
    if (!key || needle_size == 0 || key_size < needle_size)
        return false;
    for (size_t i = 0; i + needle_size <= key_size; i++) {
        bool match = true;
        for (size_t j = 0; j < needle_size; j++) {
            if (toupper((unsigned char) key[i + j]) != toupper((unsigned char) needle[j])) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

static bool xtc_env_key_is_secret(const char *key, size_t key_size) {
    static const char *markers[] = {"TOKEN",      "SECRET",      "PASSWORD", "PASSWD",
                                    "CREDENTIAL", "PRIVATE_KEY", "API_KEY"};
    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
        if (xtc_env_key_contains(key, key_size, markers[i]))
            return true;
    }
    return false;
}

XR_FUNC void xtc_process_redact_bytes(const uint8_t *input, size_t input_size, char *output,
                                      size_t output_size) {
    static const char replacement[] = "<redacted>";
    static const char hex[] = "0123456789ABCDEF";
    if (!output || output_size == 0)
        return;
    output[0] = '\0';
    if (!input || input_size == 0)
        return;
    size_t in_pos = 0;
    size_t out_pos = 0;
    while (in_pos < input_size && out_pos + 1 < output_size) {
        size_t secret_size = 0;
        for (char **entry = xtc_environ; entry && *entry; entry++) {
            const char *equals = strchr(*entry, '=');
            if (!equals || !xtc_env_key_is_secret(*entry, (size_t) (equals - *entry)))
                continue;
            const char *value = equals + 1;
            size_t value_size = strlen(value);
            // Very short values create excessive false positives in ordinary
            // compiler diagnostics and are not useful credentials.
            if (value_size >= 4 && value_size <= input_size - in_pos &&
                memcmp(input + in_pos, value, value_size) == 0) {
                secret_size = value_size;
                break;
            }
        }
        if (secret_size > 0) {
            size_t count = sizeof(replacement) - 1;
            if (count > output_size - out_pos - 1)
                count = output_size - out_pos - 1;
            memcpy(output + out_pos, replacement, count);
            out_pos += count;
            in_pos += secret_size;
            continue;
        }
        uint8_t byte = input[in_pos++];
        if ((byte >= 0x20 && byte <= 0x7e) || byte == '\t') {
            output[out_pos++] = (char) byte;
        } else if (out_pos + 4 < output_size) {
            output[out_pos++] = '\\';
            output[out_pos++] = 'x';
            output[out_pos++] = hex[byte >> 4];
            output[out_pos++] = hex[byte & 0x0f];
        } else {
            break;
        }
    }
    output[out_pos] = '\0';
}

static size_t xtc_process_first_line_length(const XrProcessByteBuffer *bytes) {
    if (!bytes || !bytes->data)
        return 0;
    size_t len = 0;
    while (len < bytes->length && bytes->data[len] != '\r' && bytes->data[len] != '\n')
        len++;
    return len;
}

XR_FUNC bool xtc_process_copy_ascii_line(const XrProcessByteBuffer *bytes, char *output,
                                         size_t output_size) {
    size_t len = xtc_process_first_line_length(bytes);
    if (!output || output_size == 0 || len == 0 || len >= output_size)
        return false;
    for (size_t i = 0; i < len; i++) {
        if (bytes->data[i] < 0x20 || bytes->data[i] > 0x7e)
            return false;
    }
    memcpy(output, bytes->data, len);
    output[len] = '\0';
    return true;
}

XR_FUNC bool xtc_process_copy_ascii(const XrProcessByteBuffer *bytes, char *output,
                                    size_t output_size) {
    if (!bytes || !bytes->data || !output || output_size == 0 ||
        bytes->length >= output_size)
        return false;
    for (size_t i = 0; i < bytes->length; i++) {
        uint8_t byte = bytes->data[i];
        if (byte != '\r' && byte != '\n' && byte != '\t' && (byte < 0x20 || byte > 0x7e))
            return false;
    }
    memcpy(output, bytes->data, bytes->length);
    output[bytes->length] = '\0';
    return true;
}

XR_FUNC bool xtc_process_copy_utf8_line(const XrProcessByteBuffer *bytes, char *output,
                                        size_t output_size) {
    size_t len = xtc_process_first_line_length(bytes);
    if (!output || output_size == 0 || len == 0 || len >= output_size ||
        memchr(bytes->data, '\0', len) != NULL ||
        !xr_utf8_validate((const char *) bytes->data, len))
        return false;
    memcpy(output, bytes->data, len);
    output[len] = '\0';
    return true;
}

XR_FUNC bool xtc_process_bytes_contains_ascii(const XrProcessByteBuffer *bytes,
                                              const char *needle) {
    if (!bytes || !bytes->data || !needle)
        return false;
    size_t needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > bytes->length)
        return false;
    for (size_t i = 0; i + needle_len <= bytes->length; i++)
        if (memcmp(bytes->data + i, needle, needle_len) == 0)
            return true;
    return false;
}


struct XrToolchainProcess {
    XrCompileResources *resources;
    XrProcessSpec spec;
    bool overridden[XTC_PROCESS_MAX_ENV];
#if defined(XR_OS_WINDOWS)
    wchar_t *wide_keys[XTC_PROCESS_MAX_ENV];
    int wide_lengths[XTC_PROCESS_MAX_ENV];
#endif
};
static XrProcessStatus process_resource(XrCompileResourceStatus s) {
    switch (s) {
    case XR_COMPILE_RESOURCE_OK: return XTC_PROCESS_OK;
    case XR_COMPILE_RESOURCE_BAD_ARGUMENT: return XTC_PROCESS_INVALID;
    case XR_COMPILE_RESOURCE_BUDGET: return XTC_PROCESS_BUDGET;
    case XR_COMPILE_RESOURCE_OUT_OF_MEMORY: return XTC_PROCESS_OUT_OF_MEMORY;
    }
    return XTC_PROCESS_INVALID;
}
static XrProcessStatus process_os(XrOsProcStatus s) {
    switch (s) {
    case XR_PROC_OK: return XTC_PROCESS_OK;
    case XR_PROC_INVALID_ARGUMENT: return XTC_PROCESS_INVALID;
    case XR_PROC_UNRESOLVED: return XTC_PROCESS_UNRESOLVED;
    case XR_PROC_BUDGET: return XTC_PROCESS_BUDGET;
    case XR_PROC_OUT_OF_MEMORY: return XTC_PROCESS_OUT_OF_MEMORY;
    case XR_PROC_UNSUPPORTED: return XTC_PROCESS_UNSUPPORTED;
    case XR_PROC_IO: return XTC_PROCESS_IO;
    }
    return XTC_PROCESS_IO;
}
static XrOsProcStatus process_os_resource(XrCompileResourceStatus s) {
    switch (s) {
    case XR_COMPILE_RESOURCE_OK: return XR_PROC_OK;
    case XR_COMPILE_RESOURCE_BAD_ARGUMENT: return XR_PROC_INVALID_ARGUMENT;
    case XR_COMPILE_RESOURCE_BUDGET: return XR_PROC_BUDGET;
    case XR_COMPILE_RESOURCE_OUT_OF_MEMORY: return XR_PROC_OUT_OF_MEMORY;
    }
    return XR_PROC_INVALID_ARGUMENT;
}
static XrOsProcStatus process_allocate(void *context, size_t bytes, void **out) {
    return process_os_resource(xr_compile_resources_alloc(context, bytes, out));
}
static void process_release(void *context, void *p) { (void)context; xr_compile_resources_free(p); }
static XrOsProcStatus process_charge(void *context, uint64_t units) {
    return process_os_resource(xr_compile_resources_work(context, units));
}
static XrProcessStatus process_work(XrCompileResources *r, uint64_t n) {
    return process_resource(xr_compile_resources_work(r, n));
}
static XrProcessStatus process_copy(XrCompileResources *r, const char *text, const char **out) {
    if (!text) return XTC_PROCESS_INVALID;
    size_t n = 0; XrProcessStatus s;
    for (;;) {
        s = process_work(r, 1); if (s != XTC_PROCESS_OK) return s;
        if (!text[n]) break;
        if (n == 32767) return XTC_PROCESS_BUDGET;
        ++n;
    }
    s = process_work(r, n); if (s != XTC_PROCESS_OK) return s;
    if (!xr_utf8_validate(text, n)) return XTC_PROCESS_INVALID;
    void *p = NULL; s = process_resource(xr_compile_resources_alloc(r, n + 1, &p));
    if (s != XTC_PROCESS_OK) return s;
    s = process_work(r, n + 1);
    if (s != XTC_PROCESS_OK) { xr_compile_resources_free(p); return s; }
    memcpy(p, text, n + 1); *out = p; return XTC_PROCESS_OK;
}
static bool process_absolute(const char *s) {
#if defined(XR_OS_WINDOWS)
    return s && isalpha((unsigned char)s[0]) && s[1] == ':' && (s[2] == '/' || s[2] == '\\');
#else
    return s && s[0] == '/';
#endif
}
static XrProcessStatus process_env_add(XrToolchainProcess *p, const char *key,
    const char *value, bool replace, bool system_entry) {
    const char *k = NULL, *v = NULL;
#if defined(XR_OS_WINDOWS)
    wchar_t *wide = NULL; int units = 0;
#endif
    XrProcessStatus s = process_copy(p->resources, key, &k);
    if (s != XTC_PROCESS_OK) return s;
    size_t length = 0;
    for (;;) { s = process_work(p->resources, 1); if (s != XTC_PROCESS_OK) goto done; if (!k[length]) break; ++length; }
    if (!length) { s = XTC_PROCESS_INVALID; goto done; }
    for (size_t i = 0; i < length; ++i) {
        s = process_work(p->resources, 1); if (s != XTC_PROCESS_OK) goto done;
        if (k[i] == '=' && !(system_entry && !i && length > 1)) { s = XTC_PROCESS_INVALID; goto done; }
    }
#if defined(XR_OS_WINDOWS)
    s = process_work(p->resources, length + 1); if (s != XTC_PROCESS_OK) goto done;
    units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, k, (int)length + 1, NULL, 0);
    if (!units) { s = process_os(xr_proc_last_error()); goto done; }
    s = process_resource(xr_compile_resources_alloc(p->resources, (size_t)units * sizeof(wchar_t), (void **)&wide));
    if (s != XTC_PROCESS_OK) goto done;
    s = process_work(p->resources, length + 1 + (uint64_t)units * sizeof(wchar_t)); if (s != XTC_PROCESS_OK) goto done;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, k, (int)length + 1, wide, units)) { s = process_os(xr_proc_last_error()); goto done; }
#endif
    size_t slot = p->spec.env_count;
    for (size_t i = 0; i < slot; ++i) {
        bool equal = false;
#if defined(XR_OS_WINDOWS)
        s = process_work(p->resources, 1 + (uint64_t)(units + p->wide_lengths[i]) * sizeof(wchar_t));
        if (s != XTC_PROCESS_OK) goto done;
        int order = CompareStringOrdinal(wide, units - 1, p->wide_keys[i], p->wide_lengths[i] - 1, TRUE);
        if (!order) { s = process_os(xr_proc_last_error()); goto done; }
        equal = order == CSTR_EQUAL;
#else
        const char *existing = p->spec.env_keys[i]; size_t at = 0;
        for (;;) {
            s = process_work(p->resources, 2); if (s != XTC_PROCESS_OK) goto done;
            if (k[at] != existing[at]) break;
            if (!k[at]) { equal = true; break; } ++at;
        }
#endif
        if (equal) {
            if (!replace || p->overridden[i]) { s = XTC_PROCESS_INVALID; goto done; }
            slot = i; break;
        }
    }
    if (slot == XTC_PROCESS_MAX_ENV) { s = XTC_PROCESS_BUDGET; goto done; }
    s = process_copy(p->resources, value, &v); if (s != XTC_PROCESS_OK) goto done;
    if (slot < p->spec.env_count) {
        xr_compile_resources_free((void *)p->spec.env_keys[slot]);
        xr_compile_resources_free((void *)p->spec.env_values[slot]);
#if defined(XR_OS_WINDOWS)
        xr_compile_resources_free(p->wide_keys[slot]);
#endif
    } else ++p->spec.env_count;
    p->spec.env_keys[slot] = k; p->spec.env_values[slot] = v; p->overridden[slot] = !system_entry;
#if defined(XR_OS_WINDOWS)
    p->wide_keys[slot] = wide; p->wide_lengths[slot] = units;
#endif
    return XTC_PROCESS_OK;
 done:
#if defined(XR_OS_WINDOWS)
    xr_compile_resources_free(wide);
#endif
    xr_compile_resources_free((void *)k); xr_compile_resources_free((void *)v); return s;
}
#if defined(XR_OS_WINDOWS)
static XrProcessStatus process_from_wide(XrCompileResources *r, const wchar_t *wide, char **output, size_t *units) {
    size_t n = 0; XrProcessStatus s;
    for (;;) { s = process_work(r, sizeof(wchar_t)); if (s != XTC_PROCESS_OK) return s; if (!wide[n]) break; ++n; }
    if (units) *units = n + 1;
    if (n >= INT_MAX) return XTC_PROCESS_BUDGET;
    s = process_work(r, (n + 1) * sizeof(wchar_t)); if (s != XTC_PROCESS_OK) return s;
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, (int)n + 1, NULL, 0, NULL, NULL);
    if (!length) return process_os(xr_proc_last_error());
    s = process_resource(xr_compile_resources_alloc(r, (size_t)length, (void **)output));
    if (s != XTC_PROCESS_OK) return s;
    s = process_work(r, (n + 1) * sizeof(wchar_t) + (uint64_t)length);
    if (s == XTC_PROCESS_OK && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, (int)n + 1, *output, length, NULL, NULL)) s = process_os(xr_proc_last_error());
    if (s != XTC_PROCESS_OK) { xr_compile_resources_free(*output); *output = NULL; }
    return s;
}
#endif
static XrProcessStatus process_snapshot(XrToolchainProcess *p) {
    XrProcessStatus s = process_work(p->resources, 1); if (s != XTC_PROCESS_OK) return s;
#if defined(XR_OS_WINDOWS)
    LPWCH environment = GetEnvironmentStringsW();
    if (!environment) return process_os(xr_proc_last_error());
    for (const wchar_t *entry = environment;;) {
        s = process_work(p->resources, sizeof(wchar_t)); if (s != XTC_PROCESS_OK || !*entry) break;
        char *pair = NULL; size_t units = 0; s = process_from_wide(p->resources, entry, &pair, &units);
        if (s != XTC_PROCESS_OK) break;
        char *equals = pair + (pair[0] == '=' ? 1 : 0);
        for (;;) { s = process_work(p->resources, 1); if (s != XTC_PROCESS_OK || !*equals || *equals == '=') break; ++equals; }
        if (s == XTC_PROCESS_OK) {
            if (!*equals || equals == pair) s = XTC_PROCESS_INVALID;
            else { *equals = 0; s = process_env_add(p, pair, equals + 1, false, true); }
        }
        xr_compile_resources_free(pair); if (s != XTC_PROCESS_OK) break;
        entry += units;
    }
    FreeEnvironmentStringsW(environment);
    if (s != XTC_PROCESS_OK || p->spec.cwd) return s;
    s = process_work(p->resources, 1); if (s != XTC_PROCESS_OK) return s;
    DWORD units = GetCurrentDirectoryW(0, NULL); if (!units) return process_os(xr_proc_last_error());
    wchar_t *cwd = NULL; s = process_resource(xr_compile_resources_alloc(p->resources, (size_t)units * sizeof(wchar_t), (void **)&cwd));
    if (s != XTC_PROCESS_OK) return s;
    s = process_work(p->resources, 1 + (uint64_t)units * sizeof(wchar_t));
    if (s == XTC_PROCESS_OK) {
        DWORD actual = GetCurrentDirectoryW(units, cwd);
        if (!actual || actual >= units) s = XTC_PROCESS_IO;
        else { char *text = NULL; s = process_from_wide(p->resources, cwd, &text, NULL); p->spec.cwd = text; }
    }
    xr_compile_resources_free(cwd); return s;
#else
    for (char **entry = xtc_environ; entry && *entry; ++entry) {
        const char *copy = NULL; s = process_copy(p->resources, *entry, &copy); if (s != XTC_PROCESS_OK) return s;
        char *equals = (char *)copy;
        for (;;) { s = process_work(p->resources, 1); if (s != XTC_PROCESS_OK || !*equals || *equals == '=') break; ++equals; }
        if (s == XTC_PROCESS_OK) {
            if (!*equals) s = XTC_PROCESS_INVALID;
            else { *equals = 0; s = process_env_add(p, copy, equals + 1, false, true); }
        }
        xr_compile_resources_free((void *)copy); if (s != XTC_PROCESS_OK) return s;
    }
    if (!p->spec.cwd) {
        char cwd[32768]; s = process_work(p->resources, 1); if (s != XTC_PROCESS_OK) return s;
        if (!getcwd(cwd, sizeof(cwd))) return process_os(xr_proc_last_error());
        s = process_copy(p->resources, cwd, &p->spec.cwd);
    }
    return s;
#endif
}
XR_FUNC void xtc_process_free(XrToolchainProcess *p) {
    if (!p) return;
    xr_compile_resources_free((void *)p->spec.executable); xr_compile_resources_free((void *)p->spec.cwd);
    for (size_t i = 0; i < XTC_PROCESS_MAX_ARGS; ++i) xr_compile_resources_free((void *)p->spec.argv[i]);
    for (size_t i = 0; i < p->spec.env_count; ++i) { xr_compile_resources_free((void *)p->spec.env_keys[i]); xr_compile_resources_free((void *)p->spec.env_values[i]); }
    for (size_t i = 0; i < p->spec.env_count; ++i) {
#if defined(XR_OS_WINDOWS)
        xr_compile_resources_free(p->wide_keys[i]);
#endif
    }
    xr_compile_resources_free(p);
}
XR_FUNC XrProcessStatus xtc_process_prepare(XrCompileResources *r, const XrProcessSpec *spec, XrToolchainProcess **output) {
    if (!r || !spec || !output || *output || !spec->executable || !spec->argv[0] ||
        spec->env_count > XTC_PROCESS_MAX_ENV || (spec->environment_source != XTC_PROCESS_ENV_EXPLICIT && spec->environment_source != XTC_PROCESS_ENV_SNAPSHOT) ||
        !spec->output_limit || spec->output_limit == SIZE_MAX || !spec->timeout_ms ||
        (spec->image_mode != XR_PROC_IMAGES_NONE && spec->image_mode != XR_PROC_IMAGES_WINDOWS_TREE) ||
        ((spec->image_mode == XR_PROC_IMAGES_WINDOWS_TREE) != (spec->image_observer.observe != NULL))) return XTC_PROCESS_INVALID;
    XrToolchainProcess *p = NULL;
    XrProcessStatus s = process_resource(xr_compile_resources_calloc(r, 1, sizeof(*p), (void **)&p));
    if (s != XTC_PROCESS_OK) return s;
    p->spec.image_mode = spec->image_mode; p->spec.image_observer = spec->image_observer;
    p->resources = r; p->spec.timeout_ms = spec->timeout_ms; p->spec.output_limit = spec->output_limit;
    s = process_copy(r, spec->executable, &p->spec.executable); if (s != XTC_PROCESS_OK) goto fail;
    if (!process_absolute(p->spec.executable)) { s = XTC_PROCESS_INVALID; goto fail; }
    if (spec->cwd) { s = process_copy(r, spec->cwd, &p->spec.cwd); if (s != XTC_PROCESS_OK) goto fail; }
    size_t argc = 0;
    for (; argc < XTC_PROCESS_MAX_ARGS && spec->argv[argc]; ++argc) {
        s = process_copy(r, spec->argv[argc], &p->spec.argv[argc]); if (s != XTC_PROCESS_OK) goto fail;
    }
    if (argc == XTC_PROCESS_MAX_ARGS) { s = XTC_PROCESS_INVALID; goto fail; }
    if (spec->environment_source == XTC_PROCESS_ENV_SNAPSHOT) { s = process_snapshot(p); if (s != XTC_PROCESS_OK) goto fail; }
    if (!process_absolute(p->spec.cwd)) { s = XTC_PROCESS_INVALID; goto fail; }
    for (size_t i = 0; i < spec->env_count; ++i) {
        s = process_env_add(p, spec->env_keys[i], spec->env_values[i], spec->environment_source == XTC_PROCESS_ENV_SNAPSHOT, false);
        if (s != XTC_PROCESS_OK) goto fail;
    }
    *output = p; return XTC_PROCESS_OK;
 fail:
    xtc_process_free(p); return s;
}
static XrProcessStatus process_capture_init(XrCompileResources *r, XtcCapture *c, size_t limit) {
    c->limit = limit; c->cap = limit < 4096 ? limit + 1 : 4096;
    XrProcessStatus s = process_resource(xr_compile_resources_alloc(r, c->cap, (void **)&c->data));
    if (s == XTC_PROCESS_OK) c->data[0] = 0; return s;
}
static XrProcessStatus process_capture_append(XrCompileResources *r, XtcCapture *c, const uint8_t *bytes, size_t n) {
    size_t available = c->limit - c->len, accepted = n < available ? n : available;
    if (accepted < n) c->truncated = true;
    size_t needed = c->len + accepted + 1;
    if (needed > c->cap) {
        size_t next = c->cap > SIZE_MAX / 2 ? c->limit + 1 : c->cap * 2;
        if (next < needed) next = needed;
        if (next > c->limit + 1) next = c->limit + 1;
        XrProcessStatus s = process_resource(xr_compile_resources_resize(r, (void **)&c->data, next));
        if (s != XTC_PROCESS_OK) return s; c->cap = next;
    }
    XrProcessStatus s = process_work(r, accepted + 1); if (s != XTC_PROCESS_OK) return s;
    memcpy(c->data + c->len, bytes, accepted); c->len += accepted; c->data[c->len] = 0;
    return XTC_PROCESS_OK;
}
/* A single bounded read per stream keeps cancellation and the other pipe fair. */
static XrProcessStatus process_capture_read(XrCompileResources *r, XrPipeHandle handle, XtcCapture *c) {
    if (c->eof) return XTC_PROCESS_OK;
    uint8_t bytes[4096]; size_t available = 0; bool eof = false;
    XrProcessStatus s = process_work(r, 1); if (s != XTC_PROCESS_OK) return s;
    XrPipeIoStatus result = xr_pipe_probe(handle, &available, &eof);
    if (result == XR_PIPE_IO_WOULD_BLOCK) return XTC_PROCESS_OK;
    if (result == XR_PIPE_IO_ERROR) return process_os(xr_proc_last_error());
    if (eof) { c->eof = true; return XTC_PROCESS_OK; }
    size_t requested = available < sizeof(bytes) ? available : sizeof(bytes);
    s = process_work(r, requested + 1); if (s != XTC_PROCESS_OK) return s;
    int64_t n = xr_pipe_read(handle, bytes, requested);
    if (n < 0) return process_os(xr_proc_last_error());
    if (!n) { c->eof = true; return XTC_PROCESS_OK; }
    return process_capture_append(r, c, bytes, (size_t)n);
}
XR_FUNC XrProcessStatus xtc_process_run(const XrToolchainProcess *p,
    XrProcessCancelled cancelled, void *context, XrProcessResult *output) {
    if (!p || !output || output->stdout_bytes.data || output->stderr_bytes.data) return XTC_PROCESS_INVALID;
    XrProcessResult result = {0}; result.exit_code = -1;
    XrCompileResources *r = p->resources;
    XtcCapture captures[2] = {{0},{0}};
    XrPipe pipes[2] = {{XR_PIPE_INVALID, XR_PIPE_INVALID},{XR_PIPE_INVALID, XR_PIPE_INVALID}};
    XrProcId pid = XR_PROC_INVALID; bool exited = false;
    bool images_drained = p->spec.image_mode == XR_PROC_IMAGES_NONE;
    uint64_t start = xr_time_monotonic_ms();
    XrProcessStatus s = process_capture_init(r, &captures[0], p->spec.output_limit);
    if (s != XTC_PROCESS_OK) goto done;
    s = process_capture_init(r, &captures[1], p->spec.output_limit); if (s != XTC_PROCESS_OK) goto done;
    for (unsigned i = 0; i < 2; ++i) {
        s = process_work(r, 1); if (s != XTC_PROCESS_OK) goto done;
        if (xr_pipe_create(&pipes[i], NULL) != 0) { s = process_os(xr_proc_last_error()); goto done; }
    }
    if (cancelled && cancelled(context)) { s = XTC_PROCESS_CANCELLED; goto done; }
    XrProcSpawnOptions options = {0};
    options.memory = (XrProcMemory){r, process_allocate, process_release, process_charge};
    options.cwd = p->spec.cwd; options.env_keys = p->spec.env_keys; options.env_values = p->spec.env_values; options.env_count = p->spec.env_count;
    options.complete_environment = true; options.new_process_group = true;
    options.image_mode = p->spec.image_mode; options.image_observer = p->spec.image_observer;
    options.has_stdout = options.has_stderr = true; options.stdout_write = pipes[0].write; options.stderr_write = pipes[1].write;
    s = process_os(xr_proc_spawn(p->spec.executable, p->spec.argv, &options, &pid));
    for (unsigned i = 0; i < 2; ++i) { if (xr_pipe_close(pipes[i].write) != 0 && s == XTC_PROCESS_OK) s = XTC_PROCESS_IO; pipes[i].write = XR_PIPE_INVALID; }
    if (s != XTC_PROCESS_OK) goto done;
    while (!exited || !images_drained || !captures[0].eof || !captures[1].eof) {
        s = process_work(r, 1); if (s != XTC_PROCESS_OK) break;
        if (cancelled && cancelled(context)) { s = XTC_PROCESS_CANCELLED; break; }
        if (xr_time_monotonic_ms() - start >= p->spec.timeout_ms) { s = XTC_PROCESS_TIMEOUT; break; }
        bool image_progress = false;
        if (!images_drained) {
            XrProcImagePumpResult pump;
            s = process_os(xr_proc_pump_images(pid, &pump));
            if (s != XTC_PROCESS_OK) break;
            images_drained = pump.drained; image_progress = pump.progressed;
        }
        s = process_capture_read(r, pipes[0].read, &captures[0]); if (s != XTC_PROCESS_OK) break;
        s = process_capture_read(r, pipes[1].read, &captures[1]); if (s != XTC_PROCESS_OK) break;
        if (!exited) {
            s = process_work(r, 1); if (s != XTC_PROCESS_OK) break;
            XrProcWaitResult wait = xr_proc_try_wait(pid, &result.exit_code);
            if (wait == XR_PROC_WAIT_ERROR) { s = process_os(xr_proc_last_error()); break; }
            exited = wait == XR_PROC_WAIT_EXITED;
        }
        /* The active image pump already waits for an event that can wake it.
         * Sleeping again would throttle each debuggee scheduling handoff. */
        if (images_drained && !image_progress && (!exited || !captures[0].eof || !captures[1].eof)) xr_time_sleep_ms(1);
    }
 done:
    if (pid != XR_PROC_INVALID) {
        if (s != XTC_PROCESS_OK) {
            /* Cleanup needs no new ledger permission and never publishes bytes. */
            (void)xr_proc_kill_tree(pid, 9);
            /* Close owns the final reap even if termination or polling failed. */
        }
        if (xr_proc_close(pid) != 0 && s == XTC_PROCESS_OK) s = XTC_PROCESS_IO;
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (s != XTC_PROCESS_OK && pipes[i].read != XR_PIPE_INVALID) {
            uint8_t discard[4096]; int64_t n;
            while (xr_pipe_try_read(pipes[i].read, discard, sizeof(discard), &n) == XR_PIPE_IO_OK && n > 0) {}
        }
        if (xr_pipe_close(pipes[i].read) != 0 && s == XTC_PROCESS_OK) s = XTC_PROCESS_IO;
        if (xr_pipe_close(pipes[i].write) != 0 && s == XTC_PROCESS_OK) s = XTC_PROCESS_IO;
    }
    if (s == XTC_PROCESS_OK) {
        result.duration_ms = xr_time_monotonic_ms() - start;
        result.stdout_bytes = (XrProcessByteBuffer){captures[0].data, captures[0].len, captures[0].truncated};
        result.stderr_bytes = (XrProcessByteBuffer){captures[1].data, captures[1].len, captures[1].truncated};
        *output = result;
    } else { xr_compile_resources_free(captures[0].data); xr_compile_resources_free(captures[1].data); }
    return s;
}
XR_FUNC void xtc_process_spec_init(XrProcessSpec *spec, const char *executable, uint32_t timeout_ms) {
    if (!spec) return;
    memset(spec, 0, sizeof(*spec)); spec->executable = executable; spec->argv[0] = executable;
    spec->timeout_ms = timeout_ms; spec->output_limit = XTC_PROCESS_DEFAULT_OUTPUT_LIMIT;
}
XR_FUNC void xtc_process_result_free(XrProcessResult *result) {
    if (!result) return;
    xr_compile_resources_free(result->stdout_bytes.data); xr_compile_resources_free(result->stderr_bytes.data);
    memset(result, 0, sizeof(*result));
}
XR_FUNC const char *xtc_process_status_name(XrProcessStatus s) {
    switch (s) {
    case XTC_PROCESS_OK: return "ok";
    case XTC_PROCESS_INVALID: return "invalid process request";
    case XTC_PROCESS_UNRESOLVED: return "process input not found";
    case XTC_PROCESS_BUDGET: return "process budget exhausted";
    case XTC_PROCESS_OUT_OF_MEMORY: return "process out of memory";
    case XTC_PROCESS_IO: return "process IO failure";
    case XTC_PROCESS_TIMEOUT: return "process timed out";
    case XTC_PROCESS_CANCELLED: return "process cancelled";
    case XTC_PROCESS_UNSUPPORTED: return "unsupported process request";
    }
    return "invalid process status";
}
