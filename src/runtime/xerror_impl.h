/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xerror_impl.h - XrError full definition (internal, include only in .c files)
 *
 * KEY CONCEPT:
 *   XrError is a GC-managed object that can be passed as XrValue.
 *   Include only in .c files to avoid circular dependencies.
 */

#ifndef XERROR_IMPL_H
#define XERROR_IMPL_H

#include "xerror.h"
#include "value/xvalue.h"

// XrError is a GC-managed object (can be passed as XrValue)
struct XrError {
    XrGCHeader gc;
    XrErrorCode code;
    XrString *message;
    int line;
    int column;
    const char *file;
};

#endif // XERROR_IMPL_H
