/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_stdio.c - Cumulative framed stdio ownership and exact output writes
 */
#include "xdap_stdio.h"
#include "../../base/xframing.h"
#include <stdio.h>
#include <string.h>
#ifdef XR_OS_WINDOWS
#include "xdap_stdio_win.inc.c"
#else
#include "xdap_stdio_unix.inc.c"
#endif
struct XdapStdio {
    XrCompileResources *resources;
    char *bytes;
    size_t length,capacity,limit,header;
    int content,input_mode,output_mode;
    bool eof,modes;
};
static XrXirStatus stdio_resource(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
        status==XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
}
XR_FUNC void xdap_stdio_free(XdapStdio *io) {
    if (!io) return;
    if (io->modes) {
#ifdef XR_OS_WINDOWS
        stdio_platform_mode(_fileno(stdin),io->input_mode);stdio_platform_mode(_fileno(stdout),io->output_mode);
#else
        fcntl(STDIN_FILENO,F_SETFL,io->input_mode);
#endif
    }
    xr_compile_resources_free(io->bytes);xr_compile_resources_free(io);
}
XR_FUNC XrXirStatus xdap_stdio_new(XrCompileResources *resources,size_t limit,XdapStdio **output) {
    if (!resources || !output || *output || !limit || limit>1048576) return XR_XIR_BAD_STRUCTURE;
    XdapStdio *io=NULL;XrXirStatus status=stdio_resource(xr_compile_resources_calloc(resources,1,sizeof(*io),(void **)&io));
    if (status!=XR_XIR_OK) return status;
    io->resources=resources;io->limit=limit;io->content=-1;
    io->capacity=limit+256<4096 ? limit+256 : 4096;
    status=stdio_resource(xr_compile_resources_alloc(resources,io->capacity,(void **)&io->bytes));
    if (status!=XR_XIR_OK) { xdap_stdio_free(io);return status; }
#ifdef XR_OS_WINDOWS
    io->input_mode=stdio_platform_mode(_fileno(stdin),_O_BINARY);
    if (io->input_mode<0) { xdap_stdio_free(io);return XR_XIR_IO; }
    io->output_mode=stdio_platform_mode(_fileno(stdout),_O_BINARY);
    if (io->output_mode<0) { stdio_platform_mode(_fileno(stdin),io->input_mode);xdap_stdio_free(io);return XR_XIR_IO; }
#else
    io->input_mode=fcntl(STDIN_FILENO,F_GETFL);
    if (io->input_mode<0 || fcntl(STDIN_FILENO,F_SETFL,io->input_mode|O_NONBLOCK)<0) { xdap_stdio_free(io);return XR_XIR_IO; }
#endif
    io->modes=true;*output=io;return XR_XIR_OK;
}
static XrXirStatus stdio_extract(XdapStdio *io,char **output,size_t *length) {
    if (io->content<0) {
        size_t scanned=io->length>256 ? 256 : io->length;
        XrXirStatus status=stdio_resource(xr_compile_resources_work(io->resources,scanned));
        if (status!=XR_XIR_OK) return status;
        XrFrameStatus parsed=xr_frame_parse(io->bytes,scanned,&io->header,&io->content);
        if (parsed==XR_FRAME_ERROR || (io->content<0 && io->length>=256)) return XR_XIR_BAD_STRUCTURE;
        if (io->content<0) return XR_XIR_OK;
        if ((size_t)io->content>io->limit) return XR_XIR_BUDGET;
    }
    size_t total=io->header+(size_t)io->content;
    if (io->length<total) return XR_XIR_OK;
    char *message=NULL;XrXirStatus status=stdio_resource(xr_compile_resources_alloc(io->resources,(size_t)io->content+1,(void **)&message));
    if (status!=XR_XIR_OK) return status;
    status=stdio_resource(xr_compile_resources_work(io->resources,io->length));
    if (status!=XR_XIR_OK) { xr_compile_resources_free(message);return status; }
    memcpy(message,io->bytes+io->header,(size_t)io->content);message[io->content]=0;
    size_t remaining=io->length-total;memmove(io->bytes,io->bytes+total,remaining);
    *length=(size_t)io->content;*output=message;io->length=remaining;io->header=0;io->content=-1;
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xdap_stdio_read(XdapStdio *io,char **output,size_t *length,bool *eof) {
    if (!io || !output || *output || !length || !eof) return XR_XIR_BAD_STRUCTURE;
    *eof=false;XrXirStatus status=stdio_extract(io,output,length);
    if (status!=XR_XIR_OK || *output) return status;
    if (io->eof) { *eof=true;return io->length ? XR_XIR_IO : XR_XIR_OK; }
    if (io->length==io->capacity) {
        size_t capacity=io->capacity*2;if (capacity>io->limit+256) capacity=io->limit+256;
        if (capacity<=io->capacity) return XR_XIR_BUDGET;
        status=stdio_resource(xr_compile_resources_resize(io->resources,(void **)&io->bytes,capacity));
        if (status!=XR_XIR_OK) return status;
        io->capacity=capacity;
    }
    size_t count=0;status=stdio_platform_read(io->bytes+io->length,io->capacity-io->length,&count,&io->eof);
    if (status!=XR_XIR_OK) return status;
    status=stdio_resource(xr_compile_resources_work(io->resources,count));if (status!=XR_XIR_OK) return status;
    io->length+=count;status=stdio_extract(io,output,length);
    if (status==XR_XIR_OK && !*output && io->eof) { *eof=true;if (io->length) status=XR_XIR_IO; }
    return status;
}
XR_FUNC XrXirStatus xdap_stdio_write(void *context,const char *bytes,size_t length) {
    XdapStdio *io=context;
    if (!io || (!bytes && length) || length>io->limit) return XR_XIR_BAD_STRUCTURE;
    char header[64];int size=xr_frame_write_header(header,sizeof(header),length);
    if (size<0) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=stdio_resource(xr_compile_resources_work(io->resources,length+(uint32_t)size));
    if (status!=XR_XIR_OK) return status;
    if (fwrite(header,1,(size_t)size,stdout)!=(size_t)size || fwrite(bytes,1,length,stdout)!=length || fflush(stdout)) return XR_XIR_IO;
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xdap_stdio_wait(XdapStdio *io,uint32_t milliseconds) {
    if (!io || milliseconds>1000) return XR_XIR_BAD_STRUCTURE;
    return stdio_platform_wait(milliseconds);
}
