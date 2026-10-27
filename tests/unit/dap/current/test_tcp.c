/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_tcp.c - Actual loopback framing, EOF and compiler-owned failure prefixes
 */
#define main reference_json_main
#include "test_json.c"
#undef main
#include "app/dap/xdap_tcp.h"
#include "os/os_net.h"
static xr_socket_t peer_new(XdapTcp *io) {
    xr_socket_t peer=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);CHECK(peer!=XR_INVALID_SOCKET);
    struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons((uint16_t)xdap_tcp_port(io));
    CHECK(!connect(peer,(struct sockaddr *)&address,sizeof(address)));CHECK(!xr_socket_set_nodelay(peer,true));return peer;
}
static void peer_send(xr_socket_t peer,const char *bytes,size_t length) {
    while (length) { ssize_t sent=xr_socket_send(peer,bytes,length);CHECK(sent>0);bytes+=sent;length-=(size_t)sent; }
}
static XrXirStatus read_message(XdapTcp *io,char **message,size_t *length,bool *eof) {
    XrXirStatus status=XR_XIR_OK;
    for (unsigned i=0;i<100 && !*message && !*eof && status==XR_XIR_OK;++i) {
        status=xdap_tcp_read(io,message,length,eof);
        if (status==XR_XIR_OK && !*message && !*eof) CHECK(xdap_tcp_wait(io,10)==XR_XIR_OK);
    }
    return status;
}
static void socket_behavior(void) {
    XrCompileResourceStats baseline;XrCompileResources *owner=owner_new(&baseline);XdapTcp *io=NULL;
    CHECK(xdap_tcp_new(owner,0,8192,&io)==XR_XIR_OK && xdap_tcp_port(io));
    CHECK(xdap_tcp_write(io,"x",1)==XR_XIR_IO);
    xr_socket_t peer=peer_new(io);char *message=NULL;size_t length=777;bool eof=false;
    const char first[]="Content-Length: 2\r\n\r\nab";
    for (size_t i=0;i<sizeof(first)-1;++i) {
        peer_send(peer,first+i,1);CHECK(xdap_tcp_read(io,&message,&length,&eof)==XR_XIR_OK && !eof);
        CHECK(message || length==777);if (message) { CHECK(i==sizeof(first)-2 && length==2 && !memcmp(message,"ab",2)); }
    }
    if (!message) CHECK(read_message(io,&message,&length,&eof)==XR_XIR_OK);
    CHECK(message && length==2);xr_compile_resources_free(message);message=NULL;
    const char value[]="owned\0value\n";CHECK(xdap_tcp_write(io,value,sizeof(value)-1)==XR_XIR_OK);
    char received[128],expected[128];int header=snprintf(expected,sizeof(expected),"Content-Length: %zu\r\n\r\n",sizeof(value)-1);
    CHECK(header>0);memcpy(expected+header,value,sizeof(value)-1);size_t total=(size_t)header+sizeof(value)-1,at=0;
    while (at<total) { ssize_t count=xr_recv_timeout(peer,received+at,total-at,1000);CHECK(count>0);at+=(size_t)count; }
    CHECK(!memcmp(received,expected,total));
    const char last[]="Content-Length: 1\r\n\r\ncContent-Length: 1\r\n\r\nd";
    peer_send(peer,last,sizeof(last)-1);CHECK(!shutdown(peer,XR_SHUT_WR));
    for (unsigned i=0;i<2;++i) { length=777;eof=false;CHECK(read_message(io,&message,&length,&eof)==XR_XIR_OK);
        CHECK(message && length==1 && message[0]==(i ? 'd' : 'c'));xr_compile_resources_free(message);message=NULL; }
    length=777;eof=false;CHECK(read_message(io,&message,&length,&eof)==XR_XIR_OK && eof && !message && length==777);
    CHECK(!xr_closesocket(peer));xdap_tcp_free(io);owner_free(owner,baseline);
    puts("Actual loopback fragmentation, exact NUL output, buffered final frames before peer EOF: PASS");
}
static void rejected_inputs(void) {
    const char *bad[]={"Content-Length: 2junk\r\n\r\naa","Content-Length: 2\r\nContent-Length: 2\r\n\r\naa",
        "X-Content-Length: 2\r\n\r\naa","Content-Length: 9000\r\n\r\n","Content-Length: 10\r\n\r\naa"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        XrCompileResourceStats baseline;XrCompileResources *owner=owner_new(&baseline);XdapTcp *io=NULL;
        CHECK(xdap_tcp_new(owner,0,8192,&io)==XR_XIR_OK);xr_socket_t peer=peer_new(io);
        peer_send(peer,bad[i],strlen(bad[i]));CHECK(!shutdown(peer,XR_SHUT_WR));
        XrXirStatus expected=i==3 ? XR_XIR_BUDGET : i==4 ? XR_XIR_IO : XR_XIR_BAD_STRUCTURE;
        for (unsigned retry=0;retry<2;++retry) { char *message=NULL;size_t length=777;bool eof=false;
            CHECK(read_message(io,&message,&length,&eof)==expected && !message && length==777); }
        if (i==4) CHECK(xdap_tcp_write(io,"x",1)==XR_XIR_IO && xdap_tcp_wait(io,0)==XR_XIR_IO);
        CHECK(!xr_closesocket(peer));xdap_tcp_free(io);owner_free(owner,baseline);
    }
    XrCompileResourceStats baseline;XrCompileResources *owner=owner_new(&baseline);XdapTcp *io=NULL,*other=NULL;
    CHECK(xdap_tcp_new(owner,0,8192,&io)==XR_XIR_OK);uint32_t port=xdap_tcp_port(io);
    CHECK(xdap_tcp_new(owner,port,8192,&other)==XR_XIR_IO && !other);
    CHECK(xdap_tcp_new(owner,65536,8192,&other)==XR_XIR_BAD_STRUCTURE && !other);
    CHECK(xdap_tcp_wait(io,1001)==XR_XIR_BAD_STRUCTURE);xdap_tcp_free(io);
    CHECK(xdap_tcp_new(owner,port,8192,&other)==XR_XIR_OK);xdap_tcp_free(other);owner_free(owner,baseline);
    puts("Repeated strict rejection, partial EOF, occupied port, owned port release and input bounds: PASS");
}
static size_t socket_failure(unsigned mode,size_t fail) {
    XrCompileResourceStats baseline;XrCompileResources *owner=owner_new(&baseline);XdapTcp *io=NULL;
    xr_socket_t peer=XR_INVALID_SOCKET;size_t body=mode==2 ? 6000 : 2;
    if (mode) {
        CHECK(xdap_tcp_new(owner,0,8192,&io)==XR_XIR_OK);peer=peer_new(io);
        char bytes[7000];int header=snprintf(bytes,sizeof(bytes),"Content-Length: %zu\r\n\r\n",body);CHECK(header>0);
        memset(bytes+header,'a',body);peer_send(peer,bytes,(size_t)header+body);
    }
    char *message=NULL;size_t length=777;bool eof=false;size_t begin=runtime_attempts;
    runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirStatus status=!mode ? xdap_tcp_new(owner,0,8192,&io) : read_message(io,&message,&length,&eof);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail==SIZE_MAX) { CHECK(status==XR_XIR_OK);if (mode) CHECK(message && length==body); }
    else {
        CHECK(status==XR_XIR_OUT_OF_MEMORY && sites>fail && !message && length==777);
        XrCompileResourceStats spent={0};CHECK(xr_compile_resources_stats(owner,&spent)==XR_COMPILE_RESOURCE_OK);
        if (!mode) { CHECK(!io && spent.held_bytes==baseline.held_bytes);CHECK(xdap_tcp_new(owner,0,8192,&io)==XR_XIR_OK); }
        else { CHECK(read_message(io,&message,&length,&eof)==XR_XIR_OK);CHECK(message && length==body); }
        XrCompileResourceStats retry={0};CHECK(xr_compile_resources_stats(owner,&retry)==XR_COMPILE_RESOURCE_OK);
        CHECK(retry.allocated_bytes>=spent.allocated_bytes);
    }
    if (message) for (size_t i=0;i<length;++i) CHECK(message[i]=='a');
    xr_compile_resources_free(message);if (peer!=XR_INVALID_SOCKET) CHECK(!xr_closesocket(peer));
    xdap_tcp_free(io);owner_free(owner,baseline);return sites;
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    socket_behavior();rejected_inputs();
    for (unsigned mode=0;mode<3;++mode) { size_t sites=socket_failure(mode,SIZE_MAX);CHECK(sites);
        if (resources) for (size_t i=0;i<sites;++i) socket_failure(mode,i);
        printf("Actual TCP allocation slice %u: %zu sites, injection=%s; original ledger retry and physical0/0\n",mode,sites,resources ? "ALL" : "NOT_RUN"); }
    return 0;
}
