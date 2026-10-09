/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef XLSP_DOCUMENT_STORE_H
#define XLSP_DOCUMENT_STORE_H
#include "../../base/xdefs.h"
#include <stdbool.h>
#include <stdint.h>
typedef struct XrLspDocument XrLspDocument;
typedef struct XrLspDocBucket {
    XrLspDocument *doc;
    const char *uri; /* Deep owned in this bucket's trailing storage. */
    struct XrLspDocBucket *next;
} XrLspDocBucket;
typedef struct XrLspDocTable {
    XrLspDocBucket **buckets;
    int bucket_count,doc_count;
    uint64_t generation;
    void (*destroy)(XrLspDocument *);
} XrLspDocTable;
typedef struct XlspDocumentInsertion {
    XrLspDocTable *table;
    XrLspDocBucket *bucket,**buckets;
    int bucket_count;
    uint64_t generation;
} XlspDocumentInsertion;
/* Event-loop ownership: no concurrent table mutation. Runtime UI allocations
 * use the system allocator, separately from compiler artifact ledgers. */
XR_FUNC XrLspDocTable *xlsp_document_store_new(void (*)(XrLspDocument *));
XR_FUNC void xlsp_document_store_free(XrLspDocTable *);
XR_FUNC XrLspDocument *xlsp_document_store_get(const XrLspDocTable *,const char *);
/* Prepare borrows doc and copies URI, leaving every existing bucket and count unchanged.
 * Commit transfers document ownership only after generation revalidation.
 * Abort never destroys the caller-owned staged document. */
XR_FUNC bool xlsp_document_store_prepare(XrLspDocTable *,const char *,XrLspDocument *,XlspDocumentInsertion *);
XR_FUNC bool xlsp_document_store_commit(XlspDocumentInsertion *);
XR_FUNC void xlsp_document_store_abort(XlspDocumentInsertion *);
XR_FUNC void xlsp_document_store_remove(XrLspDocTable *,const char *);
#endif
