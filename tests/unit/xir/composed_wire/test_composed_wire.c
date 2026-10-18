/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_composed_wire.c - Complete independent packets across composed fields
 *
 * KEY CONCEPT:
 *   A current packet must survive input destruction and reproduce every byte.
 *   Historical headers are rejected before any payload allocation is attempted.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_compile_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)

typedef struct ComposedWireCase {
    const char *name;
    const uint8_t *original;
    size_t original_length;
    const uint8_t *current;
    size_t current_length;
    int expected;
} ComposedWireCase;

#include "catalog.inc.c"
#include "ordinals.inc.c"

_Static_assert(XR_XIR_CHECKED_SCHEMA==34 && XR_XIR_CHECKED_CONTRACT==90,
    "The independently encoded packets describe the complete composed fields");
_Static_assert(sizeof(composed_wire_cases)/sizeof(*composed_wire_cases)==300,
    "Every fixed current input remains an original rejection case");

static XrXirCompileContext wire_context(void) {
    const XrCompileResourceLimits limits={UINT64_C(67108864),UINT64_C(8388608),UINT64_C(128000000)};
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    return context;
}

static XrCompileResourceStats wire_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    return stats;
}

static void wire_original(const XrXirCompileContext *context,const ComposedWireCase *test) {
    const XrCompileResourceStats before=wire_stats(context);
    XrXirArtifact *artifact=NULL;
    XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_checked_read(context,test->original,test->original_length,&artifact,&diagnostic);
    if (status!=XR_XIR_BAD_STRUCTURE) fprintf(stderr,"%s original status=%u\n",test->name,status);
    CHECK(status==XR_XIR_BAD_STRUCTURE && diagnostic.status==XR_XIR_BAD_STRUCTURE && !artifact);
    XrCompileResourceStats after=wire_stats(context);
    CHECK(after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes);
    CHECK(after.live_bytes==before.live_bytes);
    artifact=(XrXirArtifact *)(uintptr_t)1;
    CHECK(xr_xir_compile_checked_read(context,test->original,test->original_length,&artifact,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(artifact==(XrXirArtifact *)(uintptr_t)1);
    after=wire_stats(context);
    CHECK(after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes);
    CHECK(after.live_bytes==before.live_bytes);
}

static void wire_current(const ComposedWireCase *test) {
    CHECK(test->current && test->current_length && test->expected>=0);
    XrXirCompileContext context=wire_context();
    const XrCompileResourceStats before=wire_stats(&context);
    void *input=NULL;
    CHECK(xr_compile_resources_alloc(context.resources,test->current_length,&input)==XR_COMPILE_RESOURCE_OK);
    memcpy(input,test->current,test->current_length);
    XrXirArtifact *artifact=NULL;
    XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_checked_read(&context,input,test->current_length,&artifact,&diagnostic);
    memset(input,0xa5,test->current_length);
    xr_compile_resources_free(input);
    if ((int)status!=test->expected)
        fprintf(stderr,"%s current status=%u expected=%d f%u b%u i%u\n",test->name,status,test->expected,
            diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK((int)status==test->expected);
    if (status==XR_XIR_OK) {
        CHECK(artifact && xr_xir_compile_artifact_verify(artifact,NULL)==XR_XIR_OK);
        XrXirCheckedPacket encoded={0};
        CHECK(xr_xir_compile_checked_write(artifact,&encoded,NULL)==XR_XIR_OK);
        CHECK(encoded.length==test->current_length && !memcmp(encoded.bytes,test->current,encoded.length));
        xr_xir_compile_checked_packet_free(&encoded);
    } else CHECK(!artifact);
    xr_xir_compile_artifact_free(artifact);
    CHECK(wire_stats(&context).live_bytes==before.live_bytes);
    xr_compile_resources_release(context.resources);
}

int main(void) {
    XrXirCompileContext context=wire_context();
    unsigned current=0,positive=0;
    for (size_t i=0;i<sizeof(composed_wire_cases)/sizeof(*composed_wire_cases);++i) {
        const ComposedWireCase *test=&composed_wire_cases[i];
        wire_original(&context,test);
        if (test->expected>=0) {
            wire_current(test);++current;
            if (test->expected==XR_XIR_OK) ++positive;
        }
    }
    xr_compile_resources_release(context.resources);
    CHECK(current==86 && positive==70);
    printf("300 original early rejections and %u complete current cases (%u accepted) PASS\n",current,positive);
    return 0;
}
