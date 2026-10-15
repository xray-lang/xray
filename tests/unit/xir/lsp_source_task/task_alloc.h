/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef LSP_SOURCE_TASK_ALLOC_H
#define LSP_SOURCE_TASK_ALLOC_H
#include "base/xmalloc.h"
XR_FUNC void *task_test_malloc(size_t);
XR_FUNC void *task_test_calloc(size_t, size_t);
XR_FUNC void task_test_free(void *);
#undef xr_malloc
#undef xr_calloc
#undef xr_free
#define xr_malloc task_test_malloc
#define xr_calloc task_test_calloc
#define xr_free task_test_free
#endif
