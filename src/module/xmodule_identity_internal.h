/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_identity_internal.h - One identity grammar with explicit traversal policy
 */
#ifndef XMODULE_IDENTITY_INTERNAL_H
#define XMODULE_IDENTITY_INTERNAL_H
#include "xmodule_identity.h"
typedef struct XrModuleIdentityWork {
    void *context;
    bool (*charge)(void *, uint64_t);
    XrModuleStatus status;
} XrModuleIdentityWork;
XR_FUNC bool xr_module_identity_work(XrModuleIdentityWork *work, uint64_t count);
XR_FUNC bool xr_module_identity_length(XrModuleIdentityWork *work, const char *text, size_t *output);
XR_FUNC bool xr_module_identity_absolute(XrModuleIdentityWork *work, const char *path);
XR_FUNC bool xr_module_identity_authority_walk(XrModuleIdentityWork *work, const XrModuleIdentityAuthority *authority);
XR_FUNC bool xr_module_identity_logical_walk(XrModuleIdentityWork *work, const char *text, size_t length);
#endif // XMODULE_IDENTITY_INTERNAL_H
