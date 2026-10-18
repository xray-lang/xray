/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_stdio_unix.inc.c - Unix nonblocking pipe reads with caller-owned descriptors
 */
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
static XrXirStatus stdio_platform_read(char *bytes,size_t capacity,size_t *length,bool *eof) {
    ssize_t result=read(STDIN_FILENO,bytes,capacity);
    if (result<0) return errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR ? XR_XIR_OK : XR_XIR_IO;
    *length=(size_t)result;*eof=result==0;return XR_XIR_OK;
}
static XrXirStatus stdio_platform_wait(uint32_t milliseconds) {
    struct pollfd input={STDIN_FILENO,POLLIN,0};
    int status=poll(&input,1,(int)milliseconds);
    return status>=0 || errno==EINTR ? XR_XIR_OK : XR_XIR_IO;
}
