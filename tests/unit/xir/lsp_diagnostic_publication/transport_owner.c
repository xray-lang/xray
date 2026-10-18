/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#define xlsp_transport_write publication_transport_write_actual
#include "app/lsp/xlsp_transport.c"
#undef xlsp_transport_write
#include "os/os_pipe.h"
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
static size_t publication_write_count,publication_write_fail=SIZE_MAX;
XR_FUNC void publication_transport_fault(size_t after){publication_write_fail=publication_write_count+after;}
XR_FUNC size_t publication_transport_writes(void){return publication_write_count;}
XR_FUNC void xlsp_transport_write(XrLspTransport *transport,const char *text,size_t length) {
    if(publication_write_count++==publication_write_fail) {
        XrPipe pipe={XR_PIPE_INVALID,XR_PIPE_INVALID};
        if(xr_pipe_create(&pipe,NULL)!=0||xr_pipe_close(pipe.read)!=0)abort();
        int fd=_open_osfhandle(pipe.write,_O_BINARY);if(fd<0){xr_pipe_close(pipe.write);abort();}
        int saved=transport->write_fd;transport->write_fd=fd;
        publication_transport_write_actual(transport,text,length);
        _close(fd);transport->write_fd=saved;return;
    }
    publication_transport_write_actual(transport,text,length);
}
