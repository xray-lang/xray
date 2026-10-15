/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef JSON_CLONE_TEST_ALLOCATOR_H
#define JSON_CLONE_TEST_ALLOCATOR_H
#include "base/xdefs.h"
#include <stddef.h>
#include <stdbool.h>
typedef struct CloneStats {size_t attempts,live,bytes;bool injected;} CloneStats;
XR_FUNC void *clone_test_malloc(size_t);
XR_FUNC void *clone_test_calloc(size_t,size_t);
XR_FUNC void *clone_test_realloc(void *,size_t);
XR_FUNC char *clone_test_strdup(const char *);
XR_FUNC void clone_test_free(void *);
XR_FUNC CloneStats clone_stats(void);
XR_FUNC void clone_fault(size_t);
XR_FUNC size_t clone_forced_cleanup(void);
#endif
