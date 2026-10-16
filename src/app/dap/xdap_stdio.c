/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_stdio.c - Owned platform stdio provider for strict DAP framing
 */
#include "xdap_stdio.h"
#include "xdap_framed.h"
#include <stdio.h>
#ifdef XR_OS_WINDOWS
#include "xdap_stdio_win.inc.c"
#else
#include "xdap_stdio_unix.inc.c"
#endif
struct XdapStdio {
    XdapFramed *framed;
    int input_mode;
    bool modes;
};
static XrXirStatus stdio_read(void *context,char *bytes,size_t capacity,size_t *length,bool *eof) {
    (void)context;return stdio_platform_read(bytes,capacity,length,eof);
}
static XrXirStatus stdio_wait(void *context,uint32_t milliseconds) {
    (void)context;return stdio_platform_wait(milliseconds);
}
static XrXirStatus stdio_write(void *context,const char *bytes,size_t length) {
    (void)context;
#ifdef XR_OS_WINDOWS
    return stdio_platform_write(bytes,length);
#else
    return fwrite(bytes,1,length,stdout)==length && !fflush(stdout) ? XR_XIR_OK : XR_XIR_IO;
#endif
}
XR_FUNC void xdap_stdio_free(XdapStdio *io) {
    if (!io) return;
    xdap_framed_free(io->framed);
#ifndef XR_OS_WINDOWS
    if (io->modes) fcntl(STDIN_FILENO,F_SETFL,io->input_mode);
#endif
    xr_compile_resources_free(io);
}
XR_FUNC XrXirStatus xdap_stdio_new(XrCompileResources *resources,size_t limit,XdapStdio **output) {
    if (!resources || !output || *output || !limit || limit>1048576) return XR_XIR_BAD_STRUCTURE;
    XdapStdio *io=NULL;XrCompileResourceStatus allocated=xr_compile_resources_calloc(resources,1,sizeof(*io),(void **)&io);
    if (allocated!=XR_COMPILE_RESOURCE_OK) return allocated==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
#ifdef XR_OS_WINDOWS
    status=stdio_platform_open();
#else
    io->input_mode=fcntl(STDIN_FILENO,F_GETFL);
    if (io->input_mode<0 || fcntl(STDIN_FILENO,F_SETFL,io->input_mode|O_NONBLOCK)<0) status=XR_XIR_IO;
#endif
    if (status!=XR_XIR_OK) { xdap_stdio_free(io);return status; }
    io->modes=true;
    XdapByteStream stream={io,stdio_read,stdio_write,stdio_wait};status=xdap_framed_new(resources,limit,&stream,&io->framed);
    if (status!=XR_XIR_OK) { xdap_stdio_free(io);return status; }
    *output=io;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xdap_stdio_read(XdapStdio *io,char **output,size_t *length,bool *eof) {
    return io ? xdap_framed_read(io->framed,output,length,eof) : XR_XIR_BAD_STRUCTURE;
}
XR_FUNC XrXirStatus xdap_stdio_write(void *context,const char *bytes,size_t length) {
    XdapStdio *io=context;return io ? xdap_framed_write(io->framed,bytes,length) : XR_XIR_BAD_STRUCTURE;
}
XR_FUNC XrXirStatus xdap_stdio_wait(XdapStdio *io,uint32_t milliseconds) {
    return io ? xdap_framed_wait(io->framed,milliseconds) : XR_XIR_BAD_STRUCTURE;
}
XR_FUNC XrXirOutputStatus xdap_stdio_host_output(void *context,XrXirOutputStream stream,const char *bytes,size_t length) {
    if (context || (length && !bytes) || length>1048576 || (stream!=XR_XIR_STDOUT && stream!=XR_XIR_STDERR))
        return XR_XIR_OUTPUT_BAD_ARGUMENT;
#ifdef XR_OS_WINDOWS
    XrXirStatus status=stdio_platform_write_stream(stream==XR_XIR_STDOUT ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE,bytes,length);
    return status==XR_XIR_OK ? XR_XIR_OUTPUT_OK : XR_XIR_OUTPUT_ERROR;
#else
    FILE *output=stream==XR_XIR_STDOUT ? stdout : stderr;
    return fwrite(bytes,1,length,output)==length && !fflush(output) ? XR_XIR_OUTPUT_OK : XR_XIR_OUTPUT_ERROR;
#endif
}
