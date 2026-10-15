/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef LSP_DOCUMENT_TEST_ALLOC_H
#define LSP_DOCUMENT_TEST_ALLOC_H
#include "base/xmalloc.h"
XR_FUNC void *document_test_malloc(size_t);
XR_FUNC void *document_test_calloc(size_t, size_t);
XR_FUNC void *document_test_realloc(void *, size_t);
XR_FUNC void document_test_free(void *);
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc document_test_malloc
#define xr_calloc document_test_calloc
#define xr_realloc document_test_realloc
#define xr_free document_test_free
#endif
