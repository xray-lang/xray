/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_identity_internal.h - One identity grammar with explicit failure state
 */
#ifndef XMODULE_IDENTITY_INTERNAL_H
#define XMODULE_IDENTITY_INTERNAL_H
#include "xmodule_identity.h"
typedef struct XrModuleIdentityScan {
    XrModuleStatus status;
} XrModuleIdentityScan;
XR_FUNC bool xr_module_identity_length(XrModuleIdentityScan *work, const char *text, size_t *output);
XR_FUNC bool xr_module_identity_absolute(XrModuleIdentityScan *work, const char *path);
XR_FUNC bool xr_module_identity_authority_walk(XrModuleIdentityScan *work, const XrModuleIdentityAuthority *authority);
XR_FUNC bool xr_module_identity_logical_walk(XrModuleIdentityScan *work, const char *text, size_t length);
#endif // XMODULE_IDENTITY_INTERNAL_H
