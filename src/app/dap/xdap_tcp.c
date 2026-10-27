/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_tcp.c - One owned loopback socket with bounded nonblocking I/O
 */
#include "xdap_tcp.h"
#include "../../os/os_net.h"
#include "../../execution/xr_xir_host_time.h"
struct XdapTcp {
    XdapFramed *framed;
    xr_socket_t listener,peer;
    uint32_t port;
    bool startup,failed;
};
static bool tcp_selectable(xr_socket_t socket_fd) {
#ifdef XR_OS_WINDOWS
    return socket_fd!=XR_INVALID_SOCKET;
#else
    return socket_fd>=0 && socket_fd<FD_SETSIZE;
#endif
}
static XrXirStatus tcp_ready(xr_socket_t socket_fd,bool write,uint32_t milliseconds) {
    if (!tcp_selectable(socket_fd)) return XR_XIR_IO;

    fd_set fds;FD_ZERO(&fds);FD_SET(socket_fd,&fds);
    struct timeval timeout={(long)(milliseconds/1000),(long)((milliseconds%1000)*1000)};
    /* Winsock ignores nfds. Keep the native-width socket value intact. */
#ifdef XR_OS_WINDOWS
    int nfds=0;
#else
    int nfds=socket_fd+1;
#endif
    int ready=select(nfds,write ? NULL : &fds,write ? &fds : NULL,NULL,&timeout);
    return ready>=0 || xr_get_socket_error()==XR_EINTR ? XR_XIR_OK : XR_XIR_IO;
}
static XrXirStatus tcp_read_bytes(void *context,char *bytes,size_t capacity,size_t *length,bool *eof) {
    XdapTcp *io=context;
    if (io->peer==XR_INVALID_SOCKET) {
        xr_socket_t peer=accept(io->listener,NULL,NULL);
        if (peer==XR_INVALID_SOCKET) return xr_socket_err_is_again(xr_get_socket_error()) || xr_get_socket_error()==XR_EINTR ? XR_XIR_OK : XR_XIR_IO;
        if (!tcp_selectable(peer) || xr_socket_set_nonblocking(peer) || xr_socket_set_nodelay(peer,true)) {
            xr_closesocket(peer);return XR_XIR_IO;
        }
#if !defined(XR_OS_WINDOWS) && defined(SO_NOSIGPIPE)
        int no_sigpipe=1;
        if (setsockopt(peer,SOL_SOCKET,SO_NOSIGPIPE,&no_sigpipe,sizeof(no_sigpipe))) { xr_closesocket(peer);return XR_XIR_IO; }
#endif
        io->peer=peer;xr_closesocket(io->listener);io->listener=XR_INVALID_SOCKET;
    }
    ssize_t received=xr_socket_recv(io->peer,bytes,capacity);
    if (received<0) return xr_socket_err_is_again(xr_get_socket_error()) || xr_get_socket_error()==XR_EINTR ? XR_XIR_OK : XR_XIR_IO;
    *length=(size_t)received;*eof=received==0;return XR_XIR_OK;
}
static XrXirStatus tcp_clock(uint64_t *output) {

    XrXirTimeProvider time={0};xr_xir_host_time_provider(&time);int64_t now=0;
    if (!time.clock || time.clock(time.context,XR_XIR_CLOCK_MONOTONIC,&now)!=XR_XIR_TIME_OK || now<0) return XR_XIR_IO;
    *output=(uint64_t)now;return XR_XIR_OK;
}
static XrXirStatus tcp_write_bytes(void *context,const char *bytes,size_t length) {
    XdapTcp *io=context;if (io->peer==XR_INVALID_SOCKET) return XR_XIR_IO;
    uint64_t start=0;XrXirStatus status=tcp_clock(&start);if (status!=XR_XIR_OK) return status;
    while (length) {
        size_t chunk=length>65536 ? 65536 : length;
#ifdef XR_OS_WINDOWS
        ssize_t written=xr_socket_send(io->peer,bytes,chunk);
#elif defined(MSG_NOSIGNAL)
        /* The endpoint must report a closed peer instead of terminating its host. */
        ssize_t written=send(io->peer,bytes,chunk,MSG_NOSIGNAL);
#else
        ssize_t written=xr_socket_send(io->peer,bytes,chunk);
#endif
        if (written>0) { bytes+=written;length-=(size_t)written; }
        else if (!written || (!xr_socket_err_is_again(xr_get_socket_error()) && xr_get_socket_error()!=XR_EINTR)) return XR_XIR_IO;
        if (!length) return XR_XIR_OK;
        uint64_t now=0;status=tcp_clock(&now);if (status!=XR_XIR_OK) return status;
        if (now<start || now-start>=UINT64_C(1000000000)) return XR_XIR_IO;
        status=tcp_ready(io->peer,true,10);if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus tcp_wait_bytes(void *context,uint32_t milliseconds) {
    XdapTcp *io=context;return tcp_ready(io->peer!=XR_INVALID_SOCKET ? io->peer : io->listener,false,milliseconds);
}
XR_FUNC void xdap_tcp_free(XdapTcp *io) {
    if (!io) return;
    if (io->peer!=XR_INVALID_SOCKET) xr_closesocket(io->peer);
    if (io->listener!=XR_INVALID_SOCKET) xr_closesocket(io->listener);
    if (io->startup) xr_winsock_cleanup();
    xdap_framed_free(io->framed);xr_compile_resources_free(io);
}
XR_FUNC XrXirStatus xdap_tcp_new(XrCompileResources *resources,uint32_t port,size_t limit,XdapTcp **output) {
    if (!resources || !output || *output || port>65535 || !limit || limit>1048576) return XR_XIR_BAD_STRUCTURE;
#if !defined(XR_OS_WINDOWS) && !defined(MSG_NOSIGNAL) && !defined(SO_NOSIGPIPE)
    return XR_XIR_UNSUPPORTED;
#endif
    XdapTcp *io=NULL;XrCompileResourceStatus allocated=xr_compile_resources_calloc(resources,1,sizeof(*io),(void **)&io);
    if (allocated!=XR_COMPILE_RESOURCE_OK) return allocated==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
        allocated==XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_XIR_MEMORY_LIMIT :
        allocated==XR_COMPILE_RESOURCE_OVERFLOW ? XR_XIR_OVERFLOW :
        allocated==XR_COMPILE_RESOURCE_CANCELLED ? XR_XIR_CANCELLED :
        allocated==XR_COMPILE_RESOURCE_DEADLINE ? XR_XIR_DEADLINE :
        allocated==XR_COMPILE_RESOURCE_CLOCK_FAILURE ? XR_XIR_CLOCK_FAILURE : XR_XIR_BAD_STRUCTURE;
    io->listener=io->peer=XR_INVALID_SOCKET;
    XrXirStatus status=XR_XIR_IO;
    if (xr_winsock_init()) goto failed;
    io->startup=true;io->listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if (!tcp_selectable(io->listener) || xr_socket_set_nonblocking(io->listener)) goto failed;
    struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons((uint16_t)port);
    if (bind(io->listener,(struct sockaddr *)&address,sizeof(address)) || listen(io->listener,1)) goto failed;
    socklen_t address_length=sizeof(address);
    if (getsockname(io->listener,(struct sockaddr *)&address,&address_length) || address_length!=sizeof(address)) goto failed;
    io->port=ntohs(address.sin_port);
    XdapByteStream stream={io,tcp_read_bytes,tcp_write_bytes,tcp_wait_bytes};status=xdap_framed_new(resources,limit,&stream,&io->framed);
    if (status!=XR_XIR_OK) goto failed;
    *output=io;return XR_XIR_OK;
failed:
    xdap_tcp_free(io);return status;
}
XR_FUNC uint32_t xdap_tcp_port(const XdapTcp *io) { return io ? io->port : 0; }
XR_FUNC XrXirStatus xdap_tcp_read(XdapTcp *io,char **output,size_t *length,bool *eof) {
    if (!io) return XR_XIR_BAD_STRUCTURE;
    if (io->failed) return XR_XIR_IO;
    XrXirStatus status=xdap_framed_read(io->framed,output,length,eof);
    if (status==XR_XIR_IO) io->failed=true;
    return status;
}
XR_FUNC XrXirStatus xdap_tcp_write(void *context,const char *bytes,size_t length) {
    XdapTcp *io=context;if (!io) return XR_XIR_BAD_STRUCTURE;
    if (io->failed) return XR_XIR_IO;
    XrXirStatus status=xdap_framed_write(io->framed,bytes,length);
    if (status==XR_XIR_IO && io->peer!=XR_INVALID_SOCKET) io->failed=true;
    return status;
}
XR_FUNC XrXirStatus xdap_tcp_wait(XdapTcp *io,uint32_t milliseconds) {
    if (!io) return XR_XIR_BAD_STRUCTURE;
    if (io->failed) return XR_XIR_IO;
    XrXirStatus status=xdap_framed_wait(io->framed,milliseconds);if (status==XR_XIR_IO) io->failed=true;
    return status;
}
