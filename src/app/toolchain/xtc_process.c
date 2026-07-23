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
#include "../../os/os_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct XtcCapture {
    char *data;
    size_t len;
    size_t cap;
    size_t limit;
    bool truncated;
    bool eof;
} XtcCapture;

static void xtc_process_error(char *err, size_t err_size, const char *message) {
    if (err && err_size > 0)
        snprintf(err, err_size, "%s", message);
}

static bool xtc_capture_init(XtcCapture *capture, size_t limit) {
    memset(capture, 0, sizeof(*capture));
    capture->limit = limit;
    capture->cap = limit < 4096 ? limit + 1 : 4096;
    if (capture->cap == 0)
        capture->cap = 1;
    capture->data = (char *) malloc(capture->cap);  // xr:allow-raw-alloc
    if (!capture->data)
        return false;
    capture->data[0] = '\0';
    return true;
}

static bool xtc_capture_reserve(XtcCapture *capture, size_t needed) {
    if (needed <= capture->cap)
        return true;
    size_t next = capture->cap;
    while (next < needed) {
        size_t doubled = next > SIZE_MAX / 2 ? SIZE_MAX : next * 2;
        next = doubled > capture->limit + 1 ? capture->limit + 1 : doubled;
        if (next < needed)
            return false;
    }
    char *data = (char *) realloc(capture->data, next);  // xr:allow-raw-alloc
    if (!data)
        return false;
    capture->data = data;
    capture->cap = next;
    return true;
}

static bool xtc_capture_append(XtcCapture *capture, const char *data, size_t len) {
    size_t available = capture->len < capture->limit ? capture->limit - capture->len : 0;
    size_t accepted = len < available ? len : available;
    if (accepted > 0) {
        if (!xtc_capture_reserve(capture, capture->len + accepted + 1))
            return false;
        memcpy(capture->data + capture->len, data, accepted);
        capture->len += accepted;
        capture->data[capture->len] = '\0';
    }
    if (accepted < len)
        capture->truncated = true;
    return true;
}

#ifndef XR_OS_WINDOWS
static bool xtc_capture_drain(XrPipeHandle handle, XtcCapture *capture) {
    char buffer[4096];
    for (;;) {
        int64_t count = 0;
        XrPipeIoStatus status = xr_pipe_try_read(handle, buffer, sizeof(buffer), &count);
        if (status == XR_PIPE_IO_WOULD_BLOCK)
            return true;
        if (status == XR_PIPE_IO_ERROR)
            return false;
        if (count == 0) {
            capture->eof = true;
            return true;
        }
        if (!xtc_capture_append(capture, buffer, (size_t) count))
            return false;
    }
}
#endif

XR_FUNC void xtc_process_spec_init(XrProcessSpec *spec, const char *executable,
                                   uint32_t timeout_ms) {
    if (!spec)
        return;
    memset(spec, 0, sizeof(*spec));
    spec->executable = executable;
    spec->argv[0] = executable;
    spec->timeout_ms = timeout_ms;
    spec->output_limit = XTC_PROCESS_DEFAULT_OUTPUT_LIMIT;
}

