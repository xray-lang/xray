/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_diagnostic.h - Internal module-loader diagnostic identities
 */

#ifndef XMODULE_DIAGNOSTIC_H
#define XMODULE_DIAGNOSTIC_H

#include "../runtime/value/xchunk.h"

/* Embedded bytecode already carries its producer's source identity.  A
 * stripped container has no file identity, so the diagnostic reports only
 * the module instead of manufacturing an artifact path. */
typedef struct XrModuleExecutionFailureIdentity {
    const char *source_file;
    const char *module_name;
    bool has_source_file;
} XrModuleExecutionFailureIdentity;

static inline XrModuleExecutionFailureIdentity
xr_module_embedded_execution_failure_identity(const XrProto *proto, const char *module_name) {
    const char *source_file = proto ? proto->source_file : NULL;
    XrModuleExecutionFailureIdentity identity = {
        .source_file = source_file,
        .module_name = module_name,
        .has_source_file = source_file && source_file[0] != '\0',
    };
    return identity;
}

#endif /* XMODULE_DIAGNOSTIC_H */
