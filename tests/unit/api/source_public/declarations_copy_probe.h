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
enum {DC_FUNCTIONS,DC_MODULE_NAME,DC_PUBLICATION};
typedef struct DeclarationsCopyProbe {
    unsigned phase,mode,witnesses,copies;
    size_t polls,bytes;
    bool wanted,oom_seen,implementations_returned;
    const XrXirDeclarations *source;
    XrXirDeclarations *copy;
    unsigned char *destination;
    XrCompileRequestControl *control;
    XrCompileResources *resources;
    uint64_t held_empty,held_before,held_after;
    void (*capture)(struct DeclarationsCopyProbe *,XrCompileResources *);
    void (*fail_next)(void);
} DeclarationsCopyProbe;
XR_FUNC void test_declarations_copy_begin(DeclarationsCopyProbe *probe);
XR_FUNC void test_declarations_copy_end(DeclarationsCopyProbe *probe);
#endif // DECLARATIONS_COPY_PROBE_H
