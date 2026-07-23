/*
 * xray - Lightweight typed scripting with native concurrency
 * Public attribute registry shared by language tooling.
 */

#ifndef XATTRIBUTE_REGISTRY_H
#define XATTRIBUTE_REGISTRY_H

#include "xast_types.h"
#include "../../base/xdefs.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct XrPublicAttributeInfo {
    AttributeKind kind;
    const char *spelling;
    const char *targets;
    const char *arguments;
    const char *phase;
    bool production;
    const char *impact;
    const char *stability;
} XrPublicAttributeInfo;

XR_FUNC size_t xr_public_attribute_count(void);
XR_FUNC const XrPublicAttributeInfo *xr_public_attribute_at(size_t index);
XR_FUNC const XrPublicAttributeInfo *xr_public_attribute_by_kind(AttributeKind kind);
XR_FUNC const XrPublicAttributeInfo *xr_public_attribute_by_name(const char *name, size_t length);

#endif /* XATTRIBUTE_REGISTRY_H */
