/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * declarations_copy_probe.h - Actual declaration receiver observations
 */
#ifndef DECLARATIONS_COPY_PROBE_H
#define DECLARATIONS_COPY_PROBE_H
#include "xir/xxir.h"
#include "xir/xxir_implementation.h"
enum {DC_FUNCTIONS,DC_MODULE_NAME,DC_PUBLICATION};
enum {IC_RECORD=3,IC_BINDING,IC_ARGUMENT,IC_PUBLICATION};
typedef struct DeclarationsCopyProbe {
    unsigned phase,mode,witnesses,copies;
    size_t polls,bytes;
    bool wanted,oom_seen,implementations_returned;
    bool implementation;
    const XrXirImplementationTable *implementation_source;
    XrXirImplementationTable *implementation_copy;
    size_t argument_bytes;
    const void *argument_source;
    const XrXirDeclarations *source;
    XrXirDeclarations *copy;
    unsigned char *destination;
    XrCompileRequestControl *control;
    XrCompileResources *resources;
    uint64_t held_empty,held_before,held_after;
    uint64_t implementation_held_before,implementation_held_after;
    void (*capture)(struct DeclarationsCopyProbe *,XrCompileResources *);
    void (*fail_next)(void);
} DeclarationsCopyProbe;
XR_FUNC void test_declarations_copy_begin(DeclarationsCopyProbe *probe);
XR_FUNC void test_declarations_copy_end(DeclarationsCopyProbe *probe);
XR_FUNC XrXirStatus test_implementation_copy(const XrXirCompileContext *,
    const XrXirImplementationTable *,XrCompileRequestControl *,XrXirImplementationTable **);
#endif // DECLARATIONS_COPY_PROBE_H
