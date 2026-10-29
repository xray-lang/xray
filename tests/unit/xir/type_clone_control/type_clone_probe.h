/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * type_clone_probe.h - Actual type-copy storage observations are private to this test executable.
 */
#ifndef TYPE_CLONE_PROBE_H
#define TYPE_CLONE_PROBE_H
#include "xir/xxir_types.h"
#include "xir/xxir_interface.h"
#include "xir/xxir_constraints.h"
typedef enum TcPhase {TC_NODES_ZERO,TC_METHODS_ZERO,TC_PARAMETERS,TC_NAME,TC_ARGUMENTS,TC_FIELDS,TC_FIELD_ROWS,TC_PUBLICATION} TcPhase;
typedef enum TcMode {TC_SUCCESS,TC_CANCEL,TC_DEADLINE,TC_OOM} TcMode;
typedef struct TcProbe {
    TcPhase phase;TcMode mode;
    const XrXirTypes *source;
    const void *target;
    const XrXirNominalDeclaration *fields;
    XrXirTypes *copy;
    XrXirNominalTable *nominals;
    XrXirTypes **output;
    XrXirTypes *before_output;
    unsigned char *destination;
    size_t bytes,zero_count,zero_size,polls,witnesses,attempts,calls;
    XrCompileResources *resources;
    XrCompileRequestControl *control;
    uint64_t held_initial,held_before,held_after;
    bool requested,active,completed,watch,zero,wanted,oom_seen,retired,disabled;
} TcProbe;
XR_FUNC void tc_probe_begin(TcProbe *);
XR_FUNC void tc_probe_end(TcProbe *);
XR_FUNC TcProbe *tc_probe_current(void);
XR_FUNC bool tc_probe_select(TcProbe *,const XrXirTypes *);
XR_FUNC void tc_probe_observe(XrCompileRequestControl *);
XR_FUNC void *tc_probe_alloc(const XrXirCompileContext *,size_t,XrXirStatus *,const char *);
XR_FUNC void *tc_probe_calloc(const XrXirCompileContext *,size_t,size_t,XrXirStatus *,const char *);
XR_FUNC bool tc_probe_copy_begin(const void *,size_t,size_t,XrCompileRequestControl *);
XR_FUNC bool tc_probe_zero_begin(size_t,size_t,XrCompileRequestControl *);
XR_FUNC void tc_probe_span_end(bool,XrXirStatus);
XR_FUNC int tc_cancel(const void *);
XR_FUNC size_t tc_memory_live(void);
XR_FUNC size_t tc_memory_bytes(void);
XR_FUNC size_t tc_memory_attempts(void);
XR_FUNC void tc_memory_fail_next(void);
XR_FUNC void tc_memory_reset_failure(void);
XR_FUNC uint64_t tc_memory_initial(const XrCompileResources *);
XR_FUNC void tc_memory_reset(void);
#endif // TYPE_CLONE_PROBE_H
