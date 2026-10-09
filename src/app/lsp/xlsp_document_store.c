/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xlsp_document_store.h"
#include "../../base/xhash.h"
#include "../../base/xmalloc.h"
#include <limits.h>
#include <string.h>
#define LSP_STORE_INITIAL 32
#define LSP_STORE_MAX (1<<16)
static uint32_t store_hash(const char *uri){return xr_hash_bytes(uri,strlen(uri));}
XrLspDocTable *xlsp_document_store_new(void (*destroy)(XrLspDocument *)) {
    if(!destroy)return NULL;
    XrLspDocTable *t=xr_calloc(1,sizeof(*t));if(!t)return NULL;
    t->buckets=xr_calloc(LSP_STORE_INITIAL,sizeof(*t->buckets));
    if(!t->buckets){xr_free(t);return NULL;}
    t->bucket_count=LSP_STORE_INITIAL;t->destroy=destroy;return t;
}
void xlsp_document_store_free(XrLspDocTable *t) {
    if(!t)return;
    for(int i=0;i<t->bucket_count;++i) {
        XrLspDocBucket *b=t->buckets[i];
        while(b){XrLspDocBucket *next=b->next;t->destroy(b->doc);xr_free(b);b=next;}
    }
    xr_free(t->buckets);xr_free(t);
}
XrLspDocument *xlsp_document_store_get(const XrLspDocTable *t,const char *uri) {
    if(!t||!uri||t->bucket_count<=0||!t->buckets)return NULL;
    for(XrLspDocBucket *b=t->buckets[store_hash(uri)%(uint32_t)t->bucket_count];b;b=b->next)
        if(!strcmp(b->uri,uri))return b->doc;
    return NULL;
}
void xlsp_document_store_abort(XlspDocumentInsertion *p) {
    if(!p)return;xr_free(p->buckets);xr_free(p->bucket);*p=(XlspDocumentInsertion){0};
}
bool xlsp_document_store_prepare(XrLspDocTable *t,const char *uri,XrLspDocument *doc,XlspDocumentInsertion *out) {
    if(!t||!uri||!doc||!out||out->table||out->bucket||out->buckets||out->bucket_count||out->generation||
        t->bucket_count<=0||!t->buckets||t->doc_count<0||t->doc_count==INT_MAX||t->generation==UINT64_MAX||xlsp_document_store_get(t,uri))return false;
    XlspDocumentInsertion p={0};p.table=t;p.generation=t->generation;p.bucket_count=t->bucket_count;
    size_t length=strlen(uri);if(length>SIZE_MAX-sizeof(*p.bucket)-1)return false;
    p.bucket=xr_calloc(1,sizeof(*p.bucket)+length+1);if(!p.bucket)return false;
    char *key=(char *)(p.bucket+1);memcpy(key,uri,length+1);p.bucket->doc=doc;p.bucket->uri=key;
    if((uint64_t)t->doc_count*4>(uint64_t)t->bucket_count*3&&t->bucket_count<LSP_STORE_MAX) {
        p.bucket_count=t->bucket_count>LSP_STORE_MAX/2?LSP_STORE_MAX:t->bucket_count*2;
        p.buckets=xr_calloc((size_t)p.bucket_count,sizeof(*p.buckets));
        if(!p.buckets){xlsp_document_store_abort(&p);return false;}
    }
    *out=p;return true;
}
bool xlsp_document_store_commit(XlspDocumentInsertion *p) {
    if(!p||!p->table||!p->bucket)return false;
    XrLspDocTable *t=p->table;
    if(t->generation!=p->generation||t->generation==UINT64_MAX||xlsp_document_store_get(t,p->bucket->uri))return false;
    /* Every allocation has succeeded before the first mutation. */
    if(p->buckets) {
        for(int i=0;i<t->bucket_count;++i) {
            XrLspDocBucket *b=t->buckets[i];
            while(b){XrLspDocBucket *next=b->next;uint32_t h=store_hash(b->uri)%(uint32_t)p->bucket_count;b->next=p->buckets[h];p->buckets[h]=b;b=next;}
        }
        xr_free(t->buckets);t->buckets=p->buckets;t->bucket_count=p->bucket_count;p->buckets=NULL;
    }
    uint32_t hash=store_hash(p->bucket->uri)%(uint32_t)t->bucket_count;
    p->bucket->next=t->buckets[hash];t->buckets[hash]=p->bucket;++t->doc_count;++t->generation;
    p->bucket=NULL;*p=(XlspDocumentInsertion){0};return true;
}
void xlsp_document_store_remove(XrLspDocTable *t,const char *uri) {
    if(!t||!uri||t->bucket_count<=0||!t->buckets)return;
    XrLspDocBucket **link=&t->buckets[store_hash(uri)%(uint32_t)t->bucket_count];
    while(*link) {
        XrLspDocBucket *b=*link;
        if(!strcmp(b->uri,uri)) {
            *link=b->next;--t->doc_count;
            /* Saturation still invalidates every outstanding prepared lease. */
            if(t->generation!=UINT64_MAX)++t->generation;
            t->destroy(b->doc);xr_free(b);return;
        }
        link=&b->next;
    }
}
