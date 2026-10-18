/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_framed.c - Cumulative framing shared by owned host byte streams
 */
#include "xdap_framed.h"
#include "../../base/xframing.h"
#include <stdio.h>
#include <string.h>
struct XdapFramed {
    XdapByteStream stream;
    XrCompileResources *resources;
    char *bytes;
    size_t length,capacity,limit,header;
    int content;
    bool eof;
};
static XrXirStatus framed_resource(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
        status==XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
}
static bool framed_header_name(const char *bytes,size_t length) {
    static const char name[]="content-length";
    if (length!=sizeof(name)-1) return false;
    for (size_t i=0;i<length;++i) {
        unsigned char c=(unsigned char)bytes[i];if (c>='A' && c<='Z') c=(unsigned char)(c+('a'-'A'));
        if (c!=(unsigned char)name[i]) return false;
    }
    return true;
}
/* A field match must occupy a complete header line. The shared framing parser
 * locates the separator; this boundary rejects ambiguous length declarations. */
static bool framed_header_valid(const char *bytes,size_t header,int content) {
    if (header<4 || header>256) return false;
    size_t at=0;unsigned lengths=0;
    while (at<header-2) {
        size_t end=at;while (end+1<header && !(bytes[end]=='\r' && bytes[end+1]=='\n')) ++end;
        if (end+1>=header || end==at) return false;
        size_t colon=at;while (colon<end && bytes[colon]!=':') {
            unsigned char c=(unsigned char)bytes[colon];
            if (!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-')) return false;
            ++colon;
        }
        if (colon==at || colon==end) return false;
        for (size_t i=colon+1;i<end;++i) {
            unsigned char c=(unsigned char)bytes[i];if ((c<32 && c!='\t') || c>126) return false;
        }
        if (framed_header_name(bytes+at,colon-at)) {
            if (++lengths!=1) return false;
            size_t value=colon+1;while (value<end && (bytes[value]==' ' || bytes[value]=='\t')) ++value;
            uint64_t length=0;size_t digits=value;
            while (value<end && bytes[value]>='0' && bytes[value]<='9') {
                length=length*10+(uint32_t)(bytes[value++]-'0');if (length>XR_FRAME_MAX_BODY) return false;
            }
            if (value==digits) return false;
            while (value<end && (bytes[value]==' ' || bytes[value]=='\t')) ++value;
            if (value!=end || length!=(uint64_t)content) return false;
        }
        at=end+2;
    }
    return lengths==1 && at==header-2;
}
XR_FUNC void xdap_framed_free(XdapFramed *io) {
    if (!io) return;
    xr_compile_resources_free(io->bytes);xr_compile_resources_free(io);
}
XR_FUNC XrXirStatus xdap_framed_new(XrCompileResources *resources,size_t limit,const XdapByteStream *stream,XdapFramed **output) {
    if (!resources || !output || *output || !limit || limit>1048576 || !stream ||
        !stream->read || !stream->write || !stream->wait) return XR_XIR_BAD_STRUCTURE;
    XdapFramed *io=NULL;XrXirStatus status=framed_resource(xr_compile_resources_calloc(resources,1,sizeof(*io),(void **)&io));
    if (status!=XR_XIR_OK) return status;
    io->resources=resources;io->limit=limit;io->content=-1;io->stream=*stream;
    io->capacity=limit+256<4096 ? limit+256 : 4096;
    status=framed_resource(xr_compile_resources_alloc(resources,io->capacity,(void **)&io->bytes));
    if (status!=XR_XIR_OK) { xdap_framed_free(io);return status; }
    *output=io;return XR_XIR_OK;
}
static XrXirStatus framed_extract(XdapFramed *io,char **output,size_t *length) {
    if (io->content<0) {
        size_t scanned=io->length>256 ? 256 : io->length;
        /* Pay the bounded separator/field comparisons and strict line scan. */
        XrXirStatus status=framed_resource(xr_compile_resources_work(io->resources,scanned*16));
        if (status!=XR_XIR_OK) return status;
        size_t header=0;int content=-1;
        XrFrameStatus parsed=xr_frame_parse(io->bytes,scanned,&header,&content);
        if (parsed==XR_FRAME_ERROR || (content<0 && io->length>=256)) return XR_XIR_BAD_STRUCTURE;
        if (content<0) return XR_XIR_OK;
        if (!framed_header_valid(io->bytes,header,content)) return XR_XIR_BAD_STRUCTURE;
        if ((size_t)content>io->limit) return XR_XIR_BUDGET;
        io->header=header;io->content=content;
    }
    size_t total=io->header+(size_t)io->content;
    if (io->length<total) return XR_XIR_OK;
    char *message=NULL;XrXirStatus status=framed_resource(xr_compile_resources_alloc(io->resources,(size_t)io->content+1,(void **)&message));
    if (status!=XR_XIR_OK) return status;
    status=framed_resource(xr_compile_resources_work(io->resources,io->length));
    if (status!=XR_XIR_OK) { xr_compile_resources_free(message);return status; }
    memcpy(message,io->bytes+io->header,(size_t)io->content);message[io->content]=0;
    size_t remaining=io->length-total;memmove(io->bytes,io->bytes+total,remaining);
    *length=(size_t)io->content;*output=message;io->length=remaining;io->header=0;io->content=-1;
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xdap_framed_read(XdapFramed *io,char **output,size_t *length,bool *eof) {
    if (!io || !output || *output || !length || !eof) return XR_XIR_BAD_STRUCTURE;
    *eof=false;XrXirStatus status=framed_extract(io,output,length);
    if (status!=XR_XIR_OK || *output) return status;
    if (io->eof) { *eof=true;return io->length ? XR_XIR_IO : XR_XIR_OK; }
    if (io->length==io->capacity) {
        size_t capacity=io->capacity*2;if (capacity>io->limit+256) capacity=io->limit+256;
        if (capacity<=io->capacity) return XR_XIR_BUDGET;
        status=framed_resource(xr_compile_resources_resize(io->resources,(void **)&io->bytes,capacity));
        if (status!=XR_XIR_OK) return status;
        io->capacity=capacity;
    }
    size_t count=0;status=io->stream.read(io->stream.context,io->bytes+io->length,io->capacity-io->length,&count,&io->eof);
    if (status!=XR_XIR_OK) return status;
    if (count>io->capacity-io->length) return XR_XIR_BAD_STRUCTURE;
    status=framed_resource(xr_compile_resources_work(io->resources,count));if (status!=XR_XIR_OK) return status;
    io->length+=count;status=framed_extract(io,output,length);
    if (status==XR_XIR_OK && !*output && io->eof) { *eof=true;if (io->length) status=XR_XIR_IO; }
    return status;
}
XR_FUNC XrXirStatus xdap_framed_write(void *context,const char *bytes,size_t length) {
    XdapFramed *io=context;
    if (!io || (!bytes && length) || length>io->limit) return XR_XIR_BAD_STRUCTURE;
    char header[64];int size=xr_frame_write_header(header,sizeof(header),length);
    if (size<0) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=framed_resource(xr_compile_resources_work(io->resources,length+(uint32_t)size));
    if (status!=XR_XIR_OK) return status;
    status=io->stream.write(io->stream.context,header,(size_t)size);if (status!=XR_XIR_OK) return status;
    return io->stream.write(io->stream.context,bytes,length);
}
XR_FUNC XrXirStatus xdap_framed_wait(XdapFramed *io,uint32_t milliseconds) {
    if (!io || milliseconds>1000) return XR_XIR_BAD_STRUCTURE;
    return io->stream.wait(io->stream.context,milliseconds);
}
