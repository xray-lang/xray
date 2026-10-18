/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "../lsp_json_clone/test_allocator.h"
#include "base/xmalloc.h"
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#undef xr_strdup
#define xr_malloc clone_test_malloc
#define xr_calloc clone_test_calloc
#define xr_realloc clone_test_realloc
#define xr_free clone_test_free
#define xr_strdup clone_test_strdup
#ifdef DECODE_BEFORE
#include "before_xjson.c"
#else
#include "base/xjson.c"
#endif
