/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * pipe_probe_posix_cases.h - Preserve bytes published as the writer closes
 */
#include "os/os_pipe.h"
#include <sys/ioctl.h>

static XrPipeHandle probe_writer = XR_PIPE_INVALID;
static bool probe_publish_after_count;
static const uint8_t probe_tail[] = {'t', 'a', 'i', 'l', 0, 255};

/* Publish after the kernel's count observation and before the caller's next
 * system call. This forces the empty-to-readable-and-closed race without
 * sleeps, threads or a probabilistic child-process schedule. */
static int probe_ioctl(int descriptor, unsigned long request, void *output) {
    int result = ioctl(descriptor, request, output);
    if (!result && request == FIONREAD && probe_publish_after_count) {
        probe_publish_after_count = false;
        CHECK(xr_pipe_write(probe_writer, probe_tail, sizeof(probe_tail)) == (int64_t)sizeof(probe_tail));
        CHECK(xr_pipe_close(probe_writer) == 0);
        probe_writer = XR_PIPE_INVALID;
    }
    return result;
}
#define ioctl(descriptor, request, output) probe_ioctl(descriptor, request, output)
#include "os/unix/pipe_unix.c"
#undef ioctl

static void pipe_probe_closed_tail(void) {
    XrPipe channel = {XR_PIPE_INVALID, XR_PIPE_INVALID};
    CHECK(xr_pipe_create(&channel, NULL) == 0);
    size_t available = SIZE_MAX;
    bool eof = true;
    CHECK(xr_pipe_probe(channel.read, &available, &eof) == XR_PIPE_IO_WOULD_BLOCK);
    CHECK(!available && !eof);

    probe_writer = channel.write;
    probe_publish_after_count = true;
    XrPipeIoStatus status = xr_pipe_probe(channel.read, &available, &eof);
    CHECK(!probe_publish_after_count && probe_writer == XR_PIPE_INVALID && !eof);
    if (status == XR_PIPE_IO_WOULD_BLOCK)
        status = xr_pipe_probe(channel.read, &available, &eof);
    CHECK(status == XR_PIPE_IO_OK && available == sizeof(probe_tail) && !eof);

    uint8_t received[sizeof(probe_tail)] = {0};
    CHECK(xr_pipe_read(channel.read, received, 2) == 2);
    CHECK(xr_pipe_probe(channel.read, &available, &eof) == XR_PIPE_IO_OK);
    CHECK(available == sizeof(probe_tail) - 2 && !eof);
    CHECK(xr_pipe_read(channel.read, received + 2, available) == (int64_t)available);
    CHECK(!memcmp(received, probe_tail, sizeof(received)));
    CHECK(xr_pipe_probe(channel.read, &available, &eof) == XR_PIPE_IO_OK);
    CHECK(!available && eof);
    CHECK(xr_pipe_close(channel.read) == 0);
}
