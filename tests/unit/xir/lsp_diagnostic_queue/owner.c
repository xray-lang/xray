/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "../lsp_document_owner/alloc.h"
#include "os/os_time.h"
XR_FUNC uint64_t diagnostic_queue_now(void);
#define xr_time_monotonic_ms diagnostic_queue_now
#include "app/lsp/xlsp_document_owner.c"
#undef xr_time_monotonic_ms
#include "app/lsp/xlsp_document_cache.c"
#include "app/lsp/xlsp_document_store.c"
