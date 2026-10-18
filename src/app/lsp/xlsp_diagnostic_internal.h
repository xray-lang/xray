/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef XLSP_DIAGNOSTIC_INTERNAL_H
#define XLSP_DIAGNOSTIC_INTERNAL_H
#include "xlsp_diagnostic_owner.h"
#include "xlsp_document_owner.h"
#include "xlsp_document_store.h"
#include "xlsp_protocol_output.h"
#include "xlsp_index_json.h"
#include <string.h>
static inline const XrJsonValue *diagnostic_uri(const XrJsonValue *params) {
    XrJsonValue *uri=params?xjson_get((XrJsonValue *)params,"uri"):NULL;
    return uri&&uri->type==XR_JSON_STRING&&uri->as.string&&uri->string_len?uri:NULL;
}
static inline XrJsonValue *diagnostic_clear_new(const XrJsonValue *uri) {
    XrJsonValue *clear=xjson_new_object();if(!clear)return NULL;
    if(!index_json_member(clear,"uri",xjson_new_string_n(uri->as.string,uri->string_len))||
       !index_json_member(clear,"diagnostics",xjson_new_array())){xjson_free(clear);return NULL;}
    return clear;
}
static inline bool diagnostic_equal(const XrJsonValue *a,const XrJsonValue *b) {
    return a&&b&&a->string_len==b->string_len&&!memcmp(a->as.string,b->as.string,a->string_len);
}
/* Pay for validation and the later infallible ownership scan before output.
 * The event thread cannot reenter document mutation during transport writes. */
static inline XrXirStatus diagnostic_survey(XrLspServer *server,XlspSourceOpen *open,const XrJsonValue *uri) {
    XrLspDocTable *table=server->doc_table;
    if(!table||table->bucket_count<=0||!table->buckets||table->doc_count<0)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=xlsp_source_open_diagnostic_work(open,(uint64_t)table->bucket_count*2);
    if(status!=XR_XIR_OK)return status;
    size_t count=0;
    for(int i=0;i<table->bucket_count;++i)for(XrLspDocBucket *bucket=table->buckets[i];bucket;bucket=bucket->next) {
        status=xlsp_source_open_diagnostic_work(open,2*sizeof(*bucket));if(status!=XR_XIR_OK)return status;
        if(count++==(size_t)table->doc_count||!bucket->doc||bucket->doc->server!=server)return XR_XIR_BAD_STRUCTURE;
        const XrJsonValue *clear=xlsp_source_open_published_clear(bucket->doc->source_open);
        if(!clear)continue;
        const XrJsonValue *owned=diagnostic_uri(clear);if(!owned)return XR_XIR_BAD_STRUCTURE;
        uint64_t bytes=owned->string_len<uri->string_len?owned->string_len:uri->string_len;
        if(bytes>UINT64_MAX/4)return XR_XIR_BUDGET;
        status=xlsp_source_open_diagnostic_work(open,4*bytes);if(status!=XR_XIR_OK)return status;
    }
    return count==(size_t)table->doc_count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
static inline void diagnostic_evict(XrLspServer *server,const XrJsonValue *uri) {
    XrLspDocTable *table=server->doc_table;
    for(int i=0;i<table->bucket_count;++i)for(XrLspDocBucket *bucket=table->buckets[i];bucket;bucket=bucket->next) {
        XlspSourceOpen *open=bucket->doc->source_open;
        if(diagnostic_equal(uri,diagnostic_uri(xlsp_source_open_published_clear(open)))) {
            XrJsonValue *empty=NULL;xlsp_source_open_publication_commit(open,&empty);
        }
    }
}
#endif // XLSP_DIAGNOSTIC_INTERNAL_H
