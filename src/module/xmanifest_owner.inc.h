/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmanifest_owner.inc.h - Explicit ownership and first failure for manifest operations
 */
#ifndef XMANIFEST_OWNER_INTERNAL_H
#define XMANIFEST_OWNER_INTERNAL_H
#include "xnative_package.h"
#include "../base/xio_policy.inc.h"
#include "../base/xfileio.h"
#include "../os/os_file_read.h"
#include <limits.h>
#include <stdarg.h>
#include <string.h>

typedef struct ManifestContext {
    XrOsIoPolicy policy;
    XrManifestStatus status;
    XrManifestDiagnostic *diagnostic;
} ManifestContext;
typedef union ManifestAllocation {
    struct { XrOsIoPolicy policy; size_t bytes; } owner;
    long double floating;
    uint64_t integer;
    void *pointer;
} ManifestAllocation;
static inline const XrOsIoPolicy *manifest_policy(const void *owner) {
    return &((const ManifestAllocation *)owner-1)->owner.policy;
}
static inline bool manifest_uses_policy(const void *owner, const XrOsIoPolicy *policy) {
    if (!owner || !io_policy_valid(policy)) return false;
    const XrOsIoPolicy *held = manifest_policy(owner);
    return held->context == policy->context && held->alloc == policy->alloc &&
        held->free == policy->free && held->work == policy->work;
}
static inline bool manifest_status(ManifestContext *ctx, XrManifestStatus status) {
    if (ctx->status == XR_MANIFEST_OK) ctx->status = status;
    return ctx->status == XR_MANIFEST_OK;
}
static inline XrManifestStatus manifest_io_status(XrOsIoStatus status) {
    switch (status) {
    case XR_OS_IO_OK: return XR_MANIFEST_OK;
    case XR_OS_IO_NOT_FOUND: return XR_MANIFEST_NOT_FOUND;
    case XR_OS_IO_BUDGET: return XR_MANIFEST_BUDGET;
    case XR_OS_IO_OUT_OF_MEMORY: return XR_MANIFEST_OUT_OF_MEMORY;
    case XR_OS_IO_BAD_ARGUMENT: return XR_MANIFEST_BAD_ARGUMENT;
    case XR_OS_IO_UNSUPPORTED: return XR_MANIFEST_UNSUPPORTED;
    default: return XR_MANIFEST_IO;
    }
}
static inline bool manifest_io(ManifestContext *ctx, XrOsIoStatus status) {
    return manifest_status(ctx,manifest_io_status(status));
}
static inline bool manifest_work(ManifestContext *ctx, uint64_t units) {
    return ctx->status == XR_MANIFEST_OK && manifest_io(ctx,ctx->policy.work(ctx->policy.context,units));
}
static inline void manifest_free(void *memory) {
    if (!memory) return;
    ManifestAllocation *header = (ManifestAllocation *)memory-1;
    XrOsIoPolicy policy = header->owner.policy;
    policy.free(policy.context,header);
}
static inline void *manifest_alloc(ManifestContext *ctx, size_t bytes) {
    if (ctx->status != XR_MANIFEST_OK) return NULL;
    if (bytes > SIZE_MAX-sizeof(ManifestAllocation)) {
        manifest_status(ctx,XR_MANIFEST_BUDGET); return NULL;
    }
    void *memory = NULL;
    if (!manifest_io(ctx,ctx->policy.alloc(ctx->policy.context,bytes+sizeof(ManifestAllocation),&memory))) return NULL;
    ManifestAllocation *header = memory;
    header->owner.policy = ctx->policy; header->owner.bytes = bytes;
    return header+1;
}
static inline void *manifest_calloc(ManifestContext *ctx, size_t count, size_t size) {
    if (size && count > SIZE_MAX/size) { manifest_status(ctx,XR_MANIFEST_BUDGET); return NULL; }
    size_t bytes = count*size;
    void *memory = manifest_alloc(ctx,bytes);
    if (!memory) return NULL;
    if (!manifest_work(ctx,bytes)) { manifest_free(memory); return NULL; }
    memset(memory,0,bytes); return memory;
}
static inline size_t manifest_length(ManifestContext *ctx, const char *text) {
    size_t length = 0;
    if (!text) { manifest_status(ctx,XR_MANIFEST_BAD_ARGUMENT); return 0; }
    while (manifest_work(ctx,1)) {
        if (!text[length]) return length;
        if (length == SIZE_MAX-1) { manifest_status(ctx,XR_MANIFEST_BUDGET); return 0; }
        ++length;
    }
    return 0;
}
static inline void *manifest_copy(ManifestContext *ctx, void *out, const void *in, size_t bytes) {
    if (!manifest_work(ctx,bytes)) return out;
    if (bytes) memcpy(out,in,bytes);
    return out;
}
static inline char *manifest_duplicate(ManifestContext *ctx, const char *text) {
    size_t length = manifest_length(ctx,text);
    char *copy = manifest_alloc(ctx,length+1);
    if (copy && !manifest_work(ctx,length+1)) { manifest_free(copy); return NULL; }
    if (copy) memcpy(copy,text,length+1);
    return copy;
}
static inline int manifest_compare(ManifestContext *ctx, const char *left, const char *right) {
    for (;;) {
        if (!manifest_work(ctx,2)) return INT_MIN;
        unsigned char a=(unsigned char)*left++,b=(unsigned char)*right++;
        if (a != b || !a) return (int)a-(int)b;
    }
}
static inline int manifest_compare_n(ManifestContext *ctx, const char *left, const char *right, size_t length) {
    for (size_t i=0;i<length;++i) {
        if (!manifest_work(ctx,2)) return INT_MIN;
        unsigned char a=(unsigned char)left[i],b=(unsigned char)right[i];
        if (a != b || !a) return (int)a-(int)b;
    }
    return 0;
}
static inline int manifest_compare_bytes(ManifestContext *ctx, const void *left, const void *right, size_t length) {
    const unsigned char *a=left,*b=right;
    for (size_t i=0;i<length;++i) {
        if (!manifest_work(ctx,2)) return INT_MIN;
        unsigned char x=a[i],y=b[i]; if (x != y) return (int)x-(int)y;
    }
    return 0;
}
static inline const char *manifest_find(ManifestContext *ctx, const char *text, int ch, bool last) {
    const char *found=NULL;
    for (;;) {
        if (!manifest_work(ctx,1)) return NULL;
        unsigned char byte=(unsigned char)*text;
        if (byte == (unsigned char)ch) { found=text; if (!last) return found; }
        if (!byte) return found;
        ++text;
    }
}
static inline bool manifest_toml_status(ManifestContext *ctx, XrTomlParseStatus status) {
    switch (status) {
    case XR_TOML_PARSE_OK: return true;
    case XR_TOML_PARSE_INVALID: return manifest_status(ctx,XR_MANIFEST_INVALID);
    case XR_TOML_PARSE_LIMIT: return manifest_status(ctx,XR_MANIFEST_LIMIT);
    case XR_TOML_PARSE_BUDGET: return manifest_status(ctx,XR_MANIFEST_BUDGET);
    case XR_TOML_PARSE_OUT_OF_MEMORY: return manifest_status(ctx,XR_MANIFEST_OUT_OF_MEMORY);
    case XR_TOML_PARSE_IO: return manifest_status(ctx,XR_MANIFEST_IO);
    default: return manifest_status(ctx,XR_MANIFEST_BAD_ARGUMENT);
    }
}
static inline XrTomlValue *manifest_get(ManifestContext *ctx, XrTomlValue *table, const char *key) {
    XrTomlValue *value=NULL;
    if (ctx->status != XR_MANIFEST_OK || !table) return NULL;
    if (!manifest_toml_status(ctx,xtoml_owned_get(table,key,&value))) return NULL;
    return value;
}
static inline XrTomlValue *manifest_get_type(ManifestContext *ctx, XrTomlValue *table, const char *key, XrTomlType type) {
    XrTomlValue *value=manifest_get(ctx,table,key);
    if (value && !manifest_work(ctx,1)) return NULL;
    if (value && value->type != type) { manifest_status(ctx,XR_MANIFEST_INVALID); return NULL; }
    return value;
}
static inline const char *manifest_string(ManifestContext *ctx, XrTomlValue *table, const char *key) {
    XrTomlValue *value=manifest_get_type(ctx,table,key,XR_TOML_STRING);
    return value ? value->as.string : NULL;
}
static inline int64_t manifest_integer(ManifestContext *ctx, XrTomlValue *table, const char *key, int64_t fallback) {
    XrTomlValue *value=manifest_get_type(ctx,table,key,XR_TOML_INTEGER);
    return value ? value->as.integer : fallback;
}
static inline bool manifest_boolean(ManifestContext *ctx, XrTomlValue *table, const char *key, bool fallback) {
    XrTomlValue *value=manifest_get_type(ctx,table,key,XR_TOML_BOOL);
    return value ? value->as.boolean : fallback;
}
static inline XrOsIoStatus manifest_os_status(XrManifestStatus status) {
    switch (status) {
    case XR_MANIFEST_OK: return XR_OS_IO_OK;
    case XR_MANIFEST_BUDGET: case XR_MANIFEST_LIMIT: return XR_OS_IO_BUDGET;
    case XR_MANIFEST_OUT_OF_MEMORY: return XR_OS_IO_OUT_OF_MEMORY;
    case XR_MANIFEST_NOT_FOUND: return XR_OS_IO_NOT_FOUND;
    case XR_MANIFEST_INVALID: case XR_MANIFEST_BAD_ARGUMENT: return XR_OS_IO_BAD_ARGUMENT;
    case XR_MANIFEST_UNSUPPORTED: return XR_OS_IO_UNSUPPORTED;
    default: return XR_OS_IO_IO;
    }
}
/* This synchronous adapter adds the same private header to OS-owned outputs.
 * Headers retain the original policy, never this stack context. */
