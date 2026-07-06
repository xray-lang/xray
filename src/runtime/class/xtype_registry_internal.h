/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_registry_internal.h - Minimal type-name registry metadata
 *
 * KEY CONCEPT:
 *   Public runtime metadata wrapper APIs have been removed. The
 *   remaining metadata is a small name -> XrClass identity entry used by
 *   runtime class lookup while the full TypeIdTable split lands.
 */

#ifndef XTYPE_REGISTRY_INTERNAL_H
#define XTYPE_REGISTRY_INTERNAL_H

#include "xclass.h"

typedef struct XrTypeMetadata XrTypeMetadata;

// klass is sole data source; NULL for special types like void
struct XrTypeMetadata {
    XrClass *klass;
    const char *name;  // Only used when klass==NULL
};

#endif  // XTYPE_REGISTRY_INTERNAL_H
