/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_json.c - Metered DAP JSON receiving and serialization
 */
#include "xdap_json.h"
#include "../../base/xjson_cursor.h"
#include "../../shared/xr_utf8_core.h"
#include <string.h>
#include <stdio.h>

typedef struct JsonReceiver {
    XdapJsonDocument document;
    XrJsonCursor cursor;
    uint32_t depth_limit;
} JsonReceiver;
static XrXirStatus json_resource(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        status==XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET :
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BAD_STRUCTURE;
}
static XrJsonCursorStatus json_work(void *context,uint64_t work) {
    return xr_compile_resources_work(context,work)==XR_COMPILE_RESOURCE_OK ?
        XR_JSON_CURSOR_OK : XR_JSON_CURSOR_BUDGET;
}
static bool json_fail(JsonReceiver *p,XrJsonCursorStatus status) {
    return xr_json_cursor_fail(&p->cursor,status);
}
static bool json_literal(JsonReceiver *p,const char *literal) {
    for (size_t i=0;literal[i];++i) {
        uint8_t byte=0;
        if (!xr_json_cursor_byte(&p->cursor,&byte) || byte!=(uint8_t)literal[i])
            return json_fail(p,XR_JSON_CURSOR_INVALID);
    }
    return true;
}
static bool json_number(JsonReceiver *p,XdapJsonNode *node) {
    XrJsonCursor *c=&p->cursor;const char *start=c->cursor;uint8_t b=0;
    if (!xr_json_cursor_peek(c,&b)) return false;
    if (b=='-') { ++c->cursor; if (!xr_json_cursor_peek(c,&b)) return false; }
    if (b<'0' || b>'9') return json_fail(p,XR_JSON_CURSOR_INVALID);
    bool zero=b=='0';uint32_t digits=0;
    while (c->cursor!=c->end) {
        if (!xr_json_cursor_peek(c,&b)) return false;
        if (b<'0' || b>'9') break;
        if (zero && digits) return json_fail(p,XR_JSON_CURSOR_INVALID);
        ++c->cursor;++digits;
    }
    node->integer=true;
    if (c->cursor!=c->end) {
        if (!xr_json_cursor_peek(c,&b)) return false;
        if (b=='.') {
            ++c->cursor;digits=0;node->integer=false;
            while (c->cursor!=c->end) {
                if (!xr_json_cursor_peek(c,&b)) return false;
                if (b<'0' || b>'9') break;
                ++c->cursor;++digits;
            }
            if (!digits) return json_fail(p,XR_JSON_CURSOR_INVALID);
        }
    }
    if (c->cursor!=c->end) {
        if (!xr_json_cursor_peek(c,&b)) return false;
        if (b=='e' || b=='E') {
            ++c->cursor;digits=0;node->integer=false;
            if (!xr_json_cursor_peek(c,&b)) return false;
            if (b=='+' || b=='-') ++c->cursor;
            while (c->cursor!=c->end) {
                if (!xr_json_cursor_peek(c,&b)) return false;
                if (b<'0' || b>'9') break;
                ++c->cursor;++digits;
            }
            if (!digits) return json_fail(p,XR_JSON_CURSOR_INVALID);
        }
    }
    node->kind=XDAP_JSON_NUMBER;node->text=start;node->length=(size_t)(c->cursor-start);
    return true;
}
static bool json_value(JsonReceiver *,uint32_t,uint32_t *);
static bool json_children(JsonReceiver *p,uint32_t parent,uint32_t depth,bool object) {
    XrJsonCursor *c=&p->cursor;uint8_t b=0;uint32_t last=0;
    if (!xr_json_cursor_take(c,object ? '{' : '[') || !xr_json_cursor_space(c) ||
        !xr_json_cursor_peek(c,&b)) return false;
    if (b==(uint8_t)(object ? '}' : ']')) { ++c->cursor; return true; }
    for (;;) {
        const char *key=NULL;size_t key_length=0;
        if (object) {
            if (!xr_json_cursor_string(c,128,&key) || !xr_json_cursor_length(c,key,&key_length) ||
                !xr_json_cursor_take(c,':')) return false;
            for (uint32_t i=p->document.nodes[parent].first;i;i=p->document.nodes[i].next) {
                if (!xr_json_cursor_work(c,1)) return false;
                if (xr_json_cursor_equal(c,key,p->document.nodes[i].key)) return json_fail(p,XR_JSON_CURSOR_INVALID);
                if (c->status!=XR_JSON_CURSOR_OK) return false;
            }
        }
        uint32_t child=0;
        if (!json_value(p,depth+1,&child)) return false;
        p->document.nodes[child].key=key;p->document.nodes[child].key_length=key_length;
        if (last) p->document.nodes[last].next=child;
        else p->document.nodes[parent].first=child;
        last=child;++p->document.nodes[parent].count;
        if (!xr_json_cursor_space(c) || !xr_json_cursor_byte(c,&b)) return false;
        if (b==(uint8_t)(object ? '}' : ']')) return true;
        if (b!=',') return json_fail(p,XR_JSON_CURSOR_INVALID);
    }
}
static bool json_value(JsonReceiver *p,uint32_t depth,uint32_t *output) {
    if (depth>p->depth_limit || p->document.count==p->document.capacity)
        return json_fail(p,XR_JSON_CURSOR_BUDGET);
    uint8_t b=0;
    if (!xr_json_cursor_space(&p->cursor) || !xr_json_cursor_peek(&p->cursor,&b)) return false;
    uint32_t index=++p->document.count;XdapJsonNode *node=&p->document.nodes[index];
    bool ok=false;
    if (b=='{' || b=='[') {
        node->kind=b=='{' ? XDAP_JSON_OBJECT : XDAP_JSON_ARRAY;
        ok=json_children(p,index,depth,b=='{');
    } else if (b=='"') {
        node->kind=XDAP_JSON_STRING;
        ok=xr_json_cursor_string(&p->cursor,(size_t)(p->cursor.end-p->cursor.begin),&node->text) &&
            xr_json_cursor_length(&p->cursor,node->text,&node->length);
    } else if (b=='t' || b=='f') {
        node->kind=XDAP_JSON_BOOL;node->boolean=b=='t';ok=json_literal(p,b=='t' ? "true" : "false");
    } else if (b=='n') { node->kind=XDAP_JSON_NULL;ok=json_literal(p,"null"); }
    else ok=json_number(p,node);
    if (ok) *output=index;
    return ok;
}
XR_FUNC void xdap_json_document_free(XdapJsonDocument *document) {
    if (!document) return;
    xr_compile_resources_free(document->nodes);xr_compile_resources_free(document->bytes);
    *document=(XdapJsonDocument){0};
}
XR_FUNC XrXirStatus xdap_json_parse(XrCompileResources *resources,const char *bytes,size_t length,
    uint32_t nodes,uint32_t depth,XdapJsonDocument *output) {
    if (!output || output->resources || output->bytes || output->nodes || output->count || output->capacity || output->root ||
        !resources || !bytes || !length || length==SIZE_MAX || !nodes || !depth || depth>64) return XR_XIR_BAD_STRUCTURE;
    JsonReceiver p={0};p.document.resources=resources;p.depth_limit=depth;
    size_t capacity=length/2+1;if (capacity>nodes) capacity=nodes;
    if (capacity>=UINT32_MAX) return XR_XIR_BUDGET;
    p.document.capacity=(uint32_t)capacity;
    XrXirStatus status=json_resource(xr_compile_resources_alloc(resources,length+1,(void **)&p.document.bytes));
    if (status!=XR_XIR_OK) goto done;
    status=json_resource(xr_compile_resources_work(resources,length));
    if (status!=XR_XIR_OK) goto done;
    memcpy(p.document.bytes,bytes,length);p.document.bytes[length]=0;
    status=json_resource(xr_compile_resources_calloc(resources,capacity+1,sizeof(*p.document.nodes),(void **)&p.document.nodes));
    if (status!=XR_XIR_OK) goto done;
    p.cursor=xr_json_cursor_make(p.document.bytes,length,resources,json_work);
    if (!json_value(&p,1,&p.document.root) || !xr_json_cursor_space(&p.cursor) || p.cursor.cursor!=p.cursor.end) {
        status=p.cursor.status==XR_JSON_CURSOR_BUDGET ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;goto done;
    }
    *output=p.document;p.document=(XdapJsonDocument){0};
done:
    xdap_json_document_free(&p.document);return status;
}
XR_FUNC XrXirStatus xdap_json_member(const XdapJsonDocument *d,uint32_t object,const char *key,uint32_t *output) {
    if (!d || !d->resources || !d->nodes || !object || object>d->count ||
        d->nodes[object].kind!=XDAP_JSON_OBJECT || !key || !output) return XR_XIR_BAD_STRUCTURE;
    XrJsonCursor c=xr_json_cursor_make(NULL,0,d->resources,json_work);
    for (uint32_t i=d->nodes[object].first;i;i=d->nodes[i].next) {
        if (!xr_json_cursor_work(&c,1)) return XR_XIR_BUDGET;
        if (xr_json_cursor_equal(&c,d->nodes[i].key,key)) { *output=i;return XR_XIR_OK; }
        if (c.status!=XR_JSON_CURSOR_OK) return XR_XIR_BUDGET;
    }
    *output=0;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xdap_json_u64(const XdapJsonDocument *d,uint32_t index,uint64_t *output) {
    if (!d || !d->resources || !d->nodes || !index || index>d->count || !output) return XR_XIR_BAD_STRUCTURE;
    const XdapJsonNode *n=&d->nodes[index];
    if (n->kind!=XDAP_JSON_NUMBER || !n->integer || !n->length || n->text[0]=='-') return XR_XIR_BAD_STRUCTURE;
    XrJsonCursor c=xr_json_cursor_make((char *)n->text,n->length,d->resources,json_work);uint64_t number=0;
    if (!xr_json_cursor_u64(&c,&number) || c.cursor!=c.end)
        return c.status==XR_JSON_CURSOR_BUDGET ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
    *output=number;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xdap_json_i64(const XdapJsonDocument *d,uint32_t index,int64_t *output) {
    if (!d || !d->resources || !d->nodes || !index || index>d->count || !output) return XR_XIR_BAD_STRUCTURE;
    const XdapJsonNode *n=&d->nodes[index];
    if (n->kind!=XDAP_JSON_NUMBER || !n->integer || !n->length) return XR_XIR_BAD_STRUCTURE;
    bool negative=n->text[0]=='-';size_t first=negative ? 1 : 0;uint64_t value=0;
    uint64_t limit=negative ? UINT64_C(1)<<63 : INT64_MAX;
    for (size_t i=first;i<n->length;++i) {
        if (xr_compile_resources_work(d->resources,1)!=XR_COMPILE_RESOURCE_OK) return XR_XIR_BUDGET;
        uint32_t digit=(uint8_t)n->text[i]-'0';
        if (digit>9 || value>(limit-digit)/10) return XR_XIR_BAD_STRUCTURE;
        value=value*10+digit;
    }
    *output=negative ? value==(UINT64_C(1)<<63) ? INT64_MIN : -(int64_t)value : (int64_t)value;
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xdap_json_equal(const XdapJsonDocument *d,uint32_t index,const char *expected,bool *output) {
    if (!d || !d->resources || !d->nodes || !index || index>d->count || !expected || !output ||
        d->nodes[index].kind!=XDAP_JSON_STRING) return XR_XIR_BAD_STRUCTURE;
    XrJsonCursor c=xr_json_cursor_make(NULL,0,d->resources,json_work);
    bool equal=xr_json_cursor_equal(&c,d->nodes[index].text,expected);
    if (c.status!=XR_JSON_CURSOR_OK) return XR_XIR_BUDGET;
    *output=equal;return XR_XIR_OK;
}
XR_FUNC XdapJsonWriter xdap_json_writer(XrCompileResources *resources,size_t limit) {
    return (XdapJsonWriter){.resources=resources,.limit=limit,
        .status=resources && limit && limit<SIZE_MAX ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE};
}
XR_FUNC bool xdap_json_text(XdapJsonWriter *w,const char *bytes,size_t length) {
    if (!w || w->status!=XR_XIR_OK) return false;
    if ((!bytes && length) || w->length>w->limit || length>w->limit-w->length) {
        w->status=XR_XIR_BUDGET;return false;
    }
    if (w->length+length>=w->capacity) {
        size_t capacity=w->capacity ? w->capacity : 256;
        size_t required=w->length+length+1;
        while (capacity<required) {
            if (capacity>SIZE_MAX/2) { capacity=required;break; }capacity*=2;
        }
        if (capacity>w->limit+1) capacity=w->limit+1;
        w->status=json_resource(xr_compile_resources_resize(w->resources,(void **)&w->bytes,capacity));
        if (w->status!=XR_XIR_OK) return false;
        w->capacity=capacity;
    }
    w->status=json_resource(xr_compile_resources_work(w->resources,length));
    if (w->status!=XR_XIR_OK) return false;
    if (length) memcpy(w->bytes+w->length,bytes,length);
    w->length+=length;w->bytes[w->length]=0;return true;
}
static int json_utf8_read(void *context,const uint8_t *address,uint8_t *output) {
    XdapJsonWriter *w=context;
    w->status=json_resource(xr_compile_resources_work(w->resources,1));
    if (w->status!=XR_XIR_OK) return 0;
    *output=*address;return 1;
}
XR_FUNC bool xdap_json_quote(XdapJsonWriter *w,const char *bytes,size_t length) {
    if (!w || w->status!=XR_XIR_OK || (!bytes && length)) return false;
    XrUtf8ScanResult scan;
    if (!xr_utf8_core_scan_strict_read((const uint8_t *)bytes,length,json_utf8_read,w,&scan) || scan.error!=XR_UTF8_OK) {
        if (w->status==XR_XIR_OK) w->status=XR_XIR_BAD_STRUCTURE;
        return false;
    }
    if (!xdap_json_text(w,"\"",1)) return false;
    static const char hex[]="0123456789abcdef";
    for (size_t i=0;i<length;++i) {
        uint8_t c=(uint8_t)bytes[i];char escape[6]={'\\','u','0','0',hex[c>>4],hex[c&15]};
        if (c<32) { if (!xdap_json_text(w,escape,6)) return false; }
        else if (c=='"' || c=='\\') {
            char pair[2]={'\\',(char)c};if (!xdap_json_text(w,pair,2)) return false;
        } else if (!xdap_json_text(w,bytes+i,1)) return false;
    }
    return xdap_json_text(w,"\"",1);
}
XR_FUNC bool xdap_json_integer(XdapJsonWriter *w,int64_t value) {
    char number[32];int length=snprintf(number,sizeof(number),"%lld",(long long)value);
    return length>0 && xdap_json_text(w,number,(size_t)length);
}
XR_FUNC bool xdap_json_unsigned(XdapJsonWriter *w,uint64_t value) {
    char number[32];int length=snprintf(number,sizeof(number),"%llu",(unsigned long long)value);
    return length>0 && xdap_json_text(w,number,(size_t)length);
}
XR_FUNC void xdap_json_writer_free(XdapJsonWriter *w) {
    if (!w) return;
    xr_compile_resources_free(w->bytes);*w=(XdapJsonWriter){0};
}
