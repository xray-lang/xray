/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_program_final_storage.inc.c - Live spans before Final traversal
 */
#define FINAL_STORAGE(pointer,count,type) do { \
    XrXirStatus range_status=effect_program_range(work,pointer,count,sizeof(type),_Alignof(type)); \
    if (range_status!=XR_XIR_OK) return range_status; \
} while (0)

/* The ledger proves readable current storage only. Every semantic equation,
 * physical role, cause, bound and least result remains an independent duty. */

#undef FINAL_STORAGE
