/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstdio_binary.h - Hosted CRT byte streams before their first read or write
 */
#ifndef XSTDIO_BINARY_H
#define XSTDIO_BINARY_H
#include "xdefs.h"
#include <stdbool.h>
#include <stdio.h>
#if defined(XR_OS_WINDOWS)
#include <fcntl.h>
#include <io.h>
#endif
/* An owning executable calls this in its own CRT before any stream input.
 * Embedders retain that responsibility; Instance/configuration APIs do not
 * mutate process stream modes or recover bytes already translated by text I/O. */
static inline bool xr_stdio_binary_init(void) {
#if defined(XR_OS_WINDOWS)
    FILE *streams[]={stdin,stdout,stderr};int descriptors[3];
    for(unsigned n=0;n<3;++n) {
        descriptors[n]=_fileno(streams[n]);if(descriptors[n]<0)return false;
    }
    for(unsigned n=0;n<3;++n)if(_setmode(descriptors[n],_O_BINARY)==-1)return false;
#endif
    return true;
}
#endif // XSTDIO_BINARY_H
