/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_artifact.h - AOT output ownership model
 */

#ifndef XAOT_ARTIFACT_H
#define XAOT_ARTIFACT_H

typedef enum XaotArtifactKind {
    /* Closed-world program. Owns main(), runtime globals, and startup. */
    XAOT_ARTIFACT_EXECUTABLE = 0,
    /* Open-world dynamic library. Owns runtime globals and load-time init. */
    XAOT_ARTIFACT_SHARED_LIBRARY,
    /* Manifest-rooted code embedded in an existing hosted runtime. */
    XAOT_ARTIFACT_HOSTED_FRAGMENT,
} XaotArtifactKind;

#endif  // XAOT_ARTIFACT_H
