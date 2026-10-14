/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_runtime_owner.c - Original output consumers across complete runtime faults
 *
 * KEY CONCEPT:
 *   The real public entry survives compiler destruction. Every observed runtime
 *   allocation cut preserves physical ownership and the original independent bytes.
 */
#include "xray_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir/xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
#include "xir/xxir_vm.c"
#include "xir/xxir_task_budget.c"
#include "xir/xxir_task.c"
#include "execution/xr_xir_host_execution.c"
#include "api/xsource_api.c"

typedef struct Output {
    const unsigned char *expected;
    size_t length, written;
} Output;

static XrSourceWriteStatus write_output(void *pointer, XrSourceOutputStream stream,
    const char *bytes, size_t length) {
    Output *out = pointer;
    CHECK(stream == XR_SOURCE_STDOUT && length <= out->length - out->written);
    CHECK(!memcmp(out->expected + out->written, bytes, length));
    out->written += length;
    return XR_SOURCE_WRITE_OK;
}

static unsigned char *read_expected(const char *path, size_t *length) {
    FILE *file = fopen(path,"rb");
    CHECK(file && !fseek(file,0,SEEK_END));
    long size = ftell(file);
    CHECK(size >= 0 && (uint64_t)size < (UINT64_C(1) << 24) && !fseek(file,0,SEEK_SET));
    unsigned char *bytes = xr_malloc((size_t)size + 1);
    CHECK(bytes && fread(bytes,1,(size_t)size,file)==(size_t)size && !fclose(file));
    *length = (size_t)size;
    return bytes;
}

static size_t execute(XrSourceProgram *program, Output *output, size_t point) {
    runtime_attempts = 0;
    runtime_fail_at = point;
    output->written = 0;
    XrSourceCallOptions options;
    CHECK(xray_source_call_options_init(&options,sizeof(options)) == XR_SOURCE_OK);
    options.write = write_output;
    options.context = output;
    XrSourceCall *call = NULL;
    XrSourceResult *result = NULL;
    XrSourceEvent event = {0};
    XrSourceStatus status = xray_source_call_begin(program,&options,&call);
    if (status == XR_SOURCE_OK) {
        size_t ticks = 0;
        do {
            CHECK(++ticks <= options.poll_limit);
            status = xray_source_call_poll(call,8,&event);
            if (status != XR_SOURCE_OK) break;
        } while (event.state == XR_SOURCE_RUN_READY);
        CHECK(event.state != XR_SOURCE_RUN_SUSPENDED);
        if (status == XR_SOURCE_OK) status = xray_source_call_take(call,&result);
    }
    size_t attempts = runtime_attempts;
    if (point == SIZE_MAX) {
        CHECK(status == XR_SOURCE_OK && result && output->written == output->length);
    } else {
        CHECK(attempts > point);
        if (!result) CHECK(status == XR_SOURCE_OUT_OF_MEMORY);
    }
    runtime_fail_at = SIZE_MAX;
    if (call && !result) {
        /* A failed facade or take allocation leaves the real terminal owner.
         * Retry in the same Call, without refunding or recreating its ledger. */
        CHECK(xray_source_call_poll(call,8,&event) == XR_SOURCE_OK);
        CHECK(event.state != XR_SOURCE_RUN_READY && event.state != XR_SOURCE_RUN_SUSPENDED);
        CHECK(xray_source_call_take(call,&result) == XR_SOURCE_OK && result);
    }
    if (result) {
        CHECK(xray_source_result_event(result,&event) == XR_SOURCE_OK);
        if (point == SIZE_MAX) CHECK(event.state == XR_SOURCE_RUN_RETURNED);
        else CHECK(event.state == XR_SOURCE_RUN_OUT_OF_MEMORY || event.state == XR_SOURCE_RUN_RETURNED);
        if (event.state == XR_SOURCE_RUN_RETURNED) CHECK(output->written == output->length);
    }
    XrSourceEvent cleanup = {0};
    CHECK(xray_source_call_drop(&call,&cleanup) == XR_SOURCE_OK && !call);
    xray_source_program_free(program);
    if (point == SIZE_MAX) {
        int64_t value = -1;
        CHECK(xray_source_result_i64(result,&value) == XR_SOURCE_OK && value == 0);
    }
    xray_source_result_free(result);
    CHECK(runtime_live == 1 && runtime_bytes == output->length + 1);
    return attempts;
}

static XrSourceProgram *compile_original(const char *path, const char *stdlib) {
    XrSourceCompilerOptions options;
    CHECK(xray_source_compiler_options_init(&options,sizeof(options)) == XR_SOURCE_OK);
    options.stdlib_path = stdlib;
    options.allocated_bytes = UINT64_C(64) << 20;
    options.live_bytes = UINT64_C(8) << 20;
    options.work = UINT64_C(128000000);
    XrSourceCompiler *compiler = NULL;
    XrSourceProgram *program = NULL;
    XrSourceDiagnostic *diagnostic = NULL;
    CHECK(xray_source_compiler_new(&options,&compiler) == XR_SOURCE_OK);
    XrSourceStatus status = xray_source_compile_file(compiler,path,&program,&diagnostic);
    if (status != XR_SOURCE_OK && diagnostic) {
        XrSourceDiagnosticInfo info;
        CHECK(xray_source_diagnostic_info(diagnostic,&info) == XR_SOURCE_OK);
        fprintf(stderr,"original consumer rejected: stage=%u line=%d column=%d %s\n",
            info.stage,info.line,info.column,info.message);
    }
    CHECK(status == XR_SOURCE_OK && program && !diagnostic);
    xray_source_compiler_free(compiler);
    return program;
}

int main(int argc, char **argv) {
    CHECK(argc == 4 && !runtime_live && !runtime_bytes);
    size_t length = 0;
    unsigned char *bytes = read_expected(argv[2],&length);
    Output output = {bytes,length,0};
    size_t sites = execute(compile_original(argv[1],argv[3]),&output,SIZE_MAX);
    CHECK(sites);
    printf("observed original runtime sites=%zu\n",sites);
    fflush(stdout);
    for (size_t point = 0; point < sites; ++point) {
        /* Each independently compiled fault input owns a new original ledger.
         * Only the failed take is retried in its same Call; no refund is made. */
        (void)execute(compile_original(argv[1],argv[3]),&output,point);
        printf("runtime-cut %zu/%zu every execution owner closed\n",point+1,sites);
        if ((point + 1) % 50 == 0) fflush(stdout);
    }
    xr_free(bytes);
    CHECK(!runtime_live && !runtime_bytes);
    printf("complete original runtime sites=%zu covered=%zu final physical=0/0\n",sites,sites);
    return 0;
}
