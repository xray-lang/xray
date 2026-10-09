/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "app/lsp/xlsp_document_store.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
typedef struct StoreAllocation {void *pointer;size_t bytes;} StoreAllocation;
static StoreAllocation blocks[128];static size_t calls,fail_at=SIZE_MAX,live,bytes,releases;
static void *store_calloc(size_t count,size_t size) {
    if(calls++==fail_at)return NULL;CHECK(!count||size<=SIZE_MAX/count);
    void *pointer=xr_calloc(count,size);if(!pointer)return NULL;
    size_t i=0;while(i<128&&blocks[i].pointer)++i;CHECK(i<128);blocks[i]=(StoreAllocation){pointer,count*size};++live;bytes+=count*size;return pointer;
}
static void store_free(void *pointer) {
    if(!pointer)return;size_t i=0;while(i<128&&blocks[i].pointer!=pointer)++i;CHECK(i<128);
    --live;bytes-=blocks[i].bytes;blocks[i]=(StoreAllocation){0};xr_free(pointer);
}
#pragma push_macro("xr_calloc")
#pragma push_macro("xr_free")
#undef xr_calloc
#undef xr_free
#define xr_calloc store_calloc
#define xr_free store_free
#include "app/lsp/xlsp_document_store.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_calloc")
/* Store's public payload is opaque; these markers are caller-owned until
 * commit, exactly as GUI documents are. Callback identity is independently fixed. */
typedef struct StoreDoc {bool released;unsigned ordinal;} StoreDoc;
static void release_doc(XrLspDocument *opaque) {
    StoreDoc *doc=(StoreDoc *)opaque;CHECK(!doc->released);doc->released=true;++releases;
}
static void fixed_cases(void) {
    for(size_t fail=0;fail<2;++fail) {
        calls=0;fail_at=fail;CHECK(!xlsp_document_store_new(release_doc));CHECK(!live&&!bytes&&calls==fail+1);
    }
    fail_at=SIZE_MAX;calls=0;releases=0;XrLspDocTable *table=xlsp_document_store_new(release_doc);CHECK(table&&calls==2);
    StoreDoc docs[28]={{0}};char names[28][16];
    for(unsigned i=0;i<28;++i){docs[i].ordinal=i;CHECK(snprintf(names[i],sizeof(names[i]),"doc-%02u",i)>0);}
    XlspDocumentInsertion pending={0};size_t old_live=live,old_bytes=bytes;
    calls=0;fail_at=0;CHECK(!xlsp_document_store_prepare(table,names[0],(XrLspDocument *)&docs[0],&pending));
    CHECK(!pending.table&&table->doc_count==0&&live==old_live&&bytes==old_bytes&&!releases);
    fail_at=SIZE_MAX;
    for(unsigned i=0;i<25;++i) {
        CHECK(xlsp_document_store_prepare(table,names[i],(XrLspDocument *)&docs[i],&pending));
        CHECK(table->doc_count==(int)i&&!docs[i].released);CHECK(xlsp_document_store_commit(&pending));
    }
    CHECK(table->bucket_count==32&&table->doc_count==25);uint64_t generation=table->generation;
    XrLspDocBucket **old_buckets=table->buckets;old_live=live;old_bytes=bytes;
    for(size_t fail=0;fail<2;++fail) {
        calls=0;fail_at=fail;
        CHECK(!xlsp_document_store_prepare(table,names[25],(XrLspDocument *)&docs[25],&pending));
        CHECK(calls==fail+1&&!pending.table&&!pending.bucket&&!pending.buckets&&live==old_live&&bytes==old_bytes);
        CHECK(table->buckets==old_buckets&&table->bucket_count==32&&table->doc_count==25&&table->generation==generation&&!releases);
        for(unsigned i=0;i<25;++i)CHECK(xlsp_document_store_get(table,names[i])==(XrLspDocument *)&docs[i]);
    }
    fail_at=SIZE_MAX;calls=0;
    CHECK(xlsp_document_store_prepare(table,names[25],(XrLspDocument *)&docs[25],&pending)&&calls==2);
    CHECK(table->buckets==old_buckets&&table->doc_count==25);
    CHECK(xlsp_document_store_commit(&pending)&&table->bucket_count==64&&table->doc_count==26);
    /* Bucket key is deep owned, so changing the original producer buffer is safe. */
    memset(names[25],'?',sizeof(names[25]));CHECK(xlsp_document_store_get(table,"doc-25")== (XrLspDocument *)&docs[25]);
    calls=0;CHECK(!xlsp_document_store_prepare(table,names[0],(XrLspDocument *)&docs[26],&pending)&&calls==0);
    CHECK(xlsp_document_store_prepare(table,names[26],(XrLspDocument *)&docs[26],&pending));
    XlspDocumentInsertion other={0};CHECK(xlsp_document_store_prepare(table,names[27],(XrLspDocument *)&docs[27],&other));
    CHECK(xlsp_document_store_commit(&other));CHECK(!xlsp_document_store_commit(&pending));xlsp_document_store_abort(&pending);
    CHECK(!docs[26].released&&table->doc_count==27);xlsp_document_store_remove(table,"doc-25");CHECK(docs[25].released&&releases==1);
    xlsp_document_store_free(table);CHECK(releases==27&&!live&&!bytes&&!docs[26].released);
}
int main(void){fixed_cases();puts("LSP real store init 2, insertion 1, growth 2 FI; no partial publication; physical=0/0");return 0;}
