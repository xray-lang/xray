/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_lexical.h - Source names paired with owned stop observations
 */
#ifndef XDAP_LEXICAL_H
#define XDAP_LEXICAL_H
#include "xdap_launch.h"
typedef struct XdapLexicalView {
    const XdapLaunch *launch;
    const XrXirSourceDebugFunction *function;
    const XrXirSourceView *source;
    const XrXirVmDebugSnapshot *snapshot;
    uint32_t frame,environment;
    uint64_t frame_id,epoch;
} XdapLexicalView;
/* The view borrows its launch and snapshot. Source facts select a value ordinal;
 * only the owned snapshot can admit a read. Failure preserves the output. */
XR_FUNC XrXirStatus xdap_lexical_open(const XdapLaunch *,const XrXirVmDebugSnapshot *,uint32_t,XdapLexicalView *);
XR_FUNC XrXirValueStatus xdap_lexical_read(const XdapLexicalView *,uint32_t,XrXirValue *,const char **,size_t *);
/* The global ordinal selects owned producer facts. Actual slot values and
 * access availability come exclusively from this frame's captured snapshot. */
XR_FUNC XrXirValueStatus xdap_global_read(const XdapLexicalView *,uint32_t,XrXirValue *,const char **,size_t *);
#endif // XDAP_LEXICAL_H
