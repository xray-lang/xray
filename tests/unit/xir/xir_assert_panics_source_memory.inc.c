/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_assert_panics_source_memory.inc.c - Exact Source allocation accounting
 */
static size_t source_attempts,source_fail_at=SIZE_MAX,source_live,source_bytes,source_peak;
typedef struct PanicsSourceMemory {void *pointer;size_t bytes;} PanicsSourceMemory;
static PanicsSourceMemory source_owned[8192];
static void *panics_source_calloc(size_t count,size_t size) {
    CHECK(!size || count<=SIZE_MAX/size);
    if (source_attempts++==source_fail_at) return NULL;
    void *pointer=xr_calloc(count,size);
    if (pointer) {
        CHECK(source_live<8192);source_owned[source_live++]=(PanicsSourceMemory){pointer,count*size};
        source_bytes+=count*size;if (source_bytes>source_peak) source_peak=source_bytes;
    }
    return pointer;
}
static void panics_source_free(void *pointer) {
    for (size_t i=0;pointer && i<source_live;++i) if (source_owned[i].pointer==pointer) {
        source_bytes-=source_owned[i].bytes;source_owned[i]=source_owned[--source_live];break;
    }
    xr_free(pointer);
}