XR_FUNC bool xtc_process_run(const XrProcessSpec *spec, XrProcessResult *out, char *err,
                             size_t err_size) {
    if (!spec || !spec->executable || !spec->executable[0] || !spec->argv[0] || !out) {
        xtc_process_error(err, err_size, "invalid process specification");
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->exit_code = -1;
    uint64_t start_ms = xr_time_monotonic_ms();
    uint64_t deadline_ms = start_ms + (spec->timeout_ms ? spec->timeout_ms : 30000);

#ifdef XR_OS_WINDOWS
    XrProcSpawnOptions options = {0};
    options.cwd = spec->cwd;
    options.env_keys = spec->env_keys;
    options.env_values = spec->env_values;
    options.env_count = spec->env_count;
    options.new_process_group = true;
    XrProcId pid = xr_proc_spawn_ex(spec->executable, spec->argv, &options);
    if (pid == XR_PROC_INVALID) {
        xtc_process_error(err, err_size, "failed to spawn process");
        return false;
    }
    for (;;) {
        XrProcWaitResult wait = xr_proc_try_wait(pid, &out->exit_code);
        if (wait == XR_PROC_WAIT_EXITED)
            break;
        if (wait == XR_PROC_WAIT_ERROR) {
            xtc_process_error(err, err_size, "failed to wait for process");
            return false;
        }
        if (xr_time_monotonic_ms() >= deadline_ms) {
            (void) xr_proc_kill_tree(pid, 9);
            (void) xr_proc_wait(pid, &out->exit_code);
            out->timed_out = true;
            break;
        }
        xr_time_sleep_ms(5);
    }
    out->stdout_data = (char *) malloc(1);  // xr:allow-raw-alloc
    out->stderr_data = (char *) malloc(1);  // xr:allow-raw-alloc
    if (!out->stdout_data || !out->stderr_data) {
        xtc_process_result_free(out);
        xtc_process_error(err, err_size, "out of memory while capturing process output");
        return false;
    }
    out->stdout_data[0] = '\0';
    out->stderr_data[0] = '\0';
#else
    XrPipe stdout_pipe = {XR_PIPE_INVALID, XR_PIPE_INVALID};
    XrPipe stderr_pipe = {XR_PIPE_INVALID, XR_PIPE_INVALID};
    XrPipeOptions pipe_options = {.read_inheritable = false, .write_inheritable = true};
    XtcCapture stdout_capture = {0};
    XtcCapture stderr_capture = {0};
    size_t output_limit =
        spec->output_limit ? spec->output_limit : XTC_PROCESS_DEFAULT_OUTPUT_LIMIT;

    if (!xtc_capture_init(&stdout_capture, output_limit) ||
        !xtc_capture_init(&stderr_capture, output_limit)) {
        free(stdout_capture.data);  // xr:allow-raw-alloc
        free(stderr_capture.data);  // xr:allow-raw-alloc
        xtc_process_error(err, err_size, "out of memory while preparing process capture");
        return false;
    }
    if (xr_pipe_create(&stdout_pipe, &pipe_options) != 0 ||
        xr_pipe_create(&stderr_pipe, &pipe_options) != 0) {
        xr_pipe_close(stdout_pipe.read);
        xr_pipe_close(stdout_pipe.write);
        xr_pipe_close(stderr_pipe.read);
        xr_pipe_close(stderr_pipe.write);
        free(stdout_capture.data);  // xr:allow-raw-alloc
        free(stderr_capture.data);  // xr:allow-raw-alloc
        xtc_process_error(err, err_size, "failed to create process capture pipes");
        return false;
    }

    XrProcSpawnOptions options = {0};
    options.cwd = spec->cwd;
    options.env_keys = spec->env_keys;
    options.env_values = spec->env_values;
    options.env_count = spec->env_count;
    options.has_stdout = true;
    options.stdout_write = stdout_pipe.write;
    options.has_stderr = true;
    options.stderr_write = stderr_pipe.write;
    options.new_process_group = true;
    XrProcId pid = xr_proc_spawn_ex(spec->executable, spec->argv, &options);
    xr_pipe_close(stdout_pipe.write);
    xr_pipe_close(stderr_pipe.write);
    stdout_pipe.write = XR_PIPE_INVALID;
    stderr_pipe.write = XR_PIPE_INVALID;
    if (pid == XR_PROC_INVALID) {
        xr_pipe_close(stdout_pipe.read);
        xr_pipe_close(stderr_pipe.read);
        free(stdout_capture.data);  // xr:allow-raw-alloc
        free(stderr_capture.data);  // xr:allow-raw-alloc
        xtc_process_error(err, err_size, "failed to spawn process");
        return false;
    }

    bool exited = false;
    bool io_ok = true;
    while (!exited || !stdout_capture.eof || !stderr_capture.eof) {
        io_ok = xtc_capture_drain(stdout_pipe.read, &stdout_capture) &&
                xtc_capture_drain(stderr_pipe.read, &stderr_capture);
        if (!io_ok)
            break;
        if (!exited) {
            XrProcWaitResult wait = xr_proc_try_wait(pid, &out->exit_code);
            if (wait == XR_PROC_WAIT_EXITED)
                exited = true;
            else if (wait == XR_PROC_WAIT_ERROR)
                break;
            else if (xr_time_monotonic_ms() >= deadline_ms) {
                (void) xr_proc_kill_tree(pid, 9);
                (void) xr_proc_wait(pid, &out->exit_code);
                out->timed_out = true;
                exited = true;
            }
        }
        if (!exited || !stdout_capture.eof || !stderr_capture.eof)
            xr_time_sleep_ms(2);
    }
    xr_pipe_close(stdout_pipe.read);
    xr_pipe_close(stderr_pipe.read);
    if (!io_ok) {
        if (!exited) {
            (void) xr_proc_kill_tree(pid, 9);
            (void) xr_proc_wait(pid, &out->exit_code);
        }
        free(stdout_capture.data);  // xr:allow-raw-alloc
        free(stderr_capture.data);  // xr:allow-raw-alloc
        xtc_process_error(err, err_size, "failed while capturing process output");
        return false;
    }
    out->stdout_data = stdout_capture.data;
    out->stderr_data = stderr_capture.data;
    out->output_truncated = stdout_capture.truncated || stderr_capture.truncated;
#endif

    out->duration_ms = xr_time_monotonic_ms() - start_ms;
    return true;
}

XR_FUNC void xtc_process_result_free(XrProcessResult *result) {
    if (!result)
        return;
    free(result->stdout_data);  // xr:allow-raw-alloc
    free(result->stderr_data);  // xr:allow-raw-alloc
    memset(result, 0, sizeof(*result));
    result->exit_code = -1;
}
