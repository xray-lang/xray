/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * nominal_projection_probe.h - Real synchronous projection observation
 */
#ifndef NOMINAL_PROJECTION_PROBE_H
#define NOMINAL_PROJECTION_PROBE_H
#include "xir/xxir_nominal.h"
enum {NP_LONG_SCAN,NP_FIELD_SCAN,NP_VARIANT_SCAN,NP_LONG_COPY,NP_VARIANT_COPY,NP_PUBLICATION};
typedef struct NominalProjectionProbe {
    unsigned phase,mode,witnesses,scanned,tail_scanned;
    size_t polls;
    bool wanted,oom_seen,copying;
    const char *long_name,*last_field,*variant_name;
    unsigned char *destination;
    const XrXirNominalTable *table;
    XrXirNominalIdentity *identities;
    XrCompileRequestControl *control;
    XrCompileResources *resources;
    uint64_t held_before,held_after,held_empty;
    void (*capture)(struct NominalProjectionProbe *,XrCompileResources *);
    void (*fail_next)(void);
} NominalProjectionProbe;
XR_FUNC void test_nominal_projection_begin(NominalProjectionProbe *probe);
XR_FUNC void test_nominal_projection_end(NominalProjectionProbe *probe);
#endif // NOMINAL_PROJECTION_PROBE_H
