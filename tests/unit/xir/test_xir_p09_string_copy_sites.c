/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_string_copy_sites.c - Locate actual execution copy costs
 *
 * KEY CONCEPT:
 *   Source locations come from the included production implementations.
 *   This diagnostic sample does not replace the million-item acceptance run.
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
typedef struct CopySite { const char *file; unsigned line; uint64_t calls, bytes; } CopySite;
static CopySite copy_sites[256];
static unsigned copy_site_count;
static bool copy_site_enabled;
static void copy_site_record(size_t bytes, const char *file, unsigned line) {
    if (!copy_site_enabled) return;
    unsigned i = 0;
    for (; i < copy_site_count; ++i)
        if (copy_sites[i].line == line && !strcmp(copy_sites[i].file, file)) break;
    if (i == copy_site_count) {
        if (copy_site_count == 256) abort();
        copy_sites[copy_site_count++] = (CopySite){file, line, 0, 0};
    }
    if (bytes > UINT64_MAX - copy_sites[i].bytes || copy_sites[i].calls == UINT64_MAX) abort();
    copy_sites[i].bytes += bytes; ++copy_sites[i].calls;
}
static void *copy_site_memcpy(void *destination, const void *source, size_t bytes, const char *file, unsigned line) {
    copy_site_record(bytes, file, line); return memcpy(destination, source, bytes);
}
static void *copy_site_memmove(void *destination, const void *source, size_t bytes, const char *file, unsigned line) {
    copy_site_record(bytes, file, line); return memmove(destination, source, bytes);
}
#define memcpy(destination, source, bytes) copy_site_memcpy(destination, source, bytes, __FILE__, __LINE__)
#define memmove(destination, source, bytes) copy_site_memmove(destination, source, bytes, __FILE__, __LINE__)
#define main original_capacity_main
#include "test_xir_array_capacity.c"
#undef main
#undef memmove
#undef memcpy
static int copy_site_order(const void *first, const void *second) {
    const CopySite *a = first, *b = second;
    return a->bytes < b->bytes ? 1 : a->bytes > b->bytes ? -1 : 0;
}
int main(int argc, char **argv) {
    CHECK(argc == 3); char *end = NULL;
    unsigned long mode = strtoul(argv[1], &end, 10); CHECK(end && !*end && mode <= 1);
    const char *name = argv[2]; CHECK(!strcmp(name, "short") || !strcmp(name, "digits"));
    CapacityCompile run = capacity_build(SIZE_MAX, capacity_limits(), (unsigned)mode, NULL);
    CHECK(run.status == XR_XIR_OK);
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.poll_limit = 68096;
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(run.program, &config, &instance) == XR_XIR_CALL_READY);
    XrXirValue init = capacity_run(instance, run.program->declarations->entry_function);
    CHECK(init.type == XR_XIR_I64 && !init.payload); xr_xir_value_drop(&init);
    XrXirValue arguments[] = {{XR_XIR_I64, 0, 1000}, {0}};
    CHECK(xr_xir_string_new(instance->domain, "x", 1, &arguments[1]) == XR_XIR_VALUE_OK);
    copy_site_enabled = true;
    XrXirCallStatus status = xr_xir_instance_start(instance, capacity_find(run.module, name), arguments, 2);
    while (status == XR_XIR_CALL_READY) status = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
    copy_site_enabled = false; CHECK(status == XR_XIR_CALL_RETURNED);
    XrXirValue result = {0}; CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&result, &bytes, &length) && length == 1000);
    for (size_t i = 0; i < length; ++i) CHECK(bytes[i] == (!strcmp(name, "digits") ? (char)('0' + i % 10) : 'x'));
    qsort(copy_sites, copy_site_count, sizeof(*copy_sites), copy_site_order);
    for (unsigned i = 0; i < copy_site_count; ++i)
        printf("COPY_SITE file=%s line=%u calls=%llu bytes=%llu\n", copy_sites[i].file, copy_sites[i].line,
            (unsigned long long)copy_sites[i].calls, (unsigned long long)copy_sites[i].bytes);
    xr_xir_value_drop(&result); xr_xir_value_drop(&arguments[1]);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    capacity_release(&run); instance_compile_report(); return 0;
}
