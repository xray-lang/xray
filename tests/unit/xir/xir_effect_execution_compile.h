/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_effect_execution_compile.h - Fresh semantic probes with retained compiler metadata
 */
#ifndef XIR_EFFECT_EXECUTION_COMPILE_H
#define XIR_EFFECT_EXECUTION_COMPILE_H
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_source.h"
/* The checker parses the actual manifest and module graph using one session
 * and ledger. Returned snapshots and artifacts retain that producer ledger. */
static inline XrXirStatus effect_execution_check(const XrXirSourceRequest *request,
    XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic) {
    const XrXirCompileContext *context=effects_source_owner(UINT64_C(64)*1024*1024,UINT64_C(128000000));
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrXirSourceRequest local=*request;local.context=context;local.session=session;
    XrXirStatus status=xr_xir_compile_source_check(&local,output,diagnostic,NULL);
    xr_compile_session_free(session);return status;
}
#endif // XIR_EFFECT_EXECUTION_COMPILE_H
