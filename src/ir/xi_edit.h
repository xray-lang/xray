/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_edit.h - Audited mutation boundary for canonical Xi rewrites
 */

#ifndef XI_EDIT_H
#define XI_EDIT_H

#include "xi_pass.h"
#include "../base/xdefs.h"
#include <stddef.h>
#include <stdint.h>

typedef struct XiEditFingerprint {
    uint64_t cfg;
    uint64_t values;
    uint64_t types;
    uint64_t memory;
    uint64_t calls;
} XiEditFingerprint;

typedef struct XiEditSession {
    XiFunc *func;
    XiEditFingerprint before;
    uint64_t ir_revision;
    uint64_t cfg_revision;
    uint64_t memory_revision;
    uint64_t call_revision;
    bool active;
} XiEditSession;

XR_FUNC bool xi_edit_begin(XiEditSession *session, XiFunc *func);
XR_FUNC bool xi_edit_finish(XiEditSession *session, XiPassChange reported,
                            XiEvidenceDomainMask explicitly_invalidates,
                            XiEvidenceDomainMask declared_preserves, XiPassOutcome *outcome,
                            char *error, size_t error_size);
XR_FUNC XiEditFingerprint xi_edit_fingerprint(const XiFunc *func);

#endif  // XI_EDIT_H