static inline XrOsIoStatus manifest_os_alloc(void *opaque, size_t bytes, void **out) {
    ManifestContext *ctx=opaque; void *value=manifest_alloc(ctx,bytes);
    if (value) *out=value;
    return manifest_os_status(ctx->status);
}
static inline void manifest_os_free(void *opaque, void *memory) { (void)opaque; manifest_free(memory); }
static inline XrOsIoStatus manifest_os_work(void *opaque, uint64_t units) {
    ManifestContext *ctx=opaque; manifest_work(ctx,units); return manifest_os_status(ctx->status);
}
static inline XrOsIoPolicy manifest_os_policy(ManifestContext *ctx) {
    XrOsIoPolicy policy={ctx,manifest_os_alloc,manifest_os_free,manifest_os_work}; return policy;
}
static inline char *manifest_join(ManifestContext *ctx, const char *root, const char *path) {
    char *value=NULL; XrOsIoPolicy policy=manifest_os_policy(ctx);
    if (ctx->status == XR_MANIFEST_OK) manifest_io(ctx,xr_path_join_owned(&policy,root,path,&value));
    return value;
}
static inline char *manifest_realpath(ManifestContext *ctx, const char *path) {
    char *value=NULL; XrOsIoPolicy policy=manifest_os_policy(ctx);
    if (ctx->status == XR_MANIFEST_OK) manifest_io(ctx,xr_realpath_owned(&policy,path,&value));
    return value;
}
static inline bool manifest_read_under_root(ManifestContext *ctx, const char *root,
    const char *logical, size_t limit, XrFileBytes *output) {
    if (ctx->status!=XR_MANIFEST_OK) return false;
    XrOsIoPolicy policy=manifest_os_policy(ctx);
    XrFileReadStatus status=xr_os_io_read_under_root(&policy,root,logical,limit,output);
    if (ctx->status!=XR_MANIFEST_OK)return false;
    switch (status) {
    case XR_FILE_READ_OK:return true;
    case XR_FILE_READ_MISSING:return manifest_status(ctx,XR_MANIFEST_NOT_FOUND);
    case XR_FILE_READ_LIMIT:return manifest_status(ctx,XR_MANIFEST_LIMIT);
    case XR_FILE_READ_OUT_OF_MEMORY:return manifest_status(ctx,XR_MANIFEST_OUT_OF_MEMORY);
    case XR_FILE_READ_FORBIDDEN:return manifest_status(ctx,XR_MANIFEST_INVALID);
    case XR_FILE_READ_BAD_ARGUMENT:return manifest_status(ctx,XR_MANIFEST_BAD_ARGUMENT);
    default:return manifest_status(ctx,XR_MANIFEST_IO);
    }
}
static inline bool manifest_absolute(ManifestContext *ctx, const char *path) {
    size_t length=manifest_length(ctx,path);
    if (ctx->status != XR_MANIFEST_OK || !length) return false;
    if (!manifest_work(ctx,length < 3 ? length : 3)) return false;
    unsigned char first=(unsigned char)path[0],second=length>1?(unsigned char)path[1]:0,third=length>2?(unsigned char)path[2]:0;
#ifdef XR_OS_WINDOWS
    return (length>=2 && (first=='/'||first=='\\') && (second=='/'||second=='\\')) ||
        (length>=3 && ((first>='A'&&first<='Z')||(first>='a'&&first<='z')) &&
         second==':' && (third=='/'||third=='\\'));
#else
    (void)second;(void)third;
    return first=='/';
#endif
}
typedef struct ManifestWriter {
    ManifestContext *ctx; char *buffer; size_t capacity, length; FILE *file;
} ManifestWriter;
static inline bool manifest_put(ManifestWriter *writer, char byte) {
    if (!manifest_work(writer->ctx,1)) return false;
    if (writer->file) {
        if (fputc((unsigned char)byte,writer->file)==EOF) return manifest_status(writer->ctx,XR_MANIFEST_IO);
    } else if (writer->capacity && writer->length < writer->capacity-1) writer->buffer[writer->length]=byte;
    if (writer->length==SIZE_MAX-1) return manifest_status(writer->ctx,XR_MANIFEST_BUDGET);
    ++writer->length;return true;
}
static inline bool manifest_vformat(ManifestWriter *writer, const char *format, va_list args) {
    ManifestContext *ctx=writer->ctx;
    for (;;) {
        if (!manifest_work(ctx,1)) return false;
        char byte=*format++;
        if (!byte) break;
        if (byte!='%') { if (!manifest_put(writer,byte)) return false;continue; }
        if (!manifest_work(ctx,1)) return false;
        byte=*format++;unsigned width=0;char padding=' ';
        if (byte=='0') { padding='0';if (!manifest_work(ctx,1))return false;byte=*format++; }
        while (byte>='0'&&byte<='9') {
            if (width>1000) return manifest_status(ctx,XR_MANIFEST_BAD_ARGUMENT);
            width=width*10+(unsigned)(byte-'0');
            if (!manifest_work(ctx,1))return false;byte=*format++;
        }
        unsigned modifier=0;
        if (byte=='z') { modifier=1;if (!manifest_work(ctx,1))return false;byte=*format++; }
        else if (byte=='l') {
            if (!manifest_work(ctx,1))return false;byte=*format++;
            if (byte!='l')return manifest_status(ctx,XR_MANIFEST_BAD_ARGUMENT);
            modifier=2;if (!manifest_work(ctx,1))return false;byte=*format++;
        }
        if (byte=='%') { if(!manifest_put(writer,'%'))return false;continue; }
        if (byte=='s') {
            const char *text=va_arg(args,const char *);if(!text)text="(null)";
            for (;;) {
                if(!manifest_work(ctx,1))return false;char ch=*text++;
                if(!ch)break;if(!manifest_put(writer,ch))return false;
            }
            continue;
        }
        if (byte!='u'&&byte!='d'&&byte!='x')return manifest_status(ctx,XR_MANIFEST_BAD_ARGUMENT);
        uint64_t value;bool negative=false;
        if (byte=='d') {
            int64_t signed_value=modifier==2?va_arg(args,long long):(int64_t)va_arg(args,int);
            negative=signed_value<0;value=negative?UINT64_C(0)-(uint64_t)signed_value:(uint64_t)signed_value;
        } else value=modifier==2?va_arg(args,unsigned long long):modifier==1?(uint64_t)va_arg(args,size_t):(uint64_t)va_arg(args,unsigned);
        unsigned base=byte=='x'?16:10;char digits[32];size_t count=0;
        do { if(!manifest_work(ctx,1))return false;digits[count++]="0123456789abcdef"[value%base];value/=base; } while(value);
        if(negative&&!manifest_put(writer,'-'))return false;
        while(width>count+(negative?1u:0u)) { if(!manifest_put(writer,padding))return false;--width; }
        while(count)if(!manifest_put(writer,digits[--count]))return false;
    }
    if(writer->buffer&&writer->capacity) {
        if(!manifest_work(ctx,1))return false;
        writer->buffer[writer->length<writer->capacity?writer->length:writer->capacity-1]=0;
    }
    return true;
}
static inline int manifest_format(ManifestContext *ctx, char *out, size_t size, const char *format, ...) {
    ManifestWriter writer={ctx,out,size,0,NULL};va_list args;va_start(args,format);
    bool ok=manifest_vformat(&writer,format,args);va_end(args);
    if(!ok)return -1;
    if(writer.length>INT_MAX) { manifest_status(ctx,XR_MANIFEST_BUDGET);return -1; }
    return (int)writer.length;
}
static inline void manifest_print(ManifestContext *ctx, FILE *out, const char *format, ...) {
    ManifestWriter writer={ctx,NULL,0,0,out};va_list args;va_start(args,format);
    manifest_vformat(&writer,format,args);va_end(args);
}
static inline void manifest_verror(ManifestContext *ctx, const char *format, va_list args) {
    if (ctx->status!=XR_MANIFEST_OK) return;
    ctx->status=XR_MANIFEST_INVALID;
    if (!ctx->diagnostic) return;
    /* A known schema failure remains the first cause. Optional diagnostics use
     * the same work policy and are published only if their rendering succeeds. */
    ManifestContext render={ctx->policy,XR_MANIFEST_OK,NULL};
    char message[sizeof(ctx->diagnostic->message)];
    ManifestWriter writer={&render,message,sizeof(message),0,NULL};
    if (manifest_vformat(&writer,format,args)) {
        size_t bytes=writer.length<sizeof(message)?writer.length+1:sizeof(message);
        if (manifest_work(&render,bytes))memcpy(ctx->diagnostic->message,message,bytes);
    }
}
static inline bool manifest_error_format(ManifestContext *ctx, const char *format, ...) {
    va_list args;va_start(args,format);manifest_verror(ctx,format,args);va_end(args);
    return false;
}
static inline bool manifest_error(ManifestContext *ctx, const char *message) {
    return manifest_error_format(ctx,"%s",message);
}
#endif /* XMANIFEST_OWNER_INTERNAL_H */
